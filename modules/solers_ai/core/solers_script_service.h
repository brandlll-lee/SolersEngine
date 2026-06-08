/**************************************************************************/
/*  solers_script_service.h                                               */
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
#include "core/variant/dictionary.h"

class SolersActionTimeline;
class SolersFileCheckpoint;

class SolersScriptService : public Object {
	GDCLASS(SolersScriptService, Object);

	SolersActionTimeline *action_timeline = nullptr;
	SolersFileCheckpoint *file_checkpoint = nullptr;

	bool _normalize_project_path(const String &p_path, String &r_res_path, String &r_error, bool p_allow_project_data = false) const;
	void _collect_script_files(const String &p_dir, int p_max_files, Array &r_files, bool &r_truncated) const;
	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	Dictionary _validate_source(const String &p_path, const String &p_source) const;

protected:
	static void _bind_methods();

public:
	void set_action_timeline(SolersActionTimeline *p_action_timeline);
	void set_file_checkpoint(SolersFileCheckpoint *p_file_checkpoint);

	Dictionary write_file(const Dictionary &p_args);
	Dictionary patch_file(const Dictionary &p_args);
	Dictionary validate_script(const Dictionary &p_args) const;
	Dictionary validate_project_scripts(const Dictionary &p_args) const;
	Dictionary open_script(const Dictionary &p_args) const;
};
