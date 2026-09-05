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

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_project_observation.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_runtime_observation.h"
#include "modules/solers_ai/core/solers_scene_observation.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

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
	int tokens = 0;
	int cursor = CLAMP((int)p_args.get("cursor", 0), 0, classes.size());
	for (; cursor < classes.size(); cursor++) {
		Dictionary request = Dictionary(classes[cursor]).duplicate();
		if (!request.has("max_members") && p_args.has("max_members")) {
			request["max_members"] = p_args["max_members"];
		}
		const Dictionary inspected = reflection_service->introspect_class(request);
		if (!(bool)inspected.get("ok", false)) {
			Dictionary error;
			error["request_index"] = cursor;
			error["class_name"] = request.get("class_name", String());
			error["error"] = inspected.get("error", Dictionary());
			errors.push_back(error);
			continue;
		}
		Dictionary class_data = inspected.get("data", Dictionary());
		class_data["request_index"] = cursor;
		if (!SolersContextManager::append_bounded(inspected_classes, class_data, p_args.get("max_results", 32), SolersContextManager::TOOL_RESULT_MAX_TOKENS, tokens)) {
			break;
		}
	}
	Dictionary data;
	data["classes"] = inspected_classes;
	data["errors"] = errors;
	data["requested_count"] = classes.size();
	data["complete"] = errors.is_empty() && cursor == classes.size();
	if (cursor < classes.size()) {
		data["next_cursor"] = cursor;
	}
	return _ok(data);
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

Dictionary SolersToolRegistry::_run_control(const Dictionary &p_args, const String &p_call_id, const SolersToolContext *p_context) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	ERR_FAIL_NULL_V(run_bar, _error("EDITOR_RUN_BAR_UNAVAILABLE", "The editor runtime controller is not available.", false));
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const String action = p_args.get("action", String());
	const bool was_playing = run_bar->is_playing();
	auto require_run_permission = [p_context, &p_args]() {
		return p_context ? p_context->require_permission(SolersPermissionManager::PERMISSION_RUN_PROJECT, p_args) : Dictionary();
	};
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
		const Dictionary denied = require_run_permission();
		if (!denied.is_empty()) {
			return denied;
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
			const Dictionary denied = require_run_permission();
			if (!denied.is_empty()) {
				return denied;
			}
			run_bar->play_current_scene();
		}
		command_accepted = true;
	} else if (action == "stop") {
		if (was_playing) {
			const Dictionary denied = require_run_permission();
			if (!denied.is_empty()) {
				return denied;
			}
			run_bar->stop_playing();
		}
		command_accepted = true;
	} else if (action == "suspend" || action == "resume" || action == "next_frame") {
		GameViewDebugger *game_debugger = _solers_game_view_debugger();
		if (!debugger || !debugger->is_session_active() || !game_debugger) {
			return _error("RUNTIME_NOT_CONNECTED", "The native Game View debugger is not connected.");
		}
		const Dictionary denied = require_run_permission();
		if (!denied.is_empty()) {
			return denied;
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
			const Dictionary denied = require_run_permission();
			if (!denied.is_empty()) {
				return denied;
			}
			debugger->debug_break();
		}
		command_accepted = true;
	} else if (action == "debug_continue" || action == "debug_step" || action == "debug_next" || action == "debug_out") {
		if (!debugger || !debugger->is_debuggable()) {
			return _error("RUNTIME_NOT_BREAKED", "Break at a debuggable stack frame before stepping or continuing.");
		}
		const Dictionary denied = require_run_permission();
		if (!denied.is_empty()) {
			return denied;
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

Dictionary SolersToolRegistry::_poll_runtime_control(const Dictionary &p_args, const SolersToolContext *p_context) const {
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
			if (p_context) {
				const Dictionary denied = p_context->require_permission(SolersPermissionManager::PERMISSION_RUN_PROJECT, p_args);
				if (!denied.is_empty()) {
					return denied;
				}
				if ((int64_t)runtime_observation->get_runtime_status().get("runtime_epoch", 0) != (int64_t)p_args.get("runtime_epoch", 0)) {
					return _error("STALE_RUNTIME_EPOCH", "The runtime changed while the property write was waiting for approval.");
				}
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
	if (project_observation) {
		SolersProjectObservation *project = project_observation;
		_add("search", "Search live project paths or the actual UTF-8-readable files indexed by Godot. Results use a stable cursor.", R"({"type":"object","properties":{"type":{"type":"string","enum":["path","text","symbol"]},"query":{"type":"string"},"cursor":{"type":"integer","minimum":0},"max_results":{"type":"integer","minimum":1,"maximum":1000}},"required":["type","query"],"additionalProperties":false})", [this, project](const SolersToolContext &ctx, const Dictionary &a) {
			if (String(a.get("type", String())) != "path" && String(a.get("query", String())).is_empty()) {
				return _error("INVALID_ARGUMENT", "query is required for text and symbol search.");
			}
			return _ok(project->search_project(a, ctx.result_token_budget));
		});
	}
	if (scene_observation) {
		SolersSceneObservation *scene = scene_observation;
		_add("render.capture", "Capture content-addressed pixels from an explicit editor or runtime state. Receipts bind editor pixels to UndoRedo identity and runtime pixels to runtime_epoch.", R"({"type":"object","properties":{"target":{"type":"string","enum":["editor","camera","focus","top_down","orthographic","runtime"]},"source_state":{"type":"object","properties":{"history_id":{"type":"integer"},"version":{"type":"integer","minimum":0},"root_object_id":{"type":"string"}},"required":["history_id","version"],"additionalProperties":true},"camera_path":{"type":"string"},"axis":{"type":"string","enum":["x","y","z"]},"direction":{"type":"string","enum":["positive","negative"]},"focus_paths":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":32,"uniqueItems":true},"include_render_state":{"type":"boolean"},"section_position":{"type":"number"},"debug_draw":{"type":"integer","minimum":0}},"required":["target"],"additionalProperties":false})", [scene](const SolersToolContext &, const Dictionary &a) { return scene->capture_viewport(a); }, [scene](const SolersToolContext &, const Dictionary &a) { return scene->poll_viewport_capture(a); }, [scene](const SolersToolContext &, const Dictionary &a) { return scene->is_viewport_capture_ready(a); });
	}
}

