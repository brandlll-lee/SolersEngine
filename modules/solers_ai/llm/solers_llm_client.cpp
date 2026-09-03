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

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/templates/hash_set.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_provider_auth.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

// DNS, connect, headers, and streamed chunks share one no-progress budget.
// HTTP state transitions and body chunks reset it; the session owns retries.
static constexpr uint64_t SOLERS_LLM_NO_PROGRESS_TIMEOUT_MSEC = 120000;

Dictionary SolersLLMClient::_redacted_request_body(const String &p_body) const {
	Dictionary out;
	const Variant parsed = JSON::parse_string(p_body);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return out;
	}
	const Dictionary body = parsed;
	out["model"] = body.get("model", String());
	out["stream"] = body.get("stream", false);
	out["store"] = body.get("store", Variant());
	if (body.has("max_tokens")) {
		out["max_tokens"] = body["max_tokens"];
	}
	if (body.has("max_output_tokens")) {
		out["max_output_tokens"] = body["max_output_tokens"];
	}
	if (body.has("reasoning_effort")) {
		out["reasoning_effort"] = body["reasoning_effort"];
	}
	const Array tools = body.get("tools", Array());
	out["tool_count"] = tools.size();
	Array messages;
	const Array native_messages = body.get("messages", Array());
	for (int i = 0; i < native_messages.size(); i++) {
		const Dictionary item = native_messages[i];
		Dictionary redacted;
		redacted["role"] = item.get("role", String());
		if (item.has("tool_call_id")) {
			redacted["tool_call_id"] = item.get("tool_call_id", String());
		}
		const Array tool_calls = item.get("tool_calls", Array());
		if (!tool_calls.is_empty()) {
			Array redacted_calls;
			for (int c = 0; c < tool_calls.size(); c++) {
				const Dictionary call = tool_calls[c];
				const Dictionary fn = call.get("function", Dictionary());
				Dictionary redacted_call;
				redacted_call["id"] = call.get("id", String());
				redacted_call["name"] = fn.get("name", String());
				redacted_calls.push_back(redacted_call);
			}
			redacted["tool_calls"] = redacted_calls;
		}
		messages.push_back(redacted);
	}
	out["messages"] = messages;
	return out;
}

void SolersLLMClient::_trace(const String &p_event, const Dictionary &p_payload) const {
	if (trace_path.is_empty()) {
		return;
	}
	DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(trace_path.get_base_dir()));
	Ref<FileAccess> file = FileAccess::open(trace_path, FileAccess::READ_WRITE);
	if (file.is_null()) {
		file = FileAccess::open(trace_path, FileAccess::WRITE);
	}
	if (file.is_null()) {
		return;
	}
	file->seek_end();
	Dictionary entry;
	entry["ts"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	entry["event"] = p_event;
	entry["payload"] = p_payload;
	file->store_line(JSON::stringify(entry, "", false, true));
}

void SolersLLMClient::_fail(const String &p_code, const String &p_message, int p_retryable, const String &p_failure_kind, const Dictionary &p_details) {
	// Preserve any HTTP status / response headers captured before the failure so
	// the retry layer can classify (5xx vs 4xx) and honor Retry-After headers.
	const Variant http_status = last_error.get("http_status", Variant());
	const Variant headers = last_error.get("headers", Variant());
	last_error.clear();
	last_error["code"] = p_code;
	last_error["message"] = p_message;
	const int status = http_status.get_type() == Variant::NIL ? 0 : (int)http_status;
	last_error["retryable"] = p_retryable >= 0 ? p_retryable != 0 : status == 429 || status >= 500;
	if (!p_failure_kind.is_empty() || status == 413) {
		last_error["failure_kind"] = status == 413 ? "context_overflow" : p_failure_kind;
	}
	if (http_status.get_type() != Variant::NIL) {
		last_error["http_status"] = http_status;
	}
	if (headers.get_type() != Variant::NIL) {
		last_error["headers"] = headers;
	}
	if (!p_details.is_empty()) {
		last_error["details"] = p_details;
	}
	Dictionary payload;
	payload["code"] = p_code;
	payload["retryable"] = last_error["retryable"];
	payload["response_bytes"] = response_bytes;
	if (!p_details.is_empty()) {
		payload["details"] = p_details;
	}
	_trace("fail", payload);
	state = STATE_FAILED;
	if (http.is_valid()) {
		http->close();
	}
}

