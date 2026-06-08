/**************************************************************************/
/*  solers_agent_runtime.cpp                                              */
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

#include "solers_agent_runtime.h"

#include "core/object/class_db.h"
#include "core/os/time.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

void SolersAgentRuntime::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tool_registry", "tool_registry"), &SolersAgentRuntime::set_tool_registry);
	ClassDB::bind_method(D_METHOD("set_observation_service", "observation_service"), &SolersAgentRuntime::set_observation_service);
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersAgentRuntime::set_action_timeline);
	ClassDB::bind_method(D_METHOD("start_turn", "args"), &SolersAgentRuntime::start_turn);
	ClassDB::bind_method(D_METHOD("run_tool_batch", "tool_calls"), &SolersAgentRuntime::run_tool_batch);
	ClassDB::bind_method(D_METHOD("abort_current_turn"), &SolersAgentRuntime::abort_current_turn);
	ClassDB::bind_method(D_METHOD("get_status"), &SolersAgentRuntime::get_status);

	BIND_ENUM_CONSTANT(STATE_IDLE);
	BIND_ENUM_CONSTANT(STATE_RUNNING);
	BIND_ENUM_CONSTANT(STATE_WAITING_FOR_APPROVAL);
	BIND_ENUM_CONSTANT(STATE_COMPLETED);
	BIND_ENUM_CONSTANT(STATE_FAILED);
	BIND_ENUM_CONSTANT(STATE_ABORTED);
}

String SolersAgentRuntime::_state_to_string(AgentState p_state) const {
	switch (p_state) {
		case STATE_IDLE:
			return "idle";
		case STATE_RUNNING:
			return "running";
		case STATE_WAITING_FOR_APPROVAL:
			return "waiting_for_approval";
		case STATE_COMPLETED:
			return "completed";
		case STATE_FAILED:
			return "failed";
		case STATE_ABORTED:
			return "aborted";
	}
	return "unknown";
}

Dictionary SolersAgentRuntime::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersAgentRuntime::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

void SolersAgentRuntime::set_tool_registry(SolersToolRegistry *p_tool_registry) {
	tool_registry = p_tool_registry;
}

void SolersAgentRuntime::set_observation_service(SolersObservationService *p_observation_service) {
	observation_service = p_observation_service;
}

void SolersAgentRuntime::set_action_timeline(SolersActionTimeline *p_action_timeline) {
	action_timeline = p_action_timeline;
}

Dictionary SolersAgentRuntime::start_turn(const Dictionary &p_args) {
	if (state == STATE_RUNNING) {
		return _error("AGENT_BUSY", "A Solers agent turn is already running.");
	}

	active_turn_id = next_turn_id++;
	abort_requested = false;
	state = STATE_RUNNING;

	Dictionary data;
	data["turn_id"] = active_turn_id;
	data["state"] = _state_to_string(state);
	data["objective"] = p_args.get("objective", String());
	data["mode"] = p_args.get("mode", "tool_batch");
	if (observation_service) {
		const int max_depth = p_args.get("max_scene_depth", 2);
		const int max_children = p_args.get("max_children_per_node", 16);
		data["initial_snapshot"] = observation_service->get_editor_snapshot(max_depth, max_children);
	}

	if (action_timeline) {
		action_timeline->record_event("agent_turn_started", data);
	}

	Array tool_calls = p_args.get("tool_calls", Array());
	if (tool_calls.is_empty()) {
		state = STATE_COMPLETED;
		data["state"] = _state_to_string(state);
		last_result = _ok(data);
		if (action_timeline) {
			action_timeline->record_event("agent_turn_completed", data);
		}
		return last_result;
	}

	return run_tool_batch(tool_calls);
}

Dictionary SolersAgentRuntime::run_tool_batch(const Array &p_tool_calls) {
	if (!tool_registry) {
		state = STATE_FAILED;
		return _error("TOOL_REGISTRY_UNAVAILABLE", "SolersToolRegistry is not initialized.", false);
	}
	if (state != STATE_RUNNING) {
		state = STATE_RUNNING;
		abort_requested = false;
		if (active_turn_id == 0) {
			active_turn_id = next_turn_id++;
		}
	}

	const uint64_t started_msec = Time::get_singleton()->get_ticks_msec();
	Array results;
	Dictionary batch_payload;
	batch_payload["turn_id"] = active_turn_id;
	batch_payload["count"] = p_tool_calls.size();
	if (action_timeline) {
		action_timeline->record_event("agent_tool_batch_started", batch_payload);
	}

	for (int i = 0; i < p_tool_calls.size(); i++) {
		if (abort_requested) {
			state = STATE_ABORTED;
			break;
		}

		const Variant call_variant = p_tool_calls[i];
		if (call_variant.get_type() != Variant::DICTIONARY) {
			Dictionary invalid;
			invalid["index"] = i;
			invalid["result"] = _error("INVALID_TOOL_CALL", "Tool call must be a Dictionary.");
			results.push_back(invalid);
			state = STATE_FAILED;
			break;
		}

		Dictionary call = call_variant;
		const StringName tool_name = StringName(call.get("name", String()));
		const Dictionary args = call.get("arguments", call.get("args", Dictionary()));

		Dictionary item;
		item["index"] = i;
		item["name"] = tool_name;
		item["arguments"] = args;
		Dictionary result = tool_registry->call_tool(tool_name, args);
		item["result"] = result;
		results.push_back(item);

		if (!(bool)result.get("ok", false)) {
			Dictionary error = result.get("error", Dictionary());
			const String code = error.get("code", String());
			if (code == "USER_APPROVAL_REQUIRED") {
				state = STATE_WAITING_FOR_APPROVAL;
			} else {
				state = STATE_FAILED;
			}
			break;
		}
	}

	if (state == STATE_RUNNING) {
		state = abort_requested ? STATE_ABORTED : STATE_COMPLETED;
	}

	Dictionary data;
	data["turn_id"] = active_turn_id;
	data["state"] = _state_to_string(state);
	data["results"] = results;
	data["duration_msec"] = (int)(Time::get_singleton()->get_ticks_msec() - started_msec);

	if (action_timeline) {
		action_timeline->record_event(state == STATE_COMPLETED ? "agent_turn_completed" : "agent_turn_interrupted", data);
	}

	last_result = _ok(data);
	return last_result;
}

void SolersAgentRuntime::abort_current_turn() {
	abort_requested = true;
	if (state == STATE_RUNNING || state == STATE_WAITING_FOR_APPROVAL) {
		state = STATE_ABORTED;
	}
	Dictionary data;
	data["turn_id"] = active_turn_id;
	data["state"] = _state_to_string(state);
	if (action_timeline) {
		action_timeline->record_event("agent_turn_aborted", data);
	}
}

Dictionary SolersAgentRuntime::get_status() const {
	Dictionary data;
	data["state"] = _state_to_string(state);
	data["turn_id"] = active_turn_id;
	data["next_turn_id"] = next_turn_id;
	data["abort_requested"] = abort_requested;
	data["last_result"] = last_result;
	return data;
}
