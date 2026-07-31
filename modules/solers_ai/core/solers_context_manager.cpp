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
#include "core/templates/hash_set.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

const char *SolersContextManager::COMPACTION_SUMMARY_PREFIX =
		"The conversation so far has been compacted to free up context. What follows is your own working summary of this task — use it to continue your train of thought rather than starting over. Treat it as notes, not proof: where it says a step was done, tests passed, or a fix worked, verify that yourself before relying on it.";
const char *SolersContextManager::COMPACTION_INSTRUCTION =
		"You are about to run out of context. Write a first-person handoff note to yourself so you can seamlessly continue this task after the earlier conversation is cleared.\n\n"
		"Write the note as your own continuing train of thought, in present tense. Preserve the latest user request verbatim, active constraints, exact files and commands already used, verified results versus uncertainty, and the precise next action. Be concise and self-sufficient. Respond with text only and do not call tools.";
const char *SolersContextManager::CANCELLED_TOOL_RESULT =
		"{\"ok\":false,\"error\":{\"code\":\"TOOL_CANCELLED\",\"message\":\"This call never produced a result: the turn ended before it finished. Re-run it if its output is still needed.\",\"recoverable\":true}}";

int SolersContextManager::estimate_tokens(const String &p_text) {
	int ascii_count = 0;
	int non_ascii_count = 0;
	for (int i = 0; i < p_text.length(); i++) {
		if (p_text[i] <= 127) {
			ascii_count++;
		} else {
			non_ascii_count++;
		}
	}
	return (ascii_count + 3) / 4 + non_ascii_count;
}

int SolersContextManager::_estimate_message_tokens(const Dictionary &p_message) {
	int total = estimate_tokens(String(p_message.get("role", String())));
	total += estimate_tokens(String(p_message.get("content", String())));
	total += estimate_tokens(String(p_message.get("name", String())));
	total += estimate_tokens(String(p_message.get("tool_call_id", String())));
	const Array tool_calls = p_message.get("tool_calls", Array());
	for (int i = 0; i < tool_calls.size(); i++) {
		const Dictionary call = tool_calls[i];
		total += estimate_tokens(String(call.get("name", String())));
		total += estimate_tokens(String(call.get("arguments", String())));
	}
	const Array attachments = p_message.get("attachments", Array());
	total += attachments.size() * MEDIA_TOKEN_ESTIMATE;
	return total;
}

int SolersContextManager::estimate_messages_tokens(const Array &p_messages) {
	int total = 0;
	for (int i = 0; i < p_messages.size(); i++) {
		total += _estimate_message_tokens(p_messages[i]);
	}
	return total;
}

String SolersContextManager::_truncate_text(const String &p_text, int p_max_tokens) {
	if (p_max_tokens <= 0) {
		return String();
	}
	int ascii_count = 0;
	int non_ascii_count = 0;
	int end = 0;
	for (int i = 0; i < p_text.length(); i++) {
		if (p_text[i] <= 127) {
			ascii_count++;
		} else {
			non_ascii_count++;
		}
		if ((ascii_count + 3) / 4 + non_ascii_count > p_max_tokens) {
			break;
		}
		end = i + 1;
	}
	return p_text.left(end);
}

int SolersContextManager::tool_result_token_budget(int p_context_window) {
	if (p_context_window <= 0) {
		return TOOL_RESULT_MAX_TOKENS;
	}
	return CLAMP(p_context_window / TOOL_RESULT_WINDOW_FRACTION, TOOL_RESULT_MIN_TOKENS, TOOL_RESULT_MAX_TOKENS);
}

