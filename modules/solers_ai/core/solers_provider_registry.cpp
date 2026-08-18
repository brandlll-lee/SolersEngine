/**************************************************************************/
/*  solers_provider_registry.cpp                                          */
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
#include "core/templates/hash_set.h"

#include "modules/solers_ai/llm/solers_models_dev.h"

void SolersProviderRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_provider_profile", "provider"), &SolersProviderRegistry::get_provider_profile);
	ClassDB::bind_method(D_METHOD("resolve_provider_profile", "provider", "base_url_override", "model"), &SolersProviderRegistry::resolve_provider_profile, DEFVAL(String()), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("list_provider_profiles"), &SolersProviderRegistry::list_provider_profiles);
	ClassDB::bind_method(D_METHOD("list_popular_provider_ids"), &SolersProviderRegistry::list_popular_provider_ids);
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

String SolersProviderRegistry::_default_model_for_catalog(const Dictionary &p_catalog) const {
	const String declared = String(p_catalog.get("default_model", String())).strip_edges();
	if (!declared.is_empty()) {
		return declared;
	}
	const Dictionary models = p_catalog.get("models", Dictionary());
	const Array ids = models.keys();
	if (ids.is_empty()) {
		return String();
	}
	return String(ids[0]);
}

Dictionary SolersProviderRegistry::_profile_from_catalog(const Dictionary &p_catalog) const {
	const String id = p_catalog.get("id", String());
	const String npm = p_catalog.get("npm", String());
	String protocol = _protocol_for_package(npm);
	if (protocol.is_empty()) {
		protocol = p_catalog.get("protocol", String());
	}
	if (id.is_empty() || protocol.is_empty()) {
		return Dictionary();
	}
	Dictionary profile;
	profile["id"] = id;
	profile["label"] = p_catalog.get("name", id);
	profile["protocol"] = protocol;
	profile["default_base_url"] = String(p_catalog.get("api", String())).strip_edges().trim_suffix("/");
	profile["default_model"] = _default_model_for_catalog(p_catalog);
	profile["catalog_provider"] = id;
	const bool local = p_catalog.get("local", false);
	profile["local"] = local;
	profile["auth_type"] = local ? "none" : "api_key";
	profile["auth_header"] = p_catalog.get("auth_header", String());
	profile["auth_prefix"] = p_catalog.get("auth_prefix", String());
	profile["api_key_required"] = !local;
	const Array env = p_catalog.get("env", Array());
	profile["api_key_env"] = env.is_empty() ? String() : String(env[0]);
	profile["notes"] = String();
	profile["source_kind"] = "catalog";
	return profile;
}

String SolersProviderRegistry::_protocol_for_package(const String &p_package) const {
	static const struct {
		const char *package;
		const char *protocol;
	} ROUTES[] = {
		{ "@ai-sdk/openai", "openai-responses" },
		{ "@ai-sdk/openai-compatible", "openai-chat" },
		{ "@openrouter/ai-sdk-provider", "openai-chat" },
		{ "@ai-sdk/anthropic", "anthropic-messages" },
		{ "@ai-sdk/xai", "openai-responses" },
	};
	for (const auto &route : ROUTES) {
		if (p_package == route.package) {
			return route.protocol;
		}
	}
	return String();
}

Dictionary SolersProviderRegistry::_default_custom_profile(const String &p_id) const {
	Dictionary profile;
	profile["id"] = p_id;
	profile["label"] = p_id == "custom_openai_compatible" ? "Custom OpenAI-compatible" : p_id;
	profile["protocol"] = "openai-chat";
	profile["default_base_url"] = String();
	profile["default_model"] = String();
	profile["catalog_provider"] = p_id;
	profile["local"] = false;
	profile["auth_type"] = "api_key";
	profile["auth_header"] = "Authorization";
	profile["auth_prefix"] = "Bearer ";
	profile["api_key_required"] = true;
	profile["api_key_env"] = String();
	profile["notes"] = "OpenAI-compatible gateway (LiteLLM, OpenRouter-style, vLLM, or private deployments).";
	profile["source_kind"] = "custom";
	return profile;
}

