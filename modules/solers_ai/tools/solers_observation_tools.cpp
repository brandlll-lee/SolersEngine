/**************************************************************************/
/*  solers_observation_tools.cpp                                          */
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

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/run/editor_run_bar.h"
#include "editor/run/game_view_plugin.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_builtin_skills.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_project_observation.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_runtime_observation.h"
#include "modules/solers_ai/core/solers_scene_observation.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/plugins/solers_plugin.h"

Dictionary SolersToolRegistry::_inspect_engine(const Dictionary &p_args) {
	if (!reflection_service) {
		return _error("ENGINE_INSPECTION_UNAVAILABLE", "ClassDB inspection is unavailable.", false);
	}
	const Array classes = p_args.get("classes", Array());
	if (classes.is_empty()) {
		Dictionary search_args = p_args.duplicate(true);
		search_args.erase("classes");
		Dictionary result = reflection_service->search_classes(search_args);
		return result;
	}
	Array inspected_classes;
	Array errors;
	for (int i = 0; i < classes.size(); i++) {
		const Variant value = classes[i];
		Dictionary request = value;
		if (!request.has("max_members") && p_args.has("max_members")) {
			request["max_members"] = p_args["max_members"];
		}
		const Dictionary inspected = reflection_service->introspect_class(request);
		if (!(bool)inspected.get("ok", false)) {
			Dictionary error;
			error["request_index"] = i;
			error["class_name"] = Dictionary(value).get("class_name", String());
			error["error"] = inspected.get("error", Dictionary());
			errors.push_back(error);
			continue;
		}
		Dictionary class_data = inspected.get("data", Dictionary());
		class_data["request_index"] = i;
		inspected_classes.push_back(class_data);
	}
	Dictionary data;
	data["classes"] = inspected_classes;
	data["errors"] = errors;
	data["requested_count"] = classes.size();
	data["complete"] = errors.is_empty();
	return _ok(data);
}

Dictionary SolersToolRegistry::_with_added_tools(const Dictionary &p_result, const Array &p_tools) const {
	if (!(bool)p_result.get("ok", false) || p_tools.is_empty()) {
		return p_result;
	}
	Dictionary result = p_result.duplicate(true);
	Array added = result.get("added_tools", Array());
	for (const Variant &name : p_tools) {
		SolersTool *const *tool = tools.getptr(StringName(String(name)));
		if (tool && *tool && (*tool)->exposure() == SolersToolExposure::DEFERRED && !added.has(name)) {
			added.push_back(name);
		}
	}
	result["added_tools"] = added;
	return result;
}

static GameViewDebugger *_solers_game_view_debugger() {
	EditorData &editor_data = EditorNode::get_editor_data();
	for (int i = 0; i < editor_data.get_editor_plugin_count(); i++) {
		if (GameViewPluginBase *plugin = Object::cast_to<GameViewPluginBase>(editor_data.get_editor_plugin(i))) {
			return plugin->get_debugger().ptr();
		}
	}
	return nullptr;
}

Dictionary SolersToolRegistry::build_delivery_report(const Dictionary &p_args, int p_token_budget) const {
	ERR_FAIL_NULL_V(project_observation, Dictionary());
	Dictionary report = project_observation->inspect_project_delivery(p_args, p_token_budget);
	if (!resource_service) {
		return report;
	}
	const Dictionary export_result = resource_service->validate_export_presets(Dictionary({ { "debug", p_args.get("debug_export", false) } }));
	if (!(bool)export_result.get("ok", false)) {
		report["export"] = export_result;
		return report;
	}
	const Dictionary export_data = export_result.get("data", Dictionary());
	report["export"] = export_data;
	Array blockers = report.get("blockers", Array());
	Array advisories = report.get("advisories", Array());
	if ((int)export_data.get("preset_count", 0) == 0) {
		advisories.push_back(Dictionary({ { "code", "NO_EXPORT_PRESET" } }));
	} else if (!(bool)export_data.get("valid", false)) {
		blockers.push_back(Dictionary({ { "code", "INVALID_EXPORT_PRESET" }, { "error_count", export_data.get("error_count", 0) }, { "missing_template_count", export_data.get("missing_template_count", 0) } }));
	}
	report["blockers"] = blockers;
	report["advisories"] = advisories;
	const String session_id = p_args.get("_session_id", String());
	if (asset_service && !session_id.is_empty()) {
		HashSet<String> unreferenced;
		for (const Variant &item : Array(report.get("unreferenced_from_roots", Array()))) {
			unreferenced.insert(String(Dictionary(item).get("path", String())));
		}
		Array artifacts;
		for (const Variant &item : asset_service->list_assets()) {
			const Dictionary asset = item;
			if (String(asset.get("session_id", String())) != session_id || !(bool)asset.get("in_current_project", false)) {
				continue;
			}
			const Array files = Array(asset.get("project_entrypoints", Array())).is_empty() ? Array(asset.get("project_files", Array())) : Array(asset.get("project_entrypoints", Array()));
			bool consumed = false;
			for (const Variant &file : files) {
				consumed = consumed || !unreferenced.has(String(file));
			}
			if (!consumed) {
				artifacts.push_back(Dictionary({ { "asset_id", asset.get("id", String()) }, { "files", files }, { "sidecar", asset.get("sidecar_file", String()) } }));
			}
		}
		report["unconsumed_agent_artifacts"] = artifacts;
		if (!artifacts.is_empty()) {
			blockers.push_back(Dictionary({ { "code", "UNCONSUMED_AGENT_ARTIFACTS" }, { "count", artifacts.size() }, { "artifacts", artifacts } }));
			report["blockers"] = blockers;
		}
	}
	return report;
}