Array SolersContextManager::repair_tool_pairing(const Array &p_messages) {
	Array repaired;
	Vector<String> open_ids; // calls of the assistant turn currently being answered
	Vector<String> open_names;
	HashSet<String> answered;

	// Stubs land immediately after the run of real results, keeping the
	// assistant turn and its answers adjacent the way providers require.
	auto close_open_calls = [&]() {
		for (int i = 0; i < open_ids.size(); i++) {
			if (!answered.has(open_ids[i])) {
				repaired.push_back(SolersLLMMessage::tool_result(open_ids[i], open_names[i], String::utf8(CANCELLED_TOOL_RESULT)));
			}
		}
		open_ids.clear();
		open_names.clear();
		answered.clear();
	};

	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		const String role = message.get("role", String());
		if (role == String(SolersLLMRole::TOOL)) {
			const String id = message.get("tool_call_id", String());
			// A result for an unknown or already-answered id is rejected just
			// as hard as a missing one, so it is dropped instead of forwarded.
			if (id.is_empty() || answered.has(id) || open_ids.find(id) < 0) {
				continue;
			}
			answered.insert(id);
			repaired.push_back(message);
			continue;
		}
		close_open_calls();
		repaired.push_back(message);
		if (role != String(SolersLLMRole::ASSISTANT)) {
			continue;
		}
		const Array tool_calls = message.get("tool_calls", Array());
		for (int call_index = 0; call_index < tool_calls.size(); call_index++) {
			const Dictionary call = tool_calls[call_index];
			const String id = call.get("id", String());
			if (!id.is_empty()) {
				open_ids.push_back(id);
				open_names.push_back(call.get("name", String()));
			}
		}
	}
	close_open_calls();
	return repaired;
}

Array SolersContextManager::_select_recent_user_messages(const Array &p_messages) {
	Array candidates;
	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		const String origin = message.get("origin", String());
		if (String(message.get("role", String())) == String(SolersLLMRole::USER) &&
				origin != "compaction_summary" && origin != "tool_capture" && origin != "solers_state" && origin != "solers_continuation" && origin != "background_job_delta") {
			candidates.push_back(message);
		}
	}

	Array reverse_selected;
	int remaining = COMPACT_USER_MESSAGE_MAX_TOKENS;
	for (int i = candidates.size() - 1; i >= 0 && remaining > 0; i--) {
		Dictionary message = Dictionary(candidates[i]).duplicate(true);
		const int tokens = _estimate_message_tokens(message);
		if (tokens <= remaining) {
			reverse_selected.push_back(message);
			remaining -= tokens;
			continue;
		}
		const int non_content_tokens = tokens - estimate_tokens(String(message.get("content", String())));
		message["content"] = _truncate_text(String(message.get("content", String())), MAX(0, remaining - non_content_tokens));
		message.erase("tool_calls");
		reverse_selected.push_back(message);
		break;
	}

	Array selected;
	for (int i = reverse_selected.size() - 1; i >= 0; i--) {
		selected.push_back(reverse_selected[i]);
	}
	return selected;
}

String SolersContextManager::_build_summary_text(const String &p_summary, const Dictionary &p_plan) {
	String text = String::utf8(COMPACTION_SUMMARY_PREFIX) + "\n" + p_summary.strip_edges();
	if (p_summary.strip_edges().is_empty()) {
		text += "(no summary available)";
	}

	const Array steps = p_plan.get("plan", Array());
	if (!steps.is_empty()) {
		text += "\n\n## Current plan";
		const String explanation = String(p_plan.get("explanation", String())).strip_edges();
		if (!explanation.is_empty()) {
			text += "\n" + explanation;
		}
		for (int i = 0; i < steps.size(); i++) {
			const Dictionary step = steps[i];
			text += vformat("\n- [%s] %s", String(step.get("status", "pending")), String(step.get("step", String())));
		}
	}
	return text;
}

void SolersContextManager::record_usage(int p_input_tokens, int p_covered_message_count) {
	if (p_input_tokens < 0 || p_covered_message_count < 0) {
		return;
	}
	authoritative_tokens = p_input_tokens;
	covered_message_count = p_covered_message_count;
	last_estimated_tokens = p_input_tokens;
}

int SolersContextManager::get_token_count_with_pending(const Array &p_messages, const String &p_system_prompt, const Array &p_tools) const {
	if (authoritative_tokens > 0 && covered_message_count <= p_messages.size()) {
		Array pending;
		for (int i = covered_message_count; i < p_messages.size(); i++) {
			pending.push_back(p_messages[i]);
		}
		return authoritative_tokens + estimate_messages_tokens(pending);
	}
	return estimate_tokens(p_system_prompt) + estimate_tokens(JSON::stringify(p_tools, "", false, true)) + estimate_messages_tokens(p_messages);
}

