/**************************************************************************/
/*  solers_plugin_tripo.cpp                                               */
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

#include "solers_plugin_tripo.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/os/time.h"

static Dictionary _tripo_operation(const String &p_id) {
	Dictionary op;
	op["operation_id"] = p_id;
	op["agent_supported"] = true;
	Dictionary
		requires;
	requires["kind"] = "3d";
	requires["status"] = "ready";
	Dictionary schema;
	schema["type"] = "object";
	Dictionary properties;
	Array required;
	Dictionary traits;
	traits["model_state"] = "static_model";

	if (p_id == "texture") {
		op["label"] = "Retexture";
		op["intent"] = "material.retexture";
		op["endpoint"] = "/v3/models/texture";
		properties = JSON::parse_string(R"({"model":{"type":"string","enum":["v3.0-20250812","v2.5-20250123"],"default":"v3.0-20250812","label":"Texture Model"},"texture_quality":{"type":"string","enum":["standard","detailed","extreme"],"default":"detailed","label":"Texture Quality"},"pbr":{"type":"boolean","default":true,"label":"PBR Maps"},"texture_prompt":{"type":"object","label":"Texture Prompt","properties":{"text":{"type":"string","label":"Prompt"}},"required":["text"]},"part_names":{"type":"array","label":"Parts","items":{"type":"string"}}})");
	} else if (p_id == "convert") {
		op["label"] = "Convert";
		op["intent"] = "model.convert";
		op["endpoint"] = "/v3/models/convert";
		properties = JSON::parse_string(R"({"format":{"type":"string","enum":["GLTF","USDZ","FBX","OBJ","STL","3MF"],"label":"Format"},"quad":{"type":"boolean","label":"Quad Topology"},"face_limit":{"type":"integer","minimum":1,"label":"Face Limit"},"flatten_bottom":{"type":"boolean","label":"Flatten Bottom"},"scale":{"type":"number","exclusiveMinimum":0,"label":"Scale"},"axis":{"type":"string","enum":["Y_UP","Z_UP"],"label":"Up Axis"},"fbx_preset":{"type":"string","enum":["blender","3dsmax","mixamo"],"label":"FBX Preset"}})");
		required.push_back("format");
	} else if (p_id == "segment") {
		op["label"] = "Split Parts";
		op["intent"] = "geometry.segment";
		op["endpoint"] = "/v3/mesh/segment";
		properties = JSON::parse_string(R"({"model":{"type":"string","enum":["v1.0-20250506","v2.0-20260430"],"default":"v2.0-20260430","label":"Segmentation Model"},"segmentation_granularity":{"type":"string","enum":["simple","balanced","detailed"],"default":"balanced","label":"Granularity"},"split_by_connectivity":{"type":"boolean","default":true,"label":"Split Connected Parts"}})");
		traits["parts"] = "segmented";
	} else if (p_id == "complete") {
		op["label"] = "Complete Parts";
		op["intent"] = "geometry.complete";
		op["endpoint"] = "/v3/mesh/complete";
		properties = Dictionary();
	} else if (p_id == "decimate") {
		op["label"] = "Optimize";
		op["intent"] = "geometry.remesh";
		op["endpoint"] = "/v3/mesh/decimate";
		properties = JSON::parse_string(R"({"model":{"type":"string","enum":["v2.0-20260430","v1.0-20250506"],"default":"v2.0-20260430","label":"Decimation Model"},"face_limit":{"type":"integer","minimum":100,"label":"Target Faces"},"quad":{"type":"boolean","label":"Quad Topology"},"bake":{"type":"boolean","default":true,"label":"Bake Textures"},"part_names":{"type":"array","label":"Parts","items":{"type":"string"}}})");
		required.push_back("face_limit");
		Array remediates;
		remediates.push_back("triangle_budget");
		op["remediates"] = remediates;
	} else if (p_id == "rig_check") {
		op["label"] = "Check Rig";
		op["intent"] = "rig.check";
		op["presentation_order"] = 10;
		op["endpoint"] = "/v3/animations/rig-check";
		requires["model_state"] = "static_model";
		properties = Dictionary();
	} else if (p_id == "rig") {
		op["label"] = "Rig";
		op["intent"] = "rig.bind";
		op["presentation_order"] = 20;
		op["endpoint"] = "/v3/animations/rig";
		requires["model_state"] = "static_model";
		properties = JSON::parse_string(R"({"model":{"type":"string","enum":["v1.0-20240301","v2.5-20260210"],"default":"v2.5-20260210","label":"Rig Model"},"rig_type":{"type":"string","enum":["biped","quadruped","hexapod","octopod","avian","serpentine","aquatic"],"default":"biped","label":"Rig Type"},"spec":{"type":"string","enum":["tripo","mixamo"],"default":"mixamo","label":"Skeleton"},"out_format":{"type":"string","enum":["glb","fbx"],"default":"glb","label":"Format"}})");
		traits["model_state"] = "rigged_model";
		traits["rig"] = "present";
	} else if (p_id == "retarget") {
		op["label"] = "Animate";
		op["intent"] = "animation.preset";
		op["presentation_order"] = 30;
		op["endpoint"] = "/v3/animations/retarget";
		requires["rig"] = "present";
		properties = JSON::parse_string(R"({"animation":{"type":"string","label":"Animation"},"animations":{"type":"array","minItems":1,"items":{"type":"string"},"label":"Animations"},"out_format":{"type":"string","enum":["glb","fbx"],"default":"glb","label":"Format"},"bake_animation":{"type":"boolean","default":true,"label":"Bake Animation"},"export_with_geometry":{"type":"boolean","default":true,"label":"Export with Geometry"},"animate_in_place":{"type":"boolean","default":false,"label":"Animate in Place"}})");
		Array modes;
		modes.push_back(JSON::parse_string(R"({"required":["animation"],"not":{"required":["animations"]}})"));
		modes.push_back(JSON::parse_string(R"({"required":["animations"],"not":{"required":["animation"]}})"));
		schema["oneOf"] = modes;
		traits["model_state"] = "animated_model";
		traits["animation"] = "present";
	} else {
		return Dictionary();
	}
	op["provider_operation_id"] = "tripo.v3." + p_id;
	op["description"] = String(op["label"]);
	op["category"] = p_id == "rig" || p_id == "rig_check" || p_id == "retarget" ? "Animation" : "Model";
	op["stage"] = p_id == "texture" ? "texturing" : p_id + "ing";
	op["docs"] = "https://developers.tripo3d.ai/en/docs" + String(op["endpoint"]).trim_prefix("/v3");
	op["source_inputs"] = p_id == "retarget"
			? JSON::parse_string(R"({"task":{"option":"input","manifest_fields":["provider_task_id"]}})")
			: JSON::parse_string(R"({"task":{"option":"input","manifest_fields":["provider_task_id"]},"model":{"option":"input","formats":["glb","gltf","fbx","obj","stl","3mf"]}})");
	schema["properties"] = properties;
	schema["required"] = required;
	op["options_schema"] = schema;
	op["requires"] =
		requires;
	op["result_traits"] = traits;
	return op;
}

