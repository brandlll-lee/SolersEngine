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
#include "core/debugger/engine_debugger.h"
#include "core/input/input.h"
#include "core/input/input_map.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/decal.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/animation.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/packed_scene.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_GDSCRIPT_ENABLED
#include "modules/gdscript/gdscript.h"
#endif
#ifdef MODULE_CSG_ENABLED
#include "modules/csg/csg_shape.h"
#endif
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_builtin_skills.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_path_utils.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_project_observation.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_runtime_observation.h"
#include "modules/solers_ai/core/solers_scene_observation.h"
#include "modules/solers_ai/core/solers_script_context.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/generated/terrain3d_lock.gen.h"
#include "modules/solers_ai/llm/solers_protocol_anthropic_messages.h"
#include "modules/solers_ai/llm/solers_protocol_openai_chat.h"
#include "modules/solers_ai/llm/solers_protocol_openai_responses.h"
#include "modules/solers_ai/plugins/solers_plugin.h"
#include "modules/solers_ai/tests/support/solers_test_state.h"
#ifdef MODULE_GLTF_ENABLED
#include "modules/gltf/extensions/gltf_document_extension_convert_importer_mesh.h"
#include "modules/gltf/gltf_document.h"
#endif

TEST_FORCE_LINK(test_solers_tools)

void solers_runtime_bridge_initialize();

namespace TestSolersTools {

TEST_CASE("[SolersPath] project paths share one native boundary") {
	const SolersPath::NormalizedPath relative = SolersPath::normalize_project_path(" folder\\note..txt ");
	REQUIRE(relative.valid);
	CHECK(relative.value == "res://folder/note..txt");

	const SolersPath::NormalizedPath traversal = SolersPath::normalize_project_path("res://../outside.txt");
	CHECK_FALSE(traversal.valid);
	CHECK_FALSE(traversal.error.is_empty());

	const SolersPath::NormalizedPath git_metadata = SolersPath::normalize_project_path("res://nested/.git/config");
	CHECK_FALSE(git_metadata.valid);
	CHECK(git_metadata.error == "Refusing to operate on .git metadata.");

	const String project_data_path = ProjectSettings::get_singleton()->get_project_data_path();
	const String project_data_child = project_data_path == "res://" ? String("res://.godot/state.cfg") : project_data_path.path_join("state.cfg");
	const SolersPath::NormalizedPath project_data = SolersPath::normalize_project_path(project_data_child);
	CHECK_FALSE(project_data.valid);
	const SolersPath::NormalizedPath allowed_project_data = SolersPath::normalize_project_path(project_data_child, true);
	CHECK(allowed_project_data.valid);
}

class TestEngineDebugger : public EngineDebugger {
public:
	String last_message;
	Array last_data;

	TestEngineDebugger() { singleton = this; }

	void send_message(const String &p_message, const Array &p_data) override {
		last_message = p_message;
		last_data = p_data;
	}
	void send_error(const String &, const String &, int, const String &, const String &, bool, ErrorHandlerType) override {}
	void debug(bool, bool) override {}
};

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

TEST_CASE("[SolersToolRegistry] registers tools by lookup, not a hardcoded catalog") {
	// Behavior contract (no-patch): a brand-new tool the dispatcher has never
	// special-cased becomes discoverable + dispatchable purely by registering
	// it — no catalog entry, no dispatcher branch, no name-prefix classifier.
	SolersToolRegistry registry;

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_domains = SolersToolMutationDomain::NONE;
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
			schema, SolersToolExposure::MODEL, cap,
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

TEST_CASE("[SolersToolRegistry] deferred tools expose and execute their own schemas") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	int executions = 0;
	SolersToolCapability capability;
	capability.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	capability.operation_domain = SolersOperationDomain::EDITOR;
	capability.operation_mode = SolersOperationMode::QUERY;
	const Dictionary schema({ { "type", "object" }, { "properties", Dictionary({ { "sample", Dictionary({ { "type", "string" } }) } }) }, { "required", Array({ "sample" }) }, { "additionalProperties", false } });
	registry.register_tool(memnew(SolersFunctionTool("synthetic.authority_probe", "Observe one synthetic authority fact.", schema, SolersToolExposure::DEFERRED, capability,
			[&executions](const SolersToolContext &, const Dictionary &) {
				executions++;
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "fact", "stable" } }) } });
			})));

	const Dictionary definition = registry.get_tool_definition(SNAME("synthetic.authority_probe"));
	REQUIRE_FALSE(definition.is_empty());
	CHECK(Dictionary(definition.get("input_schema", Dictionary())).recursive_equal(schema, 0));

	const Dictionary rejected = registry.call_tool(SNAME("synthetic.authority_probe"), Dictionary());
	REQUIRE_FALSE((bool)rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "TOOL_ARGUMENT_INVALID");
	CHECK(executions == 0);

	const Dictionary observed = registry.call_tool(SNAME("synthetic.authority_probe"), Dictionary({ { "sample", "value" } }));
	REQUIRE((bool)observed.get("ok", false));
	CHECK(Dictionary(observed.get("data", Dictionary())).get("fact", String()) == "stable");
	CHECK(executions == 1);
}

TEST_CASE("[SolersToolRegistry] asset operations expose the provider-qualified registry contract") {
	SolersAssetService assets;
	SolersToolRegistry registry;
	registry.set_asset_service(&assets);
	registry.register_default_tools();
	const Dictionary tool = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "asset.run_operation");
	REQUIRE_FALSE(tool.is_empty());
	const Dictionary schema = tool.get("input_schema", Dictionary());
	const Dictionary properties = schema.get("properties", Dictionary());
	const Array providers = Dictionary(properties.get("provider", Dictionary())).get("enum", Array());
	CHECK(Array(schema.get("required", Array())).has("provider"));
	CHECK(String(Dictionary(properties.get("provider", Dictionary())).get("description", String())).contains("asset.capabilities"));
	int operation_plugins = 0;
	for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
		if (plugin->get_operation_defs().is_empty()) {
			continue;
		}
		operation_plugins++;
		CHECK(providers.has(String(Dictionary(plugin->get_profile()).get("id", String())).to_lower()));
	}
	CHECK(providers.size() == operation_plugins);
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
	cap.mutation_domains = SolersToolMutationDomain::IRREVERSIBLE;
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
			SNAME("synthetic.write"), "Synthetic write fixture.", schema, SolersToolExposure::MODEL, cap,
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

	Dictionary unsupported_args;
	unsupported_args["amount"] = 1.0;
	unsupported_args["unexpected"] = true;
	const Dictionary unsupported = registry.call_tool(SNAME("synthetic.write"), unsupported_args);
	CHECK_FALSE((bool)unsupported.get("ok", true));
	const String unsupported_message = Dictionary(unsupported.get("error", Dictionary())).get("message", String());
	CHECK(unsupported_message.contains("unexpected"));
	CHECK(unsupported_message.contains("Supported fields: amount"));
	const Dictionary echoed_arguments = Dictionary(unsupported.get("data", Dictionary())).get("arguments", Dictionary());
	CHECK((double)echoed_arguments.get("amount", 0.0) == 1.0);
	CHECK((bool)echoed_arguments.get("unexpected", false));
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

TEST_CASE("[SolersToolRegistry] JSON integers retain JSON Schema integer semantics") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	Dictionary schema({ { "type", "object" }, { "properties", Dictionary({ { "count", Dictionary({ { "type", "integer" } }) } }) }, { "required", Array({ "count" }) }, { "additionalProperties", false } });
	SolersToolCapability capability;
	capability.operation_domain = SolersOperationDomain::EDITOR;
	capability.operation_mode = SolersOperationMode::QUERY;
	registry.register_tool(memnew(SolersFunctionTool("synthetic.integer", "Synthetic integer contract.", schema, SolersToolExposure::DEFERRED, capability,
			[](const SolersToolContext &, const Dictionary &p_args) {
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "count", p_args.get("count", 0) } }) } });
			})));

	const Variant whole_number = JSON::parse_string(R"({"count":20})");
	REQUIRE(whole_number.get_type() == Variant::DICTIONARY);
	CHECK(Dictionary(whole_number).get("count", Variant()).get_type() == Variant::FLOAT);
	CHECK(registry.call_tool("synthetic.integer", Dictionary(whole_number)).get("ok", false));

	const Variant fraction = JSON::parse_string(R"({"count":20.5})");
	const Dictionary rejected = registry.call_tool("synthetic.integer", Dictionary(fraction));
	CHECK_FALSE((bool)rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "TOOL_ARGUMENT_INVALID");
}

