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
#include "core/os/thread.h"
#include "editor/settings/editor_settings.h"
#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_provider_auth.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/llm/solers_models_dev.h"

TEST_FORCE_LINK(test_solers_provider)

namespace TestSolersProvider {

class SyntheticAuth final : public SolersProviderAuth {
	StringName method_id;
	Dictionary credential;

public:
	bool started = false;

	explicit SyntheticAuth(const StringName &p_method_id) :
			method_id(p_method_id) {}
	void set_credential(const Dictionary &p_credential) { credential = p_credential; }
	StringName get_method_id() const override { return method_id; }
	Dictionary start(const Dictionary &p_inputs) override {
		started = true;
		if (p_inputs.has("credential")) {
			credential = Dictionary(p_inputs["credential"]);
		}
		return Dictionary({ { "ok", true }, { "input", p_inputs } });
	}
	void poll() override {}
	void cancel() override { started = false; }
	Dictionary get_status() const override { return Dictionary({ { "state", started ? "pending" : "idle" }, { "active", started } }); }
	Dictionary take_credential() override {
		Dictionary out = credential.duplicate(true);
		credential.clear();
		return out;
	}
	bool is_active() const override { return started; }
	Dictionary prepare_request(const Dictionary &p_credential, const Dictionary &p_profile, const Dictionary &p_request, bool p_force_refresh) const override {
		Dictionary headers;
		headers["Synthetic-Auth"] = p_force_refresh ? "refreshed" : "ready";
		return Dictionary({ { "ok", true }, { "credential", p_credential }, { "profile", p_profile }, { "request", p_request }, { "headers", headers } });
	}
};

static void _mark_thread_started(void *p_userdata) {
	*static_cast<bool *>(p_userdata) = true;
}

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

	CHECK(openai.get("protocol", String()) == "openai-responses");
	CHECK(openai.get("default_base_url", String()) == "https://api.openai.com/v1");
	CHECK(anthropic.get("protocol", String()) == "anthropic-messages");
	CHECK(anthropic.get("default_base_url", String()) == "https://api.anthropic.com");
	CHECK(relay.get("protocol", String()) == "anthropic-messages");
	CHECK(relay.get("auth_header", String()) == "x-api-key");
	CHECK(relay.get("catalog_provider", String()) == "anthropic");
	CHECK(relay.get("base_url", String()) == "https://relay.example");
	CHECK_FALSE(relay.has("catalog_limits_authoritative"));
	CHECK_FALSE(registry.get_provider_profile("custom_openai_compatible").has("context_window"));
}

TEST_CASE("[SolersProviderRegistry] selected providers use catalog-aligned protocols and endpoints") {
	SolersProviderRegistry registry;
	const Dictionary expected = {
		{ "minimax", Array({ "anthropic-messages", "https://api.minimax.io/anthropic" }) },
		{ "moonshotai", Array({ "openai-chat", "https://api.moonshot.ai/v1" }) },
		{ "xai", Array({ "openai-responses", "https://api.x.ai/v1" }) },
		{ "zhipuai", Array({ "openai-chat", "https://open.bigmodel.cn/api/paas/v4" }) },
		{ "zai-coding-plan", Array({ "openai-chat", "https://api.z.ai/api/coding/paas/v4" }) },
		{ "opencode", Array({ "openai-chat", "https://opencode.ai/zen/v1" }) },
		{ "opencode-go", Array({ "openai-chat", "https://opencode.ai/zen/go/v1" }) },
	};
	for (const Variant &id_value : expected.keys()) {
		const String id = id_value;
		const Array contract = expected[id];
		const Dictionary profile = registry.resolve_provider_profile(id);
		CHECK(profile.get("protocol", String()) == contract[0]);
		CHECK(profile.get("base_url", String()) == contract[1]);
	}
}

TEST_CASE("[Editor][SolersProviderAuth] methods are provider-scoped and profile-declared") {
	SolersProviderRegistry registry;
	SyntheticAuth codex_method(SNAME("chatgpt-browser"));
	SyntheticAuth other_method(SNAME("chatgpt-browser"));
	SolersSettingsService service;
	service.set_provider_registry(&registry);
	service.register_auth_method("openai_codex", &codex_method, false);
	service.register_auth_method("future-provider", &other_method, false);
	const Dictionary profile = registry.get_provider_profile("openai_codex");
	Dictionary credential;
	credential["method_id"] = "chatgpt-browser";
	CHECK(service.get_auth_method("openai_codex", profile, credential) == &codex_method);
	CHECK(service.get_auth_method("future-provider", profile, credential) == &other_method);
	CHECK_FALSE(service.start_provider_auth("openai_codex", "not-declared").get("ok", true));
	CHECK(service.start_provider_auth("openai_codex", "chatgpt-browser").get("ok", false));
	CHECK(codex_method.started);
	service.cancel_auth();
}