Dictionary SolersPluginTripo::get_profile() const {
	Dictionary profile;
	profile["id"] = "tripo";
	profile["label"] = "Tripo 3D";
	Array kinds;
	kinds.push_back("3d");
	profile["kinds"] = kinds;
	profile["base_url"] = "https://openapi.tripo3d.ai";
	profile["api_key_env"] = "TRIPO_API_KEY";
	profile["docs"] = "https://developers.tripo3d.ai";
	profile["description"] = "Generates and edits persistent 3D assets through Tripo's versioned v3 task API.";
	profile["requires_api_key"] = true;
	profile["supports_generation"] = true;
	profile["supports_catalog"] = false;
	profile["supports_resume"] = true;
	profile["generation_presets"] = JSON::parse_string(R"([
		{"id":"tripo-h3.1","label":"Tripo H3.1","description":"Highest geometry fidelity","default":true,"kind":"3d","options":{"model":"v3.1-20260211","texture":true,"pbr":true,"texture_quality":"detailed","export_uv":true},"input_modes":[{"id":"text","label":"Text prompt","default":true,"provider_mode":"text_to_model","provider_endpoint":"/v3/generation/text-to-model","min_reference_images":0,"max_reference_images":0,"hidden_fields":["enable_image_autofix","texture_alignment"]},{"id":"image","label":"Single image","provider_mode":"image_to_model","provider_endpoint":"/v3/generation/image-to-model","min_reference_images":1,"max_reference_images":1},{"id":"multiview","label":"Multi-view","provider_mode":"multiview_to_model","provider_endpoint":"/v3/generation/multiview-to-model","min_reference_images":2,"max_reference_images":4}],"featured_fields":["texture","pbr","texture_quality","geometry_quality","face_limit"],"presentation":{"controls":{"texture_quality":{"control":"segmented"},"geometry_quality":{"control":"segmented"},"face_limit":{"control":"slider"}}}},
		{"id":"tripo-p1","label":"Tripo P1","description":"Smart low-poly topology","kind":"3d","options":{"model":"P1-20260311","texture":true,"pbr":true,"export_uv":true},"input_modes":[{"id":"text","label":"Text prompt","default":true,"provider_mode":"text_to_model","provider_endpoint":"/v3/generation/text-to-model","min_reference_images":0,"max_reference_images":0,"hidden_fields":["enable_image_autofix","texture_alignment"]},{"id":"image","label":"Single image","provider_mode":"image_to_model","provider_endpoint":"/v3/generation/image-to-model","min_reference_images":1,"max_reference_images":1},{"id":"multiview","label":"Multi-view","provider_mode":"multiview_to_model","provider_endpoint":"/v3/generation/multiview-to-model","min_reference_images":2,"max_reference_images":4}],"featured_fields":["face_limit","texture","pbr"],"hidden_fields":["quad","smart_low_poly","generate_parts","geometry_quality"],"option_constraints":{"face_limit":{"minimum":48,"maximum":20000}},"presentation":{"controls":{"face_limit":{"control":"slider"}}}}
	])");
	return profile;
}

