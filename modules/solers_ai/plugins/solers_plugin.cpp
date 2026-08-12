/**************************************************************************/
/*  solers_plugin.cpp                                                     */
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

#include "solers_plugin.h"

#include "solers_plugin_ambientcg.h"
#include "solers_plugin_elevenlabs.h"
#include "solers_plugin_meshy.h"
#include "solers_plugin_polyhaven.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/json.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "editor/settings/editor_settings.h"

// Manifest files are persisted snapshots, not the live task database. Keep
// every in-process reader outside the short replace window so a reader can
// never observe the temporary absence created by DirAccess on Windows.
static Mutex solers_plugin_manifest_mutex;

Vector<SolersPlugin *> SolersPluginRegistry::plugins;
Vector<SolersPlugin *> SolersPluginRegistry::builtins;

void SolersPluginJob::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_asset_id"), &SolersPluginJob::get_asset_id);
	ClassDB::bind_method(D_METHOD("get_api_key"), &SolersPluginJob::get_api_key);
	ClassDB::bind_method(D_METHOD("get_base_url"), &SolersPluginJob::get_base_url);
	ClassDB::bind_method(D_METHOD("get_staging_dir"), &SolersPluginJob::get_staging_dir);
	ClassDB::bind_method(D_METHOD("is_resume"), &SolersPluginJob::is_resume);
	ClassDB::bind_method(D_METHOD("is_cancelled"), &SolersPluginJob::is_cancelled);
	ClassDB::bind_method(D_METHOD("get_state"), &SolersPluginJob::get_state);
	ClassDB::bind_method(D_METHOD("set_state", "state"), &SolersPluginJob::set_state);
	ClassDB::bind_method(D_METHOD("fail", "code", "message"), &SolersPluginJob::fail);
	ClassDB::bind_method(D_METHOD("add_file", "path"), &SolersPluginJob::add_file);
	ClassDB::bind_method(D_METHOD("get_files"), &SolersPluginJob::get_files);
	ClassDB::bind_method(D_METHOD("set_preview_url", "url"), &SolersPluginJob::set_preview_url);
	ClassDB::bind_method(D_METHOD("get_preview_url"), &SolersPluginJob::get_preview_url);
	ClassDB::bind_method(D_METHOD("http_request", "method", "url", "headers", "body", "timeout_msec", "max_body_bytes"), &SolersPluginJob::http_request_bound, DEFVAL(-1));
}

void SolersPluginJob::setup(const String &p_asset_id, const String &p_api_key, const String &p_base_url, const String &p_staging_dir, bool p_resume, const SafeFlag *p_abort, const Dictionary &p_state, const std::function<void(const Dictionary &)> &p_persist) {
	asset_id = p_asset_id;
	api_key = p_api_key;
	base_url = p_base_url;
	staging_dir = p_staging_dir;
	resume = p_resume;
	abort = p_abort;
	state = p_state.duplicate(true);
	persist = p_persist;
}

void SolersPluginJob::set_state(const Dictionary &p_state) {
	state = p_state.duplicate(true);
	if (persist) {
		persist(state);
	}
}

void SolersPluginJob::fail(const String &p_code, const String &p_message) {
	Dictionary failed = get_state();
	failed["status"] = "failed";
	failed["error"] = SolersPlugin::error_data(p_code, p_message);
	set_state(failed);
}

Dictionary SolersPluginJob::http_request(const String &p_method, const String &p_url, const Vector<String> &p_headers, const PackedByteArray &p_body, uint64_t p_timeout_msec, int64_t p_max_body_bytes) const {
	return SolersPlugin::http_request(p_method, p_url, p_headers, p_body, p_timeout_msec, p_max_body_bytes, abort);
}

Dictionary SolersPluginJob::http_request_bound(const String &p_method, const String &p_url, const PackedStringArray &p_headers, const PackedByteArray &p_body, int64_t p_timeout_msec, int64_t p_max_body_bytes) const {
	Vector<String> headers;
	for (const String &header : p_headers) {
		headers.push_back(header);
	}
	return http_request(p_method, p_url, headers, p_body, (uint64_t)MAX((int64_t)0, p_timeout_msec), p_max_body_bytes);
}

