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
/* estimator covers pending messages. Durable history stays append-only   */
/* on disk; prepare_request projects a lean model-facing view by stubbing */
/* oldest tool bodies when over the working-set budget. Full compaction   */
/* remains the sole durable rewrite (recent user spine + handoff).        */
/**************************************************************************/

#pragma once

#include "core/typedefs.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersContextManager {
	static constexpr int COMPACT_USER_MESSAGE_MAX_TOKENS = 20000;
	static constexpr double COMPACTION_TRIGGER_RATIO = 0.60;
	static constexpr int RESERVED_CONTEXT_TOKENS = 50000;
	static constexpr int MEDIA_TOKEN_ESTIMATE = 2000;
	// Model-facing working set: project tool bodies down when the request
	// estimate exceeds this fraction of the window (fallback when unknown).
	static constexpr double WORKING_SET_RATIO = 0.45;
	static constexpr int WORKING_SET_FALLBACK_TOKENS = 80000;
	// One tool result may claim at most this fraction of the model window,
	// clamped to a fixed band: the floor keeps small windows usable, the
	// ceiling stops any single observation from displacing working history
	// (the band codex and opencode converged on).
	static constexpr int TOOL_RESULT_WINDOW_FRACTION = 4;
	static constexpr int TOOL_RESULT_MIN_TOKENS = 4000;
	static constexpr int TOOL_RESULT_MAX_TOKENS = 8000;

	int authoritative_tokens = 0;
	int covered_message_count = 0;
	int last_estimated_tokens = 0;
	int compaction_count = 0;
	int last_compacted_token_count = -1;

	static int _estimate_message_tokens(const Dictionary &p_message);
	static String _truncate_text(const String &p_text, int p_max_tokens);
	static Array _select_recent_user_messages(const Array &p_messages);
	static String _build_summary_text(const String &p_summary, const Dictionary &p_plan);
	static int _working_set_budget(int p_context_window);
	static String _omitted_tool_stub(const Dictionary &p_message);

public:
	static const char *COMPACTION_SUMMARY_PREFIX;
	static const char *COMPACTION_INSTRUCTION;

	static int estimate_tokens(const String &p_text);
	static int estimate_messages_tokens(const Array &p_messages);
	// Budget for one tool result, resolved from the active model window.
	static int tool_result_token_budget(int p_context_window);
	// Keep the head and tail of an oversized payload and elide the middle,
	// measuring in estimated tokens (mirrors codex/pi middle truncation).
	static String middle_truncate(const String &p_text, int p_max_tokens);

	void record_usage(int p_input_tokens, int p_covered_message_count);
	int get_token_count_with_pending(const Array &p_messages, const String &p_system_prompt, const Array &p_tools) const;
	bool should_compact(int p_used_tokens, int p_context_window) const;
	bool should_compact(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window) const;
	bool is_overflow(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window) const;

	// Model-facing projection: durable history is unchanged. When the request
	// estimate exceeds the working-set budget, oldest tool message bodies are
	// stubbed (role/tool_call_id/name kept) while user/assistant stay intact
	// and the newest tool results are protected from the tail inward.
	Array prepare_request(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window);
	Dictionary apply_compaction(const Array &p_messages, const String &p_summary, const Dictionary &p_plan = Dictionary());
	Array shrink_compaction_history(const Array &p_messages, int p_attempt) const;
	void reset();

	int get_last_estimated_tokens() const { return last_estimated_tokens; }
	int get_compaction_count() const { return compaction_count; }
};
