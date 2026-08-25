/**************************************************************************/
/*  solers_observation_service.cpp                                        */
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

#include "solers_observation_service.h"

#include "solers_tool.h"
#include "solers_trace.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/debugger/debugger_marshalls.h"
#include "core/input/input_map.h"
#include "core/io/dir_access.h"
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
#include "editor/run/game_view_plugin.h"
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
#include "modules/solers_ai/core/solers_resource_service.h"

static constexpr uint64_t SOLERS_CAPTURE_TIMEOUT_MSEC = 10000;
static constexpr uint64_t SOLERS_RUNTIME_QUERY_TIMEOUT_MSEC = 3000;
static constexpr int SOLERS_RUNTIME_EVENT_LIMIT = 512;
static constexpr int SOLERS_RUNTIME_PROPERTY_LIMIT = 64;
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

static void _solers_accumulate_world_aabb(Node *p_node, AABB &r_bounds, bool &r_found) {
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		const AABB bounds = geometry->get_global_transform().xform(geometry->get_aabb());
		if (geometry->is_visible_in_tree() && bounds.has_volume()) {
			r_bounds = r_found ? r_bounds.merge(bounds) : bounds;
			r_found = true;
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_accumulate_world_aabb(p_node->get_child(i), r_bounds, r_found);
	}
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

int SolersObservationService::get_capture_settle_frame_count(bool p_sdfgi_enabled, int p_convergence_setting) {
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
String SolersObservationService::render_state_fingerprint(const Dictionary &p_state) {
	return JSON::stringify(p_state, "", true, true).sha256_text();
}
Dictionary SolersObservationService::describe_render_state(Viewport *p_viewport, Camera3D *p_camera, Node *p_scene_root) const {
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
	return SolersObservationService::get_capture_settle_frame_count(sdfgi_enabled, (int)GLOBAL_GET("rendering/global_illumination/sdfgi/frames_to_converge"));
}

void SolersObservationService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_project_info"), &SolersObservationService::get_project_info);
	ClassDB::bind_method(D_METHOD("get_project_settings_summary"), &SolersObservationService::get_project_settings_summary);
	ClassDB::bind_method(D_METHOD("search_project", "args", "token_budget"), &SolersObservationService::search_project, DEFVAL(INT32_MAX));
	ClassDB::bind_method(D_METHOD("observe_path", "path"), &SolersObservationService::observe_path);
	ClassDB::bind_method(D_METHOD("digest_packed_scene", "path", "max_nodes"), &SolersObservationService::digest_packed_scene, DEFVAL(96));
	ClassDB::bind_method(D_METHOD("read_project_file", "path", "line_start", "line_count", "raw", "token_budget"), &SolersObservationService::read_project_file, DEFVAL(1), DEFVAL(200), DEFVAL(false), DEFVAL(INT32_MAX));
	ClassDB::bind_method(D_METHOD("get_open_scenes"), &SolersObservationService::get_open_scenes);
	ClassDB::bind_method(D_METHOD("get_selection"), &SolersObservationService::get_selection);
	ClassDB::bind_method(D_METHOD("query_scene_nodes", "args", "token_budget"), &SolersObservationService::query_scene_nodes);
	ClassDB::bind_method(D_METHOD("get_runtime_status"), &SolersObservationService::get_runtime_status);
	ClassDB::bind_method(D_METHOD("observe_runtime", "args", "token_budget"), &SolersObservationService::observe_runtime, DEFVAL(INT32_MAX));
	ClassDB::bind_method(D_METHOD("capture_viewport", "args"), &SolersObservationService::capture_viewport);
}

void SolersObservationService::_render_frame_post_draw() {
	render_post_draw_sequence++;
}

Dictionary SolersObservationService::_capture_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

Dictionary SolersObservationService::_runtime_capture_unavailable() const {
	Dictionary failure = _capture_error("RUNTIME_CAPTURE_UNAVAILABLE", "Runtime capture transport cannot request a root viewport screenshot.", true);
	Dictionary data;
	data["runtime"] = get_runtime_status();
	failure["data"] = data;
	return failure;
}

Dictionary SolersObservationService::_capture_image(const Ref<Image> &p_image, const String &p_target, const String &p_capture_id) {
	if (p_image.is_null() || p_image->get_width() <= 1 || p_image->get_height() <= 1) {
		return _capture_error("VIEWPORT_IMAGE_UNAVAILABLE", "The viewport has not produced a readable frame.", true);
	}

	Ref<Image> image = p_image;
	const int max_dimension = MAX(image->get_width(), image->get_height());
	if (max_dimension > SOLERS_CAPTURE_MAX_DIMENSION) {
		image = p_image->duplicate();
		const double scale = (double)SOLERS_CAPTURE_MAX_DIMENSION / (double)max_dimension;
		image->resize(MAX(1, (int)(image->get_width() * scale)), MAX(1, (int)(image->get_height() * scale)), Image::INTERPOLATE_LANCZOS);
	}

	const String capture_dir = ProjectSettings::get_singleton()->globalize_path(ProjectSettings::get_singleton()->get_project_data_path().path_join("solers/captures"));
	const Error directory_error = DirAccess::make_dir_recursive_absolute(capture_dir);
	if (directory_error != OK) {
		return _capture_error("CAPTURE_DIRECTORY_FAILED", vformat("Could not create the capture directory: %s", capture_dir), false);
	}
	const String temporary_path = capture_dir.path_join(p_capture_id + ".tmp.png");
	const Error save_error = image->save_png(temporary_path);
	if (save_error != OK) {
		return _capture_error("CAPTURE_SAVE_FAILED", vformat("Could not save the viewport capture: %s", temporary_path), false);
	}
	const String content_sha256 = FileAccess::get_sha256(temporary_path);
	const String absolute_path = capture_dir.path_join(content_sha256 + ".png");
	if (FileAccess::exists(absolute_path)) {
		DirAccess::remove_absolute(temporary_path);
	} else if (DirAccess::rename_absolute(temporary_path, absolute_path) != OK) {
		return _capture_error("CAPTURE_SAVE_FAILED", "Could not commit content-addressed viewport evidence.", false);
	}

	Dictionary attachment;
	attachment["id"] = p_capture_id;
	attachment["source"] = "tool_capture";
	attachment["target"] = p_target;
	attachment["type"] = "image";
	attachment["mime_type"] = "image/png";
	attachment["local_path"] = absolute_path;
	attachment["width"] = image->get_width();
	attachment["height"] = image->get_height();

	Dictionary data;
	data["status"] = "complete";
	data["target"] = p_target;
	data["capture_id"] = p_capture_id;
	data["width"] = image->get_width();
	data["height"] = image->get_height();
	data["content_sha256"] = content_sha256;
	attachment["content_sha256"] = content_sha256;
	data["attachment"] = attachment;

	Array attachments;
	attachments.push_back(attachment);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	result["attachments"] = attachments;
	return result;
}

Dictionary SolersObservationService::_attach_render_receipt(Dictionary p_result, const Dictionary &p_pending) {
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
	if (target != "runtime") {
		receipt["source_state"] = p_pending.get("source_state", Dictionary());
		receipt["render_state_sha256"] = p_pending.get("render_state_sha256", String());
		data["render_state"] = p_pending.get("render_state", Dictionary());
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

Dictionary SolersObservationService::_editor_3d_viewport_state() const {
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

Dictionary SolersObservationService::_render_state_for_pending(const Dictionary &p_pending) const {
	Viewport *viewport = Object::cast_to<Viewport>(ObjectDB::get_instance(ObjectID((uint64_t)(int64_t)p_pending.get("viewport_id", 0))));
	Camera3D *camera = Object::cast_to<Camera3D>(ObjectDB::get_instance(ObjectID((uint64_t)(int64_t)p_pending.get("camera_id", 0))));
	EditorInterface *editor = EditorInterface::get_singleton();
	return describe_render_state(viewport, camera, editor ? editor->get_edited_scene_root() : nullptr);
}

void SolersObservationService::_runtime_screenshot_ready(int64_t, int64_t, const String &p_path, const Rect2i &, const String &p_capture_id) {
	const Dictionary *entry = pending_captures.getptr(p_capture_id);
	// Ignore screenshots from expired requests or earlier runtime epochs.
	const Dictionary pending_data = entry ? Dictionary(entry->get("data", Dictionary())) : Dictionary();
	const uint64_t requested_epoch = (uint64_t)(int64_t)pending_data.get("runtime_epoch", 0);
	if (!entry || requested_epoch != runtime_epoch) {
		if (!p_path.is_empty()) {
			DirAccess::remove_absolute(p_path);
		}
		return;
	}
	Ref<Image> image = p_path.is_empty() ? Ref<Image>() : Image::load_from_file(p_path);
	if (!p_path.is_empty()) {
		DirAccess::remove_absolute(p_path);
	}
	// Compressed or empty images cannot serve as pixel evidence.
	if (image.is_null() || image->is_empty() || image->is_compressed()) {
		pending_captures[p_capture_id] = _capture_error("RUNTIME_CAPTURE_DECODE_FAILED", "The runtime returned an unreadable screenshot.", true);
		return;
	}
	Dictionary result = _capture_image(image, "runtime", p_capture_id);
	if ((bool)result.get("ok", false)) {
		Dictionary data = result.get("data", Dictionary());
		data["frame_valid"] = true;
		result["data"] = data;
	}
	pending_captures[p_capture_id] = _attach_render_receipt(result, pending_data);
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
Dictionary SolersObservationService::_register_pending_capture(const String &p_target, const Dictionary &p_extra) {
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
	data["runtime_epoch"] = (int64_t)runtime_epoch;
	for (const Variant *key = p_extra.next(nullptr); key; key = p_extra.next(key)) {
		data[*key] = p_extra[*key];
	}
	if (p_target == "runtime") {
		Dictionary source_state;
		source_state["runtime_epoch"] = (int64_t)runtime_epoch;
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

Dictionary SolersObservationService::_finish_frame_gated_capture(const String &p_capture_id, const Dictionary &p_data) {
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
		data["readback_render_state"] = readback_state;
		failure["data"] = data;
		return failure;
	}
	if (target == "editor") {
		Node3DEditor *editor_3d = Node3DEditor::get_singleton();
		Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
		if (!editor_viewport || !editor_viewport->get_viewport_node()) {
			return _capture_error("EDITOR_VIEWPORT_UNAVAILABLE", "The active 3D editor viewport is unavailable.", true);
		}
		Dictionary result = _capture_image(editor_viewport->get_viewport_node()->get_texture()->get_image(), "editor", p_capture_id);
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
	Dictionary result = _capture_image(viewport->get_texture()->get_image(), target, p_capture_id);
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
		for (const char *key : { "camera_path", "camera_forward_hit_node", "camera_forward_hit_distance", "orientation", "axis", "direction", "section_position", "focus_paths", "view_spec_hash" }) {
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

Dictionary SolersObservationService::_poll_pending_capture(const String &p_capture_id) {
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
			data["runtime_epoch"] = (int64_t)runtime_epoch;
			Dictionary source_state;
			source_state["runtime_epoch"] = (int64_t)runtime_epoch;
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

Dictionary SolersObservationService::_begin_scene_view_capture(const String &p_target, const Dictionary &p_args) {
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

	Dictionary extra;
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
		const String node_path = String(p_args.get("node_path", String())).strip_edges();
		if (!node_path.is_empty()) {
			scene_camera = Object::cast_to<Camera3D>(edited_root->get_node_or_null(NodePath(node_path)));
			if (!scene_camera) {
				memdelete(viewport);
				PackedStringArray camera_paths;
				_solers_collect_camera_paths(edited_root, edited_root, camera_paths);
				return _capture_error("CAMERA_NOT_FOUND", vformat("No Camera3D at node_path: %s. %s Camera3D nodes present: %s.", node_path, _solers_scene_path_hint(edited_root), camera_paths.is_empty() ? String("none") : String(", ").join(camera_paths)), true);
			}
		} else {
			scene_camera = _solers_find_first_camera(edited_root);
			if (!scene_camera) {
				memdelete(viewport);
				return _capture_error("NO_CAMERA_IN_SCENE", "The edited scene has no Camera3D. Add one or pass node_path.", true);
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
	} else { // top_down / orthographic
		AABB bounds;
		bool found = false;
		const Array focus_paths = p_args.get("focus_paths", Array());
		if (focus_paths.is_empty()) {
			_solers_accumulate_world_aabb(edited_root, bounds, found);
		} else {
			for (int i = 0; i < focus_paths.size(); i++) {
				const String focus_path = String(focus_paths[i]).strip_edges();
				Node *focus = edited_root->get_node_or_null(NodePath(focus_path));
				if (!focus) {
					memdelete(viewport);
					return _capture_error("FOCUS_NODE_NOT_FOUND", vformat("No edited-scene node at focus_paths[%d]: %s. %s", i, focus_path, _solers_scene_path_hint(edited_root)), true);
				}
				_solers_accumulate_world_aabb(focus, bounds, found);
			}
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
	extra["viewport_id"] = (int64_t)(uint64_t)viewport->get_instance_id();
	extra["camera_id"] = (int64_t)(uint64_t)camera->get_instance_id();
	extra["owns_viewport"] = true;
	extra["viewport_rid"] = (int64_t)viewport->get_viewport_rid().get_id();
	if (p_args.has("source_state")) {
		extra["source_state"] = p_args["source_state"];
	}
	return _register_pending_capture(p_target, extra);
}

Dictionary SolersObservationService::capture_viewport(const Dictionary &p_args) {
	const String requested_id = String(p_args.get("capture_id", String())).strip_edges();
	if (!requested_id.is_empty()) {
		return _poll_pending_capture(requested_id);
	}

	const String target = String(p_args.get("target", String())).strip_edges().to_lower();
	if (target != "runtime" && !_solers_capture_source_is_current(p_args.get("source_state", Dictionary()))) {
		return _capture_error("CAPTURE_SOURCE_CONFLICT", "The edited scene changed before capture started.", true);
	}
	if (p_args.has("debug_draw") && target != "camera" && target != "top_down" && target != "orthographic") {
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
		if (p_args.has("source_state")) {
			extra["source_state"] = p_args["source_state"];
		}
		return _register_pending_capture("editor", extra);
	}
	if (target == "camera" || target == "top_down" || target == "orthographic") {
		return _begin_scene_view_capture(target, p_args);
	}
	if (target == "runtime") {
		if (!_is_runtime_visual_ready()) {
			Dictionary extra;
			extra["awaiting_runtime_ready"] = true;
			extra["runtime"] = get_runtime_status();
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

Dictionary SolersObservationService::poll_viewport_capture(const Dictionary &p_args) {
	const String capture_id = String(p_args.get("capture_id", String())).strip_edges();
	if (capture_id.is_empty()) {
		return _capture_error("INVALID_CAPTURE_CONTINUATION", "capture_id is required to continue a viewport capture.", false);
	}
	return _poll_pending_capture(capture_id);
}

bool SolersObservationService::is_viewport_capture_ready(const Dictionary &p_args) {
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

bool SolersObservationService::_normalize_project_path(const String &p_path, String &r_res_path, String &r_error) const {
	String path = p_path.strip_edges().replace_char('\\', '/');
	if (path.is_empty()) {
		r_error = "Path is empty.";
		return false;
	}

	if (path.is_absolute_path() && !path.begins_with("res://")) {
		r_error = "Only res:// or project-relative paths are allowed.";
		return false;
	}

	if (!path.begins_with("res://")) {
		path = String("res://").path_join(path);
	}

	path = path.simplify_path();
	if (!path.begins_with("res://") || path.contains("..")) {
		r_error = "Path escapes the project root.";
		return false;
	}

	r_res_path = path;
	return true;
}

void SolersObservationService::_refresh_project_files() {
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!filesystem || filesystem->is_scanning() || filesystem->is_importing() || !filesystem->get_filesystem()) {
		return;
	}
	PackedStringArray snapshot;
	LocalVector<EditorFileSystemDirectory *> stack;
	stack.push_back(filesystem->get_filesystem());
	while (!stack.is_empty()) {
		EditorFileSystemDirectory *directory = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		for (int i = 0; i < directory->get_file_count(); i++) {
			snapshot.push_back(directory->get_file_path(i));
		}
		for (int i = 0; i < directory->get_subdir_count(); i++) {
			stack.push_back(directory->get_subdir(i));
		}
	}
	MutexLock lock(project_files_mutex);
	project_files = snapshot;
	project_files_ready = true;
}

static Dictionary _solers_node_identity(Node *p_node, Node *p_root) {
	Dictionary identity;
	identity["node_path"] = p_node == p_root ? String(".") : String(p_root->get_path_to(p_node));
	identity["object_id"] = solers_object_id_to_string(p_node->get_instance_id());
	identity["name"] = p_node->get_name();
	identity["class_name"] = p_node->get_class();
	identity["child_count"] = p_node->get_child_count();
	identity["scene_file_path"] = p_node->get_scene_file_path();
	const Ref<Script> script = p_node->get_script();
	if (script.is_valid()) {
		identity["script_path"] = script->get_path();
	}
	return identity;
}

Dictionary SolersObservationService::get_project_info() const {
	Dictionary info;
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(project_settings, info);

	const String project_name = GLOBAL_GET("application/config/name");
	const String main_scene = GLOBAL_GET("application/run/main_scene");

	info["name"] = project_name;
	info["resource_path"] = project_settings->get_resource_path();
	info["main_scene"] = main_scene;
	info["godot_version"] = VERSION_FULL_NAME;
	info["engine_distribution"] = "Solers Engine";
	info["project_settings_path"] = project_settings->get_resource_path().path_join("project.godot");
	return info;
}

Dictionary SolersObservationService::get_project_settings_summary() const {
	Dictionary summary;
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(project_settings, summary);

	summary["application/config/name"] = GLOBAL_GET("application/config/name");
	summary["application/run/main_scene"] = GLOBAL_GET("application/run/main_scene");
	summary["rendering/renderer/rendering_method"] = GLOBAL_GET("rendering/renderer/rendering_method");
	summary["rendering/lights_and_shadows/use_physical_light_units"] = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");
	summary["display/window/size/viewport_width"] = GLOBAL_GET("display/window/size/viewport_width");
	summary["display/window/size/viewport_height"] = GLOBAL_GET("display/window/size/viewport_height");
	summary["project_settings_path"] = project_settings->get_resource_path().path_join("project.godot");
	return summary;
}

static bool _solers_append_bounded(Array &r_results, const Dictionary &p_entry, int p_max_results, int p_token_budget, int &r_tokens);

Dictionary SolersObservationService::inspect_project_delivery(const Dictionary &p_args, int p_token_budget) const {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(settings, Dictionary());
	Array blockers;
	Array roots = p_args.get("roots", Array());
	const String main_scene = GLOBAL_GET("application/run/main_scene");
	if (!main_scene.is_empty()) {
		roots.push_back(main_scene);
	}
	List<PropertyInfo> setting_properties;
	settings->get_property_list(&setting_properties);
	for (const PropertyInfo &property : setting_properties) {
		const String name = property.name;
		if (name.begins_with("autoload/")) {
			const String path = String(settings->get_setting(name)).trim_prefix("*");
			if (!path.is_empty()) {
				roots.push_back(path);
			}
		}
	}

	Array normalized_roots;
	Vector<String> queue;
	HashSet<String> reachable;
	for (int i = 0; i < roots.size(); i++) {
		String path;
		String error;
		if (!_normalize_project_path(roots[i], path, error)) {
			blockers.push_back(Dictionary({ { "code", "INVALID_ROOT" }, { "path", roots[i] }, { "message", error } }));
			continue;
		}
		if (!reachable.has(path)) {
			reachable.insert(path);
			queue.push_back(path);
			normalized_roots.push_back(path);
		}
	}
	for (int queue_index = 0; queue_index < queue.size(); queue_index++) {
		const String path = queue[queue_index];
		if (!FileAccess::exists(path)) {
			blockers.push_back(Dictionary({ { "code", "MISSING_DEPENDENCY" }, { "path", path } }));
			continue;
		}
		List<String> dependencies;
		ResourceLoader::get_dependencies(path, &dependencies, true);
		for (const String &raw_dependency : dependencies) {
			const String dependency = ResourceUID::ensure_path(raw_dependency.get_slice("::", 0));
			if (dependency.begins_with("res://") && !reachable.has(dependency)) {
				reachable.insert(dependency);
				queue.push_back(dependency);
			}
		}
	}

	PackedStringArray files;
	{
		MutexLock lock(project_files_mutex);
		files = project_files;
	}
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!project_files_ready || !filesystem || filesystem->is_scanning() || filesystem->is_importing()) {
		blockers.push_back(Dictionary({ { "code", "PROJECT_INDEX_UNAVAILABLE" }, { "message", "EditorFileSystem has not committed a stable project index." } }));
	}
	Dictionary hashes;
	Array unreferenced;
	int report_tokens = 0;
	bool truncated = false;
	for (int i = 0; i < files.size(); i++) {
		const String path = files[i];
		int file_index = -1;
		EditorFileSystemDirectory *directory = filesystem ? filesystem->find_file(path, &file_index) : nullptr;
		if (directory && !directory->get_file_import_is_valid(file_index)) {
			blockers.push_back(Dictionary({ { "code", "INVALID_IMPORT" }, { "path", path } }));
		}
		if (FileAccess::exists(path)) {
			const String sha = FileAccess::get_sha256(path);
			Array group = hashes.get(sha, Array());
			group.push_back(path);
			hashes[sha] = group;
		}
		if (!reachable.has(path)) {
			truncated |= !_solers_append_bounded(unreferenced, Dictionary({ { "path", path } }), INT32_MAX, p_token_budget, report_tokens);
		}
	}
	Array duplicate_groups;
	for (const Variant *sha = hashes.next(nullptr); sha; sha = hashes.next(sha)) {
		const Array paths = hashes[*sha];
		if (paths.size() > 1) {
			truncated |= !_solers_append_bounded(duplicate_groups, Dictionary({ { "sha256", *sha }, { "paths", paths } }), INT32_MAX, p_token_budget, report_tokens);
		}
	}

	Array input_actions;
	if (InputMap *input_map = InputMap::get_singleton()) {
		for (const StringName &action : input_map->get_actions()) {
			const List<Ref<InputEvent>> *events = input_map->action_get_events(action);
			input_actions.push_back(Dictionary({ { "name", action }, { "event_count", events ? events->size() : 0 } }));
		}
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	Node *edited_root = EditorInterface::get_singleton() ? EditorInterface::get_singleton()->get_edited_scene_root() : nullptr;
	if (edited_root && undo_redo) {
		const int history_id = EditorNode::get_editor_data().get_current_edited_scene_history_id();
		if (history_id != EditorUndoRedoManager::INVALID_HISTORY && undo_redo->is_history_unsaved(history_id)) {
			blockers.push_back(Dictionary({ { "code", "UNSAVED_SCENE" }, { "path", edited_root->get_scene_file_path() } }));
		}
	}

	return Dictionary({ { "status", "complete" }, { "roots", normalized_roots }, { "reachable_count", reachable.size() }, { "project_file_count", files.size() }, { "input_actions", input_actions }, { "unreferenced_from_roots", unreferenced }, { "duplicate_content", duplicate_groups }, { "blockers", blockers }, { "truncated", truncated } });
}

static bool _solers_text_search_extension(const String &p_extension) {
	return p_extension == "gd" || p_extension == "cs" || p_extension == "shader" || p_extension == "gdshader" ||
			p_extension == "tscn" || p_extension == "tres" || p_extension == "cfg" || p_extension == "json" ||
			p_extension == "xml" || p_extension == "md" || p_extension == "txt" || p_extension == "cpp" ||
			p_extension == "h" || p_extension == "py";
}

static bool _solers_identifier_character(char32_t p_character) {
	return p_character == '_' || (p_character >= '0' && p_character <= '9') || (p_character >= 'A' && p_character <= 'Z') ||
			(p_character >= 'a' && p_character <= 'z') || p_character >= 128;
}

static int _solers_find_text(const String &p_line, const String &p_query, bool p_symbol) {
	const String line = p_line.to_lower();
	const String query = p_query.to_lower();
	int from = 0;
	while (from <= line.length() - query.length()) {
		const int position = line.find(query, from);
		if (position < 0 || !p_symbol) {
			return position;
		}
		const bool left_boundary = position == 0 || !_solers_identifier_character(line[position - 1]);
		const int end = position + query.length();
		const bool right_boundary = end == line.length() || !_solers_identifier_character(line[end]);
		if (left_boundary && right_boundary) {
			return position;
		}
		from = position + 1;
	}
	return -1;
}

static Dictionary _solers_project_result(const String &p_path) {
	Dictionary result;
	result["path"] = p_path;
	return result;
}

static bool _solers_append_bounded(Array &r_results, const Dictionary &p_entry, int p_max_results, int p_token_budget, int &r_tokens) {
	const int tokens = SolersContextManager::estimate_tokens(JSON::stringify(p_entry));
	if (r_results.size() >= p_max_results || (!r_results.is_empty() && r_tokens + tokens > MAX(1, p_token_budget))) {
		return false;
	}
	r_results.push_back(p_entry);
	r_tokens += tokens;
	return true;
}

static bool _solers_is_packed_scene_type(const String &p_resource_type) {
	return p_resource_type == "PackedScene";
}

Dictionary SolersObservationService::observe_path(const String &p_path) const {
	Dictionary result;
	String res_path;
	String path_error;
	if (!_normalize_project_path(p_path, res_path, path_error)) {
		result["ok"] = false;
		result["error"] = path_error;
		return result;
	}
	if (res_path != "res://" && res_path.ends_with("/")) {
		res_path = res_path.substr(0, res_path.length() - 1);
	}

	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	EditorFileSystemDirectory *efs_dir = (efs && !efs->is_scanning()) ? efs->get_filesystem_path(res_path) : nullptr;
	const bool is_directory = efs_dir != nullptr || DirAccess::open(res_path).is_valid();
	if (is_directory) {
		const int max_children = 48;
		Array children;
		Dictionary type_counts;
		int file_count = 0;
		int subdir_count = 0;
		bool truncated = false;

		if (efs_dir) {
			subdir_count = efs_dir->get_subdir_count();
			file_count = efs_dir->get_file_count();
			for (int i = 0; i < efs_dir->get_subdir_count(); i++) {
				EditorFileSystemDirectory *sub = efs_dir->get_subdir(i);
				if (!sub) {
					continue;
				}
				type_counts["directory"] = (int)type_counts.get("directory", 0) + 1;
				if (children.size() >= max_children) {
					truncated = true;
					continue;
				}
				Dictionary child;
				child["path"] = sub->get_path();
				child["kind"] = "directory";
				children.push_back(child);
			}
			for (int i = 0; i < efs_dir->get_file_count(); i++) {
				const String child_type = String(efs_dir->get_file_type(i));
				const String count_key = child_type.is_empty() ? String("file") : child_type;
				type_counts[count_key] = (int)type_counts.get(count_key, 0) + 1;
				if (children.size() >= max_children) {
					truncated = true;
					continue;
				}
				Dictionary child;
				const String child_path = efs_dir->get_file_path(i);
				child["path"] = child_path;
				child["kind"] = count_key;
				child["import_valid"] = efs_dir->get_file_import_is_valid(i);
				const ResourceUID::ID child_uid = ResourceLoader::get_resource_uid(child_path);
				if (child_uid != ResourceUID::INVALID_ID) {
					child["uid"] = ResourceUID::get_singleton()->id_to_text(child_uid);
				}
				child["dependency_count"] = efs_dir->get_file_deps(i).size();
				children.push_back(child);
			}
		} else {
			Ref<DirAccess> dir = DirAccess::open(res_path);
			if (dir.is_valid()) {
				dir->list_dir_begin();
				for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
					if (name == "." || name == "..") {
						continue;
					}
					const String child_path = res_path.path_join(name);
					if (dir->current_is_dir()) {
						subdir_count++;
						if (children.size() < max_children) {
							Dictionary child;
							child["path"] = child_path;
							child["kind"] = "directory";
							children.push_back(child);
						} else {
							truncated = true;
						}
						type_counts["directory"] = (int)type_counts.get("directory", 0) + 1;
					} else {
						file_count++;
						const String child_type = ResourceLoader::get_resource_type(child_path);
						const String kind = child_type.is_empty() ? String("file") : child_type;
						if (children.size() < max_children) {
							Dictionary child;
							child["path"] = child_path;
							child["kind"] = kind;
							children.push_back(child);
						} else {
							truncated = true;
						}
						type_counts[kind] = (int)type_counts.get(kind, 0) + 1;
					}
				}
			}
		}

		Dictionary digest;
		digest["kind"] = "directory";
		digest["path"] = res_path;
		digest["file_count"] = file_count;
		digest["subdir_count"] = subdir_count;
		digest["children"] = children;
		digest["type_counts"] = type_counts;
		digest["truncated"] = truncated || (file_count + subdir_count) > children.size();
		digest["summary"] = vformat("Directory with %d files and %d subdirectories.", file_count, subdir_count);
		result["ok"] = true;
		result["path"] = res_path;
		result["digest"] = digest;
		return result;
	}

	if (!FileAccess::exists(res_path)) {
		result["ok"] = false;
		result["error"] = vformat("Path does not exist.%s", solers_file_suggestions(res_path));
		result["path"] = res_path;
		return result;
	}

	const String resource_type = ResourceLoader::get_resource_type(res_path);
	if (_solers_is_packed_scene_type(resource_type)) {
		return digest_packed_scene(res_path, 96);
	}

	Dictionary digest;
	digest["kind"] = resource_type.is_empty() ? String("file") : resource_type;
	digest["path"] = res_path;
	if (!resource_type.is_empty()) {
		digest["resource_type"] = resource_type;
	}
	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(res_path);
	if (uid != ResourceUID::INVALID_ID) {
		digest["uid"] = ResourceUID::get_singleton()->id_to_text(uid);
	}
	digest["size_bytes"] = FileAccess::get_size(res_path);

	Array dependencies;
	List<String> dep_list;
	ResourceLoader::get_dependencies(res_path, &dep_list, true);
	const int max_deps = 32;
	int dep_i = 0;
	for (const String &raw_dependency : dep_list) {
		if (dep_i >= max_deps) {
			break;
		}
		dependencies.push_back(ResourceUID::ensure_path(raw_dependency.get_slice("::", 0)));
		dep_i++;
	}
	digest["dependencies"] = dependencies;
	digest["dependencies_truncated"] = dep_list.size() > dependencies.size();
	digest["summary"] = resource_type.is_empty()
			? vformat("File (%d bytes).", (int64_t)digest.get("size_bytes", 0))
			: vformat("%s resource (%d bytes, %d deps).", resource_type, (int64_t)digest.get("size_bytes", 0), dependencies.size());

	result["ok"] = true;
	result["path"] = res_path;
	result["digest"] = digest;
	return result;
}

Dictionary SolersObservationService::digest_packed_scene(const String &p_path, int p_max_nodes) const {
	Dictionary result;
	String res_path;
	String path_error;
	if (!_normalize_project_path(p_path, res_path, path_error)) {
		result["ok"] = false;
		result["error"] = path_error;
		return result;
	}
	const String resource_type = ResourceLoader::get_resource_type(res_path);
	if (!_solers_is_packed_scene_type(resource_type)) {
		result["ok"] = false;
		result["error"] = vformat("Not a PackedScene (resource_type=%s).", resource_type.is_empty() ? String("unknown") : resource_type);
		result["path"] = res_path;
		return result;
	}

	const Ref<PackedScene> scene = ResourceLoader::load(res_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_REUSE);
	if (scene.is_null()) {
		result["ok"] = false;
		result["error"] = "Unable to load PackedScene.";
		result["path"] = res_path;
		return result;
	}
	const Ref<SceneState> state = scene->get_state();
	if (state.is_null()) {
		result["ok"] = false;
		result["error"] = "PackedScene has no SceneState.";
		result["path"] = res_path;
		return result;
	}

	const int max_nodes = CLAMP(p_max_nodes, 1, 256);
	const int node_count = state->get_node_count();
	Array nodes;
	Array scripts;
	Array external_scenes;
	HashSet<String> seen_scripts;
	HashSet<String> seen_scenes;

	for (int i = 0; i < node_count; i++) {
		const String node_path = String(state->get_node_path(i));
		const String name = String(state->get_node_name(i));
		const String node_type = String(state->get_node_type(i));
		if (nodes.size() < max_nodes) {
			Dictionary node;
			node["path"] = node_path;
			node["name"] = name;
			node["type"] = node_type;
			if (state->is_node_instance_placeholder(i)) {
				const String placeholder = state->get_node_instance_placeholder(i);
				node["instance"] = placeholder;
				if (!placeholder.is_empty() && !seen_scenes.has(placeholder)) {
					seen_scenes.insert(placeholder);
					external_scenes.push_back(placeholder);
				}
			} else {
				const Ref<PackedScene> inst = state->get_node_instance(i);
				if (inst.is_valid()) {
					const String inst_path = inst->get_path();
					if (!inst_path.is_empty()) {
						node["instance"] = inst_path;
						if (!seen_scenes.has(inst_path)) {
							seen_scenes.insert(inst_path);
							external_scenes.push_back(inst_path);
						}
					}
				}
			}
			nodes.push_back(node);
		}
		for (int p = 0; p < state->get_node_property_count(i); p++) {
			if (state->get_node_property_name(i, p) != StringName("script")) {
				continue;
			}
			const Variant value = state->get_node_property_value(i, p);
			String script_path;
			if (value.get_type() == Variant::OBJECT) {
				const Ref<Resource> res = value;
				if (res.is_valid()) {
					script_path = res->get_path();
				}
			} else if (value.get_type() == Variant::STRING) {
				script_path = String(value);
			}
			if (!script_path.is_empty() && !seen_scripts.has(script_path)) {
				seen_scripts.insert(script_path);
				scripts.push_back(script_path);
			}
		}
	}

	Array dependencies;
	List<String> dep_list;
	ResourceLoader::get_dependencies(res_path, &dep_list, true);
	const int max_deps = 32;
	int dep_i = 0;
	for (const String &raw_dependency : dep_list) {
		if (dep_i >= max_deps) {
			break;
		}
		dependencies.push_back(ResourceUID::ensure_path(raw_dependency.get_slice("::", 0)));
		dep_i++;
	}

	Dictionary digest;
	digest["kind"] = "PackedScene";
	digest["path"] = res_path;
	digest["resource_type"] = resource_type;
	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(res_path);
	if (uid != ResourceUID::INVALID_ID) {
		digest["uid"] = ResourceUID::get_singleton()->id_to_text(uid);
	}
	if (node_count > 0) {
		digest["root_name"] = String(state->get_node_name(0));
		digest["root_type"] = String(state->get_node_type(0));
	}
	digest["node_count"] = node_count;
	digest["nodes_reported"] = nodes.size();
	digest["nodes_truncated"] = node_count > nodes.size();
	digest["nodes"] = nodes;
	digest["scripts"] = scripts;
	digest["external_scenes"] = external_scenes;
	digest["dependencies"] = dependencies;
	digest["dependencies_truncated"] = dep_list.size() > dependencies.size();
	digest["summary"] = vformat("PackedScene root=%s (%s), %d nodes, %d scripts, %d external scenes.",
			String(digest.get("root_name", String())), String(digest.get("root_type", String())), node_count, scripts.size(), external_scenes.size());

	result["ok"] = true;
	result["path"] = res_path;
	result["digest"] = digest;
	return result;
}

Dictionary SolersObservationService::search_project(const Dictionary &p_args, int p_token_budget) const {
	const String type = String(p_args.get("type", "path")).to_lower();
	const String query = String(p_args.get("query", String())).strip_edges();
	const int max_results = p_args.has("max_results") ? MAX(1, (int)p_args["max_results"]) : INT32_MAX;
	const int cursor = MAX(0, (int)p_args.get("cursor", 0));
	Array results;
	int result_tokens = 0;
	int scanned = 0;
	bool truncated = false;
	bool available = true;

	if (type == "path") {
		PackedStringArray files;
		{
			MutexLock lock(project_files_mutex);
			files = project_files;
			available = project_files_ready;
		}
		int match_index = 0;
		for (int i = 0; i < files.size(); i++) {
			scanned++;
			if (String(files[i]).findn(query) < 0 || match_index++ < cursor) {
				continue;
			}
			if (!_solers_append_bounded(results, _solers_project_result(files[i]), max_results, p_token_budget, result_tokens)) {
				truncated = true;
				break;
			}
		}
	} else {
		PackedStringArray files;
		{
			MutexLock lock(project_files_mutex);
			files = project_files;
			available = project_files_ready;
		}
		if (type == "text" || type == "symbol") {
			int match_index = 0;
			for (int i = 0; i < files.size(); i++) {
				const String path = files[i];
				if (!_solers_text_search_extension(path.get_extension().to_lower())) {
					continue;
				}
				scanned++;
				Error open_error = OK;
				Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ, &open_error);
				if (file.is_null() || open_error != OK) {
					continue;
				}
				int line_index = 0;
				while (!file->eof_reached()) {
					const String line = file->get_line();
					const int column = _solers_find_text(line, query, type == "symbol");
					if (column < 0) {
						line_index++;
						continue;
					}
					if (match_index++ < cursor) {
						line_index++;
						continue;
					}
					Dictionary match = _solers_project_result(path);
					match["line"] = line_index + 1;
					match["column"] = column + 1;
					match["preview"] = line.strip_edges().left(240);
					if (!_solers_append_bounded(results, match, max_results, p_token_budget, result_tokens)) {
						truncated = true;
						break;
					}
					line_index++;
				}
				if (truncated) {
					break;
				}
			}
		}
	}

	Dictionary result;
	result["available"] = available;
	if (!available) {
		result["code"] = "EDITOR_INDEX_UPDATING";
	}
	result["type"] = type;
	result["query"] = query;
	result["results"] = results;
	result["count"] = results.size();
	result["cursor"] = cursor;
	if (truncated) {
		result["next_cursor"] = cursor + results.size();
	}
	result["scanned_count"] = scanned;
	result["truncated"] = truncated;
	return result;
}

Dictionary SolersObservationService::read_project_file(const String &p_path, int p_line_start, int p_line_count, bool p_raw, int p_token_budget) const {
	Dictionary result;
	String res_path;
	String path_error;
	if (!_normalize_project_path(p_path, res_path, path_error)) {
		result["ok"] = false;
		result["error"] = path_error;
		return result;
	}

	if (!FileAccess::exists(res_path)) {
		result["ok"] = false;
		result["error"] = vformat("File does not exist.%s", solers_file_suggestions(res_path));
		result["path"] = res_path;
		return result;
	}

	const String resource_type = p_raw ? String() : ResourceLoader::get_resource_type(res_path);
	if (_solers_is_packed_scene_type(resource_type)) {
		const Dictionary observed = observe_path(res_path);
		result["ok"] = false;
		result["code"] = "SCENE_TEXT_DENIED";
		result["error"] = "PackedScene source text is not the scene fact model. The digest field is the complete answer for structure/identity — do not retry with raw=true. Use object.query target=scene for live details. Pass raw=true only when editing .tscn/.scn source text.";
		result["path"] = res_path;
		result["resource_type"] = resource_type;
		result["recovery"] = "Use the included digest or object.query target=scene. Do not call project.read_file with raw=true for observation.";
		if ((bool)observed.get("ok", false) && observed.has("digest")) {
			result["digest"] = observed["digest"];
		}
		return result;
	}

	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(res_path, FileAccess::READ, &err);
	if (file.is_null() || err != OK) {
		result["ok"] = false;
		result["error"] = "Unable to open file for reading.";
		result["path"] = res_path;
		return result;
	}

	const int64_t length = file->get_length();
	const int line_start = MAX(1, p_line_start);
	const int line_count = MAX(1, p_line_count);
	int line = 1;
	while (line < line_start && !file->eof_reached()) {
		static_cast<void>(file->get_line());
		line++;
	}
	String content;
	int returned = 0;
	while (returned < line_count && !file->eof_reached()) {
		const String next = file->get_line();
		const String candidate = content + (content.is_empty() ? String() : "\n") + next;
		if (!content.is_empty() && SolersContextManager::estimate_tokens(candidate) > MAX(1, p_token_budget)) {
			break;
		}
		content = candidate;
		returned++;
		line++;
	}
	result["ok"] = true;
	result["path"] = res_path;
	result["size_bytes"] = length;
	result["sha256"] = FileAccess::get_sha256(res_path);
	result["line_start"] = line_start;
	result["line_end"] = line - 1;
	result["content"] = content;
	result["truncated"] = !file->eof_reached();
	if (!file->eof_reached()) {
		result["next_line"] = line;
	}
	return result;
}

Dictionary SolersObservationService::get_open_scenes() const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	PackedStringArray paths = editor_interface->get_open_scenes();
	TypedArray<Node> roots = editor_interface->get_open_scene_roots();
	Node *edited_root = editor_interface->get_edited_scene_root();

	Array path_array;
	for (int i = 0; i < paths.size(); i++) {
		path_array.push_back(paths[i]);
	}

	result["count"] = roots.size();
	result["paths"] = path_array;
	Array identities;
	for (int i = 0; i < roots.size(); i++) {
		Node *root = Object::cast_to<Node>(roots[i]);
		if (root) {
			identities.push_back(_solers_node_identity(root, root));
		}
	}
	result["roots"] = identities;
	result["current_scene_path"] = edited_root ? edited_root->get_scene_file_path() : String();
	return result;
}

Dictionary SolersObservationService::get_selection() const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	EditorSelection *selection = editor_interface->get_selection();
	if (!selection) {
		result["count"] = 0;
		result["nodes"] = Array();
		return result;
	}

	TypedArray<Node> selected_nodes = selection->get_selected_nodes();
	Node *edited_root = editor_interface->get_edited_scene_root();
	result["count"] = selected_nodes.size();
	Array identities;
	for (int i = 0; edited_root && i < selected_nodes.size(); i++) {
		Node *node = Object::cast_to<Node>(selected_nodes[i]);
		if (node) {
			identities.push_back(_solers_node_identity(node, edited_root));
		}
	}
	result["nodes"] = identities;
	return result;
}

Dictionary SolersObservationService::query_scene_nodes(const Dictionary &p_args, int p_token_budget) const {
	Dictionary result;
	Node *edited_root = SceneTree::get_singleton()->get_edited_scene_root();
	ERR_FAIL_NULL_V(edited_root, result);

	const Array requested_paths = p_args.get("node_paths", Array());
	const String path_prefix = p_args.get("path_prefix", String());
	const String name_contains = p_args.get("name_contains", String());
	const String class_name = p_args.get("class_name", String());
	const String script_path = p_args.get("script_path", String());
	const int cursor = MAX(0, (int)p_args.get("cursor", 0));
	const int max_results = p_args.has("max_results") ? MAX(1, (int)p_args["max_results"]) : INT32_MAX;
	Vector<Node *> pending;
	Array errors;
	if (requested_paths.is_empty()) {
		pending.push_back(edited_root);
	} else {
		for (int i = requested_paths.size() - 1; i >= 0; i--) {
			const String path = requested_paths[i];
			Node *node = edited_root->get_node_or_null(NodePath(path));
			if (node) {
				pending.push_back(node);
			} else {
				errors.push_back(Dictionary({ { "index", i }, { "requested_path", path }, { "code", "NODE_NOT_FOUND" } }));
			}
		}
	}

	Array nodes;
	int matched = 0;
	int tokens = 0;
	bool has_more = false;
	while (!pending.is_empty()) {
		Node *node = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		if (requested_paths.is_empty()) {
			for (int child = node->get_child_count() - 1; child >= 0; child--) {
				pending.push_back(node->get_child(child));
			}
		}
		const String node_path = node == edited_root ? "." : String(edited_root->get_path_to(node));
		Ref<Script> script = node->get_script();
		const String node_script_path = script.is_valid() ? script->get_path() : String();
		if ((!path_prefix.is_empty() && node_path != path_prefix && !node_path.begins_with(path_prefix.trim_suffix("/") + "/")) ||
				(!name_contains.is_empty() && String(node->get_name()).findn(name_contains) < 0) ||
				(!class_name.is_empty() && !node->is_class(class_name)) ||
				(!script_path.is_empty() && node_script_path != script_path)) {
			continue;
		}
		if (matched++ < cursor) {
			continue;
		}
		Dictionary entry = _solers_node_identity(node, edited_root);
		if (!_solers_append_bounded(nodes, entry, max_results, p_token_budget, tokens)) {
			has_more = true;
			break;
		}
	}
	result["nodes"] = nodes;
	result["errors"] = errors;
	result["cursor"] = cursor;
	result["count"] = nodes.size();
	if (has_more) {
		result["next_cursor"] = cursor + nodes.size();
	}
	return result;
}

Dictionary SolersObservationService::get_runtime_status() const {
	Dictionary result;
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	const bool playing = run_bar && run_bar->is_playing();
	result["is_playing"] = playing;
	result["playing_scene"] = playing ? run_bar->get_playing_scene() : String();
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	result["debugger_connected"] = debugger && debugger->is_session_active();
	result["is_breaked"] = debugger && debugger->is_breaked();
	result["remote_pid"] = debugger ? debugger->get_remote_pid() : 0;
	result["error_count"] = debugger ? debugger->get_error_count() : 0;
	result["warning_count"] = debugger ? debugger->get_warning_count() : 0;
	result["runtime_epoch"] = (int64_t)runtime_epoch;
	result["capture_ready"] = _is_runtime_visual_ready();
	return result;
}

void SolersObservationService::_append_runtime_event(const StringName &p_type, const Dictionary &p_data, bool p_persist) {
	Dictionary event = p_data.duplicate(true);
	event["type"] = p_type;
	event["cursor"] = (int64_t)++runtime_cursor;
	event["runtime_epoch"] = (int64_t)runtime_epoch;
	event["ticks_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	runtime_events.push_back(event);
	if (runtime_events.size() > SOLERS_RUNTIME_EVENT_LIMIT) {
		runtime_events.remove_at(0);
	}
	if (p_persist) {
		Dictionary audit;
		audit["event_type"] = "runtime_observation";
		ProjectSettings *project_settings = ProjectSettings::get_singleton();
		audit["project_path"] = project_settings ? project_settings->get_resource_path() : String();
		audit["observation"] = event;
		solers_transcript_write(audit);
	}
}

bool SolersObservationService::_is_runtime_visual_ready() const {
	if (!GameViewDebugger::has_active_capture_session()) {
		return false;
	}
	for (int i = runtime_events.size() - 1; i >= 0; i--) {
		const Dictionary event = runtime_events[i];
		if ((uint64_t)(int64_t)event.get("runtime_epoch", 0) != runtime_epoch) {
			continue;
		}
		if (StringName(event.get("type", String())) == SNAME("started")) {
			return true;
		}
	}
	return false;
}

bool SolersObservationService::_request_runtime_screenshot(const String &p_capture_id) {
	return GameViewDebugger::request_root_viewport_screenshot(callable_mp(this, &SolersObservationService::_runtime_screenshot_ready).bind(p_capture_id));
}

bool SolersObservationService::_request_runtime_frame(const String &p_call_id, const Array &p_focus_paths) {
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	if (!debugger || !debugger->is_session_active() || p_call_id.is_empty()) {
		return false;
	}
	debugger->send_message("solers:observe_frame", { p_call_id, (int64_t)runtime_epoch, p_focus_paths });
	return true;
}

bool SolersObservationService::_request_runtime_objects(const String &p_call_id, const Array &p_requests) {
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	if (!debugger || !debugger->is_session_active() || p_call_id.is_empty()) {
		return false;
	}
	debugger->send_message("solers:observe_objects", { p_call_id, (int64_t)runtime_epoch, p_requests });
	return true;
}

void SolersObservationService::_runtime_started() {
	runtime_epoch++;
	runtime_query.clear();
	runtime_control_result.clear();
	runtime_object_cache.clear();
	runtime_stack_frames.clear();
	performance_sample_cursor = 0;
	_append_runtime_event(SNAME("started"), Dictionary(), true);
}

void SolersObservationService::_runtime_stopped() {
	performance_capture_active = false;
	performance_monitor_names.clear();
	performance_monitor_types.clear();
	runtime_query.clear();
	runtime_control_result.clear();
	runtime_object_cache.clear();
	runtime_stack_frames.clear();
	performance_sample_cursor = 0;
	_append_runtime_event(SNAME("stopped"), Dictionary(), true);
}

void SolersObservationService::_runtime_output(const String &p_message, int p_level) {
	Dictionary data;
	data["message"] = p_message.left(4096);
	data["level"] = p_level;
	data["truncated"] = p_message.length() > 4096;
	// Ring keeps the evidence; transcript does not need per-line floods.
	_append_runtime_event(SNAME("output"), data, false);
}

void SolersObservationService::_runtime_breaked(bool p_breaked, bool p_can_debug, const String &p_reason, bool p_has_stackdump) {
	Dictionary data;
	data["breaked"] = p_breaked;
	data["can_debug"] = p_can_debug;
	data["reason"] = p_reason;
	data["has_stackdump"] = p_has_stackdump;
	if (!p_breaked) {
		runtime_stack_frames.clear();
	}
	_append_runtime_event(SNAME("break"), data, false);
}

static Variant _solers_bounded_runtime_value(const Variant &p_value) {
	const String encoded = JSON::stringify(p_value, "", false, true);
	if (encoded.utf8().length() <= 4096) {
		return p_value;
	}
	Dictionary summary;
	summary["type"] = Variant::get_type_name(p_value.get_type());
	summary["preview"] = encoded.left(4096);
	summary["truncated"] = true;
	return summary;
}

void SolersObservationService::_runtime_debug_data(const String &p_message, const Array &p_data) {
	if (p_message == "solers:input_result" && p_data.size() == 1 && p_data[0].get_type() == Variant::DICTIONARY) {
		runtime_control_result = Dictionary(p_data[0]).duplicate(true);
		return;
	}
	if (p_message == "solers:frame_result" && p_data.size() == 1 && p_data[0].get_type() == Variant::DICTIONARY) {
		Dictionary frame = Dictionary(p_data[0]).duplicate(true);
		if ((uint64_t)(int64_t)frame.get("runtime_epoch", 0) != runtime_epoch) {
			return;
		}
		const String call_id = frame.get("call_id", String());
		if (String(runtime_query.get("call_id", String())) == call_id && runtime_query.get("target", String()) == "spatial") {
			frame["available"] = frame.get("ok", false);
			_finish_runtime_query(frame);
		}
		return;
	}
	if (p_message == "solers:objects_result" && p_data.size() == 1 && p_data[0].get_type() == Variant::DICTIONARY) {
		const Dictionary observed = p_data[0];
		if ((uint64_t)(int64_t)observed.get("runtime_epoch", 0) != runtime_epoch || String(observed.get("call_id", String())) != String(runtime_query.get("call_id", String())) || runtime_query.get("target", String()) != "scene" || runtime_query.get("stage", String()) != "objects") {
			return;
		}
		if (!(bool)observed.get("ok", false)) {
			Dictionary result;
			result["available"] = false;
			result["reason"] = observed.get("code", "runtime_observation_failed");
			result["errors"] = Array({ observed });
			_finish_runtime_query(result);
			return;
		}
		const Dictionary handles = runtime_query.get("handles", Dictionary());
		const Array requested_ids = runtime_query.get("object_ids", Array());
		Array nodes;
		Array errors = runtime_query.get("errors", Array());
		const Array native_errors = observed.get("errors", Array());
		errors.append_array(native_errors);
		Dictionary found;
		for (int i = 0; i < native_errors.size(); i++) {
			const String key = Dictionary(native_errors[i]).get("object_id", String());
			if (!key.is_empty()) {
				found[key] = true;
			}
		}
		const Array observed_nodes = observed.get("nodes", Array());
		for (int i = 0; i < observed_nodes.size(); i++) {
			const Dictionary native_node = observed_nodes[i];
			const String key = native_node.get("object_id", String());
			Dictionary entry = handles.get(key, Dictionary());
			if (entry.is_empty()) {
				continue;
			}
			const Dictionary exact = native_node.get("properties", Dictionary());
			const Dictionary property_info = native_node.get("property_info", Dictionary());
			const String observation_id = (String::num_uint64(runtime_epoch) + "\n" + String(entry.get("node_path", String())) + "\n" + key + "\n" + JSON::stringify(exact, "", false, true)).sha256_text();
			runtime_object_cache[key] = Dictionary({ { "runtime_epoch", (int64_t)runtime_epoch }, { "node_path", entry.get("node_path", String()) }, { "properties", exact }, { "property_info", property_info }, { "observation_id", observation_id } });
			Dictionary projected;
			const Array property_names = exact.keys();
			for (int property_index = 0; property_index < MIN(property_names.size(), SOLERS_RUNTIME_PROPERTY_LIMIT); property_index++) {
				const Variant property = property_names[property_index];
				projected[property] = _solers_bounded_runtime_value(solers_summarize_display_value(exact[property]));
			}
			entry["properties"] = projected;
			entry["property_info"] = property_info;
			entry["owner_path"] = native_node.get("owner_path", String());
			entry["scene_file_path"] = native_node.get("scene_file_path", String());
			entry["observation_id"] = observation_id;
			nodes.push_back(entry);
			found[key] = true;
		}
		for (int i = 0; i < requested_ids.size(); i++) {
			const String key = requested_ids[i];
			if (!found.has(key)) {
				const Dictionary handle = handles.get(key, Dictionary());
				errors.push_back(Dictionary({ { "node_path", handle.get("node_path", String()) }, { "object_id", key }, { "reason", "object_not_returned" } }));
			}
		}
		_finish_runtime_query(Dictionary({ { "available", true }, { "nodes", nodes }, { "errors", errors } }));
		return;
	}
	if (p_message == "error") {
		DebuggerMarshalls::OutputError output_error;
		if (output_error.deserialize(p_data)) {
			Dictionary event;
			event["error"] = output_error.error;
			event["details"] = output_error.error_descr;
			event["source_file"] = output_error.source_file;
			event["source_function"] = output_error.source_func;
			event["source_line"] = output_error.source_line;
			event["warning"] = output_error.warning;
			_append_runtime_event(SNAME("error"), event, false);
			return;
		}
	}
	if (p_message == "performance:profile_names" && p_data.size() == 2) {
		performance_monitor_names = p_data[0];
		performance_monitor_types = p_data[1];
		return;
	}
	if (p_message == "performance:profile_frame") {
		Dictionary event;
		Array samples;
		const int sample_count = MIN(p_data.size(), 128);
		for (int i = 0; i < sample_count; i++) {
			Dictionary sample;
			sample["name"] = i < performance_monitor_names.size() ? performance_monitor_names[i] : Variant(i);
			sample["value"] = p_data[i];
			if (i < performance_monitor_types.size()) {
				sample["type"] = performance_monitor_types[i];
			}
			samples.push_back(sample);
		}
		event["samples"] = samples;
		event["truncated"] = sample_count < p_data.size();
		_append_runtime_event(SNAME("performance"), event);
		performance_sample_cursor = runtime_cursor;
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		if (performance_capture_active && debugger && debugger->is_session_active()) {
			debugger->toggle_profiler("performance", false, Array());
		}
		performance_capture_active = false;
		return;
	}
}

#ifdef DEBUG_ENABLED
Array SolersObservationService::project_runtime_tree(const SceneDebuggerTree &p_tree, uint64_t p_epoch) {
	Array nodes;
	LocalVector<String> parent_paths;
	LocalVector<int> parent_children;
	for (const SceneDebuggerTree::RemoteNode &node : p_tree.nodes) {
		String parent_path;
		if (!parent_paths.is_empty()) {
			const uint32_t parent = parent_paths.size() - 1;
			parent_path = parent_paths[parent];
			if (--parent_children[parent] == 0) {
				parent_paths.resize(parent);
				parent_children.resize(parent);
			}
		}
		Dictionary entry;
		entry["runtime_epoch"] = (int64_t)p_epoch;
		entry["node_path"] = parent_path + "/" + node.name;
		entry["name"] = node.name;
		entry["class_name"] = node.type_name;
		entry["object_id"] = solers_object_id_to_string(ObjectID(node.id));
		entry["child_count"] = node.child_count;
		entry["scene_file_path"] = node.scene_file_path;
		nodes.push_back(entry);
		if (node.child_count > 0) {
			parent_paths.push_back(entry["node_path"]);
			parent_children.push_back(node.child_count);
		}
	}
	return nodes;
}
#endif

void SolersObservationService::_runtime_tree_updated() {
#ifdef DEBUG_ENABLED
	if (runtime_query.get("target", String()) != "scene" || runtime_query.get("stage", String()) != "tree") {
		return;
	}
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const SceneDebuggerTree *tree = debugger ? debugger->get_remote_tree() : nullptr;
	Dictionary result;
	Array nodes;
	Dictionary handles;
	Array errors;
	if (tree) {
		const Array requested_paths = runtime_query.get("node_paths", Array());
		const String path_prefix = runtime_query.get("path_prefix", String());
		const String name_contains = runtime_query.get("name_contains", String());
		const String class_name = runtime_query.get("class_name", String());
		const int cursor = MAX(0, (int)runtime_query.get("cursor", 0));
		const int max_results = runtime_query.has("max_results") ? MAX(1, (int)runtime_query["max_results"]) : INT32_MAX;
		const int token_budget = MAX(1, runtime_result_token_budget);
		const Array projected = project_runtime_tree(*tree, runtime_epoch);
		Dictionary found_paths;
		int matched = 0;
		int tokens = 0;
		bool has_more = false;
		for (int i = 0; i < projected.size(); i++) {
			const Dictionary entry = projected[i];
			const String node_path = entry.get("node_path", String());
			const bool selected = requested_paths.is_empty() ? (path_prefix.is_empty() || node_path == path_prefix || node_path.begins_with(path_prefix.trim_suffix("/") + "/")) &&
							(name_contains.is_empty() || String(entry.get("name", String())).findn(name_contains) >= 0) &&
							(class_name.is_empty() || String(entry.get("class_name", String())) == class_name)
															 : requested_paths.has(node_path);
			if (!selected) {
				continue;
			}
			found_paths[node_path] = true;
			if (matched++ < cursor) {
				continue;
			}
			if (!_solers_append_bounded(nodes, entry, max_results, token_budget, tokens)) {
				has_more = true;
				break;
			}
			handles[String(entry.get("object_id", String()))] = entry;
		}
		if (!requested_paths.is_empty()) {
			for (int i = 0; i < requested_paths.size(); i++) {
				if (!found_paths.has(requested_paths[i])) {
					Dictionary error;
					error["node_path"] = requested_paths[i];
					error["reason"] = "node_not_found";
					errors.push_back(error);
				}
			}
		}
		result["cursor"] = cursor;
		if (has_more) {
			result["next_cursor"] = cursor + nodes.size();
		}
	}
	result["available"] = tree != nullptr;
	result["nodes"] = nodes;
	result["errors"] = errors;
	if (!tree || nodes.is_empty()) {
		_finish_runtime_query(result);
		return;
	}
	runtime_query["stage"] = "objects";
	runtime_query["handles"] = handles;
	runtime_query["errors"] = errors;
	Array object_ids;
	Array requests;
	const Array properties = runtime_query.get("properties", Array());
	for (int i = 0; i < nodes.size(); i++) {
		const Dictionary node = nodes[i];
		const String object_id = node.get("object_id", String());
		object_ids.push_back(object_id);
		requests.push_back(Dictionary({ { "object_id", object_id }, { "node_path", node.get("node_path", String()) }, { "properties", properties } }));
	}
	runtime_query["object_ids"] = object_ids;
	runtime_query["call_id"] = "objects_" + String::num_uint64(runtime_query_sequence);
	if (!_request_runtime_objects(runtime_query.get("call_id", String()), requests)) {
		_finish_runtime_query(Dictionary({ { "available", false }, { "nodes", Array() }, { "errors", errors }, { "reason", "runtime_not_connected" } }));
	}
#endif
}

void SolersObservationService::_runtime_stack_dump(const Array &p_frames) {
	runtime_stack_frames.clear();
	for (int i = 0; i < MIN(p_frames.size(), 64); i++) {
		runtime_stack_frames.push_back(p_frames[i]);
	}
}

void SolersObservationService::_finish_runtime_query(Dictionary p_result) {
	if (runtime_query.is_empty()) {
		return;
	}
	const String target = runtime_query.get("target", String());
	p_result["target"] = target;
	p_result["runtime_epoch"] = (int64_t)runtime_epoch;
	runtime_query["result"] = _runtime_observation_result(p_result);
}

Dictionary SolersObservationService::_runtime_observation_result(Dictionary p_result) const {
	if (String(p_result.get("status", String())) == "pending") {
		return p_result;
	}
	const bool source_available = p_result.get("available", true);
	Array missing = p_result.get("errors", Array());
	const String reason = p_result.get("reason", String());
	if (!reason.is_empty()) {
		missing.push_back(Dictionary({ { "reason", reason } }));
	}
	Dictionary availability;
	availability["state"] = !source_available ? "unavailable" : missing.is_empty() ? "complete"
																				   : "partial";
	availability["missing"] = missing;
	p_result.erase("available");
	p_result["status"] = "complete";
	p_result["availability"] = availability;
	return p_result;
}

void SolersObservationService::_bind_runtime_debugger() {
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const ObjectID debugger_id = debugger ? debugger->get_instance_id() : ObjectID();
	if (debugger_id == observed_debugger_id) {
		return;
	}
	if (ScriptEditorDebugger *previous = Object::cast_to<ScriptEditorDebugger>(ObjectDB::get_instance(observed_debugger_id))) {
		previous->disconnect(SNAME("started"), callable_mp(this, &SolersObservationService::_runtime_started));
		previous->disconnect(SNAME("stopped"), callable_mp(this, &SolersObservationService::_runtime_stopped));
		previous->disconnect(SNAME("output"), callable_mp(this, &SolersObservationService::_runtime_output));
		previous->disconnect(SNAME("breaked"), callable_mp(this, &SolersObservationService::_runtime_breaked));
		previous->disconnect(SNAME("debug_data"), callable_mp(this, &SolersObservationService::_runtime_debug_data));
		previous->disconnect(SNAME("remote_tree_updated"), callable_mp(this, &SolersObservationService::_runtime_tree_updated));
		previous->disconnect(SNAME("stack_dump"), callable_mp(this, &SolersObservationService::_runtime_stack_dump));
	}
	observed_debugger_id = debugger_id;
	if (!debugger) {
		return;
	}
	debugger->connect(SNAME("started"), callable_mp(this, &SolersObservationService::_runtime_started));
	debugger->connect(SNAME("stopped"), callable_mp(this, &SolersObservationService::_runtime_stopped));
	debugger->connect(SNAME("output"), callable_mp(this, &SolersObservationService::_runtime_output));
	debugger->connect(SNAME("breaked"), callable_mp(this, &SolersObservationService::_runtime_breaked));
	debugger->connect(SNAME("debug_data"), callable_mp(this, &SolersObservationService::_runtime_debug_data));
	debugger->connect(SNAME("remote_tree_updated"), callable_mp(this, &SolersObservationService::_runtime_tree_updated));
	debugger->connect(SNAME("stack_dump"), callable_mp(this, &SolersObservationService::_runtime_stack_dump));
	if (debugger->is_session_active()) {
		runtime_epoch++;
		_append_runtime_event(SNAME("started"), Dictionary(), true);
	}
}

void SolersObservationService::poll() {
	if (!project_files_ready) {
		_refresh_project_files();
	}
	_bind_runtime_debugger();
}

bool SolersObservationService::is_runtime_observation_ready(const Dictionary &p_args) const {
	const uint64_t request_id = (int64_t)p_args.get("_runtime_request", 0);
	if (request_id > 0) {
		return (uint64_t)(int64_t)runtime_query.get("_runtime_request", 0) != request_id || runtime_query.has("result") ||
				(uint64_t)(int64_t)p_args.get("_runtime_epoch", 0) != runtime_epoch ||
				OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)p_args.get("deadline_msec", 0);
	}
	if (String(p_args.get("target", "events")) != "performance") {
		return true;
	}
	const uint64_t since_cursor = (int64_t)p_args.get("since_cursor", 0);
	if (performance_sample_cursor > since_cursor) {
		return true;
	}
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	return !debugger || !debugger->is_session_active() || OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)p_args.get("deadline_msec", 0);
}

Dictionary SolersObservationService::observe_runtime(const Dictionary &p_args, int p_token_budget) {
	const String target = p_args.get("target", "events");
	if (target == "scene" || target == "spatial") {
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		auto unavailable = [this, &target](const String &p_reason) {
			Dictionary result;
			result["target"] = target;
			result["available"] = false;
			result["reason"] = p_reason;
			result["runtime"] = get_runtime_status();
			return _runtime_observation_result(result);
		};
		const uint64_t request_id = (int64_t)p_args.get("_runtime_request", 0);
		if (request_id > 0) {
			if ((uint64_t)(int64_t)runtime_query.get("_runtime_request", 0) == request_id && runtime_query.has("result")) {
				Dictionary result = Dictionary(runtime_query["result"]).duplicate(true);
				result["runtime"] = get_runtime_status();
				runtime_query.clear();
				return result;
			}
			if ((uint64_t)(int64_t)runtime_query.get("_runtime_request", 0) != request_id || !debugger || !debugger->is_session_active() || (uint64_t)(int64_t)p_args.get("_runtime_epoch", 0) != runtime_epoch || OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)p_args.get("deadline_msec", 0)) {
				runtime_query.clear();
				return unavailable(!debugger || !debugger->is_session_active() ? "runtime_not_connected" : "query_expired");
			}
			return Dictionary({ { "target", target }, { "status", "pending" }, { "poll_args", p_args } });
		}
		const Array focus_paths = p_args.get("focus_paths", Array());
		if (target == "spatial" && focus_paths.is_empty()) {
			return unavailable("focus_paths_required");
		}
		if (!debugger || !debugger->is_session_active()) {
			return unavailable("runtime_not_connected");
		}
		if (!runtime_query.is_empty()) {
			return unavailable("query_in_progress");
		}
		runtime_query = p_args.duplicate(true);
		runtime_query["target"] = target;
		runtime_query["_runtime_request"] = (int64_t)++runtime_query_sequence;
		runtime_query["_runtime_epoch"] = (int64_t)runtime_epoch;
		runtime_query["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + SOLERS_RUNTIME_QUERY_TIMEOUT_MSEC);
		if (target == "scene") {
			runtime_query["stage"] = "tree";
			runtime_result_token_budget = MAX(1, p_token_budget);
			debugger->request_remote_tree();
			return Dictionary({ { "target", target }, { "status", "pending" }, { "poll_args", runtime_query.duplicate(true) } });
		}
		runtime_query["call_id"] = "spatial_" + String::num_uint64(runtime_query_sequence);
		if (!_request_runtime_frame(runtime_query.get("call_id", String()), focus_paths)) {
			runtime_query.clear();
			return unavailable("runtime_not_connected");
		}
		return Dictionary({ { "target", target }, { "status", "pending" }, { "poll_args", runtime_query.duplicate(true) } });
	}
	if (target == "stack") {
		const Dictionary status = get_runtime_status();
		Dictionary result;
		result["target"] = target;
		result["available"] = status.get("is_breaked", false);
		result["runtime_epoch"] = (int64_t)runtime_epoch;
		result["frames"] = runtime_stack_frames;
		if (!(bool)status.get("is_breaked", false)) {
			result["reason"] = "runtime_not_breaked";
		}
		return _runtime_observation_result(result);
	}
	const uint64_t since_cursor = p_args.has("since_cursor") ? (int64_t)p_args["since_cursor"] : (target == "performance" ? runtime_cursor : 0);
	const bool include_events = (bool)p_args.get("include_events", false);
	const int max_events = CLAMP((int)p_args.get("max_events", include_events ? 32 : 0), 0, 256);
	Array events;
	Dictionary error_digest;
	uint64_t consumed_cursor = since_cursor;
	bool truncated = false;
	int64_t epoch_error_count = 0;

	for (int i = 0; i < runtime_events.size(); i++) {
		const Dictionary event = runtime_events[i];
		const uint64_t event_cursor = (int64_t)event.get("cursor", 0);
		const uint64_t event_epoch = (int64_t)event.get("runtime_epoch", 0);
		if (event_cursor <= since_cursor) {
			continue;
		}
		if (include_events && max_events > 0 && events.size() >= max_events) {
			truncated = true;
			break;
		}
		if (event_epoch != runtime_epoch) {
			consumed_cursor = event_cursor;
			continue;
		}
		consumed_cursor = event_cursor;

		const StringName event_type = StringName(event.get("type", String()));
		if (event_type == SNAME("error") && !(bool)event.get("warning", false)) {
			epoch_error_count++;
			const String message = String(event.get("error", String())) + "\n" + String(event.get("details", String()));
			const String fingerprint = String::num_uint64((String(event_type) + String::chr(0x1f) + message.strip_edges()).hash64());
			Dictionary entry = error_digest.has(fingerprint) ? Dictionary(error_digest[fingerprint]) : Dictionary();
			entry["fingerprint"] = fingerprint;
			entry["count"] = (int64_t)entry.get("count", 0) + 1;
			entry["last_cursor"] = (int64_t)event_cursor;
			entry["sample"] = event;
			error_digest[fingerprint] = entry;
		}

		if (include_events && max_events > 0) {
			events.push_back(event);
		}
	}

	Dictionary result;
	result["target"] = target;
	result["runtime"] = get_runtime_status();
	result["cursor"] = (int64_t)consumed_cursor;
	result["runtime_epoch"] = (int64_t)runtime_epoch;
	result["error_digest"] = error_digest;
	result["epoch_error_count"] = epoch_error_count;
	result["events"] = events;
	result["truncated"] = truncated;
	if (target == "performance" && performance_sample_cursor <= since_cursor) {
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		if (!debugger || !debugger->is_session_active()) {
			result["performance_unavailable"] = "runtime_not_connected";
		} else {
			Dictionary poll_args = p_args.duplicate(true);
			poll_args["since_cursor"] = (int64_t)since_cursor;
			const uint64_t deadline = (int64_t)p_args.get("deadline_msec", 0);
			if (deadline == 0) {
				poll_args["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + 3000);
			}
			if (OS::get_singleton()->get_ticks_msec() < (uint64_t)(int64_t)poll_args.get("deadline_msec", 0)) {
				if (!performance_capture_active) {
					debugger->toggle_profiler("performance", true, Array());
					performance_capture_active = true;
				}
				result["status"] = "pending";
				result["poll_args"] = poll_args;
			} else {
				if (performance_capture_active) {
					debugger->toggle_profiler("performance", false, Array());
					performance_capture_active = false;
				}
				result["performance_unavailable"] = "sample_timeout";
			}
		}
	}
	return _runtime_observation_result(result);
}

bool SolersObservationService::get_runtime_property(uint64_t p_epoch, const NodePath &p_node_path, ObjectID p_object_id, const StringName &p_property, Variant &r_value, PropertyInfo &r_info, String &r_observation_id) const {
	const Dictionary cached = runtime_object_cache.get(solers_object_id_to_string(p_object_id), Dictionary());
	const Dictionary properties = cached.get("properties", Dictionary());
	const Dictionary property_info = cached.get("property_info", Dictionary());
	if (p_epoch != runtime_epoch || p_epoch != (uint64_t)(int64_t)cached.get("runtime_epoch", 0) || NodePath(cached.get("node_path", String())) != p_node_path || !properties.has(p_property) || !property_info.has(p_property)) {
		return false;
	}
	r_value = properties[p_property];
	r_info = PropertyInfo::from_dict(property_info[p_property]);
	r_observation_id = cached.get("observation_id", String());
	return true;
}

void SolersObservationService::clear_runtime_control_result() {
	runtime_control_result.clear();
}

Dictionary SolersObservationService::get_runtime_control_result(const String &p_call_id) const {
	if (String(runtime_control_result.get("call_id", String())) != p_call_id) {
		return Dictionary();
	}
	return runtime_control_result.duplicate(true);
}

SolersObservationService::SolersObservationService() {
	if (EditorFileSystem *filesystem = EditorFileSystem::get_singleton()) {
		filesystem->connect(SNAME("filesystem_changed"), callable_mp(this, &SolersObservationService::_refresh_project_files));
	}
	if (RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->connect(SNAME("frame_post_draw"), callable_mp(this, &SolersObservationService::_render_frame_post_draw));
	}
}