void SolersPlugin::_bind_methods() {
	ClassDB::bind_static_method("SolersPlugin", D_METHOD("register_plugin", "plugin"), &SolersPluginRegistry::register_plugin);
	ClassDB::bind_static_method("SolersPlugin", D_METHOD("unregister_plugin", "plugin"), &SolersPluginRegistry::unregister_plugin);
	GDVIRTUAL_BIND(_get_profile);
	GDVIRTUAL_BIND(_get_generation_options_schema, "kind");
	GDVIRTUAL_BIND(_get_operation_defs);
	GDVIRTUAL_BIND(_prepare_generate, "kind", "args", "manifest");
	GDVIRTUAL_BIND(_prepare_operation, "operation", "source_manifest", "provider_options");
	GDVIRTUAL_BIND(_capability_extras, "manifest");
	GDVIRTUAL_BIND(_catalog_search, "args");
	GDVIRTUAL_BIND(_catalog_inspect, "args");
	GDVIRTUAL_BIND(_run_job, "job");
}

Dictionary SolersPlugin::get_profile() const {
	Dictionary profile;
	GDVIRTUAL_CALL(_get_profile, profile);
	return profile;
}

Dictionary SolersPlugin::get_generation_options_schema(const String &p_kind) const {
	Dictionary schema;
	GDVIRTUAL_CALL(_get_generation_options_schema, p_kind, schema);
	return schema;
}

Array SolersPlugin::get_operation_defs() const {
	Array defs;
	GDVIRTUAL_CALL(_get_operation_defs, defs);
	return defs;
}

Dictionary SolersPlugin::prepare_generate(const String &p_kind, const Dictionary &p_args, Dictionary &r_manifest) const {
	Dictionary result;
	GDVIRTUAL_CALL(_prepare_generate, p_kind, p_args, r_manifest, result);
	return result;
}

Dictionary SolersPlugin::prepare_operation(const Dictionary &p_operation, const Dictionary &p_source_manifest, Dictionary &r_provider_options) const {
	Dictionary result;
	GDVIRTUAL_CALL(_prepare_operation, p_operation, p_source_manifest, r_provider_options, result);
	return result;
}

Dictionary SolersPlugin::capability_extras(const Dictionary &p_manifest) const {
	Dictionary result;
	GDVIRTUAL_CALL(_capability_extras, p_manifest, result);
	return result;
}

Dictionary SolersPlugin::catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	Dictionary result;
	if (GDVIRTUAL_CALL(_catalog_search, p_args, result)) {
		return result;
	}
	Dictionary out;
	out["ok"] = false;
	out["error"] = error_data("CATALOG_UNSUPPORTED", "This connector does not expose a catalog.");
	return out;
}

Dictionary SolersPlugin::catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	Dictionary result;
	if (GDVIRTUAL_CALL(_catalog_inspect, p_args, result)) {
		return result;
	}
	Dictionary out;
	out["ok"] = false;
	out["error"] = error_data("CATALOG_UNSUPPORTED", "This connector does not expose a catalog.");
	return out;
}

Dictionary SolersPlugin::cached_inspection(const String &p_kind, const String &p_asset_id) const {
	MutexLock lock(catalog_cache_mutex);
	return Dictionary(catalog_inspections.get(p_kind.to_lower() + "|" + p_asset_id.to_lower(), Dictionary())).duplicate(true);
}

void SolersPlugin::run_job(const Ref<SolersPluginJob> &p_job) {
	if (GDVIRTUAL_CALL(_run_job, p_job)) {
		return;
	}
	p_job->fail("UNSUPPORTED_PROVIDER", "This connector does not implement job execution.");
}

static bool _solers_plugin_id_char(char32_t p_char);

