/**************************************************************************/
/*  solers_script_authorities.cpp                                         */
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

#include "core/io/config_file.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"

#include "modules/solers_ai/core/solers_script_context.h"
#include "modules/solers_ai/core/solers_script_service.h"

static Dictionary _authority_ok(const Dictionary &p_data = Dictionary()) {
	return Dictionary({ { "ok", true }, { "data", p_data } });
}

static Dictionary _authority_error(const String &p_code, const String &p_message) {
	return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", p_code }, { "message", p_message }, { "recoverable", true } }) } });
}

static void _publish_authority_files(const String &p_source_path, const Dictionary &p_result) {
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!filesystem) {
		return;
	}
	filesystem->update_file(p_source_path);
	const Dictionary data = p_result.get("data", Dictionary());
	for (const Variant &value : Array(data.get("outputs", Array()))) {
		const String path = Dictionary(value).get("path", String());
		if (!path.is_empty()) {
			filesystem->update_file(path);
		}
	}
}

static Dictionary _validate_scene_authority(const String &p_path) {
	EditorNode *editor = EditorNode::get_singleton();
	Node *edited_scene = editor ? editor->get_edited_scene() : nullptr;
	EditorData &editor_data = EditorNode::get_editor_data();
	if (edited_scene && edited_scene->get_scene_file_path() == p_path && editor_data.is_scene_changed(editor_data.get_edited_scene())) {
		return _authority_error("SCENE_HAS_UNSAVED_CHANGES", "Save or revert the open scene before running scene.script on it.");
	}
	return _authority_ok();
}

static Dictionary _prepare_scene_authority(const String &p_path) {
	const Ref<PackedScene> scene = ResourceLoader::load(p_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP);
	Node *root = scene.is_valid() ? scene->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED) : nullptr;
	SceneTree *tree = SceneTree::get_singleton();
	if (!root || !tree || !tree->get_root()) {
		if (root) {
			memdelete(root);
		}
		return _authority_error("SCENE_LOAD_FAILED", vformat("Failed to instantiate scene: %s", p_path));
	}
	tree->get_root()->add_child(root);
	return _authority_ok(Dictionary({ { "subject", root }, { "import_controls", false } }));
}

static Dictionary _commit_scene_authority(const Ref<SolersScriptContext> &p_context) {
	Node *root = Object::cast_to<Node>(p_context->get_subject());
	Ref<PackedScene> packed;
	packed.instantiate();
	const Error pack_error = root ? packed->pack(root) : ERR_INVALID_DATA;
	const Error save_error = pack_error == OK ? ResourceSaver::save(packed, p_context->get_source_path()) : pack_error;
	return save_error == OK ? _authority_ok() : _authority_error("SCENE_SAVE_FAILED", vformat("Failed to save scripted scene transaction, error code %d.", save_error));
}

static void _release_scene_authority(const Ref<SolersScriptContext> &p_context) {
	Node *root = Object::cast_to<Node>(p_context->get_subject());
	if (!root) {
		return;
	}
	if (root->get_parent()) {
		root->get_parent()->remove_child(root);
	}
	memdelete(root);
}

static Dictionary _prepare_asset_authority(const String &p_path) {
	const Ref<Resource> resource = ResourceLoader::load(p_path, String(), ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP);
	if (resource.is_null()) {
		return _authority_error("ASSET_LOAD_FAILED", vformat("Failed to load imported asset: %s", p_path));
	}
	Dictionary options;
	Ref<ConfigFile> config;
	config.instantiate();
	if (config->load(p_path + ".import") == OK && config->has_section("params")) {
		for (const String &key : config->get_section_keys("params")) {
			options[key] = config->get_value("params", key);
		}
	}
	return _authority_ok(Dictionary({ { "subject", resource }, { "import_options", options }, { "import_controls", true } }));
}

static Dictionary _commit_asset_authority(const Ref<SolersScriptContext> &p_context) {
	if (!p_context->has_changed() && !p_context->needs_reimport()) {
		return _authority_ok();
	}
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!filesystem || !filesystem->get_filesystem() || filesystem->is_scanning()) {
		return _authority_error("ASSET_IMPORT_BUSY", "Godot's editor filesystem is not ready to commit the asset transaction.");
	}
	Ref<ConfigFile> config;
	config.instantiate();
	const String import_path = p_context->get_source_path() + ".import";
	const Error load_error = config->load(import_path);
	if (load_error != OK) {
		return _authority_error("IMPORT_SETTINGS_UNAVAILABLE", vformat("Failed to load import settings, error code %d.", load_error));
	}
	const Dictionary options = p_context->get_import_options();
	for (const Variant *key = options.next(nullptr); key; key = options.next(key)) {
		config->set_value("params", *key, options[*key]);
	}
	const Error save_error = config->save(import_path);
	if (save_error != OK) {
		return _authority_error("IMPORT_SETTINGS_SAVE_FAILED", vformat("Failed to save import settings, error code %d.", save_error));
	}
	filesystem->reimport_files({ p_context->get_source_path() });
	return _authority_ok(Dictionary({ { "reimported", true } }));
}

void solers_script_authorities_initialize() {
	SolersScriptAuthority scene;
	scene.target_argument = SNAME("scene_path");
	scene.validate = _validate_scene_authority;
	scene.prepare = _prepare_scene_authority;
	scene.commit = _commit_scene_authority;
	scene.release = _release_scene_authority;
	scene.publish = _publish_authority_files;
	SolersScriptService::register_authority(SNAME("scene"), scene);

	SolersScriptAuthority asset;
	asset.target_argument = SNAME("asset_path");
	asset.prepare = _prepare_asset_authority;
	asset.commit = _commit_asset_authority;
	asset.release = [](const Ref<SolersScriptContext> &) {};
	asset.publish = _publish_authority_files;
	SolersScriptService::register_authority(SNAME("asset"), asset);
}

void solers_script_authorities_uninitialize() {
	SolersScriptService::clear_authorities();
}
