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

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/tcp_server.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_builtin_skills.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_retry.h"
#include "modules/solers_ai/llm/solers_models_dev.h"
#include "modules/solers_ai/llm/solers_protocol_anthropic_messages.h"
#include "modules/solers_ai/llm/solers_protocol_openai_chat.h"
#include "modules/solers_ai/llm/solers_protocol_openai_responses.h"
#include "modules/solers_ai/editor/solers_chat_cells.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "modules/solers_modeling/core/solers_model_operation.h"
#include "modules/solers_modeling/core/solers_model_source.h"
#include "scene/3d/path_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/main/resource_preloader.h"
#include "scene/resources/resource_format_text.h"
#include "scene/resources/curve.h"
#include "scene/resources/environment.h"
#include "scene/resources/mesh.h"
#include "scene/resources/3d/primitive_meshes.h"

namespace TestSolersProviderGateway {

struct ModelingTestFiles {
	Vector<String> paths;
	~ModelingTestFiles() {
		for (const String &path : paths) {
			if (FileAccess::exists(path)) {
				DirAccess::remove_absolute(path);
			}
		}
	}
	void add(const String &p_path) {
		paths.push_back(p_path);
		if (FileAccess::exists(p_path)) {
			DirAccess::remove_absolute(p_path);
		}
	}
};

class SolersTestImportFormatSupportQuery : public EditorFileSystemImportFormatSupportQuery {
public:
	Vector<String> extensions;
	bool queried = false;

	virtual bool is_active() const override { return true; }
	virtual Vector<String> get_file_extensions() const override { return extensions; }
	virtual bool query() override {
		queried = true;
		return false;
	}
};

class SolersTestImportLifecycleObserver : public Object {
public:
	EditorFileSystem *filesystem = nullptr;
	bool observed = false;
	bool all_callbacks_inside_import = true;

	void on_resources_reimported(const PackedStringArray &) {
		observed = true;
		all_callbacks_inside_import = all_callbacks_inside_import && filesystem && filesystem->is_importing();
	}
};

Dictionary make_user_message(const String &p_text) {
	Dictionary message;
	message["role"] = "user";
	message["content"] = p_text;
	return message;
}

TEST_CASE("[SolersTrace] transcript parser skips incomplete audit records silently") {
	Dictionary record;
	CHECK_FALSE(solers_transcript_parse_record("", record));
	CHECK_FALSE(solers_transcript_parse_record("{incomplete", record));
	CHECK_FALSE(solers_transcript_parse_record("[]", record));
	CHECK(solers_transcript_parse_record("{\"kind\":\"tool_result\",\"call_id\":\"call_1\"}", record));
	CHECK(record.get("call_id", String()) == "call_1");
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

TEST_CASE("[SolersSecretStore] strict credentials are protected and recoverable") {
	const String secret = "synthetic-oauth-credential";
	const String stored = SolersSecretStore::protect_strict(secret);
	REQUIRE_FALSE(stored.is_empty());
	CHECK(SolersSecretStore::is_protected(stored));
	CHECK(stored != secret);
	CHECK(SolersSecretStore::unprotect(stored) == secret);
}

TEST_CASE("[SolersProviderRegistry] exposes canonical transport profiles") {
	SolersProviderRegistry registry;

	Dictionary openai = registry.get_provider_profile("openai");
	Dictionary anthropic = registry.get_provider_profile("anthropic_messages");

	CHECK(openai.get("kind", String()) == "openai_compatible");
	CHECK(openai.get("default_base_url", String()) == "https://api.openai.com/v1");
	CHECK(anthropic.get("kind", String()) == "anthropic_messages");
	CHECK(anthropic.get("default_base_url", String()) == "https://api.anthropic.com");
}

TEST_CASE("[SolersProviderRegistry] routes custom gateways through the explicit custom profile") {
	SolersProviderRegistry registry;

	Dictionary config;
	config["provider"] = "custom_openai_compatible";
	config["model"] = "synthetic-model";
	config["base_url"] = "https://gateway.example/v1";
	config["api_key"] = "synthetic-key";

	const Dictionary result = registry.validate_config(config);
	CHECK(result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("valid", false));
	CHECK(data.get("effective_base_url", String()) == "https://gateway.example/v1");

	Dictionary unknown = config;
	unknown["provider"] = "synthetic-unregistered-provider";
	const Dictionary rejected = registry.validate_config(unknown);
	CHECK_FALSE(rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "UNKNOWN_PROVIDER");
}

TEST_CASE("[Editor][SolersSettingsService] local model policy migrates without changing provider configuration") {
	EditorSettings *settings = EditorSettings::get_singleton();
	REQUIRE(settings != nullptr);

	const String prefix = "solers/ai/";
	Array paths;
	paths.push_back(prefix + "settings_version");
	paths.push_back(prefix + "privacy_mode");
	paths.push_back(prefix + "local_models_only");
	paths.push_back(prefix + "provider");
	for (const String &provider : { String("ollama"), String("custom_openai_compatible") }) {
		for (const String &key : { String("configured"), String("model"), String("base_url"), String("api_key") }) {
			paths.push_back(prefix + "providers/" + provider + "/" + key);
		}
	}
	Dictionary original;
	for (const Variant &path_value : paths) {
		const String path = path_value;
		if (settings->has_setting(path)) {
			original[path] = settings->get_setting(path);
		}
		settings->erase(path);
	}

	SolersProviderRegistry registry;
	for (const bool enabled : { false, true }) {
		settings->set_manually(prefix + "settings_version", 3);
		settings->set_manually(prefix + "privacy_mode", enabled);
		settings->erase(prefix + "local_models_only");
		SolersSettingsService migration_service;
		migration_service.set_provider_registry(&registry);
		CHECK(migration_service.get_local_models_only() == enabled);
		CHECK_FALSE(settings->has_setting(prefix + "privacy_mode"));
		CHECK((int)settings->get_setting(prefix + "settings_version") == 4);
	}

	settings->set_manually(prefix + "settings_version", 4);
	settings->erase(prefix + "local_models_only");
	SolersSettingsService service;
	service.set_provider_registry(&registry);
	CHECK_FALSE(service.get_local_models_only());

	Dictionary local;
	local["provider"] = "ollama";
	local["model"] = "qwen3";
	local["base_url"] = "http://127.0.0.1:11434";
	service.set_local_models_only(true);
	service.set_provider_config(local);
	Dictionary local_config = service.get_provider_config_for("ollama").get("data", Dictionary());
	CHECK(local_config.get("connected", false));
	CHECK(local_config.get("available", false));
	CHECK(service.get_local_models_only());

	Dictionary remote;
	remote["provider"] = "custom_openai_compatible";
	remote["model"] = "synthetic-model";
	remote["base_url"] = "https://gateway.example/v1";
	remote["api_key"] = "synthetic-key";
	service.set_local_models_only(false);
	service.set_provider_config(remote);
	service.set_local_models_only(true);
	Dictionary remote_config = service.get_provider_config_for("custom_openai_compatible").get("data", Dictionary());
	CHECK(remote_config.get("connected", false));
	CHECK_FALSE(remote_config.get("available", true));
	CHECK(remote_config.get("model", String()) == "synthetic-model");
	CHECK(remote_config.get("base_url", String()) == "https://gateway.example/v1");
	CHECK(Dictionary(service.get_provider_config().get("data", Dictionary())).get("provider", String()) == "custom_openai_compatible");

	SolersToolRegistry tool_registry;
	SolersAgentSession session;
	session.set_tool_registry(&tool_registry);
	session.set_settings_service(&service);
	Dictionary turn;
	turn["prompt"] = "This request must not reach the network.";
	const Dictionary blocked = session.start_turn(turn);
	CHECK_FALSE(blocked.get("ok", true));
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "LOCAL_MODELS_ONLY");

	service.set_provider_config(remote);
	CHECK(service.get_local_models_only());
	service.disconnect_provider("custom_openai_compatible");
	service.set_provider_config(remote);
	CHECK_FALSE(service.get_local_models_only());

	for (const Variant &path_value : paths) {
		const String path = path_value;
		settings->erase(path);
		if (original.has(path)) {
			settings->set_manually(path, original[path]);
		}
	}
	EditorSettings::save();
}

TEST_CASE("[SolersToolRegistry] registers tools by lookup, not a hardcoded catalog") {
	// Behavior contract (no-patch): a brand-new tool the dispatcher has never
	// special-cased becomes discoverable + dispatchable purely by registering
	// it — no catalog entry, no dispatcher branch, no name-prefix classifier.
	SolersToolRegistry registry;

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_kind = "none";
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

	CHECK(registry.get_tool_count() == 1);
	Array tools = registry.list_tools();
	REQUIRE(tools.size() == 1);
	Dictionary tool = tools[0];
	CHECK(tool.get("name", String()) == "synthetic.echo");
	CHECK(tool.get("model_name", String()) == "synthetic_echo");
	CHECK_FALSE((bool)tool.get("requires_approval", true));
	CHECK(tool.get("execution", String()) == "worker");
	const Dictionary retry_of = Dictionary(Dictionary(tool.get("input_schema", Dictionary())).get("properties", Dictionary())).get("retry_of", Dictionary());
	CHECK(String(retry_of.get("description", String())).contains("error.failure_id"));
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

TEST_CASE("[SolersToolRegistry][SolersModeling][SceneTree] projects every native operation without a second catalog") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, true);
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	HashMap<StringName, Dictionary> tools;
	for (const Variant &value : registry.list_tools()) {
		const Dictionary tool = value;
		tools[StringName(tool.get("name", String()))] = tool;
	}
	for (const SolersModelOperationDefinition &operation : SolersModelOperationRegistry::get_singleton()->get_operations()) {
		const StringName tool_name = StringName("model." + String(operation.id));
		INFO(tool_name);
		REQUIRE(tools.has(tool_name));
		const Dictionary tool = tools[tool_name];
		CHECK(tool.get("exposure", String()) == "deferred");
		CHECK(tool.get("undoable", false));
		const Dictionary schema = tool.get("input_schema", Dictionary());
		const Dictionary properties = schema.get("properties", Dictionary());
		const Dictionary operation_properties = operation.parameters_schema.get("properties", Dictionary());
		const Variant *key = nullptr;
		while ((key = operation_properties.next(key))) {
			CHECK(properties.get(*key, Variant()) == operation_properties[*key]);
		}
		CHECK(properties.has("path"));
		CHECK(properties.has("expected_revision"));
		CHECK(Array(schema.get("required", Array())).has("path"));
	}

	const String path = "res://.godot/solers_modeling_tool_contract.smodel";
	const String copy_path = "res://.godot/solers_modeling_tool_contract_copy.smodel";
	ModelingTestFiles cleanup;
	cleanup.add(path);
	cleanup.add(copy_path);
	Dictionary create;
	create["path"] = path;
	create["primitive"] = "box";
	REQUIRE((bool)registry.call_tool(SNAME("model.create"), create).get("ok", false));
	Dictionary inspect_args;
	inspect_args["path"] = path;
	const Dictionary inspected = registry.call_tool(SNAME("model.inspect"), inspect_args);
	REQUIRE((bool)inspected.get("ok", false));
	const int64_t revision = Dictionary(inspected.get("data", Dictionary())).get("revision", -1);
	Dictionary transform;
	transform["path"] = path;
	transform["expected_revision"] = revision;
	transform["translation"] = Vector3(1, 2, 3);
	REQUIRE((bool)registry.call_tool(SNAME("model.transform"), transform).get("ok", false));
	const Dictionary stale = registry.call_tool(SNAME("model.transform"), transform);
	CHECK_FALSE((bool)stale.get("ok", true));
	CHECK(Dictionary(stale.get("error", Dictionary())).get("code", String()) == "MODEL_REVISION_CONFLICT");

	const Dictionary after_transform = registry.call_tool(SNAME("model.inspect"), inspect_args);
	const int64_t transformed_revision = Dictionary(after_transform.get("data", Dictionary())).get("revision", -1);
	Dictionary configure;
	configure["path"] = path;
	configure["expected_revision"] = transformed_revision;
	configure["collision"] = "trimesh";
	REQUIRE((bool)registry.call_tool(SNAME("model.configure_build"), configure).get("ok", false));
	const Dictionary configured_data = Dictionary(registry.call_tool(SNAME("model.inspect"), inspect_args)).get("data", Dictionary());
	const int64_t configured_revision = configured_data.get("revision", -1);
	Dictionary failed_batch;
	failed_batch["path"] = path;
	failed_batch["expected_revision"] = configured_revision;
	Array failed_operations;
	Dictionary partial_transform;
	partial_transform["translation"] = Vector3(99, 0, 0);
	Dictionary partial_item;
	partial_item["operation"] = "transform";
	partial_item["parameters"] = partial_transform;
	failed_operations.push_back(partial_item);
	Dictionary invalid_item;
	invalid_item["operation"] = "not_an_operation";
	failed_operations.push_back(invalid_item);
	failed_batch["operations"] = failed_operations;
	CHECK_FALSE((bool)registry.call_tool(SNAME("model.batch"), failed_batch).get("ok", true));
	const Dictionary after_failed_batch = Dictionary(registry.call_tool(SNAME("model.inspect"), inspect_args)).get("data", Dictionary());
	const int64_t after_failed_revision = after_failed_batch.get("revision", -1);
	const String after_failed_hash = after_failed_batch.get("source_hash", String());
	const String configured_hash = configured_data.get("source_hash", String());
	CHECK(after_failed_revision == configured_revision);
	CHECK(after_failed_hash == configured_hash);
	Dictionary batch;
	batch["path"] = path;
	batch["expected_revision"] = configured_revision;
	Array operations;
	for (const Vector3 &offset : { Vector3(0.25, 0, 0), Vector3(0, 0.5, 0) }) {
		Dictionary item;
		item["operation"] = "transform";
		Dictionary parameters;
		parameters["translation"] = offset;
		item["parameters"] = parameters;
		operations.push_back(item);
	}
	batch["operations"] = operations;
	REQUIRE((bool)registry.call_tool(SNAME("model.batch"), batch).get("ok", false));
	CHECK((bool)registry.call_tool(SNAME("model.validate"), inspect_args).get("ok", false));

	Dictionary save;
	save["source_path"] = path;
	save["destination_path"] = copy_path;
	REQUIRE((bool)registry.call_tool(SNAME("model.save"), save).get("ok", false));
	Dictionary copy_args;
	copy_args["path"] = copy_path;
	const Dictionary original_data = Dictionary(registry.call_tool(SNAME("model.inspect"), inspect_args)).get("data", Dictionary());
	const Dictionary copied_data = Dictionary(registry.call_tool(SNAME("model.inspect"), copy_args)).get("data", Dictionary());
	CHECK(original_data.get("document", Dictionary()) == copied_data.get("document", Dictionary()));
	SolersEditableMesh persisted;
	REQUIRE(SolersModelSource::load(copy_path, persisted) == OK);
	Ref<ArrayMesh> runtime_mesh = persisted.compile();
	REQUIRE(runtime_mesh.is_valid());
	CHECK(runtime_mesh->has_meta("solers_build_settings"));
	CHECK(runtime_mesh->has_meta("solers_collision_shape"));
}

