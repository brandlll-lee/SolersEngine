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
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "scene/main/node.h"

class Image;
class ScriptEditorDebugger;

class SolersObservationService : public Object {
	GDCLASS(SolersObservationService, Object);

	uint64_t capture_sequence = 0;
	HashMap<String, Dictionary> pending_captures;
	ObjectID observed_debugger_id;
	uint64_t runtime_cursor = 0;
	uint64_t runtime_epoch = 0;
	Vector<Dictionary> runtime_events;
	bool performance_capture_active = false;
	Array performance_monitor_names;
	Array performance_monitor_types;

	Dictionary _serialize_node(Node *p_node, Node *p_edited_root, int p_depth, int p_max_depth, int p_max_children_per_node) const;
	Array _serialize_node_array(const TypedArray<Node> &p_nodes, Node *p_edited_root, int p_max_depth, int p_max_children_per_node) const;
	bool _normalize_project_path(const String &p_path, String &r_res_path, String &r_error) const;
	bool _collect_project_files_indexed(const String &p_query, int p_max_files, Array &r_files, int &r_scanned_count, bool &r_truncated) const;
	void _collect_project_files(const String &p_dir, const String &p_query, int p_max_files, Array &r_files, int &r_scanned_count, bool &r_truncated, uint64_t p_deadline_msec) const;
	Dictionary _search_project_paths(const String &p_query, int p_max_files) const;
	Dictionary _capture_error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	Dictionary _capture_image(const Ref<Image> &p_image, const String &p_target, const String &p_capture_id = String());
	Dictionary _register_pending_capture(const String &p_target, const Dictionary &p_extra);
	Dictionary _poll_pending_capture(const String &p_capture_id);
	Dictionary _finish_frame_gated_capture(const String &p_capture_id, const Dictionary &p_data);
	Dictionary _begin_scene_view_capture(const String &p_target, const Dictionary &p_args);
	Dictionary _editor_3d_viewport_state() const;
	void _runtime_screenshot_ready(int64_t p_width, int64_t p_height, const String &p_path, const Rect2i &p_rect, const String &p_capture_id);
	void _append_runtime_event(const StringName &p_type, const Dictionary &p_data = Dictionary(), bool p_persist = false);
	void _restore_runtime_events();
	void _bind_runtime_debugger();
	void _runtime_started();
	void _runtime_stopped();
	void _runtime_output(const String &p_message, int p_level);
	void _runtime_breaked(bool p_breaked, bool p_can_debug, const String &p_reason, bool p_has_stackdump);
	void _runtime_debug_data(const String &p_message, const Array &p_data);
	void _runtime_tree_updated();
	bool _has_runtime_event_after(const StringName &p_type, uint64_t p_cursor) const;

protected:
	static void _bind_methods();

public:
	static int get_capture_settle_frame_count(bool p_sdfgi_enabled, int p_convergence_setting);
	static Dictionary image_statistics(const Ref<Image> &p_image);
	Dictionary get_project_info() const;
	Dictionary get_project_settings_summary() const;
	Dictionary list_project_files(int p_max_files = 512) const;
	Dictionary search_project(const Dictionary &p_args) const;
	Dictionary read_project_file(const String &p_path, int p_max_bytes = 262144) const;
	Dictionary get_open_scenes(int p_max_depth = 1, int p_max_children_per_node = 16) const;
	Dictionary get_selection(int p_max_depth = 1, int p_max_children_per_node = 16) const;
	Dictionary get_scene_tree(int p_max_depth = 8, int p_max_children_per_node = 128) const;
	Dictionary get_runtime_status() const;
	Dictionary observe_runtime(const Dictionary &p_args);
	bool is_runtime_observation_ready(const Dictionary &p_args) const;
	Dictionary get_editor_logs(int p_max_messages = 200) const;
	Dictionary get_editor_snapshot(int p_max_scene_depth = 4, int p_max_children_per_node = 64, bool p_include_remote_scene = false) const;
	Dictionary capture_viewport(const Dictionary &p_args);
	Dictionary poll_viewport_capture(const Dictionary &p_args);
	bool is_viewport_capture_ready(const Dictionary &p_args);
	void poll();

	SolersObservationService();
};
