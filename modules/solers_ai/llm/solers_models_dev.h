/**************************************************************************/
/*  solers_models_dev.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Data-driven provider/model registry, modeled on opencode's            */
/* `ModelsDev` service: provider connection facts and per-model metadata  */
/* (context/output limits, capabilities) come from DATA, not a hardcoded  */
/* code catalog. The dataset is fetched at runtime from models.dev (and   */
/* cached), with a small built-in seed for offline/first-run so the common */
/* providers work immediately. This replaces guessing a model's context    */
/* window with its real limit (the data half of the connection layer).    */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersModelsDev {
	// providerID -> { id, name, npm, api, env(Array), local(bool),
	//                 models: { modelID -> { id, name, context, output,
	//                 reasoning, reasoning_options, tool_call, attachment } } }
	HashMap<StringName, Dictionary> providers;
	mutable Mutex providers_mutex;
	String cache_path;

	Thread refresh_thread;
	SafeFlag refresh_started;

	void _load_seed();
	void _load_cache();
	// Parse a models.dev `api.json` document and overlay it onto `providers`.
	void _ingest(const Dictionary &p_root);
	static String _resolve_cache_path();

	static void _refresh_func(void *p_userdata);
	void _run_refresh();

public:
	// Loads seed + cache synchronously. Network refresh is kicked lazily by
	// refresh() so editor startup can finish loading TLS certificates first.
	void initialize();
	void refresh();

	bool has_provider(const StringName &p_id) const;
	Dictionary get_provider(const StringName &p_id) const;
	// Ordered catalog entries for Settings UI / Runtime assembly.
	Array list_providers() const;
	// Model ids only — no deep-copy of the full provider document.
	Array list_model_ids(const StringName &p_provider) const;
	// Popular subset ids (catalog ordering data). Missing entries skipped by callers.
	Array list_popular_provider_ids() const;
	// Legacy connection ids that map through canonical_provider_id (migration enum).
	Array list_legacy_provider_ids() const;
	// Legacy Solers connection id → models.dev catalog id.
	String canonical_provider_id(const String &p_id) const;
	// Per-model metadata, or an empty dictionary when unknown.
	Dictionary get_model(const StringName &p_provider, const String &p_model) const;
	// Returns 1 when declared, 0 when explicitly absent, and -1 when unknown.
	static int input_modality_support(const Dictionary &p_model, const String &p_modality);
	// Effort values declared by models.dev. Unknown/custom reasoning models keep
	// the generic High/Extra High controls instead of losing the capability.
	static Array reasoning_efforts(const Dictionary &p_model);
	// Catalog entry by id, falling back to matching the endpoint URL (custom
	// gateway profiles). Empty dictionary when the catalog has no answer.
	Dictionary find_provider(const String &p_id, const String &p_api_url = String()) const;

	SolersModelsDev();
	~SolersModelsDev();
};
