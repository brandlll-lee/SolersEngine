/**************************************************************************/
/*  solers_settings_service.cpp                                           */
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

#include "solers_settings_service.h"

#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "editor/settings/editor_settings.h"

#include "modules/solers_ai/core/solers_codex_auth.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/solers_ai/llm/solers_models_dev.h"

static constexpr int SOLERS_PROVIDER_SETTINGS_VERSION = 6;

void SolersSettingsService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_provider_registry", "provider_registry"), &SolersSettingsService::set_provider_registry);
	ClassDB::bind_method(D_METHOD("get_local_models_only"), &SolersSettingsService::get_local_models_only);
	ClassDB::bind_method(D_METHOD("set_local_models_only", "enabled"), &SolersSettingsService::set_local_models_only);
	ClassDB::bind_method(D_METHOD("get_provider_config"), &SolersSettingsService::get_provider_config);
	ClassDB::bind_method(D_METHOD("get_provider_config_for", "provider"), &SolersSettingsService::get_provider_config_for);
	ClassDB::bind_method(D_METHOD("set_provider_config", "args"), &SolersSettingsService::set_provider_config);
	ClassDB::bind_method(D_METHOD("disconnect_provider", "provider"), &SolersSettingsService::disconnect_provider);
	ClassDB::bind_method(D_METHOD("list_provider_profiles"), &SolersSettingsService::list_provider_profiles);
	ClassDB::bind_method(D_METHOD("list_connected_provider_configs"), &SolersSettingsService::list_connected_provider_configs);
	ClassDB::bind_method(D_METHOD("list_provider_view"), &SolersSettingsService::list_provider_view);
	ClassDB::bind_method(D_METHOD("validate_provider_config", "args"), &SolersSettingsService::validate_provider_config);
	ClassDB::bind_method(D_METHOD("start_codex_login", "provider"), &SolersSettingsService::start_codex_login);
	ClassDB::bind_method(D_METHOD("cancel_codex_login"), &SolersSettingsService::cancel_codex_login);
	ClassDB::bind_method(D_METHOD("get_codex_auth_status", "provider"), &SolersSettingsService::get_codex_auth_status);
	ClassDB::bind_method(D_METHOD("disconnect_codex", "provider"), &SolersSettingsService::disconnect_codex);
}

Dictionary SolersSettingsService::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersSettingsService::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

String SolersSettingsService::_setting_path(const String &p_key) const {
	return "solers/ai/" + p_key;
}

String SolersSettingsService::_provider_setting_path(const String &p_provider, const String &p_key) const {
	return _setting_path("providers/" + p_provider + "/" + p_key);
}