bool SolersContextManager::should_compact(int p_used_tokens, int p_context_window, int p_max_output_tokens) const {
	if (p_context_window <= 0 || p_max_output_tokens <= 0) {
		return false;
	}
	if (last_compacted_token_count >= 0 && p_used_tokens <= last_compacted_token_count) {
		return false;
	}
	// OpenCode COMPACTION_BUFFER: reserve min(20k, maxOutput), not the full
	// wire budget — otherwise a large (but capped) max_output still collapses
	// usable input toward zero on mid-size windows.
	static constexpr int COMPACTION_BUFFER = 20000;
	const int reserve = MIN(p_max_output_tokens, COMPACTION_BUFFER);
	return p_used_tokens + reserve >= p_context_window;
}

bool SolersContextManager::should_compact(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window, int p_max_output_tokens) const {
	return should_compact(get_token_count_with_pending(p_messages, p_system_prompt, p_tools), p_context_window, p_max_output_tokens);
}

bool SolersContextManager::is_overflow(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window) const {
	return p_context_window > 0 && get_token_count_with_pending(p_messages, p_system_prompt, p_tools) >= p_context_window;
}

Array SolersContextManager::prepare_request(const Array &p_messages, const String &p_system_prompt, const Array &p_tools) {
	// Repairing pairing is the only rewrite a projection may perform, and it
	// is a pure function of history: the same exchange always projects to the
	// same bytes, so consecutive requests share a prefix the provider can
	// keep serving from cache. Shrinking happens at compaction, never here.
	const Array projected = repair_tool_pairing(p_messages);
	last_estimated_tokens = estimate_tokens(p_system_prompt) + estimate_tokens(JSON::stringify(p_tools, "", false, true)) + estimate_messages_tokens(projected);
	return projected;
}

Dictionary SolersContextManager::apply_compaction(const Array &p_messages, const String &p_summary, const Dictionary &p_plan) {
	const int tokens_before = estimate_messages_tokens(p_messages);
	Array compacted = _select_recent_user_messages(p_messages);
	const int kept_user_message_count = compacted.size();
	const String context_summary = _build_summary_text(p_summary, p_plan);
	Dictionary summary_message = SolersLLMMessage::user(context_summary);
	summary_message["origin"] = "compaction_summary";
	compacted.push_back(summary_message);

	const int tokens_after = estimate_messages_tokens(compacted);
	authoritative_tokens = tokens_after;
	covered_message_count = compacted.size();
	last_estimated_tokens = tokens_after;
	last_compacted_token_count = tokens_after;
	compaction_count++;

	Dictionary result;
	result["messages"] = compacted;
	result["summary"] = p_summary.strip_edges();
	result["context_summary"] = context_summary;
	result["compacted_count"] = p_messages.size();
	result["tokens_before"] = tokens_before;
	result["tokens_after"] = tokens_after;
	result["kept_user_message_count"] = kept_user_message_count;
	return result;
}

Array SolersContextManager::shrink_compaction_history(const Array &p_messages, int p_attempt) const {
	static const double ratios[] = { 0.7, 0.5, 0.35 };
	if (p_messages.size() <= 1) {
		return p_messages.duplicate(true);
	}
	const int ratio_index = CLAMP(p_attempt - 1, 0, 2);
	const int token_budget = (int)((double)estimate_messages_tokens(p_messages) * ratios[ratio_index]);
	int start = p_messages.size();
	int tokens = 0;
	for (int i = p_messages.size() - 1; i >= 0; i--) {
		const int message_tokens = _estimate_message_tokens(p_messages[i]);
		if (tokens + message_tokens > token_budget) {
			break;
		}
		tokens += message_tokens;
		start = i;
	}
	if (start == 0) {
		start = 1;
	}
	while (start < p_messages.size() && String(Dictionary(p_messages[start]).get("role", String())) == String(SolersLLMRole::TOOL)) {
		start++;
	}
	Array result;
	for (int i = start; i < p_messages.size(); i++) {
		result.push_back(p_messages[i]);
	}
	return result;
}

void SolersContextManager::reset() {
	authoritative_tokens = 0;
	covered_message_count = 0;
	last_estimated_tokens = 0;
	compaction_count = 0;
	last_compacted_token_count = -1;
}
