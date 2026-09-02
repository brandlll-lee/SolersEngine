/**************************************************************************/
/*  solers_agent_transcript.cpp                                           */
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

#include "solers_agent_session.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "scene/main/node.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_retry.h"
#include "modules/solers_ai/llm/solers_models_dev.h"

String SolersAgentSession::_make_session_id() const {
	return OS::get_singleton()->get_unique_id() + "-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
}

static void _solers_finish_timeline_tool(Dictionary &r_call, bool p_ok, const String &p_error, int p_duration) {
	r_call["finished"] = true;
	r_call["ok"] = p_ok;
	r_call["error_message"] = p_error;
	r_call["duration_msec"] = p_duration;
}

static Dictionary _solers_restore_model_context(const Dictionary &p_event) {
	Dictionary message;
	message["role"] = SolersContextManager::MODEL_CONTEXT_ROLE;
	for (const char *field : { "content", "origin", "ephemeral", "attachments" }) {
		if (p_event.has(field)) {
			message[field] = p_event[field];
		}
	}
	return message;
}

static Array _solers_restore_attachment_paths(const Array &p_attachments) {
	Array restored;
	const String attachment_dir = solers_session_dir().path_join("attachments");
	for (const Variant &item : p_attachments) {
		if (item.get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary attachment = Dictionary(item).duplicate(true);
		const String sha = String(attachment.get("content_sha256", String())).strip_edges().to_lower();
		if (!sha.is_empty()) {
			const String path = attachment_dir.path_join(sha + ".png");
			if (FileAccess::exists(path) && FileAccess::get_sha256(path) == sha) {
				attachment["local_path"] = path;
			}
		}
		restored.push_back(attachment);
	}
	return restored;
}

static void _solers_project_timeline_event(const Dictionary &p_event, Array &r_entries, HashMap<String, Dictionary> &r_open_tools) {
	const String type = p_event.get("event_type", String());
	if (type == "message") {
		const String role = p_event.get("role", String());
		const String content = p_event.get("content", String());
		const String reasoning = p_event.get("reasoning", String());
		if ((role != SolersLLMRole::USER || !solers_transcript_is_human_message(p_event)) && role != SolersLLMRole::ASSISTANT) {
			return;
		}
		Dictionary entry;
		entry["event_id"] = p_event.get("event_id", 0);
		entry["role"] = role;
		entry["content"] = content;
		for (const char *field : { "reasoning", "attachments", "mentions", "origin", "wall", "session_revision" }) {
			if (p_event.has(field)) {
				entry[field] = p_event[field];
			}
		}
		if (entry.has("attachments")) {
			entry["attachments"] = _solers_restore_attachment_paths(entry["attachments"]);
		}
		Array calls = Array(p_event.get("tool_calls", Array())).duplicate(true);
		for (int i = calls.size() - 1; i >= 0; i--) {
			const Dictionary call = calls[i];
			if (!(bool)call.get("timeline_visible", true)) {
				calls.remove_at(i);
			}
		}
		if (!calls.is_empty()) {
			entry["tool_calls"] = calls;
			for (const Variant &item : calls) {
				Dictionary call = item;
				r_open_tools[call.get("id", String())] = call;
			}
		}
		if (!content.is_empty() || !reasoning.is_empty() || !calls.is_empty() || !Array(entry.get("attachments", Array())).is_empty()) {
			r_entries.push_back(entry);
		}
		return;
	}
	if (type == "tool_result") {
		const String call_id = p_event.get("call_id", String());
		if (call_id.is_empty()) {
			return;
		}
		Dictionary *call = r_open_tools.getptr(call_id);
		if (call) {
			const bool ok = p_event.get("ok", false);
			_solers_finish_timeline_tool(*call, ok, ok ? String() : String(Dictionary(p_event.get("error", Dictionary())).get("message", String())), p_event.get("duration_msec", 0));
		}
		r_open_tools.erase(call_id);
		return;
	}
	if (type == "context_compaction" || type == "context.apply_compaction") {
		Dictionary entry;
		entry["event_id"] = p_event.get("event_id", 0);
		entry["compaction_id"] = p_event.get("compaction_id", entry["event_id"]);
		entry["role"] = "context_compaction";
		entry["phase"] = type == "context.apply_compaction" ? "completed" : p_event.get("phase", "completed");
		for (int i = r_entries.size() - 1; i >= 0; i--) {
			const Dictionary existing = r_entries[i];
			if (String(existing.get("role", String())) == "context_compaction" && (int64_t)existing.get("compaction_id", -1) == (int64_t)entry["compaction_id"]) {
				entry["event_id"] = existing.get("event_id", entry["event_id"]);
				r_entries[i] = entry;
				return;
			}
		}
		r_entries.push_back(entry);
		return;
	}
	if (type == "turn_outcome") {
		for (KeyValue<String, Dictionary> &call : r_open_tools) {
			_solers_finish_timeline_tool(call.value, false, "Interrupted", 0);
		}
		r_open_tools.clear();
		const String outcome = p_event.get("outcome", String());
		if (outcome != "completed") {
			Dictionary entry;
			entry["event_id"] = p_event.get("event_id", 0);
			entry["role"] = "turn_outcome";
			entry["content"] = p_event.get("message", String());
			r_entries.push_back(entry);
		}
	}
}

Dictionary SolersAgentSession::_read_transcript_state(const String &p_project_path, const String &p_session_id) const {
	Array restored;
	Array restored_timeline;
	HashMap<String, Dictionary> restored_open_tools;
	Array restored_background_assets;
	Dictionary restored_plan;
	Dictionary restored_last_request_usage;
	String restored_outcome;
	int restored_compaction_count = 0;
	int restored_turn_id = 0;
	uint64_t restored_authored_revision = 0;
	Array restored_reversals;
	Array restored_added_tool_names;
	Dictionary pending_rewind;
	Array committed_rewinds;
	if (p_project_path.is_empty() || p_session_id.is_empty()) {
		Dictionary empty;
		empty["messages"] = restored;
		empty["timeline_entries"] = restored_timeline;
		return empty;
	}
	HashSet<int64_t> active_event_ids;
	Vector<int64_t> active_event_order;
	struct ProjectionScan {
		const String *project_path = nullptr;
		const String *session_id = nullptr;
		HashSet<int64_t> *active_ids = nullptr;
		Vector<int64_t> *active_order = nullptr;
		Dictionary *pending_rewind = nullptr;
		Array *committed_rewinds = nullptr;
		int64_t sequence = 0;
		int restored_turn_id = 0;
		uint64_t restored_revision = 0;
	} projection;
	projection.project_path = &p_project_path;
	projection.session_id = &p_session_id;
	projection.active_ids = &active_event_ids;
	projection.active_order = &active_event_order;
	projection.pending_rewind = &pending_rewind;
	projection.committed_rewinds = &committed_rewinds;
	solers_transcript_foreach_session(p_session_id, &projection, [](void *p_userdata, const String &p_record) -> bool {
		ProjectionScan &scan = *static_cast<ProjectionScan *>(p_userdata);
		Dictionary event;
		if (!solers_transcript_parse_record(p_record.strip_edges(), event) || String(event.get("project_path", String())) != *scan.project_path || String(event.get("session_id", String())) != *scan.session_id) {
			return true;
		}
		scan.sequence = MAX(scan.sequence + 1, (int64_t)event.get("event_id", 0));
		const int64_t event_id = event.get("event_id", scan.sequence);
		scan.restored_turn_id = MAX(scan.restored_turn_id, (int)event.get("turn_id", 0));
		scan.restored_revision = MAX(scan.restored_revision, (uint64_t)(int64_t)event.get("session_revision", 0));
		const String type = event.get("event_type", String());
		if (type == "rewind_prepared") {
			*scan.pending_rewind = event.get("transaction", Dictionary());
			return true;
		}
		if (type == "rewind_aborted") {
			scan.pending_rewind->clear();
			return true;
		}
		if (type == "session_rewind") {
			const int64_t target_event_id = event.get("target_event_id", 0);
			while (!scan.active_order->is_empty() && scan.active_order->get(scan.active_order->size() - 1) >= target_event_id) {
				scan.active_ids->erase(scan.active_order->get(scan.active_order->size() - 1));
				scan.active_order->remove_at(scan.active_order->size() - 1);
			}
			const Dictionary transaction = event.get("transaction", Dictionary());
			if (!transaction.is_empty()) {
				scan.committed_rewinds->push_back(transaction);
			}
			scan.pending_rewind->clear();
			return true;
		}
		scan.active_ids->insert(event_id);
		scan.active_order->push_back(event_id);
		return true;
	});
	transcript_event_sequence = projection.sequence;
	restored_turn_id = projection.restored_turn_id;
	restored_authored_revision = projection.restored_revision;
	struct ScanState {
		const String *project_path = nullptr;
		const String *session_id = nullptr;
		Array *restored = nullptr;
		Array *restored_timeline = nullptr;
		HashMap<String, Dictionary> *restored_open_tools = nullptr;
		Array *restored_background_assets = nullptr;
		Dictionary *restored_plan = nullptr;
		Dictionary *restored_last_request_usage = nullptr;
		String *restored_outcome = nullptr;
		int *restored_compaction_count = nullptr;
		const HashSet<int64_t> *active_event_ids = nullptr;
		int64_t replay_sequence = 0;
		Array *restored_reversals = nullptr;
		Array *restored_added_tool_names = nullptr;
	} restore_state;
	restore_state.project_path = &p_project_path;
	restore_state.session_id = &p_session_id;
	restore_state.restored = &restored;
	restore_state.restored_timeline = &restored_timeline;
	restore_state.restored_open_tools = &restored_open_tools;
	restore_state.restored_background_assets = &restored_background_assets;
	restore_state.restored_plan = &restored_plan;
	restore_state.restored_last_request_usage = &restored_last_request_usage;
	restore_state.restored_outcome = &restored_outcome;
	restore_state.restored_compaction_count = &restored_compaction_count;
	restore_state.active_event_ids = &active_event_ids;
	restore_state.restored_reversals = &restored_reversals;
	restore_state.restored_added_tool_names = &restored_added_tool_names;
	solers_transcript_foreach_session(p_session_id, &restore_state, [](void *p_userdata, const String &p_record) -> bool {
		ScanState &scan = *static_cast<ScanState *>(p_userdata);
		const String line = p_record.strip_edges();
		if (line.is_empty()) {
			return true;
		}

		Dictionary event;
		if (!solers_transcript_parse_record(line, event)) {
			return true;
		}
		if (String(event.get("project_path", String())) != *scan.project_path || String(event.get("session_id", String())) != *scan.session_id) {
			return true;
		}
		scan.replay_sequence = MAX(scan.replay_sequence + 1, (int64_t)event.get("event_id", 0));
		if (!event.has("event_id")) {
			event["event_id"] = scan.replay_sequence;
		}
		if (!scan.active_event_ids->has(event.get("event_id", 0))) {
			return true;
		}

		const String event_type = event.get("event_type", String());
		if ((event_type == "context.apply_compaction" || event_type == "context_compaction") && String(event.get("phase", event_type == "context.apply_compaction" ? "completed" : String())) == "completed") {
			(*scan.restored_compaction_count)++;
		}
		const Dictionary usage = event.get("usage", Dictionary());
		if (!usage.is_empty()) {
			*scan.restored_last_request_usage = usage.duplicate(true);
		}
		_solers_project_timeline_event(event, *scan.restored_timeline, *scan.restored_open_tools);
		if (event_type == "checkpoint_created") {
			Dictionary checkpoint = event.get("checkpoint", Dictionary());
			scan.restored_reversals->push_back(checkpoint.duplicate(true));
			return true;
		}
		if (event_type == "checkpoint_cleared") {
			scan.restored_reversals->clear();
			const Dictionary barrier = event.get("barrier", Dictionary());
			if (!barrier.is_empty()) {
				scan.restored_reversals->push_back(barrier.duplicate(true));
			}
			return true;
		}
		if (event_type == "checkpoint_consumed") {
			const String reversal_id = event.get("reversal_id", String());
			for (int i = scan.restored_reversals->size() - 1; i >= 0; i--) {
				if (reversal_id.is_empty() || String(Dictionary((*scan.restored_reversals)[i]).get("id", String())) == reversal_id) {
					scan.restored_reversals->remove_at(i);
					break;
				}
			}
			return true;
		}
		if (event_type == "tool_result") {
			const Array added_tool_names = event.get("added_tool_names", Array());
			for (const Variant &value : added_tool_names) {
				const String model_name = String(value).strip_edges();
				if (!model_name.is_empty() && !scan.restored_added_tool_names->has(model_name)) {
					scan.restored_added_tool_names->push_back(model_name);
				}
			}
			const String call_id = event.get("call_id", String());
			const String tool = event.get("tool", String());
			const String delivered = event.get("content", String());
			if (!call_id.is_empty() && !delivered.is_empty()) {
				scan.restored->push_back(SolersLLMMessage::tool_result(call_id, tool, delivered, Array(), added_tool_names));
			}
			return true;
		}
		if (event_type == "plan_updated") {
			(*scan.restored_plan)["explanation"] = event.get("explanation", String());
			(*scan.restored_plan)["plan"] = event.get("plan", Array());
			return true;
		}
		if (event_type == "context.apply_compaction" || (event_type == "context_compaction" && String(event.get("phase", String())) == "completed")) {
			const Array compacted = event.get("messages", Array());
			if (!compacted.is_empty()) {
				*scan.restored = compacted.duplicate(true);
			}
			const Dictionary plan = event.get("current_plan", Dictionary());
			if (!plan.is_empty()) {
				*scan.restored_plan = plan.duplicate(true);
			}
			return true;
		}
		if (event_type == "turn_outcome") {
			*scan.restored_outcome = event.get("outcome", String());
			scan.restored_plan->clear();
			*scan.restored = SolersContextManager::project_completed_turns(*scan.restored);
			return true;
		}
		if (event_type == "background_asset_delivery") {
			scan.restored_background_assets->push_back(event);
			return true;
		}
		if (event_type == "background_asset_consumed") {
			const String asset_id = event.get("asset_id", String());
			for (int i = scan.restored_background_assets->size() - 1; i >= 0; i--) {
				if (String(Dictionary((*scan.restored_background_assets)[i]).get("asset_id", String())) == asset_id) {
					scan.restored_background_assets->remove_at(i);
				}
			}
			return true;
		}
		if (event_type == "model_context") {
			scan.restored->push_back(_solers_restore_model_context(event));
			return true;
		}

		const String role = event.get("role", String());
		const String content = event.get("content", String());
		const Array tool_calls = event.get("tool_calls", Array());
		const String reasoning = event.get("reasoning", String());
		if (role == SolersLLMRole::USER) {
			if (content.is_empty()) {
				return true;
			}
			if (!solers_transcript_is_human_message(event)) {
				scan.restored->push_back(_solers_restore_model_context(event));
				return true;
			}
			Array mentions = event.get("mentions", Array());
			const bool had_block = content.find("[Selected Solers context]") >= 0;
			const String display = SolersMention::strip_prompt_block(content);
			if (mentions.is_empty() && had_block) {
				mentions = SolersMention::parse(display);
			}
			String model_content = had_block ? content : (display + SolersMention::prompt_block(mentions));
			const Array attachments = _solers_restore_attachment_paths(event.get("attachments", Array()));
			Dictionary user_message = SolersLLMMessage::user(model_content);
			user_message["turn_id"] = event.get("turn_id", 0);
			if (!attachments.is_empty()) {
				user_message["attachments"] = attachments;
			}
			if (!mentions.is_empty()) {
				user_message["mentions"] = mentions;
			}
			scan.restored->push_back(user_message);
		} else if (role == SolersLLMRole::ASSISTANT) {
			if (content.is_empty() && reasoning.is_empty() && tool_calls.is_empty()) {
				return true;
			}
			Dictionary assistant_message = SolersLLMMessage::assistant(content, tool_calls);
			if (!reasoning.is_empty()) {
				assistant_message["reasoning"] = reasoning;
			}
			scan.restored->push_back(assistant_message);
		}
		return true;
	});
	for (KeyValue<String, Dictionary> &call : restored_open_tools) {
		_solers_finish_timeline_tool(call.value, false, "Interrupted", 0);
	}

	Dictionary state;
	state["messages"] = restored;
	state["timeline_entries"] = restored_timeline;
	state["plan"] = restored_plan;
	state["last_request_usage"] = restored_last_request_usage;
	state["outcome"] = restored_outcome;
	state["compaction_count"] = restored_compaction_count;
	state["turn_id"] = restored_turn_id;
	state["background_assets"] = restored_background_assets;
	state["session_revision"] = (int64_t)restored_authored_revision;
	state["reversals"] = restored_reversals;
	state["added_tool_names"] = restored_added_tool_names;
	state["pending_rewind"] = pending_rewind;
	state["committed_rewinds"] = committed_rewinds;
	return state;
}

void SolersAgentSession::_stamp_transcript_event(Dictionary &r_event) const {
	if (r_event.has("event_id")) {
		transcript_event_sequence = MAX(transcript_event_sequence, (int64_t)r_event["event_id"]);
	} else {
		r_event["event_id"] = ++transcript_event_sequence;
	}
	r_event["project_path"] = project_path;
	r_event["session_id"] = session_id;
	r_event["session_revision"] = (int64_t)authored_revision;
}

int64_t SolersAgentSession::_write_transcript_event(const String &p_type, const Dictionary &p_payload) const {
	Dictionary event = p_payload.duplicate(true);
	event["event_type"] = p_type;
	event["turn_id"] = turn_id;
	event["wall"] = Time::get_singleton()->get_unix_time_from_system();
	_stamp_transcript_event(event);
	_solers_project_timeline_event(event, timeline_entries, open_timeline_tools);
	solers_transcript_write(event);
	return event["event_id"];
}

Error SolersAgentSession::_write_transcript_event_durable(const String &p_type, const Dictionary &p_payload, int64_t &r_event_id) const {
	Dictionary event = p_payload.duplicate(true);
	event["event_type"] = p_type;
	event["turn_id"] = turn_id;
	event["wall"] = Time::get_singleton()->get_unix_time_from_system();
	_stamp_transcript_event(event);
	r_event_id = event["event_id"];
	const Error write_error = solers_transcript_write(event);
	if (write_error != OK) {
		return write_error;
	}
	const Error flush_error = solers_transcript_flush(session_id);
	if (flush_error != OK) {
		struct DurableCheck {
			int64_t id;
			String type;
			bool found = false;
		} check{ r_event_id, p_type };
		solers_transcript_foreach_session(session_id, &check, [](void *p_data, const String &p_line) -> bool {
			DurableCheck &state = *static_cast<DurableCheck *>(p_data);
			Dictionary stored;
			state.found = solers_transcript_parse_record(p_line, stored) && (int64_t)stored.get("event_id", 0) == state.id && String(stored.get("event_type", String())) == state.type;
			return !state.found;
		});
		if (!check.found) {
			return flush_error;
		}
	}
	_solers_project_timeline_event(event, timeline_entries, open_timeline_tools);
	return OK;
}

void SolersAgentSession::_write_prepared_journal_event(SolersPreparedToolCall *p_call) const {
	if (!p_call || p_call->journal_event.is_empty()) {
		return;
	}
	Dictionary payload = p_call->journal_event;
	const String type = payload.get("event_type", String());
	payload.erase("event_type");
	_write_transcript_event(type, payload);
	p_call->journal_event.clear();
}

int64_t SolersAgentSession::_write_transcript_message(const String &p_role, const String &p_content, const Array &p_mentions, const Array &p_tool_calls, const String &p_reasoning, const Array &p_attachments, int64_t p_event_id) const {
	Dictionary event;
	event["event_type"] = "message";
	event["turn_id"] = turn_id;
	event["role"] = p_role;
	event["author"] = p_role == SolersLLMRole::USER ? "human" : "agent";
	event["content"] = p_content;
	if (!p_mentions.is_empty()) {
		event["mentions"] = p_mentions;
	}
	if (!p_tool_calls.is_empty()) {
		event["tool_calls"] = p_tool_calls.duplicate(true);
	}
	if (!p_reasoning.is_empty()) {
		event["reasoning"] = p_reasoning;
	}
	// Metadata only (same ids the model saw) — never persist image bytes in transcript.
	if (!p_attachments.is_empty()) {
		Array attachment_meta;
		for (int i = 0; i < p_attachments.size(); i++) {
			const Dictionary attachment = p_attachments[i];
			Dictionary meta;
			meta["id"] = attachment.get("id", String());
			meta["filename"] = attachment.get("filename", String());
			if (attachment.has("mime_type")) {
				meta["mime_type"] = attachment.get("mime_type", String());
			}
			for (const char *field : { "content_sha256", "width", "height" }) {
				if (attachment.has(field)) {
					meta[field] = attachment[field];
				}
			}
			attachment_meta.push_back(meta);
		}
		event["attachments"] = attachment_meta;
	}
	if (p_event_id > 0) {
		event["event_id"] = p_event_id;
	}
	event["wall"] = Time::get_singleton()->get_unix_time_from_system();
	_stamp_transcript_event(event);
	_solers_project_timeline_event(event, timeline_entries, open_timeline_tools);
	solers_transcript_write(event);
	return event["event_id"];
}

void SolersAgentSession::_write_transcript_plan() const {
	Dictionary event = current_plan.duplicate(true);
	_write_transcript_event("plan_updated", event);
}

int64_t SolersAgentSession::_write_transcript_compaction(const String &p_phase, const Dictionary &p_payload) const {
	Dictionary event = p_payload.duplicate(true);
	event["compaction_id"] = compaction_id;
	event["phase"] = p_phase;
	return _write_transcript_event("context_compaction", event);
}

void SolersAgentSession::_write_transcript_tool(const String &p_call_id, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, const String &p_delivered_content, const Array &p_added_tool_names) const {
	Dictionary event;
	event["role"] = "tool";
	event["call_id"] = p_call_id;
	event["tool"] = p_canonical_name;
	event["ok"] = p_result.get("ok", false);
	event["queue_msec"] = tool_started_msec >= tool_queued_msec ? (int64_t)(tool_started_msec - tool_queued_msec) : 0;
	event["run_msec"] = tool_completed_msec >= tool_started_msec ? (int64_t)(tool_completed_msec - tool_started_msec) : 0;
	event["delivery_msec"] = tool_completed_msec ? (int64_t)(OS::get_singleton()->get_ticks_msec() - tool_completed_msec) : 0;
	event["duration_msec"] = event["run_msec"];
	event["content"] = p_delivered_content;
	if (!p_added_tool_names.is_empty()) {
		event["added_tool_names"] = p_added_tool_names.duplicate();
	}
	if (tool_registry) {
		event["args"] = tool_registry->redact_tool_args_for_audit(StringName(p_canonical_name), p_args);
		event["args_summary"] = tool_registry->summarize_tool_args_for_audit(StringName(p_canonical_name), p_args);
		event["result_summary"] = tool_registry->summarize_tool_result_for_audit(p_result);
		event["resource_accesses"] = tool_registry->resolve_resource_access(StringName(p_canonical_name), p_args);
	} else {
		event["args"] = p_args;
	}
	if (!(bool)p_result.get("ok", false)) {
		event["error"] = p_result.get("error", Dictionary());
	}
	const Dictionary result_data = p_result.get("data", Dictionary());
	if (result_data.has("artifact")) {
		event["artifact"] = result_data["artifact"];
	}
	_write_transcript_event("tool_result", event);
}

void SolersAgentSession::_ensure_godot_log_audit(bool p_turn_active) {
	if (p_turn_active) {
		MutexLock lock(godot_log_mutex);
		godot_log_error_count = 0;
		godot_log_warning_count = 0;
	}
	godot_log_turn_active = p_turn_active;
	if (!godot_log_audit_installed) {
		godot_error_handler.errfunc = _godot_error_callback;
		godot_error_handler.userdata = this;
		add_error_handler(&godot_error_handler);
		godot_log_audit_installed = true;
	}
}

void SolersAgentSession::_release_godot_log_audit() {
	if (!godot_log_audit_installed) {
		return;
	}
	remove_error_handler(&godot_error_handler);
	godot_log_audit_installed = false;
	godot_log_turn_active = false;
}

// Repeats fold into counts, so this caps only distinct message texts per model boundary.
static constexpr int MAX_UNIQUE_PENDING_DIAGNOSTICS = 256;

// Fold one occurrence into an aggregated group array: identical
// (severity, message, call_id) entries become a single record carrying a
// count and first/last timestamps. Returns true when the group is new.
static bool _solers_accumulate_diagnostic(Array &r_groups, const Dictionary &p_event, int p_group_index) {
	if (p_group_index >= 0) {
		Dictionary group = r_groups[p_group_index];
		group["count"] = (int)group.get("count", 1) + 1;
		group["last_ticks_msec"] = p_event.get("ticks_msec", 0);
		r_groups[p_group_index] = group;
		return false;
	}
	Dictionary group = p_event.duplicate();
	group["count"] = 1;
	group["first_ticks_msec"] = p_event.get("ticks_msec", 0);
	group["last_ticks_msec"] = p_event.get("ticks_msec", 0);
	group.erase("ticks_msec");
	r_groups.push_back(group);
	return true;
}

void SolersAgentSession::_godot_error_callback(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
	const String message = p_message && *p_message ? String::utf8(p_message) : String::utf8(p_error);
	static_cast<SolersAgentSession *>(p_self)->_on_godot_error(message, p_type, Thread::get_caller_id(), p_function ? String::utf8(p_function) : String(), p_file ? String::utf8(p_file) : String(), p_line);
}

void SolersAgentSession::_on_godot_error(const String &p_message, ErrorHandlerType p_type, int64_t p_source_thread, const String &p_function, const String &p_file, int p_line) {
	const bool is_error = p_type != ERR_HANDLER_WARNING;
	Dictionary event;
	event["severity"] = is_error ? "error" : "warning";
	event["source"] = "godot_editor";
	event["message"] = p_message;
	event["turn_active"] = godot_log_turn_active;
	event["ticks_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	event["thread_id"] = p_source_thread;
	event["function"] = p_function;
	event["file"] = p_file;
	event["line"] = p_line;
	{
		MutexLock lock(godot_log_mutex);
		if (is_error) {
			godot_log_error_count++;
		} else {
			godot_log_warning_count++;
		}
		const String group_key = String(event.get("severity", String())) + "\n" + p_message;
		const int *existing = pending_godot_diagnostic_index.getptr(group_key);
		if (existing) {
			_solers_accumulate_diagnostic(pending_godot_diagnostics, event, *existing);
		} else if (pending_godot_diagnostic_index.size() < MAX_UNIQUE_PENDING_DIAGNOSTICS) {
			_solers_accumulate_diagnostic(pending_godot_diagnostics, event, -1);
			pending_godot_diagnostic_index[group_key] = pending_godot_diagnostics.size() - 1;
		} else {
			pending_godot_diagnostics_overflow++;
		}
	}
}

Dictionary SolersAgentSession::_take_godot_diagnostics() {
	Dictionary diagnostics;
	{
		MutexLock lock(godot_log_mutex);
		if (pending_godot_diagnostics.is_empty()) {
			return diagnostics;
		}
		// Entries are already aggregated at ingestion; counts carry the
		// occurrence totals.
		int errors = 0;
		int warnings = 0;
		for (int i = 0; i < pending_godot_diagnostics.size(); i++) {
			const Dictionary event = pending_godot_diagnostics[i];
			const int count = event.get("count", 1);
			if (String(event.get("severity", String())) == "error") {
				errors += count;
			} else {
				warnings += count;
			}
		}
		diagnostics["errors"] = errors;
		diagnostics["warnings"] = warnings;
		diagnostics["events"] = pending_godot_diagnostics.duplicate();
		if (pending_godot_diagnostics_overflow > 0) {
			diagnostics["suppressed_unique_messages"] = pending_godot_diagnostics_overflow;
		}
		pending_godot_diagnostics.clear();
		pending_godot_diagnostic_index.clear();
		pending_godot_diagnostics_overflow = 0;
	}
	_write_transcript_event("godot_diagnostics_delivered", diagnostics);
	return diagnostics;
}

// Enriched mentions are the audit authority (same array the model sees).
// strip_prompt_block is display-only — never treat stripped transcript text as the fact source.
Dictionary SolersAgentSession::preview_rewind_to_event(int64_t p_event_id) const {
	if (running) {
		return _error("AGENT_BUSY", "Wait for the current Agent turn to finish before editing history.");
	}
	if (!session_tools_registry || p_event_id <= 0) {
		return _error("REWIND_UNAVAILABLE", "Historical project rewind is unavailable for this session.");
	}
	Dictionary target;
	int target_index = -1;
	for (int i = 0; i < timeline_entries.size(); i++) {
		const Dictionary entry = timeline_entries[i];
		if ((int64_t)entry.get("event_id", 0) == p_event_id && String(entry.get("role", String())) == SolersLLMRole::USER) {
			target = entry;
			target_index = i;
			break;
		}
	}
	if (target_index < 0) {
		return _error("REWIND_TARGET_NOT_FOUND", "The selected user message is not active in this session.");
	}
	if (!target.has("session_revision")) {
		return _error("REWIND_BOUNDARY_UNAVAILABLE", "This message predates native mutation boundaries and cannot be edited safely.");
	}
	const Array attachments = _solers_restore_attachment_paths(target.get("attachments", Array()));
	for (const Variant &item : attachments) {
		const Dictionary attachment = item;
		const String sha = attachment.get("content_sha256", String());
		const String path = attachment.get("local_path", String());
		if (!sha.is_empty() && (path.is_empty() || !FileAccess::exists(path) || FileAccess::get_sha256(path) != sha)) {
			return _error("REWIND_ATTACHMENT_CONFLICT", "An attachment from the selected message is missing or no longer matches its recorded SHA-256.");
		}
	}
	const uint64_t target_revision = (int64_t)target.get("session_revision", 0);
	const Dictionary registry_preview = session_tools_registry->preview_session_rewind(session_id, target_revision);
	if (!(bool)registry_preview.get("ok", false)) {
		return registry_preview;
	}
	int following_messages = 0;
	for (int i = target_index + 1; i < timeline_entries.size(); i++) {
		const String role = Dictionary(timeline_entries[i]).get("role", String());
		if (role == SolersLLMRole::USER || role == SolersLLMRole::ASSISTANT) {
			following_messages++;
		}
	}
	Dictionary data = Dictionary(registry_preview.get("data", Dictionary())).duplicate(true);
	data["target_event_id"] = p_event_id;
	data["target_revision"] = (int64_t)target_revision;
	data["content"] = target.get("content", String());
	data["attachments"] = attachments;
	data["following_messages"] = following_messages;
	return _ok(data);
}
Dictionary SolersAgentSession::rewind_to_event(int64_t p_event_id) {
	const Dictionary preview = preview_rewind_to_event(p_event_id);
	if (!(bool)preview.get("ok", false)) {
		return preview;
	}
	const Dictionary preview_data = preview.get("data", Dictionary());
	const uint64_t target_revision = (int64_t)preview_data.get("target_revision", 0);
	const Dictionary prepared = session_tools_registry->prepare_session_rewind(session_id, target_revision);
	if (!(bool)prepared.get("ok", false)) {
		return prepared;
	}
	const Dictionary transaction = prepared.get("data", Dictionary());
	Dictionary journal_payload;
	journal_payload["target_event_id"] = p_event_id;
	journal_payload["transaction"] = transaction;
	int64_t journal_event_id = 0;
	if (_write_transcript_event_durable("rewind_prepared", journal_payload, journal_event_id) != OK) {
		session_tools_registry->abort_session_rewind(transaction);
		return _error("REWIND_JOURNAL_FAILED", "The rewind recovery record could not be persisted; the project was not changed.");
	}
	const Dictionary applied = session_tools_registry->apply_session_rewind(transaction);
	if (!(bool)applied.get("ok", false)) {
		const Dictionary compensated = session_tools_registry->abort_session_rewind(transaction);
		Dictionary aborted;
		aborted["transaction_id"] = transaction.get("transaction_id", String());
		aborted["reason"] = (bool)compensated.get("ok", false) ? String(Dictionary(applied.get("error", Dictionary())).get("message", String())) : String("rewind compensation failed");
		_write_transcript_event_durable("rewind_aborted", aborted, journal_event_id);
		return applied;
	}
	if (_write_transcript_event_durable("session_rewind", journal_payload, journal_event_id) != OK) {
		const Dictionary compensated = session_tools_registry->abort_session_rewind(transaction);
		Dictionary aborted;
		aborted["transaction_id"] = transaction.get("transaction_id", String());
		aborted["reason"] = "session_rewind journal commit failed";
		_write_transcript_event_durable("rewind_aborted", aborted, journal_event_id);
		return _error((bool)compensated.get("ok", false) ? "REWIND_JOURNAL_FAILED" : "REWIND_COMPENSATION_FAILED", "The session marker could not be persisted; the original project state was restored when possible.");
	}
	session_tools_registry->finish_session_rewind(transaction);
	const Dictionary state = _read_transcript_state(project_path, session_id);
	messages = state.get("messages", Array());
	_load_active_tools(state.get("added_tool_names", Array()));
	timeline_entries = state.get("timeline_entries", Array());
	open_timeline_tools.clear();
	current_plan = state.get("plan", Dictionary());
	last_request_usage = state.get("last_request_usage", Dictionary());
	last_outcome = state.get("outcome", String());
	if (context_manager) {
		context_manager->reset();
		context_manager->set_compaction_count(state.get("compaction_count", 0));
	}
	turn_id = state.get("turn_id", turn_id);
	authored_revision = (int64_t)state.get("session_revision", authored_revision);
	session_tools_registry->restore_session_reversals(session_id, state.get("reversals", Array()));
	Dictionary data;
	data["target_event_id"] = p_event_id;
	data["content"] = preview_data.get("content", String());
	data["attachments"] = preview_data.get("attachments", Array());
	data["following_messages"] = preview_data.get("following_messages", 0);
	data["action_count"] = preview_data.get("action_count", 0);
	data["file_count"] = preview_data.get("file_count", 0);
	return _ok(data);
}