void SolersSettingsService::_migrate_provider_settings() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings) {
		return;
	}
	const int version = settings->has_setting(_setting_path("settings_version")) ? (int)settings->get_setting(_setting_path("settings_version")) : 0;
	if (version >= SOLERS_PROVIDER_SETTINGS_VERSION) {
		return;
	}

	String provider = settings->has_setting(_setting_path("provider")) ? String(settings->get_setting(_setting_path("provider"))) : String();
	const String previous_provider = provider;
	if (version < 5 && provider == "openai_responses") {
		provider = "openai";
	} else if (version < 5 && provider == "anthropic") {
		provider = "anthropic_messages";
	} else if (provider == "custom_openai_responses" || (version < 6 && !provider.is_empty() && provider_registry && provider_registry->get_provider_profile(provider).is_empty())) {
		provider = "custom_openai_compatible";
	}
	if (version >= 2 && !previous_provider.is_empty() && previous_provider != provider) {
		static const char *PROVIDER_KEYS[] = { "configured", "model", "reasoning_effort", "base_url", "api_key", "oauth" };
		for (const char *key : PROVIDER_KEYS) {
			const String old_path = _provider_setting_path(previous_provider, key);
			if (settings->has_setting(old_path)) {
				settings->set_manually(_provider_setting_path(provider, key), settings->get_setting(old_path));
				settings->erase(old_path);
			}
		}
		settings->set_manually(_setting_path("provider"), provider);
	}
	const bool has_legacy_config = version < 2 && (!provider.is_empty() || settings->has_setting(_setting_path("model")) || settings->has_setting(_setting_path("base_url")) || settings->has_setting(_setting_path("api_key")));
	if (has_legacy_config) {
		if (provider.is_empty()) {
			provider = "ollama";
		}
		settings->set_manually(_setting_path("provider"), provider);
		settings->set_manually(_provider_setting_path(provider, "configured"), true);
		if (settings->has_setting(_setting_path("model"))) {
			settings->set_manually(_provider_setting_path(provider, "model"), settings->get_setting(_setting_path("model")));
			settings->erase(_setting_path("model"));
		}
		if (settings->has_setting(_setting_path("base_url"))) {
			settings->set_manually(_provider_setting_path(provider, "base_url"), settings->get_setting(_setting_path("base_url")));
			settings->erase(_setting_path("base_url"));
		}
		if (settings->has_setting(_setting_path("api_key"))) {
			settings->set_manually(_provider_setting_path(provider, "api_key"), settings->get_setting(_setting_path("api_key")));
			settings->erase(_setting_path("api_key"));
		}
	}
	if (version < 4) {
		const String old_path = _setting_path("privacy_mode");
		const String new_path = _setting_path("local_models_only");
		if (settings->has_setting(old_path) && !settings->has_setting(new_path)) {
			settings->set_manually(new_path, settings->get_setting(old_path));
		}
		if (settings->has_setting(old_path)) {
			settings->erase(old_path);
		}
	}
	if (version < 5) {
		// Single alias authority: SolersModelsDev::canonical_provider_id.
		SolersModelsDev *md = provider_registry ? provider_registry->get_models_dev() : nullptr;
		if (md) {
			static const char *PROVIDER_KEYS[] = { "configured", "model", "base_url", "api_key", "oauth", "reasoning_effort" };
			for (const Variant &from_v : md->list_legacy_provider_ids()) {
				const String from = from_v;
				const String to = md->canonical_provider_id(from);
				if (from.is_empty() || to.is_empty() || to == from) {
					continue;
				}
				for (const char *key : PROVIDER_KEYS) {
					const String old_path = _provider_setting_path(from, key);
					if (!settings->has_setting(old_path)) {
						continue;
					}
					const String new_path = _provider_setting_path(to, key);
					if (!settings->has_setting(new_path)) {
						settings->set_manually(new_path, settings->get_setting(old_path));
					}
					settings->erase(old_path);
				}
				if (provider == from) {
					provider = to;
					settings->set_manually(_setting_path("provider"), provider);
				}
			}
		}
	}
	if (settings->has_setting(_setting_path("custom_provider_ids"))) {
		settings->erase(_setting_path("custom_provider_ids"));
	}
	settings->set_manually(_setting_path("settings_version"), SOLERS_PROVIDER_SETTINGS_VERSION);
	EditorSettings::save();
}

Dictionary SolersSettingsService::_get_stored_auth(const String &p_provider) const {
	Dictionary auth;
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !provider_registry) {
		return auth;
	}
	const Dictionary profile = provider_registry->get_provider_profile(p_provider);
	const String auth_type = profile.get("auth_type", "api_key");
	if (auth_type == "none") {
		auth["type"] = "none";
		return auth;
	}
	if (auth_type == "oauth") {
		const String path = _provider_setting_path(p_provider, "oauth");
		if (!settings->has_setting(path)) {
			return auth;
		}
		const String stored = settings->get_setting(path);
		if (!SolersSecretStore::is_protected(stored)) {
			return auth;
		}
		const String json = SolersSecretStore::unprotect(stored);
		const Variant parsed = JSON::parse_string(json);
		return parsed.get_type() == Variant::DICTIONARY ? Dictionary(parsed) : Dictionary();
	}

	String key;
	String source = "none";
	const String path = _provider_setting_path(p_provider, "api_key");
	if (settings->has_setting(path)) {
		key = SolersSecretStore::unprotect(String(settings->get_setting(path)));
		if (!key.is_empty()) {
			source = "settings";
		}
	}
	if (key.is_empty()) {
		const String env_name = profile.get("api_key_env", String());
		if (!env_name.is_empty() && OS::get_singleton()->has_environment(env_name)) {
			key = OS::get_singleton()->get_environment(env_name);
			if (!key.is_empty()) {
				source = "environment";
			}
		}
	}
	auth["type"] = "api_key";
	auth["key"] = key;
	auth["source"] = source;
	return auth;
}

