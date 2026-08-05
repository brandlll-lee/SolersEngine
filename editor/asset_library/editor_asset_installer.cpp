/**************************************************************************/
/*  editor_asset_installer.cpp                                            */
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

#include "editor_asset_installer.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/zip_io.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/file_system/editor_paths.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_toaster.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/link_button.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"

static Dictionary _package_error(const String &p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = true;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

static Dictionary _package_ok(const Variant &p_data) {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

static bool _package_safe_relative_path(const String &p_path, String &r_path) {
	String path = p_path.replace_char('\\', '/').strip_edges();
	while (path.begins_with("./")) {
		path = path.substr(2);
	}
	if (path.is_empty() || path.begins_with("/") || path.contains(":")) {
		return false;
	}
	const bool directory = path.ends_with("/");
	if (directory) {
		path = path.trim_suffix("/");
	}
	path = path.simplify_path();
	if (path.is_empty() || path == "." || path == ".." || path.begins_with("../") || path.contains("/../")) {
		return false;
	}
	r_path = directory ? path + "/" : path;
	return true;
}

static bool _package_safe_project_path(const String &p_target_dir, const String &p_relative, String &r_res_path, String &r_absolute_path) {
	String target_dir = p_target_dir.replace_char('\\', '/').simplify_path();
	if (!target_dir.begins_with("res://")) {
		return false;
	}
	String relative;
	if (!_package_safe_relative_path(p_relative, relative)) {
		return false;
	}
	relative = relative.trim_suffix("/");
	const String joined = target_dir.path_join(relative).simplify_path();
	const String base = target_dir.trim_suffix("/");
	if (joined != base && !joined.begins_with(base + "/")) {
		return false;
	}
	r_res_path = joined;
	r_absolute_path = ProjectSettings::get_singleton()->globalize_path(joined).simplify_path();
	return true;
}

static bool _package_path_uses_link(const String &p_absolute_path) {
	Ref<DirAccess> fs = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (fs.is_null()) {
		return true;
	}
	String current = p_absolute_path.get_base_dir();
	while (!current.is_empty()) {
		if (DirAccess::exists(current) && fs->is_link(current)) {
			return true;
		}
		const String parent = current.get_base_dir();
		if (parent == current) {
			break;
		}
		current = parent;
	}
	return false;
}

static void _package_remove_tree(const String &p_path) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_valid()) {
		dir->erase_contents_recursive();
	}
	DirAccess::remove_absolute(p_path);
}

static bool _package_is_executable(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	return extension == "gd" || extension == "cs" || extension == "gdextension" || extension == "gdnlib" || extension == "dll" || extension == "so" || extension == "dylib" || extension == "wasm" || extension == "exe" || p_path.get_file() == "plugin.cfg";
}

Dictionary EditorAssetPackageInstaller::inspect_package(const String &p_package_path, const String &p_target_dir, bool p_skip_toplevel) {
	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	unzFile package = unzOpen2(p_package_path.utf8().get_data(), &io);
	if (!package) {
		return _package_error("PACKAGE_INVALID", "The asset package is not a readable ZIP archive.");
	}

	struct Entry {
		String source;
		uint64_t size = 0;
		bool directory = false;
	};
	Vector<Entry> entries;
	HashSet<String> normalized_paths;
	uint64_t total_size = 0;
	String common_root;
	bool common_root_valid = true;
	int ret = unzGoToFirstFile(package);
	while (ret == UNZ_OK) {
		unz_file_info info;
		char filename[16384];
		ret = unzGetCurrentFileInfo(package, &info, filename, sizeof(filename), nullptr, 0, nullptr, 0);
		if (ret != UNZ_OK) {
			unzClose(package);
			return _package_error("PACKAGE_INVALID", "The asset package directory could not be read.");
		}
		String source;
		if (!_package_safe_relative_path(String::utf8(filename), source)) {
			unzClose(package);
			return _package_error("PACKAGE_PATH_INVALID", vformat("Unsafe archive path: %s", String::utf8(filename)));
		}
		const String duplicate_key = source.to_lower();
		if (normalized_paths.has(duplicate_key)) {
			unzClose(package);
			return _package_error("PACKAGE_PATH_DUPLICATE", vformat("Duplicate archive path: %s", source));
		}
		normalized_paths.insert(duplicate_key);
		Entry entry;
		entry.source = source;
		entry.directory = source.ends_with("/");
		entry.size = entry.directory ? 0 : info.uncompressed_size;
		// ponytail: plugin packages are capped to bound ZIP-bomb memory; raise only
		// when a verified real plugin exceeds these limits.
		if (entry.size > 256ULL * 1024ULL * 1024ULL || total_size + entry.size > 1024ULL * 1024ULL * 1024ULL) {
			unzClose(package);
			return _package_error("PACKAGE_TOO_LARGE", "The asset package exceeds the safe extraction limit.");
		}
		total_size += entry.size;
		entries.push_back(entry);
		const String root = source.get_slice("/", 0);
		if (common_root.is_empty()) {
			common_root = root;
		} else if (root != common_root) {
			common_root_valid = false;
		}
		ret = unzGoToNextFile(package);
	}
	unzClose(package);

	if (entries.is_empty()) {
		return _package_error("PACKAGE_EMPTY", "The asset package contains no files.");
	}
	const String prefix = p_skip_toplevel && common_root_valid ? common_root + "/" : String();
	Array files;
	Array conflicts;
	bool contains_executable_code = false;
	HashSet<String> mapped_paths;
	for (const Entry &entry : entries) {
		String relative = entry.source;
		if (!prefix.is_empty()) {
			if (relative == common_root + "/") {
				continue;
			}
			relative = relative.trim_prefix(prefix);
		}
		String target_res;
		String target_absolute;
		if (!_package_safe_project_path(p_target_dir, relative, target_res, target_absolute)) {
			return _package_error("PACKAGE_PATH_INVALID", vformat("Unsafe install target for %s", entry.source));
		}
		const String mapped_key = target_res.to_lower();
		if (mapped_paths.has(mapped_key)) {
			return _package_error("PACKAGE_PATH_DUPLICATE", vformat("Multiple archive entries map to %s", target_res));
		}
		mapped_paths.insert(mapped_key);
		const bool executable = !entry.directory && _package_is_executable(relative);
		contains_executable_code |= executable;
		Dictionary file;
		file["source_path"] = entry.source;
		file["relative_path"] = relative;
		file["target_path"] = target_res;
		file["size"] = (int64_t)entry.size;
		file["directory"] = entry.directory;
		file["executable"] = executable;
		file["conflict"] = FileAccess::exists(target_res) || DirAccess::exists(target_res);
		if ((bool)file["conflict"] && !entry.directory) {
			conflicts.push_back(target_res);
		}
		files.push_back(file);
	}
	Dictionary data;
	data["files"] = files;
	data["conflicts"] = conflicts;
	data["total_uncompressed_size"] = (int64_t)total_size;
	data["toplevel_prefix"] = prefix;
	data["contains_executable_code"] = contains_executable_code;
	return _package_ok(data);
}