TEST_CASE("[SolersToolRegistry] preserves internal session context without changing the bound API") {
	SolersToolRegistry registry;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_permission_manager(&permissions);

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_domains = SolersToolMutationDomain::NONE;
	registry.register_tool(memnew(SolersFunctionTool(
			"synthetic.context", "Returns its internal execution context.", empty_tool_schema(), SolersToolExposure::MODEL, cap,
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
		CHECK(skill.content.contains("## Scope"));
		CHECK(skill.content.contains("## Native model"));
		CHECK(skill.content.contains("## Compatibility and prerequisites"));
		CHECK(skill.content.contains("## Authoritative state"));
		CHECK(skill.content.contains("## Official references"));
		CHECK(skill.content.contains("docs.godotengine.org/en/latest/"));
		CHECK_FALSE(skill.content.contains("\ntools:"));
		CHECK_FALSE(skill.content.contains("## Recipes"));
		CHECK_FALSE(skill.content.contains("## Laws"));
	}

	SolersBuiltinSkillView rendering;
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-3d-rendering", rendering));
	CHECK(rendering.content.contains("LightmapGI"));
	CHECK(rendering.content.contains("VoxelGI"));
	CHECK(rendering.content.contains("SDFGI"));
	CHECK(rendering.content.contains("UV2"));
	CHECK(rendering.content.contains("Forward+"));
	CHECK(rendering.content.contains("CameraAttributes"));

	SolersBuiltinSkillView missing;
	CHECK_FALSE(SolersBuiltinSkills::find_by_name("synthetic-never-registered-skill", missing));
	CHECK_FALSE(SolersBuiltinSkills::find_by_name("photorealism-pipeline", missing));
}

TEST_CASE("[SolersToolRegistry] skill.read serves compiled builtin skills") {
	SolersPermissionManager permissions;
	SolersReflectionService reflection;
	SolersToolRegistry registry;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection);
	registry.register_default_tools();

	Dictionary args;
	args["name"] = "godot-3d-rendering";
	const Dictionary result = registry.call_tool(StringName("skill.read"), args);
	REQUIRE((bool)result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("name", String()) == "godot-3d-rendering");
	CHECK(!String(data.get("content", String())).is_empty());
	CHECK(String(data.get("content", String())).contains("## Native model"));
	CHECK_FALSE(result.has("added_tools"));
}

TEST_CASE("[SolersToolRegistry] optional tools are discovered from live capability metadata") {
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	const Dictionary result = registry.call_tool(StringName("tool.search"), Dictionary({ { "query", "revert" }, { "mode", "apply" } }));
	REQUIRE((bool)result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	const Array matches = data.get("tools", Array());
	REQUIRE(matches.size() == 1);
	CHECK(Dictionary(matches[0]).get("name", String()) == "history.revert");
	CHECK(Array(result.get("added_tools", Array())).has("history.revert"));
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
	SolersPermissionManager permissions;
	SolersProjectObservation project_observation;
	SolersReflectionService reflection_service;
	SolersResourceService resource_service;
	SolersRuntimeObservation runtime_observation;
	SolersSceneObservation scene_observation;
	SolersScriptService script_service;
	SolersToolRegistry registry;
	registry.set_asset_service(&asset_service);
	registry.set_permission_manager(&permissions);
	registry.set_project_observation(&project_observation);
	registry.set_reflection_service(&reflection_service);
	registry.set_resource_service(&resource_service);
	registry.set_runtime_observation(&runtime_observation);
	registry.set_scene_observation(&scene_observation);
	registry.set_script_service(&script_service);
	registry.register_default_tools();

	const Array tools = registry.list_tools();
	Array request_tools;
	for (const Variant &item : tools) {
		const Dictionary definition = item;
		check_portable_tool_schema(definition.get("input_schema", Dictionary()));
		if (definition.get("exposure", String()) != "model") {
			continue;
		}
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
	REQUIRE(anthropic.size() == request_tools.size());
	REQUIRE(chat.size() == request_tools.size());
	REQUIRE(responses.size() == request_tools.size());
	for (int i = 0; i < request_tools.size(); i++) {
		check_portable_tool_schema(Dictionary(anthropic[i]).get("input_schema", Dictionary()));
		check_portable_tool_schema(Dictionary(Dictionary(chat[i]).get("function", Dictionary())).get("parameters", Dictionary()));
		const Dictionary response_tool = responses[i];
		check_portable_tool_schema(response_tool.get("parameters", Dictionary()));
		CHECK_FALSE(response_tool.get("strict", true));
	}
	const Dictionary query = solers_test_find_dictionary(tools, SNAME("name"), "scene.inspect");
	REQUIRE_FALSE(query.is_empty());
	CHECK_FALSE(Dictionary(Dictionary(query.get("input_schema", Dictionary())).get("properties", Dictionary())).has("expected_state"));
	const Dictionary resource_edit = solers_test_find_dictionary(tools, SNAME("name"), "resource.edit");
	REQUIRE_FALSE(resource_edit.is_empty());
	const Dictionary resource_edit_schema = resource_edit.get("input_schema", Dictionary());
	CHECK_FALSE(Array(resource_edit_schema.get("required", Array())).has("expected_state"));
	const Dictionary state_schema = Dictionary(resource_edit_schema.get("properties", Dictionary())).get("expected_state", Dictionary());
	const Dictionary resources_schema = Dictionary(state_schema.get("properties", Dictionary())).get("resources", Dictionary());
	CHECK(Array(Dictionary(resources_schema.get("items", Dictionary())).get("required", Array())) == Array({ "path", "exists" }));
	const Dictionary script_edit = solers_test_find_dictionary(tools, SNAME("name"), "script.edit");
	const Dictionary script_properties = Dictionary(script_edit.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(script_properties.has("expected_sha256"));
	CHECK(Dictionary(script_properties.get("expected_sha256", Dictionary())).get("pattern", String()) == "^[0-9a-f]{64}$");
	CHECK_FALSE(script_properties.has("occurrence"));

	Dictionary invalid;
	invalid["oneOf"] = Array();
	ERR_PRINT_OFF;
	registry.register_tool(memnew(SolersFunctionTool("synthetic.invalid", "Invalid root.", invalid, SolersToolExposure::MODEL, SolersToolCapability(), [](const SolersToolContext &, const Dictionary &) { return Dictionary(); })));
	ERR_PRINT_ON;
	CHECK(registry.get_tool_count() == tools.size());
}

TEST_CASE("[SolersToolRegistry] ClassDB member queries match whitespace-separated property names") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersReflectionService reflection_service;
	SolersSceneObservation observation_service;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection_service);
	registry.set_scene_observation(&observation_service);
	registry.register_default_tools();
	const Dictionary tool = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "engine.describe");
	const Dictionary engine_properties = Dictionary(tool.get("input_schema", Dictionary())).get("properties", Dictionary());
	const Dictionary classes_schema = engine_properties.get("classes", Dictionary());
	const Dictionary class_schema = classes_schema.get("items", Dictionary());
	CHECK_FALSE(Array(class_schema.get("required", Array())).has("max_members"));
	CHECK(engine_properties.has("max_members"));
	CHECK(Dictionary(tool.get("ui", Dictionary())).get("running", String()) == "Searching");
	CHECK(PackedStringArray(tool.get("ui_subject_paths", PackedStringArray())).has("/classes/0/class_name"));
	const Dictionary capture_tool = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "render.capture");
	CHECK(String(capture_tool.get("description", String())).contains("geometric framing facts"));
	const Dictionary capture_properties = Dictionary(capture_tool.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(capture_properties.has("focus_paths"));
	CHECK(capture_properties.has("camera_path"));
	CHECK(capture_properties.has("include_render_state"));
	CHECK_FALSE(capture_properties.has("node_path"));
	CHECK(Array(Dictionary(capture_properties.get("target", Dictionary())).get("enum", Array())).has("focus"));
	CHECK(PackedStringArray(capture_tool.get("required_model_inputs", PackedStringArray())).has("image"));
	const Dictionary scene_update_tool = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "scene.node.update");
	CHECK(Dictionary(scene_update_tool.get("ui", Dictionary())).get("completed", String()) == "Updated");

	Dictionary class_request;
	class_request["class_name"] = "CameraAttributesPhysical";
	class_request["include_inherited"] = true;
	class_request["member_query"] = "exposure_aperture exposure_sensitivity";
	Array classes({ class_request, Dictionary({ { "class_name", "SolersMissingNativeClass" } }), Dictionary({ { "class_name", "Node3D" } }) });
	Dictionary args;
	args["classes"] = classes;
	const Dictionary result = registry.call_tool(SNAME("engine.describe"), args);
	REQUIRE((bool)result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	const Array described = data.get("classes", Array());
	REQUIRE(described.size() == 2);
	CHECK((int)Dictionary(described[0]).get("request_index", -1) == 0);
	CHECK((int)Dictionary(described[1]).get("request_index", -1) == 2);
	const Array errors = data.get("errors", Array());
	REQUIRE(errors.size() == 1);
	CHECK((int)Dictionary(errors[0]).get("request_index", -1) == 1);
	CHECK(Dictionary(Dictionary(errors[0]).get("error", Dictionary())).get("code", String()) == "UNKNOWN_CLASS");
	CHECK_FALSE((bool)data.get("complete", true));
	CHECK((int)data.get("requested_count", 0) == 3);
	args["classes"] = Array({ Dictionary({ { "class_name", "SolersMissingOne" } }), Dictionary({ { "class_name", "SolersMissingTwo" } }) });
	const Dictionary all_invalid = registry.call_tool(SNAME("engine.describe"), args);
	REQUIRE((bool)all_invalid.get("ok", false));
	const Dictionary invalid_data = all_invalid.get("data", Dictionary());
	CHECK(Array(invalid_data.get("classes", Array())).is_empty());
	const Array invalid_errors = invalid_data.get("errors", Array());
	REQUIRE(invalid_errors.size() == 2);
	CHECK((int)Dictionary(invalid_errors[0]).get("request_index", -1) == 0);
	CHECK((int)Dictionary(invalid_errors[1]).get("request_index", -1) == 1);
	CHECK(Dictionary(Dictionary(invalid_errors[1]).get("error", Dictionary())).get("code", String()) == "UNKNOWN_CLASS");
	CHECK_FALSE((bool)invalid_data.get("complete", true));
	const Array properties = Dictionary(described[0]).get("properties", Array());
	HashSet<String> names;
	for (int i = 0; i < properties.size(); i++) {
		const Dictionary property = properties[i];
		names.insert(property.get("name", String()));
		CHECK_FALSE(property.has("wire_shape"));
	}
	CHECK(names.has("exposure_aperture"));
	CHECK(names.has("exposure_sensitivity"));

	Dictionary paged_args;
	paged_args["max_members"] = 1;
	paged_args["classes"] = Array({ Dictionary({ { "class_name", "Node3D" } }), Dictionary({ { "class_name", "Resource" }, { "max_members", 2 } }) });
	const Dictionary paged = registry.call_tool(SNAME("engine.describe"), paged_args);
	REQUIRE((bool)paged.get("ok", false));
	const Array paged_classes = Dictionary(paged.get("data", Dictionary())).get("classes", Array());
	REQUIRE(paged_classes.size() == 2);
	CHECK(PackedStringArray(Dictionary(paged_classes[0]).get("member_names", PackedStringArray())).size() == 1);
	CHECK(PackedStringArray(Dictionary(paged_classes[1]).get("member_names", PackedStringArray())).size() == 2);
}

TEST_CASE("[SolersToolRegistry] project.search gives empty queries one explicit meaning") {
	SolersProjectObservation observation;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_project_observation(&observation);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	Dictionary args;
	args["type"] = "path";
	args["query"] = "";
	args["max_results"] = 2;
	const Dictionary listed = registry.call_tool(SNAME("project.search"), args);
	REQUIRE((bool)listed.get("ok", false));
	const Dictionary list_data = listed.get("data", Dictionary());
	CHECK(list_data.get("type", String()) == "path");
	CHECK(list_data.get("query", String()) == "");
	CHECK((int)list_data.get("count", 0) <= 2);

	args.erase("max_results");
	const Dictionary unpaged = registry.call_tool(SNAME("project.search"), args);
	CHECK((bool)unpaged.get("ok", false));
	CHECK((int)Dictionary(unpaged.get("data", Dictionary())).get("count", 0) >= (int)list_data.get("count", 0));

	for (const String &type : { String("text"), String("symbol") }) {
		args["type"] = type;
		const Dictionary rejected = registry.call_tool(SNAME("project.search"), args);
		CHECK_FALSE((bool)rejected.get("ok", true));
		CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "INVALID_ARGUMENT");
	}
}