TEST_CASE("[SolersToolRegistry][SolersModeling][SceneTree] builds product-ready room and hard-surface prop") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, true);
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	ModelingTestFiles cleanup;
	const String room_path = "res://.godot/solers_modeling_room_acceptance.smodel";
	const String prop_path = "res://.godot/solers_modeling_prop_acceptance.smodel";
	cleanup.add(room_path);
	cleanup.add(prop_path);

	auto operation = [](const StringName &p_name, const Dictionary &p_parameters = Dictionary()) {
		Dictionary item;
		item["operation"] = p_name;
		item["parameters"] = p_parameters;
		return item;
	};
	auto box = [&](const Vector3 &p_size, const Vector3 &p_center) {
		Dictionary parameters;
		parameters["size"] = p_size;
		parameters["center"] = p_center;
		return operation(SNAME("create_box"), parameters);
	};

	Dictionary create_room;
	create_room["path"] = room_path;
	create_room["primitive"] = "box";
	Dictionary floor;
	floor["size"] = Vector3(8, 0.2, 6);
	floor["center"] = Vector3(0, -0.1, 0);
	create_room["parameters"] = floor;
	REQUIRE((bool)registry.call_tool(SNAME("model.create"), create_room).get("ok", false));
	Array room_operations;
	room_operations.push_back(box(Vector3(8, 0.2, 6), Vector3(0, 3.1, 0)));
	room_operations.push_back(box(Vector3(0.2, 3, 6), Vector3(-3.9, 1.5, 0)));
	room_operations.push_back(box(Vector3(0.2, 3, 6), Vector3(3.9, 1.5, 0)));
	room_operations.push_back(box(Vector3(2.5, 3, 0.2), Vector3(-2.75, 1.5, -2.9)));
	room_operations.push_back(box(Vector3(2.5, 3, 0.2), Vector3(2.75, 1.5, -2.9)));
	room_operations.push_back(box(Vector3(3, 0.8, 0.2), Vector3(0, 0.4, -2.9)));
	room_operations.push_back(box(Vector3(3, 0.7, 0.2), Vector3(0, 2.65, -2.9)));
	room_operations.push_back(box(Vector3(3.4, 3, 0.2), Vector3(-2.3, 1.5, 2.9)));
	room_operations.push_back(box(Vector3(3.4, 3, 0.2), Vector3(2.3, 1.5, 2.9)));
	room_operations.push_back(box(Vector3(1.2, 0.8, 0.2), Vector3(0, 2.6, 2.9)));
	room_operations.push_back(box(Vector3(0.1, 2.3, 0.12), Vector3(-0.65, 1.15, 2.78)));
	room_operations.push_back(box(Vector3(0.1, 2.3, 0.12), Vector3(0.65, 1.15, 2.78)));
	room_operations.push_back(box(Vector3(1.4, 0.1, 0.12), Vector3(0, 2.3, 2.78)));
	room_operations.push_back(box(Vector3(7.6, 0.12, 0.08), Vector3(0, 0.06, -2.76)));
	room_operations.push_back(box(Vector3(7.6, 0.12, 0.08), Vector3(0, 0.06, 2.76)));
	Dictionary trim_material;
	trim_material["material_index"] = 1;
	room_operations.push_back(operation(SNAME("set_material"), trim_material));
	Dictionary room_uv;
	room_uv["resolution"] = 512;
	room_uv["padding"] = 2;
	room_operations.push_back(operation(SNAME("unwrap_uv"), room_uv));
	Dictionary room_build;
	room_build["weighted_normals"] = true;
	room_build["generate_uv2"] = true;
	room_build["lightmap_texel_size"] = 0.1;
	room_build["collision"] = "trimesh";
	Array room_lods;
	Dictionary room_lod;
	room_lod["ratio"] = 0.5;
	room_lod["distance"] = 18.0;
	room_lods.push_back(room_lod);
	room_build["lod_levels"] = room_lods;
	room_operations.push_back(operation(SNAME("configure_build"), room_build));
	Dictionary room_batch;
	room_batch["path"] = room_path;
	room_batch["operations"] = room_operations;
	REQUIRE((bool)registry.call_tool(SNAME("model.batch"), room_batch).get("ok", false));
	Dictionary room_args;
	room_args["path"] = room_path;
	REQUIRE((bool)registry.call_tool(SNAME("model.validate"), room_args).get("ok", false));
	const Dictionary room_data = Dictionary(registry.call_tool(SNAME("model.inspect"), room_args)).get("data", Dictionary());
	CHECK((int)room_data.get("face_count", 0) >= 90);
	CHECK((int)room_data.get("boundary_edge_count", -1) == 0);
	CHECK((int)room_data.get("non_manifold_edge_count", -1) == 0);

	Dictionary create_prop;
	create_prop["path"] = prop_path;
	create_prop["primitive"] = "box";
	Dictionary prop_body;
	prop_body["size"] = Vector3(1.2, 0.45, 0.65);
	prop_body["center"] = Vector3(0.8, 0, 0);
	create_prop["parameters"] = prop_body;
	REQUIRE((bool)registry.call_tool(SNAME("model.create"), create_prop).get("ok", false));
	Array prop_operations;
	Dictionary cylinder;
	cylinder["segments"] = 24;
	cylinder["radius"] = 0.14;
	cylinder["depth"] = 0.8;
	cylinder["center"] = Vector3(0.8, 0.35, 0);
	prop_operations.push_back(operation(SNAME("create_cylinder"), cylinder));
	Dictionary accent_material;
	accent_material["material_index"] = 1;
	prop_operations.push_back(operation(SNAME("set_material"), accent_material));
	Dictionary mirror;
	mirror["type"] = "mirror";
	Dictionary mirror_parameters;
	mirror_parameters["axis"] = "x";
	mirror_parameters["origin"] = Vector3();
	mirror["parameters"] = mirror_parameters;
	prop_operations.push_back(operation(SNAME("add_modifier"), mirror));
	Dictionary array;
	array["type"] = "array";
	Dictionary array_parameters;
	array_parameters["count"] = 3;
	array_parameters["offset"] = Vector3(0, 0, 1.0);
	array["parameters"] = array_parameters;
	prop_operations.push_back(operation(SNAME("add_modifier"), array));
	Dictionary bevel;
	bevel["type"] = "bevel";
	Dictionary bevel_parameters;
	bevel_parameters["width"] = 0.035;
	bevel_parameters["segments"] = 2;
	bevel["parameters"] = bevel_parameters;
	prop_operations.push_back(operation(SNAME("add_modifier"), bevel));
	prop_operations.push_back(operation(SNAME("apply_modifiers")));
	Dictionary prop_uv;
	prop_uv["resolution"] = 512;
	prop_uv["padding"] = 2;
	prop_operations.push_back(operation(SNAME("unwrap_uv"), prop_uv));
	Dictionary prop_build;
	prop_build["weighted_normals"] = true;
	prop_build["generate_uv2"] = true;
	prop_build["collision"] = "convex";
	Array prop_lods;
	Dictionary prop_lod_near;
	prop_lod_near["ratio"] = 0.6;
	prop_lod_near["distance"] = 8.0;
	prop_lods.push_back(prop_lod_near);
	Dictionary prop_lod_far;
	prop_lod_far["ratio"] = 0.3;
	prop_lod_far["distance"] = 20.0;
	prop_lods.push_back(prop_lod_far);
	prop_build["lod_levels"] = prop_lods;
	prop_operations.push_back(operation(SNAME("configure_build"), prop_build));
	Dictionary prop_batch;
	prop_batch["path"] = prop_path;
	prop_batch["operations"] = prop_operations;
	REQUIRE((bool)registry.call_tool(SNAME("model.batch"), prop_batch).get("ok", false));
	Dictionary prop_args;
	prop_args["path"] = prop_path;
	REQUIRE((bool)registry.call_tool(SNAME("model.validate"), prop_args).get("ok", false));
	const Dictionary prop_data = Dictionary(registry.call_tool(SNAME("model.inspect"), prop_args)).get("data", Dictionary());
	CHECK((int)prop_data.get("face_count", 0) > 100);
	CHECK((int)prop_data.get("boundary_edge_count", -1) == 0);
	CHECK((int)prop_data.get("non_manifold_edge_count", -1) == 0);
}

