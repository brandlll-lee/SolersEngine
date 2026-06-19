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

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_provider_gateway.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_provider_catalog.h"
#include "modules/solers_ai/llm/solers_protocol_anthropic_messages.h"
#include "modules/solers_ai/llm/solers_protocol_openai_chat.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "scene/3d/world_environment.h"
#include "scene/main/resource_preloader.h"
#include "scene/resources/environment.h"

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

Dictionary find_event_kind(const Array &p_events, const String &p_kind) {
	for (int i = 0; i < p_events.size(); i++) {
		const Dictionary event = p_events[i];
		if (event.get("kind", String()) == p_kind) {
			return event;
		}
	}
	return Dictionary();
}

Dictionary find_tool_def(const Array &p_tools, const String &p_name) {
	for (int i = 0; i < p_tools.size(); i++) {
		const Dictionary tool = p_tools[i];
		if (tool.get("name", String()) == p_name) {
			return tool;
		}
	}
	return Dictionary();
}

int count_tools_by_exposure(const Array &p_tools, const String &p_exposure) {
	int count = 0;
	for (int i = 0; i < p_tools.size(); i++) {
		const Dictionary tool = p_tools[i];
		if (tool.get("exposure", String()) == p_exposure) {
			count++;
		}
	}
	return count;
}

Dictionary search_deferred_tools(SolersToolRegistry &p_registry, const String &p_query, int p_max_results = 10) {
	Dictionary args;
	args["query"] = p_query;
	args["max_results"] = p_max_results;
	return p_registry.call_tool("tool.search", args);
}

bool search_result_has_tool(const Dictionary &p_result, const String &p_name) {
	const Dictionary data = p_result.get("data", Dictionary());
	const Array tools = data.get("tools", Array());
	return !find_tool_def(tools, p_name).is_empty();
}

