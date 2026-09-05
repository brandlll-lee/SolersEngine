/**************************************************************************/
/*  test_solers_agent.cpp                                                 */
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

#include "core/io/json.h"
#include "core/io/tcp_server.h"
#include "core/os/os.h"
#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool.h"
#include "modules/solers_ai/core/solers_tool_executor.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/tests/support/solers_test_state.h"

TEST_FORCE_LINK(test_solers_agent)

namespace TestSolersAgent {

TEST_CASE("[SolersContextManager] compaction is a bookmark projection") {
	Array messages;
	messages.push_back(Dictionary({ { "role", "user" }, { "content", "old" }, { "event_id", 1 } }));
	messages.push_back(Dictionary({ { "role", "user" }, { "content", "current instruction" }, { "event_id", 2 } }));
	const Array projected = SolersContextManager::project_compacted(messages, Dictionary({ { "summary", "Continue from current engine state." }, { "first_kept_event_id", 2 } }));
	REQUIRE(projected.size() == 2);
	CHECK(String(Dictionary(projected[0]).get("content", String())).find("Continue from current engine state.") >= 0);
	CHECK((int64_t)Dictionary(projected[1]).get("event_id", 0) == 2);
	CHECK(messages.size() == 2);
}

TEST_CASE("[SolersContextManager] compaction preserves complete tool pairs across turns") {
	Array messages;
	messages.push_back(SolersLLMMessage::user("inspect"));
	messages.push_back(SolersLLMMessage::assistant(String(), Array({ Dictionary({ { "id", "c1" }, { "name", "synthetic.inspect" }, { "arguments", "{\"cursor\":7}" } }) })));
	messages.push_back(SolersLLMMessage::tool_result("c1", "synthetic.inspect", "{\"ok\":false,\"error\":{\"code\":\"STALE\"}}"));
	messages.push_back(SolersLLMMessage::assistant("Inspect again.", Array()));
	messages.push_back(SolersLLMMessage::user("continue"));
	for (int i = 0; i < messages.size(); i++) {
		Dictionary message = messages[i];
		message["event_id"] = i + 1;
	}
	const Array projected = SolersContextManager::project_compacted(messages, Dictionary({ { "summary", "Earlier work" }, { "first_kept_event_id", 2 } }));
	REQUIRE(projected.size() == messages.size());
	for (int i = 1; i < messages.size(); i++) {
		CHECK(projected[i] == messages[i]);
	}
}

TEST_CASE("[Editor][SolersAgentSession] protocol completion controls execution and preserves tool results") {
	bool truncated = true;
	SUBCASE("truncated arguments are not executed") {}
	SUBCASE("complete results retain structured content beyond the text budget") {
		truncated = false;
	}
	Ref<TCPServer> server;
	server.instantiate();
	REQUIRE(server->listen(0, IPAddress("127.0.0.1")) == OK);

	EditorSettings *editor_settings = EditorSettings::get_singleton();
	REQUIRE(editor_settings != nullptr);
	const String prefix = "solers/ai/";
	const String provider = "custom_openai_compatible";
	Array paths({ prefix + "provider", prefix + "local_models_only", prefix + "model_overrides" });
	for (const String &key : { String("configured"), String("model"), String("base_url"), String("api_key") }) {
		paths.push_back(prefix + "providers/" + provider + "/" + key);
	}
	ScopedEditorSettings restore(editor_settings, paths);

	SolersProviderRegistry providers;
	SolersSettingsService settings;
	settings.set_provider_registry(&providers);
	Dictionary config({ { "provider", provider }, { "model", "synthetic-model" }, { "base_url", vformat("http://127.0.0.1:%d/v1", server->get_local_port()) }, { "api_key", "synthetic-key" } });
	REQUIRE(settings.set_provider_config(config).get("ok", false));

	int executions = 0;
	const Dictionary payload({ { "ok", true }, { "data", String("value").repeat(SolersContextManager::TOOL_RESULT_MAX_TOKENS) } });
	SolersPermissionManager permissions;
	SolersToolRegistry tools;
	tools.set_permission_manager(&permissions);
	tools.register_tool(memnew(SolersFunctionTool("synthetic.write", "Synthetic write", Dictionary({ { "type", "object" }, { "properties", Dictionary() } }), [&executions, &payload](const SolersToolContext &, const Dictionary &) {
		executions++;
		return payload;
	})));

	SolersTestPaths cleanup;
	SolersAgentSession session;
	session.set_model_catalog(nullptr);
	cleanup.add("res://.solers/sessions/" + String(session.get_status().get("session_id", String())).sha256_text() + ".jsonl");
	session.set_settings_service(&settings);
	CHECK((int)session.get_status().get("context_window", 0) == 128000);
	session.set_tool_registry(&tools);
	session.set_permission_manager(&permissions);
	REQUIRE(session.start_turn(Dictionary({ { "prompt", "write" } })).get("ok", false));

	const String body = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"synthetic.write\",\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}\n\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\ndata: [DONE]\n\n";
	const String completed_body = truncated ? body : body.replace("\"length\"", "\"tool_calls\"");
	const String response = vformat("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", completed_body.utf8().length(), completed_body);
	Dictionary tool_result;
	Ref<StreamPeerTCP> connection;
	const uint64_t started = OS::get_singleton()->get_ticks_msec();
	while (tool_result.is_empty() && OS::get_singleton()->get_ticks_msec() - started < 3000) {
		if (server->is_connection_available()) {
			connection = server->take_connection();
			const CharString bytes = response.utf8();
			connection->put_data((const uint8_t *)bytes.get_data(), bytes.length());
			server->stop();
		}
		session.poll();
		tool_result = solers_test_find_dictionary(session.get_messages(), "role", SolersLLMRole::TOOL);
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE_FALSE(tool_result.is_empty());
	CHECK(executions == (truncated ? 0 : 1));
	const Dictionary delivered = JSON::parse_string(tool_result.get("content", String()));
	if (truncated) {
		CHECK(Dictionary(delivered.get("error", Dictionary())).get("code", String()) == "TOOL_CALL_TRUNCATED");
	} else {
		CHECK((bool)delivered.get("ok", false));
		CHECK(String(delivered.get("data", String())).sha256_text() == String(payload["data"]).sha256_text());
	}
	session.abort();
}

TEST_CASE("[SolersPermissionManager] approval decisions are one-shot and cancellable") {
	SolersPermissionManager permissions;
	const Dictionary request = permissions.request_user_approval("synthetic.write", Dictionary({ { "path", "res://file.txt" } }), SolersPermissionManager::PERMISSION_EDIT_FILES);
	const int request_id = request.get("id", 0);
	REQUIRE(request_id > 0);
	CHECK(permissions.get_request_decision(request_id) == SolersPermissionManager::DECISION_PENDING);
	REQUIRE(permissions.approve_request(request_id));
	CHECK(permissions.get_request_decision(request_id) == SolersPermissionManager::DECISION_APPROVED);
	CHECK(permissions.consume_approval(request_id, "synthetic.write"));
	CHECK_FALSE(permissions.consume_approval(request_id, "synthetic.write"));

	const Dictionary second = permissions.request_user_approval("synthetic.cancel", Dictionary(), SolersPermissionManager::PERMISSION_EDIT_FILES);
	const int second_id = second.get("id", 0);
	REQUIRE(second_id > 0);
	REQUIRE(permissions.cancel_request(second_id));
	CHECK(permissions.get_request_decision(second_id) == SolersPermissionManager::DECISION_UNKNOWN);
}

} // namespace TestSolersAgent
