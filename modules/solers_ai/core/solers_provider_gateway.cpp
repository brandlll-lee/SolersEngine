/**************************************************************************/
/*  solers_provider_gateway.cpp                                           */
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

#include "solers_provider_gateway.h"

#include "core/object/class_db.h"

void SolersProviderGateway::_bind_methods() {
	ClassDB::bind_method(D_METHOD("build_request", "request"), &SolersProviderGateway::build_request);
	ClassDB::bind_method(D_METHOD("generate", "request"), &SolersProviderGateway::generate);
}

Dictionary SolersProviderGateway::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersProviderGateway::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

String SolersProviderGateway::_join_url(const String &p_base_url, const String &p_path) const {
	String base_url = p_base_url;
	while (base_url.ends_with("/")) {
		base_url = base_url.substr(0, base_url.length() - 1);
	}

	String path = p_path;
	while (path.begins_with("/")) {
		path = path.substr(1);
	}

	return base_url + "/" + path;
}

Dictionary SolersProviderGateway::_build_openai_responses_request(const Dictionary &p_request) const {
	const String base_url = p_request.get("base_url", "https://api.openai.com/v1");
	const String path = "/responses";

	Dictionary body;
	body["model"] = p_request.get("model", String());
	body["input"] = p_request.get("messages", Array());
	if (p_request.has("tools")) {
		body["tools"] = p_request["tools"];
	}
	if (p_request.has("response_format")) {
		body["text"] = p_request["response_format"];
	}

	Dictionary headers;
	if (!String(p_request.get("api_key", String())).is_empty()) {
		headers["Authorization"] = "Bearer <redacted>";
	}
	headers["Content-Type"] = "application/json";

	Dictionary data;
	data["provider"] = "openai_responses";
	data["method"] = "POST";
	data["path"] = path;
	data["url"] = _join_url(base_url, path);
	data["headers"] = headers;
	data["body"] = body;
	return _ok(data);
}

Dictionary SolersProviderGateway::_build_anthropic_messages_request(const Dictionary &p_request) const {
	const String base_url = p_request.get("base_url", "https://api.anthropic.com");
	const String path = "/v1/messages";

	Dictionary body;
	body["model"] = p_request.get("model", String());
	body["max_tokens"] = p_request.get("max_tokens", 4096);
	body["messages"] = p_request.get("messages", Array());
	if (p_request.has("system")) {
		body["system"] = p_request["system"];
	}
	if (p_request.has("tools")) {
		body["tools"] = p_request["tools"];
	}

	Dictionary headers;
	if (!String(p_request.get("api_key", String())).is_empty()) {
		headers["x-api-key"] = "<redacted>";
	}
	headers["anthropic-version"] = p_request.get("anthropic_version", "2023-06-01");
	headers["Content-Type"] = "application/json";

	Dictionary data;
	data["provider"] = "anthropic_messages";
	data["method"] = "POST";
	data["path"] = path;
	data["url"] = _join_url(base_url, path);
	data["headers"] = headers;
	data["body"] = body;
	return _ok(data);
}

Dictionary SolersProviderGateway::_build_openai_compatible_request(const Dictionary &p_request) const {
	const String base_url = p_request.get("base_url", "http://127.0.0.1:11434/v1");
	const String path = "/chat/completions";

	Dictionary body;
	body["model"] = p_request.get("model", String());
	body["messages"] = p_request.get("messages", Array());
	if (p_request.has("tools")) {
		body["tools"] = p_request["tools"];
	}

	Dictionary headers;
	if (!String(p_request.get("api_key", String())).is_empty()) {
		headers["Authorization"] = "Bearer <redacted>";
	}
	headers["Content-Type"] = "application/json";

	Dictionary data;
	data["provider"] = p_request.get("provider", "openai_compatible");
	data["method"] = "POST";
	data["path"] = path;
	data["url"] = _join_url(base_url, path);
	data["headers"] = headers;
	data["body"] = body;
	return _ok(data);
}

Dictionary SolersProviderGateway::_generate_mock_response(const Dictionary &p_request) const {
	Array events;

	Dictionary text_event;
	text_event["type"] = "text_delta";
	text_event["delta"] = vformat("Plan for objective: %s", String(p_request.get("objective", String())));
	events.push_back(text_event);

	Dictionary completed_event;
	completed_event["type"] = "completed";
	completed_event["finish_reason"] = "stop";
	events.push_back(completed_event);

	Dictionary data;
	data["provider"] = "mock";
	data["model"] = p_request.get("model", "solers-mock");
	data["events"] = events;
	data["tool_calls"] = p_request.get("mock_tool_calls", Array());
	data["finish_reason"] = "stop";
	return _ok(data);
}

Dictionary SolersProviderGateway::build_request(const Dictionary &p_request) const {
	const String provider = p_request.get("provider", "openai_compatible");
	if (provider == "openai_responses" || provider == "openai") {
		return _build_openai_responses_request(p_request);
	}
	if (provider == "anthropic_messages" || provider == "anthropic") {
		return _build_anthropic_messages_request(p_request);
	}
	if (provider == "openai_compatible" || provider == "ollama" || provider == "gemini") {
		return _build_openai_compatible_request(p_request);
	}
	if (provider == "mock") {
		Dictionary data;
		data["provider"] = "mock";
		data["method"] = "MOCK";
		data["body"] = p_request;
		return _ok(data);
	}
	return _error("UNKNOWN_PROVIDER_ADAPTER", vformat("Unknown provider adapter: %s", provider), true);
}

Dictionary SolersProviderGateway::generate(const Dictionary &p_request) const {
	const String provider = p_request.get("provider", "openai_compatible");
	if (provider == "mock") {
		return _generate_mock_response(p_request);
	}

	Dictionary request_result = build_request(p_request);
	if (!(bool)request_result.get("ok", false)) {
		return request_result;
	}

	return _error("PROVIDER_TRANSPORT_UNAVAILABLE", "SolersProviderGateway has built the provider request, but real HTTP transport is not enabled in this slice.", true);
}
