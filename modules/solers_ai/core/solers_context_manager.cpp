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

#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

const char *SolersContextManager::COMPACTION_SUMMARY_PREFIX =
		"Task continuation: preserve the user's goal, durable constraints, decisions, and precise next action. The structured checkpoint below is authoritative for completed engine work and failed operations.";
const char *SolersContextManager::CANCELLED_TOOL_RESULT =
		"{\"ok\":false,\"error\":{\"code\":\"TOOL_CANCELLED\",\"message\":\"This call never produced a result: the turn ended before it finished. Re-run it if its output is still needed.\",\"recoverable\":true}}";
const char *SolersContextManager::MODEL_CONTEXT_ROLE = "model_context";
static constexpr int SOLERS_PRUNE_PROTECT_TOKENS = 40000;
static constexpr int SOLERS_PRUNE_MINIMUM_TOKENS = 20000;

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
	Dictionary latest_summary, facts, failures;
	int projection_start = 0;
	for (int i = p_messages.size() - 1; i >= 0; i--) {
		const Dictionary message = p_messages[i];
		if (String(message.get("origin", String())) == "compaction_summary") {
			latest_summary = message;
			projection_start = i + 1;
			break;
		}
	}
	Array conversation;
	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		const String role = message.get("role", String());
		const String origin = message.get("origin", String());
		if ((bool)message.get("ephemeral", false)) {
			continue;
		}
		if (origin == "compaction_summary") {
			continue;
		}
		if (origin == "turn_checkpoint") {
			Ref<JSON> json;
			json.instantiate();
			if (json->parse(message.get("content", String())) == OK && json->get_data().get_type() == Variant::DICTIONARY) {
				const Dictionary checkpoint = json->get_data();
				facts.merge(checkpoint.get("facts", Dictionary()), true);
				failures.merge(checkpoint.get("failures", Dictionary()), true);
			}
			continue;
		}
		if (i >= projection_start && role == String(SolersLLMRole::USER)) {
			conversation.push_back(message);
			continue;
		}
		if (i >= projection_start && role == String(SolersLLMRole::ASSISTANT)) {
			const String content = message.get("content", String());
			if (!content.is_empty()) {
				conversation.push_back(Dictionary({ { "role", SolersLLMRole::ASSISTANT }, { "content", content } }));
			}
			continue;
		}
		if (role != String(SolersLLMRole::TOOL)) {
			continue;
		}
		Ref<JSON> json;
		json.instantiate();
		if (json->parse(message.get("content", String())) != OK || json->get_data().get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary result = json->get_data();
		const String tool = message.get("name", String());
		if (!(bool)result.get("ok", false)) {
			failures[tool] = result.get("error", Dictionary());
			continue;
		}
		failures.erase(tool);
		const Dictionary receipt = Dictionary(Dictionary(result.get("data", Dictionary())).get("mutation", Dictionary())).get("receipt", Dictionary());
		const Dictionary scene = receipt.get("scene_after", Dictionary());
		if (!scene.is_empty()) {
			facts["scene"] = scene;
		}
		const Array resources = receipt.get("resources_after", Array());
		for (int resource_index = 0; resource_index < resources.size(); resource_index++) {
			const Dictionary resource = resources[resource_index];
			const String path = resource.get("path", String());
			if (!path.is_empty()) {
				facts[path] = resource;
			}
		}
	}
	Array projected;
	if (!latest_summary.is_empty()) {
		projected.push_back(latest_summary);
	}
	projected.append_array(conversation);
	if (!facts.is_empty() || !failures.is_empty()) {
		projected.push_back(Dictionary({ { "role", MODEL_CONTEXT_ROLE }, { "origin", "turn_checkpoint" }, { "content", JSON::stringify(Dictionary({ { "facts", facts }, { "failures", failures } }), "", false, true) } }));
	}
	return projected;
}

