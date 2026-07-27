/**************************************************************************/
/*  solers_context_manager.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Provider usage is authoritative for messages already sent; a cheap     */
/* estimator covers pending messages. Bytes already sent are immutable:   */
/* the provider has them cached and they are paid for, so the projection  */
/* never rewrites them. Eviction happens only at a monotonically          */
/* advancing cut, where compaction freezes the prefix into one summary    */
/* that never changes again and history resumes append-only.              */
/**************************************************************************/

#pragma once

#include "core/typedefs.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersContextManager {
	static constexpr int COMPACT_USER_MESSAGE_MAX_TOKENS = 20000;
	static constexpr int MEDIA_TOKEN_ESTIMATE = 2000;
	// One tool result may claim at most this fraction of the model window,
	// clamped to a fixed band: the floor keeps small windows usable, the
	// ceiling stops any single observation from displacing working history
	// (the band codex and opencode converged on).
	static constexpr int TOOL_RESULT_WINDOW_FRACTION = 4;
	static constexpr int TOOL_RESULT_MIN_TOKENS = 4000;
	static constexpr int TOOL_RESULT_MAX_TOKENS = 8000;

public:
	// Floor for a window learned from a rejected request, so one pathological
	// overflow cannot shrink the session down to an unusable budget.
	static constexpr int MIN_LEARNED_CONTEXT_TOKENS = 16000;

private:

	int authoritative_tokens = 0;
	int covered_message_count = 0;
	int last_estimated_tokens = 0;
	int compaction_count = 0;
	int last_compacted_token_count = -1;

	static int _estimate_message_tokens(const Dictionary &p_message);
	static String _truncate_text(const String &p_text, int p_max_tokens);
	static Array _select_recent_user_messages(const Array &p_messages);
	static String _build_summary_text(const String &p_summary, const Dictionary &p_plan);

public:
	static const char *COMPACTION_SUMMARY_PREFIX;
	static const char *COMPACTION_INSTRUCTION;
	static const char *CANCELLED_TOOL_RESULT;

	// Provider contract as a pure function over history: every assistant
	// tool_call is answered by exactly one tool message with the same id, and
	// no tool message answers a call that was never made. Turns killed
	// mid-queue break that invariant in durable history permanently, so the
	// projection repairs it rather than inferring why the turn ended.
	static Array repair_tool_pairing(const Array &p_messages);

	static int estimate_tokens(const String &p_text);
	static int estimate_messages_tokens(const Array &p_messages);
	// Budget for one tool result, resolved from the active model window.
	static int tool_result_token_budget(int p_context_window);

	void record_usage(int p_input_tokens, int p_covered_message_count);
	int get_token_count_with_pending(const Array &p_messages, const String &p_system_prompt, const Array &p_tools) const;
	// Compact once the next response no longer fits: the model states its own
	// output ceiling, so the headroom is a declared fact rather than a guessed
	// fraction of the window.
	bool should_compact(int p_used_tokens, int p_context_window, int p_max_output_tokens) const;
	bool should_compact(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window, int p_max_output_tokens) const;
	bool is_overflow(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window) const;

	// Model-facing projection: durable history is unchanged and every message
	// that has already been sent keeps its bytes, so each request extends the
	// previous prefix instead of replacing it.
	Array prepare_request(const Array &p_messages, const String &p_system_prompt, const Array &p_tools);
	Dictionary apply_compaction(const Array &p_messages, const String &p_summary, const Dictionary &p_plan = Dictionary());
	Array shrink_compaction_history(const Array &p_messages, int p_attempt) const;
	void reset();

	int get_last_estimated_tokens() const { return last_estimated_tokens; }
	int get_compaction_count() const { return compaction_count; }
};
