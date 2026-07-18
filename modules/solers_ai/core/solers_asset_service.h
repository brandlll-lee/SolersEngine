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
#include "core/templates/hash_set.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

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
	struct CatalogPageJob {
		String url;
		Dictionary response;
	};
	struct CatalogPageBatch {
		CatalogPageJob *jobs = nullptr;
		const SafeFlag *cancel_requested = nullptr;
	};

	mutable Mutex tasks_mutex;
	HashMap<String, Task *> tasks;
	mutable Mutex terminal_events_mutex;
	Array terminal_events;
	HashMap<String, Dictionary> project_imports;
	mutable Mutex project_imports_mutex;
	bool project_import_signals_connected = false;
	bool project_import_wave_active = false;
	uint64_t project_import_wave_id = 0;
	mutable Mutex catalog_cache_mutex;
	Dictionary ambientcg_assets_cache;
	Dictionary polyhaven_assets_cache;
	Dictionary catalog_inspections;
	Dictionary plugin_inspections;

	static String _asset_root();
	static String _asset_dir(const String &p_asset_id);
	static String _manifest_path(const String &p_asset_id);
	static String _source_dir(const String &p_asset_id);
	static String _safe_slug(const String &p_text);
	static String _default_provider(const String &p_kind);
	static String _default_base_url(const String &p_provider);
	static String _default_env(const String &p_provider);
	static String _setting_path(const String &p_kind, const String &p_key);
	static Dictionary _http_request(const String &p_method, const String &p_url, const Vector<String> &p_headers, const PackedByteArray &p_body, uint64_t p_timeout_msec, int64_t p_max_body_bytes = -1, const SafeFlag *p_cancel_requested = nullptr, int p_retry_count = 0);
	static bool _write_json_atomic(const String &p_path, const Dictionary &p_data, String &r_error);
	static Dictionary _read_json_file(const String &p_path);
	static bool _write_bytes_atomic(const String &p_path, const PackedByteArray &p_bytes, String &r_error);
	static bool _copy_file(const String &p_from, const String &p_to, String &r_error);
	static bool _remove_dir_recursive(const String &p_path, String &r_error);
	static Dictionary _ambientcg_asset_metadata(const String &p_asset_id, const SafeFlag *p_cancel_requested = nullptr);
	static bool _ambientcg_extract_archive(Task *p_task, const PackedByteArray &p_archive, Array &r_files, String &r_error);
	static Dictionary _polyhaven_asset_metadata(const String &p_asset_id, const SafeFlag *p_cancel_requested = nullptr);
	static bool _polyhaven_download_file(Task *p_task, const String &p_label, const Dictionary &p_spec, Array &r_files, Dictionary &r_checksums, String &r_error, const String &p_relative_path = String());
	static bool _polyhaven_download_variant(Task *p_task, const Dictionary &p_files, const String &p_kind, const String &p_variant, Array &r_files, Dictionary &r_maps, Dictionary &r_checksums, String &r_error);
	static void _download_preview(Task *p_task, Dictionary &r_state, const String &p_url);
	static Array _meshy_animation_actions();
	static bool _meshy_animation_action_exists(int64_t p_action_id);
	static Dictionary _meshy_poll(Task *p_task, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers);
	static Dictionary _meshy_submit_and_poll(Task *p_task, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers, const Dictionary &p_body, const String &p_stage);
	static bool _meshy_download_model(Task *p_task, Dictionary &r_state, const Dictionary &p_detail, const String &p_asset_id, Array &r_files, String &r_preview_url);
	static void _meshy_download_basic_animations(Task *p_task, Dictionary &r_state, const Dictionary &p_detail, const String &p_asset_id);
	static void _task_func(void *p_userdata);
	void _catalog_fetch_page(uint32_t p_index, CatalogPageBatch *p_batch);
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
	void _ensure_project_import_signals();
	void _start_project_import_wave();
	void _on_project_filesystem_changed();
	void _on_project_resources_reimported(const PackedStringArray &p_resources);

protected:
	static void _bind_methods();

public:
	static Array normalize_polyhaven_variants(const Dictionary &p_files, const String &p_kind);
	static String match_catalog_variant_id(const Array &p_variants, const String &p_id);
	static Array rank_catalog_assets(const Array &p_assets, const String &p_query);
	static bool is_trusted_plugin(const Dictionary &p_args);
	Dictionary generate(const Dictionary &p_args);
	Dictionary generate_for_session(const Dictionary &p_args, const String &p_session_id);
	Dictionary catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested = nullptr);
	Dictionary catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested = nullptr);
	Dictionary catalog_acquire(const Dictionary &p_args, const String &p_session_id);
	Dictionary plugin_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested = nullptr);
	Dictionary plugin_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested = nullptr);
	Dictionary plugin_agent_contract(const Dictionary &p_args) const;
	Dictionary plugin_list(const Dictionary &p_args) const;
	Dictionary plugin_ensure(const Dictionary &p_args);
	Dictionary wait_jobs(const Dictionary &p_args, const String &p_session_id) const;
	Dictionary capabilities(const Dictionary &p_args) const;
	Dictionary run_operation(const Dictionary &p_args);
	Dictionary run_operation_for_session(const Dictionary &p_args, const String &p_session_id);
	Dictionary status(const Dictionary &p_args) const;
	Dictionary list_local(const Dictionary &p_args) const;
	Dictionary import_to_project(const Dictionary &p_args);
	Dictionary poll_project_import(const Dictionary &p_args);
	bool is_project_import_ready(const Dictionary &p_args) const;
	Dictionary get_project_import_coordinator_state() const;
	Dictionary delete_asset(const Dictionary &p_args);
	Array take_terminal_events(const String &p_session_id);
	Array pending_terminal_events(const String &p_session_id) const;
	bool has_active_tasks(const String &p_session_id) const;
	void mark_terminal_delivered(const String &p_asset_id, const String &p_session_id);
	void poll(bool p_allow_new_import_wave = true);

	SolersAssetService();
	~SolersAssetService();
};