TEST_CASE("[SolersProviderGateway] builds OpenAI-compatible Chat Completions request shape") {
	SolersProviderGateway gateway;

	Dictionary request = make_base_request("open-ai");
	request["base_url"] = "https://ai.nodeseek.in/v1";
	request["api_key"] = "sk-secret";

	Dictionary result = gateway.build_request(request);

	CHECK(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("method", String()) == "POST");
	CHECK(data.get("path", String()) == "/chat/completions");
	CHECK(data.get("url", String()) == "https://ai.nodeseek.in/v1/chat/completions");

	Dictionary body = data.get("body", Dictionary());
	CHECK(body.get("model", String()) == "solers-test-model");
	CHECK(body.has("messages"));
	CHECK_FALSE(body.get("store", true));
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

TEST_CASE("[SolersProviderRegistry] exposes gateway adapter profiles") {
	SolersProviderRegistry registry;

	Dictionary openai = registry.get_provider_profile("openai");
	Dictionary anthropic = registry.get_provider_profile("anthropic_messages");

	CHECK(openai.get("kind", String()) == "openai_compatible");
	CHECK(openai.get("default_base_url", String()) == "https://api.openai.com/v1");
	CHECK(anthropic.get("kind", String()) == "anthropic_messages");
	CHECK(anthropic.get("default_base_url", String()) == "https://api.anthropic.com");
}

TEST_CASE("[SolersProviderRegistry] accepts unknown custom provider ids when base_url is set") {
	SolersProviderRegistry registry;

	Dictionary config;
	config["provider"] = "open-ai";
	config["model"] = "gpt-5.5";
	config["base_url"] = "https://ai.nodeseek.in/v1";
	config["privacy_mode"] = false;
	config["api_key"] = "sk-test";

	Dictionary result = registry.validate_config(config);

	CHECK(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("valid", false));
	Dictionary profile = data.get("profile", Dictionary());
	CHECK(profile.get("id", String()) == "open-ai");
	CHECK(profile.get("kind", String()) == "openai_compatible");
	CHECK(data.get("effective_base_url", String()) == "https://ai.nodeseek.in/v1");
}

TEST_CASE("[SolersToolRegistry] registers tools by lookup, not a hardcoded catalog") {
	// Behavior contract (no-patch): a brand-new tool the dispatcher has never
	// special-cased becomes discoverable + dispatchable purely by registering
	// it — no catalog entry, no dispatcher branch, no name-prefix classifier.
	SolersToolRegistry registry;

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_kind = "none";

	Dictionary schema;
	schema["type"] = "object";
	Dictionary properties;
	properties["value"] = Dictionary();
	properties["optional_empty"] = Dictionary();
	properties["required_empty"] = Dictionary();
	schema["properties"] = properties;
	Array required;
	required.push_back("required_empty");
	schema["required"] = required;

	registry.register_tool(memnew(SolersFunctionTool(
			StringName("synthetic.echo"),
			"A brand-new tool the dispatcher has never special-cased.",
			schema, SolersToolExposure::DIRECT, cap,
			[](const SolersToolContext &, const Dictionary &a) {
				Dictionary data;
				data["echo"] = a.get("value", String());
				data["has_optional_empty"] = a.has("optional_empty");
				data["has_required_empty"] = a.has("required_empty");
				data["has_unknown_empty"] = a.has("unknown_empty");
				Dictionary result;
				result["ok"] = true;
				result["data"] = data;
				return result;
			})));

	CHECK(registry.get_tool_count() == 1);
	Array tools = registry.list_tools();
	REQUIRE(tools.size() == 1);
	Dictionary tool = tools[0];
	CHECK(tool.get("name", String()) == "synthetic.echo");
	CHECK(tool.get("model_name", String()) == "synthetic_echo");
	CHECK_FALSE((bool)tool.get("requires_approval", true));
	CHECK(registry.get_model_tool_name("synthetic.echo") == "synthetic_echo");
	CHECK(registry.resolve_model_tool_name("synthetic_echo") == StringName("synthetic.echo"));

	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_permission_manager(&permissions);

	Dictionary args;
	args["value"] = "hi";
	args["optional_empty"] = "";
	args["required_empty"] = "";
	args["unknown_empty"] = "";
	Dictionary result = registry.call_tool("synthetic.echo", args);
	CHECK(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("echo", String()) == "hi");
	CHECK_FALSE((bool)data.get("has_optional_empty", true));
	CHECK((bool)data.get("has_required_empty", false));
	CHECK((bool)data.get("has_unknown_empty", false));
}

TEST_CASE("[SolersToolRegistry] default model surface is primitive-first") {
	SolersObservationService observation_service;
	SolersPermissionManager permissions;
	SolersReflectionService reflection_service;
	SolersResourceService resource_service;
	SolersScriptService script_service;
	SolersToolRegistry registry;
	registry.set_observation_service(&observation_service);
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection_service);
	registry.set_resource_service(&resource_service);
	registry.set_script_service(&script_service);
	registry.register_default_tools();

	const Array tools = registry.list_tools();
	CHECK(count_tools_by_exposure(tools, "direct") <= 10);

	const char *required_direct[] = {
		"class.introspect",
		"object.get_property",
		"object.call_method",
		"objects.batch",
		"editor.get_snapshot",
		"project.read_file",
		"project.write_file",
		"script.patch",
		"runtime.control",
		"tool.search",
	};
	for (const char *name : required_direct) {
		Dictionary tool = find_tool_def(tools, name);
		REQUIRE_FALSE(tool.is_empty());
		CHECK(tool.get("exposure", String()) == "direct");
	}
	Dictionary batch_tool = find_tool_def(tools, "objects.batch");
	REQUIRE_FALSE(batch_tool.is_empty());
	Dictionary batch_schema = batch_tool.get("input_schema", Dictionary());
	Dictionary batch_properties = batch_schema.get("properties", Dictionary());
	Dictionary operations_schema = batch_properties.get("operations", Dictionary());
	const String operations_description = operations_schema.get("description", String());
	CHECK(operations_description.contains("parent_path"));
	CHECK(operations_description.contains("new_parent_path"));
	Dictionary write_file = find_tool_def(tools, "project.write_file");
	CHECK(write_file.get("permission", String()) == "edit_files");
	Dictionary write_schema = write_file.get("input_schema", Dictionary());
	Dictionary write_properties = write_schema.get("properties", Dictionary());
	CHECK_FALSE(write_properties.has("reimport"));

	Dictionary set_property = find_tool_def(tools, "object.set_property");
	REQUIRE_FALSE(set_property.is_empty());
	CHECK(set_property.get("exposure", String()) == "deferred");
	Dictionary search_files = find_tool_def(tools, "project.search_files");
	REQUIRE_FALSE(search_files.is_empty());
	CHECK(search_files.get("exposure", String()) == "deferred");
	CHECK(find_tool_def(tools, "project.get_info").is_empty());
	CHECK(find_tool_def(tools, "project.get_settings_summary").is_empty());
	CHECK(find_tool_def(tools, "project.list_files").is_empty());
	CHECK(find_tool_def(tools, "script.read").is_empty());
	CHECK(find_tool_def(tools, "scene.get_open_scenes").is_empty());
	CHECK(find_tool_def(tools, "scene.get_tree").is_empty());
	CHECK(find_tool_def(tools, "selection.get_nodes").is_empty());
	CHECK(find_tool_def(tools, "runtime.get_status").is_empty());
	CHECK(find_tool_def(tools, "editor.get_logs").is_empty());
	Dictionary editor_invoke = find_tool_def(tools, "editor.invoke");
	REQUIRE_FALSE(editor_invoke.is_empty());
	CHECK(editor_invoke.get("exposure", String()) == "deferred");
	Dictionary export_run = find_tool_def(tools, "export.run_preset");
	REQUIRE_FALSE(export_run.is_empty());
	CHECK(export_run.get("exposure", String()) == "deferred");
	CHECK(find_tool_def(tools, "runtime.get_logs").is_empty());
	CHECK(find_tool_def(tools, "script.open_in_editor").is_empty());
	CHECK(find_tool_def(tools, "validation.assert_no_errors").is_empty());
	CHECK(find_tool_def(tools, "validation.read_editor_errors").is_empty());
	CHECK(find_tool_def(tools, "validation.validate_project_scripts").is_empty());
	CHECK(find_tool_def(tools, "validation.run_scene_smoke").is_empty());
	CHECK(find_tool_def(tools, "scene.save").is_empty());
	CHECK(find_tool_def(tools, "node.add").is_empty());
	CHECK(registry.should_autocommit_scene_after_tool("objects.batch"));
	CHECK(registry.should_autocommit_scene_after_tool("object.set_property"));
	CHECK_FALSE(registry.should_autocommit_scene_after_tool("project.write_file"));

	for (int i = 0; i < tools.size(); i++) {
		const Dictionary tool = tools[i];
		if (tool.get("exposure", String()) != "direct") {
			continue;
		}
		const String name = tool.get("name", String());
		CHECK_FALSE(name.begins_with("node."));
		CHECK_FALSE(name.begins_with("scene."));
		CHECK_FALSE(name.begins_with("provider."));
		CHECK_FALSE(name.begins_with("timeline."));
		CHECK_FALSE(name.begins_with("rpc."));
		CHECK_FALSE(name.begins_with("approvals."));
		CHECK_FALSE(name.begins_with("validation."));
		CHECK(name != "runtime.capture_screenshot");
	CHECK(name != "editor.capture_screenshot");
	}
}

