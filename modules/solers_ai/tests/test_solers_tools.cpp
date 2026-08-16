/**************************************************************************/
/*  test_solers_tools.cpp                                                 */
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

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_builtin_skills.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/generated/terrain3d_lock.gen.h"
#include "modules/solers_ai/llm/solers_protocol_anthropic_messages.h"
#include "modules/solers_ai/llm/solers_protocol_openai_chat.h"
#include "modules/solers_ai/llm/solers_protocol_openai_responses.h"
#include "modules/solers_ai/tests/support/solers_test_state.h"
#ifdef MODULE_GLTF_ENABLED
#include "modules/gltf/extensions/gltf_document_extension_convert_importer_mesh.h"
#include "modules/gltf/gltf_document.h"
#endif

TEST_FORCE_LINK(test_solers_tools)

namespace TestSolersTools {

class ScopedEditedSceneRoot {
	SceneTree *tree = nullptr;
	Node *previous = nullptr;

public:
	ScopedEditedSceneRoot(SceneTree *p_tree, Node *p_root) :
			tree(p_tree), previous(p_tree->get_edited_scene_root()) {
		tree->set_edited_scene_root(p_root);
	}

	~ScopedEditedSceneRoot() {
		tree->set_edited_scene_root(previous);
	}
};

Dictionary empty_tool_schema() {
	Dictionary schema;
	schema["type"] = "object";
	schema["properties"] = Dictionary();
	return schema;
}

void check_portable_tool_schema(const Dictionary &p_schema) {
	CHECK(p_schema.get("type", String()) == "object");
	CHECK(p_schema.get("properties", Variant()).get_type() == Variant::DICTIONARY);
	CHECK_FALSE(p_schema.has("oneOf"));
	CHECK_FALSE(p_schema.has("anyOf"));
	CHECK_FALSE(p_schema.has("allOf"));
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
	return !solers_test_find_dictionary(tools, SNAME("name"), p_name).is_empty();
}

TEST_CASE("[SolersToolRegistry] registers tools by lookup, not a hardcoded catalog") {
	// Behavior contract (no-patch): a brand-new tool the dispatcher has never
	// special-cased becomes discoverable + dispatchable purely by registering
	// it — no catalog entry, no dispatcher branch, no name-prefix classifier.
	SolersToolRegistry registry;

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_policy = SolersToolMutationPolicy::READ_ONLY;
	cap.execution = SolersToolExecution::WORKER_THREAD;

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

	Array tools = registry.list_tools();
	REQUIRE(tools.size() == 1);
	Dictionary tool = tools[0];
	CHECK(tool.get("name", String()) == "synthetic.echo");

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

TEST_CASE("[SolersPermissionManager] third-party addon code always requires explicit approval") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_all(true);
	CHECK_FALSE(permissions.is_auto_approved(SolersPermissionManager::PERMISSION_INSTALL_PLUGIN));
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_INSTALL_PLUGIN, true);
	CHECK_FALSE(permissions.get_auto_approve_permission(SolersPermissionManager::PERMISSION_INSTALL_PLUGIN));

	SolersAssetService assets;
	SolersToolRegistry registry;
	registry.set_asset_service(&assets);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	Dictionary third_party;
	third_party["source"] = "assetlib";
	third_party["plugin_id"] = "123";
	third_party["version"] = "1.0.0";
	third_party["sha256"] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	const Dictionary blocked = registry.call_tool("addon.ensure", third_party);
	CHECK_FALSE((bool)blocked.get("ok", true));
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "USER_APPROVAL_REQUIRED");
	CHECK(permissions.get_pending_request_count() == 1);

	Dictionary bundled = third_party.duplicate(true);
	bundled["source"] = "bundled";
	bundled["plugin_id"] = "terrain3d";
	bundled["version"] = SOLERS_TERRAIN3D_VERSION;
	bundled["sha256"] = SOLERS_TERRAIN3D_SHA256;
	Dictionary untrusted_bundle = bundled.duplicate(true);
	untrusted_bundle["sha256"] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	CHECK(SolersAssetService::is_trusted_addon(bundled));
	CHECK_FALSE(SolersAssetService::is_trusted_addon(untrusted_bundle));
	const Dictionary untrusted = registry.call_tool("addon.ensure", untrusted_bundle);
	CHECK_FALSE((bool)untrusted.get("ok", true));
	CHECK(Dictionary(untrusted.get("error", Dictionary())).get("code", String()) == "USER_APPROVAL_REQUIRED");
	CHECK(permissions.get_pending_request_count() == 2);

	const Dictionary reached_handler = registry.call_tool("addon.ensure", bundled);
	CHECK_FALSE((bool)reached_handler.get("ok", true));
	CHECK(Dictionary(reached_handler.get("error", Dictionary())).get("code", String()) == "PLUGIN_INSPECTION_REQUIRED");
}