TEST_CASE("[SolersToolRegistry] preserves internal session context without changing the bound API") {
	SolersToolRegistry registry;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_permission_manager(&permissions);

	SolersToolCapability cap;
	cap.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	cap.mutation_kind = "none";
	registry.register_tool(memnew(SolersFunctionTool(
			"synthetic.context", "Returns its internal execution context.", Dictionary(), SolersToolExposure::DIRECT, cap,
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

TEST_CASE("[SolersBuiltinSkills] compiled index exposes catalog summary without full content") {
	CHECK(SolersBuiltinSkills::get_count() >= 1);

	SolersBuiltinSkillView skill;
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-3d-scene-building", skill));
	CHECK(skill.name == "godot-3d-scene-building");
	CHECK(!skill.description.is_empty());
	CHECK(skill.content.contains("Prove one unfamiliar operation before repeating it"));
	CHECK(skill.content.contains("Omit `placement_roots` and `placements` until physical prop/fixture support must be verified"));
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-native-capabilities", skill));
	CHECK(skill.content.contains("Solers exposes Godot's real ClassDB"));
	REQUIRE(SolersBuiltinSkills::find_by_name("godot-rendering-lighting", skill));
	CHECK(skill.content.contains("one coherent renderer and one final GI path"));

	const String catalog = SolersBuiltinSkills::build_catalog_prompt();
	CHECK(catalog.contains("godot-3d-scene-building"));
	CHECK_FALSE(catalog.contains("Visual verification loop"));

	SolersBuiltinSkillView missing;
	CHECK_FALSE(SolersBuiltinSkills::find_by_name("synthetic-never-registered-skill", missing));
}

TEST_CASE("[SolersToolRegistry] skill.read serves compiled builtin skills") {
	SolersPermissionManager permissions;
	SolersToolRegistry registry;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	Dictionary args;
	args["name"] = "godot-3d-scene-building";
	const Dictionary result = registry.call_tool(StringName("skill.read"), args);
	REQUIRE((bool)result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("name", String()) == "godot-3d-scene-building");
	CHECK(!String(data.get("content", String())).is_empty());
	CHECK(String(data.get("content", String())).contains("Prove one unfamiliar operation before repeating it"));
	const Array required_tools = data.get("required_tools", Array());
	CHECK(required_tools.has("resource.get_info"));
	CHECK(required_tools.has("resource.create"));
	CHECK(required_tools.has("resource.set_property"));
	CHECK(required_tools.has("editor.invoke"));
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

TEST_CASE("[SolersToolRegistry] validate_builtin_skills passes when required tools are registered") {
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
	registry.validate_builtin_skills();

	HashSet<StringName> registered_tools;
	const Array definitions = registry.list_tools();
	for (int i = 0; i < definitions.size(); i++) {
		const Dictionary definition = definitions[i];
		registered_tools.insert(StringName(definition.get("name", String())));
	}
	CHECK(SolersBuiltinSkills::validate_required_tools(registered_tools).is_empty());
}

TEST_CASE("[SolersToolRegistry] default model surface is primitive-first") {
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

	const char *required_direct[] = {
		"class.search",
		"class.introspect",
		"objects.batch",
		"editor.get_snapshot",
		"project.read_file",
		"project.search_files",
		"project.write_file",
		"script.patch",
		"script.validate",
		"resource.get_info",
		"resource.create",
		"resource.get_property",
		"resource.set_property",
		"native.instantiate",
		"native.load",
		"native.list_properties",
		"native.get",
		"native.set",
		"native.list_methods",
		"native.call",
		"native.save",
		"native.free",
		"scene.instantiate",
		"object.get_property",
		"object.set_property",
		"object.call_method",
		"editor.invoke",
		"runtime.control",
		"tool.search",
		"skill.read",
	};
	for (const char *name : required_direct) {
		Dictionary tool = find_tool_def(tools, name);
		REQUIRE_FALSE(tool.is_empty());
		CHECK(tool.get("exposure", String()) == "direct");
	}
	const char *required_deferred[] = {
		"scene.validate_spatial",
		"scene.validate_structure",
		"scene.bake_csg",
		"mesh.unwrap_uv2",
		"lightmap.bake",
		"viewport.capture",
		"asset.catalog.search",
		"asset.catalog.inspect",
		"asset.catalog.acquire",
		"job.wait",
		"asset.list_local",
		"asset.import_to_project",
		"asset.generate",
		"asset.status",
	};
	for (const char *name : required_deferred) {
		Dictionary tool = find_tool_def(tools, name);
		REQUIRE_FALSE(tool.is_empty());
		CHECK(tool.get("exposure", String()) == "deferred");
	}
	CHECK(find_tool_def(tools, "asset.refine_to_ready").is_empty());
	CHECK(find_tool_def(tools, "asset.optimize_geometry").is_empty());
	CHECK(find_tool_def(tools, "asset.restyle_material").is_empty());
	CHECK(find_tool_def(tools, "ambientcg.search").is_empty());
	CHECK(find_tool_def(tools, "ambientcg.acquire").is_empty());
	Dictionary catalog_search = find_tool_def(tools, "asset.catalog.search");
	Dictionary catalog_search_properties = Dictionary(catalog_search.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(Array(Dictionary(catalog_search_properties.get("provider", Dictionary())).get("enum", Array())).has("ambientcg"));
	CHECK(Array(Dictionary(catalog_search_properties.get("provider", Dictionary())).get("enum", Array())).has("polyhaven"));
	CHECK(Array(Dictionary(catalog_search_properties.get("kind", Dictionary())).get("enum", Array())).has("3d"));
	Dictionary catalog_acquire = find_tool_def(tools, "asset.catalog.acquire");
	Dictionary catalog_acquire_properties = Dictionary(catalog_acquire.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(catalog_acquire_properties.has("source_version"));
	CHECK(Array(Dictionary(catalog_acquire_properties.get("kind", Dictionary())).get("enum", Array())).has("3d"));
	Dictionary catalog_inspect = find_tool_def(tools, "asset.catalog.inspect");
	Dictionary catalog_inspect_properties = Dictionary(catalog_inspect.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(catalog_inspect_properties.has("asset_id"));
	CHECK(Array(Dictionary(catalog_inspect_properties.get("kind", Dictionary())).get("enum", Array())).has("3d"));
	CHECK(Dictionary(Dictionary(find_tool_def(tools, "job.wait").get("input_schema", Dictionary())).get("properties", Dictionary())).has("ids"));
	Dictionary create_resource = find_tool_def(tools, "resource.create");
	Dictionary create_schema = create_resource.get("input_schema", Dictionary());
	Dictionary create_properties = create_schema.get("properties", Dictionary());
	CHECK(create_properties.has("properties"));
	CHECK_FALSE(Dictionary(Dictionary(find_tool_def(tools, "native.set").get("input_schema", Dictionary())).get("properties", Dictionary())).has("save"));
	CHECK_FALSE(Dictionary(Dictionary(find_tool_def(tools, "native.call").get("input_schema", Dictionary())).get("properties", Dictionary())).has("save"));
	const Dictionary native_save_schema = find_tool_def(tools, "native.save").get("input_schema", Dictionary());
	CHECK(Array(native_save_schema.get("required", Array())).has("path"));
	Dictionary batch_tool = find_tool_def(tools, "objects.batch");
	REQUIRE_FALSE(batch_tool.is_empty());
	CHECK_FALSE(String(batch_tool.get("description", String())).contains("call_method"));
	CHECK(String(batch_tool.get("description", String())).contains("empty editor scene"));
	CHECK(batch_tool.get("execution", String()) == "main_thread");
	CHECK(batch_tool.get("ephemeral_result", false));
	Dictionary batch_schema = batch_tool.get("input_schema", Dictionary());
	Dictionary batch_properties = batch_schema.get("properties", Dictionary());
	Dictionary operations_schema = batch_properties.get("operations", Dictionary());
	CHECK_FALSE(operations_schema.has("maxItems"));
	const String operations_description = operations_schema.get("description", String());
	CHECK(operations_description.contains("parent_path"));
	CHECK(operations_description.contains("new_parent_path"));
	CHECK(operations_description.contains("properties"));
	Dictionary operation_items = operations_schema.get("items", Dictionary());
	Dictionary operation_properties = operation_items.get("properties", Dictionary());
	Array operation_names = Dictionary(operation_properties.get("op", Dictionary())).get("enum", Array());
	CHECK(operation_names.has("get_property"));
	CHECK(operation_names.has("list_properties"));
	CHECK(operation_properties.has("node_path"));
	CHECK(operation_properties.has("property"));
	CHECK(operation_properties.has("max_properties"));
	Dictionary instantiate_scene = find_tool_def(tools, "scene.instantiate");
	Dictionary instantiate_properties = Dictionary(instantiate_scene.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(instantiate_properties.has("resource_path"));
	CHECK(instantiate_properties.has("parent_path"));
	CHECK(instantiate_properties.has("name"));
	CHECK(instantiate_properties.has("properties"));
	Dictionary structure_validation = find_tool_def(tools, "scene.validate_structure");
	Dictionary structure_properties = Dictionary(structure_validation.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(structure_properties.has("placement_roots"));
	CHECK(structure_properties.has("placements"));
	CHECK(structure_properties.has("reference_layout"));
	const Array required_structure_fields = Dictionary(structure_validation.get("input_schema", Dictionary())).get("required", Array());
	CHECK(required_structure_fields.has("structure_roots"));
	CHECK(required_structure_fields.has("relations"));
	CHECK_FALSE(required_structure_fields.has("placement_roots"));
	CHECK_FALSE(required_structure_fields.has("placements"));
	Dictionary relation_items = Dictionary(Dictionary(structure_properties.get("relations", Dictionary())).get("items", Dictionary()));
	Dictionary relation_properties = relation_items.get("properties", Dictionary());
	const Array relation_kinds = Dictionary(relation_properties.get("kind", Dictionary())).get("enum", Array());
	CHECK(relation_kinds.has("align"));
	CHECK(relation_kinds.has("no_overlap"));
	CHECK(relation_properties.has("a_anchor"));
	CHECK(relation_properties.has("b_anchor"));
	CHECK(String(find_tool_def(tools, "mesh.unwrap_uv2").get("description", String())).contains("off the editor thread"));
	CHECK(find_tool_def(tools, "class.introspect").get("ephemeral_result", false));
	CHECK(find_tool_def(tools, "viewport.capture").get("ephemeral_result", false));
	const Dictionary capture_properties = Dictionary(Dictionary(find_tool_def(tools, "viewport.capture").get("input_schema", Dictionary())).get("properties", Dictionary()));
	CHECK(Array(Dictionary(capture_properties.get("target", Dictionary())).get("enum", Array())).has("orthographic"));
	CHECK_FALSE(find_tool_def(tools, "scene.validate_spatial").get("produces_scene_validation", false));
	CHECK(find_tool_def(tools, "scene.validate_structure").get("produces_scene_validation", false));
	CHECK(find_tool_def(tools, "editor.action.list").is_empty());
	CHECK(find_tool_def(tools, "editor.action.execute").is_empty());
	CHECK_FALSE(find_tool_def(tools, "skill.read").get("ephemeral_result", true));
	Dictionary write_file = find_tool_def(tools, "project.write_file");
	CHECK(write_file.get("permission", String()) == "edit_files");
	Dictionary write_schema = write_file.get("input_schema", Dictionary());
	Dictionary write_properties = write_schema.get("properties", Dictionary());
	CHECK_FALSE(write_properties.has("reimport"));
	CHECK_FALSE(write_properties.has("validate_if_script"));
	Dictionary patch_file = find_tool_def(tools, "script.patch");
	Dictionary patch_schema = patch_file.get("input_schema", Dictionary());
	Dictionary patch_properties = patch_schema.get("properties", Dictionary());
	CHECK_FALSE(patch_properties.has("validate_if_script"));

	Dictionary search_files = find_tool_def(tools, "project.search_files");
	REQUIRE_FALSE(search_files.is_empty());
	CHECK(search_files.get("exposure", String()) == "direct");
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
	CHECK(editor_invoke.get("exposure", String()) == "direct");
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

	for (int i = 0; i < tools.size(); i++) {
		const Dictionary tool = tools[i];
		if (tool.get("exposure", String()) != "direct") {
			continue;
		}
		const String name = tool.get("name", String());
		CHECK_FALSE(name.begins_with("node."));
		CHECK_FALSE(name.begins_with("provider."));
		CHECK_FALSE(name.begins_with("timeline."));
		CHECK_FALSE(name.begins_with("rpc."));
		CHECK_FALSE(name.begins_with("approvals."));
		CHECK_FALSE(name.begins_with("validation."));
		CHECK(name != "runtime.capture_screenshot");
		CHECK(name != "editor.capture_screenshot");
	}
}

TEST_CASE("[SolersScriptService] script edits cannot bypass validation") {
	const String path = "res://solers_script_validation_contract.gd";
	const String fs_path = ProjectSettings::get_singleton()->globalize_path(path);
	if (FileAccess::exists(path)) {
		DirAccess::remove_file_or_error(fs_path);
	}

	SolersScriptService script_service;

	Dictionary bypass_args;
	bypass_args["path"] = path;
	bypass_args["content"] = "extends Node\n";
	bypass_args["validate_if_script"] = false;
	Dictionary bypass = script_service.write_file(bypass_args);
	REQUIRE_FALSE((bool)bypass.get("ok", true));
	Dictionary bypass_error = bypass.get("error", Dictionary());
	CHECK(bypass_error.get("code", String()) == "SCRIPT_VALIDATION_REQUIRED");
	CHECK_FALSE(FileAccess::exists(path));

	const String valid_source = "extends Node\nfunc value() -> int:\n\treturn 1\n";
	Dictionary write_args;
	write_args["path"] = path;
	write_args["content"] = valid_source;
	Dictionary written = script_service.write_file(write_args);
	REQUIRE((bool)written.get("ok", false));

	Dictionary patch_args;
	patch_args["path"] = path;
	patch_args["old_text"] = "\treturn 1";
	patch_args["new_text"] = "\treturn +";
	Dictionary patched = script_service.patch_file(patch_args);
	REQUIRE_FALSE((bool)patched.get("ok", true));
	Dictionary patch_error = patched.get("error", Dictionary());
	CHECK(patch_error.get("code", String()) == "SCRIPT_VALIDATE_FAILED");
	CHECK(FileAccess::get_file_as_string(path) == valid_source);

	DirAccess::remove_file_or_error(fs_path);
}

TEST_CASE("[SolersScriptService] native serialized resources require native resource APIs") {
	REQUIRE(ResourceFormatLoaderText::singleton != nullptr);
	List<String> extensions;
	ResourceFormatLoaderText::singleton->get_recognized_extensions(&extensions);
	REQUIRE_FALSE(extensions.is_empty());

	SolersScriptService script_service;
	for (const String &extension : extensions) {
		const String path = "res://solers_native_resource_contract." + extension;
		const String fs_path = ProjectSettings::get_singleton()->globalize_path(path);
		if (FileAccess::exists(path)) {
			DirAccess::remove_file_or_error(fs_path);
		}
		Dictionary args;
		args["path"] = path;
		args["content"] = "native resource text must not bypass ResourceSaver";
		const Dictionary result = script_service.write_file(args);
		CHECK_FALSE((bool)result.get("ok", true));
		CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "NATIVE_RESOURCE_WRITE_BLOCKED");
		CHECK_FALSE(FileAccess::exists(path));
	}
}

TEST_CASE("[SolersResourceService] native RefCounted workflow converts typed arrays and saves resources") {
	const String path = "res://solers_native_surface_contract.tres";
	const String fs_path = ProjectSettings::get_singleton()->globalize_path(path);
	if (FileAccess::exists(path)) {
		DirAccess::remove_file_or_error(fs_path);
	}

	SolersResourceService service;
	Dictionary instantiate_args;
	instantiate_args["class_name"] = "SurfaceTool";
	const Dictionary instantiated = service.native_instantiate(instantiate_args);
	REQUIRE((bool)instantiated.get("ok", false));
	const Dictionary surface_handle = instantiated.get("data", Dictionary());

	Dictionary call_args;
	call_args["object_id"] = surface_handle;
	call_args["method"] = "begin";
	Array invalid_begin_args;
	invalid_begin_args.push_back(Mesh::PRIMITIVE_TRIANGLES);
	invalid_begin_args.push_back(0);
	call_args["args"] = invalid_begin_args;
	const Dictionary invalid_begin = service.native_call(call_args);
	CHECK_FALSE((bool)invalid_begin.get("ok", true));
	CHECK(Dictionary(invalid_begin.get("error", Dictionary())).get("code", String()) == "INVALID_ARGUMENT_COUNT");

	Array begin_args;
	begin_args.push_back(Mesh::PRIMITIVE_TRIANGLES);
	call_args["args"] = begin_args;
	REQUIRE((bool)service.native_call(call_args).get("ok", false));

	Array vertices;
	Dictionary vertex;
	vertex["x"] = 0.0;
	vertex["y"] = 0.0;
	vertex["z"] = 0.0;
	vertices.push_back(vertex);
	vertex["x"] = 1.0;
	vertices.push_back(vertex);
	vertex["x"] = 0.0;
	vertex["y"] = 1.0;
	vertices.push_back(vertex);
	Array fan_args;
	fan_args.push_back(vertices);
	call_args["method"] = "add_triangle_fan";
	call_args["args"] = fan_args;
	REQUIRE((bool)service.native_call(call_args).get("ok", false));

	call_args["method"] = "generate_normals";
	call_args["args"] = Array();
	REQUIRE((bool)service.native_call(call_args).get("ok", false));
	call_args["method"] = "commit_to_arrays";
	const Dictionary committed = service.native_call(call_args);
	REQUIRE((bool)committed.get("ok", false));
	const Array surface_arrays = Dictionary(committed.get("data", Dictionary())).get("result", Array());
	REQUIRE(surface_arrays.size() == Mesh::ARRAY_MAX);
	CHECK(PackedVector3Array(surface_arrays[Mesh::ARRAY_VERTEX]).size() == 3);
	CHECK(PackedVector3Array(surface_arrays[Mesh::ARRAY_NORMAL]).size() == 3);

	instantiate_args["class_name"] = "ArrayMesh";
	const Dictionary mesh_instantiated = service.native_instantiate(instantiate_args);
	REQUIRE((bool)mesh_instantiated.get("ok", false));
	const Dictionary mesh_handle = mesh_instantiated.get("data", Dictionary());

	Dictionary save_args;
	save_args["object_id"] = mesh_handle;
	save_args["path"] = path;
	REQUIRE((bool)service.native_save(save_args).get("ok", false));
	REQUIRE(FileAccess::exists(path));

	const Ref<Resource> loaded_resource = ResourceLoader::load(path);
	const Ref<ArrayMesh> mesh = loaded_resource;
	REQUIRE(mesh.is_valid());
	CHECK(mesh->get_surface_count() == 0);

	Dictionary free_args;
	free_args["object_id"] = surface_handle;
	CHECK((bool)service.native_free(free_args).get("ok", false));
	free_args["object_id"] = mesh_handle;
	CHECK((bool)service.native_free(free_args).get("ok", false));
	DirAccess::remove_file_or_error(fs_path);
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
	result["ok"] = false;
	Dictionary error;
	error["code"] = "BATCH_FAILED";
	result["error"] = error;
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

	Dictionary export_query = search_deferred_tools(registry, "export preset", 10);
	REQUIRE((bool)export_query.get("ok", false));
	const bool found_export_tool = search_result_has_tool(export_query, "export.list_presets") ||
			search_result_has_tool(export_query, "export.run_preset") ||
			search_result_has_tool(export_query, "export.validate_presets");
	CHECK(found_export_tool);
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

	Dictionary editor_result = search_deferred_tools(registry, "editor invoke", 20);
	REQUIRE((bool)editor_result.get("ok", false));
	CHECK_FALSE(search_result_has_tool(editor_result, "editor.invoke"));
}

TEST_CASE("[SolersToolRegistry] tool.search uses broad OR matching and prioritizes names") {
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
			Dictionary(), SolersToolExposure::DEFERRED, cap,
			[](const SolersToolContext &, const Dictionary &) {
				Dictionary result;
				result["ok"] = true;
				return result;
			})));

	Dictionary result = search_deferred_tools(registry, "needle metadataquartz", 5);
	REQUIRE((bool)result.get("ok", false));
	CHECK(search_result_has_tool(result, "synthetic.opaque"));
	CHECK(search_result_has_tool(result, "synthetic.needle"));
	const Array matches = Dictionary(result.get("data", Dictionary())).get("tools", Array());
	REQUIRE(matches.size() >= 2);
	CHECK(Dictionary(matches[0]).get("name", String()) == "synthetic.needle");
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
	registry.register_tool(memnew(SolersFunctionTool("test.resource", "test", Dictionary(), SolersToolExposure::HIDDEN, capability,
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

	Dictionary first = registry.redact_tool_args_for_audit("synthetic.write", registry.normalize_tool_args("synthetic.write", first_args));
	Dictionary second = registry.redact_tool_args_for_audit("synthetic.write", registry.normalize_tool_args("synthetic.write", second_args));

	CHECK(first.get("path", String()) == second.get("path", String()));
	CHECK(first.get("content", String()) == "<redacted>");
	CHECK(second.get("content", String()) == "<redacted>");
	CHECK_FALSE(first.has("content_base64"));
	CHECK_FALSE(second.has("content_base64"));
	CHECK(first.size() == second.size());
}

TEST_CASE("[SolersToolRegistry] replay protection preserves sensitive arguments without plaintext") {
	SolersToolRegistry registry;
	SolersToolCapability cap;
	cap.redact_args.push_back("content");
	registry.register_tool(memnew(SolersFunctionTool(
			StringName("synthetic.replay"), "Replay fixture.", Dictionary(), SolersToolExposure::DIRECT, cap,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary(); })));

	Dictionary args;
	args["path"] = "res://same.gd";
	args["content"] = "sensitive replay payload";
	const Dictionary protected_args = registry.protect_tool_args_for_replay(SNAME("synthetic.replay"), args);
	CHECK(protected_args.get("path", String()) == "res://same.gd");
	CHECK_FALSE(JSON::stringify(protected_args).contains("sensitive replay payload"));
	const Dictionary restored = registry.restore_tool_args_from_replay(SNAME("synthetic.replay"), protected_args);
	CHECK(restored.get("content", String()) == "sensitive replay payload");
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
		CHECK_FALSE((bool)result.get("ok", true));
		Dictionary batch_error = result.get("error", Dictionary());
		CHECK(batch_error.get("code", String()) == "BATCH_FAILED");
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
	CHECK_FALSE((bool)unknown.get("ok", true));
	Dictionary batch_error = unknown.get("error", Dictionary());
	CHECK(batch_error.get("code", String()) == "BATCH_FAILED");
	Dictionary data = unknown.get("data", Dictionary());
	Array entries = data.get("results", Array());
	REQUIRE(entries.size() == 1);
	Dictionary entry = entries[0];
	Dictionary op_result = entry.get("result", Dictionary());
	Dictionary error = op_result.get("error", Dictionary());
	CHECK(error.get("code", String()) == "UNKNOWN_OP");

	Dictionary method_op;
	method_op["op"] = "call_method";
	Array method_ops;
	method_ops.push_back(method_op);
	args["operations"] = method_ops;
	Dictionary method_result = reflection_service.batch(args);
	Dictionary method_data = method_result.get("data", Dictionary());
	Array method_entries = method_data.get("results", Array());
	REQUIRE(method_entries.size() == 1);
	Dictionary method_entry = method_entries[0];
	Dictionary method_error = Dictionary(method_entry.get("result", Dictionary())).get("error", Dictionary());
	CHECK(method_error.get("code", String()) == "UNKNOWN_OP");
}

TEST_CASE("[SolersReflectionService] batch has no fixed operation-count cutoff") {
	SolersReflectionService reflection_service;
	Array operations;
	for (int i = 0; i < 65; i++) {
		Dictionary operation;
		operation["op"] = "synthetic_future_op";
		operations.push_back(operation);
	}
	Dictionary args;
	args["operations"] = operations;
	const Dictionary result = reflection_service.batch(args);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "BATCH_FAILED");
}

TEST_CASE("[SolersResourceService] property coercion accepts named and nested Godot components") {
	Node3D *node = memnew(Node3D);
	Dictionary position;
	position["x"] = 1;
	position["y"] = 2.5;
	position["z"] = -3;
	Variant coerced;
	String error;
	REQUIRE(solers_coerce_property_value(node, SNAME("position"), position, coerced, error));
	CHECK(Vector3(coerced) == Vector3(1, 2.5, -3));

	Dictionary origin;
	origin["x"] = 4;
	origin["y"] = 5;
	origin["z"] = 6;
	Dictionary transform;
	transform["origin"] = origin;
	REQUIRE(solers_coerce_property_value(node, SNAME("transform"), transform, coerced, error));
	CHECK(Transform3D(coerced).origin == Vector3(4, 5, 6));
	memdelete(node);
}

TEST_CASE("[SolersReflectionService] structural topology requires one real contact graph") {
	SolersReflectionService reflection_service;
	Array single_member;
	single_member.push_back("RoomShell");
	CHECK(reflection_service.validate_structure_topology(single_member, Array()).get("ok", false));
	Array members;
	members.push_back("Room/Structure/Floor");
	members.push_back("Room/Structure/Wall");
	members.push_back("Room/FixedFinish/DoorTrim");

	Dictionary floor_wall;
	floor_wall["a"] = members[0];
	floor_wall["b"] = members[1];
	floor_wall["kind"] = "max_gap";
	Dictionary wall_trim = floor_wall.duplicate(true);
	wall_trim["a"] = members[1];
	wall_trim["b"] = members[2];
	Array relations;
	relations.push_back(floor_wall);
	relations.push_back(wall_trim);
	CHECK(reflection_service.validate_structure_topology(members, relations).get("ok", false));

	Dictionary shared_datum;
	shared_datum["a"] = members[1];
	shared_datum["b"] = members[2];
	shared_datum["kind"] = "align";
	shared_datum["axis"] = "y";
	shared_datum["a_anchor"] = "max";
	shared_datum["b_anchor"] = "max";
	Dictionary opening_clearance;
	opening_clearance["a"] = members[0];
	opening_clearance["b"] = members[2];
	opening_clearance["kind"] = "no_overlap";
	Array clearance_axes;
	clearance_axes.push_back("x");
	clearance_axes.push_back("z");
	opening_clearance["axes"] = clearance_axes;
	relations.push_back(shared_datum);
	relations.push_back(opening_clearance);
	CHECK(reflection_service.validate_structure_topology(members, relations).get("ok", false));
	Dictionary invalid_datum = shared_datum.duplicate(true);
	invalid_datum.erase("axis");
	relations[2] = invalid_datum;
	Dictionary invalid_datum_result = reflection_service.validate_structure_topology(members, relations);
	CHECK_FALSE((bool)invalid_datum_result.get("ok", true));
	CHECK(Dictionary(invalid_datum_result.get("error", Dictionary())).get("code", String()) == "INVALID_STRUCTURE_CONTRACT");
	relations.resize(2);

	Dictionary partial_axis = wall_trim.duplicate(true);
	Array axes;
	axes.push_back("y");
	partial_axis["axes"] = axes;
	relations[1] = partial_axis;
	Dictionary rejected = reflection_service.validate_structure_topology(members, relations);
	CHECK_FALSE((bool)rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "INVALID_STRUCTURE_CONTRACT");

	Dictionary contains = wall_trim.duplicate(true);
	contains["kind"] = "contains";
	relations[1] = contains;
	rejected = reflection_service.validate_structure_topology(members, relations);
	CHECK_FALSE((bool)rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "DISCONNECTED_STRUCTURE_CONTRACT");
}

TEST_CASE("[SolersReflectionService] exact structural contacts expose any positive gap") {
	const AABB wall(Vector3(), Vector3(1, 1, 1));
	const AABB touching(Vector3(1, 0, 0), Vector3(1, 1, 1));
	const AABB one_millimeter_gap(Vector3(1.001, 0, 0), Vector3(1, 1, 1));
	const AABB three_centimeter_gap(Vector3(1.03, 0, 0), Vector3(1, 1, 1));
	CHECK(Math::is_zero_approx(SolersReflectionService::get_aabb_max_gap(wall, touching)));
	CHECK(SolersReflectionService::get_aabb_max_gap(wall, one_millimeter_gap) > 0.0);
	CHECK(SolersReflectionService::get_aabb_max_gap(wall, three_centimeter_gap) > 0.0);
}

TEST_CASE("[SceneTree][SolersGeometryFacts] mesh facts expose bounds and topology") {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(2, 4, 6));

	const Dictionary facts = solers_describe_mesh(box);
	CHECK((int)facts.get("mesh_count", 0) == 1);
	CHECK((int)facts.get("triangle_count", 0) == 12);
	const Array size = Dictionary(facts.get("aabb", Dictionary())).get("size", Array());
	REQUIRE(size.size() == 3);
	CHECK(Math::is_equal_approx((double)size[0], 2.0));
	CHECK(Math::is_equal_approx((double)size[1], 4.0));
	CHECK(Math::is_equal_approx((double)size[2], 6.0));
}

TEST_CASE("[SolersReflectionService] placement topology requires one support for every logical member") {
	SolersReflectionService reflection_service;
	Array members;
	members.push_back("Props/Table");
	members.push_back("Props/Lamp");

	Dictionary table_support;
	table_support["member"] = members[0];
	table_support["supported_by"] = "Structure/Floor";
	Dictionary lamp_support;
	lamp_support["member"] = members[1];
	lamp_support["supported_by"] = members[0];
	lamp_support["support_member"] = members[0];
	Array placements;
	placements.push_back(table_support);
	placements.push_back(lamp_support);
	CHECK(reflection_service.validate_placement_topology(members, placements).get("ok", false));

	placements.resize(1);
	Dictionary rejected = reflection_service.validate_placement_topology(members, placements);
	CHECK_FALSE((bool)rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "INCOMPLETE_PLACEMENT_CONTRACT");

	placements.push_back(lamp_support);
	Dictionary self_support = lamp_support.duplicate(true);
	self_support["supported_by"] = members[1];
	self_support["support_member"] = members[1];
	placements[1] = self_support;
	rejected = reflection_service.validate_placement_topology(members, placements);
	CHECK_FALSE((bool)rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "INVALID_PLACEMENT_CONTRACT");

	Dictionary table_cycle = table_support.duplicate(true);
	table_cycle["supported_by"] = members[1];
	table_cycle["support_member"] = members[1];
	placements[0] = table_cycle;
	placements[1] = lamp_support;
	rejected = reflection_service.validate_placement_topology(members, placements);
	CHECK_FALSE((bool)rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "CYCLIC_PLACEMENT_SUPPORT");
}

TEST_CASE("[SolersToolRegistry] objects.batch access follows its operation contract") {
	SolersReflectionService reflection_service;
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection_service);
	registry.register_default_tools();

	Dictionary read_op;
	read_op["op"] = "list_properties";
	read_op["node_path"] = "ReferenceCamera";
	Array read_operations;
	read_operations.push_back(read_op);
	Dictionary read_args;
	read_args["operations"] = read_operations;
	CHECK(registry.is_read_only("objects.batch", read_args));
	const Array read_access = registry.resolve_resource_access("objects.batch", read_args);
	REQUIRE(read_access.size() == 1);
	CHECK(Dictionary(read_access[0]).get("mode", String()) == "read");
	CHECK(Dictionary(read_access[0]).get("key", String()) == "scene:ReferenceCamera");

	Dictionary alias_read_op = read_op.duplicate(true);
	alias_read_op.erase("node_path");
	alias_read_op["path"] = "ReferenceCamera";
	Array alias_operations;
	alias_operations.push_back(alias_read_op);
	Dictionary alias_args;
	alias_args["operations"] = alias_operations;
	const Array alias_access = registry.resolve_resource_access("objects.batch", alias_args);
	REQUIRE(alias_access.size() == 1);
	CHECK(Dictionary(alias_access[0]).get("key", String()) == "scene:ReferenceCamera");

	Dictionary write_op;
	write_op["op"] = "set_property";
	write_op["node_path"] = "ReferenceCamera";
	write_op["property"] = "fov";
	write_op["value"] = 55.0;
	Array write_operations;
	write_operations.push_back(write_op);
	Dictionary write_args;
	write_args["operations"] = write_operations;
	CHECK_FALSE(registry.is_read_only("objects.batch", write_args));

	Array mixed_operations = read_operations.duplicate(true);
	mixed_operations.push_back(write_op);
	Dictionary mixed_args;
	mixed_args["operations"] = mixed_operations;
	CHECK_FALSE(registry.is_read_only("objects.batch", mixed_args));
}

TEST_CASE("[SolersObservationService] empty file search lists bounded project files") {
	SolersObservationService observation_service;
	Dictionary result = observation_service.search_project_files("  ", 4);
	CHECK(result.get("ok", false));
	CHECK(result.get("mode", String()) == "list_all");
	CHECK(result.has("files"));
	CHECK((int)result.get("count", -1) >= 0);
}

TEST_CASE("[SolersObservationService] visual statistics expose color and regional evidence") {
	Ref<Image> reference = Image::create_empty(12, 12, false, Image::FORMAT_RGBA8);
	reference->fill(Color(0.24, 0.24, 0.24));
	const Dictionary reference_stats = SolersObservationService::image_statistics(reference);
	CHECK(Array(reference_stats.get("mean_rgb_chromaticity", Array())).size() == 3);
	CHECK(Array(reference_stats.get("region_luminance_3x3", Array())).size() == 9);
}

TEST_CASE("[SolersObservationService] capture settling follows the SDFGI convergence setting") {
	CHECK(SolersObservationService::get_capture_settle_frame_count(false, 4) == 1);
	CHECK(SolersObservationService::get_capture_settle_frame_count(true, 0) == 5);
	CHECK(SolersObservationService::get_capture_settle_frame_count(true, 4) == 25);
	CHECK(SolersObservationService::get_capture_settle_frame_count(true, 99) == 30);
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

	Dictionary native_load_args;
	native_load_args["path"] = path;
	Dictionary native_loaded = resource_service.native_load(native_load_args);
	REQUIRE(native_loaded.get("ok", false));
	Dictionary call_args;
	call_args["object_id"] = native_loaded.get("data", Dictionary());
	call_args["method"] = "get_path";
	Dictionary call = resource_service.native_call(call_args);
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

TEST_CASE("[SolersResourceService] create initializes properties and accepts listed Resource types") {
	const String texture_path = "res://.solers_texture_array_contract.tres";
	const String node_path = "res://.solers_visual_shader_node_contract.tres";
	const String invalid_path = "res://.solers_invalid_object_contract.tres";
	SolersResourceService resource_service;

	Dictionary create_texture;
	create_texture["class_name"] = "CompressedTexture2DArray";
	create_texture["path"] = texture_path;
	REQUIRE(resource_service.create_resource(create_texture).get("ok", false));

	Dictionary node_properties;
	node_properties["texture_array"] = texture_path;
	Dictionary create_node;
	create_node["class_name"] = "VisualShaderNodeTexture2DArray";
	create_node["path"] = node_path;
	create_node["properties"] = node_properties;
	const Dictionary created_node = resource_service.create_resource(create_node);
	REQUIRE(created_node.get("ok", false));
	CHECK((int)Dictionary(created_node.get("data", Dictionary())).get("initialized_property_count", 0) == 1);

	Ref<Resource> node = ResourceLoader::load(node_path);
	REQUIRE(node.is_valid());
	Ref<Resource> texture = node->get("texture_array");
	REQUIRE(texture.is_valid());
	CHECK(texture->is_class("CompressedTexture2DArray"));

	Dictionary invalid_properties;
	invalid_properties["texture_array"] = Dictionary();
	create_node["path"] = invalid_path;
	create_node["properties"] = invalid_properties;
	const Dictionary invalid = resource_service.create_resource(create_node);
	CHECK_FALSE((bool)invalid.get("ok", true));
	CHECK(Dictionary(invalid.get("error", Dictionary())).get("code", String()) == "INVALID_PROPERTY_VALUE");
	CHECK_FALSE(FileAccess::exists(invalid_path));

	DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(node_path));
	DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(texture_path));
}

TEST_CASE("[SolersAssetService][SceneTree] texture-set import follows requested manifest roles") {
	const String asset_id = ".solers_map_role_import_contract";
	const String asset_dir = "user://solers_library/assets/" + asset_id;
	const String source_dir = asset_dir.path_join("source");
	const String target_dir = "res://.solers_map_role_import_contract";
	const String color_path = source_dir.path_join("color.png");
	const String normal_path = source_dir.path_join("normal.png");
	const String detail_path = source_dir.path_join("detail.png");
	auto remove_if_exists = [](const String &p_path) {
		if (FileAccess::exists(p_path)) {
			DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(p_path));
		}
	};
	remove_if_exists(target_dir.path_join("color.png"));
	remove_if_exists(target_dir.path_join("normal.png"));
	remove_if_exists(target_dir.path_join("detail.png"));
	REQUIRE(DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(source_dir)) == OK);
	Ref<Image> image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
	image->fill(Color(0.25, 0.5, 0.75, 1.0));
	REQUIRE(image->save_png(ProjectSettings::get_singleton()->globalize_path(color_path)) == OK);
	REQUIRE(image->save_png(ProjectSettings::get_singleton()->globalize_path(normal_path)) == OK);
	REQUIRE(image->save_png(ProjectSettings::get_singleton()->globalize_path(detail_path)) == OK);

	Array files;
	files.push_back(color_path);
	files.push_back(normal_path);
	files.push_back(detail_path);
	Dictionary map_files;
	map_files["surface_color"] = color_path;
	map_files["surface_normal"] = normal_path;
	map_files["optional_detail"] = detail_path;
	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = "material";
	manifest["name"] = "Map Role Import Contract";
	manifest["status"] = "ready";
	manifest["files"] = files;
	manifest["import_files"] = files;
	manifest["entrypoints"] = files;
	manifest["map_files"] = map_files;
	Ref<FileAccess> manifest_file = FileAccess::open(ProjectSettings::get_singleton()->globalize_path(asset_dir.path_join("manifest.json")), FileAccess::WRITE);
	REQUIRE(manifest_file.is_valid());
	manifest_file->store_string(JSON::stringify(manifest));
	manifest_file.unref();

	SolersAssetService asset_service;
	Dictionary args;
	args["asset_id"] = asset_id;
	args["target_dir"] = target_dir;
	Dictionary result = asset_service.import_to_project(args);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "MAP_SELECTION_REQUIRED");

	Array selected_roles;
	selected_roles.push_back("surface_color");
	selected_roles.push_back("surface_normal");
	args["map_types"] = selected_roles;
	result = asset_service.import_to_project(args);
	REQUIRE(result.get("ok", false));
	CHECK(FileAccess::exists(target_dir.path_join("color.png")));
	CHECK(FileAccess::exists(target_dir.path_join("normal.png")));
	CHECK_FALSE(FileAccess::exists(target_dir.path_join("detail.png")));

	remove_if_exists(target_dir.path_join("color.png"));
	remove_if_exists(target_dir.path_join("normal.png"));
	remove_if_exists(color_path);
	remove_if_exists(normal_path);
	remove_if_exists(detail_path);
	remove_if_exists(asset_dir.path_join("manifest.json"));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(source_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(asset_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(target_dir));
}

TEST_CASE("[SolersAssetService] Meshy topology target is an integer contract for Text-to-3D") {
	SolersAssetService asset_service;
	Dictionary provider_options;
	provider_options["target_polycount"] = 80000.5;
	Dictionary args;
	args["kind"] = "3d";
	args["provider"] = "meshy";
	args["prompt"] = "one isolated wooden bookshelf";
	args["provider_options"] = provider_options;
	const Dictionary result = asset_service.generate(args);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "INVALID_ARGUMENT");
	CHECK(String(Dictionary(result.get("error", Dictionary())).get("message", String())).contains("integer"));
	provider_options["target_polycount"] = 80000.0;
	provider_options["should_remesh"] = false;
	args["provider_options"] = provider_options;
	const Dictionary remesh_contract = asset_service.generate(args);
	CHECK_FALSE((bool)remesh_contract.get("ok", true));
	CHECK(String(Dictionary(remesh_contract.get("error", Dictionary())).get("message", String())).contains("should_remesh=true"));
}

TEST_CASE("[SolersAssetService][SceneTree] static models use Godot native lightmap import UV2") {
	EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
	if (!filesystem) {
		WARN("EditorFileSystem is initialized after the command-line unit test runner; run this contract in an editor integration test.");
		return;
	}
	const String asset_id = ".solers_static_lightmap_import_contract";
	const String asset_dir = "user://solers_library/assets/" + asset_id;
	const String source_dir = asset_dir.path_join("source");
	const String source_path = source_dir.path_join("quad.obj");
	const String target_dir = "res://.solers_static_lightmap_import_contract";
	const String target_path = target_dir.path_join("quad.obj");
	auto remove_if_exists = [](const String &p_path) {
		if (FileAccess::exists(p_path)) {
			DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(p_path));
		}
	};
	remove_if_exists(target_path);
	remove_if_exists(target_path + ".import");
	remove_if_exists(target_path + ".unwrap_cache");
	REQUIRE(DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(source_dir)) == OK);
	Ref<FileAccess> obj = FileAccess::open(ProjectSettings::get_singleton()->globalize_path(source_path), FileAccess::WRITE);
	REQUIRE(obj.is_valid());
	obj->store_string(
			"o Quad\n"
			"v -1 0 -1\n"
			"v 1 0 -1\n"
			"v 1 0 1\n"
			"v -1 0 1\n"
			"vn 0 1 0\n"
			"f 1//1 3//1 2//1\n"
			"f 1//1 4//1 3//1\n");
	obj.unref();
	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = "3d";
	manifest["name"] = "Static Lightmap Import Contract";
	manifest["status"] = "ready";
	Array files;
	files.push_back(source_path);
	manifest["files"] = files;
	manifest["import_files"] = files;
	manifest["entrypoints"] = files;
	Dictionary traits;
	traits["model_state"] = "static_model";
	manifest["traits"] = traits;
	Ref<FileAccess> manifest_file = FileAccess::open(ProjectSettings::get_singleton()->globalize_path(asset_dir.path_join("manifest.json")), FileAccess::WRITE);
	REQUIRE(manifest_file.is_valid());
	manifest_file->store_string(JSON::stringify(manifest));
	manifest_file.unref();

	SolersAssetService asset_service;
	Dictionary args;
	args["asset_id"] = asset_id;
	args["target_dir"] = target_dir;
	Dictionary result = asset_service.import_to_project(args);
	REQUIRE(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	for (int i = 0; i < 1000 && String(data.get("status", String())) == "pending"; i++) {
		filesystem->notification(Node::NOTIFICATION_PROCESS);
		MessageQueue::get_singleton()->flush();
		asset_service.poll();
		result = asset_service.poll_project_import(data.get("poll_args", Dictionary()));
		REQUIRE(result.get("ok", false));
		data = result.get("data", Dictionary());
		if (String(data.get("status", String())) == "pending") {
			OS::get_singleton()->delay_usec(10000);
		}
	}
	CHECK(String(data.get("status", String())) != "pending");
	const Dictionary inspection = data.get("import", Dictionary());
	CHECK(inspection.get("uv2_complete", false));
	CHECK((int64_t)inspection.get("triangle_count", 0) == 2);
	Ref<ConfigFile> import_config;
	import_config.instantiate();
	REQUIRE(import_config->load(target_path + ".import") == OK);
	CHECK((bool)import_config->get_value("params", "generate_lightmap_uv2", false));
	CHECK(FileAccess::exists(target_path + ".unwrap_cache"));

	remove_if_exists(target_path);
	remove_if_exists(target_path + ".import");
	remove_if_exists(target_path + ".unwrap_cache");
	remove_if_exists(source_path);
	remove_if_exists(asset_dir.path_join("manifest.json"));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(source_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(asset_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(target_dir));
}

TEST_CASE("[SolersAssetService][SceneTree] project import follows native Material dependencies") {
	EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
	if (!filesystem) {
		WARN("EditorFileSystem is initialized after the command-line unit test runner; run this contract in an editor integration test.");
		return;
	}
	SolersTestImportLifecycleObserver lifecycle_observer;
	lifecycle_observer.filesystem = filesystem;
	const Callable lifecycle_callback = callable_mp(&lifecycle_observer, &SolersTestImportLifecycleObserver::on_resources_reimported);
	filesystem->connect(SNAME("resources_reimported"), lifecycle_callback);
	const String asset_id = ".solers_material_import_contract";
	const String second_asset_id = ".solers_material_import_contract_b";
	const String asset_dir = "user://solers_library/assets/" + asset_id;
	const String second_asset_dir = "user://solers_library/assets/" + second_asset_id;
	const String source_dir = asset_dir.path_join("source");
	const String second_source_dir = second_asset_dir.path_join("source");
	const String target_dir = "res://.solers_material_import_contract";
	const String second_target_dir = "res://.solers_material_import_contract_b";
	auto remove_if_exists = [](const String &p_path) {
		if (FileAccess::exists(p_path)) {
			DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(p_path));
		}
	};
	remove_if_exists(target_dir.path_join("material.tres"));
	remove_if_exists(target_dir.path_join("surface.png"));
	remove_if_exists(target_dir.path_join("surface.png.import"));
	remove_if_exists(target_dir.path_join("unused.tres"));
	remove_if_exists(second_target_dir.path_join("material.tres"));
	remove_if_exists(second_target_dir.path_join("textures/surface.png"));
	remove_if_exists(second_target_dir.path_join("textures/surface.png.import"));
	remove_if_exists(second_target_dir.path_join("buffers/model.bin"));
	remove_if_exists(second_source_dir.path_join("surface.png"));
	REQUIRE(DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(source_dir)) == OK);
	REQUIRE(DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(second_source_dir)) == OK);

	auto write_text = [](const String &p_path, const String &p_content) {
		const String absolute = ProjectSettings::get_singleton()->globalize_path(p_path);
		REQUIRE(DirAccess::make_dir_recursive_absolute(absolute.get_base_dir()) == OK);
		Ref<FileAccess> file = FileAccess::open(absolute, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string(p_content);
	};
	const String texture_path = source_dir.path_join("surface.png");
	const String material_path = source_dir.path_join("material.tres");
	const String unused_path = source_dir.path_join("unused.tres");
	Ref<Image> image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
	image->fill(Color(0.25, 0.5, 0.75, 1.0));
	REQUIRE(image->save_png(ProjectSettings::get_singleton()->globalize_path(texture_path)) == OK);
	write_text(material_path,
			"[gd_resource type=\"StandardMaterial3D\" load_steps=2 format=3]\n"
			"[ext_resource type=\"Texture2D\" path=\"./surface.png\" id=\"1\"]\n"
			"[resource]\n"
			"albedo_texture = ExtResource(\"1\")\n");
	write_text(unused_path, "[gd_resource type=\"Resource\" format=3]\n[resource]\n");

	Array files;
	files.push_back(material_path);
	files.push_back(texture_path);
	files.push_back(unused_path);
	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = "material";
	manifest["name"] = "Material Import Contract";
	manifest["status"] = "ready";
	manifest["files"] = files;
	Array import_files;
	import_files.push_back(material_path);
	import_files.push_back(texture_path);
	manifest["import_files"] = import_files;
	Array entrypoints;
	entrypoints.push_back(material_path);
	manifest["entrypoints"] = entrypoints;
	write_text(asset_dir.path_join("manifest.json"), JSON::stringify(manifest));
	const String second_texture_path = second_source_dir.path_join("textures/surface.png");
	const String second_material_path = second_source_dir.path_join("material.tres");
	const String second_binary_path = second_source_dir.path_join("buffers/model.bin");
	Ref<Image> second_image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
	second_image->fill(Color(0.75, 0.5, 0.25, 1.0));
	REQUIRE(DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(second_texture_path).get_base_dir()) == OK);
	REQUIRE(second_image->save_png(ProjectSettings::get_singleton()->globalize_path(second_texture_path)) == OK);
	write_text(second_material_path,
			"[gd_resource type=\"StandardMaterial3D\" load_steps=2 format=3]\n"
			"[ext_resource type=\"Texture2D\" path=\"./textures/surface.png\" id=\"1\"]\n"
			"[resource]\n"
			"albedo_texture = ExtResource(\"1\")\n");
	write_text(second_binary_path, "provider-declared package dependency");
	Array second_files;
	second_files.push_back(second_material_path);
	second_files.push_back(second_texture_path);
	second_files.push_back(second_binary_path);
	Dictionary second_manifest;
	second_manifest["id"] = second_asset_id;
	second_manifest["kind"] = "material";
	second_manifest["name"] = "Material Import Contract B";
	second_manifest["status"] = "ready";
	second_manifest["files"] = second_files;
	Array no_entrypoints;
	second_manifest["entrypoints"] = no_entrypoints;
	write_text(second_asset_dir.path_join("manifest.json"), JSON::stringify(second_manifest));

	SolersAssetService asset_service;
	Dictionary args;
	args["asset_id"] = asset_id;
	args["target_dir"] = target_dir;
	Dictionary result = asset_service.import_to_project(args);
	REQUIRE(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	REQUIRE(data.get("status", String()) == "pending");
	Dictionary second_args;
	second_args["asset_id"] = second_asset_id;
	second_args["target_dir"] = second_target_dir;
	Dictionary second_result = asset_service.import_to_project(second_args);
	REQUIRE(second_result.get("ok", false));
	Dictionary second_data = second_result.get("data", Dictionary());
	REQUIRE(second_data.get("status", String()) == "pending");
	const Dictionary staged_imports = asset_service.get_project_import_coordinator_state();
	CHECK_FALSE(staged_imports.get("wave_active", true));
	CHECK((int)staged_imports.get("queued_count", 0) == 2);
	const Dictionary first_poll_args = data.get("poll_args", Dictionary());
	Dictionary duplicate_pending = asset_service.import_to_project(args);
	REQUIRE(duplicate_pending.get("ok", false));
	const Dictionary duplicate_data = duplicate_pending.get("data", Dictionary());
	CHECK(duplicate_data.get("reused", false));
	CHECK(Dictionary(duplicate_data.get("poll_args", Dictionary())).get("_import_id", String()) == first_poll_args.get("_import_id", String()));
	for (int i = 0; i < 1000 && (String(data.get("status", String())) == "pending" || String(second_data.get("status", String())) == "pending"); i++) {
		filesystem->notification(Node::NOTIFICATION_PROCESS);
		MessageQueue::get_singleton()->flush();
		asset_service.poll();
		if (String(data.get("status", String())) == "pending") {
			result = asset_service.poll_project_import(data.get("poll_args", Dictionary()));
			REQUIRE(result.get("ok", false));
			data = result.get("data", Dictionary());
		}
		if (String(second_data.get("status", String())) == "pending") {
			second_result = asset_service.poll_project_import(second_data.get("poll_args", Dictionary()));
			REQUIRE(second_result.get("ok", false));
			second_data = second_result.get("data", Dictionary());
		}
		if (String(data.get("status", String())) == "pending" || String(second_data.get("status", String())) == "pending") {
			OS::get_singleton()->delay_usec(10000);
		}
	}
	CHECK(data.get("status", String()) != "pending");
	CHECK(second_data.get("status", String()) != "pending");
	CHECK(lifecycle_observer.observed);
	CHECK(lifecycle_observer.all_callbacks_inside_import);
	filesystem->disconnect(SNAME("resources_reimported"), lifecycle_callback);
	CHECK((int)data.get("source_file_count", 0) == 3);
	CHECK((int)data.get("skipped_source_file_count", 0) == 1);
	const Dictionary import_inspection = data.get("import", Dictionary());
	CHECK((int)import_inspection.get("file_count", 0) == 2);
	CHECK((int)import_inspection.get("entrypoint_count", 0) == 1);
	CHECK((int)import_inspection.get("loadable_count", 0) == 1);
	const Dictionary second_import_inspection = second_data.get("import", Dictionary());
	CHECK((int)second_data.get("source_file_count", 0) == 3);
	CHECK((int)second_data.get("skipped_source_file_count", 0) == 1);
	CHECK((int)second_import_inspection.get("file_count", 0) == 2);
	CHECK((int)second_import_inspection.get("import_valid_count", 0) == 2);
	CHECK((int)second_import_inspection.get("entrypoint_count", -1) == 1);
	CHECK((int)second_import_inspection.get("loadable_count", -1) == 1);
	const Array imported_resources = data.get("resources", Array());
	REQUIRE(imported_resources.size() == 1);
	CHECK(Dictionary(imported_resources[0]).get("path", String()) == target_dir.path_join("material.tres"));
	CHECK(FileAccess::exists(target_dir.path_join("material.tres")));
	CHECK(FileAccess::exists(target_dir.path_join("surface.png.import")));
	CHECK_FALSE(FileAccess::exists(target_dir.path_join("unused.tres")));
	CHECK(ResourceLoader::load(target_dir.path_join("material.tres"), String(), ResourceFormatLoader::CACHE_MODE_REUSE).is_valid());
	CHECK(ResourceLoader::load(second_target_dir.path_join("material.tres"), String(), ResourceFormatLoader::CACHE_MODE_REUSE).is_valid());
	CHECK(FileAccess::exists(second_target_dir.path_join("textures/surface.png")));
	CHECK_FALSE(FileAccess::exists(second_target_dir.path_join("buffers/model.bin")));

	const uint64_t imported_mtime = FileAccess::get_modified_time(target_dir.path_join("surface.png"));
	SolersAssetService restarted_service;
	Dictionary repeated = restarted_service.import_to_project(args);
	REQUIRE(repeated.get("ok", false));
	CHECK(String(Dictionary(repeated.get("data", Dictionary())).get("status", String())) != "pending");
	CHECK(FileAccess::get_modified_time(target_dir.path_join("surface.png")) == imported_mtime);

	Array cleanup_files;
	cleanup_files.push_back(target_dir.path_join("material.tres"));
	cleanup_files.push_back(target_dir.path_join("surface.png"));
	cleanup_files.push_back(target_dir.path_join("surface.png.import"));
	cleanup_files.push_back(target_dir.path_join("unused.tres"));
	cleanup_files.push_back(material_path);
	cleanup_files.push_back(texture_path);
	cleanup_files.push_back(unused_path);
	cleanup_files.push_back(asset_dir.path_join("manifest.json"));
	cleanup_files.push_back(second_target_dir.path_join("material.tres"));
	cleanup_files.push_back(second_target_dir.path_join("textures/surface.png"));
	cleanup_files.push_back(second_target_dir.path_join("textures/surface.png.import"));
	cleanup_files.push_back(second_target_dir.path_join("buffers/model.bin"));
	cleanup_files.push_back(second_material_path);
	cleanup_files.push_back(second_texture_path);
	cleanup_files.push_back(second_binary_path);
	cleanup_files.push_back(second_source_dir.path_join("surface.png"));
	cleanup_files.push_back(second_asset_dir.path_join("manifest.json"));
	for (int i = 0; i < cleanup_files.size(); i++) {
		remove_if_exists(cleanup_files[i]);
	}
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(target_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(second_target_dir.path_join("textures")));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(second_target_dir.path_join("buffers")));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(source_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(second_source_dir.path_join("textures")));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(second_source_dir.path_join("buffers")));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(asset_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(second_target_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(second_source_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(second_asset_dir));
}

TEST_CASE("[SolersAssetService][SceneTree] project import never opens interactive format configuration") {
	EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
	if (!filesystem) {
		WARN("EditorFileSystem is initialized after the command-line unit test runner; run this contract in an editor integration test.");
		return;
	}
	const String asset_id = ".solers_noninteractive_import_contract";
	const String asset_dir = "user://solers_library/assets/" + asset_id;
	const String source_path = asset_dir.path_join("source/payload.futureformat");
	const String target_dir = "res://.solers_noninteractive_import_contract";
	const String target_path = target_dir.path_join("payload.futureformat");
	REQUIRE(DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(source_path).get_base_dir()) == OK);
	Ref<FileAccess> source = FileAccess::open(ProjectSettings::get_singleton()->globalize_path(source_path), FileAccess::WRITE);
	REQUIRE(source.is_valid());
	source->store_string("synthetic import format payload");
	source.unref();

	Array files;
	files.push_back(source_path);
	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = "3d";
	manifest["name"] = "Noninteractive Import Contract";
	manifest["status"] = "ready";
	manifest["files"] = files;
	manifest["import_files"] = files;
	manifest["entrypoints"] = files;
	Ref<FileAccess> manifest_file = FileAccess::open(ProjectSettings::get_singleton()->globalize_path(asset_dir.path_join("manifest.json")), FileAccess::WRITE);
	REQUIRE(manifest_file.is_valid());
	manifest_file->store_string(JSON::stringify(manifest));
	manifest_file.unref();

	Ref<SolersTestImportFormatSupportQuery> query;
	query.instantiate();
	query->extensions.push_back("futureformat");
	filesystem->add_import_format_support_query(query);
	SolersAssetService asset_service;
	Dictionary args;
	args["asset_id"] = asset_id;
	args["target_dir"] = target_dir;
	const Dictionary result = asset_service.import_to_project(args);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "IMPORT_FORMAT_CONFIGURATION_REQUIRED");
	CHECK_FALSE(query->queried);
	CHECK_FALSE(FileAccess::exists(target_path));
	filesystem->remove_import_format_support_query(query);

	DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(target_path));
	DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(source_path));
	DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(asset_dir.path_join("manifest.json")));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(asset_dir.path_join("source")));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(asset_dir));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(target_dir));
}