Dictionary SolersSettingsService::_get_provider_config(const String &p_provider, bool p_include_secret) const {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !provider_registry || p_provider.is_empty()) {
		return Dictionary();
	}
	const Dictionary profile = provider_registry->get_provider_profile(p_provider);
	if (profile.is_empty()) {
		return Dictionary();
	}

	Dictionary data;
	data["provider"] = p_provider;
	data["profile"] = profile;
	data["local_models_only"] = get_local_models_only();
	data["configured"] = settings->has_setting(_provider_setting_path(p_provider, "configured")) && (bool)settings->get_setting(_provider_setting_path(p_provider, "configured"));
	data["model"] = settings->has_setting(_provider_setting_path(p_provider, "model")) ? String(settings->get_setting(_provider_setting_path(p_provider, "model"))) : String(profile.get("default_model", String()));
	data["reasoning_effort"] = settings->has_setting(_provider_setting_path(p_provider, "reasoning_effort")) ? String(settings->get_setting(_provider_setting_path(p_provider, "reasoning_effort"))) : String();
	data["base_url"] = settings->has_setting(_provider_setting_path(p_provider, "base_url")) ? String(settings->get_setting(_provider_setting_path(p_provider, "base_url"))) : String(profile.get("default_base_url", String()));
	for (const String &key : { String("context_window"), String("max_tokens") }) {
		const String path = _provider_setting_path(p_provider, key);
		if (settings->has_setting(path) && (int)settings->get_setting(path) > 0) {
			data[key] = settings->get_setting(path);
		}
	}

	// Credential PRESENCE only: a stored blob / env var existing is the
	// authoritative signal that a credential was configured. Decryption
	// (DPAPI is an lsass RPC per call) happens exclusively at request time in
	// resolve_active_provider(); status/UI paths must stay crypto-free. An
	// undecryptable blob (settings copied across machines) thus reads as
	// configured here and fails loudly at first request instead.
	const String auth_type = profile.get("auth_type", "api_key");
	bool api_key_configured = false;
	String api_key_source = "none";
	if (auth_type == "api_key") {
		const String key_path = _provider_setting_path(p_provider, "api_key");
		if (settings->has_setting(key_path) && !String(settings->get_setting(key_path)).is_empty()) {
			api_key_configured = true;
			api_key_source = "settings";
		} else {
			const String env_name = profile.get("api_key_env", String());
			if (!env_name.is_empty() && OS::get_singleton()->has_environment(env_name) && !OS::get_singleton()->get_environment(env_name).is_empty()) {
				api_key_configured = true;
				api_key_source = "environment";
			}
		}
	}
	bool oauth_configured = false;
	if (auth_type == "oauth") {
		const String oauth_path = _provider_setting_path(p_provider, "oauth");
		oauth_configured = settings->has_setting(oauth_path) && SolersSecretStore::is_protected(String(settings->get_setting(oauth_path)));
	}
	data["api_key_configured"] = api_key_configured;
	data["api_key_source"] = api_key_source;
	data["oauth_configured"] = oauth_configured;
	data["api_key"] = "<redacted>";

	const Dictionary validation = provider_registry->validate_config(data).get("data", Dictionary());
	data["validation"] = validation;

	const bool credential_ready = auth_type == "none" || api_key_configured || oauth_configured;
	const bool connection_declared = auth_type == "none" ? (bool)data["configured"] : credential_ready;
	const bool connected = connection_declared && validation.get("valid", false);
	data["connected"] = connected;
	data["available"] = connected && (!(bool)data["local_models_only"] || (bool)profile.get("local", false));
	data["active"] = settings->has_setting(_setting_path("provider")) && String(settings->get_setting(_setting_path("provider"))) == p_provider;
	String source = "none";
	if (auth_type == "oauth" && oauth_configured) {
		source = "oauth";
	} else if (api_key_source == "environment") {
		source = "env";
	} else if (api_key_configured) {
		source = String(profile.get("source_kind", String())) == "custom" ? "custom" : "api";
	} else if (auth_type == "none" && connection_declared) {
		source = "config";
	}
	data["source"] = source;
	if (p_include_secret) {
		data["auth"] = _get_stored_auth(p_provider);
	}
	return data;
}

