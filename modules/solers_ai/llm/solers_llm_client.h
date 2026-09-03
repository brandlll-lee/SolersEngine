/**************************************************************************/
/*  solers_llm_client.h                                                   */
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

#pragma once

#include "core/io/http_client.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

#include "modules/solers_ai/llm/solers_llm_protocol.h"

class SolersProviderAuth;

class SolersLLMClient {
public:
	enum State {
		STATE_IDLE,
		STATE_CONNECTING,
		STATE_REQUESTING,
		STATE_STREAMING,
		STATE_DONE,
		STATE_FAILED,
	};

private:
	SolersLLMProtocolRegistry *protocol_registry = nullptr;

	// --- Worker thread + shared state (the only cross-thread surface). -------
	Thread thread;
	mutable Mutex mutex;
	SafeFlag abort_requested;
	bool thread_active = false; // main-thread bookkeeping only

	Array shared_events; // produced by worker, drained by main (mutex-guarded)
	State shared_state = STATE_IDLE; // mutex-guarded snapshot for main-thread queries
	Dictionary shared_error; // mutex-guarded
	Dictionary shared_auth_update; // rotated OAuth credentials, consumed on main
	int64_t shared_request_body_bytes = 0;

	// --- Worker-owned request inputs (set by begin() before the thread runs). -
	String host;
	int port = 443;
	bool use_tls = true;
	String request_path;
	Vector<String> request_headers;
	String request_body;
	String trace_path;
	Dictionary initial_stream_state;
	Dictionary worker_profile;
	Dictionary worker_auth;
	Dictionary worker_request;
	StringName worker_protocol_id;
	SolersProviderAuth *worker_auth_method = nullptr; // Owned by SettingsService.

	// --- Worker-owned transient state (touched only on the worker thread). ---
	Ref<HTTPClient> http;
	SolersLLMProtocol *active_protocol = nullptr;
	State state = STATE_IDLE;
	bool request_sent = false;
	bool response_checked = false;
	bool capturing_error = false;
	bool oauth_401_retried = false;
	String sse_buffer;
	String error_buffer;
	Dictionary stream_state;
	Dictionary last_error; // worker-owned; copied into shared_error on failure
	uint64_t last_progress_msec = 0;
	State prev_state = STATE_IDLE;
	// S4 stream telemetry: counts text deltas + bytes for this request, summarized
	// into the stream_finish trace so each session's delta cadence is visible.
	int stream_text_delta_count = 0;
	int stream_text_bytes = 0;
	bool stream_saw_finish = false;
	bool stream_has_content = false;
	int response_bytes = 0;
	String response_prefix;

	Dictionary _redacted_request_body(const String &p_body) const;
	void _trace(const String &p_event, const Dictionary &p_payload = Dictionary()) const;
	void _drain_records(Array &r_events);
	void _complete_stream(Array &r_batch);
	void _fail_provider_response();
	void _fail(const String &p_code, const String &p_message, int p_retryable = -1, const String &p_failure_kind = String(), const Dictionary &p_details = Dictionary());

	static void _thread_func(void *p_userdata);
	void _run_worker();
	void _publish(const Array &p_events, State p_state);
	bool _prepare_auth_headers(bool p_force_oauth_refresh = false);
	bool _configure_endpoint(const Dictionary &p_profile);
	void _set_request_header(const String &p_name, const String &p_value);
	void _join_worker();

public:
	void set_protocol_registry(SolersLLMProtocolRegistry *p_registry) { protocol_registry = p_registry; }

	// Prepares and validates the request on the calling (main) thread, then
	// hands the network loop to a worker thread. Returns OK or an error code;
	// on synchronous failure `get_error()` carries the canonical error payload.
	Error begin(const Dictionary &p_request, const Dictionary &p_profile, const Dictionary &p_auth, SolersProviderAuth *p_auth_method = nullptr);

	// Drains canonical events produced by the worker since the last call. Does
	// no network I/O on the main thread.
	Array poll();

	State get_state() const;
	bool is_busy() const;
	bool is_done() const;
	bool is_failed() const;
	Dictionary get_error() const;
	Dictionary take_auth_update();
	int64_t get_request_body_bytes() const;
	void abort();

	SolersLLMClient() {}
	~SolersLLMClient();
};
