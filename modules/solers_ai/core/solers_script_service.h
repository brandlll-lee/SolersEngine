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
#include "core/os/process_id.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

#include <functional>

class SolersScriptContext;
struct SolersToolContext;

struct SolersScriptAuthority {
	StringName target_argument;
	String description;
	std::function<Dictionary(const String &)> validate;
	std::function<Dictionary(const String &)> prepare;
	std::function<Dictionary(const Ref<SolersScriptContext> &, const String &)> commit;
	std::function<void(const Ref<SolersScriptContext> &)> release;
	std::function<Dictionary(const String &, const Dictionary &)> publish;
};

class SolersScriptService : public Object {
	GDCLASS(SolersScriptService, Object);

	struct ScriptTask {
		ProcessID process_id = 0;
		uint64_t deadline_msec = 0;
		String call_id;
		String directory;
		String result_path;
		String progress_path;
		String cancel_path;
		String source_path;
		StringName authority;
		String source_sha256;
		Dictionary source_state;
		Dictionary result;
	};

	static HashMap<StringName, SolersScriptAuthority> authorities;
	Error project_settings_save_error = OK;
	HashMap<String, ScriptTask> script_tasks;

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	Dictionary _write_file(const Dictionary &p_args, const SolersToolContext *p_context);
	Dictionary _patch_file(const Dictionary &p_args, const SolersToolContext *p_context);
	Dictionary _remove_project_path(const Dictionary &p_args, const SolersToolContext *p_context);
	Dictionary _edit_path(const Dictionary &p_args, const SolersToolContext *p_context);
	Dictionary _edit_project(const Dictionary &p_args, const SolersToolContext *p_context);
	Dictionary _validate_source(const String &p_path, const String &p_source) const;
	Dictionary _read_json_file(const String &p_path) const;
	void _write_task_result(const Dictionary &p_request, const Dictionary &p_result) const;
	void _remove_task_files(const ScriptTask &p_task) const;
	void _apply_project_settings(const Dictionary &p_values, const PackedStringArray &p_erase);
	static const SolersScriptAuthority *_get_authority(const StringName &p_name);

protected:
	static void _bind_methods();

public:
	static void register_authority(const StringName &p_name, const SolersScriptAuthority &p_authority);
	static void clear_authorities();
	static Dictionary get_authority_schema();

	Dictionary write_file(const Dictionary &p_args);
	Dictionary write_file_with_context(const Dictionary &p_args, const SolersToolContext &p_context);
	Dictionary patch_file(const Dictionary &p_args);
	Dictionary patch_file_with_context(const Dictionary &p_args, const SolersToolContext &p_context);
	Dictionary remove_project_path(const Dictionary &p_args);
	Dictionary remove_project_path_with_context(const Dictionary &p_args, const SolersToolContext &p_context);
	Dictionary edit_path(const Dictionary &p_args);
	Dictionary edit_path_with_context(const Dictionary &p_args, const SolersToolContext &p_context);
	Dictionary edit_project(const Dictionary &p_args);
	Dictionary edit_project_with_context(const Dictionary &p_args, const SolersToolContext &p_context);
	Dictionary edit_script(const Dictionary &p_args);
	Dictionary validate_script(const Dictionary &p_args) const;
	Dictionary start_authority_script(const StringName &p_authority, const Dictionary &p_args, const String &p_call_id, const SolersToolContext *p_context);
	Dictionary start_script(const StringName &p_authority, const Dictionary &p_args, const String &p_call_id) { return start_authority_script(p_authority, p_args, p_call_id, nullptr); }
	Dictionary poll_authority_script(const Dictionary &p_args);
	bool is_authority_script_ready(const Dictionary &p_args) const;
	void complete_authority_script(const Dictionary &p_args);
	Dictionary prepare_script_task(const String &p_request_path);
	Dictionary call_script_task(Object *p_instance, const Ref<SolersScriptContext> &p_context) const;
	Dictionary finish_script_task(const Ref<SolersScriptContext> &p_context, const String &p_request_path);

	~SolersScriptService();
};

void solers_script_authorities_initialize();
void solers_script_authorities_uninitialize();
