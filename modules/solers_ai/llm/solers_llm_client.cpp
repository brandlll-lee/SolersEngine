/**************************************************************************/
/*  solers_llm_client.cpp                                                 */
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

#include "solers_llm_client.h"

#include "core/crypto/crypto.h"
#include "core/io/json.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

void SolersLLMClient::_reset() {
	http = Ref<HTTPClient>();
	active_protocol = nullptr;
	request_sent = false;
	response_checked = false;
	capturing_error = false;
	sse_buffer = String();
	error_buffer = String();
	stream_state = Dictionary();
}

void SolersLLMClient::_fail(const String &p_code, const String &p_message, Array &r_events) {
	last_error.clear();
	last_error["code"] = p_code;
	last_error["message"] = p_message;
	r_events.push_back(SolersLLMEvent::error(p_code, p_message));
	state = STATE_FAILED;
	if (http.is_valid()) {
		http->close();
	}
}

Error SolersLLMClient::begin(const Dictionary &p_request, const Dictionary &p_profile, const String &p_api_key) {
	_reset();
	last_error.clear();

	if (!protocol_registry) {
		last_error["code"] = "NO_PROTOCOL_REGISTRY";
		last_error["message"] = "SolersLLMClient has no protocol registry.";
		state = STATE_FAILED;
		return ERR_UNCONFIGURED;
	}

	const StringName protocol_id = StringName(p_profile.get("protocol", String()));
	active_protocol = protocol_registry->get(protocol_id);
	if (!active_protocol) {
		last_error["code"] = "UNKNOWN_PROTOCOL";
		last_error["message"] = vformat("No LLM protocol registered for id '%s'.", String(protocol_id));
		state = STATE_FAILED;
		return ERR_UNAVAILABLE;
	}

	// Resolve endpoint: scheme://host[:port]/path_prefix + protocol default path.
	String base_url = String(p_profile.get("base_url", String())).strip_edges();
	if (base_url.is_empty()) {
		last_error["code"] = "NO_BASE_URL";
		last_error["message"] = "Provider profile has no base URL configured.";
		state = STATE_FAILED;
		return ERR_UNCONFIGURED;
	}

	use_tls = true;
	port = 443;
	if (base_url.begins_with("https://")) {
		base_url = base_url.substr(8);
		use_tls = true;
		port = 443;
	} else if (base_url.begins_with("http://")) {
		base_url = base_url.substr(7);
		use_tls = false;
		port = 80;
	}
	const int slash = base_url.find("/");
	String authority = slash >= 0 ? base_url.substr(0, slash) : base_url;
	String path_prefix = slash >= 0 ? base_url.substr(slash) : String();
	path_prefix = path_prefix.trim_suffix("/");
	const int colon = authority.find(":");
	if (colon >= 0) {
		host = authority.substr(0, colon);
		port = authority.substr(colon + 1).to_int();
	} else {
		host = authority;
	}
	request_path = path_prefix + active_protocol->get_default_path();

	// Build body via the protocol, serialize to JSON.
	const Dictionary body = active_protocol->build_request_body(p_request);
	request_body = JSON::stringify(body, "", false, true);

	// Build headers: content/accept, auth (data-driven from profile), and any
	// protocol-mandated headers.
	request_headers.clear();
	request_headers.push_back("Content-Type: application/json");
	request_headers.push_back("Accept: text/event-stream");
	const String api_key = p_api_key.strip_edges();
	if (!api_key.is_empty()) {
		const String auth_header = p_profile.get("auth_header", "Authorization");
		const String auth_prefix = p_profile.get("auth_prefix", "Bearer ");
		request_headers.push_back(auth_header + ": " + auth_prefix + api_key);
	}
	Dictionary extra_headers;
	active_protocol->augment_headers(extra_headers, p_request);
	for (const Variant *k = extra_headers.next(nullptr); k; k = extra_headers.next(k)) {
		request_headers.push_back(String(*k) + ": " + String(extra_headers[*k]));
	}

	http = HTTPClient::create();
	if (http.is_null()) {
		last_error["code"] = "NO_HTTP_CLIENT";
		last_error["message"] = "Failed to create HTTPClient.";
		state = STATE_FAILED;
		return ERR_CANT_CREATE;
	}
	Ref<TLSOptions> tls = use_tls ? TLSOptions::client() : Ref<TLSOptions>();
	const Error err = http->connect_to_host(host, port, tls);
	if (err != OK) {
		last_error["code"] = "CONNECT_FAILED";
		last_error["message"] = vformat("connect_to_host failed for %s:%d (error %d).", host, port, err);
		state = STATE_FAILED;
		return err;
	}
	stream_state = active_protocol->begin_stream(p_request);
	state = STATE_CONNECTING;
	return OK;
}