TEST_CASE("[SolersToolRegistry] schema preflight runs before approval or handler side effects") {
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	int calls = 0;
	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_EDIT_FILES;
	cap.mutation_policy = SolersToolMutationPolicy::IRREVERSIBLE;
	Dictionary amount;
	amount["type"] = "number";
	amount["exclusiveMinimum"] = 0;
	Dictionary properties;
	properties["amount"] = amount;
	Dictionary schema;
	schema["type"] = "object";
	schema["properties"] = properties;
	Array required;
	required.push_back("amount");
	schema["required"] = required;
	schema["additionalProperties"] = false;
	registry.register_tool(memnew(SolersFunctionTool(
			SNAME("synthetic.write"), "Synthetic write fixture.", schema, SolersToolExposure::DIRECT, cap,
			[&calls](const SolersToolContext &, const Dictionary &) {
				calls++;
				Dictionary result;
				result["ok"] = true;
				result["data"] = Dictionary();
				return result;
			})));

	Dictionary invalid_args;
	invalid_args["amount"] = -1.0;
	const Dictionary invalid = registry.call_tool(SNAME("synthetic.write"), invalid_args);
	CHECK_FALSE((bool)invalid.get("ok", true));
	CHECK(Dictionary(invalid.get("error", Dictionary())).get("code", String()) == "TOOL_ARGUMENT_INVALID");
	CHECK(calls == 0);
	CHECK(permissions.get_pending_request_count() == 0);

	Dictionary valid_args;
	valid_args["amount"] = 1.0;
	const Dictionary awaiting = registry.call_tool(SNAME("synthetic.write"), valid_args);
	CHECK_FALSE((bool)awaiting.get("ok", true));
	CHECK(Dictionary(awaiting.get("error", Dictionary())).get("code", String()) == "USER_APPROVAL_REQUIRED");
	CHECK(calls == 0);
	CHECK(permissions.get_pending_request_count() == 1);
}

TEST_CASE("[SolersToolRegistry] preserves internal session context without changing the bound API") {
	SolersToolRegistry registry;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_permission_manager(&permissions);

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_policy = SolersToolMutationPolicy::READ_ONLY;
	registry.register_tool(memnew(SolersFunctionTool(
			"synthetic.context", "Returns its internal execution context.", empty_tool_schema(), SolersToolExposure::DIRECT, cap,
			[](const SolersToolContext &p_context, const Dictionary &) {
				Dictionary data;
				data["session_id"] = p_context.session_id;
				Dictionary result;
				result["ok"] = true;
				result["data"] = data;
				return result;
			})));

	SolersToolContext context;
	context.session_id = "session-from-agent";
	const Dictionary result = registry.call_tool_with_context("synthetic.context", Dictionary(), context);
	CHECK(result.get("ok", false));
	CHECK(Dictionary(result.get("data", Dictionary())).get("session_id", String()) == "session-from-agent");
}

TEST_CASE("[SolersTool] pending work has a distinct continuation callback") {
	int starts = 0;
	int polls = 0;
	SolersToolCapability capability;
	SolersFunctionTool tool(
			"synthetic.pending", "Synthetic pending tool.", Dictionary(), SolersToolExposure::HIDDEN, capability,
			[&starts](const SolersToolContext &, const Dictionary &) {
				starts++;
				Dictionary data;
				data["status"] = "pending";
				Dictionary result;
				result["ok"] = true;
				result["data"] = data;
				return result;
			},
			[&polls](const SolersToolContext &, const Dictionary &) {
				polls++;
				Dictionary data;
				data["status"] = "complete";
				Dictionary result;
				result["ok"] = true;
				result["data"] = data;
				return result;
			});

	CHECK(tool.execute(SolersToolContext(), Dictionary()).get("ok", false));
	CHECK(tool.poll(SolersToolContext(), Dictionary()).get("ok", false));
	CHECK(starts == 1);
	CHECK(polls == 1);
}