Dictionary SolersPluginTripo::get_generation_options_schema(const String &p_kind) const {
	if (p_kind != "3d") {
		return Dictionary();
	}
	return JSON::parse_string(R"({
		"model":{"type":"string","enum":["v3.1-20260211","P1-20260311"],"default":"v3.1-20260211","label":"Model"},
		"texture":{"type":"boolean","default":true,"label":"Generate Textures"},"pbr":{"type":"boolean","default":true,"label":"PBR Maps"},"texture_quality":{"type":"string","enum":["standard","detailed","extreme"],"default":"detailed","label":"Texture Quality"},
		"face_limit":{"type":"integer","minimum":100,"maximum":2000000,"label":"Face Limit"},"geometry_quality":{"type":"string","enum":["standard","detailed"],"default":"detailed","label":"Geometry Quality"},"auto_size":{"type":"boolean","label":"Automatic Metric Size"},"quad":{"type":"boolean","label":"Quad Topology"},"smart_low_poly":{"type":"boolean","label":"Smart Low Poly"},"generate_parts":{"type":"boolean","label":"Editable Parts"},"compress":{"type":"string","enum":["geometry"],"label":"Compression"},"export_uv":{"type":"boolean","default":true,"label":"Export UV"},"enable_image_autofix":{"type":"boolean","label":"Improve Reference"},"texture_alignment":{"type":"string","enum":["original_image","geometry"],"label":"Texture Alignment"}
	})");
}

Array SolersPluginTripo::get_operation_defs() const {
	Array result;
	const char *ids[] = { "texture", "convert", "segment", "complete", "decimate", "rig_check", "rig", "retarget" };
	for (const char *id : ids) {
		result.push_back(_tripo_operation(id));
	}
	return result;
}

static bool _tripo_boolean(const Dictionary &p_options, const String &p_name, String &r_error) {
	if (!p_options.has(p_name) || p_options[p_name].get_type() == Variant::BOOL) {
		return true;
	}
	r_error = p_name + " must be a boolean.";
	return false;
}

