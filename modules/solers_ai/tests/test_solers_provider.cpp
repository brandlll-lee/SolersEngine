/**************************************************************************/
/*  test_solers_provider.cpp                                              */
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

#include "core/config/project_settings.h"
#include "core/io/config_file.h"
#include "editor/settings/editor_settings.h"
#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/llm/solers_models_dev.h"

TEST_FORCE_LINK(test_solers_provider)

namespace TestSolersProvider {

class ScopedEditorSettings {
	EditorSettings *settings = nullptr;
	Array paths;
	Dictionary values;

public:
	ScopedEditorSettings(EditorSettings *p_settings, const Array &p_paths) :
			settings(p_settings), paths(p_paths) {
		for (const Variant &path_value : paths) {
			const String path = path_value;
			if (settings->has_setting(path)) {
				values[path] = settings->get_setting(path);
			}
			settings->erase(path);
		}
	}

	~ScopedEditorSettings() {
		for (const Variant &path_value : paths) {
			const String path = path_value;
			settings->erase(path);
			if (values.has(path)) {
				settings->set_manually(path, values[path]);
			}
		}
		EditorSettings::save();
	}
};

TEST_CASE("[SolersSecretStore] strict credentials are protected and recoverable") {
	const String secret = "synthetic-oauth-credential";
	const String stored = SolersSecretStore::protect_strict(secret);
	REQUIRE_FALSE(stored.is_empty());
	CHECK(SolersSecretStore::is_protected(stored));
	CHECK(stored != secret);
	CHECK(SolersSecretStore::unprotect(stored) == secret);
}

TEST_CASE("[SolersProviderRegistry] assembles catalog and AuthHook overlays") {
	SolersProviderRegistry registry;

	Dictionary openai = registry.get_provider_profile("openai");
	Dictionary anthropic = registry.get_provider_profile("anthropic");
	Dictionary relay = registry.resolve_provider_profile("anthropic", "https://relay.example/v1");

	CHECK(openai.get("protocol", String()) == "openai-chat");
	CHECK(openai.get("default_base_url", String()) == "https://api.openai.com/v1");
	CHECK(anthropic.get("protocol", String()) == "anthropic-messages");
	CHECK(anthropic.get("default_base_url", String()) == "https://api.anthropic.com");
	CHECK(registry.resolve_provider_profile("openai").get("catalog_limits_authoritative", false));
	CHECK(relay.get("protocol", String()) == "anthropic-messages");
	CHECK(relay.get("auth_header", String()) == "x-api-key");
	CHECK_FALSE(relay.get("catalog_limits_authoritative", true));
}

TEST_CASE("[SolersProviderRegistry] requires an explicit connection profile") {
	SolersProviderRegistry registry;

	Dictionary config;
	config["provider"] = "custom_openai_compatible";
	config["model"] = "synthetic-model";
	config["base_url"] = "https://gateway.example/v1";
	config["api_key"] = "synthetic-key";

	const Dictionary result = registry.validate_config(config);
	CHECK(result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	CHECK(data.get("valid", false));
	CHECK(data.get("effective_base_url", String()) == "https://gateway.example/v1");

	Dictionary fresh;
	fresh["provider"] = "synthetic-brand-new-gateway";
	fresh["model"] = "synthetic-model";
	fresh["base_url"] = "https://gateway.example/v1";
	fresh["api_key"] = "synthetic-key";
	const Dictionary accepted = registry.validate_config(fresh);
	CHECK_FALSE(accepted.get("ok", true));
	CHECK(Dictionary(accepted.get("error", Dictionary())).get("code", String()) == "PROVIDER_CONNECTION_UNDECLARED");
}

TEST_CASE("[Editor][SolersSettingsService] v6 migrates one explicit custom connection") {
	EditorSettings *settings = EditorSettings::get_singleton();
	REQUIRE(settings != nullptr);

	const String prefix = "solers/ai/";
	const String legacy_id = "synthetic-legacy-gateway";
	Array paths;
	paths.push_back(prefix + "settings_version");
	paths.push_back(prefix + "local_models_only");
	paths.push_back(prefix + "provider");
	paths.push_back(prefix + "custom_provider_ids");
	for (const String &provider : { legacy_id, String("custom_openai_compatible") }) {
		for (const String &key : { String("configured"), String("model"), String("reasoning_effort"), String("base_url"), String("api_key") }) {
			paths.push_back(prefix + "providers/" + provider + "/" + key);
		}
	}
	ScopedEditorSettings restore(settings, paths);

	SolersProviderRegistry registry;
	settings->set_manually(prefix + "settings_version", 5);
	settings->set_manually(prefix + "provider", legacy_id);
	settings->set_manually(prefix + "providers/" + legacy_id + "/configured", true);
	settings->set_manually(prefix + "providers/" + legacy_id + "/model", "synthetic-model");
	settings->set_manually(prefix + "providers/" + legacy_id + "/reasoning_effort", "high");
	settings->set_manually(prefix + "providers/" + legacy_id + "/base_url", "https://gateway.example/v1");
	settings->set_manually(prefix + "providers/" + legacy_id + "/api_key", SolersSecretStore::protect("synthetic-key"));
	Array custom_ids;
	custom_ids.push_back(legacy_id);
	settings->set_manually(prefix + "custom_provider_ids", custom_ids);

	SolersSettingsService service;
	service.set_provider_registry(&registry);
	CHECK((int)settings->get_setting(prefix + "settings_version") == 6);
	CHECK(String(settings->get_setting(prefix + "provider")) == "custom_openai_compatible");
	CHECK_FALSE(settings->has_setting(prefix + "custom_provider_ids"));
	CHECK_FALSE(settings->has_setting(prefix + "providers/" + legacy_id + "/configured"));
	const Dictionary migrated = service.get_provider_config().get("data", Dictionary());
	CHECK(migrated.get("connected", false));
	CHECK(migrated.get("model", String()) == "synthetic-model");
	CHECK(migrated.get("reasoning_effort", String()) == "high");
	CHECK(migrated.get("base_url", String()) == "https://gateway.example/v1");
	CHECK(Dictionary(migrated.get("profile", Dictionary())).get("protocol", String()) == "openai-chat");

	Dictionary undeclared;
	undeclared["provider"] = "synthetic-never-special-cased";
	undeclared["model"] = "m1";
	undeclared["base_url"] = "https://example.test/v1";
	undeclared["api_key"] = "k";
	const Dictionary rejected = service.set_provider_config(undeclared);
	CHECK_FALSE(rejected.get("ok", true));
	CHECK(Dictionary(rejected.get("error", Dictionary())).get("code", String()) == "PROVIDER_CONNECTION_UNDECLARED");

	settings->set_manually(prefix + "settings_version", 5);
	settings->set_manually(prefix + "provider", "anthropic");
	SolersSettingsService known_service;
	known_service.set_provider_registry(&registry);
	CHECK(String(settings->get_setting(prefix + "provider")) == "anthropic");
}

TEST_CASE("[SolersModelsDev] input modality support is model-level and unknown is permissive") {
	Dictionary vision_model;
	Array modalities;
	modalities.push_back("text");
	modalities.push_back("image");
	vision_model["input_modalities"] = modalities;
	CHECK(SolersModelsDev::input_modality_support(vision_model, "image") == 1);

	modalities.erase("image");
	vision_model["input_modalities"] = modalities;
	CHECK(SolersModelsDev::input_modality_support(vision_model, "image") == 0);
	CHECK(SolersModelsDev::input_modality_support(Dictionary(), "image") == -1);
}

TEST_CASE("[SolersModelsDev] reasoning effort options are model-declared") {
	Dictionary effort_option;
	effort_option["type"] = "effort";
	Array declared_values;
	declared_values.push_back("low");
	declared_values.push_back("xhigh");
	effort_option["values"] = declared_values;
	Array reasoning_options;
	reasoning_options.push_back(effort_option);
	Dictionary declared_model;
	declared_model["reasoning"] = true;
	declared_model["reasoning_options"] = reasoning_options;
	CHECK(SolersModelsDev::reasoning_efforts(declared_model) == declared_values);

	Dictionary non_reasoning_model;
	non_reasoning_model["reasoning"] = false;
	CHECK(SolersModelsDev::reasoning_efforts(non_reasoning_model).is_empty());

	CHECK(SolersModelsDev::reasoning_efforts(Dictionary()).is_empty());
}

} // namespace TestSolersProvider
