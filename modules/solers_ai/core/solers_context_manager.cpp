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
#include "modules/solers_ai/llm/solers_llm_message.h"

const char *SolersContextManager::TOOL_RESULT_PLACEHOLDER =
		"[Old tool result content cleared]";
const char *SolersContextManager::COMPACTION_SUMMARY_PREFIX =
		"The conversation so far has been compacted to free up context. What follows is your own working summary of this task — use it to continue your train of thought rather than starting over. Treat it as notes, not proof: where it says a step was done, tests passed, or a fix worked, verify that yourself before relying on it.";
const char *SolersContextManager::COMPACTION_INSTRUCTION =
		"You are about to run out of context. Write a first-person handoff note to yourself so you can seamlessly continue this task after the earlier conversation is cleared.\n\n"
		"Write the note as your own continuing train of thought, in present tense. Preserve the latest user request verbatim, active constraints, exact files and commands already used, verified results versus uncertainty, and the precise next action. Be concise and self-sufficient. Respond with text only and do not call tools.";

static bool _is_generated_evidence(const Dictionary &p_attachment) {
	const String source = p_attachment.get("source", String());
	return source == "tool_capture" || source == "asset_catalog_preview";
}

static void _strip_generated_evidence(Dictionary &r_message) {
	const Array attachments = r_message.get("attachments", Array());
	Array kept;
	for (int i = 0; i < attachments.size(); i++) {
		const Dictionary attachment = attachments[i];
		if (!_is_generated_evidence(attachment)) {
			kept.push_back(attachment);
		}
	}
	if (kept.is_empty()) {
		r_message.erase("attachments");
	} else {
		r_message["attachments"] = kept;
	}
}

// Tool-generated images are one-step evidence: keep every image produced
// after the latest assistant response, then remove it once the model has seen
// it. User attachments are durable. Contact sheets are replaceable, so only
// the newest pending one is needed.
static Array _project_evidence_lifecycle(const Array &p_messages) {
	int last_assistant_message = -1;
	int latest_state_message = -1;
	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		if (String(message.get("role", String())) == String(SolersLLMRole::ASSISTANT)) {
			last_assistant_message = i;
		}
		if (String(message.get("origin", String())) == "solers_state") {
			latest_state_message = i;
		}
	}

	int latest_pending_catalog_message = -1;
	for (int i = last_assistant_message + 1; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		const Array attachments = message.get("attachments", Array());
		for (int j = 0; j < attachments.size(); j++) {
			const Dictionary attachment = attachments[j];
			if (String(attachment.get("source", String())) == "asset_catalog_preview") {
				latest_pending_catalog_message = i;
			}
		}
	}

	Array projected;
	for (int i = 0; i < p_messages.size(); i++) {
		Dictionary message = Dictionary(p_messages[i]).duplicate(true);
		if (String(message.get("origin", String())) == "solers_state" && i != latest_state_message) {
			continue;
		}
		const Array attachments = message.get("attachments", Array());
		if (!attachments.is_empty()) {
			Array kept;
			for (int j = 0; j < attachments.size(); j++) {
				const Dictionary attachment = attachments[j];
				const String source = attachment.get("source", String());
				if (source == "tool_capture") {
					if (i > last_assistant_message) {
						kept.push_back(attachment);
					}
					continue;
				}
				if (source == "asset_catalog_preview") {
					if (i == latest_pending_catalog_message) {
						kept.push_back(attachment);
					}
					continue;
				}
				kept.push_back(attachment);
			}
			if (kept.is_empty()) {
				message.erase("attachments");
			} else {
				message["attachments"] = kept;
			}
		}
		projected.push_back(message);
	}
	return projected;
}

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
		return TOOL_RESULT_FALLBACK_TOKENS;
	}
	return MAX(TOOL_RESULT_MIN_TOKENS, p_context_window / TOOL_RESULT_WINDOW_FRACTION);
}

// Suffix of p_text whose estimated token count is at most p_max_tokens.
static String _tail_by_tokens(const String &p_text, int p_max_tokens) {
	if (p_max_tokens <= 0) {
		return String();
	}
	int ascii_count = 0;
	int non_ascii_count = 0;
	int start = p_text.length();
	for (int i = p_text.length() - 1; i >= 0; i--) {
		if (p_text[i] <= 127) {
			ascii_count++;
		} else {
			non_ascii_count++;
		}
		if ((ascii_count + 3) / 4 + non_ascii_count > p_max_tokens) {
			break;
		}
		start = i;
	}
	return p_text.substr(start);
}