void SolersToolRegistry::_register_runtime_tools() {
	if (!runtime_observation) {
		return;
	}
	SolersRuntimeObservation *runtime = runtime_observation;
	_add("runtime.observe", "Observe one canonical runtime snapshot through Godot's native debugger. Every result carries the current runtime_epoch.", R"({"type":"object","properties":{"target":{"type":"string","enum":["scene","spatial","stack","performance"]},"node_paths":{"type":"array","items":{"type":"string"},"maxItems":64,"uniqueItems":true},"focus_paths":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":32,"uniqueItems":true},"path_prefix":{"type":"string"},"name_contains":{"type":"string"},"class_name":{"type":"string"},"cursor":{"type":"integer","minimum":0},"properties":{"type":"array","items":{"type":"string","minLength":1},"maxItems":64,"uniqueItems":true},"max_results":{"type":"integer","minimum":1}},"required":["target"],"additionalProperties":false})", [runtime](const SolersToolContext &ctx, const Dictionary &a) { return Dictionary({ { "ok", true }, { "data", runtime->observe_runtime(a, ctx.result_token_budget) } }); }, [runtime](const SolersToolContext &ctx, const Dictionary &a) { return Dictionary({ { "ok", true }, { "data", runtime->observe_runtime(a, ctx.result_token_budget) } }); }, [runtime](const SolersToolContext &, const Dictionary &a) { return runtime->is_runtime_observation_ready(a); });

	_add("runtime.control", "Control the active debugger or make one runtime-only property change guarded by runtime_epoch, ObjectID, and observation_id.", R"({"type":"object","properties":{"action":{"type":"string","enum":["play_current_scene","stop","suspend","resume","next_frame","debug_break","debug_continue","debug_step","debug_next","debug_out","set_input_actions","set_property"]},"runtime_epoch":{"type":"integer","minimum":0},"actions":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"object","properties":{"name":{"type":"string","minLength":1},"strength":{"type":"number","exclusiveMinimum":0,"maximum":1}},"required":["name","strength"],"additionalProperties":false}},"physics_frames":{"type":"integer","minimum":1},"observations":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{"node_path":{"type":"string"},"properties":{"type":"array","minItems":1,"maxItems":32,"uniqueItems":true,"items":{"type":"string","minLength":1}}},"required":["node_path","properties"],"additionalProperties":false}},"node_path":{"type":"string"},"object_id":{"type":"string"},"property":{"type":"string","minLength":1},"observation_id":{"type":"string","minLength":64,"maxLength":64},"value":{}},"required":["action"],"additionalProperties":false})", [this](const SolersToolContext &ctx, const Dictionary &a) { return _run_control(a, ctx.call_id, &ctx); }, [this](const SolersToolContext &ctx, const Dictionary &a) { return _poll_runtime_control(a, &ctx); }, [this](const SolersToolContext &, const Dictionary &a) { return _is_runtime_control_ready(a); });

	if (!script_service) {
		return;
	}
	_add("runtime.script", "Run bounded GDScript inside the real game process for the exact supplied runtime_epoch.", R"({"type":"object","properties":{"runtime_epoch":{"type":"integer","minimum":1},"source":{"type":"string","minLength":1,"writeOnly":true},"timeout_msec":{"type":"integer","minimum":1000,"maximum":600000}},"required":["runtime_epoch","source"],"additionalProperties":false})", [this, runtime](const SolersToolContext &ctx, const Dictionary &a) {
		const Dictionary validation = script_service->validate_script(Dictionary({ { "path", "res://solers_runtime_script.gd" }, { "source", a.get("source", String()) } }));
		if (!(bool)validation.get("ok", false) || !(bool)Dictionary(validation.get("data", Dictionary())).get("valid", false)) {
			Dictionary failure = _error("SCRIPT_VALIDATION_FAILED", "runtime.script did not pass Godot's registered GDScript parser.");
			failure["data"] = validation.get("data", Dictionary());
			return failure;
		}
		return runtime->start_runtime_script(a, ctx.call_id, &ctx); }, [runtime](const SolersToolContext &, const Dictionary &a) { return runtime->poll_runtime_script(a); }, [runtime](const SolersToolContext &, const Dictionary &a) { return runtime->is_runtime_script_ready(a); }, [runtime](const SolersToolContext &ctx, const Dictionary &, const Dictionary &) { runtime->clear_runtime_script(ctx.call_id); });
}

