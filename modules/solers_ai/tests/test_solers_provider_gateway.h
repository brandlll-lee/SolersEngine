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
#include "core/templates/pair.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/asset_library/editor_asset_installer.h"
#include "editor/settings/editor_settings.h"
#include "modules/zip/zip_packer.h"
#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/plugins/solers_plugin.h"
#include "modules/solers_ai/plugins/solers_plugin_meshy.h"
#include "modules/solers_ai/plugins/solers_plugin_polyhaven.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
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

class SolersSyntheticFuturePlugin : public SolersPlugin {
public:
	bool job_ran = false;

	Dictionary get_profile() const override {
		Dictionary profile;
		profile["id"] = "synthetic-future";
		profile["label"] = "Synthetic Future";
		Array kinds;
		kinds.push_back("novel-geometry");
		profile["kinds"] = kinds;
		profile["supports_generation"] = true;
		profile["supports_catalog"] = false;
		profile["supports_resume"] = false;
		profile["requires_api_key"] = false;
		return profile;
	}

	Dictionary get_generation_options_schema(const String &p_kind) const override {
		Dictionary density;
		density["type"] = "number";
		density["description"] = "Synthetic contract option.";
		Dictionary properties;
		properties["density"] = density;
		return properties;
	}

	void run_job(const Ref<SolersPluginJob> &p_job) override {
		job_ran = p_job.is_valid();
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

Dictionary find_operation_def(const Array &p_operations, const String &p_operation_id) {
	for (int i = 0; i < p_operations.size(); i++) {
		const Dictionary operation = p_operations[i];
		if (String(operation.get("operation_id", String())) == p_operation_id) {
			return operation;
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

void write_test_zip(const String &p_path, const Vector<Pair<String, String>> &p_files) {
	Ref<ZIPPacker> packer;
	packer.instantiate();
	REQUIRE(packer->open(p_path, ZIPPacker::APPEND_CREATE) == OK);
	for (const Pair<String, String> &file : p_files) {
		REQUIRE(packer->start_file(file.first) == OK);
		REQUIRE(packer->write_file(file.second.to_utf8_buffer()) == OK);
		REQUIRE(packer->close_file() == OK);
	}
	REQUIRE(packer->close() == OK);
}

TEST_CASE("[SolersSecretStore] strict credentials are protected and recoverable") {
	const String secret = "synthetic-oauth-credential";
	const String stored = SolersSecretStore::protect_strict(secret);
	REQUIRE_FALSE(stored.is_empty());
	CHECK(SolersSecretStore::is_protected(stored));
	CHECK(stored != secret);
	CHECK(SolersSecretStore::unprotect(stored) == secret);
}

TEST_CASE("[SolersProviderRegistry] assembles catalog and AuthHook overlays") {
	SolersProviderRegistry registry;

	Dictionary openai = registry.get_provider_profile("openai");
	Dictionary anthropic = registry.get_provider_profile("anthropic");

	CHECK(openai.get("protocol", String()) == "openai-chat");
	CHECK(openai.get("default_base_url", String()) == "https://api.openai.com/v1");
	CHECK(anthropic.get("protocol", String()) == "anthropic-messages");
	CHECK(anthropic.get("default_base_url", String()) == "https://api.anthropic.com");
	CHECK(registry.is_known_provider("openai"));
	CHECK(registry.is_known_provider("openai_codex"));
}

TEST_CASE("[SolersProviderRegistry] routes custom gateways without a hardcoded allowlist") {
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

	// Behavior contract: a brand-new provider id is OpenAI-compatible custom —
	// no UNKNOWN_PROVIDER allowlist patch required.
	Dictionary fresh;
	fresh["provider"] = "synthetic-brand-new-gateway";
	fresh["model"] = "synthetic-model";
	fresh["base_url"] = "https://gateway.example/v1";
	fresh["api_key"] = "synthetic-key";
	const Dictionary accepted = registry.validate_config(fresh);
	CHECK(accepted.get("ok", false));
	CHECK(Dictionary(accepted.get("data", Dictionary())).get("valid", false));
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

	SolersModelsDev models_dev;
	models_dev.initialize();
	SolersProviderRegistry registry;
	registry.set_models_dev(&models_dev, false);
	for (const bool enabled : { false, true }) {
		settings->set_manually(prefix + "settings_version", 3);
		settings->set_manually(prefix + "privacy_mode", enabled);
		settings->erase(prefix + "local_models_only");
		SolersSettingsService migration_service;
		migration_service.set_provider_registry(&registry);
		CHECK(migration_service.get_local_models_only() == enabled);
		CHECK_FALSE(settings->has_setting(prefix + "privacy_mode"));
		CHECK((int)settings->get_setting(prefix + "settings_version") == 5);
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

	// Contract: never-special-cased catalog-style id connects via assembler.
	Dictionary brand_new;
	brand_new["provider"] = "synthetic-never-special-cased";
	brand_new["model"] = "m1";
	brand_new["base_url"] = "https://example.test/v1";
	brand_new["api_key"] = "k";
	service.set_local_models_only(false);
	const Dictionary brand_saved = service.set_provider_config(brand_new);
	CHECK(brand_saved.get("ok", false));
	CHECK(Dictionary(brand_saved.get("data", Dictionary())).get("connected", false));
	service.disconnect_provider("synthetic-never-special-cased");
	service.set_provider_config(remote);
	service.set_local_models_only(true);

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

	CHECK(registry.get_tool_count() == 1);
	Array tools = registry.list_tools();
	REQUIRE(tools.size() == 1);
	Dictionary tool = tools[0];
	CHECK(tool.get("name", String()) == "synthetic.echo");
	CHECK(tool.get("model_name", String()) == "synthetic_echo");
	CHECK(tool.get("execution", String()) == "worker");
	CHECK_FALSE(Dictionary(Dictionary(tool.get("input_schema", Dictionary())).get("properties", Dictionary())).has("retry_of"));
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
	bundled["version"] = "1.0.2-stable";
	bundled["sha256"] = "a071850250ec5e596aa54da61c01d75768774eb379ee997584d426a45f4884a2";
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

TEST_CASE("[Editor][SolersAddon] package paths are validated and project writes roll back") {
	const String unsafe_zip = "user://.solers_unsafe_plugin_contract.zip";
	const String package_zip = "user://.solers_plugin_transaction_contract.zip";
	const String target_dir = "res://.solers_plugin_transaction_contract";
	const String target_absolute = ProjectSettings::get_singleton()->globalize_path(target_dir);
	if (Ref<DirAccess> old = DirAccess::open(target_absolute); old.is_valid()) {
		old->erase_contents_recursive();
		DirAccess::remove_absolute(target_absolute);
	}
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(unsafe_zip));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(package_zip));

	Vector<Pair<String, String>> unsafe_files;
	unsafe_files.push_back(Pair<String, String>("../escape.txt", "escape"));
	write_test_zip(unsafe_zip, unsafe_files);
	const Dictionary unsafe = EditorAssetPackageInstaller::inspect_package(unsafe_zip, target_dir);
	CHECK_FALSE((bool)unsafe.get("ok", true));
	CHECK(Dictionary(unsafe.get("error", Dictionary())).get("code", String()) == "PACKAGE_PATH_INVALID");

	Vector<Pair<String, String>> package_files;
	package_files.push_back(Pair<String, String>("good.txt", "after"));
	package_files.push_back(Pair<String, String>("bad.txt", "child"));
	write_test_zip(package_zip, package_files);
	REQUIRE(DirAccess::make_dir_recursive_absolute(target_absolute) == OK);
	Ref<FileAccess> existing = FileAccess::open(target_dir.path_join("good.txt"), FileAccess::WRITE);
	REQUIRE(existing.is_valid());
	existing->store_string("before");
	existing.unref();
	Ref<FileAccess> blocker = FileAccess::open(target_dir.path_join("blocker"), FileAccess::WRITE);
	REQUIRE(blocker.is_valid());
	blocker->store_string("not a directory");
	blocker.unref();

	Dictionary mappings;
	mappings["good.txt"] = "good.txt";
	mappings["bad.txt"] = "blocker/child.txt";
	PackedStringArray selected;
	selected.push_back("good.txt");
	selected.push_back("bad.txt");
	PackedStringArray overwrites;
	overwrites.push_back(target_dir.path_join("good.txt"));
	Dictionary manifest;
	manifest["version"] = 1;
	const String manifest_path = target_dir.path_join("lock.json");
	const Dictionary failed = EditorAssetPackageInstaller::install_package(package_zip, target_dir, mappings, selected, overwrites, PackedStringArray(), manifest_path, manifest);
	CHECK_FALSE((bool)failed.get("ok", true));
	CHECK(FileAccess::get_file_as_string(target_dir.path_join("good.txt")) == "before");
	CHECK_FALSE(FileAccess::exists(manifest_path));

	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(target_dir.path_join("blocker")));
	Dictionary good_mapping;
	good_mapping["good.txt"] = "good.txt";
	PackedStringArray good_selection;
	good_selection.push_back("good.txt");
	const Dictionary conflict = EditorAssetPackageInstaller::install_package(package_zip, target_dir, good_mapping, good_selection);
	CHECK_FALSE((bool)conflict.get("ok", true));
	CHECK(FileAccess::get_file_as_string(target_dir.path_join("good.txt")) == "before");

	const Dictionary installed = EditorAssetPackageInstaller::install_package(package_zip, target_dir, mappings, selected, overwrites, PackedStringArray(), manifest_path, manifest);
	REQUIRE(installed.get("ok", false));
	CHECK(FileAccess::get_file_as_string(target_dir.path_join("good.txt")) == "after");
	CHECK(FileAccess::get_file_as_string(target_dir.path_join("blocker/child.txt")) == "child");
	CHECK(FileAccess::exists(manifest_path));

	Dictionary upgraded_manifest;
	upgraded_manifest["version"] = 2;
	PackedStringArray removals;
	removals.push_back(target_dir.path_join("blocker/child.txt"));
	const Dictionary upgraded = EditorAssetPackageInstaller::install_package(package_zip, target_dir, good_mapping, good_selection, overwrites, removals, manifest_path, upgraded_manifest);
	REQUIRE(upgraded.get("ok", false));
	CHECK(FileAccess::get_file_as_string(target_dir.path_join("good.txt")) == "after");
	CHECK_FALSE(FileAccess::exists(target_dir.path_join("blocker/child.txt")));
	CHECK(FileAccess::get_file_as_string(manifest_path).contains("\"version\": 2"));

	if (Ref<DirAccess> cleanup = DirAccess::open(target_absolute); cleanup.is_valid()) {
		cleanup->erase_contents_recursive();
		DirAccess::remove_absolute(target_absolute);
	}
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(unsafe_zip));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(package_zip));
}

TEST_CASE("[Editor][SolersAddon] bundled Terrain3D archive is pinned and self-describing") {
	const String invalid_archive = "user://.solers_invalid_terrain_bundle.zip";
	Vector<Pair<String, String>> invalid_files;
	invalid_files.push_back(Pair<String, String>("addons/terrain_3d/plugin.cfg", "[plugin]"));
	write_test_zip(invalid_archive, invalid_files);
	const String previous_override = OS::get_singleton()->get_environment("SOLERS_TERRAIN3D_ARCHIVE");
	OS::get_singleton()->set_environment("SOLERS_TERRAIN3D_ARCHIVE", ProjectSettings::get_singleton()->globalize_path(invalid_archive));
	SolersAssetService invalid_assets;
	Dictionary args;
	args["source"] = "bundled";
	args["plugin_id"] = "terrain3d";
	const Dictionary mismatch = invalid_assets.addon_inspect(args);
	if (previous_override.is_empty()) {
		OS::get_singleton()->unset_environment("SOLERS_TERRAIN3D_ARCHIVE");
	} else {
		OS::get_singleton()->set_environment("SOLERS_TERRAIN3D_ARCHIVE", previous_override);
	}
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(invalid_archive));
	CHECK_FALSE((bool)mismatch.get("ok", true));
	CHECK(Dictionary(mismatch.get("error", Dictionary())).get("code", String()) == "PLUGIN_HASH_MISMATCH");

	SolersAssetService assets;
	const Dictionary result = assets.addon_inspect(args);
	REQUIRE(result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("version", String()) == "1.0.2-stable");
	CHECK(data.get("sha256", String()) == "a071850250ec5e596aa54da61c01d75768774eb379ee997584d426a45f4884a2");
	CHECK(data.get("license", String()) == "MIT");
	CHECK((bool)data.get("contains_executable_code", false));
	CHECK(Array(data.get("plugin_names", Array())).has("terrain_3d"));
	CHECK_FALSE(Array(data.get("gdextensions", Array())).is_empty());
	const Dictionary contract = data.get("agent_contract", Dictionary());
	CHECK((int)contract.get("schema_version", 0) == 1);
	CHECK(!String(contract.get("contract_id", String())).is_empty());
	CHECK(Array(contract.get("entry_classes", Array())).has("Terrain3DData"));
	CHECK_FALSE(Array(contract.get("workflow", Array())).is_empty());
	CHECK(String(Array(contract.get("workflow", Array()))[0]).contains("data_directory"));
	Dictionary contract_args;
	contract_args["source"] = "bundled";
	contract_args["plugin_id"] = "terrain3d";
	contract_args["version"] = data.get("version", String());
	contract_args["sha256"] = data.get("sha256", String());
	CHECK(assets.addon_agent_contract(contract_args).get("ok", false));
	contract_args["sha256"] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	CHECK_FALSE((bool)assets.addon_agent_contract(contract_args).get("ok", true));
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

TEST_CASE("[SolersToolRegistry] file checkpoint reversal survives session restoration") {
	const String path = "res://.solers_reversal_contract.txt";
	const String global_path = ProjectSettings::get_singleton()->globalize_path(path);
	if (FileAccess::exists(path)) {
		DirAccess::remove_file_or_error(global_path);
	}
	const String session_id = "reversal-contract-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
	const String project_path = "test://solers-reversal-contract";

	SolersPermissionManager write_permissions;
	write_permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, true);
	SolersFileCheckpoint write_checkpoint;
	SolersScriptService write_scripts;
	SolersToolRegistry write_registry;
	write_registry.set_permission_manager(&write_permissions);
	write_registry.set_file_checkpoint(&write_checkpoint);
	write_registry.set_script_service(&write_scripts);
	write_registry.register_default_tools();
	SolersToolContext write_context;
	write_context.call_id = "create-file";
	write_context.session_id = session_id;
	write_context.project_path = project_path;
	Dictionary write_args;
	write_args["operation"] = "write_file";
	write_args["path"] = path;
	write_args["content"] = "created by reversible contract\n";
	const Dictionary write_result = write_registry.call_tool_with_context(SNAME("project.edit"), write_args, write_context);
	REQUIRE((bool)write_result.get("ok", false));
	const Dictionary mutation = Dictionary(write_result.get("data", Dictionary())).get("mutation", Dictionary());
	const String reversal_id = mutation.get("reversal_id", String());
	REQUIRE(!reversal_id.is_empty());
	CHECK(FileAccess::exists(path));

	SolersPermissionManager restore_permissions;
	restore_permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, true);
	SolersFileCheckpoint restore_checkpoint;
	SolersScriptService restore_scripts;
	SolersToolRegistry restore_registry;
	restore_registry.set_permission_manager(&restore_permissions);
	restore_registry.set_file_checkpoint(&restore_checkpoint);
	restore_registry.set_script_service(&restore_scripts);
	restore_registry.register_default_tools();
	SolersAgentSession restored_session;
	restored_session.set_tool_registry(&restore_registry);
	restored_session.set_session(project_path, session_id);
	CHECK((int64_t)restored_session.get_status().get("session_revision", -1) == 1);

