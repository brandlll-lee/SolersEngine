/**************************************************************************/
/*  solers_asset_service.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

class SolersAssetService : public Object {
	GDCLASS(SolersAssetService, Object);

	struct Task {
		String asset_id;
		String api_key;
		Thread thread;
		SafeFlag abort;
		SafeFlag done;
		Mutex mutex;
		Dictionary state;
	};

	mutable Mutex tasks_mutex;
	HashMap<String, Task *> tasks;

	static String _asset_root();
	static String _asset_dir(const String &p_asset_id);
	static String _manifest_path(const String &p_asset_id);
	static String _source_dir(const String &p_asset_id);
	static String _safe_slug(const String &p_text);
	static String _default_provider(const String &p_kind);
	static String _default_base_url(const String &p_provider);
	static String _default_env(const String &p_provider);
	static String _setting_path(const String &p_kind, const String &p_key);
	static Dictionary _http_request(const String &p_method, const String &p_url, const Vector<String> &p_headers, const PackedByteArray &p_body, uint64_t p_timeout_msec, int64_t p_max_body_bytes = -1);
	static bool _write_json_atomic(const String &p_path, const Dictionary &p_data, String &r_error);
	static Dictionary _read_json_file(const String &p_path);
	static bool _write_bytes_atomic(const String &p_path, const PackedByteArray &p_bytes, String &r_error);
	static bool _copy_file(const String &p_from, const String &p_to, String &r_error);
	static void _download_preview(Task *p_task, Dictionary &r_state, const String &p_url);
	static Dictionary _meshy_poll(Task *p_task, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers);
	static Dictionary _meshy_submit_and_poll(Task *p_task, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers, const Dictionary &p_body, const String &p_stage);
	static bool _meshy_download_model(Task *p_task, Dictionary &r_state, const Dictionary &p_detail, const String &p_asset_id, Array &r_files, String &r_preview_url);
	static void _task_func(void *p_userdata);
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

protected:
	static void _bind_methods();

public:
	Dictionary generate(const Dictionary &p_args);
	Dictionary refine_to_ready(const Dictionary &p_args);
	Dictionary optimize_geometry(const Dictionary &p_args);
	Dictionary restyle_material(const Dictionary &p_args);
	Dictionary status(const Dictionary &p_args) const;
	Dictionary list_local(const Dictionary &p_args) const;
	Dictionary import_to_project(const Dictionary &p_args) const;

	SolersAssetService();
	~SolersAssetService();
};