void SolersLLMClient::_fail_provider_response() {
	const Array events = active_protocol ? active_protocol->parse_event(stream_state, "error", error_buffer) : Array();
	for (const Variant &item : events) {
		const Dictionary event = item;
		if (String(event.get("kind", String())) == SolersLLMEventKind::ERROR) {
			_fail(event.get("code", "HTTP_ERROR"), event.get("message", "Provider returned an error."), event.has("retryable") ? ((bool)event["retryable"] ? 1 : 0) : -1, event.get("failure_kind", String()), event.get("details", Dictionary()));
			return;
		}
	}
	_fail("HTTP_ERROR", error_buffer.is_empty() ? "Provider returned an error." : error_buffer);
}

void SolersLLMClient::_set_request_header(const String &p_name, const String &p_value) {
	for (int i = request_headers.size() - 1; i >= 0; i--) {
		if (request_headers[i].get_slice(":", 0).nocasecmp_to(p_name) == 0) {
			request_headers.remove_at(i);
		}
	}
	request_headers.push_back(p_name + ": " + p_value);
}

bool SolersLLMClient::_configure_endpoint(const Dictionary &p_profile) {
	String base_url = String(p_profile.get("base_url", String())).strip_edges();
	if (base_url.is_empty()) {
		_fail("NO_BASE_URL", "Provider profile has no base URL configured.");
		return false;
	}
	use_tls = base_url.begins_with("https://");
	port = use_tls ? 443 : 80;
	base_url = base_url.trim_prefix(use_tls ? "https://" : "http://");
	const int slash = base_url.find("/");
	const String authority = slash >= 0 ? base_url.substr(0, slash) : base_url;
	const String path_prefix = (slash >= 0 ? base_url.substr(slash) : String()).trim_suffix("/");
	const int colon = authority.find(":");
	host = colon >= 0 ? authority.substr(0, colon) : authority;
	if (colon >= 0) {
		port = authority.substr(colon + 1).to_int();
	}
	request_path = path_prefix + active_protocol->get_default_path();
	return true;
}

bool SolersLLMClient::_prepare_auth_headers(bool p_force_oauth_refresh) {
	request_headers.clear();
	_set_request_header("Content-Type", "application/json");
	_set_request_header("Accept", "text/event-stream");
	const Dictionary profile_headers = worker_profile.get("headers", Dictionary());
	for (const Variant *key = profile_headers.next(nullptr); key; key = profile_headers.next(key)) {
		_set_request_header(String(*key), String(profile_headers[*key]));
	}
	Dictionary protocol_headers;
	active_protocol->augment_headers(protocol_headers, worker_request);
	for (const Variant *key = protocol_headers.next(nullptr); key; key = protocol_headers.next(key)) {
		_set_request_header(String(*key), String(protocol_headers[*key]));
	}

	const String auth_type = worker_auth.get("type", String(worker_profile.get("auth_type", "api_key")));
	if (auth_type == "none") {
		return _configure_endpoint(worker_profile);
	}
	if (auth_type == "oauth") {
		if (!worker_auth_method) {
			_fail("OAUTH_METHOD_UNAVAILABLE", "The stored credential references an unavailable authorization method.");
			return false;
		}
		const Dictionary result = worker_auth_method->prepare_request(worker_auth, worker_profile, worker_request, p_force_oauth_refresh);
		if (!result.get("ok", false)) {
			const Dictionary error = result.get("error", Dictionary());
			_fail(error.get("code", "OAUTH_PREPARE_FAILED"), error.get("message", "Authorization could not prepare the request."));
			return false;
		}
		const Dictionary next_auth = result.get("credential", worker_auth);
		if (next_auth != worker_auth) {
			worker_auth = next_auth;
			MutexLock lock(mutex);
			shared_auth_update = worker_auth.duplicate(true);
		}
		worker_profile = result.get("profile", worker_profile);
		if (!_configure_endpoint(worker_profile)) {
			return false;
		}
		const Dictionary auth_headers = result.get("headers", Dictionary());
		for (const Variant *key = auth_headers.next(nullptr); key; key = auth_headers.next(key)) {
			_set_request_header(String(*key), String(auth_headers[*key]));
		}
		return true;
	}

	const String key = worker_auth.get("key", String());
	if (!key.is_empty()) {
		const String auth_header = worker_profile.get("auth_header", "Authorization");
		const String auth_prefix = worker_profile.get("auth_prefix", "Bearer ");
		_set_request_header(auth_header, auth_prefix + key);
	}
	return _configure_endpoint(worker_profile);
}