Dictionary EditorAssetPackageInstaller::install_package(const String &p_package_path, const String &p_target_dir, const Dictionary &p_mapped_files, const PackedStringArray &p_selected_files, const PackedStringArray &p_overwrite_files, const PackedStringArray &p_remove_files, const String &p_manifest_path, const Dictionary &p_manifest) {
	if (p_selected_files.is_empty()) {
		return _package_error("PACKAGE_SELECTION_EMPTY", "No package files were selected for installation.");
	}
	HashSet<String> selected;
	for (const String &source : p_selected_files) {
		selected.insert(source);
	}
	HashSet<String> allowed_overwrites;
	for (const String &path : p_overwrite_files) {
		allowed_overwrites.insert(path.replace_char('\\', '/').simplify_path().to_lower());
	}

	struct InstallFile {
		String source;
		String target_res;
		String target_absolute;
		String staged_absolute;
		String backup_absolute;
		uint64_t size = 0;
		bool had_backup = false;
		bool committed = false;
		bool removal_only = false;
	};
	Vector<InstallFile> files;
	const String transaction_root = EditorPaths::get_singleton()->get_project_settings_dir().path_join("solers_package_install_" + itos(OS::get_singleton()->get_ticks_usec()));
	const String stage_root = transaction_root.path_join("stage");
	const String backup_root = transaction_root.path_join("backup");
	if (DirAccess::make_dir_recursive_absolute(stage_root) != OK || DirAccess::make_dir_recursive_absolute(backup_root) != OK) {
		_package_remove_tree(transaction_root);
		return _package_error("PACKAGE_STAGE_FAILED", "Could not create the package transaction directory.");
	}

	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	unzFile package = unzOpen2(p_package_path.utf8().get_data(), &io);
	if (!package) {
		_package_remove_tree(transaction_root);
		return _package_error("PACKAGE_INVALID", "The asset package is not a readable ZIP archive.");
	}
	String failure;
	uint64_t total_size = 0;
	HashSet<String> install_targets;
	int ret = unzGoToFirstFile(package);
	while (ret == UNZ_OK && failure.is_empty()) {
		unz_file_info info;
		char filename[16384];
		ret = unzGetCurrentFileInfo(package, &info, filename, sizeof(filename), nullptr, 0, nullptr, 0);
		if (ret != UNZ_OK) {
			failure = "The asset package directory could not be read.";
			break;
		}
		String source;
		if (!_package_safe_relative_path(String::utf8(filename), source)) {
			failure = vformat("Unsafe archive path: %s", String::utf8(filename));
			break;
		}
		if (!selected.has(source) || source.ends_with("/")) {
			ret = unzGoToNextFile(package);
			continue;
		}
		if (!p_mapped_files.has(source)) {
			failure = vformat("No install mapping exists for %s", source);
			break;
		}
		String target_res;
		String target_absolute;
		if (!_package_safe_project_path(p_target_dir, p_mapped_files[source], target_res, target_absolute) || _package_path_uses_link(target_absolute)) {
			failure = vformat("Unsafe install target for %s", source);
			break;
		}
		if (FileAccess::exists(target_absolute) && !allowed_overwrites.has(target_res.to_lower())) {
			failure = vformat("Install target already exists: %s", target_res);
			break;
		}
		if (info.uncompressed_size > 256ULL * 1024ULL * 1024ULL) {
			failure = vformat("Package file exceeds the safe extraction limit: %s", source);
			break;
		}
		total_size += info.uncompressed_size;
		if (total_size > 1024ULL * 1024ULL * 1024ULL) {
			failure = "The selected package files exceed the safe extraction limit.";
			break;
		}
		const String target_key = target_res.to_lower();
		if (install_targets.has(target_key)) {
			failure = vformat("Multiple package files map to %s", target_res);
			break;
		}
		install_targets.insert(target_key);
		InstallFile install_file;
		install_file.source = source;
		install_file.target_res = target_res;
		install_file.target_absolute = target_absolute;
		install_file.staged_absolute = stage_root.path_join(target_res.trim_prefix("res://"));
		install_file.backup_absolute = backup_root.path_join(target_res.trim_prefix("res://"));
		install_file.size = info.uncompressed_size;
		if (DirAccess::make_dir_recursive_absolute(install_file.staged_absolute.get_base_dir()) != OK) {
			failure = vformat("Could not create staging directory for %s", source);
			break;
		}
		if (unzOpenCurrentFile(package) != UNZ_OK) {
			failure = vformat("Could not open %s in the package", source);
			break;
		}
		Ref<FileAccess> output = FileAccess::open(install_file.staged_absolute, FileAccess::WRITE);
		if (output.is_null()) {
			unzCloseCurrentFile(package);
			failure = vformat("Could not stage %s", source);
			break;
		}
		PackedByteArray buffer;
		buffer.resize(64 * 1024);
		uint64_t extracted = 0;
		while (extracted < info.uncompressed_size) {
			const int requested = (int)MIN((uint64_t)buffer.size(), (uint64_t)info.uncompressed_size - extracted);
			const int read = unzReadCurrentFile(package, buffer.ptrw(), requested);
			if (read <= 0) {
				failure = vformat("Could not extract %s from the package", source);
				break;
			}
			output->store_buffer(buffer.ptr(), read);
			extracted += read;
		}
		if (unzCloseCurrentFile(package) != UNZ_OK || extracted != info.uncompressed_size) {
			failure = vformat("Could not extract %s from the package", source);
			break;
		}
		files.push_back(install_file);
		ret = unzGoToNextFile(package);
	}
	unzClose(package);
	if (failure.is_empty() && files.size() != selected.size()) {
		failure = "One or more selected package files were not found in the archive.";
	}
	if (failure.is_empty()) {
		for (const String &remove_path : p_remove_files) {
			String target_res;
			String target_absolute;
			if (!_package_safe_project_path("res://", remove_path.trim_prefix("res://"), target_res, target_absolute) || _package_path_uses_link(target_absolute) || install_targets.has(target_res.to_lower())) {
				failure = vformat("Unsafe package removal target: %s", remove_path);
				break;
			}
			InstallFile remove_file;
			remove_file.source = "<removed>";
			remove_file.target_res = target_res;
			remove_file.target_absolute = target_absolute;
			remove_file.backup_absolute = backup_root.path_join(target_res.trim_prefix("res://"));
			remove_file.removal_only = true;
			files.push_back(remove_file);
		}
	}

	if (failure.is_empty() && !p_manifest_path.is_empty()) {
		String manifest_res;
		String manifest_absolute;
		if (!_package_safe_project_path("res://", p_manifest_path.trim_prefix("res://"), manifest_res, manifest_absolute) || _package_path_uses_link(manifest_absolute)) {
			failure = "The package manifest path is unsafe.";
		} else {
			InstallFile manifest_file;
			manifest_file.source = "<generated-manifest>";
			manifest_file.target_res = manifest_res;
			manifest_file.target_absolute = manifest_absolute;
			manifest_file.staged_absolute = stage_root.path_join(manifest_res.trim_prefix("res://"));
			manifest_file.backup_absolute = backup_root.path_join(manifest_res.trim_prefix("res://"));
			DirAccess::make_dir_recursive_absolute(manifest_file.staged_absolute.get_base_dir());
			Ref<FileAccess> output = FileAccess::open(manifest_file.staged_absolute, FileAccess::WRITE);
			if (output.is_null()) {
				failure = "Could not stage the package manifest.";
			} else {
				output->store_string(JSON::stringify(p_manifest, "  "));
				files.push_back(manifest_file);
			}
		}
	}

	if (failure.is_empty()) {
		for (InstallFile &file : files) {
			if (FileAccess::exists(file.target_absolute)) {
				file.had_backup = true;
				if (DirAccess::make_dir_recursive_absolute(file.backup_absolute.get_base_dir()) != OK || DirAccess::rename_absolute(file.target_absolute, file.backup_absolute) != OK) {
					failure = vformat("Could not back up %s", file.target_res);
					break;
				}
			}
			if (file.removal_only) {
				file.committed = true;
				continue;
			}
			if (DirAccess::make_dir_recursive_absolute(file.target_absolute.get_base_dir()) != OK || DirAccess::rename_absolute(file.staged_absolute, file.target_absolute) != OK) {
				failure = vformat("Could not install %s", file.target_res);
				if (file.had_backup) {
					DirAccess::rename_absolute(file.backup_absolute, file.target_absolute);
					file.had_backup = false;
				}
				break;
			}
			file.committed = true;
		}
	}

	if (!failure.is_empty()) {
		for (int i = files.size() - 1; i >= 0; i--) {
			InstallFile &file = files.write[i];
			if (file.committed) {
				DirAccess::remove_absolute(file.target_absolute);
			}
			if (file.had_backup) {
				DirAccess::make_dir_recursive_absolute(file.target_absolute.get_base_dir());
				DirAccess::rename_absolute(file.backup_absolute, file.target_absolute);
			}
		}
		_package_remove_tree(transaction_root);
		return _package_error("PACKAGE_INSTALL_FAILED", failure);
	}

	Array installed;
	Array removed;
	for (const InstallFile &file : files) {
		if (file.removal_only) {
			removed.push_back(file.target_res);
		} else {
			installed.push_back(file.target_res);
		}
	}
	_package_remove_tree(transaction_root);
	Dictionary data;
	data["installed_files"] = installed;
	data["removed_files"] = removed;
	data["count"] = installed.size() + removed.size();
	return _package_ok(data);
}