TEST_CASE("[SolersBuiltinSkills] registry catalog and content stay consistent without pinned name lists") {
	const int count = SolersBuiltinSkills::get_count();
	REQUIRE(count > 0);

	const String catalog = SolersBuiltinSkills::build_catalog_prompt();
	CHECK(catalog.contains("Built-in Solers skills"));
	CHECK_FALSE(catalog.contains("## Laws"));

	Vector<String> catalog_names;
	const PackedStringArray lines = catalog.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = String(lines[i]).strip_edges();
		if (!line.begins_with("- ") || !line.contains(":")) {
			continue;
		}
		catalog_names.push_back(line.substr(2, line.find(":") - 2).strip_edges());
	}
	CHECK(catalog_names.size() == count);

	for (int i = 0; i < catalog_names.size(); i++) {
		const String name = catalog_names[i];
		SolersBuiltinSkillView skill;
		REQUIRE(SolersBuiltinSkills::find_by_name(name, skill));
		CHECK(skill.name == name);
		CHECK(!skill.description.is_empty());
		CHECK(skill.content.contains("## When to use"));
		CHECK(skill.content.contains("## Verify"));
		CHECK_FALSE(skill.content.contains("photorealism-pipeline"));
		CHECK_FALSE(skill.name == "photorealism-pipeline");
		CHECK_FALSE(skill.name == "destruction-vfx");
		CHECK_FALSE(skill.name == "godot-plugins-terrain");
	}

	SolersBuiltinSkillView rendering;
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-3d-rendering", rendering));
	// Extra parentheses: doctest can only decompose a single comparison, so a
	// chained condition has to reach it as one already-evaluated bool.
	CHECK((rendering.content.contains("one authoritative final GI path") || rendering.content.contains("One authoritative final GI path") || rendering.content.contains("one final GI")));
	CHECK(rendering.content.contains("physical_light_units"));
	CHECK(rendering.description.to_lower().contains("photoreal"));

	SolersBuiltinSkillView camera;
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-camera-cinematography", camera));
	CHECK(camera.content.contains("interpolate_with"));
	CHECK(camera.content.contains("make_current"));
	CHECK(camera.content.contains("godot-3d-rendering"));
	CHECK_FALSE(camera.content.contains("photorealism-pipeline"));

	SolersBuiltinSkillView terrain;
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-procedural-terrain", terrain));
	CHECK(terrain.content.contains("addon.inspect"));
	CHECK_FALSE(terrain.content.contains("Terrain3D"));

	SolersBuiltinSkillView vfx;
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-vfx-particles", vfx));
	CHECK(vfx.content.contains("decompress"));

	SolersBuiltinSkillView shaders;
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-shaders", shaders));
	CHECK(shaders.content.contains("shader_type"));

	SolersBuiltinSkillView assets;
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-project-editor-assets", assets));
	CHECK(assets.content.contains(".import"));
	CHECK(assets.content.contains("asset.capabilities"));
	CHECK_FALSE(assets.content.contains("meshy-6"));
	CHECK_FALSE(assets.content.contains("job.wait"));

	SolersBuiltinSkillView missing;
	CHECK_FALSE(SolersBuiltinSkills::find_by_name("synthetic-never-registered-skill", missing));
	CHECK_FALSE(SolersBuiltinSkills::find_by_name("photorealism-pipeline", missing));
}

TEST_CASE("[SolersToolRegistry] skill.read serves compiled builtin skills") {
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	Dictionary args;
	args["name"] = "godot-3d-rendering";
	const Dictionary result = registry.call_tool(StringName("skill.read"), args);
	REQUIRE((bool)result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("name", String()) == "godot-3d-rendering");
	CHECK(!String(data.get("content", String())).is_empty());
	CHECK(String(data.get("content", String())).contains("physical_light_units"));
	CHECK_FALSE(data.has("required_tools"));
}

TEST_CASE("[SolersToolRegistry] skill.read rejects unknown builtin skills") {
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	Dictionary args;
	args["name"] = "synthetic-never-registered-skill";
	const Dictionary result = registry.call_tool(StringName("skill.read"), args);
	CHECK_FALSE((bool)result.get("ok", true));
	const Dictionary error = result.get("error", Dictionary());
	CHECK(error.get("code", String()) == "UNKNOWN_SKILL");
}

TEST_CASE("[SolersToolRegistry] default tools keep one portable ABI across provider protocols") {
	SolersAssetService asset_service;
	SolersObservationService observation_service;
	SolersPermissionManager permissions;
	SolersReflectionService reflection_service;
	SolersResourceService resource_service;
	SolersScriptService script_service;
	SolersToolRegistry registry;
	registry.set_asset_service(&asset_service);
	registry.set_observation_service(&observation_service);
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection_service);
	registry.set_resource_service(&resource_service);
	registry.set_script_service(&script_service);
	registry.register_default_tools();

	const Array tools = registry.list_tools();
	Array request_tools;
	for (const Variant &item : tools) {
		const Dictionary definition = item;
		check_portable_tool_schema(definition.get("input_schema", Dictionary()));
		Dictionary tool;
		tool["name"] = definition.get("model_name", definition.get("name", String()));
		tool["description"] = definition.get("description", String());
		tool["parameters"] = definition.get("input_schema", Dictionary());
		request_tools.push_back(tool);
	}
	Dictionary request;
	request["model"] = "claude-test";
	request["messages"] = Array();
	request["tools"] = request_tools;
	const Array anthropic = SolersAnthropicMessagesProtocol().build_request_body(request).get("tools", Array());
	const Array chat = SolersOpenAIChatProtocol().build_request_body(request).get("tools", Array());
	const Array responses = SolersOpenAIResponsesProtocol().build_request_body(request).get("tools", Array());
	REQUIRE(anthropic.size() == tools.size());
	REQUIRE(chat.size() == tools.size());
	REQUIRE(responses.size() == tools.size());
	for (int i = 0; i < tools.size(); i++) {
		check_portable_tool_schema(Dictionary(anthropic[i]).get("input_schema", Dictionary()));
		check_portable_tool_schema(Dictionary(Dictionary(chat[i]).get("function", Dictionary())).get("parameters", Dictionary()));
		const Dictionary response_tool = responses[i];
		check_portable_tool_schema(response_tool.get("parameters", Dictionary()));
		CHECK_FALSE(response_tool.get("strict", true));
	}
	const Dictionary transaction = solers_test_find_dictionary(tools, SNAME("name"), "object.transaction");
	const Dictionary operation = Dictionary(Dictionary(Dictionary(transaction.get("input_schema", Dictionary())).get("properties", Dictionary())).get("operations", Dictionary())).get("items", Dictionary());
	const Dictionary operation_properties = operation.get("properties", Dictionary());
	const Array operation_names = Dictionary(operation_properties.get("op", Dictionary())).get("enum", Array());
	CHECK((operation_names.has("update") && !operation_names.has("set_property")));
	CHECK((operation_properties.has("properties") && !operation_properties.has("property") && !operation_properties.has("value")));

	Dictionary invalid;
	invalid["oneOf"] = Array();
	ERR_PRINT_OFF;
	registry.register_tool(memnew(SolersFunctionTool("synthetic.invalid", "Invalid root.", invalid, SolersToolExposure::DIRECT, SolersToolCapability(), [](const SolersToolContext &, const Dictionary &) { return Dictionary(); })));
	ERR_PRINT_ON;
	CHECK(registry.get_tool_count() == tools.size());
}

