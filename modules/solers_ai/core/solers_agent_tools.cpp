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

#include "core/io/json.h"
#include "core/os/os.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_tool_executor.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

void SolersAgentSession::_poll_tool_queue() {
	if (!active_tool_call_id.is_empty()) {
		_poll_tool_executing();
		return;
	}
	if (tool_executor && !tool_executor->is_idle()) {
		return;
	}
	if (tool_queue_index >= tool_queue.size()) {
		tool_queue.clear();
		tool_queue_index = 0;
		retry_attempt = 0;
		const Error error = _dispatch_model_request();
		if (error != OK) {
			current_reasoning = String();
		}
		return;
	}

	Dictionary call = tool_queue[tool_queue_index];
	const String model_name = call.get("name", String());
	const String canonical_name = call.get("canonical_name", model_name);
	const String requested_name = call.get("requested_name", model_name);
	const String call_id = call.get("id", String());
	const String arguments = call.get("arguments", "{}");
	if (!(bool)call.get("ui_announced", false)) {
		emit_signal(SNAME("tool_call_started"), call_id, canonical_name.is_empty() ? requested_name : canonical_name, arguments);
		call["ui_announced"] = true;
		tool_queue[tool_queue_index] = call;
	}

	Dictionary audit;
	audit["call_id"] = call_id;
	audit["tool"] = canonical_name.is_empty() ? requested_name : canonical_name;
	audit["arguments_sha256"] = arguments.sha256_text();
	audit["arguments_bytes"] = arguments.utf8().length();
	_write_transcript_event("tool_started", audit);

	const Dictionary preflight_result = call.get("preflight_result", Dictionary());
	if (!preflight_result.is_empty()) {
		tool_started_msec = OS::get_singleton()->get_ticks_msec();
		tool_completed_msec = tool_started_msec;
		_deliver_tool_result(call_id, model_name, canonical_name.is_empty() ? requested_name : canonical_name,
				call.get("parsed_args", Dictionary()), preflight_result, tool_started_msec);
		tool_queue_index++;
		return;
	}
	_start_tool_execution(call, call.get("parsed_args", Dictionary()));
}

void SolersAgentSession::_start_tool_execution(const Dictionary &p_call, const Dictionary &p_args) {
	ERR_FAIL_NULL(tool_executor);
	ERR_FAIL_COND(!tool_executor->is_idle());
	active_tool_call_id = p_call.get("id", String());
	active_tool_model_name = p_call.get("name", String());
	active_tool_canonical_name = p_call.get("canonical_name", active_tool_model_name);
	active_tool_args = p_args.duplicate(true);
	tool_queued_msec = (uint64_t)(int64_t)p_call.get("queued_msec", (int64_t)OS::get_singleton()->get_ticks_msec());
	tool_started_msec = OS::get_singleton()->get_ticks_msec();
	tool_completed_msec = 0;
	approval_announced = false;

	SolersToolContext context;
	context.call_id = active_tool_call_id;
	context.session_id = session_id;
	context.project_path = project_path;
	context.mentions = turn_mentions;
	if (tool_queue_index >= 0 && tool_queue_index < tool_queue.size()) {
		context.result_token_budget = Dictionary(tool_queue[tool_queue_index]).get("result_token_budget", SolersContextManager::TOOL_RESULT_MAX_TOKENS);
	}
	const int requested_timeout = (int)active_tool_args.get("timeout_msec", (int)TOOL_EXECUTION_TIMEOUT_MSEC);
	const uint64_t timeout_msec = MIN((uint64_t)MAX(requested_timeout, 1), TOOL_EXECUTION_TIMEOUT_MSEC);
	const Dictionary start_error = tool_executor->start(StringName(active_tool_canonical_name), active_tool_args, context, timeout_msec);
	if (!start_error.is_empty()) {
		_finish_active_tool(start_error);
	} else if (tool_executor->is_terminal()) {
		_finish_active_tool(tool_executor->take_result());
	}
}

