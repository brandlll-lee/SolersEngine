/**************************************************************************/
/*  solers_runtime_bridge.cpp                                             */
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

#include "core/config/engine.h"
#include "core/debugger/engine_debugger.h"
#include "core/io/json.h"
#include "core/io/resource.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/mesh.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_script_context.h"
#include "modules/solers_ai/core/solers_tool.h"

#ifdef MODULE_GDSCRIPT_ENABLED
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_function.h"
#endif

static void _solers_send_runtime_result(const String &p_message, const Dictionary &p_result) {
	if (EngineDebugger *debugger = EngineDebugger::get_singleton()) {
		debugger->send_message(p_message, { p_result });
	}
}

static Array _solers_vector3_wire(const Vector3 &p_value) {
	return Array({ p_value.x, p_value.y, p_value.z });
}

static Dictionary _solers_aabb_wire(const AABB &p_aabb) {
	Dictionary result;
	result["position"] = _solers_vector3_wire(p_aabb.position);
	result["size"] = _solers_vector3_wire(p_aabb.size);
	result["center"] = _solers_vector3_wire(p_aabb.get_center());
	return result;
}

struct SolersRuntimeBounds {
	AABB visual;
	AABB collision;
	bool has_visual = false;
	bool has_collision = false;
	int visual_count = 0;
	int collision_count = 0;
};

static void _solers_merge_aabb(AABB &r_total, bool &r_has_total, const AABB &p_aabb) {
	if (r_has_total) {
		r_total = r_total.merge(p_aabb);
	} else {
		r_total = p_aabb;
		r_has_total = true;
	}
}