TEST_CASE("[SolersToolRegistry] ClassDB member queries match whitespace-separated property names") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersReflectionService reflection_service;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection_service);
	registry.register_default_tools();

	Dictionary class_request;
	class_request["class_name"] = "CameraAttributesPhysical";
	class_request["max_members"] = 16;
	class_request["include_inherited"] = true;
	class_request["member_query"] = "exposure_aperture exposure_sensitivity";
	Array classes;
	classes.push_back(class_request);
	Dictionary args;
	args["classes"] = classes;
	const Dictionary result = registry.call_tool(SNAME("engine.describe"), args);
	REQUIRE((bool)result.get("ok", false));
	const Array described = Dictionary(result.get("data", Dictionary())).get("classes", Array());
	REQUIRE(described.size() == 1);
	const Array properties = Dictionary(described[0]).get("properties", Array());
	HashSet<String> names;
	for (int i = 0; i < properties.size(); i++) {
		names.insert(Dictionary(properties[i]).get("name", String()));
	}
	CHECK(names.has("exposure_aperture"));
	CHECK(names.has("exposure_sensitivity"));
}

TEST_CASE("[SolersToolRegistry] Resource facts round-trip through state-checked transactions") {
	const String path = "res://.solers_transaction_contract.tres";
	SolersTestPaths cleanup;
	cleanup.add(path);
	SolersFileCheckpoint checkpoints;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_SCENE, true);
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, true);
	SolersReflectionService reflection_service;
	SolersResourceService resources;
	SolersToolRegistry registry;
	registry.set_file_checkpoint(&checkpoints);
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection_service);
	registry.set_resource_service(&resources);
	registry.register_default_tools();
	SolersToolContext context;
	context.call_id = "create_resource";
	context.session_id = "resource-transaction-contract";

	Dictionary create;
	create["op"] = "create";
	create["class_name"] = "Environment";
	create["path"] = path;
	Array create_operations;
	create_operations.push_back(create);
	Dictionary create_args;
	create_args["scope"] = "resource";
	create_args["operations"] = create_operations;
	const Dictionary created = registry.call_tool_with_context(SNAME("object.transaction"), create_args, context);
	REQUIRE((bool)created.get("ok", false));
	const String sha = FileAccess::get_sha256(path);
	REQUIRE(sha.length() == 64);
	Dictionary query;
	query["target"] = "resource";
	query["path"] = path;
	Array queried_properties;
	queried_properties.push_back("background_color");
	query["properties"] = queried_properties;
	context.call_id = "query_resource";
	const Dictionary observed = registry.call_tool_with_context(SNAME("object.query"), query, context);
	REQUIRE((bool)observed.get("ok", false));
	Dictionary expected_state = observed.get("data", Dictionary());

	Dictionary update;
	update["op"] = "update";
	update["path"] = path;
	expected_state["sha256"] = String("0000000000000000000000000000000000000000000000000000000000000000");
	update["expected_state"] = expected_state;
	Dictionary properties;
	properties["background_color"] = solers_summarize_display_value(Color(0.25, 0.5, 0.75, 1.0));
	update["properties"] = properties;
	Array update_operations;
	update_operations.push_back(update);
	Dictionary update_args;
	update_args["scope"] = "resource";
	update_args["operations"] = update_operations;
	context.call_id = "stale_resource";
	const Dictionary stale = registry.call_tool_with_context(SNAME("object.transaction"), update_args, context);
	CHECK_FALSE((bool)stale.get("ok", true));
	CHECK(Dictionary(stale.get("error", Dictionary())).get("code", String()) == "RESOURCE_STATE_CONFLICT");
	CHECK(FileAccess::get_sha256(path) == sha);

	expected_state["sha256"] = sha;
	update["expected_state"] = expected_state;
	update["properties"] = properties;
	update_operations[0] = update;
	update_args["operations"] = update_operations;
	context.call_id = "update_resource";
	const Dictionary updated = registry.call_tool_with_context(SNAME("object.transaction"), update_args, context);
	REQUIRE((bool)updated.get("ok", false));
	const Dictionary mutation = Dictionary(updated.get("data", Dictionary())).get("mutation", Dictionary());
	CHECK(mutation.has("session_revision"));
	REQUIRE_FALSE(String(mutation.get("reversal_id", String())).is_empty());
	CHECK_FALSE(mutation.has("authored_revision"));
	const Dictionary receipt = mutation.get("receipt", Dictionary());
	CHECK(Array(receipt.get("resources_before", Array())).size() == 1);
	CHECK(Array(receipt.get("resources_after", Array())).size() == 1);
	const String updated_sha = Dictionary(Array(receipt.get("resources_after", Array()))[0]).get("sha256", String());
	CHECK(updated_sha.length() == 64);
	context.call_id = "verify_resource";
	const Dictionary verified = registry.call_tool_with_context(SNAME("object.query"), query, context);
	const Dictionary color = Dictionary(Dictionary(Dictionary(verified.get("data", Dictionary())).get("properties", Dictionary())).get("background_color", Dictionary())).get("value", Dictionary());
	CHECK(Math::is_equal_approx((double)color.get("r", 0.0), 0.25));
	CHECK(Math::is_equal_approx((double)color.get("b", 0.0), 0.75));

	update["expected_state"] = verified.get("data", Dictionary());
	properties["background_color"] = "Color(1, 0, 0, 1)";
	update["properties"] = properties;
	update_operations[0] = update;
	update_args["operations"] = update_operations;
	context.call_id = "reject_scalar_color";
	CHECK_FALSE((bool)registry.call_tool_with_context(SNAME("object.transaction"), update_args, context).get("ok", true));
	CHECK(FileAccess::get_sha256(path) == updated_sha);

	Dictionary revert_args;
	revert_args["reversal_id"] = mutation.get("reversal_id", String());
	context.call_id = "revert_resource";
	const Dictionary reverted = registry.call_tool_with_context(SNAME("history.revert"), revert_args, context);
	INFO(JSON::stringify(reverted));
	REQUIRE((bool)reverted.get("ok", false));
	CHECK(FileAccess::get_sha256(path) == sha);
}

