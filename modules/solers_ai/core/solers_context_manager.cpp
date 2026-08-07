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

#include "core/templates/hash_map.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

const char *SolersContextManager::COMPACTION_SUMMARY_PREFIX =
		"The conversation so far has been compacted to free up context. What follows is your own working summary of this task — use it to continue your train of thought rather than starting over. Treat it as notes, not proof: where it says a step was done, tests passed, or a fix worked, verify that yourself before relying on it.";
const char *SolersContextManager::COMPACTION_INSTRUCTION =
		"You are about to run out of context. Write a first-person handoff note to yourself so you can seamlessly continue this task after the earlier conversation is cleared.\n\n"
		"Write the note as your own continuing train of thought, in present tense. Preserve the latest user request verbatim, active constraints, exact files and commands already used, verified results versus uncertainty, and the precise next action. Be concise and self-sufficient. Respond with text only and do not call tools.";
const char *SolersContextManager::CANCELLED_TOOL_RESULT =
		"{\"ok\":false,\"error\":{\"code\":\"TOOL_CANCELLED\",\"message\":\"This call never produced a result: the turn ended before it finished. Re-run it if its output is still needed.\",\"recoverable\":true}}";
const char *SolersContextManager::MODEL_CONTEXT_ROLE = "model_context";

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

Array SolersContextManager::repair_tool_pairing(const Array &p_messages) {
	Array repaired;
	HashMap<String, String> open_calls;
	auto close_open_calls = [&]() {
		for (const KeyValue<String, String> &call : open_calls) {
			repaired.push_back(SolersLLMMessage::tool_result(call.key, call.value, String::utf8(CANCELLED_TOOL_RESULT)));
		}
		open_calls.clear();
	};
	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		const String role = message.get("role", String());
		if (role == String(SolersLLMRole::TOOL)) {
			const String id = message.get("tool_call_id", String());
			if (open_calls.erase(id)) {
				repaired.push_back(message);
			}
			continue;
		}
		close_open_calls();
		repaired.push_back(message);
		const Array calls = role == String(SolersLLMRole::ASSISTANT) ? Array(message.get("tool_calls", Array())) : Array();
		for (int call_index = 0; call_index < calls.size(); call_index++) {
			const Dictionary call = calls[call_index];
			const String id = call.get("id", String());
			if (!id.is_empty()) {
				open_calls[id] = call.get("name", String());
			}
		}
	}
	close_open_calls();
	return repaired;
}

Array SolersContextManager::project_completed_turns(const Array &p_messages) {
	Array projected;
	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		const String role = message.get("role", String());
		if ((bool)message.get("ephemeral", false) || String(message.get("origin", String())) == "turn_checkpoint" || role == String(SolersLLMRole::TOOL)) {
			continue;
		}
		if (role == String(SolersLLMRole::ASSISTANT) && !Array(message.get("tool_calls", Array())).is_empty()) {
			continue;
		}
		projected.push_back(message);
	}
	return projected;
}

String SolersContextManager::_build_summary_text(const String &p_summary) {
	String text = String::utf8(COMPACTION_SUMMARY_PREFIX) + "\n" + p_summary.strip_edges();
	if (p_summary.strip_edges().is_empty()) {
		text += "(no summary available)";
	}
	return text;
}

void SolersContextManager::record_usage(int p_input_tokens, int p_covered_message_count, int p_transient_tokens) {
	if (p_input_tokens < 0 || p_covered_message_count < 0 || p_transient_tokens < 0) {
		return;
	}
	authoritative_tokens = MAX(0, p_input_tokens - p_transient_tokens);
	covered_message_count = p_covered_message_count;
	last_estimated_tokens = p_input_tokens;
}

int SolersContextManager::get_token_count_with_pending(const Array &p_messages, const String &p_system_prompt, int p_tool_tokens, int p_transient_tokens) {
	int total = 0;
	if (authoritative_tokens > 0 && covered_message_count <= p_messages.size()) {
		Array pending;
		for (int i = covered_message_count; i < p_messages.size(); i++) {
			pending.push_back(p_messages[i]);
		}
		total = authoritative_tokens + estimate_messages_tokens(pending) + p_transient_tokens;
	} else {
		total = estimate_tokens(p_system_prompt) + p_tool_tokens + estimate_messages_tokens(p_messages) + p_transient_tokens;
	}
	last_estimated_tokens = total;
	return total;
}

bool SolersContextManager::should_compact(int p_used_tokens, int p_context_window, int p_max_output_tokens) const {
	if (p_context_window <= 0 || p_max_output_tokens <= 0) {
		return false;
	}
	if (last_compacted_token_count >= 0 && p_used_tokens <= last_compacted_token_count) {
		return false;
	}
	return p_used_tokens + p_max_output_tokens >= p_context_window;
}

bool SolersContextManager::should_compact(const Array &p_messages, const String &p_system_prompt, int p_tool_tokens, int p_context_window, int p_max_output_tokens, int p_transient_tokens) {
	return should_compact(get_token_count_with_pending(p_messages, p_system_prompt, p_tool_tokens, p_transient_tokens), p_context_window, p_max_output_tokens);
}

Dictionary SolersContextManager::apply_compaction(const Array &p_messages, const String &p_summary, int p_token_budget) {
	const int tokens_before = estimate_messages_tokens(p_messages);
	const String context_summary = _build_summary_text(p_summary);
	Dictionary summary_message;
	summary_message["role"] = MODEL_CONTEXT_ROLE;
	summary_message["content"] = context_summary;
	summary_message["origin"] = "compaction_summary";
	int remaining = MAX(0, p_token_budget - _estimate_message_tokens(summary_message));
	Array recent_users;
	for (int i = p_messages.size() - 1; i >= 0; i--) {
		Dictionary message = p_messages[i];
		if (String(message.get("role", String())) != SolersLLMRole::USER || (bool)message.get("ephemeral", false)) {
			continue;
		}
		message.erase("attachments");
		const int tokens = _estimate_message_tokens(message);
		if (tokens > remaining) {
			break;
		}
		recent_users.push_back(message);
		remaining -= tokens;
	}
	Array compacted;
	for (int i = recent_users.size() - 1; i >= 0; i--) {
		compacted.push_back(recent_users[i]);
	}
	compacted.push_back(summary_message);
	const int tokens_after = estimate_messages_tokens(compacted);
	authoritative_tokens = tokens_after;
	covered_message_count = compacted.size();
	last_estimated_tokens = tokens_after;
	last_compacted_token_count = tokens_after;
	compaction_count++;

	Dictionary result;
	result["messages"] = compacted;
	result["tokens_before"] = tokens_before;
	result["tokens_after"] = tokens_after;
	return result;
}

void SolersContextManager::reset() {
	authoritative_tokens = 0;
	covered_message_count = 0;
	last_estimated_tokens = 0;
	compaction_count = 0;
	last_compacted_token_count = -1;
}
