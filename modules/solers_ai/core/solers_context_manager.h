/**************************************************************************/
/*  solers_context_manager.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Context management — the harness's third necessary element. Modeled on  */
/* opencode `session/compaction.ts` and Claude Code's tiered cascade:      */
/*                                                                         */
/*   Tier 1 (prune, no LLM): clear the oldest tool-result payloads,        */
/*     keeping the most recent ones within a token budget. Surgical,       */
/*     cheap, runs every request.                                          */
/*   Tier 2 (elide, no LLM): if still over budget, drop whole middle       */
/*     turns, preserving the first turn (intent) and the most recent       */
/*     turns, replacing the gap with a single elision marker.              */
/*                                                                         */
/* Together these bound the request size deterministically, which is the   */
/* fix for the unbounded-history OOM. An LLM-summary tier can layer on top  */
/* later without changing this contract.                                   */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersContextManager {
	// Reclaim oldest tool outputs once recent tool output exceeds this many
	// (estimated) tokens. Mirrors opencode's PRUNE_PROTECT.
	static constexpr int TOOL_RESULT_PROTECT_TOKENS = 40000;
	// Whole turns to always keep at the tail during tier-2 elision.
	static constexpr int KEEP_RECENT_TURNS = 4;
	static constexpr int MIN_KEEP_RECENT_TURNS = 1;

	int last_estimated_tokens = 0;
	int prune_count = 0;
	int compaction_count = 0;

	static int _estimate_tokens(const Array &p_messages);
	static int _estimate_text_tokens(const String &p_text);
	// Tier 1: replace stale tool-result content with a placeholder in place.
	int _prune(Array &p_messages) const;
	// Tier 2: drop whole middle turns, keeping first + recent turns.
	Array _elide(const Array &p_messages, int p_context_window, int p_output_reserve, int p_keep_recent) const;

public:
	static const char *TOOL_RESULT_PLACEHOLDER;
	static const char *ELISION_MARKER;

	bool is_overflow(const Array &p_messages, int p_context_window, int p_output_reserve) const;

	// Returns a compacted copy of the conversation that fits the budget. The
	// caller adopts the result as its canonical history (compaction is
	// intentionally destructive, exactly like opencode/Claude Code).
	Array compact(const Array &p_messages, int p_context_window, int p_output_reserve);

	int get_last_estimated_tokens() const { return last_estimated_tokens; }
	int get_prune_count() const { return prune_count; }
	int get_compaction_count() const { return compaction_count; }
};
