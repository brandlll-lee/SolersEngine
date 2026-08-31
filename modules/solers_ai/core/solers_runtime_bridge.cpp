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
#include "core/input/input.h"
#include "core/input/input_map.h"
#include "core/io/json.h"
#include "core/io/resource.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/templates/hash_set.h"
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
#include "modules/solers_ai/core/solers_tool.h"

static HashSet<StringName> solers_owned_input_actions;

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

static Dictionary _solers_runtime_property_snapshot(const Array &p_requests) {
	Array values;
	Array missing;
	Window *root = SceneTree::get_singleton() ? SceneTree::get_singleton()->get_root() : nullptr;
	for (int i = 0; i < p_requests.size(); i++) {
		if (p_requests[i].get_type() != Variant::DICTIONARY) {
			missing.push_back(Dictionary({ { "request_index", i }, { "reason", "invalid_observation" } }));
			continue;
		}
		const Dictionary request = p_requests[i];
		const String path = request.get("node_path", String());
		Node *node = root ? root->get_node_or_null(NodePath(path)) : nullptr;
		if (!node) {
			missing.push_back(Dictionary({ { "node_path", path }, { "reason", "node_not_found" } }));
			continue;
		}
		Dictionary properties;
		const Array names = request.get("properties", Array());
		for (int property_index = 0; property_index < names.size(); property_index++) {
			const StringName name = names[property_index];
			bool valid = false;
			const Variant value = _solers_runtime_wire_value(node->get(name, &valid));
			if (valid) {
				properties[name] = value;
			} else {
				missing.push_back(Dictionary({ { "node_path", path }, { "property", name }, { "reason", "property_not_found" } }));
			}
		}
		values.push_back(Dictionary({ { "node_path", path }, { "object_id", solers_object_id_to_string(node->get_instance_id()) }, { "properties", properties } }));
	}
	return Dictionary({ { "values", values }, { "missing", missing } });
}

class SolersRuntimeBridge : public Object {
	Array pending_frames;
	Array pending_inputs;
	bool frame_callback_requested = false;

public:
	void request_frame(const String &p_call_id, int64_t p_runtime_epoch, const Array &p_focus_paths) {
		RenderingServer *rendering = RenderingServer::get_singleton();
		if (!rendering) {
			Dictionary result({ { "call_id", p_call_id }, { "runtime_epoch", p_runtime_epoch }, { "ok", false }, { "code", "RENDERING_UNAVAILABLE" }, { "message", "RenderingServer is unavailable." } });
			_solers_send_runtime_result("solers:frame_result", result);
			return;
		}
		pending_frames.push_back(Dictionary({ { "call_id", p_call_id }, { "runtime_epoch", p_runtime_epoch }, { "focus_paths", p_focus_paths } }));
		if (!frame_callback_requested) {
			frame_callback_requested = true;
			rendering->request_frame_drawn_callback(callable_mp(this, &SolersRuntimeBridge::frame_drawn));
		}
	}