String SolersContextManager::middle_truncate(const String &p_text, int p_max_tokens) {
	const int total_tokens = estimate_tokens(p_text);
	if (p_max_tokens <= 0 || total_tokens <= p_max_tokens) {
		return p_text;
	}
	const int keep_each_side = p_max_tokens / 2;
	const String head = _truncate_text(p_text, keep_each_side);
	const String tail = _tail_by_tokens(p_text, keep_each_side);
	return head +
			vformat("\n...[Solers: middle of this output elided, %d of ~%d tokens kept. Re-run the tool with tighter limits for the full data.]...\n",
					p_max_tokens, total_tokens) +
			tail;
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

Array SolersContextManager::_micro_project(const Array &p_messages) const {
	Array result = p_messages.duplicate(true);
	for (int i = 0; i < result.size() && i < micro_cutoff; i++) {
		Dictionary message = result[i];
		if (String(message.get("role", String())) != String(SolersLLMRole::TOOL) ||
				String(message.get("retention", String())) != "ephemeral") {
			continue;
		}
		message["content"] = TOOL_RESULT_PLACEHOLDER;
		message.erase("attachments");
		result[i] = message;
	}
	return result;
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

bool SolersContextManager::should_compact(int p_used_tokens, int p_context_window) const {
	if (p_context_window <= 0) {
		return false;
	}
	if (last_compacted_token_count >= 0 && p_used_tokens <= last_compacted_token_count) {
		return false;
	}
	if ((double)p_used_tokens >= (double)p_context_window * COMPACTION_TRIGGER_RATIO) {
		return true;
	}
	return RESERVED_CONTEXT_TOKENS > 0 &&
			RESERVED_CONTEXT_TOKENS < p_context_window &&
			p_used_tokens + RESERVED_CONTEXT_TOKENS >= p_context_window;
}

bool SolersContextManager::should_compact(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window) const {
	return should_compact(get_token_count_with_pending(p_messages, p_system_prompt, p_tools), p_context_window);
}

bool SolersContextManager::is_overflow(const Array &p_messages, const String &p_system_prompt, const Array &p_tools, int p_context_window) const {
	return p_context_window > 0 && get_token_count_with_pending(p_messages, p_system_prompt, p_tools) >= p_context_window;
}

Array SolersContextManager::prepare_request(const Array &p_messages, const String &p_system_prompt, const Array &p_tools) {
	last_estimated_tokens = get_token_count_with_pending(p_messages, p_system_prompt, p_tools);
	int next_cutoff = 0;
	for (int i = p_messages.size() - 1; i >= 0; i--) {
		if (String(Dictionary(p_messages[i]).get("role", String())) == String(SolersLLMRole::ASSISTANT)) {
			next_cutoff = i;
			break;
		}
	}
	if (next_cutoff != micro_cutoff) {
		micro_cutoff = next_cutoff;
		micro_compaction_count++;
	}
	return _project_evidence_lifecycle(_micro_project(p_messages));
}

Dictionary SolersContextManager::apply_compaction(const Array &p_messages, const String &p_summary, const Dictionary &p_plan) {
	const int tokens_before = estimate_messages_tokens(p_messages);
	Array compacted = _select_recent_user_messages(p_messages);
	const int kept_user_message_count = compacted.size();
	Dictionary latest_state;
	for (int i = p_messages.size() - 1; i >= 0; i--) {
		const Dictionary message = p_messages[i];
		if (String(message.get("origin", String())) == "solers_state") {
			latest_state = message.duplicate(true);
			_strip_generated_evidence(latest_state);
			break;
		}
	}
	if (!latest_state.is_empty()) {
		compacted.push_back(latest_state);
	}
	const String context_summary = _build_summary_text(p_summary, p_plan);
	Dictionary summary_message = SolersLLMMessage::user(context_summary);
	summary_message["origin"] = "compaction_summary";
	compacted.push_back(summary_message);

	const int tokens_after = estimate_messages_tokens(compacted);
	authoritative_tokens = tokens_after;
	covered_message_count = compacted.size();
	last_estimated_tokens = tokens_after;
	last_compacted_token_count = tokens_after;
	micro_cutoff = 0;
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
	micro_cutoff = 0;
	last_estimated_tokens = 0;
	micro_compaction_count = 0;
	compaction_count = 0;
	last_compacted_token_count = -1;
}
