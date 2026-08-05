/**************************************************************************/
/*  solers_provider_registry.h                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersModelsDev;

// Assembles transport profiles from models.dev catalog + a small AuthHook
// overlay table (Codex OAuth, custom OpenAI-compatible template). New catalog
// providers need zero Solers code changes.
class SolersProviderRegistry : public Object {
	GDCLASS(SolersProviderRegistry, Object);

	SolersModelsDev *models_dev = nullptr; // Owned.
	Dictionary overlays; // id -> special profile (AuthHooks).
	Array overlay_order;
	bool owns_models_dev = true;

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	void _register_auth_hooks();
	Dictionary _profile_from_catalog(const Dictionary &p_catalog) const;
	Dictionary _default_custom_profile(const String &p_id) const;
	String _default_model_for_catalog(const Dictionary &p_catalog) const;
	static String _protocol_for_npm(const String &p_npm);
	static bool _looks_local_api(const String &p_api);

protected:
	static void _bind_methods();

public:
	SolersModelsDev *get_models_dev() const { return models_dev; }
	void set_models_dev(SolersModelsDev *p_models_dev, bool p_owned = false);

	Dictionary get_provider_profile(const String &p_provider) const;
	Dictionary resolve_provider_profile(const String &p_provider, const String &p_base_url_override = String()) const;
	// Catalog + AuthHook overlays (OpenCode-style "all"). Custom user ids resolve
	// via get_provider_profile when configured.
	Array list_provider_profiles() const;
	Array list_popular_provider_ids() const;
	Array list_overlay_provider_ids() const;
	Dictionary validate_config(const Dictionary &p_config) const;
	bool is_model_allowed(const String &p_provider, const String &p_model) const;
	bool is_known_provider(const String &p_provider) const;

	SolersProviderRegistry();
	~SolersProviderRegistry();
};