TEST_CASE("[SolersResourceService] resource property updates are atomic and save once") {
	const String path = "res://.solers_resource_properties_contract.tres";
	SolersResourceService service;
	Dictionary create;
	create["class_name"] = "Curve3D";
	create["path"] = path;
	REQUIRE(service.create_resource(create).get("ok", false));

	Dictionary properties;
	properties["bake_interval"] = 0.37;
	properties["up_vector_enabled"] = false;
	Dictionary set;
	set["path"] = path;
	set["properties"] = properties;
	const Dictionary updated = service.set_resource_property(set);
	REQUIRE(updated.get("ok", false));
	CHECK((int)Dictionary(updated.get("data", Dictionary())).get("updated_property_count", 0) == 2);

	Dictionary invalid_properties = properties.duplicate();
	invalid_properties["not_a_property"] = 1;
	set["properties"] = invalid_properties;
	CHECK_FALSE((bool)service.set_resource_property(set).get("ok", true));

	Ref<Resource> material = ResourceLoader::load(path, String(), ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(material.is_valid());
	CHECK(Math::is_equal_approx((double)material->get("bake_interval"), 0.37));
	CHECK_FALSE((bool)material->get("up_vector_enabled"));
	DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(path));
}

TEST_CASE("[SolersReflectionService] introspection reports native Object argument classes") {
	SolersReflectionService reflection_service;
	Dictionary args;
	args["class_name"] = "Mesh";
	args["include_inherited"] = false;
	const Dictionary result = reflection_service.introspect_class(args);
	REQUIRE(result.get("ok", false));

	const Array methods = Dictionary(result.get("data", Dictionary())).get("methods", Array());
	bool found = false;
	for (int i = 0; i < methods.size(); i++) {
		const Dictionary method = methods[i];
		if (String(method.get("name", String())) != "surface_set_material") {
			continue;
		}
		const Array arguments = method.get("arguments", Array());
		REQUIRE(arguments.size() == 2);
		CHECK(Dictionary(arguments[1]).get("class_name", String()) == "Material");
		found = true;
		break;
	}
	CHECK(found);
}

