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

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/config/project_settings.h"
#include "core/object/message_queue.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/editor_interface.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_retry.h"
#include "modules/solers_ai/llm/solers_models_dev.h"
#include "scene/main/node.h"

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
	uint64_t scene_digest_before = 0;
	uint64_t geometry_digest_before = 0;
	SolersPreparedToolCall *prepared_call = nullptr;
	bool is_resume = false;
	uint64_t started_msec = 0;
};

static bool _is_session_tool(const String &p_name) {
	return p_name == "update_plan";
}

// Shared by terminal auto-commit and env context — same conflict suffix rules.
static String _solers_pending_scene_path(const Node *p_root) {
	ERR_FAIL_NULL_V(p_root, "res://scene.tscn");
	String base = String(p_root->get_name()).validate_filename().to_lower();
	if (base.is_empty()) {
		base = "scene";
	}
	String save_path = "res://" + base + ".tscn";
	for (int i = 2; i < 100 && FileAccess::exists(save_path); i++) {
		save_path = vformat("res://%s_%d.tscn", base, i);
	}
	return save_path;
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
	ADD_SIGNAL(MethodInfo("compaction_started"));
	ADD_SIGNAL(MethodInfo("compaction_completed", PropertyInfo(Variant::DICTIONARY, "result")));
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

Dictionary SolersAgentSession::_read_transcript_state(const String &p_project_path, const String &p_session_id) const {
	Array restored;
	Array restored_background_assets;
	Dictionary restored_plan;
	String restored_outcome;
	int restored_turn_id = 0;
	uint64_t restored_authored_revision = 0;
	uint64_t restored_runtime_epoch = 0;
	uint64_t restored_observed_revision = 0;
	Dictionary restored_reversal;
	if (p_project_path.is_empty() || p_session_id.is_empty()) {
		Dictionary empty;
		empty["messages"] = restored;
		return empty;
	}

	struct ScanState {
		const String *project_path = nullptr;
		const String *session_id = nullptr;
		Array *restored = nullptr;
		Array *restored_background_assets = nullptr;
		Dictionary *restored_plan = nullptr;
		String *restored_outcome = nullptr;
		int *restored_turn_id = nullptr;
		uint64_t *restored_authored_revision = nullptr;
		uint64_t *restored_runtime_epoch = nullptr;
		uint64_t *restored_observed_revision = nullptr;
		Dictionary *restored_reversal = nullptr;
	} scan;
	scan.project_path = &p_project_path;
	scan.session_id = &p_session_id;
	scan.restored = &restored;
	scan.restored_background_assets = &restored_background_assets;
	scan.restored_plan = &restored_plan;
	scan.restored_outcome = &restored_outcome;
	scan.restored_turn_id = &restored_turn_id;
	scan.restored_authored_revision = &restored_authored_revision;
	scan.restored_runtime_epoch = &restored_runtime_epoch;
	scan.restored_observed_revision = &restored_observed_revision;
	scan.restored_reversal = &restored_reversal;

	solers_transcript_foreach_line(&scan, [](void *p_userdata, const String &p_record) -> bool {
		ScanState &scan = *static_cast<ScanState *>(p_userdata);
		const String line = p_record.strip_edges();
		if (line.is_empty() || !solers_transcript_line_has_session(line, *scan.session_id)) {
			return true;
		}

		// result_replay is an encrypted full tool body, often 100KB+. It is
		// audit-only; restore uses the already-delivered content field.
		// Stripping it before JSON::parse keeps session switches responsive.
		String parse_line = line;
		if (parse_line.find("\"result_replay\"") >= 0) {
			parse_line = solers_transcript_strip_json_string_field(parse_line, "result_replay");
		}

		Dictionary event;
		if (!solers_transcript_parse_record(parse_line, event)) {
			return true;
		}
		if (String(event.get("project_path", String())) != *scan.project_path || String(event.get("session_id", String())) != *scan.session_id) {
			return true;
		}
		*scan.restored_turn_id = MAX(*scan.restored_turn_id, (int)event.get("turn_id", 0));
		*scan.restored_authored_revision = MAX(*scan.restored_authored_revision, (uint64_t)(int64_t)event.get("authored_revision", 0));
		*scan.restored_runtime_epoch = MAX(*scan.restored_runtime_epoch, (uint64_t)(int64_t)event.get("runtime_epoch", 0));
		*scan.restored_observed_revision = MAX(*scan.restored_observed_revision, (uint64_t)(int64_t)event.get("observed_revision", 0));

		const String event_type = event.get("event_type", String());
		if (event_type == "reversal_created") {
			*scan.restored_reversal = event.get("reversal", Dictionary()).duplicate(true);
			return true;
		}
		if (event_type == "reversal_cleared" || event_type == "reversal_consumed") {
			scan.restored_reversal->clear();
			return true;
		}
		if (event_type == "tool_result") {
			String tool_content = String(event.get("content", String()));
			if (tool_content.is_empty()) {
				// Never unprotect result_replay here — that rehydrates the full
				// payload we just refused to parse. UI/model get a slim envelope.
				Dictionary fallback;
				fallback["ok"] = event.get("ok", false);
				if (event.has("error")) {
					fallback["error"] = event["error"];
				}
				if (event.has("result_summary")) {
					fallback["summary"] = event["result_summary"];
				}
				tool_content = JSON::stringify(fallback, "", false, true);
			}
			Dictionary tool_message = SolersLLMMessage::tool_result(
					event.get("call_id", String()),
					event.get("tool", String()),
					tool_content);
			tool_message["ok"] = event.get("ok", false);
			tool_message["duration_msec"] = event.get("duration_msec", event.get("run_msec", 0));
			if (event.has("error")) {
				tool_message["error"] = event["error"];
			}
			scan.restored->push_back(tool_message);
			return true;
		}
		if (event_type == "plan_updated") {
			(*scan.restored_plan)["explanation"] = event.get("explanation", String());
			(*scan.restored_plan)["plan"] = event.get("plan", Array());
			return true;
		}
		if (event_type == "context.apply_compaction") {
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

		const String role = event.get("role", String());
		const String content = event.get("content", String());
		const Array tool_calls = event.get("tool_calls", Array());
		const String reasoning = event.get("reasoning", String());
		if (role == SolersLLMRole::USER) {
			if (content.is_empty()) {
				return true;
			}
			Array mentions = event.get("mentions", Array());
			const bool had_block = content.find("[Selected Solers context]") >= 0;
			const String display = SolersMention::strip_prompt_block(content);
			if (mentions.is_empty() && had_block) {
				mentions = SolersMention::parse(display);
			}
			// Restore path has no live observation; keep path metadata (fresh turns re-observe).
			String model_content = had_block ? content : (display + SolersMention::prompt_block(mentions));
			Dictionary user_message = SolersLLMMessage::user(model_content);
			if (!mentions.is_empty()) {
				user_message["mentions"] = mentions;
			}
			scan.restored->push_back(user_message);
		} else if (role == SolersLLMRole::ASSISTANT) {
			if (content.is_empty() && tool_calls.is_empty() && reasoning.is_empty()) {
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

	Dictionary state;
	state["messages"] = restored;
	state["plan"] = restored_plan;
	state["outcome"] = restored_outcome;
	state["turn_id"] = restored_turn_id;
	state["background_assets"] = restored_background_assets;
	state["authored_revision"] = (int64_t)restored_authored_revision;
	state["runtime_epoch"] = (int64_t)restored_runtime_epoch;
	state["observed_revision"] = (int64_t)restored_observed_revision;
	state["reversal"] = restored_reversal;
	return state;
}

void SolersAgentSession::_stamp_transcript_event(Dictionary &r_event) const {
	r_event["project_path"] = project_path;
	r_event["session_id"] = session_id;
	r_event["authored_revision"] = (int64_t)authored_revision;
	r_event["runtime_epoch"] = (int64_t)runtime_epoch;
	r_event["observed_revision"] = (int64_t)observed_revision;
}

void SolersAgentSession::_write_transcript_event(const String &p_type, const Dictionary &p_payload) const {
	Dictionary event = p_payload.duplicate(true);
	event["event_type"] = p_type;
	event["turn_id"] = turn_id;
	event["wall"] = Time::get_singleton()->get_unix_time_from_system();
	_stamp_transcript_event(event);
	solers_transcript_write(event);
}

void SolersAgentSession::_write_transcript_message(const String &p_role, const String &p_content, const Array &p_mentions, const Array &p_tool_calls, const String &p_reasoning, const Array &p_attachments) const {
	Dictionary event;
	event["event_type"] = "message";
	event["turn_id"] = turn_id;
	event["role"] = p_role;
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
			attachment_meta.push_back(meta);
		}
		event["attachments"] = attachment_meta;
	}
	event["wall"] = Time::get_singleton()->get_unix_time_from_system();
	_stamp_transcript_event(event);
	solers_transcript_write(event);
}

void SolersAgentSession::_write_transcript_plan() const {
	Dictionary event = current_plan.duplicate(true);
	_write_transcript_event("plan_updated", event);
}

void SolersAgentSession::_write_transcript_compaction(const Dictionary &p_result) const {
	Dictionary event = p_result.duplicate(true);
	event["current_plan"] = current_plan.duplicate(true);
	_write_transcript_event("context.apply_compaction", event);
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
		event["args"] = tool_registry->protect_tool_args_for_replay(StringName(p_canonical_name), p_args);
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
	const String replay_result = SolersSecretStore::protect(JSON::stringify(p_result, "", false, true));
	if (SolersSecretStore::is_protected(replay_result)) {
		event["result_replay"] = replay_result;
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

bool SolersAgentSession::_poll_state_observation() {
	if (authored_revision <= observed_revision) {
		return false;
	}
	// Revisions are internal cache and scheduling state. A write must not inject
	// another synthetic user turn; the model can request a snapshot explicitly.
	observed_revision = authored_revision;
	return false;
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
	if (!attachments.is_empty()) {
		message["attachments"] = attachments;
	}
	if (!enriched_mentions.is_empty()) {
		message["mentions"] = enriched_mentions;
	}
	pending_steering_messages.push_back(message);
	Dictionary payload;
	payload["queued"] = pending_steering_messages.size();
	_write_transcript_event("steering_queued", payload);
	Dictionary data;
	data["queued"] = pending_steering_messages.size();
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
		_write_transcript_message("user", SolersMention::strip_prompt_block(message.get("content", String())), mentions, Array(), String(), message.get("attachments", Array()));
	}
	pending_steering_messages.clear();
	return true;
}

Dictionary SolersAgentSession::_commit_dirty_scene_if_needed() {
	Dictionary data;

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	Node *root = editor_interface ? editor_interface->get_edited_scene_root() : nullptr;
	const String path = root ? root->get_scene_file_path() : String();

	bool dirty_before = false;
	int history_id = -1;
	if (EditorUndoRedoManager::get_singleton() && EditorNode::get_singleton()) {
		history_id = EditorNode::get_editor_data().get_current_edited_scene_history_id();
		dirty_before = EditorUndoRedoManager::get_singleton()->is_history_unsaved(history_id);
	}
	if (!dirty_before) {
		return data;
	}

	Error err = ERR_UNCONFIGURED;
	String save_path = path;
	bool assigned_path = false;
	if (editor_interface && root && save_path.is_empty()) {
		// A dirty edited scene without a file yet gets a deterministic home
		// derived from its root name, so authored work is always committable
		// instead of silently evaporating with the in-memory scene.
		save_path = _solers_pending_scene_path(root);
		editor_interface->save_scene_as(save_path, false);
		assigned_path = root->get_scene_file_path() == save_path && FileAccess::exists(save_path);
		err = assigned_path ? OK : ERR_CANT_CREATE;
	} else if (editor_interface && root && !save_path.is_empty()) {
		err = editor_interface->save_scene();
	}
	const bool dirty_after = history_id >= 0 && EditorUndoRedoManager::get_singleton() ?
			EditorUndoRedoManager::get_singleton()->is_history_unsaved(history_id) :
			dirty_before;

	data["ok"] = err == OK;
	data["path"] = save_path;
	if (assigned_path) {
		data["assigned_path"] = true;
	}
	data["dirty_before"] = dirty_before;
	data["dirty_after"] = dirty_after;
	data["error"] = err;
	SOLERS_TRACE("harness.scene_commit", vformat("ok=%d path=%s dirty_before=%d dirty_after=%d err=%d", (int)(err == OK), path, (int)dirty_before, (int)dirty_after, (int)err));
	return data;
}

String SolersAgentSession::_default_system_prompt() const {
	String prompt =
			"You are Solers, an AI agent living natively inside the Solers game engine editor (a Godot 4 fork).\n\n"
			"Operating contract:\n"
			"- Prefer the smallest coherent native change. Inspect live state before editing; do not guess APIs or project contents.\n"
			"- Digests in [Selected Solers context] and Current engine context are authoritative bounded observations of those selections. Answer identity/overview questions from them; use tools only for facts they mark incomplete/truncated or that need live/runtime measurement. Do not rediscover a selection with search/script.run.\n"
			"- Built-in tools are already available. If tool.search is present, it discovers only external plugin, connector, or MCP tools. Skills provide knowledge and never activate capabilities.\n"
			"- Keep scene edits undoable and authored in scene/resources. Hierarchy scaffolding uses scene.edit (create_node/instantiate/set_property batches). Write or patch code only when native composition cannot express the requested behavior.\n"
			"- Scene edits live in the editor undo history until committed. Solers auto-commits the edited scene on every terminal path (and assigns res://<root>.tscn when still unsaved); the engine also saves before playback. Do not save via engine.execute or EditorInterface — a failed commit is reported explicitly.\n"
			"- Appearance: viewport.capture (prefer target=camera for static look-dev; runtime only when gameplay must run). Support/gaps/containment/alignment: scene.validate — a capture is not a measurement.\n"
			"- Use script.run only for algorithmic or bulk mutation scene.edit/resource.edit/script.edit cannot express — not for building scene trees (do not use persist_host_children to scaffold hierarchy).\n"
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

Dictionary SolersAgentSession::_environment_context_message(bool p_include_observation_delta) {
	if (!tool_registry || !tool_registry->observation_service) {
		return Dictionary();
	}
	SolersObservationService *observation = tool_registry->observation_service;
	Dictionary context;
	context["project"] = observation->get_project_info();
	context["project_settings"] = observation->get_project_settings_summary();
	context["authored_revision"] = (int64_t)authored_revision;
	context["platform"] = OS::get_singleton()->get_name();
	context["runtime"] = observation->get_runtime_status();
	context["enabled_plugins"] = ProjectSettings::get_singleton()->has_setting("editor_plugins/enabled") ? GLOBAL_GET("editor_plugins/enabled") : Variant(PackedStringArray());
	// Addon health up front: a half-loaded addon (missing classes, load
	// errors, pending restart) is the model's first fact, not something it has
	// to rediscover through failing tool calls. Healthy addons are byte-identical
	// in every request of every turn, so they ride as a count instead.
	if (tool_registry->asset_service) {
		const Dictionary addon_report = tool_registry->asset_service->addon_list(Dictionary());
		const Array addons = Dictionary(addon_report.get("data", Dictionary())).get("plugins", Array());
		Array unhealthy;
		int ready_count = 0;
		for (const Variant &value : addons) {
			const Dictionary addon = value;
			if ((bool)addon.get("ready", false)) {
				ready_count++;
				continue;
			}
			Dictionary summary;
			summary["id"] = addon.get("key", String());
			summary["version"] = addon.get("version", String());
			summary["installed"] = addon.get("installed", false);
			summary["enabled"] = addon.get("enabled", false);
			summary["restart_required"] = addon.get("restart_required", false);
			summary["missing_classes"] = addon.get("missing_classes", Array());
			summary["load_errors"] = addon.get("load_errors", Array());
			unhealthy.push_back(summary);
		}
		context["installed_addons_ready_count"] = ready_count;
		if (!unhealthy.is_empty()) {
			context["installed_addons_unhealthy"] = unhealthy;
		}
	}
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	Node *edited_root = editor_interface ? editor_interface->get_edited_scene_root() : nullptr;
	const String current_scene_path = edited_root ? edited_root->get_scene_file_path() : String();
	context["current_scene"] = current_scene_path;
	if (edited_root && current_scene_path.is_empty()) {
		// Explicit fact: in-memory only until terminal auto-commit assigns res://.
		context["current_scene_unsaved"] = true;
		context["current_scene_pending_path"] = _solers_pending_scene_path(edited_root);
	} else if (!current_scene_path.is_empty()) {
		const Dictionary observed = observation->observe_path(current_scene_path);
		if ((bool)observed.get("ok", false) && observed.has("digest")) {
			context["current_scene_digest"] = observed["digest"];
		}
	}

	Dictionary selection = observation->get_selection(0, 0);
	Array selected_nodes = selection.get("nodes", Array());
	if (selected_nodes.size() > 16) {
		selected_nodes.resize(16);
		selection["truncated"] = true;
	}
	selection["nodes"] = selected_nodes;
	context["selection"] = selection;

	if (p_include_observation_delta) {
		Dictionary observe_args;
		observe_args["since_cursor"] = (int64_t)runtime_observation_cursor;
		const Dictionary runtime_observations = observation->observe_runtime(observe_args);
		runtime_observation_cursor = (int64_t)runtime_observations.get("cursor", runtime_observation_cursor);
		const Dictionary error_digest = runtime_observations.get("error_digest", Dictionary());
		if (!error_digest.is_empty()) {
			context["runtime_error_digest"] = error_digest;
			context["runtime_epoch_error_count"] = runtime_observations.get("epoch_error_count", 0);
		}
	}
	// Editor diagnostics are current engine facts, so they ride this
	// per-request message instead of being appended to durable history where
	// one noisy turn would keep paying for itself on every later request.
	const Dictionary diagnostics = _take_godot_diagnostics();
	if (!diagnostics.is_empty()) {
		context["godot_diagnostics"] = diagnostics;
	}
	Dictionary message = SolersLLMMessage::user(
			"Current engine context (authoritative, bounded, and refreshed for this request):\n" + JSON::stringify(context, "", false, true));
	message["origin"] = "solers_state";
	return message;
}

Array SolersAgentSession::_collect_tools() const {
	Array out;
	if (!tool_registry) {
		return out;
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
		Dictionary schema = def.get("input_schema", Dictionary());
		if (schema.is_empty()) {
			schema["type"] = "object";
			schema["properties"] = Dictionary();
		}
		tool["parameters"] = schema;
		out.push_back(tool);
	}
	return out;
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

	int resolved_context = (int)profile.get("context_window", 0);
	if ((int)model.get("context", 0) > 0) {
		resolved_context = model.get("context", 0);
	}
	const Dictionary model_limits = Dictionary(profile.get("model_limits", Dictionary())).get(model_id, Dictionary());
	if ((int)model_limits.get("context", 0) > 0) {
		resolved_context = model_limits.get("context", 0);
	}
	if (active_provider.has("context_window")) {
		resolved_context = (int)active_provider["context_window"];
	}
	// A rejected request outranks every advertised number: it is the only
	// statement the endpoint itself has made about its real window.
	if (learned_context_ceiling > 0) {
		resolved_context = resolved_context > 0 ? MIN(resolved_context, learned_context_ceiling) : learned_context_ceiling;
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
		resolved_output = 8192;
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

Dictionary SolersAgentSession::_build_request(const Array &p_messages, const String &p_request_system_prompt) const {
	Dictionary request;
	request["model"] = active_provider.get("model", String());
	request["system"] = p_request_system_prompt;
	request["tools"] = _collect_tools();
	request["messages"] = p_messages;
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

Dictionary SolersAgentSession::_redacted_request_graph(const Dictionary &p_request, const Dictionary &p_profile) const {
	Dictionary graph;
	graph["provider"] = active_provider.get("provider", String());
	graph["model"] = p_request.get("model", String());
	graph["protocol"] = p_profile.get("protocol", String());
	if (p_request.has("max_tokens")) {
		graph["max_tokens"] = p_request["max_tokens"];
	}
	if (p_request.has("reasoning_effort")) {
		graph["reasoning_effort"] = p_request["reasoning_effort"];
	}
	graph["tool_count"] = Array(p_request.get("tools", Array())).size();

	Array items;
	if (!protocol_registry) {
		graph["items"] = items;
		return graph;
	}

	const StringName protocol_id = StringName(p_profile.get("protocol", String()));
	const SolersLLMProtocol *protocol = protocol_registry->get(protocol_id);
	if (!protocol) {
		graph["items"] = items;
		return graph;
	}

	const Dictionary body = protocol->build_request_body(p_request);
	const Array request_messages = body.has("messages") ? Array(body["messages"]) : Array(body.get("input", Array()));
	for (int i = 0; i < request_messages.size(); i++) {
		const Dictionary item = request_messages[i];
		Dictionary redacted;
		if (item.has("type")) {
			redacted["type"] = item.get("type", String());
		} else if (item.has("role")) {
			redacted["role"] = item.get("role", String());
		}
		if (item.has("tool_call_id")) {
			redacted["tool_call_id"] = item.get("tool_call_id", String());
		}
		if (item.has("name")) {
			redacted["name"] = item.get("name", String());
		}
		const Array tool_calls = item.get("tool_calls", Array());
		if (!tool_calls.is_empty()) {
			Array redacted_calls;
			for (int c = 0; c < tool_calls.size(); c++) {
				const Dictionary call = tool_calls[c];
				const Dictionary fn = call.get("function", Dictionary());
				Dictionary redacted_call;
				redacted_call["id"] = call.get("id", String());
				redacted_call["name"] = fn.get("name", String());
				redacted_calls.push_back(redacted_call);
			}
			redacted["tool_calls"] = redacted_calls;
		}
		items.push_back(redacted);
	}
	graph["items"] = items;
	return graph;
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

Error SolersAgentSession::_dispatch_model_request(bool p_skip_compaction) {
	const Dictionary availability_error = _provider_dispatch_error();
	if (!availability_error.is_empty()) {
		const Dictionary error = availability_error.get("error", Dictionary());
		_finish_turn("failed", error.get("message", "The selected provider is unavailable."), error);
		return ERR_UNAVAILABLE;
	}
	if (model_request_budget > 0 && model_request_index >= model_request_budget) {
		_finish_turn("paused", vformat("Turn paused after reaching its model request budget (%d). Start a new turn to continue with the persisted plan and evidence.", model_request_budget));
		return ERR_BUSY;
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
	const Dictionary auth = active_provider.get("auth", Dictionary());

	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url);
	if (system_prompt.is_empty()) {
		system_prompt = _default_system_prompt();
	}
	const Array tools = _collect_tools();
	if (!p_skip_compaction && context_manager && context_manager->should_compact(messages, system_prompt, tools, context_window, max_output_tokens)) {
		return _begin_compaction(false);
	}

	Array request_messages = context_manager
			? context_manager->prepare_request(messages, system_prompt, tools)
			: SolersContextManager::repair_tool_pairing(messages);
	// The dynamic engine facts ride at the end of the projection instead of
	// inside the system prompt, so every request keeps a byte-stable prefix
	// (system + tools + history) for provider prompt caching.
	const Dictionary environment_message = _environment_context_message(true);
	if (!environment_message.is_empty()) {
		request_messages.push_back(environment_message);
	}
	Dictionary request = _build_request(request_messages, system_prompt);
	current_provider_metadata.clear();
	model_request_index++;
	Dictionary request_event;
	request_event["request_index"] = model_request_index;
	request_event["provider"] = provider_id;
	request_event["model"] = active_provider.get("model", String());
	request_event["message_count"] = request_messages.size();
	_write_transcript_event("model_request_started", request_event);

	_record("agent_model_request_graph", _redacted_request_graph(request, profile));

	SOLERS_TRACE("session.begin", "client->begin() (joins prior worker thread)");
	const Error err = client->begin(request, profile, auth);
	SOLERS_TRACE("session.begin_done", vformat("err=%d", (int)err));
	if (err != OK) {
		const Dictionary error = client->get_error();
		_finish_turn("failed", String(error.get("message", "Failed to start the model request.")), error);
		return err;
	}
	running = true;
	phase = PHASE_STREAMING;
	streamed_tool_calls.clear();
	text_delta_count = 0;
	last_text_delta_msec = 0;
	SOLERS_TRACE("session.dispatch", vformat("model request started (turn=%d)", turn_id));
	emit_signal(SNAME("model_request_started"));
	return OK;
}

Error SolersAgentSession::_begin_compaction(bool p_from_overflow) {
	if (!context_manager || messages.is_empty()) {
		return ERR_UNAVAILABLE;
	}
	compaction_source_messages = messages.duplicate(true);
	compaction_request_attempt = 0;
	compaction_triggered_by_overflow = p_from_overflow;
	current_text = String();
	current_reasoning = String();
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	last_stop_reason = String();
	phase = PHASE_COMPACTING;
	Dictionary payload;
	payload["source"] = p_from_overflow ? "overflow" : "auto";
	payload["tokens_before"] = context_manager->get_token_count_with_pending(messages, system_prompt, _collect_tools());
	_record("context_compaction_started", payload);
	emit_signal(SNAME("compaction_started"));
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
	const Dictionary auth = active_provider.get("auth", Dictionary());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url);

	Array history = compaction_source_messages;
	if (compaction_request_attempt > 0) {
		history = context_manager->shrink_compaction_history(history, compaction_request_attempt);
	}
	history = context_manager->prepare_request(history, system_prompt, Array());
	String instruction = SolersContextManager::COMPACTION_INSTRUCTION;
	if (!current_plan.is_empty()) {
		instruction += "\n\nCurrent plan:\n" + JSON::stringify(current_plan, "  ", false, true);
	}
	history.push_back(SolersLLMMessage::user(instruction));

	Dictionary request = _build_request(history, system_prompt);
	request["tools"] = Array();
	current_provider_metadata.clear();
	_record("context_compaction_request_graph", _redacted_request_graph(request, profile));

	const Error err = client->begin(request, profile, auth);
	if (err != OK) {
		const Dictionary error = client->get_error();
		_finish_turn("failed", String(error.get("message", "Failed to start context compaction.")), error);
		return err;
	}
	running = true;
	phase = PHASE_COMPACTING;
	text_delta_count = 0;
	last_text_delta_msec = 0;
	return OK;
}

bool SolersAgentSession::_is_context_overflow(const Dictionary &p_error) const {
	const String code = String(p_error.get("code", String())).to_lower();
	const String type = String(p_error.get("type", String())).to_lower();
	const String message = String(p_error.get("message", String())).to_lower();
	if (code.contains("context_length") || type.contains("context_length") ||
			message.contains("context_length_exceeded") || message.contains("context window") ||
			message.contains("maximum context") || message.contains("context too long")) {
		return true;
	}
	return (int)p_error.get("http_status", 0) == 413;
}

// An overflow proves the assumed window is a lie: the request we just sent did
// not fit. Clamp to what actually failed so compaction breathes against the
// real limit from here on instead of chasing catalog metadata.
void SolersAgentSession::_learn_context_ceiling() {
	const int rejected_tokens = context_manager ? context_manager->get_last_estimated_tokens() : 0;
	if (rejected_tokens <= 0) {
		return;
	}
	const int ceiling = MAX(SolersContextManager::MIN_LEARNED_CONTEXT_TOKENS, (int)((double)rejected_tokens * 0.9));
	if (learned_context_ceiling > 0 && learned_context_ceiling <= ceiling) {
		return;
	}
	Dictionary payload;
	payload["advertised_context_window"] = context_window;
	payload["rejected_tokens"] = rejected_tokens;
	learned_context_ceiling = ceiling;
	_refresh_active_model_limits();
	payload["context_window"] = context_window;
	_write_transcript_event("model_context_window_corrected", payload);
	_record("agent_model_context_window_corrected", payload);
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
		} else if (kind == SolersLLMEventKind::FINISH) {
			last_stop_reason = event.get("stop_reason", String());
		}
	}

	if (client->is_failed()) {
		const Dictionary error = client->get_error();
		if (_is_context_overflow(error) && compaction_request_attempt < 3) {
			_learn_context_ceiling();
			compaction_request_attempt++;
			current_text = String();
			current_reasoning = String();
			_dispatch_compaction_request();
			return;
		}
		if (_schedule_llm_retry(error)) {
			return;
		}
		_finish_turn("failed", String(error.get("message", "Context compaction failed.")), error);
		return;
	}
	if (!client->is_done()) {
		return;
	}
	if ((current_text.strip_edges().is_empty() || last_stop_reason == SolersLLMStopReason::MAX_TOKENS) && compaction_request_attempt < 5) {
		compaction_request_attempt++;
		current_text = String();
		current_reasoning = String();
		_dispatch_compaction_request();
		return;
	}
	if (current_text.strip_edges().is_empty()) {
		const Dictionary error = _error("COMPACTION_FAILED", "The model did not produce a usable context summary.").get("error", Dictionary());
		_finish_turn("failed", String(error.get("message", "Context compaction failed.")), error);
		return;
	}
	_on_compaction_complete();
}

void SolersAgentSession::_on_compaction_complete() {
	const Dictionary result = context_manager->apply_compaction(compaction_source_messages, current_text, current_plan);
	messages = result.get("messages", Array());
	_write_transcript_compaction(result);
	_record("context_compaction_completed", result);
	emit_signal(SNAME("compaction_completed"), result);

	compaction_source_messages.clear();
	compaction_request_attempt = 0;
	compaction_triggered_by_overflow = false;
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
	model_request_budget = MAX(0, (int)p_args.get("max_model_requests", 0));

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
	Dictionary user_message = SolersLLMMessage::user(model_prompt);
	if (!turn_attachments.is_empty()) {
		user_message["attachments"] = turn_attachments.duplicate(true);
	}
	if (!turn_mentions.is_empty()) {
		user_message["mentions"] = turn_mentions.duplicate(true);
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
	compaction_request_attempt = 0;
	retry_attempt = 0;
	retry_resume_msec = 0;
	model_request_index = 0;
	turn_id++;
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
	_write_transcript_message("user", prompt, turn_mentions, Array(), String(), turn_attachments);

	const Error err = _dispatch_model_request();
	if (err != OK) {
		return _error("DISPATCH_FAILED", "Failed to dispatch the model request.");
	}
	Dictionary data;
	data["turn_id"] = turn_id;
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
		if (phase == PHASE_COMPACTING) {
			_dispatch_compaction_request();
		} else {
			_dispatch_model_request();
		}
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
	Dictionary protocol_error;
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
			if (context_manager) {
				// Canonical input_tokens excludes cached shares; the window is
				// occupied by fresh + cache-read + cache-write together.
				const int prompt_tokens = (int)e.get("input_tokens", 0) + MAX(0, (int)e.get("cache_read_tokens", 0)) + MAX(0, (int)e.get("cache_write_tokens", 0));
				context_manager->record_usage(prompt_tokens, messages.size());
			}
		} else if (kind == SolersLLMEventKind::FINISH) {
			last_stop_reason = e.get("stop_reason", String());
			current_provider_metadata = e.get("provider_metadata", Dictionary());
		} else if (kind == SolersLLMEventKind::ERROR) {
			if (protocol_error.is_empty()) {
				protocol_error["code"] = e.get("code", "PROTOCOL_ERROR");
				protocol_error["message"] = e.get("message", "The provider stream reported a protocol error.");
				protocol_error["recoverable"] = true;
			}
		}
	}
	if (!protocol_error.is_empty()) {
		client->abort();
		_finish_turn("failed", protocol_error.get("message", String()), protocol_error);
		return;
	}

	if (client->is_failed()) {
		const Dictionary error = client->get_error();
		if (_is_context_overflow(error)) {
			_learn_context_ceiling();
			overflow_compaction_attempts++;
			if (overflow_compaction_attempts <= 3) {
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
	retry_attempt = 0;
	overflow_compaction_attempts = 0;
	Dictionary response_event;
	response_event["request_index"] = model_request_index;
	response_event["stop_reason"] = last_stop_reason;
	response_event["tool_call_count"] = pending_tool_calls.size();
	response_event["text_bytes"] = current_text.utf8().length();
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
	messages.push_back(SolersLLMMessage::assistant(current_text, pending_tool_calls, current_provider_metadata));
	if (!current_text.is_empty() || !pending_tool_calls.is_empty() || !current_reasoning.is_empty()) {
		_write_transcript_message("assistant", current_text, Array(), pending_tool_calls, current_reasoning);
	}
	if (pending_tool_calls.is_empty()) {
		const String final_text = current_text;
		if (final_text.is_empty() && current_reasoning.is_empty()) {
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
		if (!current_text.is_empty()) {
			emit_signal(SNAME("assistant_message"), current_text);
		}
		last_assistant_msec = OS::get_singleton()->get_ticks_msec();
		current_text = String();
		current_reasoning = String();
		pending_tool_calls.clear();
		streamed_tool_calls.clear();
		if (_demand_scene_validation()) {
			return;
		}
		_finish_turn("completed", final_text);
		return;
	}
	if (!current_text.is_empty()) {
		emit_signal(SNAME("assistant_message"), current_text);
	}
	last_assistant_msec = OS::get_singleton()->get_ticks_msec();

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

// geometry_revision is advanced by the engine when an edit actually moves
// geometry; scene_validation_revision only by a tool that measured it. A gap
// between them is unverified authored work, stated as a fact rather than
// inferred from what the model said it did. The door asks once per gap: the
// model gets one more step to measure, and the turn outcome carries the debt
// either way, so this can never become a retry loop.
bool SolersAgentSession::_demand_scene_validation() {
	if (geometry_revision == 0 || geometry_revision <= scene_validation_revision) {
		return false;
	}
	if (scene_validation_demanded_revision == geometry_revision) {
		return false;
	}
	scene_validation_demanded_revision = geometry_revision;
	const String demand = "This turn moved scene geometry that no native measurement has checked. Run scene.validate against the nodes you changed (mode \"spatial\" for the relations you intended, mode \"structure\" for support and layout) and fix whatever it reports before you report this work done. A viewport capture cannot substitute for it: an image does not measure position, containment, or support.";
	Dictionary message = SolersLLMMessage::user(demand);
	message["origin"] = "solers_continuation";
	messages.push_back(message);
	_write_transcript_message("user", demand, Array());
	Dictionary payload;
	payload["geometry_revision"] = (int64_t)geometry_revision;
	payload["scene_validation_revision"] = (int64_t)scene_validation_revision;
	_write_transcript_event("scene_validation_demanded", payload);
	_dispatch_model_request();
	return true;
}

void SolersAgentSession::_finish_turn(const String &p_outcome, const String &p_message, const Dictionary &p_error) {
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
	// Every terminal path commits authored scene work — including aborts.
	// Stopping the loop must never discard finished edits; explicit reversal
	// (history.revert, editor undo) is the mechanism that undoes work.
	const Dictionary scene_commit = _commit_dirty_scene_if_needed();
	if (!scene_commit.is_empty()) {
		data["scene_commit"] = scene_commit;
		if (!(bool)scene_commit.get("ok", true)) {
			// A failed commit means authored work is still memory-only. Put the
			// native cause into history so the next turn acts on it instead of
			// assuming the scene was saved.
			const String commit_path = scene_commit.get("path", String());
			const String commit_message = vformat("Solers could not commit the edited scene when this turn ended (path: %s, error %d). The scene is still unsaved and its authored work exists only in memory; Solers will retry the commit on the next terminal path.", commit_path.is_empty() ? String("<unassigned>") : commit_path, (int)scene_commit.get("error", 0));
			Dictionary message = SolersLLMMessage::user(commit_message);
			message["origin"] = "godot_diagnostics";
			messages.push_back(message);
			// Authored work that never reached disk is not a completed turn,
			// whatever the model claimed in its closing message.
			if (outcome == "completed") {
				outcome = "failed";
				error = _error("SCENE_COMMIT_FAILED", commit_message).get("error", Dictionary());
			}
		}
	}
	data["outcome"] = outcome;
	// Unmeasured authored geometry is reported, never hidden, even when the
	// turn is allowed to end with it outstanding.
	const bool scene_validation_pending = geometry_revision > scene_validation_revision;
	if (scene_validation_pending) {
		data["scene_validation_pending"] = true;
	}
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

	Dictionary transcript;
	transcript["outcome"] = outcome;
	transcript["message"] = p_message;
	if (scene_validation_pending) {
		transcript["scene_validation_pending"] = true;
	}
	transcript["stop_reason"] = last_stop_reason;
	{
		MutexLock lock(godot_log_mutex);
		transcript["godot_log_errors"] = godot_log_error_count;
		transcript["godot_log_warnings"] = godot_log_warning_count;
	}
	if (!scene_commit.is_empty()) {
		transcript["scene_commit"] = scene_commit;
	}
	if (!last_usage.is_empty()) {
		transcript["usage"] = last_usage;
	}
	if (!error.is_empty()) {
		transcript["error"] = error;
	}
	_write_transcript_event("turn_outcome", transcript);
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
	compaction_source_messages.clear();
	compaction_request_attempt = 0;
	overflow_compaction_attempts = 0;
	compaction_triggered_by_overflow = false;
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
		if (_poll_state_observation()) {
			return; // the automatic capture is still rendering; poll again next tick.
		}
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
	audit["args"] = tool_registry ? tool_registry->protect_tool_args_for_replay(StringName(canonical_name), parsed_args) : parsed_args;
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
			Dictionary result = cached->duplicate(true);
			Dictionary data = result.get("data", Dictionary());
			data["unchanged"] = true;
			data["authored_revision"] = (int64_t)authored_revision;
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
	pending->scene_digest_before = deferred_scene_digest_before;
	pending->geometry_digest_before = deferred_geometry_digest_before;
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
		deferred_scene_digest_before = pending->scene_digest_before;
		deferred_geometry_digest_before = pending->geometry_digest_before;
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
		queued_msec = (int64_t)Dictionary(tool_queue[p_queue_index]).get("queued_msec", queued_msec);
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
		_deliver_tool_result(entry.get("id", String()), entry.get("model_name", String()), canonical_name, entry.get("args", Dictionary()), result);
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
				entry.has("result") ? Dictionary(entry.get("result", cancelled)) : cancelled);
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
	deferred_scene_digest_before = 0;
	deferred_geometry_digest_before = 0;
	if (tool_registry && tool_registry->reflection_service && tool_registry->affects_scene_state(StringName(p_canonical_name))) {
		deferred_scene_digest_before = tool_registry->reflection_service->get_scene_state_digest();
		deferred_geometry_digest_before = tool_registry->reflection_service->get_spatial_geometry_digest();
	}
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
	if (deferred_canonical_name == "viewport.capture") {
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
		if ((bool)result.get("ok", false) && (bool)godot_error.get("handler_window", false) && !checkpointed_commit) {
			// Errors reported while the handler was on an audited stack make a
			// success verdict false evidence; deferred-window events stay
			// attached as evidence without overriding the handler's verdict.
			result["ok"] = false;
			result["error"] = godot_error;
		}
	}
	const bool tool_succeeded = (bool)result.get("ok", false);
	Dictionary data = result.get("data", Dictionary());
	bool scene_state_changed = false;
	bool spatial_geometry_changed = false;
	if (tool_registry && tool_registry->reflection_service && tool_registry->affects_scene_state(StringName(canonical_name))) {
		const uint64_t scene_digest_after = tool_registry->reflection_service->get_scene_state_digest();
		const uint64_t geometry_digest_after = tool_registry->reflection_service->get_spatial_geometry_digest();
		scene_state_changed = scene_digest_after != deferred_scene_digest_before || (bool)data.get("scene_state_changed", false);
		spatial_geometry_changed = geometry_digest_after != deferred_geometry_digest_before || (bool)data.get("spatial_geometry_changed", false);
		data["scene_state_changed"] = scene_state_changed;
		data["spatial_geometry_changed"] = spatial_geometry_changed;
		result["data"] = data;
	}
	if (tool_succeeded) {
		if (tool_registry && !tool_registry->is_read_only(StringName(canonical_name), args)) {
			if (tool_registry->affects_scene_state(StringName(canonical_name))) {
				if (scene_state_changed) {
					authored_revision++;
					scene_revision = authored_revision;
				}
				if (spatial_geometry_changed) {
					geometry_revision = authored_revision;
					render_artifacts.clear();
					if ((bool)data.get("preserves_structure_validation", false) && scene_validation_revision > 0) {
						scene_validation_revision = geometry_revision;
					}
				} else if ((bool)data.get("mesh_data_changed", false)) {
					render_artifacts.erase("lightmap");
				}
			} else if (tool_registry->affects_authored_state(StringName(canonical_name)) && (bool)data.get("authored_state_changed", true)) {
				authored_revision++;
			}
		}
		const Dictionary artifact = data.get("artifact", Dictionary());
		const String artifact_kind = artifact.get("kind", String());
		if (!artifact_kind.is_empty()) {
			render_artifacts[artifact_kind] = artifact;
		}
		if (tool_registry && tool_registry->produces_scene_validation(StringName(canonical_name))) {
			scene_validation_revision = geometry_revision;
		}
		if (canonical_name == "scene.inspect") {
			observed_revision = authored_revision;
		} else if (canonical_name == "viewport.capture") {
			const String target = data.get("target", String());
			if (target == "runtime") {
				runtime_capture_revision = authored_revision;
			} else if ((bool)data.get("material_preview", true)) {
				if (target == "camera") {
					camera_capture_revision = authored_revision;
				}
				observed_revision = authored_revision;
				if ((bool)data.get("frame_valid", true)) {
					editor_capture_revision = authored_revision;
				}
			}
		} else if (canonical_name == "runtime.control") {
			if ((bool)data.get("started_by_call", false) || (bool)data.get("stopped_by_call", false)) {
				runtime_epoch++;
			}
		}
	}
	if (deferred_prepared_call) {
		tool_registry->_complete_prepared_tool(*deferred_prepared_call, result);
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
	const bool cacheable = canonical_name != "viewport.capture" && !result.has("attachments") && !Dictionary(result.get("data", Dictionary())).has("poll_args");
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

String SolersAgentSession::_spill_tool_result(const String &p_call_id, const String &p_content) {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	const String name = p_call_id.validate_filename();
	if (!project_settings || name.is_empty()) {
		return String();
	}
	const String directory = project_settings->get_project_data_path().path_join("solers/tool-results");
	if (DirAccess::make_dir_recursive_absolute(project_settings->globalize_path(directory)) != OK) {
		return String();
	}
	const String path = directory.path_join(name + ".json");
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		return String();
	}
	file->store_string(p_content);
	return path;
}

String SolersAgentSession::deliverable_tool_result(const String &p_call_id, const Dictionary &p_result, int p_budget) {
	const String full_result = JSON::stringify(p_result, "", false, true);
	const int tokens = SolersContextManager::estimate_tokens(full_result);
	if (tokens <= p_budget) {
		return full_result;
	}
	// Over-budget: keep authoritative digests, spill the fat details. Never clip
	// JSON mid-string, and never leave the model with only data_keys.
	Dictionary envelope;
	envelope["ok"] = p_result.get("ok", false);
	if (p_result.has("error")) {
		envelope["error"] = p_result["error"];
	}
	if (p_result.has("diagnostics")) {
		envelope["diagnostics"] = p_result["diagnostics"];
	}
	const Dictionary data = p_result.get("data", Dictionary());
	Dictionary kept_data;
	if (data.has("digest")) {
		kept_data["digest"] = data["digest"];
	}
	if (p_result.has("digest")) {
		kept_data["digest"] = p_result["digest"];
	}
	if (!kept_data.is_empty()) {
		envelope["data"] = kept_data;
	}
	Dictionary elided;
	elided["tokens"] = tokens;
	elided["token_budget"] = p_budget;
	elided["data_keys"] = data.keys();
	elided["kept_digest"] = kept_data.has("digest");
	const String spill_path = _spill_tool_result(p_call_id, full_result);
	if (spill_path.is_empty()) {
		elided["recovery"] = kept_data.has("digest") ? "Digest retained above; re-run with narrower arguments for full details." : "Re-run this call with narrower arguments; the complete body could not be written to disk.";
	} else {
		elided["complete_result_path"] = spill_path;
		elided["recovery"] = kept_data.has("digest") ? vformat("Digest retained above. Full body spilled to %s if you need non-digest details.", spill_path) : vformat("Read %s for the complete body, or re-run this call with narrower arguments.", spill_path);
	}
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

void SolersAgentSession::_deliver_tool_result(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result) {
	Dictionary result = p_result.duplicate(true);
	result["call_id"] = p_id;
	// A result widens the tool surface by declaring what it unlocks, so this
	// path never has to recognise which tool produced it.
	const PackedStringArray unlocked = Dictionary(result.get("data", Dictionary())).get("unlock_tools", PackedStringArray());
	for (int i = 0; i < unlocked.size(); i++) {
		task_deferred_tools.insert(StringName(unlocked[i]));
	}
	const Array accesses = tool_registry && !p_canonical_name.is_empty() ? tool_registry->resolve_resource_access(StringName(p_canonical_name), p_args) : Array();
	const Dictionary diagnostics = _take_godot_diagnostics();
	if (!diagnostics.is_empty()) {
		result["diagnostics"] = diagnostics;
	}
	// The transcript records exactly the bytes the model was given, so a
	// restored session rebuilds the same conversation it originally had.
	const String content = deliverable_tool_result(p_id, result, SolersContextManager::tool_result_token_budget(context_window));
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
	scene_revision = 0;
	geometry_revision = 0;
	editor_capture_revision = 0;
	camera_capture_revision = 0;
	runtime_capture_revision = 0;
	scene_validation_revision = 0;
	scene_validation_demanded_revision = 0;
	runtime_observation_cursor = 0;
	render_artifacts.clear();
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
	pending_steering_messages.clear();
	current_plan.clear();
	last_outcome = String();
	last_stop_reason = String();
	last_usage.clear();
	last_assistant_msec = 0;
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
	runtime_epoch = 0;
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
	const Dictionary state = _read_transcript_state(project_path, session_id);
	messages = state.get("messages", Array());
	current_plan = state.get("plan", Dictionary());
	last_outcome = state.get("outcome", String());
	turn_id = state.get("turn_id", 0);
	authored_revision = (int64_t)state.get("authored_revision", 0);
	runtime_epoch = (int64_t)state.get("runtime_epoch", 0);
	observed_revision = (int64_t)state.get("observed_revision", 0);
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
	Dictionary message = SolersLLMMessage::user("Background job terminal delta. Continue the original task using these persisted facts; call asset.status only if you need more detail from a job that has already reached a project-import terminal state:\n" + JSON::stringify(deliveries, "", false, true));
	message["origin"] = "background_job_delta";
	messages.push_back(message);
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
	status["model_request_budget"] = model_request_budget;
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
	status["authored_revision"] = (int64_t)authored_revision;
	status["runtime_epoch"] = (int64_t)runtime_epoch;
	status["observed_revision"] = (int64_t)observed_revision;
	status["editor_capture_revision"] = (int64_t)editor_capture_revision;
	status["scene_validation_revision"] = (int64_t)scene_validation_revision;
	status["geometry_revision"] = (int64_t)geometry_revision;
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
