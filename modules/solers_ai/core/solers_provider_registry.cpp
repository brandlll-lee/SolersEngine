/**************************************************************************/
/*  solers_provider_registry.cpp                                           */
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

#include "solers_provider_registry.h"

#include "core/object/class_db.h"
#include "core/os/os.h"

void SolersProviderRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_provider_profile", "provider"), &SolersProviderRegistry::get_provider_profile);
	ClassDB::bind_method(D_METHOD("resolve_provider_profile", "provider", "base_url_override"), &SolersProviderRegistry::resolve_provider_profile, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("list_provider_profiles"), &SolersProviderRegistry::list_provider_profiles);
	ClassDB::bind_method(D_METHOD("validate_config", "config"), &SolersProviderRegistry::validate_config);
	ClassDB::bind_method(D_METHOD("is_model_allowed", "provider", "model"), &SolersProviderRegistry::is_model_allowed);
}

Dictionary SolersProviderRegistry::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersProviderRegistry::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

Dictionary SolersProviderRegistry::_make_profile(const String &p_id, const String &p_label, const String &p_kind, const String &p_default_base_url, const String &p_default_model, bool p_local, bool p_api_key_required, const Array &p_features, const String &p_notes, const String &p_api_key_env, const String &p_catalog_provider, const String &p_protocol) const {
	Dictionary profile;
	profile["id"] = p_id;
	profile["label"] = p_label;
	profile["kind"] = p_kind;
	profile["protocol"] = p_protocol;
	profile["default_base_url"] = p_default_base_url;
	profile["default_model"] = p_default_model;
	profile["catalog_provider"] = p_catalog_provider.is_empty() ? p_id : p_catalog_provider;
	profile["local"] = p_local;
	profile["auth_type"] = p_local ? "none" : "api_key";
	profile["auth_header"] = "Authorization";
	profile["auth_prefix"] = "Bearer ";
	profile["api_key_required"] = p_api_key_required;
	profile["features"] = p_features;
	profile["notes"] = p_notes;
	profile["api_key_env"] = p_api_key_env;
	return profile;
}

void SolersProviderRegistry::_register_profile(const Dictionary &p_profile) {
	const String id = p_profile.get("id", String());
	ERR_FAIL_COND(id.is_empty());
	profiles[id] = p_profile;
	profile_order.push_back(id);
}

