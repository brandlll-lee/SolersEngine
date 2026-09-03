/**************************************************************************/
/*  solers_builtin_tools.cpp                                              */
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

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"

#include "modules/solers_ai/core/solers_builtin_skills.h"
#include "modules/solers_ai/core/solers_path_utils.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

void SolersToolRegistry::_register_builtin_tools() {
	_add("read", "Read one UTF-8 project file or a registered built-in skill. Use skill://<name> for skills.", R"({"type":"object","properties":{"path":{"type":"string","minLength":1},"line_start":{"type":"integer","minimum":1},"line_count":{"type":"integer","minimum":1,"maximum":2000}},"required":["path"],"additionalProperties":false})", [this](const SolersToolContext &ctx, const Dictionary &a) {
		const String requested_path = String(a.get("path", String())).strip_edges();
		String content;
		String description;
		String display_name;
		if (requested_path.begins_with("skill://")) {
			SolersBuiltinSkillView skill;
			if (!SolersBuiltinSkills::find_by_name(requested_path.trim_prefix("skill://"), skill)) {
				return _error("UNKNOWN_SKILL", vformat("Unknown built-in skill: %s", requested_path.trim_prefix("skill://")));
			}
			content = skill.content;
			description = skill.description;
			display_name = skill.name;
		} else {
			const SolersPath::NormalizedPath normalized = SolersPath::normalize_project_path(requested_path, true);
			if (!normalized.valid) {
				return _error("INVALID_PATH", normalized.error);
			}
			if (DirAccess::exists(normalized.value)) {
				return _error("PATH_IS_DIRECTORY", vformat("Path is a directory: %s", normalized.value));
			}
			Error read_error = OK;
			content = FileAccess::get_file_as_string(normalized.value, &read_error);
			if (read_error != OK) {
				return _error("FILE_READ_FAILED", vformat("Failed to read '%s' (error %d).", normalized.value, read_error));
			}
			display_name = normalized.value;
		}

		const PackedStringArray lines = content.split("\n", true);
		const int first = MIN(MAX((int)a.get("line_start", 1) - 1, 0), lines.size());
		const int line_count = MIN((int)a.get("line_count", 400), 2000);
		String selected;
		for (int i = first; i < MIN(first + line_count, lines.size()); i++) {
			if (!selected.is_empty()) {
				selected += "\n";
			}
			selected += lines[i];
		}
		selected = SolersContextManager::clamp_to_tokens(selected, ctx.result_token_budget);
		Dictionary data;
		data["path"] = requested_path;
		data["name"] = display_name;
		data["description"] = description;
		data["content"] = selected;
		data["line_start"] = first + 1;
		data["line_end"] = MIN(first + line_count, lines.size());
		data["eof"] = first + line_count >= lines.size();
		if (!requested_path.begins_with("skill://")) {
			data["sha256"] = FileAccess::get_sha256(SolersPath::normalize_project_path(requested_path, true).value);
		}
		return _ok(data);
	});
}

void SolersToolRegistry::register_default_tools() {
	_clear_tools();
	_register_builtin_tools();
	_register_observation_tools();
	_register_reflection_tools();
	_register_script_tools();
	_register_runtime_tools();
	_rebuild_tool_catalog();
}
