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
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"

// ---------------------------------------------------------------------------
// SolersLLMClient — non-blocking streaming HTTP execution for one request.
//
// Transport, orthogonal to the protocol. Given a resolved provider profile, an
// api key and a canonical request, it builds the body+headers via the protocol,
// opens a TLS connection, POSTs, and incrementally reads the SSE response. It
// never blocks the editor: callers `begin()` once, then `poll()` every frame
// (the editor plugin already polls Solers services from NOTIFICATION_PROCESS).
// Each `poll()` returns the canonical `LLMEvent`s produced since the last call.
//
// Pull model (no signals): the agent session owns the loop and drains events,
// which keeps the control flow explicit and unit-testable.
// ---------------------------------------------------------------------------
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
	Ref<HTTPClient> http;
	SolersLLMProtocol *active_protocol = nullptr;
	State state = STATE_IDLE;

	String host;
	int port = 443;
	bool use_tls = true;
	String request_path;
	Vector<String> request_headers;
	String request_body;

	bool request_sent = false;
	bool response_checked = false;
	bool capturing_error = false;
	String sse_buffer;
	String error_buffer;
	Dictionary stream_state;
	Dictionary last_error;

	void _drain_records(Array &r_events);
	void _fail(const String &p_code, const String &p_message, Array &r_events);
	void _reset();

public:
	void set_protocol_registry(SolersLLMProtocolRegistry *p_registry) { protocol_registry = p_registry; }

	// Prepares and opens the request. Returns OK or an error code; on failure
	// `get_error()` carries the canonical error payload.
	Error begin(const Dictionary &p_request, const Dictionary &p_profile, const String &p_api_key);

	// Advances the connection/stream and returns events produced this tick.
	Array poll();

	State get_state() const { return state; }
	bool is_busy() const { return state == STATE_CONNECTING || state == STATE_REQUESTING || state == STATE_STREAMING; }
	bool is_done() const { return state == STATE_DONE; }
	bool is_failed() const { return state == STATE_FAILED; }
	Dictionary get_error() const { return last_error; }

	void abort();

	SolersLLMClient() {}
};