Dictionary SolersToolRegistry::_run_control(const Dictionary &p_args, const String &p_call_id) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	ERR_FAIL_NULL_V(run_bar, _error("EDITOR_RUN_BAR_UNAVAILABLE", "The editor runtime controller is not available.", false));
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const String action = p_args.get("action", String());
	const bool was_playing = run_bar->is_playing();
	bool command_accepted = false;
	if (action == "set_input_actions") {
		ERR_FAIL_NULL_V(runtime_observation, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		if (!debugger || !debugger->is_session_active()) {
			return _error("RUNTIME_NOT_CONNECTED", "Start the project before setting runtime input.");
		}
		const int64_t epoch = p_args.get("runtime_epoch", 0);
		const int64_t current_epoch = runtime_observation->get_runtime_status().get("runtime_epoch", 0);
		const int physics_frames = p_args.get("physics_frames", 0);
		const Variant observations = p_args.get("observations", Variant());
		if (p_call_id.is_empty() || epoch <= 0 || epoch != current_epoch || !p_args.has("actions") || physics_frames <= 0 || observations.get_type() != Variant::ARRAY || Array(observations).is_empty()) {
			return _error("STALE_RUNTIME_EPOCH", "set_input_actions requires actions, positive physics_frames, observations, and the current runtime_epoch returned by runtime.observe.");
		}
		runtime_observation->clear_runtime_control_result();
		debugger->send_message("solers:set_input_actions", { p_call_id, epoch, p_args["actions"], physics_frames, observations });
		Dictionary poll_args;
		poll_args["action"] = action;
		poll_args["call_id"] = p_call_id;
		poll_args["runtime_epoch"] = epoch;
		Dictionary pending;
		pending["status"] = "pending";
		pending["poll_args"] = poll_args;
		return _ok(pending);
	}
	if (action == "set_property") {
		ERR_FAIL_NULL_V(runtime_observation, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		if (!debugger || !debugger->is_session_active()) {
			return _error("RUNTIME_NOT_CONNECTED", "Start the project before editing runtime state.");
		}
		if (runtime_observation->has_runtime_query()) {
			return _error("RUNTIME_QUERY_BUSY", "Wait for the active native runtime observation before changing runtime state.");
		}
		const uint64_t epoch = (int64_t)p_args.get("runtime_epoch", 0);
		const NodePath node_path = NodePath(p_args.get("node_path", String()));
		ObjectID object_id;
		const bool valid_object_id = solers_object_id_from_variant(p_args.get("object_id", Variant()), object_id);
		const StringName property = p_args.get("property", String());
		const String observation_id = p_args.get("observation_id", String());
		if (node_path.is_empty() || !node_path.is_absolute() || !valid_object_id || property.is_empty() || observation_id.is_empty() || !p_args.has("value")) {
			return _error("INVALID_ARGUMENT", "set_property requires runtime_epoch, absolute node_path, object_id, property, observation_id, and value from runtime.observe.");
		}
		Variant before;
		PropertyInfo property_info;
		String cached_observation_id;
		if (!runtime_observation->get_runtime_property(epoch, node_path, object_id, property, before, property_info, cached_observation_id) || cached_observation_id != observation_id) {
			return _error("STALE_RUNTIME_OBSERVATION", "Observe this exact runtime node property in the current epoch before changing it.");
		}
		Variant value;
		String coercion_error;
		if (!solers_coerce_variant_value(property_info, p_args.get("value", Variant()), value, coercion_error)) {
			return _error("INVALID_PROPERTY_VALUE", coercion_error);
		}
		Dictionary query_args;
		query_args["target"] = "scene";
		Array node_paths;
		node_paths.push_back(String(node_path));
		query_args["node_paths"] = node_paths;
		Array properties;
		properties.push_back(property);
		query_args["properties"] = properties;
		Dictionary pending = runtime_observation->observe_runtime(query_args);
		if (pending.get("status", String()) != "pending") {
			return _error("RUNTIME_VERIFY_UNAVAILABLE", "The native debugger could not start post-write verification.");
		}
		Dictionary poll_args = pending.get("poll_args", Dictionary());
		poll_args["action"] = action;
		poll_args["runtime_epoch"] = (int64_t)epoch;
		poll_args["node_path"] = String(node_path);
		poll_args["object_id"] = solers_object_id_to_string(object_id);
		poll_args["property"] = property;
		poll_args["before"] = before;
		poll_args["value"] = value;
		poll_args["phase"] = "prewrite";
		pending["poll_args"] = poll_args;
		return _ok(pending);
	}
	if (action == "play_current_scene") {
		if (!was_playing && !editor_interface->get_edited_scene_root()) {
			return _error("CURRENT_SCENE_UNAVAILABLE", "Open a scene before starting the project.");
		}
		if (!was_playing) {
			run_bar->play_current_scene();
		}
		command_accepted = true;
	} else if (action == "stop") {
		if (was_playing) {
			run_bar->stop_playing();
		}
		command_accepted = true;
	} else if (action == "suspend" || action == "resume" || action == "next_frame") {
		GameViewDebugger *game_debugger = _solers_game_view_debugger();
		if (!debugger || !debugger->is_session_active() || !game_debugger) {
			return _error("RUNTIME_NOT_CONNECTED", "The native Game View debugger is not connected.");
		}
		if (action == "next_frame") {
			game_debugger->next_frame();
		} else {
			game_debugger->set_suspend(action == "suspend");
		}
		command_accepted = true;
	} else if (action == "debug_break") {
		if (!debugger || !debugger->is_session_active()) {
			return _error("RUNTIME_NOT_CONNECTED", "The native script debugger is not connected.");
		}
		if (!debugger->is_breaked()) {
			debugger->debug_break();
		}
		command_accepted = true;
	} else if (action == "debug_continue" || action == "debug_step" || action == "debug_next" || action == "debug_out") {
		if (!debugger || !debugger->is_debuggable()) {
			return _error("RUNTIME_NOT_BREAKED", "Break at a debuggable stack frame before stepping or continuing.");
		}
		if (action == "debug_continue") {
			debugger->debug_continue();
		} else if (action == "debug_step") {
			debugger->debug_step();
		} else if (action == "debug_next") {
			debugger->debug_next();
		} else {
			debugger->debug_out();
		}
		command_accepted = true;
	} else {
		return _error("INVALID_ARGUMENT", "Unknown runtime action.");
	}
	if (command_accepted) {
		Dictionary data;
		data["action"] = action;
		data["command_accepted"] = true;
		data["runtime_epoch"] = runtime_observation ? (int64_t)runtime_observation->get_runtime_status().get("runtime_epoch", 0) : 0;
		return _ok(data);
	}

	return _error("RUNTIME_CONTROL_FAILED", "The native runtime command was not accepted.", false);
}

bool SolersToolRegistry::_is_runtime_control_ready(const Dictionary &p_args) const {
	const String action = p_args.get("action", String());
	if (action == "set_input_actions") {
		if (!runtime_observation) {
			return true;
		}
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		return !debugger || !debugger->is_session_active() ||
				(int64_t)runtime_observation->get_runtime_status().get("runtime_epoch", 0) != (int64_t)p_args.get("runtime_epoch", 0) ||
				!runtime_observation->get_runtime_control_result(p_args.get("call_id", String())).is_empty();
	}
	if (action == "set_property") {
		return !runtime_observation || runtime_observation->is_runtime_observation_ready(p_args);
	}
	return true;
}

Dictionary SolersToolRegistry::_poll_runtime_control(const Dictionary &p_args) const {
	const String action = p_args.get("action", String());
	if (action == "set_input_actions") {
		ERR_FAIL_NULL_V(runtime_observation, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		const int64_t epoch = p_args.get("runtime_epoch", 0);
		if (!debugger || !debugger->is_session_active()) {
			return _error("RUNTIME_NOT_CONNECTED", "The runtime stopped before applying input.");
		}
		if ((int64_t)runtime_observation->get_runtime_status().get("runtime_epoch", 0) != epoch) {
			return _error("STALE_RUNTIME_EPOCH", "The runtime epoch changed before applying input.");
		}
		const Dictionary result = runtime_observation->get_runtime_control_result(p_args.get("call_id", String()));
		if (result.is_empty()) {
			Dictionary pending;
			pending["status"] = "pending";
			pending["poll_args"] = p_args;
			return _ok(pending);
		}
		if (!(bool)result.get("ok", false)) {
			return _error(result.get("code", "RUNTIME_INPUT_REJECTED"), result.get("message", "The runtime rejected the input state."));
		}
		Dictionary data;
		data["action"] = action;
		data["runtime_epoch"] = epoch;
		data["input_state_applied"] = true;
		data["physics_frames"] = result.get("physics_frames", 0);
		data["before"] = result.get("before", Array());
		data["after"] = result.get("after", Array());
		data["availability"] = result.get("availability", Dictionary());
		return _ok(data);
	}
	if (action == "set_property") {
		ERR_FAIL_NULL_V(runtime_observation, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		Dictionary observed = runtime_observation->observe_runtime(p_args);
		if (observed.get("status", String()) == "pending") {
			return _ok(observed);
		}
		if (String(Dictionary(observed.get("availability", Dictionary())).get("state", String())) != "complete") {
			return _error("RUNTIME_VERIFY_UNAVAILABLE", "The native debugger could not read the runtime property.");
		}
		const NodePath node_path = NodePath(p_args.get("node_path", String()));
		ObjectID object_id;
		if (!solers_object_id_from_variant(p_args.get("object_id", Variant()), object_id)) {
			return _error("RUNTIME_CONTINUATION_INVALID", "The runtime object_id is invalid.", false);
		}
		const StringName property = p_args.get("property", String());
		Variant current;
		PropertyInfo property_info;
		String observation_id;
		if (!runtime_observation->get_runtime_property((int64_t)p_args.get("runtime_epoch", 0), node_path, object_id, property, current, property_info, observation_id)) {
			return _error("RUNTIME_OBJECT_DISAPPEARED", "The canonical runtime node or property disappeared before verification.");
		}
		if (String(p_args.get("phase", String())) == "prewrite") {
			if (current != p_args.get("before", Variant())) {
				return _error("RUNTIME_STATE_CONFLICT", "The runtime property changed after it was observed; observe it again before writing.");
			}
			EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
			ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
			if (!debugger || !debugger->is_session_active() || (int64_t)runtime_observation->get_runtime_status().get("runtime_epoch", 0) != (int64_t)p_args.get("runtime_epoch", 0)) {
				return _error("STALE_RUNTIME_EPOCH", "The runtime changed before the property write.");
			}
			debugger->update_remote_object(object_id, property, p_args.get("value", Variant()));
			Dictionary query_args;
			query_args["target"] = "scene";
			Array node_paths;
			node_paths.push_back(String(node_path));
			query_args["node_paths"] = node_paths;
			Array requested_properties;
			requested_properties.push_back(property);
			query_args["properties"] = requested_properties;
			Dictionary pending = runtime_observation->observe_runtime(query_args);
			if (pending.get("status", String()) != "pending") {
				return _error("RUNTIME_VERIFY_UNAVAILABLE", "The native debugger could not start post-write verification.");
			}
			Dictionary poll_args = pending.get("poll_args", Dictionary());
			for (const char *key : { "action", "runtime_epoch", "node_path", "object_id", "property", "before", "value" }) {
				poll_args[key] = p_args.get(key, Variant());
			}
			poll_args["phase"] = "postwrite";
			pending["poll_args"] = poll_args;
			return _ok(pending);
		}
		if (String(p_args.get("phase", String())) != "postwrite") {
			return _error("RUNTIME_CONTINUATION_INVALID", "Unknown runtime property transaction phase.", false);
		}
		if (current != p_args.get("value", Variant())) {
			return _error("RUNTIME_POSTCONDITION_FAILED", "Godot did not retain the requested runtime property value.");
		}
		Dictionary data;
		data["action"] = action;
		data["runtime_only"] = true;
		data["runtime_epoch"] = observed.get("runtime_epoch", 0);
		data["node_path"] = String(node_path);
		data["object_id"] = solers_object_id_to_string(object_id);
		data["property"] = property;
		data["before"] = solers_summarize_display_value(p_args.get("before", Variant()));
		data["after"] = solers_summarize_display_value(current);
		return _ok(data);
	}
	return _error("RUNTIME_CONTINUATION_INVALID", "Only runtime property verification has a continuation.", false);
}

void SolersToolRegistry::_register_observation_tools() {
	SolersProjectObservation *project = project_observation;
	SolersRuntimeObservation *runtime = runtime_observation;
	SolersSceneObservation *scene = scene_observation;

	if (project) {
		_add_observe_exposed("project.search", "Discover project paths or search text files on the worker. An empty path query lists the project skeleton; text and symbol require a query. Results use a stable cursor.", R"({"type":"object","properties":{"type":{"type":"string","enum":["path","text","symbol"]},"query":{"type":"string","description":"Case-insensitive path/text query; may be empty only for type=path."},"cursor":{"type":"integer","minimum":0},"max_results":{"type":"integer","minimum":1,"description":"Optional page size; the result budget remains authoritative."}},"required":["type","query"],"additionalProperties":false})", SolersToolExposure::MODEL, [this, project](const SolersToolContext &ctx, const Dictionary &a) {
				const String type = a.get("type", String());
				if (type != "path" && String(a.get("query", String())).is_empty()) {
					return _error("INVALID_ARGUMENT", "query is required for text and symbol search.");
				}
				return _ok(project->search_project(a, ctx.result_token_budget)); }, {}, {}, {}, SolersToolUiKind::SEARCH, SolersToolExecution::WORKER_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY, PackedStringArray({ "/query", "/type" }));
		_add_observe_exposed("project.read_file", "Read a bounded line range from a project text file. Continue from next_line; PackedScene defaults to a native digest unless raw=true is required for source editing.", R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to read."},"line_start":{"type":"integer","minimum":1},"line_count":{"type":"integer","minimum":1},"raw":{"type":"boolean","description":"Only for PackedScene source editing. Default false."}},"required":["path"],"additionalProperties":false})", SolersToolExposure::MODEL, [this, project](const SolersToolContext &ctx, const Dictionary &a) {
				const Dictionary file = project->read_project_file(a.get("path", String()), (int)a.get("line_start", 1), (int)a.get("line_count", 200), (bool)a.get("raw", false), ctx.result_token_budget);
				if (!(bool)file.get("ok", false)) {
					Dictionary error;
					error["code"] = file.get("code", "READ_FAILED");
					error["message"] = file.get("error", String("Unable to read file."));
					error["recoverable"] = true;
					Dictionary result;
					result["ok"] = false;
					result["error"] = error;
					if (file.has("digest")) {
						Dictionary data;
						data["digest"] = file["digest"];
						data["path"] = file.get("path", String());
						result["data"] = data;
					}
					return result;
				}
				return _ok(file); }, _access_by_arg("read", "project:", "path"), {}, {}, SolersToolUiKind::READ, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
	}
	if (runtime) {
		_add_observe_exposed("runtime.observe", "Observe one canonical runtime snapshot through Godot's native debugger. Scene returns typed property receipts; spatial returns post-draw subtree AABBs, camera projection, and physics ray facts.", R"({"type":"object","properties":{"target":{"type":"string","enum":["scene","spatial","stack","performance"]},"node_paths":{"type":"array","items":{"type":"string","pattern":"^/"},"maxItems":64,"uniqueItems":true},"focus_paths":{"type":"array","items":{"type":"string","pattern":"^/"},"minItems":1,"maxItems":32,"uniqueItems":true},"path_prefix":{"type":"string","pattern":"^/"},"name_contains":{"type":"string"},"class_name":{"type":"string"},"cursor":{"type":"integer","minimum":0},"properties":{"type":"array","items":{"type":"string","minLength":1},"maxItems":64,"uniqueItems":true},"max_results":{"type":"integer","minimum":1}},"required":["target"],"additionalProperties":false})", SolersToolExposure::MODEL, [this, runtime](const SolersToolContext &ctx, const Dictionary &a) { return _ok(runtime->observe_runtime(a, ctx.result_token_budget)); }, {}, [this, runtime](const SolersToolContext &ctx, const Dictionary &a) { return _ok(runtime->observe_runtime(a, ctx.result_token_budget)); }, [runtime](const SolersToolContext &, const Dictionary &a) { return runtime->is_runtime_observation_ready(a); }, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::RUNTIME, SolersOperationMode::QUERY, PackedStringArray({ "/focus_paths/0", "/node_paths/0", "/target" }));
	}
	if (project) {
		_add_observe_exposed("project.delivery_report", "Inspect current project delivery facts through ProjectSettings, ResourceLoader, EditorFileSystem, InputMap, UndoRedo, and EditorExport. Unreferenced and duplicate files are advisories, never automatic deletion decisions.", R"({"type":"object","properties":{"roots":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"string","pattern":"^res://"},"description":"Additional authoritative roots for dynamically loaded content."},"debug_export":{"type":"boolean"}},"additionalProperties":false})", SolersToolExposure::DEFERRED, [this](const SolersToolContext &ctx, const Dictionary &a) { Dictionary args = a; args["_session_id"] = ctx.session_id; return _ok(build_delivery_report(args, ctx.result_token_budget)); }, {}, {}, {}, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
	}
	if (scene) {
		SolersToolHostPolicy capture_host;
		capture_host.required_model_inputs.push_back("image");
		// Capture returns Godot-owned scene identity alongside the image.
		// The runtime bridge reports the loaded root and scene path.
		// The model decides whether those facts explain the requested result.
		// No semantic visibility or visual-quality verdict is produced here.
		_add_observe_exposed("render.capture", "Capture content-addressed visual evidence from an explicit native state. Edited-scene receipts bind pixels to the World3D fingerprint; runtime receipts bind pixels to the runtime epoch. focus_paths return geometric framing facts, not a semantic visibility verdict. debug_draw uses Godot's Viewport enum; inspect it with engine.describe.", R"({"type":"object","properties":{"target":{"type":"string","enum":["editor","camera","focus","top_down","orthographic","runtime"]},"source_state":{"type":"object","properties":{"history_id":{"type":"integer"},"version":{"type":"integer","minimum":0},"root_object_id":{"type":"string","pattern":"^-?[0-9]+$"}},"required":["history_id","version"],"additionalProperties":true},"camera_path":{"type":"string"},"axis":{"type":"string","enum":["x","y","z"]},"direction":{"type":"string","enum":["positive","negative"]},"focus_paths":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":32,"uniqueItems":true},"include_render_state":{"type":"boolean","description":"Include the full World3D state; hashes remain available by default."},"section_position":{"type":"number"},"debug_draw":{"type":"integer","minimum":0}},"required":["target"],"additionalProperties":false})", SolersToolExposure::MODEL, [scene](const SolersToolContext &, const Dictionary &a) { return scene->capture_viewport(a); }, {}, [scene](const SolersToolContext &, const Dictionary &a) { return scene->poll_viewport_capture(a); }, [scene](const SolersToolContext &, const Dictionary &a) { return scene->is_viewport_capture_ready(a); }, SolersToolUiKind::CAPTURE, SolersToolExecution::MAIN_THREAD, capture_host, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY, PackedStringArray({ "/focus_paths/0", "/camera_path", "/target" }));
	}

	if (resource_service) {
		SolersResourceService *svc = resource_service;
		_add_observe_exposed("export.list_presets", "List Godot export platforms and export presets from the current project.", R"({"type":"object","properties":{"include_platforms":{"type":"boolean","description":"Include available export platforms. Default true."}}})", SolersToolExposure::DEFERRED, [svc](const SolersToolContext &, const Dictionary &a) { return svc->list_export_presets(a); }, {}, {}, {}, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
		_add_observe_exposed("export.validate_presets", "Validate configured export presets without exporting build artifacts.", R"({"type":"object","properties":{"debug":{"type":"boolean","description":"Validate against the debug export template. Default false."}}})", SolersToolExposure::DEFERRED, [svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_export_presets(a); }, {}, {}, {}, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
		_add_operation(SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY, "export.run_preset", "Run Godot's native EditorExportPlatform::export_project for one export preset.",
				R"({"type":"object","properties":{"preset_index":{"type":"integer","description":"Export preset index from export.list_presets."},"preset_name":{"type":"string","description":"Export preset name when index is unknown."},"debug":{"type":"boolean","description":"Export debug build. Default false."},"export_path":{"type":"string","description":"Optional output path override; defaults to the preset export_path."}}})",
				SolersPermissionManager::PERMISSION_EXPORT_BUILD, SolersToolMutationDomain::IRREVERSIBLE,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->run_export_preset(a); }, {});
	}
}

void SolersToolRegistry::_register_runtime_tools() {
	const SolersPermissionManager::Permission run_project = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	_add_operation(SolersOperationDomain::RUNTIME, SolersOperationMode::APPLY, "runtime.control", "Control Godot's active debugger, apply the complete Solers-owned input action state across declared physics frames with before/after property facts, or make one preconditioned runtime-only property change. For set_property, copy runtime_epoch, node_path, object_id, property, and observation_id exactly from one runtime.observe node receipt. Omitted input actions are released; an empty actions array releases all.", R"({"type":"object","properties":{"action":{"type":"string","enum":["play_current_scene","stop","suspend","resume","next_frame","debug_break","debug_continue","debug_step","debug_next","debug_out","set_input_actions","set_property"]},"runtime_epoch":{"type":"integer","minimum":0},"actions":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"object","properties":{"name":{"type":"string","minLength":1},"strength":{"type":"number","exclusiveMinimum":0,"maximum":1}},"required":["name","strength"],"additionalProperties":false}},"physics_frames":{"type":"integer","minimum":1},"observations":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{"node_path":{"type":"string","pattern":"^/"},"properties":{"type":"array","minItems":1,"maxItems":32,"uniqueItems":true,"items":{"type":"string","minLength":1}}},"required":["node_path","properties"],"additionalProperties":false}},"node_path":{"type":"string","pattern":"^/"},"object_id":{"type":"string","pattern":"^-?[0-9]+$"},"property":{"type":"string","minLength":1},"observation_id":{"type":"string","minLength":64,"maxLength":64},"value":{}},"required":["action"],"additionalProperties":false})", run_project, SolersToolMutationDomain::IRREVERSIBLE, [this](const SolersToolContext &ctx, const Dictionary &a) { return _run_control(a, ctx.call_id); }, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "runtime:";
				accesses.push_back(access);
				return accesses; }, StringName(), [this](const SolersToolContext &, const Dictionary &a) { return _poll_runtime_control(a); }, [this](const SolersToolContext &, const Dictionary &a) { return _is_runtime_control_ready(a); });
	if (!script_service || !runtime_observation) {
		return;
	}
	Vector<String> redact;
	redact.push_back("source");
	_add("runtime.script", "Run bounded GDScript inside the real game process. The script extends RefCounted and defines func run(ctx); ctx.subject is the live root Window. The function may await native frame or physics signals and returns only after the real runtime finishes. A timeout stops the game process so the editor remains responsive.", R"({"type":"object","properties":{"runtime_epoch":{"type":"integer","minimum":1},"source":{"type":"string","minLength":1},"timeout_msec":{"type":"integer","minimum":1000,"maximum":600000}},"required":["runtime_epoch","source"],"additionalProperties":false})", run_project, SolersToolMutationDomain::IRREVERSIBLE, redact, SolersToolExposure::MODEL, [this](const SolersToolContext &ctx, const Dictionary &a) {
				const Dictionary validation = script_service->validate_script(Dictionary({ { "path", "res://solers_runtime_script.gd" }, { "source", a.get("source", String()) } }));
				if (!(bool)validation.get("ok", false) || !(bool)Dictionary(validation.get("data", Dictionary())).get("valid", false)) {
					Dictionary failure = _error("SCRIPT_VALIDATION_FAILED", "runtime.script did not pass Godot's GDScript parser.");
					failure["data"] = validation.get("data", Dictionary());
					return failure;
				}
				return runtime_observation->start_runtime_script(a, ctx.call_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &) { return Array({ Dictionary({ { "mode", "write" }, { "key", "runtime:" } }) }); }, [this](const SolersToolContext &, const Dictionary &a) { return runtime_observation->poll_runtime_script(a); }, [this](const SolersToolContext &, const Dictionary &a) { return runtime_observation->is_runtime_script_ready(a); }, [this](const SolersToolContext &ctx, const Dictionary &, const Dictionary &) { runtime_observation->clear_runtime_script(ctx.call_id); }, {}, {}, SolersToolUiKind::RUN, {}, SNAME("SceneTree"), SolersOperationDomain::RUNTIME, SolersOperationMode::APPLY, {}, SNAME("runtime"));
}

void SolersToolRegistry::_register_reflection_tools() {
	if (!reflection_service) {
		return;
	}
	SolersReflectionService *ref = reflection_service;
	const SolersPermissionManager::Permission edit_scene = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	_add_observe_exposed("scene.inspect", "Inspect the live edited scene tree and typed native node facts. Use node filters for discovery and node_paths for exact inspection; results carry a scene receipt and cursors.", R"({"type":"object","properties":{"include_selection":{"type":"boolean"},"node_paths":{"type":"array","items":{"type":"string"},"uniqueItems":true,"minItems":1,"maxItems":64},"path_prefix":{"type":"string"},"name_contains":{"type":"string"},"class_name":{"type":"string"},"script_path":{"type":"string","pattern":"^res://"},"cursor":{"type":"integer","minimum":0},"max_results":{"type":"integer","minimum":1},"include_connections":{"type":"boolean"},"properties":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":128}},"additionalProperties":false})", SolersToolExposure::MODEL, [this, ref](const SolersToolContext &ctx, const Dictionary &a) {
				Node *edited_root = SceneTree::get_singleton()->get_edited_scene_root();
				if (!edited_root) {
					return _error("NO_EDITED_SCENE", "Open a scene before inspecting its live nodes.", true);
				}
				Dictionary data;
				const Dictionary queried = scene_observation->query_scene_nodes(a, ctx.result_token_budget);
				const Array queried_nodes = queried.get("nodes", Array());
				Array node_paths;
				for (int i = 0; i < queried_nodes.size(); i++) {
					node_paths.push_back(Dictionary(queried_nodes[i]).get("node_path", String()));
				}
				if ((bool)a.get("include_selection", false)) {
					data["selection"] = scene_observation->get_selection();
				}
				if (!node_paths.is_empty()) {
					Dictionary inspect_args = a.duplicate(true);
					inspect_args["node_paths"] = node_paths;
					const Dictionary inspected = ref->inspect_nodes(inspect_args);
					if (!(bool)inspected.get("ok", false)) {
						return inspected;
					}
					data.merge(inspected.get("data", Dictionary()), true);
				} else {
					data["nodes"] = queried_nodes;
					data["count"] = 0;
				}
				data["query_errors"] = queried.get("errors", Array());
				data["cursor"] = queried.get("cursor", 0);
				if (queried.has("next_cursor")) {
					data["next_cursor"] = queried["next_cursor"];
				}
				if (queried_nodes.is_empty() && !Array(queried.get("errors", Array())).is_empty()) {
					Dictionary result = _error("NODE_QUERY_FAILED", "No requested live scene node exists.");
					result["data"] = data;
					return result;
				}
				data["state"] = _scene_state_receipt();
				return _ok(data); }, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "read";
				access["key"] = "scene:";
				accesses.push_back(access);
				return accesses; }, {}, {}, SolersToolUiKind::SCENE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY, PackedStringArray({ "/node_paths/0", "/name_contains", "/class_name" }));
	_add_observe_exposed("resource.inspect", "Inspect one exact res:// Resource through Godot's native ResourceLoader and reflection facts. Properties and method calls are explicit and bounded.", R"({"type":"object","properties":{"path":{"type":"string","pattern":"^res://"},"type_hint":{"type":"string"},"include_dependencies":{"type":"boolean"},"max_dependencies":{"type":"integer","minimum":0,"maximum":2048},"properties":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":128},"method_calls":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string","minLength":1},"arguments":{"type":"array","maxItems":32}},"required":["name"],"additionalProperties":false},"maxItems":32}},"required":["path"],"additionalProperties":false})", SolersToolExposure::MODEL, [this](const SolersToolContext &, const Dictionary &a) {
				if (!resource_service) {
					return _error("RESOURCE_INSPECTION_UNAVAILABLE", "Resource inspection is unavailable.", false);
				}
				Dictionary result = resource_service->inspect_resource(a);
				if ((bool)result.get("ok", false) && file_checkpoint) {
					Dictionary data = result.get("data", Dictionary());
					data["state"] = _resource_state_receipt(file_checkpoint, data.get("path", a.get("path", String())));
					result["data"] = data;
				}
				return result; }, _access_by_arg("read", "project:", "path"), {}, {}, SolersToolUiKind::READ, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY, PackedStringArray({ "/path" }));
	_add_observe_exposed("object.inspect", "Inspect one known live Godot ObjectID through native properties and explicitly requested method calls. Use ObjectID values returned by engine or scene facts.", R"({"type":"object","properties":{"object_id":{"type":"string","pattern":"^-?[0-9]+$"},"properties":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":128},"method_calls":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string","minLength":1},"arguments":{"type":"array","maxItems":32}},"required":["name"],"additionalProperties":false},"maxItems":32}},"required":["object_id"],"additionalProperties":false})", SolersToolExposure::MODEL, [this](const SolersToolContext &, const Dictionary &a) {
				if (!resource_service) {
					return _error("OBJECT_INSPECTION_UNAVAILABLE", "Object inspection is unavailable.", false);
				}
				Dictionary args;
				args["object_id"] = a.get("object_id", Variant());
				Dictionary listed = resource_service->native_list_properties(args);
				if (!(bool)listed.get("ok", false)) {
					return listed;
				}
				Dictionary data = listed.get("data", Dictionary());
				const Array requested = a.get("properties", Array());
				if (!requested.is_empty()) {
					Dictionary values;
					Dictionary errors;
					for (int i = 0; i < requested.size(); i++) {
						args["property"] = requested[i];
						const Dictionary value = resource_service->native_get(args);
						if (!(bool)value.get("ok", false)) {
							errors[requested[i]] = value.get("error", Dictionary());
							continue;
						}
						values[requested[i]] = Dictionary(value.get("data", Dictionary())).get("value", Variant());
					}
					data["values"] = values;
					if (!errors.is_empty()) {
						data["property_errors"] = errors;
					}
				}
				Array method_results;
				for (const Variant &call_value : Array(a.get("method_calls", Array()))) {
					const Dictionary call = call_value;
					const StringName method = StringName(String(call.get("name", String())));
					const Array arguments = call.get("arguments", Array());
					args["method"] = method;
					args["arguments"] = arguments;
					const Dictionary result = resource_service->native_get(args);
					Dictionary item = result.get("data", Dictionary());
					item.erase("object");
					if (!(bool)result.get("ok", false)) {
						item["method"] = method;
						item["arguments"] = arguments;
						item["error"] = result.get("error", Dictionary());
					}
					method_results.push_back(item);
				}
				if (!method_results.is_empty()) {
					data["method_results"] = method_results;
				}
				return _ok(data); }, _access_by_arg("read", "engine-object:", "object_id"), {}, {}, SolersToolUiKind::READ, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY, PackedStringArray({ "/object_id" }));
	_add_observe_exposed("spatial.inspect", "Measure native spatial relations between known scene node paths. Results are geometry facts, not a visibility or correctness verdict.", R"({"type":"object","properties":{"relations":{"type":"array","minItems":1,"maxItems":128,"items":{"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"}},"required":["a","b"],"additionalProperties":false}}},"required":["relations"],"additionalProperties":false})", SolersToolExposure::MODEL, [ref](const SolersToolContext &, const Dictionary &a) { return ref->measure_spatial_relations(a); }, [](const Dictionary &) {
				Array accesses;
			Dictionary access;
			access["mode"] = "read";
			access["key"] = "scene:";
			accesses.push_back(access);
			return accesses; }, {}, {}, SolersToolUiKind::SCENE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.node.create", "Create an instantiable Godot Node through the current scene UndoRedo action.", R"({"type":"object","properties":{"class_name":{"type":"string","minLength":1},"name":{"type":"string"},"parent_path":{"type":"string"},"properties":{"type":"object"}},"required":["class_name"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->create_node(a); }, _access_by_arg("write", "scene:", "parent_path"), SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.instance.instantiate", "Instantiate a PackedScene through the current scene UndoRedo action.", R"({"type":"object","properties":{"source_path":{"type":"string","pattern":"^res://"},"parent_path":{"type":"string"},"name":{"type":"string"},"properties":{"type":"object"}},"required":["source_path"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->instantiate_scene(a); }, [](const Dictionary &a) {
			Array accesses = _access_by_arg("read", "project:", "source_path")(a);
			accesses.append_array(_access_by_arg("write", "scene:", "parent_path")(a));
			return accesses; }, SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.node.update", "Set typed Godot properties through the current scene UndoRedo action.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1},"properties":{"type":"object","minProperties":1}},"required":["node_path","properties"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->update_node(a); }, _access_by_arg("write", "scene:", "node_path"), SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.node.reparent", "Reparent a live Node through the current scene UndoRedo action.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1},"new_parent_path":{"type":"string","minLength":1},"position":{"type":"integer"}},"required":["node_path","new_parent_path"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->reparent_node(a); }, [](const Dictionary &a) {
			Array accesses = _access_by_arg("write", "scene:", "node_path")(a);
			accesses.append_array(_access_by_arg("write", "scene:", "new_parent_path")(a));
			return accesses; }, SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.signal.connect", "Connect a live Godot signal through the current scene UndoRedo action.", R"({"type":"object","properties":{"source_path":{"type":"string","minLength":1},"signal":{"type":"string","minLength":1},"target_path":{"type":"string","minLength":1},"method":{"type":"string","minLength":1},"flags":{"type":"integer"}},"required":["source_path","signal","target_path","method"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->connect_signal(a); }, [](const Dictionary &a) {
			Array accesses = _access_by_arg("write", "scene:", "source_path")(a);
			accesses.append_array(_access_by_arg("write", "scene:", "target_path")(a));
			return accesses; }, SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.script.attach", "Attach a loaded Script resource through the current scene UndoRedo action.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1},"script_path":{"type":"string","pattern":"^res://"}},"required":["node_path","script_path"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->attach_script(a); }, [](const Dictionary &a) {
			Array accesses = _access_by_arg("write", "scene:", "node_path")(a);
			accesses.append_array(_access_by_arg("read", "project:", "script_path")(a));
			return accesses; }, SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.node.remove", "Remove a live Node through the current scene UndoRedo action.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1}},"required":["node_path"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->remove_node(a); }, _access_by_arg("write", "scene:", "node_path"), SNAME("Node"));
	if (resource_service) {
		SolersResourceService *resources = resource_service;
		_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "resource.edit", "Create an absent Resource or update an existing one through ClassDB, PropertyInfo, ResourceLoader, and ResourceSaver. class_name is required only when the path does not exist; properties are required only when it does.", R"({"type":"object","properties":{"path":{"type":"string","pattern":"^res://"},"class_name":{"type":"string","minLength":1},"properties":{"type":"object"},"type_hint":{"type":"string"}},"required":["path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::FILES, [resources](const SolersToolContext &, const Dictionary &a) { return resources->edit_resource(a); }, _access_by_arg("write", "project:", "path"), SNAME("Resource"));
	}
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.open", "Open a res:// scene via EditorInterface and return its native history receipt.", R"({"type":"object","properties":{"path":{"type":"string","pattern":"^res://","description":"Saved editable PackedScene to open; imported scenes remain read-only resources."}},"required":["path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_OBSERVE, SolersToolMutationDomain::IRREVERSIBLE, [ref](const SolersToolContext &, const Dictionary &a) { return ref->open_scene(a); }, _access_by_arg("read", "project:", "path"));
	_add_observe_exposed("engine.describe", "Search ClassDB or inspect exact classes. member_query returns matching typed members and documentation.", R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Fuzzy class search."},"inherits":{"type":"string"},"max_results":{"type":"integer","minimum":1,"maximum":200},"max_members":{"type":"integer","minimum":1,"maximum":256,"description":"Default page size for every exact class request."},"classes":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{"class_name":{"type":"string","minLength":1},"include_inherited":{"type":"boolean"},"member_query":{"type":"string","description":"Filter typed members/docs; omit for names only."},"cursor":{"type":"integer","minimum":0,"description":"Cursor returned by the previous page. Default 0."},"max_members":{"type":"integer","minimum":1,"maximum":256,"description":"Optional page size shared by methods, properties, signals, and constants."}},"required":["class_name"],"additionalProperties":false},"description":"Exact classes to introspect. Lean without member_query; expand with member_query."}},"additionalProperties":false})", SolersToolExposure::MODEL, [this](const SolersToolContext &, const Dictionary &a) { return _inspect_engine(a); }, {}, {}, {}, SolersToolUiKind::SEARCH, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY, PackedStringArray({ "/query", "/classes/0/class_name" }));
}