void SolersProviderRegistry::_register_default_profiles() {
	profiles.clear();
	profile_order.clear();

	Array codex_features;
	codex_features.push_back("responses");
	codex_features.push_back("tool_calls");
	codex_features.push_back("reasoning");
	codex_features.push_back("vision");
	Dictionary codex = _make_profile("openai_codex", "ChatGPT Codex", "openai_responses", "https://chatgpt.com/backend-api/codex", "gpt-5.5", false, false, codex_features, "Sign in with ChatGPT to use Codex through an eligible Plus, Pro, Business, Edu, or Enterprise plan.", String(), "openai", "openai-responses");
	codex["auth_type"] = "oauth";
	codex["oauth_kind"] = "codex";
	codex["supports_max_output_tokens"] = false;
	Dictionary codex_headers;
	codex_headers["originator"] = "solers";
	codex_headers["User-Agent"] = "Solers";
	codex["headers"] = codex_headers;
	Array codex_models;
	codex_models.push_back("gpt-5.5");
	codex_models.push_back("gpt-5.4");
	codex_models.push_back("gpt-5.4-mini");
	codex_models.push_back("gpt-5.3-codex-spark");
	codex["allowed_models"] = codex_models;
	Dictionary codex_model_labels;
	codex_model_labels["gpt-5.5"] = "GPT-5.5";
	codex_model_labels["gpt-5.4"] = "GPT-5.4";
	codex_model_labels["gpt-5.4-mini"] = "GPT-5.4 Mini";
	codex_model_labels["gpt-5.3-codex-spark"] = "GPT-5.3 Codex Spark";
	codex["model_labels"] = codex_model_labels;
	Dictionary codex_model_limits;
	Dictionary gpt55_limits;
	gpt55_limits["context"] = 400000;
	gpt55_limits["output"] = 128000;
	codex_model_limits["gpt-5.5"] = gpt55_limits;
	codex["model_limits"] = codex_model_limits;
	_register_profile(codex);

	Array openai_features;
	openai_features.push_back("chat_completions");
	openai_features.push_back("tool_calls");
	openai_features.push_back("structured_outputs");
	openai_features.push_back("vision");
	_register_profile(_make_profile("openai", "OpenAI", "openai_compatible", "https://api.openai.com/v1", "gpt-5", false, true, openai_features, "OpenAI Chat Completions provider. Solers uses the same OpenAI-compatible tool-call loop as custom gateways.", "OPENAI_API_KEY"));

	Array anthropic_features;
	anthropic_features.push_back("messages");
	anthropic_features.push_back("tool_calls");
	anthropic_features.push_back("vision");
	anthropic_features.push_back("extended_thinking");
	Dictionary anthropic = _make_profile("anthropic_messages", "Anthropic Messages", "anthropic_messages", "https://api.anthropic.com", "claude-sonnet-4.6", false, true, anthropic_features, "Use native Anthropic Messages for provider-specific features.", "ANTHROPIC_API_KEY", "anthropic", "anthropic-messages");
	anthropic["auth_header"] = "x-api-key";
	anthropic["auth_prefix"] = String();
	_register_profile(anthropic);

	Array gemini_features;
	gemini_features.push_back("openai_compatible");
	gemini_features.push_back("native_generate_content");
	gemini_features.push_back("vision");
	gemini_features.push_back("long_context");
	_register_profile(_make_profile("gemini", "Google Gemini", "openai_compatible", "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-2.5-pro", false, true, gemini_features, "OpenAI-compatible endpoint is useful for shared gateway code; native API can expose Gemini-specific features.", "GEMINI_API_KEY", "google"));

	Array deepseek_features;
	deepseek_features.push_back("openai_compatible");
	deepseek_features.push_back("reasoning");
	deepseek_features.push_back("streaming");
	_register_profile(_make_profile("deepseek", "DeepSeek", "openai_compatible", "https://api.deepseek.com", "deepseek-v4-pro", false, true, deepseek_features, "Provider model aliases can change; Solers validates shape, not remote availability.", "DEEPSEEK_API_KEY"));

	Array qwen_features;
	qwen_features.push_back("openai_compatible");
	qwen_features.push_back("long_context");
	qwen_features.push_back("vision");
	_register_profile(_make_profile("qwen", "Qwen / Alibaba Cloud Model Studio", "openai_compatible", "https://dashscope.aliyuncs.com/compatible-mode/v1", "qwen-plus", false, true, qwen_features, "Default is the Beijing-region DashScope compatible-mode endpoint; international accounts use dashscope-intl.aliyuncs.com.", "DASHSCOPE_API_KEY"));

	Array ollama_features;
	ollama_features.push_back("openai_compatible");
	ollama_features.push_back("local");
	ollama_features.push_back("chat_completions");
	_register_profile(_make_profile("ollama", "Ollama", "openai_compatible", "http://127.0.0.1:11434/v1", "qwen3:8b", true, false, ollama_features, "Local runtime. API key is required by some clients but ignored by Ollama."));

	Array lmstudio_features;
	lmstudio_features.push_back("openai_compatible");
	lmstudio_features.push_back("local");
	lmstudio_features.push_back("chat_completions");
	_register_profile(_make_profile("lm_studio", "LM Studio", "openai_compatible", "http://127.0.0.1:1234/v1", "", true, false, lmstudio_features, "Local server defaults to port 1234; model id depends on loaded model."));

	Array custom_features;
	custom_features.push_back("openai_compatible");
	custom_features.push_back("chat_completions");
	custom_features.push_back("user_defined");
	_register_profile(_make_profile("custom_openai_compatible", "Custom OpenAI-compatible", "openai_compatible", "", "", false, true, custom_features, "For LiteLLM, OpenRouter-style, vLLM, or private gateway deployments."));
}

Dictionary SolersProviderRegistry::get_provider_profile(const String &p_provider) const {
	if (!profiles.has(p_provider)) {
		return Dictionary();
	}
	return profiles[p_provider];
}

