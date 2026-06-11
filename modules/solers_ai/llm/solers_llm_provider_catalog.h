/**************************************************************************/
/*  solers_llm_provider_catalog.h                                         */
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

#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// ---------------------------------------------------------------------------
// SolersLLMProviderCatalog — the data that turns a wire protocol into a usable
// provider, with zero per-provider code branches.
//
// A profile is pure data: which protocol it speaks, its default base URL, and
// how its API key is carried (which header + value prefix). "OpenAI",
// "Anthropic" and "OpenAI-compatible" (relays, Ollama, LM Studio, DeepSeek,
// Qwen, ...) are all just rows here. Adding a provider that follows one of the
// existing protocols is a one-line `_define()` call, never a new code path.
//
// Profile shape (Dictionary):
//   id            StringName-able key, e.g. "openai"
//   label         human label
//   protocol      protocol id resolved via SolersLLMProtocolRegistry
//   base_url      default endpoint base (user config may override)
//   auth_header   header carrying the key, e.g. "Authorization" / "x-api-key"
//   auth_prefix   value prefix, e.g. "Bearer " / ""
//   api_key_env   environment variable fallback for the key
//   local         true for local runtimes that need no key
// ---------------------------------------------------------------------------
class SolersLLMProviderCatalog {
	HashMap<StringName, Dictionary> profiles;

	void _define(const String &p_id, const String &p_label, const String &p_protocol, const String &p_base_url, const String &p_auth_header, const String &p_auth_prefix, const String &p_api_key_env, bool p_local);

public:
	bool has(const StringName &p_id) const;
	Dictionary get_profile(const StringName &p_id) const;
	Array list_profiles() const;

	// Builds the effective profile for a request: starts from the registered
	// profile (or a generic openai-compatible profile for unknown ids) and
	// overlays any user-provided base_url so relays/self-hosted endpoints work.
	Dictionary resolve(const StringName &p_id, const String &p_base_url_override) const;

	void register_builtin_profiles();

	SolersLLMProviderCatalog() {}
};