void SolersAgentSession::_poll_tool_executing() {
	ERR_FAIL_NULL(tool_executor);
	if (tool_executor->get_call_id() != active_tool_call_id) {
		_finish_active_tool(_tool_denied_result("TOOL_EXECUTOR_OWNERSHIP_LOST", "The serial tool executor no longer owns this call."));
		return;
	}
	tool_executor->poll();
	if (tool_executor->is_awaiting_approval() && !approval_announced) {
		approval_announced = true;
		emit_signal(SNAME("tool_call_awaiting_approval"), active_tool_call_id, active_tool_canonical_name);
	}
	if (tool_executor->get_state() == SolersToolExecutor::STATE_CONTINUING) {
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		if (last_progress_call_id != active_tool_call_id || now - last_progress_msec >= 1000) {
			last_progress_call_id = active_tool_call_id;
			last_progress_msec = now;
			_write_transcript_event("tool_progress", Dictionary({ { "call_id", active_tool_call_id }, { "tool", active_tool_canonical_name }, { "status", "pending" } }));
		}
	}
	if (tool_executor->is_terminal()) {
		_finish_active_tool(tool_executor->take_result());
	}
}

void SolersAgentSession::_finish_active_tool(const Dictionary &p_result) {
	const String call_id = active_tool_call_id;
	const String model_name = active_tool_model_name;
	const String canonical_name = active_tool_canonical_name;
	const Dictionary args = active_tool_args;
	const uint64_t started_msec = tool_started_msec;
	tool_completed_msec = OS::get_singleton()->get_ticks_msec();
	active_tool_call_id = String();
	active_tool_model_name = String();
	active_tool_canonical_name = String();
	active_tool_args.clear();
	approval_announced = false;
	_deliver_tool_result(call_id, model_name, canonical_name, args, p_result, started_msec);
	tool_queue_index++;
}

Dictionary SolersAgentSession::_tool_denied_result(const String &p_code, const String &p_message) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = true;
	return Dictionary({ { "ok", false }, { "error", error } });
}

void SolersAgentSession::_deliver_tool_result(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, uint64_t p_started_msec) {
	Dictionary result = p_result.duplicate(true);
	result["call_id"] = p_id;
	const Dictionary diagnostics = _take_godot_diagnostics();
	if (!diagnostics.is_empty()) {
		result["diagnostics"] = diagnostics;
	}
	const String content = SolersContextManager::clamp_to_tokens(JSON::stringify(result, "", false, true), SolersContextManager::TOOL_RESULT_MAX_TOKENS);
	_write_transcript_tool(p_id, p_canonical_name, p_args, result, content);
	const uint64_t completed_msec = tool_completed_msec > 0 ? tool_completed_msec : OS::get_singleton()->get_ticks_msec();
	const uint64_t duration = completed_msec >= p_started_msec ? completed_msec - p_started_msec : 0;
	emit_signal(SNAME("tool_call_finished"), p_id, p_canonical_name, result, (int64_t)duration);

	messages.push_back(SolersLLMMessage::tool_result(p_id, p_model_name, content, result.get("attachments", Array())));
}

void SolersAgentSession::_cancel_undelivered_tools() {
	if (tool_queue_index >= tool_queue.size()) {
		return;
	}
	const Dictionary cancelled = _tool_denied_result("TOOL_CANCELLED", "The turn ended before this tool reached a terminal state.");
	if (tool_executor && tool_executor->is_active()) {
		tool_executor->cancel();
		if (tool_executor->is_terminal()) {
			tool_executor->take_result();
		}
	}
	for (int i = tool_queue_index; i < tool_queue.size(); i++) {
		const Dictionary call = tool_queue[i];
		tool_completed_msec = OS::get_singleton()->get_ticks_msec();
		_deliver_tool_result(call.get("id", String()), call.get("name", String()), call.get("canonical_name", call.get("name", String())), call.get("parsed_args", Dictionary()), cancelled, tool_started_msec > 0 ? tool_started_msec : tool_completed_msec);
	}
	tool_queue_index = tool_queue.size();
}