void EditorAssetInstaller::_item_checked_cbk() {
	if (updating_source || !source_tree->get_edited()) {
		return;
	}

	updating_source = true;
	TreeItem *item = source_tree->get_edited();
	item->propagate_check(0);
	_fix_conflicted_indeterminate_state(source_tree->get_root(), 0);
	_update_confirm_button();
	_rebuild_destination_tree();
	updating_source = false;
}

// Determine parent state based on non-conflict children, to avoid indeterminate state, and allow toggle dir with conflicts.
bool EditorAssetInstaller::_fix_conflicted_indeterminate_state(TreeItem *p_item, int p_column) {
	if (p_item->get_child_count() == 0) {
		return false;
	}
	bool all_non_conflict_checked = true;
	bool all_non_conflict_unchecked = true;
	bool has_conflict_child = false;
	bool has_indeterminate_child = false;
	TreeItem *child_item = p_item->get_first_child();
	while (child_item) {
		has_conflict_child |= _fix_conflicted_indeterminate_state(child_item, p_column);
		Dictionary child_meta = child_item->get_metadata(p_column);
		bool child_conflict = child_meta.get("is_conflict", false);
		if (child_conflict) {
			child_item->set_checked(p_column, false);
			has_conflict_child = true;
		} else {
			bool child_checked = child_item->is_checked(p_column);
			bool child_indeterminate = child_item->is_indeterminate(p_column);
			all_non_conflict_checked &= (child_checked || child_indeterminate);
			all_non_conflict_unchecked &= !child_checked;
			has_indeterminate_child |= child_indeterminate;
		}
		child_item = child_item->get_next();
	}
	if (has_indeterminate_child) {
		p_item->set_indeterminate(p_column, true);
	} else if (all_non_conflict_checked) {
		p_item->set_checked(p_column, true);
	} else if (all_non_conflict_unchecked) {
		p_item->set_checked(p_column, false);
	}
	if (has_conflict_child) {
		p_item->set_custom_color(p_column, get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
	} else {
		p_item->clear_custom_color(p_column);
	}
	return has_conflict_child;
}

bool EditorAssetInstaller::_is_item_checked(const String &p_source_path) const {
	return file_item_map.has(p_source_path) && (file_item_map[p_source_path]->is_checked(0) || file_item_map[p_source_path]->is_indeterminate(0));
}

void EditorAssetInstaller::open_asset(const String &p_path, bool p_autoskip_toplevel) {
	package_path = p_path;
	asset_files.clear();

	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);

	unzFile pkg = unzOpen2(p_path.utf8().get_data(), &io);
	if (!pkg) {
		EditorToaster::get_singleton()->popup_str(vformat(TTR("Error opening asset file for \"%s\" (not in ZIP format)."), asset_name), EditorToaster::SEVERITY_ERROR);
		return;
	}

	int ret = unzGoToFirstFile(pkg);

	while (ret == UNZ_OK) {
		//get filename
		unz_file_info info;
		char fname[16384];
		unzGetCurrentFileInfo(pkg, &info, fname, 16384, nullptr, 0, nullptr, 0);

		String source_name = String::utf8(fname);

		// Create intermediate directories if they aren't reported by unzip.
		// We are only interested in subfolders, so skip the root slash.
		int separator = source_name.find_char('/', 1);
		while (separator != -1) {
			String dir_name = source_name.substr(0, separator + 1);
			if (!dir_name.is_empty() && !asset_files.has(dir_name)) {
				asset_files.insert(dir_name);
			}

			separator = source_name.find_char('/', separator + 1);
		}

		if (!source_name.is_empty() && !asset_files.has(source_name)) {
			asset_files.insert(source_name);
		}

		ret = unzGoToNextFile(pkg);
	}

	unzClose(pkg);

	asset_title_label->set_text(asset_name);

	_check_has_toplevel();
	// Default to false, unless forced. Don't skip "addons" by default
	skip_toplevel = p_autoskip_toplevel && toplevel_prefix != "addons/";
	skip_toplevel_check->set_block_signals(true);
	skip_toplevel_check->set_pressed(!skip_toplevel_check->is_disabled() && skip_toplevel);
	skip_toplevel_check->set_block_signals(false);

	_update_file_mappings();
	_rebuild_source_tree();
	_rebuild_destination_tree();

	popup_centered_clamped(Size2(620, 640) * EDSCALE);
}

