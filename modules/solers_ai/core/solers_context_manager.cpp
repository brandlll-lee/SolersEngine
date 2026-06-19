/**************************************************************************/
/*  solers_context_manager.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_context_manager.h"

#include "core/io/json.h"
#include "core/templates/vector.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

const char *SolersContextManager::TOOL_RESULT_PLACEHOLDER =
		"[Solers: old tool result cleared to reclaim context. Re-run the tool if you need this data again.]";
const char *SolersContextManager::ELISION_MARKER =
		"[Solers: earlier conversation elided to fit the model context window. The first request and the most recent turns are kept.]";

// Cheap, provider-agnostic token estimate (~4 chars/token), matching opencode's
// `Token.estimate(JSON.stringify(...))`. Good enough for budget decisions; the
// authoritative count still comes back from the provider's usage event.
int SolersContextManager::_estimate_text_tokens(const String &p_text) {
	return p_text.length() / 4 + 1;
}

int SolersContextManager::_estimate_tokens(const Array &p_messages) {
	const String serialized = JSON::stringify(p_messages, "", false, true);
	return serialized.length() / 4 + 1;
}

int SolersContextManager::_prune(Array &p_messages) const {
	int budget = TOOL_RESULT_PROTECT_TOKENS;
	int total = 0;
	int pruned = 0;
	for (int i = p_messages.size() - 1; i >= 0; i--) {
		Dictionary message = p_messages[i];
		if (String(message.get("role", String())) != String(SolersLLMRole::TOOL)) {
			continue;
		}
		const String content = message.get("content", String());
		if (content == String(TOOL_RESULT_PLACEHOLDER)) {
			continue;
		}
		total += _estimate_text_tokens(content);
		if (total > budget) {
			message["content"] = TOOL_RESULT_PLACEHOLDER;
			p_messages[i] = message;
			pruned++;
		}
	}
	return pruned;
}

Array SolersContextManager::_elide(const Array &p_messages, int p_context_window, int p_output_reserve, int p_keep_recent) const {
	// Turn starts are user messages (system lives outside `messages`).
	Vector<int> turn_starts;
	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		if (String(message.get("role", String())) == String(SolersLLMRole::USER)) {
			turn_starts.push_back(i);
		}
	}
	// Need at least the first turn plus the kept recent turns to elide anything.
	if (turn_starts.size() <= p_keep_recent + 1) {
		return p_messages;
	}

	const int second_turn_start = turn_starts[1];
	const int recent_turn_start = turn_starts[turn_starts.size() - p_keep_recent];

	Array result;
	// Head: the entire first turn (intent + its tool work).
	for (int i = 0; i < second_turn_start; i++) {
		result.push_back(p_messages[i]);
	}
	// Single elision marker standing in for the dropped middle turns.
	result.push_back(SolersLLMMessage::user(ELISION_MARKER));
	// Tail: the most recent whole turns.
	for (int i = recent_turn_start; i < p_messages.size(); i++) {
		result.push_back(p_messages[i]);
	}
	return result;
}

bool SolersContextManager::is_overflow(const Array &p_messages, int p_context_window, int p_output_reserve) const {
	const int usable = p_context_window - p_output_reserve;
	return _estimate_tokens(p_messages) >= usable;
}

Array SolersContextManager::compact(const Array &p_messages, int p_context_window, int p_output_reserve) {
	Array work = p_messages.duplicate(true);

	// Tier 1: prune stale tool-result payloads (every request).
	const int pruned = _prune(work);
	if (pruned > 0) {
		prune_count += pruned;
	}

	// Tier 2: if still over budget, elide middle turns. Shrink the kept-recent
	// window progressively so even a single oversized history converges.
	if (is_overflow(work, p_context_window, p_output_reserve)) {
		for (int keep = KEEP_RECENT_TURNS; keep >= MIN_KEEP_RECENT_TURNS; keep--) {
			Array elided = _elide(work, p_context_window, p_output_reserve, keep);
			if (elided.size() == work.size()) {
				// No turns could be dropped at this granularity.
				continue;
			}
			work = elided;
			compaction_count++;
			if (!is_overflow(work, p_context_window, p_output_reserve)) {
				break;
			}
		}
	}

	last_estimated_tokens = _estimate_tokens(work);
	return work;
}