Error SolersLLMClient::begin(const Dictionary &p_request, const Dictionary &p_profile, const Dictionary &p_auth, SolersProviderAuth *p_auth_method) {
	// Join any prior worker before reconfiguring shared state.
	abort_requested.set();
	_join_worker();
	abort_requested.clear();

	{
		MutexLock lock(mutex);
		shared_events = Array();
		shared_error = Dictionary();
		shared_auth_update = Dictionary();
		shared_request_body_bytes = 0;
		shared_state = STATE_IDLE;
	}
	last_error.clear();

	if (!protocol_registry) {
		last_error["code"] = "NO_PROTOCOL_REGISTRY";
		last_error["message"] = "SolersLLMClient has no protocol registry.";
		_publish(Array(), STATE_FAILED);
		return ERR_UNCONFIGURED;
	}

	const StringName protocol_id = StringName(p_profile.get("protocol", String()));
	active_protocol = protocol_registry->get(protocol_id);
	if (!active_protocol) {
		last_error["code"] = "UNKNOWN_PROTOCOL";
		last_error["message"] = vformat("No LLM protocol registered for id '%s'.", String(protocol_id));
		_publish(Array(), STATE_FAILED);
		return ERR_UNAVAILABLE;
	}

	worker_request = p_request;
	worker_protocol_id = protocol_id;
	request_body = String();
	trace_path = solers_session_dir().path_join("provider_trace.jsonl");

	request_headers.clear();
	request_headers.push_back("Content-Type: application/json");
	request_headers.push_back("Accept: text/event-stream");
	worker_profile = p_profile.duplicate(true);
	worker_auth = p_auth.duplicate(true);
	worker_auth_method = p_auth_method;

	// Reset worker-owned transient state for the fresh request.
	http = Ref<HTTPClient>();
	request_sent = false;
	response_checked = false;
	capturing_error = false;
	oauth_401_retried = false;
	sse_buffer = String();
	error_buffer = String();
	initial_stream_state = Dictionary();
	stream_state = Dictionary();
	state = STATE_CONNECTING;

	{
		MutexLock lock(mutex);
		shared_state = STATE_CONNECTING;
	}

	thread.start(&SolersLLMClient::_thread_func, this);
	thread_active = true;
	return OK;
}

void SolersLLMClient::_thread_func(void *p_userdata) {
	SolersLLMClient *client = static_cast<SolersLLMClient *>(p_userdata);
	client->_run_worker();
}

void SolersLLMClient::_publish(const Array &p_events, State p_state) {
	MutexLock lock(mutex);
	for (int i = 0; i < p_events.size(); i++) {
		shared_events.push_back(p_events[i]);
	}
	shared_state = p_state;
	if (p_state == STATE_FAILED && !last_error.is_empty()) {
		shared_error = last_error.duplicate(true);
	}
}

