/**************************************************************************/
/*  solers_plugin_elevenlabs.cpp                                          */
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

Dictionary SolersPluginElevenLabs::get_generation_options_schema(const String &p_kind) const {
	const char *schema_json = p_kind == "sfx"
			? R"json({"duration_seconds":{"type":"number","minimum":0.5,"maximum":30.0,"label":"Duration (seconds)"},"loop":{"type":"boolean","default":false,"label":"Seamless Loop"},"prompt_influence":{"type":"number","minimum":0.0,"maximum":1.0,"default":0.3,"label":"Prompt Influence"},"model_id":{"type":"string","enum":["eleven_text_to_sound_v2"],"default":"eleven_text_to_sound_v2","label":"Model"},"output_format":{"type":"string","enum":["mp3_44100_128","mp3_44100_192"],"default":"mp3_44100_128","label":"Output Format"}})json"
			: p_kind == "music"
			? R"json({"composition_plan":{"type":"object","label":"Composition Plan","description":"Optional ElevenLabs composition plan. Use instead of a prompt."},"music_length_ms":{"type":"integer","minimum":3000,"maximum":600000,"label":"Length (milliseconds)"},"model_id":{"type":"string","enum":["music_v1","music_v2"],"default":"music_v1","label":"Model"},"force_instrumental":{"type":"boolean","default":false,"label":"Instrumental"},"output_format":{"type":"string","enum":["mp3_44100_128","mp3_44100_192"],"default":"mp3_44100_128","label":"Output Format"}})json"
			: nullptr;
	if (!schema_json) {
		return Dictionary();
	}
	const Variant parsed = JSON::parse_string(schema_json);
	return parsed.get_type() == Variant::DICTIONARY ? (Dictionary)parsed : Dictionary();
}

Dictionary SolersPluginElevenLabs::prepare_generate(const String &p_kind, const Dictionary &p_args, Dictionary &r_manifest) const {
	Dictionary options = Dictionary(r_manifest.get("provider_options", Dictionary())).duplicate(true);
	const String prompt = String(p_args.get("prompt", String())).strip_edges();
	if (p_kind == "sfx") {
		if (prompt.is_empty()) {
			return error_data("INVALID_ARGUMENT", "Sound effect generation requires a prompt.");
		}
		if (options.has("duration_seconds")) {
			const Variant duration = options["duration_seconds"];
			if (!duration.is_num() || (double)duration < 0.5 || (double)duration > 30.0) {
				return error_data("INVALID_ARGUMENT", "duration_seconds must be between 0.5 and 30.");
			}
		}
		if (options.has("prompt_influence")) {
			const Variant influence = options["prompt_influence"];
			if (!influence.is_num() || (double)influence < 0.0 || (double)influence > 1.0) {
				return error_data("INVALID_ARGUMENT", "prompt_influence must be between 0 and 1.");
			}
		}
	} else if (p_kind == "music") {
		const bool has_plan = options.has("composition_plan");
		const bool has_prompt = !prompt.is_empty();
		if (has_prompt == has_plan) {
			return error_data("INVALID_ARGUMENT", "Music generation requires exactly one prompt or composition_plan.");
		}
		if (options.has("music_length_ms")) {
			const Variant length = options["music_length_ms"];
			if (length.get_type() != Variant::INT || (int64_t)length < 3000 || (int64_t)length > 600000) {
				return error_data("INVALID_ARGUMENT", "music_length_ms must be an integer between 3000 and 600000.");
			}
		}
	} else {
		return error_data("INVALID_ARGUMENT", "ElevenLabs generates music or sfx assets.");
	}
	const String output_format = String(options.get("output_format", "mp3_44100_128")).to_lower();
	if (output_format != "mp3_44100_128" && output_format != "mp3_44100_192") {
		return error_data("INVALID_ARGUMENT", "output_format must be an importable MP3 format.");
	}
	options["output_format"] = output_format;
	r_manifest["provider_options"] = options;
	return Dictionary();
}

void SolersPluginElevenLabs::run_job(const Ref<SolersPluginJob> &p_job) {
	Dictionary state = p_job->get_state();
	const String kind = state.get("kind", String());
	const String prompt = state.get("prompt", String());
	const Dictionary provider_options = state.get("provider_options", Dictionary());
	Dictionary body;
	merge_options(body, provider_options);
	const String output_format = String(body.get("output_format", "mp3_44100_128"));
	body.erase("output_format");
	if (kind == "music") {
		if (!body.has("prompt") && !body.has("composition_plan")) {
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
	const String url = clean_base_url(p_job->get_base_url()) + endpoint + "?output_format=" + output_format.uri_encode();
	Dictionary response = p_job->http_request(HTTPClient::METHOD_POST, url, headers, utf8_bytes(JSON::stringify(body)), 120000);
	if (!(bool)response.get("ok", false)) {
		state["status"] = "failed";
		state["error"] = response.get("error", Dictionary());
		p_job->set_state(state);
		return;
	}
	const String file_path = p_job->get_staging_dir().path_join(p_job->get_asset_id() + ".mp3");
	String write_error;
	if (!write_bytes_atomic(file_path, response.get("body", PackedByteArray()), write_error)) {
		p_job->fail("WRITE_FAILED", write_error);
		return;
	}
	p_job->add_file(file_path);
}
