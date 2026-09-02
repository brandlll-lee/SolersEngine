/**************************************************************************/
/*  solers_tool_registry.cpp                                              */
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
#include "modules/solers_ai/core/solers_builtin_skills.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

void SolersToolRegistry::_register_skill_tools() {
	_add_observe_exposed("skill.read", "Read one built-in Solers skill by exact name. Skills teach how to use existing native tools; they do not execute work.", R"({"type":"object","properties":{"name":{"type":"string","description":"Built-in skill name from the system skill catalog."}},"required":["name"]})", SolersToolExposure::MODEL, [this](const SolersToolContext &, const Dictionary &a) {
				const String name = String(a.get("name", String())).strip_edges();
				if (name.is_empty()) {
					return _error("INVALID_ARGUMENT", "name is required.");
				}
				SolersBuiltinSkillView skill;
				if (!SolersBuiltinSkills::find_by_name(name, skill)) {
					return _error("UNKNOWN_SKILL", vformat("Unknown built-in skill: %s", name));
				}
				Dictionary data;
				data["name"] = skill.name;
				data["description"] = skill.description;
				data["content"] = skill.content;
				Array tools;
				for (const String &tool : skill.tools) {
					tools.push_back(tool);
				}
				return _with_added_tools(_ok(data), tools); }, {}, {}, {}, SolersToolUiKind::READ, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY, PackedStringArray({ "/name" }));
}

void SolersToolRegistry::register_default_tools() {
	_clear_tools();
	_add("history.revert", "Revert the latest reversible Agent mutation when its native UndoRedo version or file hashes still match.", R"({"type":"object","properties":{"reversal_id":{"type":"string","minLength":1}},"required":["reversal_id"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_SCENE, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DEFERRED, [this](const SolersToolContext &ctx, const Dictionary &a) { return _revert_latest(ctx, a); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &) {
			Array accesses;
			Dictionary access;
			access["mode"] = "write";
			access["key"] = "*";
			accesses.push_back(access);
			return accesses; }, {}, {}, {}, [this](const Dictionary &a) {
			const Dictionary *record = _find_reversal(String(a.get("reversal_id", String())));
			return record && _mutation_record_has_domain(*record, "files") ? SolersPermissionManager::PERMISSION_EDIT_FILES : SolersPermissionManager::PERMISSION_EDIT_SCENE; }, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::EDITOR, SolersOperationMode::APPLY);
	_register_skill_tools();
	_register_reflection_tools();
	_register_observation_tools();
	_register_script_tools();
	_register_runtime_tools();
	_register_asset_tools();
	_register_addon_tools();
	_rebuild_tool_catalog();
}