void SolersLLMClient::_run_worker() {
	Array batch;
	if (!_prepare_auth_headers()) {
		_publish(batch, state);
		return;
	}
	Array messages = SolersContextManager::repair_tool_pairing(worker_request.get("messages", Array()));
	worker_request["messages"] = SolersLLMMessage::project_attachments(messages);
	request_body = JSON::stringify(active_protocol->build_request_body(worker_request), "", false, true);
	initial_stream_state = active_protocol->begin_stream(worker_request);
	Dictionary trace_payload;
	trace_payload["host"] = host;
	trace_payload["path"] = request_path;
	trace_payload["protocol"] = String(worker_protocol_id);
	trace_payload["body_bytes"] = request_body.utf8().length();
	trace_payload["body"] = _redacted_request_body(request_body);
	_trace("request_prepared", trace_payload);
	{
		MutexLock lock(mutex);
		shared_request_body_bytes = request_body.utf8().length();
	}

	http = HTTPClient::create();
	if (http.is_null()) {
		_fail("NO_HTTP_CLIENT", "Failed to create HTTPClient.");
		_publish(batch, state);
		return;
	}
	Ref<TLSOptions> tls = use_tls ? TLSOptions::client() : Ref<TLSOptions>();
	const Error err = http->connect_to_host(host, port, tls);
	if (err != OK) {
		const String connect_message = vformat("connect_to_host failed for %s:%d (error %d).", host, port, err);
		_fail("CONNECT_FAILED", connect_message, true);
		_publish(batch, state);
		http = Ref<HTTPClient>();
		return;
	}

	stream_state = initial_stream_state;
	state = STATE_CONNECTING;
	last_progress_msec = OS::get_singleton()->get_ticks_msec();
	prev_state = state;
	stream_text_delta_count = 0;
	stream_text_bytes = 0;
	stream_saw_finish = false;
	stream_has_content = false;
	response_bytes = 0;
	response_prefix = String();

	while (!abort_requested.is_set()) {
		batch.clear();

		http->poll();
		const HTTPClient::Status st = http->get_status();
		switch (st) {
			case HTTPClient::STATUS_RESOLVING:
			case HTTPClient::STATUS_CONNECTING: {
				state = STATE_CONNECTING;
			} break;
			case HTTPClient::STATUS_CANT_RESOLVE: {
				_fail("CANT_RESOLVE", vformat("Could not resolve host '%s'.", host), true);
			} break;
			case HTTPClient::STATUS_CANT_CONNECT: {
				_fail("CANT_CONNECT", vformat("Could not connect to '%s:%d'.", host, port), true);
			} break;
			case HTTPClient::STATUS_CONNECTION_ERROR: {
				_fail("CONNECTION_ERROR", "HTTP connection error.", true);
			} break;
			case HTTPClient::STATUS_TLS_HANDSHAKE_ERROR: {
				_fail("TLS_ERROR", "TLS handshake failed.", true);
			} break;
			case HTTPClient::STATUS_CONNECTED: {
				if (!request_sent) {
					const CharString body_utf8 = request_body.utf8();
					const Error request_err = http->request(HTTPClient::METHOD_POST, request_path, request_headers, (const uint8_t *)body_utf8.get_data(), body_utf8.length());
					request_sent = true;
					if (request_err != OK) {
						_fail("REQUEST_FAILED", vformat("HTTP request failed (error %d).", request_err), true);
					} else {
						state = STATE_REQUESTING;
					}
				} else if (response_checked) {
					if (capturing_error) {
						_fail_provider_response();
					} else {
						_complete_stream(batch);
					}
				}
			} break;
			case HTTPClient::STATUS_REQUESTING: {
				state = STATE_REQUESTING;
			} break;
			case HTTPClient::STATUS_BODY: {
				if (!response_checked) {
					const int code = http->get_response_code();
					List<String> header_list;
					http->get_response_headers(&header_list);
					Dictionary headers_dict;
					for (const String &header : header_list) {
						const int colon = header.find(":");
						if (colon > 0) {
							const String name = header.substr(0, colon).strip_edges();
							headers_dict[name] = header.substr(colon + 1).strip_edges();
							if (name.nocasecmp_to("Content-Type") == 0) {
								last_error["response_content_type"] = headers_dict[name];
							}
						}
					}
					const bool refreshable_oauth = String(worker_auth.get("type", String())) == "oauth" && worker_auth_method;
					if (code == 401 && refreshable_oauth && !oauth_401_retried) {
						oauth_401_retried = true;
						http->close();
						if (!_prepare_auth_headers(true)) {
							break;
						}
						http = HTTPClient::create();
						const Ref<TLSOptions> retry_tls = use_tls ? TLSOptions::client() : Ref<TLSOptions>();
						const Error retry_error = http.is_valid() ? http->connect_to_host(host, port, retry_tls) : ERR_CANT_CREATE;
						if (retry_error != OK) {
							_fail("OAUTH_RETRY_CONNECT_FAILED", "Could not reconnect after refreshing authorization.", true);
						} else {
							request_sent = false;
							response_checked = false;
							capturing_error = false;
							error_buffer = String();
							sse_buffer = String();
							response_bytes = 0;
							response_prefix = String();
							stream_state = initial_stream_state;
							last_error.clear();
							state = STATE_CONNECTING;
							_trace("oauth_401_retry");
						}
						break;
					}
					response_checked = true;
					if (code < 200 || code >= 300) {
						capturing_error = true;
						last_error["http_status"] = code;
						last_error["headers"] = headers_dict;
						Dictionary payload;
						payload["status"] = code;
						_trace("http_error_status", payload);
					} else {
						state = STATE_STREAMING;
						Dictionary payload;
						payload["status"] = code;
						_trace("http_status", payload);
					}
				}
				const PackedByteArray chunk = http->read_response_body_chunk();
				if (chunk.size() > 0) {
					last_progress_msec = OS::get_singleton()->get_ticks_msec();
					response_bytes += chunk.size();
					const String s = String::utf8((const char *)chunk.ptr(), chunk.size());
					if (response_prefix.length() < 512) {
						response_prefix += s;
						if (response_prefix.length() > 512) {
							response_prefix = response_prefix.substr(0, 512);
						}
					}
					if (capturing_error) {
						error_buffer += s;
					} else {
						sse_buffer += s;
						_drain_records(batch);
					}
				}
			} break;
			case HTTPClient::STATUS_DISCONNECTED: {
				if (capturing_error) {
					_fail_provider_response();
				} else {
					_complete_stream(batch);
				}
			} break;
			default: {
			} break;
		}

		if (state != prev_state) {
			last_progress_msec = OS::get_singleton()->get_ticks_msec();
			prev_state = state;
		}
		if (state != STATE_DONE && state != STATE_FAILED && OS::get_singleton()->get_ticks_msec() - last_progress_msec > SOLERS_LLM_NO_PROGRESS_TIMEOUT_MSEC) {
			_fail("NO_PROGRESS_TIMEOUT", vformat("The model provider made no progress for %d seconds. Reconnecting.", (int)(SOLERS_LLM_NO_PROGRESS_TIMEOUT_MSEC / 1000)), true);
		}

		_publish(batch, state);

		if (state == STATE_DONE || state == STATE_FAILED) {
			break;
		}
		OS::get_singleton()->delay_usec(5000);
	}

	if (http.is_valid()) {
		http->close();
	}
	http = Ref<HTTPClient>();
}

