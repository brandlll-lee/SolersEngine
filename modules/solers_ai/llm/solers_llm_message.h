/**************************************************************************/
/*  solers_llm_message.h                                                  */
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

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// ---------------------------------------------------------------------------
// Solers LLM canonical intermediate representation.
//
// Inspired by opencode's `packages/llm` design: every wire protocol lowers a
// single provider-agnostic request into its native body, and lifts its native
// streaming frames back into a single provider-agnostic event stream. Solers
// carries that representation as plain Godot `Dictionary`/`Array` values so it
// flows for free across the tool registry, the RPC/MCP layer, and the dock UI
// without bespoke marshalling.
//
// This header is the single source of truth for the *shape* of those values.
// It owns only constants and small constructor helpers (no behaviour), so the
// protocols, the client, and the agent session all speak the exact same
// vocabulary. Changing a field name here changes it everywhere (locality).
// ---------------------------------------------------------------------------

// Canonical message roles (request side).
class SolersLLMRole {
public:
	static const char *SYSTEM;
	static const char *USER;
	static const char *ASSISTANT;
	static const char *TOOL;
};

// Canonical streaming event kinds (response side). A protocol's stream parser
// only ever emits these; downstream consumers never branch on provider names.
class SolersLLMEventKind {
public:
	static const char *TEXT_DELTA; // incremental assistant text
	static const char *REASONING_DELTA; // incremental thinking/reasoning text
	static const char *TOOL_CALL; // a fully-assembled tool call (id, name, arguments)
	static const char *USAGE; // token accounting
	static const char *FINISH; // turn finished (carries stop reason)
	static const char *ERROR; // protocol/transport surfaced error
};

// Canonical stop reasons, normalised across providers.
class SolersLLMStopReason {
public:
	static const char *END_TURN; // model finished its message
	static const char *TOOL_USE; // model wants tool results before continuing
	static const char *MAX_TOKENS; // output truncated by token budget
	static const char *STOP; // explicit stop sequence / other terminal
};

// Small, allocation-free constructors so protocols read declaratively instead
// of hand-building dictionaries inline. These are the only writers of the
// event shape; everything else just reads the keys.
class SolersLLMEvent {
public:
	static Dictionary text_delta(const String &p_text) {
		Dictionary e;
		e["kind"] = SolersLLMEventKind::TEXT_DELTA;
		e["text"] = p_text;
		return e;
	}

	static Dictionary reasoning_delta(const String &p_text) {
		Dictionary e;
		e["kind"] = SolersLLMEventKind::REASONING_DELTA;
		e["text"] = p_text;
		return e;
	}

	static Dictionary tool_call(const String &p_id, const String &p_name, const String &p_arguments_json) {
		Dictionary e;
		e["kind"] = SolersLLMEventKind::TOOL_CALL;
		e["id"] = p_id;
		e["name"] = p_name;
		e["arguments"] = p_arguments_json; // raw JSON string; the session parses it
		return e;
	}

	static Dictionary usage(int p_input_tokens, int p_output_tokens) {
		Dictionary e;
		e["kind"] = SolersLLMEventKind::USAGE;
		e["input_tokens"] = p_input_tokens;
		e["output_tokens"] = p_output_tokens;
		return e;
	}

	static Dictionary finish(const String &p_stop_reason) {
		Dictionary e;
		e["kind"] = SolersLLMEventKind::FINISH;
		e["stop_reason"] = p_stop_reason;
		return e;
	}

	static Dictionary error(const String &p_code, const String &p_message) {
		Dictionary e;
		e["kind"] = SolersLLMEventKind::ERROR;
		e["code"] = p_code;
		e["message"] = p_message;
		return e;
	}
};

// Constructors for canonical request messages, mirroring the role vocabulary.
class SolersLLMMessage {
public:
	static Dictionary system(const String &p_text) {
		Dictionary m;
		m["role"] = SolersLLMRole::SYSTEM;
		m["content"] = p_text;
		return m;
	}

	static Dictionary user(const String &p_text) {
		Dictionary m;
		m["role"] = SolersLLMRole::USER;
		m["content"] = p_text;
		return m;
	}

	// Assistant turn that requested tools. `p_tool_calls` is an Array of
	// { id, name, arguments(JSON string) }. `p_text` may be empty.
	static Dictionary assistant(const String &p_text, const Array &p_tool_calls) {
		Dictionary m;
		m["role"] = SolersLLMRole::ASSISTANT;
		m["content"] = p_text;
		if (!p_tool_calls.is_empty()) {
			m["tool_calls"] = p_tool_calls;
		}
		return m;
	}

	// Result of executing a tool call, fed back to the model next turn.
	static Dictionary tool_result(const String &p_tool_call_id, const String &p_name, const String &p_content) {
		Dictionary m;
		m["role"] = SolersLLMRole::TOOL;
		m["tool_call_id"] = p_tool_call_id;
		m["name"] = p_name;
		m["content"] = p_content;
		return m;
	}
};