void SolersProviderRegistry::_register_auth_hooks() {
	overlays.clear();
	overlay_order.clear();

	// Product overlay. OAuth behavior is registered independently by method id.
	Dictionary codex;
	codex["id"] = "openai_codex";
	codex["label"] = "ChatGPT Codex";
	codex["protocol"] = "openai-responses";
	codex["default_base_url"] = "https://chatgpt.com/backend-api/codex";
	codex["default_model"] = "gpt-5.6";
	codex["catalog_provider"] = "openai";
	codex["local"] = false;
	codex["auth_type"] = "oauth";
	Dictionary browser_auth;
	browser_auth["id"] = "chatgpt-browser";
	browser_auth["type"] = "oauth";
	browser_auth["label"] = "ChatGPT Pro/Plus (browser)";
	Array auth_methods;
	auth_methods.push_back(browser_auth);
	codex["auth_methods"] = auth_methods;
	codex["auth_header"] = "Authorization";
	codex["auth_prefix"] = "Bearer ";
	codex["api_key_required"] = false;
	codex["api_key_env"] = String();
	codex["supports_max_output_tokens"] = false;
	codex["notes"] = "Sign in with ChatGPT to use Codex through an eligible Plus, Pro, Business, Edu, or Enterprise plan.";
	codex["source_kind"] = "overlay";
	Dictionary codex_headers;
	codex_headers["originator"] = "solers";
	codex_headers["User-Agent"] = "Solers";
	codex["headers"] = codex_headers;
	Array codex_models;
	codex_models.push_back("gpt-5.6");
	codex_models.push_back("gpt-5.6-sol");
	codex_models.push_back("gpt-5.6-terra");
	codex_models.push_back("gpt-5.6-luna");
	codex_models.push_back("gpt-5.5");
	codex_models.push_back("gpt-5.4");
	codex_models.push_back("gpt-5.4-mini");
	codex_models.push_back("gpt-5.3-codex-spark");
	codex["allowed_models"] = codex_models;
	Dictionary codex_model_labels;
	codex_model_labels["gpt-5.6"] = "GPT-5.6";
	codex_model_labels["gpt-5.6-sol"] = "GPT-5.6 Sol";
	codex_model_labels["gpt-5.6-terra"] = "GPT-5.6 Terra";
	codex_model_labels["gpt-5.6-luna"] = "GPT-5.6 Luna";
	codex_model_labels["gpt-5.5"] = "GPT-5.5";
	codex_model_labels["gpt-5.4"] = "GPT-5.4";
	codex_model_labels["gpt-5.4-mini"] = "GPT-5.4 Mini";
	codex_model_labels["gpt-5.3-codex-spark"] = "GPT-5.3 Codex Spark";
	codex["model_labels"] = codex_model_labels;
	Dictionary codex_model_limits;
	Dictionary gpt56_limits;
	gpt56_limits["context"] = 1050000;
	gpt56_limits["output"] = 128000;
	codex_model_limits["gpt-5.6"] = gpt56_limits;
	codex_model_limits["gpt-5.6-sol"] = gpt56_limits;
	codex_model_limits["gpt-5.6-terra"] = gpt56_limits;
	codex_model_limits["gpt-5.6-luna"] = gpt56_limits;
	Dictionary gpt55_limits;
	gpt55_limits["context"] = 400000;
	gpt55_limits["output"] = 128000;
	codex_model_limits["gpt-5.5"] = gpt55_limits;
	codex["model_limits"] = codex_model_limits;
	overlays["openai_codex"] = codex;
	overlay_order.push_back("openai_codex");

	// Custom provider template.
	overlays["custom_openai_compatible"] = _default_custom_profile("custom_openai_compatible");
	overlay_order.push_back("custom_openai_compatible");
}

void SolersProviderRegistry::set_models_dev(SolersModelsDev *p_models_dev, bool p_owned) {
	if (owns_models_dev && models_dev && models_dev != p_models_dev) {
		memdelete(models_dev);
	}
	models_dev = p_models_dev;
	owns_models_dev = p_owned && p_models_dev != nullptr;
}

Dictionary SolersProviderRegistry::get_provider_profile(const String &p_provider) const {
	if (p_provider.is_empty()) {
		return Dictionary();
	}
	if (overlays.has(p_provider)) {
		return overlays[p_provider];
	}
	if (models_dev) {
		const Dictionary catalog = models_dev->get_provider(StringName(p_provider));
		if (!catalog.is_empty()) {
			return _profile_from_catalog(catalog);
		}
		// Logo / catalog id aliases (data map, not behavior branches).
		const String canonical = models_dev->canonical_provider_id(p_provider);
		if (canonical != p_provider) {
			const Dictionary aliased = models_dev->get_provider(StringName(canonical));
			if (!aliased.is_empty()) {
				Dictionary profile = _profile_from_catalog(aliased);
				profile["id"] = p_provider;
				profile["catalog_provider"] = canonical;
				return profile;
			}
		}
	}
	return Dictionary();
}

