/**************************************************************************/
/*  solers_context_manager.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Context management derived from Kimi Code's FullCompaction and         */
/* MicroCompaction. Provider usage is authoritative for messages already  */
/* sent; a cheap estimator covers pending messages. Old tool payloads are  */
/* cleared only in the request projection, while full compaction replaces */
/* history with recent real user input plus one model-written handoff.     */
/**************************************************************************/

#pragma once

#include "core/typedefs.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersContextManager {
	static constexpr int COMPACT_USER_MESSAGE_MAX_TOKENS = 20000;
	static constexpr double COMPACTION_TRIGGER_RATIO = 0.85;
	static constexpr int RESERVED_CONTEXT_TOKENS = 50000;
	static constexpr int MEDIA_TOKEN_ESTIMATE = 2000;
	// One tool result may claim at most this fraction of the model window;
	// the floor keeps small windows usable and the fallback covers models
	// whose limits are still unknown.
	static constexpr int TOOL_RESULT_WINDOW_FRACTION = 4;
	static constexpr int TOOL_RESULT_MIN_TOKENS = 4000;
	static constexpr int TOOL_RESULT_FALLBACK_TOKENS = 16000;

	int authoritative_tokens = 0;
	int covered_message_count = 0;
	int micro_cutoff = 0;
	int last_estimated_tokens = 0;
	int micro_compaction_count = 0;
	int compaction_count = 0;
	int last_compacted_token_count = -1;

	static int _estimate_message_tokens(const Dictionary &p_message);
	static String _truncate_text(const String &p_text, int p_max_tokens);
	static Array _select_recent_user_messages(const Array &p_messages);
	static String _build_summary_text(const String &p_summary, const Dictionary &p_plan);
	Array _micro_project(const Array &p_messages) const;

public:
	static const char *TOOL_RESULT_PLACEHOLDER;
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

	Array prepare_request(const Array &p_messages, const String &p_system_prompt, const Array &p_tools);
	Dictionary apply_compaction(const Array &p_messages, const String &p_summary, const Dictionary &p_plan = Dictionary());
	Array shrink_compaction_history(const Array &p_messages, int p_attempt) const;
	void reset();

	int get_last_estimated_tokens() const { return last_estimated_tokens; }
	int get_micro_compaction_count() const { return micro_compaction_count; }
	int get_compaction_count() const { return compaction_count; }
	int get_micro_cutoff() const { return micro_cutoff; }
};
