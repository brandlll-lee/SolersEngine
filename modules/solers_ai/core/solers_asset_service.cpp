/**************************************************************************/
/*  solers_asset_service.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_asset_service.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "modules/solers_ai/core/solers_secret_store.h"

void SolersAssetService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("generate", "args"), &SolersAssetService::generate);
	ClassDB::bind_method(D_METHOD("capabilities", "args"), &SolersAssetService::capabilities);
	ClassDB::bind_method(D_METHOD("run_operation", "args"), &SolersAssetService::run_operation);
	ClassDB::bind_method(D_METHOD("refine_to_ready", "args"), &SolersAssetService::refine_to_ready);
	ClassDB::bind_method(D_METHOD("optimize_geometry", "args"), &SolersAssetService::optimize_geometry);
	ClassDB::bind_method(D_METHOD("restyle_material", "args"), &SolersAssetService::restyle_material);
	ClassDB::bind_method(D_METHOD("status", "args"), &SolersAssetService::status);
	ClassDB::bind_method(D_METHOD("list_local", "args"), &SolersAssetService::list_local);
	ClassDB::bind_method(D_METHOD("import_to_project", "args"), &SolersAssetService::import_to_project);
}

Dictionary SolersAssetService::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersAssetService::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

Dictionary SolersAssetService::_error_data(const String &p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	return error;
}

String SolersAssetService::_asset_root() {
	return "user://solers_library/assets";
}

String SolersAssetService::_asset_dir(const String &p_asset_id) {
	return _asset_root().path_join(p_asset_id);
}

String SolersAssetService::_manifest_path(const String &p_asset_id) {
	return _asset_dir(p_asset_id).path_join("manifest.json");
}

String SolersAssetService::_source_dir(const String &p_asset_id) {
	return _asset_dir(p_asset_id).path_join("source");
}

String SolersAssetService::_safe_slug(const String &p_text) {
	const String text = p_text.strip_edges().to_lower();
	String out;
	bool sep = false;
	for (int i = 0; i < text.length(); i++) {
		const char32_t c = text[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (ok) {
			out += String::chr(c);
			sep = false;
		} else if (!sep) {
			out += "-";
			sep = true;
		}
	}
	while (out.begins_with("-")) {
		out = out.substr(1);
	}
	while (out.ends_with("-")) {
		out = out.substr(0, out.length() - 1);
	}
	return out.is_empty() ? "asset" : out;
}

String SolersAssetService::_default_provider(const String &p_kind) {
	return p_kind == "3d" ? "meshy" : "elevenlabs";
}

String SolersAssetService::_default_base_url(const String &p_provider) {
	if (p_provider == "meshy") {
		return "https://api.meshy.ai";
	}
	if (p_provider == "elevenlabs") {
		return "https://api.elevenlabs.io";
	}
	return String();
}

String SolersAssetService::_default_env(const String &p_provider) {
	if (p_provider == "meshy") {
		return "MESHY_API_KEY";
	}
	if (p_provider == "elevenlabs") {
		return "ELEVENLABS_API_KEY";
	}
	return String();
}

String SolersAssetService::_setting_path(const String &p_kind, const String &p_key) {
	return "solers/ai_assets/" + p_kind + "/" + p_key;
}

static Dictionary _solers_option_schema(const String &p_type, const String &p_label) {
	Dictionary option;
	option["type"] = p_type;
	option["label"] = p_label;
	return option;
}

static Dictionary _solers_operation_def(const String &p_operation_id) {
	Dictionary op;
	op["operation_id"] = p_operation_id;
	op["agent_supported"] = true;
	const String docs_page = p_operation_id == "rig_humanoid" ? String("rigging") : (p_operation_id == "animate_humanoid" ? String("animation") : (p_operation_id == "remesh" ? String("remesh") : (p_operation_id == "retexture" ? String("retexture") : String("text-to-3d"))));
	op["docs"] = String("https://docs.meshy.ai/en/api/") + docs_page;

	Dictionary requires;
	requires["kind"] = "3d";
	Dictionary schema;
	Dictionary properties;
	Array required;

	if (p_operation_id == "refine") {
		op["label"] = "Refine to Ready";
		op["category"] = "Model";
		op["provider_operation_id"] = "meshy.openapi.v2.text-to-3d.refine";
		op["endpoint"] = "/openapi/v2/text-to-3d";
		requires["status"] = "draft";
		Array task_id_fields;
		task_id_fields.push_back("preview_task_id");
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
	} else if (p_operation_id == "remesh") {
		op["label"] = "Optimize";
		op["category"] = "Geometry";
		op["provider_operation_id"] = "meshy.openapi.v1.remesh";
		op["endpoint"] = "/openapi/v1/remesh";
		requires["status"] = "ready";
		requires["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
		Array quality;
		quality.push_back("balanced");
		quality.push_back("game_ready");
		quality.push_back("high_detail");
		Dictionary quality_option = _solers_option_schema("string", "Quality");
		quality_option["enum"] = quality;
		properties["quality"] = quality_option;
	} else if (p_operation_id == "retexture") {
		op["label"] = "Restyle";
		op["category"] = "Material";
		op["provider_operation_id"] = "meshy.openapi.v1.retexture";
		op["endpoint"] = "/openapi/v1/retexture";
		requires["status"] = "ready";
		requires["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
		properties["text_style_prompt"] = _solers_option_schema("string", "Style Prompt");
		required.push_back("text_style_prompt");
	} else if (p_operation_id == "rig_humanoid") {
		op["label"] = "Rig";
		op["category"] = "Character";
		op["provider_operation_id"] = "meshy.openapi.v1.rigging";
		op["endpoint"] = "/openapi/v1/rigging";
		requires["status"] = "ready";
		requires["model_state"] = "static_model";
		Array task_id_fields;
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
		properties["humanoid_confirmed"] = _solers_option_schema("boolean", "This is a humanoid character");
		required.push_back("humanoid_confirmed");
	} else if (p_operation_id == "animate_humanoid") {
		op["label"] = "Animate";
		op["category"] = "Character";
		op["provider_operation_id"] = "meshy.openapi.v1.animations";
		op["endpoint"] = "/openapi/v1/animations";
		requires["status"] = "ready";
		requires["rig"] = "humanoid";
		Array task_id_fields;
		task_id_fields.push_back("rig_task_id");
		task_id_fields.push_back("provider_task_id");
		requires["task_id_fields"] = task_id_fields;
		properties["action_id"] = _solers_option_schema("integer", "Action ID");
		required.push_back("action_id");
	} else {
		return Dictionary();
	}

	schema["type"] = "object";
	schema["properties"] = properties;
	schema["required"] = required;
	op["options_schema"] = schema;
	op["requires"] = requires;
	return op;
}

static Array _solers_operation_defs(const String &p_provider) {
	Array ops;
	if (p_provider != "meshy") {
		return ops;
	}
	const char *operation_ids[] = { "refine", "remesh", "retexture", "rig_humanoid", "animate_humanoid" };
	for (const char *operation_id : operation_ids) {
		ops.push_back(_solers_operation_def(operation_id));
	}
	return ops;
}

static String _solers_asset_status(const Dictionary &p_manifest) {
	return String(p_manifest.get("status", "unknown")).to_lower();
}

static Dictionary _solers_asset_traits(const Dictionary &p_manifest) {
	Dictionary traits = p_manifest.get("traits", Dictionary()).duplicate(true);
	if (!traits.has("model_state") && String(p_manifest.get("kind", String())).to_lower() == "3d") {
		const String operation = String(p_manifest.get("operation", String()));
		if (operation == "rig_humanoid") {
			traits["model_state"] = "rigged_model";
			traits["rig"] = "humanoid";
		} else if (operation == "animate_humanoid") {
			traits["model_state"] = "animated_model";
			traits["rig"] = "humanoid";
			traits["animation"] = "present";
		} else {
			traits["model_state"] = "static_model";
		}
	}
	return traits;
}

static String _solers_first_manifest_field(const Dictionary &p_manifest, const Array &p_fields) {
	for (int i = 0; i < p_fields.size(); i++) {
		const String field = String(p_fields[i]);
		const String value = String(p_manifest.get(field, String())).strip_edges();
		if (!value.is_empty()) {
			return value;
		}
	}
	return String();
}

static bool _solers_schema_ui_supported(const Dictionary &p_schema) {
	const Dictionary properties = p_schema.get("properties", Dictionary());
	const Array required = p_schema.get("required", Array());
	for (int i = 0; i < required.size(); i++) {
		const String name = String(required[i]);
		const Dictionary property = properties.get(name, Dictionary());
		const String type = String(property.get("type", String()));
		if (type != "string" && type != "boolean") {
			return false;
		}
	}
	return true;
}

static bool _solers_manifest_matches_operation(const Dictionary &p_manifest, const Dictionary &p_operation, String &r_reason) {
	const Dictionary requires = p_operation.get("requires", Dictionary());
	const String required_kind = String(requires.get("kind", String()));
	if (!required_kind.is_empty() && String(p_manifest.get("kind", String())).to_lower() != required_kind) {
		r_reason = "Asset kind does not match.";
		return false;
	}
	const String required_status = String(requires.get("status", String()));
	if (!required_status.is_empty() && _solers_asset_status(p_manifest) != required_status) {
		r_reason = "Asset status does not match.";
		return false;
	}
	const Dictionary traits = _solers_asset_traits(p_manifest);
	const char *trait_names[] = { "model_state", "rig" };
	for (const char *trait_name : trait_names) {
		const String required_trait = String(requires.get(trait_name, String()));
		if (!required_trait.is_empty() && String(traits.get(trait_name, String())) != required_trait) {
			r_reason = "Asset traits do not match.";
			return false;
		}
	}
	const Array task_id_fields = requires.get("task_id_fields", Array());
	if (!task_id_fields.is_empty() && _solers_first_manifest_field(p_manifest, task_id_fields).is_empty()) {
		r_reason = "Provider task id is missing.";
		return false;
	}
	return true;
}

Dictionary SolersAssetService::_provider_config(const String &p_kind, const String &p_provider) const {
	EditorSettings *settings = EditorSettings::get_singleton();
	String provider = p_provider;
	if (provider.is_empty() && settings && settings->has_setting(_setting_path(p_kind, "provider"))) {
		provider = String(settings->get_setting(_setting_path(p_kind, "provider")));
	}
	if (provider.is_empty()) {
		provider = _default_provider(p_kind);
	}
	Dictionary config;
	config["provider"] = provider;
	config["base_url"] = settings && settings->has_setting(_setting_path(p_kind, "base_url")) ? String(settings->get_setting(_setting_path(p_kind, "base_url"))) : _default_base_url(provider);
	String key;
	if (settings && settings->has_setting(_setting_path(p_kind, "api_key"))) {
		key = SolersSecretStore::unprotect(String(settings->get_setting(_setting_path(p_kind, "api_key"))));
	}
	const String env = _default_env(provider);
	if (key.is_empty() && !env.is_empty() && OS::get_singleton()->has_environment(env)) {
		key = OS::get_singleton()->get_environment(env);
	}
	config["api_key"] = key;
	config["api_key_env"] = env;
	return config;
}

static String _solers_header_value(const List<String> &p_headers, const String &p_name) {
	const String needle = p_name.to_lower() + ":";
	for (const String &header : p_headers) {
		if (header.to_lower().begins_with(needle)) {
			return header.substr(needle.length()).strip_edges();
		}
	}
	return String();
}

static String _solers_url_scheme_name(const String &p_scheme) {
	String scheme = p_scheme.to_lower();
	if (scheme.ends_with("://")) {
		scheme = scheme.substr(0, scheme.length() - 3);
	}
	return scheme;
}

static String _solers_url_origin(const String &p_scheme, const String &p_host, int p_port) {
	const String scheme = _solers_url_scheme_name(p_scheme);
	String origin = scheme + "://" + p_host;
	if (!((scheme == "https" && p_port == 443) || (scheme == "http" && p_port == 80))) {
		origin += ":" + itos(p_port);
	}
	return origin;
}

static bool _solers_same_origin(const String &p_a, const String &p_b) {
	String a_scheme;
	String a_host;
	int a_port = 0;
	String a_path;
	String a_fragment;
	String b_scheme;
	String b_host;
	int b_port = 0;
	String b_path;
	String b_fragment;
	if (p_a.parse_url(a_scheme, a_host, a_port, a_path, a_fragment) != OK || p_b.parse_url(b_scheme, b_host, b_port, b_path, b_fragment) != OK) {
		return false;
	}
	a_scheme = _solers_url_scheme_name(a_scheme);
	b_scheme = _solers_url_scheme_name(b_scheme);
	if (a_port <= 0) {
		a_port = a_scheme == "https" ? 443 : 80;
	}
	if (b_port <= 0) {
		b_port = b_scheme == "https" ? 443 : 80;
	}
	return a_scheme == b_scheme && a_host.to_lower() == b_host.to_lower() && a_port == b_port;
}

static bool _solers_has_secret_header(const Vector<String> &p_headers) {
	for (const String &header : p_headers) {
		const String lower = header.to_lower();
		if (lower.begins_with("authorization:") || lower.begins_with("xi-api-key:")) {
			return true;
		}
	}
	return false;
}

static Dictionary _solers_headers_dict(const List<String> &p_headers) {
	Dictionary headers;
	for (const String &header : p_headers) {
		const int colon = header.find(":");
		if (colon > 0) {
			headers[header.substr(0, colon).strip_edges()] = header.substr(colon + 1).strip_edges();
		}
	}
	return headers;
}

static Dictionary _solers_http_error(const String &p_code, const String &p_message, const String &p_url, HTTPClient::Status p_status, int p_response_code, const List<String> &p_headers = List<String>()) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["url"] = p_url;
	error["http_client_status"] = (int)p_status;
	if (p_response_code > 0) {
		error["status"] = p_response_code;
	}
	if (!p_headers.is_empty()) {
		error["headers"] = _solers_headers_dict(p_headers);
	}
	return error;
}

Dictionary SolersAssetService::_http_request(const String &p_method, const String &p_url, const Vector<String> &p_headers, const PackedByteArray &p_body, uint64_t p_timeout_msec, int64_t p_max_body_bytes) {
	String current_url = p_url;
	String redirected_from;
	for (int redirect_count = 0; redirect_count <= 3; redirect_count++) {
		String scheme;
		String host;
		int port = 0;
		String path;
		String fragment;
		Error parse_err = current_url.parse_url(scheme, host, port, path, fragment);
		if (parse_err != OK || host.is_empty()) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _error_data("INVALID_URL", "Invalid provider URL.");
			return out;
		}
		scheme = _solers_url_scheme_name(scheme);
		if (scheme != "http" && scheme != "https") {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _error_data("INVALID_URL", "Provider URL must use http or https.");
			return out;
		}
		if (path.is_empty()) {
			path = "/";
		}
		const bool tls = scheme == "https";
		if (port <= 0) {
			port = tls ? 443 : 80;
		}

		Ref<HTTPClient> http = HTTPClient::create();
		if (http.is_null()) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _error_data("NO_HTTP_CLIENT", "Failed to create HTTPClient.");
			return out;
		}
		Ref<TLSOptions> tls_options = tls ? TLSOptions::client() : Ref<TLSOptions>();
		Error err = http->connect_to_host(host, port, tls_options);
		if (err != OK) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _error_data("CONNECT_FAILED", vformat("connect_to_host failed for %s:%d (error %d).", host, port, err));
			out["url"] = current_url;
			return out;
		}

		const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + p_timeout_msec;
		bool requested = false;
		bool reading = false;
		bool disconnected = false;
		PackedByteArray body;
		List<String> response_headers;
		bool response_headers_read = false;
		int response_code = 0;
		HTTPClient::Status last_status = HTTPClient::STATUS_DISCONNECTED;
		while (OS::get_singleton()->get_ticks_msec() < deadline) {
			http->poll();
			const HTTPClient::Status status = http->get_status();
			last_status = status;
			if (status == HTTPClient::STATUS_RESOLVING || status == HTTPClient::STATUS_CONNECTING || status == HTTPClient::STATUS_REQUESTING) {
				OS::get_singleton()->delay_usec(10000);
				continue;
			}
			if (status == HTTPClient::STATUS_CONNECTED) {
				if (!requested) {
					const HTTPClient::Method method = p_method == "GET" ? HTTPClient::METHOD_GET : HTTPClient::METHOD_POST;
					err = http->request(method, path, p_headers, p_body.ptr(), p_body.size());
					if (err != OK) {
						Dictionary out;
						out["ok"] = false;
						out["error"] = _error_data("REQUEST_FAILED", vformat("HTTP request failed (error %d).", err));
						out["url"] = current_url;
						return out;
					}
					requested = true;
				} else if (reading) {
					break;
				}
				OS::get_singleton()->delay_usec(10000);
				continue;
			}
			if (status == HTTPClient::STATUS_BODY) {
				reading = true;
				response_code = http->get_response_code();
				if (!response_headers_read) {
					http->get_response_headers(&response_headers);
					response_headers_read = true;
				}
				const PackedByteArray chunk = http->read_response_body_chunk();
				if (!chunk.is_empty()) {
					body.append_array(chunk);
					if (p_max_body_bytes >= 0 && body.size() > p_max_body_bytes) {
						Dictionary out;
						out["ok"] = false;
						out["error"] = _solers_http_error("HTTP_BODY_TOO_LARGE", "HTTP response body exceeded the local size limit.", current_url, status, response_code, response_headers);
						return out;
					}
				}
				OS::get_singleton()->delay_usec(5000);
				continue;
			}
			if (status == HTTPClient::STATUS_DISCONNECTED) {
				disconnected = true;
				break;
			}
			Dictionary out;
			out["ok"] = false;
			out["error"] = _solers_http_error("HTTP_STATUS_ERROR", "HTTP connection failed.", current_url, status, response_code, response_headers);
			return out;
		}

		if (!disconnected && OS::get_singleton()->get_ticks_msec() >= deadline) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = _solers_http_error("HTTP_TIMEOUT", "HTTP request timed out.", current_url, last_status, response_code, response_headers);
			return out;
		}

		if (response_code == 301 || response_code == 302 || response_code == 303 || response_code == 307 || response_code == 308) {
			const String location = _solers_header_value(response_headers, "Location");
			if (location.is_empty()) {
				Dictionary out;
				out["ok"] = false;
				out["status"] = response_code;
				out["url"] = current_url;
				Dictionary error = _solers_http_error("HTTP_REDIRECT_MISSING_LOCATION", "HTTP redirect did not include a Location header.", current_url, last_status, response_code, response_headers);
				out["error"] = error;
				return out;
			}
			String next_url = location;
			if (next_url.begins_with("/")) {
				next_url = _solers_url_origin(scheme, host, port) + next_url;
			} else if (!next_url.begins_with("http://") && !next_url.begins_with("https://")) {
				next_url = _solers_url_origin(scheme, host, port) + path.get_base_dir().path_join(next_url);
			}
			if (_solers_has_secret_header(p_headers) && !_solers_same_origin(current_url, next_url)) {
				Dictionary out;
				out["ok"] = false;
				out["status"] = response_code;
				out["url"] = current_url;
				out["redirected_to"] = next_url;
				Dictionary error = _solers_http_error("HTTP_REDIRECT_CROSS_ORIGIN", "Provider redirected to another origin while sensitive headers were present.", current_url, last_status, response_code, response_headers);
				error["location"] = location;
				error["redirected_to"] = next_url;
				out["error"] = error;
				return out;
			}
			redirected_from = current_url;
			current_url = next_url;
			continue;
		}

		Dictionary out;
		out["ok"] = response_code >= 200 && response_code < 300;
		out["status"] = response_code;
		out["url"] = current_url;
		if (!redirected_from.is_empty()) {
			out["redirected_from"] = redirected_from;
			out["redirected_to"] = current_url;
		}
		out["body"] = body;
		if (!(bool)out["ok"]) {
			Dictionary error = _solers_http_error("HTTP_ERROR", String::utf8((const char *)body.ptr(), MIN(body.size(), 2048)), current_url, last_status, response_code, response_headers);
			if (!redirected_from.is_empty()) {
				error["redirected_from"] = redirected_from;
				error["redirected_to"] = current_url;
			}
			out["error"] = error;
		}
		return out;
	}

	Dictionary out;
	out["ok"] = false;
	out["error"] = _error_data("HTTP_REDIRECT_LIMIT", "HTTP redirect limit exceeded.");
	return out;
}

bool SolersAssetService::_write_json_atomic(const String &p_path, const Dictionary &p_data, String &r_error) {
	const String dir = p_path.get_base_dir();
	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(dir));
	if (dir_err != OK) {
		r_error = "Failed to create asset directory.";
		return false;
	}
	const String tmp = p_path + ".tmp";
	Ref<FileAccess> file = FileAccess::open(tmp, FileAccess::WRITE);
	if (file.is_null()) {
		r_error = "Failed to open asset manifest.";
		return false;
	}
	file->store_string(JSON::stringify(p_data, "", false, true));
	file.unref();
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
	if (DirAccess::rename_absolute(ProjectSettings::get_singleton()->globalize_path(tmp), ProjectSettings::get_singleton()->globalize_path(p_path)) != OK) {
		r_error = "Failed to commit asset manifest.";
		return false;
	}
	return true;
}

Dictionary SolersAssetService::_read_json_file(const String &p_path) {
	if (!FileAccess::exists(p_path)) {
		return Dictionary();
	}
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(p_path));
	return parsed.get_type() == Variant::DICTIONARY ? (Dictionary)parsed : Dictionary();
}

bool SolersAssetService::_write_bytes_atomic(const String &p_path, const PackedByteArray &p_bytes, String &r_error) {
	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(p_path.get_base_dir()));
	if (dir_err != OK) {
		r_error = vformat("Failed to create asset file directory '%s' (error %d).", p_path.get_base_dir(), dir_err);
		return false;
	}
	const String tmp = p_path + ".tmp";
	Ref<FileAccess> file = FileAccess::open(tmp, FileAccess::WRITE);
	if (file.is_null()) {
		r_error = vformat("Failed to open asset file '%s'.", tmp);
		return false;
	}
	if (!p_bytes.is_empty()) {
		file->store_buffer(p_bytes.ptr(), p_bytes.size());
	}
	file.unref();
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
	const Error rename_err = DirAccess::rename_absolute(ProjectSettings::get_singleton()->globalize_path(tmp), ProjectSettings::get_singleton()->globalize_path(p_path));
	if (rename_err != OK) {
		r_error = vformat("Failed to commit asset file '%s' (error %d).", p_path, rename_err);
		return false;
	}
	return true;
}

bool SolersAssetService::_copy_file(const String &p_from, const String &p_to, String &r_error) {
	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(p_to.get_base_dir()));
	if (dir_err != OK) {
		r_error = "Failed to create target directory.";
		return false;
	}
	Error copy_err = DirAccess::copy_absolute(ProjectSettings::get_singleton()->globalize_path(p_from), ProjectSettings::get_singleton()->globalize_path(p_to));
	if (copy_err != OK) {
		r_error = vformat("Failed to copy %s.", p_from);
		return false;
	}
	return true;
}

void SolersAssetService::_download_preview(Task *p_task, Dictionary &r_state, const String &p_url) {
	if (p_url.is_empty()) {
		return;
	}
	Dictionary response = _http_request("GET", p_url, Vector<String>(), PackedByteArray(), 60000, 2 * 1024 * 1024);
	if (!(bool)response.get("ok", false)) {
		r_state["preview_error"] = response.get("error", Dictionary());
		_set_task_state(p_task, r_state);
		return;
	}

	const PackedByteArray bytes = response.get("body", PackedByteArray());
	Ref<Image> image;
	image.instantiate();
	Error err = image->load_png_from_buffer(bytes);
	if (err != OK) {
		err = image->load_jpg_from_buffer(bytes);
	}
	if (err != OK) {
		err = image->load_webp_from_buffer(bytes);
	}
	if (err != OK || image->is_empty()) {
		r_state["preview_error"] = _error_data("PREVIEW_DECODE_FAILED", "Provider thumbnail could not be decoded.");
		_set_task_state(p_task, r_state);
		return;
	}

	const int max_side = 512;
	const int width = image->get_width();
	const int height = image->get_height();
	if (width > max_side || height > max_side) {
		const float scale = MIN((float)max_side / (float)width, (float)max_side / (float)height);
		image->resize(MAX(1, (int)Math::round(width * scale)), MAX(1, (int)Math::round(height * scale)), Image::INTERPOLATE_LANCZOS);
	}
	image->convert(Image::FORMAT_RGB8);

	PackedByteArray preview = image->save_jpg_to_buffer(0.72f);
	if (preview.size() > 500 * 1024) {
		preview = image->save_jpg_to_buffer(0.55f);
	}
	if (preview.size() > 500 * 1024) {
		image->resize(MAX(1, image->get_width() * 3 / 4), MAX(1, image->get_height() * 3 / 4), Image::INTERPOLATE_LANCZOS);
		preview = image->save_jpg_to_buffer(0.45f);
	}
	if (preview.is_empty()) {
		r_state["preview_error"] = _error_data("PREVIEW_ENCODE_FAILED", "Provider thumbnail could not be encoded.");
		_set_task_state(p_task, r_state);
		return;
	}
	if (preview.size() > 500 * 1024) {
		r_state["preview_error"] = _error_data("PREVIEW_TOO_LARGE", "Provider thumbnail stayed above 500KB after resizing.");
		_set_task_state(p_task, r_state);
		return;
	}

	const String preview_path = _asset_dir(p_task->asset_id).path_join("preview.jpg");
	String write_error;
	if (!_write_bytes_atomic(preview_path, preview, write_error)) {
		r_state["preview_error"] = _error_data("PREVIEW_WRITE_FAILED", write_error);
		_set_task_state(p_task, r_state);
		return;
	}
	r_state["preview_file"] = preview_path;
	r_state.erase("preview_error");
	_set_task_state(p_task, r_state);
}

void SolersAssetService::_set_task_state(Task *p_task, const Dictionary &p_state) {
	{
		MutexLock lock(p_task->mutex);
		p_task->state = p_state.duplicate(true);
	}
	String error;
	_write_json_atomic(_manifest_path(p_task->asset_id), p_state, error);
}

Dictionary SolersAssetService::_task_state(Task *p_task) {
	MutexLock lock(p_task->mutex);
	return p_task->state.duplicate(true);
}

void SolersAssetService::_task_func(void *p_userdata) {
	Task *task = static_cast<Task *>(p_userdata);
	_run_task(task);
	task->done.set();
}

static PackedByteArray _utf8_bytes(const String &p_text) {
	CharString utf8 = p_text.utf8();
	PackedByteArray bytes;
	bytes.resize(utf8.length());
	if (utf8.length() > 0) {
		memcpy(bytes.ptrw(), utf8.get_data(), utf8.length());
	}
	return bytes;
}

static Dictionary _parse_json_body(const Dictionary &p_response) {
	const PackedByteArray body = p_response.get("body", PackedByteArray());
	const Variant parsed = JSON::parse_string(String::utf8((const char *)body.ptr(), body.size()));
	return parsed.get_type() == Variant::DICTIONARY ? (Dictionary)parsed : Dictionary();
}

static void _merge_options(Dictionary &r_body, const Dictionary &p_options) {
	for (const Variant *K = p_options.next(nullptr); K; K = p_options.next(K)) {
		r_body[*K] = p_options[*K];
	}
}

static String _solers_clean_base_url(const String &p_base_url) {
	String clean = p_base_url;
	while (clean.ends_with("/")) {
		clean = clean.substr(0, clean.length() - 1);
	}
	return clean;
}

static String _solers_meshy_task_id(const Dictionary &p_response) {
	return p_response.get("result", p_response.get("id", p_response.get("task_id", String())));
}

Dictionary SolersAssetService::_meshy_poll(Task *p_task, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers) {
	Dictionary detail;
	for (int i = 0; i < 90 && !p_task->abort.is_set(); i++) {
		Dictionary poll = _http_request("GET", p_url, p_headers, PackedByteArray(), 60000);
		if (!(bool)poll.get("ok", false)) {
			r_state["status"] = "failed";
			r_state["error"] = poll.get("error", Dictionary());
			_set_task_state(p_task, r_state);
			return Dictionary();
		}
		detail = _parse_json_body(poll);
		const String status = String(detail.get("status", String())).to_upper();
		r_state["provider_status"] = status;
		r_state["progress"] = detail.get("progress", r_state.get("progress", 0));
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
		_set_task_state(p_task, r_state);
		if (status == "SUCCEEDED") {
			return detail;
		}
		if (status == "FAILED" || status == "CANCELED" || status == "CANCELLED") {
			r_state["status"] = "failed";
			r_state["error"] = _error_data("PROVIDER_FAILED", "Meshy task failed.");
			r_state["provider_response"] = detail;
			_set_task_state(p_task, r_state);
			return Dictionary();
		}
		OS::get_singleton()->delay_usec(10000000);
	}
	if (p_task->abort.is_set()) {
		r_state["status"] = "cancelled";
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	} else {
		r_state["status"] = "failed";
		r_state["error"] = _error_data("PROVIDER_TIMEOUT", "Meshy task did not finish before the local wait limit.");
		r_state["provider_response"] = detail;
		r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	}
	_set_task_state(p_task, r_state);
	return Dictionary();
}

Dictionary SolersAssetService::_meshy_submit_and_poll(Task *p_task, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers, const Dictionary &p_body, const String &p_stage) {
	r_state["stage"] = p_stage;
	r_state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	_set_task_state(p_task, r_state);

	Dictionary submit = _http_request("POST", p_url, p_headers, _utf8_bytes(JSON::stringify(p_body)), 120000);
	if (!(bool)submit.get("ok", false)) {
		r_state["status"] = "failed";
		r_state["error"] = submit.get("error", Dictionary());
		_set_task_state(p_task, r_state);
		return Dictionary();
	}
	const Dictionary submitted = _parse_json_body(submit);
	String provider_task_id = _solers_meshy_task_id(submitted);
	if (provider_task_id.is_empty()) {
		r_state["status"] = "failed";
		r_state["error"] = _error_data("BAD_PROVIDER_RESPONSE", "Meshy response did not contain a task id.");
		r_state["provider_response"] = submitted;
		_set_task_state(p_task, r_state);
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
	_set_task_state(p_task, r_state);
	return _meshy_poll(p_task, r_state, p_url + "/" + provider_task_id, p_headers);
}

bool SolersAssetService::_meshy_download_model(Task *p_task, Dictionary &r_state, const Dictionary &p_detail, const String &p_asset_id, Array &r_files, String &r_preview_url) {
	const Dictionary model_urls = p_detail.get("model_urls", Dictionary());
	String ext = "glb";
	String url = model_urls.get("glb", String());
	if (url.is_empty()) {
		url = model_urls.get("fbx", String());
		ext = "fbx";
	}
	if (url.is_empty()) {
		url = model_urls.get("obj", String());
		ext = "obj";
	}
	if (url.is_empty()) {
		url = p_detail.get("model_url", String());
		ext = url.get_extension().get_slice("?", 0).to_lower();
		if (ext.is_empty() || ext.length() > 8) {
			ext = "glb";
		}
	}
	if (url.is_empty()) {
		const Dictionary result = p_detail.get("result", Dictionary());
		url = result.get("rigged_character_glb_url", result.get("animation_glb_url", String()));
		ext = "glb";
		if (url.is_empty()) {
			url = result.get("rigged_character_fbx_url", result.get("animation_fbx_url", String()));
			ext = "fbx";
		}
	}
	if (url.is_empty()) {
		r_state["status"] = "failed";
		r_state["error"] = _error_data("NO_DOWNLOAD_URL", "Meshy response did not include a downloadable model URL.");
		r_state["provider_response"] = p_detail;
		_set_task_state(p_task, r_state);
		return false;
	}

	r_state["provider_response"] = p_detail;
	r_preview_url = p_detail.get("thumbnail_url", String());
	Dictionary download = _http_request("GET", url, Vector<String>(), PackedByteArray(), 180000);
	if (!(bool)download.get("ok", false)) {
		r_state["status"] = "failed";
		r_state["error"] = download.get("error", Dictionary());
		_set_task_state(p_task, r_state);
		return false;
	}
	const String file_path = _source_dir(p_asset_id).path_join(p_asset_id + "." + ext);
	String write_error;
	if (!_write_bytes_atomic(file_path, download.get("body", PackedByteArray()), write_error)) {
		r_state["status"] = "failed";
		Dictionary error = _error_data("WRITE_FAILED", write_error);
		error["path"] = file_path;
		r_state["error"] = error;
		_set_task_state(p_task, r_state);
		return false;
	}
	r_files.push_back(file_path);
	return true;
}

void SolersAssetService::_run_task(Task *p_task) {
	Dictionary state = _task_state(p_task);
	const String kind = state.get("kind", String());
	const String provider = state.get("provider", String());
	const String prompt = state.get("prompt", String());
	const String base_url = state.get("base_url", String());
	const String api_key = p_task->api_key;
	const String profile = state.get("profile", String("game_default"));
	const Dictionary provider_options = state.get("provider_options", Dictionary());

	state["status"] = "running";
	state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	_set_task_state(p_task, state);

	if (api_key.is_empty()) {
		state["status"] = "failed";
		state["error"] = _error_data("API_KEY_MISSING", "Asset provider API key is not configured.");
		_set_task_state(p_task, state);
		return;
	}

	Array files;
	String preview_url;
	String write_error;
	if (provider == "elevenlabs") {
		Dictionary body;
		_merge_options(body, provider_options);
		if (kind == "music") {
			if (!body.has("prompt")) {
				body["prompt"] = prompt;
			}
		} else {
			if (!body.has("text")) {
				body["text"] = prompt;
			}
		}
		const String json = JSON::stringify(body);
		Vector<String> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Accept: audio/mpeg");
		headers.push_back("xi-api-key: " + api_key);
		const String endpoint = kind == "music" ? "/v1/music" : "/v1/sound-generation";
		String clean_base_url = base_url;
		while (clean_base_url.ends_with("/")) {
			clean_base_url = clean_base_url.substr(0, clean_base_url.length() - 1);
		}
		Dictionary response = _http_request("POST", clean_base_url + endpoint, headers, _utf8_bytes(json), 120000);
		if (!(bool)response.get("ok", false)) {
			state["status"] = "failed";
			state["error"] = response.get("error", Dictionary());
			_set_task_state(p_task, state);
			return;
		}
		const String output_format = String(provider_options.get("output_format", String())).to_lower();
		const String ext = output_format.contains("wav") ? "wav" : "mp3";
		const String file_path = _source_dir(p_task->asset_id).path_join(p_task->asset_id + "." + ext);
		if (!_write_bytes_atomic(file_path, response.get("body", PackedByteArray()), write_error)) {
			state["status"] = "failed";
			state["error"] = _error_data("WRITE_FAILED", write_error);
			_set_task_state(p_task, state);
			return;
		}
		files.push_back(file_path);
	} else if (provider == "meshy") {
		Vector<String> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Authorization: Bearer " + api_key);
		const String clean_base_url = _solers_clean_base_url(base_url);
		const String operation = String(state.get("operation", "generate"));
		if (operation == "refine") {
			Dictionary refine_body;
			_merge_options(refine_body, provider_options);
			refine_body["mode"] = "refine";
			if (!refine_body.has("preview_task_id")) {
				refine_body["preview_task_id"] = state.get("source_task_id", String());
			}
			if (!refine_body.has("enable_pbr")) {
				refine_body["enable_pbr"] = true;
			}
			if (!refine_body.has("target_formats")) {
				Array formats;
				formats.push_back("glb");
				refine_body["target_formats"] = formats;
			}
			const Dictionary detail = _meshy_submit_and_poll(p_task, state, clean_base_url + "/openapi/v2/text-to-3d", headers, refine_body, "refining");
			if (detail.is_empty() || !_meshy_download_model(p_task, state, detail, p_task->asset_id, files, preview_url)) {
				return;
			}
		} else if (operation == "rig_humanoid" || operation == "animate_humanoid") {
			Dictionary body;
			_merge_options(body, provider_options);
			const String source_task_id = state.get("source_task_id", String());
			if (operation == "rig_humanoid" && !source_task_id.is_empty() && !body.has("input_task_id")) {
				body["input_task_id"] = source_task_id;
			}
			if (operation == "animate_humanoid" && !source_task_id.is_empty() && !body.has("rig_task_id")) {
				body["rig_task_id"] = source_task_id;
			}
			const String endpoint = operation == "rig_humanoid" ? "/openapi/v1/rigging" : "/openapi/v1/animations";
			const String stage = operation == "rig_humanoid" ? "rigging" : "animating";
			const Dictionary detail = _meshy_submit_and_poll(p_task, state, clean_base_url + endpoint, headers, body, stage);
			if (detail.is_empty() || !_meshy_download_model(p_task, state, detail, p_task->asset_id, files, preview_url)) {
				return;
			}
		} else if (operation == "remesh" || operation == "retexture") {
			Dictionary body;
			_merge_options(body, provider_options);
			const String source_task_id = state.get("source_task_id", String());
			if (!source_task_id.is_empty() && !body.has("input_task_id")) {
				body["input_task_id"] = source_task_id;
			}
			if (!body.has("target_formats")) {
				Array formats;
				formats.push_back("glb");
				body["target_formats"] = formats;
			}
			const String endpoint = operation == "remesh" ? "/openapi/v1/remesh" : "/openapi/v1/retexture";
			const String stage = operation == "remesh" ? "optimizing" : "restyling";
			const Dictionary detail = _meshy_submit_and_poll(p_task, state, clean_base_url + endpoint, headers, body, stage);
			if (detail.is_empty() || !_meshy_download_model(p_task, state, detail, p_task->asset_id, files, preview_url)) {
				return;
			}
		} else {
			Dictionary preview_body;
			_merge_options(preview_body, provider_options);
			const bool draft_only = String(preview_body.get("mode", String())).to_lower() == "preview";
			preview_body["mode"] = "preview";
			if (!preview_body.has("prompt")) {
				preview_body["prompt"] = prompt;
			}
			if (profile == "game_default" && !preview_body.has("model_type")) {
				preview_body["model_type"] = "lowpoly";
			}
			if (!preview_body.has("target_formats")) {
				Array formats;
				formats.push_back("glb");
				preview_body["target_formats"] = formats;
			}
			const String text_to_3d_url = clean_base_url + "/openapi/v2/text-to-3d";
			Dictionary preview_detail = _meshy_submit_and_poll(p_task, state, text_to_3d_url, headers, preview_body, "previewing");
			if (preview_detail.is_empty()) {
				return;
			}
			if (draft_only) {
				state["status"] = "draft";
				state["stage"] = "draft";
				if (!_meshy_download_model(p_task, state, preview_detail, p_task->asset_id, files, preview_url)) {
					return;
				}
			} else {
				Dictionary refine_body;
				_merge_options(refine_body, provider_options);
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
				Dictionary refine_detail = _meshy_submit_and_poll(p_task, state, text_to_3d_url, headers, refine_body, "refining");
				if (refine_detail.is_empty() || !_meshy_download_model(p_task, state, refine_detail, p_task->asset_id, files, preview_url)) {
					return;
				}
			}
		}
	} else {
		state["status"] = "failed";
		state["error"] = _error_data("UNSUPPORTED_PROVIDER", vformat("Unsupported asset provider: %s", provider));
		_set_task_state(p_task, state);
		return;
	}

	if (String(state.get("status", String())) != "draft") {
		state["status"] = "ready";
		state["stage"] = "ready";
	}
	state["files"] = files;
	state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	_set_task_state(p_task, state);
	_download_preview(p_task, state, preview_url);
}

Dictionary SolersAssetService::_manifest_for_asset(const String &p_asset_id) const {
	Task *const *task = tasks.getptr(p_asset_id);
	if (task && *task) {
		return _task_state(*task);
	}
	return _read_json_file(_manifest_path(p_asset_id));
}

void SolersAssetService::_cleanup_finished_task(const String &p_asset_id) const {
	Task *task = nullptr;
	{
		MutexLock lock(tasks_mutex);
		Task *const *found = const_cast<HashMap<String, Task *> *>(&tasks)->getptr(p_asset_id);
		if (!found || !*found) {
			return;
		}
		task = *found;
		const String status = String(_task_state(task).get("status", String()));
		if (status != "ready" && status != "draft" && status != "failed" && status != "cancelled") {
			return;
		}
		if (!task->done.is_set()) {
			return;
		}
		const_cast<HashMap<String, Task *> *>(&tasks)->erase(p_asset_id);
	}
	if (task->thread.is_started()) {
		task->thread.wait_to_finish();
	}
	memdelete(task);
}

Dictionary SolersAssetService::generate(const Dictionary &p_args) {
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	if (kind != "3d" && kind != "music" && kind != "sfx") {
		return _error("INVALID_ARGUMENT", "kind must be 3d, music, or sfx.");
	}
	const String prompt = String(p_args.get("prompt", String())).strip_edges();
	if (prompt.is_empty()) {
		return _error("INVALID_ARGUMENT", "prompt is required.");
	}
	String provider = String(p_args.get("provider", String())).strip_edges().to_lower();
	const Dictionary provider_config = _provider_config(kind, provider);
	provider = String(provider_config.get("provider", provider));
	const String asset_id = vformat("%s_%s", itos((int64_t)Time::get_singleton()->get_unix_time_from_system()), (kind + prompt + provider).md5_text().substr(0, 10));

	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = kind;
	manifest["name"] = p_args.get("name", prompt);
	manifest["prompt"] = prompt;
	manifest["profile"] = p_args.get("profile", "game_default");
	manifest["provider"] = provider;
	manifest["base_url"] = provider_config.get("base_url", String());
	manifest["provider_options"] = p_args.get("provider_options", Dictionary());
	if (kind == "3d") {
		Dictionary traits;
		traits["model_state"] = "static_model";
		manifest["traits"] = traits;
	}
	manifest["status"] = "queued";
	manifest["created_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	manifest["files"] = Array();
	return _queue_manifest(manifest, provider_config);
}

Dictionary SolersAssetService::_queue_manifest(const Dictionary &p_manifest, const Dictionary &p_provider_config) {
	const String asset_id = p_manifest.get("id", String());
	if (asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset id is required.");
	}
	String error;
	if (!_write_json_atomic(_manifest_path(asset_id), p_manifest, error)) {
		return _error("WRITE_FAILED", error);
	}

	Task *task = memnew(Task);
	task->asset_id = asset_id;
	task->api_key = p_provider_config.get("api_key", String());
	_set_task_state(task, p_manifest);
	{
		MutexLock lock(tasks_mutex);
		tasks[asset_id] = task;
	}
	task->thread.start(&SolersAssetService::_task_func, task);

	return _ok(p_manifest.duplicate(true));
}

Dictionary SolersAssetService::capabilities(const Dictionary &p_args) const {
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset_id is required.");
	}
	const Dictionary manifest = _manifest_for_asset(asset_id);
	if (manifest.is_empty()) {
		return _error("NOT_FOUND", "Asset was not found.");
	}
	Array operations;
	Array available_operations;
	const Array defs = _solers_operation_defs(String(manifest.get("provider", String())).to_lower());
	for (int i = 0; i < defs.size(); i++) {
		Dictionary operation = Dictionary(defs[i]).duplicate(true);
		String reason;
		const bool available = _solers_manifest_matches_operation(manifest, operation, reason);
		operation["available"] = available;
		operation["ui_supported"] = available && _solers_schema_ui_supported(operation.get("options_schema", Dictionary()));
		if (!available) {
			operation["reason"] = reason;
		} else {
			available_operations.push_back(operation);
		}
		operations.push_back(operation);
	}
	Dictionary data;
	data["asset_id"] = asset_id;
	data["operations"] = operations;
	data["available_operations"] = available_operations;
	return _ok(data);
}

Dictionary SolersAssetService::run_operation(const Dictionary &p_args) {
	const String source_asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (source_asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset_id is required.");
	}
	const String operation_id = String(p_args.get("operation_id", String())).strip_edges();
	if (operation_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "operation_id is required.");
	}
	const Dictionary source = _manifest_for_asset(source_asset_id);
	if (source.is_empty()) {
		return _error("NOT_FOUND", "Source asset was not found.");
	}
	const String provider = String(source.get("provider", String())).to_lower();
	Dictionary operation;
	const Array defs = _solers_operation_defs(provider);
	for (int i = 0; i < defs.size(); i++) {
		const Dictionary candidate = defs[i];
		if (String(candidate.get("operation_id", String())) == operation_id) {
			operation = candidate;
			break;
		}
	}
	if (operation.is_empty()) {
		return _error("OPERATION_NOT_FOUND", "Asset operation is not supported by this provider.");
	}
	String reason;
	if (!_solers_manifest_matches_operation(source, operation, reason)) {
		return _error("OPERATION_NOT_AVAILABLE", reason);
	}
	Dictionary options = p_args.get("options", Dictionary());
	const Dictionary raw_provider_options = p_args.get("raw_provider_options", Dictionary());
	if (!raw_provider_options.is_empty() && !(bool)p_args.get("raw_confirmed", false)) {
		return _error("RAW_OPTIONS_REQUIRE_CONFIRMATION", "raw_provider_options requires raw_confirmed=true.");
	}
	const Dictionary schema = operation.get("options_schema", Dictionary());
	const Dictionary properties = schema.get("properties", Dictionary());
	const Array required = schema.get("required", Array());
	for (int i = 0; i < required.size(); i++) {
		const String name = String(required[i]);
		if (!options.has(name) && !raw_provider_options.has(name)) {
			return _error("INVALID_ARGUMENT", vformat("Missing required option: %s", name));
		}
		const Variant value = options.has(name) ? options[name] : raw_provider_options[name];
		const String type = String(Dictionary(properties.get(name, Dictionary())).get("type", String()));
		if (type == "string" && String(value).strip_edges().is_empty()) {
			return _error("INVALID_ARGUMENT", vformat("Missing required option: %s", name));
		}
	}
	if (operation_id == "rig_humanoid" && !(bool)options.get("humanoid_confirmed", raw_provider_options.get("humanoid_confirmed", false))) {
		return _error("HUMANOID_CONFIRMATION_REQUIRED", "Rigging requires confirming this is a humanoid character.");
	}

	const Dictionary requires = operation.get("requires", Dictionary());
	const String source_task_id = _solers_first_manifest_field(source, requires.get("task_id_fields", Array()));
	const Dictionary provider_config = _provider_config("3d", provider);
	Dictionary provider_options;
	_merge_options(provider_options, options);
	_merge_options(provider_options, raw_provider_options);
	provider_options.erase("humanoid_confirmed");
	if (operation_id == "refine") {
		provider_options["mode"] = "refine";
		if (!provider_options.has("preview_task_id")) {
			provider_options["preview_task_id"] = source_task_id;
		}
		if (!provider_options.has("enable_pbr")) {
			provider_options["enable_pbr"] = true;
		}
	} else if (operation_id == "remesh") {
		if (!provider_options.has("input_task_id")) {
			provider_options["input_task_id"] = source_task_id;
		}
		if (!provider_options.has("topology")) {
			provider_options["topology"] = "triangle";
		}
		if (!provider_options.has("target_polycount")) {
			const String quality = String(options.get("quality", "balanced")).to_lower();
			provider_options["target_polycount"] = quality == "game_ready" ? 15000 : (quality == "high_detail" ? 80000 : 30000);
		}
	} else if (operation_id == "retexture") {
		if (!provider_options.has("input_task_id")) {
			provider_options["input_task_id"] = source_task_id;
		}
		if (!provider_options.has("enable_original_uv")) {
			provider_options["enable_original_uv"] = true;
		}
		if (!provider_options.has("enable_pbr")) {
			provider_options["enable_pbr"] = true;
		}
		if (!provider_options.has("remove_lighting")) {
			provider_options["remove_lighting"] = true;
		}
	} else if (operation_id == "rig_humanoid") {
		if (!provider_options.has("input_task_id")) {
			provider_options["input_task_id"] = source_task_id;
		}
	} else if (operation_id == "animate_humanoid") {
		if (!provider_options.has("rig_task_id")) {
			provider_options["rig_task_id"] = source_task_id;
		}
	}
	if ((operation_id == "refine" || operation_id == "remesh" || operation_id == "retexture") && !provider_options.has("target_formats")) {
		Array formats;
		formats.push_back("glb");
		provider_options["target_formats"] = formats;
	}

	const String label = String(operation.get("label", operation_id));
	const String name = String(source.get("name", source_asset_id)) + " " + label;
	const String prompt = String(source.get("prompt", String()));
	const String asset_id = vformat("%s_%s", itos((int64_t)Time::get_singleton()->get_unix_time_from_system()), ("3d" + source_asset_id + operation_id + JSON::stringify(provider_options)).md5_text().substr(0, 10));
	Dictionary traits = _solers_asset_traits(source);
	if (operation_id == "rig_humanoid") {
		traits["model_state"] = "rigged_model";
		traits["rig"] = "humanoid";
	} else if (operation_id == "animate_humanoid") {
		traits["model_state"] = "animated_model";
		traits["rig"] = "humanoid";
		traits["animation"] = "present";
	} else {
		traits["model_state"] = "static_model";
	}
	Dictionary manifest;
	manifest["id"] = asset_id;
	manifest["kind"] = "3d";
	manifest["name"] = name;
	manifest["prompt"] = prompt;
	manifest["profile"] = source.get("profile", "game_default");
	manifest["provider"] = provider;
	manifest["base_url"] = provider_config.get("base_url", String());
	manifest["provider_options"] = provider_options;
	manifest["parent_asset_id"] = source_asset_id;
	manifest["operation"] = operation_id;
	manifest["operation_label"] = label;
	manifest["provider_operation_id"] = operation.get("provider_operation_id", String());
	manifest["docs"] = operation.get("docs", String());
	manifest["source_task_id"] = source_task_id;
	manifest["traits"] = traits;
	manifest["status"] = "queued";
	manifest["stage"] = "queued";
	manifest["created_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	manifest["files"] = Array();
	return _queue_manifest(manifest, provider_config);
}

Dictionary SolersAssetService::refine_to_ready(const Dictionary &p_args) {
	Dictionary args = p_args.duplicate(true);
	args["operation_id"] = "refine";
	return run_operation(args);
}

Dictionary SolersAssetService::optimize_geometry(const Dictionary &p_args) {
	Dictionary args;
	args["asset_id"] = p_args.get("asset_id", String());
	args["operation_id"] = "remesh";
	Dictionary options;
	options["quality"] = p_args.get("quality", "balanced");
	args["options"] = options;
	return run_operation(args);
}

Dictionary SolersAssetService::restyle_material(const Dictionary &p_args) {
	Dictionary args;
	args["asset_id"] = p_args.get("asset_id", String());
	args["operation_id"] = "retexture";
	Dictionary options;
	options["text_style_prompt"] = p_args.get("text_style_prompt", String("Cleaner game material"));
	args["options"] = options;
	return run_operation(args);
}

Dictionary SolersAssetService::status(const Dictionary &p_args) const {
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset_id is required.");
	}
	Dictionary manifest = _manifest_for_asset(asset_id);
	if (manifest.is_empty()) {
		return _error("NOT_FOUND", "Asset task was not found.");
	}
	_cleanup_finished_task(asset_id);
	return _ok(manifest);
}

Dictionary SolersAssetService::list_local(const Dictionary &p_args) const {
	const String kind_filter = String(p_args.get("kind", String())).strip_edges().to_lower();
	const String query = String(p_args.get("query", String())).strip_edges().to_lower();
	const int limit = MAX(1, MIN((int)p_args.get("limit", 128), 512));

	Array assets;
	const PackedStringArray dirs = DirAccess::get_directories_at(_asset_root());
	for (int i = 0; i < dirs.size() && assets.size() < limit; i++) {
		Dictionary manifest = _read_json_file(_manifest_path(dirs[i]));
		if (manifest.is_empty()) {
			continue;
		}
		const String kind = String(manifest.get("kind", String())).to_lower();
		if (!kind_filter.is_empty() && kind != kind_filter) {
			continue;
		}
		const String haystack = (String(manifest.get("name", String())) + " " + String(manifest.get("prompt", String()))).to_lower();
		if (!query.is_empty() && !haystack.contains(query)) {
			continue;
		}
		manifest.erase("api_key");
		assets.push_back(manifest);
	}
	Dictionary data;
	data["assets"] = assets;
	data["count"] = assets.size();
	return _ok(data);
}

Dictionary SolersAssetService::import_to_project(const Dictionary &p_args) const {
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if (asset_id.is_empty()) {
		return _error("INVALID_ARGUMENT", "asset_id is required.");
	}
	const Dictionary manifest = _manifest_for_asset(asset_id);
	if (manifest.is_empty()) {
		return _error("NOT_FOUND", "Asset was not found.");
	}
	const String status = String(manifest.get("status", String()));
	if (status != "ready" && status != "draft") {
		return _error("ASSET_NOT_READY", "Asset is not ready to import.");
	}
	const Array files = manifest.get("files", Array());
	if (files.is_empty()) {
		return _error("ASSET_HAS_NO_FILES", "Asset has no source files.");
	}
	String target_dir = String(p_args.get("target_dir", String()));
	if (target_dir.is_empty()) {
		target_dir = "res://solers_assets/" + String(manifest.get("kind", "asset")) + "/" + _safe_slug(String(manifest.get("name", asset_id)));
	}
	target_dir = target_dir.replace_char('\\', '/').simplify_path();
	if (!target_dir.begins_with("res://") || target_dir.contains("..")) {
		return _error("INVALID_TARGET", "target_dir must stay inside res://.");
	}
	Array imported;
	for (int i = 0; i < files.size(); i++) {
		const String src = String(files[i]);
		const String dst = target_dir.path_join(src.get_file());
		String err;
		if (!_copy_file(src, dst, err)) {
			return _error("IMPORT_FAILED", err);
		}
		imported.push_back(dst);
		if (EditorFileSystem::get_singleton()) {
			EditorFileSystem::get_singleton()->update_file(dst);
		}
	}
	Dictionary data;
	data["asset_id"] = asset_id;
	data["target_dir"] = target_dir;
	data["files"] = imported;
	return _ok(data);
}

SolersAssetService::SolersAssetService() {}

SolersAssetService::~SolersAssetService() {
	Vector<Task *> pending;
	{
		MutexLock lock(tasks_mutex);
		for (const KeyValue<String, Task *> &E : tasks) {
			pending.push_back(E.value);
		}
		tasks.clear();
	}
	for (int i = 0; i < pending.size(); i++) {
		pending[i]->abort.set();
		if (pending[i]->thread.is_started()) {
			pending[i]->thread.wait_to_finish();
		}
		memdelete(pending[i]);
	}
}
