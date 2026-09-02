/**************************************************************************/
/*  solers_runtime_observation.cpp                                        */
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
#include "modules/solers_ai/core/solers_runtime_observation.h"

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
#include "modules/solers_ai/core/solers_tool.h"
#include "modules/solers_ai/core/solers_trace.h"

static constexpr uint64_t SOLERS_RUNTIME_QUERY_TIMEOUT_MSEC = 3000;
static constexpr int SOLERS_RUNTIME_EVENT_LIMIT = 512;
static constexpr int SOLERS_RUNTIME_PROPERTY_LIMIT = 64;

Dictionary SolersRuntimeObservation::get_runtime_status() const {
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

void SolersRuntimeObservation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_runtime_status"), &SolersRuntimeObservation::get_runtime_status);
	ClassDB::bind_method(D_METHOD("observe_runtime", "args", "token_budget"), &SolersRuntimeObservation::observe_runtime, DEFVAL(INT32_MAX));
}

bool SolersRuntimeObservation::_is_runtime_visual_ready() const {
	if (!GameViewDebugger::has_active_capture_session()) {
		return false;
	}
	for (int i = runtime_events.size() - 1; i >= 0; i--) {
		const Dictionary event = runtime_events[i];
		if ((uint64_t)(int64_t)event.get("runtime_epoch", 0) == runtime_epoch && StringName(event.get("type", String())) == SNAME("started")) {
			return true;
		}
	}
	return false;
}

void SolersRuntimeObservation::_append_runtime_event(const StringName &p_type, const Dictionary &p_data, bool p_persist) {
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

bool SolersRuntimeObservation::_request_runtime_frame(const String &p_call_id, const Array &p_focus_paths) {
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	if (!debugger || !debugger->is_session_active() || p_call_id.is_empty()) {
		return false;
	}
	debugger->send_message("solers:observe_frame", { p_call_id, (int64_t)runtime_epoch, p_focus_paths });
	return true;
}

bool SolersRuntimeObservation::_request_runtime_objects(const String &p_call_id, const Array &p_requests) {
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	if (!debugger || !debugger->is_session_active() || p_call_id.is_empty()) {
		return false;
	}
	debugger->send_message("solers:observe_objects", { p_call_id, (int64_t)runtime_epoch, p_requests });
	return true;
}

void SolersRuntimeObservation::_runtime_started() {
	runtime_epoch++;
	runtime_query.clear();
	runtime_control_result.clear();
	runtime_object_cache.clear();
	runtime_stack_frames.clear();
	performance_sample_cursor = 0;
	_append_runtime_event(SNAME("started"), Dictionary(), true);
}

void SolersRuntimeObservation::_runtime_stopped() {
	performance_capture_active = false;
	performance_monitor_names.clear();
	performance_monitor_types.clear();
	runtime_query.clear();
	runtime_control_result.clear();
	if (!runtime_script.is_empty() && !runtime_script.has("result")) {
		runtime_script["result"] = Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "RUNTIME_STOPPED" }, { "message", "The game process stopped before runtime.script completed." }, { "recoverable", true } }) } });
	}
	runtime_object_cache.clear();
	runtime_stack_frames.clear();
	performance_sample_cursor = 0;
	_append_runtime_event(SNAME("stopped"), Dictionary(), true);
}

void SolersRuntimeObservation::_runtime_output(const String &p_message, int p_level) {
	Dictionary data;
	data["message"] = p_message.left(4096);
	data["level"] = p_level;
	data["truncated"] = p_message.length() > 4096;
	// Ring keeps the evidence; transcript does not need per-line floods.
	_append_runtime_event(SNAME("output"), data, false);
}

