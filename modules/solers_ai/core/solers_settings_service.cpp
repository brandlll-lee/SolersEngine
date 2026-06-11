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

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/settings/editor_settings.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_secret_store.h"

void SolersSettingsService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_provider_registry", "provider_registry"), &SolersSettingsService::set_provider_registry);
	ClassDB::bind_method(D_METHOD("get_provider_config"), &SolersSettingsService::get_provider_config);
	ClassDB::bind_method(D_METHOD("set_provider_config", "args"), &SolersSettingsService::set_provider_config);
	ClassDB::bind_method(D_METHOD("list_provider_profiles"), &SolersSettingsService::list_provider_profiles);
	ClassDB::bind_method(D_METHOD("validate_provider_config", "args"), &SolersSettingsService::validate_provider_config);
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

void SolersSettingsService::set_provider_registry(SolersProviderRegistry *p_provider_registry) {
	provider_registry = p_provider_registry;
}

Dictionary SolersSettingsService::get_provider_config() const {
	EditorSettings *settings = EditorSettings::get_singleton();
	ERR_FAIL_NULL_V(settings, _error("EDITOR_SETTINGS_UNAVAILABLE", "EditorSettings is not available.", false));

	Dictionary data;
	data["privacy_mode"] = settings->has_setting(_setting_path("privacy_mode")) ? (bool)settings->get_setting(_setting_path("privacy_mode")) : true;
	const String provider = settings->has_setting(_setting_path("provider")) ? String(settings->get_setting(_setting_path("provider"))) : "ollama";
	data["provider"] = provider;
	data["model"] = settings->has_setting(_setting_path("model")) ? String(settings->get_setting(_setting_path("model"))) : String();
	data["base_url"] = settings->has_setting(_setting_path("base_url")) ? String(settings->get_setting(_setting_path("base_url"))) : String();

	// A key counts as configured when stored *or* supplied via the provider's
	// environment variable (BYOK env fallback).
	bool key_set = settings->has_setting(_setting_path("api_key")) && !String(settings->get_setting(_setting_path("api_key"))).is_empty();
	String key_source = key_set ? "settings" : "none";
	if (!key_set && provider_registry) {
		const Dictionary profile = provider_registry->get_provider_profile(provider);
		const String env_name = profile.get("api_key_env", String());
		if (!env_name.is_empty() && OS::get_singleton()->has_environment(env_name) && !OS::get_singleton()->get_environment(env_name).is_empty()) {
			key_set = true;
			key_source = "environment";
		}
	}
	data["api_key_configured"] = key_set;
	data["api_key_source"] = key_source;
	data["api_key"] = "<redacted>";
	if (provider_registry) {
		Dictionary validation = provider_registry->validate_config(data);
		data["validation"] = validation.get("data", Dictionary());
	}
	return _ok(data);
}

Dictionary SolersSettingsService::set_provider_config(const Dictionary &p_args) {
	EditorSettings *settings = EditorSettings::get_singleton();
	ERR_FAIL_NULL_V(settings, _error("EDITOR_SETTINGS_UNAVAILABLE", "EditorSettings is not available.", false));

	if (p_args.has("privacy_mode")) {
		settings->set_manually(_setting_path("privacy_mode"), (bool)p_args["privacy_mode"]);
	}
	if (p_args.has("provider")) {
		settings->set_manually(_setting_path("provider"), String(p_args["provider"]));
	}
	if (p_args.has("model")) {
		settings->set_manually(_setting_path("model"), String(p_args["model"]));
	}
	if (p_args.has("base_url")) {
		settings->set_manually(_setting_path("base_url"), String(p_args["base_url"]));
	}
	if (p_args.has("api_key")) {
		// Never persist plaintext: wrap with DPAPI (Windows) or machine-bound AES.
		settings->set_manually(_setting_path("api_key"), SolersSecretStore::protect(String(p_args["api_key"])));
	}
	EditorSettings::save();

	Dictionary data = get_provider_config().get("data", Dictionary());
	data["saved"] = true;
	return _ok(data);
}

Dictionary SolersSettingsService::list_provider_profiles() const {
	ERR_FAIL_NULL_V(provider_registry, _error("PROVIDER_REGISTRY_UNAVAILABLE", "Solers provider registry is not initialized.", false));
	Dictionary data;
	data["profiles"] = provider_registry->list_provider_profiles();
	data["count"] = ((Array)data["profiles"]).size();
	return _ok(data);
}

Dictionary SolersSettingsService::validate_provider_config(const Dictionary &p_args) const {
	ERR_FAIL_NULL_V(provider_registry, _error("PROVIDER_REGISTRY_UNAVAILABLE", "Solers provider registry is not initialized.", false));

	Dictionary config = get_provider_config().get("data", Dictionary());
	for (const Variant *K = p_args.next(nullptr); K; K = p_args.next(K)) {
		config[*K] = p_args[*K];
	}
	return provider_registry->validate_config(config);
}

Dictionary SolersSettingsService::resolve_active_provider() const {
	Dictionary out;
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings) {
		return out;
	}
	const String provider = settings->has_setting(_setting_path("provider")) ? String(settings->get_setting(_setting_path("provider"))) : String("openai");
	out["provider"] = provider;
	out["model"] = settings->has_setting(_setting_path("model")) ? String(settings->get_setting(_setting_path("model"))) : String();
	out["base_url"] = settings->has_setting(_setting_path("base_url")) ? String(settings->get_setting(_setting_path("base_url"))) : String();
	out["privacy_mode"] = settings->has_setting(_setting_path("privacy_mode")) ? (bool)settings->get_setting(_setting_path("privacy_mode")) : true;

	// Stored key (decrypted) → provider env var fallback (OPENAI_API_KEY, ...).
	String api_key;
	if (settings->has_setting(_setting_path("api_key"))) {
		api_key = SolersSecretStore::unprotect(String(settings->get_setting(_setting_path("api_key"))));
	}
	if (api_key.is_empty() && provider_registry) {
		const Dictionary profile = provider_registry->get_provider_profile(provider);
		const String env_name = profile.get("api_key_env", String());
		if (!env_name.is_empty() && OS::get_singleton()->has_environment(env_name)) {
			api_key = OS::get_singleton()->get_environment(env_name);
		}
	}
	out["api_key"] = api_key;
	return out;
}