TEST_CASE("[SolersTool] ObjectID wire values are lossless decimal strings") {
	const ObjectID high_bit_id((uint64_t(1) << 63) | 42);
	const String encoded = solers_object_id_to_string(high_bit_id);
	CHECK(encoded == "-9223372036854775766");
	ObjectID decoded;
	REQUIRE(solers_object_id_from_variant(encoded, decoded));
	CHECK(decoded == high_bit_id);

	for (const Variant &invalid : { Variant((int64_t)42), Variant(42.0), Variant("4.2"), Variant("4e2"), Variant("+42"), Variant(" 42"), Variant("0") }) {
		ObjectID ignored;
		CHECK_FALSE(solers_object_id_from_variant(invalid, ignored));
	}

	SolersResourceService resources;
	SolersReflectionService reflection;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection);
	registry.set_resource_service(&resources);
	registry.register_default_tools();
	Dictionary args;
	args["object_id"] = 42;
	const Dictionary rejected = registry.call_tool(SNAME("object.inspect"), args);
	CHECK_FALSE((bool)rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "TOOL_ARGUMENT_INVALID");
}

TEST_CASE("[SolersResourceService] PropertyInfo coercion and animation inventory use native resource facts") {
	const Dictionary color_shape = solers_variant_wire_shape(Variant::COLOR);
	CHECK(color_shape.get("encoding", String()) == "named_members");
	const Dictionary color_members = color_shape.get("members", Dictionary());
	CHECK(color_members.get("r", String()) == "float");
	CHECK(color_members.get("g", String()) == "float");
	CHECK(color_members.get("b", String()) == "float");
	CHECK(color_members.get("a", String()) == "float");
	CHECK(solers_variant_wire_shape(Variant::INT).is_empty());

	Variant value;
	String error;
	REQUIRE(solers_coerce_variant_value(PropertyInfo(Variant::COLOR, "light_color"), Dictionary({ { "r", 1.0 }, { "g", 0.5 }, { "b", 0.25 }, { "a", 1.0 } }), value, error));
	CHECK(value.get_type() == Variant::COLOR);
	CHECK(Color(value).is_equal_approx(Color(1.0, 0.5, 0.25, 1.0)));
	DirectionalLight3D *light = memnew(DirectionalLight3D);
	REQUIRE(solers_coerce_property_value(light, SNAME("light_color"), Dictionary({ { "r", 0.1 }, { "g", 0.2 }, { "b", 0.3 }, { "a", 1.0 } }), value, error));
	CHECK(Color(value).is_equal_approx(Color(0.1, 0.2, 0.3, 1.0)));
	REQUIRE(solers_coerce_property_value(light, SNAME("light_color"), "#19334c", value, error));
	CHECK(Color(value).is_equal_approx(Color("#19334c")));
	memdelete(light);
	REQUIRE(solers_coerce_variant_value(PropertyInfo(Variant::VECTOR3, "position"), Dictionary({ { "x", 1.0 }, { "y", 2.0 }, { "z", 3.0 } }), value, error));
	CHECK(value.get_type() == Variant::VECTOR3);
	CHECK(Vector3(value).is_equal_approx(Vector3(1, 2, 3)));
	CHECK_FALSE(solers_coerce_variant_value(PropertyInfo(Variant::VECTOR3, "position"), "Vector3(1, 2, 3)", value, error));

	const String path = "res://.solers_animation_inventory.tscn";
	SolersTestPaths cleanup;
	cleanup.add(path);
	Node *root = memnew(Node);
	AnimationPlayer *player = memnew(AnimationPlayer);
	player->set_name("Player");
	root->add_child(player);
	player->set_owner(root);
	Ref<AnimationLibrary> library;
	library.instantiate();
	Ref<Animation> animation;
	animation.instantiate();
	animation->set_length(1.25);
	animation->set_loop_mode(Animation::LOOP_LINEAR);
	animation->add_track(Animation::TYPE_VALUE);
	REQUIRE(library->add_animation(SNAME("Idle"), animation) == OK);
	REQUIRE(player->add_animation_library(SNAME("locomotion"), library) == OK);
	Ref<PackedScene> packed;
	packed.instantiate();
	REQUIRE(packed->pack(root) == OK);
	memdelete(root);
	REQUIRE(ResourceSaver::save(packed, path) == OK);

	SolersResourceService resources;
	const Dictionary result = resources.get_resource_info(Dictionary({ { "path", path }, { "include_dependencies", false } }));
	REQUIRE((bool)result.get("ok", false));
	const Array players = Dictionary(result.get("data", Dictionary())).get("animation_players", Array());
	REQUIRE(players.size() == 1);
	const Dictionary player_info = players[0];
	CHECK(Array(player_info.get("libraries", Array())).has("locomotion"));
	const Array clips = player_info.get("clips", Array());
	REQUIRE(clips.size() == 1);
	CHECK(Dictionary(clips[0]).get("name", String()) == "locomotion/Idle");
	CHECK(Math::is_equal_approx((double)Dictionary(clips[0]).get("length", 0.0), 1.25));
	CHECK((int)Dictionary(clips[0]).get("loop_mode", -1) == Animation::LOOP_LINEAR);
	CHECK((int)Dictionary(clips[0]).get("track_count", 0) == 1);
}

TEST_CASE("[SolersToolRegistry] a registered deferred tool needs no dispatcher branch") {
	const String path = "res://.solers_synthetic_capability.txt";
	SolersTestPaths cleanup;
	cleanup.add(path);
	SolersFileCheckpoint checkpoints;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, true);
	SolersReflectionService reflection;
	SolersToolRegistry registry;
	registry.set_file_checkpoint(&checkpoints);
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection);
	registry.register_default_tools();

	bool executed = false;
	SolersToolCapability capability;
	capability.permission = SolersPermissionManager::PERMISSION_EDIT_FILES;
	capability.mutation_domains = SolersToolMutationDomain::FILES;
	capability.operation_domain = SolersOperationDomain::EDITOR;
	capability.operation_mode = SolersOperationMode::APPLY;
	capability.resource_access = [path](const Dictionary &) {
		return Array({ Dictionary({ { "mode", "write" }, { "key", "project:" + path } }) });
	};
	Dictionary schema({ { "type", "object" }, { "properties", Dictionary({ { "content", Dictionary({ { "type", "string" } }) } }) }, { "required", Array({ "content" }) }, { "additionalProperties", false } });
	registry.register_tool(memnew(SolersFunctionTool("synthetic.operation", "Synthetic operation contract.", schema, SolersToolExposure::DEFERRED, capability,
			[&executed, path](const SolersToolContext &, const Dictionary &p_args) {
				executed = true;
				Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
				if (file.is_null()) {
					return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "WRITE_FAILED" } }) } });
				}
				file->store_string(p_args.get("content", String()));
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "path", path }, { "authored_state_changed", true } }) } });
			})));

	const Dictionary definition = registry.get_tool_definition(SNAME("synthetic.operation"));
	REQUIRE_FALSE(definition.is_empty());
	CHECK_FALSE(Array(Dictionary(definition.get("input_schema", Dictionary())).get("required", Array())).has("expected_state"));

	SolersToolContext context;
	context.call_id = "synthetic-operation";
	context.session_id = "synthetic-operation-contract";
	Dictionary args;
	args["expected_state"] = Dictionary({ { "resources", Array() } });
	args["content"] = "native capability";
	const Dictionary incomplete = registry.call_tool_with_context("synthetic.operation", args, context);
	CHECK_FALSE((bool)incomplete.get("ok", true));
	CHECK(Dictionary(incomplete.get("error", Dictionary())).get("code", String()) == "RESOURCE_STATE_INVALID");
	CHECK_FALSE(executed);
	args["expected_state"] = Dictionary({ { "resources", Array({ Dictionary({ { "path", path }, { "exists", false } }) }) } });
	const Dictionary result = registry.call_tool_with_context("synthetic.operation", args, context);
	REQUIRE((bool)result.get("ok", false));
	CHECK(executed);
	CHECK(FileAccess::get_file_as_string(path) == "native capability");
	const Dictionary mutation = Dictionary(result.get("data", Dictionary())).get("mutation", Dictionary());
	CHECK(Array(mutation.get("domains", Array())).has("files"));

	context.call_id = "synthetic-revert";
	const Dictionary reverted = registry.call_tool_with_context("history.revert", Dictionary({ { "reversal_id", mutation.get("reversal_id", String()) } }), context);
	REQUIRE((bool)reverted.get("ok", false));
	CHECK_FALSE(FileAccess::exists(path));
}

