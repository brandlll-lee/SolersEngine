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
#include "editor/settings/editor_settings.h"
#include "modules/solers_ai/core/solers_codex_auth.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_secret_store.h"

static constexpr int SOLERS_PROVIDER_SETTINGS_VERSION = 3;

void SolersSettingsService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_provider_registry", "provider_registry"), &SolersSettingsService::set_provider_registry);
	ClassDB::bind_method(D_METHOD("get_provider_config"), &SolersSettingsService::get_provider_config);
	ClassDB::bind_method(D_METHOD("get_provider_config_for", "provider"), &SolersSettingsService::get_provider_config_for);
	ClassDB::bind_method(D_METHOD("set_provider_config", "args"), &SolersSettingsService::set_provider_config);
	ClassDB::bind_method(D_METHOD("disconnect_provider", "provider"), &SolersSettingsService::disconnect_provider);
	ClassDB::bind_method(D_METHOD("list_provider_profiles"), &SolersSettingsService::list_provider_profiles);
	ClassDB::bind_method(D_METHOD("list_connected_provider_configs"), &SolersSettingsService::list_connected_provider_configs);
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
	if (provider == "openai_responses") {
		provider = "openai";
	} else if (provider == "anthropic") {
		provider = "anthropic_messages";
	} else if (provider == "custom_openai_responses" || (!provider.is_empty() && provider_registry && provider_registry->get_provider_profile(provider).is_empty())) {
		provider = "custom_openai_compatible";
	}
	if (version >= 2 && !previous_provider.is_empty() && previous_provider != provider) {
		static const char *PROVIDER_KEYS[] = { "configured", "model", "base_url", "api_key", "oauth" };
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
	data["privacy_mode"] = settings->has_setting(_setting_path("privacy_mode")) ? (bool)settings->get_setting(_setting_path("privacy_mode")) : true;
	data["configured"] = settings->has_setting(_provider_setting_path(p_provider, "configured")) && (bool)settings->get_setting(_provider_setting_path(p_provider, "configured"));
	data["model"] = settings->has_setting(_provider_setting_path(p_provider, "model")) ? String(settings->get_setting(_provider_setting_path(p_provider, "model"))) : String(profile.get("default_model", String()));
	data["reasoning_effort"] = settings->has_setting(_provider_setting_path(p_provider, "reasoning_effort")) ? String(settings->get_setting(_provider_setting_path(p_provider, "reasoning_effort"))) : String();
	data["base_url"] = settings->has_setting(_provider_setting_path(p_provider, "base_url")) ? String(settings->get_setting(_provider_setting_path(p_provider, "base_url"))) : String(profile.get("default_base_url", String()));

	const Dictionary auth = _get_stored_auth(p_provider);
	const String auth_type = profile.get("auth_type", "api_key");
	const bool api_key_configured = auth_type == "api_key" && !String(auth.get("key", String())).is_empty();
	const bool oauth_configured = auth_type == "oauth" && !String(auth.get("refresh", String())).is_empty();
	data["api_key_configured"] = api_key_configured;
	data["api_key_source"] = auth.get("source", "none");
	data["oauth_configured"] = oauth_configured;
	data["api_key"] = "<redacted>";

	Dictionary validation_input = data.duplicate(true);
	const Dictionary validation = provider_registry->validate_config(validation_input).get("data", Dictionary());
	data["validation"] = validation;

	Dictionary connection_input = validation_input;
	connection_input["privacy_mode"] = false;
	const Dictionary connection_validation = provider_registry->validate_config(connection_input).get("data", Dictionary());
	const bool credential_ready = auth_type == "none" || api_key_configured || oauth_configured;
	const bool connection_declared = auth_type == "none" ? (bool)data["configured"] : credential_ready;
	const bool connected = connection_declared && connection_validation.get("valid", false);
	data["connected"] = connected;
	data["available"] = connected && (!(bool)data["privacy_mode"] || (bool)profile.get("local", false));
	data["active"] = settings->has_setting(_setting_path("provider")) && String(settings->get_setting(_setting_path("provider"))) == p_provider;
	if (p_include_secret) {
		data["auth"] = auth;
	}
	return data;
}

void SolersSettingsService::set_provider_registry(SolersProviderRegistry *p_provider_registry) {
	provider_registry = p_provider_registry;
	_migrate_provider_settings();
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
	if (!provider_registry || provider_registry->get_provider_profile(provider).is_empty()) {
		return _error("UNKNOWN_PROVIDER", vformat("Unknown Solers provider profile: %s", provider));
	}
	if (p_args.has("model") && !provider_registry->is_model_allowed(provider, String(p_args["model"]))) {
		return _error("MODEL_NOT_ALLOWED", "The selected model is not available through this provider connection.");
	}

	if (p_args.has("privacy_mode")) {
		settings->set_manually(_setting_path("privacy_mode"), (bool)p_args["privacy_mode"]);
	}
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
	if (p_args.has("api_key") && !String(p_args["api_key"]).is_empty()) {
		settings->set_manually(_provider_setting_path(provider, "api_key"), SolersSecretStore::protect(String(p_args["api_key"])));
	}
	settings->emit_signal(SNAME("settings_changed"));
	settings->notify_changes();
	EditorSettings::save();
	Dictionary data = _get_provider_config(provider, false);
	data["saved"] = true;
	return _ok(data);
}

Dictionary SolersSettingsService::disconnect_provider(const String &p_provider) {
	EditorSettings *settings = EditorSettings::get_singleton();
	ERR_FAIL_NULL_V(settings, _error("EDITOR_SETTINGS_UNAVAILABLE", "EditorSettings is not available.", false));
	static const char *KEYS[] = { "configured", "model", "reasoning_effort", "base_url", "api_key", "oauth" };
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
	const Array profiles = provider_registry->list_provider_profiles();
	for (const Variant &profile_value : profiles) {
		const String provider = Dictionary(profile_value).get("id", String());
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
	config["privacy_mode"] = false; // Clicking OAuth is explicit consent to remote model access.
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
