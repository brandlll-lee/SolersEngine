/**************************************************************************/
/*  solers_model_modifier.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "modules/solers_modeling/core/solers_editable_mesh.h"

class SolersModelModifierEvaluator {
public:
	static Error evaluate(const SolersEditableMesh &p_source, SolersEditableMesh &r_result, String *r_error = nullptr);
};