int64_t SolersLLMClient::get_request_body_bytes() const {
	MutexLock lock(mutex);
	return shared_request_body_bytes;
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
				const Dictionary event = produced[i];
				const String kind = event.get("kind", String());
				if (kind == SolersLLMEventKind::TEXT_DELTA) {
					stream_text_delta_count++;
					stream_text_bytes += String(event.get("text", String())).length();
					stream_has_content = true;
				} else if (kind == SolersLLMEventKind::REASONING_DELTA) {
					stream_has_content = true;
				} else if (kind == SolersLLMEventKind::TOOL_CALL || kind == SolersLLMEventKind::TOOL_INPUT_START || kind == SolersLLMEventKind::TOOL_INPUT_DELTA) {
					stream_has_content = true;
					if (kind == SolersLLMEventKind::TOOL_CALL) {
						Dictionary payload;
						payload["id"] = event.get("id", String());
						payload["name"] = event.get("name", String());
						_trace("stream_tool_call", payload);
					}
				} else if (kind == SolersLLMEventKind::FINISH) {
					stream_saw_finish = true;
					Dictionary payload;
					payload["stop_reason"] = event.get("stop_reason", String());
					payload["text_delta_count"] = stream_text_delta_count;
					payload["text_bytes"] = stream_text_bytes;
					_trace("stream_finish", payload);
				} else if (kind == SolersLLMEventKind::ERROR) {
					const int retryable = event.has("retryable") ? ((bool)event["retryable"] ? 1 : 0) : (stream_has_content && !stream_saw_finish ? 1 : 0);
					_fail(String(event.get("code", "PROVIDER_STREAM_ERROR")), String(event.get("message", "Provider stream reported an error.")), retryable, event.get("failure_kind", String()), event.get("details", Dictionary()));
					last_error["response_bytes"] = response_bytes;
				}
				r_events.push_back(produced[i]);
			}
		}
		sep = sse_buffer.find("\n\n");
	}
}