TEST_CASE("[SolersToolRegistry] built-ins retain deferred exposure and remain discoverable") {
	SolersObservationService observation_service;
	SolersPermissionManager permissions;
	SolersResourceService resource_service;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_observation_service(&observation_service);
	registry.set_permission_manager(&permissions);
	registry.set_resource_service(&resource_service);
	registry.register_default_tools();
	const Array catalog = registry.list_tools();
	CHECK(solers_test_find_dictionary(catalog, SNAME("name"), "tool.search").get("exposure", String()) == "direct");
	CHECK(solers_test_find_dictionary(catalog, SNAME("name"), "export.run_preset").get("exposure", String()) == "deferred");
	const Dictionary result = search_deferred_tools(registry, "export preset", 10);
	CHECK((bool)result.get("ok", false));
	CHECK(search_result_has_tool(result, "export.run_preset"));
	CHECK(Array(Dictionary(result.get("data", Dictionary())).get("unlock_tools", Array())).has("export.run_preset"));
}

TEST_CASE("[SolersToolRegistry] deferred search prioritizes exact ids and never returns direct tools") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_policy = SolersToolMutationPolicy::READ_ONLY;
	auto add_external = [&](const StringName &p_name, const String &p_description) {
		registry.register_tool(memnew(SolersFunctionTool(
				p_name, p_description, empty_tool_schema(), SolersToolExposure::DEFERRED, cap,
				[](const SolersToolContext &, const Dictionary &) {
					Dictionary result;
					result["ok"] = true;
					return result;
				})));
	};
	add_external(SNAME("plugin.mesh.inspect"), "Inspect an external plugin mesh.");
	add_external(SNAME("plugin.mesh.repair"), "Repair an external plugin mesh.");

	Dictionary result = search_deferred_tools(registry, "plugin.mesh.inspect", 1);
	REQUIRE((bool)result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	Array matches = data.get("tools", Array());
	REQUIRE(matches.size() == 1);
	CHECK(Dictionary(matches[0]).get("name", String()) == "plugin.mesh.inspect");

	result = search_deferred_tools(registry, "property", 20);
	REQUIRE((bool)result.get("ok", false));
	matches = Dictionary(result.get("data", Dictionary())).get("tools", Array());
	for (int i = 0; i < matches.size(); i++) {
		const Dictionary tool = matches[i];
		CHECK(tool.get("exposure", String()) == "deferred");
	}
	CHECK_FALSE(search_result_has_tool(result, "object.transaction"));
}