void EditorAssetInstaller::_update_file_mappings() {
	mapped_files.clear();

	bool first = true;
	for (const String &E : asset_files) {
		if (first) {
			first = false;

			if (!toplevel_prefix.is_empty() && skip_toplevel) {
				continue;
			}
		}

		String path = E; // We're going to mutate it.
		if (!toplevel_prefix.is_empty() && skip_toplevel) {
			path = path.trim_prefix(toplevel_prefix);
		}

		mapped_files[E] = path;
	}
}

void EditorAssetInstaller::_rebuild_source_tree() {
	updating_source = true;
	source_tree->clear();

	TreeItem *root = source_tree->create_item();
	root->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
	root->set_checked(0, true);
	root->set_icon(0, get_theme_icon(SNAME("folder"), SNAME("FileDialog")));
	root->set_text(0, "/");
	root->set_editable(0, true);

	file_item_map.clear();
	HashMap<String, TreeItem *> directory_item_map;
	int num_file_conflicts = 0;
	first_file_conflict = nullptr;

	for (const String &E : asset_files) {
		String path = E; // We're going to mutate it.

		bool is_directory = false;
		if (path.ends_with("/")) {
			path = path.trim_suffix("/");
			is_directory = true;
		}

		TreeItem *parent_item;

		int separator = path.rfind_char('/');
		if (separator == -1) {
			parent_item = root;
		} else {
			String parent_path = path.substr(0, separator);
			HashMap<String, TreeItem *>::Iterator I = directory_item_map.find(parent_path);
			ERR_CONTINUE(!I);
			parent_item = I->value;
		}

		TreeItem *ti;
		if (is_directory) {
			ti = _create_dir_item(source_tree, parent_item, path, directory_item_map);
		} else {
			ti = _create_file_item(source_tree, parent_item, path, &num_file_conflicts);
		}
		file_item_map[E] = ti;
	}

	_update_conflict_status(num_file_conflicts);
	_update_confirm_button();

	updating_source = false;
}

