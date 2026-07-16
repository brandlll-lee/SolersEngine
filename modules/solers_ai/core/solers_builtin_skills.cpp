/**************************************************************************/
/*  solers_builtin_skills.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_builtin_skills.h"

#include "core/variant/variant.h"
#include "modules/solers_ai/generated/solers_builtin_skills.gen.h"

static Vector<String> _split_required_tools(const char *p_csv) {
	Vector<String> tools;
	if (!p_csv || !p_csv[0]) {
		return tools;
	}
	const String csv = String::utf8(p_csv);
	int start = 0;
	for (int i = 0; i <= csv.length(); i++) {
		if (i == csv.length() || csv[i] == U';') {
			const String tool = csv.substr(start, i - start).strip_edges();
			if (!tool.is_empty()) {
				tools.push_back(tool);
			}
			start = i + 1;
		}
	}
	return tools;
}

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
			r_skill.content = String::utf8(record.content);
			r_skill.required_tools = _split_required_tools(record.required_tools_csv);
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

Vector<String> SolersBuiltinSkills::validate_required_tools(const HashSet<StringName> &p_registered_tools) {
	Vector<String> missing;
	for (int i = 0; i < SOLERS_BUILTIN_SKILL_COUNT; i++) {
		const SolersBuiltinSkillRecord &record = SOLERS_BUILTIN_SKILLS[i];
		const Vector<String> required = _split_required_tools(record.required_tools_csv);
		for (int j = 0; j < required.size(); j++) {
			if (!p_registered_tools.has(StringName(required[j]))) {
				missing.push_back(vformat("%s requires missing tool %s", String::utf8(record.name), required[j]));
			}
		}
	}
	return missing;
}
