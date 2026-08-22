/**************************************************************************/
/*  test_solers_assets.cpp                                                */
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
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/templates/pair.h"
#include "editor/asset_library/editor_asset_installer.h"
#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/generated/terrain3d_lock.gen.h"
#include "modules/solers_ai/plugins/solers_plugin.h"
#include "modules/solers_ai/plugins/solers_plugin_meshy.h"
#include "modules/solers_ai/plugins/solers_plugin_polyhaven.h"
#include "modules/solers_ai/tests/support/solers_test_state.h"
#include "modules/zip/zip_packer.h"

TEST_FORCE_LINK(test_solers_assets)

namespace TestSolersAssets {

class ScopedEnvironment {
	String name;
	String previous;
	bool existed = false;

public:
	ScopedEnvironment(const String &p_name, const String &p_value) :
			name(p_name), previous(OS::get_singleton()->get_environment(p_name)), existed(OS::get_singleton()->has_environment(p_name)) {
		OS::get_singleton()->set_environment(name, p_value);
	}

	~ScopedEnvironment() {
		if (existed) {
			OS::get_singleton()->set_environment(name, previous);
		} else {
			OS::get_singleton()->unset_environment(name);
		}
	}
};

class ScopedPluginRegistration {
	SolersPlugin *plugin = nullptr;

public:
	explicit ScopedPluginRegistration(SolersPlugin *p_plugin) : plugin(p_plugin) {
		SolersPluginRegistry::register_plugin(plugin);
	}