Dictionary SolersPluginTripo::prepare_generate(const String &p_kind, const Dictionary &p_args, Dictionary &r_manifest) const {
	if (p_kind != "3d") {
		return error_data("INVALID_ARGUMENT", "Tripo generation currently supports 3D assets.");
	}
	Dictionary options = Dictionary(r_manifest.get("provider_options", Dictionary())).duplicate(true);
	const Array attachments = r_manifest.get("source_attachments", Array());
	const String prompt = String(p_args.get("prompt", String())).strip_edges();
	const String input_mode_id = String(p_args.get("input_mode", String()));
	const Array presets = get_profile().get("generation_presets", Array());
	Dictionary model_preset;
	const String default_model = presets.is_empty() ? String() : String(Dictionary(Dictionary(presets[0]).get("options", Dictionary())).get("model", String()));
	const String model = String(options.get("model", default_model));
	for (const Variant &preset_value : presets) {
		const Dictionary preset = preset_value;
		if (Dictionary(preset.get("options", Dictionary())).get("model", String()) == model) {
			model_preset = preset;
			break;
		}
	}
	if (model_preset.is_empty()) {
		return error_data("INVALID_ARGUMENT", "Unknown Tripo generation model.");
	}
	Dictionary input_mode;
	for (const Variant &value : Array(model_preset.get("input_modes", Array()))) {
		const Dictionary mode = value;
		if (mode.get("id", String()) == input_mode_id) {
			input_mode = mode;
			break;
		}
	}
	if (input_mode.is_empty()) {
		return error_data("INVALID_ARGUMENT", "The selected Tripo model does not support this input_mode.");
	}
	const int minimum_images = input_mode.get("min_reference_images", 0);
	const int maximum_images = input_mode.get("max_reference_images", 0);
	if (attachments.size() < minimum_images || attachments.size() > maximum_images) {
		return error_data("INVALID_ARGUMENT", vformat("Tripo input_mode '%s' requires %d to %d reference images.", input_mode_id, minimum_images, maximum_images));
	}
	if (input_mode_id == "text" ? prompt.is_empty() : !prompt.is_empty()) {
		return error_data("INVALID_ARGUMENT", input_mode_id == "text" ? "Tripo text input requires a prompt." : "Tripo image inputs do not accept a geometry prompt.");
	}
	options["model"] = model;
	Array hidden_fields = Array(model_preset.get("hidden_fields", Array())).duplicate();
	hidden_fields.append_array(input_mode.get("hidden_fields", Array()));
	for (const Variant &field : hidden_fields) {
		if (options.has(field)) {
			return error_data("INVALID_ARGUMENT", vformat("%s does not support option '%s'.", model, field));
		}
	}
	String option_error;
	const char *booleans[] = { "texture", "pbr", "auto_size", "quad", "smart_low_poly", "generate_parts", "export_uv", "enable_image_autofix" };
	for (const char *name : booleans) {
		if (!_tripo_boolean(options, name, option_error)) {
			return error_data("INVALID_ARGUMENT", option_error);
		}
	}
	if (options.has("face_limit")) {
		const Variant value = options["face_limit"];
		const Dictionary limit = Dictionary(model_preset.get("option_constraints", Dictionary())).get("face_limit", Dictionary());
		const int64_t minimum = limit.get("minimum", 100);
		const int64_t maximum = limit.get("maximum", 2000000);
		if (value.get_type() != Variant::INT || (int64_t)value < minimum || (int64_t)value > maximum) {
			return error_data("INVALID_ARGUMENT", vformat("face_limit must be an integer from %d to %d for %s.", minimum, maximum, model));
		}
	}
	const String quality = String(options.get("texture_quality", "detailed")).to_lower();
	if (quality != "standard" && quality != "detailed" && quality != "extreme") {
		return error_data("INVALID_ARGUMENT", "texture_quality must be standard, detailed, or extreme.");
	}
	options["texture_quality"] = quality;
	if ((bool)options.get("generate_parts", false) && ((bool)options.get("texture", false) || (bool)options.get("pbr", false) || (bool)options.get("quad", false))) {
		return error_data("INVALID_ARGUMENT", "generate_parts cannot be combined with texture, pbr, or quad.");
	}
	if ((bool)options.get("pbr", false) && !(bool)options.get("texture", true)) {
		return error_data("INVALID_ARGUMENT", "pbr requires texture=true.");
	}
	r_manifest["provider_options"] = options;
	r_manifest["input_mode"] = input_mode_id;
	r_manifest["generation_mode"] = input_mode.get("provider_mode", String());
	r_manifest["provider_endpoint"] = input_mode.get("provider_endpoint", String());
	Dictionary traits;
	traits["model_state"] = "static_model";
	r_manifest["traits"] = traits;
	return Dictionary();
}

