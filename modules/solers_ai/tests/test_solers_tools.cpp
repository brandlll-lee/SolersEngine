/**************************************************************************/
/*  test_solers_tools.cpp                                                 */
/**************************************************************************/

#include "core/variant/dictionary.h"
#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "modules/solers_ai/core/solers_project_observation.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_runtime_observation.h"
#include "modules/solers_ai/core/solers_scene_observation.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool.h"
#include "modules/solers_ai/core/solers_tool_executor.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

TEST_FORCE_LINK(test_solers_tools)

namespace TestSolersTools {

static void configure_registry(SolersToolRegistry &r_registry, SolersPermissionManager &r_permissions,
		SolersProjectObservation &r_project, SolersReflectionService &r_reflection,
		SolersResourceService &r_resource, SolersRuntimeObservation &r_runtime,
		SolersSceneObservation &r_scene, SolersScriptService &r_script) {
	r_permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	r_registry.set_permission_manager(&r_permissions);
	r_registry.set_project_observation(&r_project);
	r_registry.set_reflection_service(&r_reflection);
	r_registry.set_resource_service(&r_resource);
	r_registry.set_runtime_observation(&r_runtime);
	r_registry.set_scene_observation(&r_scene);
	r_registry.set_script_service(&r_script);
	r_registry.register_default_tools();
}

TEST_CASE("[SolersToolRegistry] default surface is exactly sixteen tools") {
	SolersPermissionManager permissions;
	SolersProjectObservation project;
	SolersReflectionService reflection;
	SolersResourceService resource;
	SolersRuntimeObservation runtime;
	SolersSceneObservation scene;
	SolersScriptService script;
	SolersToolRegistry registry;
	configure_registry(registry, permissions, project, reflection, resource, runtime, scene, script);

	const PackedStringArray expected = {
		"search", "read", "edit", "project.settings", "script.validate", "engine.describe",
		"object.inspect", "scene.inspect", "scene.open", "scene.edit", "resource.edit", "render.capture",
		"editor.script", "runtime.observe", "runtime.control", "runtime.script"
	};
	CHECK(registry.get_tool_count() == expected.size());
	for (const String &name : expected) {
		const Dictionary definition = registry.get_tool_definition(name);
		REQUIRE_FALSE(definition.is_empty());
		CHECK_FALSE(definition.has("permission"));
		CHECK_FALSE(definition.has("exposure"));
		CHECK_FALSE(definition.has("mutation_domains"));
		CHECK_FALSE(definition.has("execution_policy"));
	}
}

TEST_CASE("[SolersToolRegistry] schema validation and writeOnly audit redaction are authoritative") {
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_tool(memnew(SolersFunctionTool("synthetic.secret", "Secret input", Dictionary({
		{ "type", "object" },
		{ "properties", Dictionary({ { "secret", Dictionary({ { "type", "string" }, { "writeOnly", true } }) } }) },
		{ "required", Array({ "secret" }) },
		{ "additionalProperties", false }
	}), [](const SolersToolContext &, const Dictionary &) {
		return Dictionary({ { "ok", true }, { "data", Dictionary({ { "accepted", true } }) } });
	})));

	const Dictionary invalid = registry.call_tool("synthetic.secret", Dictionary());
	CHECK_FALSE((bool)invalid.get("ok", true));
	CHECK(Dictionary(invalid.get("error", Dictionary())).get("code", String()) == "TOOL_ARGUMENT_INVALID");
	const Dictionary redacted = registry.redact_tool_args_for_audit("synthetic.secret", Dictionary({ { "secret", "hidden" } }));
	CHECK(String(redacted.get("secret", String())) == "<redacted>");
}

TEST_CASE("[SolersToolExecutor] owns one serial slot and reaches a terminal continuation") {
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	int executions = 0;
	int polls = 0;
	const Dictionary schema({ { "type", "object" }, { "properties", Dictionary() }, { "additionalProperties", false } });
	registry.register_tool(memnew(SolersFunctionTool("synthetic.pending", "Pending test tool", schema,
			[&executions](const SolersToolContext &, const Dictionary &) {
				executions++;
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "status", "pending" }, { "poll_args", Dictionary({ { "cursor", 1 } }) } }) } });
			},
			[&polls](const SolersToolContext &, const Dictionary &) {
				polls++;
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "done", true } }) } });
			})));

	SolersToolExecutor executor;
	executor.configure(&registry, &permissions);
	SolersToolContext context;
	context.call_id = "executor-test";
	context.permission_gate = [](SolersPermissionManager::Permission, const Dictionary &) { return Dictionary(); };
	CHECK(executor.start("synthetic.pending", Dictionary(), context, 10000).is_empty());
	const Dictionary busy = executor.start("synthetic.pending", Dictionary(), context, 10000);
	CHECK_FALSE((bool)busy.get("ok", true));
	CHECK(Dictionary(busy.get("error", Dictionary())).get("code", String()) == "TOOL_EXECUTOR_BUSY");
	executor.poll();
	CHECK(executions == 1);
	CHECK(executor.get_state() == SolersToolExecutor::STATE_CONTINUING);
	executor.poll();
	CHECK(polls == 1);
	CHECK(executor.is_terminal());
	const Dictionary terminal = executor.take_result();
	CHECK_FALSE(terminal.is_empty());
	CHECK(terminal.has("ok"));
	CHECK((bool)terminal.get("ok", false));
}

