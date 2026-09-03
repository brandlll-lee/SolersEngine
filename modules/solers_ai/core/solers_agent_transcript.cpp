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
#include "core/os/os.h"
#include "core/os/time.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

String SolersAgentSession::_make_session_id() const {
	return OS::get_singleton()->get_unique_id() + "-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
}

static void _solers_finish_timeline_tool(Dictionary &r_call, bool p_ok, const String &p_error, int p_duration) {
	r_call["finished"] = true;
	r_call["ok"] = p_ok;
	r_call["error_message"] = p_error;
	r_call["duration_msec"] = p_duration;
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
		if (role != SolersLLMRole::USER && role != SolersLLMRole::ASSISTANT) {
			return;
		}
		const String content = p_event.get("content", String());
		const String reasoning = p_event.get("reasoning", String());
		Array calls = p_event.get("tool_calls", Array());
		Dictionary entry;
		entry["event_id"] = p_event.get("event_id", 0);
		entry["role"] = role;
		entry["content"] = content;
		if (!reasoning.is_empty()) {
			entry["reasoning"] = reasoning;
		}
		for (const char *field : { "attachments", "mentions", "origin", "wall" }) {
			if (p_event.has(field)) {
				entry[field] = p_event[field];
			}
		}
		if (entry.has("attachments")) {
			entry["attachments"] = _solers_restore_attachment_paths(entry["attachments"]);
		}
		if (!calls.is_empty()) {
			entry["tool_calls"] = calls;
			for (const Variant &item : calls) {
				const Dictionary call = item;
				const String call_id = call.get("id", String());
				if (!call_id.is_empty()) {
					r_open_tools[call_id] = call;
				}
			}
		}
		if (!content.is_empty() || !reasoning.is_empty() || !calls.is_empty() || !Array(entry.get("attachments", Array())).is_empty()) {
			r_entries.push_back(entry);
		}
		return;
	}
	if (type == "tool_result") {
		const String call_id = p_event.get("call_id", String());
		Dictionary *call = r_open_tools.getptr(call_id);
		if (call) {
			const bool ok = p_event.get("ok", false);
			_solers_finish_timeline_tool(*call, ok, ok ? String() : String(Dictionary(p_event.get("error", Dictionary())).get("message", String())), p_event.get("duration_msec", 0));
		}
		r_open_tools.erase(call_id);
		return;
	}
	if (type == "context_compaction") {
		Dictionary entry;
		entry["event_id"] = p_event.get("event_id", 0);
		entry["compaction_id"] = p_event.get("compaction_id", entry["event_id"]);
		entry["role"] = "context_compaction";
		entry["phase"] = p_event.get("phase", "completed");
		for (int i = r_entries.size() - 1; i >= 0; i--) {
			const Dictionary existing = r_entries[i];
			if (existing.get("role", String()) == "context_compaction" && existing.get("compaction_id", -1) == entry["compaction_id"]) {
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
		if (p_event.get("outcome", String()) != "completed") {
			r_entries.push_back(Dictionary({
				{ "event_id", p_event.get("event_id", 0) },
				{ "role", "turn_outcome" },
				{ "content", p_event.get("message", String()) },
			}));
		}
	}
}

static void _solers_remove_projected_after(Array &r_values, int64_t p_event_id) {
	for (int i = r_values.size() - 1; i >= 0; i--) {
		if ((int64_t)Dictionary(r_values[i]).get("event_id", 0) >= p_event_id) {
			r_values.remove_at(i);
		}
	}
}

Dictionary SolersAgentSession::_read_transcript_state(const String &p_project_path, const String &p_session_id) const {
	Array restored;
	Array restored_timeline;
	HashMap<String, Dictionary> open_tools;
	Dictionary restored_last_request_usage;
	String restored_outcome;
	int restored_compaction_count = 0;
	int restored_turn_id = 0;
	int64_t sequence = 0;

	if (p_project_path.is_empty() || p_session_id.is_empty()) {
		return Dictionary({ { "messages", restored }, { "timeline_entries", restored_timeline } });
	}

	struct ScanState {
		const String *project_path;
		const String *session_id;
		Array *messages;
		Array *timeline;
		HashMap<String, Dictionary> *open_tools;
		Dictionary *last_usage;
		String *outcome;
		int *compaction_count;
		int *turn_id;
		int64_t *sequence;
	};
	ScanState scan{ &p_project_path, &p_session_id, &restored, &restored_timeline, &open_tools, &restored_last_request_usage, &restored_outcome, &restored_compaction_count, &restored_turn_id, &sequence };

	solers_transcript_foreach_session(p_session_id, &scan, [](void *p_userdata, const String &p_record) -> bool {
		ScanState &scan = *static_cast<ScanState *>(p_userdata);
		Dictionary event;
		if (!solers_transcript_parse_record(p_record.strip_edges(), event) ||
				String(event.get("project_path", String())) != *scan.project_path ||
				String(event.get("session_id", String())) != *scan.session_id) {
			return true;
		}
		*scan.sequence = MAX(*scan.sequence + 1, (int64_t)event.get("event_id", 0));
		if (!event.has("event_id")) {
			event["event_id"] = *scan.sequence;
		}
		const int64_t event_id = event.get("event_id", *scan.sequence);
		*scan.turn_id = MAX(*scan.turn_id, (int)event.get("turn_id", 0));
		const String type = event.get("event_type", String());

		if (type == "conversation_rewind") {
			const int64_t target = event.get("target_event_id", 0);
			if (target > 0) {
				_solers_remove_projected_after(*scan.messages, target);
				_solers_remove_projected_after(*scan.timeline, target);
				scan.open_tools->clear();
			}
			return true;
		}
		if (type == "context_compaction" && String(event.get("phase", String())) == "completed") {
			(*scan.compaction_count)++;
			const Array compacted = event.get("messages", Array());
			if (!compacted.is_empty()) {
				*scan.messages = compacted.duplicate(true);
			}
		}
		const Dictionary usage = event.get("usage", Dictionary());
		if (!usage.is_empty()) {
			*scan.last_usage = usage.duplicate(true);
		}
		_solers_project_timeline_event(event, *scan.timeline, *scan.open_tools);

		if (type == "model_context") {
			Dictionary message;
			message["role"] = SolersContextManager::MODEL_CONTEXT_ROLE;
			message["content"] = event.get("content", String());
			message["origin"] = event.get("origin", "solers_state");
			message["event_id"] = event_id;
			scan.messages->push_back(message);
			return true;
		}
		if (type == "tool_result") {
			const String call_id = event.get("call_id", String());
			const String tool = event.get("tool", String());
			const String content = event.get("content", String());
			if (!call_id.is_empty()) {
				Dictionary message = SolersLLMMessage::tool_result(call_id, tool, content);
				message["event_id"] = event_id;
				scan.messages->push_back(message);
			}
			return true;
		}
		if (type == "turn_outcome") {
			*scan.outcome = event.get("outcome", String());
			*scan.messages = SolersContextManager::project_completed_turns(*scan.messages);
			return true;
		}

		const String role = event.get("role", String());
		const String content = event.get("content", String());
		const String reasoning = event.get("reasoning", String());
		const Array tool_calls = event.get("tool_calls", Array());
		if (role == SolersLLMRole::USER && solers_transcript_is_human_message(event)) {
			const Array mentions = event.get("mentions", Array());
			const String display = SolersMention::strip_prompt_block(content);
			const String model_content = content.find("[Selected Solers context]") >= 0 ? content : display + SolersMention::prompt_block(mentions);
			Dictionary message = SolersLLMMessage::user(model_content);
			message["event_id"] = event_id;
			message["turn_id"] = event.get("turn_id", 0);
			if (!mentions.is_empty()) {
				message["mentions"] = mentions;
			}
			const Array attachments = _solers_restore_attachment_paths(event.get("attachments", Array()));
			if (!attachments.is_empty()) {
				message["attachments"] = attachments;
			}
			scan.messages->push_back(message);
		} else if (role == SolersLLMRole::ASSISTANT && (!content.is_empty() || !reasoning.is_empty() || !tool_calls.is_empty())) {
			Dictionary message = SolersLLMMessage::assistant(content, tool_calls);
			message["event_id"] = event_id;
			if (!reasoning.is_empty()) {
				message["reasoning"] = reasoning;
			}
			scan.messages->push_back(message);
		}
		return true;
	});

	for (KeyValue<String, Dictionary> &call : open_tools) {
		_solers_finish_timeline_tool(call.value, false, "Interrupted", 0);
	}
	Dictionary state;
	state["messages"] = restored;
	state["timeline_entries"] = restored_timeline;
	state["last_request_usage"] = restored_last_request_usage;
	state["outcome"] = restored_outcome;
	state["compaction_count"] = restored_compaction_count;
	state["turn_id"] = restored_turn_id;
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
	r_event_id = _write_transcript_event(p_type, p_payload);
	return solers_transcript_flush(session_id);
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
	if (!p_attachments.is_empty()) {
		Array attachment_metadata;
		for (const Variant &value : p_attachments) {
			const Dictionary attachment = value;
			Dictionary item;
			for (const char *field : { "id", "filename", "mime_type", "content_sha256", "width", "height" }) {
				if (attachment.has(field)) {
					item[field] = attachment[field];
				}
			}
			attachment_metadata.push_back(item);
		}
		event["attachments"] = attachment_metadata;
	}
	if (p_event_id > 0) {
		event["event_id"] = p_event_id;
	}
	return _write_transcript_event("message", event);
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
	event["duration_msec"] = event["run_msec"];
	event["content"] = p_delivered_content;
	if (tool_registry) {
		event["args"] = tool_registry->redact_tool_args_for_audit(StringName(p_canonical_name), p_args);
		event["result_summary"] = tool_registry->summarize_tool_result_for_audit(p_result);
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

static constexpr int MAX_UNIQUE_PENDING_DIAGNOSTICS = 256;

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

Dictionary SolersAgentSession::_take_godot_diagnostics() {
	Dictionary diagnostics;
	{
		MutexLock lock(godot_log_mutex);
		if (pending_godot_diagnostics.is_empty()) {
			return diagnostics;
		}
		int errors = 0;
		int warnings = 0;
		for (const Variant &value : pending_godot_diagnostics) {
			const Dictionary event = value;
			if (String(event.get("severity", String())) == "error") {
				errors += (int)event.get("count", 1);
			} else {
				warnings += (int)event.get("count", 1);
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

Dictionary SolersAgentSession::preview_rewind_to_event(int64_t p_event_id) const {
	if (running) {
		return _error("AGENT_BUSY", "Wait for the current Agent turn to finish before editing conversation history.");
	}
	int target_index = -1;
	Dictionary target;
	for (int i = 0; i < timeline_entries.size(); i++) {
		const Dictionary entry = timeline_entries[i];
		if ((int64_t)entry.get("event_id", 0) == p_event_id && entry.get("role", String()) == SolersLLMRole::USER) {
			target_index = i;
			target = entry;
			break;
		}
	}
	if (target_index < 0) {
		return _error("REWIND_TARGET_NOT_FOUND", "The selected user message is not present in this session.");
	}
	int following_messages = 0;
	for (int i = target_index + 1; i < timeline_entries.size(); i++) {
		const String role = Dictionary(timeline_entries[i]).get("role", String());
		if (role == SolersLLMRole::USER || role == SolersLLMRole::ASSISTANT) {
			following_messages++;
		}
	}
	Dictionary data;
	data["target_event_id"] = p_event_id;
	data["content"] = target.get("content", String());
	data["attachments"] = _solers_restore_attachment_paths(target.get("attachments", Array()));
	data["following_messages"] = following_messages;
	data["action_count"] = 0;
	data["file_count"] = 0;
	data["project_state_unchanged"] = true;
	return _ok(data);
}

Dictionary SolersAgentSession::rewind_to_event(int64_t p_event_id) {
	const Dictionary preview = preview_rewind_to_event(p_event_id);
	if (!(bool)preview.get("ok", false)) {
		return preview;
	}
	for (int i = messages.size() - 1; i >= 0; i--) {
		if ((int64_t)Dictionary(messages[i]).get("event_id", INT64_MAX) >= p_event_id) {
			messages.remove_at(i);
		}
	}
	_solers_remove_projected_after(timeline_entries, p_event_id);
	_write_transcript_event("conversation_rewind", Dictionary({ { "target_event_id", p_event_id }, { "project_state_unchanged", true } }));
	Dictionary data = preview.get("data", Dictionary()).duplicate(true);
	data["rewound"] = true;
	return _ok(data);
}
