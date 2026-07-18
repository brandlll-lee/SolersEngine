/**************************************************************************/
/*  solers_agent_session.cpp                                              */
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

#include "solers_agent_session.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/message_queue.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/editor_interface.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_retry.h"
#include "modules/solers_ai/llm/solers_models_dev.h"
#include "scene/main/node.h"

struct SolersAgentSession::ToolThreadState {
	SolersToolRegistry *registry = nullptr;
	SolersAgentSession *session = nullptr;
	SolersPreparedToolCall call;
	Dictionary poll_args;
	bool polling = false;
	Dictionary result;
	SafeFlag done;
	uint64_t worker_thread_id = Thread::UNASSIGNED_ID;
	uint64_t token = 0;
};

struct SolersAgentSession::PendingToolExecution {
	int queue_index = -1;
	String call_id;
	String model_name;
	String canonical_name;
	Dictionary poll_args;
	Dictionary initial_args;
	Array resource_accesses;
	uint64_t scene_digest_before = 0;
	uint64_t geometry_digest_before = 0;
	SolersPreparedToolCall *prepared_call = nullptr;
	bool is_resume = false;
	uint64_t started_msec = 0;
};

static bool _is_session_tool(const String &p_name) {
	return p_name == "update_plan";
}

static Dictionary _update_plan_schema() {
	Dictionary status;
	status["type"] = "string";
	Array values;
	values.push_back("pending");
	values.push_back("in_progress");
	values.push_back("completed");
	status["enum"] = values;

	Dictionary step_properties;
	Dictionary step_text;
	step_text["type"] = "string";
	step_properties["step"] = step_text;
	step_properties["status"] = status;
	Dictionary step;
	step["type"] = "object";
	step["properties"] = step_properties;
	Array step_required;
	step_required.push_back("step");
	step_required.push_back("status");
	step["required"] = step_required;
	step["additionalProperties"] = false;

	Dictionary properties;
	Dictionary explanation;
	explanation["type"] = "string";
	properties["explanation"] = explanation;
	Dictionary plan;
	plan["type"] = "array";
	plan["items"] = step;
	properties["plan"] = plan;

	Dictionary schema;
	schema["type"] = "object";
	schema["properties"] = properties;
	Array required;
	required.push_back("plan");
	schema["required"] = required;
	schema["additionalProperties"] = false;
	return schema;
}

void SolersAgentSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_turn", "args"), &SolersAgentSession::start_turn);
	ClassDB::bind_method(D_METHOD("poll"), &SolersAgentSession::poll);
	ClassDB::bind_method(D_METHOD("abort"), &SolersAgentSession::abort);
	ClassDB::bind_method(D_METHOD("reset_conversation"), &SolersAgentSession::reset_conversation);
	ClassDB::bind_method(D_METHOD("get_status"), &SolersAgentSession::get_status);

	ADD_SIGNAL(MethodInfo("model_request_started"));
	ADD_SIGNAL(MethodInfo("assistant_delta", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("reasoning_delta", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("assistant_message", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("tool_call_started", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::STRING, "arguments")));
	ADD_SIGNAL(MethodInfo("tool_call_updated", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::STRING, "arguments")));
	ADD_SIGNAL(MethodInfo("tool_call_awaiting_approval", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name")));
	ADD_SIGNAL(MethodInfo("tool_call_finished", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::DICTIONARY, "result"), PropertyInfo(Variant::INT, "duration_msec")));
	ADD_SIGNAL(MethodInfo("turn_completed", PropertyInfo(Variant::DICTIONARY, "result")));
	ADD_SIGNAL(MethodInfo("turn_failed", PropertyInfo(Variant::DICTIONARY, "error")));
	ADD_SIGNAL(MethodInfo("turn_retrying", PropertyInfo(Variant::INT, "attempt"), PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("plan_updated", PropertyInfo(Variant::STRING, "explanation"), PropertyInfo(Variant::ARRAY, "plan")));
	ADD_SIGNAL(MethodInfo("compaction_started"));
	ADD_SIGNAL(MethodInfo("compaction_completed", PropertyInfo(Variant::DICTIONARY, "result")));
}

Dictionary SolersAgentSession::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersAgentSession::_error(const String &p_code, const String &p_message) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

void SolersAgentSession::_record(const String &p_event, const Dictionary &p_payload) const {
	if (action_timeline) {
		action_timeline->record_event(p_event, p_payload);
	}
}

Dictionary SolersAgentSession::validate_plan(const Dictionary &p_args) {
	Dictionary result;
	Dictionary error;
	for (const Variant *key = p_args.next(nullptr); key; key = p_args.next(key)) {
		const String name = String(*key);
		if (name != "explanation" && name != "plan") {
			error["code"] = "INVALID_PLAN";
			error["message"] = vformat("Unknown update_plan field: %s.", name);
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
	}
	if (!p_args.has("plan") || p_args["plan"].get_type() != Variant::ARRAY) {
		error["code"] = "INVALID_PLAN";
		error["message"] = "update_plan requires a plan array.";
		result["ok"] = false;
		result["error"] = error;
		return result;
	}

	const Array plan = p_args["plan"];
	int in_progress_count = 0;
	for (int i = 0; i < plan.size(); i++) {
		if (plan[i].get_type() != Variant::DICTIONARY) {
			error["code"] = "INVALID_PLAN";
			error["message"] = "Each plan item must be an object.";
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		const Dictionary item = plan[i];
		for (const Variant *key = item.next(nullptr); key; key = item.next(key)) {
			const String name = String(*key);
			if (name != "step" && name != "status") {
				error["code"] = "INVALID_PLAN";
				error["message"] = vformat("Unknown plan item field: %s.", name);
				result["ok"] = false;
				result["error"] = error;
				return result;
			}
		}
		const String step = String(item.get("step", String())).strip_edges();
		const String status = item.get("status", String());
		if (step.is_empty() || (status != "pending" && status != "in_progress" && status != "completed")) {
			error["code"] = "INVALID_PLAN";
			error["message"] = "Each plan item needs a non-empty step and status pending, in_progress, or completed.";
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		if (status == "in_progress") {
			in_progress_count++;
		}
	}
	if (in_progress_count > 1) {
		error["code"] = "INVALID_PLAN";
		error["message"] = "At most one plan item may be in_progress.";
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	result["ok"] = true;
	result["data"] = p_args.duplicate(true);
	return result;
}

void SolersAgentSession::set_tool_registry(SolersToolRegistry *p_tool_registry) {
	tool_registry = p_tool_registry;
	_register_session_tools();
}

void SolersAgentSession::_register_session_tools() {
	if (!tool_registry || session_tools_registry == tool_registry) {
		return;
	}

	SolersToolCapability capability;
	capability.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	capability.mutation_kind = "none";
	tool_registry->register_tool(memnew(SolersFunctionTool(
			"update_plan",
			"Optionally replace the concise UI progress plan. This does not control tool access or task completion.",
			_update_plan_schema(),
			SolersToolExposure::DIRECT,
			capability,
			[this](const SolersToolContext &, const Dictionary &p_args) { return _handle_update_plan(p_args); })));
	session_tools_registry = tool_registry;
}

Dictionary SolersAgentSession::_handle_update_plan(const Dictionary &p_args) {
	const Dictionary validation = validate_plan(p_args);
	if (!(bool)validation.get("ok", false)) {
		return validation;
	}
	current_plan = p_args.duplicate(true);
	_write_transcript_plan();
	emit_signal(SNAME("plan_updated"), current_plan.get("explanation", String()), current_plan.get("plan", Array()));
	Dictionary data;
	data["message"] = "Plan updated";
	return _ok(data);
}

String SolersAgentSession::_make_session_id() const {
	return OS::get_singleton()->get_unique_id() + "-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
}

Dictionary SolersAgentSession::_read_transcript_state(const String &p_project_path, const String &p_session_id) const {
	Array restored;
	Array restored_background_assets;
	Dictionary restored_plan;
	String restored_outcome;
	int restored_turn_id = 0;
	if (p_project_path.is_empty() || p_session_id.is_empty()) {
		Dictionary empty;
		empty["messages"] = restored;
		return empty;
	}

	const Vector<String> transcript_lines = solers_transcript_read_snapshot();
	if (transcript_lines.is_empty()) {
		Dictionary empty;
		empty["messages"] = restored;
		return empty;
	}

	for (const String &record : transcript_lines) {
		const String line = record.strip_edges();
		if (line.is_empty()) {
			continue;
		}
		Dictionary event;
		if (!solers_transcript_parse_record(line, event)) {
			continue;
		}
		if (String(event.get("project_path", String())) != p_project_path || String(event.get("session_id", String())) != p_session_id) {
			continue;
		}
		restored_turn_id = MAX(restored_turn_id, (int)event.get("turn_id", 0));

		const String event_type = event.get("event_type", String());
		if (event_type == "tool_result") {
			continue;
		}
		if (event_type == "plan_updated") {
			restored_plan["explanation"] = event.get("explanation", String());
			restored_plan["plan"] = event.get("plan", Array());
			continue;
		}
		if (event_type == "context.apply_compaction") {
			const Array compacted = event.get("messages", Array());
			if (!compacted.is_empty()) {
				restored = compacted.duplicate(true);
			}
			const Dictionary plan = event.get("current_plan", Dictionary());
			if (!plan.is_empty()) {
				restored_plan = plan.duplicate(true);
			}
			continue;
		}
		if (event_type == "turn_outcome") {
			restored_outcome = event.get("outcome", String());
			continue;
		}
		if (event_type == "background_asset_delivery") {
			restored_background_assets.push_back(event);
			continue;
		}
		if (event_type == "background_asset_consumed") {
			const String asset_id = event.get("asset_id", String());
			for (int i = restored_background_assets.size() - 1; i >= 0; i--) {
				if (String(Dictionary(restored_background_assets[i]).get("asset_id", String())) == asset_id) {
					restored_background_assets.remove_at(i);
				}
			}
			continue;
		}

		const String role = event.get("role", String());
		const String content = event.get("content", String());
		if (content.is_empty()) {
			continue;
		}
		if (role == SolersLLMRole::USER) {
			restored.push_back(SolersLLMMessage::user(content));
		} else if (role == SolersLLMRole::ASSISTANT) {
			restored.push_back(SolersLLMMessage::assistant(content, Array()));
		}
	}

	Dictionary state;
	state["messages"] = restored;
	state["plan"] = restored_plan;
	state["outcome"] = restored_outcome;
	state["turn_id"] = restored_turn_id;
	state["background_assets"] = restored_background_assets;
	return state;
}

void SolersAgentSession::_stamp_transcript_event(Dictionary &r_event) const {
	r_event["project_path"] = project_path;
	r_event["session_id"] = session_id;
	r_event["authored_revision"] = (int64_t)authored_revision;
	r_event["runtime_epoch"] = (int64_t)runtime_epoch;
	r_event["observed_revision"] = (int64_t)observed_revision;
}

void SolersAgentSession::_write_transcript_event(const String &p_type, const Dictionary &p_payload) const {
	Dictionary event = p_payload.duplicate(true);
	event["event_type"] = p_type;
	event["turn_id"] = turn_id;
	event["wall"] = Time::get_singleton()->get_unix_time_from_system();
	_stamp_transcript_event(event);
	solers_transcript_write(event);
}

void SolersAgentSession::_write_transcript_message(const String &p_role, const String &p_content) const {
	Dictionary event;
	event["event_type"] = "message";
	event["turn_id"] = turn_id;
	event["role"] = p_role;
	event["content"] = p_content;
	event["wall"] = Time::get_singleton()->get_unix_time_from_system();
	_stamp_transcript_event(event);
	solers_transcript_write(event);
}

void SolersAgentSession::_write_transcript_plan() const {
	Dictionary event = current_plan.duplicate(true);
	_write_transcript_event("plan_updated", event);
}

void SolersAgentSession::_write_transcript_compaction(const Dictionary &p_result) const {
	Dictionary event = p_result.duplicate(true);
	event["current_plan"] = current_plan.duplicate(true);
	_write_transcript_event("context.apply_compaction", event);
}

void SolersAgentSession::_write_transcript_tool(const String &p_call_id, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result) const {
	Dictionary event;
	event["role"] = "tool";
	event["call_id"] = p_call_id;
	event["tool"] = p_canonical_name;
	event["ok"] = p_result.get("ok", false);
	event["queue_msec"] = tool_started_msec >= tool_queued_msec ? (int64_t)(tool_started_msec - tool_queued_msec) : 0;
	event["run_msec"] = tool_completed_msec >= tool_started_msec ? (int64_t)(tool_completed_msec - tool_started_msec) : 0;
	event["delivery_msec"] = tool_completed_msec ? (int64_t)(OS::get_singleton()->get_ticks_msec() - tool_completed_msec) : 0;
	event["duration_msec"] = event["run_msec"];
	if (tool_registry) {
		event["args"] = tool_registry->protect_tool_args_for_replay(StringName(p_canonical_name), p_args);
		event["result_summary"] = tool_registry->summarize_tool_result_for_audit(p_result);
		event["resource_accesses"] = tool_registry->resolve_resource_access(StringName(p_canonical_name), p_args);
	} else {
		event["args"] = p_args;
	}
	if (!(bool)p_result.get("ok", false)) {
		event["error"] = p_result.get("error", Dictionary());
	}
	const Dictionary result_data = p_result.get("data", Dictionary());
	if (result_data.has("artifact")) {
		event["artifact"] = result_data["artifact"];
	}
	const String replay_result = SolersSecretStore::protect(JSON::stringify(p_result, "", false, true));
	if (SolersSecretStore::is_protected(replay_result)) {
		event["result_replay"] = replay_result;
	}
	_write_transcript_event("tool_result", event);
}

void SolersAgentSession::_ensure_godot_log_audit(bool p_turn_active) {
	EditorLog *log = EditorNode::get_singleton() ? EditorNode::get_log() : nullptr;
	if (p_turn_active) {
		MutexLock lock(godot_log_mutex);
		godot_log_error_count = 0;
		godot_log_warning_count = 0;
		worker_tool_audits.clear();
		main_thread_tool_audit.clear();
		attributable_tool_errors.clear();
		authored_revision = 0;
		runtime_epoch = 0;
		scene_revision = 0;
		geometry_revision = 0;
		observed_revision = 0;
		editor_capture_revision = 0;
		camera_capture_revision = 0;
		runtime_capture_revision = 0;
		scene_validation_revision = 0;
		pending_godot_diagnostics.clear();
	}
	godot_log_turn_active = p_turn_active;
	Dictionary baseline;
	baseline["available"] = log != nullptr;
	baseline["turn_active"] = p_turn_active;
	if (log) {
		const Callable callback = callable_mp(this, &SolersAgentSession::_on_godot_log_message);
		if (!godot_log_audit_installed) {
			log->set_message_audit_callback(callback);
			godot_log_audit_installed = true;
			godot_log_object_id = log->get_instance_id();
		}
		baseline["counts"] = log->get_message_counts();
	}
	_write_transcript_event("godot_log_baseline", baseline);
}

void SolersAgentSession::_release_godot_log_audit() {
	if (!godot_log_audit_installed) {
		return;
	}
	EditorLog *log = Object::cast_to<EditorLog>(ObjectDB::get_instance(godot_log_object_id));
	if (log) {
		log->clear_message_audit_callback(callable_mp(this, &SolersAgentSession::_on_godot_log_message));
	}
	godot_log_audit_installed = false;
	godot_log_object_id = ObjectID();
	godot_log_turn_active = false;
	{
		MutexLock lock(godot_log_mutex);
		main_thread_tool_audit.clear();
		worker_tool_audits.clear();
		attributable_tool_errors.clear();
	}
}

void SolersAgentSession::_on_godot_log_message(const String &p_message, int p_type, int64_t p_source_thread) {
	if (p_type != EditorLog::MSG_TYPE_ERROR && p_type != EditorLog::MSG_TYPE_WARNING) {
		return;
	}
	const bool is_error = p_type == EditorLog::MSG_TYPE_ERROR;
	Dictionary event;
	event["severity"] = is_error ? "error" : "warning";
	event["source"] = "godot_editor";
	event["message"] = p_message;
	event["turn_active"] = godot_log_turn_active;
	event["ticks_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	event["thread_id"] = p_source_thread;
	if (godot_log_turn_active) {
		MutexLock lock(godot_log_mutex);
		const Dictionary *worker_audit = worker_tool_audits.getptr((uint64_t)p_source_thread);
		const Dictionary active_audit = worker_audit ? *worker_audit : main_thread_tool_audit;
		if (!active_audit.is_empty()) {
			event["call_id"] = active_audit.get("call_id", String());
			event["tool"] = active_audit.get("tool", String());
		}
		if (is_error) {
			godot_log_error_count++;
			if (event.has("tool")) {
				const String call_id = event.get("call_id", String());
				if (!call_id.is_empty()) {
					Dictionary scoped = attributable_tool_errors.get(call_id, Dictionary());
					Array events = scoped.get("events", Array());
					events.push_back(event);
					scoped["call_id"] = call_id;
					scoped["tool"] = event.get("tool", String());
					scoped["resource_accesses"] = active_audit.get("resource_accesses", Array());
					scoped["events"] = events;
					attributable_tool_errors[call_id] = scoped;
				}
			}
		} else {
			godot_log_warning_count++;
		}
		pending_godot_diagnostics.push_back(event);
	}
	_write_transcript_event("godot_log", event);
}

void SolersAgentSession::_begin_main_thread_tool_audit() {
	Dictionary audit;
	audit["call_id"] = deferred_call_id;
	audit["tool"] = deferred_canonical_name;
	audit["resource_accesses"] = deferred_resource_accesses;
	MutexLock lock(godot_log_mutex);
	main_thread_tool_audit = audit;
}

void SolersAgentSession::_end_main_thread_tool_audit() {
	MutexLock lock(godot_log_mutex);
	main_thread_tool_audit.clear();
}

void SolersAgentSession::_register_worker_tool_audit(uint64_t p_thread_id, const String &p_call_id, const String &p_tool, const Array &p_resource_accesses) {
	if (p_thread_id == Thread::UNASSIGNED_ID) {
		return;
	}
	Dictionary audit;
	audit["call_id"] = p_call_id;
	audit["tool"] = p_tool;
	audit["resource_accesses"] = p_resource_accesses;
	MutexLock lock(godot_log_mutex);
	worker_tool_audits[p_thread_id] = audit;
}

Dictionary SolersAgentSession::_consume_attributable_tool_error(const String &p_call_id) {
	Dictionary scoped;
	{
		MutexLock lock(godot_log_mutex);
		scoped = attributable_tool_errors.get(p_call_id, Dictionary());
		attributable_tool_errors.erase(p_call_id);
	}
	const Array events = scoped.get("events", Array());
	if (events.is_empty()) {
		return Dictionary();
	}
	const String first_message = Dictionary(events[0]).get("message", "Godot reported an error while the native tool handler was executing.");
	Dictionary error;
	error["code"] = "GODOT_TOOL_ERROR";
	error["message"] = events.size() == 1 ? first_message : vformat("Godot reported %d errors while the native tool handler was executing. First error: %s", events.size(), first_message);
	error["recoverable"] = true;
	error["source"] = "godot_editor";
	error["events"] = events;
	return error;
}

Dictionary SolersAgentSession::_take_godot_diagnostics() {
	Dictionary diagnostics;
	{
		MutexLock lock(godot_log_mutex);
		if (pending_godot_diagnostics.is_empty()) {
			return diagnostics;
		}
		Array grouped;
		int errors = 0;
		int warnings = 0;
		for (int i = 0; i < pending_godot_diagnostics.size(); i++) {
			const Dictionary event = pending_godot_diagnostics[i];
			const String severity = event.get("severity", String());
			if (severity == "error") {
				errors++;
			} else {
				warnings++;
			}
			int match = -1;
			for (int j = 0; j < grouped.size(); j++) {
				const Dictionary candidate = grouped[j];
				if (candidate.get("severity", String()) == severity && candidate.get("message", String()) == event.get("message", String()) &&
						candidate.get("call_id", String()) == event.get("call_id", String()) && candidate.get("tool", String()) == event.get("tool", String())) {
					match = j;
					break;
				}
			}
			if (match >= 0) {
				Dictionary candidate = grouped[match];
				candidate["count"] = (int)candidate.get("count", 1) + 1;
				candidate["last_ticks_msec"] = event.get("ticks_msec", 0);
				grouped[match] = candidate;
			} else {
				Dictionary candidate;
				candidate["severity"] = severity;
				candidate["message"] = event.get("message", String());
				candidate["count"] = 1;
				candidate["first_ticks_msec"] = event.get("ticks_msec", 0);
				candidate["last_ticks_msec"] = event.get("ticks_msec", 0);
				if (event.has("call_id")) {
					candidate["call_id"] = event["call_id"];
					candidate["tool"] = event.get("tool", String());
				}
				grouped.push_back(candidate);
			}
		}
		diagnostics["errors"] = errors;
		diagnostics["warnings"] = warnings;
		diagnostics["events"] = grouped;
		pending_godot_diagnostics.clear();
	}
	_write_transcript_event("godot_diagnostics_delivered", diagnostics);
	return diagnostics;
}

void SolersAgentSession::_flush_godot_diagnostics() {
	const Dictionary diagnostics = _take_godot_diagnostics();
	if (diagnostics.is_empty()) {
		return;
	}
	Dictionary message = SolersLLMMessage::user("Godot emitted these diagnostics since the last model boundary. Treat errors as current evidence and re-verify after fixing them:\n" + JSON::stringify(diagnostics, "", false, true));
	message["origin"] = "godot_diagnostics";
	messages.push_back(message);
}

String SolersAgentSession::_readonly_cache_key(const StringName &p_name, const Dictionary &p_args) const {
	const Dictionary normalized = tool_registry ? tool_registry->normalize_tool_args(p_name, p_args) : p_args;
	const uint64_t revision = tool_registry && tool_registry->caches_across_revisions(p_name) ? 0 : authored_revision;
	return String(p_name) + ":" + String::num_uint64(revision) + ":" + String::num_uint64(normalized.hash());
}

bool SolersAgentSession::_poll_state_observation() {
	if (authored_revision <= observed_revision) {
		return false;
	}
	// Revisions are internal cache and scheduling state. A write must not inject
	// another synthetic user turn; the model can request a snapshot explicitly.
	observed_revision = authored_revision;
	return false;
}

Dictionary SolersAgentSession::_commit_dirty_scene_if_needed() {
	Dictionary data;

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	Node *root = editor_interface ? editor_interface->get_edited_scene_root() : nullptr;
	const String path = root ? root->get_scene_file_path() : String();

	bool dirty_before = false;
	int history_id = -1;
	if (EditorUndoRedoManager::get_singleton() && EditorNode::get_singleton()) {
		history_id = EditorNode::get_editor_data().get_current_edited_scene_history_id();
		dirty_before = EditorUndoRedoManager::get_singleton()->is_history_unsaved(history_id);
	}
	if (!dirty_before) {
		return data;
	}

	Error err = ERR_UNCONFIGURED;
	if (editor_interface && root && !path.is_empty()) {
		err = editor_interface->save_scene();
	}
	const bool dirty_after = history_id >= 0 && EditorUndoRedoManager::get_singleton() ?
			EditorUndoRedoManager::get_singleton()->is_history_unsaved(history_id) :
			dirty_before;

	data["ok"] = err == OK;
	data["path"] = path;
	data["dirty_before"] = dirty_before;
	data["dirty_after"] = dirty_after;
	data["error"] = err;
	SOLERS_TRACE("harness.scene_commit", vformat("ok=%d path=%s dirty_before=%d dirty_after=%d err=%d", (int)(err == OK), path, (int)dirty_before, (int)dirty_after, (int)err));
	return data;
}

String SolersAgentSession::_default_system_prompt() const {
	String prompt =
			"You are Solers, an AI agent living natively inside the Solers game engine editor (a Godot 4 fork).\n\n"
			"Operating contract:\n"
			"- Prefer the smallest coherent native change. Inspect live state before editing; do not guess APIs or scene contents.\n"
			"- Built-in tools are already available. If tool.search is present, it discovers only external plugin, connector, or MCP tools. Skills provide knowledge and never activate capabilities.\n"
			"- Keep scene edits undoable and authored in scene/resources. Write or patch code only when native composition cannot express the requested behavior.\n"
			"- Background tools return stable job ids immediately. Continue independent work; when nothing else is runnable, call job.wait once with the required ids. Solers resumes this same task when any requested job reaches a terminal state.\n"
			"- Tool argument errors are stable and occur before execution. Correct the named argument or change resource state before retrying; unchanged failures are deduplicated automatically. Godot diagnostics are evidence, not a replacement for the tool's result.\n"
			"- For native modeling, use model.create, model.inspect, one or a few typed atomic model.batch transactions, model.validate, and model.save. Pass expected_revision and bind later operations to named prior results instead of guessing topology ids.\n"
			"- Use update_plan only as a concise optional progress display. A final assistant message ends the task directly.";
	if (tool_registry) {
		const String skill_catalog = tool_registry->get_skill_catalog_prompt();
		if (!skill_catalog.is_empty()) {
			prompt += "\n\n" + skill_catalog;
		}
	}
	return prompt;
}

Array SolersAgentSession::_collect_tools() const {
	Array out;
	if (!tool_registry) {
		return out;
	}
	const Array defs = tool_registry->list_tools();
	for (int i = 0; i < defs.size(); i++) {
		const Dictionary def = defs[i];
		const String exposure = def.get("exposure", "direct");
		const StringName canonical_name = StringName(def.get("name", String()));
		if (exposure == "hidden" || (exposure == "deferred" && !task_deferred_tools.has(canonical_name))) {
			continue;
		}
		Dictionary tool;
		tool["name"] = def.get("model_name", def.get("name", String()));
		tool["canonical_name"] = def.get("name", String());
		tool["description"] = def.get("description", String());
		Dictionary schema = def.get("input_schema", Dictionary());
		if (schema.is_empty()) {
			schema["type"] = "object";
			schema["properties"] = Dictionary();
		}
		tool["parameters"] = schema;
		out.push_back(tool);
	}
	return out;
}

bool SolersAgentSession::_refresh_active_model_limits() {
	if (!settings_service || !models_dev) {
		return false;
	}
	const String provider_id = active_provider.get("provider", String());
	const String model_id = active_provider.get("model", String());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, active_provider.get("base_url", String()));
	const StringName catalog_provider = StringName(profile.get("catalog_provider", provider_id));
	const Dictionary model = models_dev->get_model(catalog_provider, model_id);

	int resolved_context = (int)profile.get("context_window", 0);
	if ((int)model.get("context", 0) > 0) {
		resolved_context = model.get("context", 0);
	}
	const Dictionary model_limits = Dictionary(profile.get("model_limits", Dictionary())).get(model_id, Dictionary());
	if ((int)model_limits.get("context", 0) > 0) {
		resolved_context = model_limits.get("context", 0);
	}
	if (active_provider.has("context_window")) {
		resolved_context = (int)active_provider["context_window"];
	}

	int resolved_output = (int)profile.get("max_output_tokens", 8192);
	if ((int)model.get("output", 0) > 0) {
		resolved_output = model.get("output", 0);
	}
	if ((int)model_limits.get("output", 0) > 0) {
		resolved_output = model_limits.get("output", 0);
	}
	if (active_provider.has("max_tokens")) {
		resolved_output = (int)active_provider["max_tokens"];
	}

	const int previous_context = context_window;
	const int previous_output = max_output_tokens;
	context_window = resolved_context > 0 ? resolved_context : 0;
	max_output_tokens = resolved_output > 0 ? resolved_output : 8192;
	return context_window != previous_context || max_output_tokens != previous_output;
}

int SolersAgentSession::_active_model_input_support(const String &p_modality) const {
	if (!settings_service || !models_dev) {
		return -1;
	}
	const String provider_id = active_provider.get("provider", String());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, active_provider.get("base_url", String()));
	const StringName catalog_provider = StringName(profile.get("catalog_provider", provider_id));
	return SolersModelsDev::input_modality_support(models_dev->get_model(catalog_provider, active_provider.get("model", String())), p_modality);
}

Dictionary SolersAgentSession::_build_request(const Array &p_messages) const {
	Dictionary request;
	request["model"] = active_provider.get("model", String());
	request["system"] = system_prompt;
	request["tools"] = _collect_tools();
	request["messages"] = p_messages;
	const String reasoning_effort = String(active_provider.get("reasoning_effort", String())).strip_edges();
	if (!reasoning_effort.is_empty()) {
		const String provider_id = active_provider.get("provider", String());
		const Dictionary active_profile = settings_service ? settings_service->resolve_provider_profile(provider_id, active_provider.get("base_url", String())) : Dictionary();
		const StringName catalog_provider = StringName(active_profile.get("catalog_provider", provider_id));
		const Dictionary model = models_dev ? models_dev->get_model(catalog_provider, active_provider.get("model", String())) : Dictionary();
		if (SolersModelsDev::reasoning_efforts(model).has(reasoning_effort)) {
			request["reasoning_effort"] = reasoning_effort;
		}
	}
	const Dictionary profile = active_provider.get("profile", Dictionary());
	if (profile.get("supports_max_output_tokens", true)) {
		request["max_tokens"] = max_output_tokens;
	}
	request["session_id"] = session_id;
	return request;
}

Dictionary SolersAgentSession::_redacted_request_graph(const Dictionary &p_request, const Dictionary &p_profile) const {
	Dictionary graph;
	graph["provider"] = active_provider.get("provider", String());
	graph["model"] = p_request.get("model", String());
	graph["protocol"] = p_profile.get("protocol", String());

	Array items;
	if (!protocol_registry) {
		graph["items"] = items;
		return graph;
	}

	const StringName protocol_id = StringName(p_profile.get("protocol", String()));
	const SolersLLMProtocol *protocol = protocol_registry->get(protocol_id);
	if (!protocol) {
		graph["items"] = items;
		return graph;
	}

	const Dictionary body = protocol->build_request_body(p_request);
	const Array request_messages = body.has("messages") ? Array(body["messages"]) : Array(body.get("input", Array()));
	for (int i = 0; i < request_messages.size(); i++) {
		const Dictionary item = request_messages[i];
		Dictionary redacted;
		if (item.has("type")) {
			redacted["type"] = item.get("type", String());
		} else if (item.has("role")) {
			redacted["role"] = item.get("role", String());
		}
		if (item.has("tool_call_id")) {
			redacted["tool_call_id"] = item.get("tool_call_id", String());
		}
		if (item.has("name")) {
			redacted["name"] = item.get("name", String());
		}
		const Array tool_calls = item.get("tool_calls", Array());
		if (!tool_calls.is_empty()) {
			Array redacted_calls;
			for (int c = 0; c < tool_calls.size(); c++) {
				const Dictionary call = tool_calls[c];
				const Dictionary fn = call.get("function", Dictionary());
				Dictionary redacted_call;
				redacted_call["id"] = call.get("id", String());
				redacted_call["name"] = fn.get("name", String());
				redacted_calls.push_back(redacted_call);
			}
			redacted["tool_calls"] = redacted_calls;
		}
		items.push_back(redacted);
	}
	graph["items"] = items;
	return graph;
}

Dictionary SolersAgentSession::_provider_dispatch_error() const {
	if (!settings_service) {
		return _error("AGENT_UNCONFIGURED", "Solers agent session is missing its settings service.");
	}
	const String provider = active_provider.get("provider", String());
	const Dictionary current = settings_service->get_provider_config_for(provider).get("data", Dictionary());
	if (!current.get("connected", false)) {
		return _error("PROVIDER_NOT_CONNECTED", "The selected provider is not connected. Open Provider Settings to connect it.");
	}
	if (!current.get("available", false)) {
		return _error("LOCAL_MODELS_ONLY", "Local Models Only blocks the selected remote provider. Choose a local model or disable Local Models Only in Provider Settings.");
	}
	return Dictionary();
}

Error SolersAgentSession::_dispatch_model_request(bool p_skip_compaction) {
	const Dictionary availability_error = _provider_dispatch_error();
	if (!availability_error.is_empty()) {
		const Dictionary error = availability_error.get("error", Dictionary());
		_finish_turn("failed", error.get("message", "The selected provider is unavailable."), error);
		return ERR_UNAVAILABLE;
	}
	if (model_request_budget > 0 && model_request_index >= model_request_budget) {
		_finish_turn("paused", vformat("Turn paused after reaching its model request budget (%d). Start a new turn to continue with the persisted plan and evidence.", model_request_budget));
		return ERR_BUSY;
	}
	_append_background_asset_deltas(false);
	_flush_godot_diagnostics();
	if (_refresh_active_model_limits()) {
		Dictionary limits;
		limits["context_window"] = context_window;
		limits["max_output_tokens"] = max_output_tokens;
		limits["known"] = context_window > 0;
		_write_transcript_event("model_limits_updated", limits);
		_record("agent_model_limits_updated", limits);
	}
	const String provider_id = active_provider.get("provider", String());
	const String base_url = active_provider.get("base_url", String());
	const Dictionary auth = active_provider.get("auth", Dictionary());

	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url);
	const Array tools = _collect_tools();
	if (!p_skip_compaction && context_manager && context_manager->should_compact(messages, system_prompt, tools, context_window)) {
		return _begin_compaction(false);
	}

	Array request_messages = messages;
	if (context_manager) {
		request_messages = context_manager->prepare_request(
				messages,
				system_prompt,
				tools);
	}
	Dictionary request = _build_request(request_messages);
	current_provider_metadata.clear();
	model_request_index++;
	Dictionary request_event;
	request_event["request_index"] = model_request_index;
	request_event["provider"] = provider_id;
	request_event["model"] = active_provider.get("model", String());
	request_event["message_count"] = request_messages.size();
	_write_transcript_event("model_request_started", request_event);

	_record("agent_model_request_graph", _redacted_request_graph(request, profile));

	SOLERS_TRACE("session.begin", "client->begin() (joins prior worker thread)");
	const Error err = client->begin(request, profile, auth);
	SOLERS_TRACE("session.begin_done", vformat("err=%d", (int)err));
	if (err != OK) {
		const Dictionary error = client->get_error();
		_finish_turn("failed", String(error.get("message", "Failed to start the model request.")), error);
		return err;
	}
	running = true;
	phase = PHASE_STREAMING;
	streamed_tool_calls.clear();
	text_delta_count = 0;
	last_text_delta_msec = 0;
	SOLERS_TRACE("session.dispatch", vformat("model request started (turn=%d)", turn_id));
	emit_signal(SNAME("model_request_started"));
	return OK;
}

Error SolersAgentSession::_begin_compaction(bool p_from_overflow) {
	if (!context_manager || messages.is_empty()) {
		return ERR_UNAVAILABLE;
	}
	compaction_source_messages = messages.duplicate(true);
	compaction_request_attempt = 0;
	compaction_triggered_by_overflow = p_from_overflow;
	current_text = String();
	current_reasoning = String();
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	last_stop_reason = String();
	phase = PHASE_COMPACTING;
	Dictionary payload;
	payload["source"] = p_from_overflow ? "overflow" : "auto";
	payload["tokens_before"] = context_manager->get_token_count_with_pending(messages, system_prompt, _collect_tools());
	_record("context_compaction_started", payload);
	emit_signal(SNAME("compaction_started"));
	return _dispatch_compaction_request();
}

Error SolersAgentSession::_dispatch_compaction_request() {
	const Dictionary availability_error = _provider_dispatch_error();
	if (!availability_error.is_empty()) {
		const Dictionary error = availability_error.get("error", Dictionary());
		_finish_turn("failed", error.get("message", "The selected provider is unavailable."), error);
		return ERR_UNAVAILABLE;
	}
	_refresh_active_model_limits();
	const String provider_id = active_provider.get("provider", String());
	const String base_url = active_provider.get("base_url", String());
	const Dictionary auth = active_provider.get("auth", Dictionary());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url);

	Array history = compaction_source_messages;
	if (compaction_request_attempt > 0) {
		history = context_manager->shrink_compaction_history(history, compaction_request_attempt);
	}
	history = context_manager->prepare_request(
			history,
			system_prompt,
			Array());
	String instruction = SolersContextManager::COMPACTION_INSTRUCTION;
	if (!current_plan.is_empty()) {
		instruction += "\n\nCurrent plan:\n" + JSON::stringify(current_plan, "  ", false, true);
	}
	history.push_back(SolersLLMMessage::user(instruction));

	Dictionary request = _build_request(history);
	request["tools"] = Array();
	current_provider_metadata.clear();
	_record("context_compaction_request_graph", _redacted_request_graph(request, profile));

	const Error err = client->begin(request, profile, auth);
	if (err != OK) {
		const Dictionary error = client->get_error();
		_finish_turn("failed", String(error.get("message", "Failed to start context compaction.")), error);
		return err;
	}
	running = true;
	phase = PHASE_COMPACTING;
	text_delta_count = 0;
	last_text_delta_msec = 0;
	return OK;
}

bool SolersAgentSession::_is_context_overflow(const Dictionary &p_error) const {
	const String code = String(p_error.get("code", String())).to_lower();
	const String type = String(p_error.get("type", String())).to_lower();
	const String message = String(p_error.get("message", String())).to_lower();
	if (code.contains("context_length") || type.contains("context_length") ||
			message.contains("context_length_exceeded") || message.contains("context window") ||
			message.contains("maximum context") || message.contains("context too long")) {
		return true;
	}
	return (int)p_error.get("http_status", 0) == 413;
}

bool SolersAgentSession::_schedule_llm_retry(const Dictionary &p_error) {
	if (!SolersLLMRetry::is_retryable(p_error)) {
		return false;
	}
	retry_attempt++;
	const uint64_t wait = SolersLLMRetry::delay_msec(retry_attempt, p_error);
	retry_resume_msec = OS::get_singleton()->get_ticks_msec() + wait;
	current_text = String();
	current_reasoning = String();
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	Dictionary payload;
	payload["attempt"] = retry_attempt;
	payload["delay_msec"] = (int)wait;
	payload["code"] = p_error.get("code", String());
	payload["http_status"] = p_error.get("http_status", 0);
	payload["phase"] = phase == PHASE_COMPACTING ? "compaction" : "model";
	_record("agent_turn_retrying", payload);
	_write_transcript_event("model_retry", payload);
	String message = String(p_error.get("message", String())).strip_edges();
	const int http_status = p_error.get("http_status", 0);
	if (http_status > 0) {
		message = vformat("HTTP %d%s", http_status, message.is_empty() ? String() : ": " + message);
	}
	message += vformat(" (retrying in %s s)", String::num(double(wait) / 1000.0, 1));
	emit_signal(SNAME("turn_retrying"), retry_attempt, message);
	return true;
}

void SolersAgentSession::_poll_compaction() {
	const Array events = client->poll();
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		const String kind = event.get("kind", String());
		if (kind == SolersLLMEventKind::TEXT_DELTA) {
			current_text += String(event.get("text", String()));
		} else if (kind == SolersLLMEventKind::REASONING_DELTA) {
			current_reasoning += String(event.get("text", String()));
		} else if (kind == SolersLLMEventKind::USAGE) {
			last_usage = event;
		} else if (kind == SolersLLMEventKind::FINISH) {
			last_stop_reason = event.get("stop_reason", String());
		}
	}

	if (client->is_failed()) {
		const Dictionary error = client->get_error();
		if (_is_context_overflow(error) && compaction_request_attempt < 3) {
			compaction_request_attempt++;
			current_text = String();
			current_reasoning = String();
			_dispatch_compaction_request();
			return;
		}
		if (_schedule_llm_retry(error)) {
			return;
		}
		_finish_turn("failed", String(error.get("message", "Context compaction failed.")), error);
		return;
	}
	if (!client->is_done()) {
		return;
	}
	if ((current_text.strip_edges().is_empty() || last_stop_reason == SolersLLMStopReason::MAX_TOKENS) && compaction_request_attempt < 5) {
		compaction_request_attempt++;
		current_text = String();
		current_reasoning = String();
		_dispatch_compaction_request();
		return;
	}
	if (current_text.strip_edges().is_empty()) {
		const Dictionary error = _error("COMPACTION_FAILED", "The model did not produce a usable context summary.").get("error", Dictionary());
		_finish_turn("failed", String(error.get("message", "Context compaction failed.")), error);
		return;
	}
	_on_compaction_complete();
}

void SolersAgentSession::_on_compaction_complete() {
	const Dictionary result = context_manager->apply_compaction(compaction_source_messages, current_text, current_plan);
	messages = result.get("messages", Array());
	_write_transcript_compaction(result);
	_record("context_compaction_completed", result);
	emit_signal(SNAME("compaction_completed"), result);

	compaction_source_messages.clear();
	compaction_request_attempt = 0;
	compaction_triggered_by_overflow = false;
	retry_attempt = 0;
	retry_resume_msec = 0;
	current_text = String();
	current_reasoning = String();
	last_stop_reason = String();
	_dispatch_model_request(true);
}

Dictionary SolersAgentSession::_tool_call_from_event(const Dictionary &p_event) const {
	const String requested_name = p_event.get("name", String());
	const StringName canonical_name = tool_registry ? tool_registry->resolve_model_tool_name(requested_name) : StringName();
	String model_name = requested_name;
	if (!String(canonical_name).is_empty() && tool_registry) {
		const String registered_model_name = tool_registry->get_model_tool_name(canonical_name);
		if (!registered_model_name.is_empty()) {
			model_name = registered_model_name;
		}
	}

	Dictionary call;
	call["id"] = p_event.get("id", String());
	call["name"] = model_name;
	call["canonical_name"] = String(canonical_name);
	call["requested_name"] = requested_name;
	call["arguments"] = p_event.get("arguments", String());
	const Dictionary provider_metadata = p_event.get("provider_metadata", Dictionary());
	if (!provider_metadata.is_empty()) {
		call["provider_metadata"] = provider_metadata;
		if (provider_metadata.has("status")) {
			call["status"] = provider_metadata["status"];
		}
	}
	return call;
}

Dictionary SolersAgentSession::_merge_streamed_tool_call(const Dictionary &p_call) {
	Dictionary call = p_call.duplicate(true);
	const String id = call.get("id", String());
	if (id.is_empty()) {
		return call;
	}

	const Dictionary previous = streamed_tool_calls.get(id, Dictionary());
	if (!previous.is_empty()) {
		if (String(call.get("name", String())).is_empty()) {
			call["name"] = previous.get("name", String());
		}
		if (String(call.get("canonical_name", String())).is_empty()) {
			call["canonical_name"] = previous.get("canonical_name", String());
		}
		if (String(call.get("requested_name", String())).is_empty()) {
			call["requested_name"] = previous.get("requested_name", String());
		}
		if (String(call.get("arguments", String())).is_empty()) {
			call["arguments"] = previous.get("arguments", String());
		}
		if ((bool)previous.get("ui_announced", false)) {
			call["ui_announced"] = true;
		}
	}
	streamed_tool_calls[id] = call;
	return call;
}

Dictionary SolersAgentSession::_surface_tool_call(const Dictionary &p_call) {
	Dictionary call = _merge_streamed_tool_call(p_call);
	const String id = call.get("id", String());
	if (id.is_empty()) {
		return call;
	}

	const bool was_announced = (bool)call.get("ui_announced", false);
	call["ui_announced"] = true;
	streamed_tool_calls[id] = call;
	const String canonical_name = call.get("canonical_name", String());
	const String arguments = call.get("arguments", String());
	if (_is_session_tool(canonical_name)) {
		return call;
	}
	if (was_announced) {
		emit_signal(SNAME("tool_call_updated"), id, canonical_name, arguments);
	} else {
		emit_signal(SNAME("tool_call_started"), id, canonical_name, arguments);
	}
	return call;
}

Array SolersAgentSession::_attachments_for_ids(const Array &p_ids) const {
	Array out;
	for (int i = 0; i < p_ids.size(); i++) {
		const String wanted = String(p_ids[i]).strip_edges();
		if (wanted.is_empty()) {
			continue;
		}
		bool found = false;
		for (int m = messages.size() - 1; m >= 0; m--) {
			const Dictionary message = messages[m];
			const Array attachments = message.get("attachments", Array());
			for (int a = 0; a < attachments.size(); a++) {
				const Dictionary attachment = attachments[a];
				if (String(attachment.get("id", String())).strip_edges() == wanted) {
					out.push_back(attachment);
					found = true;
					break;
				}
			}
			if (found) {
				break;
			}
		}
	}
	return out;
}

Dictionary SolersAgentSession::start_turn(const Dictionary &p_args) {
	if (running) {
		return _error("AGENT_BUSY", "A Solers agent turn is already running.");
	}
	if (!tool_registry || !settings_service) {
		return _error("AGENT_UNCONFIGURED", "Solers agent session is missing its services.");
	}

	const String prompt = String(p_args.get("prompt", String())).strip_edges();
	turn_attachments = p_args.get("attachments", Array()).duplicate(true);
	if (prompt.is_empty() && turn_attachments.is_empty()) {
		return _error("EMPTY_PROMPT", "Prompt is empty.");
	}
	background_resume_suppressed = false;
	waiting_background_asset_ids.clear();
	model_request_budget = MAX(0, (int)p_args.get("max_model_requests", 0));

	active_provider = settings_service->resolve_active_provider();
	const String provider_id = active_provider.get("provider", String());
	const String model = active_provider.get("model", String());
	const String base_url = active_provider.get("base_url", String());
	const Dictionary auth = active_provider.get("auth", Dictionary());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url);
	const bool local = profile.get("local", false);

	_refresh_active_model_limits();

	const Dictionary availability_error = _provider_dispatch_error();
	if (!availability_error.is_empty()) {
		Dictionary e = availability_error;
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (models_dev) {
		models_dev->refresh();
	}
	if (model.is_empty()) {
		Dictionary e = _error("NO_MODEL", "No model is configured. Choose one from a connected provider in the Chat panel.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (!settings_service->is_model_allowed(provider_id, model)) {
		Dictionary e = _error("MODEL_NOT_ALLOWED", "The selected model is not available through this provider connection.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (String(profile.get("base_url", String())).strip_edges().is_empty()) {
		Dictionary e = _error("NO_BASE_URL", "No base URL configured for this provider.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	const String auth_type = auth.get("type", String());
	const bool credential_ready = auth_type == "oauth" ? (!String(auth.get("access", String())).is_empty() || !String(auth.get("refresh", String())).is_empty()) : !String(auth.get("key", String())).is_empty();
	if (!local && !credential_ready) {
		Dictionary e = _error("NO_PROVIDER_CREDENTIAL", "The selected provider is not connected. Open Provider Settings to connect it.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (!turn_attachments.is_empty() && _active_model_input_support("image") == 0) {
		Dictionary e = _error("VISION_CAPABILITY_REQUIRED", "The selected model does not support image input. Choose a vision-capable model before sending image references.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}

	if (system_prompt.is_empty()) {
		system_prompt = _default_system_prompt();
	}
	String model_prompt = prompt;
	if (!turn_attachments.is_empty()) {
		model_prompt += model_prompt.is_empty() ? String() : "\n\n";
		model_prompt += "[Attached images available for tools]\n";
		for (int i = 0; i < turn_attachments.size(); i++) {
			const Dictionary attachment = turn_attachments[i];
			model_prompt += vformat("- %s (%s)\n", String(attachment.get("id", String())), String(attachment.get("filename", String("image"))));
		}
	}
	Dictionary user_message = SolersLLMMessage::user(model_prompt);
	if (!turn_attachments.is_empty()) {
		user_message["attachments"] = turn_attachments.duplicate(true);
	}
	messages.push_back(user_message);
	current_text = String();
	current_reasoning = String();
	current_provider_metadata.clear();
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	readonly_cache.clear();
	failed_resource_accesses.clear();
	last_usage.clear();
	overflow_compaction_attempts = 0;
	compaction_request_attempt = 0;
	retry_attempt = 0;
	retry_resume_msec = 0;
	model_request_index = 0;
	turn_id++;
	_ensure_godot_log_audit(true);

	Dictionary turn_started;
	turn_started["model"] = model;
	turn_started["provider"] = provider_id;
	turn_started["context_window"] = context_window;
	turn_started["max_output_tokens"] = max_output_tokens;
	turn_started["model_limits_known"] = context_window > 0;
	_write_transcript_event("turn_started", turn_started);
	_record("agent_turn_started", turn_started);
	_write_transcript_message("user", model_prompt);

	const Error err = _dispatch_model_request();
	if (err != OK) {
		return _error("DISPATCH_FAILED", "Failed to dispatch the model request.");
	}
	Dictionary data;
	data["turn_id"] = turn_id;
	return _ok(data);
}

void SolersAgentSession::poll() {
	if (!running || !client) {
		return;
	}
	const Dictionary auth_update = client->take_auth_update();
	if (!auth_update.is_empty() && settings_service) {
		const String provider = active_provider.get("provider", String());
		settings_service->store_provider_auth(provider, auth_update);
		active_provider["auth"] = auth_update;
	}
	if (retry_resume_msec != 0) {
		if (OS::get_singleton()->get_ticks_msec() < retry_resume_msec) {
			return;
		}
		retry_resume_msec = 0;
		if (phase == PHASE_COMPACTING) {
			_dispatch_compaction_request();
		} else {
			_dispatch_model_request();
		}
		return;
	}
	if (phase == PHASE_WAITING) {
		return;
	}
	if (phase == PHASE_AWAITING_APPROVAL || phase == PHASE_TOOL_EXECUTING || phase == PHASE_TOOLS) {
		// Admit every immediately runnable call from one model response in the
		// same main-loop turn. Pending calls release the execution slot, so
		// independent imports can join one native scan wave. Worker calls and
		// unresolved approvals naturally stop this bounded drive loop.
		const int drive_budget = MAX(8, tool_queue.size() * 4 + pending_tool_executions.size() * 2);
		for (int i = 0; i < drive_budget && running; i++) {
			const Phase before_phase = phase;
			const int before_queue = tool_queue_index;
			const int before_delivery = tool_delivery_index;
			const int before_pending = pending_tool_executions.size();
			const uint64_t before_token = tool_exec_token;
			if (phase == PHASE_AWAITING_APPROVAL) {
				_poll_awaiting_approval();
			} else if (phase == PHASE_TOOL_EXECUTING) {
				_poll_tool_executing();
			} else if (phase == PHASE_TOOLS) {
				_poll_tool_queue();
			} else {
				break;
			}
			if (phase == PHASE_STREAMING || phase == PHASE_COMPACTING || !running) {
				break;
			}
			const bool progressed = phase != before_phase || tool_queue_index != before_queue || tool_delivery_index != before_delivery || pending_tool_executions.size() != before_pending || tool_exec_token != before_token;
			if (!progressed) {
				break;
			}
		}
		return;
	}
	if (phase == PHASE_COMPACTING) {
		_poll_compaction();
		return;
	}
	const Array events = client->poll();
	Dictionary protocol_error;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary e = events[i];
		const String kind = e.get("kind", String());
		if (kind == SolersLLMEventKind::TEXT_DELTA) {
			const String text = e.get("text", String());
			current_text += text;
			const uint64_t now = OS::get_singleton()->get_ticks_msec();
			const uint64_t gap = last_text_delta_msec ? (now - last_text_delta_msec) : 0;
			last_text_delta_msec = now;
			text_delta_count++;
			if (text_delta_count == 1 || gap > 800 || (text_delta_count % 40) == 0) {
				SOLERS_TRACE("session.text_delta", vformat("#%d gap=%dms +%dB total=%dB", text_delta_count, (int)gap, text.length(), current_text.length()));
			}
			emit_signal(SNAME("assistant_delta"), text);
		} else if (kind == SolersLLMEventKind::REASONING_DELTA) {
			const String text = e.get("text", String());
			current_reasoning += text;
			emit_signal(SNAME("reasoning_delta"), text);
		} else if (kind == SolersLLMEventKind::TOOL_INPUT_START || kind == SolersLLMEventKind::TOOL_INPUT_DELTA) {
			_surface_tool_call(_tool_call_from_event(e));
		} else if (kind == SolersLLMEventKind::TOOL_CALL) {
			Dictionary call = _surface_tool_call(_tool_call_from_event(e));
			pending_tool_calls.push_back(call);
		} else if (kind == SolersLLMEventKind::USAGE) {
			last_usage = e;
			if (context_manager) {
				context_manager->record_usage((int)e.get("input_tokens", 0), messages.size());
			}
		} else if (kind == SolersLLMEventKind::FINISH) {
			last_stop_reason = e.get("stop_reason", String());
			current_provider_metadata = e.get("provider_metadata", Dictionary());
		} else if (kind == SolersLLMEventKind::ERROR) {
			if (protocol_error.is_empty()) {
				protocol_error["code"] = e.get("code", "PROTOCOL_ERROR");
				protocol_error["message"] = e.get("message", "The provider stream reported a protocol error.");
				protocol_error["recoverable"] = true;
			}
		}
	}
	if (!protocol_error.is_empty()) {
		client->abort();
		_finish_turn("failed", protocol_error.get("message", String()), protocol_error);
		return;
	}

	if (client->is_failed()) {
		const Dictionary error = client->get_error();
		if (_is_context_overflow(error)) {
			overflow_compaction_attempts++;
			if (overflow_compaction_attempts <= 3) {
				_begin_compaction(true);
				return;
			}
		}
		if (_schedule_llm_retry(error)) {
			return;
		}
		_finish_turn("failed", String(error.get("message", "Agent turn failed.")), error);
		return;
	}
	if (client->is_done()) {
		_on_model_turn_complete();
	}
}

void SolersAgentSession::_on_model_turn_complete() {
	retry_attempt = 0;
	overflow_compaction_attempts = 0;
	Dictionary response_event;
	response_event["request_index"] = model_request_index;
	response_event["stop_reason"] = last_stop_reason;
	response_event["tool_call_count"] = pending_tool_calls.size();
	response_event["text_bytes"] = current_text.utf8().length();
	if (!last_usage.is_empty()) {
		response_event["usage"] = last_usage;
	}
	_write_transcript_event("model_response", response_event);
	HashSet<String> operation_ids;
	for (int i = 0; i < pending_tool_calls.size(); i++) {
		const String operation_id = String(Dictionary(pending_tool_calls[i]).get("id", String())).strip_edges();
		if (operation_id.is_empty() || operation_ids.has(operation_id)) {
			Dictionary error;
			error["code"] = "INVALID_TOOL_CALL_ID";
			error["message"] = operation_id.is_empty() ? "The provider returned a tool call without a stable operation id." : vformat("The provider reused operation id '%s' for more than one tool call in the same response.", operation_id);
			error["recoverable"] = true;
			_finish_turn("failed", error.get("message", String()), error);
			return;
		}
		operation_ids.insert(operation_id);
	}
	messages.push_back(SolersLLMMessage::assistant(current_text, pending_tool_calls, current_provider_metadata));
	if (!current_text.is_empty() || !pending_tool_calls.is_empty()) {
		_write_transcript_message("assistant", current_text);
	}
	if (pending_tool_calls.is_empty()) {
		const String final_text = current_text;
		if (!current_text.is_empty()) {
			emit_signal(SNAME("assistant_message"), current_text);
		}
		last_assistant_msec = OS::get_singleton()->get_ticks_msec();
		current_text = String();
		current_reasoning = String();
		pending_tool_calls.clear();
		streamed_tool_calls.clear();
		_finish_turn("completed", final_text);
		return;
	}
	if (!current_text.is_empty()) {
		emit_signal(SNAME("assistant_message"), current_text);
	}
	last_assistant_msec = OS::get_singleton()->get_ticks_msec();

	tool_queue = pending_tool_calls.duplicate();
	const uint64_t queued_msec = OS::get_singleton()->get_ticks_msec();
	for (int i = 0; i < tool_queue.size(); i++) {
		Dictionary call = tool_queue[i];
		call["queued_msec"] = (int64_t)queued_msec;
		const String model_name = call.get("name", String());
		const String canonical_name = call.get("canonical_name", model_name);
		const String requested_name = call.get("requested_name", model_name);
		const String arguments = call.get("arguments", "{}");
		Dictionary preflight_result;
		Dictionary parsed_args;
		if (canonical_name.is_empty()) {
			preflight_result = _tool_denied_result("TOOL_NOT_FOUND", vformat("Model requested an unknown Solers tool: %s.", requested_name));
		} else {
			Ref<JSON> json;
			json.instantiate();
			const Error parse_error = json->parse(arguments.is_empty() ? "{}" : arguments);
			const Variant parsed = parse_error == OK ? json->get_data() : Variant();
			if (parse_error != OK || parsed.get_type() != Variant::DICTIONARY) {
				preflight_result = _tool_denied_result("TOOL_ARGUMENT_INVALID", "Tool arguments must be a complete JSON object.");
			} else if (!tool_registry) {
				preflight_result = _tool_denied_result("AGENT_UNCONFIGURED", "Tool registry unavailable.");
			} else {
				parsed_args = parsed;
				SolersToolContext context;
				context.call_id = call.get("id", String());
				context.session_id = session_id;
				context.authored_revision = authored_revision;
				String failure_cache_key;
				Dictionary normalized_args;
				preflight_result = tool_registry->_preflight_tool_call(StringName(canonical_name), parsed_args, context, normalized_args, failure_cache_key);
			}
		}
		call["parsed_args"] = parsed_args;
		if (!preflight_result.is_empty()) {
			call["preflight_result"] = preflight_result;
		}
		tool_queue[i] = call;
	}
	tool_queue_index = 0;
	tool_delivery_index = 0;
	completed_tool_results.clear();
	failed_resource_accesses.clear();
	tool_started_announced = false;
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	current_text = String();
	current_reasoning = String();
	phase = PHASE_TOOLS;
	SOLERS_TRACE("session.tools", vformat("entering tool queue (%d call(s))", tool_queue.size()));
}

void SolersAgentSession::_finish_turn(const String &p_outcome, const String &p_message, const Dictionary &p_error) {
	if (tool_thread_state) {
		tool_cancel_requested.set();
		_collect_tool_thread_result(true);
	}
	Dictionary data;
	data["outcome"] = p_outcome;
	data["text"] = p_message;
	data["reasoning"] = current_reasoning;
	data["stop_reason"] = last_stop_reason;
	const Dictionary scene_commit = p_outcome == "aborted" ? Dictionary() : _commit_dirty_scene_if_needed();
	if (!scene_commit.is_empty()) {
		data["scene_commit"] = scene_commit;
	}
	if (!last_usage.is_empty()) {
		data["usage"] = last_usage;
	}
	if (!p_error.is_empty()) {
		data["error"] = p_error;
	}
	if (turn_runtime_owned) {
		EditorInterface *editor_interface = EditorInterface::get_singleton();
		if (editor_interface && editor_interface->is_playing_scene()) {
			editor_interface->stop_playing_scene();
			data["runtime_stopped"] = true;
		}
	}

	Dictionary transcript;
	transcript["outcome"] = p_outcome;
	transcript["message"] = p_message;
	transcript["stop_reason"] = last_stop_reason;
	{
		MutexLock lock(godot_log_mutex);
		transcript["godot_log_errors"] = godot_log_error_count;
		transcript["godot_log_warnings"] = godot_log_warning_count;
	}
	if (!scene_commit.is_empty()) {
		transcript["scene_commit"] = scene_commit;
	}
	if (!last_usage.is_empty()) {
		transcript["usage"] = last_usage;
	}
	if (!p_error.is_empty()) {
		transcript["error"] = p_error;
	}
	_write_transcript_event("turn_outcome", transcript);
	godot_log_turn_active = false;
	last_outcome = p_outcome;
	running = false;
	phase = PHASE_STREAMING;
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	tool_queue.clear();
	_clear_pending_tools();
	completed_tool_results.clear();
	failed_resource_accesses.clear();
	readonly_cache.clear();
	if (tool_registry) {
		tool_registry->clear_task_state(session_id);
	}
	task_deferred_tools.clear();
	tool_queue_index = 0;
	tool_delivery_index = 0;
	tool_started_announced = false;
	tool_queued_msec = 0;
	tool_started_msec = 0;
	tool_completed_msec = 0;
	deferred_queue_index = -1;
	awaiting_call.clear();
	awaiting_approval_id = 0;
	retry_attempt = 0;
	retry_resume_msec = 0;
	compaction_source_messages.clear();
	compaction_request_attempt = 0;
	overflow_compaction_attempts = 0;
	compaction_triggered_by_overflow = false;
	tool_exec_token++;
	tool_exec_requested = false;
	last_progress_call_id = String();
	last_progress_msec = 0;
	deferred_done = false;
	deferred_result.clear();
	deferred_args.clear();
	deferred_initial_args.clear();
	deferred_resource_accesses.clear();
	deferred_is_resume = false;
	deferred_polling = false;
	deferred_call_id = String();
	deferred_model_name = String();
	deferred_canonical_name = String();
	current_text = String();
	current_reasoning = String();
	turn_attachments.clear();
	waiting_background_asset_ids.clear();
	turn_runtime_owned = false;
	if (p_outcome == "aborted") {
		background_resume_suppressed = true;
	}
	if (p_outcome == "failed") {
		_record("agent_turn_failed", data);
		emit_signal(SNAME("turn_failed"), p_error);
	} else {
		_record("agent_turn_completed", data);
		emit_signal(SNAME("turn_completed"), data);
	}
}

void SolersAgentSession::_poll_tool_queue() {
	if (!_flush_tool_results() || !running) {
		return;
	}

	if (tool_queue_index >= tool_queue.size()) {
		if (_resume_ready_pending_tool()) {
			return;
		}
		if (!pending_tool_executions.is_empty()) {
			return;
		}
		if (tool_delivery_index < tool_queue.size()) {
			return;
		}
		if (_poll_state_observation()) {
			return; // the automatic capture is still rendering; poll again next tick.
		}
		tool_queue.clear();
		completed_tool_results.clear();
		tool_queue_index = 0;
		tool_delivery_index = 0;
		tool_started_announced = false;
		retry_attempt = 0;
		const Error err = _dispatch_model_request();
		if (err != OK) {
			current_reasoning = String();
		}
		return;
	}

	const int queue_index = tool_queue_index;
	const Dictionary call = tool_queue[queue_index];
	const String name = call.get("name", String());
	const String canonical_name = call.get("canonical_name", name);
	const String requested_name = call.get("requested_name", name);
	const String id = call.get("id", String());
	const String arguments = call.get("arguments", "{}");

	if (!tool_started_announced) {
		tool_started_announced = true;
		tool_started_msec = 0;
		if (!(bool)call.get("ui_announced", false)) {
			emit_signal(SNAME("tool_call_started"), id, canonical_name, arguments);
		}
	}

	auto write_rejected_start = [&](const String &p_tool) {
		tool_started_msec = OS::get_singleton()->get_ticks_msec();
		Dictionary audit;
		audit["call_id"] = id;
		audit["tool"] = p_tool;
		audit["arguments_sha256"] = arguments.sha256_text();
		audit["arguments_bytes"] = arguments.utf8().length();
		_write_transcript_event("tool_started", audit);
	};
	const Dictionary preflight_result = call.get("preflight_result", Dictionary());
	if (!preflight_result.is_empty()) {
		const String rejected_name = canonical_name.is_empty() ? requested_name : canonical_name;
		write_rejected_start(rejected_name);
		Dictionary args = call.get("parsed_args", Dictionary());
		if (args.is_empty() && !arguments.is_empty() && arguments != "{}") {
			args["arguments_sha256"] = arguments.sha256_text();
			args["arguments_bytes"] = arguments.utf8().length();
		}
		_queue_tool_result(queue_index, id, name, rejected_name, args, preflight_result, tool_started_msec);
		tool_queue_index++;
		tool_started_announced = false;
		return;
	}
	const Dictionary parsed_args = call.get("parsed_args", Dictionary());
	const Array accesses = tool_registry ? tool_registry->resolve_resource_access(StringName(canonical_name), parsed_args) : Array();
	if (_conflicts_with_pending(accesses)) {
		if (_resume_ready_pending_tool()) {
			return;
		}
		return;
	}
	tool_started_msec = OS::get_singleton()->get_ticks_msec();
	Dictionary audit;
	audit["call_id"] = id;
	audit["tool"] = canonical_name;
	audit["resume"] = false;
	audit["args"] = tool_registry ? tool_registry->protect_tool_args_for_replay(StringName(canonical_name), parsed_args) : parsed_args;
	_write_transcript_event("tool_started", audit);
	if (tool_registry && SolersToolRegistry::has_write_conflict(failed_resource_accesses, accesses)) {
		const Dictionary skipped = _tool_denied_result("SKIPPED_AFTER_FAILURE", "Skipped because a failed prerequisite touched the same resource. Re-evaluate that resource before retrying.");
		_queue_tool_result(queue_index, id, name, canonical_name, parsed_args, skipped, tool_started_msec);
		tool_queue_index++;
		tool_started_announced = false;
		return;
	}
	if (tool_registry && tool_registry->is_read_only(StringName(canonical_name), parsed_args)) {
		const String cache_key = _readonly_cache_key(StringName(canonical_name), parsed_args);
		const Dictionary *cached = readonly_cache.getptr(cache_key);
		if (cached) {
			Dictionary result = cached->duplicate(true);
			Dictionary data = result.get("data", Dictionary());
			data["unchanged"] = true;
			data["authored_revision"] = (int64_t)authored_revision;
			result["data"] = data;
			_queue_tool_result(queue_index, id, name, canonical_name, parsed_args, result, tool_started_msec);
			tool_queue_index++;
			tool_started_announced = false;
			return;
		}
	}
	_schedule_tool_execution(queue_index, id, name, canonical_name, parsed_args, false);
	tool_queue_index++;
	tool_started_announced = false;
}

bool SolersAgentSession::_conflicts_with_pending(const Array &p_accesses) const {
	for (int i = 0; i < pending_tool_executions.size(); i++) {
		const Array &pending_accesses = pending_tool_executions[i]->resource_accesses;
		if (SolersToolRegistry::has_write_conflict(pending_accesses, p_accesses) || SolersToolRegistry::has_write_conflict(p_accesses, pending_accesses)) {
			return true;
		}
	}
	return false;
}

void SolersAgentSession::_park_pending_tool(const Dictionary &p_poll_args) {
	PendingToolExecution *pending = memnew(PendingToolExecution);
	pending->queue_index = deferred_queue_index;
	pending->call_id = deferred_call_id;
	pending->model_name = deferred_model_name;
	pending->canonical_name = deferred_canonical_name;
	pending->poll_args = p_poll_args.duplicate(true);
	pending->initial_args = deferred_initial_args.duplicate(true);
	pending->resource_accesses = deferred_resource_accesses.duplicate(true);
	pending->scene_digest_before = deferred_scene_digest_before;
	pending->geometry_digest_before = deferred_geometry_digest_before;
	pending->prepared_call = deferred_prepared_call;
	pending->is_resume = deferred_is_resume;
	pending->started_msec = tool_started_msec;
	pending_tool_executions.push_back(pending);

	deferred_prepared_call = nullptr;
	deferred_queue_index = -1;
	deferred_call_id = String();
	deferred_model_name = String();
	deferred_canonical_name = String();
	deferred_args.clear();
	deferred_initial_args.clear();
	deferred_resource_accesses.clear();
	deferred_result.clear();
	deferred_done = false;
	deferred_polling = false;
	deferred_is_resume = false;
	tool_exec_requested = false;
	phase = PHASE_TOOLS;
}

bool SolersAgentSession::_resume_ready_pending_tool() {
	if (!tool_registry) {
		return false;
	}
	for (int i = 0; i < pending_tool_executions.size(); i++) {
		PendingToolExecution *pending = pending_tool_executions[i];
		if (pending->prepared_call && !tool_registry->_is_prepared_tool_ready(*pending->prepared_call, pending->poll_args)) {
			continue;
		}
		pending_tool_executions.remove_at(i);
		deferred_queue_index = pending->queue_index;
		deferred_call_id = pending->call_id;
		deferred_model_name = pending->model_name;
		deferred_canonical_name = pending->canonical_name;
		deferred_args = pending->poll_args;
		deferred_initial_args = pending->initial_args;
		deferred_resource_accesses = pending->resource_accesses;
		deferred_scene_digest_before = pending->scene_digest_before;
		deferred_geometry_digest_before = pending->geometry_digest_before;
		deferred_prepared_call = pending->prepared_call;
		deferred_is_resume = pending->is_resume;
		tool_started_msec = pending->started_msec;
		deferred_polling = true;
		deferred_done = false;
		deferred_result.clear();
		tool_cancel_requested.clear();
		phase = PHASE_TOOL_EXECUTING;
		++tool_exec_token;
		tool_exec_requested = true;
		memdelete(pending);
		return true;
	}
	return false;
}

void SolersAgentSession::_queue_tool_result(int p_queue_index, const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, uint64_t p_started_msec) {
	const uint64_t completed_msec = OS::get_singleton()->get_ticks_msec();
	Dictionary terminal;
	terminal["id"] = p_id;
	terminal["model_name"] = p_model_name;
	terminal["canonical_name"] = p_canonical_name;
	terminal["args"] = p_args.duplicate(true);
	terminal["result"] = p_result.duplicate(true);
	int64_t queued_msec = completed_msec;
	if (p_queue_index >= 0 && p_queue_index < tool_queue.size()) {
		queued_msec = (int64_t)Dictionary(tool_queue[p_queue_index]).get("queued_msec", queued_msec);
	}
	terminal["queued_msec"] = queued_msec;
	terminal["started_msec"] = (int64_t)(p_started_msec > 0 ? p_started_msec : completed_msec);
	terminal["completed_msec"] = (int64_t)completed_msec;
	completed_tool_results[p_queue_index] = terminal;
}

bool SolersAgentSession::_flush_tool_results() {
	while (running) {
		const Dictionary *terminal = completed_tool_results.getptr(tool_delivery_index);
		if (!terminal) {
			return true;
		}
		const Dictionary entry = *terminal;
		completed_tool_results.erase(tool_delivery_index);
		tool_queued_msec = (uint64_t)(int64_t)entry.get("queued_msec", 0);
		tool_started_msec = (uint64_t)(int64_t)entry.get("started_msec", 0);
		tool_completed_msec = (uint64_t)(int64_t)entry.get("completed_msec", tool_started_msec);
		const String canonical_name = entry.get("canonical_name", String());
		const Dictionary result = entry.get("result", Dictionary());
		_deliver_tool_result(entry.get("id", String()), entry.get("model_name", String()), canonical_name, entry.get("args", Dictionary()), result);
		tool_delivery_index++;
	}
	return false;
}

void SolersAgentSession::_clear_pending_tools(const Dictionary &p_terminal_result) {
	for (int i = 0; i < pending_tool_executions.size(); i++) {
		PendingToolExecution *pending = pending_tool_executions[i];
		if (pending->prepared_call) {
			if (tool_registry && !p_terminal_result.is_empty()) {
				tool_registry->_complete_prepared_tool(*pending->prepared_call, p_terminal_result);
			}
			memdelete(pending->prepared_call);
		}
		memdelete(pending);
	}
	pending_tool_executions.clear();
}

void SolersAgentSession::_schedule_tool_execution(int p_queue_index, const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, bool p_is_resume) {
	if (tool_thread_state) {
		tool_cancel_requested.set();
		_collect_tool_thread_result(true);
	}
	tool_cancel_requested.clear();
	if (deferred_prepared_call) {
		memdelete(deferred_prepared_call);
		deferred_prepared_call = nullptr;
	}
	deferred_call_id = p_id;
	deferred_queue_index = p_queue_index;
	deferred_model_name = p_model_name;
	deferred_canonical_name = p_canonical_name;
	deferred_args = p_args.duplicate(true);
	deferred_initial_args = deferred_args;
	deferred_resource_accesses = tool_registry ? tool_registry->resolve_resource_access(StringName(p_canonical_name), p_args) : Array();
	deferred_scene_digest_before = 0;
	deferred_geometry_digest_before = 0;
	if (tool_registry && tool_registry->reflection_service && tool_registry->affects_scene_state(StringName(p_canonical_name))) {
		deferred_scene_digest_before = tool_registry->reflection_service->get_scene_state_digest();
		deferred_geometry_digest_before = tool_registry->reflection_service->get_spatial_geometry_digest();
	}
	if (p_canonical_name == "asset.generate") {
		Array ids = deferred_args.get("input_attachments", Array());
		if (!ids.is_empty()) {
			deferred_args["_attachments"] = _attachments_for_ids(ids);
		}
	}
	deferred_result = Dictionary();
	deferred_done = false;
	deferred_polling = false;
	deferred_is_resume = p_is_resume;
	phase = PHASE_TOOL_EXECUTING;
	const uint64_t token = ++tool_exec_token;
	tool_exec_requested = true;
	SOLERS_TRACE("session.schedule_tool", vformat("%s resume=%d token=%d", p_canonical_name, (int)p_is_resume, (int)token));
}

void SolersAgentSession::_execute_deferred_tool(uint64_t p_token) {
	if (!running || phase != PHASE_TOOL_EXECUTING || p_token != tool_exec_token) {
		SOLERS_TRACE("session.exec_tool", vformat("stale deferred call dropped (token=%d cur=%d running=%d)", (int)p_token, (int)tool_exec_token, (int)running));
		return;
	}
	if (!tool_registry) {
		deferred_result = _error("AGENT_UNCONFIGURED", "Tool registry unavailable.").get("error", Dictionary());
		Dictionary wrap;
		wrap["ok"] = false;
		wrap["error"] = deferred_result;
		deferred_result = wrap;
		deferred_done = true;
		return;
	}
	if (deferred_canonical_name == "viewport.capture") {
		if (_active_model_input_support("image") == 0) {
			deferred_result = _tool_denied_result("VISION_CAPABILITY_REQUIRED", "The selected model does not support image input, so viewport captures cannot be returned to it.");
			deferred_done = true;
			return;
		}
	}
	SOLERS_TRACE("session.exec_tool", vformat("BEGIN %s", deferred_canonical_name));
	SolersToolContext context;
	context.call_id = deferred_call_id;
	context.session_id = session_id;
	context.authored_revision = authored_revision;
	context.cancel_requested = &tool_cancel_requested;
	if (!deferred_prepared_call) {
		SolersPreparedToolCall prepared;
		const Dictionary preparation_error = tool_registry->_prepare_tool_call(StringName(deferred_canonical_name), deferred_args, context, prepared);
		if (!preparation_error.is_empty()) {
			deferred_result = preparation_error;
			deferred_done = true;
			return;
		}
		deferred_prepared_call = memnew(SolersPreparedToolCall(prepared));
	}
	if (deferred_prepared_call->execution == SolersToolExecution::WORKER_THREAD) {
		ToolThreadState *state = memnew(ToolThreadState);
		state->registry = tool_registry;
		state->session = this;
		state->call = *deferred_prepared_call;
		state->poll_args = deferred_args;
		state->polling = deferred_polling;
		state->token = p_token;
		tool_thread_state = state;
		tool_thread.start(&SolersAgentSession::_tool_thread_func, state);
		if (!tool_thread.is_started()) {
			_tool_thread_func(state);
		}
		return;
	}
	_begin_main_thread_tool_audit();
	deferred_result = deferred_polling ? tool_registry->_poll_prepared_tool(*deferred_prepared_call, deferred_args) : tool_registry->_execute_prepared_tool(*deferred_prepared_call);
	_end_main_thread_tool_audit();
	deferred_done = true;
	SOLERS_TRACE("session.exec_tool", vformat("END %s ok=%d", deferred_canonical_name, (int)(bool)deferred_result.get("ok", false)));
}

void SolersAgentSession::_tool_thread_func(void *p_userdata) {
	ToolThreadState *state = static_cast<ToolThreadState *>(p_userdata);
	state->worker_thread_id = Thread::get_caller_id();
	if (state->session) {
		state->session->_register_worker_tool_audit(state->worker_thread_id, state->call.context.call_id, state->call.name, state->session->deferred_resource_accesses);
	}
	state->result = state->polling ? state->registry->_poll_prepared_tool(state->call, state->poll_args) : state->registry->_execute_prepared_tool(state->call);
	state->done.set();
}

bool SolersAgentSession::_collect_tool_thread_result(bool p_wait) {
	ToolThreadState *state = tool_thread_state;
	if (!state || (!p_wait && !state->done.is_set())) {
		return false;
	}
	if (tool_thread.is_started()) {
		tool_thread.wait_to_finish();
	}
	if (CallQueue *queue = MessageQueue::get_main_singleton(); queue && queue->flush() == ERR_BUSY && !p_wait) {
		return false;
	}
	if (state->token == tool_exec_token && running && phase == PHASE_TOOL_EXECUTING) {
		deferred_result = state->result;
		deferred_done = true;
		SOLERS_TRACE("session.exec_tool", vformat("END %s ok=%d", deferred_canonical_name, (int)(bool)deferred_result.get("ok", false)));
	}
	{
		MutexLock lock(godot_log_mutex);
		worker_tool_audits.erase(state->worker_thread_id);
	}
	memdelete(state);
	tool_thread_state = nullptr;
	return true;
}

void SolersAgentSession::_poll_tool_executing() {
	if (tool_exec_requested) {
		if (deferred_polling && deferred_prepared_call) {
			_begin_main_thread_tool_audit();
			const bool ready = tool_registry->_is_prepared_tool_ready(*deferred_prepared_call, deferred_args);
			_end_main_thread_tool_audit();
			if (!ready) {
				return;
			}
		}
		tool_exec_requested = false;
		_execute_deferred_tool(tool_exec_token);
	}
	_collect_tool_thread_result(false);
	if (!deferred_done) {
		return;
	}

	Dictionary result = deferred_result;
	const String id = deferred_call_id;
	const String model_name = deferred_model_name;
	const String canonical_name = deferred_canonical_name;
	const Dictionary args = deferred_initial_args;
	const bool is_resume = deferred_is_resume;

	deferred_done = false;
	deferred_result = Dictionary();
	_consume_attributable_tool_error(id);

	if (!is_resume && permission_manager && _is_awaiting_approval_result(result)) {
		const Dictionary error = result.get("error", Dictionary());
		awaiting_call = Dictionary();
		awaiting_call["id"] = id;
		awaiting_call["name"] = model_name;
		awaiting_call["canonical_name"] = canonical_name;
		awaiting_call["parsed_args"] = args;
		awaiting_call["queue_index"] = deferred_queue_index;
		awaiting_call["started_msec"] = (int64_t)tool_started_msec;
		awaiting_approval_id = (int)error.get("approval_id", 0);
		phase = PHASE_AWAITING_APPROVAL;
		SOLERS_TRACE("session.await_approval", vformat("%s approval_id=%d", canonical_name, awaiting_approval_id));
		emit_signal(SNAME("tool_call_awaiting_approval"), id, canonical_name);
		return;
	}

	if (is_resume && _is_awaiting_approval_result(result)) {
		result = _tool_denied_result("APPROVAL_EXPIRED", "The approval expired before the tool could run. Ask again to retry.");
	}

	const Dictionary pending_data = result.get("data", Dictionary());
	const Variant poll_args = pending_data.get("poll_args", Variant());
	if ((bool)result.get("ok", false) && String(pending_data.get("status", String())) == "pending" && poll_args.get_type() == Variant::DICTIONARY) {
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		// Pending tools are re-polled several times per second; log progress at
		// most once per second per call so the transcript stays readable.
		if (last_progress_call_id != id || now - last_progress_msec >= 1000) {
			last_progress_call_id = id;
			last_progress_msec = now;
			Dictionary progress;
			progress["call_id"] = id;
			progress["tool"] = canonical_name;
			progress["status"] = "pending";
			progress["data"] = pending_data;
			_write_transcript_event("tool_progress", progress);
			emit_signal(SNAME("tool_call_updated"), id, canonical_name, JSON::stringify(pending_data, "", false, true));
		}
		_park_pending_tool(Dictionary(poll_args));
		return;
	}
	const bool tool_succeeded = (bool)result.get("ok", false);
	Dictionary data = result.get("data", Dictionary());
	bool scene_state_changed = false;
	bool spatial_geometry_changed = false;
	if (tool_registry && tool_registry->reflection_service && tool_registry->affects_scene_state(StringName(canonical_name))) {
		const uint64_t scene_digest_after = tool_registry->reflection_service->get_scene_state_digest();
		const uint64_t geometry_digest_after = tool_registry->reflection_service->get_spatial_geometry_digest();
		scene_state_changed = scene_digest_after != deferred_scene_digest_before || (bool)data.get("scene_state_changed", false);
		spatial_geometry_changed = geometry_digest_after != deferred_geometry_digest_before || (bool)data.get("spatial_geometry_changed", false);
		data["scene_state_changed"] = scene_state_changed;
		data["spatial_geometry_changed"] = spatial_geometry_changed;
		result["data"] = data;
	}
	if (tool_succeeded) {
		if (tool_registry && !tool_registry->is_read_only(StringName(canonical_name), args)) {
			if (tool_registry->affects_scene_state(StringName(canonical_name))) {
				if (scene_state_changed) {
					authored_revision++;
					scene_revision = authored_revision;
				}
				if (spatial_geometry_changed) {
					geometry_revision = authored_revision;
					render_artifacts.clear();
					if ((bool)data.get("preserves_structure_validation", false) && scene_validation_revision > 0) {
						scene_validation_revision = geometry_revision;
					}
				} else if ((bool)data.get("mesh_data_changed", false)) {
					render_artifacts.erase("lightmap");
				}
			} else if (tool_registry->affects_authored_state(StringName(canonical_name)) && (bool)data.get("authored_state_changed", true)) {
				authored_revision++;
			}
		}
		const Dictionary artifact = data.get("artifact", Dictionary());
		const String artifact_kind = artifact.get("kind", String());
		if (!artifact_kind.is_empty()) {
			render_artifacts[artifact_kind] = artifact;
		}
		if (tool_registry && tool_registry->produces_scene_validation(StringName(canonical_name))) {
			scene_validation_revision = geometry_revision;
		}
		if (canonical_name == "editor.get_snapshot") {
			observed_revision = authored_revision;
		} else if (canonical_name == "viewport.capture") {
			const String target = data.get("target", String());
			if (target == "runtime") {
				runtime_capture_revision = authored_revision;
			} else if ((bool)data.get("material_preview", true)) {
				if (target == "camera") {
					camera_capture_revision = authored_revision;
				}
				observed_revision = authored_revision;
				if ((bool)data.get("frame_valid", true)) {
					editor_capture_revision = authored_revision;
				}
			}
		} else if (canonical_name == "runtime.control") {
			if ((bool)data.get("started_by_call", false) || (bool)data.get("stopped_by_call", false)) {
				runtime_epoch++;
			}
		}
	}
	if (deferred_prepared_call) {
		tool_registry->_complete_prepared_tool(*deferred_prepared_call, result);
		memdelete(deferred_prepared_call);
		deferred_prepared_call = nullptr;
	}
	deferred_polling = false;
	// The read cache exists to dedupe repeated identical reads at one state
	// authored revision. Two result shapes are excluded because they are not pure
	// functions of that revision: visual evidence (attachments; live runtime
	// frames and the user-driven editor camera change without a revision
	// bump), and in-flight pending envelopes (their capture_id is consumed by
	// the poll loop).
	const bool cacheable = canonical_name != "viewport.capture" && !result.has("attachments") && !Dictionary(result.get("data", Dictionary())).has("poll_args");
	if (tool_succeeded && cacheable && tool_registry && tool_registry->is_read_only(StringName(canonical_name), args)) {
		readonly_cache[_readonly_cache_key(StringName(canonical_name), args)] = result.duplicate(true);
	}

	const int queue_index = deferred_queue_index;
	const uint64_t started_msec = tool_started_msec;
	deferred_queue_index = -1;
	phase = PHASE_TOOLS;
	_queue_tool_result(queue_index, id, model_name, canonical_name, args, result, started_msec);
}

bool SolersAgentSession::_is_awaiting_approval_result(const Dictionary &p_result) const {
	if ((bool)p_result.get("ok", false)) {
		return false;
	}
	const Dictionary error = p_result.get("error", Dictionary());
	return String(error.get("code", String())) == "USER_APPROVAL_REQUIRED";
}

Dictionary SolersAgentSession::_tool_denied_result(const String &p_code, const String &p_message) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = true;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

void SolersAgentSession::_deliver_tool_result(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result) {
	Dictionary result = p_result.duplicate(true);
	result["call_id"] = p_id;
	if ((bool)result.get("ok", false) && p_canonical_name == "tool.search") {
		Dictionary data = result.get("data", Dictionary());
		Array task_tools;
		const Array tools = data.get("tools", Array());
		for (int i = 0; i < tools.size(); i++) {
			const StringName name = StringName(Dictionary(tools[i]).get("name", String()));
			if (name != StringName()) {
				task_deferred_tools.insert(name);
				task_tools.push_back(String(name));
			}
		}
		data["task_tools"] = task_tools;
		result["data"] = data;
	}
	const Array accesses = tool_registry && !p_canonical_name.is_empty() ? tool_registry->resolve_resource_access(StringName(p_canonical_name), p_args) : Array();
	const Dictionary diagnostics = _take_godot_diagnostics();
	if (!diagnostics.is_empty()) {
		result["diagnostics"] = diagnostics;
	}
	_write_transcript_tool(p_id, p_canonical_name, p_args, result);
	const uint64_t duration_msec = tool_completed_msec >= tool_started_msec ? tool_completed_msec - tool_started_msec : 0;
	if (!_is_session_tool(p_canonical_name)) {
		emit_signal(SNAME("tool_call_finished"), p_id, p_canonical_name, result, (int64_t)duration_msec);
	}

	// Runtime ownership follows the authoritative post-state so turn cleanup
	// stops only the game instance started by this session.
	const Dictionary data = result.get("data", Dictionary());
	if (p_canonical_name == "job.wait" && (bool)result.get("ok", false)) {
		waiting_background_asset_ids.clear();
		const Array pending_ids = data.get("pending_ids", Array());
		for (int i = 0; i < pending_ids.size(); i++) {
			waiting_background_asset_ids.insert(String(pending_ids[i]));
		}
		const Array terminal = data.get("terminal", Array());
		for (int terminal_index = 0; terminal_index < terminal.size(); terminal_index++) {
			const String asset_id = String(Dictionary(terminal[terminal_index]).get("id", String()));
			if (asset_id.is_empty()) {
				continue;
			}
			delivered_background_assets.insert(asset_id);
			if (tool_registry && tool_registry->asset_service) {
				tool_registry->asset_service->mark_terminal_delivered(asset_id, session_id);
			}
			for (int pending_index = pending_background_assets.size() - 1; pending_index >= 0; pending_index--) {
				if (String(Dictionary(pending_background_assets[pending_index]).get("asset_id", String())) == asset_id) {
					pending_background_assets.remove_at(pending_index);
				}
			}
			Dictionary consumed;
			consumed["asset_id"] = asset_id;
			_write_transcript_event("background_asset_consumed", consumed);
		}
	}
	if ((bool)data.get("started_by_call", false)) {
		turn_runtime_owned = true;
	}
	if ((bool)data.get("stopped_by_call", false)) {
		turn_runtime_owned = false;
	}

	String content = JSON::stringify(result, "", false, true);
	if (context_window > 0 && content.utf8().length() > context_window) {
		const int original_len = content.length();
		content = content.left(context_window / 2);
		content += vformat("\n...[Solers: tool result truncated, %d of %d characters kept. Re-run the tool with tighter limits for the full data.]", content.length(), original_len);
	}
	Dictionary message = SolersLLMMessage::tool_result(p_id, p_model_name, content, result.get("attachments", Array()));
	if (tool_registry && tool_registry->is_result_ephemeral(StringName(p_canonical_name))) {
		message["retention"] = "ephemeral";
	}
	messages.push_back(message);

	if (!(bool)result.get("ok", false)) {
		const Dictionary error = result.get("error", Dictionary());
		if (String(error.get("code", String())) != "SKIPPED_AFTER_FAILURE" && tool_registry) {
			for (int i = 0; i < accesses.size(); i++) {
				// Only failed writes fence later tools: a failed read (for
				// example a capture that timed out) leaves no partial state,
				// and its wildcard read key must not skip the whole queue.
				if (String(Dictionary(accesses[i]).get("mode", "write")) == "write") {
					failed_resource_accesses.push_back(accesses[i]);
				}
			}
		}
	}
}

void SolersAgentSession::_poll_awaiting_approval() {
	const String id = awaiting_call.get("id", String());
	const String model_name = awaiting_call.get("name", String());
	const String canonical_name = awaiting_call.get("canonical_name", model_name);
	const int queue_index = awaiting_call.get("queue_index", deferred_queue_index);
	const uint64_t started_msec = (uint64_t)(int64_t)awaiting_call.get("started_msec", tool_started_msec);

	const SolersPermissionManager::RequestDecision decision =
			permission_manager ? permission_manager->get_request_decision(awaiting_approval_id)
							   : SolersPermissionManager::DECISION_UNKNOWN;

	if (permission_manager && decision == SolersPermissionManager::DECISION_PENDING) {
		return; // still waiting on the user
	}

	if (permission_manager && decision == SolersPermissionManager::DECISION_APPROVED) {
		// Resume by re-issuing the same call through the scheduler so it runs
		// from the top of the next poll tick, never nested inside this phase
		// handler (the original crash path executed the approved tool
		// synchronously right here).
		Dictionary args = awaiting_call.get("parsed_args", Dictionary());
		args["approval_id"] = awaiting_approval_id;
		const int approval_id = awaiting_approval_id;
		awaiting_call.clear();
		awaiting_approval_id = 0;
		SOLERS_TRACE("session.approval_granted", vformat("%s approval_id=%d -> deferred execute", canonical_name, approval_id));
		_schedule_tool_execution(queue_index, id, model_name, canonical_name, args, true);
		return;
	}

	Dictionary result;
	if (!permission_manager) {
		result = _tool_denied_result("APPROVAL_UNAVAILABLE", "No permission manager is available to resolve the approval.");
	} else if (decision == SolersPermissionManager::DECISION_REJECTED) {
		result = _tool_denied_result("USER_REJECTED", "The user denied this tool call.");
	} else {
		result = _tool_denied_result("APPROVAL_EXPIRED", "The approval request is no longer available.");
	}

	const Dictionary args = awaiting_call.get("parsed_args", Dictionary());
	awaiting_call.clear();
	awaiting_approval_id = 0;
	deferred_queue_index = -1;
	phase = PHASE_TOOLS;
	_queue_tool_result(queue_index, id, model_name, canonical_name, args, result, started_msec);
}

void SolersAgentSession::abort() {
	if (!running) {
		return;
	}
	if (client) {
		client->abort();
	}
	if (tool_thread_state) {
		tool_cancel_requested.set();
		_collect_tool_thread_result(true);
	}
	if (deferred_prepared_call) {
		if (tool_registry) {
			const Dictionary cancelled = _tool_denied_result("TOOL_CANCELLED", "The turn was aborted while the tool was running.");
			tool_registry->_complete_prepared_tool(*deferred_prepared_call, cancelled);
		}
		memdelete(deferred_prepared_call);
		deferred_prepared_call = nullptr;
	}
	_clear_pending_tools(_tool_denied_result("TOOL_CANCELLED", "The turn was aborted while the tool was pending."));
	_finish_turn("aborted", "Turn aborted.");
}

void SolersAgentSession::shutdown() {
	abort();
	_release_godot_log_audit();
}

void SolersAgentSession::reset_conversation() {
	abort();
	_release_godot_log_audit();
	messages.clear();
	current_plan.clear();
	last_outcome = String();
	last_stop_reason = String();
	last_usage.clear();
	last_assistant_msec = 0;
	task_deferred_tools.clear();
	pending_background_assets.clear();
	delivered_background_assets.clear();
	waiting_background_asset_ids.clear();
	render_artifacts.clear();
	background_resume_suppressed = false;
	if (context_manager) {
		context_manager->reset();
	}
	session_id = _make_session_id();
	if (!project_path.is_empty()) {
		_ensure_godot_log_audit(false);
	}
}

void SolersAgentSession::set_project_path(const String &p_project_path) {
	project_path = p_project_path;
	if (!project_path.is_empty() && !godot_log_audit_installed) {
		_ensure_godot_log_audit(false);
	}
}

void SolersAgentSession::set_session(const String &p_project_path, const String &p_session_id) {
	abort();
	_release_godot_log_audit();
	project_path = p_project_path;
	render_artifacts.clear();
	task_deferred_tools.clear();
	if (!p_session_id.is_empty()) {
		session_id = p_session_id;
	}
	const Dictionary state = _read_transcript_state(project_path, session_id);
	messages = state.get("messages", Array());
	current_plan = state.get("plan", Dictionary());
	last_outcome = state.get("outcome", String());
	turn_id = state.get("turn_id", 0);
	pending_background_assets = state.get("background_assets", Array());
	background_resume_suppressed = false;
	delivered_background_assets.clear();
	waiting_background_asset_ids.clear();
	for (int i = 0; i < pending_background_assets.size(); i++) {
		delivered_background_assets.insert(Dictionary(pending_background_assets[i]).get("asset_id", String()));
	}
	if (context_manager) {
		context_manager->reset();
	}
	if (!project_path.is_empty()) {
		_ensure_godot_log_audit(false);
	}
}

bool SolersAgentSession::enqueue_background_asset(const Dictionary &p_manifest) {
	const String asset_id = String(p_manifest.get("id", String())).strip_edges();
	if (asset_id.is_empty() || delivered_background_assets.has(asset_id)) {
		return false;
	}
	const String asset_session_id = String(p_manifest.get("session_id", String())).strip_edges();
	if (asset_session_id.is_empty() || asset_session_id != session_id) {
		return false;
	}
	Dictionary delivery;
	delivery["asset_id"] = asset_id;
	delivery["status"] = p_manifest.get("status", String());
	delivery["provider"] = p_manifest.get("provider", String());
	const char *fields[] = { "kind", "name", "stage", "entrypoints", "dimensions", "preview_file", "error", "traits", "animations", "source_provider", "source_asset_id", "source_variant", "source_version", "license", "attribution", "parent_asset_id", "operation_id", "generation_mode", "provider_endpoint", "provider_request", "input_attachment_ids" };
	for (const char *field : fields) {
		if (p_manifest.has(field)) {
			delivery[field] = p_manifest[field];
		}
	}
	if (p_manifest.has("map_files")) {
		Array map_types = Dictionary(p_manifest["map_files"]).keys();
		map_types.sort();
		delivery["map_types"] = map_types;
	}
	_write_transcript_event("background_asset_delivery", delivery);
	delivered_background_assets.insert(asset_id);
	pending_background_assets.push_back(delivery);
	return true;
}

void SolersAgentSession::resume_background_assets() {
	_resume_next_background_asset();
}

bool SolersAgentSession::_append_background_asset_deltas(bool p_waited_only) {
	if (pending_background_assets.is_empty()) {
		return false;
	}
	Array deliveries;
	Array remaining;
	for (int i = 0; i < pending_background_assets.size(); i++) {
		const Dictionary delivery = pending_background_assets[i];
		const String asset_id = delivery.get("asset_id", String());
		if (p_waited_only && !waiting_background_asset_ids.has(asset_id)) {
			remaining.push_back(delivery);
			continue;
		}
		deliveries.push_back(delivery);
		waiting_background_asset_ids.erase(asset_id);
		Dictionary consumed;
		consumed["asset_id"] = asset_id;
		_write_transcript_event("background_asset_consumed", consumed);
	}
	if (deliveries.is_empty()) {
		return false;
	}
	pending_background_assets = remaining;
	Dictionary message = SolersLLMMessage::user("Background job terminal delta. Continue the original task using these persisted facts; query asset.status by id only if more detail is needed:\n" + JSON::stringify(deliveries, "", false, true));
	message["origin"] = "background_job_delta";
	messages.push_back(message);
	return true;
}

void SolersAgentSession::_resume_next_background_asset() {
	if (!is_waiting_for_background_assets() || background_resume_suppressed || !_append_background_asset_deltas(true)) {
		return;
	}
	_dispatch_model_request();
}

Array SolersAgentSession::get_messages() const {
	return messages.duplicate(true);
}

Dictionary SolersAgentSession::get_status() const {
	Dictionary status;
	status["running"] = running;
	status["turn_id"] = turn_id;
	status["message_count"] = messages.size();
	status["provider"] = active_provider.get("provider", String());
	status["model"] = active_provider.get("model", String());
	status["model_request_budget"] = model_request_budget;
	status["context_window"] = context_window;
	status["max_output_tokens"] = max_output_tokens;
	status["model_limits_known"] = context_window > 0;
	status["project_path"] = project_path;
	status["session_id"] = session_id;
	status["compacting"] = phase == PHASE_COMPACTING;
	status["plan"] = current_plan.duplicate(true);
	status["last_outcome"] = last_outcome;
	status["pending_background_asset_count"] = pending_background_assets.size();
	{
		MutexLock lock(godot_log_mutex);
		status["godot_log_errors"] = godot_log_error_count;
		status["godot_log_warnings"] = godot_log_warning_count;
	}
	status["authored_revision"] = (int64_t)authored_revision;
	status["runtime_epoch"] = (int64_t)runtime_epoch;
	status["observed_revision"] = (int64_t)observed_revision;
	status["editor_capture_revision"] = (int64_t)editor_capture_revision;
	status["scene_validation_revision"] = (int64_t)scene_validation_revision;
	status["geometry_revision"] = (int64_t)geometry_revision;
	if (context_manager) {
		status["context_tokens"] = context_manager->get_last_estimated_tokens();
		status["micro_compaction_count"] = context_manager->get_micro_compaction_count();
		status["compaction_count"] = context_manager->get_compaction_count();
	}
	return status;
}

bool SolersAgentSession::is_admitting_tool_calls() const {
	if (!running) {
		return false;
	}
	if (phase == PHASE_TOOL_EXECUTING) {
		return !deferred_polling;
	}
	if (phase != PHASE_TOOLS || tool_queue_index >= tool_queue.size()) {
		return false;
	}
	const Dictionary call = tool_queue[tool_queue_index];
	const String canonical_name = call.get("canonical_name", call.get("name", String()));
	Ref<JSON> json;
	json.instantiate();
	const String arguments = call.get("arguments", "{}");
	if (canonical_name.is_empty() || json->parse(arguments.is_empty() ? "{}" : arguments) != OK || json->get_data().get_type() != Variant::DICTIONARY) {
		return true;
	}
	const Dictionary args = json->get_data();
	const Array accesses = tool_registry ? tool_registry->resolve_resource_access(StringName(canonical_name), args) : Array();
	return !_conflicts_with_pending(accesses);
}

Array SolersAgentSession::list_model_providers() {
	if (models_dev) {
		models_dev->refresh();
	}
	return models_dev ? models_dev->list_providers() : Array();
}

SolersAgentSession::SolersAgentSession() {
	session_id = _make_session_id();
	protocol_registry = memnew(SolersLLMProtocolRegistry);
	protocol_registry->register_builtin_protocols();
	client = memnew(SolersLLMClient);
	client->set_protocol_registry(protocol_registry);
	context_manager = memnew(SolersContextManager);
	models_dev = memnew(SolersModelsDev);
	models_dev->initialize();
}

SolersAgentSession::~SolersAgentSession() {
	shutdown();
	if (tool_thread_state) {
		tool_cancel_requested.set();
		_collect_tool_thread_result(true);
	}
	if (deferred_prepared_call) {
		memdelete(deferred_prepared_call);
		deferred_prepared_call = nullptr;
	}
	if (context_manager) {
		memdelete(context_manager);
		context_manager = nullptr;
	}
	if (client) {
		memdelete(client);
		client = nullptr;
	}
	if (models_dev) {
		memdelete(models_dev);
		models_dev = nullptr;
	}
	if (protocol_registry) {
		memdelete(protocol_registry);
		protocol_registry = nullptr;
	}
}
