/**************************************************************************/
/*  solers_runtime_input_bridge.cpp                                       */
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

#include "modules/solers_ai/core/solers_tool.h"

static HashSet<StringName> solers_owned_input_actions;

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
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		if (geometry->is_visible_in_tree()) {
			_solers_merge_aabb(r_bounds.visual, r_bounds.has_visual, geometry->get_global_transform().xform(geometry->get_aabb()));
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

static Dictionary _solers_screen_projection(Camera3D *p_camera, const AABB &p_aabb) {
	Dictionary result;
	if (!p_camera || !p_camera->get_viewport()) {
		result["available"] = false;
		return result;
	}
	Rect2 projected;
	bool has_point = false;
	for (int i = 0; i < 8; i++) {
		const Vector3 point = p_aabb.position + Vector3((i & 1) ? p_aabb.size.x : 0.0, (i & 2) ? p_aabb.size.y : 0.0, (i & 4) ? p_aabb.size.z : 0.0);
		if (p_camera->is_position_behind(point)) {
			continue;
		}
		const Vector2 screen = p_camera->unproject_position(point);
		if (has_point) {
			projected = projected.expand(screen);
		} else {
			projected = Rect2(screen, Vector2());
			has_point = true;
		}
	}
	const Vector2 viewport_size = p_camera->get_viewport()->get_visible_rect().size;
	const Rect2 visible = projected.intersection(Rect2(Vector2(), viewport_size));
	result["available"] = has_point;
	if (has_point) {
		Array rect;
		rect.push_back(projected.position.x);
		rect.push_back(projected.position.y);
		rect.push_back(projected.size.x);
		rect.push_back(projected.size.y);
		result["rect"] = rect;
		result["visible_fraction"] = viewport_size.x > 0.0 && viewport_size.y > 0.0 ? visible.get_area() / (viewport_size.x * viewport_size.y) : 0.0;
	}
	return result;
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
		facts["screen_projection"] = _solers_screen_projection(p_camera, bounds.visual);
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

class SolersRuntimeFrameBridge : public Object {
	Array pending;

public:
	void request(const String &p_call_id, int64_t p_runtime_epoch, const Array &p_focus_paths) {
		pending.push_back(Dictionary({ { "call_id", p_call_id }, { "runtime_epoch", p_runtime_epoch }, { "focus_paths", p_focus_paths } }));
		RenderingServer *rendering = RenderingServer::get_singleton();
		const Callable callback = callable_mp(this, &SolersRuntimeFrameBridge::frame_post_draw);
		if (rendering && !rendering->is_connected(SNAME("frame_post_draw"), callback)) {
			rendering->connect(SNAME("frame_post_draw"), callback, Object::CONNECT_ONE_SHOT);
		}
	}

	void frame_post_draw() {
		const Array requests = pending;
		pending.clear();
		Window *root = SceneTree::get_singleton() ? SceneTree::get_singleton()->get_root() : nullptr;
		Camera3D *camera = root ? root->get_camera_3d() : nullptr;
		for (int request_index = 0; request_index < requests.size(); request_index++) {
			const Dictionary request = requests[request_index];
			Dictionary result;
			result["call_id"] = request.get("call_id", String());
			result["runtime_epoch"] = request.get("runtime_epoch", 0);
			result["runtime_frame"] = (int64_t)Engine::get_singleton()->get_frames_drawn();
			result["ok"] = root != nullptr;
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
			if (EngineDebugger::is_active()) {
				EngineDebugger::get_singleton()->send_message("solers:frame_result", { result });
			}
		}
	}
};

static SolersRuntimeFrameBridge *solers_runtime_frame_bridge = nullptr;

static void _solers_send_input_result(const String &p_call_id, int64_t p_runtime_epoch, bool p_ok, const String &p_code = String(), const String &p_message = String()) {
	Dictionary result;
	result["call_id"] = p_call_id;
	result["runtime_epoch"] = p_runtime_epoch;
	result["ok"] = p_ok;
	if (!p_ok) {
		result["code"] = p_code;
		result["message"] = p_message;
	}
	if (EngineDebugger::is_active()) {
		EngineDebugger::get_singleton()->send_message("solers:input_result", { result });
	}
}

static Error _solers_capture_runtime_input(void *, const String &p_message, const Array &p_args, bool &r_captured) {
	r_captured = p_message == "set_input_actions" || p_message == "observe_frame";
	if (!r_captured) {
		return OK;
	}
	const String call_id = p_args.size() > 0 ? String(p_args[0]) : String();
	const int64_t runtime_epoch = p_args.size() > 1 ? (int64_t)p_args[1] : 0;
	if (p_message == "observe_frame") {
		if (p_args.size() != 3 || call_id.is_empty() || runtime_epoch <= 0 || p_args[2].get_type() != Variant::ARRAY || !solers_runtime_frame_bridge) {
			return ERR_INVALID_PARAMETER;
		}
		solers_runtime_frame_bridge->request(call_id, runtime_epoch, p_args[2]);
		return OK;
	}
	if (p_args.size() != 3 || call_id.is_empty() || runtime_epoch <= 0 || p_args[2].get_type() != Variant::ARRAY) {
		_solers_send_input_result(call_id, runtime_epoch, false, "INVALID_INPUT_REQUEST", "set_input_actions requires call_id, runtime_epoch, and an actions array.");
		return OK;
	}

	Input *input = Input::get_singleton();
	InputMap *input_map = InputMap::get_singleton();
	if (!input || !input_map) {
		_solers_send_input_result(call_id, runtime_epoch, false, "INPUT_UNAVAILABLE", "Godot Input and InputMap must be initialized.");
		return OK;
	}

	const Array actions = p_args[2];
	HashSet<StringName> next_actions;
	for (int i = 0; i < actions.size(); i++) {
		if (actions[i].get_type() != Variant::DICTIONARY) {
			_solers_send_input_result(call_id, runtime_epoch, false, "INVALID_INPUT_ACTION", "Every input action must be an object with name and strength.");
			return OK;
		}
		const Dictionary action = actions[i];
		const StringName name = action.get("name", StringName());
		const Variant strength_value = action.get("strength", Variant());
		if (name.is_empty() || (strength_value.get_type() != Variant::INT && strength_value.get_type() != Variant::FLOAT)) {
			_solers_send_input_result(call_id, runtime_epoch, false, "INVALID_INPUT_ACTION", "Every input action requires a non-empty name and numeric strength.");
			return OK;
		}
		const double strength = strength_value;
		if (!Math::is_finite(strength) || strength <= 0.0 || strength > 1.0 || next_actions.has(name) || !input_map->has_action(name)) {
			_solers_send_input_result(call_id, runtime_epoch, false, "INVALID_INPUT_ACTION", vformat("Input action '%s' is unknown, duplicated, or has strength outside (0, 1].", name));
			return OK;
		}
		next_actions.insert(name);
	}

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
	_solers_send_input_result(call_id, runtime_epoch, true);
	return OK;
}

void solers_runtime_input_bridge_initialize() {
	if (!solers_runtime_frame_bridge) {
		solers_runtime_frame_bridge = memnew(SolersRuntimeFrameBridge);
	}
	if (!EngineDebugger::has_capture(SNAME("solers"))) {
		EngineDebugger::register_message_capture(SNAME("solers"), EngineDebugger::Capture(nullptr, _solers_capture_runtime_input));
	}
}

void solers_runtime_input_bridge_uninitialize() {
	Input *input = Input::get_singleton();
	InputMap *input_map = InputMap::get_singleton();
	for (const StringName &action : solers_owned_input_actions) {
		if (input && input_map && input_map->has_action(action)) {
			input->action_release(action);
		}
	}
	solers_owned_input_actions.clear();
	if (solers_runtime_frame_bridge) {
		memdelete(solers_runtime_frame_bridge);
		solers_runtime_frame_bridge = nullptr;
	}
	if (EngineDebugger::has_capture(SNAME("solers"))) {
		EngineDebugger::unregister_message_capture(SNAME("solers"));
	}
}
