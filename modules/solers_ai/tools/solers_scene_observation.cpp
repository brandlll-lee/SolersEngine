/**************************************************************************/
/*  solers_scene_observation.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "modules/solers_ai/core/solers_scene_observation.h"

#include "core/config/engine.h"
#include "core/debugger/debugger_marshalls.h"
#include "core/input/input_map.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/run/editor_run_bar.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/debugger/scene_debugger_object.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/environment.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/sky.h"
#include "servers/rendering/rendering_server.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_path_utils.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_runtime_observation.h"
#include "modules/solers_ai/core/solers_script_context.h"
#include "modules/solers_ai/core/solers_tool.h"
#include "modules/solers_ai/core/solers_trace.h"

static constexpr uint64_t SOLERS_CAPTURE_TIMEOUT_MSEC = 10000;
// Keep encoded captures small enough for model requests without losing layout
// readability.
static constexpr int SOLERS_CAPTURE_MAX_DIMENSION = 1280;

static bool _solers_capture_source_is_current(const Dictionary &p_source) {
	if (p_source.is_empty()) {
		return true;
	}
	EditorNode *editor = EditorNode::get_singleton();
	Node *root = editor && EditorNode::get_editor_data().get_edited_scene_count() > 0 ? editor->get_edited_scene() : nullptr;
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	const int history_id = root ? EditorNode::get_editor_data().get_current_edited_scene_history_id() : EditorUndoRedoManager::INVALID_HISTORY;
	if (!root || !manager || history_id == EditorUndoRedoManager::INVALID_HISTORY) {
		return false;
	}
	const uint64_t version = manager->get_or_create_history(history_id).undo_redo->get_version();
	ObjectID source_root_id;
	return (int64_t)p_source.get("history_id", -1) == history_id &&
			(uint64_t)(int64_t)p_source.get("version", -1) == version &&
			(!p_source.has("root_object_id") || (solers_object_id_from_variant(p_source["root_object_id"], source_root_id) && source_root_id == root->get_instance_id()));
}

static Array _solers_vector3_array(const Vector3 &p_vector) {
	// Snapped to 1e-4: full double precision multiplies the serialized tree
	// size several times over without carrying placement information.
	Array values;
	values.push_back(Math::snapped(p_vector.x, (real_t)0.0001));
	values.push_back(Math::snapped(p_vector.y, (real_t)0.0001));
	values.push_back(Math::snapped(p_vector.z, (real_t)0.0001));
	return values;
}

// Nearest visible-geometry AABB hit along a ray. Works on whitebox scenes with
// no physics bodies, so the agent learns "the camera stares into a wall 0.5m
// away" directly from the scene snapshot.
static void _solers_collect_ray_hit(Node *p_node, Node *p_root, const Vector3 &p_from, const Vector3 &p_dir, real_t &r_best_distance, String &r_best_path) {
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		const AABB bounds = geometry->get_global_transform().xform(geometry->get_aabb());
		if (geometry->is_visible_in_tree() && bounds.has_volume()) {
			bool inside = false;
			Vector3 point;
			if (bounds.find_intersects_ray(p_from, p_dir, inside, &point) && !inside) {
				const real_t distance = p_from.distance_to(point);
				if (distance > (real_t)0.01 && (r_best_distance < 0 || distance < r_best_distance)) {
					r_best_distance = distance;
					r_best_path = String(p_root->get_path_to(geometry));
				}
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_ray_hit(p_node->get_child(i), p_root, p_from, p_dir, r_best_distance, r_best_path);
	}
}

static String _solers_scene_path_hint(Node *p_root);

static bool _solers_focus_bounds(Node *p_root, const Array &p_paths, AABB &r_bounds, String &r_code, String &r_error) {
	bool found = false;
	for (int i = 0; i < p_paths.size(); i++) {
		const String path = String(p_paths[i]).strip_edges();
		Node *focus = p_root->get_node_or_null(NodePath(path));
		if (!focus) {
			r_code = "FOCUS_NODE_NOT_FOUND";
			r_error = vformat("No edited-scene node at focus_paths[%d]: %s. %s", i, path, _solers_scene_path_hint(p_root));
			return false;
		}
		solers_accumulate_world_aabb(focus, r_bounds, found);
	}
	if (!found) {
		r_code = "FOCUS_BOUNDS_UNAVAILABLE";
		r_error = "The requested focus nodes expose no finite visible VisualInstance3D bounds.";
	}
	return found;
}

static Dictionary _solers_framing_facts(Camera3D *p_camera, const AABB &p_bounds, const Array &p_paths) {
	Dictionary framing = solers_project_aabb(p_camera, p_bounds);
	framing["focus_paths"] = p_paths;
	framing["world_aabb"] = solers_aabb_data(p_bounds);
	return framing;
}

static Camera3D *_solers_find_first_camera(Node *p_node) {
	if (Camera3D *camera = Object::cast_to<Camera3D>(p_node)) {
		return camera;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (Camera3D *camera = _solers_find_first_camera(p_node->get_child(i))) {
			return camera;
		}
	}
	return nullptr;
}

// A failed node lookup should navigate, not dead-end: report where paths are
// anchored and what actually exists so one error is enough to self-correct.
static String _solers_scene_path_hint(Node *p_root) {
	PackedStringArray children;
	for (int i = 0; i < p_root->get_child_count() && i < 16; i++) {
		children.push_back(String(p_root->get_child(i)->get_name()));
	}
	return vformat("Paths are relative to the edited scene root '%s' (top-level children: %s).", String(p_root->get_name()), children.is_empty() ? String("none") : String(", ").join(children));
}

static void _solers_collect_camera_paths(Node *p_node, Node *p_root, PackedStringArray &r_paths) {
	if (Object::cast_to<Camera3D>(p_node) && r_paths.size() < 16) {
		r_paths.push_back(String(p_root->get_path_to(p_node)));
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_camera_paths(p_node->get_child(i), p_root, r_paths);
	}
}

// The editor draws lazily; queue one full redraw so a frame-gated capture is
// guaranteed to see a freshly rendered frame instead of timing out while the
// editor idles.
static void _solers_request_editor_redraw() {
	if (EditorNode::get_singleton() && EditorNode::get_singleton()->get_gui_base()) {
		EditorNode::get_singleton()->get_gui_base()->queue_redraw();
	}
}

int SolersSceneObservation::get_capture_settle_frame_count(bool p_sdfgi_enabled, int p_convergence_setting) {
	if (!p_sdfgi_enabled) {
		return 1;
	}
	static constexpr int frames[RSE::ENV_SDFGI_CONVERGE_MAX] = { 5, 10, 15, 20, 25, 30 };
	return frames[CLAMP(p_convergence_setting, 0, (int)RSE::ENV_SDFGI_CONVERGE_MAX - 1)];
}

static Ref<Environment> _solers_capture_environment(const Ref<World3D> &p_world, const Ref<Environment> &p_override = Ref<Environment>()) {
	if (p_override.is_valid()) {
		return p_override;
	}
	if (p_world.is_null()) {
		return Ref<Environment>();
	}
	const Ref<Environment> environment = p_world->get_environment();
	return environment.is_valid() ? environment : p_world->get_fallback_environment();
}

static Array _solers_render_color(const Color &p_color) {
	Array value;
	value.push_back(p_color.r);
	value.push_back(p_color.g);
	value.push_back(p_color.b);
	value.push_back(p_color.a);
	return value;
}
static Dictionary _solers_render_resource(const Ref<Resource> &p_resource) {
	Dictionary facts;
	if (p_resource.is_null()) {
		return facts;
	}
	facts["class_name"] = p_resource->get_class();
	facts["resource_path"] = p_resource->get_path();
	facts["rid"] = (int64_t)p_resource->get_rid().get_id();
	return facts;
}
static Variant _solers_render_value(const Variant &p_value) {
	if (p_value.get_type() == Variant::OBJECT) {
		return _solers_render_resource(Ref<Resource>(p_value));
	}
	return p_value;
}
static Dictionary _solers_shader_facts(const Ref<Material> &p_material) {
	Dictionary facts = _solers_render_resource(p_material);
	const Ref<ShaderMaterial> shader_material = p_material;
	const Ref<Shader> shader = shader_material.is_valid() ? shader_material->get_shader() : Ref<Shader>();
	if (shader.is_null()) {
		return facts;
	}
	facts["shader"] = _solers_render_resource(shader);
	facts["shader_source_sha256"] = shader->get_code().sha256_text();
	Dictionary parameters;
	List<PropertyInfo> uniforms;
	shader->get_shader_uniform_list(&uniforms);
	for (const PropertyInfo &uniform : uniforms) {
		if (uniform.type == Variant::NIL || (uniform.usage & PROPERTY_USAGE_GROUP)) {
			continue;
		}
		const Variant override_value = shader_material->get_shader_parameter(uniform.name);
		Dictionary parameter;
		parameter["source"] = override_value.get_type() == Variant::NIL ? "shader_default" : "material_override";
		parameter["value"] = _solers_render_value(override_value.get_type() == Variant::NIL ? RenderingServer::get_singleton()->shader_get_parameter_default(shader->get_rid(), uniform.name) : override_value);
		parameters[uniform.name] = parameter;
	}
	facts["parameters"] = parameters;
	return facts;
}
static void _solers_collect_render_lights(Node *p_node, Node *p_root, const Ref<World3D> &p_world, bool p_physical_units, Array &r_lights) {
	if (Light3D *light = Object::cast_to<Light3D>(p_node)) {
		if (light->is_visible_in_tree() && light->get_world_3d() == p_world) {
			Dictionary facts;
			facts["node_path"] = String(p_root->get_path_to(light));
			facts["class_name"] = light->get_class();
			facts["color"] = _solers_render_color(light->get_color());
			facts["energy_multiplier"] = light->get_param(Light3D::PARAM_ENERGY);
			facts["intensity"] = light->get_param(Light3D::PARAM_INTENSITY);
			facts["intensity_unit"] = p_physical_units ? (light->get_light_type() == RSE::LIGHT_DIRECTIONAL ? "lux" : "lumens") : "disabled";
			facts["indirect_energy"] = light->get_param(Light3D::PARAM_INDIRECT_ENERGY);
			facts["shadow_enabled"] = light->has_shadow();
			facts["editor_only"] = light->is_editor_only();
			if (DirectionalLight3D *directional = Object::cast_to<DirectionalLight3D>(light)) {
				facts["direction"] = _solers_vector3_array(-directional->get_global_basis().get_column(2).normalized());
				facts["sky_mode"] = (int)directional->get_sky_mode();
			}
			r_lights.push_back(facts);
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_render_lights(p_node->get_child(i), p_root, p_world, p_physical_units, r_lights);
	}
}
String SolersSceneObservation::render_state_fingerprint(const Dictionary &p_state) {
	return JSON::stringify(p_state, "", true, true).sha256_text();
}
Dictionary SolersSceneObservation::describe_render_state(Viewport *p_viewport, Camera3D *p_camera, Node *p_scene_root) const {
	Dictionary state;
	const Ref<World3D> world = p_viewport ? p_viewport->find_world_3d() : Ref<World3D>();
	if (world.is_null()) {
		return state;
	}
	p_camera = p_camera ? p_camera : p_viewport->get_camera_3d();
	const bool physical_units = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");
	state["scenario_rid"] = (int64_t)world->get_scenario().get_id();
	state["viewport_debug_draw"] = (int)p_viewport->get_debug_draw();
	state["physical_light_units"] = physical_units;
	const Ref<Environment> camera_environment = p_camera ? p_camera->get_environment() : Ref<Environment>();
	const Ref<Environment> environment = _solers_capture_environment(world, camera_environment);
	state["environment_source"] = camera_environment.is_valid() ? "camera" : (world->get_environment().is_valid() ? "world" : "fallback");
	if (environment.is_valid()) {
		Dictionary facts = _solers_render_resource(environment);
		facts["background_mode"] = (int)environment->get_background();
		facts["background_color"] = _solers_render_color(environment->get_bg_color());
		facts["background_energy_multiplier"] = environment->get_bg_energy_multiplier();
		facts["background_intensity"] = environment->get_bg_intensity();
		facts["ambient_source"] = (int)environment->get_ambient_source();
		facts["ambient_color"] = _solers_render_color(environment->get_ambient_light_color());
		facts["ambient_energy"] = environment->get_ambient_light_energy();
		facts["ambient_sky_contribution"] = environment->get_ambient_light_sky_contribution();
		facts["reflection_source"] = (int)environment->get_reflection_source();
		facts["tonemap_mode"] = (int)environment->get_tonemapper();
		facts["tonemap_exposure"] = environment->get_tonemap_exposure();
		facts["tonemap_white"] = environment->get_tonemap_white();
		facts["tonemap_agx_white"] = environment->get_tonemap_agx_white();
		facts["tonemap_agx_contrast"] = environment->get_tonemap_agx_contrast();
		facts["glow_enabled"] = environment->is_glow_enabled();
		facts["ssao_enabled"] = environment->is_ssao_enabled();
		facts["ssil_enabled"] = environment->is_ssil_enabled();
		facts["sdfgi_enabled"] = environment->is_sdfgi_enabled();
		facts["sdfgi_read_sky_light"] = environment->is_sdfgi_reading_sky_light();
		facts["fog_enabled"] = environment->is_fog_enabled();
		const Ref<Sky> sky = environment->get_sky();
		facts["sky"] = _solers_render_resource(sky);
		if (sky.is_valid()) {
			facts["sky_material"] = _solers_shader_facts(sky->get_material());
		}
		state["environment"] = facts;
	}
	const Ref<CameraAttributes> camera_attributes = p_camera ? p_camera->get_attributes() : Ref<CameraAttributes>();
	const Ref<CameraAttributes> attributes = camera_attributes.is_valid() ? camera_attributes : world->get_camera_attributes();
	state["camera_attributes_source"] = camera_attributes.is_valid() ? "camera" : (world->get_camera_attributes().is_valid() ? "world" : "none");
	if (attributes.is_valid()) {
		Dictionary facts = _solers_render_resource(attributes);
		facts["exposure_multiplier"] = attributes->get_exposure_multiplier();
		facts["exposure_sensitivity"] = attributes->get_exposure_sensitivity();
		facts["auto_exposure_enabled"] = attributes->is_auto_exposure_enabled();
		facts["exposure_normalization"] = physical_units ? attributes->calculate_exposure_normalization() : 1.0;
		facts["effective_exposure_multiplier"] = attributes->get_exposure_multiplier() * (float)facts["exposure_normalization"];
		if (const CameraAttributesPhysical *physical = Object::cast_to<CameraAttributesPhysical>(attributes.ptr())) {
			facts["aperture"] = physical->get_aperture();
			facts["shutter_speed"] = physical->get_shutter_speed();
		}
		state["camera_attributes"] = facts;
	}
	if (p_camera) {
		Dictionary facts;
		facts["projection"] = (int)p_camera->get_projection();
		facts["fov"] = p_camera->get_fov();
		facts["near"] = p_camera->get_near();
		facts["far"] = p_camera->get_far();
		facts["cull_mask"] = (int64_t)p_camera->get_cull_mask();
		facts["position"] = _solers_vector3_array(p_camera->get_global_position());
		facts["forward"] = _solers_vector3_array(-p_camera->get_global_basis().get_column(2).normalized());
		if (p_scene_root && (p_scene_root == p_camera || p_scene_root->is_ancestor_of(p_camera))) {
			facts["node_path"] = String(p_scene_root->get_path_to(p_camera));
		}
		state["camera"] = facts;
	}
	Array lights;
	if (p_scene_root) {
		_solers_collect_render_lights(p_scene_root, p_scene_root, world, physical_units, lights);
	}
	state["lights"] = lights;
	return state;
}

static int _solers_capture_settle_frames(const Ref<World3D> &p_world, const Ref<Environment> &p_override = Ref<Environment>()) {
	const Ref<Environment> environment = _solers_capture_environment(p_world, p_override);
	const bool sdfgi_enabled = environment.is_valid() && environment->is_sdfgi_enabled();
	return SolersSceneObservation::get_capture_settle_frame_count(sdfgi_enabled, (int)GLOBAL_GET("rendering/global_illumination/sdfgi/frames_to_converge"));
}

void SolersSceneObservation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_open_scenes"), &SolersSceneObservation::get_open_scenes);
	ClassDB::bind_method(D_METHOD("get_selection"), &SolersSceneObservation::get_selection);
	ClassDB::bind_method(D_METHOD("query_scene_nodes", "args", "token_budget"), &SolersSceneObservation::query_scene_nodes);
	ClassDB::bind_method(D_METHOD("capture_viewport", "args"), &SolersSceneObservation::capture_viewport);
}

uint64_t SolersSceneObservation::_runtime_epoch() const {
	return runtime_observation ? (uint64_t)(int64_t)runtime_observation->get_runtime_status().get("runtime_epoch", 0) : 0;
}

bool SolersSceneObservation::_is_runtime_visual_ready() const {
	return runtime_observation && (bool)runtime_observation->get_runtime_status().get("capture_ready", false);
}

bool SolersSceneObservation::_request_runtime_screenshot(const String &p_capture_id) {
	EditorDebuggerNode *node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = node ? node->get_current_debugger() : nullptr;
	if (!debugger || !debugger->is_session_active()) {
		return false;
	}
	const Callable callback = callable_mp(this, &SolersSceneObservation::_runtime_screenshot_ready);
	if (!debugger->is_connected(SNAME("debug_data"), callback)) {
		debugger->connect(SNAME("debug_data"), callback);
	}
	debugger->send_message("solers:capture", { p_capture_id, (int64_t)_runtime_epoch() });
	return true;
}

void SolersSceneObservation::_render_frame_post_draw() {
	render_post_draw_sequence++;
}

Dictionary SolersSceneObservation::_capture_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

Dictionary SolersSceneObservation::_runtime_capture_unavailable() const {
	Dictionary failure = _capture_error("RUNTIME_CAPTURE_UNAVAILABLE", "Runtime capture transport cannot request a root viewport screenshot.", true);
	Dictionary data;
	data["runtime"] = runtime_observation ? runtime_observation->get_runtime_status() : Dictionary();
	failure["data"] = data;
	return failure;
}

Dictionary SolersSceneObservation::_attach_render_receipt(Dictionary p_result, const Dictionary &p_pending) {
	if (!(bool)p_result.get("ok", false)) {
		return p_result;
	}
	Dictionary data = p_result.get("data", Dictionary());
	Dictionary receipt;
	const String target = data.get("target", p_pending.get("target", String()));
	const String capture_id = data.get("capture_id", p_pending.get("capture_id", String()));
	receipt["capture_id"] = capture_id;
	receipt["target"] = target;
	receipt["runtime_epoch"] = p_pending.get("runtime_epoch", 0);
	receipt["image_sha256"] = data.get("content_sha256", String());
	if (target == "runtime") {
		receipt["runtime_frame"] = data.get("runtime_frame", 0);
		receipt["physics_frame"] = data.get("physics_frame", 0);
	}
	if (target != "runtime") {
		receipt["source_state"] = p_pending.get("source_state", Dictionary());
		receipt["render_state_sha256"] = p_pending.get("render_state_sha256", String());
		if ((bool)p_pending.get("include_render_state", false)) {
			data["render_state"] = p_pending.get("render_state", Dictionary());
		}
	}

	const Dictionary source_state = p_pending.get("source_state", Dictionary());
	String view_key = target + ":" + String(source_state.get("scene_path", String()));
	for (const char *field : { "camera_path", "view_spec_hash" }) {
		if (p_pending.has(field)) {
			view_key += ":" + String(p_pending[field]);
		}
	}
	if (target == "editor") {
		view_key += ":" + String::num_int64((int64_t)p_pending.get("viewport_rid", 0));
	}
	receipt["view_key"] = view_key.sha256_text();
	Dictionary attachment = data.get("attachment", Dictionary());
	attachment["view_key"] = receipt["view_key"];
	data["attachment"] = attachment;
	p_result["attachments"] = Array({ attachment });
	const Dictionary *previous = last_render_by_view.getptr(view_key);
	const bool same_pixels = previous && String(previous->get("image_sha256", String())) == String(receipt.get("image_sha256", String()));
	receipt["same_pixels"] = same_pixels;
	if (target != "runtime") {
		receipt["same_render_state"] = previous && String(previous->get("render_state_sha256", String())) == String(receipt.get("render_state_sha256", String()));
	}
	if (same_pixels) {
		receipt["same_as"] = previous->get("capture_id", String());
		receipt["same_as_source_state"] = previous->get("source_state", Dictionary());
	}
	Dictionary current;
	current["capture_id"] = capture_id;
	current["image_sha256"] = receipt.get("image_sha256", String());
	current["source_state"] = source_state;
	if (target != "runtime") {
		current["render_state_sha256"] = receipt.get("render_state_sha256", String());
	}
	last_render_by_view[view_key] = current;
	data["render_receipt"] = receipt;
	p_result["data"] = data;
	return p_result;
}

Dictionary SolersSceneObservation::_editor_3d_viewport_state() const {
	Dictionary result;
	Node3DEditor *editor_3d = Node3DEditor::get_singleton();
	Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
	Viewport *viewport = editor_viewport ? editor_viewport->get_viewport_node() : nullptr;
	result["available"] = viewport != nullptr;
	if (!viewport) {
		return result;
	}
	const Viewport::DebugDraw debug_draw = viewport->get_debug_draw();
	result["state"] = editor_viewport->get_state();
	result["debug_draw"] = (int)debug_draw;
	result["frame_valid"] = debug_draw == Viewport::DEBUG_DRAW_DISABLED;
	if (debug_draw != Viewport::DEBUG_DRAW_DISABLED) {
		result["verification_warning"] = "The editor viewport is using a debug display mode, so this image is not valid evidence of final material appearance. Switch View > Display Normal and capture again.";
	}
	return result;
}

Dictionary SolersSceneObservation::_render_state_for_pending(const Dictionary &p_pending) const {
	Viewport *viewport = Object::cast_to<Viewport>(ObjectDB::get_instance(ObjectID((uint64_t)(int64_t)p_pending.get("viewport_id", 0))));
	Camera3D *camera = Object::cast_to<Camera3D>(ObjectDB::get_instance(ObjectID((uint64_t)(int64_t)p_pending.get("camera_id", 0))));
	EditorInterface *editor = EditorInterface::get_singleton();
	return describe_render_state(viewport, camera, editor ? editor->get_edited_scene_root() : nullptr);
}

void SolersSceneObservation::_runtime_screenshot_ready(const String &p_message, const Array &p_data) {
	if (p_message != "solers:capture_result" || p_data.size() != 1 || p_data[0].get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary result = p_data[0];
	const String capture_id = result.get("call_id", String());
	const Dictionary *entry = pending_captures.getptr(capture_id);
	const Dictionary pending = entry ? Dictionary(entry->get("data", Dictionary())) : Dictionary();
	if (!entry || (int64_t)result.get("runtime_epoch", 0) != (int64_t)pending.get("runtime_epoch", 0) || (uint64_t)(int64_t)result.get("runtime_epoch", 0) != _runtime_epoch()) {
		return;
	}
	pending_captures[capture_id] = _attach_render_receipt(result, pending);
}

static void _solers_free_capture_viewport(const Dictionary &p_data) {
	if (!(bool)p_data.get("owns_viewport", false)) {
		return;
	}
	const int64_t viewport_id = p_data.get("viewport_id", 0);
	if (viewport_id == 0) {
		return;
	}
	if (SubViewport *viewport = Object::cast_to<SubViewport>(ObjectDB::get_instance(ObjectID((uint64_t)viewport_id)))) {
		viewport->queue_free();
	}
}

// Captures resolve asynchronously through one pending/poll contract.
Dictionary SolersSceneObservation::_register_pending_capture(const String &p_target, const Dictionary &p_extra) {
	// Sweep abandoned captures and their transient viewports.
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	LocalVector<String> expired;
	for (const KeyValue<String, Dictionary> &entry : pending_captures) {
		const Dictionary data = entry.value.get("data", Dictionary());
		if (String(data.get("status", String())) == "pending" && now >= (uint64_t)(int64_t)data.get("deadline_msec", 0)) {
			_solers_free_capture_viewport(data);
			expired.push_back(entry.key);
		}
	}
	for (const String &key : expired) {
		pending_captures.erase(key);
	}

	const String capture_id = vformat("%s_%d", p_target, ++capture_sequence);
	Dictionary data;
	data["status"] = "pending";
	data["target"] = p_target;
	data["capture_id"] = capture_id;
	data["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + SOLERS_CAPTURE_TIMEOUT_MSEC);
	// Late screenshots cannot cross runtime epochs.
	data["runtime_epoch"] = (int64_t)_runtime_epoch();
	for (const Variant *key = p_extra.next(nullptr); key; key = p_extra.next(key)) {
		data[*key] = p_extra[*key];
	}
	if (p_target == "runtime") {
		Dictionary source_state;
		source_state["runtime_epoch"] = (int64_t)_runtime_epoch();
		data["source_state"] = source_state;
	} else if (p_target == "editor") {
		Node3DEditor *editor_3d = Node3DEditor::get_singleton();
		Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
		Viewport *viewport = editor_viewport ? editor_viewport->get_viewport_node() : nullptr;
		if (viewport) {
			data["viewport_id"] = (int64_t)(uint64_t)viewport->get_instance_id();
			data["viewport_rid"] = (int64_t)viewport->get_viewport_rid().get_id();
		}
	}
	if (p_target != "runtime") {
		const Dictionary render_state = _render_state_for_pending(data);
		if (render_state.is_empty()) {
			_solers_free_capture_viewport(data);
			return _capture_error("RENDER_STATE_UNAVAILABLE", "The requested viewport has no authoritative World3D render state.", true);
		}
		data["render_state"] = render_state;
		data["render_state_sha256"] = render_state_fingerprint(render_state);
		if (p_target == "editor") {
			Node3DEditor *editor_3d = Node3DEditor::get_singleton();
			Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
			Viewport *viewport = editor_viewport ? editor_viewport->get_viewport_node() : nullptr;
			const Ref<World3D> world = viewport ? viewport->find_world_3d() : Ref<World3D>();
			data["render_frames_required"] = _solers_capture_settle_frames(world);
		}
		const uint64_t start_frame = Engine::get_singleton()->get_frames_drawn();
		const int render_frames = MAX(1, (int)data.get("render_frames_required", 1));
		data["start_frame"] = (int64_t)start_frame;
		data["ready_frame"] = (int64_t)(start_frame + render_frames);
		data["start_post_draw"] = (int64_t)render_post_draw_sequence;
		data["ready_post_draw"] = (int64_t)(render_post_draw_sequence + render_frames);
		_solers_request_editor_redraw();
	}
	Dictionary poll_args;
	poll_args["target"] = p_target;
	poll_args["capture_id"] = capture_id;
	data["poll_args"] = poll_args;
	Dictionary pending;
	pending["ok"] = true;
	pending["data"] = data;
	pending_captures[capture_id] = pending;
	return pending;
}

Dictionary SolersSceneObservation::_finish_frame_gated_capture(const String &p_capture_id, const Dictionary &p_data) {
	const String target = p_data.get("target", String());
	if (!_solers_capture_source_is_current(p_data.get("source_state", Dictionary()))) {
		_solers_free_capture_viewport(p_data);
		return _capture_error("CAPTURE_SOURCE_CHANGED", "The edited scene changed while the requested viewport frame was rendering.", true);
	}
	const Dictionary readback_state = _render_state_for_pending(p_data);
	const String readback_sha256 = render_state_fingerprint(readback_state);
	if (readback_state.is_empty() || readback_sha256 != String(p_data.get("render_state_sha256", String()))) {
		_solers_free_capture_viewport(p_data);
		Dictionary failure = _capture_error("CAPTURE_RENDER_STATE_CHANGED", "The World3D render state changed while the requested frame was rendering.", true);
		Dictionary data;
		data["requested_render_state_sha256"] = p_data.get("render_state_sha256", String());
		data["readback_render_state_sha256"] = readback_sha256;
		if ((bool)p_data.get("include_render_state", false)) {
			data["readback_render_state"] = readback_state;
		}
		failure["data"] = data;
		return failure;
	}
	if (target == "editor") {
		Node3DEditor *editor_3d = Node3DEditor::get_singleton();
		Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
		if (!editor_viewport || !editor_viewport->get_viewport_node()) {
			return _capture_error("EDITOR_VIEWPORT_UNAVAILABLE", "The active 3D editor viewport is unavailable.", true);
		}
		Dictionary result = SolersScriptContext::store_image(editor_viewport->get_viewport_node()->get_texture()->get_image(), "editor", p_capture_id);
		if ((bool)result.get("ok", false)) {
			Dictionary data = result.get("data", Dictionary());
			const Dictionary viewport_state = _editor_3d_viewport_state();
			const int64_t frames_waited = (int64_t)Engine::get_singleton()->get_frames_drawn() - (int64_t)p_data.get("start_frame", 0);
			const int64_t post_draws_waited = (int64_t)render_post_draw_sequence - (int64_t)p_data.get("start_post_draw", 0);
			const bool settled = post_draws_waited >= (int64_t)p_data.get("render_frames_required", 1);
			data["editor_viewport"] = viewport_state;
			data["frame_valid"] = settled && (bool)viewport_state.get("frame_valid", false);
			data["render_frames_required"] = p_data.get("render_frames_required", 1);
			data["render_frames_waited"] = frames_waited;
			data["render_post_draws_waited"] = post_draws_waited;
			for (const char *key : { "focus_paths", "framing" }) {
				if (p_data.has(key)) {
					data[key] = p_data[key];
				}
			}
			result["data"] = data;
		}
		return _attach_render_receipt(result, p_data);
	}

	// camera / top_down render into a transient SubViewport that shares the
	// edited scene's World3D.
	const int64_t viewport_id = p_data.get("viewport_id", 0);
	SubViewport *viewport = Object::cast_to<SubViewport>(ObjectDB::get_instance(ObjectID((uint64_t)viewport_id)));
	if (!viewport) {
		return _capture_error("CAPTURE_VIEWPORT_LOST", "The transient capture viewport was freed before the frame rendered.", true);
	}
	viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
	Dictionary result = SolersScriptContext::store_image(viewport->get_texture()->get_image(), target, p_capture_id);
	viewport->queue_free();
	if ((bool)result.get("ok", false)) {
		Dictionary data = result.get("data", Dictionary());
		const int64_t frames_waited = (int64_t)Engine::get_singleton()->get_frames_drawn() - (int64_t)p_data.get("start_frame", 0);
		const int64_t post_draws_waited = (int64_t)render_post_draw_sequence - (int64_t)p_data.get("start_post_draw", 0);
		const bool settled = post_draws_waited >= (int64_t)p_data.get("render_frames_required", 1);
		data["frame_valid"] = settled;
		data["render_frames_required"] = p_data.get("render_frames_required", 1);
		data["render_frames_waited"] = frames_waited;
		data["render_post_draws_waited"] = post_draws_waited;
		for (const char *key : { "camera_path", "camera_forward_hit_node", "camera_forward_hit_distance", "orientation", "axis", "direction", "section_position", "focus_paths", "framing", "view_spec_hash" }) {
			if (p_data.has(key)) {
				data[key] = p_data[key];
			}
		}
		if (data.has("camera_forward_hit_node") || data.has("camera_forward_hit_distance")) {
			Dictionary framing;
			framing["camera_path"] = data.get("camera_path", String());
			framing["hit_node"] = data.get("camera_forward_hit_node", String());
			framing["hit_distance"] = data.get("camera_forward_hit_distance", 0.0);
			data["framing"] = framing;
		}
		result["data"] = data;
	}
	return _attach_render_receipt(result, p_data);
}

Dictionary SolersSceneObservation::_poll_pending_capture(const String &p_capture_id) {
	Dictionary *entry = pending_captures.getptr(p_capture_id);
	if (!entry) {
		return _capture_error("CAPTURE_NOT_FOUND", vformat("Unknown capture_id: %s", p_capture_id), true);
	}
	Dictionary result = *entry;
	Dictionary data = result.get("data", Dictionary());
	if (!(bool)result.get("ok", false) || String(data.get("status", String())) == "complete") {
		pending_captures.erase(p_capture_id);
		return result;
	}

	const String target = data.get("target", String());
	const bool deadline_reached = OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)data.get("deadline_msec", 0);

	if (target == "runtime" && (bool)data.get("awaiting_runtime_ready", false)) {
		if (_is_runtime_visual_ready()) {
			data["awaiting_runtime_ready"] = false;
			// The epoch is stamped when the request actually goes out, not when
			// the capture was parked: the runtime only started just now.
			data["runtime_epoch"] = (int64_t)_runtime_epoch();
			Dictionary source_state;
			source_state["runtime_epoch"] = (int64_t)_runtime_epoch();
			data["source_state"] = source_state;
			data["screenshot_requested"] = true;
			result["data"] = data;
			pending_captures[p_capture_id] = result;
			if (!_request_runtime_screenshot(p_capture_id)) {
				pending_captures.erase(p_capture_id);
				return _runtime_capture_unavailable();
			}
			entry = pending_captures.getptr(p_capture_id);
			return entry ? *entry : result;
		}
		if (deadline_reached) {
			pending_captures.erase(p_capture_id);
			return _capture_error("CAPTURE_TIMEOUT", "The runtime viewport was not visually ready before the capture deadline.", true);
		}
		return result;
	}
	if (target == "runtime" && !(bool)data.get("screenshot_requested", false)) {
		data["screenshot_requested"] = true;
		result["data"] = data;
		pending_captures[p_capture_id] = result;
		if (!_request_runtime_screenshot(p_capture_id)) {
			pending_captures.erase(p_capture_id);
			return _runtime_capture_unavailable();
		}
		return result;
	}

	if (target != "runtime" && render_post_draw_sequence >= (uint64_t)(int64_t)data.get("ready_post_draw", 0)) {
		const Dictionary final_result = _finish_frame_gated_capture(p_capture_id, data);
		pending_captures.erase(p_capture_id);
		return final_result;
	}
	if (deadline_reached) {
		_solers_free_capture_viewport(data);
		pending_captures.erase(p_capture_id);
		return _capture_error("CAPTURE_TIMEOUT", vformat("The %s viewport did not produce a frame before the capture deadline.", target), true);
	}
	if (target != "runtime") {
		_solers_request_editor_redraw();
	}
	return result;
}

Dictionary SolersSceneObservation::_begin_scene_view_capture(const String &p_target, const Dictionary &p_args) {
	Node3DEditor *editor_3d = Node3DEditor::get_singleton();
	Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
	Viewport *world_viewport = editor_viewport ? editor_viewport->get_viewport_node() : nullptr;
	if (!world_viewport) {
		return _capture_error("EDITOR_VIEWPORT_UNAVAILABLE", "The 3D editor viewport (and its World3D) is unavailable.", true);
	}
	Node *edited_root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!edited_root) {
		return _capture_error("NO_EDITED_SCENE", "No scene is being edited.", true);
	}

	const Array focus_paths = p_args.get("focus_paths", Array());
	AABB focus_bounds;
	bool has_focus_bounds = false;
	if (!focus_paths.is_empty()) {
		String focus_code;
		String focus_error;
		has_focus_bounds = _solers_focus_bounds(edited_root, focus_paths, focus_bounds, focus_code, focus_error);
		if (!has_focus_bounds) {
			return _capture_error(focus_code, focus_error, true);
		}
	} else if (p_target == "focus") {
		return _capture_error("FOCUS_PATHS_REQUIRED", "target=focus requires at least one focus_paths entry.", true);
	}

	Dictionary extra;
	extra["include_render_state"] = (bool)p_args.get("include_render_state", false);
	Vector3 orthographic_target;
	Ref<Environment> camera_environment;
	SubViewport *viewport = memnew(SubViewport);
	viewport->set_name("SolersCaptureViewport");
	if (p_args.has("debug_draw")) {
		PropertyInfo debug_draw_property;
		const int debug_draw = (int)p_args["debug_draw"];
		if (!ClassDB::get_property_info(SNAME("Viewport"), SNAME("debug_draw"), &debug_draw_property) || debug_draw < 0 || debug_draw >= debug_draw_property.hint_string.get_slice_count(",")) {
			memdelete(viewport);
			return _capture_error("INVALID_DEBUG_DRAW", "debug_draw must be a value from Godot's Viewport.debug_draw enum.", true);
		}
		viewport->set_debug_draw((Viewport::DebugDraw)debug_draw);
	}
	const Ref<World3D> world = world_viewport->find_world_3d();
	viewport->set_world_3d(world);
	viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
	Camera3D *camera = memnew(Camera3D);
	viewport->add_child(camera);

	if (p_target == "camera") {
		Camera3D *scene_camera = nullptr;
		const String camera_path = String(p_args.get("camera_path", String())).strip_edges();
		if (!camera_path.is_empty()) {
			scene_camera = Object::cast_to<Camera3D>(edited_root->get_node_or_null(NodePath(camera_path)));
			if (!scene_camera) {
				memdelete(viewport);
				PackedStringArray camera_paths;
				_solers_collect_camera_paths(edited_root, edited_root, camera_paths);
				return _capture_error("CAMERA_NOT_FOUND", vformat("No Camera3D at camera_path: %s. %s Camera3D nodes present: %s.", camera_path, _solers_scene_path_hint(edited_root), camera_paths.is_empty() ? String("none") : String(", ").join(camera_paths)), true);
			}
		} else {
			scene_camera = _solers_find_first_camera(edited_root);
			if (!scene_camera) {
				memdelete(viewport);
				return _capture_error("NO_CAMERA_IN_SCENE", "The edited scene has no Camera3D. Add one or pass camera_path.", true);
			}
		}
		int width = GLOBAL_GET("display/window/size/viewport_width");
		int height = GLOBAL_GET("display/window/size/viewport_height");
		if (width <= 0 || height <= 0) {
			width = 1152;
			height = 648;
		}
		const int longest = MAX(width, height);
		if (longest > SOLERS_CAPTURE_MAX_DIMENSION) {
			width = MAX(1, width * SOLERS_CAPTURE_MAX_DIMENSION / longest);
			height = MAX(1, height * SOLERS_CAPTURE_MAX_DIMENSION / longest);
		}
		viewport->set_size(Size2i(width, height));
		camera->set_projection(scene_camera->get_projection());
		camera->set_fov(scene_camera->get_fov());
		camera->set_size(scene_camera->get_size());
		camera->set_near(scene_camera->get_near());
		camera->set_far(scene_camera->get_far());
		camera->set_keep_aspect_mode(scene_camera->get_keep_aspect_mode());
		camera->set_cull_mask(scene_camera->get_cull_mask());
		camera->set_environment(scene_camera->get_environment());
		camera_environment = scene_camera->get_environment();
		camera->set_attributes(scene_camera->get_attributes());
		camera->set_transform(scene_camera->get_global_transform());
		extra["camera_path"] = String(edited_root->get_path_to(scene_camera));

		const Vector3 origin = scene_camera->get_global_position();
		const Vector3 forward = -scene_camera->get_global_transform().basis.get_column(2).normalized();
		real_t hit_distance = -1;
		String hit_path;
		_solers_collect_ray_hit(edited_root, edited_root, origin, forward, hit_distance, hit_path);
		if (hit_distance > 0) {
			extra["camera_forward_hit_node"] = hit_path;
			extra["camera_forward_hit_distance"] = hit_distance;
		}
		extra["focus_paths"] = focus_paths;
	} else if (p_target == "focus") {
		Camera3D *editor_camera = editor_viewport->get_camera_3d();
		if (!editor_camera) {
			memdelete(viewport);
			return _capture_error("EDITOR_CAMERA_UNAVAILABLE", "The active 3D editor camera is unavailable.", true);
		}
		Size2i capture_size = world_viewport->get_visible_rect().size;
		if (capture_size.x <= 0 || capture_size.y <= 0) {
			capture_size = Size2i(1152, 648);
		}
		const int longest = MAX(capture_size.x, capture_size.y);
		if (longest > SOLERS_CAPTURE_MAX_DIMENSION) {
			capture_size = capture_size * SOLERS_CAPTURE_MAX_DIMENSION / longest;
		}
		viewport->set_size(capture_size);
		camera->set_perspective(editor_camera->get_fov(), editor_camera->get_near(), editor_camera->get_far());
		camera->set_keep_aspect_mode(editor_camera->get_keep_aspect_mode());
		camera->set_cull_mask(editor_camera->get_cull_mask());
		camera->set_environment(editor_camera->get_environment());
		camera->set_attributes(editor_camera->get_attributes());
		camera_environment = editor_camera->get_environment();
		camera->set_transform(solers_frame_aabb(editor_camera->get_global_transform(), focus_bounds, editor_camera->get_fov(), (real_t)capture_size.x / (real_t)capture_size.y, editor_camera->get_keep_aspect_mode(), editor_camera->get_near()));
		camera->set_far(MAX(editor_camera->get_far(), camera->get_position().distance_to(focus_bounds.get_center()) + focus_bounds.size.length() * 0.5));
		extra["focus_paths"] = focus_paths;
		extra["orientation"] = "Perspective focus view using the active editor camera orientation.";
		extra["view_spec_hash"] = String::num_uint64(p_args.hash());
	} else { // top_down / orthographic
		AABB bounds = focus_bounds;
		bool found = has_focus_bounds;
		if (!found) {
			solers_accumulate_world_aabb(edited_root, bounds, found);
		}
		if (!found) {
			// Nodes rendering through RenderingServer directly (for example
			// GDExtension terrain) are invisible to the node AABB walk, so
			// frame a default volume and let the caller judge the image.
			bounds = AABB(Vector3(-64, -16, -64), Vector3(128, 96, 128));
		}
		extra["bounds_source"] = found ? "visible_geometry" : "default_volume";
		String axis_name = p_target == "top_down" ? String("y") : String(p_args.get("axis", String())).to_lower();
		String direction_name = p_target == "top_down" ? String("positive") : String(p_args.get("direction", String())).to_lower();
		const int axis = axis_name == "x" ? Vector3::AXIS_X : (axis_name == "y" ? Vector3::AXIS_Y : (axis_name == "z" ? Vector3::AXIS_Z : -1));
		if (axis < 0 || (direction_name != "positive" && direction_name != "negative")) {
			memdelete(viewport);
			return _capture_error("INVALID_ORTHOGRAPHIC_VIEW", "orthographic capture requires axis=x|y|z and direction=positive|negative.", true);
		}
		viewport->set_size(Size2i(1024, 1024));
		orthographic_target = bounds.get_center();
		const int perpendicular_a = (axis + 1) % 3;
		const int perpendicular_b = (axis + 2) % 3;
		const real_t span = MAX(bounds.size[perpendicular_a], bounds.size[perpendicular_b]);
		const real_t distance = bounds.size[axis] + MAX(span, (real_t)1.0);
		Vector3 view_axis;
		view_axis[axis] = direction_name == "positive" ? 1.0 : -1.0;
		Vector3 camera_position = orthographic_target + view_axis * distance;
		if (p_args.has("section_position")) {
			const Variant section = p_args["section_position"];
			if (section.get_type() != Variant::FLOAT && section.get_type() != Variant::INT) {
				memdelete(viewport);
				return _capture_error("INVALID_SECTION_POSITION", "section_position must be a world-space number on the view axis.", true);
			}
			camera_position[axis] = (real_t)section + view_axis[axis] * (real_t)0.05;
			extra["section_position"] = section;
		}
		camera->set_orthogonal(MAX(span * (real_t)1.15, (real_t)1.0), 0.05, distance * 2.0 + 10.0);
		camera->set_transform(Transform3D(Basis(), camera_position));
		extra["axis"] = axis_name;
		extra["direction"] = direction_name;
		extra["focus_paths"] = focus_paths;
		extra["view_spec_hash"] = String::num_uint64(p_args.hash());
		extra["orientation"] = axis == Vector3::AXIS_Y ? String("Orthographic top view. Image up = -Z, image right = +X.") : vformat("Orthographic %s-axis view. Image up = +Y.", axis_name.to_upper());
	}
	camera->set_current(true);
	extra["render_frames_required"] = _solers_capture_settle_frames(world, camera_environment);

	EditorNode *editor_node = EditorNode::get_singleton();
	if (!editor_node) {
		memdelete(viewport);
		return _capture_error("EDITOR_UNAVAILABLE", "EditorNode is unavailable.", false);
	}
	editor_node->add_child(viewport);
	if (p_target == "top_down" || p_target == "orthographic") {
		// look_at needs a valid global basis, so orient after entering the tree.
		const Vector3 up = String(extra.get("axis", String())) == "y" ? Vector3(0, 0, -1) : Vector3(0, 1, 0);
		camera->look_at_from_position(camera->get_global_position(), orthographic_target, up);
	}
	if (has_focus_bounds) {
		const Dictionary framing = _solers_framing_facts(camera, focus_bounds, focus_paths);
		if (!(bool)framing.get("in_frame", false)) {
			viewport->queue_free();
			return _capture_error("FOCUS_OUT_OF_FRAME", "The requested focus bounds do not intersect the capture frame.", true);
		}
		if (p_target == "focus" && !(bool)framing.get("fully_in_frame", false)) {
			viewport->queue_free();
			return _capture_error("FOCUS_FRAMING_FAILED", "The perspective focus camera did not contain the complete requested bounds.", true);
		}
		extra["framing"] = framing;
	}
	extra["viewport_id"] = (int64_t)(uint64_t)viewport->get_instance_id();
	extra["camera_id"] = (int64_t)(uint64_t)camera->get_instance_id();
	extra["owns_viewport"] = true;
	extra["viewport_rid"] = (int64_t)viewport->get_viewport_rid().get_id();
	if (p_args.has("source_state")) {
		extra["source_state"] = p_args["source_state"];
	}
	return _register_pending_capture(p_target, extra);
}

Dictionary SolersSceneObservation::capture_viewport(const Dictionary &p_args) {
	const String requested_id = String(p_args.get("capture_id", String())).strip_edges();
	if (!requested_id.is_empty()) {
		return _poll_pending_capture(requested_id);
	}

	const String target = String(p_args.get("target", String())).strip_edges().to_lower();
	if (target != "runtime" && !_solers_capture_source_is_current(p_args.get("source_state", Dictionary()))) {
		return _capture_error("CAPTURE_SOURCE_CONFLICT", "The edited scene changed before capture started.", true);
	}
	if (p_args.has("debug_draw") && target != "camera" && target != "focus" && target != "top_down" && target != "orthographic") {
		return _capture_error("DEBUG_DRAW_TARGET_UNSUPPORTED", "debug_draw is available on transient camera and orthographic captures.", true);
	}
	if (target == "editor") {
		EditorNode *editor_node = EditorNode::get_singleton();
		if (!editor_node || editor_node->get_editor_main_screen()->get_selected_index() != EditorMainScreen::EDITOR_3D) {
			return _capture_error("EDITOR_3D_NOT_ACTIVE", "The 3D editor must be the active main screen before capturing it.", true);
		}
		Node3DEditor *editor_3d = Node3DEditor::get_singleton();
		Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
		if (!editor_viewport || !editor_viewport->get_viewport_node()) {
			return _capture_error("EDITOR_VIEWPORT_UNAVAILABLE", "The active 3D editor viewport is unavailable.", true);
		}
		Dictionary extra;
		extra["include_render_state"] = (bool)p_args.get("include_render_state", false);
		if (p_args.has("source_state")) {
			extra["source_state"] = p_args["source_state"];
		}
		const Array focus_paths = p_args.get("focus_paths", Array());
		if (!focus_paths.is_empty()) {
			Node *edited_root = EditorInterface::get_singleton()->get_edited_scene_root();
			AABB focus_bounds;
			String focus_code;
			String focus_error;
			if (!edited_root || !_solers_focus_bounds(edited_root, focus_paths, focus_bounds, focus_code, focus_error)) {
				return _capture_error(focus_code.is_empty() ? String("NO_EDITED_SCENE") : focus_code, focus_error.is_empty() ? String("No scene is being edited.") : focus_error, true);
			}
			const Dictionary framing = _solers_framing_facts(editor_viewport->get_camera_3d(), focus_bounds, focus_paths);
			if (!(bool)framing.get("in_frame", false)) {
				return _capture_error("FOCUS_OUT_OF_FRAME", "The requested focus bounds do not intersect the active editor viewport.", true);
			}
			extra["focus_paths"] = focus_paths;
			extra["framing"] = framing;
		}
		return _register_pending_capture("editor", extra);
	}
	if (target == "camera" || target == "focus" || target == "top_down" || target == "orthographic") {
		return _begin_scene_view_capture(target, p_args);
	}
	if (target == "runtime") {
		if (!_is_runtime_visual_ready()) {
			Dictionary extra;
			extra["awaiting_runtime_ready"] = true;
			extra["runtime"] = runtime_observation ? runtime_observation->get_runtime_status() : Dictionary();
			return _register_pending_capture("runtime", extra);
		}
		Dictionary extra;
		extra["screenshot_requested"] = true;
		const Dictionary pending = _register_pending_capture("runtime", extra);
		const String capture_id = Dictionary(pending.get("data", Dictionary())).get("capture_id", String());
		if (!_request_runtime_screenshot(capture_id)) {
			pending_captures.erase(capture_id);
			return _runtime_capture_unavailable();
		}
		return pending;
	}
	return _capture_error("INVALID_CAPTURE_TARGET", "target must be one of 'editor', 'runtime', 'camera', 'top_down', or 'orthographic'.", true);
}

Dictionary SolersSceneObservation::poll_viewport_capture(const Dictionary &p_args) {
	const String capture_id = String(p_args.get("capture_id", String())).strip_edges();
	if (capture_id.is_empty()) {
		return _capture_error("INVALID_CAPTURE_CONTINUATION", "capture_id is required to continue a viewport capture.", false);
	}
	return _poll_pending_capture(capture_id);
}

bool SolersSceneObservation::is_viewport_capture_ready(const Dictionary &p_args) {
	const String capture_id = String(p_args.get("capture_id", String())).strip_edges();
	const Dictionary *entry = pending_captures.getptr(capture_id);
	if (!entry) {
		return true;
	}
	const Dictionary data = entry->get("data", Dictionary());
	if (!(bool)entry->get("ok", false) || String(data.get("status", String())) != "pending") {
		return true;
	}
	if (OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)data.get("deadline_msec", 0)) {
		return true;
	}
	const String target = data.get("target", String());
	if (target == "runtime") {
		return (bool)data.get("awaiting_runtime_ready", false) ? _is_runtime_visual_ready() : !(bool)data.get("screenshot_requested", false);
	}
	const bool ready = render_post_draw_sequence >= (uint64_t)(int64_t)data.get("ready_post_draw", 0);
	if (!ready) {
		_solers_request_editor_redraw();
	}
	return ready;
}

SolersSceneObservation::SolersSceneObservation() {
	if (RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->connect(SNAME("frame_post_draw"), callable_mp(this, &SolersSceneObservation::_render_frame_post_draw));
	}
}
