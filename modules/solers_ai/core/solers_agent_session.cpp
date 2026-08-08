/**************************************************************************/
/*  solers_agent_session.cpp                                              */
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
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/editor_interface.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/main/node.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_retry.h"
#include "modules/solers_ai/llm/solers_models_dev.h"

struct SolersAgentSession::ToolThreadState {
	SolersToolRegistry *registry = nullptr;
	SolersAgentSession *session = nullptr;
	SolersPreparedToolCall call;
	Dictionary poll_args;
	bool polling = false;
	Dictionary result;
	SafeFlag done;
	uint64_t worker_thread_id = Thread::UNASSIGNED_ID;
	uint64_t token = 0;
};

struct SolersAgentSession::PendingToolExecution {
	int queue_index = -1;
	String call_id;
	String model_name;
	String canonical_name;
	Dictionary poll_args;
	Dictionary initial_args;
	Array resource_accesses;
	SolersPreparedToolCall *prepared_call = nullptr;
	bool is_resume = false;
	uint64_t started_msec = 0;
};

static bool _is_session_tool(const String &p_name) {
	return p_name == "update_plan";
}

static Dictionary _update_plan_schema() {
	Dictionary status;
	status["type"] = "string";
	Array values;
	values.push_back("pending");
	values.push_back("in_progress");
	values.push_back("completed");
	status["enum"] = values;

	Dictionary step_properties;
	Dictionary step_text;
	step_text["type"] = "string";
	step_properties["step"] = step_text;
	step_properties["status"] = status;
	Dictionary step;
	step["type"] = "object";
	step["properties"] = step_properties;
	Array step_required;
	step_required.push_back("step");
	step_required.push_back("status");
	step["required"] = step_required;
	step["additionalProperties"] = false;

	Dictionary properties;
	Dictionary explanation;
	explanation["type"] = "string";
	properties["explanation"] = explanation;
	Dictionary plan;
	plan["type"] = "array";
	plan["items"] = step;
	properties["plan"] = plan;

	Dictionary schema;
	schema["type"] = "object";
	schema["properties"] = properties;
	Array required;
	required.push_back("plan");
	schema["required"] = required;
	schema["additionalProperties"] = false;
	return schema;
}

void SolersAgentSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_turn", "args"), &SolersAgentSession::start_turn);
	ClassDB::bind_method(D_METHOD("queue_user_message", "args"), &SolersAgentSession::queue_user_message);
	ClassDB::bind_method(D_METHOD("poll"), &SolersAgentSession::poll);
	ClassDB::bind_method(D_METHOD("abort"), &SolersAgentSession::abort);
	ClassDB::bind_method(D_METHOD("reset_conversation"), &SolersAgentSession::reset_conversation);
	ClassDB::bind_method(D_METHOD("get_status"), &SolersAgentSession::get_status);

	ADD_SIGNAL(MethodInfo("model_request_started"));
	ADD_SIGNAL(MethodInfo("timeline_entry_committed", PropertyInfo(Variant::INT, "event_id"), PropertyInfo(Variant::STRING, "role")));
	ADD_SIGNAL(MethodInfo("assistant_delta", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("reasoning_delta", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("assistant_message", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("tool_call_started", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::STRING, "arguments")));
	ADD_SIGNAL(MethodInfo("tool_call_updated", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::STRING, "arguments")));
	ADD_SIGNAL(MethodInfo("tool_call_awaiting_approval", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name")));
	ADD_SIGNAL(MethodInfo("tool_call_finished", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::DICTIONARY, "result"), PropertyInfo(Variant::INT, "duration_msec")));
	ADD_SIGNAL(MethodInfo("turn_completed", PropertyInfo(Variant::DICTIONARY, "result")));
	ADD_SIGNAL(MethodInfo("turn_failed", PropertyInfo(Variant::DICTIONARY, "error")));
	ADD_SIGNAL(MethodInfo("turn_retrying", PropertyInfo(Variant::INT, "attempt"), PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("turn_waiting", PropertyInfo(Variant::DICTIONARY, "waiting")));
	ADD_SIGNAL(MethodInfo("plan_updated", PropertyInfo(Variant::STRING, "explanation"), PropertyInfo(Variant::ARRAY, "plan")));
}

Dictionary SolersAgentSession::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersAgentSession::_error(const String &p_code, const String &p_message) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

void SolersAgentSession::_record(const String &p_event, const Dictionary &p_payload) const {
	if (action_timeline) {
		action_timeline->record_event(p_event, p_payload);
	}
}

Dictionary SolersAgentSession::validate_plan(const Dictionary &p_args) {
	Dictionary result;
	Dictionary error;
	for (const Variant *key = p_args.next(nullptr); key; key = p_args.next(key)) {
		const String name = String(*key);
		if (name != "explanation" && name != "plan") {
			error["code"] = "INVALID_PLAN";
			error["message"] = vformat("Unknown update_plan field: %s.", name);
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
	}
	if (!p_args.has("plan") || p_args["plan"].get_type() != Variant::ARRAY) {
		error["code"] = "INVALID_PLAN";
		error["message"] = "update_plan requires a plan array.";
		result["ok"] = false;
		result["error"] = error;
		return result;
	}

	const Array plan = p_args["plan"];
	int in_progress_count = 0;
	for (int i = 0; i < plan.size(); i++) {
		if (plan[i].get_type() != Variant::DICTIONARY) {
			error["code"] = "INVALID_PLAN";
			error["message"] = "Each plan item must be an object.";
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		const Dictionary item = plan[i];
		for (const Variant *key = item.next(nullptr); key; key = item.next(key)) {
			const String name = String(*key);
			if (name != "step" && name != "status") {
				error["code"] = "INVALID_PLAN";
				error["message"] = vformat("Unknown plan item field: %s.", name);
				result["ok"] = false;
				result["error"] = error;
				return result;
			}
		}
		const String step = String(item.get("step", String())).strip_edges();
		const String status = item.get("status", String());
		if (step.is_empty() || (status != "pending" && status != "in_progress" && status != "completed")) {
			error["code"] = "INVALID_PLAN";
			error["message"] = "Each plan item needs a non-empty step and status pending, in_progress, or completed.";
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		if (status == "in_progress") {
			in_progress_count++;
		}
	}
	if (in_progress_count > 1) {
		error["code"] = "INVALID_PLAN";
		error["message"] = "At most one plan item may be in_progress.";
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	result["ok"] = true;
	result["data"] = p_args.duplicate(true);
	return result;
}

void SolersAgentSession::set_tool_registry(SolersToolRegistry *p_tool_registry) {
	tool_registry = p_tool_registry;
	_register_session_tools();
}

void SolersAgentSession::set_models_dev(SolersModelsDev *p_models_dev, bool p_owned) {
	if (owns_models_dev && models_dev && models_dev != p_models_dev) {
		memdelete(models_dev);
	}
	models_dev = p_models_dev;
	owns_models_dev = p_owned && p_models_dev != nullptr;
}

void SolersAgentSession::_register_session_tools() {
	if (!tool_registry || session_tools_registry == tool_registry) {
		return;
	}

	SolersToolCapability capability;
	capability.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	capability.mutation_policy = SolersToolMutationPolicy::READ_ONLY;
	tool_registry->register_tool(memnew(SolersFunctionTool(
			"update_plan",
			"Optionally replace the concise UI progress plan. This does not control tool access or task completion.",
			_update_plan_schema(),
			SolersToolExposure::DIRECT,
			capability,
			[this](const SolersToolContext &, const Dictionary &p_args) { return _handle_update_plan(p_args); })));
	session_tools_registry = tool_registry;
}

Dictionary SolersAgentSession::_handle_update_plan(const Dictionary &p_args) {
	const Dictionary validation = validate_plan(p_args);
	if (!(bool)validation.get("ok", false)) {
		return validation;
	}
	current_plan = p_args.duplicate(true);
	_write_transcript_plan();
	emit_signal(SNAME("plan_updated"), current_plan.get("explanation", String()), current_plan.get("plan", Array()));
	Dictionary data;
	data["message"] = "Plan updated";
	return _ok(data);
}

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
		for (const char *field : { "reasoning", "attachments", "origin" }) {
			if (p_event.has(field)) {
				entry[field] = p_event[field];
			}
		}
		Array calls = Array(p_event.get("tool_calls", Array())).duplicate(true);
		for (int i = calls.size() - 1; i >= 0; i--) {
			const Dictionary call = calls[i];
			if (_is_session_tool(call.get("canonical_name", call.get("name", String())))) {
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
	String restored_outcome;
	int restored_turn_id = 0;
	uint64_t restored_authored_revision = 0;
	uint64_t restored_observed_revision = 0;
	Dictionary restored_reversal;
	HashSet<String> restored_model_attachments;
	if (p_project_path.is_empty() || p_session_id.is_empty()) {
		Dictionary empty;
		empty["messages"] = restored;
		empty["timeline_entries"] = restored_timeline;
		return empty;
	}

	struct ScanState {
		const String *project_path = nullptr;
		const String *session_id = nullptr;
		Array *restored = nullptr;
		Array *restored_timeline = nullptr;
		HashMap<String, Dictionary> *restored_open_tools = nullptr;
		Array *restored_background_assets = nullptr;
		Dictionary *restored_plan = nullptr;
		String *restored_outcome = nullptr;
		int *restored_turn_id = nullptr;
		uint64_t *restored_authored_revision = nullptr;
		uint64_t *restored_observed_revision = nullptr;
		int64_t *event_sequence = nullptr;
		Dictionary *restored_reversal = nullptr;
		HashSet<String> *restored_model_attachments = nullptr;
	} restore_state;
	restore_state.project_path = &p_project_path;
	restore_state.session_id = &p_session_id;
	restore_state.restored = &restored;
	restore_state.restored_timeline = &restored_timeline;
	restore_state.restored_open_tools = &restored_open_tools;
	restore_state.restored_background_assets = &restored_background_assets;
	restore_state.restored_plan = &restored_plan;
	restore_state.restored_outcome = &restored_outcome;
	restore_state.restored_turn_id = &restored_turn_id;
	restore_state.restored_authored_revision = &restored_authored_revision;
	restore_state.restored_observed_revision = &restored_observed_revision;
	restore_state.event_sequence = &transcript_event_sequence;
	restore_state.restored_reversal = &restored_reversal;
	restore_state.restored_model_attachments = &restored_model_attachments;
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
		*scan.event_sequence = MAX(*scan.event_sequence + 1, (int64_t)event.get("event_id", 0));
		if (!event.has("event_id")) {
			event["event_id"] = *scan.event_sequence;
		}
		*scan.restored_turn_id = MAX(*scan.restored_turn_id, (int)event.get("turn_id", 0));
		*scan.restored_authored_revision = MAX(*scan.restored_authored_revision, (uint64_t)(int64_t)event.get("session_revision", 0));
		*scan.restored_observed_revision = MAX(*scan.restored_observed_revision, (uint64_t)(int64_t)event.get("observed_revision", 0));

		const String event_type = event.get("event_type", String());
		_solers_project_timeline_event(event, *scan.restored_timeline, *scan.restored_open_tools);
		if (event_type == "attachment_projection_committed") {
			const Array identities = event.get("identities", Array());
			for (int i = 0; i < identities.size(); i++) {
				const String identity = identities[i];
				if (!identity.is_empty()) {
					scan.restored_model_attachments->insert(identity);
				}
			}
			return true;
		}
		if (event_type == "checkpoint_created") {
			Dictionary checkpoint = event.get("checkpoint", Dictionary());
			*scan.restored_reversal = checkpoint.duplicate(true);
			return true;
		}
		if (event_type == "checkpoint_cleared" || event_type == "checkpoint_consumed") {
			scan.restored_reversal->clear();
			return true;
		}
		if (event_type == "tool_result") {
			const String call_id = event.get("call_id", String());
			const String tool = event.get("tool", String());
			const String delivered = event.get("content", String());
			if (!call_id.is_empty() && !delivered.is_empty()) {
				scan.restored->push_back(SolersLLMMessage::tool_result(call_id, tool, delivered));
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
			// Restore path has no image bytes. Keep content-addressed references so
			// the model uses recorded facts or asks for fresh evidence.
			String model_content = had_block ? content : (display + SolersMention::prompt_block(mentions));
			const Array attachments = event.get("attachments", Array());
			for (int i = 0; i < attachments.size(); i++) {
				const Dictionary attachment = attachments[i];
				model_content += vformat("\n[Previously delivered image id=%s sha256=%s; pixel bytes are not replayed after session restore.]", String(attachment.get("id", String())), String(attachment.get("content_sha256", String())));
			}
			Dictionary user_message = SolersLLMMessage::user(model_content);
			user_message["turn_id"] = event.get("turn_id", 0);
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
	state["outcome"] = restored_outcome;
	state["turn_id"] = restored_turn_id;
	state["background_assets"] = restored_background_assets;
	state["session_revision"] = (int64_t)restored_authored_revision;
	state["observed_revision"] = (int64_t)restored_observed_revision;
	Array delivered_attachments;
	for (const String &identity : restored_model_attachments) {
		delivered_attachments.push_back(identity);
	}
	state["delivered_model_attachments"] = delivered_attachments;
	state["reversal"] = restored_reversal;
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
	r_event["observed_revision"] = (int64_t)observed_revision;
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

void SolersAgentSession::_write_transcript_tool(const String &p_call_id, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, const String &p_delivered_content) const {
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
	if (tool_registry) {
		event["args"] = tool_registry->redact_tool_args_for_audit(StringName(p_canonical_name), p_args);
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
	EditorLog *log = EditorNode::get_singleton() ? EditorNode::get_log() : nullptr;
	if (p_turn_active) {
		MutexLock lock(godot_log_mutex);
		godot_log_error_count = 0;
		godot_log_warning_count = 0;
		worker_tool_audits.clear();
		main_thread_tool_audit.clear();
		deferred_window_audit.clear();
		attributable_tool_errors.clear();
		pending_godot_diagnostics.clear();
		pending_godot_diagnostic_index.clear();
		pending_godot_diagnostics_overflow = 0;
	}
	godot_log_turn_active = p_turn_active;
	Dictionary baseline;
	baseline["available"] = log != nullptr;
	baseline["turn_active"] = p_turn_active;
	if (log) {
		const Callable callback = callable_mp(this, &SolersAgentSession::_on_godot_log_message);
		if (!godot_log_audit_installed) {
			log->set_message_audit_callback(callback);
			godot_log_audit_installed = true;
			godot_log_object_id = log->get_instance_id();
		}
		baseline["counts"] = log->get_message_counts();
	}
	_write_transcript_event("godot_log_baseline", baseline);
}

void SolersAgentSession::_release_godot_log_audit() {
	if (!godot_log_audit_installed) {
		return;
	}
	EditorLog *log = Object::cast_to<EditorLog>(ObjectDB::get_instance(godot_log_object_id));
	if (log) {
		log->clear_message_audit_callback(callable_mp(this, &SolersAgentSession::_on_godot_log_message));
	}
	godot_log_audit_installed = false;
	godot_log_object_id = ObjectID();
	godot_log_turn_active = false;
	{
		MutexLock lock(godot_log_mutex);
		main_thread_tool_audit.clear();
		deferred_window_audit.clear();
		worker_tool_audits.clear();
		attributable_tool_errors.clear();
	}
}

// Bounds on unique aggregated diagnostic messages: repeats fold into counts,
// so these only cap distinct message texts per call / per model boundary.
static constexpr int MAX_UNIQUE_DIAGNOSTICS_PER_CALL = 64;
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

void SolersAgentSession::_on_godot_log_message(const String &p_message, int p_type, int64_t p_source_thread) {
	if (p_type != EditorLog::MSG_TYPE_ERROR && p_type != EditorLog::MSG_TYPE_WARNING) {
		return;
	}
	const bool is_error = p_type == EditorLog::MSG_TYPE_ERROR;
	Dictionary event;
	event["severity"] = is_error ? "error" : "warning";
	event["source"] = "godot_editor";
	event["message"] = p_message;
	event["turn_active"] = godot_log_turn_active;
	event["ticks_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	event["thread_id"] = p_source_thread;
	if (!godot_log_turn_active) {
		_write_transcript_event("godot_log", event);
		return;
	}
	bool new_group = false;
	{
		MutexLock lock(godot_log_mutex);
		const Dictionary *worker_audit = worker_tool_audits.getptr((uint64_t)p_source_thread);
		Dictionary active_audit = worker_audit ? *worker_audit : main_thread_tool_audit;
		if (active_audit.is_empty() && !deferred_window_audit.is_empty()) {
			// No handler is on an audited stack, but exactly one parked tool
			// (an awaited script, a pending import) owns this window: its
			// continuation is the only session work the main loop can run.
			active_audit = deferred_window_audit;
			event["window"] = "deferred";
		}
		const String call_id = active_audit.get("call_id", String());
		if (!active_audit.is_empty()) {
			event["call_id"] = call_id;
			event["tool"] = active_audit.get("tool", String());
		}
		if (is_error) {
			godot_log_error_count++;
			if (event.has("tool") && !call_id.is_empty()) {
				Dictionary scoped = attributable_tool_errors.get(call_id, Dictionary());
				Array events = scoped.get("events", Array());
				int group_index = -1;
				for (int i = 0; i < events.size(); i++) {
					if (Dictionary(events[i]).get("message", String()) == p_message) {
						group_index = i;
						break;
					}
				}
				if (group_index >= 0 || events.size() < MAX_UNIQUE_DIAGNOSTICS_PER_CALL) {
					_solers_accumulate_diagnostic(events, event, group_index);
				} else {
					scoped["suppressed_unique_messages"] = (int)scoped.get("suppressed_unique_messages", 0) + 1;
				}
				scoped["call_id"] = call_id;
				scoped["tool"] = event.get("tool", String());
				scoped["resource_accesses"] = active_audit.get("resource_accesses", Array());
				scoped["events"] = events;
				attributable_tool_errors[call_id] = scoped;
			}
		} else {
			godot_log_warning_count++;
		}
		const String group_key = String(event.get("severity", String())) + "\n" + call_id + "\n" + p_message;
		const int *existing = pending_godot_diagnostic_index.getptr(group_key);
		if (existing) {
			_solers_accumulate_diagnostic(pending_godot_diagnostics, event, *existing);
		} else if (pending_godot_diagnostic_index.size() < MAX_UNIQUE_PENDING_DIAGNOSTICS) {
			new_group = _solers_accumulate_diagnostic(pending_godot_diagnostics, event, -1);
			pending_godot_diagnostic_index[group_key] = pending_godot_diagnostics.size() - 1;
		} else {
			pending_godot_diagnostics_overflow++;
		}
	}
	// One transcript record per unique message per boundary; occurrence
	// counts arrive with godot_diagnostics_delivered.
	if (new_group) {
		_write_transcript_event("godot_log", event);
	}
}

void SolersAgentSession::_begin_main_thread_tool_audit() {
	Dictionary audit;
	audit["call_id"] = deferred_call_id;
	audit["tool"] = deferred_canonical_name;
	audit["resource_accesses"] = deferred_resource_accesses;
	MutexLock lock(godot_log_mutex);
	main_thread_tool_audit = audit;
}

void SolersAgentSession::_end_main_thread_tool_audit() {
	MutexLock lock(godot_log_mutex);
	main_thread_tool_audit.clear();
}

void SolersAgentSession::_refresh_deferred_window_audit() {
	// Attribution stays authoritative: only when exactly one tool is parked is
	// the between-polls window unambiguously its continuation. With zero or
	// several parked tools the window stays unattributed rather than guessed.
	Dictionary audit;
	if (pending_tool_executions.size() == 1) {
		const PendingToolExecution *pending = pending_tool_executions[0];
		audit["call_id"] = pending->call_id;
		audit["tool"] = pending->canonical_name;
		audit["resource_accesses"] = pending->resource_accesses;
	}
	MutexLock lock(godot_log_mutex);
	deferred_window_audit = audit;
}

void SolersAgentSession::_register_worker_tool_audit(uint64_t p_thread_id, const String &p_call_id, const String &p_tool, const Array &p_resource_accesses) {
	if (p_thread_id == Thread::UNASSIGNED_ID) {
		return;
	}
	Dictionary audit;
	audit["call_id"] = p_call_id;
	audit["tool"] = p_tool;
	audit["resource_accesses"] = p_resource_accesses;
	MutexLock lock(godot_log_mutex);
	worker_tool_audits[p_thread_id] = audit;
}

Dictionary SolersAgentSession::_consume_attributable_tool_error(const String &p_call_id) {
	Dictionary scoped;
	{
		MutexLock lock(godot_log_mutex);
		scoped = attributable_tool_errors.get(p_call_id, Dictionary());
		attributable_tool_errors.erase(p_call_id);
		if (!scoped.is_empty()) {
			// These events are delivered inside the tool result itself, so the
			// boundary diagnostics channel keeps only unattributed evidence.
			Array remaining;
			for (int i = 0; i < pending_godot_diagnostics.size(); i++) {
				const Dictionary event = pending_godot_diagnostics[i];
				if (String(event.get("call_id", String())) != p_call_id) {
					remaining.push_back(event);
				}
			}
			pending_godot_diagnostics = remaining;
			pending_godot_diagnostic_index.clear();
			for (int i = 0; i < pending_godot_diagnostics.size(); i++) {
				const Dictionary event = pending_godot_diagnostics[i];
				pending_godot_diagnostic_index[String(event.get("severity", String())) + "\n" + String(event.get("call_id", String())) + "\n" + String(event.get("message", String()))] = i;
			}
		}
	}
	const Array events = scoped.get("events", Array());
	if (events.is_empty()) {
		return Dictionary();
	}
	bool handler_window = false;
	int total_occurrences = 0;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		total_occurrences += (int)event.get("count", 1);
		if (String(event.get("window", String())) != "deferred") {
			handler_window = true;
		}
	}
	const String first_message = Dictionary(events[0]).get("message", "Godot reported an error while the native tool handler was executing.");
	Dictionary error;
	error["code"] = "GODOT_TOOL_ERROR";
	error["message"] = total_occurrences == 1 ? first_message : vformat("Godot reported %d errors (%d distinct messages) while this tool call was executing. First error: %s", total_occurrences, events.size(), first_message);
	error["recoverable"] = true;
	error["source"] = "godot_editor";
	error["handler_window"] = handler_window;
	error["events"] = events;
	if (scoped.has("suppressed_unique_messages")) {
		error["suppressed_unique_messages"] = scoped["suppressed_unique_messages"];
	}
	return error;
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

String SolersAgentSession::_readonly_cache_key(const StringName &p_name, const Dictionary &p_args) const {
	const Dictionary normalized = tool_registry ? tool_registry->normalize_tool_args(p_name, p_args) : p_args;
	const uint64_t revision = tool_registry && tool_registry->caches_across_revisions(p_name) ? 0 : authored_revision;
	return String(p_name) + ":" + String::num_uint64(revision) + ":" + String::num_uint64(normalized.hash());
}

// Enriched mentions are the audit authority (same array the model sees).
// strip_prompt_block is display-only — never treat stripped transcript text as the fact source.
static Array _solers_enrich_mentions(const Array &p_mentions, SolersObservationService *p_observation) {
	Array enriched;
	for (int i = 0; i < p_mentions.size(); i++) {
		Dictionary mention = Dictionary(p_mentions[i]).duplicate(true);
		const String path = String(mention.get("path", String())).strip_edges();
		// Project paths only (res://). Scene-relative node paths are not observe_path inputs.
		if (p_observation && path.begins_with("res://") && !mention.has("digest")) {
			const Dictionary observed = p_observation->observe_path(path);
			if ((bool)observed.get("ok", false) && observed.has("digest")) {
				mention["digest"] = observed["digest"];
			}
		}
		enriched.push_back(mention);
	}
	return enriched;
}

static String _solers_mention_context(const Array &p_enriched_mentions) {
	return SolersMention::prompt_block(p_enriched_mentions);
}

Dictionary SolersAgentSession::queue_user_message(const Dictionary &p_args) {
	const String prompt = String(p_args.get("prompt", String())).strip_edges();
	const Array attachments = p_args.get("attachments", Array()).duplicate(true);
	const Array mentions = p_args.get("mentions", Array()).duplicate(true);
	if (prompt.is_empty() && attachments.is_empty()) {
		return _error("EMPTY_PROMPT", "Prompt is empty.");
	}
	if (!running) {
		return _error("AGENT_IDLE", "No agent turn is running; start a new turn instead.");
	}
	SolersObservationService *observation = tool_registry ? tool_registry->observation_service : nullptr;
	const Array enriched_mentions = _solers_enrich_mentions(mentions, observation);
	Dictionary message = SolersLLMMessage::user(prompt + _solers_mention_context(enriched_mentions));
	message["origin"] = "user_steering";
	message["turn_id"] = turn_id;
	if (!attachments.is_empty()) {
		message["attachments"] = attachments;
	}
	if (!enriched_mentions.is_empty()) {
		message["mentions"] = enriched_mentions;
	}
	message["event_id"] = ++transcript_event_sequence;
	pending_steering_messages.push_back(message);
	Dictionary payload;
	payload["queued"] = pending_steering_messages.size();
	_write_transcript_event("steering_queued", payload);
	Dictionary data;
	data["queued"] = pending_steering_messages.size();
	data["event_id"] = message["event_id"];
	return _ok(data);
}

bool SolersAgentSession::_flush_pending_steering() {
	if (pending_steering_messages.is_empty()) {
		return false;
	}
	for (int i = 0; i < pending_steering_messages.size(); i++) {
		const Dictionary message = pending_steering_messages[i];
		const Array mentions = message.get("mentions", Array());
		for (int mention_index = 0; mention_index < mentions.size(); mention_index++) {
			const Dictionary mention = mentions[mention_index];
			const String key = SolersMention::dedupe_key(mention);
			bool present = false;
			for (int active_index = 0; active_index < turn_mentions.size(); active_index++) {
				if (SolersMention::dedupe_key(turn_mentions[active_index]) == key) {
					present = true;
					break;
				}
			}
			if (!key.is_empty() && !present) {
				turn_mentions.push_back(mention);
			}
		}
		messages.push_back(message);
		const int64_t event_id = _write_transcript_message("user", SolersMention::strip_prompt_block(message.get("content", String())), mentions, Array(), String(), message.get("attachments", Array()), message.get("event_id", 0));
		emit_signal(SNAME("timeline_entry_committed"), event_id, SolersLLMRole::USER);
	}
	pending_steering_messages.clear();
	return true;
}

String SolersAgentSession::_default_system_prompt() const {
	String prompt =
			"You are Solers, an AI agent living natively inside the Solers game engine editor (a Godot 4 fork).\n\n"
			"Operating contract:\n"
			"- Prefer the smallest coherent native change. Inspect live state before editing; do not guess APIs or project contents.\n"
			"- Digests in [Selected Solers context] and Current engine context are authoritative bounded observations. Use tools only for incomplete or live facts.\n"
			"- Built-in tools are already available. If tool.search is present, it discovers only external plugin, connector, or MCP tools. Skills provide knowledge and never activate capabilities.\n"
			"- Inspect unfamiliar classes with engine.describe; ClassDB property metadata and native documentation are the authority for names, types, units, and usage.\n"
			"- Read live state with object.query. Edit scenes/resources with object.transaction and its native state/hash preconditions. Use script.compute for isolated bulk generation with declared file outputs.\n"
			"- Scene transactions are one EditorUndoRedo action and Solers persists successful live-scene changes. Resource transactions are file-checkpointed. Tool results carry native state receipts and persisted file hashes; do not issue a separate save.\n"
			"- The 3D editor viewport is the live edited scene tree. Use object.transaction for visible scene construction; scripts own runtime behavior. Establish appearance only with render.capture target=editor|camera tied to the source_state receipt (runtime proves Play only). Use object.query target=relations for world-space relations; a screenshot is not a geometry measurement.\n"
			"- Background tools return stable job ids immediately. Continue independent work; when nothing else is runnable, call job.wait once with the required ids and stop issuing tools. Solers parks this turn and resumes it with a background job delta when any requested job reaches a project-import terminal state; do not poll asset.status for progress.\n"
			"- Tool errors carry the native cause; read it, change what it names, and retry. Repeating an identical failed call wastes a step.\n"
			"- Before each non-trivial tool call or group of related calls, write one short sentence saying what you are about to do and why. Group related actions under one preamble; skip it for trivial reads. This narration is how the user follows your progress.\n"
			"- Use update_plan only as a concise optional progress display. Text without tool calls ends the task, so keep progress notes attached to tool-calling turns and finish with a clear final summary.";
	if (tool_registry) {
		const String skill_catalog = tool_registry->get_skill_catalog_prompt();
		if (!skill_catalog.is_empty()) {
			prompt += "\n\n" + skill_catalog;
		}
	}
	return prompt;
}

Dictionary SolersAgentSession::_environment_context_message() {
	if (!tool_registry || !tool_registry->observation_service) {
		return Dictionary();
	}
	SolersObservationService *observation = tool_registry->observation_service;
	Dictionary context;
	context["project"] = observation->get_project_info();
	context["session_revision"] = (int64_t)authored_revision;
	context["observed_revision"] = (int64_t)observed_revision;
	const Dictionary runtime_status = observation->get_runtime_status();
	context["runtime"] = runtime_status;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	Node *edited_root = editor_interface ? editor_interface->get_edited_scene_root() : nullptr;
	const String current_scene_path = edited_root ? edited_root->get_scene_file_path() : String();
	context["current_scene"] = current_scene_path;
	if (edited_root && current_scene_path.is_empty()) {
		context["current_scene_unsaved"] = true;
	}

	Dictionary observe_args;
	observe_args["since_cursor"] = (int64_t)runtime_observation_cursor;
	const Dictionary runtime_observations = observation->observe_runtime(observe_args);
	runtime_observation_cursor = (int64_t)runtime_observations.get("cursor", runtime_observation_cursor);
	const Dictionary error_digest = runtime_observations.get("error_digest", Dictionary());
	if (!error_digest.is_empty()) {
		context["runtime_error_digest"] = error_digest;
		context["runtime_epoch_error_count"] = runtime_observations.get("epoch_error_count", 0);
	}
	// Editor diagnostics are current engine facts, so they ride this
	// per-request message instead of being appended to durable history where
	// one noisy turn would keep paying for itself on every later request.
	const Dictionary diagnostics = _take_godot_diagnostics();
	if (!diagnostics.is_empty()) {
		context["godot_diagnostics"] = diagnostics;
	}
	Dictionary turn_diagnostics;
	{
		MutexLock lock(godot_log_mutex);
		turn_diagnostics["errors"] = godot_log_error_count;
		turn_diagnostics["warnings"] = godot_log_warning_count;
	}
	context["turn_diagnostics"] = turn_diagnostics;
	if (!latest_mutation_receipt.is_empty()) {
		bool current = true;
		const Dictionary scene = latest_mutation_receipt.get("scene_after", Dictionary());
		if (!scene.is_empty()) {
			const int history_id = EditorNode::get_editor_data().get_current_edited_scene_history_id();
			EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
			UndoRedo *history = manager ? manager->get_history_undo_redo(history_id) : nullptr;
			current = edited_root && (int64_t)edited_root->get_instance_id() == (int64_t)scene.get("root_object_id", 0) && history_id == (int)scene.get("history_id", -1) && history && (int64_t)history->get_version() == (int64_t)scene.get("version", -1);
		}
		const Array resources = latest_mutation_receipt.get("resources_after", Array());
		for (int i = 0; current && i < resources.size(); i++) {
			const Dictionary resource = resources[i];
			const String path = resource.get("path", String());
			current = FileAccess::exists(path) == (bool)resource.get("exists", false) && (!resource.has("sha256") || FileAccess::get_sha256(path) == String(resource["sha256"]));
		}
		if (current) {
			context["latest_mutation_receipt"] = latest_mutation_receipt;
		} else {
			latest_mutation_receipt.clear();
		}
	}
	Dictionary message = SolersLLMMessage::user(
			"Current engine context (authoritative, bounded, and refreshed for this request):\n" + JSON::stringify(context, "", false, true));
	message["origin"] = "solers_state";
	return message;
}

Array SolersAgentSession::_collect_tools() {
	Array out;
	if (!tool_registry) {
		return out;
	}
	const uint64_t catalog_revision = tool_registry->get_tool_catalog_revision();
	if (cached_tool_catalog_revision == catalog_revision && cached_request_deferred_count == task_deferred_tools.size()) {
		return cached_request_tools;
	}
	const Array defs = tool_registry->list_tools();
	for (int i = 0; i < defs.size(); i++) {
		const Dictionary def = defs[i];
		const String exposure = def.get("exposure", "direct");
		const StringName canonical_name = StringName(def.get("name", String()));
		if (exposure == "hidden" || (exposure == "deferred" && !task_deferred_tools.has(canonical_name))) {
			continue;
		}
		Dictionary tool;
		tool["name"] = def.get("model_name", def.get("name", String()));
		tool["canonical_name"] = def.get("name", String());
		tool["description"] = def.get("description", String());
		tool["parameters"] = def.get("input_schema", Dictionary());
		out.push_back(tool);
	}
	cached_request_tools = out;
	cached_request_tool_tokens = SolersContextManager::estimate_tokens(JSON::stringify(out, "", false, true));
	cached_request_deferred_count = task_deferred_tools.size();
	cached_tool_catalog_revision = catalog_revision;
	return cached_request_tools;
}

bool SolersAgentSession::_refresh_active_model_limits() {
	if (!settings_service || !models_dev) {
		return false;
	}
	const String provider_id = active_provider.get("provider", String());
	const String model_id = active_provider.get("model", String());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, active_provider.get("base_url", String()));
	const StringName catalog_provider = StringName(profile.get("catalog_provider", provider_id));
	const Dictionary model = models_dev->get_model(catalog_provider, model_id);

	const bool explicit_context = active_provider.has("context_window");
	const bool catalog_limits_authoritative = profile.get("catalog_limits_authoritative", false);
	const Dictionary model_limits = Dictionary(profile.get("model_limits", Dictionary())).get(model_id, Dictionary());
	int resolved_context = SolersContextManager::DEFAULT_CONTEXT_TOKENS;
	if (catalog_limits_authoritative) {
		resolved_context = (int)profile.get("context_window", 0);
		if ((int)model.get("context", 0) > 0) {
			resolved_context = model.get("context", 0);
		}
		if ((int)model_limits.get("context", 0) > 0) {
			resolved_context = model_limits.get("context", 0);
		}
	}
	if (explicit_context) {
		resolved_context = (int)active_provider["context_window"];
	}
	int resolved_output = (int)profile.get("max_output_tokens", 8192);
	if ((int)model.get("output", 0) > 0) {
		resolved_output = model.get("output", 0);
	}
	if ((int)model_limits.get("output", 0) > 0) {
		resolved_output = model_limits.get("output", 0);
	}
	if (active_provider.has("max_tokens")) {
		resolved_output = (int)active_provider["max_tokens"];
	}

	const int previous_context = context_window;
	const int previous_output = max_output_tokens;
	context_window = resolved_context > 0 ? resolved_context : 0;
	// Catalog output is advisory only. OpenCode hard-caps wire output at 32k so
	// a lying catalog (output≈context≈500k) cannot starve input or force compact.
	static constexpr int SOLERS_OUTPUT_TOKEN_MAX = 32000;
	static constexpr int SOLERS_OUTPUT_INPUT_RESERVE = 8192;
	if (resolved_output <= 0) {
		resolved_output = SolersContextManager::DEFAULT_OUTPUT_TOKENS;
	}
	resolved_output = MIN(resolved_output, SOLERS_OUTPUT_TOKEN_MAX);
	if (context_window > 0) {
		resolved_output = MIN(resolved_output, MAX(1024, context_window - SOLERS_OUTPUT_INPUT_RESERVE));
	}
	max_output_tokens = resolved_output;
	return context_window != previous_context || max_output_tokens != previous_output;
}

int SolersAgentSession::_active_model_input_support(const String &p_modality) const {
	if (!settings_service || !models_dev) {
		return -1;
	}
	const String provider_id = active_provider.get("provider", String());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, active_provider.get("base_url", String()));
	const StringName catalog_provider = StringName(profile.get("catalog_provider", provider_id));
	return SolersModelsDev::input_modality_support(models_dev->get_model(catalog_provider, active_provider.get("model", String())), p_modality);
}

Dictionary SolersAgentSession::_build_request(const Array &p_messages, const String &p_request_system_prompt, const Array &p_tools) const {
	Dictionary request;
	request["model"] = active_provider.get("model", String());
	request["system"] = p_request_system_prompt;
	request["tools"] = p_tools;
	Array provider_messages;
	for (int i = 0; i < p_messages.size(); i++) {
		Dictionary message = p_messages[i];
		if (String(message.get("role", String())) == SolersContextManager::MODEL_CONTEXT_ROLE) {
			message = message.duplicate(true);
			message["role"] = SolersLLMRole::USER;
		}
		provider_messages.push_back(message);
	}
	request["messages"] = provider_messages;
	const String reasoning_effort = String(active_provider.get("reasoning_effort", String())).strip_edges();
	if (!reasoning_effort.is_empty()) {
		const String provider_id = active_provider.get("provider", String());
		const Dictionary active_profile = settings_service ? settings_service->resolve_provider_profile(provider_id, active_provider.get("base_url", String())) : Dictionary();
		const StringName catalog_provider = StringName(active_profile.get("catalog_provider", provider_id));
		const Dictionary model = models_dev ? models_dev->get_model(catalog_provider, active_provider.get("model", String())) : Dictionary();
		if (SolersModelsDev::reasoning_efforts(model).has(reasoning_effort)) {
			request["reasoning_effort"] = reasoning_effort;
		}
	}
	const Dictionary profile = active_provider.get("profile", Dictionary());
	if (profile.get("supports_max_output_tokens", true)) {
		request["max_tokens"] = max_output_tokens;
	}
	request["session_id"] = session_id;
	return request;
}

Dictionary SolersAgentSession::_provider_dispatch_error() const {
	if (!settings_service) {
		return _error("AGENT_UNCONFIGURED", "Solers agent session is missing its settings service.");
	}
	const String provider = active_provider.get("provider", String());
	const Dictionary current = settings_service->get_provider_config_for(provider).get("data", Dictionary());
	if (!current.get("connected", false)) {
		return _error("PROVIDER_NOT_CONNECTED", "The selected provider is not connected. Open Provider Settings to connect it.");
	}
	if (!current.get("available", false)) {
		return _error("LOCAL_MODELS_ONLY", "Local Models Only blocks the selected remote provider. Choose a local model or disable Local Models Only in Provider Settings.");
	}
	return Dictionary();
}

void SolersAgentSession::_commit_attachment_projection() {
	const Array emitted = client ? client->get_emitted_attachment_identities() : Array();
	for (int i = 0; i < emitted.size(); i++) {
		pending_model_attachments.insert(emitted[i]);
	}
	if (pending_model_attachments.is_empty()) {
		return;
	}
	Array identities;
	for (const String &key : pending_model_attachments) {
		delivered_model_attachments.insert(key);
		identities.push_back(key);
	}
	Dictionary event;
	event["identities"] = identities;
	_write_transcript_event("attachment_projection_committed", event);
	pending_model_attachments.clear();
}

Error SolersAgentSession::_dispatch_model_request(bool p_skip_compaction) {
	const Dictionary availability_error = _provider_dispatch_error();
	if (!availability_error.is_empty()) {
		const Dictionary error = availability_error.get("error", Dictionary());
		_finish_turn("failed", error.get("message", "The selected provider is unavailable."), error);
		return ERR_UNAVAILABLE;
	}
	_append_background_asset_deltas(false);
	_flush_pending_steering();
	// Drop prior-turn usage so compaction headroom is not computed from a
	// stale prompt total that already included the previous max_output reserve.
	last_usage.clear();
	last_stop_reason = String();
	if (_refresh_active_model_limits()) {
		Dictionary limits;
		limits["context_window"] = context_window;
		limits["max_output_tokens"] = max_output_tokens;
		limits["known"] = context_window > 0;
		_write_transcript_event("model_limits_updated", limits);
		_record("agent_model_limits_updated", limits);
	}
	const String provider_id = active_provider.get("provider", String());
	const String base_url = active_provider.get("base_url", String());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url);
	if (system_prompt.is_empty()) {
		system_prompt = _default_system_prompt();
	}
	const Array tools = _collect_tools();
	Array request_messages = messages.duplicate(true);
	// Dynamic engine facts ride at the end. Attachment projection then replaces
	// previously consumed image bytes with stable hash references; the system,
	// tools, and text history remain cacheable.
	const Dictionary environment_message = _environment_context_message();
	request_transient_tokens = 0;
	if (!environment_message.is_empty()) {
		request_messages.push_back(environment_message);
		Array transient;
		transient.push_back(environment_message);
		request_transient_tokens = SolersContextManager::estimate_messages_tokens(transient);
	}
	if (!p_skip_compaction && context_manager && context_manager->should_compact(messages, system_prompt, cached_request_tool_tokens, context_window, max_output_tokens, request_transient_tokens)) {
		return _begin_compaction(false);
	}
	Dictionary request = _build_request(request_messages, system_prompt, tools);
	Array delivered_attachments;
	for (const String &identity : delivered_model_attachments) {
		delivered_attachments.push_back(identity);
	}
	request["_delivered_attachment_identities"] = delivered_attachments;
	phase = PHASE_STREAMING;
	streamed_tool_calls.clear();
	return _begin_provider_request(request, profile);
}

Error SolersAgentSession::_begin_provider_request(const Dictionary &p_request, const Dictionary &p_profile) {
	retry_request = p_request.duplicate(true);
	retry_profile = p_profile.duplicate(true);
	current_provider_metadata.clear();
	model_request_index++;
	const Error err = client->begin(retry_request, retry_profile, active_provider.get("auth", Dictionary()));
	if (err != OK) {
		const Dictionary error = client->get_error();
		_finish_turn("failed", String(error.get("message", "Failed to start the model request.")), error);
		return err;
	}
	Dictionary event;
	event["request_index"] = model_request_index;
	event["provider"] = active_provider.get("provider", String());
	event["model"] = active_provider.get("model", String());
	event["message_count"] = Array(retry_request.get("messages", Array())).size();
	_write_transcript_event("model_request_started", event);
	running = true;
	text_delta_count = 0;
	last_text_delta_msec = 0;
	if (phase == PHASE_STREAMING) {
		emit_signal(SNAME("model_request_started"));
	}
	return OK;
}

Error SolersAgentSession::_begin_compaction(bool p_from_overflow) {
	if (!context_manager || messages.is_empty()) {
		return ERR_UNAVAILABLE;
	}
	compaction_source_messages = messages.duplicate(true);
	current_text = String();
	current_reasoning = String();
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	last_stop_reason = String();
	phase = PHASE_COMPACTING;
	Dictionary payload;
	payload["source"] = p_from_overflow ? "overflow" : "auto";
	_collect_tools();
	payload["tokens_before"] = context_manager->get_token_count_with_pending(messages, system_prompt, cached_request_tool_tokens);
	compaction_id = ++transcript_event_sequence;
	compaction_timeline_event_id = _write_transcript_compaction("started", payload);
	emit_signal(SNAME("timeline_entry_committed"), compaction_timeline_event_id, "context_compaction");
	return _dispatch_compaction_request();
}

Error SolersAgentSession::_dispatch_compaction_request() {
	const Dictionary availability_error = _provider_dispatch_error();
	if (!availability_error.is_empty()) {
		const Dictionary error = availability_error.get("error", Dictionary());
		_finish_turn("failed", error.get("message", "The selected provider is unavailable."), error);
		return ERR_UNAVAILABLE;
	}
	_refresh_active_model_limits();
	last_usage.clear();
	last_stop_reason = String();
	const String provider_id = active_provider.get("provider", String());
	const String base_url = active_provider.get("base_url", String());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url);

	Array history = compaction_source_messages.duplicate(true);
	history.push_back(SolersLLMMessage::user(
			"Write a concise continuation note containing only the user's intent, constraints, decisions, and unresolved questions. Do not include node or resource identities, measurements, engine state, completed work, test claims, tool results, or a precise next tool action; those facts are refreshed by the engine. Respond with text only and do not call tools."));
	Dictionary request = _build_request(history, system_prompt, Array());
	Array delivered_attachments;
	for (const String &identity : delivered_model_attachments) {
		delivered_attachments.push_back(identity);
	}
	request["_delivered_attachment_identities"] = delivered_attachments;
	phase = PHASE_COMPACTING;
	request_transient_tokens = 0;
	return _begin_provider_request(request, profile);
}

bool SolersAgentSession::_is_context_overflow(const Dictionary &p_error) const {
	return String(p_error.get("failure_kind", String())) == "context_overflow";
}

bool SolersAgentSession::_schedule_llm_retry(const Dictionary &p_error) {
	if (!SolersLLMRetry::is_retryable(p_error) || retry_attempt >= MAX_LLM_RETRY_ATTEMPTS) {
		return false;
	}
	retry_attempt++;
	const uint64_t wait = SolersLLMRetry::delay_msec(retry_attempt, p_error);
	retry_resume_msec = OS::get_singleton()->get_ticks_msec() + wait;
	current_text = String();
	current_reasoning = String();
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	Dictionary payload;
	payload["attempt"] = retry_attempt;
	payload["delay_msec"] = (int)wait;
	payload["code"] = p_error.get("code", String());
	payload["http_status"] = p_error.get("http_status", 0);
	payload["phase"] = phase == PHASE_COMPACTING ? "compaction" : "model";
	_record("agent_turn_retrying", payload);
	_write_transcript_event("model_retry", payload);
	String message = String(p_error.get("message", String())).strip_edges();
	const int http_status = p_error.get("http_status", 0);
	if (http_status > 0) {
		message = vformat("HTTP %d%s", http_status, message.is_empty() ? String() : ": " + message);
	}
	message += vformat(" (retrying in %s s)", String::num(double(wait) / 1000.0, 1));
	emit_signal(SNAME("turn_retrying"), retry_attempt, message);
	return true;
}

void SolersAgentSession::_poll_compaction() {
	const Array events = client->poll();
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		const String kind = event.get("kind", String());
		if (kind == SolersLLMEventKind::TEXT_DELTA) {
			current_text += String(event.get("text", String()));
		} else if (kind == SolersLLMEventKind::REASONING_DELTA) {
			current_reasoning += String(event.get("text", String()));
		} else if (kind == SolersLLMEventKind::USAGE) {
			last_usage = event;
			turn_fresh_input_tokens += MAX(0, (int)event.get("input_tokens", 0));
			turn_cache_read_tokens += MAX(0, (int)event.get("cache_read_tokens", 0));
			turn_cache_write_tokens += MAX(0, (int)event.get("cache_write_tokens", 0));
			turn_output_tokens += MAX(0, (int)event.get("output_tokens", 0));
		} else if (kind == SolersLLMEventKind::FINISH) {
			last_stop_reason = event.get("stop_reason", String());
		}
	}

	if (client->is_failed()) {
		turn_wire_body_bytes += client->get_request_body_bytes();
		const Dictionary error = client->get_error();
		if (_schedule_llm_retry(error)) {
			return;
		}
		_finish_turn("failed", String(error.get("message", "Context compaction failed.")), error);
		return;
	}
	if (!client->is_done()) {
		return;
	}
	if (current_text.strip_edges().is_empty()) {
		const Dictionary error = _error("COMPACTION_FAILED", "The model did not produce a usable context summary.").get("error", Dictionary());
		_finish_turn("failed", String(error.get("message", "Context compaction failed.")), error);
		return;
	}
	turn_wire_body_bytes += client->get_request_body_bytes();
	_on_compaction_complete();
}

void SolersAgentSession::_on_compaction_complete() {
	_commit_attachment_projection();
	const int projection_budget = MAX(0, context_window - max_output_tokens - SolersContextManager::estimate_tokens(system_prompt) - cached_request_tool_tokens);
	const Dictionary result = context_manager->apply_compaction(compaction_source_messages, current_text, projection_budget);
	messages = result.get("messages", Array());
	_write_transcript_compaction("completed", result);
	emit_signal(SNAME("timeline_entry_committed"), compaction_timeline_event_id, "context_compaction");

	compaction_source_messages.clear();
	compaction_id = 0;
	compaction_timeline_event_id = 0;
	retry_attempt = 0;
	retry_resume_msec = 0;
	current_text = String();
	current_reasoning = String();
	last_stop_reason = String();
	_dispatch_model_request(true);
}

Dictionary SolersAgentSession::_tool_call_from_event(const Dictionary &p_event) const {
	const String requested_name = p_event.get("name", String());
	const StringName canonical_name = tool_registry ? tool_registry->resolve_model_tool_name(requested_name) : StringName();
	String model_name = requested_name;
	if (!String(canonical_name).is_empty() && tool_registry) {
		const String registered_model_name = tool_registry->get_model_tool_name(canonical_name);
		if (!registered_model_name.is_empty()) {
			model_name = registered_model_name;
		}
	}

	Dictionary call;
	call["id"] = p_event.get("id", String());
	call["name"] = model_name;
	call["canonical_name"] = String(canonical_name);
	call["requested_name"] = requested_name;
	call["arguments"] = p_event.get("arguments", String());
	const Dictionary provider_metadata = p_event.get("provider_metadata", Dictionary());
	if (!provider_metadata.is_empty()) {
		call["provider_metadata"] = provider_metadata;
		if (provider_metadata.has("status")) {
			call["status"] = provider_metadata["status"];
		}
	}
	return call;
}

Dictionary SolersAgentSession::_merge_streamed_tool_call(const Dictionary &p_call) {
	Dictionary call = p_call.duplicate(true);
	const String id = call.get("id", String());
	if (id.is_empty()) {
		return call;
	}

	const Dictionary previous = streamed_tool_calls.get(id, Dictionary());
	if (!previous.is_empty()) {
		if (String(call.get("name", String())).is_empty()) {
			call["name"] = previous.get("name", String());
		}
		if (String(call.get("canonical_name", String())).is_empty()) {
			call["canonical_name"] = previous.get("canonical_name", String());
		}
		if (String(call.get("requested_name", String())).is_empty()) {
			call["requested_name"] = previous.get("requested_name", String());
		}
		if (String(call.get("arguments", String())).is_empty()) {
			call["arguments"] = previous.get("arguments", String());
		}
		if ((bool)previous.get("ui_announced", false)) {
			call["ui_announced"] = true;
		}
	}
	streamed_tool_calls[id] = call;
	return call;
}

Dictionary SolersAgentSession::_surface_tool_call(const Dictionary &p_call) {
	Dictionary call = _merge_streamed_tool_call(p_call);
	const String id = call.get("id", String());
	if (id.is_empty()) {
		return call;
	}

	const bool was_announced = (bool)call.get("ui_announced", false);
	call["ui_announced"] = true;
	streamed_tool_calls[id] = call;
	const String canonical_name = call.get("canonical_name", String());
	const String arguments = call.get("arguments", String());
	if (_is_session_tool(canonical_name)) {
		return call;
	}
	if (was_announced) {
		emit_signal(SNAME("tool_call_updated"), id, canonical_name, arguments);
	} else {
		emit_signal(SNAME("tool_call_started"), id, canonical_name, arguments);
	}
	return call;
}

Array SolersAgentSession::_attachments_for_ids(const Array &p_ids) const {
	Array out;
	for (int i = 0; i < p_ids.size(); i++) {
		const String wanted = String(p_ids[i]).strip_edges();
		if (wanted.is_empty()) {
			continue;
		}
		bool found = false;
		for (int m = messages.size() - 1; m >= 0; m--) {
			const Dictionary message = messages[m];
			const Array attachments = message.get("attachments", Array());
			for (int a = 0; a < attachments.size(); a++) {
				const Dictionary attachment = attachments[a];
				if (String(attachment.get("id", String())).strip_edges() == wanted) {
					out.push_back(attachment);
					found = true;
					break;
				}
			}
			if (found) {
				break;
			}
		}
	}
	return out;
}

Dictionary SolersAgentSession::start_turn(const Dictionary &p_args) {
	if (running) {
		return _error("AGENT_BUSY", "A Solers agent turn is already running.");
	}
	if (!tool_registry || !settings_service) {
		return _error("AGENT_UNCONFIGURED", "Solers agent session is missing its services.");
	}

	const String prompt = String(p_args.get("prompt", String())).strip_edges();
	turn_attachments = p_args.get("attachments", Array()).duplicate(true);
	turn_mentions = p_args.get("mentions", Array()).duplicate(true);
	if (prompt.is_empty() && turn_attachments.is_empty()) {
		return _error("EMPTY_PROMPT", "Prompt is empty.");
	}
	background_resume_suppressed = false;
	waiting_background_asset_ids.clear();
	active_provider = settings_service->resolve_active_provider();
	const String provider_id = active_provider.get("provider", String());
	const String model = active_provider.get("model", String());
	const String base_url = active_provider.get("base_url", String());
	const Dictionary auth = active_provider.get("auth", Dictionary());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url);
	const bool local = profile.get("local", false);

	_refresh_active_model_limits();
	const Dictionary availability_error = _provider_dispatch_error();
	if (!availability_error.is_empty()) {
		Dictionary e = availability_error;
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (models_dev) {
		models_dev->refresh();
	}
	if (model.is_empty()) {
		Dictionary e = _error("NO_MODEL", "No model is configured. Choose one from a connected provider in the Chat panel.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (!settings_service->is_model_allowed(provider_id, model)) {
		Dictionary e = _error("MODEL_NOT_ALLOWED", "The selected model is not available through this provider connection.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (String(profile.get("base_url", String())).strip_edges().is_empty()) {
		Dictionary e = _error("NO_BASE_URL", "No base URL configured for this provider.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	const String auth_type = auth.get("type", String());
	const bool credential_ready = auth_type == "oauth" ? (!String(auth.get("access", String())).is_empty() || !String(auth.get("refresh", String())).is_empty()) : !String(auth.get("key", String())).is_empty();
	if (!local && !credential_ready) {
		Dictionary e = _error("NO_PROVIDER_CREDENTIAL", "The selected provider is not connected. Open Provider Settings to connect it.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (!turn_attachments.is_empty() && _active_model_input_support("image") == 0) {
		Dictionary e = _error("VISION_CAPABILITY_REQUIRED", "The selected model does not support image input. Choose a vision-capable model before sending image references.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}

	if (system_prompt.is_empty()) {
		system_prompt = _default_system_prompt();
	}
	SolersObservationService *observation_for_mentions = tool_registry ? tool_registry->observation_service : nullptr;
	turn_mentions = _solers_enrich_mentions(turn_mentions, observation_for_mentions);
	String model_prompt = prompt + _solers_mention_context(turn_mentions);
	if (!turn_attachments.is_empty()) {
		model_prompt += model_prompt.is_empty() ? String() : "\n\n";
		model_prompt += "[Attached images available for tools]\n";
		for (int i = 0; i < turn_attachments.size(); i++) {
			const Dictionary attachment = turn_attachments[i];
			model_prompt += vformat("- %s (%s)\n", String(attachment.get("id", String())), String(attachment.get("filename", String("image"))));
		}
	}
	turn_id++;
	Dictionary user_message = SolersLLMMessage::user(model_prompt);
	user_message["turn_id"] = turn_id;
	if (!turn_attachments.is_empty()) {
		user_message["attachments"] = turn_attachments.duplicate(true);
	}
	if (!turn_mentions.is_empty()) {
		user_message["mentions"] = turn_mentions.duplicate(true);
	}
	messages = SolersContextManager::project_completed_turns(messages);
	if (context_manager) {
		context_manager->reset();
	}
	messages.push_back(user_message);
	current_text = String();
	current_reasoning = String();
	current_provider_metadata.clear();
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	readonly_cache.clear();
	failed_resource_accesses.clear();
	last_usage.clear();
	overflow_compaction_attempts = 0;
	retry_attempt = 0;
	retry_resume_msec = 0;
	retry_request.clear();
	retry_profile.clear();
	model_request_index = 0;
	turn_fresh_input_tokens = 0;
	turn_cache_read_tokens = 0;
	turn_cache_write_tokens = 0;
	turn_output_tokens = 0;
	turn_wire_body_bytes = 0;
	_ensure_godot_log_audit(true);

	Dictionary turn_started;
	turn_started["model"] = model;
	turn_started["provider"] = provider_id;
	turn_started["context_window"] = context_window;
	turn_started["max_output_tokens"] = max_output_tokens;
	turn_started["model_limits_known"] = context_window > 0;
	_write_transcript_event("turn_started", turn_started);
	_record("agent_turn_started", turn_started);
	// Persist the same enriched mentions + attachment ids the model received (display still strips the prompt block).
	const int64_t user_event_id = _write_transcript_message("user", prompt, turn_mentions, Array(), String(), turn_attachments);

	const Error err = _dispatch_model_request();
	if (err != OK) {
		return _error("DISPATCH_FAILED", "Failed to dispatch the model request.");
	}
	Dictionary data;
	data["turn_id"] = turn_id;
	data["event_id"] = user_event_id;
	return _ok(data);
}

void SolersAgentSession::poll() {
	if (!running || !client) {
		return;
	}
	const Dictionary auth_update = client->take_auth_update();
	if (!auth_update.is_empty() && settings_service) {
		const String provider = active_provider.get("provider", String());
		settings_service->store_provider_auth(provider, auth_update);
		active_provider["auth"] = auth_update;
	}
	if (retry_resume_msec != 0) {
		if (OS::get_singleton()->get_ticks_msec() < retry_resume_msec) {
			return;
		}
		retry_resume_msec = 0;
		_begin_provider_request(retry_request, retry_profile);
		return;
	}
	if (phase == PHASE_WAITING) {
		return;
	}
	if (phase == PHASE_AWAITING_APPROVAL || phase == PHASE_TOOL_EXECUTING || phase == PHASE_TOOLS) {
		// Drive the tool queue on the main loop: MAIN_THREAD tools still run
		// one-at-a-time (single execution slot). Pending/parked calls release
		// the slot so the next non-conflicting call can start; WORKER_THREAD
		// tools and unresolved approvals stop this bounded drive loop.
		const int drive_budget = MAX(8, tool_queue.size() * 4 + pending_tool_executions.size() * 2);
		for (int i = 0; i < drive_budget && running; i++) {
			const Phase before_phase = phase;
			const int before_queue = tool_queue_index;
			const int before_delivery = tool_delivery_index;
			const int before_pending = pending_tool_executions.size();
			const uint64_t before_token = tool_exec_token;
			if (phase == PHASE_AWAITING_APPROVAL) {
				_poll_awaiting_approval();
			} else if (phase == PHASE_TOOL_EXECUTING) {
				_poll_tool_executing();
			} else if (phase == PHASE_TOOLS) {
				_poll_tool_queue();
			} else {
				break;
			}
			if (phase == PHASE_STREAMING || phase == PHASE_COMPACTING || !running) {
				break;
			}
			const bool progressed = phase != before_phase || tool_queue_index != before_queue || tool_delivery_index != before_delivery || pending_tool_executions.size() != before_pending || tool_exec_token != before_token;
			if (!progressed) {
				break;
			}
		}
		return;
	}
	if (phase == PHASE_COMPACTING) {
		_poll_compaction();
		return;
	}
	const Array events = client->poll();
	for (int i = 0; i < events.size(); i++) {
		const Dictionary e = events[i];
		const String kind = e.get("kind", String());
		if (kind == SolersLLMEventKind::TEXT_DELTA) {
			const String text = e.get("text", String());
			current_text += text;
			const uint64_t now = OS::get_singleton()->get_ticks_msec();
			const uint64_t gap = last_text_delta_msec ? (now - last_text_delta_msec) : 0;
			last_text_delta_msec = now;
			text_delta_count++;
			if (text_delta_count == 1 || gap > 800 || (text_delta_count % 40) == 0) {
				SOLERS_TRACE("session.text_delta", vformat("#%d gap=%dms +%dB total=%dB", text_delta_count, (int)gap, text.length(), current_text.length()));
			}
			emit_signal(SNAME("assistant_delta"), text);
		} else if (kind == SolersLLMEventKind::REASONING_DELTA) {
			const String text = e.get("text", String());
			current_reasoning += text;
			emit_signal(SNAME("reasoning_delta"), text);
		} else if (kind == SolersLLMEventKind::TOOL_INPUT_START || kind == SolersLLMEventKind::TOOL_INPUT_DELTA) {
			_surface_tool_call(_tool_call_from_event(e));
		} else if (kind == SolersLLMEventKind::TOOL_CALL) {
			Dictionary call = _surface_tool_call(_tool_call_from_event(e));
			pending_tool_calls.push_back(call);
		} else if (kind == SolersLLMEventKind::USAGE) {
			last_usage = e;
			turn_output_tokens += MAX(0, (int)e.get("output_tokens", 0));
			if (context_manager) {
				// Canonical input_tokens excludes cached shares; the window is
				// occupied by fresh + cache-read + cache-write together.
				const int fresh_tokens = MAX(0, (int)e.get("input_tokens", 0));
				const int cache_read_tokens = MAX(0, (int)e.get("cache_read_tokens", 0));
				const int cache_write_tokens = MAX(0, (int)e.get("cache_write_tokens", 0));
				const int prompt_tokens = fresh_tokens + cache_read_tokens + cache_write_tokens;
				turn_fresh_input_tokens += fresh_tokens;
				turn_cache_read_tokens += cache_read_tokens;
				turn_cache_write_tokens += cache_write_tokens;
				context_manager->record_usage(prompt_tokens, messages.size(), request_transient_tokens);
			}
		} else if (kind == SolersLLMEventKind::FINISH) {
			last_stop_reason = e.get("stop_reason", String());
			current_provider_metadata = e.get("provider_metadata", Dictionary());
		}
	}

	if (client->is_failed()) {
		turn_wire_body_bytes += client->get_request_body_bytes();
		const Dictionary error = client->get_error();
		if (_is_context_overflow(error)) {
			overflow_compaction_attempts++;
			if (overflow_compaction_attempts == 1) {
				_begin_compaction(true);
				return;
			}
		}
		if (_schedule_llm_retry(error)) {
			return;
		}
		_finish_turn("failed", String(error.get("message", "Agent turn failed.")), error);
		return;
	}
	if (client->is_done()) {
		_on_model_turn_complete();
	}
}

void SolersAgentSession::_on_model_turn_complete() {
	const int64_t wire_body_bytes = client->get_request_body_bytes();
	turn_wire_body_bytes += wire_body_bytes;
	_commit_attachment_projection();
	retry_attempt = 0;
	Dictionary response_event;
	response_event["request_index"] = model_request_index;
	response_event["stop_reason"] = last_stop_reason;
	response_event["tool_call_count"] = pending_tool_calls.size();
	response_event["text_bytes"] = current_text.utf8().length();
	response_event["wire_body_bytes"] = wire_body_bytes;
	response_event["new_attachment_count"] = client->get_emitted_attachment_identities().size();
	if (!last_usage.is_empty()) {
		response_event["usage"] = last_usage;
	}
	_write_transcript_event("model_response", response_event);
	HashSet<String> operation_ids;
	for (int i = 0; i < pending_tool_calls.size(); i++) {
		const String operation_id = String(Dictionary(pending_tool_calls[i]).get("id", String())).strip_edges();
		if (operation_id.is_empty() || operation_ids.has(operation_id)) {
			Dictionary error;
			error["code"] = "INVALID_TOOL_CALL_ID";
			error["message"] = operation_id.is_empty() ? "The provider returned a tool call without a stable operation id." : vformat("The provider reused operation id '%s' for more than one tool call in the same response.", operation_id);
			error["recoverable"] = true;
			_finish_turn("failed", error.get("message", String()), error);
			return;
		}
		operation_ids.insert(operation_id);
	}
	// A max-token empty response gets one bounded recovery, never a retry loop.
	if (pending_tool_calls.is_empty() && current_text.is_empty() && current_reasoning.is_empty()) {
		if (last_stop_reason == SolersLLMStopReason::MAX_TOKENS && !messages.is_empty()) {
			overflow_compaction_attempts++;
			if (overflow_compaction_attempts == 1 && _begin_compaction(true) == OK) {
				return;
			}
		}
		Dictionary error;
		error["code"] = "EMPTY_MODEL_RESPONSE";
		error["recoverable"] = true;
		if (last_stop_reason.is_empty()) {
			error["message"] = "The model stream ended without a finish signal or content. Check the provider connection and try again.";
		} else {
			error["message"] = vformat("The model returned an empty response (stop=%s). Check max_tokens budget or try again.", last_stop_reason);
		}
		_finish_turn("failed", error.get("message", String()), error);
		return;
	}
	overflow_compaction_attempts = 0;
	messages.push_back(SolersLLMMessage::assistant(current_text, pending_tool_calls, current_provider_metadata));
	if (!current_text.is_empty() || !pending_tool_calls.is_empty() || !current_reasoning.is_empty()) {
		const int64_t event_id = _write_transcript_message("assistant", current_text, Array(), pending_tool_calls, current_reasoning);
		emit_signal(SNAME("timeline_entry_committed"), event_id, SolersLLMRole::ASSISTANT);
	}
	if (pending_tool_calls.is_empty()) {
		const String final_text = current_text;
		if (!current_text.is_empty()) {
			emit_signal(SNAME("assistant_message"), current_text);
		}
		current_text = String();
		current_reasoning = String();
		pending_tool_calls.clear();
		streamed_tool_calls.clear();
		_finish_turn("completed", final_text);
		return;
	}
	if (!current_text.is_empty()) {
		emit_signal(SNAME("assistant_message"), current_text);
	}

	tool_queue = pending_tool_calls.duplicate();
	const uint64_t queued_msec = OS::get_singleton()->get_ticks_msec();
	for (int i = 0; i < tool_queue.size(); i++) {
		Dictionary call = tool_queue[i];
		call["queued_msec"] = (int64_t)queued_msec;
		const String model_name = call.get("name", String());
		const String canonical_name = call.get("canonical_name", model_name);
		const String requested_name = call.get("requested_name", model_name);
		const String arguments = call.get("arguments", "{}");
		Dictionary preflight_result;
		Dictionary parsed_args;
		if (canonical_name.is_empty()) {
			preflight_result = _tool_denied_result("TOOL_NOT_FOUND", vformat("Model requested an unknown Solers tool: %s.", requested_name));
		} else {
			Ref<JSON> json;
			json.instantiate();
			const Error parse_error = json->parse(arguments.is_empty() ? "{}" : arguments);
			const Variant parsed = parse_error == OK ? json->get_data() : Variant();
			if (parse_error != OK || parsed.get_type() != Variant::DICTIONARY) {
				preflight_result = _tool_denied_result("TOOL_ARGUMENT_INVALID", "Tool arguments must be a complete JSON object.");
			} else if (!tool_registry) {
				preflight_result = _tool_denied_result("AGENT_UNCONFIGURED", "Tool registry unavailable.");
			} else {
				parsed_args = parsed;
				SolersToolContext context;
				context.call_id = call.get("id", String());
				context.session_id = session_id;
				context.project_path = project_path;
				context.mentions = turn_mentions;
				context.authored_revision = authored_revision;
				Dictionary normalized_args;
				preflight_result = tool_registry->_preflight_tool_call(StringName(canonical_name), parsed_args, context, normalized_args);
			}
		}
		call["parsed_args"] = parsed_args;
		if (!preflight_result.is_empty()) {
			call["preflight_result"] = preflight_result;
		}
		tool_queue[i] = call;
	}
	int observation_count = 0;
	for (int i = 0; i < tool_queue.size(); i++) {
		const Dictionary call = tool_queue[i];
		if (!call.has("preflight_result") && tool_registry && tool_registry->is_read_only(StringName(call.get("canonical_name", String())), call.get("parsed_args", Dictionary()))) {
			observation_count++;
		}
	}
	if (observation_count > 0 && context_manager && context_window > 0) {
		_collect_tools();
		int remaining = MAX(0, context_window - max_output_tokens - context_manager->get_token_count_with_pending(messages, system_prompt, cached_request_tool_tokens));
		for (int i = 0; i < tool_queue.size(); i++) {
			Dictionary call = tool_queue[i];
			if (!call.has("preflight_result") && tool_registry->is_read_only(StringName(call.get("canonical_name", String())), call.get("parsed_args", Dictionary()))) {
				const int share = MAX(1, remaining / observation_count);
				call["result_token_budget"] = share;
				remaining = MAX(0, remaining - share);
				observation_count--;
				tool_queue[i] = call;
			}
		}
	}
	tool_queue_index = 0;
	tool_delivery_index = 0;
	completed_tool_results.clear();
	failed_resource_accesses.clear();
	tool_started_announced = false;
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	current_text = String();
	current_reasoning = String();
	phase = PHASE_TOOLS;
	SOLERS_TRACE("session.tools", vformat("entering tool queue (%d call(s))", tool_queue.size()));
}

void SolersAgentSession::_finish_turn(const String &p_outcome, const String &p_message, const Dictionary &p_error) {
	if (compaction_id > 0) {
		_write_transcript_compaction("failed", p_error);
		emit_signal(SNAME("timeline_entry_committed"), compaction_timeline_event_id, "context_compaction");
	}
	if (tool_thread_state) {
		tool_cancel_requested.set();
		_collect_tool_thread_result(true);
	}
	_cancel_undelivered_tools();
	// Steering that never reached a dispatch still belongs to the
	// conversation; the next turn's model request will carry it.
	_flush_pending_steering();
	String outcome = p_outcome;
	Dictionary error = p_error;
	Dictionary data;
	data["text"] = p_message;
	data["reasoning"] = current_reasoning;
	data["stop_reason"] = last_stop_reason;
	data["outcome"] = outcome;
	if (!last_usage.is_empty()) {
		data["usage"] = last_usage;
	}
	if (!error.is_empty()) {
		data["error"] = error;
	}
	if (turn_runtime_owned) {
		EditorInterface *editor_interface = EditorInterface::get_singleton();
		if (editor_interface && editor_interface->is_playing_scene()) {
			editor_interface->stop_playing_scene();
			data["runtime_stopped"] = true;
		}
	}

	Dictionary usage;
	usage["model_requests"] = model_request_index;
	usage["fresh_input_tokens"] = turn_fresh_input_tokens;
	usage["cache_read_tokens"] = turn_cache_read_tokens;
	usage["cache_write_tokens"] = turn_cache_write_tokens;
	usage["output_tokens"] = turn_output_tokens;
	usage["wire_body_bytes"] = turn_wire_body_bytes;
	data["turn_usage"] = usage;

	Dictionary transcript;
	transcript["outcome"] = outcome;
	transcript["message"] = p_message;
	transcript["stop_reason"] = last_stop_reason;
	{
		MutexLock lock(godot_log_mutex);
		transcript["godot_log_errors"] = godot_log_error_count;
		transcript["godot_log_warnings"] = godot_log_warning_count;
	}
	transcript["turn_usage"] = usage;
	if (!last_usage.is_empty()) {
		transcript["usage"] = last_usage;
	}
	if (!error.is_empty()) {
		transcript["error"] = error;
	}
	_write_transcript_event("turn_outcome", transcript);
	solers_transcript_flush(session_id);
	godot_log_turn_active = false;
	last_outcome = outcome;
	running = false;
	phase = PHASE_STREAMING;
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	tool_queue.clear();
	_clear_pending_tools();
	completed_tool_results.clear();
	failed_resource_accesses.clear();
	readonly_cache.clear();
	if (tool_registry) {
		tool_registry->clear_task_state(session_id);
	}
	task_deferred_tools.clear();
	tool_queue_index = 0;
	tool_delivery_index = 0;
	tool_started_announced = false;
	tool_queued_msec = 0;
	tool_started_msec = 0;
	tool_completed_msec = 0;
	deferred_queue_index = -1;
	awaiting_call.clear();
	awaiting_approval_id = 0;
	retry_attempt = 0;
	retry_resume_msec = 0;
	retry_request.clear();
	retry_profile.clear();
	compaction_source_messages.clear();
	compaction_id = 0;
	compaction_timeline_event_id = 0;
	overflow_compaction_attempts = 0;
	tool_exec_token++;
	tool_exec_requested = false;
	last_progress_call_id = String();
	last_progress_msec = 0;
	deferred_done = false;
	deferred_result.clear();
	deferred_args.clear();
	deferred_initial_args.clear();
	deferred_resource_accesses.clear();
	deferred_is_resume = false;
	deferred_polling = false;
	deferred_call_id = String();
	deferred_model_name = String();
	deferred_canonical_name = String();
	current_text = String();
	current_reasoning = String();
	pending_model_attachments.clear();
	turn_attachments.clear();
	turn_mentions.clear();
	waiting_background_asset_ids.clear();
	turn_runtime_owned = false;
	if (outcome == "aborted") {
		background_resume_suppressed = true;
	}
	if (outcome == "failed") {
		_record("agent_turn_failed", data);
		emit_signal(SNAME("turn_failed"), error);
	} else {
		_record("agent_turn_completed", data);
		emit_signal(SNAME("turn_completed"), data);
	}
}

void SolersAgentSession::_poll_tool_queue() {
	if (!_flush_tool_results() || !running) {
		return;
	}

	if (tool_queue_index >= tool_queue.size()) {
		if (_resume_ready_pending_tool()) {
			return;
		}
		if (!pending_tool_executions.is_empty()) {
			return;
		}
		if (tool_delivery_index < tool_queue.size()) {
			return;
		}
		observed_revision = authored_revision;
		tool_queue.clear();
		completed_tool_results.clear();
		tool_queue_index = 0;
		tool_delivery_index = 0;
		tool_started_announced = false;
		retry_attempt = 0;
		// job.wait recorded outstanding ids: park the host silently until a
		// project-import terminal delivery resumes this same turn. Without this
		// gate the model re-enters and busy-polls asset.status in the foreground.
		if (!waiting_background_asset_ids.is_empty()) {
			phase = PHASE_WAITING;
			Array pending_ids;
			for (const String &asset_id : waiting_background_asset_ids) {
				pending_ids.push_back(asset_id);
			}
			Dictionary parked;
			parked["pending_ids"] = pending_ids;
			_write_transcript_event("background_wait_parked", parked);
			emit_signal(SNAME("turn_waiting"), parked);
			return;
		}
		const Error err = _dispatch_model_request();
		if (err != OK) {
			current_reasoning = String();
		}
		return;
	}

	const int queue_index = tool_queue_index;
	const Dictionary call = tool_queue[queue_index];
	const String name = call.get("name", String());
	const String canonical_name = call.get("canonical_name", name);
	const String requested_name = call.get("requested_name", name);
	const String id = call.get("id", String());
	const String arguments = call.get("arguments", "{}");

	if (!tool_started_announced) {
		tool_started_announced = true;
		tool_started_msec = 0;
		if (!(bool)call.get("ui_announced", false)) {
			emit_signal(SNAME("tool_call_started"), id, canonical_name, arguments);
		}
	}

	auto write_rejected_start = [&](const String &p_tool) {
		tool_started_msec = OS::get_singleton()->get_ticks_msec();
		Dictionary audit;
		audit["call_id"] = id;
		audit["tool"] = p_tool;
		audit["arguments_sha256"] = arguments.sha256_text();
		audit["arguments_bytes"] = arguments.utf8().length();
		_write_transcript_event("tool_started", audit);
	};
	const Dictionary preflight_result = call.get("preflight_result", Dictionary());
	if (!preflight_result.is_empty()) {
		const String rejected_name = canonical_name.is_empty() ? requested_name : canonical_name;
		write_rejected_start(rejected_name);
		Dictionary args = call.get("parsed_args", Dictionary());
		if (args.is_empty() && !arguments.is_empty() && arguments != "{}") {
			args["arguments_sha256"] = arguments.sha256_text();
			args["arguments_bytes"] = arguments.utf8().length();
		}
		_queue_tool_result(queue_index, id, name, rejected_name, args, preflight_result, tool_started_msec);
		tool_queue_index++;
		tool_started_announced = false;
		return;
	}
	const Dictionary parsed_args = call.get("parsed_args", Dictionary());
	const Array accesses = tool_registry ? tool_registry->resolve_resource_access(StringName(canonical_name), parsed_args) : Array();
	if (_conflicts_with_pending(accesses)) {
		if (_resume_ready_pending_tool()) {
			return;
		}
		return;
	}
	tool_started_msec = OS::get_singleton()->get_ticks_msec();
	Dictionary audit;
	audit["call_id"] = id;
	audit["tool"] = canonical_name;
	audit["resume"] = false;
	audit["args"] = tool_registry ? tool_registry->redact_tool_args_for_audit(StringName(canonical_name), parsed_args) : parsed_args;
	_write_transcript_event("tool_started", audit);
	if (tool_registry && SolersToolRegistry::has_write_conflict(failed_resource_accesses, accesses)) {
		const Dictionary skipped = _tool_denied_result("SKIPPED_AFTER_FAILURE", "Skipped because a failed prerequisite touched the same resource. Re-evaluate that resource before retrying.");
		_queue_tool_result(queue_index, id, name, canonical_name, parsed_args, skipped, tool_started_msec);
		tool_queue_index++;
		tool_started_announced = false;
		return;
	}
	if (tool_registry && tool_registry->is_read_only(StringName(canonical_name), parsed_args)) {
		const String cache_key = _readonly_cache_key(StringName(canonical_name), parsed_args);
		const Dictionary *cached = readonly_cache.getptr(cache_key);
		if (cached) {
			Dictionary result;
			result["ok"] = cached->get("ok", false);
			Dictionary data;
			data["unchanged"] = true;
			data["session_revision"] = (int64_t)authored_revision;
			data["previous_result_sha256"] = JSON::stringify(*cached, "", false, true).sha256_text();
			result["data"] = data;
			_queue_tool_result(queue_index, id, name, canonical_name, parsed_args, result, tool_started_msec);
			tool_queue_index++;
			tool_started_announced = false;
			return;
		}
	}
	_schedule_tool_execution(queue_index, id, name, canonical_name, parsed_args, false);
	tool_queue_index++;
	tool_started_announced = false;
}

bool SolersAgentSession::_conflicts_with_pending(const Array &p_accesses) const {
	for (int i = 0; i < pending_tool_executions.size(); i++) {
		const Array &pending_accesses = pending_tool_executions[i]->resource_accesses;
		if (SolersToolRegistry::has_write_conflict(pending_accesses, p_accesses) || SolersToolRegistry::has_write_conflict(p_accesses, pending_accesses)) {
			return true;
		}
	}
	return false;
}

void SolersAgentSession::_park_pending_tool(const Dictionary &p_poll_args) {
	PendingToolExecution *pending = memnew(PendingToolExecution);
	pending->queue_index = deferred_queue_index;
	pending->call_id = deferred_call_id;
	pending->model_name = deferred_model_name;
	pending->canonical_name = deferred_canonical_name;
	pending->poll_args = p_poll_args.duplicate(true);
	pending->initial_args = deferred_initial_args.duplicate(true);
	pending->resource_accesses = deferred_resource_accesses.duplicate(true);
	pending->prepared_call = deferred_prepared_call;
	pending->is_resume = deferred_is_resume;
	pending->started_msec = tool_started_msec;
	pending_tool_executions.push_back(pending);
	_refresh_deferred_window_audit();

	deferred_prepared_call = nullptr;
	deferred_queue_index = -1;
	deferred_call_id = String();
	deferred_model_name = String();
	deferred_canonical_name = String();
	deferred_args.clear();
	deferred_initial_args.clear();
	deferred_resource_accesses.clear();
	deferred_result.clear();
	deferred_done = false;
	deferred_polling = false;
	deferred_is_resume = false;
	tool_exec_requested = false;
	phase = PHASE_TOOLS;
}

bool SolersAgentSession::_resume_ready_pending_tool() {
	if (!tool_registry) {
		return false;
	}
	for (int i = 0; i < pending_tool_executions.size(); i++) {
		PendingToolExecution *pending = pending_tool_executions[i];
		if (pending->prepared_call && !tool_registry->_is_prepared_tool_ready(*pending->prepared_call, pending->poll_args)) {
			continue;
		}
		pending_tool_executions.remove_at(i);
		_refresh_deferred_window_audit();
		deferred_queue_index = pending->queue_index;
		deferred_call_id = pending->call_id;
		deferred_model_name = pending->model_name;
		deferred_canonical_name = pending->canonical_name;
		deferred_args = pending->poll_args;
		deferred_initial_args = pending->initial_args;
		deferred_resource_accesses = pending->resource_accesses;
		deferred_prepared_call = pending->prepared_call;
		deferred_is_resume = pending->is_resume;
		tool_started_msec = pending->started_msec;
		deferred_polling = true;
		deferred_done = false;
		deferred_result.clear();
		tool_cancel_requested.clear();
		phase = PHASE_TOOL_EXECUTING;
		++tool_exec_token;
		tool_exec_requested = true;
		memdelete(pending);
		return true;
	}
	return false;
}

void SolersAgentSession::_queue_tool_result(int p_queue_index, const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, uint64_t p_started_msec) {
	const uint64_t completed_msec = OS::get_singleton()->get_ticks_msec();
	Dictionary terminal;
	terminal["id"] = p_id;
	terminal["model_name"] = p_model_name;
	terminal["canonical_name"] = p_canonical_name;
	terminal["args"] = p_args.duplicate(true);
	terminal["result"] = p_result.duplicate(true);
	int64_t queued_msec = completed_msec;
	if (p_queue_index >= 0 && p_queue_index < tool_queue.size()) {
		const Dictionary call = tool_queue[p_queue_index];
		queued_msec = (int64_t)call.get("queued_msec", queued_msec);
		terminal["result_token_budget"] = call.get("result_token_budget", INT32_MAX);
	}
	terminal["queued_msec"] = queued_msec;
	terminal["started_msec"] = (int64_t)(p_started_msec > 0 ? p_started_msec : completed_msec);
	terminal["completed_msec"] = (int64_t)completed_msec;
	completed_tool_results[p_queue_index] = terminal;
}

bool SolersAgentSession::_flush_tool_results() {
	while (running) {
		const Dictionary *terminal = completed_tool_results.getptr(tool_delivery_index);
		if (!terminal) {
			return true;
		}
		const Dictionary entry = *terminal;
		completed_tool_results.erase(tool_delivery_index);
		tool_queued_msec = (uint64_t)(int64_t)entry.get("queued_msec", 0);
		tool_started_msec = (uint64_t)(int64_t)entry.get("started_msec", 0);
		tool_completed_msec = (uint64_t)(int64_t)entry.get("completed_msec", tool_started_msec);
		const String canonical_name = entry.get("canonical_name", String());
		const Dictionary result = entry.get("result", Dictionary());
		_deliver_tool_result(entry.get("id", String()), entry.get("model_name", String()), canonical_name, entry.get("args", Dictionary()), result, entry.get("result_token_budget", INT32_MAX));
		tool_delivery_index++;
	}
	return false;
}

// A parked execution never reaches its own terminal result, so the registry is
// told once, here, and releases whatever per-task state it was holding.
void SolersAgentSession::_clear_pending_tools() {
	const Dictionary cancelled = _tool_denied_result("TOOL_CANCELLED", "The turn ended while this tool was still parked.");
	for (int i = 0; i < pending_tool_executions.size(); i++) {
		PendingToolExecution *pending = pending_tool_executions[i];
		if (pending->prepared_call) {
			if (tool_registry) {
				tool_registry->_complete_prepared_tool(*pending->prepared_call, cancelled);
			}
			memdelete(pending->prepared_call);
		}
		memdelete(pending);
	}
	pending_tool_executions.clear();
	_refresh_deferred_window_audit();
}

// Once a tool_call is in history it must be answered, whatever ended the turn.
// Terminal paths deliver the results the queue never got around to: the real
// completion when one already arrived, a cancellation otherwise.
void SolersAgentSession::_cancel_undelivered_tools() {
	if (tool_delivery_index >= tool_queue.size()) {
		return;
	}
	const Dictionary cancelled = _tool_denied_result("TOOL_CANCELLED", "The turn ended before this tool produced a result.");
	for (int i = tool_delivery_index; i < tool_queue.size(); i++) {
		const Dictionary call = tool_queue[i];
		const Dictionary *completed = completed_tool_results.getptr(i);
		const Dictionary entry = completed ? *completed : Dictionary();
		tool_queued_msec = (uint64_t)(int64_t)entry.get("queued_msec", (int64_t)tool_queued_msec);
		tool_started_msec = (uint64_t)(int64_t)entry.get("started_msec", (int64_t)tool_started_msec);
		tool_completed_msec = (uint64_t)(int64_t)entry.get("completed_msec", (int64_t)OS::get_singleton()->get_ticks_msec());
		const String model_name = call.get("name", String());
		_deliver_tool_result(
				call.get("id", String()),
				model_name,
				call.get("canonical_name", model_name),
				call.get("parsed_args", Dictionary()),
				entry.has("result") ? Dictionary(entry.get("result", cancelled)) : cancelled,
				call.get("result_token_budget", INT32_MAX));
	}
	tool_delivery_index = tool_queue.size();
}

void SolersAgentSession::_schedule_tool_execution(int p_queue_index, const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, bool p_is_resume) {
	if (tool_thread_state) {
		tool_cancel_requested.set();
		_collect_tool_thread_result(true);
	}
	tool_cancel_requested.clear();
	if (deferred_prepared_call) {
		memdelete(deferred_prepared_call);
		deferred_prepared_call = nullptr;
	}
	deferred_call_id = p_id;
	deferred_queue_index = p_queue_index;
	deferred_model_name = p_model_name;
	deferred_canonical_name = p_canonical_name;
	deferred_args = p_args.duplicate(true);
	deferred_initial_args = deferred_args;
	deferred_resource_accesses = tool_registry ? tool_registry->resolve_resource_access(StringName(p_canonical_name), p_args) : Array();
	if (p_canonical_name == "asset.generate") {
		Array ids = deferred_args.get("input_attachments", Array());
		if (!ids.is_empty()) {
			deferred_args["_attachments"] = _attachments_for_ids(ids);
		}
	}
	deferred_result = Dictionary();
	deferred_done = false;
	deferred_polling = false;
	deferred_is_resume = p_is_resume;
	phase = PHASE_TOOL_EXECUTING;
	const uint64_t token = ++tool_exec_token;
	tool_exec_requested = true;
	SOLERS_TRACE("session.schedule_tool", vformat("%s resume=%d token=%d", p_canonical_name, (int)p_is_resume, (int)token));
}

void SolersAgentSession::_execute_deferred_tool(uint64_t p_token) {
	if (!running || phase != PHASE_TOOL_EXECUTING || p_token != tool_exec_token) {
		SOLERS_TRACE("session.exec_tool", vformat("stale deferred call dropped (token=%d cur=%d running=%d)", (int)p_token, (int)tool_exec_token, (int)running));
		return;
	}
	if (!tool_registry) {
		deferred_result = _error("AGENT_UNCONFIGURED", "Tool registry unavailable.").get("error", Dictionary());
		Dictionary wrap;
		wrap["ok"] = false;
		wrap["error"] = deferred_result;
		deferred_result = wrap;
		deferred_done = true;
		return;
	}
	if (deferred_canonical_name == "render.capture") {
		if (_active_model_input_support("image") == 0) {
			deferred_result = _tool_denied_result("VISION_CAPABILITY_REQUIRED", "The selected model does not support image input, so viewport captures cannot be returned to it.");
			deferred_done = true;
			return;
		}
	}
	SOLERS_TRACE("session.exec_tool", vformat("BEGIN %s", deferred_canonical_name));
	SolersToolContext context;
	context.call_id = deferred_call_id;
	context.session_id = session_id;
	context.project_path = project_path;
	context.mentions = turn_mentions;
	context.authored_revision = authored_revision;
	if (deferred_queue_index >= 0 && deferred_queue_index < tool_queue.size() && Dictionary(tool_queue[deferred_queue_index]).has("result_token_budget")) {
		context.result_token_budget = Dictionary(tool_queue[deferred_queue_index]).get("result_token_budget", 1);
	} else if (context_manager && context_window > 0) {
		context.result_token_budget = MAX(1, context_window - max_output_tokens - context_manager->get_token_count_with_pending(messages, system_prompt, cached_request_tool_tokens));
	}
	context.cancel_requested = &tool_cancel_requested;
	if (!deferred_prepared_call) {
		SolersPreparedToolCall prepared;
		const Dictionary preparation_error = tool_registry->_prepare_tool_call(StringName(deferred_canonical_name), deferred_args, context, prepared);
		if (!preparation_error.is_empty()) {
			deferred_result = preparation_error;
			deferred_done = true;
			return;
		}
		deferred_prepared_call = memnew(SolersPreparedToolCall(prepared));
	}
	if (deferred_prepared_call->execution == SolersToolExecution::WORKER_THREAD) {
		ToolThreadState *state = memnew(ToolThreadState);
		state->registry = tool_registry;
		state->session = this;
		state->call = *deferred_prepared_call;
		state->poll_args = deferred_args;
		state->polling = deferred_polling;
		state->token = p_token;
		tool_thread_state = state;
		tool_thread.start(&SolersAgentSession::_tool_thread_func, state);
		if (!tool_thread.is_started()) {
			_tool_thread_func(state);
		}
		return;
	}
	_begin_main_thread_tool_audit();
	deferred_result = deferred_polling ? tool_registry->_poll_prepared_tool(*deferred_prepared_call, deferred_args) : tool_registry->_execute_prepared_tool(*deferred_prepared_call);
	_end_main_thread_tool_audit();
	deferred_done = true;
	SOLERS_TRACE("session.exec_tool", vformat("END %s ok=%d", deferred_canonical_name, (int)(bool)deferred_result.get("ok", false)));
}

void SolersAgentSession::_tool_thread_func(void *p_userdata) {
	ToolThreadState *state = static_cast<ToolThreadState *>(p_userdata);
	state->worker_thread_id = Thread::get_caller_id();
	if (state->session) {
		state->session->_register_worker_tool_audit(state->worker_thread_id, state->call.context.call_id, state->call.name, state->session->deferred_resource_accesses);
	}
	state->result = state->polling ? state->registry->_poll_prepared_tool(state->call, state->poll_args) : state->registry->_execute_prepared_tool(state->call);
	state->done.set();
}

bool SolersAgentSession::_collect_tool_thread_result(bool p_wait) {
	ToolThreadState *state = tool_thread_state;
	if (!state || (!p_wait && !state->done.is_set())) {
		return false;
	}
	if (tool_thread.is_started()) {
		tool_thread.wait_to_finish();
	}
	if (CallQueue *queue = MessageQueue::get_main_singleton(); queue && queue->flush() == ERR_BUSY && !p_wait) {
		return false;
	}
	if (state->token == tool_exec_token && running && phase == PHASE_TOOL_EXECUTING) {
		deferred_result = state->result;
		if (deferred_prepared_call) {
			deferred_prepared_call->journal_event = state->call.journal_event;
		}
		deferred_done = true;
		SOLERS_TRACE("session.exec_tool", vformat("END %s ok=%d", deferred_canonical_name, (int)(bool)deferred_result.get("ok", false)));
	}
	{
		MutexLock lock(godot_log_mutex);
		worker_tool_audits.erase(state->worker_thread_id);
	}
	memdelete(state);
	tool_thread_state = nullptr;
	return true;
}

void SolersAgentSession::_poll_tool_executing() {
	if (tool_exec_requested) {
		if (deferred_polling && deferred_prepared_call) {
			_begin_main_thread_tool_audit();
			const bool ready = tool_registry->_is_prepared_tool_ready(*deferred_prepared_call, deferred_args);
			_end_main_thread_tool_audit();
			if (!ready) {
				return;
			}
		}
		tool_exec_requested = false;
		_execute_deferred_tool(tool_exec_token);
	}
	_collect_tool_thread_result(false);
	if (!deferred_done) {
		return;
	}

	Dictionary result = deferred_result;
	const String id = deferred_call_id;
	const String model_name = deferred_model_name;
	const String canonical_name = deferred_canonical_name;
	const Dictionary args = deferred_initial_args;
	const bool is_resume = deferred_is_resume;

	deferred_done = false;
	deferred_result = Dictionary();

	if (!is_resume && permission_manager && _is_awaiting_approval_result(result)) {
		const Dictionary error = result.get("error", Dictionary());
		awaiting_call = Dictionary();
		awaiting_call["id"] = id;
		awaiting_call["name"] = model_name;
		awaiting_call["canonical_name"] = canonical_name;
		awaiting_call["parsed_args"] = args;
		awaiting_call["queue_index"] = deferred_queue_index;
		awaiting_call["started_msec"] = (int64_t)tool_started_msec;
		awaiting_approval_id = (int)error.get("approval_id", 0);
		phase = PHASE_AWAITING_APPROVAL;
		SOLERS_TRACE("session.await_approval", vformat("%s approval_id=%d", canonical_name, awaiting_approval_id));
		emit_signal(SNAME("tool_call_awaiting_approval"), id, canonical_name);
		return;
	}

	if (is_resume && _is_awaiting_approval_result(result)) {
		result = _tool_denied_result("APPROVAL_EXPIRED", "The approval expired before the tool could run. Ask again to retry.");
	}

	const Dictionary pending_data = result.get("data", Dictionary());
	const Variant poll_args = pending_data.get("poll_args", Variant());
	if ((bool)result.get("ok", false) && String(pending_data.get("status", String())) == "pending" && poll_args.get_type() == Variant::DICTIONARY) {
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		// Pending tools are re-polled several times per second; log progress at
		// most once per second per call so the transcript stays readable.
		if (last_progress_call_id != id || now - last_progress_msec >= 1000) {
			last_progress_call_id = id;
			last_progress_msec = now;
			Dictionary progress;
			progress["call_id"] = id;
			progress["tool"] = canonical_name;
			progress["status"] = "pending";
			progress["data"] = pending_data;
			_write_transcript_event("tool_progress", progress);
			emit_signal(SNAME("tool_call_updated"), id, canonical_name, JSON::stringify(pending_data, "", false, true));
		}
		_park_pending_tool(Dictionary(poll_args));
		return;
	}
	// The engine's own error stream is authoritative for whether this call's
	// native work actually succeeded. Evidence is consumed only when the call
	// settles so awaited continuations keep accumulating into the same call.
	const Dictionary godot_error = _consume_attributable_tool_error(id);
	if (!godot_error.is_empty()) {
		Dictionary evidence_data = result.get("data", Dictionary());
		evidence_data["godot_errors"] = godot_error.get("events", Array());
		result["data"] = evidence_data;
		// A FILE_CHECKPOINT mutation has already committed its write when the
		// handler reports success; content diagnostics (parser errors from the
		// reloaded script) are part of that result's own contract, so engine
		// events stay attached as evidence without contradicting the commit.
		// Reporting the write as failed would desynchronize the model from the
		// real file state and poison every follow-up edit.
		const bool checkpointed_commit = deferred_prepared_call && deferred_prepared_call->mutation_policy == SolersToolMutationPolicy::FILE_CHECKPOINT;
		const bool target_state_confirmed = (bool)Dictionary(result.get("data", Dictionary())).get("target_state_confirmed", false);
		if ((bool)result.get("ok", false) && (bool)godot_error.get("handler_window", false) && !checkpointed_commit && !target_state_confirmed) {
			result["ok"] = false;
			result["error"] = godot_error;
		}
	}
	const bool tool_succeeded = (bool)result.get("ok", false);
	Dictionary data = result.get("data", Dictionary());
	if (tool_succeeded) {
		if ((bool)data.get("authored_state_changed", false)) {
			authored_revision++;
			const Dictionary mutation = data.get("mutation", Dictionary());
			const Dictionary receipt = mutation.get("receipt", Dictionary());
			latest_mutation_receipt = receipt.duplicate(true);
			if (receipt.has("scene_after")) {
				render_artifacts.clear();
			}
		}
		const Dictionary artifact = data.get("artifact", Dictionary());
		const String artifact_kind = artifact.get("kind", String());
		if (!artifact_kind.is_empty()) {
			render_artifacts[artifact_kind] = artifact;
		}
		if (canonical_name == "object.query") {
			observed_revision = authored_revision;
		} else if (canonical_name == "render.capture") {
			observed_revision = authored_revision;
		}
	}
	if (deferred_prepared_call) {
		tool_registry->_complete_prepared_tool(*deferred_prepared_call, result);
		_write_prepared_journal_event(deferred_prepared_call);
		memdelete(deferred_prepared_call);
		deferred_prepared_call = nullptr;
	}
	deferred_polling = false;
	// The read cache exists to dedupe repeated identical reads at one state
	// authored revision. Two result shapes are excluded because they are not pure
	// functions of that revision: visual evidence (attachments; live runtime
	// frames and the user-driven editor camera change without a revision
	// bump), and in-flight pending envelopes (their capture_id is consumed by
	// the poll loop).
	const bool cacheable = canonical_name != "render.capture" && !result.has("attachments") && !Dictionary(result.get("data", Dictionary())).has("poll_args");
	if (tool_succeeded && cacheable && tool_registry && tool_registry->is_read_only(StringName(canonical_name), args)) {
		readonly_cache[_readonly_cache_key(StringName(canonical_name), args)] = result.duplicate(true);
	}

	const int queue_index = deferred_queue_index;
	const uint64_t started_msec = tool_started_msec;
	deferred_queue_index = -1;
	phase = PHASE_TOOLS;
	_queue_tool_result(queue_index, id, model_name, canonical_name, args, result, started_msec);
}

bool SolersAgentSession::_is_awaiting_approval_result(const Dictionary &p_result) const {
	if ((bool)p_result.get("ok", false)) {
		return false;
	}
	const Dictionary error = p_result.get("error", Dictionary());
	return String(error.get("code", String())) == "USER_APPROVAL_REQUIRED";
}

String SolersAgentSession::deliverable_tool_result(const String &p_call_id, const Dictionary &p_result, int p_budget) {
	const String full_result = JSON::stringify(p_result, "", false, true);
	const int tokens = SolersContextManager::estimate_tokens(full_result);
	if (tokens <= p_budget) {
		return full_result;
	}
	// Every mutating tool exposes the same receipt contract. Large observation
	// bodies are audit data and can be requested again with narrower arguments.
	Dictionary envelope;
	envelope["ok"] = p_result.get("ok", false);
	if (p_result.has("error")) {
		envelope["error"] = p_result["error"];
	}
	const Dictionary data = p_result.get("data", Dictionary());
	if (data.has("mutation")) {
		Dictionary kept_data;
		kept_data["mutation"] = data["mutation"];
		envelope["data"] = kept_data;
	}
	Dictionary elided;
	elided["tokens"] = tokens;
	elided["token_budget"] = p_budget;
	elided["result_sha256"] = full_result.sha256_text();
	elided["recovery"] = "Re-run with narrower arguments for details; mutation receipts remain above.";
	envelope["data_elided"] = elided;
	return JSON::stringify(envelope, "", false, true);
}

Dictionary SolersAgentSession::_tool_denied_result(const String &p_code, const String &p_message) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = true;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

void SolersAgentSession::_deliver_tool_result(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, int p_budget) {
	Dictionary result = p_result.duplicate(true);
	result["call_id"] = p_id;
	// A result widens the tool surface by declaring what it unlocks, so this
	// path never has to recognize which tool produced it.
	const PackedStringArray unlocked = Dictionary(result.get("data", Dictionary())).get("unlock_tools", PackedStringArray());
	for (int i = 0; i < unlocked.size(); i++) {
		task_deferred_tools.insert(StringName(unlocked[i]));
	}
	const Dictionary diagnostics = _take_godot_diagnostics();
	if (!diagnostics.is_empty()) {
		result["diagnostics"] = diagnostics;
	}
	// The transcript records exactly the bytes the model was given, so a
	// restored session rebuilds the same conversation it originally had.
	int result_budget = p_budget;
	if (context_manager && context_window > 0) {
		_collect_tools();
		const int used = context_manager->get_token_count_with_pending(messages, system_prompt, cached_request_tool_tokens);
		result_budget = MIN(result_budget, MAX(0, context_window - max_output_tokens - used));
	}
	const String content = deliverable_tool_result(p_id, result, result_budget);
	_write_transcript_tool(p_id, p_canonical_name, p_args, result, content);
	const uint64_t duration_msec = tool_completed_msec >= tool_started_msec ? tool_completed_msec - tool_started_msec : 0;
	if (!_is_session_tool(p_canonical_name)) {
		emit_signal(SNAME("tool_call_finished"), p_id, p_canonical_name, result, (int64_t)duration_msec);
	}

	// Runtime ownership follows the authoritative post-state so turn cleanup
	// stops only the game instance started by this session.
	const Dictionary data = result.get("data", Dictionary());
	if (p_canonical_name == "job.wait" && (bool)result.get("ok", false)) {
		waiting_background_asset_ids.clear();
		const Array pending_ids = data.get("pending_ids", Array());
		for (int i = 0; i < pending_ids.size(); i++) {
			waiting_background_asset_ids.insert(String(pending_ids[i]));
		}
		const Array terminal = data.get("terminal", Array());
		for (int terminal_index = 0; terminal_index < terminal.size(); terminal_index++) {
			const String asset_id = String(Dictionary(terminal[terminal_index]).get("id", String()));
			if (asset_id.is_empty()) {
				continue;
			}
			delivered_background_assets.insert(asset_id);
			if (tool_registry && tool_registry->asset_service) {
				tool_registry->asset_service->mark_terminal_delivered(asset_id, session_id);
			}
			for (int pending_index = pending_background_assets.size() - 1; pending_index >= 0; pending_index--) {
				if (String(Dictionary(pending_background_assets[pending_index]).get("asset_id", String())) == asset_id) {
					pending_background_assets.remove_at(pending_index);
				}
			}
			Dictionary consumed;
			consumed["asset_id"] = asset_id;
			_write_transcript_event("background_asset_consumed", consumed);
		}
	}
	if ((bool)data.get("started_by_call", false)) {
		turn_runtime_owned = true;
	}
	if ((bool)data.get("stopped_by_call", false)) {
		turn_runtime_owned = false;
	}

	Dictionary message = SolersLLMMessage::tool_result(p_id, p_model_name, content, result.get("attachments", Array()));
	messages.push_back(message);

	if (!(bool)result.get("ok", false)) {
		const Dictionary error = result.get("error", Dictionary());
		if (String(error.get("code", String())) != "SKIPPED_AFTER_FAILURE" && tool_registry) {
			const Array accesses = tool_registry->resolve_resource_access(StringName(p_canonical_name), p_args);
			for (int i = 0; i < accesses.size(); i++) {
				// Only failed writes fence later tools: a failed read (for
				// example a capture that timed out) leaves no partial state,
				// and its wildcard read key must not skip the whole queue.
				if (String(Dictionary(accesses[i]).get("mode", "write")) == "write") {
					failed_resource_accesses.push_back(accesses[i]);
				}
			}
		}
	}
}

void SolersAgentSession::_poll_awaiting_approval() {
	const String id = awaiting_call.get("id", String());
	const String model_name = awaiting_call.get("name", String());
	const String canonical_name = awaiting_call.get("canonical_name", model_name);
	const int queue_index = awaiting_call.get("queue_index", deferred_queue_index);
	const uint64_t started_msec = (uint64_t)(int64_t)awaiting_call.get("started_msec", tool_started_msec);

	const SolersPermissionManager::RequestDecision decision =
			permission_manager ? permission_manager->get_request_decision(awaiting_approval_id)
							   : SolersPermissionManager::DECISION_UNKNOWN;

	if (permission_manager && decision == SolersPermissionManager::DECISION_PENDING) {
		return; // still waiting on the user
	}

	if (permission_manager && decision == SolersPermissionManager::DECISION_APPROVED) {
		// Resume by re-issuing the same call through the scheduler so it runs
		// from the top of the next poll tick, never nested inside this phase
		// handler (the original crash path executed the approved tool
		// synchronously right here).
		Dictionary args = awaiting_call.get("parsed_args", Dictionary());
		args["approval_id"] = awaiting_approval_id;
		const int approval_id = awaiting_approval_id;
		awaiting_call.clear();
		awaiting_approval_id = 0;
		SOLERS_TRACE("session.approval_granted", vformat("%s approval_id=%d -> deferred execute", canonical_name, approval_id));
		_schedule_tool_execution(queue_index, id, model_name, canonical_name, args, true);
		return;
	}

	Dictionary result;
	if (!permission_manager) {
		result = _tool_denied_result("APPROVAL_UNAVAILABLE", "No permission manager is available to resolve the approval.");
	} else if (decision == SolersPermissionManager::DECISION_REJECTED) {
		result = _tool_denied_result("USER_REJECTED", "The user denied this tool call.");
	} else {
		result = _tool_denied_result("APPROVAL_EXPIRED", "The approval request is no longer available.");
	}

	const Dictionary args = awaiting_call.get("parsed_args", Dictionary());
	awaiting_call.clear();
	awaiting_approval_id = 0;
	deferred_queue_index = -1;
	phase = PHASE_TOOLS;
	_queue_tool_result(queue_index, id, model_name, canonical_name, args, result, started_msec);
}

void SolersAgentSession::abort() {
	if (!running) {
		return;
	}
	if (client) {
		client->abort();
	}
	if (tool_thread_state) {
		tool_cancel_requested.set();
		_collect_tool_thread_result(true);
	}
	if (deferred_prepared_call) {
		if (tool_registry) {
			const Dictionary cancelled = _tool_denied_result("TOOL_CANCELLED", "The turn was aborted while the tool was running.");
			tool_registry->_complete_prepared_tool(*deferred_prepared_call, cancelled);
		}
		_write_prepared_journal_event(deferred_prepared_call);
		memdelete(deferred_prepared_call);
		deferred_prepared_call = nullptr;
	}
	_finish_turn("aborted", "Turn aborted.");
}

void SolersAgentSession::shutdown() {
	abort();
	_release_godot_log_audit();
}

void SolersAgentSession::_reset_session_derived_state() {
	runtime_observation_cursor = 0;
	render_artifacts.clear();
	latest_mutation_receipt.clear();
	MutexLock lock(godot_log_mutex);
	pending_godot_diagnostics.clear();
	pending_godot_diagnostic_index.clear();
	pending_godot_diagnostics_overflow = 0;
}

void SolersAgentSession::reset_conversation() {
	abort();
	_release_godot_log_audit();
	_reset_session_derived_state();
	messages.clear();
	timeline_entries.clear();
	open_timeline_tools.clear();
	transcript_event_sequence = 0;
	delivered_model_attachments.clear();
	pending_model_attachments.clear();
	pending_steering_messages.clear();
	current_plan.clear();
	last_outcome = String();
	last_stop_reason = String();
	last_usage.clear();
	task_deferred_tools.clear();
	pending_background_assets.clear();
	delivered_background_assets.clear();
	waiting_background_asset_ids.clear();
	background_resume_suppressed = false;
	if (context_manager) {
		context_manager->reset();
	}
	if (tool_registry) {
		tool_registry->restore_session_reversal(session_id, Dictionary());
	}
	session_id = _make_session_id();
	authored_revision = 0;
	observed_revision = 0;
	if (!project_path.is_empty()) {
		_ensure_godot_log_audit(false);
	}
}

void SolersAgentSession::set_project_path(const String &p_project_path) {
	project_path = p_project_path;
	if (!project_path.is_empty() && !godot_log_audit_installed) {
		_ensure_godot_log_audit(false);
	}
}

void SolersAgentSession::set_session(const String &p_project_path, const String &p_session_id) {
	abort();
	_release_godot_log_audit();
	const String previous_session_id = session_id;
	project_path = p_project_path;
	_reset_session_derived_state();
	task_deferred_tools.clear();
	if (!p_session_id.is_empty()) {
		session_id = p_session_id;
	}
	if (tool_registry && previous_session_id != session_id) {
		tool_registry->restore_session_reversal(previous_session_id, Dictionary());
	}
	transcript_event_sequence = 0;
	const Dictionary state = _read_transcript_state(project_path, session_id);
	messages = state.get("messages", Array());
	timeline_entries = state.get("timeline_entries", Array());
	open_timeline_tools.clear();
	current_plan = state.get("plan", Dictionary());
	last_outcome = state.get("outcome", String());
	turn_id = state.get("turn_id", 0);
	authored_revision = (int64_t)state.get("session_revision", 0);
	observed_revision = (int64_t)state.get("observed_revision", 0);
	delivered_model_attachments.clear();
	pending_model_attachments.clear();
	const Array delivered_attachments = state.get("delivered_model_attachments", Array());
	for (int i = 0; i < delivered_attachments.size(); i++) {
		delivered_model_attachments.insert(delivered_attachments[i]);
	}
	if (tool_registry) {
		tool_registry->restore_session_reversal(session_id, state.get("reversal", Dictionary()));
	}
	pending_background_assets = state.get("background_assets", Array());
	background_resume_suppressed = false;
	delivered_background_assets.clear();
	waiting_background_asset_ids.clear();
	for (int i = 0; i < pending_background_assets.size(); i++) {
		delivered_background_assets.insert(Dictionary(pending_background_assets[i]).get("asset_id", String()));
	}
	if (context_manager) {
		context_manager->reset();
	}
	if (!project_path.is_empty()) {
		_ensure_godot_log_audit(false);
	}
}

bool SolersAgentSession::enqueue_background_asset(const Dictionary &p_manifest) {
	const String asset_id = String(p_manifest.get("id", String())).strip_edges();
	if (asset_id.is_empty() || delivered_background_assets.has(asset_id)) {
		return false;
	}
	const String asset_session_id = String(p_manifest.get("session_id", String())).strip_edges();
	if (asset_session_id.is_empty() || asset_session_id != session_id) {
		return false;
	}
	Dictionary delivery;
	delivery["asset_id"] = asset_id;
	delivery["status"] = p_manifest.get("status", String());
	delivery["provider"] = p_manifest.get("provider", String());
	const char *fields[] = { "kind", "name", "stage", "entrypoints", "dimensions", "preview_file", "error", "traits", "animations", "source_provider", "source_asset_id", "source_variant", "source_version", "license", "attribution", "parent_asset_id", "operation_id", "generation_mode", "provider_endpoint", "provider_request", "input_attachment_ids" };
	for (const char *field : fields) {
		if (p_manifest.has(field)) {
			delivery[field] = p_manifest[field];
		}
	}
	if (p_manifest.has("map_files")) {
		Array map_types = Dictionary(p_manifest["map_files"]).keys();
		map_types.sort();
		delivery["map_types"] = map_types;
	}
	_write_transcript_event("background_asset_delivery", delivery);
	delivered_background_assets.insert(asset_id);
	pending_background_assets.push_back(delivery);
	return true;
}

void SolersAgentSession::resume_background_assets() {
	_resume_next_background_asset();
}

bool SolersAgentSession::_append_background_asset_deltas(bool p_waited_only) {
	if (pending_background_assets.is_empty()) {
		return false;
	}
	Array deliveries;
	Array remaining;
	for (int i = 0; i < pending_background_assets.size(); i++) {
		const Dictionary delivery = pending_background_assets[i];
		const String asset_id = delivery.get("asset_id", String());
		if (p_waited_only && !waiting_background_asset_ids.has(asset_id)) {
			remaining.push_back(delivery);
			continue;
		}
		deliveries.push_back(delivery);
		waiting_background_asset_ids.erase(asset_id);
		Dictionary consumed;
		consumed["asset_id"] = asset_id;
		_write_transcript_event("background_asset_consumed", consumed);
	}
	if (deliveries.is_empty()) {
		return false;
	}
	pending_background_assets = remaining;
	Dictionary message;
	message["role"] = SolersContextManager::MODEL_CONTEXT_ROLE;
	message["content"] = "Background job terminal delta. Continue the original task using these persisted facts; call asset.status only if you need more detail from a job that has already reached a project-import terminal state:\n" + JSON::stringify(deliveries, "", false, true);
	message["origin"] = "background_job_delta";
	message["ephemeral"] = true;
	messages.push_back(message);
	_write_transcript_event("model_context", message);
	return true;
}

void SolersAgentSession::_resume_next_background_asset() {
	if (!is_waiting_for_background_assets() || background_resume_suppressed || !_append_background_asset_deltas(true)) {
		return;
	}
	_dispatch_model_request();
}

Array SolersAgentSession::get_messages() const {
	// UI restore only reads entries; a deep copy of tool payloads was free
	// cost on every session switch. Array is COW, so callers that mutate the
	// returned handle do not write through into session state.
	return messages;
}

Dictionary SolersAgentSession::get_status() const {
	Dictionary status;
	status["running"] = running;
	status["turn_id"] = turn_id;
	status["message_count"] = messages.size();
	status["provider"] = active_provider.get("provider", String());
	status["model"] = active_provider.get("model", String());
	status["model_requests"] = model_request_index;
	status["fresh_input_tokens"] = turn_fresh_input_tokens;
	status["cache_read_tokens"] = turn_cache_read_tokens;
	status["cache_write_tokens"] = turn_cache_write_tokens;
	status["output_tokens"] = turn_output_tokens;
	status["wire_body_bytes"] = turn_wire_body_bytes;
	status["context_window"] = context_window;
	status["max_output_tokens"] = max_output_tokens;
	status["model_limits_known"] = context_window > 0;
	status["project_path"] = project_path;
	status["session_id"] = session_id;
	status["compacting"] = phase == PHASE_COMPACTING;
	status["waiting_for_background"] = is_waiting_for_background_assets();
	status["waiting_background_asset_count"] = waiting_background_asset_ids.size();
	status["plan"] = current_plan.duplicate(true);
	status["last_outcome"] = last_outcome;
	status["pending_background_asset_count"] = pending_background_assets.size();
	{
		MutexLock lock(godot_log_mutex);
		status["godot_log_errors"] = godot_log_error_count;
		status["godot_log_warnings"] = godot_log_warning_count;
	}
	status["session_revision"] = (int64_t)authored_revision;
	status["runtime_epoch"] = tool_registry && tool_registry->observation_service ? tool_registry->observation_service->get_runtime_status().get("runtime_epoch", 0) : Variant((int64_t)0);
	status["observed_revision"] = (int64_t)observed_revision;
	if (context_manager) {
		status["context_tokens"] = context_manager->get_last_estimated_tokens();
		status["compaction_count"] = context_manager->get_compaction_count();
	}
	return status;
}

bool SolersAgentSession::is_admitting_tool_calls() const {
	if (!running) {
		return false;
	}
	if (phase == PHASE_TOOL_EXECUTING) {
		return !deferred_polling;
	}
	if (phase != PHASE_TOOLS || tool_queue_index >= tool_queue.size()) {
		return false;
	}
	const Dictionary call = tool_queue[tool_queue_index];
	const String canonical_name = call.get("canonical_name", call.get("name", String()));
	Ref<JSON> json;
	json.instantiate();
	const String arguments = call.get("arguments", "{}");
	if (canonical_name.is_empty() || json->parse(arguments.is_empty() ? "{}" : arguments) != OK || json->get_data().get_type() != Variant::DICTIONARY) {
		return true;
	}
	const Dictionary args = json->get_data();
	const Array accesses = tool_registry ? tool_registry->resolve_resource_access(StringName(canonical_name), args) : Array();
	return !_conflicts_with_pending(accesses);
}

SolersAgentSession::SolersAgentSession() {
	session_id = _make_session_id();
	protocol_registry = memnew(SolersLLMProtocolRegistry);
	protocol_registry->register_builtin_protocols();
	client = memnew(SolersLLMClient);
	client->set_protocol_registry(protocol_registry);
	context_manager = memnew(SolersContextManager);
	models_dev = memnew(SolersModelsDev);
	models_dev->initialize();
	owns_models_dev = true;
}

SolersAgentSession::~SolersAgentSession() {
	shutdown();
	if (tool_thread_state) {
		tool_cancel_requested.set();
		_collect_tool_thread_result(true);
	}
	if (deferred_prepared_call) {
		memdelete(deferred_prepared_call);
		deferred_prepared_call = nullptr;
	}
	if (context_manager) {
		memdelete(context_manager);
		context_manager = nullptr;
	}
	if (client) {
		memdelete(client);
		client = nullptr;
	}
	if (owns_models_dev && models_dev) {
		memdelete(models_dev);
		models_dev = nullptr;
	} else {
		models_dev = nullptr;
	}
	if (protocol_registry) {
		memdelete(protocol_registry);
		protocol_registry = nullptr;
	}
}