TEST_CASE("[SolersToolRegistry] tool.search uses Godot fuzzy fallback for external metadata") {
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
	cap.mutation_policy = SolersToolMutationPolicy::READ_ONLY;

	Dictionary payload_schema;
	payload_schema["description"] = "Accepts metadataquartz data from any future tool.";
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
	registry.register_tool(memnew(SolersFunctionTool(
			StringName("synthetic.needle"),
			"Name-priority deferred fixture.",
			empty_tool_schema(), SolersToolExposure::DEFERRED, cap,
			[](const SolersToolContext &, const Dictionary &) {
				Dictionary result;
				result["ok"] = true;
				return result;
			})));

	Dictionary result = search_deferred_tools(registry, "metadataquartz", 5);
	REQUIRE((bool)result.get("ok", false));
	CHECK(search_result_has_tool(result, "synthetic.opaque"));
	result = search_deferred_tools(registry, "synthetic.needle", 1);
	REQUIRE((bool)result.get("ok", false));
	const Array matches = Dictionary(result.get("data", Dictionary())).get("tools", Array());
	REQUIRE(matches.size() == 1);
	CHECK(Dictionary(matches[0]).get("name", String()) == "synthetic.needle");
}

TEST_CASE("[SolersToolRegistry] normalize_tool_args is public and idempotent") {
	SolersToolRegistry registry;

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_policy = SolersToolMutationPolicy::READ_ONLY;

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

TEST_CASE("[SolersToolRegistry] resource access is parameter-aware and failure conflicts only block writes") {
	SolersToolRegistry registry;
	SolersToolCapability capability;
	capability.resource_access = [](const Dictionary &p_args) {
		Array accesses;
		Dictionary access;
		access["mode"] = p_args.get("mode", "read");
		access["key"] = "project:" + String(p_args.get("path", String()));
		accesses.push_back(access);
		return accesses;
	};
	registry.register_tool(memnew(SolersFunctionTool("test.resource", "test", empty_tool_schema(), SolersToolExposure::HIDDEN, capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary(); })));

	Dictionary failed_args;
	failed_args["mode"] = "read";
	failed_args["path"] = "res://a.tres";
	const Array failed = registry.resolve_resource_access("test.resource", failed_args);
	CHECK(registry.is_read_only("test.resource", failed_args));

	Dictionary same_write_args;
	same_write_args["mode"] = "write";
	same_write_args["path"] = "res://a.tres";
	CHECK(SolersToolRegistry::has_write_conflict(failed, registry.resolve_resource_access("test.resource", same_write_args)));

	Dictionary other_write_args = same_write_args.duplicate();
	other_write_args["path"] = "res://b.tres";
	CHECK_FALSE(SolersToolRegistry::has_write_conflict(failed, registry.resolve_resource_access("test.resource", other_write_args)));
	CHECK_FALSE(SolersToolRegistry::has_write_conflict(failed, failed));
}

TEST_CASE("[SolersToolRegistry] audit redaction normalizes payload fields") {
	SolersToolRegistry registry;

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_policy = SolersToolMutationPolicy::IRREVERSIBLE;
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

	Dictionary first = registry.redact_tool_args_for_audit("synthetic.write", registry.normalize_tool_args("synthetic.write", first_args));
	Dictionary second = registry.redact_tool_args_for_audit("synthetic.write", registry.normalize_tool_args("synthetic.write", second_args));

	CHECK(first.get("path", String()) == second.get("path", String()));
	CHECK(first.get("content", String()) == "<redacted>");
	CHECK(second.get("content", String()) == "<redacted>");
	CHECK_FALSE(first.has("content_base64"));
	CHECK_FALSE(second.has("content_base64"));
	CHECK(first.size() == second.size());
}

