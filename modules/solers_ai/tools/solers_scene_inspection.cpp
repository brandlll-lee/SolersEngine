/**************************************************************************/
/*  solers_scene_inspection.cpp                                           */
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
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_path_utils.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_scene_observation.h"
#include "modules/solers_ai/core/solers_tool.h"
#include "modules/solers_ai/core/solers_trace.h"

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

Dictionary SolersSceneObservation::get_editor_state() const {
	Dictionary state;
	EditorInterface *editor = EditorInterface::get_singleton();
	Node *root = editor ? editor->get_edited_scene_root() : nullptr;
	state["current_scene"] = root ? root->get_scene_file_path() : String();
	if (root && root->get_scene_file_path().is_empty()) {
		state["current_scene_unsaved"] = true;
	}
	return state;
}

Dictionary SolersSceneObservation::get_open_scenes() const {
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

Dictionary SolersSceneObservation::get_selection() const {
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

Dictionary SolersSceneObservation::query_scene_nodes(const Dictionary &p_args, int p_token_budget) const {
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
		if (!SolersContextManager::append_bounded(nodes, entry, max_results, p_token_budget, tokens)) {
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
