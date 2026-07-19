/**************************************************************************/
/*  solers_asset_service.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_asset_service.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/extension/gdextension_manager.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/version.h"
#include "editor/asset_library/editor_asset_installer.h"
#include "editor/editor_interface.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/zip/zip_reader.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/node.h"
#include "scene/resources/mesh.h"
#include "scene/resources/packed_scene.h"

// Manifest files are persisted snapshots, not the live task database. Keep
// every in-process reader outside the short replace window so a reader can
// never observe the temporary absence created by DirAccess on Windows.
static Mutex solers_asset_manifest_mutex;

static constexpr const char *SOLERS_PLUGIN_LOCK_PATH = "res://.solers/plugins.lock.json";
static constexpr const char *SOLERS_TERRAIN3D_ID = "terrain3d";
static constexpr const char *SOLERS_TERRAIN3D_VERSION = "1.0.2-stable";
static constexpr const char *SOLERS_TERRAIN3D_SHA256 = "a071850250ec5e596aa54da61c01d75768774eb379ee997584d426a45f4884a2";
static constexpr const char *SOLERS_TERRAIN3D_ARCHIVE = "Terrain3D_v1.0.2-stable.zip";

static Dictionary _solers_terrain3d_agent_contract() {
	Dictionary contract;
	contract["schema_version"] = 1;
	contract["plugin_id"] = SOLERS_TERRAIN3D_ID;
	contract["version"] = SOLERS_TERRAIN3D_VERSION;
	contract["summary"] = "Terrain3D owns terrain data, regions, materials, assets, instancing, collision, and clipmap rendering. A terrain is not ready until data_directory is set and its managed Terrain3DData contains at least one region.";
	Array entry_classes;
	entry_classes.push_back("Terrain3D");
	entry_classes.push_back("Terrain3DData");
	entry_classes.push_back("Terrain3DRegion");
	entry_classes.push_back("Terrain3DMaterial");
	entry_classes.push_back("Terrain3DAssets");
	contract["entry_classes"] = entry_classes;
	Array capabilities;
	Dictionary terrain;
	terrain["name"] = "editable_terrain";
	terrain["description"] = "Region-based height, control, color, material, foliage, collision, and LOD data.";
	capabilities.push_back(terrain);
	contract["capabilities"] = capabilities;
	Array workflow;
	workflow.push_back("Create a Terrain3D node, set an empty data_directory and region_size, then use the node-owned data, material, and instancer objects; do not construct or save Terrain3DData as a standalone resource.");
	workflow.push_back("Import a 16/32-bit height Image through Terrain3DData.import_images, persist regions with save_directory(data_directory), then verify get_region_count is positive and region files reload before adding materials or foliage.");
	workflow.push_back("Only after the data stage passes, configure material layers, collision, navigation, instancing, camera, lighting, and runtime validation.");
	contract["workflow"] = workflow;
	Array validation;
	validation.push_back("All declared entry classes are registered and the editor plugin is enabled.");
	validation.push_back("Terrain3DData reports at least one region and the per-region resources reload from data_directory.");
	validation.push_back("The terrain scene reloads, shaders compile, and EngineDebugger starts without Terrain3D errors.");
	contract["validation"] = validation;
	return contract;
}

static Dictionary _solers_terrain3d_package() {
	Dictionary package;
	package["source"] = "bundled";
	package["plugin_id"] = SOLERS_TERRAIN3D_ID;
	package["asset_id"] = "3892";
	package["title"] = "Terrain3D";
	package["author"] = "TokisanGames";
	package["version"] = SOLERS_TERRAIN3D_VERSION;
	package["sha256"] = SOLERS_TERRAIN3D_SHA256;
	package["license"] = "MIT";
	package["godot_version"] = "4.4+";
	package["browse_url"] = "https://github.com/TokisanGames/Terrain3D";
	package["documentation_url"] = "https://terrain3d.readthedocs.io/en/stable/";
	package["description"] = "Verified first-party Terrain3D package for large editable terrain, LOD, material painting, holes, foliage, collisions, and heightmap import.";
	package["trusted"] = true;
	package["_agent_contract"] = _solers_terrain3d_agent_contract();
	return package;
}

static String _solers_plugin_key(const String &p_source, const String &p_plugin_id) {
	return p_source.to_lower() + ":" + p_plugin_id.to_lower();
}

static String _solers_plugin_inspection_key(const String &p_source, const String &p_plugin_id, const String &p_version, const String &p_sha256) {
	return _solers_plugin_key(p_source, p_plugin_id) + ":" + p_version.to_lower() + ":" + p_sha256.to_lower();
}

static String _solers_plugin_cache_root() {
	return EditorPaths::get_singleton()->get_cache_dir().path_join("solers/plugins");
}

static String _solers_terrain3d_archive_path() {
	const String override_path = OS::get_singleton()->get_environment("SOLERS_TERRAIN3D_ARCHIVE").strip_edges();
	if (!override_path.is_empty() && FileAccess::exists(override_path)) {
		return override_path;
	}
	const String bundled = OS::get_singleton()->get_executable_path().get_base_dir().path_join("solers_bundles").path_join(SOLERS_TERRAIN3D_ARCHIVE);
	if (FileAccess::exists(bundled)) {
		return bundled;
	}
	return String();
}

static Dictionary _solers_inspect_plugin_archive(const String &p_archive_path, bool p_allow_strip_toplevel) {
	Dictionary inspected = EditorAssetPackageInstaller::inspect_package(p_archive_path, "res://", false);
	if (!(bool)inspected.get("ok", false)) {
		return inspected;
	}
	auto has_plugin_config = [](const Dictionary &p_result) {
		const Dictionary data = p_result.get("data", Dictionary());
		const Array files = data.get("files", Array());
		for (const Variant &value : files) {
			const Dictionary file = value;
			const String target = file.get("target_path", String());
			if (target.begins_with("res://addons/") && target.ends_with("/plugin.cfg")) {
				return true;
			}
		}
		return false;
	};
	if (!has_plugin_config(inspected) && p_allow_strip_toplevel) {
		const Dictionary stripped = EditorAssetPackageInstaller::inspect_package(p_archive_path, "res://", true);
		if ((bool)stripped.get("ok", false) && has_plugin_config(stripped)) {
			inspected = stripped;
		}
	}
	if (!has_plugin_config(inspected)) {
		Dictionary error;
		error["code"] = "PLUGIN_PACKAGE_INVALID";
		error["message"] = "The package does not contain an addons/<plugin>/plugin.cfg entry.";
		error["recoverable"] = false;
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}

	const Dictionary package_data = inspected.get("data", Dictionary());
	const Array files = package_data.get("files", Array());
	HashSet<String> roots;
	Array plugin_names;
	Array plugin_configs;
	for (const Variant &value : files) {
		const Dictionary file = value;
		const String target = file.get("target_path", String());
		if (target.begins_with("res://addons/") && target.ends_with("/plugin.cfg")) {
			const String root = target.get_base_dir();
			roots.insert(root);
			plugin_names.push_back(root.get_file());
			plugin_configs.push_back(target);
		}
	}

	Dictionary mappings;
	PackedStringArray selected;
	Array selected_files;
	Array target_files;
	Array gdextensions;
	Array documentation;
	Array conflicts;
	for (const Variant &value : files) {
		const Dictionary file = value;
		if ((bool)file.get("directory", false)) {
			continue;
		}
		const String target = file.get("target_path", String());
		bool owned = false;
		for (const String &root : roots) {
			if (target.begins_with(root + "/")) {
				owned = true;
				break;
			}
		}
		if (!owned) {
			continue;
		}
		const String source = file.get("source_path", String());
		mappings[source] = file.get("relative_path", String());
		selected.push_back(source);
		selected_files.push_back(file);
		target_files.push_back(target);
		if ((bool)file.get("conflict", false)) {
			conflicts.push_back(target);
		}
		if (target.ends_with(".gdextension")) {
			gdextensions.push_back(target);
		}
		const String lower_file = target.get_file().to_lower();
		if (lower_file.begins_with("readme") || lower_file.begins_with("license") || lower_file.ends_with(".md")) {
			documentation.push_back(target);
		}
	}

	Dictionary data;
	data["files"] = selected_files;
	data["target_files"] = target_files;
	data["plugin_names"] = plugin_names;
	data["plugin_configs"] = plugin_configs;
	data["gdextensions"] = gdextensions;
	data["documentation"] = documentation;
	data["conflicts"] = conflicts;
	data["contains_executable_code"] = true;
	data["total_uncompressed_size"] = package_data.get("total_uncompressed_size", 0);
	data["_archive_path"] = p_archive_path;
	data["_mappings"] = mappings;
	data["_selected_files"] = selected;
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static bool _solers_validate_agent_contract(const Dictionary &p_contract, const String &p_plugin_id, const String &p_version, const String &p_sha256, Dictionary &r_contract, String &r_error) {
	static const char *allowed_keys[] = { "schema_version", "plugin_id", "version", "summary", "entry_classes", "capabilities", "workflow", "validation" };
	for (const Variant *key = p_contract.next(nullptr); key; key = p_contract.next(key)) {
		bool allowed = false;
		for (const char *allowed_key : allowed_keys) {
			if (String(*key) == allowed_key) {
				allowed = true;
				break;
			}
		}
		if (!allowed) {
			r_error = vformat("Unknown Agent Contract field '%s'. Contracts are data only and cannot declare tools or executable actions.", String(*key));
			return false;
		}
	}
	if ((int)p_contract.get("schema_version", 0) != 1 || String(p_contract.get("plugin_id", String())).to_lower() != p_plugin_id.to_lower() || String(p_contract.get("version", String())) != p_version) {
		r_error = "Agent Contract identity or schema version does not match the inspected plugin package.";
		return false;
	}
	const String summary = p_contract.get("summary", String());
	const Array entry_classes = p_contract.get("entry_classes", Array());
	const Array workflow = p_contract.get("workflow", Array());
	const Array validation = p_contract.get("validation", Array());
	if (summary.is_empty() || summary.length() > 4096 || entry_classes.is_empty() || entry_classes.size() > 64 || workflow.is_empty() || workflow.size() > 32 || validation.is_empty() || validation.size() > 32) {
		r_error = "Agent Contract summary, entry_classes, workflow, or validation is missing or exceeds its bounded size.";
		return false;
	}
	auto validate_strings = [&r_error](const Array &p_values, int p_max_length, const String &p_field) {
		for (const Variant &value : p_values) {
			if (value.get_type() != Variant::STRING || String(value).strip_edges().is_empty() || String(value).length() > p_max_length) {
				r_error = vformat("Agent Contract %s must contain bounded non-empty strings.", p_field);
				return false;
			}
		}
		return true;
	};
	if (!validate_strings(entry_classes, 128, "entry_classes") || !validate_strings(workflow, 2048, "workflow") || !validate_strings(validation, 2048, "validation")) {
		return false;
	}
	const Array capabilities = p_contract.get("capabilities", Array());
	if (capabilities.size() > 64) {
		r_error = "Agent Contract capabilities exceeds 64 entries.";
		return false;
	}
	for (const Variant &value : capabilities) {
		if (value.get_type() != Variant::DICTIONARY) {
			r_error = "Agent Contract capabilities must be objects.";
			return false;
		}
		const Dictionary capability = value;
		for (const Variant *key = capability.next(nullptr); key; key = capability.next(key)) {
			if (String(*key) != "name" && String(*key) != "description") {
				r_error = "Agent Contract capabilities may contain only name and description.";
				return false;
			}
		}
		const String name = capability.get("name", String());
		const String description = capability.get("description", String());
		if (name.is_empty() || name.length() > 128 || description.is_empty() || description.length() > 2048) {
			r_error = "Agent Contract capability name and description must be bounded non-empty strings.";
			return false;
		}
	}
	r_contract = p_contract.duplicate(true);
	r_contract["contract_id"] = (p_plugin_id.to_lower() + ":" + p_version + ":" + p_sha256.to_lower() + ":" + JSON::stringify(p_contract)).sha256_text();
	return true;
}

static bool _solers_read_archive_text(const String &p_archive_path, const String &p_source_path, String &r_text) {
	ZIPReader reader;
	if (reader.open(p_archive_path) != OK) {
		return false;
	}
	const PackedByteArray bytes = reader.read_file(p_source_path, true);
	reader.close();
	if (bytes.is_empty()) {
		return false;
	}
	r_text = String::utf8((const char *)bytes.ptr(), bytes.size());
	return true;
}

static bool _solers_agent_contract_from_archive(const Dictionary &p_inspection, Dictionary &r_contract, String &r_error) {
	const String archive_path = p_inspection.get("_archive_path", String());
	const Array files = p_inspection.get("files", Array());
	for (const Variant &file_value : files) {
		const Dictionary file = file_value;
		const String config_target = file.get("target_path", String());
		if (!config_target.ends_with("/plugin.cfg")) {
			continue;
		}
		String config_text;
		if (!_solers_read_archive_text(archive_path, file.get("source_path", String()), config_text)) {
			r_error = vformat("Unable to read %s from the plugin archive.", config_target);
			return false;
		}
		Ref<ConfigFile> config;
		config.instantiate();
		if (config->parse(config_text) != OK) {
			r_error = vformat("Unable to parse %s.", config_target);
			return false;
		}
		String declared = String(config->get_value("plugin", "agent_contract", String())).strip_edges();
		if (declared.is_empty()) {
			continue;
		}
		if (!declared.begins_with("res://")) {
			declared = config_target.get_base_dir().path_join(declared);
		}
		declared = declared.simplify_path();
		if (!declared.begins_with(config_target.get_base_dir() + "/") || declared.get_extension().to_lower() != "json") {
			r_error = "plugin.cfg agent_contract must be a JSON file inside the same plugin directory.";
			return false;
		}
		String contract_source;
		for (const Variant &candidate_value : files) {
			const Dictionary candidate = candidate_value;
			if (String(candidate.get("target_path", String())) == declared) {
				contract_source = candidate.get("source_path", String());
				break;
			}
		}
		String contract_text;
		if (contract_source.is_empty() || !_solers_read_archive_text(archive_path, contract_source, contract_text)) {
			r_error = "Declared Agent Contract is missing from the inspected package.";
			return false;
		}
		const Variant parsed = JSON::parse_string(contract_text);
		if (parsed.get_type() != Variant::DICTIONARY) {
			r_error = "Declared Agent Contract must contain one JSON object.";
			return false;
		}
		r_contract = parsed;
		return true;
	}
	return true;
}

static Dictionary _solers_public_plugin_inspection(const Dictionary &p_inspection) {
	Dictionary out = p_inspection.duplicate(true);
	out.erase("_archive_path");
	out.erase("_mappings");
	out.erase("_selected_files");
	out.erase("_agent_contract");
	return out;
}

static EditorFileSystem *_solers_editor_filesystem() {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return nullptr;
	}
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	return filesystem && filesystem->get_filesystem() ? filesystem : nullptr;
}

void SolersAssetService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("generate", "args"), &SolersAssetService::generate);
	ClassDB::bind_method(D_METHOD("capabilities", "args"), &SolersAssetService::capabilities);
	ClassDB::bind_method(D_METHOD("run_operation", "args"), &SolersAssetService::run_operation);
	ClassDB::bind_method(D_METHOD("status", "args"), &SolersAssetService::status);
	ClassDB::bind_method(D_METHOD("list_local", "args"), &SolersAssetService::list_local);
	ClassDB::bind_method(D_METHOD("import_to_project", "args"), &SolersAssetService::import_to_project);
	ClassDB::bind_method(D_METHOD("delete_asset", "args"), &SolersAssetService::delete_asset);
}

Dictionary SolersAssetService::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersAssetService::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

Dictionary SolersAssetService::_error_data(const String &p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	return error;
}

String SolersAssetService::_asset_root() {
	return "user://solers_library/assets";
}

String SolersAssetService::_asset_dir(const String &p_asset_id) {
	return _asset_root().path_join(p_asset_id);
}

String SolersAssetService::_manifest_path(const String &p_asset_id) {
	return _asset_dir(p_asset_id).path_join("manifest.json");
}

String SolersAssetService::_source_dir(const String &p_asset_id) {
	return _asset_dir(p_asset_id).path_join("source");
}

String SolersAssetService::_safe_slug(const String &p_text) {
	const String text = p_text.strip_edges().to_lower();
	String out;
	bool sep = false;
	for (int i = 0; i < text.length(); i++) {
		const char32_t c = text[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (ok) {
			out += String::chr(c);
			sep = false;
		} else if (!sep) {
			out += "-";
			sep = true;
		}
	}
	while (out.begins_with("-")) {
		out = out.substr(1);
	}
	while (out.ends_with("-")) {
		out = out.substr(0, out.length() - 1);
	}
	return out.is_empty() ? "asset" : out;
}

String SolersAssetService::_default_provider(const String &p_kind) {
	return p_kind == "3d" ? "meshy" : "elevenlabs";
}

String SolersAssetService::_default_base_url(const String &p_provider) {
	if (p_provider == "meshy") {
		return "https://api.meshy.ai";
	}
	if (p_provider == "elevenlabs") {
		return "https://api.elevenlabs.io";
	}
	return String();
}

String SolersAssetService::_default_env(const String &p_provider) {
	if (p_provider == "meshy") {
		return "MESHY_API_KEY";
	}
	if (p_provider == "elevenlabs") {
		return "ELEVENLABS_API_KEY";
	}
	return String();
}

String SolersAssetService::_setting_path(const String &p_kind, const String &p_key) {
	return "solers/ai_assets/" + p_kind + "/" + p_key;
}

static Dictionary _solers_option_schema(const String &p_type, const String &p_label) {
	Dictionary option;
	option["type"] = p_type;
	option["label"] = p_label;
	return option;
}

static Dictionary _solers_operation_def(const String &p_operation_id) {
	Dictionary op;
	op["operation_id"] = p_operation_id;
	op["agent_supported"] = true;
	const String docs_page = p_operation_id == "rig_humanoid" ? String("rigging") : (p_operation_id == "animate_humanoid" ? String("animation") : (p_operation_id == "remesh" ? String("remesh") : (p_operation_id == "retexture" ? String("retexture") : String("text-to-3d"))));
	op["docs"] = String("https://docs.meshy.ai/en/api/") + docs_page;

	Dictionary requires;
	requires["kind"] = "3d";
	Dictionary schema;
	Dictionary properties;
	Array required;

	if (p_operation_id == "refine") {
		op["label"] = "Refine to Ready";
		op["description"] = "Create a ready project version.";
		op["category"] = "Model";
		op["provider_operation_id"] = "meshy.openapi.v2.text-to-3d.refine";
		op["endpoint"] = "/openapi/v2/text-to-3d";
		requires["status"] = "draft";
		Array task_id_fields;
		task_id_fields.push_back("preview_task_id");
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
	} else if (p_operation_id == "remesh") {
		op["label"] = "Optimize";
		op["description"] = "Create a new optimized version.";
		op["category"] = "Geometry";
		op["provider_operation_id"] = "meshy.openapi.v1.remesh";
		op["endpoint"] = "/openapi/v1/remesh";
		requires["status"] = "ready";
		requires["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
		Array topology;
		topology.push_back("triangle");
		topology.push_back("quad");
		Dictionary topology_option = _solers_option_schema("string", "Topology");
		topology_option["enum"] = topology;
		topology_option["default"] = "triangle";
		properties["topology"] = topology_option;
		Dictionary polycount_option = _solers_option_schema("integer", "Target Polygons");
		polycount_option["default"] = 30000;
		properties["target_polycount"] = polycount_option;
		Array ui_order;
		ui_order.push_back("target_polycount");
		ui_order.push_back("topology");
		schema["ui_order"] = ui_order;
	} else if (p_operation_id == "retexture") {
		op["label"] = "Restyle";
		op["description"] = "Create a new material version.";
		op["category"] = "Material";
		op["provider_operation_id"] = "meshy.openapi.v1.retexture";
		op["endpoint"] = "/openapi/v1/retexture";
		requires["status"] = "ready";
		requires["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
		properties["text_style_prompt"] = _solers_option_schema("string", "Style Prompt");
		required.push_back("text_style_prompt");
	} else if (p_operation_id == "rig_humanoid") {
		op["label"] = "Rig";
		op["description"] = "Create a new rigged version.";
		op["category"] = "Character";
		op["provider_operation_id"] = "meshy.openapi.v1.rigging";
		op["endpoint"] = "/openapi/v1/rigging";
		requires["status"] = "ready";
		requires["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
		properties["humanoid_confirmed"] = _solers_option_schema("boolean", "This is a humanoid character");
		required.push_back("humanoid_confirmed");
	} else if (p_operation_id == "animate_humanoid") {
		op["label"] = "Animate";
		op["description"] = "Create a new animated version.";
		op["category"] = "Character";
		op["provider_operation_id"] = "meshy.openapi.v1.animations";
		op["endpoint"] = "/openapi/v1/animations";
		requires["status"] = "ready";
		requires["rig"] = "humanoid";
		Array task_id_fields;
		task_id_fields.push_back("rig_task_id");
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
		Dictionary action_option = _solers_option_schema("integer", "Action ID");
		action_option["description"] = "Meshy official Animation Library action_id. Use animation_actions from asset.capabilities; do not guess.";
		properties["action_id"] = action_option;
		required.push_back("action_id");
	} else {
		return Dictionary();
	}

	schema["type"] = "object";
	schema["properties"] = properties;
	schema["required"] = required;
	op["options_schema"] = schema;
	op["requires"] = requires;
	return op;
}

static Array _solers_operation_defs(const String &p_provider) {
	Array ops;
	if (p_provider != "meshy") {
		return ops;
	}
	const char *operation_ids[] = { "refine", "remesh", "retexture", "rig_humanoid", "animate_humanoid" };
	for (const char *operation_id : operation_ids) {
		ops.push_back(_solers_operation_def(operation_id));
	}
	return ops;
}

static String _solers_asset_status(const Dictionary &p_manifest) {
	return String(p_manifest.get("status", "unknown")).to_lower();
}

static Dictionary _solers_asset_traits(const Dictionary &p_manifest) {
	Dictionary traits = p_manifest.get("traits", Dictionary()).duplicate(true);
	if (!traits.has("model_state") && String(p_manifest.get("kind", String())).to_lower() == "3d") {
		const String operation = String(p_manifest.get("operation", String()));
		if (operation == "rig_humanoid") {
			traits["model_state"] = "rigged_model";
			traits["rig"] = "humanoid";
		} else if (operation == "animate_humanoid") {
			traits["model_state"] = "animated_model";
			traits["rig"] = "humanoid";
			traits["animation"] = "present";
		} else {
			traits["model_state"] = "static_model";
		}
	}
	return traits;
}

static String _solers_first_manifest_field(const Dictionary &p_manifest, const Array &p_fields) {
	for (int i = 0; i < p_fields.size(); i++) {
		const String field = String(p_fields[i]);
		const String value = String(p_manifest.get(field, String())).strip_edges();
		if (!value.is_empty()) {
			return value;
		}
	}
	return String();
}

static bool _solers_schema_ui_supported(const Dictionary &p_schema) {
	const Dictionary properties = p_schema.get("properties", Dictionary());
	const Array required = p_schema.get("required", Array());
	for (int i = 0; i < required.size(); i++) {
		const String name = String(required[i]);
		const Dictionary property = properties.get(name, Dictionary());
		const String type = String(property.get("type", String()));
		if (type != "string" && type != "boolean" && type != "integer") {
			return false;
		}
	}
	return true;
}

static bool _solers_normalize_integer_options(Dictionary &r_options, const Dictionary &p_properties, String &r_error) {
	for (const Variant *K = p_properties.next(nullptr); K; K = p_properties.next(K)) {
		const String name = String(*K);
		const Dictionary property = p_properties.get(*K, Dictionary());
		if (String(property.get("type", String())) != "integer" || !r_options.has(name)) {
			continue;
		}
		const Variant value = r_options[name];
		switch (value.get_type()) {
			case Variant::INT:
				break;
			case Variant::FLOAT: {
				const double number = (double)value;
				if (!Math::is_finite(number) || number != Math::floor(number) || number < (double)INT64_MIN || number >= (double)INT64_MAX) {
					r_error = vformat("%s must be an integer.", name);
					return false;
				}
				r_options[name] = (int64_t)number;
			} break;
			case Variant::STRING: {
				const String text = String(value).strip_edges();
				if (!text.is_valid_int()) {
					r_error = vformat("%s must be an integer.", name);
					return false;
				}
				r_options[name] = (int64_t)text.to_int();
			} break;
			default:
				r_error = vformat("%s must be an integer.", name);
				return false;
		}
	}
	return true;
}

static bool _solers_validate_bool_option(const Dictionary &p_options, const String &p_name, String &r_error) {
	if (!p_options.has(p_name) || p_options.get(p_name, Variant()).get_type() == Variant::BOOL) {
		return true;
	}
	r_error = vformat("%s must be a boolean.", p_name);
	return false;
}

static bool _solers_manifest_matches_operation(const Dictionary &p_manifest, const Dictionary &p_operation, String &r_reason) {
	const Dictionary requires = p_operation.get("requires", Dictionary());
	const String required_kind = String(requires.get("kind", String()));
	if (!required_kind.is_empty() && String(p_manifest.get("kind", String())).to_lower() != required_kind) {
		r_reason = "Asset kind does not match.";
		return false;
	}
	const String required_status = String(requires.get("status", String()));
	if (!required_status.is_empty() && _solers_asset_status(p_manifest) != required_status) {
		r_reason = "Asset status does not match.";
		return false;
	}
	const Dictionary traits = _solers_asset_traits(p_manifest);
	const char *trait_names[] = { "model_state", "rig" };
	for (const char *trait_name : trait_names) {
		const String required_trait = String(requires.get(trait_name, String()));
		if (!required_trait.is_empty() && String(traits.get(trait_name, String())) != required_trait) {
			r_reason = "Asset traits do not match.";
			return false;
		}
	}
	const Array task_id_fields = requires.get("task_id_fields", Array());
	if (!task_id_fields.is_empty() && _solers_first_manifest_field(p_manifest, task_id_fields).is_empty()) {
		r_reason = "Provider task id is missing.";
		return false;
	}
	return true;
}

Dictionary SolersAssetService::_provider_config(const String &p_kind, const String &p_provider) const {
	EditorSettings *settings = EditorSettings::get_singleton();
	String provider = p_provider;
	if (provider.is_empty() && settings && settings->has_setting(_setting_path(p_kind, "provider"))) {
		provider = String(settings->get_setting(_setting_path(p_kind, "provider")));
	}
	if (provider.is_empty()) {
		provider = _default_provider(p_kind);
	}
	Dictionary config;
	config["provider"] = provider;
	config["base_url"] = settings && settings->has_setting(_setting_path(p_kind, "base_url")) ? String(settings->get_setting(_setting_path(p_kind, "base_url"))) : _default_base_url(provider);
	String key;
	if (settings && settings->has_setting(_setting_path(p_kind, "api_key"))) {
		key = SolersSecretStore::unprotect(String(settings->get_setting(_setting_path(p_kind, "api_key"))));
	}
	const String env = _default_env(provider);
	if (key.is_empty() && !env.is_empty() && OS::get_singleton()->has_environment(env)) {
		key = OS::get_singleton()->get_environment(env);
	}
	config["api_key"] = key;
	config["api_key_env"] = env;
	return config;
}

static String _solers_header_value(const List<String> &p_headers, const String &p_name) {
	const String needle = p_name.to_lower() + ":";
	for (const String &header : p_headers) {
		if (header.to_lower().begins_with(needle)) {
			return header.substr(needle.length()).strip_edges();
		}
	}
	return String();
}

static String _solers_url_scheme_name(const String &p_scheme) {
	String scheme = p_scheme.to_lower();
	if (scheme.ends_with("://")) {
		scheme = scheme.substr(0, scheme.length() - 3);
	}
	return scheme;
}

static String _solers_url_origin(const String &p_scheme, const String &p_host, int p_port) {
	const String scheme = _solers_url_scheme_name(p_scheme);
	String origin = scheme + "://" + p_host;
	if (!((scheme == "https" && p_port == 443) || (scheme == "http" && p_port == 80))) {
		origin += ":" + itos(p_port);
	}
	return origin;
}

static bool _solers_same_origin(const String &p_a, const String &p_b) {
	String a_scheme;
	String a_host;
	int a_port = 0;
	String a_path;
	String a_fragment;
	String b_scheme;
	String b_host;
	int b_port = 0;
	String b_path;
	String b_fragment;
	if (p_a.parse_url(a_scheme, a_host, a_port, a_path, a_fragment) != OK || p_b.parse_url(b_scheme, b_host, b_port, b_path, b_fragment) != OK) {
		return false;
	}
	a_scheme = _solers_url_scheme_name(a_scheme);
	b_scheme = _solers_url_scheme_name(b_scheme);
	if (a_port <= 0) {
		a_port = a_scheme == "https" ? 443 : 80;
	}
	if (b_port <= 0) {
		b_port = b_scheme == "https" ? 443 : 80;
	}
	return a_scheme == b_scheme && a_host.to_lower() == b_host.to_lower() && a_port == b_port;
}

static bool _solers_has_secret_header(const Vector<String> &p_headers) {
	for (const String &header : p_headers) {
		const String lower = header.to_lower();
		if (lower.begins_with("authorization:") || lower.begins_with("xi-api-key:")) {
			return true;
		}
	}
	return false;
}

static Dictionary _solers_headers_dict(const List<String> &p_headers) {
	Dictionary headers;
	for (const String &header : p_headers) {
		const int colon = header.find(":");
		if (colon > 0) {
			headers[header.substr(0, colon).strip_edges()] = header.substr(colon + 1).strip_edges();
		}
	}
	return headers;
}

static Dictionary _solers_http_error(const String &p_code, const String &p_message, const String &p_url, HTTPClient::Status p_status, int p_response_code, const List<String> &p_headers = List<String>()) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["url"] = p_url;
	error["http_client_status"] = (int)p_status;
	if (p_response_code > 0) {
		error["status"] = p_response_code;
	}
	if (!p_headers.is_empty()) {
		error["headers"] = _solers_headers_dict(p_headers);
	}
	return error;
}

Dictionary SolersAssetService::_http_request(const String &p_method, const String &p_url, const Vector<String> &p_headers, const PackedByteArray &p_body, uint64_t p_timeout_msec, int64_t p_max_body_bytes, const SafeFlag *p_cancel_requested, int p_retry_count) {
	String current_url = p_url;
	String redirected_from;
	for (int redirect_count = 0; redirect_count <= 3; redirect_count++) {
		if (p_cancel_requested && p_cancel_requested->is_set()) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _error_data("HTTP_CANCELLED", "HTTP request was cancelled.");
			return out;
		}
		String scheme;
		String host;
		int port = 0;
		String path;
		String fragment;
		Error parse_err = current_url.parse_url(scheme, host, port, path, fragment);
		if (parse_err != OK || host.is_empty()) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _error_data("INVALID_URL", "Invalid provider URL.");
			return out;
		}
		scheme = _solers_url_scheme_name(scheme);
		if (scheme != "http" && scheme != "https") {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _error_data("INVALID_URL", "Provider URL must use http or https.");
			return out;
		}
		if (path.is_empty()) {
			path = "/";
		}
		const bool tls = scheme == "https";
		if (port <= 0) {
			port = tls ? 443 : 80;
		}

		Ref<HTTPClient> http = HTTPClient::create();
		if (http.is_null()) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _error_data("NO_HTTP_CLIENT", "Failed to create HTTPClient.");
			return out;
		}
		Ref<TLSOptions> tls_options = tls ? TLSOptions::client() : Ref<TLSOptions>();
		Error err = http->connect_to_host(host, port, tls_options);
		if (err != OK) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _error_data("CONNECT_FAILED", vformat("connect_to_host failed for %s:%d (error %d).", host, port, err));
			out["url"] = current_url;
			return out;
		}

		const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + p_timeout_msec;
		bool requested = false;
		bool reading = false;
		bool disconnected = false;
		PackedByteArray body;
		List<String> response_headers;
		bool response_headers_read = false;
		int response_code = 0;
		HTTPClient::Status last_status = HTTPClient::STATUS_DISCONNECTED;
		while (OS::get_singleton()->get_ticks_msec() < deadline) {
			if (p_cancel_requested && p_cancel_requested->is_set()) {
				Dictionary out;
				out["ok"] = false;
				out["error"] = _error_data("HTTP_CANCELLED", "HTTP request was cancelled.");
				return out;
			}
			http->poll();
			const HTTPClient::Status status = http->get_status();
			last_status = status;
			if (status == HTTPClient::STATUS_RESOLVING || status == HTTPClient::STATUS_CONNECTING || status == HTTPClient::STATUS_REQUESTING) {
				OS::get_singleton()->delay_usec(10000);
				continue;
			}
			if (status == HTTPClient::STATUS_CONNECTED) {
				if (!requested) {
					const HTTPClient::Method method = p_method == "GET" ? HTTPClient::METHOD_GET : HTTPClient::METHOD_POST;
					err = http->request(method, path, p_headers, p_body.ptr(), p_body.size());
					if (err != OK) {
						Dictionary out;
						out["ok"] = false;
						out["error"] = _error_data("REQUEST_FAILED", vformat("HTTP request failed (error %d).", err));
						out["url"] = current_url;
						return out;
					}
					requested = true;
				} else if (reading) {
					break;
				}
				OS::get_singleton()->delay_usec(10000);
				continue;
			}
			if (status == HTTPClient::STATUS_BODY) {
				reading = true;
				response_code = http->get_response_code();
				if (!response_headers_read) {
					http->get_response_headers(&response_headers);
					response_headers_read = true;
				}
				const PackedByteArray chunk = http->read_response_body_chunk();
				if (!chunk.is_empty()) {
					body.append_array(chunk);
					if (p_max_body_bytes >= 0 && body.size() > p_max_body_bytes) {
						Dictionary out;
						out["ok"] = false;
						out["error"] = _solers_http_error("HTTP_BODY_TOO_LARGE", "HTTP response body exceeded the local size limit.", current_url, status, response_code, response_headers);
						return out;
					}
				}
				OS::get_singleton()->delay_usec(5000);
				continue;
			}
			if (status == HTTPClient::STATUS_DISCONNECTED) {
				disconnected = true;
				break;
			}
			Dictionary out;
			out["ok"] = false;
			out["error"] = _solers_http_error("HTTP_STATUS_ERROR", "HTTP connection failed.", current_url, status, response_code, response_headers);
			return out;
		}

		if (!disconnected && OS::get_singleton()->get_ticks_msec() >= deadline) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _solers_http_error("HTTP_TIMEOUT", "HTTP request timed out.", current_url, last_status, response_code, response_headers);
			return out;
		}

		if (response_code == 301 || response_code == 302 || response_code == 303 || response_code == 307 || response_code == 308) {
			const String location = _solers_header_value(response_headers, "Location");
			if (location.is_empty()) {
				Dictionary out;
				out["ok"] = false;
				out["status"] = response_code;
				out["url"] = current_url;
				Dictionary error = _solers_http_error("HTTP_REDIRECT_MISSING_LOCATION", "HTTP redirect did not include a Location header.", current_url, last_status, response_code, response_headers);
				out["error"] = error;
				return out;
			}
			String next_url = location;
			if (next_url.begins_with("/")) {
				next_url = _solers_url_origin(scheme, host, port) + next_url;
			} else if (!next_url.begins_with("http://") && !next_url.begins_with("https://")) {
				next_url = _solers_url_origin(scheme, host, port) + path.get_base_dir().path_join(next_url);
			}
			if (_solers_has_secret_header(p_headers) && !_solers_same_origin(current_url, next_url)) {
				Dictionary out;
				out["ok"] = false;
				out["status"] = response_code;
				out["url"] = current_url;
				out["redirected_to"] = next_url;
				Dictionary error = _solers_http_error("HTTP_REDIRECT_CROSS_ORIGIN", "Provider redirected to another origin while sensitive headers were present.", current_url, last_status, response_code, response_headers);
				error["location"] = location;
				error["redirected_to"] = next_url;
				out["error"] = error;
				return out;
			}
			redirected_from = current_url;
			current_url = next_url;
			continue;
		}

		if (p_retry_count < 2 && ((p_method == "GET" && (response_code == 429 || response_code == 503)) || (p_method == "POST" && response_code == 429))) {
			const int retry_seconds = _solers_header_value(response_headers, "Retry-After").to_int();
			if (retry_seconds > 0 && retry_seconds <= 60) {
				const uint64_t retry_at = OS::get_singleton()->get_ticks_msec() + (uint64_t)retry_seconds * 1000;
				while (OS::get_singleton()->get_ticks_msec() < retry_at) {
					if (p_cancel_requested && p_cancel_requested->is_set()) {
						Dictionary cancelled;
						cancelled["ok"] = false;
						cancelled["error"] = _error_data("HTTP_CANCELLED", "HTTP retry was cancelled.");
						return cancelled;
					}
					OS::get_singleton()->delay_usec(50000);
				}
				return _http_request(p_method, p_url, p_headers, p_body, p_timeout_msec, p_max_body_bytes, p_cancel_requested, p_retry_count + 1);
			}
		}

		Dictionary out;
		out["ok"] = response_code >= 200 && response_code < 300;
		out["status"] = response_code;
		out["url"] = current_url;
		if (!redirected_from.is_empty()) {
			out["redirected_from"] = redirected_from;
			out["redirected_to"] = current_url;
		}
		out["body"] = body;
		if (!(bool)out["ok"]) {
			Dictionary error = _solers_http_error("HTTP_ERROR", String::utf8((const char *)body.ptr(), MIN(body.size(), 2048)), current_url, last_status, response_code, response_headers);
			if (!redirected_from.is_empty()) {
				error["redirected_from"] = redirected_from;
				error["redirected_to"] = current_url;
			}
			out["error"] = error;
		}
		return out;
	}

	Dictionary out;
	out["ok"] = false;
	out["error"] = _error_data("HTTP_REDIRECT_LIMIT", "HTTP redirect limit exceeded.");
	return out;
}

bool SolersAssetService::_write_json_atomic(const String &p_path, const Dictionary &p_data, String &r_error) {
	MutexLock storage_lock(solers_asset_manifest_mutex);
	const String dir = p_path.get_base_dir();
	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(dir));
	if (dir_err != OK) {
		r_error = "Failed to create asset directory.";
		return false;
	}
	const String tmp = p_path + ".tmp";
	Ref<FileAccess> file = FileAccess::open(tmp, FileAccess::WRITE);
	if (file.is_null()) {
		r_error = "Failed to open asset manifest.";
		return false;
	}
	file->store_string(JSON::stringify(p_data, "", false, true));
	file->flush();
	file.unref();
	if (DirAccess::rename_absolute(ProjectSettings::get_singleton()->globalize_path(tmp), ProjectSettings::get_singleton()->globalize_path(p_path)) != OK) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(tmp));
		r_error = "Failed to commit asset manifest.";
		return false;
	}
	return true;
}

Dictionary SolersAssetService::_read_json_file(const String &p_path) {
	MutexLock storage_lock(solers_asset_manifest_mutex);
	if (!FileAccess::exists(p_path)) {
		return Dictionary();
	}
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(FileAccess::get_file_as_string(p_path)) != OK || json->get_data().get_type() != Variant::DICTIONARY) {
		return Dictionary();
	}
	return json->get_data();
}

bool SolersAssetService::_write_bytes_atomic(const String &p_path, const PackedByteArray &p_bytes, String &r_error) {
	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(p_path.get_base_dir()));
	if (dir_err != OK) {
		r_error = vformat("Failed to create asset file directory '%s' (error %d).", p_path.get_base_dir(), dir_err);
		return false;
	}
	const String tmp = p_path + ".tmp";
	Ref<FileAccess> file = FileAccess::open(tmp, FileAccess::WRITE);
	if (file.is_null()) {
		r_error = vformat("Failed to open asset file '%s'.", tmp);
		return false;
	}
	if (!p_bytes.is_empty()) {
		file->store_buffer(p_bytes.ptr(), p_bytes.size());
	}
	file.unref();
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
	const Error rename_err = DirAccess::rename_absolute(ProjectSettings::get_singleton()->globalize_path(tmp), ProjectSettings::get_singleton()->globalize_path(p_path));
	if (rename_err != OK) {
		r_error = vformat("Failed to commit asset file '%s' (error %d).", p_path, rename_err);
		return false;
	}
	return true;
}

bool SolersAssetService::_copy_file(const String &p_from, const String &p_to, String &r_error) {
	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(p_to.get_base_dir()));
	if (dir_err != OK) {
		r_error = "Failed to create target directory.";
		return false;
	}
	Error copy_err = DirAccess::copy_absolute(ProjectSettings::get_singleton()->globalize_path(p_from), ProjectSettings::get_singleton()->globalize_path(p_to));
	if (copy_err != OK) {
		r_error = vformat("Failed to copy %s.", p_from);
		return false;
	}
	return true;
}

bool SolersAssetService::_remove_dir_recursive(const String &p_path, String &r_error) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null()) {
		r_error = vformat("Failed to open directory: %s", p_path);
		return false;
	}
	const PackedStringArray files = dir->get_files();
	for (int i = 0; i < files.size(); i++) {
		const String path = p_path.path_join(files[i]);
		const Error err = DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(path));
		if (err != OK) {
			r_error = vformat("Failed to remove file: %s", path);
			return false;
		}
	}
	const PackedStringArray dirs = dir->get_directories();
	for (int i = 0; i < dirs.size(); i++) {
		const String path = p_path.path_join(dirs[i]);
		if (!_remove_dir_recursive(path, r_error)) {
			return false;
		}
	}
	const Error err = DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
	if (err != OK) {
		r_error = vformat("Failed to remove directory: %s", p_path);
		return false;
	}
	return true;
}

static Vector<String> _polyhaven_headers();

static Error _load_image_from_signature(const PackedByteArray &p_bytes, const Ref<Image> &p_image) {
	if (p_bytes.size() >= 8 && p_bytes[0] == 0x89 && p_bytes[1] == 0x50 && p_bytes[2] == 0x4e && p_bytes[3] == 0x47 && p_bytes[4] == 0x0d && p_bytes[5] == 0x0a && p_bytes[6] == 0x1a && p_bytes[7] == 0x0a) {
		return p_image->load_png_from_buffer(p_bytes);
	}
	if (p_bytes.size() >= 3 && p_bytes[0] == 0xff && p_bytes[1] == 0xd8 && p_bytes[2] == 0xff) {
		return p_image->load_jpg_from_buffer(p_bytes);
	}
	if (p_bytes.size() >= 12 && p_bytes[0] == 'R' && p_bytes[1] == 'I' && p_bytes[2] == 'F' && p_bytes[3] == 'F' && p_bytes[8] == 'W' && p_bytes[9] == 'E' && p_bytes[10] == 'B' && p_bytes[11] == 'P') {
		return p_image->load_webp_from_buffer(p_bytes);
	}
	return ERR_FILE_UNRECOGNIZED;
}

void SolersAssetService::_download_preview(Task *p_task, Dictionary &r_state, const String &p_url) {
	if (p_url.is_empty()) {
		return;
	}
	const Vector<String> headers = String(r_state.get("provider", String())) == "polyhaven" ? _polyhaven_headers() : Vector<String>();
	Dictionary response = _http_request("GET", p_url, headers, PackedByteArray(), 60000, 2 * 1024 * 1024, &p_task->abort);
	if (!(bool)response.get("ok", false)) {
		r_state["preview_error"] = response.get("error", Dictionary());
		_set_task_state(p_task, r_state);
		return;
	}

	const PackedByteArray bytes = response.get("body", PackedByteArray());
	Ref<Image> image;
	image.instantiate();
	const Error err = _load_image_from_signature(bytes, image);
	if (err != OK || image->is_empty()) {
		r_state["preview_error"] = _error_data("PREVIEW_DECODE_FAILED", "Provider thumbnail could not be decoded.");
		_set_task_state(p_task, r_state);
		return;
	}

	const int max_side = 512;
	const int width = image->get_width();
	const int height = image->get_height();
	if (width > max_side || height > max_side) {
		const float scale = MIN((float)max_side / (float)width, (float)max_side / (float)height);
		image->resize(MAX(1, (int)Math::round(width * scale)), MAX(1, (int)Math::round(height * scale)), Image::INTERPOLATE_LANCZOS);
	}
	image->convert(Image::FORMAT_RGB8);

	PackedByteArray preview = image->save_jpg_to_buffer(0.72f);
	if (preview.size() > 500 * 1024) {
		preview = image->save_jpg_to_buffer(0.55f);
	}
	if (preview.size() > 500 * 1024) {
		image->resize(MAX(1, image->get_width() * 3 / 4), MAX(1, image->get_height() * 3 / 4), Image::INTERPOLATE_LANCZOS);
		preview = image->save_jpg_to_buffer(0.45f);
	}
	if (preview.is_empty()) {
		r_state["preview_error"] = _error_data("PREVIEW_ENCODE_FAILED", "Provider thumbnail could not be encoded.");
		_set_task_state(p_task, r_state);
		return;
	}
	if (preview.size() > 500 * 1024) {
		r_state["preview_error"] = _error_data("PREVIEW_TOO_LARGE", "Provider thumbnail stayed above 500KB after resizing.");
		_set_task_state(p_task, r_state);
		return;
	}

	const String preview_path = _asset_dir(p_task->asset_id).path_join("preview.jpg");
	String write_error;
	if (!_write_bytes_atomic(preview_path, preview, write_error)) {
		r_state["preview_error"] = _error_data("PREVIEW_WRITE_FAILED", write_error);
		_set_task_state(p_task, r_state);
		return;
	}
	r_state["preview_file"] = preview_path;
	r_state.erase("preview_error");
	_set_task_state(p_task, r_state);
}

Array SolersAssetService::_meshy_animation_actions() {
	Array candidates;
	candidates.push_back("res://modules/solers_ai/data/meshy_animation_actions.json");
	const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	candidates.push_back(exe_dir.path_join("modules/solers_ai/data/meshy_animation_actions.json").simplify_path());
	candidates.push_back(exe_dir.path_join("../modules/solers_ai/data/meshy_animation_actions.json").simplify_path());
	candidates.push_back(exe_dir.path_join("../../modules/solers_ai/data/meshy_animation_actions.json").simplify_path());

	String json;
	for (int i = 0; i < candidates.size(); i++) {
		const String path = String(candidates[i]);
		if (FileAccess::exists(path)) {
			json = FileAccess::get_file_as_string(path);
			break;
		}
	}
	const Variant parsed = JSON::parse_string(json);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return Array();
	}
	return Dictionary(parsed).get("actions", Array());
}

bool SolersAssetService::_meshy_animation_action_exists(int64_t p_action_id) {
	const Array actions = _meshy_animation_actions();
	for (int i = 0; i < actions.size(); i++) {
		const Dictionary action = actions[i];
		if ((int64_t)action.get("action_id", -1) == p_action_id) {
			return true;
		}
	}
	return false;
}

void SolersAssetService::_set_task_state(Task *p_task, const Dictionary &p_state) {
	{
		MutexLock lock(p_task->mutex);
		p_task->state = p_state.duplicate(true);
	}
	String error;
	_write_json_atomic(_manifest_path(p_task->asset_id), p_state, error);
}

Dictionary SolersAssetService::_task_state(Task *p_task) {
	MutexLock lock(p_task->mutex);
	return p_task->state.duplicate(true);
}

void SolersAssetService::_task_func(void *p_userdata) {
	Task *task = static_cast<Task *>(p_userdata);
	_run_task(task);
	task->done.set();
	if (task->service) {
		task->service->_queue_terminal_event(task);
	}
}

void SolersAssetService::_queue_terminal_event(Task *p_task) {
	Dictionary state = _task_state(p_task);
	const String status = String(state.get("status", String())).to_lower();
	if ((status != "ready" && status != "draft" && status != "failed" && status != "cancelled" && status != "interrupted") || String(state.get("session_id", String())).is_empty()) {
		return;
	}
	if (String(state.get("delivery_status", String())) != "delivered") {
		state["delivery_status"] = "pending";
		_set_task_state(p_task, state);
	}
	MutexLock lock(terminal_events_mutex);
	terminal_events.push_back(state);
}

static PackedByteArray _utf8_bytes(const String &p_text) {
	CharString utf8 = p_text.utf8();
	PackedByteArray bytes;
	bytes.resize(utf8.length());
	if (utf8.length() > 0) {
		memcpy(bytes.ptrw(), utf8.get_data(), utf8.length());
	}
	return bytes;
}

static Dictionary _parse_json_body(const Dictionary &p_response) {
	const PackedByteArray body = p_response.get("body", PackedByteArray());
	const Variant parsed = JSON::parse_string(String::utf8((const char *)body.ptr(), body.size()));
	return parsed.get_type() == Variant::DICTIONARY ? (Dictionary)parsed : Dictionary();
}

Dictionary SolersAssetService::_ambientcg_asset_metadata(const String &p_asset_id, const SafeFlag *p_cancel_requested) {
	const String url = "https://ambientcg.com/api/v3/assets?id=" + p_asset_id.uri_encode() + "&limit=1&include=id,type,title,url,tags,dimensions,maps,downloads,thumbnails";
	Dictionary response = _http_request("GET", url, Vector<String>(), PackedByteArray(), 60000, 4 * 1024 * 1024, p_cancel_requested);
	if (!(bool)response.get("ok", false)) {
		return response;
	}
	const Dictionary root = _parse_json_body(response);
	const Array assets = root.get("assets", Array());
	for (int i = 0; i < assets.size(); i++) {
		const Dictionary asset = assets[i];
		if (String(asset.get("id", String())).nocasecmp_to(p_asset_id) == 0) {
			response["asset"] = asset;
			return response;
		}
	}
	response["ok"] = false;
	response["error"] = _error_data("AMBIENTCG_NOT_FOUND", "ambientCG did not return the requested asset id.");
	return response;
}

static Dictionary _ambientcg_download_package(const Dictionary &p_asset, const String &p_package) {
	const Array downloads = p_asset.get("downloads", Array());
	for (int i = 0; i < downloads.size(); i++) {
		const Dictionary download = downloads[i];
		if (String(download.get("attributes", String())).nocasecmp_to(p_package) == 0 && String(download.get("extension", String())).to_lower() == "zip") {
			return download;
		}
	}
	return Dictionary();
}

static Vector<String> _polyhaven_headers() {
	Vector<String> headers;
	headers.push_back("User-Agent: Solers Engine development (+https://github.com/brandlll-lee/SolersEngine)");
	headers.push_back("Referer: https://github.com/brandlll-lee/SolersEngine");
	return headers;
}

Dictionary SolersAssetService::_polyhaven_asset_metadata(const String &p_asset_id, const SafeFlag *p_cancel_requested) {
	const Vector<String> headers = _polyhaven_headers();
	Dictionary info_response = _http_request("GET", "https://api.polyhaven.com/info/" + p_asset_id.uri_encode(), headers, PackedByteArray(), 60000, 2 * 1024 * 1024, p_cancel_requested);
	if (!(bool)info_response.get("ok", false)) {
		return info_response;
	}
	Dictionary files_response = _http_request("GET", "https://api.polyhaven.com/files/" + p_asset_id.uri_encode(), headers, PackedByteArray(), 60000, 8 * 1024 * 1024, p_cancel_requested);
	if (!(bool)files_response.get("ok", false)) {
		return files_response;
	}
	const Dictionary asset = _parse_json_body(info_response);
	const Dictionary files = _parse_json_body(files_response);
	if (asset.is_empty() || files.is_empty()) {
		Dictionary response;
		response["ok"] = false;
		response["error"] = _error_data("POLYHAVEN_BAD_RESPONSE", "Poly Haven did not return complete asset metadata.");
		return response;
	}
	Dictionary response;
	response["ok"] = true;
	response["asset"] = asset;
	response["files"] = files;
	return response;
}

bool SolersAssetService::_polyhaven_download_file(Task *p_task, const String &p_label, const Dictionary &p_spec, Array &r_files, Dictionary &r_checksums, String &r_error, const String &p_relative_path) {
	const String url = p_spec.get("url", String());
	const String expected_md5 = String(p_spec.get("md5", String())).to_lower();
	const int64_t expected_size = (int64_t)p_spec.get("size", -1);
	if (url.is_empty() || expected_md5.is_empty() || expected_size < 0) {
		r_error = vformat("Poly Haven metadata for %s is incomplete.", p_label);
		return false;
	}
	const Dictionary response = _http_request("GET", url, _polyhaven_headers(), PackedByteArray(), 180000, expected_size + 1, &p_task->abort);
	if (!(bool)response.get("ok", false)) {
		r_error = Dictionary(response.get("error", Dictionary())).get("message", vformat("Poly Haven download failed for %s.", p_label));
		return false;
	}
	const PackedByteArray body = response.get("body", PackedByteArray());
	if (body.size() != expected_size) {
		r_error = vformat("Poly Haven download size mismatch for %s.", p_label);
		return false;
	}
	const String source_root = _source_dir(p_task->asset_id).simplify_path();
	String file_path;
	if (!p_relative_path.is_empty()) {
		const String relative_path = p_relative_path.replace_char('\\', '/').simplify_path();
		file_path = source_root.path_join(relative_path).simplify_path();
		if (relative_path.is_empty() || relative_path.is_absolute_path() || !file_path.begins_with(source_root + "/")) {
			r_error = vformat("Poly Haven metadata for %s contains an unsafe package path.", p_label);
			return false;
		}
	} else {
		const String extension = url.get_slice("?", 0).get_extension().to_lower();
		if (extension.is_empty()) {
			r_error = vformat("Poly Haven download for %s has no file extension.", p_label);
			return false;
		}
		file_path = source_root.path_join(_safe_slug(p_label) + "." + extension);
	}
	if (!_write_bytes_atomic(file_path, body, r_error)) {
		return false;
	}
	const String actual_md5 = FileAccess::get_md5(file_path).to_lower();
	if (actual_md5 != expected_md5) {
		r_error = vformat("Poly Haven checksum mismatch for %s.", p_label);
		return false;
	}
	r_files.push_back(file_path);
	r_checksums[file_path] = actual_md5;
	return true;
}

static bool _polyhaven_file_spec_valid(const Dictionary &p_spec) {
	return !String(p_spec.get("url", String())).is_empty() && !String(p_spec.get("md5", String())).is_empty() && (int64_t)p_spec.get("size", -1) >= 0;
}

static int64_t _polyhaven_file_spec_size(const Dictionary &p_spec) {
	int64_t size = (int64_t)p_spec.get("size", 0);
	const Dictionary includes = p_spec.get("include", Dictionary());
	for (const Variant *key = includes.next(nullptr); key; key = includes.next(key)) {
		size += (int64_t)Dictionary(includes[*key]).get("size", 0);
	}
	return size;
}

Array SolersAssetService::normalize_polyhaven_variants(const Dictionary &p_files, const String &p_kind) {
	const String kind = p_kind.to_lower();
	Dictionary by_id;
	if (kind == "3d") {
		for (const Variant *format_key = p_files.next(nullptr); format_key; format_key = p_files.next(format_key)) {
			const String format = String(*format_key).to_lower();
			const Dictionary resolutions = p_files[*format_key];
			for (const Variant *resolution_key = resolutions.next(nullptr); resolution_key; resolution_key = resolutions.next(resolution_key)) {
				const Dictionary formats = resolutions[*resolution_key];
				const Dictionary spec = formats.get(*format_key, Dictionary());
				if (!_polyhaven_file_spec_valid(spec)) {
					continue;
				}
				Dictionary variant;
				variant["id"] = String(*resolution_key).to_lower() + "-" + format;
				variant["resolution"] = String(*resolution_key);
				variant["format"] = String(*format_key);
				variant["size"] = _polyhaven_file_spec_size(spec);
				variant["checksum"] = spec.get("md5", String());
				Array dependencies = Dictionary(spec.get("include", Dictionary())).keys();
				dependencies.sort();
				variant["dependencies"] = dependencies;
				by_id[variant["id"]] = variant;
			}
		}
	} else if (kind == "hdri") {
		const Dictionary resolutions = p_files.get("hdri", Dictionary());
		for (const Variant *resolution_key = resolutions.next(nullptr); resolution_key; resolution_key = resolutions.next(resolution_key)) {
			const Dictionary formats = resolutions[*resolution_key];
			for (const Variant *format_key = formats.next(nullptr); format_key; format_key = formats.next(format_key)) {
				const Dictionary spec = formats[*format_key];
				if (!_polyhaven_file_spec_valid(spec)) {
					continue;
				}
				Dictionary variant;
				variant["id"] = String(*resolution_key).to_lower() + "-" + String(*format_key).to_lower();
				variant["resolution"] = String(*resolution_key);
				variant["format"] = String(*format_key);
				variant["size"] = _polyhaven_file_spec_size(spec);
				variant["checksum"] = spec.get("md5", String());
				by_id[variant["id"]] = variant;
			}
		}
	} else if (kind == "material") {
		for (const Variant *map_key = p_files.next(nullptr); map_key; map_key = p_files.next(map_key)) {
			if (String(*map_key) == "hdri") {
				continue;
			}
			const Dictionary resolutions = p_files[*map_key];
			for (const Variant *resolution_key = resolutions.next(nullptr); resolution_key; resolution_key = resolutions.next(resolution_key)) {
				const Dictionary formats = resolutions[*resolution_key];
				for (const Variant *format_key = formats.next(nullptr); format_key; format_key = formats.next(format_key)) {
					const Dictionary spec = formats[*format_key];
					if (spec.has("include") || !_polyhaven_file_spec_valid(spec)) {
						continue;
					}
					const String id = String(*resolution_key).to_lower() + "-" + String(*format_key).to_lower();
					Dictionary variant = by_id.get(id, Dictionary());
					variant["id"] = id;
					variant["resolution"] = String(*resolution_key);
					variant["format"] = String(*format_key);
					variant["size"] = (int64_t)variant.get("size", 0) + (int64_t)spec.get("size", 0);
					Array maps = variant.get("maps", Array());
					maps.push_back(String(*map_key));
					maps.sort();
					variant["maps"] = maps;
					Dictionary checksums = variant.get("checksums", Dictionary());
					checksums[String(*map_key)] = spec.get("md5", String());
					variant["checksums"] = checksums;
					by_id[id] = variant;
				}
			}
		}
	}

	Array ids = by_id.keys();
	ids.sort();
	Array variants;
	for (int i = 0; i < ids.size(); i++) {
		variants.push_back(by_id[ids[i]]);
	}
	return variants;
}

String SolersAssetService::match_catalog_variant_id(const Array &p_variants, const String &p_id) {
	for (int i = 0; i < p_variants.size(); i++) {
		const String official_id = Dictionary(p_variants[i]).get("id", String());
		if (official_id.nocasecmp_to(p_id) == 0) {
			return official_id;
		}
	}
	return String();
}

bool SolersAssetService::_polyhaven_download_variant(Task *p_task, const Dictionary &p_files, const String &p_kind, const String &p_variant, Array &r_files, Dictionary &r_maps, Dictionary &r_checksums, String &r_error) {
	const Array available_variants = normalize_polyhaven_variants(p_files, p_kind);
	if (match_catalog_variant_id(available_variants, p_variant).is_empty()) {
		r_error = "The selected Poly Haven variant is not present in the asset's official files metadata.";
		return false;
	}
	const PackedStringArray variant_parts = p_variant.to_lower().split("-", false, 1);
	if (variant_parts.size() != 2) {
		r_error = "Poly Haven variant must use the official resolution-format form, such as 2k-jpg or 2k-hdr.";
		return false;
	}
	const String resolution = variant_parts[0];
	const String format = variant_parts[1];
	if (p_kind == "3d") {
		const Dictionary resolutions = p_files.get(format, Dictionary());
		const Dictionary formats = resolutions.get(resolution, Dictionary());
		const Dictionary spec = formats.get(format, Dictionary());
		const String primary_name = String(spec.get("url", String())).get_slice("?", 0).get_file();
		if (spec.is_empty() || primary_name.is_empty()) {
			r_error = "The selected Poly Haven model variant is not available.";
			return false;
		}
		if (!_polyhaven_download_file(p_task, p_task->asset_id + "-model-" + p_variant, spec, r_files, r_checksums, r_error, primary_name)) {
			return false;
		}
		const String model_path = r_files[r_files.size() - 1];
		const Dictionary includes = spec.get("include", Dictionary());
		Array include_paths = includes.keys();
		include_paths.sort();
		for (int i = 0; i < include_paths.size(); i++) {
			const String relative_path = include_paths[i];
			const Dictionary include_spec = includes[include_paths[i]];
			if (!_polyhaven_download_file(p_task, p_task->asset_id + "-" + relative_path, include_spec, r_files, r_checksums, r_error, relative_path)) {
				return false;
			}
		}
		r_maps["model"] = model_path;
		return true;
	}
	if (p_kind == "hdri") {
		const Dictionary resolutions = p_files.get("hdri", Dictionary());
		const Dictionary formats = resolutions.get(resolution, Dictionary());
		const Dictionary spec = formats.get(format, Dictionary());
		if (spec.is_empty()) {
			r_error = "The selected Poly Haven HDRI variant is not available.";
			return false;
		}
		if (!_polyhaven_download_file(p_task, p_task->asset_id + "-hdri-" + p_variant, spec, r_files, r_checksums, r_error)) {
			return false;
		}
		r_maps["hdri"] = r_files[r_files.size() - 1];
		return true;
	}

	for (const Variant *map_key = p_files.next(nullptr); map_key; map_key = p_files.next(map_key)) {
		const String map_name = String(*map_key);
		if (map_name == "hdri") {
			continue;
		}
		const Dictionary resolutions = p_files[*map_key];
		const Dictionary formats = resolutions.get(resolution, Dictionary());
		const Dictionary spec = formats.get(format, Dictionary());
		if (spec.is_empty() || spec.has("include") || !spec.has("url") || !spec.has("md5") || !spec.has("size")) {
			continue;
		}
		if (!_polyhaven_download_file(p_task, p_task->asset_id + "-" + map_name + "-" + p_variant, spec, r_files, r_checksums, r_error)) {
			return false;
		}
		r_maps[map_name] = r_files[r_files.size() - 1];
	}
	if (r_files.is_empty()) {
		r_error = "The selected Poly Haven material variant has no supported PBR maps.";
		return false;
	}
	return true;
}

bool SolersAssetService::_ambientcg_extract_archive(Task *p_task, const PackedByteArray &p_archive, Array &r_files, String &r_error) {
	const String archive_path = _source_dir(p_task->asset_id).path_join("ambientcg.zip");
	if (!_write_bytes_atomic(archive_path, p_archive, r_error)) {
		return false;
	}
	Ref<ZIPReader> reader;
	reader.instantiate();
	if (reader->open(archive_path) != OK) {
		r_error = "ambientCG download is not a readable ZIP archive.";
		return false;
	}
	const String source_root = _source_dir(p_task->asset_id).simplify_path();
	const PackedStringArray entries = reader->get_files();
	for (int i = 0; i < entries.size(); i++) {
		if (p_task->abort.is_set()) {
			r_error = "ambientCG extraction was cancelled.";
			reader->close();
			return false;
		}
		const String entry = entries[i];
		if (entry.is_empty() || entry.ends_with("/") || entry.begins_with("/") || entry.contains("\\")) {
			continue;
		}
		const PackedStringArray parts = entry.split("/", false);
		bool unsafe = false;
		for (int part = 0; part < parts.size(); part++) {
			if (parts[part] == "." || parts[part] == "..") {
				unsafe = true;
				break;
			}
		}
		const String output_path = source_root.path_join(entry).simplify_path();
		if (unsafe || !output_path.begins_with(source_root + "/")) {
			r_error = "ambientCG archive contains an unsafe path.";
			reader->close();
			return false;
		}
		const PackedByteArray bytes = reader->read_file(entry, true);
		if (bytes.is_empty()) {
			continue;
		}
		if (!_write_bytes_atomic(output_path, bytes, r_error)) {
			reader->close();
			return false;
		}
		r_files.push_back(output_path);
	}
	reader->close();
	if (r_files.is_empty()) {
		r_error = "ambientCG archive did not contain importable files.";
		return false;
	}
	return true;
}

static void _merge_options(Dictionary &r_body, const Dictionary &p_options) {
	for (const Variant *K = p_options.next(nullptr); K; K = p_options.next(K)) {
		r_body[*K] = p_options[*K];
	}
}

static Dictionary _select_options(const Dictionary &p_options, const char *const *p_names, int p_name_count) {
	Dictionary selected;
	for (int i = 0; i < p_name_count; i++) {
		const StringName name = p_names[i];
		if (p_options.has(name)) {
			selected[name] = p_options[name];
		}
	}
	return selected;
}

static Dictionary _meshy_text_preview_options(const Dictionary &p_options) {
	const char *const names[] = {
		"model_type", "ai_model", "should_remesh", "topology", "target_polycount",
		"decimation_mode", "pose_mode", "moderation", "target_formats", "alpha_thumbnail",
		"auto_size", "origin_at"
	};
	return _select_options(p_options, names, sizeof(names) / sizeof(names[0]));
}

static Dictionary _meshy_text_refine_options(const Dictionary &p_options) {
	const char *const names[] = {
		"enable_pbr", "hd_texture", "texture_prompt", "texture_image_url", "ai_model",
		"moderation", "remove_lighting", "target_formats", "alpha_thumbnail", "auto_size", "origin_at"
	};
	return _select_options(p_options, names, sizeof(names) / sizeof(names[0]));
}

static Array _native_resource_entrypoints(const Array &p_files) {
	Array entrypoints;
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	for (int i = 0; i < p_files.size(); i++) {
		const String path = p_files[i];
		if (importer && importer->get_importer_by_file(path).is_valid()) {
			continue;
		}
		if (ResourceLoader::get_resource_type(path) != StringName()) {
			entrypoints.push_back(path);
		}
	}
	return entrypoints;
}

static Array _model_resource_entrypoints(const Array &p_files) {
	Array entrypoints;
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	for (int i = 0; i < p_files.size(); i++) {
		const String path = p_files[i];
		const Ref<ResourceImporter> source_importer = importer ? importer->get_importer_by_file(path) : Ref<ResourceImporter>();
		const StringName type = source_importer.is_valid() ? source_importer->get_resource_type() : ResourceLoader::get_resource_type(path);
		if (type == SNAME("PackedScene") || (ClassDB::class_exists(type) && ClassDB::is_parent_class(type, SNAME("Mesh")))) {
			entrypoints.push_back(path);
		}
	}
	return entrypoints;
}

static Dictionary _project_import_selection(const Array &p_files, const Array &p_declared_import_files, const Array &p_declared_entrypoints) {
	HashMap<String, String> available;
	Array importable_files;
	Vector<String> discovered_entrypoints;
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	HashSet<String> seen_files;
	for (int i = 0; i < p_files.size(); i++) {
		const String path = String(p_files[i]).replace_char('\\', '/').simplify_path();
		if (path.is_empty() || seen_files.has(path)) {
			Dictionary selection;
			selection["valid"] = false;
			selection["error"] = vformat("Asset files contains an empty or duplicate path: %s", path);
			return selection;
		}
		seen_files.insert(path);
		available[path] = String(p_files[i]);
		const Ref<ResourceImporter> source_importer = importer ? importer->get_importer_by_file(path) : Ref<ResourceImporter>();
		const StringName type = source_importer.is_valid() ? source_importer->get_resource_type() : ResourceLoader::get_resource_type(path);
		if (source_importer.is_valid() || type != StringName()) {
			importable_files.push_back(p_files[i]);
		}
		if (type != StringName() && ClassDB::class_exists(type) && ClassDB::is_parent_class(type, SNAME("Material"))) {
			discovered_entrypoints.push_back(path);
		}
	}

	if (!p_declared_import_files.is_empty()) {
		if (p_declared_entrypoints.is_empty()) {
			Dictionary selection;
			selection["valid"] = false;
			selection["error"] = "Declared import_files requires at least one entrypoint.";
			return selection;
		}
		HashSet<String> selected;
		for (int i = 0; i < p_declared_import_files.size(); i++) {
			const String path = String(p_declared_import_files[i]).replace_char('\\', '/').simplify_path();
			if (!available.has(path) || selected.has(path)) {
				Dictionary selection;
				selection["valid"] = false;
				selection["error"] = vformat("Declared import file is missing or duplicated: %s", path);
				return selection;
			}
			selected.insert(path);
		}
		Array entrypoints;
		HashSet<String> declared_entrypoints;
		for (int i = 0; i < p_declared_entrypoints.size(); i++) {
			const String path = String(p_declared_entrypoints[i]).replace_char('\\', '/').simplify_path();
			if (!selected.has(path) || declared_entrypoints.has(path)) {
				Dictionary selection;
				selection["valid"] = false;
				selection["error"] = vformat("Declared entrypoint is not a unique import file: %s", path);
				return selection;
			}
			declared_entrypoints.insert(path);
			entrypoints.push_back(available[path]);
		}
		Array import_files;
		for (int i = 0; i < p_files.size(); i++) {
			const String path = String(p_files[i]).replace_char('\\', '/').simplify_path();
			if (selected.has(path)) {
				import_files.push_back(p_files[i]);
			}
		}
		Dictionary selection;
		selection["valid"] = true;
		selection["files"] = import_files;
		selection["entrypoints"] = entrypoints;
		return selection;
	}

	Vector<String> pending;
	Array entrypoints;
	if (!p_declared_entrypoints.is_empty()) {
		HashSet<String> declared_paths;
		for (int i = 0; i < p_declared_entrypoints.size(); i++) {
			const String path = String(p_declared_entrypoints[i]).replace_char('\\', '/').simplify_path();
			if (!available.has(path) || declared_paths.has(path)) {
				Dictionary selection;
				selection["valid"] = false;
				selection["error"] = vformat("Declared asset entrypoint is missing or duplicated: %s", path);
				return selection;
			}
			declared_paths.insert(path);
			entrypoints.push_back(available[path]);
			pending.push_back(path);
		}
	} else {
		pending = discovered_entrypoints;
		for (int i = 0; i < pending.size(); i++) {
			entrypoints.push_back(available[pending[i]]);
		}
	}
	if (entrypoints.is_empty()) {
		Dictionary selection;
		selection["valid"] = true;
		selection["files"] = importable_files;
		selection["entrypoints"] = importable_files;
		return selection;
	}

	HashSet<String> selected;
	for (int i = 0; i < pending.size(); i++) {
		const String owner = pending[i];
		if (selected.has(owner)) {
			continue;
		}
		selected.insert(owner);
		List<String> dependencies;
		ResourceLoader::get_dependencies(owner, &dependencies, true);
		for (const String &spec : dependencies) {
			String dependency = spec.get_slice("::", 0);
			if (dependency.begins_with("uid://") && spec.get_slice_count("::") >= 3) {
				dependency = spec.get_slice("::", 2);
			}
			if (dependency.is_relative_path()) {
				dependency = owner.get_base_dir().path_join(dependency);
			}
			dependency = dependency.replace_char('\\', '/').simplify_path();
			if (!available.has(dependency)) {
				Dictionary selection;
				selection["valid"] = false;
				selection["error"] = vformat("Asset entrypoint dependency is missing from files: %s", dependency);
				return selection;
			}
			pending.push_back(dependency);
		}
	}

	Array import_files;
	for (int i = 0; i < p_files.size(); i++) {
		const String path = String(p_files[i]).replace_char('\\', '/').simplify_path();
		if (selected.has(path)) {
			import_files.push_back(p_files[i]);
		}
	}
	Dictionary selection;
	selection["valid"] = true;
	selection["files"] = import_files;
	selection["entrypoints"] = entrypoints;
	return selection;
}

static String _project_import_transaction_key(const String &p_asset_id, const String &p_target_dir, const Array &p_files, const Array &p_hashes) {
	String fingerprint = p_asset_id + "\n" + p_target_dir;
	for (int i = 0; i < p_files.size(); i++) {
		const String path = p_files[i];
		fingerprint += "\n" + path + ":" + String(p_hashes[i]);
	}
	return fingerprint.md5_text();
}

static Dictionary _resource_mesh_stats(const Ref<Resource> &p_resource) {
	Ref<Mesh> mesh = p_resource;
	if (mesh.is_valid()) {
		return solers_describe_mesh(mesh);
	}
	Ref<PackedScene> packed_scene = p_resource;
	if (packed_scene.is_valid()) {
		Node *root = packed_scene->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED);
		if (root) {
			const Dictionary stats = solers_describe_geometry(root);
			memdelete(root);
			return stats;
		}
	}
	return Dictionary();
}

static Dictionary _project_import_inspection(const Array &p_files, const Array &p_entrypoints, bool p_verify_load) {
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	HashSet<String> entrypoint_set;
	for (int i = 0; i < p_entrypoints.size(); i++) {
		entrypoint_set.insert(String(p_entrypoints[i]));
	}
	Array statuses;
	int indexed_count = 0;
	int import_valid_count = 0;
	bool metadata_ready = filesystem != nullptr;

	for (int i = 0; i < p_files.size(); i++) {
		const String path = p_files[i];
		const bool exists = FileAccess::exists(path);
		int file_index = -1;
		EditorFileSystemDirectory *directory = filesystem ? filesystem->find_file(path, &file_index) : nullptr;
		const bool indexed = directory && file_index >= 0;
		const Ref<ResourceImporter> source_importer = importer ? importer->get_importer_by_file(path) : Ref<ResourceImporter>();
		const bool imported_source = source_importer.is_valid();
		const bool resource_candidate = imported_source || ResourceLoader::get_resource_type(path) != StringName() || entrypoint_set.has(path);
		const bool import_valid = exists && (!resource_candidate || (indexed && (!imported_source || directory->get_file_import_is_valid(file_index)) && (!imported_source || ResourceLoader::is_import_valid(path))));
		const String type = indexed ? String(directory->get_file_type(file_index)) : String();

		Dictionary status;
		status["path"] = path;
		status["exists"] = exists;
		status["indexed"] = indexed;
		status["imported_source"] = imported_source;
		status["resource_candidate"] = resource_candidate;
		status["import_valid"] = import_valid;
		status["resource_type"] = type;
		status["entrypoint"] = entrypoint_set.has(path);
		status["loadable"] = false;
		statuses.push_back(status);

		indexed_count += indexed ? 1 : 0;
		import_valid_count += import_valid ? 1 : 0;
		metadata_ready = metadata_ready && import_valid && (!resource_candidate || (indexed && !type.is_empty()));
	}

	int loadable_count = 0;
	int64_t triangle_count = 0;
	int mesh_count = 0;
	bool uv2_complete = true;
	if (metadata_ready && p_verify_load) {
		for (int i = 0; i < statuses.size(); i++) {
			Dictionary status = statuses[i];
			if (!(bool)status.get("entrypoint", false)) {
				continue;
			}
			Error load_error = OK;
			const Ref<Resource> resource = ResourceLoader::load(String(status["path"]), String(), ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
			const bool loadable = resource.is_valid() && load_error == OK;
			status["loadable"] = loadable;
			if (loadable) {
				const Dictionary mesh_stats = _resource_mesh_stats(resource);
				status["mesh_stats"] = mesh_stats;
				triangle_count += (int64_t)mesh_stats.get("triangle_count", 0);
				mesh_count += (int)mesh_stats.get("mesh_count", 0);
				uv2_complete = uv2_complete && ((int)mesh_stats.get("mesh_count", 0) == 0 || (bool)mesh_stats.get("uv2_complete", false));
			}
			if (!loadable) {
				status["load_error"] = (int)load_error;
			}
			statuses[i] = status;
			loadable_count += loadable ? 1 : 0;
		}
	}

	Dictionary inspection;
	inspection["files"] = statuses;
	inspection["file_count"] = p_files.size();
	inspection["indexed_count"] = indexed_count;
	inspection["import_valid_count"] = import_valid_count;
	inspection["loadable_count"] = loadable_count;
	inspection["entrypoint_count"] = p_entrypoints.size();
	inspection["mesh_count"] = mesh_count;
	inspection["triangle_count"] = triangle_count;
	inspection["uv2_complete"] = mesh_count > 0 && uv2_complete;
	inspection["ready"] = metadata_ready && p_verify_load && loadable_count == p_entrypoints.size();
	return inspection;
}

static Dictionary _project_import_completed_data(const Dictionary &p_result, const Dictionary &p_inspection) {
	Dictionary data = p_result.duplicate(true);
	Array resources;
	const Array statuses = p_inspection.get("files", Array());
	for (int i = 0; i < statuses.size(); i++) {
		const Dictionary status = statuses[i];
		if (!(bool)status.get("loadable", false)) {
			continue;
		}
		Dictionary resource;
		resource["path"] = status.get("path", String());
		resource["type"] = status.get("resource_type", String());
		resources.push_back(resource);
	}
	data["resources"] = resources;
	data["import"] = p_inspection;
	return data;
}

static Dictionary _project_import_contract_error(const Dictionary &p_result, const Dictionary &p_inspection) {
	const int64_t max_triangle_count = p_result.get("max_import_triangle_count", 0);
	if (max_triangle_count > 0 && (int64_t)p_inspection.get("triangle_count", 0) > max_triangle_count) {
		Dictionary error;
		error["code"] = "TOPOLOGY_TARGET_MISSED";
		error["message"] = vformat("The imported mesh contains %d triangles, exceeding the requested topology budget of %d triangles. Create a remeshed asset version before scene placement.", (int64_t)p_inspection.get("triangle_count", 0), max_triangle_count);
		error["diagnostics"] = p_inspection;
		return error;
	}
	const Array static_lightmap_paths = p_result.get("static_lightmap_import_paths", Array());
	if (!static_lightmap_paths.is_empty() && !(bool)p_inspection.get("uv2_complete", false)) {
		Dictionary error;
		error["code"] = "STATIC_LIGHTMAP_UV2_MISSING";
		error["message"] = "Godot completed the native Static Lightmaps import, but one or more imported mesh surfaces have no UV2.";
		error["diagnostics"] = p_inspection;
		return error;
	}
	return Dictionary();
}

static bool _solers_gltf_document_json(const String &p_path, Dictionary &r_document, String &r_error) {
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &open_error);
	if (file.is_null()) {
		r_error = vformat("Unable to open %s (error %d).", p_path, (int)open_error);
		return false;
	}
	String json_text;
	if (p_path.get_extension().to_lower() == "glb") {
		if (file->get_length() < 20) {
			r_error = "GLB file is truncated.";
			return false;
		}
		const uint32_t magic = file->get_32();
		file->get_32(); // Container version.
		file->get_32(); // Total length.
		const uint32_t chunk_length = file->get_32();
		const uint32_t chunk_type = file->get_32();
		if (magic != 0x46546C67 || chunk_type != 0x4E4F534A || (uint64_t)chunk_length > file->get_length()) {
			r_error = "GLB header does not contain a leading JSON chunk.";
			return false;
		}
		PackedByteArray bytes;
		bytes.resize(chunk_length);
		file->get_buffer(bytes.ptrw(), chunk_length);
		json_text = String::utf8((const char *)bytes.ptr(), bytes.size());
	} else {
		json_text = file->get_as_utf8_string();
	}
	JSON json;
	if (json.parse(json_text) != OK || json.get_data().get_type() != Variant::DICTIONARY) {
		r_error = vformat("%s does not contain a parsable glTF JSON document.", p_path);
		return false;
	}
	r_document = json.get_data();
	return true;
}

// Sums TRIANGLES-mode primitive counts from glTF accessor metadata, so topology
// budgets are enforced from source data before any file is copied or imported.
static int64_t _solers_gltf_source_triangle_count(const Dictionary &p_document) {
	const Array accessors = p_document.get("accessors", Array());
	const Array meshes = p_document.get("meshes", Array());
	int64_t triangles = 0;
	for (int mesh_index = 0; mesh_index < meshes.size(); mesh_index++) {
		const Array primitives = Dictionary(meshes[mesh_index]).get("primitives", Array());
		for (int primitive_index = 0; primitive_index < primitives.size(); primitive_index++) {
			const Dictionary primitive = primitives[primitive_index];
			if ((int64_t)primitive.get("mode", 4) != 4) {
				continue;
			}
			int64_t element_count = 0;
			if (primitive.has("indices")) {
				const int64_t accessor_index = primitive.get("indices", -1);
				if (accessor_index >= 0 && accessor_index < accessors.size()) {
					element_count = (int64_t)Dictionary(accessors[accessor_index]).get("count", 0);
				}
			} else {
				const Dictionary attributes = primitive.get("attributes", Dictionary());
				const int64_t accessor_index = attributes.get("POSITION", -1);
				if (accessor_index >= 0 && accessor_index < accessors.size()) {
					element_count = (int64_t)Dictionary(accessors[accessor_index]).get("count", 0);
				}
			}
			triangles += element_count / 3;
		}
	}
	return triangles;
}

static Dictionary _static_lightmap_import_config(const String &p_path) {
	Dictionary config;
	ResourceFormatImporter *format_importer = ResourceFormatImporter::get_singleton();
	const Ref<ResourceImporter> importer = format_importer ? format_importer->get_importer_by_file(p_path) : Ref<ResourceImporter>();
	if (importer.is_null()) {
		return config;
	}

	List<ResourceImporter::ImportOption> options;
	importer->get_import_options(p_path, &options);
	Dictionary desired;
	StringName option_name;
	Variant option_value;
	for (const ResourceImporter::ImportOption &option : options) {
		if (option.option.name == SNAME("meshes/light_baking")) {
			option_name = option.option.name;
			option_value = 2; // ResourceImporterScene::LIGHT_BAKE_STATIC_LIGHTMAPS.
			break;
		}
		if (option.option.name == SNAME("generate_lightmap_uv2")) {
			option_name = option.option.name;
			option_value = true;
		}
	}
	if (option_name == StringName()) {
		return config;
	}
	desired[option_name] = option_value;

	bool configured = false;
	if (FileAccess::exists(p_path + ".import")) {
		Ref<ConfigFile> sidecar;
		sidecar.instantiate();
		if (sidecar->load(p_path + ".import") == OK && sidecar->has_section_key("params", option_name)) {
			configured = sidecar->get_value("params", option_name) == option_value;
		}
	}
	config["supported"] = true;
	config["configured"] = configured;
	config["importer"] = importer->get_importer_name();
	config["options"] = desired;
	return config;
}

static bool _static_lightmap_imports_ready(const Array &p_paths) {
	for (int i = 0; i < p_paths.size(); i++) {
		const Dictionary config = _static_lightmap_import_config(String(p_paths[i]));
		if ((bool)config.get("supported", false) && !(bool)config.get("configured", false)) {
			return false;
		}
	}
	return true;
}

static Array _solers_selected_attachments(const Array &p_attachments, const Array &p_ids) {
	Array selected;
	for (int i = 0; i < p_ids.size(); i++) {
		const String wanted = String(p_ids[i]).strip_edges();
		if (wanted.is_empty()) {
			continue;
		}
		for (int j = 0; j < p_attachments.size(); j++) {
			const Dictionary attachment = p_attachments[j];
			if (String(attachment.get("id", String())).strip_edges() == wanted) {
				selected.push_back(attachment);
				break;
			}
		}
	}
	return selected;
}

static String _solers_url_path(const String &p_url) {
	String scheme;
	String host;
	int port = 0;
	String path;
	String fragment;
	if (p_url.parse_url(scheme, host, port, path, fragment) != OK) {
		return String();
	}
	return path;
}

static String _solers_attachment_data_uri(const Dictionary &p_attachment, String &r_error) {
	const String path = String(p_attachment.get("local_path", String())).strip_edges();
	const String mime = String(p_attachment.get("mime_type", String())).strip_edges();
	if (path.is_empty() || mime.is_empty()) {
		r_error = "Image attachment is missing local_path or mime_type.";
		return String();
	}
	Error read_error = OK;
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(path, &read_error);
	if (read_error != OK || bytes.is_empty()) {
		r_error = vformat("Failed to read image attachment: %s", path);
		return String();
	}
	return vformat("data:%s;base64,%s", mime, CryptoCore::b64_encode_str(bytes.ptr(), bytes.size()));
}

static String _solers_clean_base_url(const String &p_base_url) {
	String clean = p_base_url;
	while (clean.ends_with("/")) {
		clean = clean.substr(0, clean.length() - 1);
	}
	return clean;
}

static String _solers_meshy_task_id(const Dictionary &p_response) {
	return p_response.get("result", p_response.get("id", p_response.get("task_id", String())));
}

Dictionary SolersAssetService::_meshy_poll(Task *p_task, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers) {
	Dictionary detail;
	for (int i = 0; i < 90 && !p_task->abort.is_set(); i++) {
		Dictionary poll = _http_request("GET", p_url, p_headers, PackedByteArray(), 60000);
		if (!(bool)poll.get("ok", false)) {
			r_state["status"] = "failed";
			r_state["error"] = poll.get("error", Dictionary());
			_set_task_state(p_task, r_state);
			return Dictionary();
		}
		detail = _parse_json_body(poll);
		const String status = String(detail.get("status", String())).to_upper();
		r_state["provider_status"] = status;
		r_state["progress"] = detail.get("progress", r_state.get("progress", 0));
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
		_set_task_state(p_task, r_state);
		if (status == "SUCCEEDED") {
			return detail;
		}
		if (status == "FAILED" || status == "CANCELED" || status == "CANCELLED") {
			r_state["status"] = "failed";
			r_state["error"] = _error_data("PROVIDER_FAILED", "Meshy task failed.");
			r_state["provider_response"] = detail;
			_set_task_state(p_task, r_state);
			return Dictionary();
		}
		OS::get_singleton()->delay_usec(10000000);
	}
	if (p_task->abort.is_set()) {
		r_state["status"] = "interrupted";
		r_state["error"] = _error_data("LOCAL_OBSERVER_STOPPED", "Solers stopped observing this provider task; its provider task id was preserved for recovery.");
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	} else {
		r_state["status"] = "failed";
		r_state["error"] = _error_data("PROVIDER_TIMEOUT", "Meshy task did not finish before the local wait limit.");
		r_state["provider_response"] = detail;
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	}
	_set_task_state(p_task, r_state);
	return Dictionary();
}

Dictionary SolersAssetService::_meshy_submit_and_poll(Task *p_task, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers, const Dictionary &p_body, const String &p_stage) {
	Dictionary provider_request;
	provider_request["endpoint"] = _solers_url_path(p_url);
	const Array request_attachments = r_state.get("source_attachments", Array());
	provider_request["attachment_count"] = (int64_t)request_attachments.size();
	provider_request["mode"] = r_state.get("generation_mode", String());
	provider_request["input_attachment_ids"] = r_state.get("input_attachment_ids", Array());
	r_state["provider_request"] = provider_request;
	r_state["stage"] = p_stage;
	r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	_set_task_state(p_task, r_state);

	Dictionary submit = _http_request("POST", p_url, p_headers, _utf8_bytes(JSON::stringify(p_body)), 120000);
	if (!(bool)submit.get("ok", false)) {
		r_state["status"] = "failed";
		r_state["error"] = submit.get("error", Dictionary());
		_set_task_state(p_task, r_state);
		return Dictionary();
	}
	const Dictionary submitted = _parse_json_body(submit);
	String provider_task_id = _solers_meshy_task_id(submitted);
	if (provider_task_id.is_empty()) {
		r_state["status"] = "failed";
		r_state["error"] = _error_data("BAD_PROVIDER_RESPONSE", "Meshy response did not contain a task id.");
		r_state["provider_response"] = submitted;
		_set_task_state(p_task, r_state);
		return Dictionary();
	}
	r_state["provider_task_id"] = provider_task_id;
	if (p_stage == "previewing") {
		r_state["preview_task_id"] = provider_task_id;
	} else if (p_stage == "refining") {
		r_state["refine_task_id"] = provider_task_id;
	} else if (p_stage == "rigging") {
		r_state["rig_task_id"] = provider_task_id;
	} else if (p_stage == "animating") {
		r_state["animation_task_id"] = provider_task_id;
	}
	_set_task_state(p_task, r_state);
	return _meshy_poll(p_task, r_state, p_url + "/" + provider_task_id, p_headers);
}

bool SolersAssetService::_meshy_download_model(Task *p_task, Dictionary &r_state, const Dictionary &p_detail, const String &p_asset_id, Array &r_files, String &r_preview_url) {
	const Dictionary model_urls = p_detail.get("model_urls", Dictionary());
	String ext = "glb";
	String url = model_urls.get("glb", String());
	if (url.is_empty()) {
		url = model_urls.get("fbx", String());
		ext = "fbx";
	}
	if (url.is_empty()) {
		url = model_urls.get("obj", String());
		ext = "obj";
	}
	if (url.is_empty()) {
		url = p_detail.get("model_url", String());
		ext = url.get_extension().get_slice("?", 0).to_lower();
		if (ext.is_empty() || ext.length() > 8) {
			ext = "glb";
		}
	}
	if (url.is_empty()) {
		const Dictionary result = p_detail.get("result", Dictionary());
		url = result.get("rigged_character_glb_url", result.get("animation_glb_url", String()));
		ext = "glb";
		if (url.is_empty()) {
			url = result.get("rigged_character_fbx_url", result.get("animation_fbx_url", String()));
			ext = "fbx";
		}
	}
	if (url.is_empty()) {
		r_state["status"] = "failed";
		r_state["error"] = _error_data("NO_DOWNLOAD_URL", "Meshy response did not include a downloadable model URL.");
		r_state["provider_response"] = p_detail;
		_set_task_state(p_task, r_state);
		return false;
	}

	r_state["provider_response"] = p_detail;
	r_preview_url = p_detail.get("thumbnail_url", String());
	Dictionary download = _http_request("GET", url, Vector<String>(), PackedByteArray(), 180000);
	if (!(bool)download.get("ok", false)) {
		r_state["status"] = "failed";
		r_state["error"] = download.get("error", Dictionary());
		_set_task_state(p_task, r_state);
		return false;
	}
	const String file_path = _source_dir(p_asset_id).path_join(p_asset_id + "." + ext);
	String write_error;
	if (!_write_bytes_atomic(file_path, download.get("body", PackedByteArray()), write_error)) {
		r_state["status"] = "failed";
		Dictionary error = _error_data("WRITE_FAILED", write_error);
		error["path"] = file_path;
		r_state["error"] = error;
		_set_task_state(p_task, r_state);
		return false;
	}
	r_files.push_back(file_path);
	return true;
}

void SolersAssetService::_meshy_download_basic_animations(Task *p_task, Dictionary &r_state, const Dictionary &p_detail, const String &p_asset_id) {
	const Dictionary result = p_detail.get("result", Dictionary());
	const Dictionary basic = result.get("basic_animations", Dictionary());
	if (basic.is_empty()) {
		return;
	}

	Dictionary animation_root = r_state.get("animations", Dictionary());
	Dictionary basic_root = animation_root.get("basic", Dictionary());
	const char *names[] = { "walking", "running" };
	for (const char *name : names) {
		const String key = String(name);
		const String url = basic.get(key + "_glb_url", String());
		if (url.is_empty()) {
			continue;
		}
		Dictionary download = _http_request("GET", url, Vector<String>(), PackedByteArray(), 180000);
		if (!(bool)download.get("ok", false)) {
			r_state["animation_download_error"] = download.get("error", Dictionary());
			continue;
		}
		const String file_path = _source_dir(p_asset_id).path_join(p_asset_id + "_" + key + ".glb");
		String write_error;
		if (!_write_bytes_atomic(file_path, download.get("body", PackedByteArray()), write_error)) {
			r_state["animation_download_error"] = _error_data("WRITE_FAILED", write_error);
			continue;
		}
		Dictionary item;
		item["name"] = key.capitalize();
		item["source"] = "meshy.rigging.basic_animations";
		item["glb_file"] = file_path;
		item["glb_url"] = url;
		item["fbx_url"] = basic.get(key + "_fbx_url", String());
		item["armature_glb_url"] = basic.get(key + "_armature_glb_url", String());
		basic_root[key] = item;
	}
	if (!basic_root.is_empty()) {
		animation_root["basic"] = basic_root;
		r_state["animations"] = animation_root;
		r_state.erase("animation_download_error");
		_set_task_state(p_task, r_state);
	}
}

void SolersAssetService::_run_task(Task *p_task) {
	Dictionary state = _task_state(p_task);
	const String kind = state.get("kind", String());
	const String provider = state.get("provider", String());
	const String prompt = state.get("prompt", String());
	const String base_url = state.get("base_url", String());
	const String api_key = p_task->api_key;
	const String profile = state.get("profile", String("game_default"));
	const Dictionary provider_options = state.get("provider_options", Dictionary());
	const Array source_attachments = state.get("source_attachments", Array());

	state["status"] = "running";
	state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	_set_task_state(p_task, state);

	if (api_key.is_empty() && provider != "ambientcg" && provider != "polyhaven") {
		if (p_task->resume_provider_task) {
			state["status"] = "interrupted";
			state["error"] = _error_data("API_KEY_MISSING", "A provider API key is required to resume this asset task.");
			_set_task_state(p_task, state);
			return;
		}
		state["status"] = "failed";
		state["error"] = _error_data("API_KEY_MISSING", "Asset provider API key is not configured.");
		_set_task_state(p_task, state);
		return;
	}

	Array files;
	String preview_url;
	String write_error;
	if (p_task->resume_provider_task) {
		if (provider != "meshy") {
			state["status"] = "interrupted";
			state["error"] = _error_data("RESUME_UNSUPPORTED", "Only Meshy provider tasks can be resumed after an editor restart.");
			_set_task_state(p_task, state);
			return;
		}
		const String provider_task_id = state.get("provider_task_id", String());
		const String endpoint = Dictionary(state.get("provider_request", Dictionary())).get("endpoint", String());
		if (provider_task_id.is_empty() || endpoint.is_empty()) {
			state["status"] = "interrupted";
			state["error"] = _error_data("RESUME_METADATA_MISSING", "The interrupted Meshy task has no provider task id or endpoint, so Solers will not submit it again automatically.");
			_set_task_state(p_task, state);
			return;
		}
		Vector<String> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Authorization: Bearer " + api_key);
		const Dictionary detail = _meshy_poll(p_task, state, _solers_clean_base_url(base_url) + endpoint + "/" + provider_task_id, headers);
		if (detail.is_empty() || !_meshy_download_model(p_task, state, detail, p_task->asset_id, files, preview_url)) {
			return;
		}
		if (String(state.get("stage", String())) == "rigging") {
			_meshy_download_basic_animations(p_task, state, detail, p_task->asset_id);
		}
		if (String(state.get("stage", String())) == "previewing") {
			state["status"] = "draft";
			state["stage"] = "draft";
		}
	} else if (provider == "ambientcg") {
		const String source_asset_id = state.get("source_asset_id", String());
		const String variant = state.get("source_variant", String());
		const Dictionary metadata = _ambientcg_asset_metadata(source_asset_id, &p_task->abort);
		if (!(bool)metadata.get("ok", false)) {
			state["status"] = "failed";
			state["error"] = metadata.get("error", _error_data("AMBIENTCG_FAILED", "ambientCG metadata request failed."));
			_set_task_state(p_task, state);
			return;
		}
		const Dictionary asset = metadata.get("asset", Dictionary());
		const Dictionary download_spec = _ambientcg_download_package(asset, variant);
		if (download_spec.is_empty()) {
			state["status"] = "failed";
			state["error"] = _error_data("AMBIENTCG_PACKAGE_NOT_FOUND", "The selected ambientCG variant is not available for this asset.");
			_set_task_state(p_task, state);
			return;
		}
		const String url = download_spec.get("url", String());
		const int64_t size = (int64_t)download_spec.get("size", -1);
		if (url.is_empty() || size < 0) {
			state["status"] = "failed";
			state["error"] = _error_data("AMBIENTCG_BAD_METADATA", "ambientCG package metadata is incomplete.");
			_set_task_state(p_task, state);
			return;
		}
		state["download_url"] = url;
		state["download_bytes"] = size;
		state["source_url"] = asset.get("url", String());
		state["license"] = "CC0-1.0";
		state["asset_type"] = asset.get("type", String());
		state["maps"] = asset.get("maps", Array());
		state["dimensions"] = asset.get("dimensions", Dictionary());
		const Dictionary thumbnails = asset.get("thumbnails", Dictionary());
		preview_url = thumbnails.get("512-JPG-242424", thumbnails.get("512-PNG", String()));
		state["stage"] = "downloading";
		_set_task_state(p_task, state);

		Dictionary archive = _http_request("GET", url, Vector<String>(), PackedByteArray(), 180000, size + 1, &p_task->abort);
		if (!(bool)archive.get("ok", false)) {
			state["status"] = "failed";
			state["error"] = archive.get("error", Dictionary());
			_set_task_state(p_task, state);
			return;
		}
		const String provider_type = String(asset.get("type", String())).to_lower();
		const String data_type = provider_type == "3d-model" ? "3d" : provider_type;
		if (data_type != "material" && data_type != "hdri" && data_type != "3d") {
			state["status"] = "failed";
			state["error"] = _error_data("AMBIENTCG_UNSUPPORTED_TYPE", "ambientCG asset is not a supported material, HDRI, or 3D model.");
			_set_task_state(p_task, state);
			return;
		}
		if (data_type != kind) {
			state["status"] = "failed";
			state["error"] = _error_data("CATALOG_KIND_MISMATCH", "The selected ambientCG asset does not match the requested catalog kind.");
			_set_task_state(p_task, state);
			return;
		}
		const String current_version = JSON::stringify(asset.get("downloads", Array())).sha256_text();
		if (String(state.get("source_version", String())) != current_version) {
			state["status"] = "failed";
			state["error"] = _error_data("CATALOG_VERSION_CHANGED", "ambientCG changed this asset after inspection. Inspect it again before acquiring.");
			_set_task_state(p_task, state);
			return;
		}
		if (!_ambientcg_extract_archive(p_task, archive.get("body", PackedByteArray()), files, write_error)) {
			state["status"] = "failed";
			state["error"] = _error_data("AMBIENTCG_EXTRACT_FAILED", write_error);
			_set_task_state(p_task, state);
			return;
		}
		state["kind"] = data_type;
		Array entrypoints;
		Dictionary import_selection;
		if (data_type == "3d") {
			import_selection = _project_import_selection(files, Array(), Array());
			if ((bool)import_selection.get("valid", false)) {
				entrypoints = _model_resource_entrypoints(import_selection.get("files", Array()));
			}
		} else {
			entrypoints = _native_resource_entrypoints(files);
			import_selection = _project_import_selection(files, Array(), entrypoints);
		}
		if (entrypoints.is_empty() || !(bool)import_selection.get("valid", false)) {
			state["status"] = "failed";
			state["error"] = _error_data("AMBIENTCG_PACKAGE_HAS_NO_NATIVE_RESOURCE", "ambientCG package has no loadable native resource for the requested asset kind.");
			_set_task_state(p_task, state);
			return;
		}
		state["import_files"] = import_selection.get("files", Array());
		state["entrypoints"] = entrypoints;
	} else if (provider == "polyhaven") {
		const String source_asset_id = state.get("source_asset_id", String());
		const String variant = state.get("source_variant", String());
		const Dictionary metadata = _polyhaven_asset_metadata(source_asset_id, &p_task->abort);
		if (!(bool)metadata.get("ok", false)) {
			state["status"] = "failed";
			state["error"] = metadata.get("error", _error_data("POLYHAVEN_FAILED", "Poly Haven metadata request failed."));
			_set_task_state(p_task, state);
			return;
		}
		const Dictionary asset = metadata.get("asset", Dictionary());
		const int asset_type = (int)asset.get("type", -1);
		String data_type;
		if (asset_type == 0) {
			data_type = "hdri";
		} else if (asset_type == 1) {
			data_type = "material";
		} else if (asset_type == 2) {
			data_type = "3d";
		}
		if (data_type.is_empty()) {
			state["status"] = "failed";
			state["error"] = _error_data("POLYHAVEN_UNSUPPORTED_TYPE", "Poly Haven asset is not a supported texture, HDRI, or 3D model.");
			_set_task_state(p_task, state);
			return;
		}
		if (data_type != kind) {
			state["status"] = "failed";
			state["error"] = _error_data("CATALOG_KIND_MISMATCH", "The selected Poly Haven asset does not match the requested catalog kind.");
			_set_task_state(p_task, state);
			return;
		}
		state["kind"] = data_type;
		state["asset_type"] = data_type;
		state["dimensions"] = asset.get("dimensions", Array());
		state["polycount"] = asset.get("polycount", 0);
		state["texel_density"] = asset.get("texel_density", 0);
		state["files_hash"] = asset.get("files_hash", String());
		if (String(state.get("source_version", String())) != String(asset.get("files_hash", String()))) {
			state["status"] = "failed";
			state["error"] = _error_data("CATALOG_VERSION_CHANGED", "Poly Haven changed this asset after search. Refresh the catalog result and acquire its new files_hash.");
			_set_task_state(p_task, state);
			return;
		}
		state["source_url"] = "https://polyhaven.com/a/" + source_asset_id.uri_encode();
		state["license"] = "CC0-1.0";
		PackedStringArray author_names;
		const Dictionary authors = asset.get("authors", Dictionary());
		for (const Variant *author = authors.next(nullptr); author; author = authors.next(author)) {
			author_names.push_back(String(*author));
		}
		author_names.sort();
		state["attribution"] = vformat("%s by %s on Poly Haven", asset.get("name", source_asset_id), author_names.is_empty() ? String("Poly Haven contributors") : String(", ").join(author_names));
		preview_url = asset.get("thumbnail_url", String());
		state["stage"] = "downloading";
		_set_task_state(p_task, state);

		Dictionary maps;
		Dictionary checksums;
		const Dictionary provider_files = metadata.get("files", Dictionary());
		if (!_polyhaven_download_variant(p_task, provider_files, data_type, variant, files, maps, checksums, write_error)) {
			state["status"] = "failed";
			state["error"] = _error_data("POLYHAVEN_DOWNLOAD_FAILED", write_error);
			_set_task_state(p_task, state);
			return;
		}
		Array map_types = maps.keys();
		map_types.sort();
		state["maps"] = map_types;
		state["map_files"] = maps;
		state["checksums"] = checksums;
		Array entrypoints;
		for (int i = 0; i < map_types.size(); i++) {
			entrypoints.push_back(maps[map_types[i]]);
		}
		state["import_files"] = files;
		state["entrypoints"] = entrypoints;
	} else if (provider == "elevenlabs") {
		Dictionary body;
		_merge_options(body, provider_options);
		if (kind == "music") {
			if (!body.has("prompt")) {
				body["prompt"] = prompt;
			}
		} else {
			if (!body.has("text")) {
				body["text"] = prompt;
			}
		}
		const String json = JSON::stringify(body);
		Vector<String> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Accept: audio/mpeg");
		headers.push_back("xi-api-key: " + api_key);
		const String endpoint = kind == "music" ? "/v1/music" : "/v1/sound-generation";
		String clean_base_url = base_url;
		while (clean_base_url.ends_with("/")) {
			clean_base_url = clean_base_url.substr(0, clean_base_url.length() - 1);
		}
		Dictionary response = _http_request("POST", clean_base_url + endpoint, headers, _utf8_bytes(json), 120000);
		if (!(bool)response.get("ok", false)) {
			state["status"] = "failed";
			state["error"] = response.get("error", Dictionary());
			_set_task_state(p_task, state);
			return;
		}
		const String output_format = String(provider_options.get("output_format", String())).to_lower();
		const String ext = output_format.contains("wav") ? "wav" : "mp3";
		const String file_path = _source_dir(p_task->asset_id).path_join(p_task->asset_id + "." + ext);
		if (!_write_bytes_atomic(file_path, response.get("body", PackedByteArray()), write_error)) {
			state["status"] = "failed";
			state["error"] = _error_data("WRITE_FAILED", write_error);
			_set_task_state(p_task, state);
			return;
		}
		files.push_back(file_path);
	} else if (provider == "meshy") {
		Vector<String> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Authorization: Bearer " + api_key);
		const String clean_base_url = _solers_clean_base_url(base_url);
		const String operation = String(state.get("operation", "generate"));
		if (operation == "refine") {
			Dictionary refine_body = _meshy_text_refine_options(provider_options);
			refine_body["mode"] = "refine";
			if (!refine_body.has("preview_task_id")) {
				refine_body["preview_task_id"] = state.get("source_task_id", String());
			}
			if (!refine_body.has("enable_pbr")) {
				refine_body["enable_pbr"] = true;
			}
			if (!refine_body.has("target_formats")) {
				Array formats;
				formats.push_back("glb");
				refine_body["target_formats"] = formats;
			}
			const Dictionary detail = _meshy_submit_and_poll(p_task, state, clean_base_url + "/openapi/v2/text-to-3d", headers, refine_body, "refining");
			if (detail.is_empty() || !_meshy_download_model(p_task, state, detail, p_task->asset_id, files, preview_url)) {
				return;
			}
		} else if (operation == "rig_humanoid" || operation == "animate_humanoid") {
			Dictionary body;
			_merge_options(body, provider_options);
			const String source_task_id = state.get("source_task_id", String());
			if (operation == "rig_humanoid" && !source_task_id.is_empty() && !body.has("input_task_id")) {
				body["input_task_id"] = source_task_id;
			}
			if (operation == "animate_humanoid" && !source_task_id.is_empty() && !body.has("rig_task_id")) {
				body["rig_task_id"] = source_task_id;
			}
			const String endpoint = operation == "rig_humanoid" ? "/openapi/v1/rigging" : "/openapi/v1/animations";
			const String stage = operation == "rig_humanoid" ? "rigging" : "animating";
			const Dictionary detail = _meshy_submit_and_poll(p_task, state, clean_base_url + endpoint, headers, body, stage);
			if (detail.is_empty() || !_meshy_download_model(p_task, state, detail, p_task->asset_id, files, preview_url)) {
				return;
			}
			if (operation == "rig_humanoid") {
				_meshy_download_basic_animations(p_task, state, detail, p_task->asset_id);
			}
		} else if (operation == "remesh" || operation == "retexture") {
			Dictionary body;
			_merge_options(body, provider_options);
			const String source_task_id = state.get("source_task_id", String());
			if (!source_task_id.is_empty() && !body.has("input_task_id")) {
				body["input_task_id"] = source_task_id;
			}
			if (!body.has("target_formats")) {
				Array formats;
				formats.push_back("glb");
				body["target_formats"] = formats;
			}
			const String endpoint = operation == "remesh" ? "/openapi/v1/remesh" : "/openapi/v1/retexture";
			const String stage = operation == "remesh" ? "optimizing" : "restyling";
			const Dictionary detail = _meshy_submit_and_poll(p_task, state, clean_base_url + endpoint, headers, body, stage);
			if (detail.is_empty() || !_meshy_download_model(p_task, state, detail, p_task->asset_id, files, preview_url)) {
				return;
			}
		} else if (!source_attachments.is_empty()) {
			Dictionary body;
			_merge_options(body, provider_options);
			if (!body.has("prompt") && !prompt.is_empty()) {
				body["prompt"] = prompt;
			}
			if (profile == "game_default" && !body.has("model_type")) {
				body["model_type"] = "standard";
			}
			if (!body.has("ai_model")) {
				body["ai_model"] = "meshy-6";
			}
			if (!body.has("target_formats")) {
				Array formats;
				formats.push_back("glb");
				body["target_formats"] = formats;
			}
			String data_uri_error;
			if (source_attachments.size() == 1) {
				const String data_uri = _solers_attachment_data_uri(source_attachments[0], data_uri_error);
				if (data_uri.is_empty()) {
					state["status"] = "failed";
					state["error"] = _error_data("ATTACHMENT_READ_FAILED", data_uri_error);
					_set_task_state(p_task, state);
					return;
				}
				body["image_url"] = data_uri;
			} else {
				Array image_urls;
				for (int i = 0; i < source_attachments.size(); i++) {
					const String data_uri = _solers_attachment_data_uri(source_attachments[i], data_uri_error);
					if (data_uri.is_empty()) {
						state["status"] = "failed";
						state["error"] = _error_data("ATTACHMENT_READ_FAILED", data_uri_error);
						_set_task_state(p_task, state);
						return;
					}
					image_urls.push_back(data_uri);
				}
				body["image_urls"] = image_urls;
			}
			const String endpoint = source_attachments.size() == 1 ? "/openapi/v1/image-to-3d" : "/openapi/v1/multi-image-to-3d";
			const Dictionary detail = _meshy_submit_and_poll(p_task, state, clean_base_url + endpoint, headers, body, "generating");
			if (detail.is_empty() || !_meshy_download_model(p_task, state, detail, p_task->asset_id, files, preview_url)) {
				return;
			}
		} else {
			Dictionary preview_body = _meshy_text_preview_options(provider_options);
			const bool draft_only = String(provider_options.get("mode", String())).to_lower() == "preview";
			preview_body["mode"] = "preview";
			if (!preview_body.has("prompt")) {
				preview_body["prompt"] = prompt;
			}
			if (profile == "game_default" && !preview_body.has("model_type")) {
				preview_body["model_type"] = "standard";
			}
			if (!preview_body.has("ai_model")) {
				preview_body["ai_model"] = "meshy-6";
			}
			if (!preview_body.has("target_formats")) {
				Array formats;
				formats.push_back("glb");
				preview_body["target_formats"] = formats;
			}
			const String text_to_3d_url = clean_base_url + "/openapi/v2/text-to-3d";
			Dictionary preview_detail = _meshy_submit_and_poll(p_task, state, text_to_3d_url, headers, preview_body, "previewing");
			if (preview_detail.is_empty()) {
				return;
			}
			if (draft_only) {
				state["status"] = "draft";
				state["stage"] = "draft";
				if (!_meshy_download_model(p_task, state, preview_detail, p_task->asset_id, files, preview_url)) {
					return;
				}
			} else {
				Dictionary refine_body = _meshy_text_refine_options(provider_options);
				refine_body["mode"] = "refine";
				refine_body["preview_task_id"] = state.get("preview_task_id", state.get("provider_task_id", String()));
				if (!refine_body.has("enable_pbr")) {
					refine_body["enable_pbr"] = true;
				}
				if (!refine_body.has("target_formats")) {
					Array formats;
					formats.push_back("glb");
					refine_body["target_formats"] = formats;
				}
				Dictionary refine_detail = _meshy_submit_and_poll(p_task, state, text_to_3d_url, headers, refine_body, "refining");
				if (refine_detail.is_empty() || !_meshy_download_model(p_task, state, refine_detail, p_task->asset_id, files, preview_url)) {
					return;
				}
			}
		}
	} else {
		state["status"] = "failed";
		state["error"] = _error_data("UNSUPPORTED_PROVIDER", vformat("Unsupported asset provider: %s", provider));
		_set_task_state(p_task, state);
		return;
	}

	if (String(state.get("status", String())) != "draft") {
		state["status"] = "ready";
		state["stage"] = "ready";
	}
	if (!state.has("entrypoints")) {
		state["entrypoints"] = files;
	}
	if (!state.has("import_files")) {
		state["import_files"] = files;
	}
	state["files"] = files;
	state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	_set_task_state(p_task, state);
	_download_preview(p_task, state, preview_url);
}

Dictionary SolersAssetService::_manifest_for_asset(const String &p_asset_id) const {
	Task *const *task = tasks.getptr(p_asset_id);
	if (task && *task) {
		return _task_state(*task);
	}
	return _read_json_file(_manifest_path(p_asset_id));
}

void SolersAssetService::_cleanup_finished_task(const String &p_asset_id) const {
	Task *task = nullptr;
	{
		MutexLock lock(tasks_mutex);
		Task *const *found = const_cast<HashMap<String, Task *> *>(&tasks)->getptr(p_asset_id);
		if (!found || !*found) {
			return;
		}
		task = *found;
		const String status = String(_task_state(task).get("status", String()));
		if (status != "ready" && status != "draft" && status != "failed" && status != "cancelled" && status != "interrupted") {
			return;
		}
		if (!task->done.is_set()) {
			return;
		}
		const_cast<HashMap<String, Task *> *>(&tasks)->erase(p_asset_id);
	}
	if (task->thread.is_started()) {
		task->thread.wait_to_finish();
	}
	memdelete(task);
}

Dictionary SolersAssetService::_generate(const Dictionary &p_args, const String &p_session_id) {
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	if (kind != "3d" && kind != "music" && kind != "sfx") {
		return _error("INVALID_ARGUMENT", "kind must be 3d, music, or sfx.");
	}
	const String prompt = String(p_args.get("prompt", String())).strip_edges();
	const Array input_attachment_ids = p_args.get("input_attachments", Array());
	const Array source_attachments = _solers_selected_attachments(p_args.get("_attachments", Array()), input_attachment_ids);
	if (!input_attachment_ids.is_empty() && source_attachments.size() != input_attachment_ids.size()) {
		return _error("INVALID_ATTACHMENT_REFERENCE", "One or more image attachment ids could not be resolved in the current conversation.");
	}
	if (prompt.is_empty() && source_attachments.is_empty()) {
		return _error("INVALID_ARGUMENT", "prompt is required.");
	}
	if (!source_attachments.is_empty() && kind != "3d") {
		return _error("INVALID_ARGUMENT", "Image attachments are only supported for 3D assets.");
	}
	if (source_attachments.size() > 4) {
		return _error("INVALID_ARGUMENT", "Image-to-3D accepts up to 4 image attachments.");
	}
	String provider = String(p_args.get("provider", String())).strip_edges().to_lower();
	const Dictionary provider_config = _provider_config(kind, provider);
	provider = String(provider_config.get("provider", provider));
	Dictionary provider_options = p_args.get("provider_options", Dictionary());
	if (provider_options.has("pose_mode")) {
		const String pose_mode = String(provider_options.get("pose_mode", String())).strip_edges().to_lower();
		if (kind != "3d") {
			return _error("INVALID_ARGUMENT", "pose_mode is only supported for 3D assets.");
		}
		if (provider != "meshy") {
			return _error("UNSUPPORTED_PROVIDER_OPTION", vformat("pose_mode is not supported by provider %s.", provider));
		}
		if (pose_mode != "a-pose" && pose_mode != "t-pose") {
			return _error("INVALID_ARGUMENT", "pose_mode must be a-pose or t-pose.");
		}
		provider_options["pose_mode"] = pose_mode;
	}
	if (kind == "3d" && provider == "meshy") {
		String option_error;
		const char *const bool_options[] = { "should_texture", "enable_pbr", "hd_texture", "should_remesh", "image_enhancement", "remove_lighting", "moderation", "auto_size", "save_pre_remeshed_model", "alpha_thumbnail" };
		for (const char *option : bool_options) {
			if (!_solers_validate_bool_option(provider_options, option, option_error)) {
				return _error("INVALID_ARGUMENT", option_error);
			}
		}
		if (provider_options.has("target_formats") && provider_options["target_formats"].get_type() != Variant::ARRAY) {
			return _error("INVALID_ARGUMENT", "target_formats must be an array.");
		}
		Dictionary integer_properties;
		Dictionary target_polycount = _solers_option_schema("integer", "Target Polygons");
		integer_properties["target_polycount"] = target_polycount;
		String normalize_error;
		if (!_solers_normalize_integer_options(provider_options, integer_properties, normalize_error)) {
			return _error("INVALID_ARGUMENT", normalize_error);
		}
		if (provider_options.has("target_polycount") && (int64_t)provider_options.get("target_polycount", 0) <= 0) {
			return _error("INVALID_ARGUMENT", "target_polycount must be positive.");
		}
		if ((bool)provider_options.get("enable_pbr", false) && provider_options.has("should_texture") && !(bool)provider_options.get("should_texture", true)) {
			return _error("INVALID_ARGUMENT", "enable_pbr requires should_texture=true.");
		}
		if (provider_options.has("texture_prompt") && provider_options.has("texture_image_url")) {
			return _error("INVALID_ARGUMENT", "Use texture_prompt or texture_image_url, not both.");
		}
		if (provider_options.has("target_polycount")) {
			if (provider_options.has("should_remesh") && !(bool)provider_options.get("should_remesh", false)) {
				return _error("INVALID_ARGUMENT", "target_polycount requires should_remesh=true; otherwise Meshy returns its highest-detail mesh.");
			}
			provider_options["should_remesh"] = true;
		}
	}
	const String asset_id = vformat("%s_%s", itos((int64_t)Time::get_singleton()->get_unix_time_from_system()), (kind + prompt + provider).md5_text().substr(0, 10));

	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = kind;
	manifest["name"] = p_args.get("name", prompt);
	manifest["prompt"] = prompt;
	manifest["profile"] = p_args.get("profile", "game_default");
	manifest["provider"] = provider;
	manifest["base_url"] = provider_config.get("base_url", String());
	manifest["provider_options"] = provider_options;
	if (!p_session_id.is_empty()) {
		manifest["session_id"] = p_session_id;
	}
	if (!source_attachments.is_empty()) {
		manifest["source_attachments"] = source_attachments;
	}
	if (kind == "3d") {
		const String generation_mode = source_attachments.is_empty() ? "text_to_3d" : (source_attachments.size() == 1 ? "image_to_3d" : "multi_image_to_3d");
		manifest["generation_mode"] = generation_mode;
		manifest["provider_endpoint"] = generation_mode == "text_to_3d" ? "/openapi/v2/text-to-3d" : (generation_mode == "image_to_3d" ? "/openapi/v1/image-to-3d" : "/openapi/v1/multi-image-to-3d");
		manifest["input_attachment_ids"] = input_attachment_ids.duplicate(true);
		Dictionary traits;
		traits["model_state"] = "static_model";
		manifest["traits"] = traits;
	}
	manifest["status"] = "queued";
	manifest["created_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	manifest["files"] = Array();
	return _queue_manifest(manifest, provider_config);
}

Dictionary SolersAssetService::generate(const Dictionary &p_args) {
	return _generate(p_args, String());
}

Dictionary SolersAssetService::generate_for_session(const Dictionary &p_args, const String &p_session_id) {
	return _generate(p_args, p_session_id);
}

void SolersAssetService::_catalog_fetch_page(uint32_t p_index, CatalogPageBatch *p_batch) {
	CatalogPageJob &job = p_batch->jobs[p_index];
	job.response = _http_request("GET", job.url, Vector<String>(), PackedByteArray(), 60000, 8 * 1024 * 1024, p_batch->cancel_requested);
}

static PackedStringArray _solers_catalog_terms(const String &p_query) {
	const PackedStringArray raw_terms = p_query.to_lower().split(" ", false);
	PackedStringArray terms;
	HashSet<String> seen;
	for (int i = 0; i < raw_terms.size(); i++) {
		const String term = raw_terms[i].strip_edges();
		if (!term.is_empty() && !seen.has(term)) {
			seen.insert(term);
			terms.push_back(term);
		}
	}
	return terms;
}

static bool _solers_catalog_array_contains(const Array &p_values, const String &p_term) {
	for (int i = 0; i < p_values.size(); i++) {
		if (String(p_values[i]).to_lower().contains(p_term)) {
			return true;
		}
	}
	return false;
}

static void _solers_catalog_add_field(Array &r_fields, const String &p_field) {
	if (!r_fields.has(p_field)) {
		r_fields.push_back(p_field);
	}
}

struct SolersCatalogRelevanceSort {
	bool operator()(const Dictionary &p_a, const Dictionary &p_b) const {
		const char *rank_fields[] = { "_rank_exact", "_rank_primary", "_rank_category", "_rank_description" };
		for (const char *field : rank_fields) {
			const int64_t a_rank = p_a.get(field, 0);
			const int64_t b_rank = p_b.get(field, 0);
			if (a_rank != b_rank) {
				return a_rank > b_rank;
			}
		}
		const int64_t a_downloads = p_a.get("download_count", 0);
		const int64_t b_downloads = p_b.get("download_count", 0);
		if (a_downloads != b_downloads) {
			return a_downloads > b_downloads;
		}
		const int64_t a_popularity = p_a.get("_popularity_rank", INT64_MAX);
		const int64_t b_popularity = p_b.get("_popularity_rank", INT64_MAX);
		if (a_popularity != b_popularity) {
			return a_popularity < b_popularity;
		}
		return String(p_a.get("asset_id", String())) < String(p_b.get("asset_id", String()));
	}
};

Array SolersAssetService::rank_catalog_assets(const Array &p_assets, const String &p_query) {
	const String normalized_query = p_query.strip_edges().to_lower();
	const PackedStringArray terms = _solers_catalog_terms(normalized_query);
	Vector<Dictionary> matches;
	for (int i = 0; i < p_assets.size(); i++) {
		Dictionary item = Dictionary(p_assets[i]).duplicate(true);
		const String asset_id = String(item.get("asset_id", String())).to_lower();
		const String display_name = String(item.get("display_name", String())).to_lower();
		const String description = String(item.get("description", String())).to_lower();
		const Array tags = item.get("tags", Array());
		const Array categories = item.get("categories", Array());
		Array matched_terms;
		Array matched_fields;
		int primary_hits = 0;
		int category_hits = 0;
		int description_hits = 0;
		for (int term_index = 0; term_index < terms.size(); term_index++) {
			const String term = terms[term_index];
			bool matched = false;
			if (asset_id.contains(term)) {
				matched = true;
				primary_hits++;
				_solers_catalog_add_field(matched_fields, "asset_id");
			}
			if (display_name.contains(term)) {
				matched = true;
				primary_hits++;
				_solers_catalog_add_field(matched_fields, "display_name");
			}
			if (_solers_catalog_array_contains(tags, term)) {
				matched = true;
				primary_hits++;
				_solers_catalog_add_field(matched_fields, "tags");
			}
			if (_solers_catalog_array_contains(categories, term)) {
				matched = true;
				category_hits++;
				_solers_catalog_add_field(matched_fields, "categories");
			}
			if (description.contains(term)) {
				matched = true;
				description_hits++;
				_solers_catalog_add_field(matched_fields, "description");
			}
			if (matched) {
				matched_terms.push_back(term);
			}
		}
		if (matched_terms.is_empty()) {
			continue;
		}
		item["matched_terms"] = matched_terms;
		item["matched_fields"] = matched_fields;
		item["_rank_exact"] = asset_id == normalized_query || display_name == normalized_query ? 1 : 0;
		item["_rank_primary"] = primary_hits;
		item["_rank_category"] = category_hits;
		item["_rank_description"] = description_hits;
		matches.push_back(item);
	}
	matches.sort_custom<SolersCatalogRelevanceSort>();
	Array ranked;
	for (int i = 0; i < matches.size(); i++) {
		Dictionary item = matches[i];
		item.erase("_rank_exact");
		item.erase("_rank_primary");
		item.erase("_rank_category");
		item.erase("_rank_description");
		item.erase("_popularity_rank");
		ranked.push_back(item);
	}
	return ranked;
}

bool SolersAssetService::is_trusted_plugin(const Dictionary &p_args) {
	if (String(p_args.get("source", String())).to_lower() != "bundled" || String(p_args.get("plugin_id", String())).to_lower() != SOLERS_TERRAIN3D_ID) {
		return false;
	}
	const String version = String(p_args.get("version", String()));
	const String sha256 = String(p_args.get("sha256", String())).to_lower();
	return (version.is_empty() || version == String(SOLERS_TERRAIN3D_VERSION)) && (sha256.is_empty() || sha256 == String(SOLERS_TERRAIN3D_SHA256));
}

Dictionary SolersAssetService::plugin_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String query = String(p_args.get("query", String())).strip_edges();
	if (query.is_empty()) {
		return _error("INVALID_ARGUMENT", "Plugin search requires a non-empty query.");
	}
	const int limit = CLAMP((int)p_args.get("limit", 20), 1, 50);
	Array plugins;
	const Dictionary trusted = _solers_terrain3d_package();
	if (String(trusted.get("title", String())).to_lower().contains(query.to_lower()) || String(trusted.get("description", String())).to_lower().contains(query.to_lower())) {
		plugins.push_back(trusted);
	}

	const String url = vformat("https://godotengine.org/asset-library/api/asset?godot_version=%d.%d&filter=%s&page=0&max_results=%d", GODOT_VERSION_MAJOR, GODOT_VERSION_MINOR, query.uri_encode(), limit);
	const Dictionary response = _http_request("GET", url, Vector<String>(), PackedByteArray(), 60000, 4 * 1024 * 1024, p_cancel_requested);
	if (!(bool)response.get("ok", false)) {
		const Dictionary error = response.get("error", Dictionary());
		return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "PLUGIN_SEARCH_FAILED", String(error.get("message", "Godot Asset Library search failed.")));
	}
	const Dictionary root = _parse_json_body(response);
	const Array results = root.get("result", Array());
	for (const Variant &value : results) {
		if (plugins.size() >= limit) {
			break;
		}
		const Dictionary asset = value;
		if (String(asset.get("asset_id", String())) == "3892") {
			continue;
		}
		Dictionary plugin;
		plugin["source"] = "assetlib";
		plugin["plugin_id"] = asset.get("asset_id", String());
		plugin["asset_id"] = asset.get("asset_id", String());
		plugin["title"] = asset.get("title", String());
		plugin["author"] = asset.get("author", String());
		plugin["version"] = asset.get("version_string", String());
		plugin["license"] = asset.get("cost", String());
		plugin["godot_version"] = asset.get("godot_version", String());
		plugin["category"] = asset.get("category", String());
		plugin["support_level"] = asset.get("support_level", String());
		plugin["trusted"] = false;
		plugins.push_back(plugin);
	}
	Dictionary data;
	data["plugins"] = plugins;
	data["count"] = plugins.size();
	data["total_items"] = root.get("total_items", results.size());
	return _ok(data);
}

Dictionary SolersAssetService::plugin_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String source = String(p_args.get("source", String())).strip_edges().to_lower();
	const String plugin_id = String(p_args.get("plugin_id", String())).strip_edges().to_lower();
	if ((source != "bundled" && source != "assetlib") || plugin_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "Plugin inspection requires source bundled or assetlib and an exact plugin_id.");
	}

	Dictionary plugin_metadata;
	String archive_path;
	String expected_sha256;
	const bool trusted = source == "bundled";
	if (trusted) {
		if (plugin_id != SOLERS_TERRAIN3D_ID) {
			return _error("PLUGIN_NOT_FOUND", vformat("Unknown bundled plugin: %s", plugin_id));
		}
		plugin_metadata = _solers_terrain3d_package();
		archive_path = _solers_terrain3d_archive_path();
		expected_sha256 = plugin_metadata.get("sha256", String());
		if (archive_path.is_empty()) {
			return _error("PLUGIN_BUNDLE_MISSING", "The verified Terrain3D bundle is missing. Rebuild Solers or set SOLERS_TERRAIN3D_ARCHIVE to the official archive.", false);
		}
	} else {
		const Dictionary response = _http_request("GET", "https://godotengine.org/asset-library/api/asset/" + plugin_id.uri_encode(), Vector<String>(), PackedByteArray(), 60000, 4 * 1024 * 1024, p_cancel_requested);
		if (!(bool)response.get("ok", false)) {
			const Dictionary error = response.get("error", Dictionary());
			return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "PLUGIN_INSPECTION_FAILED", String(error.get("message", "Godot Asset Library inspection failed.")));
		}
		const Dictionary detail = _parse_json_body(response);
		if (detail.is_empty() || String(detail.get("type", String())) != "addon" || String(detail.get("download_url", String())).is_empty()) {
			return _error("PLUGIN_NOT_FOUND", "The requested Asset Library entry is not an installable addon.");
		}
		const PackedStringArray required_version = String(detail.get("godot_version", String())).split(".", false);
		if (required_version.size() >= 2 && (required_version[0].to_int() != GODOT_VERSION_MAJOR || required_version[1].to_int() > GODOT_VERSION_MINOR)) {
			return _error("PLUGIN_INCOMPATIBLE", vformat("Plugin requires Godot %s, but this editor is %d.%d.", detail.get("godot_version", String()), GODOT_VERSION_MAJOR, GODOT_VERSION_MINOR), false);
		}
		plugin_metadata["source"] = source;
		plugin_metadata["plugin_id"] = plugin_id;
		plugin_metadata["asset_id"] = plugin_id;
		plugin_metadata["title"] = detail.get("title", String());
		plugin_metadata["author"] = detail.get("author", String());
		plugin_metadata["version"] = detail.get("version_string", String());
		plugin_metadata["source_version"] = detail.get("version", String());
		plugin_metadata["license"] = detail.get("cost", String());
		plugin_metadata["godot_version"] = detail.get("godot_version", String());
		plugin_metadata["browse_url"] = detail.get("browse_url", String());
		plugin_metadata["issues_url"] = detail.get("issues_url", String());
		plugin_metadata["description"] = detail.get("description", String());
		plugin_metadata["support_level"] = detail.get("support_level", String());
		plugin_metadata["trusted"] = false;
		expected_sha256 = String(detail.get("download_hash", String())).to_lower();
		const String cache_dir = _solers_plugin_cache_root().path_join("assetlib").path_join(_safe_slug(plugin_id)).path_join(_safe_slug(plugin_metadata.get("version", String())));
		archive_path = cache_dir.path_join("package.zip");
		if ((bool)p_args.get("refresh", false) || !FileAccess::exists(archive_path)) {
			const Dictionary download = _http_request("GET", detail.get("download_url", String()), Vector<String>(), PackedByteArray(), 180000, 512LL * 1024LL * 1024LL, p_cancel_requested);
			if (!(bool)download.get("ok", false)) {
				const Dictionary error = download.get("error", Dictionary());
				return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "PLUGIN_DOWNLOAD_FAILED", String(error.get("message", "Plugin download failed.")));
			}
			String write_error;
			if (!_write_bytes_atomic(archive_path, download.get("body", PackedByteArray()), write_error)) {
				return _error("PLUGIN_CACHE_WRITE_FAILED", write_error, false);
			}
		}
	}

	const String actual_sha256 = FileAccess::get_sha256(archive_path).to_lower();
	if (actual_sha256.is_empty() || (!expected_sha256.is_empty() && actual_sha256 != expected_sha256)) {
		if (!trusted) {
			DirAccess::remove_absolute(archive_path);
		}
		return _error("PLUGIN_HASH_MISMATCH", vformat("Plugin archive SHA-256 mismatch. Expected %s, got %s.", expected_sha256, actual_sha256), false);
	}
	Dictionary package = _solers_inspect_plugin_archive(archive_path, !trusted);
	if (!(bool)package.get("ok", false)) {
		return package;
	}
	Dictionary inspection = package.get("data", Dictionary());
	for (const Variant *key = plugin_metadata.next(nullptr); key; key = plugin_metadata.next(key)) {
		inspection[*key] = plugin_metadata[*key];
	}
	inspection["sha256"] = actual_sha256;
	Dictionary raw_contract = inspection.get("_agent_contract", Dictionary());
	String contract_error;
	if (raw_contract.is_empty() && !_solers_agent_contract_from_archive(inspection, raw_contract, contract_error)) {
		return _error("PLUGIN_CONTRACT_INVALID", contract_error, false);
	}
	if (!raw_contract.is_empty()) {
		Dictionary contract;
		if (!_solers_validate_agent_contract(raw_contract, plugin_id, inspection.get("version", String()), actual_sha256, contract, contract_error)) {
			return _error("PLUGIN_CONTRACT_INVALID", contract_error, false);
		}
		inspection["agent_contract"] = contract;
	}
	inspection.erase("_agent_contract");
	inspection["inspection_id"] = _solers_plugin_inspection_key(source, plugin_id, inspection.get("version", String()), actual_sha256);
	const String inspection_key = inspection.get("inspection_id", String());
	{
		MutexLock lock(catalog_cache_mutex);
		plugin_inspections[inspection_key] = inspection.duplicate(true);
	}
	return _ok(_solers_public_plugin_inspection(inspection));
}

Dictionary SolersAssetService::plugin_agent_contract(const Dictionary &p_args) const {
	const String source = String(p_args.get("source", String())).strip_edges().to_lower();
	const String plugin_id = String(p_args.get("plugin_id", String())).strip_edges().to_lower();
	const String version = String(p_args.get("version", String())).strip_edges();
	const String sha256 = String(p_args.get("sha256", String())).strip_edges().to_lower();
	if (source.is_empty() || plugin_id.is_empty() || version.is_empty() || sha256.length() != 64) {
		return _error("INVALID_ARGUMENT", "source, plugin_id, version, and SHA-256 are required.");
	}
	{
		MutexLock lock(catalog_cache_mutex);
		const Dictionary inspection = plugin_inspections.get(_solers_plugin_inspection_key(source, plugin_id, version, sha256), Dictionary());
		const Dictionary contract = inspection.get("agent_contract", Dictionary());
		if (!contract.is_empty()) {
			return _ok(contract);
		}
	}
	if (source == "bundled" && plugin_id == SOLERS_TERRAIN3D_ID && version == SOLERS_TERRAIN3D_VERSION && sha256 == SOLERS_TERRAIN3D_SHA256) {
		Dictionary contract;
		String error;
		if (_solers_validate_agent_contract(_solers_terrain3d_agent_contract(), plugin_id, SOLERS_TERRAIN3D_VERSION, SOLERS_TERRAIN3D_SHA256, contract, error)) {
			return _ok(contract);
		}
	}
	return _error("PLUGIN_CONTRACT_UNAVAILABLE", "This plugin does not provide a validated Agent Contract.");
}

Dictionary SolersAssetService::plugin_list(const Dictionary &p_args) const {
	const Dictionary lock_file = _read_json_file(SOLERS_PLUGIN_LOCK_PATH);
	const Dictionary installed_plugins = lock_file.get("plugins", Dictionary());
	Array plugins;
	for (const Variant *key = installed_plugins.next(nullptr); key; key = installed_plugins.next(key)) {
		const Dictionary entry = installed_plugins[*key];
		const Array files = entry.get("files", Array());
		bool installed = !files.is_empty();
		Array load_errors = entry.get("load_errors", Array());
		for (const Variant &file : files) {
			if (!FileAccess::exists(String(file))) {
				installed = false;
				load_errors.push_back(vformat("Missing installed file: %s", file));
			}
		}
		bool enabled = installed;
		const Array names = entry.get("plugin_names", Array());
		EditorInterface *editor = EditorInterface::get_singleton();
		for (const Variant &name : names) {
			enabled = enabled && editor && editor->is_plugin_enabled(String(name));
		}
		const Dictionary agent_contract = entry.get("agent_contract", Dictionary());
		const Array required_classes = agent_contract.get("entry_classes", Array());
		PackedStringArray missing_classes;
		for (const Variant &value : required_classes) {
			const String class_name = value;
			if (!ClassDB::class_exists(class_name)) {
				missing_classes.push_back(class_name);
			}
		}
		Dictionary item = entry.duplicate(true);
		const bool restart_required = entry.get("restart_required", false);
		item["key"] = *key;
		item["installed"] = installed;
		item["enabled"] = enabled;
		item["ready"] = installed && enabled && !restart_required && missing_classes.is_empty() && load_errors.is_empty();
		item["restart_required"] = restart_required;
		item["missing_classes"] = missing_classes;
		item["load_errors"] = load_errors;
		plugins.push_back(item);
	}
	Dictionary data;
	data["plugins"] = plugins;
	data["count"] = plugins.size();
	data["lock_path"] = SOLERS_PLUGIN_LOCK_PATH;
	return _ok(data);
}

Dictionary SolersAssetService::plugin_ensure(const Dictionary &p_args) {
	const String source = String(p_args.get("source", String())).strip_edges().to_lower();
	const String plugin_id = String(p_args.get("plugin_id", String())).strip_edges().to_lower();
	const String version = String(p_args.get("version", String())).strip_edges();
	const String sha256 = String(p_args.get("sha256", String())).strip_edges().to_lower();
	if ((source != "bundled" && source != "assetlib") || plugin_id.is_empty() || version.is_empty() || sha256.length() != 64) {
		return _error("INVALID_ARGUMENT", "Plugin ensure requires the exact source, plugin_id, version, and SHA-256 returned by plugin.inspect.");
	}
	const String inspection_key = _solers_plugin_inspection_key(source, plugin_id, version, sha256);
	Dictionary lock_file = _read_json_file(SOLERS_PLUGIN_LOCK_PATH);
	if (lock_file.is_empty()) {
		lock_file["version"] = 1;
		lock_file["plugins"] = Dictionary();
	}
	Dictionary installed_plugins = lock_file.get("plugins", Dictionary());
	const String plugin_key = _solers_plugin_key(source, plugin_id);
	const Dictionary previous = installed_plugins.get(plugin_key, Dictionary());
	Dictionary inspection;
	{
		MutexLock lock(catalog_cache_mutex);
		inspection = plugin_inspections.get(inspection_key, Dictionary()).duplicate(true);
	}
	if (inspection.is_empty()) {
		const bool exact_locked_version = String(previous.get("version", String())) == version && String(previous.get("sha256", String())).to_lower() == sha256;
		const Array locked_files = previous.get("files", Array());
		bool locked_files_present = exact_locked_version && !locked_files.is_empty();
		for (const Variant &file : locked_files) {
			locked_files_present = locked_files_present && FileAccess::exists(String(file));
		}
		if (!locked_files_present) {
			return _error("PLUGIN_INSPECTION_REQUIRED", "Inspect this exact plugin version once before installation. A pinned installed version may be retried after restart without another download.");
		}
		inspection = previous.duplicate(true);
		inspection["target_files"] = locked_files;
	}
	const Array target_files = inspection.get("target_files", Array());
	bool files_present = !target_files.is_empty();
	for (const Variant &file : target_files) {
		files_present = files_present && FileAccess::exists(String(file));
	}
	const bool unchanged = files_present && String(previous.get("version", String())) == version && String(previous.get("sha256", String())).to_lower() == sha256;

	Dictionary entry;
	entry["source"] = source;
	entry["plugin_id"] = plugin_id;
	entry["asset_id"] = inspection.get("asset_id", plugin_id);
	entry["title"] = inspection.get("title", plugin_id);
	entry["author"] = inspection.get("author", String());
	entry["version"] = version;
	entry["sha256"] = sha256;
	entry["license"] = inspection.get("license", String());
	entry["browse_url"] = inspection.get("browse_url", String());
	entry["files"] = target_files;
	entry["plugin_names"] = inspection.get("plugin_names", Array());
	entry["plugin_configs"] = inspection.get("plugin_configs", Array());
	entry["gdextensions"] = inspection.get("gdextensions", Array());
	entry["desired_enabled"] = true;
	entry["agent_contract"] = inspection.get("agent_contract", previous.get("agent_contract", Dictionary()));
	entry["installed_at_unix"] = (int64_t)Time::get_singleton()->get_unix_time_from_system();
	installed_plugins[plugin_key] = entry;
	lock_file["plugins"] = installed_plugins;

	Dictionary install_result;
	if (!unchanged) {
		HashSet<String> new_files;
		for (const Variant &file : target_files) {
			new_files.insert(String(file));
		}
		PackedStringArray overwrites;
		PackedStringArray removals;
		const Array previous_files = previous.get("files", Array());
		for (const Variant &file : previous_files) {
			const String path = file;
			if (new_files.has(path)) {
				overwrites.push_back(path);
			} else {
				removals.push_back(path);
			}
		}
		install_result = EditorAssetPackageInstaller::install_package(inspection.get("_archive_path", String()), "res://", inspection.get("_mappings", Dictionary()), inspection.get("_selected_files", PackedStringArray()), overwrites, removals, SOLERS_PLUGIN_LOCK_PATH, lock_file);
		if (!(bool)install_result.get("ok", false)) {
			return install_result;
		}
		files_present = !target_files.is_empty();
		for (const Variant &file : target_files) {
			files_present = files_present && FileAccess::exists(String(file));
		}
	}

	// Persist the installed entry first; the finalize phase rebuilds
	// everything it needs from the lock file alone.
	installed_plugins[plugin_key] = entry;
	lock_file["plugins"] = installed_plugins;
	String lock_error;
	if (!_write_json_atomic(SOLERS_PLUGIN_LOCK_PATH, lock_file, lock_error)) {
		return _error("PLUGIN_LOCK_WRITE_FAILED", lock_error, false);
	}

	// The editor's own scan pipeline is the authority for everything that
	// follows an install: it registers global script class names and runs
	// ensure_extensions_loaded, which loads new GDExtensions and keeps
	// extension_list.cfg current for runtime processes. Enabling the plugin
	// before that pipeline finishes would parse addon scripts against a
	// half-built class registry, so readiness is evaluated only once the
	// scan is done (through the registry's ready/poll continuation).
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	if (!unchanged && filesystem) {
		filesystem->scan_changes();
	}
	Dictionary finalize_args;
	finalize_args["plugin_key"] = plugin_key;
	finalize_args["installed"] = !unchanged;
	if (filesystem && filesystem->is_scanning()) {
		finalize_args["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + 120000);
		Dictionary data = finalize_args.duplicate(true);
		data["status"] = "pending";
		data["poll_args"] = finalize_args;
		return _ok(data);
	}
	return plugin_ensure_finalize(finalize_args);
}

bool SolersAssetService::plugin_ensure_ready(const Dictionary &p_args) const {
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	if (!filesystem || !filesystem->is_scanning()) {
		return true;
	}
	return OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)p_args.get("deadline_msec", 0);
}

Dictionary SolersAssetService::plugin_ensure_finalize(const Dictionary &p_args) {
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	if (filesystem && filesystem->is_scanning()) {
		return _error("FILESYSTEM_SCAN_TIMEOUT", "The editor filesystem scan did not finish in time; call plugin.ensure again once it settles.");
	}
	const String plugin_key = p_args.get("plugin_key", String());
	Dictionary lock_file = _read_json_file(SOLERS_PLUGIN_LOCK_PATH);
	Dictionary installed_plugins = lock_file.get("plugins", Dictionary());
	Dictionary entry = installed_plugins.get(plugin_key, Dictionary());
	if (entry.is_empty()) {
		return _error("PLUGIN_NOT_INSTALLED", vformat("No installed plugin entry for %s.", plugin_key), false);
	}

	const Array target_files = entry.get("files", Array());
	bool files_present = !target_files.is_empty();
	for (const Variant &file : target_files) {
		files_present = files_present && FileAccess::exists(String(file));
	}

	Array load_errors;
	Array restart_reasons;
	bool restart_required = false;
	// The scan already ran the engine's ensure_extensions_loaded. An
	// extension that is still absent from the current process needs the
	// editor restart the engine itself decided on.
	GDExtensionManager *extension_manager = GDExtensionManager::get_singleton();
	Vector<String> registered_classes;
	const Array gdextensions = entry.get("gdextensions", Array());
	for (const Variant &value : gdextensions) {
		const String extension_path = value;
		if (!extension_manager || !extension_manager->is_extension_loaded(extension_path)) {
			restart_required = true;
			restart_reasons.push_back(vformat("%s is not loaded in the current editor process; restart the editor, then call plugin.ensure with the same arguments.", extension_path));
			continue;
		}
		const Ref<GDExtension> extension = extension_manager->get_extension(extension_path);
		if (extension.is_valid()) {
			List<StringName> classes;
			ClassDB::get_extension_class_list(extension, &classes);
			for (const StringName &class_name : classes) {
				registered_classes.push_back(String(class_name));
			}
		}
	}
	registered_classes.sort();
	Array class_list;
	for (const String &class_name : registered_classes) {
		class_list.push_back(class_name);
	}

	EditorInterface *editor = EditorInterface::get_singleton();
	const Array plugin_names = entry.get("plugin_names", Array());
	bool enabled = plugin_names.is_empty();
	if (!plugin_names.is_empty()) {
		enabled = editor != nullptr;
		for (const Variant &value : plugin_names) {
			const String plugin_name = value;
			if (editor && !editor->is_plugin_enabled(plugin_name) && !restart_required) {
				editor->set_plugin_enabled(plugin_name, true);
			}
			enabled = enabled && editor && editor->is_plugin_enabled(plugin_name);
		}
	}
	if (!enabled && !restart_required) {
		load_errors.push_back("The editor plugin could not be enabled in the current editor process.");
	}

	const Dictionary agent_contract = entry.get("agent_contract", Dictionary());
	const Array required_classes = agent_contract.get("entry_classes", Array());
	PackedStringArray missing_classes;
	for (const Variant &value : required_classes) {
		const String class_name = value;
		if (!ClassDB::class_exists(class_name)) {
			missing_classes.push_back(class_name);
		}
	}
	if (!missing_classes.is_empty() && !restart_required) {
		load_errors.push_back(vformat("Declared Agent Contract classes are not registered: %s", String(", ").join(missing_classes)));
	}

	const bool ready = files_present && enabled && !restart_required && missing_classes.is_empty() && load_errors.is_empty();
	entry["registered_classes"] = class_list;
	entry["enabled"] = enabled;
	entry["ready"] = ready;
	entry["restart_required"] = restart_required;
	entry["restart_reasons"] = restart_reasons;
	entry["load_errors"] = load_errors;
	installed_plugins[plugin_key] = entry;
	lock_file["plugins"] = installed_plugins;
	String lock_error;
	const bool lock_updated = _write_json_atomic(SOLERS_PLUGIN_LOCK_PATH, lock_file, lock_error);
	Dictionary data;
	data["plugin"] = entry;
	data["installed"] = p_args.get("installed", false);
	data["idempotent"] = !(bool)p_args.get("installed", false);
	data["enabled"] = enabled;
	data["ready"] = ready;
	data["restart_required"] = restart_required;
	data["restart_reasons"] = restart_reasons;
	data["registered_classes"] = class_list;
	data["missing_classes"] = missing_classes;
	data["load_errors"] = load_errors;
	if (!lock_updated) {
		Dictionary result = _error("PLUGIN_LOCK_WRITE_FAILED", lock_error, false);
		result["data"] = data;
		return result;
	}
	if (restart_required) {
		Dictionary result = _error("PLUGIN_RESTART_REQUIRED", restart_reasons.is_empty() ? "Installation completed; restart the editor once, then call plugin.ensure with the same arguments." : String(restart_reasons[0]));
		result["data"] = data;
		return result;
	}
	// The verdict follows the authoritative usability facts: files on disk and
	// every declared entry class registered in ClassDB. Residual issues (for
	// example an addon editor script that failed to enable) stay visible as
	// load_errors data without blocking a plugin whose classes already work.
	if (!files_present || !missing_classes.is_empty()) {
		Dictionary result = _error("PLUGIN_LOAD_FAILED", load_errors.is_empty() ? "The plugin failed its runtime readiness checks." : String(load_errors[0]));
		result["data"] = data;
		return result;
	}
	return _ok(data);
}

Dictionary SolersAssetService::catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String provider = String(p_args.get("provider", String())).strip_edges().to_lower();
	const String query = String(p_args.get("query", String())).strip_edges();
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	if ((provider != "ambientcg" && provider != "polyhaven") || query.is_empty() || (kind != "material" && kind != "hdri" && kind != "3d")) {
		return _error("INVALID_ARGUMENT", "Catalog search requires provider ambientcg or polyhaven, a query, and kind material, hdri, or 3d.");
	}
	const int limit = MAX(1, MIN((int)p_args.get("limit", 20), 50));
	const int offset = MAX(0, (int)p_args.get("offset", 0));
	Array catalog_assets;
	if (provider == "ambientcg") {
		{
			MutexLock lock(catalog_cache_mutex);
			catalog_assets = ambientcg_assets_cache.get(kind, Array());
		}
		if ((bool)p_args.get("refresh", false) || catalog_assets.is_empty()) {
			const String api_type = kind == "3d" ? "3d-model" : kind;
			const int page_size = 500;
			const String base_url = "https://ambientcg.com/api/v3/assets?type=" + api_type + "&sort=popular&limit=" + itos(page_size) + "&include=id,type,title,url,tags,dimensions";
			const Dictionary first_response = _http_request("GET", base_url, Vector<String>(), PackedByteArray(), 60000, 8 * 1024 * 1024, p_cancel_requested);
			if (!(bool)first_response.get("ok", false)) {
				const Dictionary error = first_response.get("error", Dictionary());
				return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "CATALOG_SEARCH_FAILED", String(error.get("message", "ambientCG catalog request failed.")));
			}
			const Dictionary first_root = _parse_json_body(first_response);
			const int official_total = (int)first_root.get("totalResults", 0);
			Array pages;
			pages.push_back(first_root.get("assets", Array()));
			const int remaining_pages = MAX(0, (official_total + page_size - 1) / page_size - 1);
			Vector<CatalogPageJob> jobs;
			jobs.resize(remaining_pages);
			for (int i = 0; i < remaining_pages; i++) {
				jobs.write[i].url = base_url + "&offset=" + itos((i + 1) * page_size);
			}
			if (!jobs.is_empty()) {
				CatalogPageBatch batch;
				batch.jobs = jobs.ptrw();
				batch.cancel_requested = p_cancel_requested;
				WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
				if (pool) {
					const WorkerThreadPool::GroupID group = pool->add_template_group_task(this, &SolersAssetService::_catalog_fetch_page, &batch, jobs.size(), MIN(4, jobs.size()), false, "ambientCG catalog pages");
					pool->wait_for_group_task_completion(group);
				} else {
					for (int i = 0; i < jobs.size(); i++) {
						_catalog_fetch_page(i, &batch);
					}
				}
				for (int i = 0; i < jobs.size(); i++) {
					if (!(bool)jobs[i].response.get("ok", false)) {
						const Dictionary error = jobs[i].response.get("error", Dictionary());
						return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "CATALOG_SEARCH_FAILED", String(error.get("message", "ambientCG catalog page request failed.")));
					}
					pages.push_back(_parse_json_body(jobs[i].response).get("assets", Array()));
				}
			}
			catalog_assets.clear();
			int popularity_rank = 0;
			for (int page_index = 0; page_index < pages.size(); page_index++) {
				const Array page = pages[page_index];
				for (int i = 0; i < page.size(); i++) {
					const Dictionary asset = page[i];
					Dictionary item;
					item["provider"] = provider;
					item["asset_id"] = asset.get("id", String());
					item["kind"] = kind;
					item["display_name"] = asset.get("title", String());
					item["tags"] = asset.get("tags", Array());
					item["dimensions"] = asset.get("dimensions", Dictionary());
					item["source_url"] = asset.get("url", String());
					item["license"] = "CC0-1.0";
					item["details_required"] = true;
					item["_popularity_rank"] = popularity_rank++;
					catalog_assets.push_back(item);
				}
			}
			MutexLock lock(catalog_cache_mutex);
			ambientcg_assets_cache[kind] = catalog_assets.duplicate(true);
		}
	} else {
		Dictionary catalog;
		{
			MutexLock lock(catalog_cache_mutex);
			catalog = polyhaven_assets_cache.get(kind, Dictionary());
		}
		if ((bool)p_args.get("refresh", false) || catalog.is_empty()) {
			const String api_type = kind == "material" ? "textures" : (kind == "hdri" ? "hdris" : "models");
			const Dictionary response = _http_request("GET", "https://api.polyhaven.com/assets?type=" + api_type, _polyhaven_headers(), PackedByteArray(), 60000, 16 * 1024 * 1024, p_cancel_requested);
			if (!(bool)response.get("ok", false)) {
				const Dictionary error = response.get("error", Dictionary());
				if (String(error.get("code", String())) == "HTTP_CANCELLED") {
					return _error("TOOL_CANCELLED", "Asset catalog search was cancelled.");
				}
				return _error("CATALOG_SEARCH_FAILED", String(error.get("message", "Poly Haven search failed.")));
			}
			catalog = _parse_json_body(response);
			if (catalog.is_empty()) {
				return _error("CATALOG_BAD_RESPONSE", "Poly Haven did not return catalog metadata.");
			}
			MutexLock lock(catalog_cache_mutex);
			polyhaven_assets_cache[kind] = catalog.duplicate(true);
		}

		// Poly Haven exposes the authoritative catalog as one metadata object,
		// so query filtering happens locally without additional per-result calls.
		for (const Variant *asset_key = catalog.next(nullptr); asset_key; asset_key = catalog.next(asset_key)) {
			const String asset_id = String(*asset_key);
			const Dictionary asset = catalog[*asset_key];
			Dictionary item;
			item["provider"] = provider;
			item["asset_id"] = asset_id;
			item["kind"] = kind;
			item["display_name"] = asset.get("name", asset_id);
			item["description"] = asset.get("description", String());
			item["tags"] = asset.get("tags", Array());
			item["categories"] = asset.get("categories", Array());
			item["dimensions"] = asset.get("dimensions", Array());
			item["max_resolution"] = asset.get("max_resolution", Array());
			item["polycount"] = asset.get("polycount", 0);
			item["texel_density"] = asset.get("texel_density", 0);
			item["files_hash"] = asset.get("files_hash", String());
			item["source_version"] = asset.get("files_hash", String());
			item["download_count"] = asset.get("download_count", 0);
			item["authors"] = asset.get("authors", Dictionary());
			item["source_url"] = "https://polyhaven.com/a/" + asset_id.uri_encode();
			item["license"] = "CC0-1.0";
			item["attribution_url"] = "https://polyhaven.com/a/" + asset_id.uri_encode();
			item["preview_url"] = asset.get("thumbnail_url", String());
			item["details_required"] = true;
			catalog_assets.push_back(item);
		}
	}
	if (p_cancel_requested && p_cancel_requested->is_set()) {
		return _error("TOOL_CANCELLED", "Asset catalog search was cancelled.");
	}
	const Array ranked = rank_catalog_assets(catalog_assets, query);
	Array assets;
	for (int i = offset; i < MIN(offset + limit, ranked.size()); i++) {
		assets.push_back(ranked[i]);
	}
	Dictionary data;
	data["provider"] = provider;
	data["kind"] = kind;
	data["assets"] = assets;
	data["count"] = assets.size();
	data["offset"] = offset;
	data["total"] = ranked.size();
	data["next"] = "Call asset.catalog.inspect with one exact provider/kind/asset_id before acquire.";
	return _ok(data);
}

Dictionary SolersAssetService::catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String provider = String(p_args.get("provider", String())).strip_edges().to_lower();
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if ((provider != "ambientcg" && provider != "polyhaven") || (kind != "material" && kind != "hdri" && kind != "3d") || asset_id.is_empty() || asset_id.contains("/") || asset_id.contains("\\") || asset_id.contains("..")) {
		return _error("INVALID_ARGUMENT", "Catalog inspect requires provider, kind, and an exact asset_id returned by search.");
	}
	const String inspection_key = provider + "|" + kind + "|" + asset_id.to_lower();
	if (!(bool)p_args.get("refresh", false)) {
		MutexLock lock(catalog_cache_mutex);
		const Dictionary cached = catalog_inspections.get(inspection_key, Dictionary());
		if (!cached.is_empty()) {
			return _ok(cached.duplicate(true));
		}
	}
	Dictionary item;
	Array variants;
	String source_version;
	if (provider == "ambientcg") {
		const Dictionary metadata_result = _ambientcg_asset_metadata(asset_id, p_cancel_requested);
		if (!(bool)metadata_result.get("ok", false)) {
			const Dictionary error = metadata_result.get("error", Dictionary());
			return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "CATALOG_INSPECT_FAILED", String(error.get("message", "ambientCG detail request failed.")));
		}
		const Dictionary asset = metadata_result.get("asset", Dictionary());
		const String asset_type = String(asset.get("type", String())).to_lower();
		const String actual_kind = asset_type == "3d-model" ? "3d" : asset_type;
		if (actual_kind != kind) {
			return _error("CATALOG_KIND_MISMATCH", "The ambientCG asset does not match the requested kind.");
		}
		const Array downloads = asset.get("downloads", Array());
		for (int i = 0; i < downloads.size(); i++) {
			const Dictionary download = downloads[i];
			if (String(download.get("extension", String())).to_lower() != "zip") {
				continue;
			}
			Dictionary variant;
			variant["id"] = download.get("attributes", String());
			variant["size"] = download.get("size", 0);
			variant["checksum_available"] = false;
			variants.push_back(variant);
		}
		source_version = JSON::stringify(downloads).sha256_text();
		item["display_name"] = asset.get("title", asset_id);
		item["tags"] = asset.get("tags", Array());
		item["maps"] = asset.get("maps", Array());
		item["dimensions"] = asset.get("dimensions", Dictionary());
		item["source_url"] = asset.get("url", String());
		const Dictionary thumbnails = asset.get("thumbnails", Dictionary());
		item["preview_url"] = thumbnails.get("512-JPG-242424", thumbnails.get("512-PNG", String()));
	} else {
		const Dictionary metadata_result = _polyhaven_asset_metadata(asset_id, p_cancel_requested);
		if (!(bool)metadata_result.get("ok", false)) {
			const Dictionary error = metadata_result.get("error", Dictionary());
			return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "CATALOG_INSPECT_FAILED", String(error.get("message", "Poly Haven detail request failed.")));
		}
		const Dictionary asset = metadata_result.get("asset", Dictionary());
		const int asset_type = (int)asset.get("type", -1);
		const String actual_kind = asset_type == 0 ? "hdri" : (asset_type == 1 ? "material" : (asset_type == 2 ? "3d" : String()));
		if (actual_kind != kind) {
			return _error("CATALOG_KIND_MISMATCH", "The Poly Haven asset does not match the requested kind.");
		}
		variants = normalize_polyhaven_variants(metadata_result.get("files", Dictionary()), kind);
		source_version = asset.get("files_hash", String());
		item["display_name"] = asset.get("name", asset_id);
		item["description"] = asset.get("description", String());
		item["tags"] = asset.get("tags", Array());
		item["categories"] = asset.get("categories", Array());
		item["dimensions"] = asset.get("dimensions", Array());
		item["polycount"] = asset.get("polycount", 0);
		item["texel_density"] = asset.get("texel_density", 0);
		item["preview_url"] = asset.get("thumbnail_url", String());
		item["source_url"] = "https://polyhaven.com/a/" + asset_id.uri_encode();
	}
	if (variants.is_empty() || source_version.is_empty()) {
		return _error("CATALOG_DETAILS_INCOMPLETE", "The provider did not return a versioned downloadable variant for this asset.");
	}
	item["provider"] = provider;
	item["asset_id"] = asset_id;
	item["kind"] = kind;
	item["source_version"] = source_version;
	item["variants"] = variants;
	item["license"] = "CC0-1.0";
	{
		MutexLock lock(catalog_cache_mutex);
		catalog_inspections[inspection_key] = item.duplicate(true);
	}
	return _ok(item);
}

Dictionary SolersAssetService::catalog_acquire(const Dictionary &p_args, const String &p_session_id) {
	const String provider = String(p_args.get("provider", String())).strip_edges().to_lower();
	const String source_asset_id = String(p_args.get("asset_id", String())).strip_edges();
	const String variant = String(p_args.get("variant", String())).strip_edges();
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	const String pinned_source_version = String(p_args.get("source_version", String())).strip_edges();
	if ((provider != "ambientcg" && provider != "polyhaven") || (kind != "material" && kind != "hdri" && kind != "3d") || source_asset_id.is_empty() || variant.is_empty() || source_asset_id.contains("/") || source_asset_id.contains("\\") || source_asset_id.contains("..") || variant.contains("/") || variant.contains("\\") || variant.contains("..")) {
		return _error("INVALID_ARGUMENT", "Catalog acquire requires official provider, kind, asset_id, and variant identifiers.");
	}
	// The service's own cached inspection is the authority for the current
	// source_version; the caller never has to transcribe it. A supplied value
	// only acts as an explicit pin and must match the cached inspection.
	String official_variant;
	String source_version;
	{
		MutexLock lock(catalog_cache_mutex);
		const Dictionary inspected = catalog_inspections.get(provider + "|" + kind + "|" + source_asset_id.to_lower(), Dictionary());
		official_variant = match_catalog_variant_id(inspected.get("variants", Array()), variant);
		if (inspected.is_empty() || official_variant.is_empty()) {
			return _error("CATALOG_INSPECTION_REQUIRED", "Acquire requires a prior asset.catalog.inspect for this asset and an exact variants[].id from it; do not guess a format.");
		}
		source_version = inspected.get("source_version", String());
		if (!pinned_source_version.is_empty() && pinned_source_version != source_version) {
			return _error("CATALOG_INSPECTION_REQUIRED", vformat("The pinned source_version no longer matches the provider's current version (%s). Re-run asset.catalog.inspect or omit source_version to use the current one.", source_version));
		}
	}
	const PackedStringArray dirs = DirAccess::get_directories_at(_asset_root());
	for (int i = 0; i < dirs.size(); i++) {
		const Dictionary manifest = _read_json_file(_manifest_path(dirs[i]));
		const String status = String(manifest.get("status", String())).to_lower();
		if (String(manifest.get("provider", String())) == provider && String(manifest.get("source_asset_id", String())).nocasecmp_to(source_asset_id) == 0 && String(manifest.get("source_variant", String())).nocasecmp_to(official_variant) == 0 && String(manifest.get("source_version", String())) == source_version &&
				(status == "queued" || status == "running" || status == "ready" || status == "draft")) {
			return _ok(manifest);
		}
	}
	const String asset_id = vformat("%s-%s_%s", itos((int64_t)Time::get_singleton()->get_unix_time_from_system()), String::num_uint64(OS::get_singleton()->get_ticks_usec()), (provider + source_asset_id + official_variant + source_version).md5_text().substr(0, 10));
	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = kind;
	manifest["name"] = p_args.get("name", source_asset_id);
	manifest["provider"] = provider;
	manifest["base_url"] = provider == "ambientcg" ? "https://ambientcg.com" : "https://polyhaven.com";
	manifest["source_provider"] = provider;
	manifest["source_asset_id"] = source_asset_id;
	manifest["source_variant"] = official_variant;
	manifest["source_version"] = source_version;
	manifest["license"] = "CC0-1.0";
	manifest["status"] = "queued";
	manifest["stage"] = "queued";
	manifest["created_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	manifest["files"] = Array();
	manifest["entrypoints"] = Array();
	if (!p_session_id.is_empty()) {
		manifest["session_id"] = p_session_id;
	}
	return _queue_manifest(manifest, Dictionary());
}

Dictionary SolersAssetService::wait_jobs(const Dictionary &p_args, const String &p_session_id) const {
	const Variant ids_value = p_args.get("ids", Variant());
	if (ids_value.get_type() != Variant::ARRAY || Array(ids_value).is_empty()) {
		return _error("INVALID_ARGUMENT", "job.wait requires one or more job ids returned by a background asset tool.");
	}
	const Array ids = ids_value;
	HashSet<String> seen;
	Array pending_ids;
	Array terminal;
	for (int i = 0; i < ids.size(); i++) {
		const String id = String(ids[i]).strip_edges();
		if (id.is_empty() || seen.has(id)) {
			return _error("INVALID_ARGUMENT", "job.wait ids must be non-empty and unique.");
		}
		seen.insert(id);
		const Dictionary manifest = _manifest_for_asset(id);
		if (manifest.is_empty()) {
			return _error("JOB_NOT_FOUND", vformat("Background job was not found: %s", id));
		}
		const String owner = String(manifest.get("session_id", String()));
		if (!owner.is_empty() && owner != p_session_id) {
			return _error("JOB_SESSION_MISMATCH", vformat("Background job belongs to another Agent session: %s", id), false);
		}
		const String status = String(manifest.get("status", String())).to_lower();
		if (status == "ready" || status == "draft" || status == "failed" || status == "cancelled" || status == "interrupted") {
			Dictionary state;
			state["id"] = id;
			state["status"] = status;
			const char *fields[] = { "kind", "name", "stage", "entrypoints", "error", "provider", "source_provider", "source_asset_id", "source_variant", "source_version", "generation_mode", "provider_request", "input_attachment_ids" };
			for (const char *field : fields) {
				if (manifest.has(field)) {
					state[field] = manifest[field];
				}
			}
			terminal.push_back(state);
		} else {
			pending_ids.push_back(id);
		}
	}
	Dictionary data;
	data["pending_ids"] = pending_ids;
	data["terminal"] = terminal;
	data["waiting"] = !pending_ids.is_empty();
	return _ok(data);
}

Dictionary SolersAssetService::_queue_manifest(const Dictionary &p_manifest, const Dictionary &p_provider_config) {
	const String asset_id = p_manifest.get("id", String());
	if (asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset id is required.");
	}
	String error;
	if (!_write_json_atomic(_manifest_path(asset_id), p_manifest, error)) {
		return _error("WRITE_FAILED", error);
	}

	Task *task = memnew(Task);
	task->asset_id = asset_id;
	task->api_key = p_provider_config.get("api_key", String());
	task->service = this;
	_set_task_state(task, p_manifest);
	{
		MutexLock lock(tasks_mutex);
		tasks[asset_id] = task;
	}
	task->thread.start(&SolersAssetService::_task_func, task);

	return _ok(p_manifest.duplicate(true));
}

Dictionary SolersAssetService::capabilities(const Dictionary &p_args) const {
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset_id is required.");
	}
	const Dictionary manifest = _manifest_for_asset(asset_id);
	if (manifest.is_empty()) {
		return _error("NOT_FOUND", "Asset was not found.");
	}
	Array operations;
	Array available_operations;
	const Array defs = _solers_operation_defs(String(manifest.get("provider", String())).to_lower());
	for (int i = 0; i < defs.size(); i++) {
		Dictionary operation = Dictionary(defs[i]).duplicate(true);
		String reason;
		const bool available = _solers_manifest_matches_operation(manifest, operation, reason);
		operation["available"] = available;
		operation["ui_supported"] = available && _solers_schema_ui_supported(operation.get("options_schema", Dictionary()));
		if (!available) {
			operation["reason"] = reason;
		} else {
			available_operations.push_back(operation);
		}
		operations.push_back(operation);
	}
	Dictionary data;
	data["asset_id"] = asset_id;
	data["operations"] = operations;
	data["available_operations"] = available_operations;
	const Dictionary animations = manifest.get("animations", Dictionary());
	const Dictionary basic_animations = animations.get("basic", Dictionary());
	if (!basic_animations.is_empty()) {
		data["basic_animations"] = basic_animations;
	}
	if (String(manifest.get("provider", String())).to_lower() == "meshy" && String(_solers_asset_traits(manifest).get("rig", String())) == "humanoid") {
		data["animation_actions"] = _meshy_animation_actions();
	}
	return _ok(data);
}

Dictionary SolersAssetService::_run_operation(const Dictionary &p_args, const String &p_session_id) {
	const String source_asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (source_asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset_id is required.");
	}
	const String operation_id = String(p_args.get("operation_id", String())).strip_edges();
	if (operation_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "operation_id is required.");
	}
	const Dictionary source = _manifest_for_asset(source_asset_id);
	if (source.is_empty()) {
		return _error("NOT_FOUND", "Source asset was not found.");
	}
	const String provider = String(source.get("provider", String())).to_lower();
	Dictionary operation;
	const Array defs = _solers_operation_defs(provider);
	for (int i = 0; i < defs.size(); i++) {
		const Dictionary candidate = defs[i];
		if (String(candidate.get("operation_id", String())) == operation_id) {
			operation = candidate;
			break;
		}
	}
	if (operation.is_empty()) {
		return _error("OPERATION_NOT_FOUND", "Asset operation is not supported by this provider.");
	}
	String reason;
	if (!_solers_manifest_matches_operation(source, operation, reason)) {
		return _error("OPERATION_NOT_AVAILABLE", reason);
	}
	Dictionary options = p_args.get("options", Dictionary());
	Dictionary raw_provider_options = p_args.get("raw_provider_options", Dictionary());
	if (!raw_provider_options.is_empty() && !(bool)p_args.get("raw_confirmed", false)) {
		return _error("RAW_OPTIONS_REQUIRE_CONFIRMATION", "raw_provider_options requires raw_confirmed=true.");
	}
	const Dictionary schema = operation.get("options_schema", Dictionary());
	const Dictionary properties = schema.get("properties", Dictionary());
	const Array required = schema.get("required", Array());
	for (int i = 0; i < required.size(); i++) {
		const String name = String(required[i]);
		if (!options.has(name) && !raw_provider_options.has(name)) {
			return _error("INVALID_ARGUMENT", vformat("Missing required option: %s", name));
		}
		const Variant value = options.has(name) ? options[name] : raw_provider_options[name];
		const String type = String(Dictionary(properties.get(name, Dictionary())).get("type", String()));
		if (type == "string" && String(value).strip_edges().is_empty()) {
			return _error("INVALID_ARGUMENT", vformat("Missing required option: %s", name));
		}
	}
	String normalize_error;
	if (!_solers_normalize_integer_options(options, properties, normalize_error) || !_solers_normalize_integer_options(raw_provider_options, properties, normalize_error)) {
		return _error("INVALID_ARGUMENT", normalize_error);
	}
	if (operation_id == "rig_humanoid" && !(bool)options.get("humanoid_confirmed", raw_provider_options.get("humanoid_confirmed", false))) {
		return _error("HUMANOID_CONFIRMATION_REQUIRED", "Rigging requires confirming this is a humanoid character.");
	}
	if (operation_id == "animate_humanoid") {
		const int64_t action_id = (int64_t)options.get("action_id", raw_provider_options.get("action_id", -1));
		if (!_meshy_animation_action_exists(action_id)) {
			return _error("INVALID_ACTION_ID", "action_id must come from Meshy's official animation_actions catalog returned by asset.capabilities.");
		}
	}

	const Dictionary requires = operation.get("requires", Dictionary());
	const String source_task_id = _solers_first_manifest_field(source, requires.get("task_id_fields", Array()));
	const Dictionary provider_config = _provider_config("3d", provider);
	Dictionary provider_options;
	_merge_options(provider_options, options);
	_merge_options(provider_options, raw_provider_options);
	provider_options.erase("humanoid_confirmed");
	if (operation_id == "refine") {
		provider_options["mode"] = "refine";
		if (!provider_options.has("preview_task_id")) {
			provider_options["preview_task_id"] = source_task_id;
		}
		if (!provider_options.has("enable_pbr")) {
			provider_options["enable_pbr"] = true;
		}
	} else if (operation_id == "remesh") {
		if (!provider_options.has("input_task_id")) {
			provider_options["input_task_id"] = source_task_id;
		}
		if (!provider_options.has("topology")) {
			provider_options["topology"] = "triangle";
		}
		if (!provider_options.has("target_polycount")) {
			provider_options["target_polycount"] = 30000;
		}
	} else if (operation_id == "retexture") {
		if (!provider_options.has("input_task_id")) {
			provider_options["input_task_id"] = source_task_id;
		}
		if (!provider_options.has("enable_original_uv")) {
			provider_options["enable_original_uv"] = true;
		}
		if (!provider_options.has("enable_pbr")) {
			provider_options["enable_pbr"] = true;
		}
		if (!provider_options.has("remove_lighting")) {
			provider_options["remove_lighting"] = true;
		}
	} else if (operation_id == "rig_humanoid") {
		if (!provider_options.has("input_task_id")) {
			provider_options["input_task_id"] = source_task_id;
		}
	} else if (operation_id == "animate_humanoid") {
		if (!provider_options.has("rig_task_id")) {
			provider_options["rig_task_id"] = source_task_id;
		}
	}
	if ((operation_id == "refine" || operation_id == "remesh" || operation_id == "retexture") && !provider_options.has("target_formats")) {
		Array formats;
		formats.push_back("glb");
		provider_options["target_formats"] = formats;
	}

	const String label = String(operation.get("label", operation_id));
	const String name = String(source.get("name", source_asset_id)) + " " + label;
	const String prompt = String(source.get("prompt", String()));
	const String asset_id = vformat("%s_%s", itos((int64_t)Time::get_singleton()->get_unix_time_from_system()), ("3d" + source_asset_id + operation_id + JSON::stringify(provider_options)).md5_text().substr(0, 10));
	Dictionary traits = _solers_asset_traits(source);
	if (operation_id == "rig_humanoid") {
		traits["model_state"] = "rigged_model";
		traits["rig"] = "humanoid";
	} else if (operation_id == "animate_humanoid") {
		traits["model_state"] = "animated_model";
		traits["rig"] = "humanoid";
		traits["animation"] = "present";
	} else {
		traits["model_state"] = "static_model";
	}
	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = "3d";
	manifest["name"] = name;
	manifest["prompt"] = prompt;
	manifest["profile"] = source.get("profile", "game_default");
	manifest["provider"] = provider;
	manifest["base_url"] = provider_config.get("base_url", String());
	manifest["provider_options"] = provider_options;
	const String session_id = p_session_id.is_empty() ? String(source.get("session_id", String())) : p_session_id;
	if (!session_id.is_empty()) {
		manifest["session_id"] = session_id;
	}
	manifest["parent_asset_id"] = source_asset_id;
	manifest["operation"] = operation_id;
	manifest["operation_label"] = label;
	manifest["provider_operation_id"] = operation.get("provider_operation_id", String());
	manifest["docs"] = operation.get("docs", String());
	manifest["source_task_id"] = source_task_id;
	manifest["traits"] = traits;
	manifest["status"] = "queued";
	manifest["stage"] = "queued";
	manifest["created_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	manifest["files"] = Array();
	return _queue_manifest(manifest, provider_config);
}

Dictionary SolersAssetService::run_operation(const Dictionary &p_args) {
	return _run_operation(p_args, String());
}

Dictionary SolersAssetService::run_operation_for_session(const Dictionary &p_args, const String &p_session_id) {
	return _run_operation(p_args, p_session_id);
}

Array SolersAssetService::take_terminal_events(const String &p_session_id) {
	Array out;
	if (p_session_id.is_empty()) {
		return out;
	}
	MutexLock lock(terminal_events_mutex);
	for (int i = terminal_events.size() - 1; i >= 0; i--) {
		const Dictionary manifest = terminal_events[i];
		if (String(manifest.get("session_id", String())) == p_session_id) {
			out.push_back(manifest);
			terminal_events.remove_at(i);
		}
	}
	return out;
}

Array SolersAssetService::pending_terminal_events(const String &p_session_id) const {
	Array out;
	if (p_session_id.is_empty()) {
		return out;
	}
	const PackedStringArray dirs = DirAccess::get_directories_at(_asset_root());
	for (int i = 0; i < dirs.size(); i++) {
		const Dictionary manifest = _read_json_file(_manifest_path(dirs[i]));
		const String status = String(manifest.get("status", String())).to_lower();
		if (String(manifest.get("session_id", String())) == p_session_id && String(manifest.get("delivery_status", String())) != "delivered" &&
				(status == "ready" || status == "draft" || status == "failed" || status == "cancelled" || status == "interrupted")) {
			out.push_back(manifest);
		}
	}
	return out;
}

bool SolersAssetService::has_active_tasks(const String &p_session_id) const {
	if (p_session_id.is_empty()) {
		return false;
	}
	const PackedStringArray dirs = DirAccess::get_directories_at(_asset_root());
	for (int i = 0; i < dirs.size(); i++) {
		const Dictionary manifest = _read_json_file(_manifest_path(dirs[i]));
		if (String(manifest.get("session_id", String())) != p_session_id) {
			continue;
		}
		const String status = String(manifest.get("status", String())).to_lower();
		if (status == "queued" || status == "running") {
			return true;
		}
	}
	return false;
}

void SolersAssetService::mark_terminal_delivered(const String &p_asset_id, const String &p_session_id) {
	if (p_asset_id.is_empty() || p_session_id.is_empty()) {
		return;
	}
	Dictionary manifest = _read_json_file(_manifest_path(p_asset_id));
	if (String(manifest.get("session_id", String())) != p_session_id) {
		return;
	}
	manifest["delivery_status"] = "delivered";
	String error;
	_write_json_atomic(_manifest_path(p_asset_id), manifest, error);
}

Dictionary SolersAssetService::status(const Dictionary &p_args) const {
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset_id is required.");
	}
	Dictionary manifest = _manifest_for_asset(asset_id);
	if (manifest.is_empty()) {
		return _error("NOT_FOUND", "Asset task was not found.");
	}
	_cleanup_finished_task(asset_id);
	return _ok(manifest);
}

Dictionary SolersAssetService::list_local(const Dictionary &p_args) const {
	const String kind_filter = String(p_args.get("kind", String())).strip_edges().to_lower();
	const String query = String(p_args.get("query", String())).strip_edges().to_lower();
	const int limit = MAX(1, MIN((int)p_args.get("limit", 128), 512));

	Array assets;
	const PackedStringArray dirs = DirAccess::get_directories_at(_asset_root());
	for (int i = 0; i < dirs.size() && assets.size() < limit; i++) {
		Dictionary manifest = _read_json_file(_manifest_path(dirs[i]));
		if (manifest.is_empty()) {
			continue;
		}
		const String kind = String(manifest.get("kind", String())).to_lower();
		if (!kind_filter.is_empty() && kind != kind_filter) {
			continue;
		}
		const String haystack = (String(manifest.get("name", String())) + " " + String(manifest.get("prompt", String()))).to_lower();
		if (!query.is_empty() && !haystack.contains(query)) {
			continue;
		}
		manifest.erase("api_key");
		assets.push_back(manifest);
	}
	Dictionary data;
	data["assets"] = assets;
	data["count"] = assets.size();
	return _ok(data);
}

Dictionary SolersAssetService::delete_asset(const Dictionary &p_args) {
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (asset_id.is_empty() || asset_id.contains("/") || asset_id.contains("\\") || asset_id.contains("..")) {
		return _error("INVALID_ARGUMENT", "asset_id is invalid.");
	}
	Task *const *task = tasks.getptr(asset_id);
	if (task && *task) {
		const String task_status = String(_task_state(*task).get("status", String())).to_lower();
		if (task_status != "ready" && task_status != "draft" && task_status != "failed" && task_status != "cancelled" && task_status != "interrupted") {
			return _error("ASSET_BUSY", "Asset is still being generated.");
		}
		_cleanup_finished_task(asset_id);
	}
	const Dictionary manifest = _read_json_file(_manifest_path(asset_id));
	if (manifest.is_empty()) {
		return _error("NOT_FOUND", "Asset was not found.");
	}
	const String status = String(manifest.get("status", String())).to_lower();
	if (status != "ready" && status != "draft" && status != "failed" && status != "cancelled" && status != "interrupted") {
		return _error("ASSET_BUSY", "Asset is still being generated.");
	}
	const PackedStringArray dirs = DirAccess::get_directories_at(_asset_root());
	for (int i = 0; i < dirs.size(); i++) {
		if (dirs[i] == asset_id) {
			continue;
		}
		const Dictionary child = _read_json_file(_manifest_path(dirs[i]));
		if (String(child.get("parent_asset_id", String())) == asset_id) {
			return _error("ASSET_HAS_CHILDREN", "Delete derived assets first.");
		}
	}

	const String asset_dir = _asset_dir(asset_id);
	const String root_abs = ProjectSettings::get_singleton()->globalize_path(_asset_root()).replace_char('\\', '/').simplify_path();
	const String asset_abs = ProjectSettings::get_singleton()->globalize_path(asset_dir).replace_char('\\', '/').simplify_path();
	if (!asset_abs.begins_with(root_abs + "/")) {
		return _error("INVALID_TARGET", "Asset directory is outside Solers Library.");
	}
	String remove_error;
	if (!_remove_dir_recursive(asset_dir, remove_error)) {
		return _error("DELETE_FAILED", remove_error);
	}
	Dictionary data;
	data["asset_id"] = asset_id;
	return _ok(data);
}

Dictionary SolersAssetService::import_to_project(const Dictionary &p_args) {
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset_id is required.");
	}
	const Dictionary manifest = _manifest_for_asset(asset_id);
	if (manifest.is_empty()) {
		return _error("NOT_FOUND", "Asset was not found.");
	}
	const String status = String(manifest.get("status", String()));
	if (status != "ready" && status != "draft") {
		return _error("ASSET_NOT_READY", "Asset is not ready to import.");
	}
	const Array source_files = manifest.get("files", Array());
	Array declared_import_files = manifest.get("import_files", Array());
	Array declared_entrypoints = manifest.get("entrypoints", Array());
	Array requested_map_types;
	const Variant requested_maps = p_args.get("map_types", Variant());
	if (requested_maps.get_type() != Variant::NIL && requested_maps.get_type() != Variant::ARRAY) {
		return _error("INVALID_ARGUMENT", "map_types must be an array of exact map role names returned by the asset manifest.");
	}
	if (requested_maps.get_type() == Variant::ARRAY) {
		requested_map_types = requested_maps;
	}
	const Dictionary map_files = manifest.get("map_files", Dictionary());
	if (String(manifest.get("kind", String())) == "material" && !map_files.is_empty()) {
		Array available_map_types = map_files.keys();
		available_map_types.sort();
		if (requested_map_types.is_empty()) {
			Dictionary error = _error("MAP_SELECTION_REQUIRED", "Texture-set imports require map_types so Godot imports only the maps the material will use.");
			Dictionary data;
			data["available_map_types"] = available_map_types;
			error["data"] = data;
			return error;
		}
		HashSet<String> selected_map_types;
		declared_import_files.clear();
		declared_entrypoints.clear();
		for (int i = 0; i < requested_map_types.size(); i++) {
			const String map_type = String(requested_map_types[i]).strip_edges();
			if (map_type.is_empty() || selected_map_types.has(map_type) || !map_files.has(map_type)) {
				Dictionary error = _error("INVALID_MAP_SELECTION", vformat("map_types contains an empty, duplicate, or unavailable role: %s", map_type));
				Dictionary data;
				data["available_map_types"] = available_map_types;
				error["data"] = data;
				return error;
			}
			selected_map_types.insert(map_type);
			declared_import_files.push_back(map_files[map_type]);
			declared_entrypoints.push_back(map_files[map_type]);
		}
	}
	const Dictionary selection = _project_import_selection(source_files, declared_import_files, declared_entrypoints);
	if (!(bool)selection.get("valid", false)) {
		return _error("INVALID_ASSET_MANIFEST", selection.get("error", "Asset import selection failed."), false);
	}
	const Array files = selection.get("files", Array());
	const Array source_entrypoints = selection.get("entrypoints", Array());
	if (files.is_empty()) {
		return _error("ASSET_HAS_NO_FILES", "Asset has no source files.");
	}
	const String asset_kind = String(manifest.get("kind", String())).to_lower();
	// Import policy is declared by the caller, never inferred from asset traits:
	// "runtime" imports geometry as-is; "baked_static" additionally configures
	// Godot's native Static Lightmaps mode (UV2 unwrap) on model entrypoints.
	String import_profile = "runtime";
	if (p_args.has("import_profile")) {
		import_profile = String(p_args.get("import_profile", String())).strip_edges().to_lower();
		if (import_profile != "runtime" && import_profile != "baked_static") {
			return _error("INVALID_ARGUMENT", "import_profile must be \"runtime\" or \"baked_static\".");
		}
		if (import_profile == "baked_static" && asset_kind != "3d") {
			return _error("INVALID_ARGUMENT", "import_profile \"baked_static\" only applies to 3d assets.");
		}
	}
	int64_t max_triangles = -1;
	String triangle_budget_source = "declared";
	if (p_args.has("max_triangles")) {
		const Variant declared_budget = p_args.get("max_triangles", Variant());
		if (!declared_budget.is_num() || (double)declared_budget != Math::floor((double)declared_budget) || (double)declared_budget < 0.0) {
			return _error("INVALID_ARGUMENT", "max_triangles must be a non-negative integer; 0 disables the topology budget.");
		}
		max_triangles = (int64_t)declared_budget;
		// The project setting is the authoritative ceiling for declared
		// budgets. A larger density is a deliberate project decision that has
		// to be made where it is visible and audited — in project settings —
		// not by inflating a tool argument mid-task.
		const int64_t budget_ceiling = ProjectSettings::get_singleton()->get_setting("solers/ai_assets/import/max_source_triangles", 2000000);
		if (asset_kind == "3d" && budget_ceiling > 0 && (max_triangles == 0 || max_triangles > budget_ceiling)) {
			return _error("BUDGET_CEILING_EXCEEDED", vformat("Declared max_triangles %d exceeds the project ceiling of %d (solers/ai_assets/import/max_source_triangles). Import a variant that fits, or raise that project setting through project.edit settings first if the project can truly afford the density.", max_triangles, budget_ceiling));
		}
	}
	const Dictionary generation_options = manifest.get("provider_options", Dictionary());
	int64_t remesh_triangle_target = 0;
	if (String(manifest.get("provider", String())).to_lower() == "meshy" && generation_options.has("target_polycount") && (bool)generation_options.get("should_remesh", false)) {
		const int64_t target_polycount = generation_options.get("target_polycount", 0);
		remesh_triangle_target = String(generation_options.get("topology", "triangle")).to_lower() == "quad" ? target_polycount * 2 : target_polycount;
	}
	if (max_triangles < 0 && asset_kind == "3d") {
		if (remesh_triangle_target > 0) {
			max_triangles = remesh_triangle_target;
			triangle_budget_source = "asset_manifest";
		} else {
			max_triangles = ProjectSettings::get_singleton()->get_setting("solers/ai_assets/import/max_source_triangles", 2000000);
			triangle_budget_source = "project_default";
		}
	}
	String target_dir = String(p_args.get("target_dir", String()));
	if (target_dir.is_empty()) {
		target_dir = "res://solers_assets/" + String(manifest.get("kind", "asset")) + "/" + _safe_slug(String(manifest.get("name", asset_id)));
	}
	target_dir = target_dir.replace_char('\\', '/').simplify_path();
	if (!target_dir.begins_with("res://") || target_dir.contains("..")) {
		return _error("INVALID_TARGET", "target_dir must stay inside res://.");
	}
	if (EditorFileSystem *filesystem = _solers_editor_filesystem()) {
		Vector<String> selected_files;
		selected_files.resize(files.size());
		for (int i = 0; i < files.size(); i++) {
			selected_files.write[i] = files[i];
		}
		if (filesystem->requires_import_format_support(selected_files)) {
			return _error("IMPORT_FORMAT_CONFIGURATION_REQUIRED", "The selected asset requires interactive importer configuration. Configure that format in the editor before importing it through an Agent tool.");
		}
	}
	// Topology budgets are enforced against glTF accessor metadata before any
	// file is copied, so a pathological model is rejected in milliseconds
	// instead of after minutes of native import work. Sources that are not
	// glTF fall through to the post-import mesh statistics contract.
	if (asset_kind == "3d" && max_triangles > 0) {
		int64_t source_triangle_count = 0;
		Array source_geometry;
		for (int i = 0; i < files.size(); i++) {
			const String src = String(files[i]);
			const String extension = src.get_extension().to_lower();
			if (extension != "gltf" && extension != "glb") {
				continue;
			}
			Dictionary document;
			String parse_error;
			if (!_solers_gltf_document_json(src, document, parse_error)) {
				continue;
			}
			const int64_t file_triangles = _solers_gltf_source_triangle_count(document);
			source_triangle_count += file_triangles;
			Dictionary entry;
			entry["path"] = src;
			entry["triangle_count"] = file_triangles;
			source_geometry.push_back(entry);
		}
		if (source_triangle_count > max_triangles) {
			// Remediation is assembled from this asset's actual capabilities so
			// the model is never pointed at an operation its provider cannot run.
			bool remesh_available = false;
			const Array operation_defs = _solers_operation_defs(String(manifest.get("provider", String())).to_lower());
			for (int i = 0; i < operation_defs.size(); i++) {
				const Dictionary operation = operation_defs[i];
				if (String(operation.get("operation_id", String())) != "remesh") {
					continue;
				}
				String availability_reason;
				remesh_available = _solers_manifest_matches_operation(manifest, operation, availability_reason);
				break;
			}
			const String remediation = remesh_available
					? String("Remesh it with asset.run_operation (operation_id=\"remesh\") or acquire a lower-poly variant.")
					: String("This asset's provider does not offer a remesh operation; acquire a lower-poly variant instead.");
			Dictionary error = _error("TOPOLOGY_BUDGET_EXCEEDED", vformat("The asset's source geometry contains %d triangles, exceeding the import budget of %d (%s). %s", source_triangle_count, max_triangles, triangle_budget_source, remediation));
			Dictionary data;
			data["triangle_count"] = source_triangle_count;
			data["max_triangles"] = max_triangles;
			data["triangle_budget_source"] = triangle_budget_source;
			data["remesh_available"] = remesh_available;
			data["files"] = source_geometry;
			error["data"] = data;
			return error;
		}
	}
	Array imported;
	Array entrypoints;
	Array source_hashes;
	Array copy_required;
	bool destination_matches = true;
	const String source_root = _source_dir(asset_id).replace_char('\\', '/').simplify_path();
	HashSet<String> source_entrypoint_set;
	for (int i = 0; i < source_entrypoints.size(); i++) {
		source_entrypoint_set.insert(String(source_entrypoints[i]).replace_char('\\', '/').simplify_path());
	}
	for (int i = 0; i < files.size(); i++) {
		const String src = String(files[i]).replace_char('\\', '/').simplify_path();
		if (!src.begins_with(source_root + "/")) {
			return _error("INVALID_ASSET_SOURCE", "Asset manifest contains a file outside its source directory.", false);
		}
		// path_to_file() yields "./name" for files at the source root; without
		// simplify_path() the copied destination keeps a literal "/./" segment,
		// which EditorFileSystem's tree lookup can never match.
		const String dst = target_dir.path_join(source_root.path_to_file(src)).simplify_path();
		imported.push_back(dst);
		if (source_entrypoint_set.has(src)) {
			entrypoints.push_back(dst);
		}
		const String source_hash = FileAccess::get_md5(src);
		source_hashes.push_back(source_hash);
		const bool matches = FileAccess::exists(dst) && source_hash == FileAccess::get_md5(dst);
		copy_required.push_back(!matches);
		destination_matches = destination_matches && matches;
	}
	const String transaction_key = _project_import_transaction_key(asset_id, target_dir, files, source_hashes);
	Dictionary result_data;
	result_data["asset_id"] = asset_id;
	result_data["target_dir"] = target_dir;
	result_data["files"] = imported;
	result_data["entrypoints"] = entrypoints;
	result_data["source_file_count"] = source_files.size();
	result_data["skipped_source_file_count"] = source_files.size() - imported.size();
	if (!requested_map_types.is_empty()) {
		result_data["map_types"] = requested_map_types;
	}
	result_data["transaction_key"] = transaction_key;
	int copied_file_count = 0;
	for (int i = 0; i < copy_required.size(); i++) {
		copied_file_count += (bool)copy_required[i];
	}
	result_data["copied_file_count"] = copied_file_count;
	result_data["authored_state_changed"] = copied_file_count > 0;
	result_data["import_profile"] = import_profile;
	if (remesh_triangle_target > 0) {
		result_data["requested_polygon_target"] = generation_options.get("target_polycount", 0);
		result_data["requested_topology"] = String(generation_options.get("topology", "triangle")).to_lower();
	}
	if (asset_kind == "3d" && max_triangles >= 0) {
		result_data["max_import_triangle_count"] = max_triangles;
		result_data["triangle_budget_source"] = triangle_budget_source;
	}
	Array static_lightmap_paths;
	Array static_lightmap_import_requests;
	bool static_lightmap_reimport_required = false;
	if (asset_kind == "3d" && import_profile == "baked_static") {
		EditorFileSystem *filesystem = _solers_editor_filesystem();
		for (int i = 0; filesystem && i < entrypoints.size(); i++) {
			const String path = entrypoints[i];
			const Dictionary config = _static_lightmap_import_config(path);
			if (!(bool)config.get("supported", false)) {
				continue;
			}
			const bool configured = config.get("configured", false);
			if (!configured) {
				Dictionary request;
				request["path"] = path;
				request["importer"] = config.get("importer", String());
				request["options"] = config.get("options", Dictionary());
				static_lightmap_import_requests.push_back(request);
			}
			static_lightmap_paths.push_back(path);
			static_lightmap_reimport_required = static_lightmap_reimport_required || !configured;
		}
	}
	if (!static_lightmap_paths.is_empty()) {
		result_data["static_lightmap_import_paths"] = static_lightmap_paths;
		result_data["static_lightmap_import_mode"] = "godot_native";
	}
	if (static_lightmap_reimport_required) {
		result_data["authored_state_changed"] = true;
	}

	{
		MutexLock lock(project_imports_mutex);
		for (const KeyValue<String, Dictionary> &E : project_imports) {
			if (String(E.value.get("transaction_key", String())) != transaction_key || String(E.value.get("status", String())) != "pending") {
				continue;
			}
			Dictionary pending;
			pending["status"] = "pending";
			pending["stage"] = E.value.get("stage", "queued");
			pending["poll_args"] = E.value.get("poll_args", Dictionary());
			pending["transaction_key"] = transaction_key;
			pending["reused"] = true;
			return _ok(pending);
		}
	}
	if (destination_matches && !static_lightmap_reimport_required && Thread::is_main_thread()) {
		const Dictionary inspection = _project_import_inspection(imported, entrypoints, true);
		if ((bool)inspection.get("ready", false)) {
			const Dictionary contract_error = _project_import_contract_error(result_data, inspection);
			if (!contract_error.is_empty()) {
				Dictionary result = _error(contract_error.get("code", "IMPORT_CONTRACT_FAILED"), contract_error.get("message", "Imported asset contract failed."));
				result["data"] = contract_error.get("diagnostics", Dictionary());
				return result;
			}
			return _ok(_project_import_completed_data(result_data, inspection));
		}
	}

	for (int i = 0; i < files.size(); i++) {
		if (!(bool)copy_required[i]) {
			continue;
		}
		const String src = String(files[i]);
		const String dst = imported[i];
		String err;
		if (!_copy_file(src, dst, err)) {
			return _error("IMPORT_FAILED", err);
		}
	}
	if (EditorFileSystem *filesystem = _solers_editor_filesystem()) {
		for (int i = 0; i < static_lightmap_import_requests.size(); i++) {
			const Dictionary request = static_lightmap_import_requests[i];
			HashMap<StringName, Variant> options;
			const Dictionary desired = request.get("options", Dictionary());
			for (const Variant *key = desired.next(nullptr); key; key = desired.next(key)) {
				options[StringName(String(*key))] = desired[*key];
			}
			filesystem->queue_import_options(request.get("path", String()), request.get("importer", String()), options);
		}
	}

	// Files Godot itself must (re)import: freshly copied sources plus any
	// entrypoint whose queued import options change its sidecar. Every path
	// listed here is settled exclusively by the editor's resources_reimported
	// signal — completion is never guessed from pipeline idleness.
	ResourceFormatImporter *format_importer = ResourceFormatImporter::get_singleton();
	HashSet<String> forced_import;
	for (int i = 0; i < static_lightmap_import_requests.size(); i++) {
		forced_import.insert(String(Dictionary(static_lightmap_import_requests[i]).get("path", String())));
	}
	Array pending_import_files;
	for (int i = 0; i < imported.size(); i++) {
		const String dst = imported[i];
		const Ref<ResourceImporter> dst_importer = format_importer ? format_importer->get_importer_by_file(dst) : Ref<ResourceImporter>();
		if (dst_importer.is_null()) {
			continue;
		}
		if (!(bool)copy_required[i] && !forced_import.has(dst) && ResourceLoader::is_import_valid(dst)) {
			continue;
		}
		pending_import_files.push_back(dst);
	}

	const String new_import_id = (asset_id + target_dir + String::num_uint64(OS::get_singleton()->get_ticks_usec())).md5_text();
	Dictionary poll_args;
	poll_args["asset_id"] = asset_id;
	poll_args["target_dir"] = target_dir;
	poll_args["_import_id"] = new_import_id;
	Dictionary state;
	state["status"] = "pending";
	state["stage"] = "queued";
	state["files"] = imported;
	state["entrypoints"] = entrypoints;
	state["pending_files"] = pending_import_files;
	state["import_file_count"] = pending_import_files.size();
	state["result"] = result_data;
	state["poll_args"] = poll_args;
	state["transaction_key"] = transaction_key;
	state["last_progress_msec"] = OS::get_singleton()->get_ticks_msec();
	const int64_t stall_timeout_seconds = ProjectSettings::get_singleton()->get_setting("solers/ai_assets/import/stall_timeout_seconds", 300);
	state["stall_timeout_msec"] = stall_timeout_seconds > 0 ? stall_timeout_seconds * 1000 : 0;
	{
		MutexLock lock(project_imports_mutex);
		project_imports[new_import_id] = state;
	}

	Dictionary pending;
	pending["status"] = "pending";
	pending["stage"] = state["stage"];
	pending["poll_args"] = poll_args;
	return _ok(pending);
}

void SolersAssetService::_ensure_project_import_signals() {
	if (project_import_signals_connected) {
		return;
	}
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	if (!filesystem) {
		return;
	}
	filesystem->connect(SNAME("resources_reimported"), callable_mp(this, &SolersAssetService::_on_project_resources_reimported));
	project_import_signals_connected = true;
}

void SolersAssetService::_on_project_resources_reimported(const PackedStringArray &p_resources) {
	// The editor emits this signal once per settled file (incremental steps,
	// modal imports, and scans alike), so it is the single authoritative
	// completion record for every pending import file.
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	HashSet<String> resources;
	for (const String &path : p_resources) {
		resources.insert(path);
	}
	MutexLock lock(project_imports_mutex);
	for (KeyValue<String, Dictionary> &E : project_imports) {
		Dictionary state = E.value;
		if (String(state.get("status", String())) != "pending") {
			continue;
		}
		const Array pending_files = state.get("pending_files", Array());
		if (pending_files.is_empty()) {
			continue;
		}
		Array remaining;
		for (int i = 0; i < pending_files.size(); i++) {
			if (!resources.has(String(pending_files[i]))) {
				remaining.push_back(pending_files[i]);
			}
		}
		if (remaining.size() != pending_files.size()) {
			state["pending_files"] = remaining;
			state["last_progress_msec"] = now;
			E.value = state;
		}
	}
}

Dictionary SolersAssetService::poll_project_import(const Dictionary &p_args) {
	const String import_id = String(p_args.get("_import_id", String())).strip_edges();
	if (import_id.is_empty()) {
		return _error("INVALID_IMPORT_CONTINUATION", "_import_id is required to continue a project import.", false);
	}
	Dictionary state;
	{
		MutexLock lock(project_imports_mutex);
		const Dictionary *stored = project_imports.getptr(import_id);
		if (stored) {
			state = *stored;
		}
	}
	if (state.is_empty()) {
		return _error("IMPORT_NOT_FOUND", "The pending project import no longer exists.");
	}
	const String import_status = state.get("status", String());
	if (import_status == "failed") {
		const Dictionary error = state.get("error", Dictionary());
		const Dictionary diagnostics = state.get("diagnostics", Dictionary());
		MutexLock lock(project_imports_mutex);
		project_imports.erase(import_id);
		Dictionary result = _error(error.get("code", "IMPORT_FAILED"), error.get("message", "Project import failed."));
		if (!diagnostics.is_empty()) {
			result["data"] = diagnostics;
		}
		return result;
	}
	if (import_status == "complete") {
		Dictionary data = state.get("result", Dictionary());
		MutexLock lock(project_imports_mutex);
		project_imports.erase(import_id);
		return _ok(data);
	}
	Dictionary data;
	data["status"] = "pending";
	data["stage"] = state.get("stage", "queued");
	data["file_count"] = Array(state.get("files", Array())).size();
	const int import_file_count = state.get("import_file_count", 0);
	data["import_file_count"] = import_file_count;
	data["imported_count"] = import_file_count - Array(state.get("pending_files", Array())).size();
	data["poll_args"] = state.get("poll_args", Dictionary());
	return _ok(data);
}

bool SolersAssetService::is_project_import_ready(const Dictionary &p_args) const {
	const String import_id = String(p_args.get("_import_id", String())).strip_edges();
	MutexLock lock(project_imports_mutex);
	const Dictionary *state = project_imports.getptr(import_id);
	return !state || String(state->get("status", String())) != "pending";
}

Dictionary SolersAssetService::get_project_import_coordinator_state() const {
	Dictionary state;
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	state["wave_active"] = filesystem && filesystem->is_incremental_importing();
	int queued = 0;
	int active = 0;
	MutexLock lock(project_imports_mutex);
	for (const KeyValue<String, Dictionary> &E : project_imports) {
		if (String(E.value.get("status", String())) != "pending") {
			continue;
		}
		if (String(E.value.get("stage", String())) == "queued") {
			queued++;
		} else {
			active++;
		}
	}
	state["queued_count"] = queued;
	state["active_count"] = active;
	return state;
}

void SolersAssetService::release_project_import(const Dictionary &p_args, const Dictionary &p_result) {
	// Invoked when the tool call reaches a terminal state without consuming
	// its continuation — an aborted turn delivers TOOL_CANCELLED here. Pending
	// states are dropped so no further batches start for them; files already
	// handed to the incremental queue finish within the frame budget and leave
	// the project in a consistent imported state.
	if ((bool)p_result.get("ok", false) || String(Dictionary(p_result.get("error", Dictionary())).get("code", String())) != "TOOL_CANCELLED") {
		return;
	}
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (asset_id.is_empty()) {
		return;
	}
	const String target_dir = String(p_args.get("target_dir", String())).replace_char('\\', '/').simplify_path();
	Vector<String> released;
	MutexLock lock(project_imports_mutex);
	for (const KeyValue<String, Dictionary> &E : project_imports) {
		if (String(E.value.get("status", String())) != "pending") {
			continue;
		}
		const Dictionary poll_args = E.value.get("poll_args", Dictionary());
		if (String(poll_args.get("asset_id", String())) != asset_id) {
			continue;
		}
		if (!target_dir.is_empty() && String(poll_args.get("target_dir", String())) != target_dir) {
			continue;
		}
		released.push_back(E.key);
	}
	for (int i = 0; i < released.size(); i++) {
		project_imports.erase(released[i]);
	}
}

void SolersAssetService::poll(bool p_allow_new_imports) {
	_ensure_project_import_signals();
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	if (!filesystem) {
		MutexLock lock(project_imports_mutex);
		for (KeyValue<String, Dictionary> &E : project_imports) {
			Dictionary state = E.value;
			if (String(state.get("status", String())) != "pending") {
				continue;
			}
			state["status"] = "failed";
			state["stage"] = "failed";
			state["error"] = _error_data("EDITOR_FILESYSTEM_UNAVAILABLE", "Godot's editor filesystem became unavailable during project import.");
			E.value = state;
		}
		return;
	}

	// 1. Drive the active incremental batch within this frame's budget. Each
	// step imports whole files and emits resources_reimporting/reimported per
	// file, so the editor keeps rendering and responding between steps.
	bool stepped = false;
	if (filesystem->is_incremental_importing()) {
		const int64_t frame_budget_msec = ProjectSettings::get_singleton()->get_setting("solers/ai_assets/import/frame_budget_msec", 8);
		filesystem->reimport_files_incremental_step((uint64_t)MAX((int64_t)1, frame_budget_msec));
		stepped = true;
	}

	// 2. Hand the next batch of queued imports to the editor once the native
	// pipeline is free. update_files() registers every copied file in the
	// filesystem index; the incremental queue then owns the actual imports.
	if (p_allow_new_imports && !stepped && !filesystem->is_scanning() && !filesystem->is_importing() && !filesystem->is_incremental_importing()) {
		Vector<String> register_files;
		Vector<String> batch_files;
		Vector<String> batch_import_ids;
		HashSet<String> seen_register;
		HashSet<String> seen_batch;
		{
			MutexLock lock(project_imports_mutex);
			const uint64_t now = OS::get_singleton()->get_ticks_msec();
			for (KeyValue<String, Dictionary> &E : project_imports) {
				Dictionary state = E.value;
				if (String(state.get("status", String())) != "pending" || String(state.get("stage", String())) != "queued") {
					continue;
				}
				const Array files = state.get("files", Array());
				for (int i = 0; i < files.size(); i++) {
					const String path = files[i];
					if (!seen_register.has(path)) {
						seen_register.insert(path);
						register_files.push_back(path);
					}
				}
				const Array pending_files = state.get("pending_files", Array());
				for (int i = 0; i < pending_files.size(); i++) {
					const String path = pending_files[i];
					if (!seen_batch.has(path)) {
						seen_batch.insert(path);
						batch_files.push_back(path);
					}
				}
				state["stage"] = "importing";
				state["last_progress_msec"] = now;
				E.value = state;
				batch_import_ids.push_back(E.key);
			}
		}
		if (!register_files.is_empty()) {
			filesystem->update_files(register_files);
		}
		if (!batch_files.is_empty() && filesystem->reimport_files_incremental_begin(batch_files) != OK) {
			// The pipeline was grabbed between the checks above; retry next frame.
			MutexLock lock(project_imports_mutex);
			for (int i = 0; i < batch_import_ids.size(); i++) {
				Dictionary *state = project_imports.getptr(batch_import_ids[i]);
				if (state && String(state->get("status", String())) == "pending") {
					(*state)["stage"] = "queued";
				}
			}
		}
	}

	// 3. Settle imports whose files the editor has all confirmed through
	// resources_reimported. Verification loads each entrypoint once (the
	// resource cache reuses it afterwards); one import settles per frame to
	// bound main-thread work.
	const bool pipeline_busy = stepped || filesystem->is_scanning() || filesystem->is_importing() || filesystem->is_incremental_importing();
	String verify_id;
	Dictionary verify_state;
	{
		MutexLock lock(project_imports_mutex);
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		for (KeyValue<String, Dictionary> &E : project_imports) {
			Dictionary state = E.value;
			if (String(state.get("status", String())) != "pending") {
				continue;
			}
			const String stage = state.get("stage", String());
			if (pipeline_busy || (!p_allow_new_imports && stage == "queued")) {
				state["last_progress_msec"] = now;
				E.value = state;
			}
			if (!pipeline_busy && verify_id.is_empty() && stage == "importing" && Array(state.get("pending_files", Array())).is_empty()) {
				verify_id = E.key;
				verify_state = state.duplicate(true);
			}
		}
	}
	if (!verify_id.is_empty()) {
		const Array files = verify_state.get("files", Array());
		const Array entrypoints = verify_state.get("entrypoints", files);
		const Dictionary inspection = _project_import_inspection(files, entrypoints, true);
		const Array static_lightmap_paths = Dictionary(verify_state.get("result", Dictionary())).get("static_lightmap_import_paths", Array());
		if (!(bool)inspection.get("ready", false)) {
			verify_state["status"] = "failed";
			verify_state["stage"] = "failed";
			verify_state["error"] = _error_data("IMPORT_FAILED", "Godot confirmed every requested file import, but one or more resources are not indexed, valid, or loadable.");
			verify_state["diagnostics"] = inspection;
		} else if (!_static_lightmap_imports_ready(static_lightmap_paths)) {
			verify_state["status"] = "failed";
			verify_state["stage"] = "failed";
			verify_state["error"] = _error_data("IMPORT_OPTIONS_NOT_APPLIED", "Godot imported the static model without applying its native lightmap UV2 options.");
			verify_state["diagnostics"] = inspection;
		} else {
			const Dictionary contract_error = _project_import_contract_error(verify_state.get("result", Dictionary()), inspection);
			if (!contract_error.is_empty()) {
				verify_state["status"] = "failed";
				verify_state["stage"] = "failed";
				verify_state["error"] = _error_data(contract_error.get("code", "IMPORT_CONTRACT_FAILED"), contract_error.get("message", "Imported asset contract failed."));
				verify_state["diagnostics"] = contract_error.get("diagnostics", inspection);
			} else {
				verify_state["status"] = "complete";
				verify_state["stage"] = "complete";
				verify_state["result"] = _project_import_completed_data(verify_state.get("result", Dictionary()), inspection);
			}
		}
		MutexLock lock(project_imports_mutex);
		if (project_imports.has(verify_id)) {
			project_imports[verify_id] = verify_state;
		}
	}

	// 4. Stall backstop: a pending import whose files stop producing
	// completion events while the pipeline sits idle (for example a modal
	// dialog holding the import lock) eventually fails instead of hanging.
	if (!pipeline_busy) {
		MutexLock lock(project_imports_mutex);
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		for (KeyValue<String, Dictionary> &E : project_imports) {
			Dictionary state = E.value;
			if (String(state.get("status", String())) != "pending") {
				continue;
			}
			const uint64_t stall_timeout_msec = state.get("stall_timeout_msec", 0);
			if (stall_timeout_msec > 0 && now - (uint64_t)state.get("last_progress_msec", 0) >= stall_timeout_msec) {
				state["status"] = "failed";
				state["stage"] = "stalled";
				state["error"] = _error_data("IMPORT_STALLED", "Godot's native import pipeline stopped making observable progress.");
				E.value = state;
			}
		}
	}
}

SolersAssetService::SolersAssetService() {
	const Error root_error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(_asset_root()));
	if (root_error != OK) {
		ERR_PRINT(vformat("Unable to initialize Solers Library at %s (error %d).", _asset_root(), root_error));
		return;
	}
	const PackedStringArray dirs = DirAccess::get_directories_at(_asset_root());
	for (int i = 0; i < dirs.size(); i++) {
		Dictionary manifest = _read_json_file(_manifest_path(dirs[i]));
		const String status = String(manifest.get("status", String())).to_lower();
		if (status != "queued" && status != "running" && status != "interrupted") {
			continue;
		}
		const String provider = String(manifest.get("provider", String())).to_lower();
		const String provider_task_id = manifest.get("provider_task_id", String());
		const String endpoint = Dictionary(manifest.get("provider_request", Dictionary())).get("endpoint", String());
		if (provider != "meshy" || provider_task_id.is_empty() || endpoint.is_empty()) {
			manifest["status"] = "interrupted";
			manifest["error"] = _error_data("RECOVERY_REQUIRES_ACTION", "Solers will not retry an unfinished provider request without a recorded provider task id.");
			manifest["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
			String error;
			_write_json_atomic(_manifest_path(dirs[i]), manifest, error);
			continue;
		}
		const Dictionary config = _provider_config("3d", provider);
		Task *task = memnew(Task);
		task->asset_id = manifest.get("id", dirs[i]);
		task->api_key = config.get("api_key", String());
		task->service = this;
		task->resume_provider_task = true;
		manifest["status"] = "queued";
		manifest["stage"] = "recovering";
		manifest["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
		_set_task_state(task, manifest);
		tasks[task->asset_id] = task;
		task->thread.start(&SolersAssetService::_task_func, task);
	}
}

SolersAssetService::~SolersAssetService() {
	Vector<Task *> pending;
	{
		MutexLock lock(tasks_mutex);
		for (const KeyValue<String, Task *> &E : tasks) {
			pending.push_back(E.value);
		}
		tasks.clear();
	}
	for (int i = 0; i < pending.size(); i++) {
		pending[i]->abort.set();
		if (pending[i]->thread.is_started()) {
			pending[i]->thread.wait_to_finish();
		}
		memdelete(pending[i]);
	}
}