TEST_CASE("[SolersToolRegistry] object inspection invokes typed ClassDB const methods") {
	Ref<Animation> animation;
	animation.instantiate();
	const int track = animation->add_track(Animation::TYPE_VALUE);
	animation->track_set_path(track, NodePath("Sprite:modulate"));
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersReflectionService reflection;
	SolersResourceService resources;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection);
	registry.set_resource_service(&resources);
	registry.register_default_tools();
	Dictionary args;
	args["object_id"] = solers_object_id_to_string(animation->get_instance_id());
	args["method_calls"] = Array({
			Dictionary({ { "name", "track_get_path" }, { "arguments", Array({ track }) } }),
			Dictionary({ { "name", "track_get_type" }, { "arguments", Array({ track }) } }),
			Dictionary({ { "name", "track_set_path" }, { "arguments", Array({ track, "Other:value" }) } }),
			Dictionary({ { "name", "track_get_path" } }),
	});
	const Dictionary result = registry.call_tool("object.inspect", args);
	REQUIRE((bool)result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	const Array method_results = data.get("method_results", Array());
	REQUIRE(method_results.size() == 4);
	CHECK(String(Dictionary(method_results[0]).get("method", String())) == "track_get_path");
	CHECK(Array(Dictionary(method_results[0]).get("arguments", Array())).size() == 1);
	CHECK(Dictionary(method_results[0]).get("value", String()) == "Sprite:modulate");
	CHECK((int)Dictionary(method_results[1]).get("value", -1) == Animation::TYPE_VALUE);
	CHECK(Dictionary(Dictionary(method_results[2]).get("error", Dictionary())).get("code", String()) == "METHOD_NOT_READABLE");
	CHECK(Dictionary(Dictionary(method_results[3]).get("error", Dictionary())).get("code", String()) == "METHOD_ARGUMENT_COUNT");
	CHECK_FALSE(data.has("expected_state"));

	SUBCASE("engine describe exposes ClassDB method call contracts") {
		const Dictionary described = registry.call_tool("engine.describe", Dictionary({ { "classes", Array({ Dictionary({ { "class_name", "Animation" }, { "include_inherited", false }, { "member_query", "track_get_path add_track" }, { "max_members", 8 } }) }) } }));
		REQUIRE((bool)described.get("ok", false));
		const Array classes = Dictionary(described.get("data", Dictionary())).get("classes", Array());
		REQUIRE(classes.size() == 1);
		const Array methods = Dictionary(classes[0]).get("methods", Array());
		const Dictionary path_method = solers_test_find_dictionary(methods, SNAME("name"), "track_get_path");
		CHECK(path_method.get("is_const", false));
		CHECK_FALSE(path_method.get("is_vararg", true));
		CHECK((int)path_method.get("required_argument_count", 0) == 1);
		const Dictionary add_track = solers_test_find_dictionary(methods, SNAME("name"), "add_track");
		const Array add_arguments = add_track.get("arguments", Array());
		REQUIRE(add_arguments.size() == 2);
		CHECK((int)Dictionary(add_arguments[1]).get("default_value", 0) == -1);
	}
}

TEST_CASE("[SolersToolRegistry] resource inspection uses the native read contract") {
	const String path = "res://.solers_resource_method_contract.tres";
	SolersTestPaths cleanup;
	cleanup.add(path);
	Ref<Image> image = Image::create_empty(8, 4, false, Image::FORMAT_RGBA8);
	Ref<ImageTexture> texture = ImageTexture::create_from_image(image);
	REQUIRE(ResourceSaver::save(texture, path) == OK);

	SolersFileCheckpoint checkpoints;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersReflectionService reflection;
	SolersResourceService resources;
	SolersToolRegistry registry;
	registry.set_file_checkpoint(&checkpoints);
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection);
	registry.set_resource_service(&resources);
	registry.register_default_tools();
	const Array method_calls = Array({ Dictionary({ { "name", "get_width" } }), Dictionary({ { "name", "get_height" } }) });
	const Dictionary result = registry.call_tool("resource.inspect", Dictionary({ { "path", path }, { "method_calls", method_calls } }));
	REQUIRE((bool)result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	const Array method_results = data.get("method_results", Array());
	REQUIRE(method_results.size() == 2);
	CHECK((int)Dictionary(method_results[0]).get("value", 0) == 8);
	CHECK((int)Dictionary(method_results[1]).get("value", 0) == 4);
	const Dictionary state = data.get("state", Dictionary());
	CHECK(Dictionary(state).get("path", String()) == path);
	CHECK(String(Dictionary(state).get("sha256", String())).length() == 64);
	CHECK_FALSE(result.has("added_tools"));
}

TEST_CASE("[SolersFileCheckpoint] directory state restores the exact recursive digest") {
	const String directory = "res://.solers_directory_checkpoint";
	const String original_file = directory.path_join("nested/original.txt");
	const String added_file = directory.path_join("added.txt");
	SolersTestPaths cleanup;
	cleanup.add(directory);
	REQUIRE(DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(original_file.get_base_dir())) == OK);
	{
		Ref<FileAccess> file = FileAccess::open(original_file, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string("before");
	}
	SolersFileCheckpoint checkpoints;
	const Dictionary before = checkpoints.get_path_state(directory).get("data", Dictionary());
	const Dictionary created = checkpoints.create_checkpoint(directory, "directory contract");
	REQUIRE((bool)created.get("ok", false));
	const Dictionary checkpoint = created.get("data", Dictionary());
	CHECK(checkpoint.get("directory", false));
	{
		Ref<FileAccess> changed = FileAccess::open(original_file, FileAccess::WRITE);
		REQUIRE(changed.is_valid());
		changed->store_string("after");
		Ref<FileAccess> added = FileAccess::open(added_file, FileAccess::WRITE);
		REQUIRE(added.is_valid());
		added->store_string("added");
	}
	CHECK(Dictionary(checkpoints.get_path_state(directory).get("data", Dictionary())).get("content_sha256", String()) != before.get("content_sha256", String()));
	REQUIRE((bool)checkpoints.restore_checkpoint_state(checkpoint).get("ok", false));
	CHECK(FileAccess::get_file_as_string(original_file) == "before");
	CHECK_FALSE(FileAccess::exists(added_file));
	CHECK(Dictionary(checkpoints.get_path_state(directory).get("data", Dictionary())).get("content_sha256", String()) == before.get("content_sha256", String()));
	checkpoints.discard_checkpoint_state(checkpoint);
	CHECK_FALSE(DirAccess::exists(checkpoint.get("checkpoint_path", String())));
}

TEST_CASE("[SolersFileCheckpoint] checkpoint sources stay inside the native checkpoint root") {
	const String target = "res://.solers_checkpoint_boundary_target.txt";
	const String outside = "res://.solers_checkpoint_boundary_source.txt";
	SolersTestPaths cleanup;
	cleanup.add(target);
	cleanup.add(outside);
	{
		Ref<FileAccess> file = FileAccess::open(target, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string("before");
	}
	{
		Ref<FileAccess> file = FileAccess::open(outside, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string("outside");
	}

	SolersFileCheckpoint checkpoints;
	const Dictionary forged = Dictionary({ { "path", target }, { "existed", true }, { "directory", false }, { "checkpoint_path", outside } });
	const Dictionary result = checkpoints.restore_checkpoint_state(forged);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "CHECKPOINT_NOT_FOUND");
	CHECK(FileAccess::get_file_as_string(target) == "before");
}

TEST_CASE("[SolersGeometryFacts][SceneTree] native bounds and framing cover arbitrary visual instances") {
	Node *host = memnew(Node);
	SceneTree::get_singleton()->get_root()->add_child(host);
	Node3D *visual_root = memnew(Node3D);
	host->add_child(visual_root);
	Decal *visual = memnew(Decal);
	visual->set_size(Vector3(2.0, 0.5, 3.0));
	visual->set_position(Vector3(20.0, 1.0, -8.0));
	visual_root->add_child(visual);

	AABB bounds;
	bool found = false;
	int visual_count = 0;
	solers_accumulate_world_aabb(visual_root, bounds, found, &visual_count);
	CHECK(found);
	CHECK(visual_count == 1);
	CHECK(bounds.has_volume());

	SubViewport *viewport = memnew(SubViewport);
	viewport->set_size(Size2i(800, 600));
	host->add_child(viewport);
	Camera3D *camera = memnew(Camera3D);
	viewport->add_child(camera);
	camera->set_perspective(70.0, 0.05, 100.0);
	camera->set_current(true);

	camera->set_far(5.0);
	const Dictionary beyond_far = solers_project_aabb(camera, bounds);
	CHECK_FALSE((bool)beyond_far.get("in_depth_range", true));
	camera->set_far(100.0);
	const Dictionary initially_outside = solers_project_aabb(camera, bounds);
	CHECK((bool)initially_outside.get("in_front", false));
	CHECK_FALSE((bool)initially_outside.get("in_frame", true));
	camera->set_transform(solers_frame_aabb(camera->get_global_transform(), bounds, camera->get_fov(), (real_t)viewport->get_size().x / (real_t)viewport->get_size().y, camera->get_keep_aspect_mode(), camera->get_near()));
	const Dictionary framed = solers_project_aabb(camera, bounds);
	CHECK((bool)framed.get("in_frame", false));
	CHECK((bool)framed.get("fully_in_frame", false));
	CHECK((real_t)framed.get("clipped_fraction", 1.0) == doctest::Approx(0.0));

	host->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SolersToolRegistry] ClassDB inheritance exposes native object facts") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersReflectionService reflection;
	SolersResourceService resources;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.set_reflection_service(&reflection);
	registry.set_resource_service(&resources);
	registry.register_default_tools();
	auto describe = [&registry](Object *p_object) {
		const Dictionary result = registry.call_tool("object.inspect", Dictionary({ { "object_id", solers_object_id_to_string(p_object->get_instance_id()) } }));
		CHECK((bool)result.get("ok", false));
		return Dictionary(result.get("data", Dictionary())); };
	LightmapGI *lightmap = memnew(LightmapGI);
	Decal *decal = memnew(Decal);
	const Dictionary lightmap_facts = describe(lightmap);
	CHECK(lightmap_facts.has("object"));
	CHECK(Dictionary(describe(decal)).has("object"));
	memdelete(lightmap);
	memdelete(decal);
}