void SolersSettingsService::set_provider_registry(SolersProviderRegistry *p_provider_registry) {
	provider_registry = p_provider_registry;
	_migrate_provider_settings();
}

bool SolersSettingsService::get_local_models_only() const {
	EditorSettings *settings = EditorSettings::get_singleton();
	return settings && settings->has_setting(_setting_path("local_models_only")) && (bool)settings->get_setting(_setting_path("local_models_only"));
}

Dictionary SolersSettingsService::set_local_models_only(bool p_enabled) {
	EditorSettings *settings = EditorSettings::get_singleton();
	ERR_FAIL_NULL_V(settings, _error("EDITOR_SETTINGS_UNAVAILABLE", "EditorSettings is not available.", false));
	if (get_local_models_only() == p_enabled) {
		Dictionary data;
		data["local_models_only"] = p_enabled;
		return _ok(data);
	}
	settings->set_manually(_setting_path("local_models_only"), p_enabled);
	settings->emit_signal(SNAME("settings_changed"));
	settings->notify_changes();
	EditorSettings::save();
	Dictionary data;
	data["local_models_only"] = p_enabled;
	return _ok(data);
}

Dictionary SolersSettingsService::get_provider_config() const {
	EditorSettings *settings = EditorSettings::get_singleton();
	ERR_FAIL_NULL_V(settings, _error("EDITOR_SETTINGS_UNAVAILABLE", "EditorSettings is not available.", false));
	const String provider = settings->has_setting(_setting_path("provider")) ? String(settings->get_setting(_setting_path("provider"))) : String();
	return _ok(_get_provider_config(provider, false));
}

Dictionary SolersSettingsService::get_provider_config_for(const String &p_provider) const {
	return _ok(_get_provider_config(p_provider, false));
}

Dictionary SolersSettingsService::set_provider_config(const Dictionary &p_args) {
	EditorSettings *settings = EditorSettings::get_singleton();
	ERR_FAIL_NULL_V(settings, _error("EDITOR_SETTINGS_UNAVAILABLE", "EditorSettings is not available.", false));
	String provider = p_args.get("provider", String());
	if (provider.is_empty() && settings->has_setting(_setting_path("provider"))) {
		provider = settings->get_setting(_setting_path("provider"));
	}
	if (provider.is_empty()) {
		return _error("PROVIDER_REQUIRED", "A provider must be selected.");
	}
	if (!provider_registry) {
		return _error("PROVIDER_REGISTRY_UNAVAILABLE", "Solers provider registry is not initialized.", false);
	}
	if (p_args.has("model") && !provider_registry->is_model_allowed(provider, String(p_args["model"]))) {
		return _error("MODEL_NOT_ALLOWED", "The selected model is not available through this provider connection.");
	}
	const Dictionary profile = provider_registry->get_provider_profile(provider);
	if (profile.is_empty()) {
		return _error("PROVIDER_CONNECTION_UNDECLARED", "Choose a catalog provider or the Custom OpenAI-compatible connection.");
	}
	const bool was_connected = _get_provider_config(provider, false).get("connected", false);
	settings->set_manually(_setting_path("provider"), provider);
	settings->set_manually(_provider_setting_path(provider, "configured"), true);
	if (p_args.has("model")) {
		const String model_path = _provider_setting_path(provider, "model");
		const String previous_model = settings->has_setting(model_path) ? String(settings->get_setting(model_path)) : String(provider_registry->get_provider_profile(provider).get("default_model", String()));
		const String model = String(p_args["model"]);
		settings->set_manually(model_path, model);
		if (model != previous_model && !p_args.has("reasoning_effort")) {
			settings->erase(_provider_setting_path(provider, "reasoning_effort"));
		}
	}
	if (p_args.has("reasoning_effort")) {
		const String effort_path = _provider_setting_path(provider, "reasoning_effort");
		const String effort = String(p_args["reasoning_effort"]).strip_edges();
		if (effort.is_empty()) {
			settings->erase(effort_path);
		} else {
			settings->set_manually(effort_path, effort);
		}
	}
	if (p_args.has("base_url")) {
		settings->set_manually(_provider_setting_path(provider, "base_url"), String(p_args["base_url"]));
	}
	for (const String &key : { String("context_window"), String("max_tokens") }) {
		if (!p_args.has(key)) {
			continue;
		}
		const String path = _provider_setting_path(provider, key);
		const int value = p_args[key];
		if (value > 0) {
			settings->set_manually(path, value);
		} else {
			settings->erase(path);
		}
	}
	if (p_args.has("api_key") && !String(p_args["api_key"]).is_empty()) {
		settings->set_manually(_provider_setting_path(provider, "api_key"), SolersSecretStore::protect(String(p_args["api_key"])));
	}
	Dictionary data = _get_provider_config(provider, false);
	if (!was_connected && data.get("connected", false) && !profile.get("local", false) && get_local_models_only()) {
		settings->set_manually(_setting_path("local_models_only"), false);
		data = _get_provider_config(provider, false);
	}
	settings->emit_signal(SNAME("settings_changed"));
	settings->notify_changes();
	EditorSettings::save();
	data["saved"] = true;
	return _ok(data);
}