void SolersRuntimeObservation::_runtime_breaked(bool p_breaked, bool p_can_debug, const String &p_reason, bool p_has_stackdump) {
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

void SolersRuntimeObservation::_runtime_debug_data(const String &p_message, const Array &p_data) {
	if (p_message == "solers:script_result" && p_data.size() == 1 && p_data[0].get_type() == Variant::DICTIONARY) {
		const Dictionary result = p_data[0];
		if ((uint64_t)(int64_t)result.get("runtime_epoch", 0) == runtime_epoch && String(result.get("call_id", String())) == String(runtime_script.get("call_id", String()))) {
			runtime_script["result"] = result;
		}
		return;
	}
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
Array SolersRuntimeObservation::project_runtime_tree(const SceneDebuggerTree &p_tree, uint64_t p_epoch) {
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

void SolersRuntimeObservation::_runtime_tree_updated() {
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
			if (!SolersContextManager::append_bounded(nodes, entry, max_results, token_budget, tokens)) {
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

void SolersRuntimeObservation::_runtime_stack_dump(const Array &p_frames) {
	runtime_stack_frames.clear();
	for (int i = 0; i < MIN(p_frames.size(), 64); i++) {
		runtime_stack_frames.push_back(p_frames[i]);
	}
}

void SolersRuntimeObservation::_finish_runtime_query(Dictionary p_result) {
	if (runtime_query.is_empty()) {
		return;
	}
	const String target = runtime_query.get("target", String());
	p_result["target"] = target;
	p_result["runtime_epoch"] = (int64_t)runtime_epoch;
	runtime_query["result"] = _runtime_observation_result(p_result);
}

Dictionary SolersRuntimeObservation::_runtime_observation_result(Dictionary p_result) const {
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

void SolersRuntimeObservation::_bind_runtime_debugger() {
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const ObjectID debugger_id = debugger ? debugger->get_instance_id() : ObjectID();
	if (debugger_id == observed_debugger_id) {
		return;
	}
	if (ScriptEditorDebugger *previous = Object::cast_to<ScriptEditorDebugger>(ObjectDB::get_instance(observed_debugger_id))) {
		previous->disconnect(SNAME("started"), callable_mp(this, &SolersRuntimeObservation::_runtime_started));
		previous->disconnect(SNAME("stopped"), callable_mp(this, &SolersRuntimeObservation::_runtime_stopped));
		previous->disconnect(SNAME("output"), callable_mp(this, &SolersRuntimeObservation::_runtime_output));
		previous->disconnect(SNAME("breaked"), callable_mp(this, &SolersRuntimeObservation::_runtime_breaked));
		previous->disconnect(SNAME("debug_data"), callable_mp(this, &SolersRuntimeObservation::_runtime_debug_data));
		previous->disconnect(SNAME("remote_tree_updated"), callable_mp(this, &SolersRuntimeObservation::_runtime_tree_updated));
		previous->disconnect(SNAME("stack_dump"), callable_mp(this, &SolersRuntimeObservation::_runtime_stack_dump));
	}
	observed_debugger_id = debugger_id;
	if (!debugger) {
		return;
	}
	debugger->connect(SNAME("started"), callable_mp(this, &SolersRuntimeObservation::_runtime_started));
	debugger->connect(SNAME("stopped"), callable_mp(this, &SolersRuntimeObservation::_runtime_stopped));
	debugger->connect(SNAME("output"), callable_mp(this, &SolersRuntimeObservation::_runtime_output));
	debugger->connect(SNAME("breaked"), callable_mp(this, &SolersRuntimeObservation::_runtime_breaked));
	debugger->connect(SNAME("debug_data"), callable_mp(this, &SolersRuntimeObservation::_runtime_debug_data));
	debugger->connect(SNAME("remote_tree_updated"), callable_mp(this, &SolersRuntimeObservation::_runtime_tree_updated));
	debugger->connect(SNAME("stack_dump"), callable_mp(this, &SolersRuntimeObservation::_runtime_stack_dump));
	if (debugger->is_session_active()) {
		runtime_epoch++;
		_append_runtime_event(SNAME("started"), Dictionary(), true);
	}
}

void SolersRuntimeObservation::poll() {
	_bind_runtime_debugger();
}

bool SolersRuntimeObservation::is_runtime_observation_ready(const Dictionary &p_args) const {
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

Dictionary SolersRuntimeObservation::observe_runtime(const Dictionary &p_args, int p_token_budget) {
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
	const uint64_t since_cursor = p_args.has("since_cursor") ? (int64_t)p_args["since_cursor"] : (target == "performance" && performance_sample_cursor > 0 ? performance_sample_cursor - 1 : 0);
	const bool include_events = (bool)p_args.get("include_events", false);
	const int max_events = CLAMP((int)p_args.get("max_events", include_events ? 32 : 0), 0, 256);
	Array events;
	Dictionary error_digest;
	Dictionary performance_sample;
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
		if (target == "performance" && event_type == SNAME("performance")) {
			performance_sample = event;
		}
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
	if (!performance_sample.is_empty()) {
		result["sample"] = performance_sample;
	}
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

bool SolersRuntimeObservation::get_runtime_property(uint64_t p_epoch, const NodePath &p_node_path, ObjectID p_object_id, const StringName &p_property, Variant &r_value, PropertyInfo &r_info, String &r_observation_id) const {
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

void SolersRuntimeObservation::clear_runtime_control_result() {
	runtime_control_result.clear();
}

Dictionary SolersRuntimeObservation::get_runtime_control_result(const String &p_call_id) const {
	if (String(runtime_control_result.get("call_id", String())) != p_call_id) {
		return Dictionary();
	}
	return runtime_control_result.duplicate(true);
}

Dictionary SolersRuntimeObservation::start_runtime_script(const Dictionary &p_args, const String &p_call_id) {
	_bind_runtime_debugger();
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const uint64_t epoch = (int64_t)p_args.get("runtime_epoch", 0);
	if (!debugger || !debugger->is_session_active()) {
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "RUNTIME_NOT_CONNECTED" }, { "message", "Start the project before running a runtime script." }, { "recoverable", true } }) } });
	}
	if (epoch == 0 || epoch != runtime_epoch) {
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "STALE_RUNTIME_EPOCH" }, { "message", "runtime_epoch must match the current runtime.observe receipt." }, { "recoverable", true } }) } });
	}
	if (!runtime_script.is_empty()) {
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "RUNTIME_SCRIPT_BUSY" }, { "message", "Wait for the active runtime script to finish." }, { "recoverable", true } }) } });
	}
	const int timeout_msec = CLAMP((int)p_args.get("timeout_msec", 30000), 1000, 600000);
	runtime_script["call_id"] = p_call_id;
	runtime_script["runtime_epoch"] = (int64_t)epoch;
	runtime_script["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + timeout_msec);
	debugger->send_message("solers:run_script", { p_call_id, (int64_t)epoch, p_args.get("source", String()), runtime_script["deadline_msec"] });
	return Dictionary({ { "ok", true }, { "data", Dictionary({ { "status", "pending" }, { "poll_args", Dictionary({ { "call_id", p_call_id } }) } }) } });
}

