/**************************************************************************/
/*  solers_project_observation.h                                          */
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

#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/variant/dictionary.h"

class SolersProjectObservation : public Object {
	GDCLASS(SolersProjectObservation, Object);

	mutable Mutex project_files_mutex;
	PackedStringArray project_files;
	bool project_files_ready = false;

	void _refresh_project_files();

protected:
	static void _bind_methods();

public:
	Dictionary get_project_info() const;
	Dictionary get_project_settings_summary() const;
	Dictionary inspect_project_delivery(const Dictionary &p_args, int p_token_budget = INT32_MAX) const;
	Dictionary search_project(const Dictionary &p_args, int p_token_budget = INT32_MAX) const;
	Dictionary observe_path(const String &p_path) const;
	Dictionary digest_packed_scene(const String &p_path, int p_max_nodes = 96) const;
	Dictionary read_project_file(const String &p_path, int p_line_start = 1, int p_line_count = 200, bool p_raw = false, int p_token_budget = INT32_MAX) const;

	SolersProjectObservation();
};
