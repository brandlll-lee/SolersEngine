/**************************************************************************/
/*  solers_llm_protocol.h                                                 */
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

#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// ---------------------------------------------------------------------------
// SolersLLMProtocol — the wire-protocol seam.
//
// A protocol is the deep module that knows exactly two things about a provider
// family's HTTP contract, and nothing else:
//
//   1. body:   how to lower a canonical `LLMRequest` (see solers_llm_message.h)
//              into the provider-native JSON request body, and which path and
//              extra headers that body needs.
//   2. stream: a state machine that lifts the provider's server-sent-event
//              frames back into canonical `LLMEvent`s. `begin_stream` seeds the
//              per-response state; `parse_event` advances it one SSE record at
//              a time and returns zero or more canonical events.
//
// Transport (sockets, TLS, polling), auth (which header carries the key), and
// endpoints (base URL) live elsewhere and are orthogonal. The same protocol
// therefore serves the official API, a relay, or any compatible endpoint —
// only the provider profile (base URL + auth) changes. That is how "implement
// the OpenAI / Anthropic protocol" lets us connect to anything that speaks it.
// ---------------------------------------------------------------------------
class SolersLLMProtocol {
public:
	virtual ~SolersLLMProtocol() {}

	// Stable identifier, e.g. "openai-chat", "anthropic-messages".
	virtual StringName get_id() const = 0;

	// Default request path appended to the provider base URL when the profile
	// does not override it, e.g. "/chat/completions" or "/v1/messages".
	virtual String get_default_path() const = 0;

	// Lower a canonical request (see solers_llm_message.h for its shape) into
	// the provider-native JSON body. Pure: no I/O, fully unit-testable.
	virtual Dictionary build_request_body(const Dictionary &p_request) const = 0;

	// Provider protocols may reject a canonical request before the transport
	// opens a connection. This is for hard wire-contract violations, not
	// provider policy errors.
	virtual bool validate_request(const Dictionary &p_request, String &r_code, String &r_message) const { return true; }

	// Protocol-mandated headers independent of auth (e.g. anthropic-version).
	// Default: nothing to add.
	virtual void augment_headers(Dictionary &r_headers, const Dictionary &p_request) const {}

	// Seed the per-response streaming parser state for a fresh request.
	virtual Dictionary begin_stream(const Dictionary &p_request) const { return Dictionary(); }

	// Advance the stream state by one decoded SSE record and return the
	// canonical events it produced (possibly empty). `p_event_name` is the SSE
	// `event:` field (empty for providers that only use `data:` lines);
	// `p_data` is the raw `data:` payload (already stripped of the prefix).
	virtual Array parse_event(Dictionary &r_state, const String &p_event_name, const String &p_data) const = 0;
};

// ---------------------------------------------------------------------------
// SolersLLMProtocolRegistry — name -> protocol lookup.
//
// Owns the concrete protocol instances and hands them out by id. The client
// resolves `profile.protocol` through this registry, so adding a new wire
// protocol is a registration, never a new branch in a dispatcher.
// ---------------------------------------------------------------------------
class SolersLLMProtocolRegistry {
	HashMap<StringName, SolersLLMProtocol *> protocols;

public:
	// Takes ownership of `p_protocol`.
	void register_protocol(SolersLLMProtocol *p_protocol);
	SolersLLMProtocol *get(const StringName &p_id) const;
	bool has(const StringName &p_id) const;
	LocalVector<StringName> list_ids() const;

	// Registers the built-in protocols (OpenAI Chat, Anthropic Messages, ...).
	void register_builtin_protocols();

	SolersLLMProtocolRegistry() {}
	~SolersLLMProtocolRegistry();
};
