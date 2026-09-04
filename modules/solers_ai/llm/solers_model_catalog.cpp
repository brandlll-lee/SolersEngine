/**************************************************************************/
/*  solers_model_catalog.cpp                                              */
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

#include "solers_model_catalog.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/json.h"
#include "core/os/os.h"

#include "modules/solers_ai/generated/solers_model_catalog.gen.h"

static constexpr uint64_t SOLERS_MODELS_DEV_FETCH_BUDGET_MSEC = 30000;
static constexpr uint64_t SOLERS_MODELS_DEV_REFRESH_TTL_MSEC = 10 * 60 * 1000;

String SolersModelCatalog::_resolve_cache_path() {
	return OS::get_singleton()->get_data_path().path_join(OS::get_singleton()->get_godot_dir_name()).path_join("solers/models.json");
}

void SolersModelCatalog::_load_builtin() {
	const Variant parsed = JSON::parse_string(SOLERS_BUILTIN_MODEL_CATALOG);
	ERR_FAIL_COND(parsed.get_type() != Variant::DICTIONARY);
	_merge_catalog(parsed, builtin_providers);
}

void SolersModelCatalog::_merge_catalog(const Dictionary &p_root, HashMap<StringName, Dictionary> &r_providers) const {
	const Array provider_ids = p_root.keys();
	for (int i = 0; i < provider_ids.size(); i++) {
		const String provider_id = provider_ids[i];
		const Variant provider_value = p_root[provider_id];
		if (provider_value.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary po = provider_value;

		Dictionary out_provider;
		if (const Dictionary *seed = r_providers.getptr(StringName(provider_id))) {
			out_provider = *seed;
		}
		out_provider["id"] = po.get("id", provider_id);
		out_provider["name"] = po.get("name", provider_id);
		out_provider["npm"] = po.get("npm", out_provider.get("npm", String()));
		const Variant provider_api = po.get("api", Variant());
		if (provider_api.get_type() == Variant::STRING && !String(provider_api).is_empty()) {
			out_provider["api"] = provider_api;
		}
		out_provider["env"] = po.get("env", Array());
		out_provider["local"] = po.get("local", false);
		for (const String &field : { String("protocol"), String("auth_header"), String("auth_prefix") }) {
			if (po.has(field)) {
				out_provider[field] = po[field];
			}
		}

		Dictionary models_out = out_provider.get("models", Dictionary());
		const Variant models_value = po.get("models", Dictionary());
		if (models_value.get_type() == Variant::DICTIONARY) {
			const Dictionary models_in = models_value;
			const Array model_ids = models_in.keys();
			for (int m = 0; m < model_ids.size(); m++) {
				const String model_id = model_ids[m];
				const Variant model_value = models_in[model_id];
				if (model_value.get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary mo = model_value;
				Dictionary limit;
				if (mo.get("limit", Dictionary()).get_type() == Variant::DICTIONARY) {
					limit = mo.get("limit", Dictionary());
				}
				Dictionary model = Dictionary(models_out.get(model_id, Dictionary())).duplicate(true);
				model["id"] = mo.get("id", model_id);
				model["name"] = mo.get("name", model_id);
				model["context"] = (int)limit.get("context", 0);
				model["output"] = (int)limit.get("output", 0);
				Dictionary cost = mo.get("cost", Dictionary());
				Array tiers;
				for (const Variant &tier_value : Array(cost.get("tiers", Array()))) {
					Dictionary tier = tier_value;
					const Dictionary threshold = tier.get("tier", Dictionary());
					const String threshold_type = threshold.get("type", String());
					if (!tier.has("input_tokens_above") && (threshold_type == "input" || threshold_type == "context")) {
						tier["input_tokens_above"] = threshold.get("size", 0);
					}
					tier.erase("tier");
					tiers.push_back(tier);
				}
				if (!tiers.is_empty()) {
					cost["tiers"] = tiers;
				}
				model["cost"] = cost;
				model["reasoning"] = mo.get("reasoning", false);
				model["reasoning_options"] = mo.get("reasoning_options", Array());
				model["tool_call"] = mo.get("tool_call", true);
				model["attachment"] = mo.get("attachment", false);
				model["status"] = mo.get("status", "active");
				const Dictionary modalities = mo.get("modalities", Dictionary());
				model["input_modalities"] = modalities.get("input", Array());
				model["output_modalities"] = modalities.get("output", Array());
				const String protocol = mo.get("protocol", String());
				if (!protocol.is_empty()) {
					model["protocol"] = protocol;
				}
				const Dictionary model_provider = mo.get("provider", Dictionary());
				const String provider_npm = model_provider.get("npm", String());
				const String provider_api = model_provider.get("api", String());
				if (!provider_npm.is_empty()) {
					model["provider_npm"] = provider_npm;
				}
				if (!provider_api.is_empty()) {
					model["provider_api"] = provider_api;
				}
				models_out[model_id] = model;
			}
		}
		out_provider["models"] = models_out;
		r_providers[StringName(provider_id)] = out_provider;
	}
}

void SolersModelCatalog::_rebuild() {
	HashMap<StringName, Dictionary> next;
	Dictionary remote;
	Dictionary overrides;
	{
		MutexLock lock(providers_mutex);
		for (const KeyValue<StringName, Dictionary> &kv : builtin_providers) {
			next[kv.key] = kv.value.duplicate(true);
		}
		remote = remote_catalog;
		overrides = model_overrides;
	}
	_merge_catalog(remote, next);
	for (const Variant &key_value : overrides.keys()) {
		const String key = key_value;
		const int separator = key.find("/");
		if (separator <= 0 || separator == key.length() - 1) {
			continue;
		}
		const StringName provider_id = key.substr(0, separator);
		const String model_id = key.substr(separator + 1);
		if (overrides[key].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary *provider = next.getptr(provider_id);
		if (!provider) {
			Dictionary created;
			created["id"] = String(provider_id);
			created["name"] = String(provider_id);
			created["models"] = Dictionary();
			next[provider_id] = created;
			provider = next.getptr(provider_id);
		}
		Dictionary models = provider->get("models", Dictionary());
		Dictionary model = models.get(model_id, Dictionary());
		model["id"] = model_id;
		model.merge(Dictionary(overrides[key]), true);
		models[model_id] = model;
		(*provider)["models"] = models;
	}
	MutexLock lock(providers_mutex);
	providers = next;
	catalog_revision++;
}

void SolersModelCatalog::_load_cache() {
	if (!FileAccess::exists(cache_path)) {
		return;
	}
	const String json = FileAccess::get_file_as_string(cache_path);
	if (json.is_empty()) {
		return;
	}
	const Variant parsed = JSON::parse_string(json);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return;
	}
	remote_catalog = parsed;
}

void SolersModelCatalog::initialize() {
	cache_path = _resolve_cache_path();
	_load_builtin();
	_load_cache();
	_rebuild();
}

void SolersModelCatalog::set_model_overrides(const Dictionary &p_overrides) {
	{
		MutexLock lock(providers_mutex);
		model_overrides = p_overrides.duplicate(true);
	}
	_rebuild();
}

void SolersModelCatalog::refresh() {
	if (refresh_started.is_set()) {
		return;
	}
	{
		MutexLock lock(providers_mutex);
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		if (last_refresh_msec > 0 && now - last_refresh_msec < SOLERS_MODELS_DEV_REFRESH_TTL_MSEC) {
			return;
		}
		last_refresh_msec = now;
	}
	if (refresh_thread.is_started()) {
		refresh_thread.wait_to_finish();
	}
	refresh_started.set();
	if (refresh_thread.start(&SolersModelCatalog::_refresh_func, this) == Thread::UNASSIGNED_ID) {
		refresh_started.clear();
		MutexLock lock(providers_mutex);
		last_refresh_msec = 0;
	}
}

void SolersModelCatalog::_refresh_func(void *p_userdata) {
	SolersModelCatalog *catalog = static_cast<SolersModelCatalog *>(p_userdata);
	catalog->_run_refresh();
	catalog->refresh_started.clear();
}

void SolersModelCatalog::_run_refresh() {
	// Best-effort background refresh. Any failure leaves the built-in/cache
	// untouched; valid data becomes visible to the current session immediately.
	Ref<HTTPClient> http = HTTPClient::create();
	if (http.is_null()) {
		return;
	}
	Ref<TLSOptions> tls = TLSOptions::client();
	if (http->connect_to_host("models.dev", 443, tls) != OK) {
		return;
	}

	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + SOLERS_MODELS_DEV_FETCH_BUDGET_MSEC;
	bool requested = false;
	bool reading_body = false;
	PackedByteArray body;

	while (OS::get_singleton()->get_ticks_msec() < deadline) {
		http->poll();
		const HTTPClient::Status status = http->get_status();
		if (status == HTTPClient::STATUS_RESOLVING || status == HTTPClient::STATUS_CONNECTING || status == HTTPClient::STATUS_REQUESTING) {
			OS::get_singleton()->delay_usec(10000);
			continue;
		}
		if (status == HTTPClient::STATUS_CONNECTED) {
			if (!requested) {
				Vector<String> headers;
				headers.push_back("Accept: application/json");
				headers.push_back("User-Agent: Solers");
				if (http->request(HTTPClient::METHOD_GET, "/api.json", headers, nullptr, 0) != OK) {
					return;
				}
				requested = true;
				OS::get_singleton()->delay_usec(10000);
				continue;
			}
			// Body fully consumed; connection back to keep-alive idle.
			if (reading_body) {
				break;
			}
			OS::get_singleton()->delay_usec(10000);
			continue;
		}
		if (status == HTTPClient::STATUS_BODY) {
			reading_body = true;
			const int code = http->get_response_code();
			if (code != 200) {
				return;
			}
			const PackedByteArray chunk = http->read_response_body_chunk();
			if (chunk.size() > 0) {
				body.append_array(chunk);
			}
			OS::get_singleton()->delay_usec(5000);
			continue;
		}
		if (status == HTTPClient::STATUS_DISCONNECTED) {
			break;
		}
		// Any error status.
		return;
	}

	if (body.is_empty()) {
		return;
	}
	const String json = String::utf8((const char *)body.ptr(), body.size());
	const Variant parsed = JSON::parse_string(json);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return;
	}
	{
		MutexLock lock(providers_mutex);
		remote_catalog = parsed;
	}
	_rebuild();

	const String path = cache_path;
	DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(json);
	}
}