TEST_CASE("[SolersToolRegistry] batch failure summaries expose failed operation") {
	SolersToolRegistry registry;

	Dictionary op_error;
	op_error["code"] = "NODE_NOT_FOUND";
	op_error["message"] = "Node not found: Forest/Tree01";
	Dictionary op_result;
	op_result["ok"] = false;
	op_result["error"] = op_error;
	Dictionary entry;
	entry["index"] = 3;
	entry["op"] = "set_property";
	entry["result"] = op_result;
	Array results;
	results.push_back(entry);

	Dictionary data;
	data["count"] = 4;
	data["completed"] = false;
	data["results"] = results;
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;

	const String summary = registry.summarize_tool_result_for_audit(result);
	CHECK(summary.contains("completed=0"));
	CHECK(summary.contains("failed_op=set_property"));
	CHECK(summary.contains("failed_index=3"));
	CHECK(summary.contains("error=NODE_NOT_FOUND"));
}

TEST_CASE("[SolersToolRegistry] tool.search token match finds deferred tools") {
	SolersObservationService observation_service;
	SolersPermissionManager permissions;
	SolersReflectionService reflection_service;
	SolersResourceService resource_service;
	SolersScriptService script_service;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);

	SolersToolRegistry registry;
	registry.set_observation_service(&observation_service);
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection_service);
	registry.set_resource_service(&resource_service);
	registry.set_script_service(&script_service);
	registry.register_default_tools();

	Dictionary editor_query = search_deferred_tools(registry, "editor invoke save_scene", 5);
	REQUIRE((bool)editor_query.get("ok", false));
	CHECK(search_result_has_tool(editor_query, "editor.invoke"));

	Dictionary reordered_query = search_deferred_tools(registry, "save scene editor", 5);
	REQUIRE((bool)reordered_query.get("ok", false));
	CHECK(search_result_has_tool(reordered_query, "editor.invoke"));

	Dictionary export_query = search_deferred_tools(registry, "export preset", 10);
	REQUIRE((bool)export_query.get("ok", false));
	const bool found_export_tool = search_result_has_tool(export_query, "export.list_presets") ||
			search_result_has_tool(export_query, "export.run_preset") ||
			search_result_has_tool(export_query, "export.validate_presets");
	CHECK(found_export_tool);

	Dictionary resource_query = search_deferred_tools(registry, "resource create", 10);
	REQUIRE((bool)resource_query.get("ok", false));
	CHECK(search_result_has_tool(resource_query, "resource.create"));
}

