/**************************************************************************/
/*  solers_reflection_service.h                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/object/object.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "scene/resources/mesh.h"

class Node;
class Node3D;
class SolersActionTimeline;
struct MethodInfo;

class SolersReflectionService : public Object {
	GDCLASS(SolersReflectionService, Object);

	SolersActionTimeline *action_timeline = nullptr;
	bool batch_action_active = false;

	struct UV2UnwrapTask {
		String operation_id;
		Array node_paths;
		Vector<ObjectID> node_ids;
		Vector<Ref<Mesh>> original_meshes;
		Vector<Ref<Mesh>> replacement_meshes;
		int next_index = 0;
		int current_index = -1;
		int already_valid_count = 0;
		Ref<ArrayMesh> current_mesh;
		Transform3D current_transform;
		Error worker_error = OK;
		Thread worker;
		SafeFlag worker_done;
		SafeFlag cancelled;
		bool worker_active = false;
	};
	HashMap<String, UV2UnwrapTask *> uv2_unwrap_tasks;

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;

	Node *_resolve_node(const String &p_node_path, String &r_error) const;

	bool _coerce_value(Node *p_node, const StringName &p_property, const Variant &p_value, Variant &r_out, String &r_error) const;
	bool _apply_initial_properties(Node *p_node, const Dictionary &p_properties, Dictionary &r_applied, String &r_error) const;

	static bool _safe_node_path(Node *p_node, String &r_out);

	Array _spatial_digest_for_results(const Array &p_results) const;
	Dictionary _spatial_facts(Node3D *p_node) const;
	// Facts a subsystem computes and no property dump contains: bone poses,
	// playback state, live particle bounds, the resolved material, the
	// environment actually in force. Dispatch is on the node's engine type,
	// which is the authority on which of these exist for it.
	Dictionary _subsystem_facts(Node *p_node) const;
	String _instance_scene_path(Node *p_node) const;
	Array _nested_instance_scenes(const Array &p_results) const;

	Dictionary _create_node(const Dictionary &p_args);
	Dictionary _reparent_node(const Dictionary &p_args);
	Dictionary _connect_signal(const Dictionary &p_args);
	Dictionary _attach_script(const Dictionary &p_args);
	Dictionary _remove_node(const Dictionary &p_args);
	Dictionary _list_properties(const Dictionary &p_args);
	Dictionary _list_signal_connections(const Dictionary &p_args);
	static void _uv2_unwrap_thread(void *p_userdata);
	Dictionary _advance_uv2_unwrap(UV2UnwrapTask *p_task);
	Dictionary _fail_uv2_unwrap(UV2UnwrapTask *p_task, const String &p_code, const String &p_message);
	Dictionary _pending_uv2_unwrap(const UV2UnwrapTask *p_task, const String &p_stage) const;
	void _free_uv2_unwrap(UV2UnwrapTask *p_task, bool p_wait);
	void _sweep_uv2_unwrap_tasks();

protected:
	static void _bind_methods();

public:
	void set_action_timeline(SolersActionTimeline *p_action_timeline) { action_timeline = p_action_timeline; }

	Dictionary search_classes(const Dictionary &p_args);

	Dictionary introspect_class(const Dictionary &p_args);

	Dictionary set_property(const Dictionary &p_args);

	Dictionary inspect_nodes(const Dictionary &p_args);
	Dictionary instantiate_scene(const Dictionary &p_args);
	Dictionary validate_spatial_relations(const Dictionary &p_args) const;
	static real_t get_aabb_max_gap(const AABB &p_a, const AABB &p_b);
	Dictionary bake_csg(const Dictionary &p_args);
	Dictionary open_scene(const Dictionary &p_args);
	Dictionary unwrap_uv2(const Dictionary &p_args, const String &p_operation_id = String());
	Dictionary poll_uv2_unwrap(const Dictionary &p_args);
	bool is_uv2_unwrap_ready(const Dictionary &p_args) const;
	void cancel_uv2_unwrap(const String &p_operation_id);
	Dictionary bake_lightmap(const Dictionary &p_args);
	uint64_t get_spatial_geometry_digest() const;
	uint64_t get_lightmap_input_digest() const;

	Dictionary batch(const Dictionary &p_args);
	Array resolve_batch_resource_access(const Dictionary &p_args) const;

	SolersReflectionService();
	~SolersReflectionService();
};
