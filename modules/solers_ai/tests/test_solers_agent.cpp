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

TEST_CASE("[SolersContextManager] compaction preserves the current instruction") {
	SolersContextManager context;
	Array messages;
	messages.push_back(Dictionary({ { "role", "user" }, { "content", String("old ").repeat(3000) }, { "turn_id", 1 } }));
	messages.push_back(Dictionary({ { "role", "user" }, { "content", "current instruction" }, { "turn_id", 2 } }));
	const Dictionary result = context.apply_compaction(messages, "Continue from current engine state.", 2000);
	REQUIRE((bool)result.get("accepted", false));
	const Array compacted = result.get("messages", Array());
	REQUIRE(compacted.size() >= 2);
	CHECK(String(Dictionary(compacted[0]).get("content", String())).find("Continue from current engine state.") >= 0);
	CHECK(context.get_compaction_count() == 1);
}

TEST_CASE("[SolersContextManager] open-turn tool evidence stays on the ledger") {
	Array messages;
	messages.push_back(SolersLLMMessage::user("lights?"));
	messages.push_back(SolersLLMMessage::assistant(String(), Array({ Dictionary({ { "id", "c1" }, { "name", "scene_inspect" }, { "arguments", "{}" } }) })));
	messages.push_back(SolersLLMMessage::tool_result("c1", "scene.inspect", "{\"ok\":true,\"data\":{}}"));
	const Array evidence = SolersContextManager::project_tool_evidence(messages);
	bool kept = false;
	for (int i = 0; i < evidence.size(); i++) {
		kept = kept || String(Dictionary(evidence[i]).get("tool_call_id", String())) == "c1";
	}
	CHECK(kept);
	CHECK(evidence.size() >= 3);
	const Array projected = SolersContextManager::project_completed_turns(messages);
	CHECK(projected.size() < evidence.size());
	CHECK(String(Dictionary(projected[projected.size() - 1]).get("tool_call_id", String())) != "c1");
}

TEST_CASE("[Editor][SolersAgentSession] truncated responses return tool failures without execution") {
	Ref<TCPServer> server;
	server.instantiate();
	REQUIRE(server->listen(0, IPAddress("127.0.0.1")) == OK);

	EditorSettings *editor_settings = EditorSettings::get_singleton();
	REQUIRE(editor_settings != nullptr);
	const String prefix = "solers/ai/";
	const String provider = "custom_openai_compatible";
	Array paths({ prefix + "provider", prefix + "local_models_only" });
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
	SolersPermissionManager permissions;
	SolersToolRegistry tools;
	tools.set_permission_manager(&permissions);
	tools.register_tool(memnew(SolersFunctionTool("synthetic.write", "Synthetic write", Dictionary({ { "type", "object" }, { "properties", Dictionary() } }), [&executions](const SolersToolContext &, const Dictionary &) {
		executions++;
		return Dictionary({ { "ok", true } });
	})));

	SolersTestPaths cleanup;
	SolersAgentSession session;
	session.set_models_dev(nullptr);
	cleanup.add("res://.solers/sessions/" + String(session.get_status().get("session_id", String())).sha256_text() + ".jsonl");
	session.set_settings_service(&settings);
	session.set_tool_registry(&tools);
	session.set_permission_manager(&permissions);
	REQUIRE(session.start_turn(Dictionary({ { "prompt", "write" } })).get("ok", false));

	const String body = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"synthetic.write\",\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}\n\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\ndata: [DONE]\n\n";
	const String response = vformat("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", body.utf8().length(), body);
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
	CHECK(executions == 0);
	const Dictionary delivered = JSON::parse_string(tool_result.get("content", String()));
	CHECK(Dictionary(delivered.get("error", Dictionary())).get("code", String()) == "TOOL_CALL_TRUNCATED");
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
