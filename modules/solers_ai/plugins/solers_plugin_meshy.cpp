/**************************************************************************/
/*  solers_plugin_meshy.cpp                                               */
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

#include "solers_plugin_meshy.h"

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/os/time.h"

static Dictionary _option_schema(const String &p_type, const String &p_label) {
	Dictionary option;
	option["type"] = p_type;
	option["label"] = p_label;
	return option;
}

static Dictionary _operation_def(const String &p_operation_id) {
	Dictionary op;
	op["operation_id"] = p_operation_id;
	op["agent_supported"] = true;
	Dictionary requirement;
	requirement["kind"] = "3d";
	Dictionary schema;
	Dictionary properties;
	Array required;
	Dictionary result_traits;

	if (p_operation_id == "refine") {
		op["label"] = "Refine to Ready";
		op["description"] = "Create a ready project version.";
		op["category"] = "Model";
		op["provider_operation_id"] = "meshy.openapi.v2.text-to-3d.refine";
		op["endpoint"] = "/openapi/v2/text-to-3d";
		requirement["status"] = "draft";
		Array task_id_fields;
		task_id_fields.push_back("preview_task_id");
		task_id_fields.push_back("provider_task_id");
		requirement["task_id_fields"] = task_id_fields;
		result_traits["model_state"] = "static_model";
	} else if (p_operation_id == "remesh") {
		op["label"] = "Optimize";
		op["description"] = "Create a new optimized version.";
		op["category"] = "Geometry";
		op["provider_operation_id"] = "meshy.openapi.v1.remesh";
		op["endpoint"] = "/openapi/v1/remesh";
		requirement["status"] = "ready";
		requirement["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requirement["task_id_fields"] = task_id_fields;
		Array topology;
		topology.push_back("triangle");
		topology.push_back("quad");
		Dictionary topology_option = _option_schema("string", "Topology");
		topology_option["enum"] = topology;
		topology_option["default"] = "triangle";
		properties["topology"] = topology_option;
		Dictionary polycount_option = _option_schema("integer", "Target Polygons");
		polycount_option["minimum"] = 100;
		polycount_option["maximum"] = 300000;
		polycount_option["default"] = 30000;
		properties["target_polycount"] = polycount_option;
		Array ui_order;
		ui_order.push_back("target_polycount");
		ui_order.push_back("topology");
		schema["ui_order"] = ui_order;
		result_traits["model_state"] = "static_model";
		Array remediates;
		remediates.push_back("triangle_budget");
		op["remediates"] = remediates;
		op["presentation"] = JSON::parse_string(R"({"controls":{"target_polycount":{"control":"slider"},"topology":{"control":"segmented"}}})");
	} else if (p_operation_id == "retexture") {
		op["label"] = "Restyle";
		op["description"] = "Create a new material version.";
		op["category"] = "Material";
		op["provider_operation_id"] = "meshy.openapi.v1.retexture";
		op["endpoint"] = "/openapi/v1/retexture";
		requirement["status"] = "ready";
		requirement["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requirement["task_id_fields"] = task_id_fields;
		const Dictionary style_schema = JSON::parse_string(R"({"properties":{"text_style_prompt":{"type":"string","label":"Style Prompt"},"image_style_url":{"type":"string","label":"Style Image URL"},"multiview_image_urls":{"type":"array","label":"Style Image URLs","items":{"type":"string"},"minItems":1,"maxItems":4},"texture_resolution":{"type":"string","label":"Texture Resolution","enum":["2k","4k","8k"]}},"oneOf":[{"required":["text_style_prompt"]},{"required":["image_style_url"]},{"required":["multiview_image_urls"]}]})");
		const Dictionary style_properties = style_schema.get("properties", Dictionary());
		for (const Variant *key = style_properties.next(nullptr); key; key = style_properties.next(key)) {
			properties[*key] = style_properties[*key];
		}
		schema["oneOf"] = style_schema.get("oneOf", Array());
		result_traits["model_state"] = "static_model";
	} else if (p_operation_id == "rig_humanoid") {
		op["label"] = "Rig";
		op["description"] = "Create a new rigged version.";
		op["category"] = "Character";
		op["provider_operation_id"] = "meshy.openapi.v1.rigging";
		op["endpoint"] = "/openapi/v1/rigging";
		requirement["status"] = "ready";
		requirement["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requirement["task_id_fields"] = task_id_fields;
		properties["humanoid_confirmed"] = _option_schema("boolean", "This is a humanoid character");
		required.push_back("humanoid_confirmed");
		result_traits["model_state"] = "rigged_model";
		result_traits["rig"] = "humanoid";
	} else if (p_operation_id == "animate_humanoid") {
		op["label"] = "Animate";
		op["description"] = "Create a new animated version.";
		op["category"] = "Character";
		op["provider_operation_id"] = "meshy.openapi.v1.animations";
		op["endpoint"] = "/openapi/v1/animations";
		requirement["status"] = "ready";
		requirement["rig"] = "humanoid";
		Array task_id_fields;
		task_id_fields.push_back("rig_task_id");
		task_id_fields.push_back("provider_task_id");
		requirement["task_id_fields"] = task_id_fields;
		const Dictionary animation_properties = JSON::parse_string(R"({"action_id":{"type":"integer","label":"Action ID","description":"Meshy official Animation Library action_id. Use animation_actions from asset.capabilities; do not guess.","enum_source":"animation_actions","enum_value":"action_id","enum_label":"name"},"post_process":{"type":"object","label":"Post Process","properties":{"operation_type":{"type":"string","label":"Operation","enum":["change_fps"]},"fps":{"type":"integer","label":"Frame Rate","enum":[24,25,30,60]}},"required":["operation_type","fps"]}})");
		properties["action_id"] = animation_properties["action_id"];
		properties["post_process"] = animation_properties["post_process"];
		required.push_back("action_id");
		result_traits["model_state"] = "animated_model";
		result_traits["rig"] = "humanoid";
		result_traits["animation"] = "present";
	} else if (p_operation_id == "convert") {
		op["label"] = "Convert Format";
		op["description"] = "Create project-ready model files in requested formats.";
		op["category"] = "Model";
		op["provider_operation_id"] = "meshy.openapi.v1.convert";
		op["endpoint"] = "/openapi/v1/convert";
		requirement["status"] = "ready";
		requirement["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requirement["task_id_fields"] = task_id_fields;
		Array formats;
		formats.push_back("glb");
		formats.push_back("fbx");
		formats.push_back("obj");
		formats.push_back("usdz");
		formats.push_back("blend");
		formats.push_back("stl");
		formats.push_back("3mf");
		Dictionary format_item = _option_schema("string", "Format");
		format_item["enum"] = formats;
		Dictionary target_formats = _option_schema("array", "Target Formats");
		target_formats["items"] = format_item;
		target_formats["minItems"] = 1;
		target_formats["uniqueItems"] = true;
		properties["target_formats"] = target_formats;
		required.push_back("target_formats");
		result_traits["model_state"] = "static_model";
	} else if (p_operation_id == "resize") {
		op["label"] = "Resize";
		op["description"] = "Set a real-world model dimension or let Meshy estimate it.";
		op["category"] = "Model";
		op["provider_operation_id"] = "meshy.openapi.v1.resize";
		op["endpoint"] = "/openapi/v1/resize";
		requirement["status"] = "ready";
		requirement["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requirement["task_id_fields"] = task_id_fields;
		Dictionary height = _option_schema("number", "Height (m)");
		height["exclusiveMinimum"] = 0.0;
		properties["resize_height"] = height;
		Dictionary longest_side = _option_schema("number", "Longest Side (m)");
		longest_side["exclusiveMinimum"] = 0.0;
		properties["resize_longest_side"] = longest_side;
		properties["auto_size"] = _option_schema("boolean", "Estimate Size Automatically");
		Dictionary origin = _option_schema("string", "Origin");
		Array origins;
		origins.push_back("bottom");
		origins.push_back("center");
		origin["enum"] = origins;
		origin["default"] = "bottom";
		properties["origin_at"] = origin;
		Array ui_order;
		ui_order.push_back("resize_height");
		ui_order.push_back("resize_longest_side");
		ui_order.push_back("auto_size");
		ui_order.push_back("origin_at");
		schema["ui_order"] = ui_order;
		Array modes;
		const char *mode_names[] = { "resize_height", "resize_longest_side", "auto_size" };
		for (const char *mode_name : mode_names) {
			Dictionary mode_schema;
			Array mode_required;
			mode_required.push_back(mode_name);
			mode_schema["required"] = mode_required;
			modes.push_back(mode_schema);
		}
		schema["oneOf"] = modes;
		result_traits["model_state"] = "static_model";
	} else if (p_operation_id == "uv_unwrap") {
		op["label"] = "UV Unwrap";
		op["description"] = "Create a clean UV layout for a mesh up to 40,000 faces.";
		op["category"] = "Geometry";
		op["provider_operation_id"] = "meshy.openapi.v1.uv-unwrap";
		op["endpoint"] = "/openapi/v1/uv-unwrap";
		requirement["status"] = "ready";
		requirement["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requirement["task_id_fields"] = task_id_fields;
		result_traits["model_state"] = "static_model";
		result_traits["uv_layout"] = "unwrapped";
		result_traits["material_state"] = "placeholder";
	} else {
		return Dictionary();
	}

	String docs_page = String(op.get("endpoint", String())).get_file();
	if (docs_page == "animations") {
		docs_page = "animation";
	}
	op["docs"] = String("https://docs.meshy.ai/en/api/") + docs_page;
	String stage = "unwrapping";
	if (p_operation_id == "refine") {
		stage = "refining";
	} else if (p_operation_id == "remesh") {
		stage = "optimizing";
	} else if (p_operation_id == "retexture") {
		stage = "restyling";
	} else if (p_operation_id == "rig_humanoid") {
		stage = "rigging";
	} else if (p_operation_id == "animate_humanoid") {
		stage = "animating";
	} else if (p_operation_id == "convert") {
		stage = "converting";
	} else if (p_operation_id == "resize") {
		stage = "resizing";
	}
	op["stage"] = stage;
	schema["type"] = "object";
	schema["properties"] = properties;
	schema["required"] = required;
	op["options_schema"] = schema;
	op["requires"] = requirement;
	op["result_traits"] = result_traits;
	return op;
}

static Array _meshy_animation_actions_from_document(const Dictionary &p_document) {
	if (p_document.has("actions")) {
		return Array(p_document.get("actions", Array())).duplicate(true);
	}
	const Array resources = Dictionary(p_document.get("result", Dictionary())).get("list", Array());
	Array actions;
	for (int i = 0; i < resources.size(); i++) {
		const Dictionary resource = resources[i];
		const Variant id = resource.get("id", Variant());
		if (!id.is_num()) {
			continue;
		}
		Dictionary action;
		action["action_id"] = (int64_t)id;
		action["name"] = resource.get("key", resource.get("name", String()));
		action["category"] = resource.get("category", String());
		action["subcategory"] = resource.get("subCategory", String());
		action["description"] = vformat("%s / %s / %s", action["category"], action["subcategory"], action["name"]);
		action["preview_url"] = resource.get("previewUrl", String());
		action["rig_type"] = resource.get("rigType", String());
		actions.push_back(action);
	}
	return actions;
}

Array SolersPluginMeshy::animation_actions() {
	static Mutex mutex;
	static bool initialized = false;
	static Array memory_actions;
	MutexLock lock(mutex);
	if (initialized) {
		return memory_actions.duplicate(true);
	}
	initialized = true;

	static const char *catalog_url = "https://api.meshy.ai/web/public/animations/resources";
	const Dictionary response = http_request("GET", catalog_url, Vector<String>(), PackedByteArray(), 30000, 4 * 1024 * 1024);
	if ((bool)response.get("ok", false)) {
		memory_actions = _meshy_animation_actions_from_document(parse_json_body(response));
		if (!memory_actions.is_empty()) {
			Dictionary cache;
			cache["provider"] = "meshy";
			cache["source"] = catalog_url;
			cache["fetched_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
			cache["actions"] = memory_actions;
			String cache_error;
			write_json_atomic("user://solers_plugin_cache/meshy_animation_actions.json", cache, cache_error);
			return memory_actions.duplicate(true);
		}
	}

	Array candidates;
	candidates.push_back("user://solers_plugin_cache/meshy_animation_actions.json");
	candidates.push_back("res://modules/solers_ai/data/meshy_animation_actions.json");
	const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	candidates.push_back(exe_dir.path_join("modules/solers_ai/data/meshy_animation_actions.json").simplify_path());
	candidates.push_back(exe_dir.path_join("../modules/solers_ai/data/meshy_animation_actions.json").simplify_path());
	candidates.push_back(exe_dir.path_join("../../modules/solers_ai/data/meshy_animation_actions.json").simplify_path());

	for (int i = 0; i < candidates.size(); i++) {
		const String path = String(candidates[i]);
		if (!FileAccess::exists(path)) {
			continue;
		}
		memory_actions = _meshy_animation_actions_from_document(read_json_file(path));
		if (!memory_actions.is_empty()) {
			return memory_actions.duplicate(true);
		}
	}
	return Array();
}

bool SolersPluginMeshy::animation_action_exists(int64_t p_action_id) {
	const Array actions = animation_actions();
	for (int i = 0; i < actions.size(); i++) {
		const Dictionary action = actions[i];
		if ((int64_t)action.get("action_id", -1) == p_action_id) {
			return true;
		}
	}
	return false;
}

Dictionary SolersPluginMeshy::get_profile() const {
	Dictionary profile;
	profile["id"] = "meshy";
	profile["label"] = "Meshy";
	Array kinds;
	kinds.push_back("3d");
	profile["kinds"] = kinds;
	profile["base_url"] = "https://api.meshy.ai";
	profile["api_key_env"] = "MESHY_API_KEY";
	profile["docs"] = "https://docs.meshy.ai";
	profile["description"] = "Generates 3D models from text or reference images, with refinement, geometry, conversion, sizing, UV, rigging, and animation operations.";
	profile["requires_api_key"] = true;
	profile["supports_generation"] = true;
	profile["supports_catalog"] = false;
	profile["supports_resume"] = true;
	profile["generation_presets"] = JSON::parse_string(R"([
		{"id":"meshy-7","label":"Meshy 7","description":"Highest detail and fidelity","default":true,"kind":"3d","options":{"model_type":"standard","ai_model":"latest","should_texture":true,"image_enhancement":true,"enable_pbr":true,"texture_resolution":"2k"},"min_reference_images":1,"max_reference_images":4,"featured_fields":["pose_mode","image_enhancement","enable_pbr","texture_resolution"],"hidden_fields":["model_type","ai_model","should_remesh","topology","target_polycount"],"presentation":{"order":["pose_mode","image_enhancement","enable_pbr","texture_resolution"],"controls":{"pose_mode":{"control":"segmented","labels":{"":"Custom","a-pose":"A Pose","t-pose":"T Pose"}},"texture_resolution":{"control":"segmented"}}}},
		{"id":"meshy-t2","label":"Meshy T2","description":"Game-ready topology from one image","kind":"3d","options":{"model_type":"smart-topology","ai_model":"meshy-t2","target_polycount":4000,"should_texture":true,"enable_pbr":true,"texture_resolution":"2k"},"min_reference_images":1,"max_reference_images":1,"featured_fields":["target_polycount","pose_mode","enable_pbr","texture_resolution"],"hidden_fields":["model_type","ai_model","ultra_mode","image_enhancement","remove_lighting","should_remesh","topology"],"presentation":{"order":["target_polycount","pose_mode","enable_pbr","texture_resolution"],"controls":{"target_polycount":{"control":"slider","minimum":100,"maximum":15000},"pose_mode":{"control":"segmented","labels":{"":"Custom","a-pose":"A Pose","t-pose":"T Pose"}},"texture_resolution":{"control":"segmented"}}}}
	])");
	return profile;
}

Dictionary SolersPluginMeshy::get_generation_options_schema(const String &p_kind) const {
	if (p_kind != "3d") {
		return Dictionary();
	}
	static const char *schema_json = R"({
		"model_type":{"type":"string","enum":["standard","smart-topology"],"default":"standard","label":"Mesh Pipeline"},"ai_model":{"type":"string","enum":["latest","meshy-7","meshy-t1","meshy-t2"],"default":"latest","label":"Model"},"pose_mode":{"type":"string","enum":["","a-pose","t-pose"],"default":"","label":"Pose"},"ultra_mode":{"type":"boolean","label":"Ultra Detail","description":"Use Meshy 7 ultra detail for Text-to-3D or single-image generation."},
		"should_texture":{"type":"boolean","default":true,"label":"Generate Textures"},"enable_pbr":{"type":"boolean","default":true,"label":"PBR Maps"},"texture_resolution":{"type":"string","enum":["2k","4k","8k"],"default":"2k","label":"Texture Resolution"},"texture_prompt":{"type":"string","label":"Texture Prompt"},"texture_image_url":{"type":"string","label":"Texture Image URL"},"texture_image_urls":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":4,"label":"Texture Image URLs"},
		"should_remesh":{"type":"boolean","label":"Remesh"},"topology":{"type":"string","enum":["triangle","quad"],"label":"Topology"},"target_polycount":{"type":"integer","minimum":100,"maximum":300000,"default":4000,"label":"Target Polygons"},"image_enhancement":{"type":"boolean","label":"Enhance Reference"},"remove_lighting":{"type":"boolean","label":"Remove Lighting"},"moderation":{"type":"boolean","label":"Moderation"},"auto_size":{"type":"boolean","label":"Estimate Size"},"origin_at":{"type":"string","enum":["bottom","center"],"label":"Origin"},
		"save_pre_remeshed_model":{"type":"boolean","label":"Keep Source Mesh"},"alpha_thumbnail":{"type":"boolean","label":"Transparent Preview"},"target_formats":{"type":"array","items":{"type":"string","enum":["glb","obj","fbx","stl","usdz","3mf"]},"default":["glb"],"label":"Output Formats"}
	})";
	const Variant parsed = JSON::parse_string(schema_json);
	return parsed.get_type() == Variant::DICTIONARY ? (Dictionary)parsed : Dictionary();
}

Array SolersPluginMeshy::get_operation_defs() const {
	Array ops;
	const char *operation_ids[] = { "refine", "remesh", "retexture", "rig_humanoid", "animate_humanoid", "convert", "resize", "uv_unwrap" };
	for (const char *operation_id : operation_ids) {
		ops.push_back(_operation_def(operation_id));
	}
	return ops;
}

static bool _validate_bool_option(const Dictionary &p_options, const String &p_name, String &r_error) {
	if (!p_options.has(p_name) || p_options.get(p_name, Variant()).get_type() == Variant::BOOL) {
		return true;
	}
	r_error = vformat("%s must be a boolean.", p_name);
	return false;
}

static bool _normalize_integer_option(Dictionary &r_options, const String &p_name, String &r_error) {
	if (!r_options.has(p_name)) {
		return true;
	}
	const Variant value = r_options[p_name];
	switch (value.get_type()) {
		case Variant::INT:
			return true;
		case Variant::FLOAT: {
			const double number = (double)value;
			if (!Math::is_finite(number) || number != Math::floor(number) || number < (double)INT64_MIN || number >= (double)INT64_MAX) {
				r_error = vformat("%s must be an integer.", p_name);
				return false;
			}
			r_options[p_name] = (int64_t)number;
			return true;
		}
		case Variant::STRING: {
			const String text = String(value).strip_edges();
			if (!text.is_valid_int()) {
				r_error = vformat("%s must be an integer.", p_name);
				return false;
			}
			r_options[p_name] = (int64_t)text.to_int();
			return true;
		}
		default:
			r_error = vformat("%s must be an integer.", p_name);
			return false;
	}
}

static bool _normalize_target_formats(Dictionary &r_options, String &r_error, bool p_allow_blend) {
	if (!r_options.has("target_formats")) {
		return true;
	}
	if (r_options["target_formats"].get_type() != Variant::ARRAY) {
		r_error = "target_formats must be an array.";
		return false;
	}
	const Array requested = r_options["target_formats"];
	if (requested.is_empty()) {
		r_error = "target_formats must contain at least one format.";
		return false;
	}
	Array normalized;
	for (int i = 0; i < requested.size(); i++) {
		const String format = String(requested[i]).strip_edges().to_lower();
		const bool supported = format == "glb" || format == "fbx" || format == "obj" || format == "usdz" || format == "stl" || format == "3mf" || (p_allow_blend && format == "blend");
		if (!supported) {
			r_error = vformat("Unsupported Meshy output format: %s.", format);
			return false;
		}
		if (normalized.has(format)) {
			r_error = vformat("target_formats contains a duplicate format: %s.", format);
			return false;
		}
		normalized.push_back(format);
	}
	r_options["target_formats"] = normalized;
	return true;
}

static bool _validate_positive_number_option(const Dictionary &p_options, const String &p_name, String &r_error) {
	if (!p_options.has(p_name)) {
		return true;
	}
	const Variant value = p_options[p_name];
	if ((value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) || !Math::is_finite((double)value) || (double)value <= 0.0) {
		r_error = vformat("%s must be a positive number of meters.", p_name);
		return false;
	}
	return true;
}

Dictionary SolersPluginMeshy::prepare_generate(const String &p_kind, const Dictionary &p_args, Dictionary &r_manifest) const {
	Dictionary provider_options = Dictionary(r_manifest.get("provider_options", Dictionary())).duplicate(true);
	const Array source_attachments = r_manifest.get("source_attachments", Array());
	if (String(p_args.get("prompt", String())).strip_edges().is_empty() && source_attachments.is_empty()) {
		return error_data("INVALID_ARGUMENT", "Meshy generation requires a prompt or reference image.");
	}
	if (source_attachments.size() > 4) {
		return error_data("INVALID_ARGUMENT", "Meshy accepts at most four reference images.");
	}
	for (const Variant &value : source_attachments) {
		const String mime_type = String(Dictionary(value).get("mime_type", String())).to_lower();
		if (mime_type != "image/png" && mime_type != "image/jpeg" && mime_type != "image/jpg") {
			return error_data("INVALID_ARGUMENT", "Meshy reference images must be PNG or JPEG.");
		}
	}
	const String model_type = String(provider_options.get("model_type", "standard")).to_lower();
	if (model_type != "standard" && model_type != "smart-topology") {
		return error_data("INVALID_ARGUMENT", "model_type must be standard or smart-topology.");
	}
	const String ai_model = String(provider_options.get("ai_model", String())).to_lower();
	if (model_type == "smart-topology") {
		if (source_attachments.size() != 1) {
			return error_data("INVALID_ARGUMENT", "Meshy Smart Topology requires exactly one reference image.");
		}
		if (!ai_model.is_empty() && ai_model != "meshy-t1" && ai_model != "meshy-t2") {
			return error_data("INVALID_ARGUMENT", "Smart Topology ai_model must be meshy-t1 or meshy-t2.");
		}
		provider_options["ai_model"] = ai_model.is_empty() ? String("meshy-t2") : ai_model;
	} else {
		if (!ai_model.is_empty() && ai_model != "meshy-7" && ai_model != "latest") {
			return error_data("INVALID_ARGUMENT", "Standard Meshy generation ai_model must be meshy-7 or latest.");
		}
		provider_options["ai_model"] = ai_model.is_empty() ? String("latest") : ai_model;
	}
	if (provider_options.has("pose_mode")) {
		const String pose_mode = String(provider_options.get("pose_mode", String())).strip_edges().to_lower();
		if (pose_mode.is_empty()) {
			provider_options.erase("pose_mode");
		} else if (pose_mode != "a-pose" && pose_mode != "t-pose") {
			return error_data("INVALID_ARGUMENT", "pose_mode must be a-pose or t-pose.");
		} else {
			provider_options["pose_mode"] = pose_mode;
		}
	}
	String option_error;
	const char *const bool_options[] = { "should_texture", "enable_pbr", "ultra_mode", "should_remesh", "image_enhancement", "remove_lighting", "moderation", "auto_size", "save_pre_remeshed_model", "alpha_thumbnail" };
	for (const char *option : bool_options) {
		if (!_validate_bool_option(provider_options, option, option_error)) {
			return error_data("INVALID_ARGUMENT", option_error);
		}
	}
	const String resolved_ai_model = String(provider_options.get("ai_model", String())).to_lower();
	const bool enhancement_pipeline_ok = model_type == "standard" &&
			(resolved_ai_model == "meshy-7" || resolved_ai_model == "latest");
	const char *const enhancement_options[] = { "image_enhancement", "remove_lighting" };
	for (const char *option : enhancement_options) {
		if ((bool)provider_options.get(option, false) && !enhancement_pipeline_ok) {
			return error_data("INVALID_ARGUMENT",
					vformat("%s requires model_type standard with ai_model meshy-7 or latest", option));
		}
	}
	if ((bool)provider_options.get("ultra_mode", false) && (!enhancement_pipeline_ok || source_attachments.size() > 1)) {
		return error_data("INVALID_ARGUMENT", "ultra_mode requires Meshy 7 text or single-image generation.");
	}
	if (provider_options.has("texture_resolution")) {
		const String resolution = String(provider_options["texture_resolution"]).to_lower();
		if (resolution != "2k" && resolution != "4k" && resolution != "8k") {
			return error_data("INVALID_ARGUMENT", "texture_resolution must be 2k, 4k, or 8k.");
		}
		provider_options["texture_resolution"] = resolution;
	}
	if (provider_options.has("texture_image_urls")) {
		if (provider_options["texture_image_urls"].get_type() != Variant::ARRAY) {
			return error_data("INVALID_ARGUMENT", "texture_image_urls must be an array.");
		}
		const int image_count = Array(provider_options["texture_image_urls"]).size();
		if (image_count < 1 || image_count > 4) {
			return error_data("INVALID_ARGUMENT", "texture_image_urls must contain one to four images.");
		}
	}
	if (model_type == "smart-topology" && provider_options.has("topology")) {
		const String topology = String(provider_options.get("topology", String())).strip_edges().to_lower();
		if (!topology.is_empty() && topology != "triangle") {
			return error_data("INVALID_ARGUMENT", "Smart Topology output is triangle-only; omit topology or use triangle.");
		}
		provider_options.erase("topology");
	}
	if (!_normalize_target_formats(provider_options, option_error, false)) {
		return error_data("INVALID_ARGUMENT", option_error);
	}
	if (!_normalize_integer_option(provider_options, "target_polycount", option_error)) {
		return error_data("INVALID_ARGUMENT", option_error);
	}
	const int64_t target_polycount = (int64_t)provider_options.get("target_polycount", 0);
	if (provider_options.has("target_polycount") && target_polycount <= 0) {
		return error_data("INVALID_ARGUMENT", "target_polycount must be positive.");
	}
	if (model_type == "smart-topology" && String(provider_options.get("ai_model", String())) == "meshy-t2" && provider_options.has("target_polycount") && (target_polycount < 100 || target_polycount > 15000)) {
		return error_data("INVALID_ARGUMENT", "Meshy Smart Topology meshy-t2 target_polycount must be between 100 and 15000.");
	}
	if (model_type == "smart-topology" && String(provider_options.get("ai_model", String())) == "meshy-t1" && provider_options.has("target_polycount")) {
		return error_data("INVALID_ARGUMENT", "Meshy Smart Topology meshy-t1 does not support target_polycount.");
	}
	if ((bool)provider_options.get("enable_pbr", false) && provider_options.has("should_texture") && !(bool)provider_options.get("should_texture", true)) {
		return error_data("INVALID_ARGUMENT", "enable_pbr requires should_texture=true.");
	}
	int texture_guides = provider_options.has("texture_prompt") ? 1 : 0;
	texture_guides += provider_options.has("texture_image_url") ? 1 : 0;
	texture_guides += provider_options.has("texture_image_urls") ? 1 : 0;
	if (texture_guides > 1) {
		return error_data("INVALID_ARGUMENT", "Use one texture prompt, one texture image, or one multiview texture image set.");
	}
	if (provider_options.has("target_polycount") && model_type != "smart-topology") {
		if (provider_options.has("should_remesh") && !(bool)provider_options.get("should_remesh", false)) {
			return error_data("INVALID_ARGUMENT", "target_polycount requires should_remesh=true; otherwise Meshy returns its highest-detail mesh.");
		}
		provider_options["should_remesh"] = true;
	}
	r_manifest["provider_options"] = provider_options;
	if (provider_options.has("target_polycount") && (model_type == "smart-topology" || (bool)provider_options.get("should_remesh", false))) {
		Dictionary constraints;
		constraints["max_triangles"] = String(provider_options.get("topology", "triangle")) == "quad" ? target_polycount * 2 : target_polycount;
		r_manifest["import_constraints"] = constraints;
	}

	const String generation_mode = source_attachments.is_empty() ? "text_to_3d" : (source_attachments.size() == 1 ? "image_to_3d" : "multi_image_to_3d");
	r_manifest["generation_mode"] = generation_mode;
	r_manifest["provider_endpoint"] = generation_mode == "text_to_3d" ? "/openapi/v2/text-to-3d" : (generation_mode == "image_to_3d" ? "/openapi/v1/image-to-3d" : "/openapi/v1/multi-image-to-3d");
	Dictionary traits;
	traits["model_state"] = "static_model";
	r_manifest["traits"] = traits;
	return Dictionary();
}

Dictionary SolersPluginMeshy::prepare_operation(const Dictionary &p_operation, const Dictionary &p_source_manifest, Dictionary &r_provider_options) const {
	const String operation_id = String(p_operation.get("operation_id", String()));
	const String source_task_id = first_manifest_field(p_source_manifest, Dictionary(p_operation.get("requires", Dictionary())).get("task_id_fields", Array()));
	if (operation_id == "rig_humanoid" && !(bool)r_provider_options.get("humanoid_confirmed", false)) {
		return error_data("HUMANOID_CONFIRMATION_REQUIRED", "Rigging requires confirming this is a humanoid character.");
	}
	if (operation_id == "animate_humanoid") {
		const int64_t action_id = (int64_t)r_provider_options.get("action_id", -1);
		if (!animation_action_exists(action_id)) {
			return error_data("INVALID_ACTION_ID", "action_id must come from Meshy's official animation_actions catalog returned by asset.capabilities.");
		}
		if (r_provider_options.has("post_process")) {
			if (r_provider_options["post_process"].get_type() != Variant::DICTIONARY) {
				return error_data("INVALID_ARGUMENT", "Animation post_process must be an object.");
			}
			const Dictionary post_process = r_provider_options["post_process"];
			const int fps = post_process.get("fps", 0);
			if (String(post_process.get("operation_type", String())) != "change_fps" || (fps != 24 && fps != 25 && fps != 30 && fps != 60)) {
				return error_data("INVALID_ARGUMENT", "Animation post_process must be change_fps with 24, 25, 30, or 60 fps.");
			}
		}
	}
	r_provider_options.erase("humanoid_confirmed");
	if (operation_id == "refine") {
		r_provider_options["mode"] = "refine";
		r_provider_options.erase("input_task_id");
		r_provider_options.erase("model_url");
		r_provider_options["preview_task_id"] = source_task_id;
		if (!r_provider_options.has("enable_pbr")) {
			r_provider_options["enable_pbr"] = true;
		}
	} else if (operation_id == "animate_humanoid") {
		r_provider_options.erase("input_task_id");
		r_provider_options.erase("model_url");
		r_provider_options["rig_task_id"] = source_task_id;
	} else {
		r_provider_options.erase("model_url");
		r_provider_options["input_task_id"] = source_task_id;
	}

	String option_error;
	if (operation_id == "remesh") {
		if (!r_provider_options.has("topology")) {
			r_provider_options["topology"] = "triangle";
		}
		if (!r_provider_options.has("target_polycount")) {
			r_provider_options["target_polycount"] = 30000;
		}
		const String topology = String(r_provider_options.get("topology", String())).strip_edges().to_lower();
		if (topology != "triangle" && topology != "quad") {
			return error_data("INVALID_ARGUMENT", "topology must be triangle or quad.");
		}
		r_provider_options["topology"] = topology;
		if (!_normalize_integer_option(r_provider_options, "target_polycount", option_error) || (int64_t)r_provider_options.get("target_polycount", 0) <= 0) {
			return error_data("INVALID_ARGUMENT", option_error.is_empty() ? String("target_polycount must be positive.") : option_error);
		}
	} else if (operation_id == "retexture") {
		int styles = r_provider_options.has("text_style_prompt") ? 1 : 0;
		styles += r_provider_options.has("image_style_url") ? 1 : 0;
		styles += r_provider_options.has("multiview_image_urls") ? 1 : 0;
		if (styles != 1) {
			return error_data("INVALID_ARGUMENT", "Retexture requires exactly one text, image, or multiview style input.");
		}
		if (r_provider_options.has("multiview_image_urls") && (r_provider_options["multiview_image_urls"].get_type() != Variant::ARRAY || Array(r_provider_options["multiview_image_urls"]).is_empty() || Array(r_provider_options["multiview_image_urls"]).size() > 4)) {
			return error_data("INVALID_ARGUMENT", "multiview_image_urls must contain one to four images.");
		}
		if (!r_provider_options.has("enable_original_uv")) {
			r_provider_options["enable_original_uv"] = true;
		}
		if (!r_provider_options.has("enable_pbr")) {
			r_provider_options["enable_pbr"] = true;
		}
		if (!r_provider_options.has("remove_lighting")) {
			r_provider_options["remove_lighting"] = true;
		}
	}
	if ((operation_id == "refine" || operation_id == "remesh" || operation_id == "retexture") && !r_provider_options.has("target_formats")) {
		Array formats;
		formats.push_back("glb");
		r_provider_options["target_formats"] = formats;
	}
	if (operation_id == "refine" || operation_id == "remesh" || operation_id == "retexture") {
		if (!_normalize_target_formats(r_provider_options, option_error, false)) {
			return error_data("INVALID_ARGUMENT", option_error);
		}
	}
	if (operation_id == "convert") {
		if (!r_provider_options.has("target_formats")) {
			return error_data("INVALID_ARGUMENT", "Convert requires target_formats.");
		}
		if (!_normalize_target_formats(r_provider_options, option_error, true)) {
			return error_data("INVALID_ARGUMENT", option_error);
		}
	}
	if (operation_id == "resize") {
		if (!_validate_positive_number_option(r_provider_options, "resize_height", option_error) || !_validate_positive_number_option(r_provider_options, "resize_longest_side", option_error) || !_validate_bool_option(r_provider_options, "auto_size", option_error)) {
			return error_data("INVALID_ARGUMENT", option_error);
		}
		int resize_modes = 0;
		resize_modes += r_provider_options.has("resize_height") ? 1 : 0;
		resize_modes += r_provider_options.has("resize_longest_side") ? 1 : 0;
		if (r_provider_options.has("auto_size")) {
			if (!(bool)r_provider_options["auto_size"]) {
				return error_data("INVALID_ARGUMENT", "auto_size must be true when supplied.");
			}
			resize_modes++;
		}
		if (resize_modes != 1) {
			return error_data("INVALID_ARGUMENT", "Specify exactly one of resize_height, resize_longest_side, or auto_size=true.");
		}
		if (r_provider_options.has("origin_at")) {
			const String origin = String(r_provider_options["origin_at"]).strip_edges().to_lower();
			if (origin != "bottom" && origin != "center") {
				return error_data("INVALID_ARGUMENT", "origin_at must be bottom or center.");
			}
			r_provider_options["origin_at"] = origin;
		}
	}
	if (operation_id == "uv_unwrap" && (int64_t)p_source_manifest.get("polycount", 0) > 40000) {
		return error_data("UV_UNWRAP_FACE_LIMIT", "Meshy UV Unwrap supports at most 40,000 faces; run Optimize first.");
	}
	if (operation_id == "remesh") {
		Dictionary constraints;
		const int64_t target_polycount = r_provider_options.get("target_polycount", 0);
		constraints["max_triangles"] = String(r_provider_options.get("topology", "triangle")) == "quad" ? target_polycount * 2 : target_polycount;
		r_provider_options["_solers_import_constraints"] = constraints;
	}
	return Dictionary();
}

Dictionary SolersPluginMeshy::capability_extras(const Dictionary &p_manifest) const {
	Dictionary extras;
	const Dictionary traits = p_manifest.get("traits", Dictionary());
	if (String(traits.get("rig", String())) == "humanoid") {
		extras["animation_actions"] = animation_actions();
	}
	return extras;
}

// --- Job execution ---------------------------------------------------------

static String _url_path(const String &p_url) {
	String scheme;
	String host;
	int port = 0;
	String path;
	String fragment;
	if (p_url.parse_url(scheme, host, port, path, fragment) != OK) {
		return String();
	}
	return path;
}

static String _attachment_data_uri(const Dictionary &p_attachment, String &r_error) {
	const String path = String(p_attachment.get("local_path", String())).strip_edges();
	const String mime = String(p_attachment.get("mime_type", String())).strip_edges();
	if (path.is_empty() || mime.is_empty()) {
		r_error = "Image attachment is missing local_path or mime_type.";
		return String();
	}
	Error read_error = OK;
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(path, &read_error);
	if (read_error != OK || bytes.is_empty()) {
		r_error = vformat("Failed to read image attachment: %s", path);
		return String();
	}
	return vformat("data:%s;base64,%s", mime, CryptoCore::b64_encode_str(bytes.ptr(), bytes.size()));
}

static Dictionary _select_options(const Dictionary &p_options, const char *const *p_names, int p_name_count) {
	Dictionary selected;
	for (int i = 0; i < p_name_count; i++) {
		const StringName name = p_names[i];
		if (p_options.has(name)) {
			selected[name] = p_options[name];
		}
	}
	return selected;
}

static Dictionary _text_preview_options(const Dictionary &p_options) {
	const char *const names[] = {
		"model_type", "ai_model", "should_remesh", "topology", "target_polycount",
		"decimation_mode", "pose_mode", "moderation", "target_formats", "alpha_thumbnail",
		"auto_size", "origin_at", "ultra_mode"
	};
	return _select_options(p_options, names, sizeof(names) / sizeof(names[0]));
}

static Dictionary _text_refine_options(const Dictionary &p_options) {
	const char *const names[] = {
		"enable_pbr", "texture_resolution", "texture_prompt", "texture_image_url", "texture_image_urls", "ai_model",
		"moderation", "remove_lighting", "target_formats", "alpha_thumbnail", "auto_size", "origin_at"
	};
	return _select_options(p_options, names, sizeof(names) / sizeof(names[0]));
}

static String _meshy_task_id(const Dictionary &p_response) {
	return p_response.get("result", p_response.get("id", p_response.get("task_id", String())));
}

Dictionary SolersPluginMeshy::_poll_task(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers) {
	Dictionary detail;
	for (int i = 0; i < 90 && !p_job->is_cancelled(); i++) {
		Dictionary poll = http_request("GET", p_url, p_headers, PackedByteArray(), 60000);
		if (!(bool)poll.get("ok", false)) {
			r_state["status"] = "failed";
			r_state["error"] = poll.get("error", Dictionary());
			p_job->set_state(r_state);
			return Dictionary();
		}
		detail = parse_json_body(poll);
		const String status = String(detail.get("status", String())).to_upper();
		r_state["provider_status"] = status;
		r_state["progress"] = detail.get("progress", r_state.get("progress", 0));
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
		p_job->set_state(r_state);
		if (status == "SUCCEEDED") {
			return detail;
		}
		if (status == "FAILED" || status == "CANCELED" || status == "CANCELLED") {
			r_state["status"] = "failed";
			r_state["error"] = error_data("PROVIDER_FAILED", "Meshy task failed.");
			r_state["provider_response"] = detail;
			p_job->set_state(r_state);
			return Dictionary();
		}
		OS::get_singleton()->delay_usec(10000000);
	}
	if (p_job->is_cancelled()) {
		r_state["status"] = "interrupted";
		r_state["error"] = error_data("LOCAL_OBSERVER_STOPPED", "Solers stopped observing this provider task; its provider task id was preserved for recovery.");
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	} else {
		r_state["status"] = "failed";
		r_state["error"] = error_data("PROVIDER_TIMEOUT", "Meshy task did not finish before the local wait limit.");
		r_state["provider_response"] = detail;
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	}
	p_job->set_state(r_state);
	return Dictionary();
}

Dictionary SolersPluginMeshy::_submit_and_poll(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers, const Dictionary &p_body, const String &p_stage) {
	Dictionary provider_request;
	provider_request["endpoint"] = _url_path(p_url);
	const Array request_attachments = r_state.get("source_attachments", Array());
	provider_request["attachment_count"] = (int64_t)request_attachments.size();
	provider_request["mode"] = r_state.get("generation_mode", String());
	provider_request["input_attachment_ids"] = r_state.get("input_attachment_ids", Array());
	r_state["provider_request"] = provider_request;
	r_state["stage"] = p_stage;
	r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	p_job->set_state(r_state);

	Dictionary submit = http_request("POST", p_url, p_headers, utf8_bytes(JSON::stringify(p_body)), 120000);
	if (!(bool)submit.get("ok", false)) {
		r_state["status"] = "failed";
		r_state["error"] = submit.get("error", Dictionary());
		p_job->set_state(r_state);
		return Dictionary();
	}
	const Dictionary submitted = parse_json_body(submit);
	String provider_task_id = _meshy_task_id(submitted);
	if (provider_task_id.is_empty()) {
		r_state["status"] = "failed";
		r_state["error"] = error_data("BAD_PROVIDER_RESPONSE", "Meshy response did not contain a task id.");
		r_state["provider_response"] = submitted;
		p_job->set_state(r_state);
		return Dictionary();
	}
	r_state["provider_task_id"] = provider_task_id;
	if (p_stage == "previewing") {
		r_state["preview_task_id"] = provider_task_id;
	} else if (p_stage == "refining") {
		r_state["refine_task_id"] = provider_task_id;
	} else if (p_stage == "rigging") {
		r_state["rig_task_id"] = provider_task_id;
	} else if (p_stage == "animating") {
		r_state["animation_task_id"] = provider_task_id;
	}
	p_job->set_state(r_state);
	return _poll_task(p_job, r_state, p_url + "/" + provider_task_id, p_headers);
}

bool SolersPluginMeshy::_download_model(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const Dictionary &p_detail) {
	const Dictionary model_urls = p_detail.get("model_urls", Dictionary());
	Array downloads;
	const char *formats[] = { "glb", "fbx", "obj", "usdz", "blend", "stl", "3mf" };
	for (const char *format_name : formats) {
		const String url = model_urls.get(format_name, String());
		if (url.is_empty()) {
			continue;
		}
		Dictionary item;
		item["extension"] = format_name;
		item["url"] = url;
		downloads.push_back(item);
	}
	if (downloads.is_empty()) {
		const String url = p_detail.get("model_url", String());
		String extension = url.get_extension().get_slice("?", 0).to_lower();
		if (!url.is_empty()) {
			if (extension.is_empty() || extension.length() > 8) {
				extension = "glb";
			}
			Dictionary item;
			item["extension"] = extension;
			item["url"] = url;
			downloads.push_back(item);
		}
	}
	if (downloads.is_empty()) {
		const Dictionary result = p_detail.get("result", Dictionary());
		String url = result.get("rigged_character_glb_url", result.get("animation_glb_url", String()));
		String extension = "glb";
		if (url.is_empty()) {
			url = result.get("rigged_character_fbx_url", result.get("animation_fbx_url", String()));
			extension = "fbx";
		}
		if (!url.is_empty()) {
			Dictionary item;
			item["extension"] = extension;
			item["url"] = url;
			downloads.push_back(item);
		}
	}
	if (downloads.is_empty()) {
		r_state["status"] = "failed";
		r_state["error"] = error_data("NO_DOWNLOAD_URL", "Meshy response did not include a downloadable model URL.");
		r_state["provider_response"] = p_detail;
		p_job->set_state(r_state);
		return false;
	}

	r_state["provider_response"] = p_detail;
	p_job->set_preview_url(p_detail.get("thumbnail_url", String()));
	Array files;
	for (int i = 0; i < downloads.size(); i++) {
		const Dictionary item = downloads[i];
		const String extension = String(item.get("extension", String())).to_lower();
		const String url = item.get("url", String());
		const Dictionary download = http_request("GET", url, Vector<String>(), PackedByteArray(), 180000);
		if (!(bool)download.get("ok", false)) {
			r_state["status"] = "failed";
			r_state["error"] = download.get("error", Dictionary());
			p_job->set_state(r_state);
			return false;
		}
		const String file_path = p_job->get_staging_dir().path_join(p_job->get_asset_id() + "." + extension);
		String write_error;
		if (!write_bytes_atomic(file_path, download.get("body", PackedByteArray()), write_error)) {
			r_state["status"] = "failed";
			Dictionary error = error_data("WRITE_FAILED", write_error);
			error["path"] = file_path;
			r_state["error"] = error;
			p_job->set_state(r_state);
			return false;
		}
		p_job->add_file(file_path);
		files.push_back(file_path);
	}
	r_state["import_files"] = files;
	r_state["entrypoints"] = files;
	return true;
}

void SolersPluginMeshy::_download_basic_animations(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const Dictionary &p_detail) {
	const Dictionary result = p_detail.get("result", Dictionary());
	const Dictionary basic = result.get("basic_animations", Dictionary());
	if (basic.is_empty()) {
		return;
	}

	Dictionary animation_root = r_state.get("animations", Dictionary());
	Dictionary basic_root = animation_root.get("basic", Dictionary());
	const char *names[] = { "walking", "running" };
	for (const char *name : names) {
		const String key = String(name);
		const String url = basic.get(key + "_glb_url", String());
		if (url.is_empty()) {
			continue;
		}
		Dictionary download = http_request("GET", url, Vector<String>(), PackedByteArray(), 180000);
		if (!(bool)download.get("ok", false)) {
			r_state["animation_download_error"] = download.get("error", Dictionary());
			continue;
		}
		const String file_path = p_job->get_staging_dir().path_join(p_job->get_asset_id() + "_" + key + ".glb");
		String write_error;
		if (!write_bytes_atomic(file_path, download.get("body", PackedByteArray()), write_error)) {
			r_state["animation_download_error"] = error_data("WRITE_FAILED", write_error);
			continue;
		}
		Dictionary item;
		item["name"] = key.capitalize();
		item["source"] = "meshy.rigging.basic_animations";
		item["glb_file"] = file_path;
		item["glb_url"] = url;
		item["fbx_url"] = basic.get(key + "_fbx_url", String());
		item["armature_glb_url"] = basic.get(key + "_armature_glb_url", String());
		basic_root[key] = item;
	}
	if (!basic_root.is_empty()) {
		animation_root["basic"] = basic_root;
		r_state["animations"] = animation_root;
		r_state.erase("animation_download_error");
		p_job->set_state(r_state);
	}
}

void SolersPluginMeshy::run_job(const Ref<SolersPluginJob> &p_job) {
	Dictionary state = p_job->get_state();
	const String prompt = state.get("prompt", String());
	const String profile = state.get("profile", String("game_default"));
	const Dictionary provider_options = state.get("provider_options", Dictionary());
	const Array source_attachments = state.get("source_attachments", Array());
	Vector<String> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + p_job->get_api_key());
	const String base_url = clean_base_url(p_job->get_base_url());

	if (p_job->is_resume()) {
		const String provider_task_id = state.get("provider_task_id", String());
		const String endpoint = Dictionary(state.get("provider_request", Dictionary())).get("endpoint", String());
		if (provider_task_id.is_empty() || endpoint.is_empty()) {
			state["status"] = "interrupted";
			state["error"] = error_data("RESUME_METADATA_MISSING", "The interrupted Meshy task has no provider task id or endpoint, so Solers will not submit it again automatically.");
			p_job->set_state(state);
			return;
		}
		const Dictionary detail = _poll_task(p_job, state, base_url + endpoint + "/" + provider_task_id, headers);
		if (detail.is_empty() || !_download_model(p_job, state, detail)) {
			return;
		}
		if (String(state.get("stage", String())) == "rigging") {
			_download_basic_animations(p_job, state, detail);
		}
		if (String(state.get("stage", String())) == "previewing") {
			state["status"] = "draft";
			state["stage"] = "draft";
		}
		p_job->set_state(state);
		return;
	}

	const String operation = String(state.get("operation", "generate"));
	if (operation != "generate") {
		const Dictionary definition = _operation_def(operation);
		const String endpoint = definition.get("endpoint", String());
		const String stage = definition.get("stage", String());
		if (definition.is_empty() || endpoint.is_empty() || stage.is_empty()) {
			p_job->fail("OPERATION_NOT_FOUND", "Meshy operation metadata is missing.");
			return;
		}
		Dictionary body;
		merge_options(body, provider_options);
		const Dictionary detail = _submit_and_poll(p_job, state, base_url + endpoint, headers, body, stage);
		if (detail.is_empty() || !_download_model(p_job, state, detail)) {
			return;
		}
		if (operation == "rig_humanoid") {
			_download_basic_animations(p_job, state, detail);
		}
	} else if (!source_attachments.is_empty()) {
		Dictionary body;
		merge_options(body, provider_options);
		if (!body.has("prompt") && !prompt.is_empty()) {
			body["prompt"] = prompt;
		}
		if (profile == "game_default" && !body.has("model_type")) {
			body["model_type"] = "standard";
		}
		if (!body.has("ai_model")) {
			body["ai_model"] = "latest";
		}
		if (!body.has("target_formats")) {
			Array formats;
			formats.push_back("glb");
			body["target_formats"] = formats;
		}
		String data_uri_error;
		if (source_attachments.size() == 1) {
			const String data_uri = _attachment_data_uri(source_attachments[0], data_uri_error);
			if (data_uri.is_empty()) {
				p_job->fail("ATTACHMENT_READ_FAILED", data_uri_error);
				return;
			}
			body["image_url"] = data_uri;
		} else {
			Array image_urls;
			for (int i = 0; i < source_attachments.size(); i++) {
				const String data_uri = _attachment_data_uri(source_attachments[i], data_uri_error);
				if (data_uri.is_empty()) {
					p_job->fail("ATTACHMENT_READ_FAILED", data_uri_error);
					return;
				}
				image_urls.push_back(data_uri);
			}
			body["image_urls"] = image_urls;
		}
		const String endpoint = source_attachments.size() == 1 ? "/openapi/v1/image-to-3d" : "/openapi/v1/multi-image-to-3d";
		const Dictionary detail = _submit_and_poll(p_job, state, base_url + endpoint, headers, body, "generating");
		if (detail.is_empty() || !_download_model(p_job, state, detail)) {
			return;
		}
	} else {
		Dictionary preview_body = _text_preview_options(provider_options);
		const bool draft_only = String(provider_options.get("mode", String())).to_lower() == "preview";
		preview_body["mode"] = "preview";
		if (!preview_body.has("prompt")) {
			preview_body["prompt"] = prompt;
		}
		if (profile == "game_default" && !preview_body.has("model_type")) {
			preview_body["model_type"] = "standard";
		}
		if (!preview_body.has("ai_model")) {
			preview_body["ai_model"] = "latest";
		}
		if (!preview_body.has("target_formats")) {
			Array formats;
			formats.push_back("glb");
			preview_body["target_formats"] = formats;
		}
		const String text_to_3d_url = base_url + "/openapi/v2/text-to-3d";
		Dictionary preview_detail = _submit_and_poll(p_job, state, text_to_3d_url, headers, preview_body, "previewing");
		if (preview_detail.is_empty()) {
			return;
		}
		if (draft_only) {
			state["status"] = "draft";
			state["stage"] = "draft";
			if (!_download_model(p_job, state, preview_detail)) {
				return;
			}
		} else {
			Dictionary refine_body = _text_refine_options(provider_options);
			refine_body["mode"] = "refine";
			refine_body["preview_task_id"] = state.get("preview_task_id", state.get("provider_task_id", String()));
			if (!refine_body.has("enable_pbr")) {
				refine_body["enable_pbr"] = true;
			}
			if (!refine_body.has("target_formats")) {
				Array formats;
				formats.push_back("glb");
				refine_body["target_formats"] = formats;
			}
			Dictionary refine_detail = _submit_and_poll(p_job, state, text_to_3d_url, headers, refine_body, "refining");
			if (refine_detail.is_empty() || !_download_model(p_job, state, refine_detail)) {
				return;
			}
		}
	}
	p_job->set_state(state);
}
