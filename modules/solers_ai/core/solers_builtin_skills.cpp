/**************************************************************************/
/*  solers_builtin_skills.cpp                                             */
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

#include "solers_builtin_skills.h"

#include "core/variant/variant.h"

#include "modules/solers_ai/generated/solers_builtin_skills.gen.h"

int SolersBuiltinSkills::get_count() {
	return SOLERS_BUILTIN_SKILL_COUNT;
}

bool SolersBuiltinSkills::find_by_name(const String &p_name, SolersBuiltinSkillView &r_skill) {
	const String name = p_name.strip_edges();
	if (name.is_empty()) {
		return false;
	}
	for (int i = 0; i < SOLERS_BUILTIN_SKILL_COUNT; i++) {
		const SolersBuiltinSkillRecord &record = SOLERS_BUILTIN_SKILLS[i];
		if (name == String::utf8(record.name)) {
			r_skill.name = String::utf8(record.name);
			r_skill.description = String::utf8(record.description);
			r_skill.tools = String::utf8(record.tools).split(",", false);
			r_skill.content = String::utf8(record.content);
			return true;
		}
	}
	return false;
}

String SolersBuiltinSkills::build_catalog_prompt() {
	if (SOLERS_BUILTIN_SKILL_COUNT <= 0) {
		return String();
	}

	String out = "\n\nBuilt-in Solers skills:\n"
				 "Use skill.read with an exact name when a skill is relevant. The catalog is only a summary; read the skill before following it.\n";
	for (int i = 0; i < SOLERS_BUILTIN_SKILL_COUNT; i++) {
		const SolersBuiltinSkillRecord &record = SOLERS_BUILTIN_SKILLS[i];
		out += vformat("- %s: %s\n", String::utf8(record.name), String::utf8(record.description));
	}
	return out.strip_edges();
}
