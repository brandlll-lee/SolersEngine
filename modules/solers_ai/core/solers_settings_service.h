/**************************************************************************/
/*  solers_settings_service.h                                             */
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

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

class SolersProviderAuth;
class SolersProviderRegistry;

class SolersSettingsService : public Object {
	GDCLASS(SolersSettingsService, Object);

	SolersProviderRegistry *provider_registry = nullptr;
	HashMap<String, SolersProviderAuth *> auth_methods;
	Vector<SolersProviderAuth *> owned_auth_methods;
	SolersProviderAuth *active_auth = nullptr;
	String active_auth_provider;
	Dictionary auth_error;

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	String _setting_path(const String &p_key) const;
	String _provider_setting_path(const String &p_provider, const String &p_key) const;
	void _migrate_provider_settings();
	Dictionary _get_provider_config(const String &p_provider, bool p_include_secret) const;
	Dictionary _get_stored_auth(const String &p_provider) const;
	StringName _resolve_auth_method_id(const Dictionary &p_profile, const Dictionary &p_credential = Dictionary()) const;

protected:
	static void _bind_methods();

public:
	SolersProviderRegistry *get_provider_registry() const { return provider_registry; }
	void set_provider_registry(SolersProviderRegistry *p_provider_registry);
	bool get_local_models_only() const;
	Dictionary set_local_models_only(bool p_enabled);
	Dictionary get_provider_config() const;
	Dictionary get_provider_config_for(const String &p_provider) const;
	Dictionary set_provider_config(const Dictionary &p_args);
	Dictionary disconnect_provider(const String &p_provider);
	Dictionary list_provider_profiles() const;
	Dictionary list_connected_provider_configs() const;
	// Provider view: { connected, popular, all, custom }.
	Dictionary list_provider_view() const;
	Dictionary validate_provider_config(const Dictionary &p_args) const;
	Dictionary resolve_provider_profile(const String &p_provider, const String &p_base_url_override = String(), const String &p_model = String()) const;
	bool is_model_allowed(const String &p_provider, const String &p_model) const;

	void register_auth_method(const String &p_provider, SolersProviderAuth *p_method, bool p_owned = true);
	SolersProviderAuth *get_auth_method(const String &p_provider, const Dictionary &p_profile, const Dictionary &p_credential = Dictionary()) const;
	Dictionary start_provider_auth(const String &p_provider, const StringName &p_method_id = StringName(), const Dictionary &p_inputs = Dictionary());
	void poll_auth();
	void cancel_auth();
	Dictionary get_auth_status(const String &p_provider) const;
	bool is_auth_active() const;
	Dictionary disconnect_auth(const String &p_provider);
	bool store_provider_auth(const String &p_provider, const Dictionary &p_auth);

	// Internal-only: returns the active provider config including the real
	// credential. Intentionally not bound so secrets never leak through tools.
	Dictionary resolve_active_provider() const;

	SolersSettingsService();
	~SolersSettingsService();
};