int SolersContextManager::prune_old_tool_outputs(Array &r_messages) {
	Array candidates;
	int protected_tokens = 0;
	int saved_tokens = 0;
	for (int i = r_messages.size() - 1; i >= 0; i--) {
		const Dictionary message = r_messages[i];
		if (String(message.get("role", String())) != String(SolersLLMRole::TOOL) || String(message.get("name", String())) == "skill.read") {
			continue;
		}
		const String content = message.get("content", String());
		const int tokens = estimate_tokens(content);
		if (protected_tokens < SOLERS_PRUNE_PROTECT_TOKENS) {
			protected_tokens += tokens;
			continue;
		}
		Ref<JSON> json;
		json.instantiate();
		const Dictionary result = json->parse(content) == OK && json->get_data().get_type() == Variant::DICTIONARY ? Dictionary(json->get_data()) : Dictionary();
		const Dictionary receipt = Dictionary(Dictionary(result.get("data", Dictionary())).get("mutation", Dictionary())).get("receipt", Dictionary());
		if (!receipt.is_empty()) {
			continue;
		}
		const String replacement = JSON::stringify(Dictionary({ { "pruned", true }, { "original_sha256", content.sha256_text() } }));
		const int saving = tokens - estimate_tokens(replacement);
		if (saving > 0) {
			candidates.push_back(Dictionary({ { "index", i }, { "content", replacement } }));
			saved_tokens += saving;
		}
	}
	if (saved_tokens < SOLERS_PRUNE_MINIMUM_TOKENS) {
		return 0;
	}
	for (int i = 0; i < candidates.size(); i++) {
		const Dictionary candidate = candidates[i];
		const int index = candidate.get("index", -1);
		Dictionary message = r_messages[index];
		message["content"] = candidate.get("content", String());
		message.erase("attachments");
		r_messages[index] = message;
	}
	authoritative_tokens = 0;
	covered_message_count = 0;
	return saved_tokens;
}

String SolersContextManager::_build_summary_text(const String &p_summary) {
	return String::utf8(COMPACTION_SUMMARY_PREFIX) + "\n" + p_summary.strip_edges();
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

Dictionary SolersContextManager::apply_compaction(const Array &p_messages, const String &p_summary, int p_token_budget) {
	const int tokens_before = estimate_messages_tokens(p_messages);
	Dictionary summary_message;
	summary_message["role"] = MODEL_CONTEXT_ROLE;
	summary_message["content"] = _build_summary_text(p_summary);
	summary_message["origin"] = "compaction_summary";

	int active_turn_id = 0;
	int latest_user_index = -1;
	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		if (String(message.get("role", String())) != String(SolersLLMRole::USER) || (bool)message.get("ephemeral", false)) {
			continue;
		}
		const int message_turn_id = message.get("turn_id", 0);
		if (message_turn_id > active_turn_id) {
			active_turn_id = message_turn_id;
			latest_user_index = i;
		} else if (message_turn_id == active_turn_id) {
			latest_user_index = i;
		}
	}

	Array compacted;
	compacted.push_back(summary_message);
	const Array projection = project_completed_turns(p_messages);
	for (int i = 0; i < projection.size(); i++) {
		const Dictionary message = projection[i];
		if (String(message.get("origin", String())) == "turn_checkpoint") {
			compacted.push_back(message);
		}
	}
	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		if (String(message.get("role", String())) == String(SolersLLMRole::USER) && (int)message.get("turn_id", 0) == active_turn_id && !(bool)message.get("ephemeral", false)) {
			compacted.push_back(message);
		}
	}

	if (p_token_budget > 0 && latest_user_index >= 0) {
		Array tail;
		for (int i = latest_user_index + 1; i < p_messages.size(); i++) {
			const Dictionary message = p_messages[i];
			const String role = message.get("role", String());
			if (!(bool)message.get("ephemeral", false) && (role == String(SolersLLMRole::ASSISTANT) || role == String(SolersLLMRole::TOOL))) {
				tail.push_back(message);
			}
		}
		tail = repair_tool_pairing(tail);
		const int tail_budget = MAX(0, p_token_budget - estimate_messages_tokens(compacted));
		int preserve_from = tail.size();
		int tail_tokens = 0;
		while (preserve_from > 0) {
			const int cost = _estimate_message_tokens(tail[preserve_from - 1]);
			if (tail_tokens + cost > tail_budget) {
				break;
			}
			tail_tokens += cost;
			preserve_from--;
		}
		while (preserve_from < tail.size() && String(Dictionary(tail[preserve_from]).get("role", String())) == String(SolersLLMRole::TOOL)) {
			preserve_from++;
		}
		compacted.append_array(tail.slice(preserve_from));
	}

	const int tokens_after = estimate_messages_tokens(compacted);
	const bool accepted = active_turn_id > 0 && tokens_after < tokens_before && (p_token_budget <= 0 || tokens_after <= p_token_budget);
	Dictionary result;
	result["accepted"] = accepted;
	result["target_tokens"] = p_token_budget;
	result["tokens_before"] = tokens_before;
	result["tokens_after"] = tokens_after;
	if (accepted) {
		result["messages"] = compacted;
		// Provider usage is authoritative for wire size. Until the next response,
		// recount only the accepted local projection.
		authoritative_tokens = 0;
		covered_message_count = 0;
		last_estimated_tokens = tokens_after;
		compaction_count++;
	}
	return result;
}

void SolersContextManager::reset() {
	authoritative_tokens = 0;
	covered_message_count = 0;
	last_estimated_tokens = 0;
	compaction_count = 0;
}