Dictionary SolersPluginTripo::prepare_operation(const Dictionary &p_operation, const Dictionary &p_source_manifest, Dictionary &r_provider_options) const {
	if (String(r_provider_options.get("input", String())).strip_edges().is_empty()) {
		return error_data("SOURCE_INPUT_MISSING", "Tripo operation requires a task id, file token, or model URL.");
	}
	if (p_operation.get("operation_id", String()) == "retarget") {
		const bool has_animation = !String(r_provider_options.get("animation", String())).strip_edges().is_empty();
		const bool has_animations = !Array(r_provider_options.get("animations", Array())).is_empty();
		if (has_animation == has_animations) {
			return error_data("INVALID_ARGUMENT", "Tripo retarget requires exactly one of animation or animations.");
		}
	}
	return Dictionary();
}

static Dictionary _tripo_data(const Dictionary &p_response, Dictionary &r_error) {
	if (!(bool)p_response.get("ok", false)) {
		r_error = p_response.get("error", SolersPlugin::error_data("PROVIDER_REQUEST_FAILED", "Tripo request failed."));
		return Dictionary();
	}
	const Dictionary parsed = SolersPlugin::parse_json_body(p_response);
	const int64_t provider_code = parsed.get("code", -1);
	if (provider_code != 0) {
		r_error["code"] = String::num_int64(provider_code);
		r_error["message"] = parsed.get("message", "Tripo rejected the request.");
		for (const char *field : { "suggestion", "request_id" }) {
			if (parsed.has(field)) {
				r_error[field] = parsed[field];
			}
		}
		return Dictionary();
	}
	if (parsed.get("data", Variant()).get_type() != Variant::DICTIONARY) {
		r_error = SolersPlugin::error_data("BAD_PROVIDER_RESPONSE", "Tripo response did not contain a data object.");
		return Dictionary();
	}
	return parsed["data"];
}

String SolersPluginTripo::_upload_attachment(const Ref<SolersPluginJob> &p_job, const Dictionary &p_attachment, const Vector<String> &p_headers, Dictionary &r_error) {
	const String path = p_attachment.get("local_path", String());
	const String format = path.get_extension().to_lower() == "jpeg" ? String("jpg") : path.get_extension().to_lower();
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(path);
	if (bytes.is_empty()) {
		r_error = error_data("ATTACHMENT_READ_FAILED", "Could not read the Tripo input attachment.");
		return String();
	}
	Dictionary body;
	body["format"] = format;
	const String base_url = clean_base_url(p_job->get_base_url());
	const Dictionary request = http_request(HTTPClient::METHOD_POST, base_url + "/v3/files/presign", p_headers, utf8_bytes(JSON::stringify(body)), 60000);
	if (!(bool)request.get("ok", false)) {
		r_error = request.get("error", error_data("PRESIGN_FAILED", "Tripo file presigning failed."));
		return String();
	}
	const Dictionary data = _tripo_data(request, r_error);
	const String upload_url = data.get("presigned_url", String());
	const String token = data.get("file_token", String());
	if (upload_url.is_empty() || token.is_empty()) {
		r_error = error_data("BAD_PROVIDER_RESPONSE", "Tripo presign response did not contain an upload URL and file token.");
		return String();
	}
	Vector<String> upload_headers;
	upload_headers.push_back("Content-Type: application/octet-stream");
	const Dictionary upload = http_request(HTTPClient::METHOD_PUT, upload_url, upload_headers, bytes, 180000);
	if (!(bool)upload.get("ok", false)) {
		r_error = upload.get("error", error_data("ATTACHMENT_UPLOAD_FAILED", "Tripo file upload failed."));
		return String();
	}
	return token;
}

