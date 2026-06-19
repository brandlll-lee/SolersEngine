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

#include "core/os/thread.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersModelsDev {
	// providerID -> { id, name, npm, api, env(Array), local(bool),
	//                 models: { modelID -> { id, name, context, output,
	//                 reasoning, tool_call, attachment } } }
	HashMap<StringName, Dictionary> providers;

	Thread refresh_thread;
	SafeFlag refresh_started;

	void _load_seed();
	void _load_cache();
	// Parse a models.dev `api.json` document and overlay it onto `providers`.
	void _ingest(const Dictionary &p_root);
	static String _cache_path();

	static void _refresh_func(void *p_userdata);
	void _run_refresh();

public:
	// Loads seed + cache (synchronous), then kicks a best-effort background
	// fetch that refreshes the cache for the next launch.
	void initialize();

	bool has_provider(const StringName &p_id) const;
	Dictionary get_provider(const StringName &p_id) const;
	// Per-model metadata, or an empty dictionary when unknown.
	Dictionary get_model(const StringName &p_provider, const String &p_model) const;
	Array list_providers() const;

	SolersModelsDev();
	~SolersModelsDev();
};
