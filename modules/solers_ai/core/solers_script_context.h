/**************************************************************************/
/*  solers_script_context.h                                               */
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

#include "core/object/ref_counted.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

#include <functional>

class SolersScriptContext;

struct SolersNativeScriptJob {
	StringName id;
	StringName target_class;
	String description;
	PackedStringArray authorities;
	Dictionary input_schema;
	std::function<Dictionary(SolersScriptContext *, Object *, const Dictionary &)> run;
};

class SolersScriptContext : public RefCounted {
	GDCLASS(SolersScriptContext, RefCounted);

	static Vector<SolersNativeScriptJob> native_jobs;

	StringName authority;
	Variant subject;
	String source_path;
	Dictionary import_options;
	PackedStringArray outputs;
	Array logs;
	Dictionary failure;
	Variant result;
	String progress_path;
	String cancel_path;
	uint64_t deadline_msec = 0;
	bool changed = false;
	bool reimport = false;
	bool import_controls = false;

	void _write_progress(float p_completion, const String &p_message) const;

protected:
	static void _bind_methods();

public:
	static void register_native_job(const SolersNativeScriptJob &p_job);
	static void clear_native_jobs();

	void initialize(const StringName &p_authority, const Variant &p_subject, const String &p_source_path,
			const Dictionary &p_import_options, bool p_import_controls, const PackedStringArray &p_outputs, const String &p_progress_path,
			const String &p_cancel_path, uint64_t p_deadline_msec);

	StringName get_authority() const { return authority; }
	Object *get_subject() const;
	String get_source_path() const { return source_path; }
	Dictionary get_import_options() const { return import_options.duplicate(true); }
	void set_import_option(const StringName &p_name, const Variant &p_value);
	void mark_changed() { changed = true; }
	void request_reimport();
	void log(const Variant &p_value);
	void fail(const String &p_code, const String &p_message);
	void set_result(const Variant &p_result) { result = p_result; }
	Array list_native_jobs(Object *p_target) const;
	Dictionary run_native_job(const StringName &p_id, Object *p_target, const Dictionary &p_args);
	bool is_cancelled() const;

	bool has_changed() const { return changed; }
	bool needs_reimport() const { return reimport; }
	bool has_failed() const { return !failure.is_empty(); }
	Dictionary get_failure() const { return failure; }
	Array get_logs() const { return logs; }
	Variant get_result() const { return result; }
	bool is_output_declared(const String &p_path) const;
	void report_progress(float p_completion, const String &p_message) const;
};

void solers_script_jobs_initialize();
void solers_script_jobs_uninitialize();