Dictionary SolersPluginTripo::_poll_task(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const Vector<String> &p_headers) {
	Dictionary detail;
	const String task_id = r_state.get("provider_task_id", String());
	for (int i = 0; i < 90 && !p_job->is_cancelled(); i++) {
		const Dictionary response = http_request(HTTPClient::METHOD_GET, clean_base_url(p_job->get_base_url()) + "/v3/tasks/" + task_id, p_headers, PackedByteArray(), 60000);
		Dictionary error;
		detail = _tripo_data(response, error);
		if (detail.is_empty()) {
			r_state["status"] = "failed";
			r_state["error"] = error;
			p_job->set_state(r_state);
			return Dictionary();
		}
		const String status = String(detail.get("status", String())).to_lower();
		r_state["provider_status"] = status;
		r_state["progress"] = detail.get("progress", r_state.get("progress", 0));
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
		p_job->set_state(r_state);
		if (status == "success") {
			return detail;
		}
		if (status == "failed" || status == "cancelled" || status == "banned") {
			r_state["status"] = "failed";
			r_state["error"] = error_data("PROVIDER_FAILED", "Tripo task failed.");
			r_state["provider_response"] = detail;
			p_job->set_state(r_state);
			return Dictionary();
		}
		OS::get_singleton()->delay_usec(2000000);
	}
	r_state["status"] = p_job->is_cancelled() ? "interrupted" : "failed";
	r_state["error"] = error_data(p_job->is_cancelled() ? "LOCAL_OBSERVER_STOPPED" : "PROVIDER_TIMEOUT", p_job->is_cancelled() ? "Solers stopped observing the Tripo task; its task id remains available for recovery." : "Tripo task did not finish before the local wait limit.");
	p_job->set_state(r_state);
	return Dictionary();
}

Dictionary SolersPluginTripo::_submit_and_poll(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const String &p_endpoint, const Vector<String> &p_headers, const Dictionary &p_body, const String &p_stage) {
	Dictionary request_fact;
	request_fact["endpoint"] = p_endpoint;
	request_fact["mode"] = r_state.get("generation_mode", r_state.get("operation", String()));
	request_fact["attachment_count"] = Array(r_state.get("source_attachments", Array())).size();
	r_state["provider_request"] = request_fact;
	r_state["stage"] = p_stage;
	r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	p_job->set_state(r_state);
	const Dictionary response = http_request(HTTPClient::METHOD_POST, clean_base_url(p_job->get_base_url()) + p_endpoint, p_headers, utf8_bytes(JSON::stringify(p_body)), 120000);
	Dictionary error;
	const Dictionary data = _tripo_data(response, error);
	const String task_id = data.get("task_id", String());
	if (task_id.is_empty()) {
		r_state["status"] = "failed";
		r_state["error"] = error.is_empty() ? error_data("BAD_PROVIDER_RESPONSE", "Tripo response did not contain a task id.") : error;
		p_job->set_state(r_state);
		return Dictionary();
	}
	r_state["provider_task_id"] = task_id;
	p_job->set_state(r_state);
	return _poll_task(p_job, r_state, p_headers);
}