TEST_CASE("[SolersToolRegistry] Resource facts round-trip through state-checked tools") {
	const String path = "res://.solers_operation_contract.tres";
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
	context.session_id = "resource-operation-contract";

	Dictionary create_args;
	create_args["expected_state"] = Dictionary({ { "resources", Array({ Dictionary({ { "path", path }, { "exists", false } }) }) } });
	create_args["class_name"] = "StandardMaterial3D";
	create_args["path"] = path;
	const Dictionary created = registry.call_tool_with_context(SNAME("resource.edit"), create_args, context);
	REQUIRE((bool)created.get("ok", false));
	const String sha = FileAccess::get_sha256(path);
	REQUIRE(sha.length() == 64);
	Dictionary query;
	query["path"] = path;
	Array queried_properties;
	queried_properties.push_back("albedo_color");
	query["properties"] = queried_properties;
	context.call_id = "query_resource";
	const Dictionary observed = registry.call_tool_with_context(SNAME("resource.inspect"), query, context);
	REQUIRE((bool)observed.get("ok", false));
	Dictionary expected_resource = Dictionary(observed.get("data", Dictionary())).get("state", Dictionary());

	expected_resource["sha256"] = String("0000000000000000000000000000000000000000000000000000000000000000");
	Dictionary properties;
	properties["albedo_color"] = "#4080bfff";
	properties["albedo_texture"] = Variant();
	Dictionary update_args;
	update_args["expected_state"] = Dictionary({ { "resources", Array({ expected_resource }) } });
	update_args["path"] = path;
	update_args["properties"] = properties;
	context.call_id = "stale_resource";
	const Dictionary stale = registry.call_tool_with_context(SNAME("resource.edit"), update_args, context);
	CHECK_FALSE((bool)stale.get("ok", true));
	CHECK(Dictionary(stale.get("error", Dictionary())).get("code", String()) == "RESOURCE_STATE_CONFLICT");
	CHECK(FileAccess::get_sha256(path) == sha);

	expected_resource["sha256"] = sha;
	update_args["expected_state"] = Dictionary({ { "resources", Array({ expected_resource }) } });
	context.call_id = "update_resource";
	const Dictionary updated = registry.call_tool_with_context(SNAME("resource.edit"), update_args, context);
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
	const Dictionary verified = registry.call_tool_with_context(SNAME("resource.inspect"), query, context);
	const Dictionary color = Dictionary(Dictionary(Dictionary(verified.get("data", Dictionary())).get("properties", Dictionary())).get("albedo_color", Dictionary())).get("value", Dictionary());
	CHECK(Math::is_equal_approx((double)color.get("r", 0.0), (double)Color("#4080bfff").r));
	CHECK(Math::is_equal_approx((double)color.get("b", 0.0), (double)Color("#4080bfff").b));

	const String failure_path = "res://.solers_operation_failure.tres";
	cleanup.add(failure_path);
	create_args.erase("class_name");
	create_args["expected_state"] = Dictionary({ { "resources", Array({ Dictionary({ { "path", failure_path }, { "exists", false } }) }) } });
	create_args["path"] = failure_path;
	context.call_id = "resource_failure";
	const Dictionary failed = registry.call_tool_with_context(SNAME("resource.edit"), create_args, context);
	CHECK_FALSE((bool)failed.get("ok", true));
	CHECK(String(Dictionary(failed.get("error", Dictionary())).get("message", String())).contains("class_name is required"));
	CHECK_FALSE(FileAccess::exists(failure_path));

	Dictionary revert_args;
	revert_args["reversal_id"] = mutation.get("reversal_id", String());
	context.call_id = "revert_resource";
	const Dictionary reverted = registry.call_tool_with_context(SNAME("history.revert"), revert_args, context);
	REQUIRE((bool)reverted.get("ok", false));
	CHECK(FileAccess::get_sha256(path) == sha);

	const String owner_path = "res://.solers_file_owner_contract.tres";
	cleanup.add(owner_path);
	Ref<StandardMaterial3D> target = ResourceLoader::load(path);
	Ref<StandardMaterial3D> owner;
	owner.instantiate();
	owner->set_next_pass(target);
	REQUIRE(ResourceSaver::save(owner, owner_path) == OK);
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (filesystem == nullptr) {
		return;
	}
	filesystem->update_file(path);
	filesystem->update_file(owner_path);
	const Dictionary owned = checkpoints.remove_project_path(path);
	CHECK_FALSE((bool)owned.get("ok", true));
	CHECK(Dictionary(owned.get("error", Dictionary())).get("code", String()) == "PATH_HAS_OWNERS");
	CHECK(FileAccess::exists(path));

	owner.unref();
	REQUIRE(DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(owner_path)) == OK);
	filesystem->update_file(owner_path);
	const Dictionary checkpoint = checkpoints.create_checkpoint(path, "file removal contract").get("data", Dictionary());
	const Dictionary removed = checkpoints.remove_project_path(path);
	CHECK((bool)removed.get("ok", false));
	if ((bool)removed.get("ok", false)) {
		CHECK_FALSE(FileAccess::exists(path));
		CHECK(target->get_path().is_empty());
		CHECK((bool)checkpoints.restore_checkpoint_state(checkpoint).get("ok", false));
		CHECK(FileAccess::get_sha256(path) == sha);
		CHECK(target->get_path() == path);
		CHECK(filesystem->find_file(path, nullptr) != nullptr);
	}
}

TEST_CASE("[SolersToolRegistry] session rewind stops at the recorded irreversible boundary") {
	SolersToolRegistry registry;
	Dictionary barrier({ { "id", "barrier-5" }, { "session_id", "session-a" }, { "session_revision", 5 }, { "domains", PackedStringArray({ "irreversible" }) } });
	Array records;
	records.push_back(barrier);
	registry.restore_session_reversals("session-a", records);
	const Dictionary blocked = registry.preview_session_rewind("session-a", 4);
	CHECK_FALSE((bool)blocked.get("ok", true));
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "REWIND_IRREVERSIBLE_BOUNDARY");
	const Dictionary after_boundary = registry.preview_session_rewind("session-a", 5);
	REQUIRE((bool)after_boundary.get("ok", false));
	CHECK((int)Dictionary(after_boundary.get("data", Dictionary())).get("action_count", -1) == 0);
}

TEST_CASE("[SolersToolRegistry] model surface keeps stable script authorities direct and narrow mutations deferred") {
	SolersFileCheckpoint checkpoints;
	SolersProjectObservation project_observation;
	SolersReflectionService reflection_service;
	SolersResourceService resource_service;
	SolersRuntimeObservation runtime_observation;
	SolersSceneObservation scene_observation;
	SolersScriptService script_service;
	SolersToolRegistry registry;
	registry.set_file_checkpoint(&checkpoints);
	registry.set_project_observation(&project_observation);
	registry.set_reflection_service(&reflection_service);
	registry.set_resource_service(&resource_service);
	registry.set_runtime_observation(&runtime_observation);
	registry.set_scene_observation(&scene_observation);
	registry.set_script_service(&script_service);
	registry.register_default_tools();
	const Array catalog = registry.list_tools();
	PackedStringArray model_tools;
	for (const Variant &item : catalog) {
		const Dictionary tool = item;
		if (tool.get("exposure", String()) == "model") {
			model_tools.push_back(tool.get("name", String()));
		}
	}
	model_tools.sort();
	CHECK(model_tools == PackedStringArray({ "asset.script", "engine.describe", "object.inspect", "project.read_file", "project.search", "render.capture", "resource.inspect", "runtime.observe", "runtime.script", "scene.inspect", "scene.script", "script.validate", "skill.read", "spatial.inspect", "tool.search" }));
	const Dictionary project_path = solers_test_find_dictionary(catalog, SNAME("name"), "project.path");
	CHECK(project_path.get("exposure", String()) == "deferred");
	const Dictionary action = Dictionary(Dictionary(project_path.get("input_schema", Dictionary())).get("properties", Dictionary())).get("action", Dictionary());
	CHECK(Array(action.get("enum", Array())).has("remove"));
	CHECK(registry.get_model_tool_name(SNAME("project.path")) == "project_path");
	CHECK(registry.resolve_model_tool_name("project_path") == "project.path");
	for (const String &name : PackedStringArray({ "scene.script", "asset.script", "runtime.script" })) {
		const Dictionary tool = solers_test_find_dictionary(catalog, SNAME("name"), name);
		CHECK(tool.get("exposure", String()) == "model");
		CHECK(Dictionary(tool.get("input_schema", Dictionary())).has("properties"));
		CHECK(registry.redact_tool_args_for_audit(name, Dictionary({ { "source", "secret source" } })).get("source", String()) == "<redacted>");
	}
	const Array scene_access = registry.resolve_resource_access(SNAME("scene.script"), Dictionary({ { "scene_path", "res://level.tscn" }, { "outputs", Array({ "res://level.lmbake" }) } }));
	CHECK(scene_access.has(Dictionary({ { "mode", "write" }, { "key", "project:res://level.tscn" } })));
	CHECK(scene_access.has(Dictionary({ { "mode", "write" }, { "key", "project:res://level.lmbake" } })));
	const Array asset_access = registry.resolve_resource_access(SNAME("asset.script"), Dictionary({ { "asset_path", "res://model.glb" } }));
	CHECK(asset_access.has(Dictionary({ { "mode", "write" }, { "key", "project:res://model.glb.import" } })));
}