Dictionary SolersSettingsService::disconnect_provider(const String &p_provider) {
	EditorSettings *settings = EditorSettings::get_singleton();
	ERR_FAIL_NULL_V(settings, _error("EDITOR_SETTINGS_UNAVAILABLE", "EditorSettings is not available.", false));
	static const char *KEYS[] = { "configured", "model", "reasoning_effort", "base_url", "context_window", "max_tokens", "api_key", "oauth" };
	for (const char *key : KEYS) {
		const String path = _provider_setting_path(p_provider, key);
		if (settings->has_setting(path)) {
			settings->erase(path);
		}
	}
	const bool was_active = settings->has_setting(_setting_path("provider")) && String(settings->get_setting(_setting_path("provider"))) == p_provider;
	if (was_active) {
		settings->erase(_setting_path("provider"));
	}
	EditorSettings::save();

	if (was_active) {
		const Dictionary remaining_data = list_connected_provider_configs().get("data", Dictionary());
		const Array remaining = remaining_data.get("providers", Array());
		for (const Variant &config_value : remaining) {
			const Dictionary config = config_value;
			if (config.get("available", false)) {
				settings->set_manually(_setting_path("provider"), config.get("provider", String()));
				break;
			}
		}
	}
	settings->emit_signal(SNAME("settings_changed"));
	settings->notify_changes();
	EditorSettings::save();
	Dictionary data;
	data["provider"] = p_provider;
	data["disconnected"] = true;
	return _ok(data);
}

Dictionary SolersSettingsService::list_provider_profiles() const {
	ERR_FAIL_NULL_V(provider_registry, _error("PROVIDER_REGISTRY_UNAVAILABLE", "Solers provider registry is not initialized.", false));
	Dictionary data;
	data["profiles"] = provider_registry->list_provider_profiles();
	data["count"] = ((Array)data["profiles"]).size();
	return _ok(data);
}

