/**************************************************************************/
/*  solers_llm_client.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_llm_client.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

// Two-phase liveness model, faithful to opencode's provider fetch wrapper
// (_research/opencode/packages/opencode/src/provider/provider.ts):
//   - HEADER phase: until the response status/headers arrive. A short-ish budget
//     guards an endpoint that is unreachable or wedged before replying. Mirrors
//     opencode's `timeoutController(headerTimeout)` that is cleared the instant
//     the fetch resolves (`.finally(() => headerTimeoutCtl?.clear())`).
//   - STREAM phase: after 2xx headers, we only bound the gap *between* received
//     chunks (inter-chunk idle), never the time since the request began. A model
//     that "thinks" for a long while before the first token is healthy, not
//     stalled. Mirrors opencode's optional `wrapSSE(chunkTimeout)`.
// Both failures are transient and retryable; the agent session retries them.
static constexpr uint64_t SOLERS_LLM_HEADER_TIMEOUT_MSEC = 30000;
static constexpr uint64_t SOLERS_LLM_CHUNK_TIMEOUT_MSEC = 120000;

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

void SolersLLMClient::_fail(const String &p_code, const String &p_message, Array &r_events) {
	// Preserve any HTTP status / response headers captured before the failure so
	// the retry layer can classify (5xx vs 4xx) and honor Retry-After headers.
	const Variant http_status = last_error.get("http_status", Variant());
	const Variant headers = last_error.get("headers", Variant());
	last_error.clear();
	last_error["code"] = p_code;
	last_error["message"] = p_message;
	if (http_status.get_type() != Variant::NIL) {
		last_error["http_status"] = http_status;
	}
	if (headers.get_type() != Variant::NIL) {
		last_error["headers"] = headers;
	}
	Dictionary payload;
	payload["code"] = p_code;
	payload["message"] = p_message;
	_trace("fail", payload);
	r_events.push_back(SolersLLMEvent::error(p_code, p_message));
	state = STATE_FAILED;
	if (http.is_valid()) {
		http->close();
	}
}

Error SolersLLMClient::begin(const Dictionary &p_request, const Dictionary &p_profile, const String &p_api_key) {
	// Join any prior worker before reconfiguring shared state.
	abort_requested.set();
	_join_worker();
	abort_requested.clear();

	{
		MutexLock lock(mutex);
		shared_events = Array();
		shared_error = Dictionary();
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

	String base_url = String(p_profile.get("base_url", String())).strip_edges();
	if (base_url.is_empty()) {
		last_error["code"] = "NO_BASE_URL";
		last_error["message"] = "Provider profile has no base URL configured.";
		_publish(Array(), STATE_FAILED);
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

	String validation_code;
	String validation_message;
	if (!active_protocol->validate_request(p_request, validation_code, validation_message)) {
		last_error["code"] = validation_code.is_empty() ? "PROTOCOL_REQUEST_INVALID" : validation_code;
		last_error["message"] = validation_message.is_empty() ? "The LLM request violates the provider protocol contract." : validation_message;
		_publish(Array(), STATE_FAILED);
		return ERR_INVALID_DATA;
	}

	const Dictionary body = active_protocol->build_request_body(p_request);
	request_body = JSON::stringify(body, "", false, true);
	trace_path = "user://solers_ai_provider_trace.jsonl";
	Dictionary trace_payload;
	trace_payload["host"] = host;
	trace_payload["path"] = request_path;
	trace_payload["protocol"] = String(protocol_id);
	trace_payload["body"] = _redacted_request_body(request_body);
	_trace("request_prepared", trace_payload);

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

	// Reset worker-owned transient state for the fresh request.
	http = Ref<HTTPClient>();
	request_sent = false;
	response_checked = false;
	capturing_error = false;
	sse_buffer = String();
	error_buffer = String();
	initial_stream_state = active_protocol->begin_stream(p_request);
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

	http = HTTPClient::create();
	if (http.is_null()) {
		last_error.clear();
		last_error["code"] = "NO_HTTP_CLIENT";
		last_error["message"] = "Failed to create HTTPClient.";
		batch.push_back(SolersLLMEvent::error("NO_HTTP_CLIENT", "Failed to create HTTPClient."));
		_publish(batch, STATE_FAILED);
		return;
	}
	Ref<TLSOptions> tls = use_tls ? TLSOptions::client() : Ref<TLSOptions>();
	const Error err = http->connect_to_host(host, port, tls);
	if (err != OK) {
		last_error.clear();
		last_error["code"] = "CONNECT_FAILED";
		const String connect_message = vformat("connect_to_host failed for %s:%d (error %d).", host, port, err);
		last_error["message"] = connect_message;
		batch.push_back(SolersLLMEvent::error("CONNECT_FAILED", connect_message));
		_publish(batch, STATE_FAILED);
		http = Ref<HTTPClient>();
		return;
	}

	stream_state = initial_stream_state;
	state = STATE_CONNECTING;
	last_progress_msec = OS::get_singleton()->get_ticks_msec();
	prev_state = state;
	stream_text_delta_count = 0;
	stream_text_bytes = 0;

	while (!abort_requested.is_set()) {
		batch.clear();

		// Phase-aware liveness: a generous inter-chunk budget once streaming,
		// a short header budget before. (opencode headerTimeout / wrapSSE.)
		{
			const bool streaming = (state == STATE_STREAMING);
			const uint64_t budget = streaming ? SOLERS_LLM_CHUNK_TIMEOUT_MSEC : SOLERS_LLM_HEADER_TIMEOUT_MSEC;
			if (budget > 0 && OS::get_singleton()->get_ticks_msec() - last_progress_msec > budget) {
				if (streaming) {
					_fail("STREAM_STALL", vformat("The model stream stalled: no data for %d seconds after streaming began. Reconnecting.", (int)(SOLERS_LLM_CHUNK_TIMEOUT_MSEC / 1000)), batch);
				} else {
					_fail("HEADER_TIMEOUT", vformat("The model provider did not send response headers within %d seconds. The endpoint may be unreachable or overloaded. Reconnecting.", (int)(SOLERS_LLM_HEADER_TIMEOUT_MSEC / 1000)), batch);
				}
				_publish(batch, state);
				break;
			}
		}

		http->poll();
		const HTTPClient::Status st = http->get_status();
		switch (st) {
			case HTTPClient::STATUS_RESOLVING:
			case HTTPClient::STATUS_CONNECTING: {
				state = STATE_CONNECTING;
			} break;
			case HTTPClient::STATUS_CANT_RESOLVE: {
				_fail("CANT_RESOLVE", vformat("Could not resolve host '%s'.", host), batch);
			} break;
			case HTTPClient::STATUS_CANT_CONNECT: {
				_fail("CANT_CONNECT", vformat("Could not connect to '%s:%d'.", host, port), batch);
			} break;
			case HTTPClient::STATUS_CONNECTION_ERROR: {
				_fail("CONNECTION_ERROR", "HTTP connection error.", batch);
			} break;
			case HTTPClient::STATUS_TLS_HANDSHAKE_ERROR: {
				_fail("TLS_ERROR", "TLS handshake failed.", batch);
			} break;
			case HTTPClient::STATUS_CONNECTED: {
				if (!request_sent) {
					const CharString body_utf8 = request_body.utf8();
					const Error request_err = http->request(HTTPClient::METHOD_POST, request_path, request_headers, (const uint8_t *)body_utf8.get_data(), body_utf8.length());
					request_sent = true;
					if (request_err != OK) {
						_fail("REQUEST_FAILED", vformat("HTTP request failed (error %d).", request_err), batch);
					} else {
						state = STATE_REQUESTING;
					}
				} else if (response_checked) {
					if (capturing_error) {
						_fail("HTTP_ERROR", error_buffer.is_empty() ? "Provider returned an error." : error_buffer, batch);
					} else {
						_drain_records(batch);
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
						List<String> header_list;
						http->get_response_headers(&header_list);
						Dictionary headers_dict;
						for (const String &h : header_list) {
							const int hcolon = h.find(":");
							if (hcolon > 0) {
								headers_dict[h.substr(0, hcolon).strip_edges()] = h.substr(hcolon + 1).strip_edges();
							}
						}
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
					const String s = String::utf8((const char *)chunk.ptr(), chunk.size());
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
					_fail("HTTP_ERROR", error_buffer.is_empty() ? "Provider returned an error." : error_buffer, batch);
				} else {
					_drain_records(batch);
					state = STATE_DONE;
				}
			} break;
			default: {
			} break;
		}

		if (state != prev_state) {
			last_progress_msec = OS::get_singleton()->get_ticks_msec();
			prev_state = state;
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
				} else if (kind == SolersLLMEventKind::TOOL_CALL) {
					Dictionary payload;
					payload["id"] = event.get("id", String());
					payload["name"] = event.get("name", String());
					_trace("stream_tool_call", payload);
				} else if (kind == SolersLLMEventKind::FINISH) {
					Dictionary payload;
					payload["stop_reason"] = event.get("stop_reason", String());
					payload["text_delta_count"] = stream_text_delta_count;
					payload["text_bytes"] = stream_text_bytes;
					_trace("stream_finish", payload);
				}
				r_events.push_back(produced[i]);
			}
		}
		sep = sse_buffer.find("\n\n");
	}
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
