/**************************************************************************/
/*  solers_context_manager.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* The transcript is the complete audit record. The model receives a      */
/* bounded projection: completed tool exchanges are represented by turn   */
/* checkpoints instead of being replayed on every later request.          */
/**************************************************************************/

#pragma once

#include "core/typedefs.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersContextManager {
	static constexpr int MEDIA_TOKEN_ESTIMATE = 2000;

private:
	int authoritative_tokens = 0;
	int covered_message_count = 0;
	int last_estimated_tokens = 0;
	int compaction_count = 0;
	int last_compacted_token_count = -1;

	static int _estimate_message_tokens(const Dictionary &p_message);
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
	static Array project_completed_turns(const Array &p_messages);

	static int estimate_tokens(const String &p_text);
	static int estimate_messages_tokens(const Array &p_messages);
	void record_usage(int p_input_tokens, int p_covered_message_count);
	int get_token_count_with_pending(const Array &p_messages, const String &p_system_prompt, const Array &p_tools) const;
	// Reserve the provider's declared output capacity. Unknown windows never compact.
	bool should_compact(int p_used_tokens, int p_context_window, int p_max_output_tokens) const;
	bool should_compact(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window, int p_max_output_tokens) const;
	bool is_overflow(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window) const;

	// Model-facing projection for the active turn. Durable history remains in
	// the transcript; malformed in-flight tool pairing is repaired here.
	Array prepare_request(const Array &p_messages, const String &p_system_prompt, const Array &p_tools);
	Dictionary apply_compaction(const Array &p_messages, const String &p_summary, const Dictionary &p_plan = Dictionary());
	void reset();

	int get_last_estimated_tokens() const { return last_estimated_tokens; }
	int get_compaction_count() const { return compaction_count; }
};
