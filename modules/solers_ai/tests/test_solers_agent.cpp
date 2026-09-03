/**************************************************************************/
/*  test_solers_agent.cpp                                                 */
/**************************************************************************/

#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_tool.h"
#include "modules/solers_ai/core/solers_tool_executor.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

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
