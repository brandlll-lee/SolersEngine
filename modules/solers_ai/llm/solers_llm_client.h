/**************************************************************************/
/*  solers_llm_client.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* SolersLLMClient — streaming HTTP transport for one model request, run  */
/* entirely on a worker thread so the editor main thread never does        */
/* network I/O or large SSE parsing (the fix for the white-screen/freeze   */
/* class of crash). Faithful to codex's async streaming: the request loop  */
/* lives off the UI thread; tool execution stays on the main thread in the */
/* agent session.                                                          */
/*                                                                         */
/* Contract is unchanged for callers: `begin()` once (synchronous setup +  */
/* validation errors still returned inline), then `poll()` every frame to  */
/* drain the canonical `LLMEvent`s the worker has produced. The transport   */
/* HTTPClient is created, used, and destroyed solely on the worker thread.  */
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

	// --- Worker-owned request inputs (set by begin() before the thread runs). -
	String host;
	int port = 443;
	bool use_tls = true;
	String request_path;
	Vector<String> request_headers;
	String request_body;
	String trace_path;
	Dictionary initial_stream_state;

	// --- Worker-owned transient state (touched only on the worker thread). ---
	Ref<HTTPClient> http;
	SolersLLMProtocol *active_protocol = nullptr;
	State state = STATE_IDLE;
	bool request_sent = false;
	bool response_checked = false;
	bool capturing_error = false;
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

	Dictionary _redacted_request_body(const String &p_body) const;
	void _trace(const String &p_event, const Dictionary &p_payload = Dictionary()) const;
	void _drain_records(Array &r_events);
	void _fail(const String &p_code, const String &p_message, Array &r_events);

	static void _thread_func(void *p_userdata);
	void _run_worker();
	void _publish(const Array &p_events, State p_state);
	void _join_worker();

public:
	void set_protocol_registry(SolersLLMProtocolRegistry *p_registry) { protocol_registry = p_registry; }

	// Prepares and validates the request on the calling (main) thread, then
	// hands the network loop to a worker thread. Returns OK or an error code;
	// on synchronous failure `get_error()` carries the canonical error payload.
	Error begin(const Dictionary &p_request, const Dictionary &p_profile, const String &p_api_key);

	// Drains canonical events produced by the worker since the last call. Does
	// no network I/O on the main thread.
	Array poll();

	State get_state() const;
	bool is_busy() const;
	bool is_done() const;
	bool is_failed() const;
	Dictionary get_error() const;

	void abort();

	SolersLLMClient() {}
	~SolersLLMClient();
};
