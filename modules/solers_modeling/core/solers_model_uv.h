/**************************************************************************/
/*  solers_model_uv.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/variant/dictionary.h"

class SolersEditableMesh;

class SolersModelUV {
public:
	static Error unwrap(SolersEditableMesh &r_mesh, const Dictionary &p_options, bool p_use_input_uvs, String *r_error = nullptr);
};