bool SolersPluginTripo::_download_result(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const Dictionary &p_detail) {
	const Dictionary output = p_detail.get("output", Dictionary());
	const String url = output.get("model_url", String());
	if (url.is_empty()) {
		if (String(r_state.get("operation", String())) == "rig_check") {
			r_state["operation_task_id"] = r_state.get("provider_task_id", String());
			r_state["provider_task_id"] = r_state.get("source_provider_input", String());
			r_state["rig_check"] = output;
			r_state["provider_response"] = p_detail;
			r_state["credits_consumed"] = p_detail.get("credits_consumed", 0.0);
			return true;
		}
		r_state["status"] = "failed";
		r_state["error"] = error_data("NO_DOWNLOAD_URL", "Tripo task did not return a model URL.");
		r_state["provider_response"] = p_detail;
		p_job->set_state(r_state);
		return false;
	}
	const Dictionary response = http_request(HTTPClient::METHOD_GET, url, Vector<String>(), PackedByteArray(), 180000);
	if (!(bool)response.get("ok", false)) {
		r_state["status"] = "failed";
		r_state["error"] = response.get("error", Dictionary());
		p_job->set_state(r_state);
		return false;
	}
	String extension = url.get_extension().get_slice("?", 0).to_lower();
	if (extension.is_empty() || extension.length() > 8) {
		extension = "glb";
	}
	const String path = p_job->get_staging_dir().path_join(p_job->get_asset_id() + "." + extension);
	String write_error;
	if (!write_bytes_atomic(path, response.get("body", PackedByteArray()), write_error)) {
		p_job->fail("WRITE_FAILED", write_error);
		return false;
	}
	p_job->add_file(path);
	p_job->set_preview_url(output.get("rendered_image_url", String()));
	Array files;
	files.push_back(path);
	r_state["import_files"] = files;
	r_state["entrypoints"] = files;
	r_state["provider_response"] = p_detail;
	r_state["credits_consumed"] = p_detail.get("credits_consumed", 0.0);
	return true;
}

void SolersPluginTripo::run_job(const Ref<SolersPluginJob> &p_job) {
	Dictionary state = p_job->get_state();
	Dictionary options = Dictionary(state.get("provider_options", Dictionary())).duplicate(true);
	Vector<String> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + p_job->get_api_key());
	if (p_job->is_resume()) {
		if (String(state.get("provider_task_id", String())).is_empty()) {
			p_job->fail("RESUME_METADATA_MISSING", "The interrupted Tripo task has no provider task id.");
			return;
		}
		const Dictionary detail = _poll_task(p_job, state, headers);
		if (!detail.is_empty() && _download_result(p_job, state, detail)) {
			p_job->set_state(state);
		}
		return;
	}
	String endpoint = state.get("provider_endpoint", String());
	String stage = "generating";
	Dictionary body;
	merge_options(body, options);
	if (String(state.get("operation", "generate")) == "generate") {
		const String generation_mode = state.get("generation_mode", String());
		const Array attachments = state.get("source_attachments", Array());
		if (generation_mode == "text_to_model") {
			body["prompt"] = state.get("prompt", String());
		} else {
			Array tokens;
			for (int i = 0; i < attachments.size(); i++) {
				Dictionary error;
				const String token = _upload_attachment(p_job, attachments[i], headers, error);
				if (token.is_empty()) {
					state["status"] = "failed";
					error["operation"] = "attachment_upload";
					state["error"] = error;
					p_job->set_state(state);
					return;
				}
				tokens.push_back(token);
			}
			if (generation_mode == "image_to_model") {
				body["input"] = tokens[0];
			} else {
				const char *views[] = { "front", "left", "back", "right" };
				Array inputs;
				for (int i = 0; i < tokens.size(); i++) {
					Dictionary view;
					view[views[i]] = tokens[i];
					inputs.push_back(view);
				}
				body["inputs"] = inputs;
			}
		}
	} else {
		const Dictionary definition = _tripo_operation(state.get("operation", String()));
		endpoint = definition.get("endpoint", String());
		stage = definition.get("stage", String());
		const String model_path = body.get("input", String());
		if (!model_path.is_empty() && FileAccess::exists(model_path)) {
			Dictionary attachment;
			attachment["local_path"] = model_path;
			Dictionary upload_error;
			const String token = _upload_attachment(p_job, attachment, headers, upload_error);
			if (token.is_empty()) {
				state["status"] = "failed";
				state["error"] = upload_error;
				p_job->set_state(state);
				return;
			}
			body["input"] = token;
		}
		if (String(state.get("operation", String())) == "rig_check") {
			state["source_provider_input"] = body.get("input", String());
		}
	}
	const Dictionary detail = _submit_and_poll(p_job, state, endpoint, headers, body, stage);
	if (!detail.is_empty() && _download_result(p_job, state, detail)) {
		p_job->set_state(state);
	}
}