void EditorAssetInstaller::_update_source_tree() {
	int num_file_conflicts = 0;
	first_file_conflict = nullptr;

	for (const KeyValue<String, TreeItem *> &E : file_item_map) {
		TreeItem *ti = E.value;
		Dictionary item_meta = ti->get_metadata(0);
		if ((bool)item_meta.get("is_dir", false)) {
			continue;
		}

		String asset_path = item_meta.get("asset_path", "");
		ERR_CONTINUE(asset_path.is_empty());

		bool target_exists = _update_source_item_status(ti, asset_path);
		if (target_exists) {
			if (first_file_conflict == nullptr) {
				first_file_conflict = ti;
			}
			num_file_conflicts += 1;
		}

		item_meta["is_conflict"] = target_exists;
		ti->set_metadata(0, item_meta);
	}

	_update_conflict_status(num_file_conflicts);
	_update_confirm_button();
}

bool EditorAssetInstaller::_update_source_item_status(TreeItem *p_item, const String &p_path) {
	ERR_FAIL_COND_V(!mapped_files.has(p_path), false);
	String target_path = target_dir_path.path_join(mapped_files[p_path]);

	bool target_exists = FileAccess::exists(target_path);
	if (target_exists) {
		p_item->set_custom_color(0, get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
		p_item->set_tooltip_text(0, vformat(TTR("%s (already exists)"), target_path));
		p_item->set_checked(0, false);
	} else {
		p_item->clear_custom_color(0);
		p_item->set_tooltip_text(0, target_path);
		p_item->set_checked(0, true);
	}

	p_item->propagate_check(0);
	_fix_conflicted_indeterminate_state(p_item->get_tree()->get_root(), 0);
	return target_exists;
}

void EditorAssetInstaller::_rebuild_destination_tree() {
	destination_tree->clear();

	TreeItem *root = destination_tree->create_item();
	root->set_icon(0, get_theme_icon(SNAME("folder"), SNAME("FileDialog")));
	root->set_text(0, target_dir_path + (target_dir_path == "res://" ? "" : "/"));

	HashMap<String, TreeItem *> directory_item_map;

	for (const KeyValue<String, String> &E : mapped_files) {
		if (!_is_item_checked(E.key)) {
			continue;
		}

		String path = E.value; // We're going to mutate it.

		bool is_directory = false;
		if (path.ends_with("/")) {
			path = path.trim_suffix("/");
			is_directory = true;
		}

		TreeItem *parent_item;

		int separator = path.rfind_char('/');
		if (separator == -1) {
			parent_item = root;
		} else {
			String parent_path = path.substr(0, separator);
			HashMap<String, TreeItem *>::Iterator I = directory_item_map.find(parent_path);
			ERR_CONTINUE(!I);
			parent_item = I->value;
		}

		if (is_directory) {
			_create_dir_item(destination_tree, parent_item, path, directory_item_map);
		} else {
			int num_file_conflicts = 0; // Don't need it, but need to pass something.
			_create_file_item(destination_tree, parent_item, path, &num_file_conflicts);
		}
	}
}

TreeItem *EditorAssetInstaller::_create_dir_item(Tree *p_tree, TreeItem *p_parent, const String &p_path, HashMap<String, TreeItem *> &p_item_map) {
	TreeItem *ti = p_tree->create_item(p_parent);

	if (p_tree == source_tree) {
		ti->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
		ti->set_editable(0, true);
		ti->set_checked(0, true);
		ti->propagate_check(0);
		_fix_conflicted_indeterminate_state(ti->get_tree()->get_root(), 0);

		Dictionary meta;
		meta["asset_path"] = p_path + "/";
		meta["is_dir"] = true;
		meta["is_conflict"] = false;
		ti->set_metadata(0, meta);
	}

	ti->set_text(0, p_path.get_file() + "/");
	ti->set_icon(0, get_theme_icon(SNAME("folder"), SNAME("FileDialog")));

	p_item_map[p_path] = ti;
	return ti;
}

TreeItem *EditorAssetInstaller::_create_file_item(Tree *p_tree, TreeItem *p_parent, const String &p_path, int *r_conflicts) {
	TreeItem *ti = p_tree->create_item(p_parent);

	if (p_tree == source_tree) {
		ti->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
		ti->set_editable(0, true);

		bool target_exists = _update_source_item_status(ti, p_path);
		if (target_exists) {
			if (first_file_conflict == nullptr) {
				first_file_conflict = ti;
			}
			*r_conflicts += 1;
		}

		Dictionary meta;
		meta["asset_path"] = p_path;
		meta["is_dir"] = false;
		meta["is_conflict"] = target_exists;
		ti->set_metadata(0, meta);
	}

	String file = p_path.get_file();
	String extension = file.get_extension().to_lower();
	if (extension_icon_map.has(extension)) {
		ti->set_icon(0, extension_icon_map[extension]);
	} else {
		ti->set_icon(0, generic_extension_icon);
	}
	ti->set_text(0, file);

	return ti;
}

void EditorAssetInstaller::_update_conflict_status(int p_conflicts) {
	if (p_conflicts >= 1) {
		asset_conflicts_link->set_text(vformat(TTRN("%d file conflicts with your project and won't be installed", "%d files conflict with your project and won't be installed", p_conflicts), p_conflicts));
		asset_conflicts_link->show();
		asset_conflicts_label->hide();
	} else {
		asset_conflicts_link->hide();
		asset_conflicts_label->show();
	}
}

void EditorAssetInstaller::_update_confirm_button() {
	TreeItem *root = source_tree->get_root();
	get_ok_button()->set_disabled(!root || (!root->is_checked(0) && !root->is_indeterminate(0)));
}

void EditorAssetInstaller::_toggle_source_tree(bool p_visible, bool p_scroll_to_error) {
	source_tree_vb->set_visible(p_visible);
	show_source_files_button->set_pressed_no_signal(p_visible); // To keep in sync if triggered by something else.

	if (p_visible) {
		show_source_files_button->set_button_icon(get_editor_theme_icon(SNAME("Back")));
		destination_tree_mc->set_theme_type_variation("");
		destination_tree->set_theme_type_variation("TreeSecondary");
		destination_tree->set_scroll_hint_mode(Tree::SCROLL_HINT_MODE_DISABLED);
	} else {
		show_source_files_button->set_button_icon(get_editor_theme_icon(SNAME("Forward")));
		destination_tree_mc->set_theme_type_variation("NoBorderHorizontalWindow");
		destination_tree->set_theme_type_variation("");
		destination_tree->set_scroll_hint_mode(Tree::SCROLL_HINT_MODE_BOTH);
	}

	if (p_visible && p_scroll_to_error && first_file_conflict) {
		source_tree->scroll_to_item(first_file_conflict, true);
	}
}

void EditorAssetInstaller::_check_has_toplevel() {
	// Check if the file structure has a distinct top-level directory. This is typical
	// for archives generated by GitHub, etc, but not for manually created ZIPs.

	toplevel_prefix = "";
	skip_toplevel_check->set_pressed(false);
	skip_toplevel_check->set_disabled(true);
	skip_toplevel_check->set_tooltip_text(TTRC("This asset doesn't have a root directory, so it can't be ignored."));

	if (asset_files.is_empty()) {
		return;
	}

	String first_asset;
	for (const String &E : asset_files) {
		if (first_asset.is_empty()) { // Checking the first file/directory.
			if (!E.ends_with("/")) {
				return; // No directories in this asset.
			}

			// We will match everything else against this directory.
			first_asset = E;
			continue;
		}

		if (!E.begins_with(first_asset)) {
			return; // Found a file or a directory that doesn't share the same base path.
		}
	}

	toplevel_prefix = first_asset;
	skip_toplevel_check->set_disabled(false);
	skip_toplevel_check->set_tooltip_text(TTRC("Ignore the root directory when extracting files."));
}

void EditorAssetInstaller::_set_skip_toplevel(bool p_checked) {
	if (skip_toplevel == p_checked) {
		return;
	}

	skip_toplevel = p_checked;
	_update_file_mappings();
	_update_source_tree();
	_rebuild_destination_tree();
}

void EditorAssetInstaller::_open_target_dir_dialog() {
	if (!target_dir_dialog) {
		target_dir_dialog = memnew(EditorFileDialog);
		target_dir_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
		target_dir_dialog->set_title(TTRC("Select Install Folder"));
		target_dir_dialog->set_current_dir(target_dir_path);
		target_dir_dialog->connect("dir_selected", callable_mp(this, &EditorAssetInstaller::_target_dir_selected));
		add_child(target_dir_dialog);
	}

	target_dir_dialog->popup_file_dialog();
}

void EditorAssetInstaller::_target_dir_selected(const String &p_target_path) {
	if (target_dir_path == p_target_path) {
		return;
	}

	target_dir_path = p_target_path;
	_update_file_mappings();
	_update_source_tree();
	_rebuild_destination_tree();
}

void EditorAssetInstaller::ok_pressed() {
	_install_asset();
}

void EditorAssetInstaller::_install_asset() {
	Dictionary mappings;
	PackedStringArray selected;
	PackedStringArray overwrites;
	for (const String &source : asset_files) {
		if (source.ends_with("/") || !_is_item_checked(source) || !mapped_files.has(source)) {
			continue;
		}
		mappings[source] = mapped_files[source];
		selected.push_back(source);
		const String target = target_dir_path.path_join(mapped_files[source]).simplify_path();
		if (FileAccess::exists(target)) {
			overwrites.push_back(target);
		}
	}
	const Dictionary result = EditorAssetPackageInstaller::install_package(package_path, target_dir_path, mappings, selected, overwrites);
	if (!(bool)result.get("ok", false)) {
		const Dictionary error = result.get("error", Dictionary());
		EditorNode::get_singleton()->show_warning(vformat(TTR("Asset \"%s\" could not be installed:\n\n%s"), asset_name, error.get("message", TTR("Unknown package error."))));
		return;
	}
	EditorNode::get_singleton()->show_warning(vformat(TTR("Asset \"%s\" installed successfully!"), asset_name), TTRC("Success!"));

	EditorFileSystem::get_singleton()->scan_changes();
}

void EditorAssetInstaller::set_asset_name(const String &p_asset_name) {
	asset_name = p_asset_name;
}

String EditorAssetInstaller::get_asset_name() const {
	return asset_name;
}

void EditorAssetInstaller::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			if (show_source_files_button->is_pressed()) {
				show_source_files_button->set_button_icon(get_editor_theme_icon(SNAME("Back")));
			} else {
				show_source_files_button->set_button_icon(get_editor_theme_icon(SNAME("Forward")));
			}
			asset_conflicts_link->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("error_color"), EditorStringName(Editor)));

			generic_extension_icon = get_editor_theme_icon(SNAME("Object"));

			extension_icon_map.clear();
			{
				extension_icon_map["bmp"] = get_editor_theme_icon(SNAME("ImageTexture"));
				extension_icon_map["dds"] = get_editor_theme_icon(SNAME("ImageTexture"));
				extension_icon_map["exr"] = get_editor_theme_icon(SNAME("ImageTexture"));
				extension_icon_map["hdr"] = get_editor_theme_icon(SNAME("ImageTexture"));
				extension_icon_map["jpg"] = get_editor_theme_icon(SNAME("ImageTexture"));
				extension_icon_map["jpeg"] = get_editor_theme_icon(SNAME("ImageTexture"));
				extension_icon_map["png"] = get_editor_theme_icon(SNAME("ImageTexture"));
				extension_icon_map["svg"] = get_editor_theme_icon(SNAME("ImageTexture"));
				extension_icon_map["tga"] = get_editor_theme_icon(SNAME("ImageTexture"));
				extension_icon_map["webp"] = get_editor_theme_icon(SNAME("ImageTexture"));

				extension_icon_map["wav"] = get_editor_theme_icon(SNAME("AudioStreamWAV"));
				extension_icon_map["ogg"] = get_editor_theme_icon(SNAME("AudioStreamOggVorbis"));
				extension_icon_map["mp3"] = get_editor_theme_icon(SNAME("AudioStreamMP3"));

				extension_icon_map["scn"] = get_editor_theme_icon(SNAME("PackedScene"));
				extension_icon_map["tscn"] = get_editor_theme_icon(SNAME("PackedScene"));
				extension_icon_map["escn"] = get_editor_theme_icon(SNAME("PackedScene"));
				extension_icon_map["dae"] = get_editor_theme_icon(SNAME("PackedScene"));
				extension_icon_map["gltf"] = get_editor_theme_icon(SNAME("PackedScene"));
				extension_icon_map["glb"] = get_editor_theme_icon(SNAME("PackedScene"));

				extension_icon_map["gdshader"] = get_editor_theme_icon(SNAME("Shader"));
				extension_icon_map["gdshaderinc"] = get_editor_theme_icon(SNAME("TextFile"));
				extension_icon_map["gd"] = get_editor_theme_icon(SNAME("GDScript"));
				if (ClassDB::class_exists("CSharpScript")) {
					extension_icon_map["cs"] = get_editor_theme_icon(SNAME("CSharpScript"));
				} else {
					// Mark C# support as unavailable.
					extension_icon_map["cs"] = get_editor_theme_icon(SNAME("ImportFail"));
				}

				extension_icon_map["res"] = get_editor_theme_icon(SNAME("Resource"));
				extension_icon_map["tres"] = get_editor_theme_icon(SNAME("Resource"));
				extension_icon_map["atlastex"] = get_editor_theme_icon(SNAME("AtlasTexture"));
				// By default, OBJ files are imported as Mesh resources rather than PackedScenes.
				extension_icon_map["obj"] = get_editor_theme_icon(SNAME("MeshItem"));

				extension_icon_map["txt"] = get_editor_theme_icon(SNAME("TextFile"));
				extension_icon_map["md"] = get_editor_theme_icon(SNAME("TextFile"));
				extension_icon_map["rst"] = get_editor_theme_icon(SNAME("TextFile"));
				extension_icon_map["json"] = get_editor_theme_icon(SNAME("TextFile"));
				extension_icon_map["yml"] = get_editor_theme_icon(SNAME("TextFile"));
				extension_icon_map["yaml"] = get_editor_theme_icon(SNAME("TextFile"));
				extension_icon_map["toml"] = get_editor_theme_icon(SNAME("TextFile"));
				extension_icon_map["cfg"] = get_editor_theme_icon(SNAME("TextFile"));
				extension_icon_map["ini"] = get_editor_theme_icon(SNAME("TextFile"));
			}
		} break;
	}
}