TEST_CASE("[SolersReflectionService] Godot indexed paths reach nested resource properties") {
	Path3D node;
	Ref<Curve3D> curve;
	curve.instantiate();
	node.set_curve(curve);

	Vector<StringName> property_path;
	property_path.push_back(SNAME("curve"));
	property_path.push_back(SNAME("bake_interval"));
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

TEST_CASE("[SolersContextManager] estimates ASCII and non-ASCII text like Kimi Code") {
	CHECK(SolersContextManager::estimate_tokens("abcd") == 1);
	CHECK(SolersContextManager::estimate_tokens("abcde") == 2);
	CHECK(SolersContextManager::estimate_tokens(String::utf8("白盒")) == 2);
	CHECK(SolersContextManager::estimate_tokens(String::utf8("ab白")) == 2);
}

TEST_CASE("[SolersContextManager] compaction strategy honors ratio and reserved context") {
	SolersContextManager context;

	CHECK_FALSE(context.should_compact(149999, 200000));
	CHECK(context.should_compact(150000, 200000));
	CHECK(context.should_compact(170000, 200000));
	CHECK_FALSE(context.should_compact(42499, 50000));
	CHECK(context.should_compact(42500, 50000));
}

TEST_CASE("[SolersContextManager] compaction requires context growth after the last compaction") {
	SolersContextManager context;
	Array messages;
	messages.push_back(SolersLLMMessage::user("Keep the room proportions realistic."));
	const Dictionary result = context.apply_compaction(messages, "Continue from the verified room shell.", Dictionary());
	const int tokens_after = result.get("tokens_after", 0);
	REQUIRE(tokens_after > 0);
	CHECK_FALSE(context.should_compact(tokens_after, tokens_after));
	CHECK(context.should_compact(tokens_after + 1, tokens_after + 1));
}

TEST_CASE("[SolersContextManager] request projection clears consumed ephemeral tool results") {
	SolersContextManager context;
	Array messages;
	messages.push_back(SolersLLMMessage::user("Inspect the scene."));
	messages.push_back(SolersLLMMessage::assistant("Inspecting.", Array()));
	Dictionary old_ephemeral = SolersLLMMessage::tool_result("call_old", "native.get", "old observation");
	old_ephemeral["retention"] = "ephemeral";
	Dictionary preview;
	preview["source"] = "asset_catalog_preview";
	Array preview_attachments;
	preview_attachments.push_back(preview);
	old_ephemeral["attachments"] = preview_attachments;
	messages.push_back(old_ephemeral);
	messages.push_back(SolersLLMMessage::tool_result("call_skill", "skill.read", "durable instructions"));
	messages.push_back(SolersLLMMessage::assistant("Applying the observation.", Array()));
	Dictionary current_ephemeral = SolersLLMMessage::tool_result("call_current", "native.get", "current observation");
	current_ephemeral["retention"] = "ephemeral";
	messages.push_back(current_ephemeral);

	const Array projected = context.prepare_request(messages, String(), Array());

	const Dictionary old_tool = projected[2];
	const Dictionary durable_tool = projected[3];
	const Dictionary current_tool = projected[5];
	CHECK(old_tool.get("content", String()) == String(SolersContextManager::TOOL_RESULT_PLACEHOLDER));
	CHECK_FALSE(old_tool.has("attachments"));
	CHECK(durable_tool.get("content", String()) == "durable instructions");
	CHECK(current_tool.get("content", String()) == "current observation");
}

TEST_CASE("[SolersContextManager] request projection keeps latest asset catalog contact sheet") {
	SolersContextManager context;
	Dictionary old_preview;
	old_preview["id"] = "catalog_old";
	old_preview["source"] = "asset_catalog_preview";
	Array old_attachments;
	old_attachments.push_back(old_preview);
	Dictionary old_tool = SolersLLMMessage::tool_result("call_old", "asset.catalog.search", "{}");
	old_tool["attachments"] = old_attachments;

	Dictionary new_preview;
	new_preview["id"] = "catalog_new";
	new_preview["source"] = "asset_catalog_preview";
	Array new_attachments;
	new_attachments.push_back(new_preview);
	Dictionary new_tool = SolersLLMMessage::tool_result("call_new", "asset.catalog.search", "{}");
	new_tool["attachments"] = new_attachments;

	Array messages;
	messages.push_back(SolersLLMMessage::user("Find wallpaper."));
	messages.push_back(old_tool);
	messages.push_back(new_tool);
	const Array projected = context.prepare_request(messages, String(), Array());
	CHECK(Array(Dictionary(projected[1]).get("attachments", Array())).is_empty());
	REQUIRE(Array(Dictionary(projected[2]).get("attachments", Array())).size() == 1);
	CHECK(Dictionary(Array(Dictionary(projected[2]).get("attachments", Array()))[0]).get("id", String()) == "catalog_new");
}

TEST_CASE("[SolersContextManager] request projection preserves user references and unconsumed captures") {
	SolersContextManager context;
	Dictionary user_attachment;
	user_attachment["id"] = "reference";
	user_attachment["source"] = "user";
	Array user_attachments;
	user_attachments.push_back(user_attachment);
	Dictionary user = SolersLLMMessage::user("Match this reference.");
	user["attachments"] = user_attachments;

	Dictionary first_capture;
	first_capture["id"] = "capture_1";
	first_capture["source"] = "tool_capture";
	Array first_attachments;
	first_attachments.push_back(first_capture);
	Dictionary first_tool = SolersLLMMessage::tool_result("call_1", "viewport_capture", "{}");
	first_tool["attachments"] = first_attachments;

	Dictionary second_capture;
	second_capture["id"] = "capture_2";
	second_capture["source"] = "tool_capture";
	Array second_attachments;
	second_attachments.push_back(second_capture);
	Dictionary second_tool = SolersLLMMessage::tool_result("call_2", "viewport_capture", "{}");
	second_tool["attachments"] = second_attachments;

	Array messages;
	messages.push_back(user);
	messages.push_back(first_tool);
	messages.push_back(second_tool);
	Array projected = context.prepare_request(messages, String(), Array());

	CHECK(Array(Dictionary(projected[0]).get("attachments", Array())).size() == 1);
	CHECK(Array(Dictionary(projected[1]).get("attachments", Array())).size() == 1);
	REQUIRE(Array(Dictionary(projected[2]).get("attachments", Array())).size() == 1);
	CHECK(Dictionary(Array(Dictionary(projected[2]).get("attachments", Array()))[0]).get("id", String()) == "capture_2");
}

TEST_CASE("[SolersContextManager] request projection does not resend consumed state captures") {
	SolersContextManager context;
	Dictionary capture;
	capture["id"] = "state_capture";
	capture["source"] = "tool_capture";
	Array attachments;
	attachments.push_back(capture);
	Dictionary state = SolersLLMMessage::user("revision 2");
	state["origin"] = "solers_state";
	state["attachments"] = attachments;

	Array messages;
	messages.push_back(SolersLLMMessage::user("Build the room."));
	messages.push_back(state);
	messages.push_back(SolersLLMMessage::assistant("I inspected revision 2.", Array()));
	const Array projected = context.prepare_request(messages, String(), Array());
	REQUIRE(projected.size() == 3);
	CHECK(Array(Dictionary(projected[1]).get("attachments", Array())).is_empty());
}

TEST_CASE("[SolersContextManager] full compaction keeps real user prompts and one plan-aware summary") {
	SolersContextManager context;
	Array messages;
	messages.push_back(SolersLLMMessage::user("Build a room from the reference."));
	messages.push_back(SolersLLMMessage::assistant("I will inspect the scene.", Array()));
	messages.push_back(SolersLLMMessage::tool_result("call_1", "editor.get_snapshot", "{\"ok\":true}"));
	messages.push_back(SolersLLMMessage::user("Keep the proportions realistic."));

	Dictionary old_summary = SolersLLMMessage::user("obsolete summary");
	old_summary["origin"] = "compaction_summary";
	messages.push_back(old_summary);

	Array steps;
	Dictionary step;
	step["step"] = "Tune lighting";
	step["status"] = "in_progress";
	steps.push_back(step);
	Dictionary plan;
	plan["plan"] = steps;

	Dictionary result = context.apply_compaction(messages, "I finished the whitebox.", plan);
	Array compacted = result.get("messages", Array());
	REQUIRE(compacted.size() == 3);
	CHECK(Dictionary(compacted[0]).get("content", String()) == "Build a room from the reference.");
	CHECK(Dictionary(compacted[1]).get("content", String()) == "Keep the proportions realistic.");
	Dictionary summary = compacted[2];
	CHECK(summary.get("origin", String()) == "compaction_summary");
	CHECK(String(summary.get("content", String())).begins_with(String::utf8(SolersContextManager::COMPACTION_SUMMARY_PREFIX)));
	CHECK(String(summary.get("content", String())).contains("Tune lighting"));

	Dictionary repeated = context.apply_compaction(compacted, "Continue from the verified whitebox.", plan);
	Array repeated_messages = repeated.get("messages", Array());
	REQUIRE(repeated_messages.size() == 3);
	CHECK(Dictionary(repeated_messages[2]).get("origin", String()) == "compaction_summary");
}

TEST_CASE("[SolersContextManager] full compaction never discards user reference images") {
	SolersContextManager context;
	Dictionary reference;
	reference["id"] = "user_reference";
	reference["source"] = "user";
	reference["type"] = "image";
	Array attachments;
	attachments.push_back(reference);
	Dictionary user = SolersLLMMessage::user(String("x").repeat(100000));
	user["attachments"] = attachments;
	Array messages;
	messages.push_back(user);

	const Dictionary result = context.apply_compaction(messages, "Continue matching the reference.", Dictionary());
	const Array compacted = result.get("messages", Array());
	REQUIRE(compacted.size() == 2);
	const Array kept = Dictionary(compacted[0]).get("attachments", Array());
	REQUIRE(kept.size() == 1);
	CHECK(Dictionary(kept[0]).get("id", String()) == "user_reference");
}

TEST_CASE("[SolersContextManager] full compaction drops consumed tool captures") {
	SolersContextManager context;
	Dictionary first_capture;
	first_capture["id"] = "capture_old";
	first_capture["source"] = "tool_capture";
	Array first_attachments;
	first_attachments.push_back(first_capture);
	Dictionary first_tool = SolersLLMMessage::tool_result("call_old", "viewport_capture", "{}");
	first_tool["attachments"] = first_attachments;

	Dictionary latest_capture;
	latest_capture["id"] = "capture_latest";
	latest_capture["source"] = "tool_capture";
	Array latest_attachments;
	latest_attachments.push_back(latest_capture);
	Dictionary latest_tool = SolersLLMMessage::tool_result("call_latest", "viewport_capture", "{}");
	latest_tool["attachments"] = latest_attachments;

	Array messages;
	messages.push_back(SolersLLMMessage::user("Build and inspect the scene."));
	messages.push_back(first_tool);
	messages.push_back(latest_tool);
	const Array compacted = Dictionary(context.apply_compaction(messages, "Continue visual verification.", Dictionary())).get("messages", Array());
	REQUIRE(compacted.size() == 2);
	CHECK(Dictionary(compacted[1]).get("origin", String()) == "compaction_summary");
}

TEST_CASE("[SolersContextManager] request and full compaction keep only the latest harness state") {
	SolersContextManager context;
	Dictionary old_state = SolersLLMMessage::user("revision 1");
	old_state["origin"] = "solers_state";
	Dictionary new_state = SolersLLMMessage::user("revision 2");
	new_state["origin"] = "solers_state";
	Dictionary state_capture;
	state_capture["id"] = "state_capture";
	state_capture["source"] = "tool_capture";
	Array state_attachments;
	state_attachments.push_back(state_capture);
	new_state["attachments"] = state_attachments;
	Array messages;
	messages.push_back(SolersLLMMessage::user("Build the room."));
	messages.push_back(old_state);
	messages.push_back(new_state);

	const Array projected = context.prepare_request(messages, "system", Array());
	REQUIRE(projected.size() == 2);
	CHECK(Dictionary(projected[1]).get("content", String()) == "revision 2");

	const Array compacted = Dictionary(context.apply_compaction(messages, "Continue.", Dictionary())).get("messages", Array());
	REQUIRE(compacted.size() == 3);
	CHECK(Dictionary(compacted[1]).get("content", String()) == "revision 2");
	CHECK(Dictionary(compacted[1]).get("origin", String()) == "solers_state");
	CHECK(Array(Dictionary(compacted[1]).get("attachments", Array())).is_empty());
}

TEST_CASE("[SolersAgentSession] leaves model request limits to the caller") {
	SolersToolRegistry registry;
	SolersAgentSession session;
	session.set_tool_registry(&registry);

	const Array tools = registry.list_tools();
	CHECK_FALSE(find_tool_def(tools, "update_plan").is_empty());
	CHECK_FALSE(find_tool_def(tools, "done").is_empty());
	const Dictionary status = session.get_status();
	CHECK_FALSE(status.has("tool_iterations"));
	CHECK_FALSE(status.has("max_tool_iterations"));
	CHECK((int)status.get("model_request_budget", -1) == 0);
}

TEST_CASE("[SolersAgentSession] unknown model capacity remains unknown") {
	SolersAgentSession session;
	const Dictionary status = session.get_status();
	CHECK((int)status.get("context_window", -1) == 0);
}

TEST_CASE("[SolersAgentSession] validates the Codex update_plan contract") {
	Array valid_steps;
	Dictionary first;
	first["step"] = "Whitebox";
	first["status"] = "completed";
	valid_steps.push_back(first);
	Dictionary second;
	second["step"] = "Lighting";
	second["status"] = "in_progress";
	valid_steps.push_back(second);
	Dictionary args;
	args["plan"] = valid_steps;
	CHECK(SolersAgentSession::validate_plan(args).get("ok", false));

	Dictionary duplicate = second.duplicate();
	duplicate["step"] = "Materials";
	valid_steps.push_back(duplicate);
	args["plan"] = valid_steps;
	Dictionary invalid = SolersAgentSession::validate_plan(args);
	CHECK_FALSE(invalid.get("ok", true));
	CHECK(Dictionary(invalid.get("error", Dictionary())).get("code", String()) == "INVALID_PLAN");

	valid_steps.resize(1);
	first["status"] = "blocked";
	valid_steps[0] = first;
	args["plan"] = valid_steps;
	CHECK_FALSE(SolersAgentSession::validate_plan(args).get("ok", true));

	first["status"] = "completed";
	valid_steps[0] = first;
	args["plan"] = valid_steps;
	args["reference_attachment_id"] = "synthetic_reference";
	Dictionary missing_camera = SolersAgentSession::validate_plan(args);
	CHECK_FALSE((bool)missing_camera.get("ok", true));
	CHECK(Dictionary(missing_camera.get("error", Dictionary())).get("code", String()) == "INVALID_PLAN");
	args["reference_camera_path"] = "ReferenceCamera";
	CHECK(SolersAgentSession::validate_plan(args).get("ok", false));

	first["evidence_required"] = true;
	valid_steps[0] = first;
	args["plan"] = valid_steps;
	Dictionary missing_evidence = SolersAgentSession::validate_plan(args);
	CHECK_FALSE((bool)missing_evidence.get("ok", true));
	CHECK(Dictionary(missing_evidence.get("error", Dictionary())).get("code", String()) == "INVALID_PLAN");
	Dictionary proof;
	proof["call_id"] = "synthetic_call";
	proof["tool"] = "asset.catalog.search";
	Array evidence;
	evidence.push_back(proof);
	first["evidence"] = evidence;
	valid_steps[0] = first;
	args["plan"] = valid_steps;
	CHECK(SolersAgentSession::validate_plan(args).get("ok", false));
}

TEST_CASE("[SolersAgentSession] completion requires verification after Godot errors") {
	Dictionary args;
	args["message"] = "Finished.";
	Dictionary state;
	state["authored_revision"] = 2;
	state["runtime_epoch"] = 7;
	state["observed_revision"] = 2;
	state["runtime_capture_revision"] = 2;
	state["scene_validation_revision"] = 2;
	state["geometry_revision"] = 2;
	CHECK(SolersAgentSession::validate_done(args, state).get("ok", false));

	state["plan_incomplete"] = true;
	CHECK(SolersAgentSession::validate_done(args, state).get("ok", false));
	state["plan_incomplete"] = false;

	state["unresolved_errors"] = 1;
	Dictionary failure_error;
	failure_error["code"] = "SPATIAL_VALIDATION_FAILED";
	failure_error["message"] = "Synthetic failure";
	Dictionary failure;
	failure["failure_id"] = "synthetic_failure";
	failure["tool"] = "scene.validate_structure";
	failure["error"] = failure_error;
	Dictionary failures;
	failures["synthetic_failure"] = failure;
	state["unresolved_tool_errors"] = failures;
	Dictionary blocked = SolersAgentSession::validate_done(args, state);
	CHECK_FALSE((bool)blocked.get("ok", true));
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "UNRESOLVED_TOOL_ERRORS");
	CHECK(Array(Dictionary(blocked.get("error", Dictionary())).get("failures", Array())).size() == 1);

	state["unresolved_errors"] = 0;
	state["plan_evidence_invalid"] = true;
	state["plan_evidence_error"] = "Synthetic evidence failure";
	CHECK(SolersAgentSession::validate_done(args, state).get("ok", false));
	state["plan_evidence_invalid"] = false;
	state["observed_revision"] = 1;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "STALE_COMPLETION_EVIDENCE");

	state["observed_revision"] = 2;
	state["dirty"] = true;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "UNSAVED_SCENE");

	state["dirty"] = false;
	state["pending_jobs"] = true;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "PENDING_BACKGROUND_JOBS");

	state["pending_jobs"] = false;
	state["scene_validation_required"] = true;
	state["scene_validation_revision"] = 1;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "STALE_SCENE_VALIDATION");

	state["scene_validation_revision"] = 2;
	state["scene_revision"] = 3;
	state["geometry_revision"] = 2;
	CHECK(SolersAgentSession::validate_done(args, state).get("ok", false));
	state["geometry_revision"] = 3;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "STALE_SCENE_VALIDATION");
	state["scene_validation_revision"] = 3;
	state["render_pipeline_required"] = true;
	state["render_pipeline_valid"] = false;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "INVALID_RENDER_PIPELINE");
	state["render_pipeline_valid"] = true;
	state["editor_capture_required"] = true;
	state["editor_capture_revision"] = 1;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "STALE_EDITOR_EVIDENCE");

	state["editor_capture_revision"] = 2;
	state["runtime_required"] = true;
	state["runtime_capture_revision"] = 1;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "STALE_RUNTIME_EVIDENCE");

	state["runtime_capture_revision"] = 2;
	state["visual_reference_required"] = true;
	state["visual_reference_attachment_valid"] = true;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "INVALID_REFERENCE_LAYOUT_EVIDENCE");
	state["reference_layout_valid"] = true;
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "MISSING_VISUAL_EVIDENCE");
	state["visual_evidence_declared"] = true;
	state["visual_evidence_valid"] = false;
	state["visual_evidence_error"] = "Synthetic stale capture.";
	blocked = SolersAgentSession::validate_done(args, state);
	CHECK(Dictionary(blocked.get("error", Dictionary())).get("code", String()) == "INVALID_VISUAL_EVIDENCE");
	state["visual_evidence_valid"] = true;
	CHECK(SolersAgentSession::validate_done(args, state).get("ok", false));
}