uint64_t SolersModelCatalog::get_catalog_revision() const {
	MutexLock lock(providers_mutex);
	return catalog_revision;
}

bool SolersModelCatalog::has_provider(const StringName &p_id) const {
	MutexLock lock(providers_mutex);
	return providers.has(p_id);
}

static Dictionary _solers_provider_meta(const Dictionary &p_provider) {
	// Metadata only — never deep-copy the models table for profile/UI hot paths.
	Dictionary out;
	out["id"] = p_provider.get("id", String());
	out["name"] = p_provider.get("name", String());
	out["api"] = p_provider.get("api", String());
	out["npm"] = p_provider.get("npm", String());
	out["env"] = Array(p_provider.get("env", Array())).duplicate();
	out["local"] = p_provider.get("local", false);
	out["protocol"] = p_provider.get("protocol", String());
	for (const String &field : { String("auth_header"), String("auth_prefix") }) {
		if (p_provider.has(field)) {
			out[field] = p_provider[field];
		}
	}
	const Variant models_v = p_provider.get("models", Dictionary());
	if (models_v.get_type() == Variant::DICTIONARY) {
		const Array ids = Dictionary(models_v).keys();
		if (!ids.is_empty()) {
			out["default_model"] = ids[0];
		}
	}
	return out;
}

