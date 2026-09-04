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

#include "modules/solers_ai/core/solers_settings_service.h"

#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "editor/settings/editor_settings.h"

#include "modules/solers_ai/core/solers_codex_auth.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/solers_ai/llm/solers_model_catalog.h"

static constexpr int SOLERS_PROVIDER_SETTINGS_VERSION = 7;

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
	ClassDB::bind_method(D_METHOD("start_provider_auth", "provider", "method_id", "inputs"), &SolersSettingsService::start_provider_auth, DEFVAL(StringName()), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("cancel_auth"), &SolersSettingsService::cancel_auth);
	ClassDB::bind_method(D_METHOD("get_auth_status", "provider"), &SolersSettingsService::get_auth_status);
	ClassDB::bind_method(D_METHOD("disconnect_auth", "provider"), &SolersSettingsService::disconnect_auth);
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

String SolersSettingsService::_model_setting_key(const String &p_provider, const String &p_model) const {
	const Dictionary profile = provider_registry ? provider_registry->get_provider_profile(p_provider) : Dictionary();
	return String(profile.get("catalog_provider", p_provider)) + "/" + p_model;
}

Dictionary SolersSettingsService::_get_model_overrides() const {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting(_setting_path("model_overrides"))) {
		return Dictionary();
	}
	const Variant value = settings->get_setting(_setting_path("model_overrides"));
	return value.get_type() == Variant::DICTIONARY ? Dictionary(value).duplicate(true) : Dictionary();
}

