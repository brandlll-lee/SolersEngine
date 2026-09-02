/**************************************************************************/
/*  solers_authoring_tools.cpp                                            */
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
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/run/editor_run_bar.h"
#include "editor/run/game_view_plugin.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_builtin_skills.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/plugins/solers_plugin.h"

Dictionary SolersToolRegistry::_compact_addon_contract(const SolersToolContext &p_context, const Dictionary &p_result) {
	if (!(bool)p_result.get("ok", false)) {
		return p_result;
	}
	Dictionary result = p_result.duplicate(true);
	Dictionary data = result.get("data", Dictionary());
	const Dictionary contract = data.get("agent_contract", Dictionary());
	const String contract_id = contract.get("contract_id", String());
	if (contract_id.is_empty()) {
		return result;
	}
	const String delivery_key = p_context.session_id + ":" + contract_id;
	if (delivered_addon_contracts.has(delivery_key)) {
		Dictionary compact;
		compact["contract_id"] = contract_id;
		compact["unchanged"] = true;
		data["agent_contract"] = compact;
		result["data"] = data;
		return result;
	}
	delivered_addon_contracts.insert(delivery_key);
	return result;
}

void SolersToolRegistry::_register_script_tools() {
	if (!script_service) {
		return;
	}
	SolersScriptService *svc = script_service;
	Vector<String> project_redact;
	project_redact.push_back("content");
	_add("project.settings", "Edit ProjectSettings values through the live engine singleton.", R"({"type":"object","properties":{"values":{"type":"object"},"erase":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::EDITOR, Vector<String>(), SolersToolExposure::DEFERRED, [svc](const SolersToolContext &, const Dictionary &a) {
			Dictionary args = a.duplicate(true);
			args["operation"] = "settings";
			return svc->edit_project(args); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &) { return Array({ Dictionary({ { "mode", "write" }, { "key", "project:res://project.godot" } }) }); }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::EDITOR, SolersOperationMode::APPLY);
	_add("project.path", "Write an ordinary project data file, create a directory, or remove a file or directory through Godot's native editor lifecycle. A native file receipt may be supplied when available.", R"({"type":"object","properties":{"action":{"type":"string","enum":["write","create_directory","remove"]},"path":{"type":"string","pattern":"^res://"},"content":{"type":"string"}},"required":["action","path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::FILES, project_redact, SolersToolExposure::DEFERRED, [this, svc](const SolersToolContext &, const Dictionary &a) {
			const String action = a.get("action", String());
			if (action == "remove") {
				return file_checkpoint ? file_checkpoint->remove_project_path(a.get("path", String())) : _error("FILE_CHECKPOINT_UNAVAILABLE", "File checkpoint service is unavailable.", false);
			}
			Dictionary args = a.duplicate(true);
			args["operation"] = action == "write" ? "write_file" : action;
			args.erase("action");
			return svc->edit_project(args); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "project:" + String(a.get("path", String()));
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, {}, SNAME("path"), [](const Dictionary &a) {
				if (String(a.get("action", String())) != "remove") {
					return true;
				}
				EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
				return !filesystem || !filesystem->is_scanning(); });

	Vector<String> script_redact;
	script_redact.push_back("content");
	script_redact.push_back("old_text");
	script_redact.push_back("new_text");
	_add("script.edit", "Create a script or replace one exact text block. Replace may use expected_sha256 from project.read_file plus unique byte-for-byte old_text. A stale hash fails without changing the file; successful writes are parser-validated, checkpointed, and return the new persisted hash.", R"({"type":"object","properties":{"operation":{"type":"string","enum":["create","replace"]},"path":{"type":"string","pattern":"^res://.*\\.(gd|cs|gdshader|gdshaderinc)$"},"content":{"type":"string"},"old_text":{"type":"string","minLength":1},"new_text":{"type":"string"},"expected_sha256":{"type":"string","pattern":"^[0-9a-f]{64}$"}},"required":["operation","path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::FILES, script_redact, SolersToolExposure::DEFERRED, [svc](const SolersToolContext &, const Dictionary &a) { return svc->edit_script(a); }, SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"), {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::EDITOR, SolersOperationMode::APPLY);
	_add_observe_exposed("script.validate", "Validate script source through Godot's registered ScriptLanguage implementation.", R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the script to validate."},"source":{"type":"string","description":"Optional source override; validates this text instead of the file content."}},"required":["path"]})", SolersToolExposure::MODEL, [svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_script(a); }, {}, {}, {}, SolersToolUiKind::READ, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);

	auto script_access = [](const char *p_target_key, bool p_import_sidecar) {
		const String target_key = p_target_key;
		return [target_key, p_import_sidecar](const Dictionary &a) {
			Array accesses;
			const String target = a.get(target_key, String());
			accesses.push_back(Dictionary({ { "mode", "write" }, { "key", target.is_empty() ? String("*") : "project:" + target } }));
			if (p_import_sidecar && !target.is_empty() && FileAccess::exists(target + ".import")) {
				accesses.push_back(Dictionary({ { "mode", "write" }, { "key", "project:" + target + ".import" } }));
			}
			const String script_path = a.get("script_path", String());
			if (!script_path.is_empty()) {
				accesses.push_back(Dictionary({ { "mode", "read" }, { "key", "project:" + script_path } }));
			}
			for (const Variant &output : Array(a.get("outputs", Array()))) {
				accesses.push_back(Dictionary({ { "mode", "write" }, { "key", "project:" + String(output) } }));
			}
			return accesses;
		};
	};
	Vector<String> authority_redact;
	authority_redact.push_back("source");
	auto complete_script = [svc](const SolersToolContext &ctx, const Dictionary &, const Dictionary &) {
		svc->complete_authority_script(Dictionary({ { "call_id", ctx.call_id } }));
	};
	_add("scene.script", "Run a bounded GDScript transaction against one saved PackedScene in an isolated Godot editor process. The script declares @tool, extends RefCounted, and defines func run(ctx); ctx.subject is the native scene root. ClassDB methods are called directly, async frame waits are allowed, declared outputs are checkpointed, and ctx.run_native_job exposes registered long native operations with progress and cancellation.", R"({"type":"object","properties":{"scene_path":{"type":"string","pattern":"^res://.*\\.(tscn|scn)$"},"source":{"type":"string","minLength":1},"script_path":{"type":"string","pattern":"^res://.*\\.gd$"},"outputs":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"string","pattern":"^res://"}},"timeout_msec":{"type":"integer","minimum":1000,"maximum":600000}},"required":["scene_path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::FILES, authority_redact, SolersToolExposure::MODEL, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->start_authority_script(SNAME("scene"), a, ctx.call_id); }, SolersToolExecution::MAIN_THREAD, script_access("scene_path", false), [svc](const SolersToolContext &, const Dictionary &a) { return svc->poll_authority_script(a); }, [svc](const SolersToolContext &, const Dictionary &a) { return svc->is_authority_script_ready(a); }, complete_script, {}, {}, SolersToolUiKind::SCENE, {}, SNAME("Node"), SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, {}, SNAME("scene"), {}, PackedStringArray({ "/scene_path" }));
	_add("asset.script", "Run a bounded GDScript transaction against one imported asset in an isolated Godot editor process. The script declares @tool, extends RefCounted, and defines func run(ctx); ctx.subject is the native imported Resource. Use ctx.set_import_option or ctx.request_reimport so Godot's importer remains authoritative; declared outputs are checkpointed.", R"({"type":"object","properties":{"asset_path":{"type":"string","pattern":"^res://"},"source":{"type":"string","minLength":1},"script_path":{"type":"string","pattern":"^res://.*\\.gd$"},"outputs":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"string","pattern":"^res://"}},"timeout_msec":{"type":"integer","minimum":1000,"maximum":600000}},"required":["asset_path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::FILES, authority_redact, SolersToolExposure::MODEL, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->start_authority_script(SNAME("asset"), a, ctx.call_id); }, SolersToolExecution::MAIN_THREAD, script_access("asset_path", true), [svc](const SolersToolContext &, const Dictionary &a) { return svc->poll_authority_script(a); }, [svc](const SolersToolContext &, const Dictionary &a) { return svc->is_authority_script_ready(a); }, complete_script, {}, {}, SolersToolUiKind::ASSET, {}, SNAME("Resource"), SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY, {}, SNAME("asset"), {}, PackedStringArray({ "/asset_path" }));
}

static Dictionary _solers_apply_plugin_mention(const SolersToolContext &p_context, const Dictionary &p_args, const String &p_capability) {
	if (!String(p_args.get("provider", String())).strip_edges().is_empty()) {
		return p_args;
	}
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	for (int i = 0; i < p_context.mentions.size(); i++) {
		const Dictionary mention = p_context.mentions[i];
		const String source = String(mention.get("source", "plugin")).strip_edges().to_lower();
		if (!source.is_empty() && source != "plugin") {
			continue;
		}
		SolersPlugin *plugin = SolersPluginRegistry::get_plugin(mention.get("id", String()));
		const Dictionary profile = plugin ? plugin->get_profile() : Dictionary();
		if (!plugin || !(bool)profile.get(p_capability, false) || (!kind.is_empty() && !Array(profile.get("kinds", Array())).has(kind))) {
			continue;
		}
		Dictionary args = p_args.duplicate(true);
		args["provider"] = profile.get("id", String());
		return args;
	}
	return p_args;
}

void SolersToolRegistry::_register_asset_tools() {
	if (!asset_service) {
		return;
	}
	SolersAssetService *svc = asset_service;
	Array generation_plugin_ids;
	Array operation_plugin_ids;
	Array generation_kinds;
	Array catalog_plugin_ids;
	Array catalog_kinds;
	Dictionary option_schemas_by_name;
	String generation_labels;
	String catalog_labels;
	auto append_unique = [](Array &r_values, const Variant &p_value) {
		if (!r_values.has(p_value)) {
			r_values.push_back(p_value);
		}
	};

	for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
		const Dictionary profile = plugin->get_profile();
		const String id = String(profile.get("id", String())).strip_edges().to_lower();
		const String label = String(profile.get("label", id));
		const Array kinds = profile.get("kinds", Array());
		if (!plugin->get_operation_defs().is_empty()) {
			append_unique(operation_plugin_ids, id);
		}
		if ((bool)profile.get("supports_generation", false)) {
			append_unique(generation_plugin_ids, id);
			generation_labels += generation_labels.is_empty() ? label : ", " + label;
			for (int i = 0; i < kinds.size(); i++) {
				const String kind = String(kinds[i]).to_lower();
				append_unique(generation_kinds, kind);
				const Dictionary schema = plugin->get_generation_options_schema(kind);
				for (const Variant *key = schema.next(nullptr); key; key = schema.next(key)) {
					const String name = String(*key);
					Dictionary option = Dictionary(schema[*key]).duplicate(true);
					const String description = String(option.get("description", String()));
					option["description"] = description.is_empty() ? label : label + ": " + description;
					Array variants = option_schemas_by_name.get(name, Array());
					const String encoded = JSON::stringify(option);
					bool duplicate = false;
					for (int variant = 0; variant < variants.size(); variant++) {
						if (JSON::stringify(variants[variant]) == encoded) {
							duplicate = true;
							break;
						}
					}
					if (!duplicate) {
						variants.push_back(option);
						option_schemas_by_name[name] = variants;
					}
				}
			}
		}
		if ((bool)profile.get("supports_catalog", false)) {
			append_unique(catalog_plugin_ids, id);
			catalog_labels += catalog_labels.is_empty() ? label : ", " + label;
			for (int i = 0; i < kinds.size(); i++) {
				append_unique(catalog_kinds, String(kinds[i]).to_lower());
			}
		}
	}

	Dictionary provider_option_properties;
	for (const Variant *key = option_schemas_by_name.next(nullptr); key; key = option_schemas_by_name.next(key)) {
		const Array variants = option_schemas_by_name[*key];
		if (variants.size() == 1) {
			provider_option_properties[*key] = variants[0];
		} else {
			Dictionary union_schema;
			union_schema["anyOf"] = variants;
			provider_option_properties[*key] = union_schema;
		}
	}
	Dictionary provider_options_schema;
	provider_options_schema["type"] = "object";
	provider_options_schema["description"] = "Options defined by the selected Solers plugin.";
	provider_options_schema["properties"] = provider_option_properties;
	provider_options_schema["additionalProperties"] = true;

	Dictionary import_profile_schema;
	import_profile_schema["type"] = "string";
	Array import_profiles;
	import_profiles.push_back("runtime");
	import_profiles.push_back("baked_static");
	import_profile_schema["enum"] = import_profiles;
	import_profile_schema["description"] = "Godot import intent. baked_static enables native lightmap UV2 generation for 3D assets.";
	Dictionary target_dir_schema;
	target_dir_schema["type"] = "string";
	target_dir_schema["description"] = "Optional res:// destination. Defaults to res://assets/<kind>/<name>-<job suffix>.";
	Dictionary max_triangles_schema;
	max_triangles_schema["type"] = "integer";
	max_triangles_schema["minimum"] = 0;
	max_triangles_schema["description"] = "Optional 3D source triangle budget. Zero disables the budget only when the project ceiling permits it.";
	Dictionary map_types_schema;
	map_types_schema["type"] = "array";
	Dictionary map_type_item;
	map_type_item["type"] = "string";
	map_types_schema["items"] = map_type_item;
	map_types_schema["uniqueItems"] = true;
	map_types_schema["description"] = "Optional exact material map roles to import; omitted roles are not copied.";

	Dictionary catalog_search_schema;
	catalog_search_schema["type"] = "object";
	Dictionary catalog_search_properties;
	Dictionary catalog_provider_schema;
	catalog_provider_schema["type"] = "string";
	catalog_provider_schema["enum"] = catalog_plugin_ids;
	catalog_provider_schema["description"] = "Registered catalog plugin.";
	catalog_search_properties["provider"] = catalog_provider_schema;
	Dictionary catalog_kind_schema;
	catalog_kind_schema["type"] = "string";
	catalog_kind_schema["enum"] = catalog_kinds;
	catalog_search_properties["kind"] = catalog_kind_schema;
	Dictionary query_schema;
	query_schema["type"] = "string";
	catalog_search_properties["query"] = query_schema;
	Dictionary limit_schema;
	limit_schema["type"] = "integer";
	limit_schema["minimum"] = 1;
	limit_schema["maximum"] = 50;
	catalog_search_properties["limit"] = limit_schema;
	Dictionary offset_schema;
	offset_schema["type"] = "integer";
	offset_schema["minimum"] = 0;
	catalog_search_properties["offset"] = offset_schema;
	Dictionary refresh_schema;
	refresh_schema["type"] = "boolean";
	catalog_search_properties["refresh"] = refresh_schema;
	catalog_search_schema["properties"] = catalog_search_properties;
	Array catalog_search_required;
	catalog_search_required.push_back("query");
	catalog_search_required.push_back("kind");
	catalog_search_schema["required"] = catalog_search_required;
	catalog_search_schema["additionalProperties"] = false;
	const CharString catalog_search_json = JSON::stringify(catalog_search_schema).utf8();
	const CharString catalog_search_description = vformat("Browse or search lightweight metadata through a registered catalog plugin (%s). Inspect a selected result before acquiring it.", catalog_labels).utf8();
	_add("asset.catalog.search", catalog_search_description.get_data(), catalog_search_json.get_data(), SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationDomain::NONE, Vector<String>(), SolersToolExposure::DEFERRED, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_search(_solers_apply_plugin_mention(ctx, a, "supports_catalog"), ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "asset-catalog-directory:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower();
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);

	Dictionary catalog_inspect_schema;
	catalog_inspect_schema["type"] = "object";
	Dictionary catalog_inspect_properties;
	catalog_inspect_properties["provider"] = catalog_provider_schema;
	catalog_inspect_properties["kind"] = catalog_kind_schema;
	Dictionary source_asset_schema;
	source_asset_schema["type"] = "string";
	source_asset_schema["minLength"] = 1;
	catalog_inspect_properties["asset_id"] = source_asset_schema;
	catalog_inspect_properties["refresh"] = refresh_schema;
	catalog_inspect_schema["properties"] = catalog_inspect_properties;
	Array catalog_inspect_required;
	catalog_inspect_required.push_back("kind");
	catalog_inspect_required.push_back("asset_id");
	catalog_inspect_schema["required"] = catalog_inspect_required;
	catalog_inspect_schema["additionalProperties"] = false;
	const CharString catalog_inspect_json = JSON::stringify(catalog_inspect_schema).utf8();
	_add("asset.catalog.inspect", "Resolve one exact catalog result into authoritative variants, dependencies, licensing, and checksums. asset.catalog.acquire accepts only a previously inspected variant.", catalog_inspect_json.get_data(), SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationDomain::NONE, Vector<String>(), SolersToolExposure::DEFERRED, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_inspect(_solers_apply_plugin_mention(ctx, a, "supports_catalog"), ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "asset-catalog-detail:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower() + ":" + String(a.get("asset_id", String())).to_lower();
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);

	Dictionary generate_schema;
	generate_schema["type"] = "object";
	Dictionary generate_properties;
	Dictionary generation_kind_schema;
	generation_kind_schema["type"] = "string";
	generation_kind_schema["enum"] = generation_kinds;
	generate_properties["kind"] = generation_kind_schema;
	Dictionary prompt_schema;
	prompt_schema["type"] = "string";
	prompt_schema["description"] = "Generation prompt. With reference images prefer one short sentence (identity/style/pose only); long multi-constraint essays dilute Image-to-3D. Text-to-3D may be slightly richer but stay concise.";
	generate_properties["prompt"] = prompt_schema;
	Dictionary attachments_schema;
	attachments_schema["type"] = "array";
	Dictionary attachment_item;
	attachment_item["type"] = "string";
	attachments_schema["items"] = attachment_item;
	attachments_schema["uniqueItems"] = true;
	attachments_schema["description"] = "Attachment ids from the current conversation, when supported by the selected plugin.";
	generate_properties["input_attachments"] = attachments_schema;
	Dictionary name_schema;
	name_schema["type"] = "string";
	generate_properties["name"] = name_schema;
	Dictionary profile_schema;
	profile_schema["type"] = "string";
	generate_properties["profile"] = profile_schema;
	Dictionary generation_provider_schema;
	generation_provider_schema["type"] = "string";
	generation_provider_schema["enum"] = generation_plugin_ids;
	generation_provider_schema["description"] = "Optional registered generation plugin. Explicit selection overrides @mention and configured defaults.";
	generate_properties["provider"] = generation_provider_schema;
	generate_properties["provider_options"] = provider_options_schema;
	generate_properties["target_dir"] = target_dir_schema;
	generate_properties["import_profile"] = import_profile_schema;
	generate_properties["max_triangles"] = max_triangles_schema;
	generate_properties["map_types"] = map_types_schema;
	generate_schema["properties"] = generate_properties;
	Array generate_required;
	generate_required.push_back("kind");
	generate_schema["required"] = generate_required;
	generate_schema["additionalProperties"] = false;
	const CharString generate_json = JSON::stringify(generate_schema).utf8();
	const CharString generate_description = vformat("Generate an asset through a registered Solers plugin (%s), persist its output in the global Studio vault, and import it into res://. Read the selected plugin's provider_options schema; the job is terminal only after Godot verifies imported resources.", generation_labels).utf8();
	SolersToolHostPolicy generate_host;
	generate_host.attachment_args.push_back("input_attachments");
	_add("asset.generate", generate_description.get_data(), generate_json.get_data(), SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DEFERRED, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->generate_for_session(_solers_apply_plugin_mention(ctx, a, "supports_generation"), ctx.session_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary job;
				job["mode"] = "write";
				job["key"] = "asset-job:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower() + ":" + String(a.get("name", String())).to_lower();
				accesses.push_back(job);
				Dictionary project;
				project["mode"] = "write";
				const String target = String(a.get("target_dir", String())).strip_edges();
				project["key"] = "project:" + (target.is_empty() ? "res://assets/" + String(a.get("kind", "asset")) : target.replace_char('\\', '/').simplify_path());
				accesses.push_back(project);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, generate_host, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);

	Dictionary acquire_schema;
	acquire_schema["type"] = "object";
	Dictionary acquire_properties;
	acquire_properties["provider"] = catalog_provider_schema;
	acquire_properties["kind"] = catalog_kind_schema;
	acquire_properties["asset_id"] = source_asset_schema;
	Dictionary variant_schema;
	variant_schema["type"] = "string";
	variant_schema["minLength"] = 1;
	acquire_properties["variant"] = variant_schema;
	Dictionary source_version_schema;
	source_version_schema["type"] = "string";
	acquire_properties["source_version"] = source_version_schema;
	acquire_properties["name"] = name_schema;
	acquire_properties["target_dir"] = target_dir_schema;
	acquire_properties["import_profile"] = import_profile_schema;
	acquire_properties["max_triangles"] = max_triangles_schema;
	acquire_properties["map_types"] = map_types_schema;
	acquire_schema["properties"] = acquire_properties;
	Array acquire_required;
	acquire_required.push_back("kind");
	acquire_required.push_back("asset_id");
	acquire_required.push_back("variant");
	acquire_schema["required"] = acquire_required;
	acquire_schema["additionalProperties"] = false;
	const CharString acquire_json = JSON::stringify(acquire_schema).utf8();
	_add("asset.catalog.acquire", "Acquire one exact inspected catalog variant, verify its source metadata and checksums, then import it directly into res:// and write project-local license/attribution metadata.", acquire_json.get_data(), SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DEFERRED, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_acquire(_solers_apply_plugin_mention(ctx, a, "supports_catalog"), ctx.session_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary job;
				job["mode"] = "write";
				job["key"] = "asset-job:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("asset_id", String())) + ":" + String(a.get("variant", String()));
				accesses.push_back(job);
				Dictionary project;
				project["mode"] = "write";
				const String target = String(a.get("target_dir", String())).strip_edges();
				project["key"] = "project:" + (target.is_empty() ? "res://assets/" + String(a.get("kind", "asset")) : target.replace_char('\\', '/').simplify_path());
				accesses.push_back(project);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);

	_add_observe_exposed("asset.capabilities", "List compatible operations from every registered Solers plugin for a project asset. asset_id accepts a job id or a res:// .solers.json sidecar path.", R"({"type":"object","properties":{"asset_id":{"type":"string","minLength":1,"description":"Job id or res:// .solers.json sidecar path."}},"required":["asset_id"],"additionalProperties":false})", SolersToolExposure::DEFERRED, [svc](const SolersToolContext &, const Dictionary &a) { return svc->capabilities(a); }, {}, {}, {}, SolersToolUiKind::ASSET, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	Dictionary operation_schema = JSON::parse_string(R"({"type":"object","properties":{"asset_id":{"type":"string","minLength":1,"description":"Source job id or res:// .solers.json sidecar path."},"provider":{"type":"string"},"operation_id":{"type":"string","minLength":1},"options":{"type":"object"},"raw_provider_options":{"type":"object","description":"Advanced plugin-native options. Requires raw_confirmed=true."},"raw_confirmed":{"type":"boolean"},"target_dir":{"type":"string","description":"Optional res:// destination for the derived asset."},"import_profile":{"type":"string","enum":["runtime","baked_static"]},"max_triangles":{"type":"integer","minimum":0},"map_types":{"type":"array","items":{"type":"string"},"uniqueItems":true}},"required":["asset_id","provider","operation_id"],"additionalProperties":false})");
	Dictionary operation_properties = operation_schema["properties"];
	Dictionary operation_provider = operation_properties["provider"];
	operation_provider["enum"] = operation_plugin_ids;
	operation_provider["description"] = "Registered provider returned with the selected asset.capabilities operation.";
	operation_properties["provider"] = operation_provider;
	operation_schema["properties"] = operation_properties;
	const CharString operation_json = JSON::stringify(operation_schema).utf8();
	_add("asset.run_operation", "Run one provider-qualified operation advertised by asset.capabilities and import the derived result directly into the project. The source may be a current job id or a res:// .solers.json sidecar.", operation_json.get_data(), SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DEFERRED, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->run_operation_for_session(a, ctx.session_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary source;
				source["mode"] = "read";
				source["key"] = "asset:" + String(a.get("asset_id", String()));
				accesses.push_back(source);
				Dictionary project;
				project["mode"] = "write";
				const String target = String(a.get("target_dir", String())).strip_edges();
				project["key"] = target.is_empty() ? String("*") : "project:" + target.replace_char('\\', '/').simplify_path();
				accesses.push_back(project);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);
	_add_observe_exposed("asset.status", "Read one asset job that has already reached a project-import terminal state (imported, draft, failed, cancelled, or interrupted). Returns ASSET_NOT_READY while the job is still processing — do not retry this call to poll progress; call job.wait once and stop issuing tools so Solers can park and resume this turn.", R"({"type":"object","properties":{"asset_id":{"type":"string","minLength":1,"description":"Stable id returned by an asset job."}},"required":["asset_id"],"additionalProperties":false})", SolersToolExposure::DEFERRED, [svc](const SolersToolContext &, const Dictionary &a) { return svc->status(a); }, _access_by_arg("read", "asset:", "asset_id"), {}, {}, SolersToolUiKind::ASSET, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	_add_observe_exposed("job.wait", "Declare background asset jobs required before the Agent can continue. When no conflict-free work remains, call once and stop issuing tools; Solers parks this turn and resumes it after a requested job reaches its project-import terminal state.", R"({"type":"object","properties":{"ids":{"type":"array","minItems":1,"items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["ids"],"additionalProperties":false})", SolersToolExposure::DEFERRED, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->wait_jobs(a, ctx.session_id); }, [](const Dictionary &a) {
				Array accesses;
				const Array ids = a.get("ids", Array());
				for (int i = 0; i < ids.size(); i++) {
					Dictionary access;
					access["mode"] = "read";
					access["key"] = "asset:" + String(ids[i]);
					accesses.push_back(access);
				}
				return accesses; }, {}, {}, SolersToolUiKind::THINK, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);
}
void SolersToolRegistry::_register_addon_tools() {
	if (!asset_service) {
		return;
	}
	SolersAssetService *service = asset_service;
	_add("addon.search", "Search installable Godot addons. Verified Solers bundles are ranked first; remaining results come from the official Godot Asset Library.", R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Plugin name or capability."},"limit":{"type":"integer","minimum":1,"maximum":50,"description":"Maximum results. Default 20."}},"required":["query"]})", SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationDomain::NONE, Vector<String>(), SolersToolExposure::DEFERRED, [service](const SolersToolContext &ctx, const Dictionary &args) { return service->addon_search(args, ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "addon-catalog:";
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	_add("addon.inspect", "Inspect one exact Godot addon before installation. Returns inert package facts plus an optional bounded, data-only Agent Contract; repeated identical contracts are returned by id without reinjecting their full content.", R"({"type":"object","properties":{"source":{"type":"string","enum":["bundled","assetlib"]},"plugin_id":{"type":"string","minLength":1,"description":"Exact package plugin_id returned by addon.search."},"refresh":{"type":"boolean","description":"Redownload Asset Library metadata and archive instead of reusing the inert cache."}},"required":["source","plugin_id"]})", SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationDomain::NONE, Vector<String>(), SolersToolExposure::DEFERRED, [this, service](const SolersToolContext &ctx, const Dictionary &args) { return _compact_addon_contract(ctx, service->addon_inspect(args, ctx.cancel_requested)); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &args) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "addon-cache:" + String(args.get("source", String())) + ":" + String(args.get("plugin_id", String()));
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, [](const Dictionary &args) { return SolersAssetService::is_trusted_addon(args) ? SolersPermissionManager::PERMISSION_OBSERVE : SolersPermissionManager::PERMISSION_NETWORK; }, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	_add_observe_exposed("addon.list", "List Godot addons installed through Solers, including pinned version, source, package hash, enabled state, registered ClassDB types, missing files, restart requirements, and load errors.", R"({"type":"object","properties":{}})", SolersToolExposure::DEFERRED, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_list(args); }, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "read";
				access["key"] = "project:res://.solers/plugins.lock.json";
				accesses.push_back(access);
				return accesses; }, {}, {}, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	_add("addon.ensure", "Install and enable one inspected exact Godot addon version. Completes after the editor filesystem scan has registered the addon's classes; success means files exist, extensions are loaded, editor plugins are enabled, and all Contract entry classes are registered.", R"({"type":"object","properties":{"source":{"type":"string","enum":["bundled","assetlib"]},"plugin_id":{"type":"string","minLength":1},"version":{"type":"string","minLength":1},"sha256":{"type":"string","pattern":"^[0-9a-fA-F]{64}$"}},"required":["source","plugin_id","version","sha256"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_INSTALL_PLUGIN, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DEFERRED, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_ensure(args); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &args) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "project:res://addons/" + String(args.get("plugin_id", String())).to_lower();
				accesses.push_back(access);
				Dictionary lock;
				lock["mode"] = "write";
				lock["key"] = "project:res://.solers/plugins.lock.json";
				accesses.push_back(lock);
				return accesses; }, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_ensure_finalize(args); }, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_ensure_ready(args); }, {}, [](const Dictionary &args) { return SolersAssetService::is_trusted_addon(args) ? SolersPermissionManager::PERMISSION_EDIT_FILES : SolersPermissionManager::PERMISSION_INSTALL_PLUGIN; }, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);
}
