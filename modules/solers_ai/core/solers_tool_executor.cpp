/**************************************************************************/
/*  solers_tool_executor.cpp                                              */
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

#include "solers_tool_executor.h"

#include "core/os/os.h"

#include "modules/solers_ai/core/solers_permission_manager.h"

Dictionary SolersToolExecutor::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	if (!context.call_id.is_empty()) {
		result["call_id"] = context.call_id;
	}
	return result;
}

void SolersToolExecutor::configure(SolersToolRegistry *p_registry, SolersPermissionManager *p_permission_manager) {
	ERR_FAIL_COND_MSG(!is_idle(), "Cannot reconfigure a Solers tool executor while a call is active.");
	registry = p_registry;
	permission_manager = p_permission_manager;
}

bool SolersToolExecutor::_prepare(const Dictionary &p_arguments) {
	if (!registry) {
		_finish(_error("AGENT_UNCONFIGURED", "SolersToolRegistry is unavailable.", false));
		return false;
	}
	SolersPreparedToolCall call;
	const Dictionary preparation_error = registry->prepare_call(tool_name, p_arguments, context, call);
	if (!preparation_error.is_empty()) {
		_finish(preparation_error);
		return false;
	}
	prepared_call = call;
	has_prepared_call = true;
	state = STATE_EXECUTING;
	return true;
}

void SolersToolExecutor::_finish(const Dictionary &p_result) {
	terminal_result = p_result.duplicate(true);
	if (has_prepared_call && registry) {
		registry->complete_call(prepared_call, terminal_result);
	}
	has_prepared_call = false;
	prepared_call = SolersPreparedToolCall();
	continuation_arguments.clear();
	if (permission_manager && approval_id > 0) {
		permission_manager->cancel_request(approval_id);
	}
	approval_id = 0;
	state = STATE_TERMINAL;
}

void SolersToolExecutor::_accept_result(const Dictionary &p_result) {
	const bool ok = p_result.get("ok", false);
	const Dictionary error = p_result.get("error", Dictionary());
	if (!ok && String(error.get("code", String())) == "USER_APPROVAL_REQUIRED") {
		approval_id = error.get("approval_id", 0);
		if (approval_id <= 0) {
			_finish(_error("APPROVAL_REQUEST_INVALID", "The permission gate returned no approval request id.", false));
			return;
		}
		has_prepared_call = false;
		prepared_call = SolersPreparedToolCall();
		state = STATE_AWAITING_APPROVAL;
		return;
	}

	const Variant data_value = p_result.get("data", Variant());
	if (ok && data_value.get_type() == Variant::DICTIONARY && String(Dictionary(data_value).get("status", String())) == "pending") {
		const Variant poll_args = Dictionary(data_value).get("poll_args", Variant());
		if (poll_args.get_type() != Variant::DICTIONARY) {
			_finish(_error("TOOL_CONTINUATION_INVALID", "The tool returned pending without structured poll_args.", false));
			return;
		}
		continuation_arguments = poll_args;
		state = STATE_CONTINUING;
		return;
	}
	_finish(p_result);
}

Dictionary SolersToolExecutor::start(const StringName &p_name, const Dictionary &p_arguments, const SolersToolContext &p_context, uint64_t p_timeout_msec) {
	if (!is_idle()) {
		return _error("TOOL_EXECUTOR_BUSY", "The serial tool executor already owns a call.", false);
	}
	tool_name = p_name;
	arguments = p_arguments.duplicate(true);
	context = p_context;
	context.cancel_requested = &cancel_requested;
	cancel_requested.clear();
	terminal_result.clear();
	continuation_arguments.clear();
	approval_id = 0;
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	deadline_msec = now + MAX((uint64_t)1, p_timeout_msec);
	state = STATE_EXECUTING;
	_prepare(arguments);
	return Dictionary();
}

void SolersToolExecutor::poll() {
	if (!is_active()) {
		return;
	}
	if (OS::get_singleton()->get_ticks_msec() >= deadline_msec) {
		cancel("TOOL_TIMEOUT", "The tool did not reach a terminal state before its deadline.");
		return;
	}
	if (state == STATE_AWAITING_APPROVAL) {
		const SolersPermissionManager::RequestDecision decision = permission_manager ? permission_manager->get_request_decision(approval_id) : SolersPermissionManager::DECISION_UNKNOWN;
		if (decision == SolersPermissionManager::DECISION_PENDING) {
			return;
		}
		if (decision != SolersPermissionManager::DECISION_APPROVED) {
			const String code = !permission_manager ? "APPROVAL_UNAVAILABLE" : decision == SolersPermissionManager::DECISION_REJECTED ? "USER_REJECTED"
																																	  : "APPROVAL_EXPIRED";
			const String message = !permission_manager ? "No permission manager is available to resolve the approval." : decision == SolersPermissionManager::DECISION_REJECTED ? "The user denied this tool call."
																																												: "The approval request is no longer available.";
			_finish(_error(code, message));
			return;
		}
		Dictionary approved_arguments = arguments.duplicate(true);
		approved_arguments["approval_id"] = approval_id;
		if (!_prepare(approved_arguments)) {
			return;
		}
	}

	if (state == STATE_CONTINUING) {
		if (!has_prepared_call || !registry) {
			_finish(_error("TOOL_CONTINUATION_INVALID", "The prepared tool call was lost before completion.", false));
			return;
		}
		if (!registry->is_call_ready(prepared_call, continuation_arguments)) {
			return;
		}
		_accept_result(registry->poll_call(prepared_call, continuation_arguments));
		return;
	}
	if (state == STATE_EXECUTING) {
		if (!has_prepared_call || !registry) {
			_finish(_error("TOOL_EXECUTION_INVALID", "The prepared tool call is unavailable.", false));
			return;
		}
		_accept_result(registry->execute_call(prepared_call));
	}
}

void SolersToolExecutor::cancel(const String &p_code, const String &p_message) {
	if (!is_active()) {
		return;
	}
	cancel_requested.set();
	if (state == STATE_AWAITING_APPROVAL && permission_manager && approval_id > 0) {
		permission_manager->cancel_request(approval_id);
	}
	_finish(_error(p_code, p_message));
}

Dictionary SolersToolExecutor::take_result() {
	if (!is_terminal()) {
		return Dictionary();
	}
	const Dictionary result = terminal_result.duplicate(true);
	_reset();
	return result;
}

void SolersToolExecutor::_reset() {
	state = STATE_IDLE;
	tool_name = StringName();
	arguments.clear();
	continuation_arguments.clear();
	context = SolersToolContext();
	prepared_call = SolersPreparedToolCall();
	has_prepared_call = false;
	approval_id = 0;
	deadline_msec = 0;
	cancel_requested.clear();
	terminal_result.clear();
}

SolersToolExecutor::~SolersToolExecutor() {
	cancel();
}