void SolersLLMClient::_drain_records(Array &r_events) {
	sse_buffer = sse_buffer.replace("\r\n", "\n");
	int sep = sse_buffer.find("\n\n");
	while (sep >= 0) {
		const String record = sse_buffer.substr(0, sep);
		sse_buffer = sse_buffer.substr(sep + 2);

		String event_name;
		String data;
		const Vector<String> lines = record.split("\n");
		for (int i = 0; i < lines.size(); i++) {
			const String line = lines[i];
			if (line.begins_with("event:")) {
				event_name = line.substr(6).strip_edges();
			} else if (line.begins_with("data:")) {
				String d = line.substr(5);
				if (d.begins_with(" ")) {
					d = d.substr(1);
				}
				data = data.is_empty() ? d : data + "\n" + d;
			}
		}
		if (!data.is_empty() && active_protocol) {
			const Array produced = active_protocol->parse_event(stream_state, event_name, data);
			for (int i = 0; i < produced.size(); i++) {
				r_events.push_back(produced[i]);
			}
		}
		sep = sse_buffer.find("\n\n");
	}
}

Array SolersLLMClient::poll() {
	Array events;
	if (state == STATE_IDLE || state == STATE_DONE || state == STATE_FAILED || http.is_null()) {
		return events;
	}

	http->poll();
	const HTTPClient::Status st = http->get_status();

	switch (st) {
		case HTTPClient::STATUS_RESOLVING:
		case HTTPClient::STATUS_CONNECTING: {
			state = STATE_CONNECTING;
		} break;

		case HTTPClient::STATUS_CANT_RESOLVE: {
			_fail("CANT_RESOLVE", vformat("Could not resolve host '%s'.", host), events);
		} break;
		case HTTPClient::STATUS_CANT_CONNECT: {
			_fail("CANT_CONNECT", vformat("Could not connect to '%s:%d'.", host, port), events);
		} break;
		case HTTPClient::STATUS_CONNECTION_ERROR: {
			_fail("CONNECTION_ERROR", "HTTP connection error.", events);
		} break;
		case HTTPClient::STATUS_TLS_HANDSHAKE_ERROR: {
			_fail("TLS_ERROR", "TLS handshake failed.", events);
		} break;

		case HTTPClient::STATUS_CONNECTED: {
			if (!request_sent) {
				const CharString body_utf8 = request_body.utf8();
				const Error err = http->request(HTTPClient::METHOD_POST, request_path, request_headers, (const uint8_t *)body_utf8.get_data(), body_utf8.length());
				request_sent = true;
				if (err != OK) {
					_fail("REQUEST_FAILED", vformat("HTTP request failed (error %d).", err), events);
				} else {
					state = STATE_REQUESTING;
				}
			} else if (response_checked) {
				// Body fully consumed; connection returned to keep-alive idle.
				if (capturing_error) {
					_fail("HTTP_ERROR", error_buffer.is_empty() ? "Provider returned an error." : error_buffer, events);
				} else {
					_drain_records(events);
					state = STATE_DONE;
				}
			}
		} break;

		case HTTPClient::STATUS_REQUESTING: {
			state = STATE_REQUESTING;
		} break;

		case HTTPClient::STATUS_BODY: {
			if (!response_checked) {
				response_checked = true;
				const int code = http->get_response_code();
				if (code < 200 || code >= 300) {
					capturing_error = true;
					last_error["http_status"] = code;
				} else {
					state = STATE_STREAMING;
				}
			}
			const PackedByteArray chunk = http->read_response_body_chunk();
			if (chunk.size() > 0) {
				const String s = String::utf8((const char *)chunk.ptr(), chunk.size());
				if (capturing_error) {
					error_buffer += s;
				} else {
					sse_buffer += s;
					_drain_records(events);
				}
			}
		} break;

		case HTTPClient::STATUS_DISCONNECTED: {
			if (capturing_error) {
				_fail("HTTP_ERROR", error_buffer.is_empty() ? "Provider returned an error." : error_buffer, events);
			} else {
				_drain_records(events);
				state = STATE_DONE;
			}
		} break;

		default: {
		} break;
	}

	return events;
}

void SolersLLMClient::abort() {
	if (http.is_valid()) {
		http->close();
	}
	_reset();
	state = STATE_IDLE;
}
