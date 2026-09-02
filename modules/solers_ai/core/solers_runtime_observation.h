/**************************************************************************/
/*  solers_runtime_observation.h                                          */
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

class SceneDebuggerTree;
class ScriptEditorDebugger;

class SolersRuntimeObservation : public Object {
	GDCLASS(SolersRuntimeObservation, Object);

	ObjectID observed_debugger_id;
	uint64_t runtime_cursor = 0;
	uint64_t runtime_epoch = 0;
	Vector<Dictionary> runtime_events;
	uint64_t runtime_query_sequence = 0;
	int runtime_result_token_budget = INT32_MAX;
	Dictionary runtime_query;
	Dictionary runtime_control_result;
	Dictionary runtime_script;
	Dictionary runtime_object_cache;
	Array runtime_stack_frames;
	bool performance_capture_active = false;
	uint64_t performance_sample_cursor = 0;
	Array performance_monitor_names;
	Array performance_monitor_types;

	void _append_runtime_event(const StringName &p_type, const Dictionary &p_data = Dictionary(), bool p_persist = false);
	void _bind_runtime_debugger();
	void _runtime_started();
	void _runtime_stopped();
	void _runtime_output(const String &p_message, int p_level);
	void _runtime_breaked(bool p_breaked, bool p_can_debug, const String &p_reason, bool p_has_stackdump);
	void _runtime_debug_data(const String &p_message, const Array &p_data);
	void _runtime_tree_updated();
	void _runtime_stack_dump(const Array &p_frames);
	Dictionary _runtime_observation_result(Dictionary p_result) const;
	void _finish_runtime_query(Dictionary p_result);
	bool _is_runtime_visual_ready() const;
	bool _request_runtime_frame(const String &p_call_id, const Array &p_focus_paths);
	bool _request_runtime_objects(const String &p_call_id, const Array &p_requests);

protected:
	static void _bind_methods();

public:
#ifdef DEBUG_ENABLED
	static Array project_runtime_tree(const SceneDebuggerTree &p_tree, uint64_t p_epoch);
#endif
	Dictionary get_runtime_status() const;
	Dictionary observe_runtime(const Dictionary &p_args, int p_token_budget = INT32_MAX);
	bool is_runtime_observation_ready(const Dictionary &p_args) const;
	bool has_runtime_query() const { return !runtime_query.is_empty(); }
	void clear_runtime_control_result();
	Dictionary get_runtime_control_result(const String &p_call_id) const;
	Dictionary start_runtime_script(const Dictionary &p_args, const String &p_call_id);
	Dictionary poll_runtime_script(const Dictionary &p_args);
	bool is_runtime_script_ready(const Dictionary &p_args) const;
	void clear_runtime_script(const String &p_call_id);
	bool get_runtime_property(uint64_t p_epoch, const NodePath &p_node_path, ObjectID p_object_id, const StringName &p_property, Variant &r_value, PropertyInfo &r_info, String &r_observation_id) const;
	void poll();

	SolersRuntimeObservation();
};
