/**************************************************************************/
/*  solers_provider_registry.h                                            */
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
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersModelsDev;

// Assembles transport profiles from catalog entries with declared connection
// facts plus the small AuthHook/custom overlay table. Metadata-only catalog
// entries are not presented as usable connections.
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

protected:
	static void _bind_methods();

public:
	SolersModelsDev *get_models_dev() const { return models_dev; }
	void set_models_dev(SolersModelsDev *p_models_dev, bool p_owned = false);

	Dictionary get_provider_profile(const String &p_provider) const;
	Dictionary resolve_provider_profile(const String &p_provider, const String &p_base_url_override = String()) const;
	// Only profiles with an explicit wire protocol and authentication contract.
	Array list_provider_profiles() const;
	Array list_popular_provider_ids() const;
	Array list_overlay_provider_ids() const;
	Dictionary validate_config(const Dictionary &p_config) const;
	bool is_model_allowed(const String &p_provider, const String &p_model) const;

	SolersProviderRegistry();
	~SolersProviderRegistry();
};