TEST_CASE("[SolersScriptContext] native jobs are discovered by authority and target class") {
	Node *scene = memnew(Node);
	LightmapGI *lightmap = memnew(LightmapGI);
	scene->add_child(lightmap);
	Ref<SolersScriptContext> scene_context;
	scene_context.instantiate();
	scene_context->initialize(SNAME("scene"), scene, "res://level.tscn", Dictionary(), false, PackedStringArray({ "res://level.lmbake" }), String(), String(), 0);
	const Array jobs = scene_context->list_native_jobs(lightmap);
	REQUIRE(jobs.size() == 1);
	const Dictionary job = jobs[0];
	CHECK(StringName(job.get("id", StringName())) == SNAME("lightmap.bake"));
	CHECK(Array(Dictionary(job.get("input_schema", Dictionary())).get("required", Array())).has("output_path"));

	Ref<SolersScriptContext> runtime_context;
	runtime_context.instantiate();
	runtime_context->initialize(SNAME("runtime"), scene, String(), Dictionary(), false, PackedStringArray(), String(), String(), 0);
	CHECK(runtime_context->list_native_jobs(lightmap).is_empty());
	memdelete(scene);
}

TEST_CASE("[SolersToolRegistry] registered deferred tools execute through direct lookup") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	const int tool_count = registry.get_tool_count();
	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_domains = SolersToolMutationDomain::NONE;
	cap.operation_domain = SolersOperationDomain::EDITOR;
	cap.operation_mode = SolersOperationMode::QUERY;
	registry.register_tool(memnew(SolersFunctionTool(
			StringName("synthetic.future"),
			"Future operation fixture.",
			empty_tool_schema(), SolersToolExposure::DEFERRED, cap,
			[](const SolersToolContext &, const Dictionary &) {
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "value", 42 } }) } });
			})));
	CHECK(registry.get_tool_count() == tool_count + 1);
	const Dictionary definition = registry.get_tool_definition(SNAME("synthetic.future"));
	REQUIRE_FALSE(definition.is_empty());
	CHECK(Dictionary(definition.get("input_schema", Dictionary())).recursive_equal(empty_tool_schema(), 0));
	const Dictionary result = registry.call_tool("synthetic.future", Dictionary());
	REQUIRE((bool)result.get("ok", false));
	CHECK((int)Dictionary(result.get("data", Dictionary())).get("value", 0) == 42);
}

TEST_CASE("[SolersToolRegistry] execution readiness follows direct tool metadata") {
	SolersToolRegistry registry;
	registry.register_default_tools();
	bool ready = false;
	SolersToolCapability capability;
	capability.operation_domain = SolersOperationDomain::EDITOR;
	capability.operation_mode = SolersOperationMode::APPLY;
	capability.execution_ready = [&ready](const Dictionary &) { return ready; };
	registry.register_tool(memnew(SolersFunctionTool(
			"synthetic.readiness",
			"Synthetic readiness contract.",
			empty_tool_schema(), SolersToolExposure::DEFERRED, capability,
			[](const SolersToolContext &, const Dictionary &) {
				return Dictionary({ { "ok", true }, { "data", Dictionary() } });
			})));
	CHECK_FALSE(registry.is_execution_ready("synthetic.readiness", Dictionary()));
	ready = true;
	CHECK(registry.is_execution_ready("synthetic.readiness", Dictionary()));
}

TEST_CASE("[SolersToolRegistry] normalize_tool_args is public and idempotent") {
	SolersToolRegistry registry;

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_domains = SolersToolMutationDomain::NONE;

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
			schema, SolersToolExposure::MODEL, cap,
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

TEST_CASE("[SolersToolRegistry] tool presentation follows capability data") {
	SolersToolRegistry registry;
	SolersToolCapability nested_capability;
	nested_capability.ui_kind = SolersToolUiKind::READ;
	nested_capability.ui_subject_paths = PackedStringArray({ "/items/0/label" });
	Dictionary nested_schema = empty_tool_schema();
	registry.register_tool(memnew(SolersFunctionTool("synthetic.presentation", "Synthetic presentation contract.", nested_schema, SolersToolExposure::MODEL, nested_capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary({ { "ok", true }, { "data", Dictionary() } }); })));

	const Dictionary definition = registry.get_tool_definition("synthetic.presentation");
	CHECK(Dictionary(definition.get("ui", Dictionary())).get("running", String()) == "Reading");
	CHECK(Dictionary(definition.get("ui", Dictionary())).get("completed", String()) == "Read");
	const Dictionary nested_args({ { "items", Array({ Dictionary({ { "label", "unseen-subject" } }) }) } });
	CHECK(registry.summarize_tool_args_for_ui("synthetic.presentation", nested_args) == "unseen-subject");

	SolersToolCapability access_capability;
	access_capability.mutation_domains = SolersToolMutationDomain::FILES;
	access_capability.operation_mode = SolersOperationMode::APPLY;
	access_capability.resource_access = [](const Dictionary &p_args) {
		return Array({ Dictionary({ { "mode", "write" }, { "key", "project:" + String(p_args.get("destination", String())) } }) });
	};
	registry.register_tool(memnew(SolersFunctionTool("synthetic.access-presentation", "Synthetic access presentation contract.", empty_tool_schema(), SolersToolExposure::MODEL, access_capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary({ { "ok", true }, { "data", Dictionary() } }); })));
	CHECK(Dictionary(registry.get_tool_definition("synthetic.access-presentation").get("ui", Dictionary())).get("running", String()) == "Writing");
	CHECK(registry.summarize_tool_args_for_ui("synthetic.access-presentation", Dictionary({ { "destination", "res://result.data" } })) == "res://result.data");
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
	cap.mutation_domains = SolersToolMutationDomain::IRREVERSIBLE;
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
			schema, SolersToolExposure::MODEL, cap,
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
TEST_CASE("[SolersToolRegistry][SceneTree][GLTF] scene.inspect returns native mesh handles and partial failures") {
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
	SolersSceneObservation observation;
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_reflection_service(&service);
	registry.set_scene_observation(&observation);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	SceneTree *tree = SceneTree::get_singleton();
	Dictionary missing;
	{
		ScopedEditedSceneRoot edited_scene(tree, root);
		SolersToolContext context;
		Dictionary args({ { "node_paths", Array({ "Suzanne", "Missing" }) }, { "properties", Array({ "mesh", "missing_property" }) } });
		const Dictionary result = registry.call_tool_with_context(SNAME("scene.inspect"), args, context);
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
		const Array filtered = Array(Dictionary(registry.call_tool_with_context(SNAME("scene.inspect"), args, context).get("data", Dictionary())).get("nodes", Array()));
		REQUIRE_FALSE(filtered.is_empty());
		for (int i = 0; i < filtered.size(); i++) {
			CHECK(String(Dictionary(filtered[i]).get("name", String())).findn("zann") >= 0);
		}
		args["node_paths"] = Array({ "Missing" });
		args.erase("name_contains");
		missing = registry.call_tool_with_context(SNAME("scene.inspect"), args, context);
	}
	memdelete(root);
	CHECK(Dictionary(missing.get("error", Dictionary())).get("code", String()) == "NODE_QUERY_FAILED");
}
#endif

#ifdef MODULE_CSG_ENABLED
TEST_CASE("[SolersToolRegistry][SceneTree][CSG] scene.inspect returns native material mapping facts") {
	Node3D *root = memnew(Node3D);
	CSGBox3D *box = memnew(CSGBox3D);
	box->set_name("MappingContract");
	root->add_child(box);
	box->set_owner(root);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_uv1_scale(Vector3(0.75, 1.25, 2.5));
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, true);
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_WORLD_TRIPLANAR, true);
	material->set_uv1_triplanar_blend_sharpness(23.0);
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	box->set_material(material);

	SolersReflectionService reflection;
	SolersSceneObservation observation;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection);
	registry.set_scene_observation(&observation);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	{
		ScopedEditedSceneRoot edited_scene(SceneTree::get_singleton(), root);
		const Dictionary args({ { "node_paths", Array({ "MappingContract" }) } });
		const Array nodes = Dictionary(registry.call_tool(SNAME("scene.inspect"), args).get("data", Dictionary())).get("nodes", Array());
		REQUIRE(nodes.size() == 1);
		const Dictionary facts = Dictionary(nodes[0]).get("material", Dictionary());
		CHECK(facts.get("source", String()) == "csg_material");
		const Dictionary uv1 = facts.get("uv1", Dictionary());
		CHECK(Array(uv1.get("scale", Array())) == Array({ 0.75, 1.25, 2.5 }));
		CHECK(uv1.get("triplanar", false));
		CHECK(uv1.get("world_triplanar", false));
		CHECK(Math::is_equal_approx((double)uv1.get("triplanar_sharpness", 0.0), 23.0));
		const Dictionary normal = Dictionary(facts.get("channels", Dictionary())).get("normal", Dictionary());
		CHECK(normal.get("enabled", false));
	}
	memdelete(root);
}
#endif

TEST_CASE("[SolersToolRegistry] scene object operation access follows its native scope") {
	SolersReflectionService reflection_service;
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection_service);
	registry.register_default_tools();

	Dictionary write_args;
	write_args["node_path"] = "ReferenceCamera";
	write_args["properties"] = Dictionary({ { "fov", 55.0 } });
	CHECK_FALSE(registry.is_read_only("scene.node.update", write_args));
	const Array write_access = registry.resolve_resource_access("scene.node.update", write_args);
	REQUIRE(write_access.size() == 1);
	CHECK(Dictionary(write_access[0]).get("mode", String()) == "write");
	CHECK(Dictionary(write_access[0]).get("key", String()) == "scene:ReferenceCamera");

	Dictionary instantiate_args;
	instantiate_args["source_path"] = "res://props/tree.glb";
	instantiate_args["parent_path"] = "Environment";
	const Array instantiate_access = registry.resolve_resource_access("scene.instance.instantiate", instantiate_args);
	REQUIRE(instantiate_access.size() == 2);
	CHECK(Dictionary(instantiate_access[0]).get("mode", String()) == "read");
	CHECK(Dictionary(instantiate_access[0]).get("key", String()) == "project:res://props/tree.glb");
	CHECK(Dictionary(instantiate_access[1]).get("key", String()) == "scene:Environment");
}