TEST_CASE("[SolersToolRegistry] tool.search never returns direct tools") {
	SolersObservationService observation_service;
	SolersPermissionManager permissions;
	SolersReflectionService reflection_service;
	SolersResourceService resource_service;
	SolersScriptService script_service;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);

	SolersToolRegistry registry;
	registry.set_observation_service(&observation_service);
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection_service);
	registry.set_resource_service(&resource_service);
	registry.set_script_service(&script_service);
	registry.register_default_tools();

	Dictionary result = search_deferred_tools(registry, "property", 20);
	REQUIRE((bool)result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	Array matches = data.get("tools", Array());
	for (int i = 0; i < matches.size(); i++) {
		const Dictionary tool = matches[i];
		CHECK(tool.get("exposure", String()) == "deferred");
	}
}

TEST_CASE("[SolersToolRegistry] tool.search discovers synthetic deferred tools by schema text") {
	SolersObservationService observation_service;
	SolersPermissionManager permissions;
	SolersReflectionService reflection_service;
	SolersResourceService resource_service;
	SolersScriptService script_service;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);

	SolersToolRegistry registry;
	registry.set_observation_service(&observation_service);
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection_service);
	registry.set_resource_service(&resource_service);
	registry.set_script_service(&script_service);
	registry.register_default_tools();

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_kind = "none";

	Dictionary payload_schema;
	payload_schema["description"] = "Accepts schemaonlyneedle data from any future tool.";
	Dictionary properties;
	properties["payload"] = payload_schema;
	Dictionary schema;
	schema["type"] = "object";
	schema["properties"] = properties;

	registry.register_tool(memnew(SolersFunctionTool(
			StringName("synthetic.opaque"),
			"Opaque deferred fixture.",
			schema, SolersToolExposure::DEFERRED, cap,
			[](const SolersToolContext &, const Dictionary &) {
				Dictionary result;
				result["ok"] = true;
				return result;
			})));

	Dictionary result = search_deferred_tools(registry, "schemaonlyneedle", 5);
	REQUIRE((bool)result.get("ok", false));
	CHECK(search_result_has_tool(result, "synthetic.opaque"));
}

TEST_CASE("[SolersToolRegistry] normalize_tool_args is public and idempotent") {
	SolersToolRegistry registry;

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_kind = "none";

	Dictionary schema;
	schema["type"] = "object";
	Dictionary properties;
	properties["value"] = Dictionary();
	properties["optional_empty"] = Dictionary();
	properties["required_empty"] = Dictionary();
	schema["properties"] = properties;
	Array required;
	required.push_back("required_empty");
	schema["required"] = required;

	registry.register_tool(memnew(SolersFunctionTool(
			StringName("synthetic.normalize"),
			"Normalizes optional empty args.",
			schema, SolersToolExposure::DIRECT, cap,
			[](const SolersToolContext &, const Dictionary &) {
				Dictionary result;
				result["ok"] = true;
				return result;
			})));

	Dictionary args;
	args["value"] = "kept";
	args["optional_empty"] = "";
	args["required_empty"] = "";
	args["unknown_empty"] = "";

	Dictionary normalized = registry.normalize_tool_args("synthetic.normalize", args);
	CHECK(normalized.get("value", String()) == "kept");
	CHECK_FALSE(normalized.has("optional_empty"));
	CHECK((bool)normalized.has("required_empty"));
	CHECK((bool)normalized.has("unknown_empty"));
	Dictionary normalized_again = registry.normalize_tool_args("synthetic.normalize", normalized);
	CHECK(normalized_again.size() == normalized.size());
	CHECK(normalized_again.get("value", String()) == "kept");
	CHECK((bool)normalized_again.has("required_empty"));
}