void SolersPluginRegistry::register_plugin(SolersPlugin *p_plugin) {
	if (!p_plugin || plugins.has(p_plugin)) {
		return;
	}
	const String id = String(p_plugin->get_profile().get("id", String())).strip_edges().to_lower();
	ERR_FAIL_COND_MSG(id.is_empty(), "A Solers plugin profile must declare a non-empty id.");
	ERR_FAIL_COND_MSG(!((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9')), "A Solers plugin id must begin with an ASCII letter or digit.");
	for (int i = 1; i < id.length(); i++) {
		ERR_FAIL_COND_MSG(!_solers_plugin_id_char(id[i]), "A Solers plugin id may contain only ASCII letters, digits, '.', '-', and '_'.");
	}
	ERR_FAIL_COND_MSG(get_plugin(id) != nullptr, vformat("A Solers plugin with id '%s' is already registered.", id));
	plugins.push_back(p_plugin);
}

void SolersPluginRegistry::unregister_plugin(SolersPlugin *p_plugin) {
	plugins.erase(p_plugin);
}

SolersPlugin *SolersPluginRegistry::get_plugin(const String &p_id) {
	const String id = p_id.strip_edges().to_lower();
	if (id.is_empty()) {
		return nullptr;
	}
	for (SolersPlugin *plugin : plugins) {
		if (String(plugin->get_profile().get("id", String())).to_lower() == id) {
			return plugin;
		}
	}
	return nullptr;
}

const Vector<SolersPlugin *> &SolersPluginRegistry::get_plugins() {
	return plugins;
}

static bool _solers_plugin_id_char(char32_t p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') || (p_char >= '0' && p_char <= '9') || p_char == '-' || p_char == '_' || p_char == '.';
}

void SolersPluginRegistry::register_builtins() {
	if (!builtins.is_empty()) {
		return;
	}
	builtins.push_back(memnew(SolersPluginMeshy));
	builtins.push_back(memnew(SolersPluginElevenLabs));
	builtins.push_back(memnew(SolersPluginAmbientCG));
	builtins.push_back(memnew(SolersPluginPolyHaven));
	for (SolersPlugin *plugin : builtins) {
		register_plugin(plugin);
	}
}

void SolersPluginRegistry::unregister_builtins() {
	for (SolersPlugin *plugin : builtins) {
		unregister_plugin(plugin);
		memdelete(plugin);
	}
	builtins.clear();
}

SolersPlugin *SolersPluginRegistry::default_generator_for_kind(const String &p_kind) {
	const String kind = p_kind.strip_edges().to_lower();
	EditorSettings *settings = EditorSettings::get_singleton();
	const String setting_path = "solers/plugins/default/" + kind;
	if (settings && settings->has_setting(setting_path)) {
		SolersPlugin *configured = get_plugin(String(settings->get_setting(setting_path)));
		if (configured && Array(configured->get_profile().get("kinds", Array())).has(kind)) {
			return configured;
		}
	}
	for (SolersPlugin *plugin : plugins) {
		const Dictionary profile = plugin->get_profile();
		if ((bool)profile.get("supports_generation", false) && Array(profile.get("kinds", Array())).has(kind)) {
			return plugin;
		}
	}
	return nullptr;
}

// --- Shared utilities ------------------------------------------------------

PackedByteArray SolersPlugin::utf8_bytes(const String &p_text) {
	CharString utf8 = p_text.utf8();
	PackedByteArray bytes;
	bytes.resize(utf8.length());
	if (utf8.length() > 0) {
		memcpy(bytes.ptrw(), utf8.get_data(), utf8.length());
	}
	return bytes;
}

Dictionary SolersPlugin::parse_json_body(const Dictionary &p_response) {
	const PackedByteArray body = p_response.get("body", PackedByteArray());
	const Variant parsed = JSON::parse_string(String::utf8((const char *)body.ptr(), body.size()));
	return parsed.get_type() == Variant::DICTIONARY ? (Dictionary)parsed : Dictionary();
}

Dictionary SolersPlugin::error_data(const String &p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	return error;
}

Dictionary SolersPlugin::ok_result(const Variant &p_data) {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersPlugin::error_result(const String &p_code, const String &p_message, bool p_recoverable) {
	Dictionary error = error_data(p_code, p_message);
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

String SolersPlugin::safe_slug(const String &p_text) {
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

String SolersPlugin::clean_base_url(const String &p_base_url) {
	String clean = p_base_url;
	while (clean.ends_with("/")) {
		clean = clean.substr(0, clean.length() - 1);
	}
	return clean;
}

void SolersPlugin::merge_options(Dictionary &r_body, const Dictionary &p_options) {
	for (const Variant *K = p_options.next(nullptr); K; K = p_options.next(K)) {
		r_body[*K] = p_options[*K];
	}
}

String SolersPlugin::first_manifest_field(const Dictionary &p_manifest, const Array &p_fields) {
	for (int i = 0; i < p_fields.size(); i++) {
		const String field = String(p_fields[i]);
		const String value = String(p_manifest.get(field, String())).strip_edges();
		if (!value.is_empty()) {
			return value;
		}
	}
	return String();
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

Dictionary SolersPlugin::http_request(const String &p_method, const String &p_url, const Vector<String> &p_headers, const PackedByteArray &p_body, uint64_t p_timeout_msec, int64_t p_max_body_bytes, const SafeFlag *p_cancel_requested, int p_retry_count) {
	String current_url = p_url;
	String redirected_from;
	for (int redirect_count = 0; redirect_count <= 3; redirect_count++) {
		if (p_cancel_requested && p_cancel_requested->is_set()) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = error_data("HTTP_CANCELLED", "HTTP request was cancelled.");
			return out;
		}
		String scheme;
		String host;
		int port = 0;
		String path;
		String fragment;
		Error parse_err = current_url.parse_url(scheme, host, port, path, fragment);
		if (parse_err != OK || host.is_empty()) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = error_data("INVALID_URL", "Invalid provider URL.");
			return out;
		}
		scheme = _solers_url_scheme_name(scheme);
		if (scheme != "http" && scheme != "https") {
			Dictionary out;
			out["ok"] = false;
			out["error"] = error_data("INVALID_URL", "Provider URL must use http or https.");
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
			out["error"] = error_data("NO_HTTP_CLIENT", "Failed to create HTTPClient.");
			return out;
		}
		Ref<TLSOptions> tls_options = tls ? TLSOptions::client() : Ref<TLSOptions>();
		Error err = http->connect_to_host(host, port, tls_options);
		if (err != OK) {
			Dictionary out;
			out["ok"] = false;
			out["error"] = error_data("CONNECT_FAILED", vformat("connect_to_host failed for %s:%d (error %d).", host, port, err));
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
			if (p_cancel_requested && p_cancel_requested->is_set()) {
				Dictionary out;
				out["ok"] = false;
				out["error"] = error_data("HTTP_CANCELLED", "HTTP request was cancelled.");
				return out;
			}
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
						out["error"] = error_data("REQUEST_FAILED", vformat("HTTP request failed (error %d).", err));
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

		if (p_retry_count < 2 && ((p_method == "GET" && (response_code == 429 || response_code == 503)) || (p_method == "POST" && response_code == 429))) {
			const int retry_seconds = _solers_header_value(response_headers, "Retry-After").to_int();
			if (retry_seconds > 0 && retry_seconds <= 60) {
				const uint64_t retry_at = OS::get_singleton()->get_ticks_msec() + (uint64_t)retry_seconds * 1000;
				while (OS::get_singleton()->get_ticks_msec() < retry_at) {
					if (p_cancel_requested && p_cancel_requested->is_set()) {
						Dictionary cancelled;
						cancelled["ok"] = false;
						cancelled["error"] = error_data("HTTP_CANCELLED", "HTTP retry was cancelled.");
						return cancelled;
					}
					OS::get_singleton()->delay_usec(50000);
				}
				return http_request(p_method, p_url, p_headers, p_body, p_timeout_msec, p_max_body_bytes, p_cancel_requested, p_retry_count + 1);
			}
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
	out["error"] = error_data("HTTP_REDIRECT_LIMIT", "HTTP redirect limit exceeded.");
	return out;
}

bool SolersPlugin::write_json_atomic(const String &p_path, const Dictionary &p_data, String &r_error) {
	MutexLock storage_lock(solers_plugin_manifest_mutex);
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
	file->flush();
	file.unref();
	if (DirAccess::rename_absolute(ProjectSettings::get_singleton()->globalize_path(tmp), ProjectSettings::get_singleton()->globalize_path(p_path)) != OK) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(tmp));
		r_error = "Failed to commit asset manifest.";
		return false;
	}
	return true;
}

Dictionary SolersPlugin::read_json_file(const String &p_path) {
	MutexLock storage_lock(solers_plugin_manifest_mutex);
	if (!FileAccess::exists(p_path)) {
		return Dictionary();
	}
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(FileAccess::get_file_as_string(p_path)) != OK || json->get_data().get_type() != Variant::DICTIONARY) {
		return Dictionary();
	}
	return json->get_data();
}

bool SolersPlugin::write_bytes_atomic(const String &p_path, const PackedByteArray &p_bytes, String &r_error) {
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

bool SolersPlugin::copy_file(const String &p_from, const String &p_to, String &r_error) {
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

bool SolersPlugin::remove_dir_recursive(const String &p_path, String &r_error) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null()) {
		r_error = vformat("Failed to open directory: %s", p_path);
		return false;
	}
	const PackedStringArray files = dir->get_files();
	for (int i = 0; i < files.size(); i++) {
		const String path = p_path.path_join(files[i]);
		const Error err = DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(path));
		if (err != OK) {
			r_error = vformat("Failed to remove file: %s", path);
			return false;
		}
	}
	const PackedStringArray dirs = dir->get_directories();
	for (int i = 0; i < dirs.size(); i++) {
		const String path = p_path.path_join(dirs[i]);
		if (!remove_dir_recursive(path, r_error)) {
			return false;
		}
	}
	const Error err = DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
	if (err != OK) {
		r_error = vformat("Failed to remove directory: %s", p_path);
		return false;
	}
	return true;
}

// --- Catalog ranking -------------------------------------------------------

static PackedStringArray _solers_catalog_terms(const String &p_query) {
	const PackedStringArray raw_terms = p_query.to_lower().split(" ", false);
	PackedStringArray terms;
	HashSet<String> seen;
	for (int i = 0; i < raw_terms.size(); i++) {
		const String term = raw_terms[i].strip_edges();
		if (!term.is_empty() && !seen.has(term)) {
			seen.insert(term);
			terms.push_back(term);
		}
	}
	return terms;
}

static bool _solers_catalog_array_contains(const Array &p_values, const String &p_term) {
	for (int i = 0; i < p_values.size(); i++) {
		if (String(p_values[i]).to_lower().contains(p_term)) {
			return true;
		}
	}
	return false;
}

static void _solers_catalog_add_field(Array &r_fields, const String &p_field) {
	if (!r_fields.has(p_field)) {
		r_fields.push_back(p_field);
	}
}

struct SolersCatalogRelevanceSort {
	bool operator()(const Dictionary &p_a, const Dictionary &p_b) const {
		const char *rank_fields[] = { "_rank_exact", "_rank_primary", "_rank_category", "_rank_description" };
		for (const char *field : rank_fields) {
			const int64_t a_rank = p_a.get(field, 0);
			const int64_t b_rank = p_b.get(field, 0);
			if (a_rank != b_rank) {
				return a_rank > b_rank;
			}
		}
		const int64_t a_downloads = p_a.get("download_count", 0);
		const int64_t b_downloads = p_b.get("download_count", 0);
		if (a_downloads != b_downloads) {
			return a_downloads > b_downloads;
		}
		const int64_t a_popularity = p_a.get("_popularity_rank", INT64_MAX);
		const int64_t b_popularity = p_b.get("_popularity_rank", INT64_MAX);
		if (a_popularity != b_popularity) {
			return a_popularity < b_popularity;
		}
		return String(p_a.get("asset_id", String())) < String(p_b.get("asset_id", String()));
	}
};

Array SolersPlugin::rank_catalog_assets(const Array &p_assets, const String &p_query) {
	const String normalized_query = p_query.strip_edges().to_lower();
	const PackedStringArray terms = _solers_catalog_terms(normalized_query);
	Vector<Dictionary> matches;
	for (int i = 0; i < p_assets.size(); i++) {
		Dictionary item = Dictionary(p_assets[i]).duplicate(true);
		const String asset_id = String(item.get("asset_id", String())).to_lower();
		const String display_name = String(item.get("display_name", String())).to_lower();
		const String description = String(item.get("description", String())).to_lower();
		const Array tags = item.get("tags", Array());
		const Array categories = item.get("categories", Array());
		Array matched_terms;
		Array matched_fields;
		int primary_hits = 0;
		int category_hits = 0;
		int description_hits = 0;
		for (int term_index = 0; term_index < terms.size(); term_index++) {
			const String term = terms[term_index];
			bool matched = false;
			if (asset_id.contains(term)) {
				matched = true;
				primary_hits++;
				_solers_catalog_add_field(matched_fields, "asset_id");
			}
			if (display_name.contains(term)) {
				matched = true;
				primary_hits++;
				_solers_catalog_add_field(matched_fields, "display_name");
			}
			if (_solers_catalog_array_contains(tags, term)) {
				matched = true;
				primary_hits++;
				_solers_catalog_add_field(matched_fields, "tags");
			}
			if (_solers_catalog_array_contains(categories, term)) {
				matched = true;
				category_hits++;
				_solers_catalog_add_field(matched_fields, "categories");
			}
			if (description.contains(term)) {
				matched = true;
				description_hits++;
				_solers_catalog_add_field(matched_fields, "description");
			}
			if (matched) {
				matched_terms.push_back(term);
			}
		}
		if (matched_terms.is_empty()) {
			continue;
		}
		item["matched_terms"] = matched_terms;
		item["matched_fields"] = matched_fields;
		item["_rank_exact"] = asset_id == normalized_query || display_name == normalized_query ? 1 : 0;
		item["_rank_primary"] = primary_hits;
		item["_rank_category"] = category_hits;
		item["_rank_description"] = description_hits;
		matches.push_back(item);
	}
	matches.sort_custom<SolersCatalogRelevanceSort>();
	Array ranked;
	for (int i = 0; i < matches.size(); i++) {
		Dictionary item = matches[i];
		item.erase("_rank_exact");
		item.erase("_rank_primary");
		item.erase("_rank_category");
		item.erase("_rank_description");
		item.erase("_popularity_rank");
		ranked.push_back(item);
	}
	return ranked;
}

String SolersPlugin::match_catalog_variant_id(const Array &p_variants, const String &p_id) {
	for (int i = 0; i < p_variants.size(); i++) {
		const String official_id = Dictionary(p_variants[i]).get("id", String());
		if (official_id.nocasecmp_to(p_id) == 0) {
			return official_id;
		}
	}
	return String();
}

// --- Import selection ------------------------------------------------------

Array SolersPlugin::native_resource_entrypoints(const Array &p_files) {
	Array entrypoints;
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	for (int i = 0; i < p_files.size(); i++) {
		const String path = p_files[i];
		if (importer && importer->get_importer_by_file(path).is_valid()) {
			continue;
		}
		if (ResourceLoader::get_resource_type(path) != StringName()) {
			entrypoints.push_back(path);
		}
	}
	return entrypoints;
}

Array SolersPlugin::model_resource_entrypoints(const Array &p_files) {
	Array entrypoints;
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	for (int i = 0; i < p_files.size(); i++) {
		const String path = p_files[i];
		const Ref<ResourceImporter> source_importer = importer ? importer->get_importer_by_file(path) : Ref<ResourceImporter>();
		const StringName type = source_importer.is_valid() ? source_importer->get_resource_type() : ResourceLoader::get_resource_type(path);
		if (type == SNAME("PackedScene") || (ClassDB::class_exists(type) && ClassDB::is_parent_class(type, SNAME("Mesh")))) {
			entrypoints.push_back(path);
		}
	}
	return entrypoints;
}

Dictionary SolersPlugin::project_import_selection(const Array &p_files, const Array &p_declared_import_files, const Array &p_declared_entrypoints) {
	HashMap<String, String> available;
	Array importable_files;
	Vector<String> discovered_entrypoints;
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	HashSet<String> seen_files;
	for (int i = 0; i < p_files.size(); i++) {
		const String path = String(p_files[i]).replace_char('\\', '/').simplify_path();
		if (path.is_empty() || seen_files.has(path)) {
			Dictionary selection;
			selection["valid"] = false;
			selection["error"] = vformat("Asset files contains an empty or duplicate path: %s", path);
			return selection;
		}
		seen_files.insert(path);
		available[path] = String(p_files[i]);
		const Ref<ResourceImporter> source_importer = importer ? importer->get_importer_by_file(path) : Ref<ResourceImporter>();
		const StringName type = source_importer.is_valid() ? source_importer->get_resource_type() : ResourceLoader::get_resource_type(path);
		if (source_importer.is_valid() || type != StringName()) {
			importable_files.push_back(p_files[i]);
		}
		if (type != StringName() && ClassDB::class_exists(type) && ClassDB::is_parent_class(type, SNAME("Material"))) {
			discovered_entrypoints.push_back(path);
		}
	}

	if (!p_declared_import_files.is_empty()) {
		if (p_declared_entrypoints.is_empty()) {
			Dictionary selection;
			selection["valid"] = false;
			selection["error"] = "Declared import_files requires at least one entrypoint.";
			return selection;
		}
		HashSet<String> selected;
		for (int i = 0; i < p_declared_import_files.size(); i++) {
			const String path = String(p_declared_import_files[i]).replace_char('\\', '/').simplify_path();
			if (!available.has(path) || selected.has(path)) {
				Dictionary selection;
				selection["valid"] = false;
				selection["error"] = vformat("Declared import file is missing or duplicated: %s", path);
				return selection;
			}
			selected.insert(path);
		}
		Array entrypoints;
		HashSet<String> declared_entrypoints;
		for (int i = 0; i < p_declared_entrypoints.size(); i++) {
			const String path = String(p_declared_entrypoints[i]).replace_char('\\', '/').simplify_path();
			if (!selected.has(path) || declared_entrypoints.has(path)) {
				Dictionary selection;
				selection["valid"] = false;
				selection["error"] = vformat("Declared entrypoint is not a unique import file: %s", path);
				return selection;
			}
			declared_entrypoints.insert(path);
			entrypoints.push_back(available[path]);
		}
		Array import_files;
		for (int i = 0; i < p_files.size(); i++) {
			const String path = String(p_files[i]).replace_char('\\', '/').simplify_path();
			if (selected.has(path)) {
				import_files.push_back(p_files[i]);
			}
		}
		Dictionary selection;
		selection["valid"] = true;
		selection["files"] = import_files;
		selection["entrypoints"] = entrypoints;
		return selection;
	}

	Vector<String> pending;
	Array entrypoints;
	if (!p_declared_entrypoints.is_empty()) {
		HashSet<String> declared_paths;
		for (int i = 0; i < p_declared_entrypoints.size(); i++) {
			const String path = String(p_declared_entrypoints[i]).replace_char('\\', '/').simplify_path();
			if (!available.has(path) || declared_paths.has(path)) {
				Dictionary selection;
				selection["valid"] = false;
				selection["error"] = vformat("Declared asset entrypoint is missing or duplicated: %s", path);
				return selection;
			}
			declared_paths.insert(path);
			entrypoints.push_back(available[path]);
			pending.push_back(path);
		}
	} else {
		pending = discovered_entrypoints;
		for (int i = 0; i < pending.size(); i++) {
			entrypoints.push_back(available[pending[i]]);
		}
	}
	if (entrypoints.is_empty()) {
		Dictionary selection;
		selection["valid"] = true;
		selection["files"] = importable_files;
		selection["entrypoints"] = importable_files;
		return selection;
	}

	HashSet<String> selected;
	for (int i = 0; i < pending.size(); i++) {
		const String owner = pending[i];
		if (selected.has(owner)) {
			continue;
		}
		selected.insert(owner);
		List<String> dependencies;
		ResourceLoader::get_dependencies(owner, &dependencies, true);
		for (const String &spec : dependencies) {
			String dependency = spec.get_slice("::", 0);
			if (dependency.begins_with("uid://") && spec.get_slice_count("::") >= 3) {
				dependency = spec.get_slice("::", 2);
			}
			if (dependency.is_relative_path()) {
				dependency = owner.get_base_dir().path_join(dependency);
			}
			dependency = dependency.replace_char('\\', '/').simplify_path();
			if (!available.has(dependency)) {
				Dictionary selection;
				selection["valid"] = false;
				selection["error"] = vformat("Asset entrypoint dependency is missing from files: %s", dependency);
				return selection;
			}
			pending.push_back(dependency);
		}
	}

	Array import_files;
	for (int i = 0; i < p_files.size(); i++) {
		const String path = String(p_files[i]).replace_char('\\', '/').simplify_path();
		if (selected.has(path)) {
			import_files.push_back(p_files[i]);
		}
	}
	Dictionary selection;
	selection["valid"] = true;
	selection["files"] = import_files;
	selection["entrypoints"] = entrypoints;
	return selection;
}