TEST_CASE("[SolersToolRegistry][SceneTree][Editor] node updates are atomic native actions") {
	if (EditorNode::get_singleton() == nullptr) {
		return;
	}
	EditorData &editor_data = EditorNode::get_editor_data();
	const int previous_scene = editor_data.get_edited_scene();
	const int scene = editor_data.add_edited_scene(-1);
	editor_data.set_edited_scene(scene);
	Node3D *root = memnew(Node3D);
	Camera3D *camera = memnew(Camera3D);
	camera->set_name("ContractCamera");
	root->add_child(camera);
	camera->set_owner(root);
	editor_data.set_edited_scene_root(root);
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	REQUIRE(manager != nullptr);
	const int history_id = editor_data.get_current_edited_scene_history_id();
	UndoRedo *history = manager->get_or_create_history(history_id).undo_redo;
	SolersReflectionService reflection;

	{
		ScopedEditedSceneRoot edited_scene(SceneTree::get_singleton(), root);
		const uint64_t before = history->get_version();
		const Dictionary invalid = reflection.update_node(Dictionary({ { "node_path", "ContractCamera" }, { "properties", Dictionary({ { "position", Vector3(1, 2, 3) }, { "missing_property", 1 } }) } }));
		CHECK_FALSE((bool)invalid.get("ok", true));
		CHECK(camera->get_position() == Vector3());
		CHECK(history->get_version() == before);

		const Dictionary unchanged = reflection.update_node(Dictionary({ { "node_path", "ContractCamera" }, { "properties", Dictionary({ { "position", Vector3() } }) } }));
		CHECK_FALSE((bool)unchanged.get("ok", true));
		CHECK(Dictionary(unchanged.get("error", Dictionary())).get("code", String()) == "STATE_ALREADY_SATISFIED");

		const Dictionary updated = reflection.update_node(Dictionary({ { "node_path", "ContractCamera" }, { "properties", Dictionary({ { "position", Vector3(1, 2, 3) }, { "current", true } }) } }));
		REQUIRE((bool)updated.get("ok", false));
		CHECK(history->get_version() == before + 1);
		CHECK(camera->get_position() == Vector3(1, 2, 3));
		CHECK(camera->is_current());
		const Dictionary updated_data = updated.get("data", Dictionary());
		CHECK(String(updated_data.get("scene_file_path", String())).is_empty());
		CHECK(String(updated_data.get("owner_object_id", String())) == solers_object_id_to_string(root->get_instance_id()));
		CHECK(String(updated_data.get("instance_scene_path", String())).is_empty());
		CHECK(String(updated_data.get("node_object_id", String())) == solers_object_id_to_string(camera->get_instance_id()));
		CHECK_FALSE((bool)updated_data.get("inside_tree", true));
		CHECK(String(updated_data.get("edited_scene_root_object_id", String())) == solers_object_id_to_string(root->get_instance_id()));
		CHECK(String(updated_data.get("node_path", String())) == "ContractCamera");
		CHECK(Dictionary(updated_data.get("properties", Dictionary())).has("position"));
		REQUIRE(manager->undo_history(history_id));
		CHECK(camera->get_position() == Vector3());
		CHECK_FALSE(camera->is_current());
	}
	editor_data.remove_scene(scene);
	if (previous_scene >= 0) {
		editor_data.set_edited_scene(previous_scene);
	}
}

TEST_CASE("[SolersToolRegistry][Editor] editor tools do not require observed state before execution") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_SCENE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	SolersToolCapability capability;
	capability.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	capability.mutation_domains = SolersToolMutationDomain::EDITOR;
	registry.register_tool(memnew(SolersFunctionTool("synthetic.editor", "Synthetic editor contract.", empty_tool_schema(), SolersToolExposure::MODEL, capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary({ { "ok", true }, { "data", Dictionary() } }); })));
	const Dictionary result = registry.call_tool(SNAME("synthetic.editor"), Dictionary());
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "UNDO_HISTORY_UNAVAILABLE");
}

TEST_CASE("[SolersToolRegistry][SceneTree][Editor] scene.open follows EditorNode scene state") {
	SolersReflectionService reflection_service;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection_service);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	const Dictionary tool = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "scene.open");
	REQUIRE_FALSE(tool.is_empty());
	CHECK(tool.get("permission", String()) == "observe");
	CHECK(PackedStringArray(tool.get("mutation_domains", PackedStringArray())).has("irreversible"));
	CHECK(tool.get("exposure", String()) == "deferred");
	CHECK_FALSE(registry.affects_scene_state(SNAME("scene.open")));
	const Dictionary schema = tool.get("input_schema", Dictionary());
	CHECK(Dictionary(schema.get("properties", Dictionary())).has("path"));
	CHECK_FALSE(Dictionary(schema.get("properties", Dictionary())).has("set_inherited"));
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
	if (!EditorNode::get_singleton()) {
		CHECK(missing_code == "EDITOR_UNAVAILABLE");
		return;
	}

	const String scene_path = "res://.solers_scene_open_contract.tscn";
	SolersTestPaths cleanup;
	cleanup.add(scene_path);
	Node *source_root = memnew(Node);
	source_root->set_name("SceneOpenContract");
	Ref<PackedScene> packed;
	packed.instantiate();
	REQUIRE(packed->pack(source_root) == OK);
	memdelete(source_root);
	REQUIRE(ResourceSaver::save(packed, scene_path) == OK);
	packed.unref();

	Dictionary open_args;
	open_args["path"] = scene_path;
	const Dictionary opened = registry.call_tool(SNAME("scene.open"), open_args);
	REQUIRE((bool)opened.get("ok", false));
	const Dictionary opened_data = opened.get("data", Dictionary());
	CHECK(opened_data.get("scene_path", String()) == scene_path);
	CHECK(opened_data.get("root_object_id", Variant()).get_type() == Variant::STRING);

	const String imported_path = TestUtils::get_data_path("models/suzanne.glb");
	REQUIRE(ResourceLoader::is_imported(imported_path));
	open_args["path"] = imported_path;
	const Dictionary imported = registry.call_tool(SNAME("scene.open"), open_args);
	CHECK_FALSE((bool)imported.get("ok", true));
	CHECK(Dictionary(imported.get("error", Dictionary())).get("code", String()) == "IMPORTED_SCENE_READ_ONLY");
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
	const Dictionary generate_definition = registry.get_tool_definition(SNAME("asset.generate"));
	CHECK(PackedStringArray(generate_definition.get("attachment_args", PackedStringArray())).has("input_attachments"));

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
	Dictionary properties;
	for (int i = 0; i < 40; i++) {
		properties[vformat("property_%d", i)] = i;
	}
	const Dictionary args({ { "node_path", "." }, { "properties", properties } });
	const Dictionary audit = registry.redact_tool_args_for_audit(SNAME("scene.node.update"), args);
	CHECK(Dictionary(audit.get("properties", Dictionary())).size() == 40);
}

TEST_CASE("[SolersRuntimeInput][SceneTree] complete action states validate before mutation and release omissions") {
	solers_runtime_bridge_initialize();
	InputMap *input_map = InputMap::get_singleton();
	Input *input = Input::get_singleton();
	REQUIRE(input_map != nullptr);
	REQUIRE(input != nullptr);
	const StringName first = SNAME("solers_test_move");
	const StringName second = SNAME("solers_test_run");
	input_map->add_action(first);
	input_map->add_action(second);

	TestEngineDebugger debugger;
	Node *probe = memnew(Node);
	probe->set_name("SolersInputProbe");
	SceneTree::get_singleton()->get_root()->add_child(probe);
	const Array observations({ Dictionary({ { "node_path", probe->get_path() }, { "properties", Array({ "process_mode" }) } }) });
	auto apply = [&](const Array &p_actions, bool p_valid = true) {
		bool captured = false;
		debugger.last_message = String();
		const Error err = debugger.capture_parse(SNAME("solers"), "set_input_actions", { "input-contract", 7, p_actions, 2, observations }, captured);
		CHECK(err == OK);
		CHECK(captured);
		if (p_valid) {
			SceneTree::get_singleton()->emit_signal(SNAME("physics_frame"));
			CHECK(debugger.last_message.is_empty());
			SceneTree::get_singleton()->emit_signal(SNAME("physics_frame"));
			CHECK(debugger.last_message == "solers:input_result");
			const Dictionary data = debugger.last_data[0];
			CHECK(Dictionary(data.get("availability", Dictionary())).get("state", String()) == "complete");
			CHECK((int)data.get("physics_frames", 0) == 2);
		}
	};
	apply(Array({ Dictionary({ { "name", first }, { "strength", 0.75 } }) }));
	CHECK(input->is_action_pressed(first));
	CHECK(Math::is_equal_approx(input->get_action_strength(first), 0.75f));
	CHECK_FALSE(input->is_action_pressed(second));

	apply(Array({ Dictionary({ { "name", first }, { "strength", 0.25 } }), Dictionary({ { "name", "solers_missing_action" }, { "strength", 1.0 } }) }), false);
	CHECK(input->is_action_pressed(first));
	CHECK(Math::is_equal_approx(input->get_action_strength(first), 0.75f));

	apply(Array({ Dictionary({ { "name", second }, { "strength", 1.0 } }) }));
	CHECK_FALSE(input->is_action_pressed(first));
	CHECK(input->is_action_pressed(second));
	apply(Array());
	CHECK_FALSE(input->is_action_pressed(first));
	CHECK_FALSE(input->is_action_pressed(second));
	probe->queue_free();
	MessageQueue::get_singleton()->flush();
	input_map->erase_action(first);
	input_map->erase_action(second);

	SolersRuntimeObservation observation;
	SolersToolRegistry registry;
	registry.set_runtime_observation(&observation);
	registry.register_default_tools();
	const Dictionary tool = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "runtime.control");
	const Dictionary properties = Dictionary(tool.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(Array(Dictionary(properties.get("action", Dictionary())).get("enum", Array())).has("set_input_actions"));
	CHECK(properties.has("observation_id"));
	CHECK(properties.has("physics_frames"));
	CHECK((int)Dictionary(properties.get("observations", Dictionary())).get("minItems", 0) == 1);
	CHECK_FALSE(properties.has("expected_value"));
	const Dictionary action_item = Dictionary(properties.get("actions", Dictionary())).get("items", Dictionary());
	CHECK(Array(action_item.get("required", Array())).has("name"));
	CHECK(Array(action_item.get("required", Array())).has("strength"));
	const Dictionary observe_tool = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "runtime.observe");
	const Dictionary observe_properties = Dictionary(observe_tool.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(Array(Dictionary(observe_properties.get("target", Dictionary())).get("enum", Array())).has("spatial"));
	CHECK((int)Dictionary(observe_properties.get("focus_paths", Dictionary())).get("minItems", 0) == 1);
}

