/**************************************************************************/
/*  solers_agent_tools.cpp                                                */
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
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "scene/main/node.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_retry.h"
#include "modules/solers_ai/llm/solers_models_dev.h"

struct SolersAgentSession::ToolThreadState {
	SolersToolRegistry *registry = nullptr;
	SolersPreparedToolCall call;
	Dictionary poll_args;
	bool polling = false;
	Dictionary result;
	SafeFlag done;
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
	SolersPreparedToolCall *prepared_call = nullptr;
	bool is_resume = false;
	uint64_t started_msec = 0;
};

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
		tool_queue.clear();
		completed_tool_results.clear();
		tool_queue_index = 0;
		tool_delivery_index = 0;
		tool_started_announced = false;
		retry_attempt = 0;
		// job.wait recorded outstanding ids: park the host silently until a
		// project-import terminal delivery resumes this same turn. Without this
		// gate the model re-enters and busy-polls asset.status in the foreground.
		if (!waiting_background_asset_ids.is_empty()) {
			phase = PHASE_WAITING;
			Array pending_ids;
			for (const String &asset_id : waiting_background_asset_ids) {
				pending_ids.push_back(asset_id);
			}
			Dictionary parked;
			parked["pending_ids"] = pending_ids;
			_write_transcript_event("background_wait_parked", parked);
			emit_signal(SNAME("turn_waiting"), parked);
			return;
		}
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
	audit["args"] = tool_registry ? tool_registry->redact_tool_args_for_audit(StringName(canonical_name), parsed_args) : parsed_args;
	_write_transcript_event("tool_started", audit);
	if (tool_registry && SolersToolRegistry::has_write_conflict(failed_resource_accesses, accesses)) {
		const Dictionary skipped = _tool_denied_result("SKIPPED_AFTER_FAILURE", "Skipped because a failed prerequisite touched the same resource. Re-evaluate that resource before retrying.");
		_queue_tool_result(queue_index, id, name, canonical_name, parsed_args, skipped, tool_started_msec);
		tool_queue_index++;
		tool_started_announced = false;
		return;
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
		const Dictionary call = tool_queue[p_queue_index];
		queued_msec = (int64_t)call.get("queued_msec", queued_msec);
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
		tool_queued_msec = (uint64_t)(int64_t)entry.get("queued_msec", 0);
		tool_started_msec = (uint64_t)(int64_t)entry.get("started_msec", 0);
		tool_completed_msec = (uint64_t)(int64_t)entry.get("completed_msec", tool_started_msec);
		const String canonical_name = entry.get("canonical_name", String());
		const Dictionary result = entry.get("result", Dictionary());
		completed_tool_results.erase(tool_delivery_index);
		_deliver_tool_result(entry.get("id", String()), entry.get("model_name", String()), canonical_name, entry.get("args", Dictionary()), result);
		tool_delivery_index++;
	}
	return false;
}

// A parked execution never reaches its own terminal result, so the registry is
// told once, here, and releases whatever per-task state it was holding.
void SolersAgentSession::_clear_pending_tools() {
	const Dictionary cancelled = _tool_denied_result("TOOL_CANCELLED", "The turn ended while this tool was still parked.");
	for (int i = 0; i < pending_tool_executions.size(); i++) {
		PendingToolExecution *pending = pending_tool_executions[i];
		if (pending->prepared_call) {
			if (tool_registry) {
				tool_registry->_complete_prepared_tool(*pending->prepared_call, cancelled);
			}
			memdelete(pending->prepared_call);
		}
		memdelete(pending);
	}
	pending_tool_executions.clear();
}

