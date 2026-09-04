/**************************************************************************/
/*  solers_model_catalog.h                                                */
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

#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersModelCatalog {
	// providerID -> { id, name, npm, api, env(Array), local(bool), protocol,
	//                 models: { modelID -> { id, name, context, output,
	//                 status, modalities, provider_npm, provider_api } } }
	HashMap<StringName, Dictionary> builtin_providers;
	HashMap<StringName, Dictionary> providers;
	Dictionary remote_catalog;
	Dictionary model_overrides;
	mutable Mutex providers_mutex;
	String cache_path;
	uint64_t catalog_revision = 0;
	uint64_t last_refresh_msec = 0;

	Thread refresh_thread;
	SafeFlag refresh_started;

	void _load_builtin();
	void _load_cache();
	void _merge_catalog(const Dictionary &p_root, HashMap<StringName, Dictionary> &r_providers) const;
	void _rebuild();
	static String _resolve_cache_path();

	static void _refresh_func(void *p_userdata);
	void _run_refresh();

public:
	// Loads built-in data + cache synchronously. Network refresh is kicked lazily by
	// refresh() so editor startup can finish loading TLS certificates first.
	void initialize();
	void refresh();
	void set_model_overrides(const Dictionary &p_overrides);
	uint64_t get_catalog_revision() const;

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
	// Effort values explicitly declared by the merged catalog.
	static Array reasoning_efforts(const Dictionary &p_model);
	SolersModelCatalog();
	~SolersModelCatalog();
};