TEST_CASE("[Editor][SolersSettingsService] settles a declared OAuth credential once") {
	EditorSettings *settings = EditorSettings::get_singleton();
	REQUIRE(settings != nullptr);
	const String prefix = "solers/ai/";
	Array paths;
	paths.push_back(prefix + "provider");
	for (const String &key : { String("configured"), String("model"), String("base_url"), String("oauth") }) {
		paths.push_back(prefix + "providers/openai_codex/" + key);
	}
	ScopedEditorSettings restore(settings, paths);

	SolersProviderRegistry registry;
	SyntheticAuth auth(SNAME("chatgpt-browser"));
	SolersSettingsService service;
	service.set_provider_registry(&registry);
	service.register_auth_method("openai_codex", &auth, false);
	auth.set_credential(Dictionary({ { "type", "oauth" }, { "method_id", "chatgpt-browser" }, { "access", "access" }, { "refresh", "refresh" } }));
	REQUIRE(service.start_provider_auth("openai_codex", "chatgpt-browser").get("ok", false));

	service.poll_auth();
	const Dictionary status = service.get_auth_status("openai_codex");
	CHECK(status.get("connected", false));
	CHECK(status.get("available", false));
	CHECK(SolersSecretStore::is_protected(String(settings->get_setting(prefix + "providers/openai_codex/oauth"))));
	service.poll_auth();
	CHECK(service.get_auth_status("openai_codex").get("connected", false));
}

TEST_CASE("[Editor][SolersSettingsService] keeps an incomplete authorization retryable") {
	SolersProviderRegistry registry;
	SyntheticAuth auth(SNAME("chatgpt-browser"));
	SolersSettingsService service;
	service.set_provider_registry(&registry);
	service.register_auth_method("openai_codex", &auth, false);
	REQUIRE(service.start_provider_auth("openai_codex", "chatgpt-browser").get("ok", false));
	service.poll_auth();
	const Dictionary pending = service.get_auth_status("openai_codex");
	CHECK(pending.get("active", false));
	CHECK(pending.get("state", String()) == "pending");
	service.cancel_auth();
	CHECK_FALSE(service.get_auth_status("openai_codex").get("active", true));
	const Dictionary retry = service.start_provider_auth("openai_codex", "chatgpt-browser");
	CHECK(retry.get("ok", false));
	CHECK(auth.started);
	service.cancel_auth();
	CHECK_FALSE(auth.started);
	CHECK_FALSE(service.get_auth_status("openai_codex").get("active", true));
}

TEST_CASE("[SolersCodexAuth] Godot Thread returns an assigned ID on success") {
	Thread thread;
	bool ran = false;
	const Thread::ID id = thread.start(&_mark_thread_started, &ran);
	REQUIRE(id != Thread::UNASSIGNED_ID);
	thread.wait_to_finish();
	CHECK(ran);
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

TEST_CASE("[Editor][SolersSettingsService] explicit token limits persist and clear") {
	EditorSettings *settings = EditorSettings::get_singleton();
	REQUIRE(settings != nullptr);
	const String prefix = "solers/ai/";
	const String provider = "custom_openai_compatible";
	Array paths;
	paths.push_back(prefix + "provider");
	for (const String &key : { String("configured"), String("model"), String("base_url"), String("context_window"), String("max_tokens"), String("api_key") }) {
		paths.push_back(prefix + "providers/" + provider + "/" + key);
	}
	ScopedEditorSettings restore(settings, paths);

	SolersProviderRegistry registry;
	SolersSettingsService service;
	service.set_provider_registry(&registry);
	Dictionary config;
	config["provider"] = provider;
	config["model"] = "synthetic-model";
	config["base_url"] = "https://gateway.example/v1";
	config["api_key"] = "synthetic-key";
	config["context_window"] = 272000;
	config["max_tokens"] = 32000;
	CHECK(service.set_provider_config(config).get("ok", false));
	Dictionary stored = service.get_provider_config().get("data", Dictionary());
	CHECK((int)stored.get("context_window", 0) == 272000);
	CHECK((int)stored.get("max_tokens", 0) == 32000);

	config["context_window"] = 0;
	config["max_tokens"] = 0;
	CHECK(service.set_provider_config(config).get("ok", false));
	stored = service.get_provider_config().get("data", Dictionary());
	CHECK_FALSE(stored.has("context_window"));
	CHECK_FALSE(stored.has("max_tokens"));
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
