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

#include "core/io/json.h"
#include "core/object/class_db.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_provider_catalog.h"

void SolersAgentSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_turn", "args"), &SolersAgentSession::start_turn);
	ClassDB::bind_method(D_METHOD("poll"), &SolersAgentSession::poll);
	ClassDB::bind_method(D_METHOD("abort"), &SolersAgentSession::abort);
	ClassDB::bind_method(D_METHOD("reset_conversation"), &SolersAgentSession::reset_conversation);
	ClassDB::bind_method(D_METHOD("get_status"), &SolersAgentSession::get_status);

	ADD_SIGNAL(MethodInfo("assistant_delta", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("reasoning_delta", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("assistant_message", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("tool_call_started", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::STRING, "arguments")));
	ADD_SIGNAL(MethodInfo("tool_call_finished", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::DICTIONARY, "result")));
	ADD_SIGNAL(MethodInfo("turn_completed", PropertyInfo(Variant::DICTIONARY, "result")));
	ADD_SIGNAL(MethodInfo("turn_failed", PropertyInfo(Variant::DICTIONARY, "error")));
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

String SolersAgentSession::_default_system_prompt() const {
	return String(
			"You are Solers, an AI agent operating inside the Solers game engine editor "
			"(a Godot-compatible engine). You help the user build games by calling the "
			"provided tools to inspect and modify the real editor: scenes, nodes, scripts, "
			"resources, running the game, and validating results. Prefer engine-native tool "
			"calls over guessing. Make one focused change at a time, then verify. When the "
			"task is complete, briefly summarize what you did.");
}

Array SolersAgentSession::_collect_tools() const {
	Array out;
	if (!tool_registry) {
		return out;
	}
	const Array defs = tool_registry->list_tools();
	for (int i = 0; i < defs.size(); i++) {
		const Dictionary def = defs[i];
		Dictionary tool;
		tool["name"] = def.get("model_name", def.get("name", String()));
		tool["canonical_name"] = def.get("name", String());
		tool["description"] = def.get("description", String());
		// The registry's input_schema is already a JSON-schema object; pass it
		// through as the canonical `parameters`.
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

Dictionary SolersAgentSession::_build_request() const {
	Dictionary request;
	request["model"] = active_provider.get("model", String());
	request["system"] = system_prompt;
	request["messages"] = messages;
	request["tools"] = _collect_tools();
	request["max_tokens"] = 4096;
	return request;
}

Error SolersAgentSession::_dispatch_model_request() {
	const String provider_id = active_provider.get("provider", String());
	const String base_url = active_provider.get("base_url", String());
	const String api_key = active_provider.get("api_key", String());

	const Dictionary profile = provider_catalog->resolve(StringName(provider_id), base_url);
	const Dictionary request = _build_request();

	const Error err = client->begin(request, profile, api_key);
	if (err != OK) {
		running = false;
		emit_signal(SNAME("turn_failed"), client->get_error());
		return err;
	}
	running = true;
	_record("agent_model_request", request);
	return OK;
}

Dictionary SolersAgentSession::start_turn(const Dictionary &p_args) {
	if (running) {
		return _error("AGENT_BUSY", "A Solers agent turn is already running.");
	}
	if (!tool_registry || !settings_service) {
		return _error("AGENT_UNCONFIGURED", "Solers agent session is missing its services.");
	}

	const String prompt = String(p_args.get("prompt", String())).strip_edges();
	if (prompt.is_empty()) {
		return _error("EMPTY_PROMPT", "Prompt is empty.");
	}

	active_provider = settings_service->resolve_active_provider();
	const String provider_id = active_provider.get("provider", String());
	const String model = active_provider.get("model", String());
	const String base_url = active_provider.get("base_url", String());
	const String api_key = active_provider.get("api_key", String());
	const Dictionary profile = provider_catalog->resolve(StringName(provider_id), base_url);
	const bool local = profile.get("local", false);

	// Hard privacy gate at the dispatch boundary — validation-layer blockers are
	// advisory, but no request may leave the machine while privacy mode is on.
	const bool privacy_mode = active_provider.get("privacy_mode", true);
	if (privacy_mode && !local) {
		Dictionary e = _error("PRIVACY_BLOCKED", "Privacy mode allows local providers only (Ollama / LM Studio). Disable privacy mode in the AI settings to use remote providers.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}

	if (model.is_empty()) {
		Dictionary e = _error("NO_MODEL", "No model is configured. Set one in Solers BYOK settings (solers/ai/model).");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (String(profile.get("base_url", String())).strip_edges().is_empty()) {
		Dictionary e = _error("NO_BASE_URL", "No base URL configured for this provider.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (!local && api_key.is_empty()) {
		Dictionary e = _error("NO_API_KEY", "No API key configured. Set it in Solers BYOK settings.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}

	if (system_prompt.is_empty()) {
		system_prompt = _default_system_prompt();
	}
	messages.push_back(SolersLLMMessage::user(prompt));
	current_text = String();
	current_reasoning = String();
	pending_tool_calls.clear();
	last_usage.clear();
	tool_iterations = 0;
	turn_id++;

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
	const Array events = client->poll();
	for (int i = 0; i < events.size(); i++) {
		const Dictionary e = events[i];
		const String kind = e.get("kind", String());
		if (kind == SolersLLMEventKind::TEXT_DELTA) {
			const String text = e.get("text", String());
			current_text += text;
			emit_signal(SNAME("assistant_delta"), text);
		} else if (kind == SolersLLMEventKind::REASONING_DELTA) {
			const String text = e.get("text", String());
			current_reasoning += text;
			emit_signal(SNAME("reasoning_delta"), text);
		} else if (kind == SolersLLMEventKind::TOOL_CALL) {
			const String requested_name = e.get("name", String());
			const StringName canonical_name = tool_registry ? tool_registry->resolve_model_tool_name(requested_name) : StringName();
			String model_name = requested_name;
			if (!String(canonical_name).is_empty() && tool_registry) {
				const String registered_model_name = tool_registry->get_model_tool_name(canonical_name);
				if (!registered_model_name.is_empty()) {
					model_name = registered_model_name;
				}
			}
			Dictionary call;
			call["id"] = e.get("id", String());
			call["name"] = model_name;
			call["canonical_name"] = String(canonical_name);
			call["requested_name"] = requested_name;
			call["arguments"] = e.get("arguments", String());
			pending_tool_calls.push_back(call);
		} else if (kind == SolersLLMEventKind::USAGE) {
			last_usage = e;
		} else if (kind == SolersLLMEventKind::FINISH) {
			last_stop_reason = e.get("stop_reason", String());
		} else if (kind == SolersLLMEventKind::ERROR) {
			// Surfaced again via client failure below.
		}
	}

	if (client->is_failed()) {
		running = false;
		emit_signal(SNAME("turn_failed"), client->get_error());
		return;
	}
	if (client->is_done()) {
		_on_model_turn_complete();
	}
}

void SolersAgentSession::_on_model_turn_complete() {
	messages.push_back(SolersLLMMessage::assistant(current_text, pending_tool_calls));
	if (!current_text.is_empty()) {
		emit_signal(SNAME("assistant_message"), current_text);
	}

	if (pending_tool_calls.is_empty()) {
		Dictionary data;
		data["text"] = current_text;
		data["reasoning"] = current_reasoning;
		data["stop_reason"] = last_stop_reason;
		if (!last_usage.is_empty()) {
			data["usage"] = last_usage;
		}
		running = false;
		current_text = String();
		current_reasoning = String();
		_record("agent_turn_completed", data);
		emit_signal(SNAME("turn_completed"), data);
		return;
	}

	tool_iterations++;
	if (tool_iterations > max_tool_iterations) {
		running = false;
		pending_tool_calls.clear();
		current_text = String();
		current_reasoning = String();
		Dictionary err;
		err["code"] = "TOOL_LOOP_LIMIT";
		err["message"] = vformat("Exceeded the maximum of %d tool iterations in one turn.", max_tool_iterations);
		emit_signal(SNAME("turn_failed"), err);
		return;
	}

	const Array calls = pending_tool_calls.duplicate();
	pending_tool_calls.clear();
	current_text = String();
	current_reasoning = String();

	for (int i = 0; i < calls.size(); i++) {
		const Dictionary call = calls[i];
		const String name = call.get("name", String());
		const String canonical_name = call.get("canonical_name", name);
		const String requested_name = call.get("requested_name", name);
		const String id = call.get("id", String());
		const String arguments = call.get("arguments", "{}");

		emit_signal(SNAME("tool_call_started"), id, canonical_name, arguments);

		const Variant parsed = JSON::parse_string(arguments.is_empty() ? "{}" : arguments);
		Dictionary result;
		if (canonical_name.is_empty()) {
			Dictionary error;
			error["code"] = "UNKNOWN_TOOL";
			error["message"] = vformat("Model requested an unknown Solers tool: %s.", requested_name);
			error["recoverable"] = true;
			result["ok"] = false;
			result["error"] = error;
		} else if (parsed.get_type() == Variant::DICTIONARY) {
			const Dictionary args = parsed;
			result = tool_registry->call_tool(StringName(canonical_name), args);
		} else {
			Dictionary error;
			error["code"] = "INVALID_TOOL_ARGUMENTS";
			error["message"] = "Tool arguments must be a JSON object.";
			error["recoverable"] = true;
			result["ok"] = false;
			result["error"] = error;
		}

		emit_signal(SNAME("tool_call_finished"), id, canonical_name, result);

		const String content = JSON::stringify(result, "", false, true);
		messages.push_back(SolersLLMMessage::tool_result(id, name, content));
	}

	// Continue the loop: ask the model again with the tool results in context.
	const Error err = _dispatch_model_request();
	if (err != OK) {
		current_reasoning = String();
	}
}

void SolersAgentSession::abort() {
	if (client) {
		client->abort();
	}
	running = false;
	pending_tool_calls.clear();
	current_text = String();
	current_reasoning = String();
}

void SolersAgentSession::reset_conversation() {
	abort();
	messages.clear();
	tool_iterations = 0;
	last_stop_reason = String();
	last_usage.clear();
}

Dictionary SolersAgentSession::get_status() const {
	Dictionary status;
	status["running"] = running;
	status["turn_id"] = turn_id;
	status["message_count"] = messages.size();
	status["provider"] = active_provider.get("provider", String());
	status["model"] = active_provider.get("model", String());
	status["tool_iterations"] = tool_iterations;
	status["max_tool_iterations"] = max_tool_iterations;
	return status;
}

SolersAgentSession::SolersAgentSession() {
	protocol_registry = memnew(SolersLLMProtocolRegistry);
	protocol_registry->register_builtin_protocols();
	provider_catalog = memnew(SolersLLMProviderCatalog);
	provider_catalog->register_builtin_profiles();
	client = memnew(SolersLLMClient);
	client->set_protocol_registry(protocol_registry);
}

SolersAgentSession::~SolersAgentSession() {
	abort();
	if (client) {
		memdelete(client);
		client = nullptr;
	}
	if (provider_catalog) {
		memdelete(provider_catalog);
		provider_catalog = nullptr;
	}
	if (protocol_registry) {
		memdelete(protocol_registry);
		protocol_registry = nullptr;
	}
}