	void frame_drawn() {
		frame_callback_requested = false;
		const Array requests = pending_frames;
		pending_frames.clear();
		Window *root = SceneTree::get_singleton() ? SceneTree::get_singleton()->get_root() : nullptr;
		Camera3D *camera = root ? root->get_camera_3d() : nullptr;
		for (int request_index = 0; request_index < requests.size(); request_index++) {
			const Dictionary request = requests[request_index];
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

	void request_input(const String &p_call_id, int64_t p_runtime_epoch, int p_physics_frames, const Array &p_observations) {
		SceneTree *tree = SceneTree::get_singleton();
		ERR_FAIL_NULL(tree);
		pending_inputs.push_back(Dictionary({ { "call_id", p_call_id }, { "runtime_epoch", p_runtime_epoch }, { "remaining_frames", p_physics_frames }, { "physics_frames", p_physics_frames }, { "observations", p_observations }, { "before", _solers_runtime_property_snapshot(p_observations) } }));
		if (!tree->is_connected(SNAME("physics_frame"), callable_mp(this, &SolersRuntimeBridge::physics_frame))) {
			tree->connect(SNAME("physics_frame"), callable_mp(this, &SolersRuntimeBridge::physics_frame));
		}
	}

	void physics_frame() {
		for (int i = pending_inputs.size() - 1; i >= 0; i--) {
			Dictionary request = pending_inputs[i];
			const int remaining = (int)request.get("remaining_frames", 1) - 1;
			if (remaining > 0) {
				request["remaining_frames"] = remaining;
				pending_inputs[i] = request;
				continue;
			}
			const Dictionary after = _solers_runtime_property_snapshot(request.get("observations", Array()));
			const Dictionary before = request.get("before", Dictionary());
			Array missing = before.get("missing", Array());
			missing.append_array(Array(after.get("missing", Array())));
			Dictionary result({ { "call_id", request.get("call_id", String()) }, { "runtime_epoch", request.get("runtime_epoch", 0) }, { "physics_frames", request.get("physics_frames", 0) } });
			result["before"] = before.get("values", Array());
			result["after"] = after.get("values", Array());
			result["availability"] = Dictionary({ { "state", missing.is_empty() ? "complete" : "partial" }, { "missing", missing } });
			result["ok"] = true;
			_solers_send_runtime_result("solers:input_result", result);
			pending_inputs.remove_at(i);
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

static void _solers_send_input_error(const String &p_call_id, int64_t p_runtime_epoch, const String &p_code, const String &p_message) {
	Dictionary result;
	result["call_id"] = p_call_id;
	result["runtime_epoch"] = p_runtime_epoch;
	result["ok"] = false;
	result["code"] = p_code;
	result["message"] = p_message;
	_solers_send_runtime_result("solers:input_result", result);
}

static Error _solers_capture_runtime(void *, const String &p_message, const Array &p_args, bool &r_captured) {
	r_captured = p_message == "set_input_actions" || p_message == "observe_frame" || p_message == "observe_objects";
	if (!r_captured) {
		return OK;
	}
	const String call_id = p_args.size() > 0 ? String(p_args[0]) : String();
	const int64_t runtime_epoch = p_args.size() > 1 ? (int64_t)p_args[1] : 0;
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
	if (p_args.size() != 5 || call_id.is_empty() || runtime_epoch <= 0 || p_args[2].get_type() != Variant::ARRAY || p_args[3].get_type() != Variant::INT || (int64_t)p_args[3] <= 0 || p_args[4].get_type() != Variant::ARRAY || !solers_runtime_bridge) {
		_solers_send_input_error(call_id, runtime_epoch, "INVALID_INPUT_REQUEST", "set_input_actions requires call_id, runtime_epoch, actions, positive physics_frames, and observations.");
		return OK;
	}

	Input *input = Input::get_singleton();
	InputMap *input_map = InputMap::get_singleton();
	if (!input || !input_map) {
		_solers_send_input_error(call_id, runtime_epoch, "INPUT_UNAVAILABLE", "Godot Input and InputMap must be initialized.");
		return OK;
	}

	const Array actions = p_args[2];
	HashSet<StringName> next_actions;
	for (int i = 0; i < actions.size(); i++) {
		if (actions[i].get_type() != Variant::DICTIONARY) {
			_solers_send_input_error(call_id, runtime_epoch, "INVALID_INPUT_ACTION", "Every input action must be an object with name and strength.");
			return OK;
		}
		const Dictionary action = actions[i];
		const StringName name = action.get("name", StringName());
		const Variant strength_value = action.get("strength", Variant());
		if (name.is_empty() || (strength_value.get_type() != Variant::INT && strength_value.get_type() != Variant::FLOAT)) {
			_solers_send_input_error(call_id, runtime_epoch, "INVALID_INPUT_ACTION", "Every input action requires a non-empty name and numeric strength.");
			return OK;
		}
		const double strength = strength_value;
		if (!Math::is_finite(strength) || strength <= 0.0 || strength > 1.0 || next_actions.has(name) || !input_map->has_action(name)) {
			_solers_send_input_error(call_id, runtime_epoch, "INVALID_INPUT_ACTION", vformat("Input action '%s' is unknown, duplicated, or has strength outside (0, 1].", name));
			return OK;
		}
		next_actions.insert(name);
	}

	const Array observations = p_args[4];
	for (const StringName &action : solers_owned_input_actions) {
		if (!next_actions.has(action) && input_map->has_action(action)) {
			input->action_release(action);
		}
	}
	for (int i = 0; i < actions.size(); i++) {
		const Dictionary action = actions[i];
		input->action_press(action.get("name", StringName()), (double)action.get("strength", 0.0));
	}
	solers_owned_input_actions = next_actions;
	solers_runtime_bridge->request_input(call_id, runtime_epoch, (int64_t)p_args[3], observations);
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
	Input *input = Input::get_singleton();
	InputMap *input_map = InputMap::get_singleton();
	for (const StringName &action : solers_owned_input_actions) {
		if (input && input_map && input_map->has_action(action)) {
			input->action_release(action);
		}
	}
	solers_owned_input_actions.clear();
	if (solers_runtime_bridge) {
		memdelete(solers_runtime_bridge);
		solers_runtime_bridge = nullptr;
	}
	if (EngineDebugger::has_capture(SNAME("solers"))) {
		EngineDebugger::unregister_message_capture(SNAME("solers"));
	}
}