TEST_CASE("[SolersRuntimeBridge][SceneTree] exact object observations use native property usage and ownership") {
	solers_runtime_bridge_initialize();
	TestEngineDebugger debugger;
	bool malformed_captured = false;
	CHECK(debugger.capture_parse(SNAME("solers"), "observe_objects", { "", 0, Dictionary() }, malformed_captured) == OK);
	CHECK(malformed_captured);
	CHECK(debugger.last_message == "solers:objects_result");
	REQUIRE(debugger.last_data.size() == 1);
	CHECK_FALSE((bool)Dictionary(debugger.last_data[0]).get("ok", true));
	CHECK(Dictionary(debugger.last_data[0]).get("code", String()) == "INVALID_OBSERVATION_REQUEST");
	malformed_captured = false;
	CHECK(debugger.capture_parse(SNAME("solers"), "observe_frame", { "missing-frame-payload", 11 }, malformed_captured) == OK);
	CHECK(malformed_captured);
	CHECK(debugger.last_message == "solers:frame_result");
	CHECK_FALSE((bool)Dictionary(debugger.last_data[0]).get("ok", true));

#ifdef MODULE_GDSCRIPT_ENABLED
	const bool languages_initialized = ScriptServer::are_languages_initialized();
	if (!languages_initialized) {
		ScriptServer::init_languages();
	}
	debugger.last_message.clear();
	debugger.last_data.clear();
	const String runtime_source = "extends RefCounted\nfunc run(ctx):\n    return {\"authority\": String(ctx.authority), \"root\": ctx.subject.name}\n";
	bool script_captured = false;
	CHECK(debugger.capture_parse(SNAME("solers"), "run_script", { "script-contract", 11, runtime_source, (int64_t)(OS::get_singleton()->get_ticks_msec() + 1000) }, script_captured) == OK);
	CHECK(script_captured);
	CHECK(debugger.last_message == "solers:script_result");
	REQUIRE(debugger.last_data.size() == 1);
	const Dictionary script_result = debugger.last_data[0];
	CHECK((bool)script_result.get("ok", false));
	const Dictionary script_value = Dictionary(script_result.get("data", Dictionary())).get("result", Dictionary());
	CHECK(script_value.get("authority", String()) == "runtime");
	CHECK_FALSE(String(script_value.get("root", String())).is_empty());
	if (!languages_initialized) {
		ScriptServer::finish_languages();
	}
#endif

	Node *scene = memnew(Node);
	scene->set_name("SolersRuntimeContract");
	scene->set_scene_file_path("res://runtime_contract.tscn");
	SceneTree::get_singleton()->get_root()->add_child(scene);
	AnimationPlayer *player = memnew(AnimationPlayer);
	player->set_name("Player");
	player->set_scene_file_path("res://player_contract.tscn");
	scene->add_child(player);
	player->set_owner(scene);
	MeshInstance3D *mesh_instance = memnew(MeshInstance3D);
	mesh_instance->set_name("Mesh");
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh_instance->set_mesh(mesh);
	scene->add_child(mesh_instance);
	mesh_instance->set_owner(scene);

	const String object_id = solers_object_id_to_string(player->get_instance_id());
	const String node_path = String(player->get_path());
	Dictionary request;
	request["object_id"] = object_id;
	request["node_path"] = node_path;
	request["properties"] = Array({ "speed_scale", "current_animation_length" });
	Dictionary stale_request = request.duplicate(true);
	stale_request["node_path"] = node_path + "/Moved";
	Dictionary invalid_id_request = request.duplicate(true);
	invalid_id_request["object_id"] = 42;
	Dictionary invalid_property_request = request.duplicate(true);
	invalid_property_request["properties"] = "speed_scale";
	Dictionary mesh_request;
	mesh_request["object_id"] = solers_object_id_to_string(mesh_instance->get_instance_id());
	mesh_request["node_path"] = String(mesh_instance->get_path());
	mesh_request["properties"] = Array({ "mesh" });
	Dictionary identity_request = request.duplicate(true);
	identity_request["properties"] = Array();
	bool captured = false;
	const Error err = debugger.capture_parse(SNAME("solers"), "observe_objects", { "objects-contract", 11, Array({ request, stale_request, "invalid", invalid_id_request, invalid_property_request, mesh_request, identity_request }) }, captured);
	CHECK(err == OK);
	CHECK(captured);
	CHECK(debugger.last_message == "solers:objects_result");
	REQUIRE(debugger.last_data.size() == 1);
	const Dictionary result = debugger.last_data[0];
	CHECK((bool)result.get("ok", false));
	CHECK_FALSE((bool)result.get("complete", true));
	CHECK((int)result.get("requested_count", 0) == 7);
	const Array nodes = result.get("nodes", Array());
	REQUIRE(nodes.size() == 3);
	const Dictionary observed = nodes[0];
	CHECK(observed.get("object_id", String()) == object_id);
	CHECK(observed.get("node_path", String()) == node_path);
	CHECK(observed.get("owner_path", String()) == String(scene->get_path()));
	CHECK(observed.get("scene_file_path", String()) == "res://player_contract.tscn");
	CHECK(String(observed.get("class_name", String())) == "AnimationPlayer");
	CHECK(String(observed.get("node_path", String())) == node_path);
	CHECK(String(observed.get("object_id", String())) == object_id);
	CHECK(observed.has("properties"));
	CHECK(observed.has("property_info"));
	const Dictionary properties = observed.get("properties", Dictionary());
	CHECK(Math::is_equal_approx((double)properties.get("speed_scale", 0.0), 1.0));
	CHECK_FALSE(properties.has("current_animation_length"));
	const Dictionary property_info = observed.get("property_info", Dictionary());
	const PropertyInfo speed_info = PropertyInfo::from_dict(property_info.get("speed_scale", Dictionary()));
	CHECK(speed_info.type == Variant::FLOAT);
	CHECK((speed_info.usage & PROPERTY_USAGE_EDITOR) != 0);
	const Dictionary observed_mesh = nodes[1];
	CHECK(observed_mesh.get("owner_path", String()) == String(scene->get_path()));
	const String mesh_wire = Dictionary(observed_mesh.get("properties", Dictionary())).get("mesh", String());
	ObjectID decoded_mesh_id;
	CHECK(solers_object_id_from_variant(mesh_wire, decoded_mesh_id));
	CHECK(decoded_mesh_id == mesh->get_instance_id());
	const PropertyInfo mesh_info = PropertyInfo::from_dict(Dictionary(observed_mesh.get("property_info", Dictionary())).get("mesh", Dictionary()));
	CHECK(mesh_info.type == Variant::OBJECT);
	CHECK(mesh_info.class_name == Mesh::get_class_static());
	const Dictionary observed_identity = nodes[2];
	CHECK(observed_identity.get("owner_path", String()) == String(scene->get_path()));
	CHECK(observed_identity.get("scene_file_path", String()) == "res://player_contract.tscn");
	CHECK(Dictionary(observed_identity.get("properties", Dictionary())).is_empty());
	CHECK(Dictionary(observed_identity.get("property_info", Dictionary())).is_empty());

	bool saw_hidden = false;
	bool saw_stale = false;
	bool saw_invalid_request = false;
	bool saw_invalid_id = false;
	bool saw_invalid_properties = false;
	const Array errors = result.get("errors", Array());
	CHECK(errors.size() == 5);
	for (int i = 0; i < errors.size(); i++) {
		const Dictionary error = errors[i];
		saw_hidden = saw_hidden || (String(error.get("property", String())) == "current_animation_length" && String(error.get("reason", String())) == "property_not_observable");
		saw_stale = saw_stale || String(error.get("reason", String())) == "node_path_changed";
		saw_invalid_request = saw_invalid_request || String(error.get("reason", String())) == "invalid_object_request";
		saw_invalid_id = saw_invalid_id || String(error.get("reason", String())) == "invalid_object_id";
		saw_invalid_properties = saw_invalid_properties || String(error.get("reason", String())) == "invalid_property_request";
	}
	CHECK(saw_hidden);
	CHECK(saw_stale);
	CHECK(saw_invalid_request);
	CHECK(saw_invalid_id);
	CHECK(saw_invalid_properties);
	SceneTree::get_singleton()->get_root()->remove_child(scene);
	memdelete(scene);
}

} // namespace TestSolersTools