TEST_CASE("[SolersAgentSession] repeated failure fingerprint uses normalized redacted args") {
	SolersToolRegistry registry;

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_kind = "file_write";
	cap.redact_args.push_back("content");

	Dictionary schema;
	schema["type"] = "object";
	Dictionary properties;
	properties["path"] = Dictionary();
	properties["content"] = Dictionary();
	properties["content_base64"] = Dictionary();
	schema["properties"] = properties;
	Array required;
	required.push_back("path");
	schema["required"] = required;

	registry.register_tool(memnew(SolersFunctionTool(
			StringName("synthetic.write"),
			"Writable fixture.",
			schema, SolersToolExposure::DIRECT, cap,
			[](const SolersToolContext &, const Dictionary &) {
				Dictionary result;
				result["ok"] = false;
				Dictionary error;
				error["code"] = "INVALID_ARGUMENT";
				result["error"] = error;
				return result;
			})));

	Dictionary first_args;
	first_args["path"] = "res://same.gd";
	first_args["content"] = "first payload";
	first_args["content_base64"] = "";

	Dictionary second_args;
	second_args["path"] = "res://same.gd";
	second_args["content"] = "second payload";

	Dictionary first = registry.redact_tool_args_for_fingerprint("synthetic.write", registry.normalize_tool_args("synthetic.write", first_args));
	Dictionary second = registry.redact_tool_args_for_fingerprint("synthetic.write", registry.normalize_tool_args("synthetic.write", second_args));

	CHECK(first.get("path", String()) == second.get("path", String()));
	CHECK(first.get("content", String()) == "<redacted>");
	CHECK(second.get("content", String()) == "<redacted>");
	CHECK_FALSE(first.has("content_base64"));
	CHECK_FALSE(second.has("content_base64"));
	CHECK(first.size() == second.size());
}

TEST_CASE("[SolersReflectionService] batch dispatches generic scene mutation ops") {
	SolersReflectionService reflection_service;

	const char *ops[] = {
		"create_node",
		"set_property",
		"reparent",
		"connect_signal",
		"attach_script",
		"remove_node",
	};
	for (const char *op_name : ops) {
		Dictionary op;
		op["op"] = op_name;
		op["class_name"] = "Node";
		op["type"] = "Node";
		op["name"] = "Synthetic";
		op["property"] = "name";
		op["value"] = "Synthetic";
		op["node_path"] = "Synthetic";
		op["new_parent_path"] = ".";
		op["source_path"] = ".";
		op["target_path"] = ".";
		op["signal"] = "ready";
		op["method"] = "_ready";
		op["script_path"] = "res://synthetic.gd";

		Array operations;
		operations.push_back(op);
		Dictionary args;
		args["operations"] = operations;

		Dictionary result = reflection_service.batch(args);
		CHECK(result.get("ok", false));
		Dictionary data = result.get("data", Dictionary());
		CHECK_FALSE((bool)data.get("completed", true));
		Array entries = data.get("results", Array());
		REQUIRE(entries.size() == 1);
		Dictionary entry = entries[0];
		Dictionary op_result = entry.get("result", Dictionary());
		Dictionary error = op_result.get("error", Dictionary());
		CHECK(error.get("code", String()) != "UNKNOWN_OP");
	}

	Dictionary unknown_op;
	unknown_op["op"] = "synthetic_future_op";
	Array unknown_ops;
	unknown_ops.push_back(unknown_op);
	Dictionary args;
	args["operations"] = unknown_ops;
	Dictionary unknown = reflection_service.batch(args);
	Dictionary data = unknown.get("data", Dictionary());
	Array entries = data.get("results", Array());
	REQUIRE(entries.size() == 1);
	Dictionary entry = entries[0];
	Dictionary op_result = entry.get("result", Dictionary());
	Dictionary error = op_result.get("error", Dictionary());
	CHECK(error.get("code", String()) == "UNKNOWN_OP");
}

