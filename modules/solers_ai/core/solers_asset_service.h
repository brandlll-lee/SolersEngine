/**************************************************************************/
/*  solers_asset_service.h                                                */
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
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

class Image;

class SolersAssetService : public Object {
	GDCLASS(SolersAssetService, Object);

	struct Task {
		String asset_id;
		String api_key;
		SolersAssetService *service = nullptr;
		Thread thread;
		SafeFlag abort;
		SafeFlag done;
		bool resume_provider_task = false;
		Mutex mutex;
		Dictionary state;
	};
	struct InputTask {
		WorkerThreadPool::TaskID id = 0;
		SolersAssetService *service = nullptr;
		Variant source;
		Variant image;
		Callable callback;
		Dictionary result;
	};
	mutable Mutex tasks_mutex;
	HashMap<String, Task *> tasks;
	Vector<InputTask *> input_tasks;
	mutable Mutex terminal_events_mutex;
	Array terminal_events;
	HashMap<String, Dictionary> project_imports;
	mutable Mutex project_imports_mutex;
	mutable Mutex catalog_cache_mutex;
	Dictionary addon_inspections;
	SafeNumeric<uint64_t> revision{ 1 };

	static String _asset_root();
	static String _asset_dir(const String &p_asset_id);
	static String _manifest_path(const String &p_asset_id);
	static String _source_dir(const String &p_asset_id);
	static void _download_preview(Task *p_task, Dictionary &r_state, const String &p_url);
	static void _task_func(void *p_userdata);
	static void _input_task_func(void *p_userdata);
	void _advance_input_tasks();
	void _queue_terminal_event(Task *p_task);
	static void _set_task_state(Task *p_task, const Dictionary &p_state);
	static Dictionary _task_state(Task *p_task);
	static Dictionary _error_data(const String &p_code, const String &p_message);
	static void _run_task(Task *p_task);

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	Dictionary _provider_config(const String &p_kind, const String &p_provider) const;
	Dictionary _manifest_for_asset(const String &p_asset_id) const;
	void _cleanup_finished_task(const String &p_asset_id) const;
	Dictionary _queue_manifest(const Dictionary &p_manifest, const Dictionary &p_provider_config);
	Dictionary _generate(const Dictionary &p_args, const String &p_session_id);
	Dictionary _run_operation(const Dictionary &p_args, const String &p_session_id);
	void _advance_project_tasks(bool p_allow_new_imports);

protected:
	static void _bind_methods();

public:
	static bool is_trusted_addon(const Dictionary &p_args);
	Dictionary generate(const Dictionary &p_args);
	Dictionary generate_for_session(const Dictionary &p_args, const String &p_session_id);
	Dictionary catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested = nullptr);
	Dictionary catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested = nullptr);
	Dictionary fetch_provider_preview(const String &p_provider, const String &p_url, const SafeFlag *p_cancel_requested = nullptr) const;
	Dictionary catalog_acquire(const Dictionary &p_args, const String &p_session_id);
	Dictionary addon_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested = nullptr);
	Dictionary addon_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested = nullptr);
	Dictionary addon_agent_contract(const Dictionary &p_args) const;
	Dictionary addon_list(const Dictionary &p_args) const;
	Dictionary addon_ensure(const Dictionary &p_args);
	Dictionary addon_ensure_finalize(const Dictionary &p_args);
	bool addon_ensure_ready(const Dictionary &p_args) const;
	Dictionary wait_jobs(const Dictionary &p_args, const String &p_session_id) const;
	Dictionary capabilities(const Dictionary &p_args) const;
	Dictionary run_operation(const Dictionary &p_args);
	Dictionary run_operation_for_session(const Dictionary &p_args, const String &p_session_id);
	Dictionary status(const Dictionary &p_args) const;
	Array list_assets() const;
	Dictionary stage_input_image(const Ref<Image> &p_image) const;
	void stage_input_image_async(const Variant &p_source, const Callable &p_callback);
	String resolve_model_file(const Dictionary &p_manifest) const;
	static bool is_project_import_terminal_status(const String &p_status);
	uint64_t get_revision() const { return revision.get(); }
	bool is_provider_configured(const String &p_kind, const String &p_provider) const;
	Dictionary start_project_import(const Dictionary &p_args);
	Dictionary poll_project_import(const Dictionary &p_args);
	Dictionary get_project_import_coordinator_state() const;
	Array take_terminal_events(const String &p_session_id);
	Array pending_terminal_events(const String &p_session_id) const;
	bool has_active_tasks(const String &p_session_id) const;
	void mark_terminal_delivered(const String &p_asset_id, const String &p_session_id);
	void poll(bool p_allow_new_imports = true);

	SolersAssetService();
	~SolersAssetService();
};