Dictionary SolersSettingsService::list_connected_provider_configs() const {
	ERR_FAIL_NULL_V(provider_registry, _error("PROVIDER_REGISTRY_UNAVAILABLE", "Solers provider registry is not initialized.", false));
	Array connected;
	HashSet<String> seen;
	Array candidates;

	auto push_candidate = [&](const String &p_id) {
		if (p_id.is_empty() || seen.has(p_id)) {
			return;
		}
		seen.insert(p_id);
		Dictionary stub;
		stub["id"] = p_id;
		candidates.push_back(stub);
	};

	// AuthHook overlays + custom slots + active provider — never walk the full
	// models.dev catalog just to discover who is connected.
	for (const Variant &id_v : provider_registry->list_overlay_provider_ids()) {
		push_candidate(id_v);
	}
	EditorSettings *settings = EditorSettings::get_singleton();
	if (settings) {
		if (settings->has_setting(_setting_path("provider"))) {
			push_candidate(String(settings->get_setting(_setting_path("provider"))));
		}
		// Authoritative: any providers/<id>/configured|api_key|oauth key present.
		List<PropertyInfo> props;
		settings->get_property_list(&props);
		const String prefix = _setting_path("providers/");
		for (const PropertyInfo &pi : props) {
			if (!String(pi.name).begins_with(prefix)) {
				continue;
			}
			const String rest = String(pi.name).substr(prefix.length());
			const int slash = rest.find("/");
			if (slash <= 0) {
				continue;
			}
			const String key = rest.substr(slash + 1);
			if (key != "configured" && key != "api_key" && key != "oauth") {
				continue;
			}
			push_candidate(rest.substr(0, slash));
		}
	}

	seen.clear();
	for (const Variant &profile_value : candidates) {
		const String provider = Dictionary(profile_value).get("id", String());
		if (provider.is_empty() || seen.has(provider)) {
			continue;
		}
		seen.insert(provider);
		const Dictionary config = _get_provider_config(provider, false);
		if (config.get("connected", false)) {
			connected.push_back(config);
		}
	}
	Dictionary data;
	data["providers"] = connected;
	data["count"] = connected.size();
	return _ok(data);
}

Dictionary SolersSettingsService::list_provider_view() const {
	ERR_FAIL_NULL_V(provider_registry, _error("PROVIDER_REGISTRY_UNAVAILABLE", "Solers provider registry is not initialized.", false));
	if (SolersModelsDev *md = provider_registry->get_models_dev()) {
		md->refresh();
	}

	const Dictionary connected_wrap = list_connected_provider_configs().get("data", Dictionary());
	const Array connected = connected_wrap.get("providers", Array());
	HashSet<String> connected_ids;
	for (const Variant &c : connected) {
		connected_ids.insert(String(Dictionary(c).get("provider", String())));
	}

	Array popular;
	HashSet<String> popular_ids_seen;
	auto push_popular = [&](const String &p_id) {
		if (p_id.is_empty() || connected_ids.has(p_id) || popular_ids_seen.has(p_id)) {
			return;
		}
		const Dictionary profile = provider_registry->get_provider_profile(p_id);
		if (profile.is_empty()) {
			return;
		}
		// Custom template belongs in the custom slot, not Popular.
		if (String(profile.get("source_kind", String())) == "custom") {
			return;
		}
		popular_ids_seen.insert(p_id);
		Dictionary row;
		row["provider"] = p_id;
		row["profile"] = profile;
		row["connected"] = false;
		row["source"] = "none";
		popular.push_back(row);
	};

	// AuthHook overlays first (source_kind=overlay) — no per-id case branches.
	for (const Variant &id_v : provider_registry->list_overlay_provider_ids()) {
		push_popular(id_v);
	}
	// Catalog popular ordering (data in models.dev, not a behavior switch).
	for (const Variant &id_v : provider_registry->list_popular_provider_ids()) {
		push_popular(id_v);
	}

	Dictionary custom_row;
	custom_row["provider"] = "custom_openai_compatible";
	custom_row["profile"] = provider_registry->get_provider_profile("custom_openai_compatible");
	custom_row["connected"] = connected_ids.has("custom_openai_compatible");
	custom_row["source"] = custom_row.get("connected", false) ? "custom" : "none";

	Dictionary data;
	data["connected"] = connected;
	data["popular"] = popular;
	data["all"] = provider_registry->list_provider_profiles();
	data["custom"] = custom_row;
	return _ok(data);
}

Dictionary SolersSettingsService::validate_provider_config(const Dictionary &p_args) const {
	ERR_FAIL_NULL_V(provider_registry, _error("PROVIDER_REGISTRY_UNAVAILABLE", "Solers provider registry is not initialized.", false));
	String provider = p_args.get("provider", String());
	if (provider.is_empty()) {
		const Dictionary active = get_provider_config().get("data", Dictionary());
		provider = active.get("provider", String());
	}
	Dictionary config = _get_provider_config(provider, false);
	for (const Variant *key = p_args.next(nullptr); key; key = p_args.next(key)) {
		config[*key] = p_args[*key];
	}
	return provider_registry->validate_config(config);
}

