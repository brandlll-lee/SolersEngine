/**************************************************************************/
/*  solers_context_manager.cpp                                            */
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

#include "solers_context_manager.h"

#include "core/io/json.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

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

String SolersContextManager::clamp_to_tokens(const String &p_text, int p_token_budget) {
	if (p_token_budget <= 0) {
		return String();
	}
	if (estimate_tokens(p_text) <= p_token_budget) {
		return p_text;
	}
	const String marker = "\n[...truncated...]\n";
	const int keep_chars = MAX(0, (p_token_budget - estimate_tokens(marker)) * 4);
	const int head = keep_chars / 2;
	const int tail = keep_chars - head;
	if (head + tail >= p_text.length()) {
		return p_text;
	}
	return p_text.substr(0, head) + marker + p_text.substr(p_text.length() - tail);
}

bool SolersContextManager::append_bounded(Array &r_results, const Dictionary &p_entry, int p_max_results, int p_token_budget, int &r_tokens) {
	const int tokens = estimate_tokens(JSON::stringify(p_entry));
	if (r_results.size() >= p_max_results || (!r_results.is_empty() && r_tokens + tokens > MAX(1, p_token_budget))) {
		return false;
	}
	r_results.push_back(p_entry);
	r_tokens += tokens;
	return true;
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

Array SolersContextManager::project_compacted(const Array &p_messages, const Dictionary &p_compaction) {
	if (p_compaction.is_empty()) {
		return p_messages.duplicate(true);
	}
	const String summary = String(p_compaction.get("summary", String())).strip_edges();
	const int64_t first_kept_event_id = p_compaction.get("first_kept_event_id", 0);
	if (summary.is_empty() || first_kept_event_id <= 0) {
		return p_messages.duplicate(true);
	}
	Array projected;
	projected.push_back(Dictionary({
			{ "role", MODEL_CONTEXT_ROLE },
			{ "origin", "compaction_summary" },
			{ "content", "Summary of earlier conversation:\n" + summary },
	}));
	for (const Variant &value : p_messages) {
		const Dictionary message = value;
		if (!(bool)message.get("ephemeral", false) && (int64_t)message.get("event_id", 0) >= first_kept_event_id) {
			projected.push_back(message);
		}
	}
	return repair_tool_pairing(projected);
}

Array SolersContextManager::project_tool_evidence(const Array &p_messages) {
	HashSet<String> completed_calls;
	HashMap<String, String> retained_evidence;
	int remaining_evidence_tokens = TOOL_RESULT_MAX_TOKENS;
	bool has_later_assistant = false;
	for (int i = p_messages.size() - 1; i >= 0; i--) {
		const Dictionary message = p_messages[i];
		const String role = message.get("role", String());
		if (role == String(SolersLLMRole::ASSISTANT)) {
			has_later_assistant = true;
		} else if (role == String(SolersLLMRole::TOOL) && has_later_assistant) {
			const String call_id = message.get("tool_call_id", String());
			if (!call_id.is_empty()) {
				completed_calls.insert(call_id);
				if (remaining_evidence_tokens > 0) {
					const String content = clamp_to_tokens(message.get("content", String()), remaining_evidence_tokens);
					retained_evidence[call_id] = content;
					remaining_evidence_tokens -= estimate_tokens(content);
				}
			}
		}
	}

	Array projected;
	for (int i = 0; i < p_messages.size(); i++) {
		Dictionary message = Dictionary(p_messages[i]).duplicate(true);
		const String role = message.get("role", String());
		if (role == String(SolersLLMRole::ASSISTANT)) {
			const Array calls = message.get("tool_calls", Array());
			Array active_calls;
			for (int call_index = 0; call_index < calls.size(); call_index++) {
				const Dictionary call = calls[call_index];
				if (!completed_calls.has(call.get("id", String()))) {
					active_calls.push_back(call);
				}
			}
			if (active_calls.is_empty()) {
				message.erase("tool_calls");
			} else {
				message["tool_calls"] = active_calls;
			}
		} else if (role == String(SolersLLMRole::TOOL)) {
			if (completed_calls.has(message.get("tool_call_id", String()))) {
				const String call_id = message.get("tool_call_id", String());
				const String *content = retained_evidence.getptr(call_id);
				if (!content) {
					continue;
				}
				message["content"] = *content;
				message["role"] = MODEL_CONTEXT_ROLE;
				message["origin"] = "tool_evidence";
				message.erase("tool_call_id");
				message.erase("name");
			}
		}
		projected.push_back(message);
	}
	return projected;
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
	return p_used_tokens + p_max_output_tokens >= p_context_window;
}

bool SolersContextManager::should_compact(const Array &p_messages, const String &p_system_prompt, int p_tool_tokens, int p_context_window, int p_max_output_tokens, int p_transient_tokens) {
	return should_compact(get_token_count_with_pending(p_messages, p_system_prompt, p_tool_tokens, p_transient_tokens), p_context_window, p_max_output_tokens);
}

void SolersContextManager::reset() {
	authoritative_tokens = 0;
	covered_message_count = 0;
	last_estimated_tokens = 0;
}
