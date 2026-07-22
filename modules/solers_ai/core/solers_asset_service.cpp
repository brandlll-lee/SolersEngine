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
#include "core/extension/gdextension_manager.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
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
#include "modules/solers_ai/plugins/solers_plugin.h"
#include "modules/zip/zip_reader.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/node.h"
#include "scene/resources/mesh.h"
#include "scene/resources/packed_scene.h"

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

static String _solers_addon_inspection_key(const String &p_source, const String &p_plugin_id, const String &p_version, const String &p_sha256) {
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

static Dictionary _solers_public_addon_inspection(const Dictionary &p_inspection) {
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
	return "user://solers_jobs";
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


static Dictionary _solers_project_request(const Dictionary &p_args, const String &p_kind, const String &p_name, const String &p_job_id) {
	String target_dir = String(p_args.get("target_dir", String())).replace_char('\\', '/').simplify_path();
	if (target_dir.is_empty()) {
		target_dir = "res://assets/" + p_kind + "/" + SolersPlugin::safe_slug(p_name) + "-" + p_job_id.right(8);
	}
	Dictionary result;
	if (!target_dir.begins_with("res://") || target_dir.contains("..")) {
		result["error"] = SolersPlugin::error_data("INVALID_TARGET", "target_dir must stay inside res://.");
		return result;
	}
	Dictionary import_options;
	const char *option_names[] = { "map_types", "import_profile", "max_triangles" };
	for (const char *name : option_names) {
		if (p_args.has(name)) {
			import_options[name] = p_args[name];
		}
	}
	result["target_dir"] = target_dir;
	result["import_options"] = import_options;
	return result;
}


static String _solers_asset_status(const Dictionary &p_manifest) {
	return String(p_manifest.get("status", "unknown")).to_lower();
}

static Dictionary _solers_asset_traits(const Dictionary &p_manifest) {
	return Dictionary(p_manifest.get("traits", Dictionary())).duplicate(true);
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

static bool _solers_manifest_matches_operation(const Dictionary &p_manifest, const Dictionary &p_operation, String &r_reason) {
	const Dictionary requires = p_operation.get("requires", Dictionary());
	const String required_kind = String(requires.get("kind", String()));
	if (!required_kind.is_empty() && String(p_manifest.get("kind", String())).to_lower() != required_kind) {
		r_reason = "Asset kind does not match.";
		return false;
	}
	const String required_status = String(requires.get("status", String()));
	String actual_status = _solers_asset_status(p_manifest);
	if (actual_status == "imported") {
		actual_status = "ready";
	}
	if (!required_status.is_empty() && actual_status != required_status) {
		r_reason = "Asset status does not match.";
		return false;
	}
	const Dictionary traits = _solers_asset_traits(p_manifest);
	for (const Variant *key = requires.next(nullptr); key; key = requires.next(key)) {
		const String name = String(*key);
		if (name == "kind" || name == "status" || name == "task_id_fields") {
			continue;
		}
		if (traits.get(name, Variant()) != requires[*key]) {
			r_reason = "Asset traits do not match.";
			return false;
		}
	}
	const Array task_id_fields = requires.get("task_id_fields", Array());
	if (!task_id_fields.is_empty() && SolersPlugin::first_manifest_field(p_manifest, task_id_fields).is_empty()) {
		r_reason = "Provider task id is missing.";
		return false;
	}
	return true;
}

Dictionary SolersAssetService::_provider_config(const String &p_kind, const String &p_provider) const {
	EditorSettings *settings = EditorSettings::get_singleton();
	SolersPlugin *plugin = p_provider.is_empty() ? SolersPluginRegistry::default_generator_for_kind(p_kind) : SolersPluginRegistry::get_plugin(p_provider);
	if (!plugin) {
		return Dictionary();
	}
	const Dictionary profile = plugin->get_profile();
	const String provider = String(profile.get("id", String())).to_lower();
	const String setting_root = "solers/plugins/" + provider + "/";
	Dictionary config;
	config["provider"] = provider;
	config["profile"] = profile;
	config["base_url"] = settings && settings->has_setting(setting_root + "base_url") ? String(settings->get_setting(setting_root + "base_url")) : String(profile.get("base_url", String()));
	String key;
	if (settings && settings->has_setting(setting_root + "api_key")) {
		key = SolersSecretStore::unprotect(String(settings->get_setting(setting_root + "api_key")));
	}
	const String env = profile.get("api_key_env", String());
	if (key.is_empty() && !env.is_empty() && OS::get_singleton()->has_environment(env)) {
		key = OS::get_singleton()->get_environment(env);
	}
	config["api_key"] = key;
	config["api_key_env"] = env;
	return config;
}

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
	Vector<String> headers;
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(r_state.get("provider", String()));
	const PackedStringArray profile_headers = plugin ? PackedStringArray(plugin->get_profile().get("download_headers", PackedStringArray())) : PackedStringArray();
	for (const String &header : profile_headers) {
		headers.push_back(header);
	}
	Dictionary response = SolersPlugin::http_request("GET", p_url, headers, PackedByteArray(), 60000, 2 * 1024 * 1024, &p_task->abort);
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
	if (!SolersPlugin::write_bytes_atomic(preview_path, preview, write_error)) {
		r_state["preview_error"] = _error_data("PREVIEW_WRITE_FAILED", write_error);
		_set_task_state(p_task, r_state);
		return;
	}
	r_state["preview_file"] = preview_path;
	r_state.erase("preview_error");
	_set_task_state(p_task, r_state);
}


void SolersAssetService::_set_task_state(Task *p_task, const Dictionary &p_state) {
	{
		MutexLock lock(p_task->mutex);
		p_task->state = p_state.duplicate(true);
	}
	String error;
	SolersPlugin::write_json_atomic(_manifest_path(p_task->asset_id), p_state, error);
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
	if ((status != "imported" && status != "draft" && status != "failed" && status != "cancelled" && status != "interrupted") || String(state.get("session_id", String())).is_empty()) {
		return;
	}
	if (String(state.get("delivery_status", String())) != "delivered") {
		state["delivery_status"] = "pending";
		_set_task_state(p_task, state);
	}
	MutexLock lock(terminal_events_mutex);
	terminal_events.push_back(state);
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


void SolersAssetService::_run_task(Task *p_task) {
	Dictionary state = _task_state(p_task);
	const String provider = state.get("provider", String());
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(provider);
	if (!plugin) {
		state["status"] = "failed";
		state["error"] = _error_data("UNSUPPORTED_PROVIDER", vformat("Solers plugin is not registered: %s", provider));
		_set_task_state(p_task, state);
		return;
	}
	const Dictionary plugin_profile = plugin->get_profile();

	state["status"] = "running";
	state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	_set_task_state(p_task, state);

	if ((bool)plugin_profile.get("requires_api_key", false) && p_task->api_key.is_empty()) {
		if (p_task->resume_provider_task) {
			state["status"] = "interrupted";
			state["error"] = _error_data("API_KEY_MISSING", "A plugin API key is required to resume this asset task.");
			_set_task_state(p_task, state);
			return;
		}
		state["status"] = "failed";
		state["error"] = _error_data("API_KEY_MISSING", "Plugin API key is not configured.");
		_set_task_state(p_task, state);
		return;
	}
	if (p_task->resume_provider_task && !(bool)plugin_profile.get("supports_resume", false)) {
		state["status"] = "interrupted";
		state["error"] = _error_data("RESUME_UNSUPPORTED", vformat("Plugin %s does not support resuming provider jobs.", provider));
		_set_task_state(p_task, state);
		return;
	}

	Ref<SolersPluginJob> job;
	job.instantiate();
	job->setup(p_task->asset_id, p_task->api_key, state.get("base_url", String()), _source_dir(p_task->asset_id), p_task->resume_provider_task, &p_task->abort, state, [p_task](const Dictionary &p_state) {
		SolersAssetService::_set_task_state(p_task, p_state);
	});
	plugin->run_job(job);
	state = job->get_state();
	const String plugin_status = String(state.get("status", String())).to_lower();
	if (plugin_status == "failed" || plugin_status == "cancelled" || plugin_status == "interrupted") {
		return;
	}
	const Array files = job->get_files();

	if (String(state.get("status", String())) != "draft") {
		state["status"] = "ready";
		state["stage"] = "ready";
	}
	if (String(state.get("kind", String())).to_lower() == "3d") {
		// Provider-declared polycounts are estimates and can be far off.
		// Measure the delivered glTF geometry once here so every later budget
		// decision and model-facing report uses the same authoritative number.
		int64_t measured_triangles = 0;
		bool measured = false;
		for (int i = 0; i < files.size(); i++) {
			const String path = String(files[i]);
			const String extension = path.get_extension().to_lower();
			if (extension != "gltf" && extension != "glb") {
				continue;
			}
			Dictionary document;
			String parse_error;
			if (_solers_gltf_document_json(path, document, parse_error)) {
				measured_triangles += _solers_gltf_source_triangle_count(document);
				measured = true;
			}
		}
		if (measured) {
			const int64_t declared = state.get("polycount", 0);
			if (declared > 0 && declared != measured_triangles) {
				state["declared_polycount"] = declared;
			}
			state["polycount"] = measured_triangles;
		}
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
	_download_preview(p_task, state, job->get_preview_url());
}

Dictionary SolersAssetService::_manifest_for_asset(const String &p_asset_id) const {
	if (p_asset_id.begins_with("res://")) {
		if (!p_asset_id.ends_with(".solers.json")) {
			return Dictionary();
		}
		Dictionary sidecar = SolersPlugin::read_json_file(p_asset_id);
		if (sidecar.is_empty() || String(sidecar.get("plugin", String())).is_empty()) {
			return Dictionary();
		}
		sidecar["id"] = sidecar.get("job_id", String());
		sidecar["provider"] = sidecar.get("plugin", String());
		sidecar["sidecar_file"] = p_asset_id;
		return sidecar;
	}
	{
		MutexLock lock(tasks_mutex);
		Task *const *task = tasks.getptr(p_asset_id);
		if (task && *task) {
			return _task_state(*task);
		}
	}
	return SolersPlugin::read_json_file(_manifest_path(p_asset_id));
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
		if (status != "imported" && status != "draft" && status != "failed" && status != "cancelled" && status != "interrupted") {
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
	if (kind.is_empty()) {
		return _error("INVALID_ARGUMENT", "kind is required.");
	}
	String provider = String(p_args.get("provider", String())).strip_edges().to_lower();
	const Dictionary provider_config = _provider_config(kind, provider);
	provider = provider_config.get("provider", String());
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(provider);
	if (!plugin) {
		return _error("PLUGIN_NOT_FOUND", "No registered Solers plugin can generate the requested asset kind.");
	}
	const Dictionary plugin_profile = plugin->get_profile();
	if (!(bool)plugin_profile.get("supports_generation", false) || !Array(plugin_profile.get("kinds", Array())).has(kind)) {
		return _error("GENERATION_UNSUPPORTED", vformat("Plugin %s does not generate asset kind %s.", provider, kind));
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
	Dictionary provider_options = p_args.get("provider_options", Dictionary());
	const String asset_id = vformat("%s-%s_%s", itos((int64_t)Time::get_singleton()->get_unix_time_from_system()), String::num_uint64(OS::get_singleton()->get_ticks_usec()), (kind + prompt + provider).md5_text().substr(0, 10));
	const String name = p_args.get("name", prompt);
	const Dictionary project_request = _solers_project_request(p_args, kind, name, asset_id);
	if (project_request.has("error")) {
		const Dictionary error = project_request["error"];
		return _error(error.get("code", "INVALID_TARGET"), error.get("message", "Invalid project target."));
	}

	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = kind;
	manifest["name"] = name;
	manifest["prompt"] = prompt;
	manifest["profile"] = p_args.get("profile", "game_default");
	manifest["provider"] = provider;
	manifest["base_url"] = provider_config.get("base_url", String());
	manifest["provider_options"] = provider_options;
	manifest["target_dir"] = project_request["target_dir"];
	manifest["import_options"] = project_request["import_options"];
	if (!p_session_id.is_empty()) {
		manifest["session_id"] = p_session_id;
	}
	if (!source_attachments.is_empty()) {
		manifest["source_attachments"] = source_attachments;
		manifest["input_attachment_ids"] = input_attachment_ids.duplicate(true);
	}
	const Dictionary prepare_error = plugin->prepare_generate(kind, p_args, manifest);
	if (!prepare_error.is_empty()) {
		return _error(prepare_error.get("code", "PLUGIN_ARGUMENT_ERROR"), prepare_error.get("message", "Plugin rejected the generation request."));
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


bool SolersAssetService::is_trusted_addon(const Dictionary &p_args) {
	if (String(p_args.get("source", String())).to_lower() != "bundled" || String(p_args.get("plugin_id", String())).to_lower() != SOLERS_TERRAIN3D_ID) {
		return false;
	}
	const String version = String(p_args.get("version", String()));
	const String sha256 = String(p_args.get("sha256", String())).to_lower();
	return (version.is_empty() || version == String(SOLERS_TERRAIN3D_VERSION)) && (sha256.is_empty() || sha256 == String(SOLERS_TERRAIN3D_SHA256));
}

Dictionary SolersAssetService::addon_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String query = String(p_args.get("query", String())).strip_edges();
	if (query.is_empty()) {
		return _error("INVALID_ARGUMENT", "Addon search requires a non-empty query.");
	}
	const int limit = CLAMP((int)p_args.get("limit", 20), 1, 50);
	Array plugins;
	const Dictionary trusted = _solers_terrain3d_package();
	if (String(trusted.get("title", String())).to_lower().contains(query.to_lower()) || String(trusted.get("description", String())).to_lower().contains(query.to_lower())) {
		plugins.push_back(trusted);
	}

	const String url = vformat("https://godotengine.org/asset-library/api/asset?godot_version=%d.%d&filter=%s&page=0&max_results=%d", GODOT_VERSION_MAJOR, GODOT_VERSION_MINOR, query.uri_encode(), limit);
	const Dictionary response = SolersPlugin::http_request("GET", url, Vector<String>(), PackedByteArray(), 60000, 4 * 1024 * 1024, p_cancel_requested);
	if (!(bool)response.get("ok", false)) {
		const Dictionary error = response.get("error", Dictionary());
		return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "PLUGIN_SEARCH_FAILED", String(error.get("message", "Godot Asset Library search failed.")));
	}
	const Dictionary root = SolersPlugin::parse_json_body(response);
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

Dictionary SolersAssetService::addon_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
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
		const Dictionary response = SolersPlugin::http_request("GET", "https://godotengine.org/asset-library/api/asset/" + plugin_id.uri_encode(), Vector<String>(), PackedByteArray(), 60000, 4 * 1024 * 1024, p_cancel_requested);
		if (!(bool)response.get("ok", false)) {
			const Dictionary error = response.get("error", Dictionary());
			return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "PLUGIN_INSPECTION_FAILED", String(error.get("message", "Godot Asset Library inspection failed.")));
		}
		const Dictionary detail = SolersPlugin::parse_json_body(response);
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
		const String cache_dir = _solers_plugin_cache_root().path_join("assetlib").path_join(SolersPlugin::safe_slug(plugin_id)).path_join(SolersPlugin::safe_slug(plugin_metadata.get("version", String())));
		archive_path = cache_dir.path_join("package.zip");
		if ((bool)p_args.get("refresh", false) || !FileAccess::exists(archive_path)) {
			const Dictionary download = SolersPlugin::http_request("GET", detail.get("download_url", String()), Vector<String>(), PackedByteArray(), 180000, 512LL * 1024LL * 1024LL, p_cancel_requested);
			if (!(bool)download.get("ok", false)) {
				const Dictionary error = download.get("error", Dictionary());
				return _error(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "PLUGIN_DOWNLOAD_FAILED", String(error.get("message", "Plugin download failed.")));
			}
			String write_error;
			if (!SolersPlugin::write_bytes_atomic(archive_path, download.get("body", PackedByteArray()), write_error)) {
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
	inspection["inspection_id"] = _solers_addon_inspection_key(source, plugin_id, inspection.get("version", String()), actual_sha256);
	const String inspection_key = inspection.get("inspection_id", String());
	{
		MutexLock lock(catalog_cache_mutex);
		addon_inspections[inspection_key] = inspection.duplicate(true);
	}
	return _ok(_solers_public_addon_inspection(inspection));
}

Dictionary SolersAssetService::addon_agent_contract(const Dictionary &p_args) const {
	const String source = String(p_args.get("source", String())).strip_edges().to_lower();
	const String plugin_id = String(p_args.get("plugin_id", String())).strip_edges().to_lower();
	const String version = String(p_args.get("version", String())).strip_edges();
	const String sha256 = String(p_args.get("sha256", String())).strip_edges().to_lower();
	if (source.is_empty() || plugin_id.is_empty() || version.is_empty() || sha256.length() != 64) {
		return _error("INVALID_ARGUMENT", "source, plugin_id, version, and SHA-256 are required.");
	}
	{
		MutexLock lock(catalog_cache_mutex);
		const Dictionary inspection = addon_inspections.get(_solers_addon_inspection_key(source, plugin_id, version, sha256), Dictionary());
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

Dictionary SolersAssetService::addon_list(const Dictionary &p_args) const {
	const Dictionary lock_file = SolersPlugin::read_json_file(SOLERS_PLUGIN_LOCK_PATH);
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

Dictionary SolersAssetService::addon_ensure(const Dictionary &p_args) {
	const String source = String(p_args.get("source", String())).strip_edges().to_lower();
	const String plugin_id = String(p_args.get("plugin_id", String())).strip_edges().to_lower();
	const String version = String(p_args.get("version", String())).strip_edges();
	const String sha256 = String(p_args.get("sha256", String())).strip_edges().to_lower();
	if ((source != "bundled" && source != "assetlib") || plugin_id.is_empty() || version.is_empty() || sha256.length() != 64) {
		return _error("INVALID_ARGUMENT", "addon.ensure requires the exact source, plugin_id, version, and SHA-256 returned by addon.inspect.");
	}
	const String inspection_key = _solers_addon_inspection_key(source, plugin_id, version, sha256);
	Dictionary lock_file = SolersPlugin::read_json_file(SOLERS_PLUGIN_LOCK_PATH);
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
		inspection = addon_inspections.get(inspection_key, Dictionary()).duplicate(true);
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
	if (!SolersPlugin::write_json_atomic(SOLERS_PLUGIN_LOCK_PATH, lock_file, lock_error)) {
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
	return addon_ensure_finalize(finalize_args);
}

bool SolersAssetService::addon_ensure_ready(const Dictionary &p_args) const {
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	if (!filesystem || !filesystem->is_scanning()) {
		return true;
	}
	return OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)p_args.get("deadline_msec", 0);
}

Dictionary SolersAssetService::addon_ensure_finalize(const Dictionary &p_args) {
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	if (filesystem && filesystem->is_scanning()) {
		return _error("FILESYSTEM_SCAN_TIMEOUT", "The editor filesystem scan did not finish in time; call addon.ensure again once it settles.");
	}
	const String plugin_key = p_args.get("plugin_key", String());
	Dictionary lock_file = SolersPlugin::read_json_file(SOLERS_PLUGIN_LOCK_PATH);
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
			restart_reasons.push_back(vformat("%s is not loaded in the current editor process; restart the editor, then call addon.ensure with the same arguments.", extension_path));
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
	const bool lock_updated = SolersPlugin::write_json_atomic(SOLERS_PLUGIN_LOCK_PATH, lock_file, lock_error);
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
		Dictionary result = _error("ADDON_RESTART_REQUIRED", restart_reasons.is_empty() ? "Installation completed; restart the editor once, then call addon.ensure with the same arguments." : String(restart_reasons[0]));
		result["data"] = data;
		return result;
	}
	// The verdict follows the authoritative usability facts: files on disk and
	// every declared entry class registered in ClassDB. Residual issues (for
	// example an addon editor script that failed to enable) stay visible as
	// load_errors data without blocking a plugin whose classes already work.
	if (!files_present || !missing_classes.is_empty()) {
		Dictionary result = _error("ADDON_LOAD_FAILED", load_errors.is_empty() ? "The addon failed its runtime readiness checks." : String(load_errors[0]));
		result["data"] = data;
		return result;
	}
	return _ok(data);
}

Dictionary SolersAssetService::catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String provider = String(p_args.get("provider", String())).strip_edges().to_lower();
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(provider);
	if (!plugin || !(bool)plugin->get_profile().get("supports_catalog", false)) {
		return _error("PLUGIN_NOT_FOUND", "Catalog search requires a registered catalog plugin.");
	}
	return plugin->catalog_search(p_args, p_cancel_requested);
}

Dictionary SolersAssetService::catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String provider = String(p_args.get("provider", String())).strip_edges().to_lower();
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(provider);
	if (!plugin || !(bool)plugin->get_profile().get("supports_catalog", false)) {
		return _error("PLUGIN_NOT_FOUND", "Catalog inspect requires a registered catalog plugin.");
	}
	return plugin->catalog_inspect(p_args, p_cancel_requested);
}

Dictionary SolersAssetService::catalog_acquire(const Dictionary &p_args, const String &p_session_id) {
	const String provider = String(p_args.get("provider", String())).strip_edges().to_lower();
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	const String source_asset_id = String(p_args.get("asset_id", String())).strip_edges();
	const String requested_variant = String(p_args.get("variant", String())).strip_edges();
	const String pinned_source_version = String(p_args.get("source_version", String())).strip_edges();
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(provider);
	const Dictionary profile = plugin ? plugin->get_profile() : Dictionary();
	if (!plugin || !(bool)profile.get("supports_catalog", false) || !Array(profile.get("kinds", Array())).has(kind) ||
			source_asset_id.is_empty() || requested_variant.is_empty() || source_asset_id.contains("/") || source_asset_id.contains("\\") || source_asset_id.contains("..") ||
			requested_variant.contains("/") || requested_variant.contains("\\") || requested_variant.contains("..")) {
		return _error("INVALID_ARGUMENT", "Catalog acquire requires a registered catalog plugin, supported kind, asset_id, and inspected variant.");
	}

	const Dictionary inspected = plugin->cached_inspection(kind, source_asset_id);
	const String official_variant = SolersPlugin::match_catalog_variant_id(inspected.get("variants", Array()), requested_variant);
	const String source_version = inspected.get("source_version", String());
	if (inspected.is_empty() || official_variant.is_empty() || source_version.is_empty()) {
		return _error("CATALOG_INSPECTION_REQUIRED", "Acquire requires a prior asset.catalog.inspect and an exact variants[].id from it.");
	}
	if (!pinned_source_version.is_empty() && pinned_source_version != source_version) {
		return _error("CATALOG_VERSION_CHANGED", "The pinned source_version no longer matches the inspected catalog asset.");
	}

	const String asset_id = vformat("%s-%s_%s", itos((int64_t)Time::get_singleton()->get_unix_time_from_system()), String::num_uint64(OS::get_singleton()->get_ticks_usec()), (provider + source_asset_id + official_variant + source_version).md5_text().substr(0, 10));
	const String name = p_args.get("name", inspected.get("display_name", source_asset_id));
	const Dictionary project_request = _solers_project_request(p_args, kind, name, asset_id);
	if (project_request.has("error")) {
		const Dictionary error = project_request["error"];
		return _error(error.get("code", "INVALID_TARGET"), error.get("message", "Invalid project target."));
	}
	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = kind;
	manifest["name"] = name;
	manifest["provider"] = provider;
	manifest["base_url"] = profile.get("base_url", String());
	manifest["source_provider"] = provider;
	manifest["source_asset_id"] = source_asset_id;
	manifest["source_variant"] = official_variant;
	manifest["source_version"] = source_version;
	manifest["license"] = inspected.get("license", profile.get("license", String()));
	manifest["attribution"] = inspected.get("attribution", profile.get("attribution", String()));
	manifest["target_dir"] = project_request["target_dir"];
	manifest["import_options"] = project_request["import_options"];
	manifest["status"] = "queued";
	manifest["stage"] = "queued";
	manifest["created_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	manifest["files"] = Array();
	manifest["entrypoints"] = Array();
	if (!p_session_id.is_empty()) {
		manifest["session_id"] = p_session_id;
	}
	return _queue_manifest(manifest, _provider_config(kind, provider));
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
		if (status == "imported" || status == "draft" || status == "failed" || status == "cancelled" || status == "interrupted") {
			Dictionary state;
			state["id"] = id;
			state["status"] = status;
			const char *fields[] = { "kind", "name", "stage", "project_entrypoints", "project_files", "target_dir", "sidecar_file", "error", "provider", "source_provider", "source_asset_id", "source_variant", "source_version", "generation_mode", "provider_request", "input_attachment_ids" };
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
	if (!SolersPlugin::write_json_atomic(_manifest_path(asset_id), p_manifest, error)) {
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
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(manifest.get("provider", String()));
	if (!plugin) {
		return _error("PLUGIN_NOT_FOUND", "The asset's Solers plugin is not registered.");
	}
	Array operations;
	Array available_operations;
	const Array defs = plugin->get_operation_defs();
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
	const Dictionary extras = plugin->capability_extras(manifest);
	for (const Variant *key = extras.next(nullptr); key; key = extras.next(key)) {
		data[*key] = extras[*key];
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
	const String source_job_id = String(source.get("id", source_asset_id));
	const String provider = String(source.get("provider", String())).to_lower();
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(provider);
	if (!plugin) {
		return _error("PLUGIN_NOT_FOUND", "The source asset's Solers plugin is not registered.");
	}
	Dictionary operation;
	const Array defs = plugin->get_operation_defs();
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
	const Dictionary requires = operation.get("requires", Dictionary());
	const String source_task_id = SolersPlugin::first_manifest_field(source, requires.get("task_id_fields", Array()));
	const String kind = String(source.get("kind", String())).to_lower();
	const Dictionary provider_config = _provider_config(kind, provider);
	Dictionary provider_options;
	SolersPlugin::merge_options(provider_options, options);
	SolersPlugin::merge_options(provider_options, raw_provider_options);
	const Dictionary prepare_error = plugin->prepare_operation(operation, source, provider_options);
	if (!prepare_error.is_empty()) {
		return _error(prepare_error.get("code", "PLUGIN_ARGUMENT_ERROR"), prepare_error.get("message", "Plugin rejected the operation request."));
	}
	const Dictionary import_constraints = provider_options.get("_solers_import_constraints", Dictionary());
	provider_options.erase("_solers_import_constraints");

	const String label = String(operation.get("label", operation_id));
	const String name = String(source.get("name", source_job_id)) + " " + label;
	const String prompt = String(source.get("prompt", String()));
	const String asset_id = vformat("%s_%s", itos((int64_t)Time::get_singleton()->get_unix_time_from_system()), (kind + source_job_id + operation_id + JSON::stringify(provider_options)).md5_text().substr(0, 10));
	Dictionary operation_args = p_args.duplicate(true);
	if (!operation_args.has("target_dir") && !String(source.get("target_dir", String())).is_empty()) {
		operation_args["target_dir"] = source["target_dir"];
	}
	const Dictionary project_request = _solers_project_request(operation_args, kind, name, asset_id);
	if (project_request.has("error")) {
		const Dictionary error = project_request["error"];
		return _error(error.get("code", "INVALID_TARGET"), error.get("message", "Invalid project target."));
	}
	Dictionary traits = _solers_asset_traits(source);
	const Dictionary result_traits = operation.get("result_traits", Dictionary());
	for (const Variant *key = result_traits.next(nullptr); key; key = result_traits.next(key)) {
		traits[*key] = result_traits[*key];
	}
	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = kind;
	manifest["name"] = name;
	manifest["prompt"] = prompt;
	manifest["profile"] = source.get("profile", "game_default");
	manifest["provider"] = provider;
	manifest["base_url"] = provider_config.get("base_url", String());
	manifest["provider_options"] = provider_options;
	if (!import_constraints.is_empty()) {
		manifest["import_constraints"] = import_constraints;
	}
	manifest["target_dir"] = project_request["target_dir"];
	manifest["import_options"] = project_request["import_options"];
	const String session_id = p_session_id.is_empty() ? String(source.get("session_id", String())) : p_session_id;
	if (!session_id.is_empty()) {
		manifest["session_id"] = session_id;
	}
	manifest["parent_asset_id"] = source_job_id;
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
		const Dictionary manifest = SolersPlugin::read_json_file(_manifest_path(dirs[i]));
		const String status = String(manifest.get("status", String())).to_lower();
		if (String(manifest.get("session_id", String())) == p_session_id && String(manifest.get("delivery_status", String())) != "delivered" &&
				(status == "imported" || status == "draft" || status == "failed" || status == "cancelled" || status == "interrupted")) {
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
		const Dictionary manifest = SolersPlugin::read_json_file(_manifest_path(dirs[i]));
		if (String(manifest.get("session_id", String())) != p_session_id) {
			continue;
		}
		const String status = String(manifest.get("status", String())).to_lower();
		if (status == "queued" || status == "running" || status == "ready" || status == "importing") {
			return true;
		}
	}
	return false;
}

void SolersAssetService::mark_terminal_delivered(const String &p_asset_id, const String &p_session_id) {
	if (p_asset_id.is_empty() || p_session_id.is_empty()) {
		return;
	}
	Task *task = nullptr;
	{
		MutexLock lock(tasks_mutex);
		Task *const *found = tasks.getptr(p_asset_id);
		task = found ? *found : nullptr;
	}
	Dictionary manifest = task ? _task_state(task) : SolersPlugin::read_json_file(_manifest_path(p_asset_id));
	if (String(manifest.get("session_id", String())) != p_session_id) {
		return;
	}
	manifest["delivery_status"] = "delivered";
	if (task) {
		_set_task_state(task, manifest);
	} else {
		String error;
		SolersPlugin::write_json_atomic(_manifest_path(p_asset_id), manifest, error);
	}
	_cleanup_finished_task(p_asset_id);
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

Dictionary SolersAssetService::start_project_import(const Dictionary &p_args) {
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
		const String stage = String(manifest.get("stage", String()));
		const int progress = (int)manifest.get("progress", 0);
		Dictionary error;
		if (status == "queued" || status == "running") {
			error = _error("ASSET_NOT_READY", vformat("Asset %s is still %s (stage: %s, %d%%). Do other work, then call job.wait with this asset id; Solers resumes you when it is ready.", asset_id, status, stage.is_empty() ? "processing" : stage, progress));
		} else {
			error = _error("ASSET_NOT_READY", vformat("Asset %s is in status \"%s\" and cannot be imported. Inspect it with asset.status, or acquire it again.", asset_id, status));
		}
		Dictionary data;
		data["status"] = status;
		data["stage"] = stage;
		data["progress"] = progress;
		error["data"] = data;
		return error;
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
			requested_map_types = available_map_types;
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
	const Dictionary selection = SolersPlugin::project_import_selection(source_files, declared_import_files, declared_entrypoints);
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
		const int64_t budget_ceiling = ProjectSettings::get_singleton()->get_setting("solers/import/max_source_triangles", 2000000);
		if (asset_kind == "3d" && budget_ceiling > 0 && (max_triangles == 0 || max_triangles > budget_ceiling)) {
			return _error("BUDGET_CEILING_EXCEEDED", vformat("Declared max_triangles %d exceeds the project ceiling of %d (solers/import/max_source_triangles). Import a variant that fits, or raise that project setting through project.edit settings first if the project can truly afford the density.", max_triangles, budget_ceiling));
		}
	}
	const int64_t plugin_triangle_limit = Dictionary(manifest.get("import_constraints", Dictionary())).get("max_triangles", 0);
	if (max_triangles < 0 && asset_kind == "3d") {
		if (plugin_triangle_limit > 0) {
			max_triangles = plugin_triangle_limit;
			triangle_budget_source = "asset_manifest";
		} else {
			max_triangles = ProjectSettings::get_singleton()->get_setting("solers/import/max_source_triangles", 2000000);
			triangle_budget_source = "project_default";
		}
	}
	String target_dir = String(p_args.get("target_dir", manifest.get("target_dir", String())));
	if (target_dir.is_empty()) {
		target_dir = "res://assets/" + String(manifest.get("kind", "asset")) + "/" + SolersPlugin::safe_slug(String(manifest.get("name", asset_id))) + "-" + asset_id.right(8);
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
			String remediation_operation;
			SolersPlugin *plugin = SolersPluginRegistry::get_plugin(manifest.get("provider", String()));
			const Array operation_defs = plugin ? plugin->get_operation_defs() : Array();
			for (int i = 0; i < operation_defs.size(); i++) {
				const Dictionary operation = operation_defs[i];
				if (!Array(operation.get("remediates", Array())).has("triangle_budget")) {
					continue;
				}
				String availability_reason;
				if (_solers_manifest_matches_operation(manifest, operation, availability_reason)) {
					remediation_operation = operation.get("operation_id", String());
					break;
				}
			}
			const String remediation = !remediation_operation.is_empty()
					? vformat("Run asset.run_operation with operation_id=\"%s\" or acquire a lower-poly variant.", remediation_operation)
					: String("This asset's provider does not offer a remesh operation; acquire a lower-poly variant instead.");
			Dictionary error = _error("TOPOLOGY_BUDGET_EXCEEDED", vformat("The asset's source geometry contains %d triangles, exceeding the import budget of %d (%s). %s", source_triangle_count, max_triangles, triangle_budget_source, remediation));
			Dictionary data;
			data["triangle_count"] = source_triangle_count;
			data["max_triangles"] = max_triangles;
			data["triangle_budget_source"] = triangle_budget_source;
			data["remediation_operation"] = remediation_operation;
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
	if (plugin_triangle_limit > 0) {
		result_data["plugin_triangle_limit"] = plugin_triangle_limit;
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
		if (!SolersPlugin::copy_file(src, dst, err)) {
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
	Array deferred_register_files;
	for (int i = 0; i < imported.size(); i++) {
		const String dst = imported[i];
		const Ref<ResourceImporter> dst_importer = format_importer ? format_importer->get_importer_by_file(dst) : Ref<ResourceImporter>();
		if (dst_importer.is_null()) {
			// Files Godot does not import (.tres, .bin, ...) may reference the
			// imported ones, so they register only after those imports settle;
			// indexing them earlier lets previews/loads race the pipeline.
			deferred_register_files.push_back(dst);
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
	state["deferred_register_files"] = deferred_register_files;
	state["import_file_count"] = pending_import_files.size();
	state["result"] = result_data;
	state["poll_args"] = poll_args;
	state["transaction_key"] = transaction_key;
	state["last_progress_msec"] = OS::get_singleton()->get_ticks_msec();
	const int64_t stall_timeout_seconds = ProjectSettings::get_singleton()->get_setting("solers/import/stall_timeout_seconds", 300);
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

void SolersAssetService::_advance_project_tasks(bool p_allow_new_imports) {
	Vector<Task *> candidates;
	{
		MutexLock lock(tasks_mutex);
		for (const KeyValue<String, Task *> &entry : tasks) {
			Task *task = entry.value;
			if (!task || !task->done.is_set()) {
				continue;
			}
			const String status = String(_task_state(task).get("status", String())).to_lower();
			if (status == "importing" || (p_allow_new_imports && status == "ready")) {
				candidates.push_back(task);
			}
		}
	}

	for (Task *task : candidates) {
		Dictionary state = _task_state(task);
		const String status = String(state.get("status", String())).to_lower();
		Dictionary result;
		if (status == "ready") {
			Dictionary args = Dictionary(state.get("import_options", Dictionary())).duplicate(true);
			args["asset_id"] = task->asset_id;
			args["target_dir"] = state.get("target_dir", String());
			result = start_project_import(args);
		} else if (status == "importing") {
			result = poll_project_import(state.get("import_poll_args", Dictionary()));
		} else {
			continue;
		}

		if (!(bool)result.get("ok", false)) {
			state["status"] = "failed";
			state["stage"] = "import_failed";
			state["error"] = result.get("error", _error_data("IMPORT_FAILED", "Project import failed."));
			state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
			_set_task_state(task, state);
			_queue_terminal_event(task);
			continue;
		}

		const Dictionary data = result.get("data", Dictionary());
		if (String(data.get("status", String())) == "pending") {
			state["status"] = "importing";
			state["stage"] = data.get("stage", "queued");
			state["import_poll_args"] = data.get("poll_args", Dictionary());
			state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
			_set_task_state(task, state);
			continue;
		}

		Dictionary sidecar;
		sidecar["schema_version"] = 1;
		sidecar["job_id"] = task->asset_id;
		sidecar["plugin"] = state.get("provider", String());
		sidecar["status"] = "imported";
		sidecar["kind"] = state.get("kind", String());
		sidecar["name"] = state.get("name", String());
		sidecar["prompt"] = state.get("prompt", String());
		sidecar["profile"] = state.get("profile", String());
		sidecar["parent_job_id"] = state.get("parent_asset_id", String());
		sidecar["operation"] = state.get("operation", String());
		sidecar["operation_label"] = state.get("operation_label", String());
		sidecar["traits"] = state.get("traits", Dictionary());
		sidecar["animations"] = state.get("animations", Dictionary());
		sidecar["source_asset_id"] = state.get("source_asset_id", String());
		sidecar["source_variant"] = state.get("source_variant", String());
		sidecar["source_version"] = state.get("source_version", String());
		sidecar["license"] = state.get("license", String());
		sidecar["attribution"] = state.get("attribution", String());
		sidecar["files"] = data.get("files", Array());
		sidecar["entrypoints"] = data.get("entrypoints", Array());
		sidecar["target_dir"] = data.get("target_dir", state.get("target_dir", String()));
		if (SolersPlugin *plugin = SolersPluginRegistry::get_plugin(state.get("provider", String()))) {
			const Array operations = plugin->get_operation_defs();
			for (int operation_index = 0; operation_index < operations.size(); operation_index++) {
				const Array task_id_fields = Dictionary(Dictionary(operations[operation_index]).get("requires", Dictionary())).get("task_id_fields", Array());
				for (int field_index = 0; field_index < task_id_fields.size(); field_index++) {
					const String field = task_id_fields[field_index];
					if (state.has(field)) {
						sidecar[field] = state[field];
					}
				}
			}
		}
		sidecar["imported_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
		const String sidecar_path = String(sidecar["target_dir"]).path_join(SolersPlugin::safe_slug(state.get("name", task->asset_id)) + "-" + task->asset_id.right(8) + ".solers.json");
		String sidecar_error;
		if (!SolersPlugin::write_json_atomic(sidecar_path, sidecar, sidecar_error)) {
			state["status"] = "failed";
			state["stage"] = "sidecar_failed";
			state["error"] = _error_data("SIDECAR_WRITE_FAILED", sidecar_error);
		} else {
			state["status"] = "imported";
			state["stage"] = "imported";
			state["project_files"] = data.get("files", Array());
			state["project_entrypoints"] = data.get("entrypoints", Array());
			state["target_dir"] = data.get("target_dir", state.get("target_dir", String()));
			state["sidecar_file"] = sidecar_path;
			state.erase("import_poll_args");
			if (EditorFileSystem *filesystem = _solers_editor_filesystem()) {
				filesystem->update_file(sidecar_path);
			}
		}
		state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
		_set_task_state(task, state);
		_queue_terminal_event(task);
	}
}

void SolersAssetService::poll(bool p_allow_new_imports) {
	_advance_project_tasks(p_allow_new_imports);
	_ensure_project_import_signals();
	EditorFileSystem *filesystem = _solers_editor_filesystem();
	if (!filesystem) {
		{
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
		}
		_advance_project_tasks(false);
		return;
	}

	// 1. Drive the active incremental batch within this frame's budget. Each
	// step imports whole files and emits resources_reimporting/reimported per
	// file, so the editor keeps rendering and responding between steps.
	bool stepped = false;
	if (filesystem->is_incremental_importing()) {
		const int64_t frame_budget_msec = ProjectSettings::get_singleton()->get_setting("solers/import/frame_budget_msec", 8);
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
				HashSet<String> deferred;
				const Array deferred_files = state.get("deferred_register_files", Array());
				for (int i = 0; i < deferred_files.size(); i++) {
					deferred.insert(String(deferred_files[i]));
				}
				for (int i = 0; i < files.size(); i++) {
					const String path = files[i];
					if (deferred.has(path)) {
						continue; // Registers after this transaction's imports settle.
					}
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
		// Every import this transaction depends on has settled; the files that
		// reference them can now register and load without racing the pipeline.
		const Array deferred_files = verify_state.get("deferred_register_files", Array());
		if (!deferred_files.is_empty()) {
			Vector<String> deferred_register;
			for (int i = 0; i < deferred_files.size(); i++) {
				deferred_register.push_back(String(deferred_files[i]));
			}
			filesystem->update_files(deferred_register);
			verify_state.erase("deferred_register_files");
		}
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
	_advance_project_tasks(p_allow_new_imports);
}

SolersAssetService::SolersAssetService() {
	const Error root_error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(_asset_root()));
	if (root_error != OK) {
		ERR_PRINT(vformat("Unable to initialize the Solers job staging directory at %s (error %d).", _asset_root(), root_error));
		return;
	}
	const PackedStringArray dirs = DirAccess::get_directories_at(_asset_root());
	for (int i = 0; i < dirs.size(); i++) {
		Dictionary manifest = SolersPlugin::read_json_file(_manifest_path(dirs[i]));
		const String status = String(manifest.get("status", String())).to_lower();
		if (status == "ready" || status == "importing") {
			Task *task = memnew(Task);
			task->asset_id = manifest.get("id", dirs[i]);
			task->service = this;
			task->done.set();
			manifest["status"] = "ready";
			manifest["stage"] = "recovering_import";
			manifest.erase("import_poll_args");
			manifest["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
			_set_task_state(task, manifest);
			tasks[task->asset_id] = task;
			continue;
		}
		if (status != "queued" && status != "running" && status != "interrupted") {
			continue;
		}
		const String provider = String(manifest.get("provider", String())).to_lower();
		SolersPlugin *plugin = SolersPluginRegistry::get_plugin(provider);
		if (!plugin || !(bool)plugin->get_profile().get("supports_resume", false)) {
			manifest["status"] = "interrupted";
			manifest["error"] = _error_data("RECOVERY_REQUIRES_ACTION", "The asset's plugin cannot resume unfinished provider work.");
			manifest["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
			String error;
			SolersPlugin::write_json_atomic(_manifest_path(dirs[i]), manifest, error);
			continue;
		}
		const Dictionary config = _provider_config(manifest.get("kind", String()), provider);
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