void SolersToolRegistry::_register_reflection_tools() {
	if (!reflection_service) {
		return;
	}
	SolersReflectionService *reflection = reflection_service;

	_add("engine.describe", "Search ClassDB or inspect exact classes and typed members from the running editor.", R"({"type":"object","properties":{"query":{"type":"string","minLength":1},"inherits":{"type":"string"},"cursor":{"type":"integer","minimum":0},"max_results":{"type":"integer","minimum":1,"maximum":200},"max_members":{"type":"integer","minimum":1,"maximum":256},"classes":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{"class_name":{"type":"string","minLength":1},"include_inherited":{"type":"boolean"},"member_query":{"type":"string"},"cursor":{"type":"integer","minimum":0},"max_members":{"type":"integer","minimum":1,"maximum":256}},"required":["class_name"],"additionalProperties":false}}},"additionalProperties":false})", [this](const SolersToolContext &, const Dictionary &a) {
		return _inspect_engine(a);
	});

	if (scene_observation) {
		SolersSceneObservation *scene = scene_observation;
		_add("scene.inspect", "Inspect live edited-scene nodes, typed properties, connections, spatial relations, and the native UndoRedo/ObjectID receipt.", R"({"type":"object","properties":{"include_selection":{"type":"boolean"},"node_paths":{"type":"array","items":{"type":"string"},"uniqueItems":true,"minItems":1,"maxItems":64},"path_prefix":{"type":"string"},"name_contains":{"type":"string"},"class_name":{"type":"string"},"script_path":{"type":"string"},"cursor":{"type":"integer","minimum":0},"max_results":{"type":"integer","minimum":1},"include_connections":{"type":"boolean"},"properties":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":128},"relations":{"type":"array","minItems":1,"maxItems":128,"items":{"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"}},"required":["a","b"],"additionalProperties":false}}},"additionalProperties":false})", [this, reflection, scene](const SolersToolContext &ctx, const Dictionary &a) {
			Node *edited_root = SceneTree::get_singleton()->get_edited_scene_root();
			if (!edited_root) {
				return _ok(Dictionary({ { "nodes", Array() }, { "count", 0 }, { "state", reflection->get_scene_state() } }));
			}
			Dictionary query_args = a.duplicate(true);
			query_args.erase("relations");
			const Dictionary queried = scene->query_scene_nodes(query_args, ctx.result_token_budget);
			const Array queried_nodes = queried.get("nodes", Array());
			Array paths;
			for (const Variant &value : queried_nodes) {
				paths.push_back(Dictionary(value).get("node_path", String()));
			}
			Dictionary data;
			if (!paths.is_empty()) {
				Dictionary inspect_args = query_args;
				inspect_args["node_paths"] = paths;
				const Dictionary inspected = reflection->inspect_nodes(inspect_args);
				if (!(bool)inspected.get("ok", false)) {
					return inspected;
				}
				data = inspected.get("data", Dictionary());
			} else {
				data["nodes"] = queried_nodes;
				data["count"] = 0;
			}
			if ((bool)a.get("include_selection", false)) {
				data["selection"] = scene->get_selection();
			}
			if (a.has("relations")) {
				const Dictionary measured = reflection->measure_spatial_relations(Dictionary({ { "relations", a["relations"] } }));
				if (!(bool)measured.get("ok", false)) {
					return measured;
				}
				data["relations"] = Dictionary(measured.get("data", Dictionary())).get("relations", Array());
			}
			data["query_errors"] = queried.get("errors", Array());
			data["cursor"] = queried.get("cursor", 0);
			if (queried.has("next_cursor")) {
				data["next_cursor"] = queried["next_cursor"];
			}
			data["state"] = reflection->get_scene_state();
			return _ok(data);
		});
	}

	if (resource_service) {
		SolersResourceService *resources = resource_service;
		_add("object.inspect", "Inspect either one exact project Resource path or one live editor-process ObjectID through native reflection.", R"({"type":"object","properties":{"path":{"type":"string"},"object_id":{"type":"string"},"type_hint":{"type":"string"},"include_dependencies":{"type":"boolean"},"max_dependencies":{"type":"integer","minimum":0,"maximum":2048},"properties":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":128},"method_calls":{"type":"array","maxItems":32,"items":{"type":"object","properties":{"name":{"type":"string","minLength":1},"arguments":{"type":"array","maxItems":32}},"required":["name"],"additionalProperties":false}}},"additionalProperties":false})", [this, resources](const SolersToolContext &, const Dictionary &a) {
			const bool has_path = !String(a.get("path", String())).is_empty();
			const bool has_object = !String(a.get("object_id", String())).is_empty();
			if (has_path == has_object) {
				return _error("INVALID_ARGUMENT", "Supply exactly one of path or object_id.");
			}
			if (has_path) {
				return resources->inspect_resource(a);
			}
			Dictionary native_args;
			native_args["object_id"] = a["object_id"];
			Dictionary listed = resources->native_list_properties(native_args);
			if (!(bool)listed.get("ok", false)) {
				return listed;
			}
			Dictionary data = listed.get("data", Dictionary());
			Dictionary values;
			Dictionary errors;
			for (const Variant &property : Array(a.get("properties", Array()))) {
				native_args["property"] = property;
				const Dictionary value = resources->native_get(native_args);
				if ((bool)value.get("ok", false)) {
					values[property] = Dictionary(value.get("data", Dictionary())).get("value", Variant());
				} else {
					errors[property] = value.get("error", Dictionary());
				}
			}
			if (!values.is_empty()) {
				data["values"] = values;
			}
			if (!errors.is_empty()) {
				data["property_errors"] = errors;
			}
			Array method_results;
			for (const Variant &value : Array(a.get("method_calls", Array()))) {
				const Dictionary call = value;
				native_args["method"] = call.get("name", String());
				native_args["arguments"] = call.get("arguments", Array());
				const Dictionary result = resources->native_get(native_args);
				Dictionary item = result.get("data", Dictionary());
				item.erase("object");
				if (!(bool)result.get("ok", false)) {
					item["method"] = native_args["method"];
					item["error"] = result.get("error", Dictionary());
				}
				method_results.push_back(item);
			}
			if (!method_results.is_empty()) {
				data["method_results"] = method_results;
			}
			return _ok(data);
		});

		_add("resource.edit", "Create or edit a Resource through ClassDB, PropertyInfo, ResourceLoader, and ResourceSaver, guarded by the supplied UID/SHA receipt.", R"({"type":"object","properties":{"path":{"type":"string","minLength":6},"class_name":{"type":"string","minLength":1},"properties":{"type":"object","writeOnly":true},"type_hint":{"type":"string"},"expected_state":{"type":"object","properties":{"exists":{"type":"boolean"},"sha256":{"type":"string"},"uid":{"type":"string"}},"required":["exists"],"additionalProperties":false}},"required":["path","expected_state"],"additionalProperties":false})", [resources](const SolersToolContext &ctx, const Dictionary &a) {
			return resources->edit_resource(a, &ctx);
		});
	}

	_add("scene.open", "Open one editable PackedScene through EditorInterface and return its live ObjectID/UndoRedo receipt.", R"({"type":"object","properties":{"path":{"type":"string","minLength":6}},"required":["path"],"additionalProperties":false})", [reflection](const SolersToolContext &ctx, const Dictionary &a) {
		return reflection->open_scene_with_context(a, &ctx);
	});
	_add("scene.edit", "Apply an ordered batch of structural live-scene operations as one native UndoRedo transaction using the current scene receipt and NodePaths from scene.inspect. Optional save_path persists the edited root.", R"({"type":"object","properties":{"expected_state":{"type":"object","properties":{"has_root":{"type":"boolean"},"root_object_id":{"type":"string"},"scene_path":{"type":"string"},"history_id":{"type":"integer"},"version":{"type":"integer","minimum":0}},"required":["has_root","history_id","version"],"additionalProperties":false},"save_path":{"type":"string"},"operations":{"type":"array","maxItems":128,"items":{"type":"object","properties":{"type":{"type":"string","enum":["create","instantiate","update","reparent","remove","connect_signal","attach_script"]},"node_path":{"type":"string"},"parent_path":{"type":"string"},"new_parent_path":{"type":"string"},"class_name":{"type":"string"},"name":{"type":"string"},"source_path":{"type":"string"},"target_path":{"type":"string"},"signal":{"type":"string"},"method":{"type":"string"},"script_path":{"type":"string"},"properties":{"type":"object","writeOnly":true},"position":{"type":"integer"},"flags":{"type":"integer"}},"required":["type"],"additionalProperties":false}}},"required":["expected_state"],"additionalProperties":false})", [reflection](const SolersToolContext &ctx, const Dictionary &a) {
		return reflection->edit_scene(a, &ctx);
	});
}