TEST_CASE("[SolersToolExecutor] approval and cancellation produce one terminal receipt") {
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_tool(memnew(SolersFunctionTool("synthetic.write", "Approval test tool",
			Dictionary({ { "type", "object" }, { "properties", Dictionary() }, { "additionalProperties", false } }),
			[](const SolersToolContext &ctx, const Dictionary &) {
				const Dictionary denied = ctx.require_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, Dictionary());
				return denied.is_empty() ? Dictionary({ { "ok", true }, { "data", Dictionary({ { "written", true } }) } }) : denied;
			})));
	SolersToolExecutor executor;
	executor.configure(&registry, &permissions);
	SolersToolContext context;
	context.call_id = "approval-test";
	CHECK(executor.start("synthetic.write", Dictionary(), context, 10000).is_empty());
	executor.poll();
	REQUIRE(executor.is_awaiting_approval());
	REQUIRE(permissions.approve_request(executor.get_approval_id()));
	executor.poll();
	REQUIRE(executor.is_terminal());
	const Dictionary approved = executor.take_result();
	CHECK_FALSE(approved.is_empty());
	CHECK((bool)approved.get("ok", false));

	SolersToolContext cancel_context;
	cancel_context.call_id = "cancel-test";
	CHECK(executor.start("synthetic.write", Dictionary(), cancel_context, 10000).is_empty());
	executor.cancel();
	REQUIRE(executor.is_terminal());
	const Dictionary cancelled = executor.take_result();
	CHECK_FALSE(cancelled.is_empty());
	CHECK_FALSE((bool)cancelled.get("ok", true));
	CHECK(Dictionary(cancelled.get("error", Dictionary())).get("code", String()) == "TOOL_CANCELLED");
	CHECK(executor.take_result().is_empty());
}

TEST_CASE("[SolersMCPAdapter] tools/call is deferred until the shared executor reaches terminal state") {
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	int calls = 0;
	registry.register_tool(memnew(SolersFunctionTool("synthetic.mcp", "MCP test tool",
			Dictionary({ { "type", "object" }, { "properties", Dictionary() }, { "additionalProperties", false } }),
			[&calls](const SolersToolContext &, const Dictionary &) {
				calls++;
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "calls", calls } }) } });
			})));
	SolersToolExecutor executor;
	executor.configure(&registry, &permissions);
	SolersMCPAdapter adapter;
	adapter.set_tool_registry(&registry);
	adapter.set_tool_executor(&executor);

	const Dictionary accepted = adapter.begin_request(Dictionary({
			{ "jsonrpc", "2.0" }, { "id", 7 }, { "method", "tools/call" },
			{ "params", Dictionary({ { "name", "synthetic.mcp" }, { "arguments", Dictionary() } }) }
	}));
	CHECK((bool)accepted.get("deferred", false));
	const int64_t token = accepted.get("request_token", 0);
	REQUIRE(token > 0);
	CHECK(adapter.take_response(token).is_empty());
	adapter.poll();
	const Dictionary response = adapter.take_response(token);
	REQUIRE_FALSE(response.is_empty());
	CHECK((int)response.get("id", 0) == 7);
	CHECK((bool)Dictionary(response.get("result", Dictionary())).get("isError", true) == false);
	CHECK(calls == 1);
}

} // namespace TestSolersTools
