/**************************************************************************/
/*  solers_agent_orchestrator.cpp                                         */
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

#include "solers_agent_orchestrator.h"

#include "core/object/class_db.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_provider_gateway.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

void SolersAgentOrchestrator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_provider_gateway", "provider_gateway"), &SolersAgentOrchestrator::set_provider_gateway);
	ClassDB::bind_method(D_METHOD("set_tool_registry", "tool_registry"), &SolersAgentOrchestrator::set_tool_registry);
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersAgentOrchestrator::set_action_timeline);
	ClassDB::bind_method(D_METHOD("start_turn", "request"), &SolersAgentOrchestrator::start_turn);
}

Dictionary SolersAgentOrchestrator::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersAgentOrchestrator::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

void SolersAgentOrchestrator::_record_phase(const String &p_phase, const Dictionary &p_payload) const {
	if (!action_timeline) {
		return;
	}
	Dictionary payload = p_payload;
	payload["phase"] = p_phase;
	action_timeline->record_event("agent_phase_completed", payload);
}

void SolersAgentOrchestrator::set_provider_gateway(SolersProviderGateway *p_provider_gateway) {
	provider_gateway = p_provider_gateway;
}

void SolersAgentOrchestrator::set_tool_registry(SolersToolRegistry *p_tool_registry) {
	tool_registry = p_tool_registry;
}

void SolersAgentOrchestrator::set_action_timeline(SolersActionTimeline *p_action_timeline) {
	action_timeline = p_action_timeline;
}

Dictionary SolersAgentOrchestrator::start_turn(const Dictionary &p_request) {
	if (!provider_gateway) {
		return _error("PROVIDER_GATEWAY_UNAVAILABLE", "SolersProviderGateway is not initialized.", false);
	}

	Array phases;
	Dictionary phase_payload;
	phase_payload["objective"] = p_request.get("objective", String());

	phases.push_back("planner");
	Dictionary planner_result = provider_gateway->generate(p_request);
	phase_payload["planner_result"] = planner_result;
	_record_phase("planner", phase_payload);
	if (!(bool)planner_result.get("ok", false)) {
		Dictionary data;
		data["state"] = "failed";
		data["phases"] = phases;
		data["planner_result"] = planner_result;
		return _ok(data);
	}

	phases.push_back("executor");
	Dictionary executor_result;
	Dictionary planner_data = planner_result.get("data", Dictionary());
	Array tool_calls = planner_data.get("tool_calls", Array());
	executor_result["tool_registry_attached"] = tool_registry != nullptr;
	executor_result["tool_results"] = Array();
	if (!tool_calls.is_empty() && !tool_registry) {
		Dictionary error;
		error["code"] = "TOOL_REGISTRY_UNAVAILABLE";
		error["message"] = "Planner returned tool calls, but SolersToolRegistry is not initialized.";
		executor_result["error"] = error;
		_record_phase("executor", executor_result);

		Dictionary data;
		data["state"] = "failed";
		data["phases"] = phases;
		data["planner_result"] = planner_result;
		data["executor_result"] = executor_result;
		return _ok(data);
	}
	Array tool_results;
	for (int i = 0; i < tool_calls.size(); i++) {
		Dictionary tool_call = tool_calls[i];
		const StringName tool_name = StringName(tool_call.get("name", String()));
		const Dictionary arguments = tool_call.get("arguments", tool_call.get("args", Dictionary()));
		Dictionary tool_result = tool_registry->call_tool(tool_name, arguments);

		Dictionary item;
		item["index"] = i;
		item["name"] = tool_name;
		item["result"] = tool_result;
		tool_results.push_back(item);

		if (!(bool)tool_result.get("ok", false)) {
			executor_result["tool_results"] = tool_results;
			executor_result["error"] = tool_result.get("error", Dictionary());
			_record_phase("executor", executor_result);

			Dictionary data;
			data["state"] = "failed";
			data["phases"] = phases;
			data["planner_result"] = planner_result;
			data["executor_result"] = executor_result;
			return _ok(data);
		}
	}
	executor_result["tool_results"] = tool_results;
	_record_phase("executor", executor_result);

	phases.push_back("verifier");
	Dictionary verifier_result;
	verifier_result["checks"] = Array();
	verifier_result["ok"] = true;
	_record_phase("verifier", verifier_result);

	phases.push_back("reporter");
	Dictionary reporter_result;
	reporter_result["summary"] = "Solers mock turn completed.";
	_record_phase("reporter", reporter_result);

	Dictionary data;
	data["state"] = "completed";
	data["phases"] = phases;
	data["planner_result"] = planner_result;
	data["executor_result"] = executor_result;
	data["verifier_result"] = verifier_result;
	data["reporter_result"] = reporter_result;
	return _ok(data);
}