Dictionary SolersModelCatalog::get_provider(const StringName &p_id) const {
	MutexLock lock(providers_mutex);
	const Dictionary *found = providers.getptr(p_id);
	return found ? _solers_provider_meta(*found) : Dictionary();
}

Dictionary SolersModelCatalog::get_model(const StringName &p_provider, const String &p_model) const {
	MutexLock lock(providers_mutex);
	const Dictionary *found = providers.getptr(p_provider);
	if (!found) {
		return Dictionary();
	}
	const Dictionary models = found->get("models", Dictionary());
	const Variant model = models.get(p_model, Variant());
	return model.get_type() == Variant::DICTIONARY ? Dictionary(model).duplicate(true) : Dictionary();
}

int SolersModelCatalog::input_modality_support(const Dictionary &p_model, const String &p_modality) {
	const Variant declared = p_model.get("input_modalities", Variant());
	if (declared.get_type() != Variant::ARRAY || Array(declared).is_empty()) {
		return -1;
	}
	return Array(declared).has(p_modality) ? 1 : 0;
}

Array SolersModelCatalog::reasoning_efforts(const Dictionary &p_model) {
	Array efforts;
	for (const Variant &option_value : Array(p_model.get("reasoning_options", Array()))) {
		if (option_value.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary option = option_value;
		if (option.get("type", String()) != "effort") {
			continue;
		}
		for (const Variant &effort_value : Array(option.get("values", Array()))) {
			const String effort = String(effort_value).strip_edges();
			if (!effort.is_empty() && !efforts.has(effort)) {
				efforts.push_back(effort);
			}
		}
	}
	return efforts;
}

Array SolersModelCatalog::list_providers() const {
	MutexLock lock(providers_mutex);
	Array result;
	Vector<String> ids;
	ids.resize(providers.size());
	int i = 0;
	for (const KeyValue<StringName, Dictionary> &kv : providers) {
		ids.write[i++] = String(kv.key);
	}
	ids.sort();
	for (const String &id : ids) {
		const Dictionary *found = providers.getptr(StringName(id));
		if (found) {
			result.push_back(_solers_provider_meta(*found));
		}
	}
	return result;
}

Array SolersModelCatalog::list_model_ids(const StringName &p_provider) const {
	MutexLock lock(providers_mutex);
	const Dictionary *found = providers.getptr(p_provider);
	if (!found) {
		return Array();
	}
	const Dictionary models = found->get("models", Dictionary());
	return models.keys();
}

namespace {
struct SolersProviderAlias {
	const char *from;
	const char *to;
};
// Single alias table — migration + runtime canonicalization.
static const SolersProviderAlias SOLERS_PROVIDER_ALIASES[] = {
	{ "gemini", "google" },
	{ "anthropic_messages", "anthropic" },
	{ "lm_studio", "lmstudio" },
};
} // namespace

Array SolersModelCatalog::list_popular_provider_ids() const {
	// Catalog ordering data, not a behavior switch.
	static const char *POPULAR[] = {
		"openai",
		"anthropic",
		"google",
		"deepseek",
		"openrouter",
		"qwen",
		"minimax",
		"moonshotai",
		"xai",
		"zhipuai",
		"zai-coding-plan",
		"opencode",
		"opencode-go",
		"ollama",
		"lmstudio",
	};
	Array ids;
	for (const char *id : POPULAR) {
		ids.push_back(String(id));
	}
	return ids;
}

Array SolersModelCatalog::list_legacy_provider_ids() const {
	Array ids;
	for (const SolersProviderAlias &alias : SOLERS_PROVIDER_ALIASES) {
		ids.push_back(String(alias.from));
	}
	return ids;
}

String SolersModelCatalog::canonical_provider_id(const String &p_id) const {
	for (const SolersProviderAlias &alias : SOLERS_PROVIDER_ALIASES) {
		if (p_id == alias.from) {
			return String(alias.to);
		}
	}
	return p_id;
}

SolersModelCatalog::SolersModelCatalog() {}

SolersModelCatalog::~SolersModelCatalog() {
	if (refresh_thread.is_started()) {
		refresh_thread.wait_to_finish();
	}
}