Dictionary SolersProviderRegistry::resolve_provider_profile(const String &p_provider, const String &p_base_url_override, const String &p_model) const {
	Dictionary profile = get_provider_profile(p_provider);
	if (profile.is_empty()) {
		return Dictionary();
	}
	String default_url = String(profile.get("default_base_url", String())).trim_suffix("/");
	if (!p_model.is_empty() && models_dev) {
		const Dictionary model = models_dev->get_model(StringName(profile.get("catalog_provider", p_provider)), p_model);
		const String package = model.get("provider_npm", String());
		if (!package.is_empty()) {
			profile["protocol"] = _protocol_for_package(package);
		}
		const String model_url = model.get("provider_api", String());
		if (!model_url.is_empty()) {
			default_url = model_url.trim_suffix("/");
		}
		profile["model_id"] = model.get("id", p_model);
	}
	const String override_url = p_base_url_override.strip_edges();
	String base_url = (override_url.is_empty() ? default_url : override_url).trim_suffix("/");
	if (String(profile.get("protocol", String())) == "anthropic-messages") {
		base_url = base_url.trim_suffix("/v1");
	}
	profile["base_url"] = base_url;
	return profile;
}

Array SolersProviderRegistry::list_provider_profiles() const {
	Array result;
	HashSet<String> seen;
	for (const Variant &id_v : overlay_order) {
		const String id = id_v;
		result.push_back(get_provider_profile(id));
		seen.insert(id);
	}
	for (const Variant &id_v : list_popular_provider_ids()) {
		const String id = id_v;
		if (!seen.has(id)) {
			const Dictionary profile = get_provider_profile(id);
			if (!profile.is_empty()) {
				result.push_back(profile);
				seen.insert(id);
			}
		}
	}
	return result;
}

Array SolersProviderRegistry::list_popular_provider_ids() const {
	return models_dev ? models_dev->list_popular_provider_ids() : Array();
}

Array SolersProviderRegistry::list_overlay_provider_ids() const {
	return overlay_order.duplicate();
}

bool SolersProviderRegistry::is_model_allowed(const String &p_provider, const String &p_model) const {
	// AuthHook overlays may declare an allowlist. Catalog providers have none —
	// do not pull a full provider profile (and formerly a models table) per id.
	if (overlays.has(p_provider)) {
		const Dictionary overlay = overlays[p_provider];
		const Array allowed_models = overlay.get("allowed_models", Array());
		return allowed_models.is_empty() || allowed_models.has(p_model.strip_edges());
	}
	if (!models_dev) {
		return true;
	}
	const Dictionary profile = get_provider_profile(p_provider);
	const Dictionary model = models_dev->get_model(StringName(profile.get("catalog_provider", p_provider)), p_model);
	return model.is_empty() || (String(model.get("status", "active")) != "deprecated" && !String(resolve_provider_profile(p_provider, String(), p_model).get("protocol", String())).is_empty());
}

Dictionary SolersProviderRegistry::validate_config(const Dictionary &p_config) const {
	const String provider = p_config.get("provider", String());
	if (provider.is_empty()) {
		return _error("PROVIDER_REQUIRED", "A provider must be selected.", true);
	}
	const Dictionary profile = get_provider_profile(provider);
	if (profile.is_empty()) {
		return _error("PROVIDER_CONNECTION_UNDECLARED", "Provider connection protocol and authentication are not declared.");
	}
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

	Array warnings;
	Array blockers;
	bool api_key_available = api_key_configured;
	if (!api_key_available) {
		const String env_name = profile.get("api_key_env", String());
		if (!env_name.is_empty() && OS::get_singleton()->has_environment(env_name) && !OS::get_singleton()->get_environment(env_name).is_empty()) {
			api_key_available = true;
		}
	}
	if (String(profile.get("auth_type", String())) == "oauth" && !oauth_configured) {
		blockers.push_back(TTR("provider requires authorization."));
	} else if (api_key_required && !api_key_available) {
		blockers.push_back(TTR("provider requires an API key (set one in AI settings or via its environment variable)."));
	}
	if (base_url.is_empty()) {
		blockers.push_back(TTR("provider requires a base_url."));
	}
	if (model.is_empty()) {
		warnings.push_back(TTR("model is empty; Solers will need an explicit model before inference."));
	} else if (!is_model_allowed(provider, model)) {
		blockers.push_back(TTR("model is deprecated or its provider protocol is not supported."));
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
	models_dev = memnew(SolersModelsDev);
	models_dev->initialize();
	owns_models_dev = true;
	_register_auth_hooks();
}

SolersProviderRegistry::~SolersProviderRegistry() {
	if (owns_models_dev && models_dev) {
		memdelete(models_dev);
		models_dev = nullptr;
	}
}
