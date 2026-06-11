/**************************************************************************/
/*  solers_llm_provider_catalog.cpp                                       */
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

#include "solers_llm_provider_catalog.h"

#include "core/variant/variant.h"

void SolersLLMProviderCatalog::_define(const String &p_id, const String &p_label, const String &p_protocol, const String &p_base_url, const String &p_auth_header, const String &p_auth_prefix, const String &p_api_key_env, bool p_local) {
	Dictionary profile;
	profile["id"] = p_id;
	profile["label"] = p_label;
	profile["protocol"] = p_protocol;
	profile["base_url"] = p_base_url;
	profile["auth_header"] = p_auth_header;
	profile["auth_prefix"] = p_auth_prefix;
	profile["api_key_env"] = p_api_key_env;
	profile["local"] = p_local;
	profiles[StringName(p_id)] = profile;
}

bool SolersLLMProviderCatalog::has(const StringName &p_id) const {
	return profiles.has(p_id);
}

Dictionary SolersLLMProviderCatalog::get_profile(const StringName &p_id) const {
	const Dictionary *found = profiles.getptr(p_id);
	return found ? *found : Dictionary();
}

Array SolersLLMProviderCatalog::list_profiles() const {
	Array out;
	for (const KeyValue<StringName, Dictionary> &kv : profiles) {
		out.push_back(kv.value);
	}
	return out;
}

Dictionary SolersLLMProviderCatalog::resolve(const StringName &p_id, const String &p_base_url_override) const {
	// Known provider -> its profile; unknown id -> generic OpenAI-compatible
	// profile so any relay/self-hosted endpoint works by supplying base_url.
	Dictionary profile;
	if (profiles.has(p_id)) {
		profile = get_profile(p_id).duplicate();
	} else {
		profile = get_profile(StringName("openai-compatible")).duplicate();
		profile["id"] = String(p_id);
	}
	const String override_url = p_base_url_override.strip_edges();
	if (!override_url.is_empty()) {
		profile["base_url"] = override_url.trim_suffix("/");
	}
	return profile;
}

void SolersLLMProviderCatalog::register_builtin_profiles() {
	// id, label, protocol, base_url, auth_header, auth_prefix, api_key_env, local
	//
	// Every id the product-side SolersProviderRegistry exposes MUST resolve to
	// the correct wire protocol here. Before this alignment, ids like
	// "anthropic_messages" fell through to the OpenAI-compatible fallback and
	// spoke the wrong protocol at the right endpoint. Endpoints/headers
	// cross-checked against the providers' official docs (2026-06).
	_define("openai", "OpenAI", "openai-chat", "https://api.openai.com/v1", "Authorization", "Bearer ", "OPENAI_API_KEY", false);
	// Registry alias; Responses API rides Chat Completions until the dedicated
	// protocol lands in SolersLLMProtocolRegistry.
	_define("openai_responses", "OpenAI (Responses alias)", "openai-chat", "https://api.openai.com/v1", "Authorization", "Bearer ", "OPENAI_API_KEY", false);

	_define("anthropic", "Anthropic", "anthropic-messages", "https://api.anthropic.com", "x-api-key", "", "ANTHROPIC_API_KEY", false);
	_define("anthropic_messages", "Anthropic Messages", "anthropic-messages", "https://api.anthropic.com", "x-api-key", "", "ANTHROPIC_API_KEY", false);

	// OpenAI-compatible vendors — same wire protocol, different endpoints.
	_define("gemini", "Google Gemini", "openai-chat", "https://generativelanguage.googleapis.com/v1beta/openai", "Authorization", "Bearer ", "GEMINI_API_KEY", false);
	_define("deepseek", "DeepSeek", "openai-chat", "https://api.deepseek.com", "Authorization", "Bearer ", "DEEPSEEK_API_KEY", false);
	_define("qwen", "Qwen / DashScope", "openai-chat", "https://dashscope.aliyuncs.com/compatible-mode/v1", "Authorization", "Bearer ", "DASHSCOPE_API_KEY", false);

	// Local runtimes — no key required.
	_define("ollama", "Ollama", "openai-chat", "http://127.0.0.1:11434/v1", "Authorization", "Bearer ", "", true);
	_define("lm_studio", "LM Studio", "openai-chat", "http://127.0.0.1:1234/v1", "Authorization", "Bearer ", "", true);

	// Generic OpenAI-compatible: relays, gateways, Together, Groq, vLLM, ...
	// also the fallback profile for unknown ids (base_url supplied by config).
	_define("custom_openai_compatible", "Custom OpenAI-compatible", "openai-chat", "", "Authorization", "Bearer ", "", false);
	_define("openai-compatible", "OpenAI-compatible", "openai-chat", "", "Authorization", "Bearer ", "", false);
}
