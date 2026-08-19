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
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "scene/main/node.h"

class Image;
class ScriptEditorDebugger;
class SceneDebuggerTree;
class Camera3D;
class Viewport;

class SolersObservationService : public Object {
	GDCLASS(SolersObservationService, Object);
	mutable Mutex project_files_mutex;
	PackedStringArray project_files;
	bool project_files_ready = false;

	uint64_t capture_sequence = 0;
	uint64_t render_post_draw_sequence = 0;
	HashMap<String, Dictionary> pending_captures;
	HashMap<String, Dictionary> last_render_by_view;
	ObjectID observed_debugger_id;
	uint64_t runtime_cursor = 0;
	uint64_t runtime_epoch = 0;
	Vector<Dictionary> runtime_events;
	uint64_t runtime_query_sequence = 0;
	int runtime_result_token_budget = INT32_MAX;
	Dictionary runtime_query;
	Dictionary runtime_control_result;
	Dictionary runtime_object_cache;
	Array runtime_stack_frames;
	bool performance_capture_active = false;
	uint64_t performance_sample_cursor = 0;
	Array performance_monitor_names;
	Array performance_monitor_types;

	bool _normalize_project_path(const String &p_path, String &r_res_path, String &r_error) const;
	void _refresh_project_files();
	void _render_frame_post_draw();
	Dictionary _capture_error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	Dictionary _runtime_capture_unavailable() const;
	Dictionary _capture_image(const Ref<Image> &p_image, const String &p_target, const String &p_capture_id = String());
	Dictionary _render_state_for_pending(const Dictionary &p_pending) const;
	Dictionary _attach_render_receipt(Dictionary p_result, const Dictionary &p_pending);
	Dictionary _register_pending_capture(const String &p_target, const Dictionary &p_extra);
	Dictionary _poll_pending_capture(const String &p_capture_id);
	Dictionary _finish_frame_gated_capture(const String &p_capture_id, const Dictionary &p_data);
	Dictionary _begin_scene_view_capture(const String &p_target, const Dictionary &p_args);
	Dictionary _editor_3d_viewport_state() const;
	void _runtime_screenshot_ready(int64_t p_width, int64_t p_height, const String &p_path, const Rect2i &p_rect, const String &p_capture_id);
	void _append_runtime_event(const StringName &p_type, const Dictionary &p_data = Dictionary(), bool p_persist = false);
	void _bind_runtime_debugger();
	void _runtime_started();
	void _runtime_stopped();
	void _runtime_output(const String &p_message, int p_level);
	void _runtime_breaked(bool p_breaked, bool p_can_debug, const String &p_reason, bool p_has_stackdump);
	void _runtime_debug_data(const String &p_message, const Array &p_data);
	void _runtime_tree_updated();
	void _runtime_stack_dump(const Array &p_frames);
	void _finish_runtime_query(Dictionary p_result);
	bool _is_runtime_visual_ready() const;
	bool _request_runtime_screenshot(const String &p_capture_id);
	bool _request_runtime_frame(const String &p_call_id, const Array &p_focus_paths);
	bool _request_runtime_objects(const String &p_call_id, const Array &p_requests);

protected:
	static void _bind_methods();

public:
#ifdef DEBUG_ENABLED
	static Array project_runtime_tree(const SceneDebuggerTree &p_tree, uint64_t p_epoch);
#endif
	static int get_capture_settle_frame_count(bool p_sdfgi_enabled, int p_convergence_setting);
	static String render_state_fingerprint(const Dictionary &p_state);
	Dictionary describe_render_state(Viewport *p_viewport, Camera3D *p_camera = nullptr, Node *p_scene_root = nullptr) const;
	Dictionary get_project_info() const;
	Dictionary get_project_settings_summary() const;
	Dictionary list_project_files(int p_max_files = 512) const;
	Dictionary search_project(const Dictionary &p_args, int p_token_budget = INT32_MAX) const;
	// Front door: bounded digest for any res:// selection (directory / Resource / file).
	// Kind comes from DirAccess + ResourceLoader / EditorFileSystem — not mention token names.
	Dictionary observe_path(const String &p_path) const;
	Dictionary digest_packed_scene(const String &p_path, int p_max_nodes = 96) const;
	// PackedScene source text is denied by default; returns observe_path digest + SCENE_TEXT_DENIED.
	Dictionary read_project_file(const String &p_path, int p_line_start = 1, int p_line_count = 200, bool p_raw = false, int p_token_budget = INT32_MAX) const;
	Dictionary get_open_scenes() const;
	Dictionary get_selection() const;
	Dictionary query_scene_nodes(const Dictionary &p_args, int p_token_budget) const;
	Dictionary get_runtime_status() const;
	Dictionary observe_runtime(const Dictionary &p_args, int p_token_budget = INT32_MAX);
	bool is_runtime_observation_ready(const Dictionary &p_args) const;
	bool has_runtime_query() const { return !runtime_query.is_empty(); }
	void clear_runtime_control_result();
	Dictionary get_runtime_control_result(const String &p_call_id) const;
	bool get_runtime_property(uint64_t p_epoch, const NodePath &p_node_path, ObjectID p_object_id, const StringName &p_property, Variant &r_value, PropertyInfo &r_info, String &r_observation_id) const;
	Dictionary capture_viewport(const Dictionary &p_args);
	Dictionary poll_viewport_capture(const Dictionary &p_args);
	bool is_viewport_capture_ready(const Dictionary &p_args);
	void poll();

	SolersObservationService();
};
