/**************************************************************************/
/*  solers_observation_service.h                                          */
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

#pragma once

#include "core/object/object.h"
#include "core/variant/dictionary.h"
#include "scene/main/node.h"

class SolersObservationService : public Object {
	GDCLASS(SolersObservationService, Object);

	Dictionary _serialize_node(Node *p_node, Node *p_edited_root, int p_depth, int p_max_depth, int p_max_children_per_node) const;
	Array _serialize_node_array(const TypedArray<Node> &p_nodes, Node *p_edited_root, int p_max_depth, int p_max_children_per_node) const;
	bool _normalize_project_path(const String &p_path, String &r_res_path, String &r_error) const;
	void _collect_project_files(const String &p_dir, const String &p_query, int p_max_files, Array &r_files, int &r_scanned_count, bool &r_truncated) const;

protected:
	static void _bind_methods();

public:
	Dictionary get_project_info() const;
	Dictionary get_project_settings_summary() const;
	Dictionary list_project_files(int p_max_files = 512) const;
	Dictionary search_project_files(const String &p_query, int p_max_files = 128) const;
	Dictionary read_project_file(const String &p_path, int p_max_bytes = 262144) const;
	Dictionary get_open_scenes(int p_max_depth = 1, int p_max_children_per_node = 16) const;
	Dictionary get_selection(int p_max_depth = 1, int p_max_children_per_node = 16) const;
	Dictionary get_scene_tree(int p_max_depth = 8, int p_max_children_per_node = 128) const;
	Dictionary get_runtime_status() const;
	Dictionary get_editor_logs(int p_max_messages = 200) const;
	Dictionary get_editor_snapshot(int p_max_scene_depth = 4, int p_max_children_per_node = 64) const;
};