TEST_CASE("[SolersObservationService] empty file search lists bounded project files") {
	SolersObservationService observation_service;
	Dictionary result = observation_service.search_project_files("  ", 4);
	CHECK(result.get("ok", false));
	CHECK(result.get("mode", String()) == "list_all");
	CHECK(result.has("files"));
	CHECK((int)result.get("count", -1) >= 0);
}

TEST_CASE("[SolersResourceService] native Resource path flow creates edits loads and assigns") {
	const String path = "res://.solers_resource_contract.tres";
	const String fs_path = ProjectSettings::get_singleton()->globalize_path(path);
	if (FileAccess::exists(path)) {
		DirAccess::remove_file_or_error(fs_path);
	}

	SolersResourceService resource_service;

	Dictionary create_args;
	create_args["class_name"] = "Resource";
	create_args["path"] = path;
	Dictionary created = resource_service.create_resource(create_args);
	REQUIRE(created.get("ok", false));

	Dictionary get_args;
	get_args["path"] = path;
	get_args["property"] = "resource_name";
	Dictionary set_initial_args = get_args;
	set_initial_args["value"] = "contract initial";
	Dictionary set_initial = resource_service.set_resource_property(set_initial_args);
	REQUIRE(set_initial.get("ok", false));

	Dictionary read = resource_service.get_resource_property(get_args);
	REQUIRE(read.get("ok", false));
	Dictionary read_data = read.get("data", Dictionary());
	CHECK(read_data.get("value", String()) == "contract initial");

	Dictionary set_args = get_args;
	set_args["value"] = "contract updated";
	Dictionary set = resource_service.set_resource_property(set_args);
	REQUIRE(set.get("ok", false));

	Dictionary call_args;
	call_args["path"] = path;
	call_args["method"] = "get_path";
	Dictionary call = resource_service.call_resource_method(call_args);
	REQUIRE(call.get("ok", false));
	Dictionary call_data = call.get("data", Dictionary());
	CHECK(call_data.get("result", String()) == path);

	Ref<Resource> loaded = ResourceLoader::load(path, "Resource");
	REQUIRE(loaded.is_valid());
	CHECK(loaded->get_name() == "contract updated");

	ResourcePreloader node;
	Vector<String> names;
	names.push_back("loaded");
	Array resources;
	resources.push_back(loaded);
	Array assigned;
	assigned.push_back(names);
	assigned.push_back(resources);
	bool valid = false;
	node.set("resources", assigned, &valid);
	CHECK(valid);
	CHECK(node.get_resource("loaded") == loaded);

	DirAccess::remove_file_or_error(fs_path);
}

TEST_CASE("[SolersReflectionService] Godot indexed paths reach nested resource properties") {
	WorldEnvironment node;
	Ref<Environment> environment;
	environment.instantiate();
	node.set_environment(environment);

	Vector<StringName> property_path;
	property_path.push_back(SNAME("environment"));
	property_path.push_back(SNAME("ambient_light_energy"));
	bool valid = false;
	node.set_indexed(property_path, 0.42, &valid);
	CHECK(valid);
	const Variant value = node.get_indexed(property_path, &valid);
	CHECK(valid);
	CHECK(Math::is_equal_approx((double)value, 0.42));
}

TEST_CASE("[SolersPermissionManager] auto approve all resolves without pending") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_all(true);
	Dictionary request = permissions.request_user_approval("synthetic.auto", Dictionary(), SolersPermissionManager::PERMISSION_EDIT_SCENE);
	CHECK(permissions.get_pending_request_count() == 0);
	CHECK(permissions.get_request_decision(request.get("id", 0)) == SolersPermissionManager::DECISION_APPROVED);
	CHECK(permissions.consume_approval(request.get("id", 0), "synthetic.auto"));
}

TEST_CASE("[SolersLLMEvent] represents streaming reasoning as canonical events") {
	Dictionary event = SolersLLMEvent::reasoning_delta("Inspecting the scene tree.");

	CHECK(event.get("kind", String()) == String(SolersLLMEventKind::REASONING_DELTA));
	CHECK(event.get("text", String()) == "Inspecting the scene tree.");
}