static void _solers_collect_runtime_bounds(Node *p_node, SolersRuntimeBounds &r_bounds) {
	if (VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(p_node)) {
		if (visual->is_visible_in_tree()) {
			_solers_merge_aabb(r_bounds.visual, r_bounds.has_visual, visual->get_global_transform().xform(visual->get_aabb()));
			r_bounds.visual_count++;
		}
	}
	if (CollisionShape3D *collision = Object::cast_to<CollisionShape3D>(p_node)) {
		const Ref<Shape3D> shape = collision->get_shape();
		if (!collision->is_disabled() && shape.is_valid()) {
			const Ref<ArrayMesh> mesh = shape->get_debug_mesh();
			if (mesh.is_valid()) {
				_solers_merge_aabb(r_bounds.collision, r_bounds.has_collision, collision->get_global_transform().xform(mesh->get_aabb()));
				r_bounds.collision_count++;
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_runtime_bounds(p_node->get_child(i), r_bounds);
	}
}

static Dictionary _solers_runtime_spatial_facts(Node *p_focus, Camera3D *p_camera) {
	Dictionary facts;
	facts["node_path"] = String(p_focus->get_path());
	facts["class_name"] = p_focus->get_class();
	SolersRuntimeBounds bounds;
	_solers_collect_runtime_bounds(p_focus, bounds);
	facts["visual_count"] = bounds.visual_count;
	facts["collision_count"] = bounds.collision_count;
	if (bounds.has_visual) {
		facts["visual_aabb"] = _solers_aabb_wire(bounds.visual);
		facts["screen_projection"] = solers_project_aabb(p_camera, bounds.visual);
	}
	if (bounds.has_collision) {
		facts["collision_aabb"] = _solers_aabb_wire(bounds.collision);
	}
	Node3D *focus_3d = Object::cast_to<Node3D>(p_focus);
	const Vector3 target = bounds.has_visual ? bounds.visual.get_center() : bounds.has_collision ? bounds.collision.get_center()
			: focus_3d																			 ? focus_3d->get_global_position()
																								 : Vector3();
	if (focus_3d) {
		facts["global_position"] = _solers_vector3_wire(focus_3d->get_global_position());
	}
	if (p_camera && focus_3d) {
		Dictionary visibility;
		visibility["distance"] = p_camera->get_global_position().distance_to(target);
		const Ref<World3D> world = focus_3d->get_world_3d();
		PhysicsDirectSpaceState3D *space = world.is_valid() ? world->get_direct_space_state() : nullptr;
		if (space) {
			PhysicsDirectSpaceState3D::RayParameters query;
			query.from = p_camera->get_global_position();
			query.to = target;
			query.collide_with_areas = true;
			PhysicsDirectSpaceState3D::RayResult hit;
			visibility["ray_hit"] = space->intersect_ray(query, hit);
			if (hit.collider) {
				Node *hit_node = Object::cast_to<Node>(hit.collider);
				visibility["hit_object_id"] = solers_object_id_to_string(hit.collider_id);
				visibility["hit_node_path"] = hit_node && hit_node->is_inside_tree() ? String(hit_node->get_path()) : String();
				visibility["hit_distance"] = query.from.distance_to(hit.position);
				visibility["focus_is_first_hit"] = hit_node && (hit_node == p_focus || p_focus->is_ancestor_of(hit_node));
			}
		}
		facts["camera_visibility"] = visibility;
	}
	return facts;
}

static Variant _solers_runtime_wire_value(const Variant &p_value);

class SolersRuntimeBridge : public Object {
	Array pending_frames;
	bool frame_callback_requested = false;

#ifdef MODULE_GDSCRIPT_ENABLED
	struct RuntimeScriptTask {
		int64_t runtime_epoch = 0;
		uint64_t deadline_msec = 0;
		Ref<GDScript> script;
		Ref<RefCounted> instance;
		Ref<GDScriptFunctionState> state;
		Ref<SolersScriptContext> context;
	};
	HashMap<String, RuntimeScriptTask> runtime_scripts;

	void _send_script_error(const String &p_call_id, int64_t p_runtime_epoch, const String &p_code, const String &p_message) {
		Dictionary result({ { "call_id", p_call_id }, { "runtime_epoch", p_runtime_epoch }, { "ok",
							   false } });
		result["error"] = Dictionary({ { "code", p_code }, { "message", p_message }, { "recoverable",
										  true } });
		_solers_send_runtime_result("solers:script_result", result);
	}

	void _finish_script(const String &p_call_id, const Variant &p_value) {
		RuntimeScriptTask *task = runtime_scripts.getptr(p_call_id);
		if (!task) {
			return;
		}
		Dictionary result({ { "call_id", p_call_id }, { "runtime_epoch",
							   task->runtime_epoch } });
		task->context->finish();
		result["attachments"] = task->context->get_captures();
		if (task->context->has_failed()) {
			result["ok"] = false;
			result["error"] = task->context->get_failure();
		} else {
			result["ok"] = true;
			result["data"] = Dictionary({ { "result", JSON::to_native(JSON::from_native(p_value)) }, { "logs", JSON::to_native(JSON::from_native(task->context->get_logs())) }, { "runtime_only",
											 true } });
		}
		runtime_scripts.erase(p_call_id);
		_solers_send_runtime_result("solers:script_result", result);
	}

	void script_completed(const Variant &p_value, const String &p_call_id) {
		_finish_script(p_call_id, p_value);
	}

	void script_frame() {
		Vector<String> expired;
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		for (const KeyValue<String, RuntimeScriptTask> &task : runtime_scripts) {
			if (now >= task.value.deadline_msec || task.value.context->is_cancelled()) {
				expired.push_back(task.key);
			}
		}
		for (const String &call_id : expired) {
			const int64_t epoch = runtime_scripts[call_id].runtime_epoch;
			runtime_scripts[call_id].context->finish();
			runtime_scripts.erase(call_id);
			_send_script_error(call_id, epoch, "SCRIPT_TIMEOUT", "runtime.script exceeded its declared timeout.");
		}
		SceneTree *tree = SceneTree::get_singleton();
		if (runtime_scripts.is_empty() && tree && tree->is_connected(SNAME("process_frame"), callable_mp(this, &SolersRuntimeBridge::script_frame))) {
			tree->disconnect(SNAME("process_frame"), callable_mp(this, &SolersRuntimeBridge::script_frame));
		}
	}
#endif

public:
	void request_script(const String &p_call_id, int64_t p_runtime_epoch, const String &p_source, uint64_t p_timeout_msec) {
#ifdef MODULE_GDSCRIPT_ENABLED
		if (runtime_scripts.has(p_call_id)) {
			_send_script_error(p_call_id, p_runtime_epoch, "RUNTIME_SCRIPT_BUSY", "This runtime script call is already active.");
			return;
		}
		SceneTree *tree = SceneTree::get_singleton();
		Window *root = tree ? tree->get_root() : nullptr;
		if (!root) {
			_send_script_error(p_call_id, p_runtime_epoch, "RUNTIME_UNAVAILABLE", "The running game has no SceneTree root.");
			return;
		}
		RuntimeScriptTask task;
		task.runtime_epoch = p_runtime_epoch;
		task.deadline_msec = OS::get_singleton()->get_ticks_msec() + p_timeout_msec;
		task.script.instantiate();
		task.script->set_source_code(p_source);
		if (task.script->reload() != OK) {
			_send_script_error(p_call_id, p_runtime_epoch, "SCRIPT_VALIDATION_FAILED", "The game process could not compile runtime.script source.");
			return;
		}
		task.context.instantiate();
		task.context->initialize(SNAME("runtime"), root, root->get_scene_file_path(), Dictionary(), false, PackedStringArray(), String(), String(), task.deadline_msec);
		task.instance.instantiate();
		task.instance->set_script(task.script);
		if (!task.instance->has_method(SNAME("run"))) {
			_send_script_error(p_call_id, p_runtime_epoch, "SCRIPT_ENTRYPOINT_MISSING", "Runtime scripts must define func run(ctx).");
			return;
		}
		Variant context = task.context;
		const Variant *arguments[] = { &context };
		Callable::CallError call_error;
		const Variant value = task.instance->callp(SNAME("run"), arguments, 1, call_error);
		if (call_error.error != Callable::CallError::CALL_OK) {
			task.context->finish();
			_send_script_error(p_call_id, p_runtime_epoch, "SCRIPT_CALL_FAILED", "Godot could not call runtime.script run(ctx).");
			return;
		}
		Ref<GDScriptFunctionState> state = value;
		if (state.is_null()) {
			runtime_scripts[p_call_id] = task;
			_finish_script(p_call_id, value);
			return;
		}
		task.state = state;
		runtime_scripts[p_call_id] = task;
		state->connect(SNAME("completed"), callable_mp(this, &SolersRuntimeBridge::script_completed).bind(p_call_id), CONNECT_ONE_SHOT);
		if (!tree->is_connected(SNAME("process_frame"), callable_mp(this, &SolersRuntimeBridge::script_frame))) {
			tree->connect(SNAME("process_frame"), callable_mp(this, &SolersRuntimeBridge::script_frame));
		}
#else
		Dictionary result({ { "call_id", p_call_id }, { "runtime_epoch", p_runtime_epoch }, { "ok", false } });
		result["error"] = Dictionary({ { "code", "GDSCRIPT_UNAVAILABLE" }, { "message", "runtime.script requires the GDScript module." }, { "recoverable", false } });
		_solers_send_runtime_result("solers:script_result", result);
#endif
	}

	void cancel_script(const String &p_call_id) {
#ifdef MODULE_GDSCRIPT_ENABLED
		if (runtime_scripts.has(p_call_id)) {
			runtime_scripts[p_call_id].context->finish();
		}
		runtime_scripts.erase(p_call_id);
#endif
	}

	void request_frame(const String &p_call_id, int64_t p_runtime_epoch, const Array &p_focus_paths, bool p_capture = false) {
		RenderingServer *rendering = RenderingServer::get_singleton();
		if (!rendering) {
			Dictionary result({ { "call_id", p_call_id }, { "runtime_epoch", p_runtime_epoch }, { "ok", false }, { "code", "RENDERING_UNAVAILABLE" }, { "message", "RenderingServer is unavailable." } });
			_solers_send_runtime_result("solers:frame_result", result);
			return;
		}
		pending_frames.push_back(Dictionary({ { "call_id", p_call_id }, { "runtime_epoch", p_runtime_epoch }, { "focus_paths", p_focus_paths }, { "capture", p_capture } }));
		if (!frame_callback_requested) {
			frame_callback_requested = true;
			rendering->request_frame_drawn_callback(callable_mp(this, &SolersRuntimeBridge::frame_drawn));
		}
	}

	void frame_drawn() {
		frame_callback_requested = false;
		const Array requests = pending_frames.duplicate();
		pending_frames.clear();
		Window *root = SceneTree::get_singleton() ? SceneTree::get_singleton()->get_root() : nullptr;
		Camera3D *camera = root ? root->get_camera_3d() : nullptr;
		for (int request_index = 0; request_index < requests.size(); request_index++) {
			const Dictionary request = requests[request_index];
			if ((bool)request.get("capture", false)) {
				Dictionary receipt = SolersScriptContext::capture_runtime_image(request["call_id"]);
				receipt["call_id"] = request["call_id"];
				receipt["runtime_epoch"] = request["runtime_epoch"];
				_solers_send_runtime_result("solers:capture_result", receipt);
				continue;
			}
			Dictionary result;
			result["call_id"] = request.get("call_id", String());
			result["runtime_epoch"] = request.get("runtime_epoch", 0);
			result["runtime_frame"] = (int64_t)Engine::get_singleton()->get_frames_drawn();
			result["ok"] = root != nullptr;
			result["scene_file_path"] = root ? root->get_scene_file_path() : String();
			result["root_object_id"] = root ? solers_object_id_to_string(root->get_instance_id()) : String();
			result["camera_path"] = camera ? String(camera->get_path()) : String();
			Array spatial;
			const Array focus_paths = request.get("focus_paths", Array());
			for (int i = 0; root && i < focus_paths.size(); i++) {
				const String path = focus_paths[i];
				Node *focus = root->get_node_or_null(NodePath(path));
				if (focus) {
					spatial.push_back(_solers_runtime_spatial_facts(focus, camera));
				} else {
					spatial.push_back(Dictionary({ { "node_path", path }, { "error", "node_not_found" } }));
				}
			}
			result["spatial"] = spatial;
			result["spatial_sha256"] = JSON::stringify(spatial, "", false, true).sha256_text();
			_solers_send_runtime_result("solers:frame_result", result);
		}
	}
};

static SolersRuntimeBridge *solers_runtime_bridge = nullptr;

static Variant _solers_runtime_wire_value(const Variant &p_value) {
	if (p_value.get_type() != Variant::OBJECT) {
		return p_value;
	}
	Object *object = p_value;
	if (!object) {
		return Variant();
	}
	Resource *resource = Object::cast_to<Resource>(object);
	return resource && !resource->get_path().is_empty() ? Variant(resource->get_path()) : Variant(solers_object_id_to_string(object->get_instance_id()));
}

static void _solers_observe_runtime_objects(const String &p_call_id, int64_t p_runtime_epoch, const Array &p_requests) {
	Array nodes;
	Array errors;
	for (int request_index = 0; request_index < p_requests.size(); request_index++) {
		if (p_requests[request_index].get_type() != Variant::DICTIONARY) {
			errors.push_back(Dictionary({ { "request_index", request_index }, { "reason", "invalid_object_request" } }));
			continue;
		}
		const Dictionary request = p_requests[request_index];
		const Variant object_id_value = request.get("object_id", Variant());
		const String object_id_string = String(object_id_value);
		const String node_path = request.get("node_path", String());
		ObjectID object_id;
		if (!solers_object_id_from_variant(object_id_value, object_id)) {
			errors.push_back(Dictionary({ { "request_index", request_index }, { "object_id", object_id_string }, { "node_path", node_path }, { "reason", "invalid_object_id" } }));
			continue;
		}
		Object *object = ObjectDB::get_instance(object_id);
		Node *node = Object::cast_to<Node>(object);
		if (!node || !node->is_inside_tree() || String(node->get_path()) != node_path) {
			Dictionary error({ { "request_index", request_index }, { "object_id", object_id_string }, { "node_path", node_path }, { "reason", node ? "node_path_changed" : "object_not_found" } });
			errors.push_back(error);
			continue;
		}

		Dictionary entry;
		entry["request_index"] = request_index;
		entry["object_id"] = object_id_string;
		entry["node_path"] = node_path;
		entry["class_name"] = node->get_class();
		Node *owner = node->get_owner();
		entry["owner_path"] = owner && owner->is_inside_tree() ? String(owner->get_path()) : String();
		entry["scene_file_path"] = node->get_scene_file_path();

		Dictionary properties;
		Dictionary property_info;
		const Variant requested_value = request.get("properties", Array());
		if (requested_value.get_type() != Variant::ARRAY) {
			errors.push_back(Dictionary({ { "request_index", request_index }, { "object_id", object_id_string }, { "node_path", node_path }, { "reason", "invalid_property_request" } }));
			continue;
		}
		const Array requested_properties = requested_value;
		Dictionary observable;
		if (!requested_properties.is_empty()) {
			List<PropertyInfo> property_list;
			object->get_property_list(&property_list);
			for (const PropertyInfo &property : property_list) {
				if ((property.usage & (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE)) && !(property.usage & PROPERTY_USAGE_SECRET)) {
					observable[property.name] = Dictionary(property);
				}
			}
		}
		for (int property_index = 0; property_index < requested_properties.size(); property_index++) {
			const StringName property = requested_properties[property_index];
			if (!observable.has(property)) {
				errors.push_back(Dictionary({ { "request_index", request_index }, { "object_id", object_id_string }, { "node_path", node_path }, { "property", property }, { "reason", "property_not_observable" } }));
				continue;
			}
			bool valid = false;
			const Variant value = _solers_runtime_wire_value(object->get(property, &valid));
			if (!valid) {
				errors.push_back(Dictionary({ { "request_index", request_index }, { "object_id", object_id_string }, { "node_path", node_path }, { "property", property }, { "reason", "property_read_failed" } }));
				continue;
			}
			properties[property] = value;
			property_info[property] = observable[property];
		}
		entry["properties"] = properties;
		entry["property_info"] = property_info;
		nodes.push_back(entry);
	}
	Dictionary result;
	result["call_id"] = p_call_id;
	result["runtime_epoch"] = p_runtime_epoch;
	result["ok"] = true;
	result["nodes"] = nodes;
	result["errors"] = errors;
	result["requested_count"] = p_requests.size();
	result["complete"] = errors.is_empty();
	_solers_send_runtime_result("solers:objects_result", result);
}

static Error _solers_capture_runtime(void *, const String &p_message, const Array &p_args, bool &r_captured) {
	r_captured = p_message == "capture" || p_message == "observe_frame" || p_message == "observe_objects" || p_message == "run_script" || p_message == "cancel_script";
	if (!r_captured) {
		return OK;
	}
	const String call_id = p_args.size() > 0 ? String(p_args[0]) : String();
	const int64_t runtime_epoch = p_args.size() > 1 ? (int64_t)p_args[1] : 0;
	if (p_message == "capture") {
		ERR_FAIL_COND_V(p_args.size() != 2 || call_id.is_empty() || runtime_epoch <= 0 || !solers_runtime_bridge, ERR_INVALID_PARAMETER);
		solers_runtime_bridge->request_frame(call_id, runtime_epoch, Array(), true);
		return OK;
	}
	if (p_message == "run_script") {
		if (p_args.size() != 4 || call_id.is_empty() || runtime_epoch <= 0 || p_args[2].get_type() != Variant::STRING || p_args[3].get_type() != Variant::INT || !solers_runtime_bridge) {
			Dictionary result({ { "call_id", call_id }, { "runtime_epoch", runtime_epoch }, { "ok", false }, { "error", Dictionary({ { "code", "INVALID_SCRIPT_REQUEST" }, { "message", "runtime.script requires call_id, runtime_epoch, source, and deadline." }, { "recoverable", true } }) } });
			_solers_send_runtime_result("solers:script_result", result);
			return OK;
		}
		solers_runtime_bridge->request_script(call_id, runtime_epoch, p_args[2], (int64_t)p_args[3]);
		return OK;
	}
	if (p_message == "cancel_script") {
		if (solers_runtime_bridge) {
			solers_runtime_bridge->cancel_script(call_id);
		}
		return OK;
	}
	if (p_message == "observe_frame" || p_message == "observe_objects") {
		if (p_args.size() != 3 || call_id.is_empty() || runtime_epoch <= 0 || p_args[2].get_type() != Variant::ARRAY || !solers_runtime_bridge) {
			Dictionary result({ { "call_id", call_id }, { "runtime_epoch", runtime_epoch }, { "ok", false }, { "code", "INVALID_OBSERVATION_REQUEST" }, { "message", "Runtime observations require call_id, runtime_epoch, and an array payload." } });
			_solers_send_runtime_result(p_message == "observe_frame" ? "solers:frame_result" : "solers:objects_result", result);
			return OK;
		}
		if (p_message == "observe_frame") {
			solers_runtime_bridge->request_frame(call_id, runtime_epoch, p_args[2]);
		} else {
			_solers_observe_runtime_objects(call_id, runtime_epoch, p_args[2]);
		}
		return OK;
	}
	return OK;
}

void solers_runtime_bridge_initialize() {
	if (!solers_runtime_bridge) {
		solers_runtime_bridge = memnew(SolersRuntimeBridge);
	}
	if (!EngineDebugger::has_capture(SNAME("solers"))) {
		EngineDebugger::register_message_capture(SNAME("solers"), EngineDebugger::Capture(nullptr, _solers_capture_runtime));
	}
}

void solers_runtime_bridge_uninitialize() {
	if (solers_runtime_bridge) {
		memdelete(solers_runtime_bridge);
		solers_runtime_bridge = nullptr;
	}
	if (EngineDebugger::has_capture(SNAME("solers"))) {
		EngineDebugger::unregister_message_capture(SNAME("solers"));
	}
}
