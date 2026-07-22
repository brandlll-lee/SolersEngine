/**************************************************************************/
/*  solers_plugin.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/object/gdvirtual.gen.inc"
#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

#include <functional>

// One in-flight provider job. The asset service owns task threading and
// manifest persistence; the job carries the state, staging directory, API
// key, and cancellation flag a plugin needs to execute the provider work.
class SolersPluginJob : public RefCounted {
	GDCLASS(SolersPluginJob, RefCounted);

	String asset_id;
	String api_key;
	String base_url;
	String staging_dir;
	bool resume = false;
	const SafeFlag *abort = nullptr;
	Dictionary state;
	std::function<void(const Dictionary &)> persist;
	Array files;
	String preview_url;

protected:
	static void _bind_methods();

public:
	void setup(const String &p_asset_id, const String &p_api_key, const String &p_base_url, const String &p_staging_dir, bool p_resume, const SafeFlag *p_abort, const Dictionary &p_state, const std::function<void(const Dictionary &)> &p_persist);

	String get_asset_id() const { return asset_id; }
	String get_api_key() const { return api_key; }
	String get_base_url() const { return base_url; }
	String get_staging_dir() const { return staging_dir; }
	bool is_resume() const { return resume; }
	bool is_cancelled() const { return abort && abort->is_set(); }
	const SafeFlag *cancel_flag() const { return abort; }

	Dictionary get_state() const { return state.duplicate(true); }
	void set_state(const Dictionary &p_state);
	void fail(const String &p_code, const String &p_message);

	void add_file(const String &p_path) { files.push_back(p_path); }
	Array get_files() const { return files; }
	void set_files(const Array &p_files) { files = p_files.duplicate(); }
	void set_preview_url(const String &p_url) { preview_url = p_url; }
	String get_preview_url() const { return preview_url; }

	Dictionary http_request(const String &p_method, const String &p_url, const Vector<String> &p_headers, const PackedByteArray &p_body, uint64_t p_timeout_msec, int64_t p_max_body_bytes = -1) const;
	Dictionary http_request_bound(const String &p_method, const String &p_url, const PackedStringArray &p_headers, const PackedByteArray &p_body, int64_t p_timeout_msec, int64_t p_max_body_bytes) const;
};

// One external service connector (Meshy, ambientCG, Poly Haven, ElevenLabs,
// ...). Built-ins are C++ subclasses registered by the module; GDExtension
// or script connectors override the GDVIRTUAL hooks and register themselves
// through SolersPluginRegistry.
class SolersPlugin : public Object {
	GDCLASS(SolersPlugin, Object);

protected:
	// Provider-level caches live on the connector so they survive asset
	// service recreation and stay owned by the code that fills them.
	mutable Mutex catalog_cache_mutex;
	Dictionary catalog_cache;
	Dictionary catalog_inspections;

	static void _bind_methods();
	GDVIRTUAL0RC(Dictionary, _get_profile)
	GDVIRTUAL1RC(Dictionary, _get_generation_options_schema, String)
	GDVIRTUAL0RC(Array, _get_operation_defs)
	GDVIRTUAL3RC(Dictionary, _prepare_generate, String, Dictionary, Dictionary)
	GDVIRTUAL3RC(Dictionary, _prepare_operation, Dictionary, Dictionary, Dictionary)
	GDVIRTUAL1RC(Dictionary, _capability_extras, Dictionary)
	GDVIRTUAL1RC(Dictionary, _catalog_search, Dictionary)
	GDVIRTUAL1RC(Dictionary, _catalog_inspect, Dictionary)
	GDVIRTUAL1(_run_job, Ref<SolersPluginJob>)

public:
	// Profile keys: id, label, kinds (Array of String), base_url,
	// api_key_env, docs, attribution, requires_api_key, supports_generation,
	// supports_catalog, supports_resume, download_headers (PackedStringArray).
	virtual Dictionary get_profile() const;
	virtual Dictionary get_generation_options_schema(const String &p_kind) const;
	virtual Array get_operation_defs() const;
	// Validates and normalizes generation args; may add provider fields to
	// r_manifest (including a normalized provider_options). Returns an empty
	// Dictionary when the job may queue, or an error_data(code, message).
	virtual Dictionary prepare_generate(const String &p_kind, const Dictionary &p_args, Dictionary &r_manifest) const;
	// Validates and fills provider defaults for one operation manifest before
	// it queues. Same error contract as prepare_generate.
	virtual Dictionary prepare_operation(const Dictionary &p_operation, const Dictionary &p_source_manifest, Dictionary &r_provider_options) const;
	// Extra provider facts merged into asset.capabilities responses.
	virtual Dictionary capability_extras(const Dictionary &p_manifest) const;
	virtual Dictionary catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested);
	virtual Dictionary catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested);
	Dictionary cached_inspection(const String &p_kind, const String &p_asset_id) const;
	virtual void run_job(const Ref<SolersPluginJob> &p_job);

	// Shared provider-side utilities (single authoritative implementations;
	// the asset service uses these too).
	static Dictionary http_request(const String &p_method, const String &p_url, const Vector<String> &p_headers, const PackedByteArray &p_body, uint64_t p_timeout_msec, int64_t p_max_body_bytes = -1, const SafeFlag *p_cancel_requested = nullptr, int p_retry_count = 0);
	static Dictionary parse_json_body(const Dictionary &p_response);
	static PackedByteArray utf8_bytes(const String &p_text);
	static Dictionary error_data(const String &p_code, const String &p_message);
	static Dictionary ok_result(const Variant &p_data);
	static Dictionary error_result(const String &p_code, const String &p_message, bool p_recoverable = true);
	static bool write_json_atomic(const String &p_path, const Dictionary &p_data, String &r_error);
	static Dictionary read_json_file(const String &p_path);
	static bool write_bytes_atomic(const String &p_path, const PackedByteArray &p_bytes, String &r_error);
	static bool copy_file(const String &p_from, const String &p_to, String &r_error);
	static bool remove_dir_recursive(const String &p_path, String &r_error);
	static String safe_slug(const String &p_text);
	static String clean_base_url(const String &p_base_url);
	static void merge_options(Dictionary &r_body, const Dictionary &p_options);
	static String first_manifest_field(const Dictionary &p_manifest, const Array &p_fields);
	static Array rank_catalog_assets(const Array &p_assets, const String &p_query);
	static String match_catalog_variant_id(const Array &p_variants, const String &p_id);
	static Dictionary project_import_selection(const Array &p_files, const Array &p_declared_import_files, const Array &p_declared_entrypoints);
	static Array native_resource_entrypoints(const Array &p_files);
	static Array model_resource_entrypoints(const Array &p_files);
};

// Process-wide connector registry. Built-ins register at editor module
// initialization; the asset service, tool registry, and UI enumerate it
// instead of hardcoding provider lists.
class SolersPluginRegistry {
	static Vector<SolersPlugin *> plugins;
	static Vector<SolersPlugin *> builtins;

public:
	static void register_plugin(SolersPlugin *p_plugin);
	static void unregister_plugin(SolersPlugin *p_plugin);
	static SolersPlugin *get_plugin(const String &p_id);
	static const Vector<SolersPlugin *> &get_plugins();
	// The default generator for a kind: the EditorSettings override when it
	// names a registered connector, otherwise the first registered connector
	// that generates this kind.
	static SolersPlugin *default_generator_for_kind(const String &p_kind);
	static void register_builtins();
	static void unregister_builtins();
};
