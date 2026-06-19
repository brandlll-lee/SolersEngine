/**************************************************************************/
/*  solers_models_dev.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_models_dev.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/json.h"
#include "core/os/os.h"

static constexpr uint64_t SOLERS_MODELS_DEV_FETCH_BUDGET_MSEC = 30000;

static Dictionary _seed_model(const String &p_id, const String &p_name, int p_context, int p_output, bool p_reasoning, bool p_tool_call) {
	Dictionary m;
	m["id"] = p_id;
	m["name"] = p_name;
	m["context"] = p_context;
	m["output"] = p_output;
	m["reasoning"] = p_reasoning;
	m["tool_call"] = p_tool_call;
	m["attachment"] = false;
	return m;
}

String SolersModelsDev::_cache_path() {
	return "user://solers_ai/models_dev.json";
}

void SolersModelsDev::_load_seed() {
	// Minimal authored offline fallback (our data, not a models.dev copy). The
	// background fetch enriches this with the full models.dev dataset. Limits
	// here are conservative fallbacks; fetched data is authoritative.
	struct SeedProvider {
		const char *id;
		const char *name;
		const char *npm;
		const char *api;
		const char *env;
		bool local;
	};
	static const SeedProvider seeds[] = {
		{ "openai", "OpenAI", "@ai-sdk/openai", "https://api.openai.com/v1", "OPENAI_API_KEY", false },
		{ "anthropic", "Anthropic", "@ai-sdk/anthropic", "https://api.anthropic.com", "ANTHROPIC_API_KEY", false },
		{ "google", "Google Gemini", "@ai-sdk/openai-compatible", "https://generativelanguage.googleapis.com/v1beta/openai", "GEMINI_API_KEY", false },
		{ "deepseek", "DeepSeek", "@ai-sdk/openai-compatible", "https://api.deepseek.com", "DEEPSEEK_API_KEY", false },
		{ "qwen", "Qwen / DashScope", "@ai-sdk/openai-compatible", "https://dashscope.aliyuncs.com/compatible-mode/v1", "DASHSCOPE_API_KEY", false },
		{ "ollama", "Ollama", "@ai-sdk/openai-compatible", "http://127.0.0.1:11434/v1", "", true },
		{ "lm_studio", "LM Studio", "@ai-sdk/openai-compatible", "http://127.0.0.1:1234/v1", "", true },
	};
	for (const SeedProvider &seed : seeds) {
		Dictionary provider;
		provider["id"] = String(seed.id);
		provider["name"] = String(seed.name);
		provider["npm"] = String(seed.npm);
		provider["api"] = String(seed.api);
		Array env;
		if (seed.env[0] != '\0') {
			env.push_back(String(seed.env));
		}
		provider["env"] = env;
		provider["local"] = seed.local;
		provider["models"] = Dictionary();
		providers[StringName(seed.id)] = provider;
	}

	// A few representative models so context limits resolve offline. Fetched
	// models.dev data supersedes these on the next launch.
	{
		Dictionary openai_models;
		openai_models["gpt-4o"] = _seed_model("gpt-4o", "GPT-4o", 128000, 16384, false, true);
		openai_models["gpt-4o-mini"] = _seed_model("gpt-4o-mini", "GPT-4o mini", 128000, 16384, false, true);
		Dictionary p = providers[StringName("openai")];
		p["models"] = openai_models;
		providers[StringName("openai")] = p;
	}
	{
		Dictionary anthropic_models;
		anthropic_models["claude-3-5-sonnet-latest"] = _seed_model("claude-3-5-sonnet-latest", "Claude 3.5 Sonnet", 200000, 8192, false, true);
		Dictionary p = providers[StringName("anthropic")];
		p["models"] = anthropic_models;
		providers[StringName("anthropic")] = p;
	}
}

void SolersModelsDev::_ingest(const Dictionary &p_root) {
	const Array provider_ids = p_root.keys();
	for (int i = 0; i < provider_ids.size(); i++) {
		const String provider_id = provider_ids[i];
		const Variant provider_value = p_root[provider_id];
		if (provider_value.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary po = provider_value;

		Dictionary out_provider;
		out_provider["id"] = po.get("id", provider_id);
		out_provider["name"] = po.get("name", provider_id);
		out_provider["npm"] = po.get("npm", "@ai-sdk/openai-compatible");
		out_provider["api"] = po.get("api", "");
		out_provider["env"] = po.get("env", Array());
		out_provider["local"] = po.get("local", false);

		Dictionary models_out;
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
				Dictionary model;
				model["id"] = mo.get("id", model_id);
				model["name"] = mo.get("name", model_id);
				model["context"] = (int)limit.get("context", 0);
				model["output"] = (int)limit.get("output", 0);
				model["reasoning"] = mo.get("reasoning", false);
				model["tool_call"] = mo.get("tool_call", true);
				model["attachment"] = mo.get("attachment", false);
				models_out[model_id] = model;
			}
		}
		out_provider["models"] = models_out;
		providers[StringName(provider_id)] = out_provider;
	}
}

void SolersModelsDev::_load_cache() {
	const String path = _cache_path();
	if (!FileAccess::exists(path)) {
		return;
	}
	const String json = FileAccess::get_file_as_string(path);
	if (json.is_empty()) {
		return;
	}
	const Variant parsed = JSON::parse_string(json);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return;
	}
	_ingest(parsed);
}

void SolersModelsDev::initialize() {
	_load_seed();
	_load_cache();
	if (!refresh_started.is_set()) {
		refresh_started.set();
		refresh_thread.start(&SolersModelsDev::_refresh_func, this);
	}
}

void SolersModelsDev::_refresh_func(void *p_userdata) {
	static_cast<SolersModelsDev *>(p_userdata)->_run_refresh();
}

void SolersModelsDev::_run_refresh() {
	// Best-effort background refresh of the cache for the next launch. Any
	// failure leaves the seed/cache untouched.
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
	if (JSON::parse_string(json).get_type() != Variant::DICTIONARY) {
		return;
	}

	const String path = _cache_path();
	DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(path.get_base_dir()));
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(json);
	}
}

bool SolersModelsDev::has_provider(const StringName &p_id) const {
	return providers.has(p_id);
}

Dictionary SolersModelsDev::get_provider(const StringName &p_id) const {
	const Dictionary *found = providers.getptr(p_id);
	return found ? *found : Dictionary();
}

Dictionary SolersModelsDev::get_model(const StringName &p_provider, const String &p_model) const {
	const Dictionary *found = providers.getptr(p_provider);
	if (!found) {
		return Dictionary();
	}
	const Dictionary models = found->get("models", Dictionary());
	const Variant model = models.get(p_model, Variant());
	return model.get_type() == Variant::DICTIONARY ? (Dictionary)model : Dictionary();
}

Array SolersModelsDev::list_providers() const {
	Array out;
	for (const KeyValue<StringName, Dictionary> &kv : providers) {
		out.push_back(kv.value);
	}
	return out;
}

SolersModelsDev::SolersModelsDev() {}

SolersModelsDev::~SolersModelsDev() {
	if (refresh_thread.is_started()) {
		refresh_thread.wait_to_finish();
	}
}