	~ScopedPluginRegistration() {
		SolersPluginRegistry::unregister_plugin(plugin);
		memdelete(plugin);
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

TEST_CASE("[Editor][SolersAddon] package paths are validated and project writes roll back") {
	const String unsafe_zip = "user://.solers_unsafe_plugin_contract.zip";
	const String package_zip = "user://.solers_plugin_transaction_contract.zip";
	const String target_dir = "res://.solers_plugin_transaction_contract";
	const String target_absolute = ProjectSettings::get_singleton()->globalize_path(target_dir);
	SolersTestPaths cleanup;
	cleanup.add(target_dir);
	cleanup.add(unsafe_zip);
	cleanup.add(package_zip);

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
}

TEST_CASE("[Editor][SolersAddon] bundled Terrain3D archive is pinned and self-describing") {
	const String invalid_archive = "user://.solers_invalid_terrain_bundle.zip";
	SolersTestPaths cleanup;
	cleanup.add(invalid_archive);
	Vector<Pair<String, String>> invalid_files;
	invalid_files.push_back(Pair<String, String>("addons/terrain_3d/plugin.cfg", "[plugin]"));
	write_test_zip(invalid_archive, invalid_files);
	Dictionary args;
	args["source"] = "bundled";
	args["plugin_id"] = "terrain3d";
	Dictionary mismatch;
	{
		ScopedEnvironment override("SOLERS_TERRAIN3D_ARCHIVE", ProjectSettings::get_singleton()->globalize_path(invalid_archive));
		SolersAssetService invalid_assets;
		mismatch = invalid_assets.addon_inspect(args);
	}
	CHECK_FALSE((bool)mismatch.get("ok", true));
	CHECK(Dictionary(mismatch.get("error", Dictionary())).get("code", String()) == "PLUGIN_HASH_MISMATCH");

	SolersAssetService assets;
	const Dictionary result = assets.addon_inspect(args);
	REQUIRE((bool)result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("version", String()) == SOLERS_TERRAIN3D_VERSION);
	CHECK(data.get("sha256", String()) == SOLERS_TERRAIN3D_SHA256);
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

TEST_CASE("[SolersPluginRegistry] an unknown connector extends every registry-driven surface") {
	const uint64_t revision_before = SolersPluginRegistry::get_revision();
	SolersSyntheticFuturePlugin *plugin = memnew(SolersSyntheticFuturePlugin);
	ScopedPluginRegistration registration(plugin);
	CHECK(SolersPluginRegistry::get_revision() == revision_before + 1);
	CHECK(SolersPluginRegistry::get_plugin("synthetic-future") == plugin);
	CHECK(SolersPluginRegistry::default_generator_for_kind("novel-geometry") == plugin);
	SolersTestPaths cleanup;
	SolersAssetService asset_service;
	CHECK(asset_service.is_provider_configured("novel-geometry", "synthetic-future"));
	SolersToolRegistry registry;
	registry.set_asset_service(&asset_service);
	registry.register_default_tools();
	const Dictionary generate = solers_test_find_dictionary(registry.list_tools(), SNAME("name"), "asset.generate");
	const Dictionary properties = Dictionary(generate.get("input_schema", Dictionary())).get("properties", Dictionary());
	CHECK(Array(Dictionary(properties.get("provider", Dictionary())).get("enum", Array())).has("synthetic-future"));
	CHECK(Array(Dictionary(properties.get("kind", Dictionary())).get("enum", Array())).has("novel-geometry"));
	CHECK(Dictionary(Dictionary(properties.get("provider_options", Dictionary())).get("properties", Dictionary())).has("density"));
	Dictionary request = JSON::parse_string(R"({"kind":"novel-geometry","provider":"synthetic-future","prompt":"global asset"})");
	const Dictionary queued = asset_service.generate(request);
	REQUIRE(queued.get("ok", false));
	const Dictionary queued_manifest = queued.get("data", Dictionary());
	CHECK(String(queued_manifest.get("target_dir", "invalid")).is_empty());
	cleanup.add("user://solers_jobs/" + String(queued_manifest.get("id", String())));
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
	Ref<SolersPluginJob> job;
	job.instantiate();
	if (SolersPlugin *registered = SolersPluginRegistry::get_plugin("synthetic-future")) {
		registered->run_job(job);
	}
	CHECK(plugin->job_ran);
}

TEST_CASE("[SolersAssetService][SceneTree] direct texture-set import follows requested manifest roles") {
	const String asset_id = ".solers_map_role_import_contract";
	const String asset_dir = "user://solers_jobs/" + asset_id;
	const String source_dir = asset_dir.path_join("source");
	const String target_dir = "res://.solers_map_role_import_contract";
	SolersTestPaths cleanup;
	cleanup.add(asset_dir);
	cleanup.add(target_dir);
	const String color_path = source_dir.path_join("color.png");
	const String normal_path = source_dir.path_join("normal.png");
	const String detail_path = source_dir.path_join("detail.png");
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
}

TEST_CASE("[SolersPluginMeshy] generation schema and validation follow the current provider contract") {
	SolersPluginMeshy meshy;
	Dictionary attachment;
	attachment["id"] = "img_contract";
	Array source_attachments;
	source_attachments.push_back(attachment);

	auto prepare = [&](const Dictionary &p_options, bool p_with_image) {
		Dictionary manifest;
		manifest["provider_options"] = p_options.duplicate(true);
		manifest["source_attachments"] = p_with_image ? source_attachments : Array();
		Dictionary args;
		args["prompt"] = "isolated game-ready character";
		return meshy.prepare_generate("3d", args, manifest);
	};

	const Dictionary legacy_options = JSON::parse_string(R"({"model_type":"standard","ai_model":"meshy-5"})");
	Dictionary rejected = prepare(legacy_options, false);
	CHECK(rejected.get("code", String()) == "INVALID_ARGUMENT");

	const Dictionary hero_options = JSON::parse_string(R"({"model_type":"standard","ai_model":"latest","image_enhancement":true,"remove_lighting":true,"ultra_mode":true,"texture_resolution":"8k"})");
	CHECK(prepare(hero_options, false).is_empty());
	CHECK_FALSE(prepare(hero_options, true).is_empty());

	const Dictionary schema = meshy.get_generation_options_schema("3d");
	CHECK_FALSE(schema.has("hd_texture"));
	CHECK(Dictionary(schema.get("ai_model", Dictionary())).get("default", String()) == "latest");
	CHECK(Array(Dictionary(schema.get("texture_resolution", Dictionary())).get("enum", Array())).has("8k"));
	CHECK(schema.has("ultra_mode"));
	Dictionary native_manifest;
	native_manifest["provider_options"] = hero_options;
	Dictionary native_args;
	native_args["prompt"] = "native import authority";
	CHECK(meshy.prepare_generate("3d", native_args, native_manifest).is_empty());
	CHECK_FALSE(native_manifest.has("import_constraints"));
}

TEST_CASE("[SolersPluginMeshy] offline operation contracts") {
	SolersPluginMeshy meshy;
	const Array operations = meshy.get_operation_defs();
	const Dictionary convert = solers_test_find_dictionary(operations, SNAME("operation_id"), "convert");
	const Dictionary resize = solers_test_find_dictionary(operations, SNAME("operation_id"), "resize");
	const Dictionary uv_unwrap = solers_test_find_dictionary(operations, SNAME("operation_id"), "uv_unwrap");
	const Dictionary animate = solers_test_find_dictionary(operations, SNAME("operation_id"), "animate_humanoid");
	REQUIRE_FALSE(convert.is_empty());
	REQUIRE_FALSE(resize.is_empty());
	REQUIRE_FALSE(uv_unwrap.is_empty());
	REQUIRE_FALSE(animate.is_empty());
	CHECK(convert.get("endpoint", String()) == "/openapi/v1/convert");
	CHECK(resize.get("endpoint", String()) == "/openapi/v1/resize");
	CHECK(uv_unwrap.get("endpoint", String()) == "/openapi/v1/uv-unwrap");
	CHECK(Array(Dictionary(convert.get("options_schema", Dictionary())).get("required", Array())).has("target_formats"));
	const Dictionary animation_properties = Dictionary(animate.get("options_schema", Dictionary())).get("properties", Dictionary());
	const Dictionary action = animation_properties.get("action_id", Dictionary());
	CHECK(action.get("enum_source", String()) == "animation_actions");
	CHECK(action.get("enum_value", String()) == "action_id");

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

TEST_CASE("[SolersAssetService] Meshy generation presets project native schema facts") {
	SolersPluginMeshy meshy;
	const Dictionary profile = meshy.get_profile();
	const Array presets = profile.get("generation_presets", Array());
	REQUIRE(presets.size() == 2);
	int default_count = 0;
	for (const Variant &value : presets) {
		const Dictionary preset = value;
		default_count += (bool)preset.get("default", false) ? 1 : 0;
		CHECK((int)preset.get("max_reference_images", 0) >= 1);
		CHECK_FALSE(Array(preset.get("featured_fields", Array())).is_empty());
		CHECK(bool(!Dictionary(preset.get("presentation", Dictionary())).is_empty() && (!(bool)preset.get("default", false) || (Dictionary(Dictionary(preset.get("presentation", Dictionary())).get("controls", Dictionary())).has("target_formats") && Dictionary(Dictionary(preset.get("presentation", Dictionary())).get("controls", Dictionary())).has("texture_image_url")))));
	}
	CHECK(default_count == 1);
	const Dictionary schema = meshy.get_generation_options_schema("3d");
	CHECK(Dictionary(schema.get("texture_resolution", Dictionary())).get("default", String()) == "2k");
}

TEST_CASE("[SolersAssetService] non-terminal asset.status rejects progress polling; job.wait declares host park") {
	const String asset_id = ".solers_background_wait_contract";
	const String asset_dir = "user://solers_jobs/" + asset_id;
	const String manifest_path = asset_dir.path_join("manifest.json");
	SolersTestPaths cleanup;
	cleanup.add(asset_dir);
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

} // namespace TestSolersAssets