#ifdef MODULE_GLTF_ENABLED
TEST_CASE("[SolersToolRegistry][SceneTree][GLTF] object.query returns native mesh handles and partial failures") {
	Ref<GLTFDocumentExtensionConvertImporterMesh> conversion;
	conversion.instantiate();
	GLTFDocument::register_gltf_document_extension(conversion);
	Ref<GLTFDocument> document;
	document.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	const Error load_error = document->append_from_file(TestUtils::get_data_path("models/suzanne.glb"), state);
	Node *root = load_error == OK ? document->generate_scene(state) : nullptr;
	GLTFDocument::unregister_gltf_document_extension(conversion);
	REQUIRE(root != nullptr);
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(root->get_node_or_null(NodePath("Suzanne")));
	REQUIRE(mesh_instance != nullptr);

	SolersReflectionService service;
	SolersObservationService observation;
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_reflection_service(&service);
	registry.set_observation_service(&observation);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	SceneTree *tree = SceneTree::get_singleton();
	Dictionary missing;
	{
		ScopedEditedSceneRoot edited_scene(tree, root);
		SolersToolContext context;
		Dictionary args({ { "target", "scene" }, { "node_paths", Array({ "Suzanne", "Missing" }) }, { "properties", Array({ "mesh", "missing_property" }) } });
		const Dictionary result = registry.call_tool_with_context(SNAME("object.query"), args, context);
		const Dictionary data = result.get("data", Dictionary());
		const Array nodes = data.get("nodes", Array());
		REQUIRE(nodes.size() == 1);
		const Dictionary material = Dictionary(nodes[0]).get("material", Dictionary());
		const Dictionary mesh_handle = material.get("mesh", Dictionary());
		CHECK(mesh_handle.get("kind", String()) == "godot_object");
		CHECK((bool)mesh_handle.get("valid", false));
		CHECK(Array(data.get("query_errors", Array())).size() == 1);
		CHECK(Dictionary(nodes[0]).has("property_errors"));
		Variant coerced;
		String error;
		REQUIRE(solers_coerce_property_value(mesh_instance, SNAME("mesh"), mesh_handle, coerced, error));
		CHECK(Ref<Mesh>(coerced) == mesh_instance->get_mesh());
		args.erase("node_paths");
		args["name_contains"] = "zann";
		const Array filtered = Array(Dictionary(registry.call_tool_with_context(SNAME("object.query"), args, context).get("data", Dictionary())).get("nodes", Array()));
		REQUIRE_FALSE(filtered.is_empty());
		for (int i = 0; i < filtered.size(); i++) {
			CHECK(String(Dictionary(filtered[i]).get("name", String())).findn("zann") >= 0);
		}
		args["node_paths"] = Array({ "Missing" });
		args.erase("name_contains");
		missing = registry.call_tool_with_context(SNAME("object.query"), args, context);
	}
	memdelete(root);
	CHECK(Dictionary(missing.get("error", Dictionary())).get("code", String()) == "NODE_QUERY_FAILED");
}
#endif

TEST_CASE("[SolersToolRegistry] scene object transaction access follows its native scope") {
	SolersReflectionService reflection_service;
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection_service);
	registry.register_default_tools();

	Dictionary write_op;
	write_op["op"] = "update";
	write_op["node_path"] = "ReferenceCamera";
	write_op["properties"] = Dictionary({ { "fov", 55.0 } });
	Array write_operations;
	write_operations.push_back(write_op);
	Dictionary write_args;
	write_args["scope"] = "scene";
	write_args["operations"] = write_operations;
	CHECK_FALSE(registry.is_read_only("object.transaction", write_args));
	const Array write_access = registry.resolve_resource_access("object.transaction", write_args);
	REQUIRE(write_access.size() == 1);
	CHECK(Dictionary(write_access[0]).get("mode", String()) == "write");
	CHECK(Dictionary(write_access[0]).get("key", String()) == "scene:ReferenceCamera");

	Dictionary instantiate;
	instantiate["op"] = "instantiate";
	instantiate["resource_path"] = "res://props/tree.glb";
	instantiate["parent_path"] = "Environment";
	Array instantiate_operations;
	instantiate_operations.push_back(instantiate);
	Dictionary instantiate_args;
	instantiate_args["scope"] = "scene";
	instantiate_args["operations"] = instantiate_operations;
	const Array instantiate_access = registry.resolve_resource_access("object.transaction", instantiate_args);
	REQUIRE(instantiate_access.size() == 2);
	CHECK(Dictionary(instantiate_access[0]).get("mode", String()) == "read");
	CHECK(Dictionary(instantiate_access[0]).get("key", String()) == "project:res://props/tree.glb");
	CHECK(Dictionary(instantiate_access[1]).get("key", String()) == "scene:Environment");
}

TEST_CASE("[SolersToolRegistry] scene.open is a thin EditorInterface open surface") {
	SolersReflectionService reflection_service;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_SCENE, true);
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection_service);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	const Dictionary tool = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "scene.open");
	REQUIRE_FALSE(tool.is_empty());
	CHECK(tool.get("permission", String()) == "observe");
	CHECK(tool.get("mutation_policy", String()) == "irreversible");
	CHECK(tool.get("exposure", String()) == "direct");
	CHECK_FALSE(registry.affects_scene_state(SNAME("scene.open")));
	const Dictionary schema = tool.get("input_schema", Dictionary());
	CHECK(Dictionary(schema.get("properties", Dictionary())).has("path"));
	CHECK(Array(schema.get("required", Array())).has("path"));

	Dictionary missing_path;
	const Dictionary missing_path_result = registry.call_tool(SNAME("scene.open"), missing_path);
	CHECK_FALSE((bool)missing_path_result.get("ok", true));
	const String missing_path_code = Dictionary(missing_path_result.get("error", Dictionary())).get("code", String());
	CHECK((missing_path_code == "SCENE_PATH_REQUIRED" || missing_path_code == "TOOL_ARGUMENT_INVALID"));

	Dictionary missing_file;
	missing_file["path"] = "res://definitely_missing_solers_scene_open_contract.tscn";
	const Dictionary missing_file_result = registry.call_tool(SNAME("scene.open"), missing_file);
	CHECK_FALSE((bool)missing_file_result.get("ok", true));
	const String missing_code = Dictionary(missing_file_result.get("error", Dictionary())).get("code", String());
	CHECK((missing_code == "SCENE_NOT_FOUND" || missing_code == "EDITOR_UNAVAILABLE"));
}

