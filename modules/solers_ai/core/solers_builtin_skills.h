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

struct SolersBuiltinSkillView {
	String name;
	String description;
	String content;
};

class SolersBuiltinSkills {
public:
	static int get_count();
	static bool find_by_name(const String &p_name, SolersBuiltinSkillView &r_skill);
	static String build_catalog_prompt();
};