void SolersSettingsService::_sync_model_catalog() const {
	if (provider_registry && provider_registry->get_model_catalog()) {
		provider_registry->get_model_catalog()->set_model_overrides(_get_model_overrides());
	}
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
		// Single alias authority: SolersModelCatalog::canonical_provider_id.
		SolersModelCatalog *md = provider_registry ? provider_registry->get_model_catalog() : nullptr;
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
	if (version < 7) {
		Dictionary overrides = _get_model_overrides();
		List<PropertyInfo> properties;
		settings->get_property_list(&properties);
		const String prefix = _setting_path("providers/");
		for (const PropertyInfo &property : properties) {
			const String path = property.name;
			if (!path.begins_with(prefix)) {
				continue;
			}
			const String rest = path.substr(prefix.length());
			const int slash = rest.find("/");
			if (slash <= 0 || rest.substr(slash + 1) != "model") {
				continue;
			}
			const String id = rest.substr(0, slash);
			const String model = settings->get_setting(path);
			if (model.is_empty()) {
				continue;
			}
			Dictionary override;
			for (const String &field : { String("base_url"), String("context_window"), String("max_tokens") }) {
				const String old_path = _provider_setting_path(id, field);
				if (!settings->has_setting(old_path)) {
					continue;
				}
				const String target = field == "base_url" ? "provider_api" : (field == "context_window" ? "context" : "output");
				override[target] = settings->get_setting(old_path);
				settings->erase(old_path);
			}
			if (!override.is_empty()) {
				overrides[_model_setting_key(id, model)] = override;
			}
		}
		settings->set_manually(_setting_path("model_overrides"), overrides);
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

StringName SolersSettingsService::_resolve_auth_method_id(const Dictionary &p_profile, const Dictionary &p_credential) const {
	const StringName stored = StringName(p_credential.get("method_id", StringName()));
	if (!stored.is_empty()) {
		return stored;
	}
	StringName only;
	for (const Variant &method_value : Array(p_profile.get("auth_methods", Array()))) {
		const Dictionary method = method_value;
		if (String(method.get("type", String())) != "oauth") {
			continue;
		}
		if (!only.is_empty()) {
			return StringName();
		}
		only = StringName(method.get("id", StringName()));
	}
	return only;
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
	const String model_id = data["model"];
	const Dictionary model = provider_registry->get_model_catalog()->get_model(StringName(profile.get("catalog_provider", p_provider)), model_id);
	data["base_url"] = model.get("provider_api", profile.get("default_base_url", String()));
	data["protocol"] = model.get("protocol", profile.get("protocol", String()));
	data["context_window"] = model.get("context", model_id.is_empty() ? 0 : 128000);
	data["max_tokens"] = model.get("output", SolersContextManager::DEFAULT_OUTPUT_TOKENS);
	data["cost"] = model.get("cost", Dictionary());

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
	_sync_model_catalog();
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
	const Dictionary profile = provider_registry->get_provider_profile(provider);
	if (profile.is_empty()) {
		return _error("PROVIDER_CONNECTION_UNDECLARED", "Choose a catalog provider or the Custom OpenAI-compatible connection.");
	}
	const bool was_connected = _get_provider_config(provider, false).get("connected", false);
	const String model = p_args.has("model") ? String(p_args["model"]).strip_edges() : String(profile.get("default_model", String()));
	const String overrides_path = _setting_path("model_overrides");
	const bool had_overrides = settings->has_setting(overrides_path);
	const Dictionary previous_overrides = _get_model_overrides();
	bool catalog_changed = false;
	if (!model.is_empty()) {
		Dictionary overrides = previous_overrides.duplicate(true);
		const String key = _model_setting_key(provider, model);
		Dictionary model_override = overrides.get(key, Dictionary());
		if (p_args.has("protocol")) {
			model_override["protocol"] = p_args["protocol"];
		}
		if (p_args.has("base_url")) {
			const String base_url = String(p_args["base_url"]).strip_edges();
			if (base_url.is_empty()) {
				model_override.erase("provider_api");
			} else {
				model_override["provider_api"] = base_url;
			}
		}
		static const char *SOURCE_FIELDS[] = { "context_window", "max_tokens" };
		static const char *MODEL_FIELDS[] = { "context", "output" };
		for (int i = 0; i < 2; i++) {
			if (!p_args.has(SOURCE_FIELDS[i])) {
				continue;
			}
			const int value = p_args.get(SOURCE_FIELDS[i], 0);
			if (value > 0) {
				model_override[MODEL_FIELDS[i]] = value;
			} else {
				model_override.erase(MODEL_FIELDS[i]);
			}
		}
		const Variant cost_value = p_args.get("cost", Variant());
		if (cost_value.get_type() == Variant::DICTIONARY) {
			const StringName catalog_provider = StringName(profile.get("catalog_provider", provider));
			Dictionary cost = provider_registry->get_model_catalog()->get_model(catalog_provider, model).get("cost", Dictionary());
			cost.merge(Dictionary(cost_value), true);
			model_override["cost"] = cost;
		}
		if (model_override.is_empty()) {
			overrides.erase(key);
		} else {
			overrides[key] = model_override;
		}
		settings->set_manually(overrides_path, overrides);
		_sync_model_catalog();
		catalog_changed = true;
	}
	if (!model.is_empty() && !provider_registry->is_model_allowed(provider, model)) {
		if (catalog_changed) {
			if (had_overrides) {
				settings->set_manually(overrides_path, previous_overrides);
			} else {
				settings->erase(overrides_path);
			}
			_sync_model_catalog();
		}
		return _error("MODEL_NOT_ALLOWED", "The selected model is not available through this provider connection.");
	}
	settings->set_manually(_setting_path("provider"), provider);
	settings->set_manually(_provider_setting_path(provider, "configured"), true);
	if (p_args.has("model")) {
		const String model_path = _provider_setting_path(provider, "model");
		const String previous_model = settings->has_setting(model_path) ? String(settings->get_setting(model_path)) : String(provider_registry->get_provider_profile(provider).get("default_model", String()));
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
	static const char *KEYS[] = { "configured", "model", "reasoning_effort", "api_key", "oauth" };
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
	if (SolersModelCatalog *md = provider_registry->get_model_catalog()) {
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

Dictionary SolersSettingsService::resolve_provider_profile(const String &p_provider, const String &p_base_url_override, const String &p_model) const {
	return provider_registry ? provider_registry->resolve_provider_profile(p_provider, p_base_url_override, p_model) : Dictionary();
}

bool SolersSettingsService::is_model_allowed(const String &p_provider, const String &p_model) const {
	return provider_registry && provider_registry->is_model_allowed(p_provider, p_model);
}

void SolersSettingsService::register_auth_method(const String &p_provider, SolersProviderAuth *p_method, bool p_owned) {
	ERR_FAIL_NULL(p_method);
	const StringName id = p_method->get_method_id();
	ERR_FAIL_COND(p_provider.is_empty() || id.is_empty());
	auth_methods[p_provider + "/" + String(id)] = p_method;
	if (p_owned) {
		owned_auth_methods.push_back(p_method);
	}
}

SolersProviderAuth *SolersSettingsService::get_auth_method(const String &p_provider, const Dictionary &p_profile, const Dictionary &p_credential) const {
	const StringName id = _resolve_auth_method_id(p_profile, p_credential);
	SolersProviderAuth *const *found = auth_methods.getptr(p_provider + "/" + String(id));
	return found ? *found : nullptr;
}

Dictionary SolersSettingsService::start_provider_auth(const String &p_provider, const StringName &p_method_id, const Dictionary &p_inputs) {
	ERR_FAIL_NULL_V(provider_registry, _error("PROVIDER_REGISTRY_UNAVAILABLE", "Solers provider registry is not initialized.", false));
	const Dictionary profile = provider_registry->get_provider_profile(p_provider);
	const StringName method_id = p_method_id.is_empty() ? _resolve_auth_method_id(profile) : p_method_id;
	bool declared = false;
	for (const Variant &method_value : Array(profile.get("auth_methods", Array()))) {
		const Dictionary method = method_value;
		if (String(method.get("type", String())) == "oauth" && StringName(method.get("id", StringName())) == method_id) {
			declared = true;
			break;
		}
	}
	if (!declared) {
		return _error("OAUTH_METHOD_NOT_DECLARED", "The selected provider does not declare this authorization method.", false);
	}
	SolersProviderAuth *const *found = auth_methods.getptr(p_provider + "/" + String(method_id));
	if (!found) {
		return _error("OAUTH_METHOD_UNAVAILABLE", "The selected authorization method is unavailable.", false);
	}
	if (active_auth && active_auth != *found && active_auth->is_active()) {
		return _error("OAUTH_BUSY", "Another authorization attempt is already active.");
	}
	active_auth = *found;
	active_auth_provider = p_provider;
	auth_error.clear();
	Dictionary result = active_auth->start(p_inputs);
	return result;
}

void SolersSettingsService::poll_auth() {
	if (!active_auth) {
		return;
	}
	active_auth->poll();
	const Dictionary profile = provider_registry ? provider_registry->get_provider_profile(active_auth_provider) : Dictionary();
	if (active_auth_provider.is_empty() || profile.is_empty()) {
		auth_error["code"] = "OAUTH_PROFILE_UNAVAILABLE";
		auth_error["message"] = "The provider authorization profile is no longer available.";
		return;
	}
	const Dictionary credential = active_auth->take_credential();
	if (credential.is_empty()) {
		return;
	}
	Dictionary config;
	config["provider"] = active_auth_provider;
	config["model"] = profile.get("default_model", String());
	config["base_url"] = profile.get("default_base_url", String());
	const Dictionary config_result = set_provider_config(config);
	if (!config_result.get("ok", false)) {
		const Dictionary error = config_result.get("error", Dictionary());
		auth_error["code"] = error.get("code", "OAUTH_CONFIG_FAILED");
		auth_error["message"] = error.get("message", "The provider configuration could not be saved.");
		return;
	}
	if (!store_provider_auth(active_auth_provider, credential)) {
		auth_error["code"] = "OAUTH_STORE_FAILED";
		auth_error["message"] = "Authorization credentials could not be protected by the operating system and were not stored.";
		return;
	}
	set_local_models_only(false); // Completing OAuth is explicit consent to remote model access.
	const Dictionary connected = _get_provider_config(active_auth_provider, false);
	if (!(bool)connected.get("connected", false)) {
		auth_error["code"] = "OAUTH_NOT_CONNECTED";
		auth_error["message"] = "Authorization was saved, but the provider did not validate as connected.";
		return;
	}
	active_auth = nullptr;
	active_auth_provider = String();
	auth_error.clear();
}

void SolersSettingsService::cancel_auth() {
	if (active_auth) {
		active_auth->cancel();
	}
}

Dictionary SolersSettingsService::get_auth_status(const String &p_provider) const {
	Dictionary status = active_auth && active_auth_provider == p_provider ? active_auth->get_status() : Dictionary();
	if (status.is_empty()) {
		status["state"] = "idle";
		status["active"] = false;
	}
	const Dictionary config = _get_provider_config(p_provider, false);
	status["connected"] = config.get("connected", false);
	status["available"] = config.get("available", false);
	if (active_auth_provider == p_provider && !auth_error.is_empty()) {
		status["state"] = "failed";
		status["code"] = auth_error.get("code", String());
		status["error"] = auth_error.get("message", String());
	}
	return status;
}

bool SolersSettingsService::is_auth_active() const {
	return active_auth && active_auth->is_active();
}

Dictionary SolersSettingsService::disconnect_auth(const String &p_provider) {
	cancel_auth();
	auth_error.clear();
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
	register_auth_method("openai_codex", memnew(SolersCodexAuth));
}

SolersSettingsService::~SolersSettingsService() {
	for (SolersProviderAuth *method : owned_auth_methods) {
		memdelete(method);
	}
	owned_auth_methods.clear();
	auth_methods.clear();
}