TEST_CASE("[SolersPermissionManager] auto approve all resolves without pending") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_all(true);
	Dictionary request = permissions.request_user_approval("synthetic.auto", Dictionary(), SolersPermissionManager::PERMISSION_EDIT_SCENE);
	CHECK(permissions.get_pending_request_count() == 0);
	CHECK(permissions.get_request_decision(request.get("id", 0)) == SolersPermissionManager::DECISION_APPROVED);
	CHECK(permissions.consume_approval(request.get("id", 0), "synthetic.auto"));
}

TEST_CASE("[SolersToolRegistry] direct asset jobs declare their project writes") {
	SolersAssetService assets;
	SolersToolRegistry registry;
	registry.set_asset_service(&assets);
	registry.register_default_tools();

	Dictionary generate_args;
	generate_args["kind"] = "synthetic_kind";
	generate_args["name"] = "contract_asset";
	generate_args["target_dir"] = "res://generated/contract_asset";
	const Array generate_accesses = registry.resolve_resource_access(SNAME("asset.generate"), generate_args);
	REQUIRE(generate_accesses.size() == 2);
	CHECK(Dictionary(generate_accesses[0]).get("mode", String()) == "write");
	CHECK(String(Dictionary(generate_accesses[0]).get("key", String())).begins_with("asset-job:"));
	CHECK(Dictionary(generate_accesses[1]).get("mode", String()) == "write");
	CHECK(Dictionary(generate_accesses[1]).get("key", String()) == "project:res://generated/contract_asset");

	Dictionary first_search;
	const Dictionary catalog_tool = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "asset.catalog.search");
	const Dictionary catalog_properties = Dictionary(catalog_tool.get("input_schema", Dictionary())).get("properties", Dictionary());
	const Array providers = Dictionary(catalog_properties.get("provider", Dictionary())).get("enum", Array());
	const Array kinds = Dictionary(catalog_properties.get("kind", Dictionary())).get("enum", Array());
	REQUIRE_FALSE(providers.is_empty());
	REQUIRE_FALSE(kinds.is_empty());
	first_search["provider"] = providers[0];
	first_search["kind"] = kinds[0];
	first_search["query"] = "books";
	const Array catalog_accesses = registry.resolve_resource_access(SNAME("asset.catalog.search"), first_search);
	REQUIRE(catalog_accesses.size() == 1);
	CHECK(Dictionary(catalog_accesses[0]).get("mode", String()) == "write");
	CHECK(String(Dictionary(catalog_accesses[0]).get("key", String())).begins_with("asset-catalog-directory:"));
	Dictionary second_search = first_search.duplicate(true);
	second_search["query"] = "picture frame";
	const Array second_catalog_accesses = registry.resolve_resource_access(SNAME("asset.catalog.search"), second_search);
	CHECK(SolersToolRegistry::has_write_conflict(catalog_accesses, second_catalog_accesses));
	CHECK(SolersToolRegistry::has_write_conflict(catalog_accesses, registry.resolve_resource_access(SNAME("asset.catalog.search"), first_search)));
	Dictionary other_kind_search = first_search.duplicate(true);
	other_kind_search["kind"] = "synthetic_other_kind";
	CHECK_FALSE(SolersToolRegistry::has_write_conflict(catalog_accesses, registry.resolve_resource_access(SNAME("asset.catalog.search"), other_kind_search)));
	Dictionary catalog_args;
	catalog_args["provider"] = providers[0];
	catalog_args["kind"] = kinds[0];
	catalog_args["asset_id"] = "synthetic_catalog_asset";
	catalog_args["variant"] = "synthetic_variant";
	const Array catalog_acquire_accesses = registry.resolve_resource_access(SNAME("asset.catalog.acquire"), catalog_args);
	REQUIRE(catalog_acquire_accesses.size() == 2);
	CHECK(Dictionary(catalog_acquire_accesses[0]).get("mode", String()) == "write");
	CHECK(String(Dictionary(catalog_acquire_accesses[0]).get("key", String())).contains("synthetic_catalog_asset"));
	CHECK(Dictionary(catalog_acquire_accesses[1]).get("mode", String()) == "write");
	CHECK(String(Dictionary(catalog_acquire_accesses[1]).get("key", String())).begins_with("project:res://assets/"));
}

TEST_CASE("[SolersToolRegistry] transcript audit preserves full redacted tool arguments") {
	SolersToolRegistry registry;
	registry.register_default_tools();
	Array operations;
	for (int i = 0; i < 40; i++) {
		Dictionary operation;
		operation["op"] = "update";
		operation["node_path"] = ".";
		operation["properties"] = Dictionary({ { "name", "Root" } });
		operations.push_back(operation);
	}
	Dictionary args;
	args["scope"] = "scene";
	args["operations"] = operations;
	const Dictionary audit = registry.redact_tool_args_for_audit(SNAME("object.transaction"), args);
	CHECK(Array(audit.get("operations", Array())).size() == 40);
}

} // namespace TestSolersTools