Dictionary SolersProviderRegistry::resolve_provider_profile(const String &p_provider, const String &p_base_url_override) const {
	Dictionary profile = get_provider_profile(p_provider);
	if (profile.is_empty()) {
		return Dictionary();
	}
	const String override_url = p_base_url_override.strip_edges();
	profile["base_url"] = override_url.is_empty() ? String(profile.get("default_base_url", String())) : override_url.trim_suffix("/");
	return profile;
}

Array SolersProviderRegistry::list_provider_profiles() const {
	Array result;
	for (const Variant &id : profile_order) {
		result.push_back(profiles.get(id, Dictionary()));
	}
	return result;
}

bool SolersProviderRegistry::is_model_allowed(const String &p_provider, const String &p_model) const {
	const Dictionary profile = get_provider_profile(p_provider);
	const Array allowed_models = profile.get("allowed_models", Array());
	return allowed_models.is_empty() || allowed_models.has(p_model.strip_edges());
}

Dictionary SolersProviderRegistry::validate_config(const Dictionary &p_config) const {
	const String provider = p_config.get("provider", "ollama");
	if (!profiles.has(provider)) {
		return _error("UNKNOWN_PROVIDER", vformat("Unknown Solers provider profile: %s", provider), true);
	}
	const Dictionary profile = profiles[provider];
	const bool privacy_mode = p_config.get("privacy_mode", true);
	const bool local = profile.get("local", false);
	const bool api_key_required = profile.get("api_key_required", true);
	String base_url = p_config.get("base_url", String());
	if (base_url.is_empty()) {
		base_url = profile.get("default_base_url", String());
	}
	String model = p_config.get("model", String());
	if (model.is_empty()) {
		model = profile.get("default_model", String());
	}
	const bool api_key_configured = p_config.get("api_key_configured", false) || !String(p_config.get("api_key", String())).is_empty();
	const bool oauth_configured = p_config.get("oauth_configured", false);

	// Human-facing diagnostics: routed through TTR so the editor UI (Project
	// Manager AI tab, Solers dock) renders them in the user's locale.
	Array warnings;
	Array blockers;
	if (privacy_mode && !local) {
		blockers.push_back(TTR("privacy_mode allows only local providers. Disable privacy_mode or choose ollama/lm_studio."));
	}
	// Env fallback counts as configured (api_key_env, e.g. OPENAI_API_KEY).
	bool api_key_available = api_key_configured;
	if (!api_key_available) {
		const String env_name = profile.get("api_key_env", String());
		if (!env_name.is_empty() && OS::get_singleton()->has_environment(env_name) && !OS::get_singleton()->get_environment(env_name).is_empty()) {
			api_key_available = true;
		}
	}
	if (String(profile.get("auth_type", String())) == "oauth" && !oauth_configured) {
		blockers.push_back(TTR("provider requires ChatGPT authorization."));
	} else if (api_key_required && !api_key_available) {
		blockers.push_back(TTR("provider requires an API key (set one in AI settings or via its environment variable)."));
	}
	if (base_url.is_empty()) {
		blockers.push_back(TTR("provider requires a base_url."));
	}
	if (model.is_empty()) {
		warnings.push_back(TTR("model is empty; Solers will need an explicit model before inference."));
	} else if (!is_model_allowed(provider, model)) {
		blockers.push_back(TTR("model is not available through ChatGPT Codex authentication."));
	}
	if (!base_url.is_empty() && !(base_url.begins_with("http://") || base_url.begins_with("https://"))) {
		blockers.push_back(TTR("base_url must start with http:// or https://."));
	}
	if (!local && base_url.begins_with("http://")) {
		warnings.push_back(TTR("remote provider base_url is not HTTPS."));
	}

	Dictionary data;
	data["provider"] = provider;
	data["profile"] = profile;
	data["valid"] = blockers.is_empty();
	data["warnings"] = warnings;
	data["blockers"] = blockers;
	data["effective_base_url"] = base_url;
	data["effective_model"] = model;
	return _ok(data);
}

SolersProviderRegistry::SolersProviderRegistry() {
	_register_default_profiles();
}