void SolersLLMClient::_complete_stream(Array &r_batch) {
	// Providers often omit the trailing blank line on the final SSE record.
	sse_buffer = sse_buffer.replace("\r\n", "\n");
	if (!sse_buffer.is_empty() && !sse_buffer.ends_with("\n\n")) {
		sse_buffer += sse_buffer.ends_with("\n") ? "\n" : "\n\n";
	}
	_drain_records(r_batch);
	if (state == STATE_FAILED) {
		return;
	}

	if (stream_saw_finish) {
		state = STATE_DONE;
		return;
	}
	if (stream_has_content) {
		_fail("STREAM_INTERRUPTED", "Provider stream ended before its terminal protocol event.", true);
		return;
	}

	// HTTP 200 with a non-SSE JSON error body: surface the message, never retry.
	const String raw = response_prefix.strip_edges();
	const bool looks_json = raw.begins_with("{") && !raw.begins_with("data:") && !raw.contains("\ndata:");
	if (looks_json) {
		String code = "PROVIDER_ERROR";
		String message = "Provider returned an error body without a stream.";
		const Variant parsed = JSON::parse_string(raw);
		if (parsed.get_type() == Variant::DICTIONARY) {
			const Dictionary obj = parsed;
			const Dictionary err = obj.get("error", Dictionary());
			code = err.get("code", obj.get("code", code));
			if (!String(err.get("message", String())).is_empty()) {
				message = err.get("message", String());
			} else if (!String(obj.get("message", String())).is_empty()) {
				message = obj.get("message", String());
			}
		}
		_fail(code, message, false, code == "context_length_exceeded" ? "context_overflow" : String());
		last_error["response_bytes"] = response_bytes;
		return;
	}

	const String media_type = String(last_error.get("response_content_type", String())).get_slice(";", 0).strip_edges().to_lower();
	if (!media_type.is_empty() && media_type != "text/event-stream" && media_type != "application/json" && !media_type.ends_with("+json")) {
		_fail("UNEXPECTED_RESPONSE_MEDIA_TYPE", vformat("Provider returned '%s' instead of a streaming protocol response.", media_type), false);
	} else {
		_fail("STREAM_ENDED_WITHOUT_FINISH", "Provider stream ended without a terminal protocol event.", true);
	}
	last_error["response_bytes"] = response_bytes;
}

Array SolersLLMClient::poll() {
	MutexLock lock(mutex);
	Array out = shared_events;
	shared_events = Array();
	return out;
}

SolersLLMClient::State SolersLLMClient::get_state() const {
	MutexLock lock(mutex);
	return shared_state;
}

bool SolersLLMClient::is_busy() const {
	MutexLock lock(mutex);
	return shared_state == STATE_CONNECTING || shared_state == STATE_REQUESTING || shared_state == STATE_STREAMING;
}

bool SolersLLMClient::is_done() const {
	MutexLock lock(mutex);
	return shared_state == STATE_DONE;
}

bool SolersLLMClient::is_failed() const {
	MutexLock lock(mutex);
	return shared_state == STATE_FAILED;
}

Dictionary SolersLLMClient::get_error() const {
	MutexLock lock(mutex);
	return shared_error;
}

Dictionary SolersLLMClient::take_auth_update() {
	MutexLock lock(mutex);
	Dictionary out = shared_auth_update;
	shared_auth_update = Dictionary();
	return out;
}

void SolersLLMClient::_join_worker() {
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	thread_active = false;
}

void SolersLLMClient::abort() {
	abort_requested.set();
	_join_worker();
	abort_requested.clear();
	MutexLock lock(mutex);
	shared_events = Array();
	shared_state = STATE_IDLE;
	shared_error = Dictionary();
}

SolersLLMClient::~SolersLLMClient() {
	abort_requested.set();
	_join_worker();
}