TEST_CASE("[SolersLLMProviderCatalog] resolves OpenAI and unknown custom providers to chat completions") {
	SolersLLMProviderCatalog catalog;
	catalog.register_builtin_profiles();

	Dictionary openai = catalog.resolve(StringName("openai"), "");
	Dictionary custom = catalog.resolve(StringName("open-ai"), "https://ai.nodeseek.in/v1");

	CHECK(openai.get("protocol", String()) == "openai-chat");
	CHECK(custom.get("id", String()) == "open-ai");
	CHECK(custom.get("protocol", String()) == "openai-chat");
	CHECK(custom.get("base_url", String()) == "https://ai.nodeseek.in/v1");
}

TEST_CASE("[SolersOpenAIChatProtocol] starts chat completions with store disabled") {
	Array messages;
	messages.push_back(SolersLLMMessage::user("What is in this project?"));

	Dictionary request;
	request["model"] = "gpt-5.5";
	request["messages"] = messages;
	request["reasoning_effort"] = "high";

	SolersOpenAIChatProtocol protocol;
	Dictionary body = protocol.build_request_body(request);
	Array lowered = body.get("messages", Array());

	REQUIRE(lowered.size() == 1);
	Dictionary user = lowered[0];

	CHECK_FALSE(body.get("store", true));
	CHECK(body.get("stream", false));
	Dictionary stream_options = body.get("stream_options", Dictionary());
	CHECK(stream_options.get("include_usage", false));
	CHECK(body.get("reasoning_effort", String()) == "high");
	CHECK(user.get("role", String()) == "user");
}

TEST_CASE("[SolersOpenAIChatProtocol] does not replay Responses item ids as Chat tool call ids") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());

	Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"fc_item_1","type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == "call_solers_0");
	CHECK(input_start.get("name", String()) == "project_get_info");
	CHECK(input_start.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	bool saw_tool_call = false;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		if (event.get("kind", String()) == String(SolersLLMEventKind::TOOL_CALL)) {
			saw_tool_call = true;
			CHECK(event.get("id", String()) == "call_solers_0");
			CHECK(event.get("name", String()) == "project_get_info");
			CHECK(event.get("arguments", String()) == "{}");
		}
	}
	CHECK(saw_tool_call);
}

TEST_CASE("[SolersOpenAIChatProtocol] rejects Responses item ids even when exposed as call_id") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());

	Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"fc_item_1","call_id":"fc_item_1","type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == "call_solers_0");
	CHECK(input_start.get("name", String()) == "project_get_info");
	CHECK(input_start.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	bool saw_tool_call = false;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		if (event.get("kind", String()) == String(SolersLLMEventKind::TOOL_CALL)) {
			saw_tool_call = true;
			CHECK(event.get("id", String()) == "call_solers_0");
			CHECK(event.get("name", String()) == "project_get_info");
			CHECK(event.get("arguments", String()) == "{}");
		}
	}
	CHECK(saw_tool_call);
}

TEST_CASE("[SolersOpenAIChatProtocol] prefers explicit call_id over leaked Responses item id") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());

	Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"fc_item_1","call_id":"call_real_1","type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == "call_real_1");
	CHECK(input_start.get("name", String()) == "project_get_info");
	CHECK(input_start.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	bool saw_tool_call = false;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		if (event.get("kind", String()) == String(SolersLLMEventKind::TOOL_CALL)) {
			saw_tool_call = true;
			CHECK(event.get("id", String()) == "call_real_1");
			CHECK(event.get("name", String()) == "project_get_info");
			CHECK(event.get("arguments", String()) == "{}");
		}
	}
	CHECK(saw_tool_call);
}