bool SolersRuntimeObservation::is_runtime_script_ready(const Dictionary &p_args) const {
	if (String(runtime_script.get("call_id", String())) != String(p_args.get("call_id", String()))) {
		return true;
	}
	return runtime_script.has("result") || OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)runtime_script.get("deadline_msec", 0);
}

Dictionary SolersRuntimeObservation::poll_runtime_script(const Dictionary &p_args) {
	const String call_id = p_args.get("call_id", String());
	if (String(runtime_script.get("call_id", String())) != call_id) {
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "RUNTIME_SCRIPT_NOT_FOUND" }, { "message", "The runtime script is no longer active." }, { "recoverable", false } }) } });
	}
	if (runtime_script.has("result")) {
		Dictionary result = runtime_script.get("result", Dictionary());
		result.erase("call_id");
		result.erase("runtime_epoch");
		return result;
	}
	if (OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)runtime_script.get("deadline_msec", 0)) {
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		if (debugger && debugger->is_session_active()) {
			debugger->send_message("solers:cancel_script", { call_id, runtime_script.get("runtime_epoch", 0) });
		}
		EditorRunBar *run_bar = EditorRunBar::get_singleton();
		if (run_bar && run_bar->is_playing()) {
			run_bar->stop_playing();
		}
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "SCRIPT_TIMEOUT" }, { "message", "runtime.script exceeded its declared timeout, so the isolated game process was stopped." }, { "recoverable", true } }) } });
	}
	return Dictionary({ { "ok", true }, { "data", Dictionary({ { "status", "pending" }, { "poll_args", p_args } }) } });
}

void SolersRuntimeObservation::clear_runtime_script(const String &p_call_id) {
	if (p_call_id.is_empty() || String(runtime_script.get("call_id", String())) == p_call_id) {
		runtime_script.clear();
	}
}

SolersRuntimeObservation::SolersRuntimeObservation() {
	_bind_runtime_debugger();
}
