/**************************************************************************/
/*  test_solers_provider_gateway.h                                        */
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

#pragma once

#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_agent_orchestrator.h"
#include "modules/solers_ai/core/solers_provider_gateway.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"

namespace TestSolersProviderGateway {

Dictionary make_user_message(const String &p_text) {
	Dictionary message;
	message["role"] = "user";
	message["content"] = p_text;
	return message;
}

Dictionary make_base_request(const String &p_provider) {
	Array messages;
	messages.push_back(make_user_message("Create a Sprite2D scene with a camera."));

	Dictionary request;
	request["provider"] = p_provider;
	request["model"] = "solers-test-model";
	request["objective"] = "Create a playable scene.";
	request["messages"] = messages;
	return request;
}

TEST_CASE("[SolersProviderGateway] builds OpenAI Responses request shape") {
	SolersProviderGateway gateway;

	Dictionary request = make_base_request("openai_responses");
	request["base_url"] = "https://api.openai.com/v1";
	request["api_key"] = "sk-secret";

	Dictionary result = gateway.build_request(request);

	CHECK(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("method", String()) == "POST");
	CHECK(data.get("path", String()) == "/responses");
	CHECK(data.get("url", String()) == "https://api.openai.com/v1/responses");

	Dictionary body = data.get("body", Dictionary());
	CHECK(body.get("model", String()) == "solers-test-model");
	CHECK(body.has("input"));
	CHECK_FALSE(body.has("api_key"));
}

TEST_CASE("[SolersProviderGateway] mock provider emits canonical events") {
	SolersProviderGateway gateway;

	Dictionary result = gateway.generate(make_base_request("mock"));

	CHECK(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("provider", String()) == "mock");

	Array events = data.get("events", Array());
	REQUIRE(events.size() == 2);
	Dictionary first = events[0];
	Dictionary second = events[1];
	CHECK(first.get("type", String()) == "text_delta");
	CHECK(second.get("type", String()) == "completed");
	CHECK(data.get("finish_reason", String()) == "stop");
}

TEST_CASE("[SolersAgentOrchestrator] runs planner executor verifier reporter phases") {
	SolersProviderGateway gateway;
	SolersAgentOrchestrator orchestrator;
	orchestrator.set_provider_gateway(&gateway);

	Dictionary request = make_base_request("mock");
	Dictionary result = orchestrator.start_turn(request);

	CHECK(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	Array phases = data.get("phases", Array());
	REQUIRE(phases.size() == 4);
	CHECK(String(phases[0]) == "planner");
	CHECK(String(phases[1]) == "executor");
	CHECK(String(phases[2]) == "verifier");
	CHECK(String(phases[3]) == "reporter");
	CHECK(data.get("state", String()) == "completed");
}

TEST_CASE("[SolersProviderRegistry] exposes gateway adapter profiles") {
	SolersProviderRegistry registry;

	Dictionary openai = registry.get_provider_profile("openai_responses");
	Dictionary anthropic = registry.get_provider_profile("anthropic_messages");

	CHECK(openai.get("kind", String()) == "openai_responses");
	CHECK(openai.get("default_base_url", String()) == "https://api.openai.com/v1");
	CHECK(anthropic.get("kind", String()) == "anthropic_messages");
	CHECK(anthropic.get("default_base_url", String()) == "https://api.anthropic.com");
}

TEST_CASE("[SolersMCPAdapter] routes orchestrator turns through JSON-RPC") {
	SolersProviderGateway gateway;
	SolersAgentOrchestrator orchestrator;
	orchestrator.set_provider_gateway(&gateway);

	SolersMCPAdapter adapter;
	adapter.set_agent_orchestrator(&orchestrator);

	Dictionary request;
	request["jsonrpc"] = "2.0";
	request["id"] = 7;
	request["method"] = "solers/agent/orchestrate";
	request["params"] = make_base_request("mock");

	Dictionary response = adapter.handle_request(request);

	CHECK(int(response.get("id", 0)) == 7);
	Dictionary result = response.get("result", Dictionary());
	CHECK(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("state", String()) == "completed");
}

TEST_CASE("[SolersAgentOrchestrator] fails executor phase when tool registry is missing") {
	SolersProviderGateway gateway;
	SolersAgentOrchestrator orchestrator;
	orchestrator.set_provider_gateway(&gateway);

	Dictionary tool_call;
	tool_call["name"] = "project.get_info";
	tool_call["arguments"] = Dictionary();
	Array tool_calls;
	tool_calls.push_back(tool_call);

	Dictionary request = make_base_request("mock");
	request["mock_tool_calls"] = tool_calls;

	Dictionary result = orchestrator.start_turn(request);

	CHECK(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("state", String()) == "failed");
	Dictionary executor_result = data.get("executor_result", Dictionary());
	Dictionary error = executor_result.get("error", Dictionary());
	CHECK(error.get("code", String()) == "TOOL_REGISTRY_UNAVAILABLE");
}

TEST_CASE("[SolersToolRegistry] exposes deterministic builtin tool metadata") {
	SolersToolRegistry registry;
	registry.register_default_tools();

	Array tools = registry.list_tools();
	REQUIRE(tools.size() > 20);

	String previous;
	bool found_patch = false;
	bool found_snapshot = false;
	for (int i = 0; i < tools.size(); i++) {
		Dictionary tool = tools[i];
		const String name = tool.get("name", String());
		const String model_name = tool.get("model_name", String());
		CHECK_FALSE(name.is_empty());
		CHECK_FALSE(model_name.is_empty());
		for (int c = 0; c < model_name.length(); c++) {
			const char32_t ch = model_name[c];
			const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
			CHECK(allowed);
		}
		CHECK(previous.is_empty() || previous <= name);
		previous = name;

		if (name == "script.patch") {
			found_patch = true;
			CHECK(tool.get("requires_approval", false));
			CHECK(tool.get("mutation_kind", String()) == "file_patch");
		} else if (name == "editor.get_snapshot") {
			found_snapshot = true;
			CHECK_FALSE((bool)tool.get("requires_approval", true));
			CHECK(model_name == "editor_get_snapshot");
		}
	}
	CHECK(found_patch);
	CHECK(found_snapshot);
	CHECK(registry.get_model_tool_name("editor.get_snapshot") == "editor_get_snapshot");
	CHECK(registry.resolve_model_tool_name("editor_get_snapshot") == StringName("editor.get_snapshot"));
}

TEST_CASE("[SolersLLMEvent] represents streaming reasoning as canonical events") {
	Dictionary event = SolersLLMEvent::reasoning_delta("Inspecting the scene tree.");

	CHECK(event.get("kind", String()) == String(SolersLLMEventKind::REASONING_DELTA));
	CHECK(event.get("text", String()) == "Inspecting the scene tree.");
}

} // namespace TestSolersProviderGateway