TEST_CASE("[SolersLLMProtocol] streams tool input before the executable tool call") {
	SolersAnthropicMessagesProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());

	Array events = protocol.parse_event(state, "content_block_start", R"json({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"call_1","name":"project_get_info","input":{}}})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == "call_1");
	CHECK(input_start.get("name", String()) == "project_get_info");

	events = protocol.parse_event(state, "content_block_delta", R"json({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{}"}})json");
	Dictionary input_delta = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_DELTA);
	REQUIRE_FALSE(input_delta.is_empty());
	CHECK(input_delta.get("arguments_delta", String()) == "{}");
	CHECK(input_delta.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "content_block_stop", R"json({"type":"content_block_stop","index":0})json");
	Dictionary tool_call = find_event_kind(events, SolersLLMEventKind::TOOL_CALL);
	REQUIRE_FALSE(tool_call.is_empty());
	CHECK(tool_call.get("id", String()) == "call_1");
	CHECK(tool_call.get("arguments", String()) == "{}");
}

TEST_CASE("[SolersOpenAIChatProtocol] replays one tool call as assistant tool_calls then tool result") {
	Array tool_calls;
	Dictionary call;
	call["id"] = "call_1";
	call["name"] = "project_get_info";
	call["arguments"] = "{}";
	tool_calls.push_back(call);

	Array messages;
	messages.push_back(SolersLLMMessage::user("What is in this project?"));
	messages.push_back(SolersLLMMessage::assistant("", tool_calls));
	messages.push_back(SolersLLMMessage::tool_result("call_1", "project_get_info", "{\"ok\":true}"));

	Dictionary request;
	request["model"] = "gpt-5.5";
	request["messages"] = messages;

	SolersOpenAIChatProtocol protocol;
	Dictionary body = protocol.build_request_body(request);
	Array lowered = body.get("messages", Array());

	REQUIRE(lowered.size() == 3);
	Dictionary user = lowered[0];
	Dictionary assistant = lowered[1];
	Dictionary output = lowered[2];

	CHECK_FALSE(body.get("store", true));
	CHECK(user.get("role", String()) == "user");
	CHECK(assistant.get("role", String()) == "assistant");
	Array native_calls = assistant.get("tool_calls", Array());
	REQUIRE(native_calls.size() == 1);
	Dictionary native_call = native_calls[0];
	Dictionary fn = native_call.get("function", Dictionary());
	CHECK(native_call.get("id", String()) == "call_1");
	CHECK(native_call.get("type", String()) == "function");
	CHECK(fn.get("name", String()) == "project_get_info");
	CHECK(fn.get("arguments", String()) == "{}");
	CHECK(output.get("role", String()) == "tool");
	CHECK(output.get("tool_call_id", String()) == "call_1");
}

TEST_CASE("[SolersOpenAIChatProtocol] preserves one assistant tool_call and tool result per parallel call") {
	Array tool_calls;
	Dictionary first;
	first["id"] = "call_a";
	first["name"] = "project_get_info";
	first["arguments"] = "{}";
	tool_calls.push_back(first);
	Dictionary second;
	second["id"] = "call_b";
	second["name"] = "scene_get_open_scenes";
	second["arguments"] = "{\"max_depth\":1}";
	tool_calls.push_back(second);

	Array messages;
	messages.push_back(SolersLLMMessage::user("Inspect the project."));
	messages.push_back(SolersLLMMessage::assistant("", tool_calls));
	messages.push_back(SolersLLMMessage::tool_result("call_a", "project_get_info", "{\"ok\":true}"));
	messages.push_back(SolersLLMMessage::tool_result("call_b", "scene_get_open_scenes", "{\"scenes\":[]}"));

	Dictionary request;
	request["model"] = "gpt-5.5";
	request["messages"] = messages;

	SolersOpenAIChatProtocol protocol;
	Dictionary body = protocol.build_request_body(request);
	Array lowered = body.get("messages", Array());

	REQUIRE(lowered.size() == 4);
	Dictionary assistant = lowered[1];
	Array native_calls = assistant.get("tool_calls", Array());
	REQUIRE(native_calls.size() == 2);
	Dictionary first_call = native_calls[0];
	Dictionary second_call = native_calls[1];
	Dictionary first_output = lowered[2];
	Dictionary second_output = lowered[3];

	CHECK(first_call.get("id", String()) == "call_a");
	CHECK(second_call.get("id", String()) == "call_b");
	CHECK(first_output.get("role", String()) == "tool");
	CHECK(first_output.get("tool_call_id", String()) == "call_a");
	CHECK(second_output.get("role", String()) == "tool");
	CHECK(second_output.get("tool_call_id", String()) == "call_b");
}

} // namespace TestSolersProviderGateway