TEST_CASE("[SolersToolRegistry] runtime lifecycle does not mutate authored project state") {
	SolersAssetService assets;
	SolersToolRegistry registry;
	registry.set_asset_service(&assets);
	registry.register_default_tools();

	CHECK_FALSE(registry.affects_authored_state(SNAME("runtime.control")));
	CHECK(registry.affects_authored_state(SNAME("asset.import_to_project")));

	Dictionary play;
	play["action"] = "play_current_scene";
	const Array runtime_accesses = registry.resolve_resource_access(SNAME("runtime.control"), play);
	REQUIRE(runtime_accesses.size() == 1);
	CHECK(Dictionary(runtime_accesses[0]).get("key", String()) == "runtime:");
}

TEST_CASE("[SolersToolRegistry] asset reads do not conflict with project imports") {
	SolersAssetService assets;
	SolersToolRegistry registry;
	registry.set_asset_service(&assets);
	registry.register_default_tools();

	const Array list_accesses = registry.resolve_resource_access(SNAME("asset.list_local"), Dictionary());
	REQUIRE(list_accesses.size() == 1);
	CHECK(Dictionary(list_accesses[0]).get("mode", String()) == "read");
	CHECK(Dictionary(list_accesses[0]).get("key", String()) == "asset-library:");

	Dictionary import_args;
	import_args["asset_id"] = "synthetic_asset";
	const Array import_accesses = registry.resolve_resource_access(SNAME("asset.import_to_project"), import_args);
	REQUIRE(import_accesses.size() == 2);
	CHECK(Dictionary(import_accesses[0]).get("mode", String()) == "read");
	CHECK(Dictionary(import_accesses[0]).get("key", String()) == "asset:synthetic_asset");
	CHECK(Dictionary(import_accesses[1]).get("mode", String()) == "write");
	const String project_key = Dictionary(import_accesses[1]).get("key", String());
	CHECK(project_key.begins_with("project:res:"));
	CHECK(project_key.ends_with("/solers_assets"));

	Dictionary first_search;
	first_search["provider"] = "polyhaven";
	first_search["kind"] = "3d";
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
	other_kind_search["kind"] = "material";
	CHECK_FALSE(SolersToolRegistry::has_write_conflict(catalog_accesses, registry.resolve_resource_access(SNAME("asset.catalog.search"), other_kind_search)));
	Dictionary catalog_args;
	catalog_args["provider"] = "polyhaven";
	catalog_args["asset_id"] = "aerial_asphalt_01";
	catalog_args["variant"] = "2k-jpg";
	const Array catalog_acquire_accesses = registry.resolve_resource_access(SNAME("asset.catalog.acquire"), catalog_args);
	REQUIRE(catalog_acquire_accesses.size() == 1);
	CHECK(Dictionary(catalog_acquire_accesses[0]).get("mode", String()) == "write");
	CHECK(String(Dictionary(catalog_acquire_accesses[0]).get("key", String())).contains("aerial_asphalt_01"));
}

