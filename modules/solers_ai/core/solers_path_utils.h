/**************************************************************************/
/*  solers_path_utils.h                                                   */
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

#include "core/config/project_settings.h"
#include "core/string/ustring.h"

namespace SolersPath {

struct NormalizedPath {
	String value;
	String error;
	bool valid = false;
};

inline String project_data_path() {
	const String resource_prefix = "res://";
	const ProjectSettings *settings = ProjectSettings::get_singleton();
	String path = settings ? settings->get_project_data_path().simplify_path() : String();
	if (path.is_empty() || path == resource_prefix) {
		path = resource_prefix + ".godot";
	}
	return path;
}

inline NormalizedPath normalize_project_path(const String &p_path, bool p_allow_project_data = false) {
	NormalizedPath result;
	const String resource_prefix = "res://";
	String path = p_path.strip_edges().replace_char('\\', '/');
	if (path.is_empty()) {
		result.error = "Path is empty.";
		return result;
	}
	if (path.is_absolute_path() && !path.begins_with(resource_prefix)) {
		result.error = "Only res:// or project-relative paths are allowed.";
		return result;
	}
	if (!path.begins_with(resource_prefix)) {
		path = resource_prefix.path_join(path);
	}

	path = path.simplify_path();
	if (!path.begins_with(resource_prefix)) {
		result.error = "Path escapes the project root.";
		return result;
	}
	const Vector<String> components = path.substr(resource_prefix.length()).split("/", false);
	for (const String &component : components) {
		if (component == "..") {
			result.error = "Path escapes the project root.";
			return result;
		}
		if (component == ".git") {
			result.error = "Refusing to operate on .git metadata.";
			return result;
		}
	}

	const String project_data_path = SolersPath::project_data_path();
	if (!p_allow_project_data && (path == project_data_path || path.begins_with(project_data_path + "/"))) {
		result.error = "Refusing to edit Godot project data directly.";
		return result;
	}

	result.value = path;
	result.valid = true;
	return result;
}

inline bool is_same_or_child(const String &p_path, const String &p_parent) {
	const String path = p_path.simplify_path();
	const String parent = p_parent.simplify_path();
	return !path.is_empty() && !parent.is_empty() && (path == parent || path.begins_with(parent + "/"));
}

} // namespace SolersPath
