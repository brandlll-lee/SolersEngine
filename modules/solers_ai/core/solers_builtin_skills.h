/**************************************************************************/
/*  solers_builtin_skills.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#pragma once

#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"

struct SolersBuiltinSkillView {
	String name;
	String description;
	String content;
	Vector<String> required_tools;
};

class SolersBuiltinSkills {
public:
	static int get_count();
	static bool find_by_name(const String &p_name, SolersBuiltinSkillView &r_skill);
	static String build_catalog_prompt();
	static Vector<String> validate_required_tools(const HashSet<StringName> &p_registered_tools);
};