TEST_CASE("[SolersToolRegistry] transcript audit preserves full redacted tool arguments") {
	SolersToolRegistry registry;
	registry.register_default_tools();
	Array operations;
	for (int i = 0; i < 40; i++) {
		Dictionary operation;
		operation["op"] = "list_properties";
		operation["node_path"] = ".";
		operations.push_back(operation);
	}
	Dictionary args;
	args["operations"] = operations;
	const Dictionary audit = registry.redact_tool_args_for_audit(SNAME("objects.batch"), args);
	CHECK(Array(audit.get("operations", Array())).size() == 40);
}

TEST_CASE("[SolersAssetService] Poly Haven acquisition binds the searched catalog version") {
	SolersAssetService assets;
	Dictionary args;
	args["provider"] = "polyhaven";
	args["kind"] = "3d";
	args["asset_id"] = "synthetic_model";
	args["variant"] = "2k-gltf";
	const Dictionary result = assets.catalog_acquire(args, String());
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "INVALID_ARGUMENT");
	CHECK(String(Dictionary(result.get("error", Dictionary())).get("message", String())).contains("source_version"));
}

TEST_CASE("[SolersAssetService] Poly Haven variants come from official file metadata") {
	Dictionary model_spec;
	model_spec["url"] = "https://example.invalid/book.fbx";
	model_spec["md5"] = "0123456789abcdef0123456789abcdef";
	model_spec["size"] = 100;
	Dictionary dependency;
	dependency["url"] = "https://example.invalid/textures/book_diffuse.jpg";
	dependency["md5"] = "fedcba9876543210fedcba9876543210";
	dependency["size"] = 25;
	Dictionary includes;
	includes["textures/book_diffuse.jpg"] = dependency;
	model_spec["include"] = includes;
	Dictionary model_formats;
	model_formats["fbx"] = model_spec;
	Dictionary model_resolutions;
	model_resolutions["1k"] = model_formats;

	Dictionary texture_spec = model_spec.duplicate(true);
	texture_spec["url"] = "https://example.invalid/book_diffuse.jpg";
	Dictionary texture_formats;
	texture_formats["jpg"] = texture_spec;
	Dictionary texture_resolutions;
	texture_resolutions["1k"] = texture_formats;

	Dictionary files;
	files["fbx"] = model_resolutions;
	files["Diffuse"] = texture_resolutions;
	const Array variants = SolersAssetService::normalize_polyhaven_variants(files, "3d");
	REQUIRE(variants.size() == 1);
	const Dictionary variant = variants[0];
	CHECK(variant.get("id", String()) == "1k-fbx");
	CHECK(variant.get("checksum", String()) == "0123456789abcdef0123456789abcdef");
	CHECK((int64_t)variant.get("size", 0) == 125);
	CHECK(Array(variant.get("dependencies", Array())).has("textures/book_diffuse.jpg"));
}

TEST_CASE("[SolersAssetService] catalog variant identity is case-insensitive and preserves the official id") {
	Dictionary variant;
	variant["id"] = "Official-MixedCase";
	Array variants;
	variants.push_back(variant);

	CHECK(SolersAssetService::match_catalog_variant_id(variants, "official-mixedcase") == "Official-MixedCase");
	CHECK(SolersAssetService::match_catalog_variant_id(variants, "missing").is_empty());
}

TEST_CASE("[SolersAssetService] catalog ranking uses partial term coverage and stable relevance") {
	Dictionary exact;
	exact["asset_id"] = "wood_bed";
	exact["display_name"] = "Wood Bed";
	Array exact_tags;
	exact_tags.push_back("furniture");
	exact_tags.push_back("bed");
	exact["tags"] = exact_tags;
	exact["download_count"] = 10;

	Dictionary partial;
	partial["asset_id"] = "wooden_chair";
	partial["display_name"] = "Wooden Chair";
	partial["description"] = "A solid wood seat";
	partial["download_count"] = 1000;

	Dictionary unrelated;
	unrelated["asset_id"] = "metal_lamp";
	unrelated["display_name"] = "Metal Lamp";

	Array candidates;
	candidates.push_back(partial);
	candidates.push_back(unrelated);
	candidates.push_back(exact);
	const Array ranked = SolersAssetService::rank_catalog_assets(candidates, "wood bed");
	REQUIRE(ranked.size() == 2);
	CHECK(Dictionary(ranked[0]).get("asset_id", String()) == "wood_bed");
	CHECK(Array(Dictionary(ranked[0]).get("matched_terms", Array())).size() == 2);
	CHECK(Dictionary(ranked[1]).get("asset_id", String()) == "wooden_chair");
	CHECK(Array(Dictionary(ranked[1]).get("matched_terms", Array())).has("wood"));
	CHECK_FALSE(Dictionary(ranked[1]).has("_rank_primary"));
}

TEST_CASE("[SolersReflectionService][SceneTree] Environment sky sources require a Sky resource") {
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
	CHECK_FALSE((bool)SolersReflectionService::validate_environment_resource(environment).get("valid", true));

	Ref<Sky> sky;
	sky.instantiate();
	environment->set_sky(sky);
	CHECK(SolersReflectionService::validate_environment_resource(environment).get("valid", false));
}

