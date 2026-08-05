/**************************************************************************/
/*  solers_plugin_elevenlabs.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_plugin_elevenlabs.h"

#include "core/io/json.h"

Dictionary SolersPluginElevenLabs::get_profile() const {
	Dictionary profile;
	profile["id"] = "elevenlabs";
	profile["label"] = "ElevenLabs";
	Array kinds;
	kinds.push_back("music");
	kinds.push_back("sfx");
	profile["kinds"] = kinds;
	profile["base_url"] = "https://api.elevenlabs.io";
	profile["api_key_env"] = "ELEVENLABS_API_KEY";
	profile["docs"] = "https://elevenlabs.io/docs/api-reference";
	profile["description"] = "Generates music and sound effects from text prompts.";
	profile["requires_api_key"] = true;
	profile["supports_generation"] = true;
	profile["supports_catalog"] = false;
	profile["supports_resume"] = false;
	return profile;
}

void SolersPluginElevenLabs::run_job(const Ref<SolersPluginJob> &p_job) {
	Dictionary state = p_job->get_state();
	const String kind = state.get("kind", String());
	const String prompt = state.get("prompt", String());
	const Dictionary provider_options = state.get("provider_options", Dictionary());
	Dictionary body;
	merge_options(body, provider_options);
	if (kind == "music") {
		if (!body.has("prompt")) {
			body["prompt"] = prompt;
		}
	} else {
		if (!body.has("text")) {
			body["text"] = prompt;
		}
	}
	Vector<String> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Accept: audio/mpeg");
	headers.push_back("xi-api-key: " + p_job->get_api_key());
	const String endpoint = kind == "music" ? "/v1/music" : "/v1/sound-generation";
	Dictionary response = p_job->http_request("POST", clean_base_url(p_job->get_base_url()) + endpoint, headers, utf8_bytes(JSON::stringify(body)), 120000);
	if (!(bool)response.get("ok", false)) {
		state["status"] = "failed";
		state["error"] = response.get("error", Dictionary());
		p_job->set_state(state);
		return;
	}
	const String output_format = String(provider_options.get("output_format", String())).to_lower();
	const String ext = output_format.contains("wav") ? "wav" : "mp3";
	const String file_path = p_job->get_staging_dir().path_join(p_job->get_asset_id() + "." + ext);
	String write_error;
	if (!write_bytes_atomic(file_path, response.get("body", PackedByteArray()), write_error)) {
		p_job->fail("WRITE_FAILED", write_error);
		return;
	}
	p_job->add_file(file_path);
}