Dictionary SolersSettingsService::resolve_provider_profile(const String &p_provider, const String &p_base_url_override) const {
	return provider_registry ? provider_registry->resolve_provider_profile(p_provider, p_base_url_override) : Dictionary();
}

bool SolersSettingsService::is_model_allowed(const String &p_provider, const String &p_model) const {
	return provider_registry && provider_registry->is_model_allowed(p_provider, p_model);
}

Dictionary SolersSettingsService::start_codex_login(const String &p_provider) {
	ERR_FAIL_NULL_V(codex_auth, _error("OAUTH_UNAVAILABLE", "ChatGPT authorization is unavailable.", false));
	ERR_FAIL_NULL_V(provider_registry, _error("PROVIDER_REGISTRY_UNAVAILABLE", "Solers provider registry is not initialized.", false));
	const Dictionary profile = provider_registry->get_provider_profile(p_provider);
	if (String(profile.get("oauth_kind", String())) != "codex") {
		return _error("OAUTH_PROFILE_INVALID", "The selected provider does not use ChatGPT Codex authorization.");
	}
	codex_provider = p_provider;
	auth_error = String();
	return codex_auth->start();
}

void SolersSettingsService::poll_auth() {
	if (!codex_auth) {
		return;
	}
	codex_auth->poll();
	const Dictionary tokens = codex_auth->take_tokens();
	if (tokens.is_empty()) {
		return;
	}
	const Dictionary profile = provider_registry ? provider_registry->get_provider_profile(codex_provider) : Dictionary();
	if (codex_provider.is_empty() || profile.is_empty()) {
		return;
	}
	if (!store_provider_auth(codex_provider, tokens)) {
		auth_error = "ChatGPT credentials could not be protected by the operating system and were not stored.";
		return;
	}
	Dictionary config;
	config["provider"] = codex_provider;
	config["model"] = profile.get("default_model", String());
	config["base_url"] = profile.get("default_base_url", String());
	set_local_models_only(false); // Completing OAuth is explicit consent to remote model access.
	set_provider_config(config);
}

void SolersSettingsService::cancel_codex_login() {
	if (codex_auth) {
		codex_auth->cancel();
	}
}

Dictionary SolersSettingsService::get_codex_auth_status(const String &p_provider) const {
	Dictionary status = codex_auth ? codex_auth->get_status() : Dictionary();
	const Dictionary config = _get_provider_config(p_provider, false);
	status["connected"] = config.get("connected", false);
	status["available"] = config.get("available", false);
	if (!auth_error.is_empty()) {
		status["state"] = "failed";
		status["error"] = auth_error;
	}
	return status;
}

bool SolersSettingsService::is_auth_active() const {
	return codex_auth && codex_auth->is_active();
}

Dictionary SolersSettingsService::disconnect_codex(const String &p_provider) {
	cancel_codex_login();
	auth_error = String();
	return disconnect_provider(p_provider);
}

bool SolersSettingsService::store_provider_auth(const String &p_provider, const Dictionary &p_auth) {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || p_auth.is_empty()) {
		return false;
	}
	const String protected_auth = SolersSecretStore::protect_strict(JSON::stringify(p_auth, "", false, true));
	if (protected_auth.is_empty()) {
		return false;
	}
	settings->set_manually(_provider_setting_path(p_provider, "oauth"), protected_auth);
	settings->set_manually(_provider_setting_path(p_provider, "configured"), true);
	settings->emit_signal(SNAME("settings_changed"));
	settings->notify_changes();
	EditorSettings::save();
	return true;
}

Dictionary SolersSettingsService::resolve_active_provider() const {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting(_setting_path("provider"))) {
		return Dictionary();
	}
	return _get_provider_config(String(settings->get_setting(_setting_path("provider"))), true);
}

SolersSettingsService::SolersSettingsService() {
	codex_auth = memnew(SolersCodexAuth);
}

SolersSettingsService::~SolersSettingsService() {
	if (codex_auth) {
		memdelete(codex_auth);
		codex_auth = nullptr;
	}
}
