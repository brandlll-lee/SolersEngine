/**************************************************************************/
/*  solers_context_manager.h                                              */
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
#include "core/typedefs.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersContextManager {
	static constexpr int MEDIA_TOKEN_ESTIMATE = 2000;

private:
	int authoritative_tokens = 0;
	int covered_message_count = 0;
	int last_estimated_tokens = 0;
	int compaction_count = 0;

	static int _estimate_message_tokens(const Dictionary &p_message);
	static String _build_summary_text(const String &p_summary);

public:
	static constexpr int DEFAULT_OUTPUT_TOKENS = 8192;
	// Ceiling for one observation, independent of where it lands in the
	// conversation so identical results stay byte-identical and cacheable.
	static constexpr int TOOL_RESULT_MAX_TOKENS = 10000;
	static const char *COMPACTION_SUMMARY_PREFIX;
	static const char *CANCELLED_TOOL_RESULT;
	static const char *MODEL_CONTEXT_ROLE;

	static Array repair_tool_pairing(const Array &p_messages);
	static Array project_completed_turns(const Array &p_messages);
	static Array project_tool_evidence(const Array &p_messages);

	static int estimate_tokens(const String &p_text);
	static int estimate_messages_tokens(const Array &p_messages);
	static String clamp_to_tokens(const String &p_text, int p_token_budget);
	static bool append_bounded(Array &r_results, const Dictionary &p_entry, int p_max_results, int p_token_budget, int &r_tokens);
	void record_usage(int p_input_tokens, int p_covered_message_count, int p_transient_tokens = 0);
	int get_token_count_with_pending(const Array &p_messages, const String &p_system_prompt, int p_tool_tokens, int p_transient_tokens = 0);
	// Reserve the provider's declared output capacity. Unknown windows never compact.
	bool should_compact(int p_used_tokens, int p_context_window, int p_max_output_tokens) const;
	bool should_compact(const Array &p_messages, const String &p_system_prompt, int p_tool_tokens, int p_context_window, int p_max_output_tokens, int p_transient_tokens = 0);
	// A zero budget builds the smallest semantic core for unknown-window overflow recovery.
	Dictionary apply_compaction(const Array &p_messages, const String &p_summary, int p_token_budget);
	void reset();

	int get_last_estimated_tokens() const { return last_estimated_tokens; }
	int get_compaction_count() const { return compaction_count; }
	void set_compaction_count(int p_count) { compaction_count = MAX(0, p_count); }
};