TEST_CASE("[SolersLLMRetry] retry classification uses structured transport facts") {
	Dictionary explicit_retry;
	explicit_retry["retryable"] = true;
	explicit_retry["message"] = "ordinary failure";
	CHECK(SolersLLMRetry::is_retryable(explicit_retry));

	Dictionary explicit_terminal;
	explicit_terminal["retryable"] = false;
	explicit_terminal["http_status"] = 503;
	CHECK_FALSE(SolersLLMRetry::is_retryable(explicit_terminal));

	Dictionary rate_limited;
	rate_limited["http_status"] = 429;
	CHECK(SolersLLMRetry::is_retryable(rate_limited));

	Dictionary text_only;
	text_only["message"] = "rate limit";
	CHECK_FALSE(SolersLLMRetry::is_retryable(text_only));
}

TEST_CASE("[SolersLLMClient] transport failures use the failed state without a protocol error event") {
	Ref<TCPServer> server;
	server.instantiate();
	REQUIRE(server->listen(0, IPAddress("127.0.0.1")) == OK);

	SolersLLMProtocolRegistry protocols;
	protocols.register_builtin_protocols();
	SolersLLMClient client;
	client.set_protocol_registry(&protocols);

	Dictionary request;
	request["model"] = "synthetic-model";
	Array messages;
	messages.push_back(SolersLLMMessage::user("test"));
	request["messages"] = messages;
	request["tools"] = Array();
	Dictionary profile;
	profile["protocol"] = "openai-chat";
	profile["base_url"] = vformat("http://127.0.0.1:%d/v1", server->get_local_port());
	Dictionary auth;
	auth["type"] = "none";
	REQUIRE(client.begin(request, profile, auth) == OK);

	Array events;
	const uint64_t started = OS::get_singleton()->get_ticks_msec();
	while (!client.is_failed() && OS::get_singleton()->get_ticks_msec() - started < 2000) {
		if (server->is_connection_available()) {
			Ref<StreamPeerTCP> accepted = server->take_connection();
			if (accepted.is_valid()) {
				accepted->disconnect_from_host();
			}
			server->stop();
		}
		events.append_array(client.poll());
		OS::get_singleton()->delay_usec(1000);
	}
	events.append_array(client.poll());
	REQUIRE(client.is_failed());
	CHECK((bool)client.get_error().get("retryable", false));
	CHECK(find_event_kind(events, SolersLLMEventKind::ERROR).is_empty());
}

TEST_CASE("[SolersModelsDev] input modality support is model-level and unknown is permissive") {
	Dictionary vision_model;
	Array modalities;
	modalities.push_back("text");
	modalities.push_back("image");
	vision_model["input_modalities"] = modalities;
	CHECK(SolersModelsDev::input_modality_support(vision_model, "image") == 1);

	modalities.erase("image");
	vision_model["input_modalities"] = modalities;
	CHECK(SolersModelsDev::input_modality_support(vision_model, "image") == 0);
	CHECK(SolersModelsDev::input_modality_support(Dictionary(), "image") == -1);
}

TEST_CASE("[SolersModelsDev] reasoning effort options are model-declared with a custom-model fallback") {
	Dictionary effort_option;
	effort_option["type"] = "effort";
	Array declared_values;
	declared_values.push_back("low");
	declared_values.push_back("xhigh");
	effort_option["values"] = declared_values;
	Array reasoning_options;
	reasoning_options.push_back(effort_option);
	Dictionary declared_model;
	declared_model["reasoning"] = true;
	declared_model["reasoning_options"] = reasoning_options;
	CHECK(SolersModelsDev::reasoning_efforts(declared_model) == declared_values);

	Dictionary non_reasoning_model;
	non_reasoning_model["reasoning"] = false;
	CHECK(SolersModelsDev::reasoning_efforts(non_reasoning_model).is_empty());

	const Array custom_efforts = SolersModelsDev::reasoning_efforts(Dictionary());
	REQUIRE(custom_efforts.size() == 2);
	CHECK(custom_efforts[0] == "high");
	CHECK(custom_efforts[1] == "xhigh");
}

TEST_CASE("[SolersPlanCell] replaces its plan snapshot in place") {
	Array first_plan;
	Dictionary first_step;
	first_step["step"] = "Whitebox";
	first_step["status"] = "in_progress";
	first_plan.push_back(first_step);
	const String first_text = SolersPlanCell::format_plan_text("Starting geometry", first_plan);

	Array second_plan;
	Dictionary second_step;
	second_step["step"] = "Whitebox";
	second_step["status"] = "completed";
	second_plan.push_back(second_step);
	const String second_text = SolersPlanCell::format_plan_text("Geometry verified", second_plan);

	CHECK(first_text.contains("Starting geometry"));
	CHECK(second_text.contains("Geometry verified"));
	CHECK(second_text.contains(String::utf8("✓ Whitebox")));
	CHECK_FALSE(second_text.contains("Starting geometry"));
}

TEST_CASE("[SolersProviderRegistry] is the single transport profile source") {
	SolersProviderRegistry registry;
	const Dictionary openai = registry.resolve_provider_profile("openai");
	const Dictionary custom = registry.resolve_provider_profile("custom_openai_compatible", "https://gateway.example/v1");
	const Dictionary codex = registry.resolve_provider_profile("openai_codex");

	CHECK(openai.get("protocol", String()) == "openai-chat");
	CHECK(custom.get("id", String()) == "custom_openai_compatible");
	CHECK(custom.get("protocol", String()) == "openai-chat");
	CHECK(custom.get("base_url", String()) == "https://gateway.example/v1");
	CHECK(codex.get("protocol", String()) == "openai-responses");
	CHECK(codex.get("auth_type", String()) == "oauth");
	CHECK(codex.get("catalog_provider", String()) == "openai");
	CHECK(Dictionary(codex.get("headers", Dictionary())).get("originator", String()) == "solers");
	CHECK_FALSE(codex.get("supports_max_output_tokens", true));
	const Dictionary gpt55_limits = Dictionary(codex.get("model_limits", Dictionary())).get("gpt-5.5", Dictionary());
	CHECK((int)gpt55_limits.get("context", 0) == 400000);
	const Array allowed_models = codex.get("allowed_models", Array());
	REQUIRE_FALSE(allowed_models.is_empty());
	CHECK(registry.is_model_allowed("openai_codex", allowed_models[0]));
	CHECK_FALSE(registry.is_model_allowed("openai_codex", "synthetic-unsupported-model"));
}

TEST_CASE("[SolersOpenAIResponsesProtocol] lowers encrypted reasoning and tool continuation") {
	Array calls;
	Dictionary call;
	call["id"] = "call_weather";
	call["name"] = "get_weather";
	call["arguments"] = "{\"city\":\"Paris\"}";
	calls.push_back(call);

	Dictionary reasoning;
	reasoning["id"] = "rs_1";
	reasoning["summary"] = Array();
	reasoning["encrypted_content"] = "encrypted";
	Array reasoning_items;
	reasoning_items.push_back(reasoning);
	Dictionary openai;
	openai["reasoning_items"] = reasoning_items;
	Dictionary metadata;
	metadata["openai_responses"] = openai;

	Array messages;
	messages.push_back(SolersLLMMessage::user("Weather?"));
	messages.push_back(SolersLLMMessage::assistant(String(), calls, metadata));
	messages.push_back(SolersLLMMessage::tool_result("call_weather", "get_weather", "sunny"));
	Dictionary request;
	request["model"] = "gpt-5.5";
	request["system"] = "Use tools.";
	request["messages"] = messages;
	request["tools"] = Array();
	request["max_tokens"] = 4096;
	request["session_id"] = "synthetic-session";
	request["reasoning_effort"] = "high";

	SolersOpenAIResponsesProtocol protocol;
	Dictionary headers;
	protocol.augment_headers(headers, request);
	CHECK(headers.get("session-id", String()) == "synthetic-session");
	const Dictionary body = protocol.build_request_body(request);
	const Array input = body.get("input", Array());
	REQUIRE(input.size() == 4);
	CHECK(Dictionary(input[0]).get("role", String()) == "user");
	CHECK(Dictionary(input[1]).get("type", String()) == "reasoning");
	CHECK(Dictionary(input[2]).get("type", String()) == "function_call");
	CHECK(Dictionary(input[3]).get("type", String()) == "function_call_output");
	CHECK_FALSE(body.get("store", true));
	CHECK(body.get("stream", false));
	CHECK((int)body.get("max_output_tokens", 0) == 4096);
	CHECK(Dictionary(body.get("reasoning", Dictionary())).get("effort", String()) == "high");
}

TEST_CASE("[SolersOpenAIResponsesProtocol] lifts streamed tool calls usage and continuation metadata") {
	SolersOpenAIResponsesProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());
	protocol.parse_event(state, "response.output_item.done", R"json({"type":"response.output_item.done","item":{"id":"rs_1","type":"reasoning","summary":[],"encrypted_content":"cipher"}})json");

	Array events = protocol.parse_event(state, "response.output_item.added", R"json({"type":"response.output_item.added","item":{"id":"fc_1","type":"function_call","call_id":"call_1","name":"project_get_info","arguments":""}})json");
	CHECK(find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START).get("id", String()) == "call_1");
	events = protocol.parse_event(state, "response.function_call_arguments.delta", R"json({"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"{}"})json");
	CHECK(find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_DELTA).get("arguments", String()) == "{}");
	events = protocol.parse_event(state, "response.output_item.done", R"json({"type":"response.output_item.done","item":{"id":"fc_1","type":"function_call","call_id":"call_1","name":"project_get_info","arguments":"{}"}})json");
	CHECK(find_event_kind(events, SolersLLMEventKind::TOOL_CALL).get("id", String()) == "call_1");

	events = protocol.parse_event(state, "response.completed", R"json({"type":"response.completed","response":{"id":"resp_1","usage":{"input_tokens":12,"output_tokens":4}}})json");
	CHECK((int)find_event_kind(events, SolersLLMEventKind::USAGE).get("input_tokens", 0) == 12);
	const Dictionary finish = find_event_kind(events, SolersLLMEventKind::FINISH);
	CHECK(finish.get("stop_reason", String()) == SolersLLMStopReason::TOOL_USE);
	const Dictionary finish_metadata = finish.get("provider_metadata", Dictionary());
	const Dictionary finish_openai = finish_metadata.get("openai_responses", Dictionary());
	CHECK(Array(finish_openai.get("reasoning_items", Array())).size() == 1);
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

TEST_CASE("[SolersAnthropicMessagesProtocol] lowers model effort into output config") {
	Dictionary request;
	request["model"] = "claude-sonnet-4.6";
	request["messages"] = Array();
	request["reasoning_effort"] = "high";

	SolersAnthropicMessagesProtocol protocol;
	const Dictionary body = protocol.build_request_body(request);
	CHECK(Dictionary(body.get("output_config", Dictionary())).get("effort", String()) == "high");
}

TEST_CASE("[SolersOpenAIChatProtocol] normalizes Responses item ids into stable Chat operation ids") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());
	const String normalized_id = "call_solers_" + String("fc_item_1").md5_text().substr(0, 24);

	Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"fc_item_1","type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == normalized_id);
	CHECK(input_start.get("name", String()) == "project_get_info");
	CHECK(input_start.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	bool saw_tool_call = false;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		if (event.get("kind", String()) == String(SolersLLMEventKind::TOOL_CALL)) {
			saw_tool_call = true;
			CHECK(event.get("id", String()) == normalized_id);
			CHECK(event.get("name", String()) == "project_get_info");
			CHECK(event.get("arguments", String()) == "{}");
		}
	}
	CHECK(saw_tool_call);
}

TEST_CASE("[SolersOpenAIChatProtocol] normalizes a Responses item id exposed as call_id") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());
	const String normalized_id = "call_solers_" + String("fc_item_1").md5_text().substr(0, 24);

	Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"fc_item_1","call_id":"fc_item_1","type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == normalized_id);
	CHECK(input_start.get("name", String()) == "project_get_info");
	CHECK(input_start.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	bool saw_tool_call = false;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		if (event.get("kind", String()) == String(SolersLLMEventKind::TOOL_CALL)) {
			saw_tool_call = true;
			CHECK(event.get("id", String()) == normalized_id);
			CHECK(event.get("name", String()) == "project_get_info");
			CHECK(event.get("arguments", String()) == "{}");
		}
	}
	CHECK(saw_tool_call);
}

TEST_CASE("[SolersOpenAIChatProtocol] rejects a tool call with no provider identity") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());

	protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	const Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	CHECK_FALSE(find_event_kind(events, SolersLLMEventKind::ERROR).is_empty());
	CHECK(find_event_kind(events, SolersLLMEventKind::TOOL_CALL).is_empty());
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

TEST_CASE("[SolersLLMProtocol] lowers tool result images for OpenAI and Anthropic") {
	const String image_path = "user://.solers_tool_image_contract.bin";
	Ref<FileAccess> image_file = FileAccess::open(image_path, FileAccess::WRITE);
	REQUIRE(image_file.is_valid());
	PackedByteArray bytes;
	bytes.push_back(1);
	bytes.push_back(2);
	bytes.push_back(3);
	image_file->store_buffer(bytes);
	image_file.unref();

	Dictionary attachment;
	attachment["id"] = "capture_contract";
	attachment["source"] = "tool_capture";
	attachment["type"] = "image";
	attachment["mime_type"] = "image/png";
	attachment["local_path"] = image_path;
	Array attachments;
	attachments.push_back(attachment);

	Array calls;
	Dictionary call;
	call["id"] = "call_capture";
	call["name"] = "viewport_capture";
	call["arguments"] = "{\"target\":\"editor\"}";
	calls.push_back(call);
	Dictionary second_call;
	second_call["id"] = "call_snapshot";
	second_call["name"] = "editor_get_snapshot";
	second_call["arguments"] = "{}";
	calls.push_back(second_call);

	Array messages;
	Dictionary user_message = SolersLLMMessage::user("Inspect the result.");
	user_message["attachments"] = attachments;
	messages.push_back(user_message);
	messages.push_back(SolersLLMMessage::assistant("", calls));
	messages.push_back(SolersLLMMessage::tool_result("call_capture", "viewport_capture", "{\"ok\":true}", attachments));
	messages.push_back(SolersLLMMessage::tool_result("call_snapshot", "editor_get_snapshot", "{\"ok\":true}"));

	Dictionary request;
	request["model"] = "custom-gateway-model";
	request["messages"] = messages;

	SolersOpenAIChatProtocol openai;
	Array openai_messages = Dictionary(openai.build_request_body(request)).get("messages", Array());
	REQUIRE(openai_messages.size() == 5);
	Array openai_user_content = Dictionary(openai_messages[0]).get("content", Array());
	REQUIRE(openai_user_content.size() == 2);
	CHECK(Dictionary(openai_user_content[1]).get("type", String()) == "image_url");
	CHECK(Dictionary(openai_messages[2]).get("role", String()) == "tool");
	CHECK(Dictionary(openai_messages[3]).get("role", String()) == "tool");
	Dictionary openai_image_message = openai_messages[4];
	CHECK(openai_image_message.get("role", String()) == "user");
	Array openai_content = openai_image_message.get("content", Array());
	REQUIRE(openai_content.size() == 2);
	CHECK(Dictionary(openai_content[1]).get("type", String()) == "image_url");

	SolersAnthropicMessagesProtocol anthropic;
	Array anthropic_messages = Dictionary(anthropic.build_request_body(request)).get("messages", Array());
	REQUIRE(anthropic_messages.size() == 3);
	Dictionary tool_user = anthropic_messages[2];
	Array tool_blocks = tool_user.get("content", Array());
	REQUIRE(tool_blocks.size() == 2);
	Array tool_content = Dictionary(tool_blocks[0]).get("content", Array());
	REQUIRE(tool_content.size() == 2);
	CHECK(Dictionary(tool_content[1]).get("type", String()) == "image");

	DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(image_path));
}

} // namespace TestSolersProviderGateway