EditorAssetInstaller::EditorAssetInstaller() {
	VBoxContainer *vb = memnew(VBoxContainer);
	add_child(vb);

	// Status bar.

	HBoxContainer *asset_status = memnew(HBoxContainer);
	vb->add_child(asset_status);

	Label *asset_label = memnew(Label);
	asset_label->set_text(TTRC("Asset:"));
	asset_label->set_theme_type_variation("HeaderSmall");
	asset_status->add_child(asset_label);

	asset_title_label = memnew(Label);
	asset_title_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	asset_status->add_child(asset_title_label);

	// File remapping controls.

	HBoxContainer *remapping_tools = memnew(HBoxContainer);
	vb->add_child(remapping_tools);

	show_source_files_button = memnew(Button);
	show_source_files_button->set_toggle_mode(true);
	show_source_files_button->set_tooltip_text(TTRC("Open the list of the asset contents and select which files to install."));
	remapping_tools->add_child(show_source_files_button);
	show_source_files_button->connect(SceneStringName(toggled), callable_mp(this, &EditorAssetInstaller::_toggle_source_tree).bind(false));

	Button *target_dir_button = memnew(Button);
	target_dir_button->set_text(TTRC("Change Install Folder"));
	target_dir_button->set_tooltip_text(TTRC("Change the folder where the contents of the asset are going to be installed."));
	remapping_tools->add_child(target_dir_button);
	target_dir_button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetInstaller::_open_target_dir_dialog));

	remapping_tools->add_child(memnew(VSeparator));

	skip_toplevel_check = memnew(CheckBox);
	skip_toplevel_check->set_text(TTRC("Ignore Asset Root"));
	skip_toplevel_check->set_tooltip_text(TTRC("Ignore the root directory when extracting files."));
	skip_toplevel_check->connect(SceneStringName(toggled), callable_mp(this, &EditorAssetInstaller::_set_skip_toplevel));
	remapping_tools->add_child(skip_toplevel_check);

	remapping_tools->add_spacer();

	asset_conflicts_label = memnew(Label);
	asset_conflicts_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	asset_conflicts_label->set_theme_type_variation("HeaderSmall");
	asset_conflicts_label->set_text(TTRC("No files conflict with your project."));
	remapping_tools->add_child(asset_conflicts_label);
	asset_conflicts_link = memnew(LinkButton);
	asset_conflicts_link->set_theme_type_variation("HeaderSmallLink");
	asset_conflicts_link->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	asset_conflicts_link->set_tooltip_text(TTRC("Show contents of the asset and conflicting files."));
	asset_conflicts_link->set_visible(false);
	remapping_tools->add_child(asset_conflicts_link);
	asset_conflicts_link->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetInstaller::_toggle_source_tree).bind(true, true));

	// File hierarchy trees.

	HSplitContainer *tree_split = memnew(HSplitContainer);
	tree_split->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	vb->add_child(tree_split);

	source_tree_vb = memnew(VBoxContainer);
	source_tree_vb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	source_tree_vb->set_visible(show_source_files_button->is_pressed());
	tree_split->add_child(source_tree_vb);

	Label *source_tree_label = memnew(Label);
	source_tree_label->set_text(TTRC("Contents of the asset:"));
	source_tree_label->set_theme_type_variation("HeaderSmall");
	source_tree_vb->add_child(source_tree_label);

	source_tree = memnew(Tree);
	source_tree->set_accessibility_name(TTRC("Source Files"));
	source_tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	source_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	source_tree->connect("item_edited", callable_mp(this, &EditorAssetInstaller::_item_checked_cbk));
	source_tree->set_theme_type_variation("TreeSecondary");
	source_tree_vb->add_child(source_tree);

	VBoxContainer *destination_tree_vb = memnew(VBoxContainer);
	destination_tree_vb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	tree_split->add_child(destination_tree_vb);

	Label *destination_tree_label = memnew(Label);
	destination_tree_label->set_text(TTRC("Installation preview:"));
	destination_tree_label->set_theme_type_variation("HeaderSmall");
	destination_tree_vb->add_child(destination_tree_label);

	destination_tree_mc = memnew(MarginContainer);
	destination_tree_mc->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	destination_tree_mc->set_theme_type_variation("NoBorderHorizontalWindow");
	destination_tree_vb->add_child(destination_tree_mc);

	destination_tree = memnew(Tree);
	destination_tree->set_accessibility_name(TTRC("Destination Files"));
	destination_tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	destination_tree->set_scroll_hint_mode(Tree::SCROLL_HINT_MODE_BOTH);
	destination_tree->connect("item_edited", callable_mp(this, &EditorAssetInstaller::_item_checked_cbk));
	destination_tree_mc->add_child(destination_tree);

	// Dialog configuration.

	set_title(TTRC("Configure Asset Before Installing"));
	set_ok_button_text(TTRC("Install"));
	set_hide_on_ok(true);
}