	SolersToolContext revert_context;
	revert_context.call_id = "revert-file";
	revert_context.session_id = session_id;
	revert_context.project_path = project_path;
	revert_context.authored_revision = 1;
	Dictionary revert_args;
	revert_args["reversal_id"] = reversal_id;
	const Dictionary revert_result = restore_registry.call_tool_with_context(SNAME("history.revert"), revert_args, revert_context);
	REQUIRE((bool)revert_result.get("ok", false));
	CHECK_FALSE(FileAccess::exists(path));
	restored_session.shutdown();
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

TEST_CASE("[SolersToolRegistry] default model surface is domain-first") {
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
		"project.read_file",
		"project.search",
		"project.edit",
		"object.query",
		"object.transaction",
		"script.edit",
		"script.validate",
		"script.compute",
		"engine.describe",
		"runtime.control",
		"runtime.observe",
		"history.revert",
		"skill.read",
		"scene.bake_csg",
		"scene.open",
		"scene.reload",
		"mesh.unwrap_uv2",
		"lightmap.bake",
		"render.capture",
		"asset.catalog.search",
		"asset.catalog.inspect",
		"asset.catalog.acquire",
		"job.wait",
		"asset.generate",
		"asset.status",
		"addon.search",
		"addon.inspect",
		"addon.list",
		"addon.ensure",
	};
	for (const char *name : required_direct) {
		Dictionary tool = find_tool_def(tools, name);
		REQUIRE_FALSE(tool.is_empty());
		CHECK(tool.get("exposure", String()) == "direct");
	}
	const char *removed_core[] = {
		"class.search", "class.introspect", "objects.batch", "editor.get_snapshot", "project.write_file", "script.patch",
		"resource.get_info", "resource.create", "resource.get_property", "resource.set_property", "native.instantiate", "native.load",
		"native.list_properties", "native.get", "native.set", "native.list_methods", "native.call", "native.save", "native.free",
		"scene.instantiate", "scene.validate_spatial", "scene.validate_structure", "object.get_property", "object.set_property",
		"object.call_method", "editor.invoke", "asset.list_local", "asset.import_to_project",
		"plugin.search", "plugin.inspect", "plugin.list", "plugin.ensure",
		"scene.inspect", "scene.edit", "scene.validate", "resource.inspect", "resource.edit", "engine.inspect", "engine.execute", "viewport.capture"
	};
	for (const char *name : removed_core) {
		CHECK(find_tool_def(tools, name).is_empty());
	}
	CHECK(find_tool_def(tools, "tool.search").is_empty());
	CHECK(find_tool_def(tools, "asset.refine_to_ready").is_empty());
	CHECK(find_tool_def(tools, "asset.optimize_geometry").is_empty());
	CHECK(find_tool_def(tools, "asset.restyle_material").is_empty());
	CHECK(find_tool_def(tools, "ambientcg.search").is_empty());
	CHECK(find_tool_def(tools, "ambientcg.acquire").is_empty());
	Dictionary catalog_search = find_tool_def(tools, "asset.catalog.search");
	Dictionary catalog_search_properties = Dictionary(catalog_search.get("input_schema", Dictionary())).get("properties", Dictionary());
	const Array catalog_providers = Dictionary(catalog_search_properties.get("provider", Dictionary())).get("enum", Array());
	CHECK_FALSE(catalog_providers.is_empty());
	for (int i = 0; i < catalog_providers.size(); i++) {
		SolersPlugin *plugin = SolersPluginRegistry::get_plugin(catalog_providers[i]);
		CHECK(plugin != nullptr);
		if (plugin) {
			CHECK((bool)plugin->get_profile().get("supports_catalog", false));
		}
	}
	Dictionary catalog_acquire = find_tool_def(tools, "asset.catalog.acquire");
	Dictionary catalog_acquire_properties = Dictionary(catalog_acquire.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(catalog_acquire_properties.has("source_version"));
	CHECK_FALSE(Array(Dictionary(catalog_acquire_properties.get("kind", Dictionary())).get("enum", Array())).is_empty());
	Dictionary catalog_inspect = find_tool_def(tools, "asset.catalog.inspect");
	Dictionary catalog_inspect_properties = Dictionary(catalog_inspect.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(catalog_inspect_properties.has("asset_id"));
	CHECK_FALSE(Array(Dictionary(catalog_inspect_properties.get("kind", Dictionary())).get("enum", Array())).is_empty());
	CHECK(Dictionary(Dictionary(find_tool_def(tools, "job.wait").get("input_schema", Dictionary())).get("properties", Dictionary())).has("ids"));
	const Dictionary object_query = find_tool_def(tools, "object.query");
	REQUIRE_FALSE(object_query.is_empty());
	CHECK(Array(Dictionary(object_query.get("input_schema", Dictionary())).get("oneOf", Array())).size() == 4);
	const Dictionary transaction = find_tool_def(tools, "object.transaction");
	REQUIRE_FALSE(transaction.is_empty());
	CHECK(transaction.get("execution", String()) == "main_thread");
	CHECK(transaction.get("mutation_policy_dynamic", false));
	CHECK(Array(Dictionary(transaction.get("input_schema", Dictionary())).get("oneOf", Array())).size() == 2);
	Dictionary engine_describe = find_tool_def(tools, "engine.describe");
	CHECK_FALSE(engine_describe.has("ephemeral_result"));
	const Dictionary engine_describe_properties = Dictionary(engine_describe.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(engine_describe_properties.has("query"));
	CHECK(engine_describe_properties.has("classes"));
	Dictionary script_compute = find_tool_def(tools, "script.compute");
	CHECK(script_compute.get("mutation_policy", String()) == "file_checkpoint");
	CHECK(script_compute.get("permission", String()) == "edit_files");
	const Dictionary compute_properties = Dictionary(Dictionary(script_compute.get("input_schema", Dictionary())).get("properties", Dictionary()));
	CHECK(compute_properties.has("source"));
	CHECK(compute_properties.has("outputs"));
	Dictionary project_edit = find_tool_def(tools, "project.edit");
	CHECK(project_edit.get("permission", String()) == "edit_files");
	CHECK(project_edit.get("mutation_policy_dynamic", false));
	CHECK(Dictionary(project_edit.get("input_schema", Dictionary())).has("oneOf"));
	CHECK(String(find_tool_def(tools, "mesh.unwrap_uv2").get("description", String())).contains("off the editor thread"));
	const Dictionary render_capture = find_tool_def(tools, "render.capture");
	CHECK_FALSE(render_capture.has("ephemeral_result"));
	CHECK(Array(Dictionary(render_capture.get("input_schema", Dictionary())).get("oneOf", Array())).size() == 2);
	CHECK(find_tool_def(tools, "editor.action.list").is_empty());
	CHECK(find_tool_def(tools, "editor.action.execute").is_empty());
	CHECK_FALSE(find_tool_def(tools, "skill.read").has("ephemeral_result"));
	Dictionary project_search = find_tool_def(tools, "project.search");
	REQUIRE_FALSE(project_search.is_empty());
	CHECK(project_search.get("exposure", String()) == "direct");
	CHECK(Dictionary(project_search.get("input_schema", Dictionary())).has("oneOf"));
	CHECK(find_tool_def(tools, "project.search_files").is_empty());
	CHECK(find_tool_def(tools, "project.get_info").is_empty());
	CHECK(find_tool_def(tools, "project.get_settings_summary").is_empty());
	CHECK(find_tool_def(tools, "project.list_files").is_empty());
	CHECK(find_tool_def(tools, "script.read").is_empty());
	CHECK(find_tool_def(tools, "scene.get_open_scenes").is_empty());
	CHECK(find_tool_def(tools, "scene.get_tree").is_empty());
	CHECK(find_tool_def(tools, "selection.get_nodes").is_empty());
	CHECK(find_tool_def(tools, "runtime.get_status").is_empty());
	CHECK(find_tool_def(tools, "editor.get_logs").is_empty());
	Dictionary export_run = find_tool_def(tools, "export.run_preset");
	REQUIRE_FALSE(export_run.is_empty());
	CHECK(export_run.get("exposure", String()) == "direct");
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

TEST_CASE("[SolersPluginRegistry] an unknown connector extends every registry-driven surface") {
	SolersSyntheticFuturePlugin *plugin = memnew(SolersSyntheticFuturePlugin);
	SolersPluginRegistry::register_plugin(plugin);

	CHECK(SolersPluginRegistry::get_plugin("synthetic-future") == plugin);
	CHECK(SolersPluginRegistry::default_generator_for_kind("novel-geometry") == plugin);

	SolersAssetService asset_service;
	SolersToolRegistry registry;
	registry.set_asset_service(&asset_service);
	registry.register_default_tools();
	const Dictionary generate = find_tool_def(registry.list_tools(), "asset.generate");
	const Dictionary properties = Dictionary(generate.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(Array(Dictionary(properties.get("provider", Dictionary())).get("enum", Array())).has("synthetic-future"));
	CHECK(Array(Dictionary(properties.get("kind", Dictionary())).get("enum", Array())).has("novel-geometry"));
	CHECK(Dictionary(Dictionary(properties.get("provider_options", Dictionary())).get("properties", Dictionary())).has("density"));

	const String partial = "Create with @synthetic-fut";
	int mention_start = -1;
	CHECK(SolersMention::query_at(partial, partial.length(), mention_start) == "synthetic-fut");
	CHECK(mention_start == 12);
	const Array mentions = SolersMention::parse("Use @synthetic-future, not @synthetic-future-extra.");
	CHECK(mentions.size() == 1);
	if (mentions.size() == 1) {
		CHECK(Dictionary(mentions[0]).get("id", String()) == "synthetic-future");
		CHECK(Dictionary(mentions[0]).get("source", String()) == "plugin");
	}

	const Array generators = SolersMention::collect_section_items("solers", nullptr, String());
	bool found_synthetic_generator = false;
	for (int i = 0; i < generators.size(); i++) {
		if (String(Dictionary(generators[i]).get("id", String())) == "synthetic-future") {
			found_synthetic_generator = true;
			break;
		}
	}
	CHECK(found_synthetic_generator);

	Dictionary file_mention;
	file_mention["source"] = "file";
	file_mention["id"] = "res://does-not-need-to-exist-for-format.tscn";
	file_mention["path"] = file_mention["id"];
	file_mention["label"] = "does-not-need-to-exist-for-format.tscn";
	CHECK(SolersMention::format_token(file_mention) == "@file:res://does-not-need-to-exist-for-format.tscn");
	CHECK(SolersMention::parse("@file:res://does-not-need-to-exist-for-format.tscn").is_empty());
	if (mentions.size() == 1) {
		CHECK(SolersMention::prompt_block(mentions).contains("[Selected Solers context]"));
		CHECK(SolersMention::dedupe_key(mentions[0]) != SolersMention::dedupe_key(file_mention));
	}

	Ref<SolersPluginJob> job;
	job.instantiate();
	if (SolersPlugin *registered = SolersPluginRegistry::get_plugin("synthetic-future")) {
		registered->run_job(job);
	}
	CHECK(plugin->job_ran);

	SolersPluginRegistry::unregister_plugin(plugin);
	memdelete(plugin);
}

TEST_CASE("[SolersToolRegistry] object transactions expose no in-editor native escape") {
	SolersReflectionService reflection_service;
	SolersResourceService resource_service;
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection_service);
	registry.set_resource_service(&resource_service);
	registry.register_default_tools();

	Dictionary native;
	native["scope"] = "native";
	native["operations"] = Array();
	const Dictionary result = registry.call_tool(SNAME("object.transaction"), native);
	CHECK_FALSE(result.get("ok", true));
	const String code = Dictionary(result.get("error", Dictionary())).get("code", String());
	CHECK((code == "TOOL_ARGUMENT_INVALID" || code == "OBJECT_SCOPE_INVALID"));
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

TEST_CASE("[SolersToolRegistry] resource transactions reject stale hashes and return native receipts") {
	const String path = "res://.solers_transaction_contract.tres";
	if (FileAccess::exists(path)) {
		DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(path));
	}
	SolersFileCheckpoint checkpoints;
	SolersPermissionManager permissions;
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
	create["class_name"] = "Gradient";
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

	Dictionary update;
	update["op"] = "update";
	update["path"] = path;
	update["expected_sha256"] = String("0000000000000000000000000000000000000000000000000000000000000000");
	Dictionary properties;
	properties["resource_name"] = "stale-write";
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

	update["expected_sha256"] = sha;
	properties["resource_name"] = "committed-write";
	update["properties"] = properties;
	update_operations[0] = update;
	update_args["operations"] = update_operations;
	context.call_id = "update_resource";
	const Dictionary updated = registry.call_tool_with_context(SNAME("object.transaction"), update_args, context);
	REQUIRE((bool)updated.get("ok", false));
	const Dictionary mutation = Dictionary(updated.get("data", Dictionary())).get("mutation", Dictionary());
	CHECK(mutation.has("session_revision"));
	CHECK_FALSE(mutation.has("authored_revision"));
	const Dictionary receipt = mutation.get("receipt", Dictionary());
	CHECK(Array(receipt.get("resources_before", Array())).size() == 1);
	CHECK(Array(receipt.get("resources_after", Array())).size() == 1);
	CHECK(String(Dictionary(Array(receipt.get("resources_after", Array()))[0]).get("sha256", String())).length() == 64);
	DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(path));
}

TEST_CASE("[SolersScriptService] project.godot cannot use the raw file path") {
	SolersScriptService scripts;
	Dictionary args;
	args["operation"] = "write_file";
	args["path"] = "res://project.godot";
	args["content"] = "[application]\n";
	const Dictionary result = scripts.edit_project(args);
	CHECK_FALSE(result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "EDITOR_OWNED_FILE");
}

TEST_CASE("[SolersScriptService] invalid source never replaces the authoritative file") {
	const String path = "res://solers_script_validation_contract.gd";
	const String fs_path = ProjectSettings::get_singleton()->globalize_path(path);
	if (FileAccess::exists(path)) {
		DirAccess::remove_file_or_error(fs_path);
	}

	SolersScriptService script_service;

	const String valid_source = "extends Node\nfunc value() -> int:\n\treturn 1\n";
	Dictionary write_args;
	write_args["path"] = path;
	write_args["content"] = valid_source;
	Dictionary written = script_service.write_file(write_args);
	REQUIRE((bool)written.get("ok", false));
	CHECK(Dictionary(written.get("data", Dictionary())).get("valid", false));
	const String valid_sha256 = FileAccess::get_sha256(path);

	Dictionary patch_args;
	patch_args["path"] = path;
	patch_args["old_text"] = "\treturn 1";
	patch_args["new_text"] = "\treturn +";
	Dictionary patched = script_service.patch_file(patch_args);
	CHECK_FALSE((bool)patched.get("ok", true));
	CHECK(Dictionary(patched.get("error", Dictionary())).get("code", String()) == "SCRIPT_VALIDATION_FAILED");
	CHECK(FileAccess::get_sha256(path) == valid_sha256);
	CHECK(FileAccess::get_file_as_string(path) == valid_source);

	DirAccess::remove_file_or_error(fs_path);

	const String shader_path = "res://solers_shader_validation_contract.gdshader";
	const String shader_fs_path = ProjectSettings::get_singleton()->globalize_path(shader_path);
	if (FileAccess::exists(shader_path)) {
		DirAccess::remove_file_or_error(shader_fs_path);
	}
	Dictionary shader_args;
	shader_args["operation"] = "create";
	shader_args["path"] = shader_path;
	shader_args["content"] = "shader_type spatial;\nvoid fragment() { ALBEDO = missing_identifier; }\n";
	const Dictionary shader_result = script_service.edit_script(shader_args);
	CHECK_FALSE((bool)shader_result.get("ok", true));
	CHECK_FALSE(FileAccess::exists(shader_path));
}

TEST_CASE("[SolersScriptService] patch matching tolerates whitespace drift and rejects ambiguity") {
	const String path = "res://solers_patch_cascade_contract.gd";
	const String fs_path = ProjectSettings::get_singleton()->globalize_path(path);
	if (FileAccess::exists(path)) {
		DirAccess::remove_file_or_error(fs_path);
	}
	SolersScriptService script_service;
	Dictionary write_args;
	write_args["path"] = path;
	write_args["content"] = String::utf8("extends Node\n\nfunc alpha() -> int:\n\treturn 1\n\nfunc beta() -> int:\n\tprint(\"hello world\")\n\treturn 2\n\nfunc first_stub() -> void:\n\tpass\n\nfunc second_stub() -> void:\n\tpass\n");
	REQUIRE((bool)script_service.write_file(write_args).get("ok", false));

	// Indentation drift: the model sent spaces where the file uses tabs. The
	// match re-anchors on trimmed lines and the replacement is re-indented to
	// the file's actual indentation.
	Dictionary drift;
	drift["path"] = path;
	drift["old_text"] = "    return 1";
	drift["new_text"] = "    return 10";
	const Dictionary drifted = script_service.patch_file(drift);
	REQUIRE((bool)drifted.get("ok", false));
	const Dictionary drift_data = drifted.get("data", Dictionary());
	CHECK(drift_data.get("match_strategy", String()) == "line_trimmed");
	CHECK(String(drift_data.get("context_after", String())).contains("return 10"));
	CHECK(FileAccess::get_file_as_string(path).contains("\treturn 10"));

	// Typographic quotes fold to their ASCII equivalents before comparison.
	Dictionary quotes;
	quotes["path"] = path;
	quotes["old_text"] = String::utf8("\tprint(\xE2\x80\x9Chello world\xE2\x80\x9D)");
	quotes["new_text"] = "\tprint(\"hello, world\")";
	const Dictionary quoted = script_service.patch_file(quotes);
	REQUIRE((bool)quoted.get("ok", false));
	CHECK(FileAccess::get_file_as_string(path).contains("hello, world"));

	// Two stub bodies share the same trimmed key: tolerant matching must ask
	// for more context instead of guessing which one was meant.
	Dictionary ambiguous;
	ambiguous["path"] = path;
	ambiguous["old_text"] = "    pass";
	ambiguous["new_text"] = "    breakpoint";
	const Dictionary ambiguous_result = script_service.patch_file(ambiguous);
	CHECK_FALSE(ambiguous_result.get("ok", true));
	CHECK(Dictionary(ambiguous_result.get("error", Dictionary())).get("code", String()) == "PATCH_TEXT_AMBIGUOUS");

	// An explicit occurrence beyond the exact count fails outright; tolerant
	// tiers never silently retarget a different occurrence.
	Dictionary occurrence;
	occurrence["path"] = path;
	occurrence["old_text"] = "pass";
	occurrence["new_text"] = "breakpoint";
	occurrence["occurrence"] = 3;
	const Dictionary occurrence_result = script_service.patch_file(occurrence);
	CHECK_FALSE(occurrence_result.get("ok", true));
	CHECK(Dictionary(occurrence_result.get("error", Dictionary())).get("code", String()) == "PATCH_TEXT_NOT_FOUND");
	CHECK(FileAccess::get_file_as_string(path).contains("pass"));

	// A genuine miss returns the real bytes around the nearest anchor line so
	// the next attempt can copy them exactly.
	Dictionary missing;
	missing["path"] = path;
	missing["old_text"] = "func beta() -> int:\n\treturn 999";
	missing["new_text"] = "func beta() -> int:\n\treturn 3";
	const Dictionary missing_result = script_service.patch_file(missing);
	CHECK_FALSE(missing_result.get("ok", true));
	CHECK(Dictionary(missing_result.get("error", Dictionary())).get("code", String()) == "PATCH_TEXT_NOT_FOUND");
	CHECK(String(Dictionary(missing_result.get("data", Dictionary())).get("closest_context", String())).contains("return 2"));

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

TEST_CASE("[SolersScriptService] isolated compute commits only verified declared resources") {
	const String path = "res://.solers_compute_contract.tres";
	const String absolute = ProjectSettings::get_singleton()->globalize_path(path);
	if (FileAccess::exists(path)) {
		DirAccess::remove_file_or_error(absolute);
	}

	Dictionary output;
	output["from"] = "generated.tres";
	output["to"] = path;
	output["resource_type"] = "Resource";
	Array outputs;
	outputs.push_back(output);
	Dictionary args;
	args["source"] = "extends SceneTree\nfunc _init():\n\tvar value := Resource.new()\n\tvalue.resource_name = \"isolated\"\n\tif ResourceSaver.save(value, \"res://generated.tres\") != OK:\n\t\tquit(2)\n\t\treturn\n\tquit()\n";
	args["outputs"] = outputs;

	SolersScriptService service;
	REQUIRE(service.compute_script("compute-contract", args).get("ok", false));
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
	while (!service.compute_script_ready("compute-contract") && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(10000);
	}
	REQUIRE(service.compute_script_ready("compute-contract"));
	const Dictionary result = service.compute_script_finalize("compute-contract");
	REQUIRE(result.get("ok", false));
	const Array committed = Dictionary(result.get("data", Dictionary())).get("outputs", Array());
	REQUIRE(committed.size() == 1);
	CHECK_FALSE(String(Dictionary(committed[0]).get("sha256", String())).is_empty());
	const Ref<Resource> loaded = ResourceLoader::load(path, "Resource", ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP);
	REQUIRE(loaded.is_valid());
	CHECK(loaded->get_name() == "isolated");
	DirAccess::remove_file_or_error(absolute);
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

TEST_CASE("[SolersToolRegistry] tool.search is absent without external deferred tools") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	CHECK(find_tool_def(registry.list_tools(), "tool.search").is_empty());
	const Dictionary result = search_deferred_tools(registry, "anything", 10);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "TOOL_NOT_FOUND");
}

TEST_CASE("[SolersToolRegistry] external search prioritizes exact ids and never returns direct tools") {
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
				p_name, p_description, Dictionary(), SolersToolExposure::DEFERRED, cap,
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
			Dictionary(), SolersToolExposure::DEFERRED, cap,
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
}

TEST_CASE("[SolersReflectionService] batch refuses mutations without an undo history") {
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
		CHECK(batch_error.get("code", String()) == "UNDO_REDO_UNAVAILABLE");
		CHECK_FALSE(result.has("data"));
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
	CHECK(batch_error.get("code", String()) == "SCENE_EDIT_FAILED");
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
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "SCENE_EDIT_FAILED");
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

TEST_CASE("[SolersToolRegistry] scene object transaction access follows its native scope") {
	SolersReflectionService reflection_service;
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection_service);
	registry.register_default_tools();

	Dictionary write_op;
	write_op["op"] = "set_property";
	write_op["node_path"] = "ReferenceCamera";
	write_op["property"] = "fov";
	write_op["value"] = 55.0;
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

TEST_CASE("[SolersObservationService] empty file search lists bounded project files") {
	SolersObservationService observation_service;
	Dictionary result = observation_service.list_project_files(4);
	CHECK(result.has("files"));
	CHECK((int)result.get("count", -1) >= 0);
}

TEST_CASE("[SolersObservationService] observe_path digests any selection by engine authority") {
	// Contract: directory and ordinary file both get a digest.kind — not only PackedScene.
	const String dir_path = "res://solers_observe_path_contract";
	const String file_path = dir_path.path_join("note.txt");
	{
		Ref<DirAccess> root = DirAccess::open("res://");
		REQUIRE(root.is_valid());
		CHECK(root->make_dir("solers_observe_path_contract") == OK);
		Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string("observe-path-contract\n");
	}
	SolersObservationService observation_service;
	const Dictionary dir_observed = observation_service.observe_path(dir_path + "/");
	REQUIRE((bool)dir_observed.get("ok", false));
	const Dictionary dir_digest = dir_observed.get("digest", Dictionary());
	CHECK(dir_digest.get("kind", String()) == "directory");
	CHECK((int)dir_digest.get("file_count", 0) >= 1);
	// EFS-backed digests project import_valid/dependency_count; DirAccess fallback may omit them.
	const Array children = dir_digest.get("children", Array());
	REQUIRE(children.size() >= 1);

	const Dictionary file_observed = observation_service.observe_path(file_path);
	REQUIRE((bool)file_observed.get("ok", false));
	CHECK_FALSE(String(Dictionary(file_observed.get("digest", Dictionary())).get("kind", String())).is_empty());

	Ref<DirAccess> cleanup = DirAccess::open(dir_path);
	REQUIRE(cleanup.is_valid());
	CHECK(cleanup->remove("note.txt") == OK);
	Ref<DirAccess> root = DirAccess::open("res://");
	REQUIRE(root.is_valid());
	CHECK(root->remove("solers_observe_path_contract") == OK);
}

TEST_CASE("[SolersObservationService] structured search and runtime observations stay bounded") {
	const String path = "res://solers_search_contract.txt";
	{
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string("solers-structured-search-contract\n");
	}
	SolersObservationService observation_service;
	Dictionary search_args;
	search_args["type"] = "path";
	search_args["query"] = "solers_search_contract";
	search_args["max_results"] = 4;
	const Dictionary search = observation_service.search_project(search_args);
	CHECK(search.get("type", String()) == "path");
	CHECK((int)search.get("count", 0) >= 1);
	CHECK((int)search.get("count", 0) <= 4);

	Dictionary observe_args;
	observe_args["since_cursor"] = 0;
	const Dictionary runtime = observation_service.observe_runtime(observe_args);
	CHECK(runtime.has("cursor"));
	CHECK(runtime.has("runtime_epoch"));
	CHECK(runtime.has("error_digest"));
	CHECK(runtime.has("epoch_error_count"));
	CHECK(Array(runtime.get("events", Array())).is_empty());

	Dictionary include_events_args;
	include_events_args["include_events"] = true;
	include_events_args["max_events"] = 2;
	const Dictionary with_events = observation_service.observe_runtime(include_events_args);
	CHECK(Array(with_events.get("events", Array())).size() <= 2);
	Ref<DirAccess> project_root = DirAccess::open("res://");
	REQUIRE(project_root.is_valid());
	CHECK(project_root->remove(path.get_file()) == OK);
}

TEST_CASE("[SolersObservationService] runtime capture waits for visual readiness") {
	SolersObservationService observation_service;
	Dictionary args;
	args["target"] = "runtime";
	const Dictionary pending = observation_service.capture_viewport(args);
	REQUIRE((bool)pending.get("ok", false));
	const Dictionary data = pending.get("data", Dictionary());
	CHECK(data.get("status", String()) == "pending");
	CHECK((int64_t)data.get("render_sequence", 0) > 0);
	CHECK(Dictionary(data.get("source_state", Dictionary())).has("runtime_epoch"));
	CHECK((bool)data.get("awaiting_runtime_ready", false));
	const Dictionary poll_args = data.get("poll_args", Dictionary());
	CHECK_FALSE(observation_service.is_viewport_capture_ready(poll_args));
}

TEST_CASE("[SolersObservationService] runtime status exposes a single capture_ready authority") {
	// capture_ready is the one fact render.capture(runtime) and context share —
	// not a second guess from ScriptEditorDebugger alone.
	SolersObservationService observation_service;
	const Dictionary status = observation_service.get_runtime_status();
	CHECK(status.has("is_playing"));
	CHECK(status.has("debugger_connected"));
	CHECK(status.has("capture_ready"));
	CHECK(status.get("capture_ready", true).get_type() == Variant::BOOL);
	// With no play session, capture must not claim readiness.
	CHECK_FALSE((bool)status.get("capture_ready", true));
}

TEST_CASE("[SolersToolRegistry] scene.open is a thin EditorInterface open surface") {
	SolersReflectionService reflection_service;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_SCENE, true);
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection_service);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	const Dictionary tool = find_tool_def(registry.list_tools(), "scene.open");
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

TEST_CASE("[SolersToolRegistry] scene.reload is a thin EditorNode reload surface") {
	SolersReflectionService reflection_service;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_reflection_service(&reflection_service);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();

	const Dictionary tool = find_tool_def(registry.list_tools(), "scene.reload");
	REQUIRE_FALSE(tool.is_empty());
	CHECK(tool.get("permission", String()) == "observe");
	CHECK(tool.get("mutation_policy", String()) == "irreversible");
	CHECK(tool.get("exposure", String()) == "direct");
	CHECK(String(tool.get("description", String())).contains("EditorNode::reload_scene"));
	CHECK(String(tool.get("description", String())).contains("@tool"));
	CHECK_FALSE(registry.affects_scene_state(SNAME("scene.reload")));
	const Dictionary schema = tool.get("input_schema", Dictionary());
	CHECK(Dictionary(schema.get("properties", Dictionary())).has("path"));
	CHECK_FALSE(schema.has("required"));

	Dictionary args;
	const Dictionary result = registry.call_tool(SNAME("scene.reload"), args);
	CHECK_FALSE((bool)result.get("ok", true));
	const String code = Dictionary(result.get("error", Dictionary())).get("code", String());
	CHECK((code == "NO_EDITED_SCENE" || code == "SCENE_PATH_REQUIRED" || code == "SCENE_NOT_OPEN"));
	CHECK((bool)Dictionary(result.get("error", Dictionary())).get("recoverable", false));

	Dictionary missing;
	missing["path"] = "res://definitely_not_open_in_editor_solers_contract.tscn";
	const Dictionary missing_result = registry.call_tool(SNAME("scene.reload"), missing);
	CHECK_FALSE((bool)missing_result.get("ok", true));
	const String missing_code = Dictionary(missing_result.get("error", Dictionary())).get("code", String());
	CHECK((missing_code == "SCENE_NOT_OPEN" || missing_code == "NO_EDITED_SCENE"));
}

TEST_CASE("[SolersScriptService] script.edit reports whether the path is the edited scene root script") {
	const String path = "res://solers_root_script_fact_contract.gd";
	const String fs_path = ProjectSettings::get_singleton()->globalize_path(path);
	if (FileAccess::exists(path)) {
		DirAccess::remove_file_or_error(fs_path);
	}
	SolersScriptService script_service;
	Dictionary args;
	args["path"] = path;
	args["content"] = "extends Node\nfunc _ready() -> void:\n\tpass\n";
	const Dictionary written = script_service.write_file(args);
	REQUIRE((bool)written.get("ok", false));
	const Dictionary data = written.get("data", Dictionary());
	CHECK(data.has("path"));
	CHECK(data.has("sha256"));
	CHECK(data.has("affects_edited_scene_root_script"));
	CHECK_FALSE((bool)data.get("affects_edited_scene_root_script", true));
	DirAccess::remove_file_or_error(fs_path);
}

TEST_CASE("[SolersToolRegistry] project.search rejects incomplete requests before execution") {
	SolersObservationService observation_service;
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_observation_service(&observation_service);
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	Dictionary args;
	args["type"] = "text";
	const Dictionary result = registry.call_tool(SNAME("project.search"), args);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "TOOL_ARGUMENT_INVALID");
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

TEST_CASE("[SolersAssetService][SceneTree] direct texture-set import follows requested manifest roles") {
	const String asset_id = ".solers_map_role_import_contract";
	const String asset_dir = "user://solers_jobs/" + asset_id;
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
	manifest["target_dir"] = target_dir;
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
	Array selected_roles;
	selected_roles.push_back("surface_color");
	selected_roles.push_back("surface_normal");
	args["map_types"] = selected_roles;
	const Dictionary result = asset_service.start_project_import(args);
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

TEST_CASE("[SolersPluginMeshy] enhancement options require standard meshy-6 pipeline") {
	SolersPluginMeshy meshy;
	Dictionary attachment;
	attachment["id"] = "img_contract";
	Array source_attachments;
	source_attachments.push_back(attachment);

	// The manifest is an in/out parameter, so it has to be an lvalue the call
	// can write back into.
	auto prepare = [&](const Dictionary &p_options, bool p_with_image) {
		Dictionary manifest;
		manifest["provider_options"] = p_options.duplicate(true);
		manifest["source_attachments"] = p_with_image ? source_attachments : Array();
		return meshy.prepare_generate("3d", Dictionary(), manifest);
	};

	Dictionary smart_options;
	smart_options["model_type"] = "smart-topology";
	smart_options["ai_model"] = "meshy-t2";
	smart_options["image_enhancement"] = true;
	Dictionary rejected = prepare(smart_options, true);
	CHECK_FALSE(rejected.is_empty());
	CHECK(rejected.get("code", String()) == "INVALID_ARGUMENT");
	CHECK(String(rejected.get("message", String())).contains("image_enhancement"));

	smart_options.erase("image_enhancement");
	smart_options["remove_lighting"] = true;
	rejected = prepare(smart_options, true);
	CHECK_FALSE(rejected.is_empty());
	CHECK(String(rejected.get("message", String())).contains("remove_lighting"));

	smart_options.erase("remove_lighting");
	smart_options["hd_texture"] = true;
	rejected = prepare(smart_options, true);
	CHECK_FALSE(rejected.is_empty());
	CHECK(String(rejected.get("message", String())).contains("hd_texture"));

	smart_options.erase("hd_texture");
	smart_options["topology"] = "quad";
	rejected = prepare(smart_options, true);
	CHECK_FALSE(rejected.is_empty());
	CHECK(String(rejected.get("message", String())).contains("triangle-only"));

	smart_options.erase("topology");
	CHECK(prepare(smart_options, true).is_empty());

	Dictionary meshy5_options;
	meshy5_options["model_type"] = "standard";
	meshy5_options["ai_model"] = "meshy-5";
	meshy5_options["hd_texture"] = true;
	rejected = prepare(meshy5_options, false);
	CHECK_FALSE(rejected.is_empty());
	CHECK(String(rejected.get("message", String())).contains("hd_texture"));

	Dictionary hero_options;
	hero_options["model_type"] = "standard";
	hero_options["ai_model"] = "meshy-6";
	hero_options["image_enhancement"] = true;
	hero_options["remove_lighting"] = true;
	hero_options["hd_texture"] = true;
	CHECK(prepare(hero_options, false).is_empty());

	const Dictionary schema = meshy.get_generation_options_schema("3d");
	CHECK(String(Dictionary(schema.get("model_type", Dictionary())).get("description", String())).contains("standard"));
	CHECK(String(Dictionary(schema.get("hd_texture", Dictionary())).get("description", String())).contains("meshy-6"));
	CHECK(String(Dictionary(schema.get("image_enhancement", Dictionary())).get("description", String())).contains("meshy-6"));
	CHECK(String(Dictionary(schema.get("remove_lighting", Dictionary())).get("description", String())).contains("meshy-6"));
}

TEST_CASE("[SolersPluginMeshy] offline operation contracts") {
	SolersPluginMeshy meshy;
	const Array operations = meshy.get_operation_defs();
	const Dictionary convert = find_operation_def(operations, "convert");
	const Dictionary resize = find_operation_def(operations, "resize");
	const Dictionary uv_unwrap = find_operation_def(operations, "uv_unwrap");
	REQUIRE_FALSE(convert.is_empty());
	REQUIRE_FALSE(resize.is_empty());
	REQUIRE_FALSE(uv_unwrap.is_empty());
	CHECK(convert.get("endpoint", String()) == "/openapi/v1/convert");
	CHECK(resize.get("endpoint", String()) == "/openapi/v1/resize");
	CHECK(uv_unwrap.get("endpoint", String()) == "/openapi/v1/uv-unwrap");
	CHECK(Array(Dictionary(convert.get("options_schema", Dictionary())).get("required", Array())).has("target_formats"));

	Dictionary source;
	source["provider_task_id"] = "meshy-source-task";
	source["polycount"] = 40000;

	Dictionary convert_options;
	convert_options["input_task_id"] = "unrelated-task";
	Array convert_formats;
	convert_formats.push_back("STL");
	convert_options["target_formats"] = convert_formats;
	Dictionary result = meshy.prepare_operation(convert, source, convert_options);
	CHECK(result.is_empty());
	CHECK(convert_options.get("input_task_id", String()) == "meshy-source-task");
	const Array normalized_formats = convert_options.get("target_formats", Array());
	CHECK(normalized_formats.size() == 1);
	CHECK(String(normalized_formats[0]) == "stl");

	Dictionary resize_options;
	resize_options["resize_height"] = 1.8;
	result = meshy.prepare_operation(resize, source, resize_options);
	CHECK(result.is_empty());
	CHECK(resize_options.get("input_task_id", String()) == "meshy-source-task");

	Dictionary conflicting_resize_options;
	conflicting_resize_options["resize_height"] = 1.8;
	conflicting_resize_options["resize_longest_side"] = 2.0;
	result = meshy.prepare_operation(resize, source, conflicting_resize_options);
	CHECK_FALSE(result.is_empty());
	CHECK(result.get("code", String()) == "INVALID_ARGUMENT");
	CHECK(String(result.get("message", String())).contains("exactly one"));

	Dictionary false_auto_size_options;
	false_auto_size_options["auto_size"] = false;
	result = meshy.prepare_operation(resize, source, false_auto_size_options);
	CHECK_FALSE(result.is_empty());
	CHECK(result.get("code", String()) == "INVALID_ARGUMENT");

	Dictionary oversized_source = source.duplicate(true);
	oversized_source["polycount"] = 40001;
	Dictionary uv_options;
	result = meshy.prepare_operation(uv_unwrap, oversized_source, uv_options);
	CHECK_FALSE(result.is_empty());
	CHECK(result.get("code", String()) == "UV_UNWRAP_FACE_LIMIT");

	uv_options.clear();
	result = meshy.prepare_operation(uv_unwrap, source, uv_options);
	CHECK(result.is_empty());
	CHECK(uv_options.get("input_task_id", String()) == "meshy-source-task");
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

TEST_CASE("[SolersAssetService] non-terminal asset.status rejects progress polling; job.wait declares host park") {
	const String asset_id = ".solers_background_wait_contract";
	const String asset_dir = "user://solers_jobs/" + asset_id;
	const String manifest_path = asset_dir.path_join("manifest.json");
	auto remove_job = [&]() {
		if (FileAccess::exists(manifest_path)) {
			DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(manifest_path));
		}
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(asset_dir));
	};
	remove_job();
	REQUIRE(DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(asset_dir)) == OK);

	// Construction adopts unfinished jobs and marks the ones no plugin can
	// resume as interrupted, so the service has to already exist before a
	// running manifest lands or recovery makes it terminal behind our back.
	SolersAssetService assets;

	Dictionary running_manifest;
	running_manifest["id"] = asset_id;
	running_manifest["session_id"] = "contract-session";
	running_manifest["kind"] = "3d";
	running_manifest["status"] = "running";
	running_manifest["stage"] = "generating";
	running_manifest["progress"] = 1.0;
	String write_error;
	REQUIRE(SolersPlugin::write_json_atomic(manifest_path, running_manifest, write_error));

	Dictionary status_args;
	status_args["asset_id"] = asset_id;
	const Dictionary running_status = assets.status(status_args);
	CHECK_FALSE((bool)running_status.get("ok", true));
	CHECK(Dictionary(running_status.get("error", Dictionary())).get("code", String()) == "ASSET_NOT_READY");
	CHECK(String(Dictionary(running_status.get("error", Dictionary())).get("message", String())).contains("job.wait"));
	CHECK_FALSE(String(Dictionary(running_status.get("error", Dictionary())).get("message", String())).contains("Inspect it with asset.status"));

	Dictionary wait_args;
	Array ids;
	ids.push_back(asset_id);
	wait_args["ids"] = ids;
	const Dictionary waiting = assets.wait_jobs(wait_args, "contract-session");
	CHECK((bool)waiting.get("ok", false));
	const Dictionary waiting_data = waiting.get("data", Dictionary());
	CHECK((bool)waiting_data.get("waiting", false));
	CHECK((bool)waiting_data.get("host_parked", false));
	CHECK(String(waiting_data.get("next_step", String())).contains("background job delta"));
	CHECK(Array(waiting_data.get("pending_ids", Array())).size() == 1);

	running_manifest["status"] = "imported";
	running_manifest["stage"] = "imported";
	REQUIRE(SolersPlugin::write_json_atomic(manifest_path, running_manifest, write_error));
	const Dictionary terminal_status = assets.status(status_args);
	CHECK((bool)terminal_status.get("ok", false));
	CHECK(Dictionary(terminal_status.get("data", Dictionary())).get("status", String()) == "imported");

	const Dictionary terminal_wait = assets.wait_jobs(wait_args, "contract-session");
	CHECK((bool)terminal_wait.get("ok", false));
	const Dictionary terminal_data = terminal_wait.get("data", Dictionary());
	CHECK_FALSE((bool)terminal_data.get("waiting", true));
	CHECK(Array(terminal_data.get("terminal", Array())).size() == 1);
	CHECK_FALSE(terminal_data.has("host_parked"));

	remove_job();
}

TEST_CASE("[SolersAssetService][SceneTree] baked_static imports use Godot native lightmap import UV2") {
	EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
	if (!filesystem) {
		WARN("EditorFileSystem is initialized after the command-line unit test runner; run this contract in an editor integration test.");
		return;
	}
	const String asset_id = ".solers_static_lightmap_import_contract";
	const String asset_dir = "user://solers_jobs/" + asset_id;
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
	manifest["target_dir"] = target_dir;
	Dictionary import_options;
	import_options["import_profile"] = "baked_static";
	manifest["import_options"] = import_options;
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
	args["import_profile"] = "baked_static";
	Dictionary result = asset_service.start_project_import(args);
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
	asset_service.poll();
	Dictionary status_args;
	status_args["asset_id"] = asset_id;
	const String sidecar_path = Dictionary(Dictionary(asset_service.status(status_args)).get("data", Dictionary())).get("sidecar_file", String());

	remove_if_exists(target_path);
	remove_if_exists(target_path + ".import");
	remove_if_exists(target_path + ".unwrap_cache");
	remove_if_exists(sidecar_path);
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
	const String asset_dir = "user://solers_jobs/" + asset_id;
	const String second_asset_dir = "user://solers_jobs/" + second_asset_id;
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
	manifest["provider"] = "synthetic-origin";
	manifest["kind"] = "material";
	manifest["name"] = "Material Import Contract";
	manifest["prompt"] = "Synthetic provenance contract";
	manifest["status"] = "ready";
	manifest["target_dir"] = target_dir;
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
	second_manifest["target_dir"] = second_target_dir;
	second_manifest["files"] = second_files;
	Array no_entrypoints;
	second_manifest["entrypoints"] = no_entrypoints;
	write_text(second_asset_dir.path_join("manifest.json"), JSON::stringify(second_manifest));

	SolersAssetService asset_service;
	Dictionary args;
	args["asset_id"] = asset_id;
	args["target_dir"] = target_dir;
	Dictionary result = asset_service.start_project_import(args);
	REQUIRE(result.get("ok", false));
	Dictionary data = result.get("data", Dictionary());
	REQUIRE(data.get("status", String()) == "pending");
	Dictionary second_args;
	second_args["asset_id"] = second_asset_id;
	second_args["target_dir"] = second_target_dir;
	Dictionary second_result = asset_service.start_project_import(second_args);
	REQUIRE(second_result.get("ok", false));
	Dictionary second_data = second_result.get("data", Dictionary());
	REQUIRE(second_data.get("status", String()) == "pending");
	const Dictionary staged_imports = asset_service.get_project_import_coordinator_state();
	CHECK_FALSE(staged_imports.get("wave_active", true));
	CHECK((int)staged_imports.get("queued_count", 0) == 2);
	const Dictionary first_poll_args = data.get("poll_args", Dictionary());
	Dictionary duplicate_pending = asset_service.start_project_import(args);
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
	asset_service.poll();
	Dictionary status_args;
	status_args["asset_id"] = asset_id;
	const Dictionary imported_status = asset_service.status(status_args);
	REQUIRE(imported_status.get("ok", false));
	const Dictionary imported_manifest = imported_status.get("data", Dictionary());
	CHECK(imported_manifest.get("status", String()) == "imported");
	const String sidecar_path = imported_manifest.get("sidecar_file", String());
	REQUIRE(FileAccess::exists(sidecar_path));
	const Dictionary sidecar = SolersPlugin::read_json_file(sidecar_path);
	CHECK((int)sidecar.get("schema_version", 0) == 1);
	CHECK(sidecar.get("job_id", String()) == asset_id);
	CHECK(sidecar.get("plugin", String()) == "synthetic-origin");
	CHECK(sidecar.get("target_dir", String()) == target_dir);
	status_args["asset_id"] = second_asset_id;
	const Dictionary second_imported_status = asset_service.status(status_args);
	const String second_sidecar_path = Dictionary(second_imported_status.get("data", Dictionary())).get("sidecar_file", String());
	CHECK(FileAccess::exists(second_sidecar_path));

	const uint64_t imported_mtime = FileAccess::get_modified_time(target_dir.path_join("surface.png"));
	SolersAssetService restarted_service;
	Dictionary repeated = restarted_service.start_project_import(args);
	REQUIRE(repeated.get("ok", false));
	CHECK(String(Dictionary(repeated.get("data", Dictionary())).get("status", String())) != "pending");
	CHECK(FileAccess::get_modified_time(target_dir.path_join("surface.png")) == imported_mtime);

	Array cleanup_files;
	cleanup_files.push_back(target_dir.path_join("material.tres"));
	cleanup_files.push_back(target_dir.path_join("surface.png"));
	cleanup_files.push_back(target_dir.path_join("surface.png.import"));
	cleanup_files.push_back(target_dir.path_join("unused.tres"));
	cleanup_files.push_back(sidecar_path);
	cleanup_files.push_back(second_sidecar_path);
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
	const String asset_dir = "user://solers_jobs/" + asset_id;
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
	manifest["target_dir"] = target_dir;
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
	const Dictionary result = asset_service.start_project_import(args);
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
	args["member_query"] = "surface_set_material";
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

TEST_CASE("[SolersReflectionService] lean introspect returns member_names without typed members") {
	SolersReflectionService reflection_service;
	Dictionary lean_args;
	lean_args["class_name"] = "Node3D";
	lean_args["include_inherited"] = false;
	lean_args["max_members"] = 1;
	const Dictionary lean = reflection_service.introspect_class(lean_args);
	REQUIRE(lean.get("ok", false));
	const Dictionary lean_data = lean.get("data", Dictionary());
	REQUIRE(PackedStringArray(lean_data.get("member_names", PackedStringArray())).size() == 1);
	CHECK((int)lean_data.get("member_count", 0) > 0);
	CHECK(lean_data.get("truncated", false));
	REQUIRE(lean_data.has("next_cursor"));
	Dictionary next_args = lean_args.duplicate();
	next_args["cursor"] = lean_data["next_cursor"];
	const Dictionary next = reflection_service.introspect_class(next_args);
	REQUIRE(next.get("ok", false));
	CHECK(PackedStringArray(Dictionary(next.get("data", Dictionary())).get("member_names", PackedStringArray())).size() == 1);
	CHECK_FALSE(lean_data.has("methods"));
	CHECK_FALSE(lean_data.has("properties"));

	Dictionary expand_args = lean_args;
	expand_args["member_query"] = "position";
	expand_args["max_members"] = 64;
	const Dictionary expanded = reflection_service.introspect_class(expand_args);
	REQUIRE(expanded.get("ok", false));
	const Dictionary expand_data = expanded.get("data", Dictionary());
	CHECK(expand_data.has("properties"));
	CHECK(Array(expand_data.get("properties", Array())).size() >= 1);
}

TEST_CASE("[SolersToolRegistry] object.query defaults lean and expands on flag") {
	SolersPermissionManager permissions;
	permissions.set_auto_approve_all(true);
	SolersObservationService observation;
	SolersReflectionService reflection_service;
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.set_observation_service(&observation);
	registry.set_reflection_service(&reflection_service);
	registry.register_default_tools();

	Dictionary lean_args;
	lean_args["target"] = "scene";
	const Dictionary lean = registry.call_tool(SNAME("object.query"), lean_args);
	REQUIRE_MESSAGE((bool)lean.get("ok", false), String(Dictionary(lean.get("error", Dictionary())).get("message", String())));
	const Dictionary lean_data = lean.get("data", Dictionary());
	CHECK(lean_data.has("state"));
	CHECK_FALSE(lean_data.has("scene_tree"));
	CHECK_FALSE(lean_data.has("selection"));

	// Expand contract: caller-facing flags remain on the scene branch schema.
	// (Headless suites may leave a non-owned edited-scene pointer; do not
	// serialize the live tree here — that path is covered by editor dogfood.)
	const Dictionary query_def = find_tool_def(registry.list_tools(), "object.query");
	REQUIRE_FALSE(query_def.is_empty());
	const Array branches = Dictionary(query_def.get("input_schema", Dictionary())).get("oneOf", Array());
	REQUIRE(branches.size() >= 1);
	const Dictionary scene_props = Dictionary(branches[0]).get("properties", Dictionary());
	CHECK(scene_props.has("include_tree"));
	CHECK(scene_props.has("include_selection"));
	CHECK(scene_props.has("node_paths"));
}

TEST_CASE("[SolersReflectionService] member_query reports unmatched tokens from ClassDB") {
	// OR matching must not silently drop a token that exists on no member of the class.
	SolersReflectionService reflection_service;
	Dictionary args;
	args["class_name"] = "ProceduralSkyMaterial";
	args["include_inherited"] = false;
	args["member_query"] = "sun_angle_max sun_disk_scale";
	const Dictionary result = reflection_service.introspect_class(args);
	REQUIRE(result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	const Array unmatched = data.get("unmatched_member_query_tokens", Array());
	REQUIRE(unmatched.size() >= 1);
	bool saw_sun_disk = false;
	for (int i = 0; i < unmatched.size(); i++) {
		const Dictionary entry = unmatched[i];
		if (String(entry.get("token", String())) != "sun_disk_scale") {
			continue;
		}
		saw_sun_disk = true;
		CHECK(Array(entry.get("nearest_members", Array())).size() >= 0);
		const PackedStringArray siblings = entry.get("classes_with_member", PackedStringArray());
		CHECK(siblings.has("PhysicalSkyMaterial"));
	}
	CHECK(saw_sun_disk);
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

// One assistant tool_call, in the shape the session records it.
static Array solers_test_tool_calls(const String &p_id, const String &p_name) {
	Dictionary call;
	call["id"] = p_id;
	call["name"] = p_name;
	call["arguments"] = "{}";
	Array calls;
	calls.push_back(call);
	return calls;
}

TEST_CASE("[SolersContextManager] pairing repair answers tool calls a killed turn abandoned") {
	// An aborted or crashed turn leaves assistant tool_calls with no results in
	// durable history; without repair every later request is rejected forever.
	Array messages;
	messages.push_back(SolersLLMMessage::user("Add a light."));
	messages.push_back(SolersLLMMessage::assistant("Editing.", solers_test_tool_calls("call_killed", "object.transaction")));
	messages.push_back(SolersLLMMessage::user("Stop, do it differently."));

	const Array repaired = SolersContextManager::repair_tool_pairing(messages);
	REQUIRE(repaired.size() == 4);
	const Dictionary stub = repaired[2];
	CHECK(String(stub.get("role", String())) == String(SolersLLMRole::TOOL));
	CHECK(String(stub.get("tool_call_id", String())) == "call_killed");
	CHECK(String(stub.get("name", String())) == "object.transaction");
	CHECK(String(stub.get("content", String())).contains("TOOL_CANCELLED"));
	CHECK(String(Dictionary(repaired[3]).get("content", String())) == "Stop, do it differently.");
}

TEST_CASE("[SolersContextManager] pairing repair keeps a complete exchange byte-identical") {
	Array messages;
	messages.push_back(SolersLLMMessage::assistant("Inspecting.", solers_test_tool_calls("call_a", "object.query")));
	messages.push_back(SolersLLMMessage::tool_result("call_a", "object.query", "observation"));

	const Array repaired = SolersContextManager::repair_tool_pairing(messages);
	REQUIRE(repaired.size() == messages.size());
	for (int i = 0; i < messages.size(); i++) {
		CHECK(Dictionary(repaired[i]) == Dictionary(messages[i]));
	}
}

TEST_CASE("[SolersContextManager] pairing repair drops results for calls never made") {
	// A duplicate or unmatched tool result is rejected by providers just as
	// hard as a missing one, so the projection must not forward it.
	Array messages;
	messages.push_back(SolersLLMMessage::assistant("Inspecting.", solers_test_tool_calls("call_a", "object.query")));
	messages.push_back(SolersLLMMessage::tool_result("call_a", "object.query", "observation"));
	messages.push_back(SolersLLMMessage::tool_result("call_a", "object.query", "duplicate"));
	messages.push_back(SolersLLMMessage::tool_result("call_ghost", "object.query", "orphan"));

	const Array repaired = SolersContextManager::repair_tool_pairing(messages);
	REQUIRE(repaired.size() == 2);
	CHECK(String(Dictionary(repaired[1]).get("content", String())) == "observation");
}

TEST_CASE("[SolersContextManager] request projection repairs pairing before budgeting") {
	SolersContextManager context;
	Array messages;
	messages.push_back(SolersLLMMessage::user("Add a light."));
	messages.push_back(SolersLLMMessage::assistant("Editing.", solers_test_tool_calls("call_killed", "object.transaction")));

	const Array projected = context.prepare_request(messages, String(), Array());
	REQUIRE(projected.size() == 3);
	CHECK(String(Dictionary(projected[2]).get("tool_call_id", String())) == "call_killed");
	CHECK(messages.size() == 2); // durable history stays append-only
}

TEST_CASE("[SolersContextManager] estimates ASCII and non-ASCII text like Kimi Code") {
	CHECK(SolersContextManager::estimate_tokens("abcd") == 1);
	CHECK(SolersContextManager::estimate_tokens("abcde") == 2);
	CHECK(SolersContextManager::estimate_tokens(String::utf8("白盒")) == 2);
	CHECK(SolersContextManager::estimate_tokens(String::utf8("ab白")) == 2);
}

TEST_CASE("[SolersContextManager] compaction reserves the provider output contract") {
	SolersContextManager context;

	CHECK_FALSE(context.should_compact(167999, 200000, 32000));
	CHECK(context.should_compact(168000, 200000, 32000));
	CHECK(context.should_compact(150000, 200000, 50000));
	CHECK_FALSE(context.should_compact(149999, 200000, 50000));
	CHECK_FALSE(context.should_compact(8000, 500000, 32000));
	CHECK(context.should_compact(468000, 500000, 32000));
	CHECK_FALSE(context.should_compact(30000, 0, 8192));
	CHECK_FALSE(context.should_compact(99071, 131072, 32000));
	CHECK(context.should_compact(99072, 131072, 32000));
	CHECK_FALSE(context.should_compact(350000, 1050000, 32000));
}

TEST_CASE("[SolersContextManager] compaction requires context growth after the last compaction") {
	SolersContextManager context;
	Array messages;
	messages.push_back(SolersLLMMessage::user("Keep the room proportions realistic."));
	const Dictionary result = context.apply_compaction(messages, "Continue from the verified room shell.", Dictionary());
	const int tokens_after = result.get("tokens_after", 0);
	REQUIRE(tokens_after > 0);
	CHECK_FALSE(context.should_compact(tokens_after, tokens_after * 2, tokens_after));
	CHECK(context.should_compact(tokens_after + 1, tokens_after * 2, tokens_after));
}

TEST_CASE("[SolersContextManager] completed turns drop tool bodies and legacy checkpoints") {
	const String fat = String("x").repeat(40000);
	Array messages;
	messages.push_back(SolersLLMMessage::user("Inspect the scene."));
	messages.push_back(SolersLLMMessage::assistant("Inspecting.", solers_test_tool_calls("call_old", "object.query")));
	messages.push_back(SolersLLMMessage::tool_result("call_old", "object.query", fat));
	messages.push_back(SolersLLMMessage::assistant("The scene has three lights.", Array()));
	Dictionary checkpoint = SolersLLMMessage::user("Runtime checkpoint: revision 3");
	checkpoint["origin"] = "turn_checkpoint";
	messages.push_back(checkpoint);
	Dictionary transient = SolersLLMMessage::user("stale engine state");
	transient["ephemeral"] = true;
	messages.push_back(transient);

	const Array projected = SolersContextManager::project_completed_turns(messages);
	REQUIRE(projected.size() == 2);
	CHECK(Dictionary(projected[0]).get("content", String()) == "Inspect the scene.");
	CHECK(Dictionary(projected[1]).get("content", String()) == "The scene has three lights.");
	CHECK(SolersContextManager::estimate_messages_tokens(projected) * 10 < SolersContextManager::estimate_messages_tokens(messages));
}

TEST_CASE("[SolersContextManager] compaction keeps the latest complete tool exchange") {
	SolersContextManager context;
	Array messages;
	messages.push_back(SolersLLMMessage::user("Build a room from the reference."));
	messages.push_back(SolersLLMMessage::assistant("Inspecting.", solers_test_tool_calls("call_old", "object.query")));
	messages.push_back(SolersLLMMessage::tool_result("call_old", "object.query", String("x").repeat(40000)));
	messages.push_back(SolersLLMMessage::assistant("Capturing.", solers_test_tool_calls("call_latest", "render.capture")));
	messages.push_back(SolersLLMMessage::tool_result("call_latest", "render.capture", "{\"camera\":5}"));

	Array steps;
	Dictionary step;
	step["step"] = "Tune lighting";
	step["status"] = "in_progress";
	steps.push_back(step);
	Dictionary plan;
	plan["plan"] = steps;

	Dictionary result = context.apply_compaction(messages, "Continue from the verified room and camera 5.", plan);
	Array compacted = result.get("messages", Array());
	REQUIRE(compacted.size() == 3);
	Dictionary summary = compacted[0];
	CHECK(summary.get("origin", String()) == "compaction_summary");
	CHECK(String(summary.get("content", String())).begins_with(String::utf8(SolersContextManager::COMPACTION_SUMMARY_PREFIX)));
	CHECK(String(summary.get("content", String())).contains("Tune lighting"));
	CHECK(String(Dictionary(compacted[1]).get("content", String())) == "Capturing.");
	CHECK(String(Dictionary(compacted[2]).get("tool_call_id", String())) == "call_latest");
	CHECK(String(Dictionary(compacted[2]).get("content", String())).contains("camera"));

	Dictionary repeated = context.apply_compaction(compacted, "Continue from the verified whitebox.", plan);
	Array repeated_messages = repeated.get("messages", Array());
	REQUIRE(repeated_messages.size() == 3);
	CHECK(Dictionary(repeated_messages[0]).get("origin", String()) == "compaction_summary");
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
	const Array kept = Dictionary(compacted[1]).get("attachments", Array());
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
	Dictionary first_tool = SolersLLMMessage::tool_result("call_old", "render_capture", "{}");
	first_tool["attachments"] = first_attachments;

	Dictionary latest_capture;
	latest_capture["id"] = "capture_latest";
	latest_capture["source"] = "tool_capture";
	Array latest_attachments;
	latest_attachments.push_back(latest_capture);
	Dictionary latest_tool = SolersLLMMessage::tool_result("call_latest", "render_capture", "{}");
	latest_tool["attachments"] = latest_attachments;

	Array messages;
	messages.push_back(SolersLLMMessage::user("Build and inspect the scene."));
	messages.push_back(SolersLLMMessage::assistant("First capture.", solers_test_tool_calls("call_old", "render.capture")));
	messages.push_back(first_tool);
	messages.push_back(SolersLLMMessage::assistant("Latest capture.", solers_test_tool_calls("call_latest", "render.capture")));
	messages.push_back(latest_tool);
	const Array compacted = Dictionary(context.apply_compaction(messages, "Continue visual verification.", Dictionary())).get("messages", Array());
	REQUIRE(compacted.size() == 3);
	CHECK(Dictionary(compacted[0]).get("origin", String()) == "compaction_summary");
	CHECK(String(Dictionary(compacted[2]).get("tool_call_id", String())) == "call_latest");
	CHECK(Array(Dictionary(compacted[2]).get("attachments", Array())).size() == 1);
}

TEST_CASE("[SolersContextManager] full compaction drops per-request harness state") {
	// solers_state messages are regenerated fresh for every request; keeping a
	// stale copy across a compaction would present outdated engine facts as
	// current.
	SolersContextManager context;
	Dictionary state = SolersLLMMessage::user("revision 2");
	state["origin"] = "solers_state";
	state["ephemeral"] = true;
	Array messages;
	messages.push_back(SolersLLMMessage::user("Build the room."));
	messages.push_back(state);

	const Array compacted = Dictionary(context.apply_compaction(messages, "Continue.", Dictionary())).get("messages", Array());
	REQUIRE(compacted.size() == 1);
	CHECK(Dictionary(compacted[0]).get("origin", String()) == "compaction_summary");
}

TEST_CASE("[SolersAgentSession] transcript restore keeps only the active tool exchange") {
	const String session_id = "restore-contract-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
	const String project_path = "test://solers-restore-contract";
	auto write_event = [&](const Dictionary &p_payload, int p_turn) {
		Dictionary event = p_payload.duplicate(true);
		event["project_path"] = project_path;
		event["session_id"] = session_id;
		event["turn_id"] = p_turn;
		solers_transcript_write(event);
	};
	Dictionary event;
	event["event_type"] = "message";
	event["role"] = SolersLLMRole::USER;
	event["content"] = "completed request";
	write_event(event, 1);
	event["role"] = SolersLLMRole::ASSISTANT;
	event["content"] = String();
	event["tool_calls"] = solers_test_tool_calls("old_call", "object.query");
	write_event(event, 1);
	event.clear();
	event["event_type"] = "tool_result";
	event["call_id"] = "old_call";
	event["tool"] = "object.query";
	event["content"] = "{\"old\":true}";
	write_event(event, 1);
	event.clear();
	event["event_type"] = "turn_outcome";
	event["outcome"] = "completed";
	write_event(event, 1);
	event.clear();
	event["event_type"] = "message";
	event["role"] = SolersLLMRole::USER;
	event["content"] = "active request";
	write_event(event, 2);
	event["role"] = SolersLLMRole::ASSISTANT;
	event["content"] = String();
	event["tool_calls"] = solers_test_tool_calls("active_call", "render.capture");
	write_event(event, 2);
	event.clear();
	event["event_type"] = "tool_result";
	event["call_id"] = "active_call";
	event["tool"] = "render.capture";
	event["content"] = "{\"camera\":\"verified\"}";
	write_event(event, 2);

	SolersAgentSession restored;
	restored.set_session(project_path, session_id);
	const Array messages = restored.get_messages();
	REQUIRE(messages.size() == 4);
	CHECK(String(Dictionary(messages[0]).get("content", String())).contains("completed request"));
	CHECK(Array(Dictionary(messages[2]).get("tool_calls", Array())).size() == 1);
	CHECK(String(Dictionary(messages[3]).get("tool_call_id", String())) == "active_call");
	CHECK(String(Dictionary(messages[3]).get("content", String())).contains("verified"));
	restored.shutdown();
}

TEST_CASE("[SolersAgentSession] a tool result reaches the model as parsable JSON at any size") {
	// The contract is on the shape, not on a size: whatever the engine
	// measured, what the model receives must parse, because an unparsable
	// body throws away every fact in it.
	Dictionary measurements;
	Array nodes;
	for (int i = 0; i < 400; i++) {
		Dictionary node;
		node["path"] = vformat("Root/Prop_%d/Mesh", i);
		node["world_aabb"] = "position=(1.5,0.25,-3) size=(2,2,2)";
		node["note"] = String("padding so this result cannot fit any budget ").repeat(4);
		nodes.push_back(node);
	}
	measurements["nodes"] = nodes;
	Dictionary digest;
	digest["path"] = "res://sample.tscn";
	digest["root_type"] = "Node3D";
	digest["node_count"] = 3;
	measurements["digest"] = digest;
	Dictionary mutation;
	mutation["session_revision"] = 7;
	Dictionary receipt;
	receipt["call_id"] = "call_over_budget";
	receipt["policy"] = "editor_undo";
	mutation["receipt"] = receipt;
	measurements["mutation"] = mutation;
	Dictionary result;
	result["ok"] = true;
	result["data"] = measurements;

	Ref<JSON> parser;
	parser.instantiate();

	const String whole = SolersAgentSession::deliverable_tool_result("call_fits", result, 1000000);
	CHECK(parser->parse(whole) == OK);
	CHECK(Dictionary(parser->get_data()).has("data"));

	const String elided = SolersAgentSession::deliverable_tool_result("call_over_budget", result, 500);
	REQUIRE(parser->parse(elided) == OK);
	const Dictionary envelope = parser->get_data();
	CHECK(envelope.get("ok", false));
	// One generic mutation receipt survives; observation-specific fields do not.
	CHECK(envelope.has("data"));
	CHECK(Dictionary(envelope.get("data", Dictionary())).has("mutation"));
	CHECK_FALSE(Dictionary(envelope.get("data", Dictionary())).has("digest"));
	CHECK_FALSE(Dictionary(envelope.get("data", Dictionary())).has("nodes"));
	const Dictionary reported = envelope.get("data_elided", Dictionary());
	CHECK((int)reported.get("token_budget", 0) == 500);
	CHECK((int)reported.get("tokens", 0) > 500);
	CHECK(String(reported.get("result_sha256", String())).length() == 64);
	CHECK_FALSE(String(reported.get("recovery", String())).is_empty());
	CHECK_FALSE(reported.has("complete_result_path"));

	// A failed call keeps its native cause even when the body is elided: the
	// error is what the next step acts on.
	Dictionary failure;
	failure["ok"] = false;
	Dictionary error;
	error["code"] = "NODE_NOT_FOUND";
	error["message"] = "Root/Missing does not exist.";
	failure["error"] = error;
	failure["data"] = measurements;
	REQUIRE(parser->parse(SolersAgentSession::deliverable_tool_result("call_failed", failure, 500)) == OK);
	const Dictionary failed_envelope = parser->get_data();
	CHECK_FALSE(failed_envelope.get("ok", true));
	CHECK(Dictionary(failed_envelope.get("error", Dictionary())).get("code", String()) == "NODE_NOT_FOUND");
}

TEST_CASE("[SolersResourceService] nearest names rank containment above similarity") {
	PackedStringArray candidates;
	candidates.push_back("get_size()");
	candidates.push_back("size_flags_horizontal");
	candidates.push_back("visible");
	candidates.push_back("scale");
	const PackedStringArray nearest = solers_nearest_names("size", candidates, 3);
	REQUIRE(nearest.size() >= 2);
	CHECK(nearest.has("get_size()"));
	CHECK(nearest.has("size_flags_horizontal"));
	CHECK_FALSE(nearest.has("visible"));
	CHECK(solers_nearest_names("", candidates, 3).is_empty());
}

TEST_CASE("[SolersResourceService] display values summarize bulk variants by type") {
	PackedVector3Array big_points;
	big_points.resize(3654);
	const Variant big_summary = solers_summarize_display_value(big_points);
	REQUIRE(big_summary.get_type() == Variant::STRING);
	CHECK(String(big_summary).contains("PackedVector3Array"));
	CHECK(String(big_summary).contains("3654"));

	PackedVector3Array small_points;
	small_points.resize(3);
	CHECK(solers_summarize_display_value(small_points).get_type() == Variant::PACKED_VECTOR3_ARRAY);

	const String long_text = String("x").repeat(5000);
	const Variant truncated = solers_summarize_display_value(long_text);
	CHECK(String(truncated).length() < 2200);
	CHECK(String(truncated).contains("5000 chars total"));
	CHECK(String(solers_summarize_display_value(String("short"))) == "short");
}

TEST_CASE("[SolersAgentSession] task completion has no Harness request budget") {
	SolersToolRegistry registry;
	SolersAgentSession session;
	session.set_tool_registry(&registry);

	const Array tools = registry.list_tools();
	CHECK_FALSE(find_tool_def(tools, "update_plan").is_empty());
	CHECK(find_tool_def(tools, "done").is_empty());
	const Dictionary status = session.get_status();
	CHECK_FALSE(status.has("tool_iterations"));
	CHECK_FALSE(status.has("max_tool_iterations"));
	CHECK_FALSE(status.has("model_request_budget"));
	CHECK_FALSE(status.has("input_token_budget"));
	CHECK((int)status.get("model_requests", 0) == 0);
	CHECK(status.has("fresh_input_tokens"));
	CHECK(status.has("cache_read_tokens"));
	CHECK(status.has("cache_write_tokens"));
	CHECK(status.has("wire_body_bytes"));
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
	CHECK(SolersAgentSession::validate_plan(args).get("ok", false));
}

TEST_CASE("[SolersToolRegistry] runtime lifecycle declares only runtime access") {
	SolersAssetService assets;
	SolersToolRegistry registry;
	registry.set_asset_service(&assets);
	registry.register_default_tools();

	Dictionary play;
	play["action"] = "play_current_scene";
	const Array runtime_accesses = registry.resolve_resource_access(SNAME("runtime.control"), play);
	REQUIRE(runtime_accesses.size() == 1);
	CHECK(Dictionary(runtime_accesses[0]).get("key", String()) == "runtime:");
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
	const Dictionary catalog_tool = find_tool_def(registry.list_tools(), "asset.catalog.search");
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
		operation["op"] = "set_property";
		operation["node_path"] = ".";
		operation["property"] = "name";
		operation["value"] = "Root";
		operations.push_back(operation);
	}
	Dictionary args;
	args["scope"] = "scene";
	args["operations"] = operations;
	const Dictionary audit = registry.redact_tool_args_for_audit(SNAME("object.transaction"), args);
	CHECK(Array(audit.get("operations", Array())).size() == 40);
}

TEST_CASE("[SolersAssetService] Poly Haven acquisition binds the inspected catalog state") {
	SolersAssetService assets;
	Dictionary args;
	args["provider"] = "polyhaven";
	args["kind"] = "3d";
	args["asset_id"] = "synthetic_model";
	args["variant"] = "2k-gltf";
	// Without a cached inspection the service refuses to acquire: the cached
	// inspect result is the version authority, never a caller-echoed hash.
	const Dictionary result = assets.catalog_acquire(args, String());
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "CATALOG_INSPECTION_REQUIRED");
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
	const Array variants = SolersPluginPolyHaven::normalize_variants(files, "3d");
	REQUIRE(variants.size() == 1);
	const Dictionary variant = variants[0];
	CHECK(variant.get("id", String()) == "1k-fbx");
	CHECK(variant.get("checksum", String()) == "0123456789abcdef0123456789abcdef");
	CHECK((int64_t)variant.get("size", 0) == 125);
	CHECK((int)variant.get("dependency_count", 0) == 1);
}

TEST_CASE("[SolersAssetService] catalog variant identity is case-insensitive and preserves the official id") {
	Dictionary variant;
	variant["id"] = "Official-MixedCase";
	Array variants;
	variants.push_back(variant);

	CHECK(SolersPlugin::match_catalog_variant_id(variants, "official-mixedcase") == "Official-MixedCase");
	CHECK(SolersPlugin::match_catalog_variant_id(variants, "missing").is_empty());
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
	const Array ranked = SolersPlugin::rank_catalog_assets(candidates, "wood bed");
	REQUIRE(ranked.size() == 2);
	CHECK(Dictionary(ranked[0]).get("asset_id", String()) == "wood_bed");
	CHECK(Array(Dictionary(ranked[0]).get("matched_terms", Array())).size() == 2);
	CHECK(Dictionary(ranked[1]).get("asset_id", String()) == "wooden_chair");
	CHECK(Array(Dictionary(ranked[1]).get("matched_terms", Array())).has("wood"));
	CHECK_FALSE(Dictionary(ranked[1]).has("_rank_primary"));
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

TEST_CASE("[SolersOpenAIChatProtocol] SSE error frames are authoritative failures") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state;
	const Array events = protocol.parse_event(state, "error", R"json({"error":{"message":"Internal server error from model.","type":"server_error","code":"Internal error"}})json");
	REQUIRE(events.size() == 1);
	CHECK(String(Dictionary(events[0]).get("kind", String())) == SolersLLMEventKind::ERROR);
	CHECK(String(Dictionary(events[0]).get("message", String())).contains("Internal server error from model"));
	CHECK(String(Dictionary(events[0]).get("code", String())) == "server_error");

	Dictionary state2;
	const Array nested = protocol.parse_event(state2, "", R"json({"error":{"message":"boom","type":"invalid_request_error"}})json");
	REQUIRE(nested.size() == 1);
	CHECK(String(Dictionary(nested[0]).get("kind", String())) == SolersLLMEventKind::ERROR);
	CHECK(String(Dictionary(nested[0]).get("message", String())) == "boom");
}

TEST_CASE("[SolersLLMClient] empty HTTP 200 stream fails without retry") {
	// Authority: zero response body bytes ⇒ identical retry cannot help.
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
	Dictionary profile;
	profile["protocol"] = "openai-chat";
	profile["base_url"] = vformat("http://127.0.0.1:%d/v1", server->get_local_port());
	Dictionary auth;
	auth["type"] = "none";
	REQUIRE(client.begin(request, profile, auth) == OK);

	bool responded = false;
	Ref<StreamPeerTCP> connection;
	const uint64_t started = OS::get_singleton()->get_ticks_msec();
	while (!client.is_failed() && !client.is_done() && OS::get_singleton()->get_ticks_msec() - started < 3000) {
		if (!responded && server->is_connection_available()) {
			connection = server->take_connection();
			REQUIRE(connection.is_valid());
			connection->poll();
			uint8_t discard[4096];
			int received = 0;
			connection->get_partial_data(discard, 4096, received);
			const CharString resp = String(
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: text/event-stream\r\n"
					"Transfer-Encoding: chunked\r\n"
					"Connection: close\r\n"
					"\r\n"
					"0\r\n"
					"\r\n")
											 .utf8();
			connection->put_data((const uint8_t *)resp.get_data(), resp.length());
			responded = true;
			server->stop();
		}
		client.poll();
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(client.is_failed());
	CHECK(String(client.get_error().get("code", String())) == "STREAM_ENDED_WITHOUT_FINISH");
	CHECK_FALSE((bool)client.get_error().get("retryable", true));
	CHECK((int)client.get_error().get("response_bytes", -1) == 0);
	CHECK_FALSE(SolersLLMRetry::is_retryable(client.get_error()));
}

TEST_CASE("[SolersLLMClient] JSON error body on HTTP 200 is terminal") {
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
	Dictionary profile;
	profile["protocol"] = "openai-chat";
	profile["base_url"] = vformat("http://127.0.0.1:%d/v1", server->get_local_port());
	Dictionary auth;
	auth["type"] = "none";
	REQUIRE(client.begin(request, profile, auth) == OK);

	const String json_body = "{\"error\":{\"message\":\"quota exhausted\"}}";
	bool responded = false;
	Ref<StreamPeerTCP> connection;
	const uint64_t started = OS::get_singleton()->get_ticks_msec();
	while (!client.is_failed() && !client.is_done() && OS::get_singleton()->get_ticks_msec() - started < 3000) {
		if (!responded && server->is_connection_available()) {
			connection = server->take_connection();
			REQUIRE(connection.is_valid());
			connection->poll();
			uint8_t discard[4096];
			int received = 0;
			connection->get_partial_data(discard, 4096, received);
			const String http = vformat(
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: application/json\r\n"
					"Content-Length: %d\r\n"
					"Connection: close\r\n"
					"\r\n"
					"%s",
					json_body.utf8().length(), json_body);
			const CharString resp = http.utf8();
			connection->put_data((const uint8_t *)resp.get_data(), resp.length());
			responded = true;
			server->stop();
		}
		client.poll();
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(client.is_failed());
	CHECK(String(client.get_error().get("code", String())) == "PROVIDER_ERROR");
	CHECK(String(client.get_error().get("message", String())).contains("quota exhausted"));
	CHECK_FALSE((bool)client.get_error().get("retryable", true));
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

TEST_CASE("[solers_format_plan_text] replaces its plan snapshot in place") {
	Array first_plan;
	Dictionary first_step;
	first_step["step"] = "Whitebox";
	first_step["status"] = "in_progress";
	first_plan.push_back(first_step);
	const String first_text = solers_format_plan_text("Starting geometry", first_plan);

	Array second_plan;
	Dictionary second_step;
	second_step["step"] = "Whitebox";
	second_step["status"] = "completed";
	second_plan.push_back(second_step);
	const String second_text = solers_format_plan_text("Geometry verified", second_plan);

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
	CHECK(custom.get("source_kind", String()) == "custom");
	CHECK((int)custom.get("context_window", 0) == 131072);
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

	SolersSettingsService view_service;
	view_service.set_provider_registry(&registry);
	const Dictionary view = view_service.list_provider_view().get("data", Dictionary());
	CHECK(view.has("connected"));
	CHECK(view.has("popular"));
	CHECK(view.has("all"));
	CHECK(view.has("custom"));
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

TEST_CASE("[SolersOpenAIChatProtocol] classifies structured context overflow without message matching") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state;
	const Array events = protocol.parse_event(state, "error", R"json({"error":{"code":"context_length_exceeded","type":"invalid_request_error","message":"localized provider text"}})json");
	const Dictionary error = find_event_kind(events, SolersLLMEventKind::ERROR);
	REQUIRE_FALSE(error.is_empty());
	CHECK(error.get("failure_kind", String()) == "context_overflow");
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

TEST_CASE("[SolersLLMMessage] attachment projection emits image bytes once across model requests") {
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
	attachment["content_sha256"] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	attachment["type"] = "image";
	attachment["mime_type"] = "image/png";
	attachment["local_path"] = image_path;
	Array attachments;
	attachments.push_back(attachment);

	Array calls;
	Dictionary call;
	call["id"] = "call_capture";
	call["name"] = "render_capture";
	call["arguments"] = "{\"target\":\"runtime\"}";
	calls.push_back(call);
	Dictionary second_call;
	second_call["id"] = "call_query";
	second_call["name"] = "object_query";
	second_call["arguments"] = "{\"target\":\"scene\"}";
	calls.push_back(second_call);

	Array messages;
	Dictionary user_message = SolersLLMMessage::user("Inspect the reference.");
	user_message["attachments"] = attachments;
	messages.push_back(user_message);
	messages.push_back(SolersLLMMessage::assistant("", calls));
	messages.push_back(SolersLLMMessage::tool_result("call_capture", "render_capture", "{\"ok\":true}", attachments));
	messages.push_back(SolersLLMMessage::tool_result("call_query", "object_query", "{\"ok\":true}"));

	HashSet<String> delivered;
	HashSet<String> emitted;
	const Array first_projection = SolersLLMMessage::project_attachments(messages, delivered, emitted);
	REQUIRE(emitted.size() == 1);
	CHECK(Array(Dictionary(first_projection[0]).get("attachments", Array())).size() == 1);
	CHECK_FALSE(Dictionary(first_projection[2]).has("attachments"));
	CHECK(String(Dictionary(first_projection[2]).get("content", String())).contains("sha256=aaaaaaaa"));

	Dictionary request;
	request["model"] = "custom-gateway-model";
	request["messages"] = first_projection;
	SolersOpenAIChatProtocol openai;
	const Dictionary first_body = openai.build_request_body(request);
	const Array openai_messages = first_body.get("messages", Array());
	REQUIRE(openai_messages.size() == 4);
	const Array user_content = Dictionary(openai_messages[0]).get("content", Array());
	REQUIRE(user_content.size() == 2);
	CHECK(Dictionary(user_content[1]).get("type", String()) == "image_url");
	CHECK(JSON::stringify(first_body).count("data:image/png;base64,") == 1);
	SolersLLMProtocolRegistry protocols;
	protocols.register_builtin_protocols();
	SolersLLMClient client;
	client.set_protocol_registry(&protocols);
	Dictionary profile;
	profile["protocol"] = "openai-chat";
	profile["base_url"] = "http://127.0.0.1:9/v1";
	Dictionary auth;
	auth["type"] = "none";
	REQUIRE(client.begin(request, profile, auth) == OK);
	CHECK(client.get_request_body_bytes() == JSON::stringify(first_body, "", false, true).utf8().length());
	client.abort();

	SolersAnthropicMessagesProtocol anthropic;
	const Dictionary anthropic_body = anthropic.build_request_body(request);
	CHECK(JSON::stringify(anthropic_body).count("\"type\":\"image\"") == 1);

	for (const String &identity : emitted) {
		delivered.insert(identity);
	}
	const Array second_projection = SolersLLMMessage::project_attachments(messages, delivered, emitted);
	CHECK(emitted.is_empty());
	CHECK_FALSE(Dictionary(second_projection[0]).has("attachments"));
	CHECK(String(Dictionary(second_projection[0]).get("content", String())).contains("earlier model request"));
	request["messages"] = second_projection;
	CHECK_FALSE(JSON::stringify(openai.build_request_body(request)).contains("data:image/png;base64,"));
	CHECK_FALSE(JSON::stringify(anthropic.build_request_body(request)).contains("\"type\":\"image\""));

	DirAccess::remove_file_or_error(ProjectSettings::get_singleton()->globalize_path(image_path));
}

} // namespace TestSolersProviderGateway
