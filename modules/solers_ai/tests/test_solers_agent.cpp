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

#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_tool.h"
#include "modules/solers_ai/core/solers_tool_executor.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

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