// Once a tool_call is in history it must be answered, whatever ended the turn.
// Terminal paths deliver the results the queue never got around to: the real
// completion when one already arrived, a cancellation otherwise.
void SolersAgentSession::_cancel_undelivered_tools() {
	if (tool_delivery_index >= tool_queue.size()) {
		return;
	}
	const Dictionary cancelled = _tool_denied_result("TOOL_CANCELLED", "The turn ended before this tool produced a result.");
	for (int i = tool_delivery_index; i < tool_queue.size(); i++) {
		const Dictionary call = tool_queue[i];
		const Dictionary *completed = completed_tool_results.getptr(i);
		const Dictionary entry = completed ? *completed : Dictionary();
		tool_queued_msec = (uint64_t)(int64_t)entry.get("queued_msec", (int64_t)tool_queued_msec);
		tool_started_msec = (uint64_t)(int64_t)entry.get("started_msec", (int64_t)tool_started_msec);
		tool_completed_msec = (uint64_t)(int64_t)entry.get("completed_msec", (int64_t)OS::get_singleton()->get_ticks_msec());
		const String model_name = call.get("name", String());
		_deliver_tool_result(
				call.get("id", String()),
				model_name,
				call.get("canonical_name", model_name),
				call.get("parsed_args", Dictionary()),
				entry.has("result") ? Dictionary(entry.get("result", cancelled)) : cancelled);
	}
	tool_delivery_index = tool_queue.size();
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
	const Dictionary definition = tool_registry ? tool_registry->get_tool_definition(StringName(p_canonical_name)) : Dictionary();
	if (!definition.is_empty()) {
		Array attachments;
		for (const String &argument : PackedStringArray(definition.get("attachment_args", PackedStringArray()))) {
			attachments.append_array(_attachments_for_ids(deferred_args.get(argument, Array())));
		}
		if (!attachments.is_empty()) {
			deferred_args["_attachments"] = attachments;
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
	const Dictionary definition = tool_registry->get_tool_definition(StringName(deferred_canonical_name));
	if (!definition.is_empty()) {
		for (const String &input : PackedStringArray(definition.get("required_model_inputs", PackedStringArray()))) {
			if (_active_model_input_support(input) == 0) {
				deferred_result = _tool_denied_result("MODEL_INPUT_CAPABILITY_REQUIRED", vformat("The selected model does not support required '%s' tool input.", input));
				deferred_done = true;
				return;
			}
		}
	}
	SOLERS_TRACE("session.exec_tool", vformat("BEGIN %s", deferred_canonical_name));
	SolersToolContext context;
	context.call_id = deferred_call_id;
	context.session_id = session_id;
	context.project_path = project_path;
	context.mentions = turn_mentions;
	context.authored_revision = authored_revision;
	if (deferred_queue_index >= 0 && deferred_queue_index < tool_queue.size() && Dictionary(tool_queue[deferred_queue_index]).has("result_token_budget")) {
		context.result_token_budget = Dictionary(tool_queue[deferred_queue_index]).get("result_token_budget", 1);
	} else if (context_manager && context_window > 0) {
		context.result_token_budget = MAX(1, context_window - max_output_tokens - context_manager->get_token_count_with_pending(messages, system_prompt, cached_request_tool_tokens));
	}
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
	deferred_result = deferred_polling ? tool_registry->_poll_prepared_tool(*deferred_prepared_call, deferred_args) : tool_registry->_execute_prepared_tool(*deferred_prepared_call);
	deferred_done = true;
	SOLERS_TRACE("session.exec_tool", vformat("END %s ok=%d", deferred_canonical_name, (int)(bool)deferred_result.get("ok", false)));
}

void SolersAgentSession::_tool_thread_func(void *p_userdata) {
	ToolThreadState *state = static_cast<ToolThreadState *>(p_userdata);
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
	if (state->token == tool_exec_token && running && phase == PHASE_TOOL_EXECUTING) {
		deferred_result = state->result;
		if (deferred_prepared_call) {
			deferred_prepared_call->journal_event = state->call.journal_event;
		}
		deferred_done = true;
		SOLERS_TRACE("session.exec_tool", vformat("END %s ok=%d", deferred_canonical_name, (int)(bool)deferred_result.get("ok", false)));
	}
	memdelete(state);
	tool_thread_state = nullptr;
	return true;
}

void SolersAgentSession::_poll_tool_executing() {
	if (tool_exec_requested) {
		if (!deferred_prepared_call && tool_registry && !tool_registry->is_execution_ready(StringName(deferred_canonical_name), deferred_args)) {
			return;
		}
		if (deferred_polling && deferred_prepared_call) {
			const bool ready = tool_registry->_is_prepared_tool_ready(*deferred_prepared_call, deferred_args);
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
		}
		_park_pending_tool(Dictionary(poll_args));
		return;
	}
	if (deferred_prepared_call) {
		result = tool_registry->_finalize_prepared_result(*deferred_prepared_call, result);
	}
	const bool tool_succeeded = (bool)result.get("ok", false);
	Dictionary data = result.get("data", Dictionary());
	if (tool_succeeded) {
		if ((bool)data.get("authored_state_changed", false)) {
			authored_revision++;
			turn_successful_mutations++;
		}
	}
	if (deferred_prepared_call) {
		tool_registry->_complete_prepared_tool(*deferred_prepared_call, result);
		_write_prepared_journal_event(deferred_prepared_call);
		memdelete(deferred_prepared_call);
		deferred_prepared_call = nullptr;
	}
	deferred_polling = false;

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
	const Dictionary diagnostics = _take_godot_diagnostics();
	if (!diagnostics.is_empty()) {
		result["diagnostics"] = diagnostics;
	}
	const Array added_tool_names = (bool)result.get("ok", false) ? _activate_tools(result.get("added_tools", Array())) : Array();
	result.erase("added_tools");
	const String content = SolersContextManager::clamp_to_tokens(JSON::stringify(result, "", false, true), SolersContextManager::TOOL_RESULT_MAX_TOKENS);
	_write_transcript_tool(p_id, p_canonical_name, p_args, result, content, added_tool_names);
	const uint64_t duration_msec = tool_completed_msec >= tool_started_msec ? tool_completed_msec - tool_started_msec : 0;
	const Dictionary definition = tool_registry ? tool_registry->get_tool_definition(StringName(p_canonical_name)) : Dictionary();
	if ((bool)definition.get("timeline_visible", true)) {
		emit_signal(SNAME("tool_call_finished"), p_id, p_canonical_name, result, (int64_t)duration_msec);
	}

	// Runtime ownership follows the authoritative post-state so turn cleanup
	// stops only the game instance started by this session.
	const Dictionary data = result.get("data", Dictionary());
	const Dictionary background_jobs = data.get("background_jobs", Dictionary());
	if (!background_jobs.is_empty() && (bool)result.get("ok", false)) {
		waiting_background_asset_ids.clear();
		const Array pending_ids = background_jobs.get("pending_ids", Array());
		for (int i = 0; i < pending_ids.size(); i++) {
			waiting_background_asset_ids.insert(String(pending_ids[i]));
		}
		const Array terminal = background_jobs.get("terminal", Array());
		for (int terminal_index = 0; terminal_index < terminal.size(); terminal_index++) {
			const String asset_id = String(Dictionary(terminal[terminal_index]).get("id", String()));
			if (asset_id.is_empty()) {
				continue;
			}
			delivered_background_assets.insert(asset_id);
			if (background_asset_delivery_handler) {
				background_asset_delivery_handler(asset_id, session_id);
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

	Dictionary message = SolersLLMMessage::tool_result(p_id, p_model_name, content, result.get("attachments", Array()), added_tool_names);
	messages.push_back(message);

	if (!(bool)result.get("ok", false)) {
		const Dictionary error = result.get("error", Dictionary());
		if (String(error.get("code", String())) != "SKIPPED_AFTER_FAILURE" && tool_registry) {
			const Array accesses = tool_registry->resolve_resource_access(StringName(p_canonical_name), p_args);
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
