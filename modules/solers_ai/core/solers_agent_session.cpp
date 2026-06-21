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
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_provider_catalog.h"
#include "modules/solers_ai/llm/solers_llm_retry.h"
#include "modules/solers_ai/llm/solers_models_dev.h"
#include "scene/main/node.h"

static String _transcript_text(const String &p_text, int p_byte_limit) {
	if (p_byte_limit <= 0 || p_text.utf8().length() <= p_byte_limit) {
		return p_text;
	}
	return p_text.left(p_byte_limit / 2) + "\n...[Solers transcript content truncated]";
}

void SolersAgentSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_turn", "args"), &SolersAgentSession::start_turn);
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

String SolersAgentSession::_make_session_id() const {
	return OS::get_singleton()->get_unique_id() + "-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
}

Array SolersAgentSession::_read_transcript_messages(const String &p_project_path, const String &p_session_id) const {
	Array restored;
	if (p_project_path.is_empty() || p_session_id.is_empty()) {
		return restored;
	}

	Ref<FileAccess> file = FileAccess::open("user://solers_ai_transcript.jsonl", FileAccess::READ);
	if (file.is_null()) {
		return restored;
	}

	while (!file->eof_reached()) {
		const String line = file->get_line().strip_edges();
		if (line.is_empty()) {
			continue;
		}
		const Variant parsed = JSON::parse_string(line);
		if (parsed.get_type() != Variant::DICTIONARY) {
			continue;
		}

		const Dictionary event = parsed;
		if (String(event.get("project_path", String())) != p_project_path || String(event.get("session_id", String())) != p_session_id) {
			continue;
		}

		const String role = event.get("role", String());
		const String content = event.get("content", String());
		if (content.is_empty()) {
			continue;
		}
		if (role == SolersLLMRole::USER) {
			restored.push_back(SolersLLMMessage::user(content));
		} else if (role == SolersLLMRole::ASSISTANT) {
			restored.push_back(SolersLLMMessage::assistant(content, Array()));
		}
	}

	return restored;
}

void SolersAgentSession::_stamp_transcript_event(Dictionary &r_event) const {
	r_event["project_path"] = project_path;
	r_event["session_id"] = session_id;
}

void SolersAgentSession::_write_transcript_message(const String &p_role, const String &p_content) const {
	Dictionary event;
	event["turn_id"] = turn_id;
	event["role"] = p_role;
	event["content"] = _transcript_text(p_content, context_window);
	_stamp_transcript_event(event);
	solers_transcript_write(event);
}

void SolersAgentSession::_write_transcript_tool(const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result) const {
	Dictionary event;
	event["turn_id"] = turn_id;
	event["role"] = "tool";
	event["tool"] = p_canonical_name;
	event["ok"] = p_result.get("ok", false);
	if (tool_registry) {
		event["args"] = tool_registry->summarize_tool_args_for_audit(StringName(p_canonical_name), p_args);
		event["result_summary"] = tool_registry->summarize_tool_result_for_audit(p_result);
	} else {
		event["args"] = p_args;
	}
	_stamp_transcript_event(event);
	solers_transcript_write(event);
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
	if (editor_interface && root && !path.is_empty()) {
		err = editor_interface->save_scene();
	}
	const bool dirty_after = history_id >= 0 && EditorUndoRedoManager::get_singleton() ?
			EditorUndoRedoManager::get_singleton()->is_history_unsaved(history_id) :
			dirty_before;

	data["ok"] = err == OK;
	data["path"] = path;
	data["dirty_before"] = dirty_before;
	data["dirty_after"] = dirty_after;
	data["error"] = err;
	SOLERS_TRACE("harness.scene_commit", vformat("ok=%d path=%s dirty_before=%d dirty_after=%d err=%d", (int)(err == OK), path, (int)dirty_before, (int)dirty_after, (int)err));
	return data;
}

String SolersAgentSession::_default_system_prompt() const {
	return vformat(
			"You are Solers, an AI agent living natively inside the Solers game engine editor (a Godot 4 fork).\n\n"
			"Iron laws:\n"
			"- Mutate scenes through objects.batch (undoable); never use menu-style scene tools.\n"
			"- objects.batch create_node uses parent_path (parent alias accepted); reparent uses new_parent_path (new_parent alias accepted).\n"
			"- project.write_file: send exactly one non-empty payload (content OR content_base64); omit unused/empty keys.\n"
			"- Discover deferred tools with tool.search (token match), then call by canonical name.\n"
			"- Persist scene edits through the harness commit path; do not script.patch scene resources.\n"
			"- Scene edits save at turn end when the editor history is dirty; no manual scene save.\n"
			"- For @tool scripts, do not claim generated preview/runtime children are persisted unless they have scene owners or are baked into the scene.\n"
			"- Prefer class.introspect + object.call_method, project assets, and short GDScript via project.write_file/attach_script over dozens of CSG primitives.\n"
			"- Budget <=%d tool calls per user request; verify with editor.get_snapshot and runtime.control when needed.",
			max_tool_iterations);
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
		if (exposure == "deferred" || exposure == "hidden") {
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

Dictionary SolersAgentSession::_build_request() const {
	Dictionary request;
	request["model"] = active_provider.get("model", String());
	if (force_final_answer) {
		request["system"] = system_prompt + "\n\nYou have reached this turn's tool-call limit. Do not request any more tools. Briefly summarize what you changed and what still needs doing, then stop.";
		request["tools"] = Array();
	} else {
		request["system"] = system_prompt;
		request["tools"] = _collect_tools();
	}
	request["messages"] = messages;
	request["max_tokens"] = max_output_tokens;
	return request;
}

Dictionary SolersAgentSession::_redacted_request_graph(const Dictionary &p_request, const Dictionary &p_profile) const {
	Dictionary graph;
	graph["provider"] = active_provider.get("provider", String());
	graph["model"] = p_request.get("model", String());
	graph["protocol"] = p_profile.get("protocol", String());

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
	const Array request_messages = body.get("messages", Array());
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

Error SolersAgentSession::_dispatch_model_request() {
	const String provider_id = active_provider.get("provider", String());
	const String base_url = active_provider.get("base_url", String());
	const String api_key = active_provider.get("api_key", String());

	const Dictionary profile = provider_catalog->resolve(StringName(provider_id), base_url);

	if (context_manager) {
		messages = context_manager->compact(messages, context_window, max_output_tokens);
	}

	Dictionary request = _build_request();

	_record("agent_model_request_graph", _redacted_request_graph(request, profile));

	SOLERS_TRACE("session.begin", "client->begin() (joins prior worker thread)");
	const Error err = client->begin(request, profile, api_key);
	SOLERS_TRACE("session.begin_done", vformat("err=%d", (int)err));
	if (err != OK) {
		running = false;
		emit_signal(SNAME("turn_failed"), client->get_error());
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
	if (was_announced) {
		emit_signal(SNAME("tool_call_updated"), id, canonical_name, arguments);
	} else {
		emit_signal(SNAME("tool_call_started"), id, canonical_name, arguments);
	}
	return call;
}

Dictionary SolersAgentSession::start_turn(const Dictionary &p_args) {
	if (running) {
		return _error("AGENT_BUSY", "A Solers agent turn is already running.");
	}
	if (!tool_registry || !settings_service) {
		return _error("AGENT_UNCONFIGURED", "Solers agent session is missing its services.");
	}

	const String prompt = String(p_args.get("prompt", String())).strip_edges();
	if (prompt.is_empty()) {
		return _error("EMPTY_PROMPT", "Prompt is empty.");
	}

	active_provider = settings_service->resolve_active_provider();
	const String provider_id = active_provider.get("provider", String());
	const String model = active_provider.get("model", String());
	const String base_url = active_provider.get("base_url", String());
	const String api_key = active_provider.get("api_key", String());
	const Dictionary profile = provider_catalog->resolve(StringName(provider_id), base_url);
	const bool local = profile.get("local", false);

	const Dictionary model_limits = provider_catalog->resolve_model_limits(StringName(provider_id), model);
	const int resolved_context = (int)model_limits.get("context_window", profile.get("context_window", 128000));
	const int resolved_output = (int)model_limits.get("max_output_tokens", profile.get("max_output_tokens", 8192));
	context_window = (int)active_provider.get("context_window", resolved_context);
	max_output_tokens = (int)active_provider.get("max_tokens", resolved_output);
	if (context_window < 8192) {
		context_window = 8192;
	}
	if (max_output_tokens < 256) {
		max_output_tokens = 256;
	}

	const bool privacy_mode = active_provider.get("privacy_mode", true);
	if (privacy_mode && !local) {
		Dictionary e = _error("PRIVACY_BLOCKED", "Privacy mode allows local providers only (Ollama / LM Studio). Disable privacy mode in the AI settings to use remote providers.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}

	if (model.is_empty()) {
		Dictionary e = _error("NO_MODEL", "No model is configured. Set one in Solers BYOK settings (solers/ai/model).");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (String(profile.get("base_url", String())).strip_edges().is_empty()) {
		Dictionary e = _error("NO_BASE_URL", "No base URL configured for this provider.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}
	if (!local && api_key.is_empty()) {
		Dictionary e = _error("NO_API_KEY", "No API key configured. Set it in Solers BYOK settings.");
		emit_signal(SNAME("turn_failed"), e.get("error", Dictionary()));
		return e;
	}

	if (system_prompt.is_empty()) {
		system_prompt = _default_system_prompt();
	}
	messages.push_back(SolersLLMMessage::user(prompt));
	current_text = String();
	current_reasoning = String();
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	last_usage.clear();
	tool_iterations = 0;
	force_final_answer = false;
	retry_attempt = 0;
	retry_resume_msec = 0;
	turn_id++;
	_write_transcript_message("user", prompt);

	Dictionary turn_started;
	turn_started["turn_id"] = turn_id;
	turn_started["model"] = model;
	turn_started["provider"] = provider_id;
	turn_started["context_window"] = context_window;
	turn_started["max_output_tokens"] = max_output_tokens;
	_record("agent_turn_started", turn_started);

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
	if (retry_resume_msec != 0) {
		if (OS::get_singleton()->get_ticks_msec() < retry_resume_msec) {
			return;
		}
		retry_resume_msec = 0;
		_dispatch_model_request();
		return;
	}
	if (phase == PHASE_AWAITING_APPROVAL) {
		_poll_awaiting_approval();
		return;
	}
	if (phase == PHASE_TOOL_EXECUTING) {
		_poll_tool_executing();
		return;
	}
	if (phase == PHASE_TOOLS) {
		_poll_tool_queue();
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
		} else if (kind == SolersLLMEventKind::FINISH) {
			last_stop_reason = e.get("stop_reason", String());
		} else if (kind == SolersLLMEventKind::ERROR) {
		}
	}

	if (client->is_failed()) {
		const Dictionary error = client->get_error();
		if (SolersLLMRetry::is_retryable(error)) {
			retry_attempt++;
			const uint64_t wait = SolersLLMRetry::delay_msec(retry_attempt, error);
			retry_resume_msec = OS::get_singleton()->get_ticks_msec() + wait;
			current_text = String();
			current_reasoning = String();
			pending_tool_calls.clear();
			streamed_tool_calls.clear();
			Dictionary retry_payload;
			retry_payload["attempt"] = retry_attempt;
			retry_payload["delay_msec"] = (int)wait;
			retry_payload["code"] = error.get("code", String());
			_record("agent_turn_retrying", retry_payload);
			emit_signal(SNAME("turn_retrying"), retry_attempt, String(error.get("message", String())));
			return;
		}
		running = false;
		emit_signal(SNAME("turn_failed"), error);
		return;
	}
	if (client->is_done()) {
		_on_model_turn_complete();
	}
}

void SolersAgentSession::_on_model_turn_complete() {
	retry_attempt = 0;
	messages.push_back(SolersLLMMessage::assistant(current_text, pending_tool_calls));
	if (!current_text.is_empty() || !pending_tool_calls.is_empty()) {
		_write_transcript_message("assistant", current_text);
	}
	if (!current_text.is_empty()) {
		emit_signal(SNAME("assistant_message"), current_text);
	}

	if (pending_tool_calls.is_empty() || force_final_answer) {
		Dictionary data;
		data["text"] = current_text;
		data["reasoning"] = current_reasoning;
		data["stop_reason"] = last_stop_reason;
		const Dictionary scene_commit = _commit_dirty_scene_if_needed();
		if (!scene_commit.is_empty()) {
			data["scene_commit"] = scene_commit;
		}
		if (!last_usage.is_empty()) {
			data["usage"] = last_usage;
		}
		if (force_final_answer && !pending_tool_calls.is_empty()) {
			data["note"] = "tool_limit_reached";
		}
		running = false;
		force_final_answer = false;
		pending_tool_calls.clear();
		streamed_tool_calls.clear();
		current_text = String();
		current_reasoning = String();
		_record("agent_turn_completed", data);
		emit_signal(SNAME("turn_completed"), data);
		return;
	}

	tool_iterations++;
	if (tool_iterations >= max_tool_iterations) {
		force_final_answer = true;
	}

	tool_queue = pending_tool_calls.duplicate();
	tool_queue_index = 0;
	tool_started_announced = false;
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	current_text = String();
	current_reasoning = String();
	phase = PHASE_TOOLS;
	SOLERS_TRACE("session.tools", vformat("entering tool queue (%d call(s), iteration=%d)", tool_queue.size(), tool_iterations));
}

void SolersAgentSession::_poll_tool_queue() {
	if (tool_queue_index >= tool_queue.size()) {
		tool_queue.clear();
		tool_queue_index = 0;
		tool_started_announced = false;
		retry_attempt = 0;
		const Error err = _dispatch_model_request();
		if (err != OK) {
			current_reasoning = String();
		}
		return;
	}

	const Dictionary call = tool_queue[tool_queue_index];
	const String name = call.get("name", String());
	const String canonical_name = call.get("canonical_name", name);
	const String requested_name = call.get("requested_name", name);
	const String id = call.get("id", String());
	const String arguments = call.get("arguments", "{}");

	if (!tool_started_announced) {
		tool_started_announced = true;
		tool_started_msec = OS::get_singleton()->get_ticks_msec();
		if (!(bool)call.get("ui_announced", false)) {
			emit_signal(SNAME("tool_call_started"), id, canonical_name, arguments);
			return;
		}
	}

	Ref<JSON> json;
	json.instantiate();
	const Error parse_err = json->parse(arguments.is_empty() ? "{}" : arguments);
	const Variant parsed = parse_err == OK ? json->get_data() : Variant();
	if (canonical_name.is_empty()) {
		Dictionary error;
		error["code"] = "UNKNOWN_TOOL";
		error["message"] = vformat("Model requested an unknown Solers tool: %s.", requested_name);
		error["recoverable"] = true;
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		_write_transcript_tool(requested_name, Dictionary(), result);
		_deliver_tool_result(id, name, canonical_name, result);
		return;
	}
	if (parse_err != OK || parsed.get_type() != Variant::DICTIONARY) {
		Dictionary error;
		error["code"] = "INVALID_TOOL_ARGUMENTS";
		error["message"] = "Tool arguments must be a complete JSON object.";
		error["recoverable"] = true;
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		Dictionary args;
		args["raw_arguments"] = arguments;
		_write_transcript_tool(canonical_name, args, result);
		_deliver_tool_result(id, name, canonical_name, result);
		return;
	}

	_schedule_tool_execution(id, name, canonical_name, parsed, false);
}

void SolersAgentSession::_schedule_tool_execution(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, bool p_is_resume) {
	deferred_call_id = p_id;
	deferred_model_name = p_model_name;
	deferred_canonical_name = p_canonical_name;
	deferred_args = p_args;
	deferred_result = Dictionary();
	deferred_done = false;
	deferred_is_resume = p_is_resume;
	phase = PHASE_TOOL_EXECUTING;
	const uint64_t token = ++tool_exec_token;
	SOLERS_TRACE("session.schedule_tool", vformat("%s resume=%d token=%d (call_deferred, off _process stack)", p_canonical_name, (int)p_is_resume, (int)token));
	callable_mp(this, &SolersAgentSession::_execute_deferred_tool).call_deferred(token);
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
	SOLERS_TRACE("session.exec_tool", vformat("BEGIN %s (off _process stack)", deferred_canonical_name));
	deferred_result = tool_registry->call_tool(StringName(deferred_canonical_name), deferred_args);
	deferred_done = true;
	SOLERS_TRACE("session.exec_tool", vformat("END %s ok=%d", deferred_canonical_name, (int)(bool)deferred_result.get("ok", false)));
}

void SolersAgentSession::_poll_tool_executing() {
	if (!deferred_done) {
		return; // deferred call has not run yet this frame
	}

	Dictionary result = deferred_result;
	const String id = deferred_call_id;
	const String model_name = deferred_model_name;
	const String canonical_name = deferred_canonical_name;
	const Dictionary args = deferred_args;
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
		awaiting_approval_id = (int)error.get("approval_id", 0);
		phase = PHASE_AWAITING_APPROVAL;
		SOLERS_TRACE("session.await_approval", vformat("%s approval_id=%d", canonical_name, awaiting_approval_id));
		emit_signal(SNAME("tool_call_awaiting_approval"), id, canonical_name);
		return;
	}

	if (is_resume && _is_awaiting_approval_result(result)) {
		result = _tool_denied_result("APPROVAL_EXPIRED", "The approval expired before the tool could run. Ask again to retry.");
	}

	_write_transcript_tool(canonical_name, args, result);
	phase = PHASE_TOOLS;
	_deliver_tool_result(id, model_name, canonical_name, result);
}

bool SolersAgentSession::_is_awaiting_approval_result(const Dictionary &p_result) const {
	if ((bool)p_result.get("ok", false)) {
		return false;
	}
	const Dictionary error = p_result.get("error", Dictionary());
	return String(error.get("code", String())) == "USER_APPROVAL_REQUIRED";
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

void SolersAgentSession::_deliver_tool_result(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_result) {
	const uint64_t duration_msec = OS::get_singleton()->get_ticks_msec() - tool_started_msec;
	emit_signal(SNAME("tool_call_finished"), p_id, p_canonical_name, p_result, (int64_t)duration_msec);

	String content = JSON::stringify(p_result, "", false, true);
	if (context_window > 0 && content.utf8().length() > context_window) {
		const int original_len = content.length();
		content = content.left(context_window / 2);
		content += vformat("\n...[Solers: tool result truncated, %d of %d characters kept. Re-run the tool with tighter limits for the full data.]", content.length(), original_len);
	}
	messages.push_back(SolersLLMMessage::tool_result(p_id, p_model_name, content));

	tool_queue_index++;
	tool_started_announced = false;
}

void SolersAgentSession::_poll_awaiting_approval() {
	const String id = awaiting_call.get("id", String());
	const String model_name = awaiting_call.get("name", String());
	const String canonical_name = awaiting_call.get("canonical_name", model_name);

	const SolersPermissionManager::RequestDecision decision =
			permission_manager ? permission_manager->get_request_decision(awaiting_approval_id)
							   : SolersPermissionManager::DECISION_UNKNOWN;

	if (permission_manager && decision == SolersPermissionManager::DECISION_PENDING) {
		return; // still waiting on the user
	}

	if (permission_manager && decision == SolersPermissionManager::DECISION_APPROVED) {
		// Resume by re-issuing the same call through the deferred scheduler so it
		// runs off the _process stack (the original crash path executed the
		// approved tool synchronously inside poll).
		Dictionary args = awaiting_call.get("parsed_args", Dictionary());
		args["approval_id"] = awaiting_approval_id;
		const int approval_id = awaiting_approval_id;
		awaiting_call.clear();
		awaiting_approval_id = 0;
		SOLERS_TRACE("session.approval_granted", vformat("%s approval_id=%d -> deferred execute", canonical_name, approval_id));
		_schedule_tool_execution(id, model_name, canonical_name, args, true);
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

	_write_transcript_tool(canonical_name, awaiting_call.get("parsed_args", Dictionary()), result);
	awaiting_call.clear();
	awaiting_approval_id = 0;
	phase = PHASE_TOOLS;
	_deliver_tool_result(id, model_name, canonical_name, result);
}

void SolersAgentSession::abort() {
	if (client) {
		client->abort();
	}
	running = false;
	phase = PHASE_STREAMING;
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	tool_queue.clear();
	tool_queue_index = 0;
	tool_started_announced = false;
	awaiting_call.clear();
	awaiting_approval_id = 0;
	force_final_answer = false;
	retry_attempt = 0;
	retry_resume_msec = 0;
	tool_exec_token++;
	deferred_done = false;
	deferred_result = Dictionary();
	deferred_args = Dictionary();
	deferred_is_resume = false;
	deferred_call_id = String();
	deferred_model_name = String();
	deferred_canonical_name = String();
	current_text = String();
	current_reasoning = String();
}

void SolersAgentSession::reset_conversation() {
	abort();
	messages.clear();
	tool_iterations = 0;
	last_stop_reason = String();
	last_usage.clear();

	Dictionary event;
	event["role"] = "session_boundary";
	event["reason"] = "reset";
	_stamp_transcript_event(event);
	solers_transcript_write(event);

	session_id = _make_session_id();

	Dictionary start_event;
	start_event["role"] = "session_start";
	start_event["pid"] = OS::get_singleton()->get_process_id();
	start_event["wall"] = Time::get_singleton()->get_unix_time_from_system();
	start_event["unique_id"] = OS::get_singleton()->get_unique_id();
	_stamp_transcript_event(start_event);
	solers_transcript_write(start_event);
}

void SolersAgentSession::set_project_path(const String &p_project_path) {
	project_path = p_project_path;
}

void SolersAgentSession::set_session(const String &p_project_path, const String &p_session_id) {
	abort();
	project_path = p_project_path;
	if (!p_session_id.is_empty()) {
		session_id = p_session_id;
	}
	messages = _read_transcript_messages(project_path, session_id);
}

Array SolersAgentSession::get_messages() const {
	return messages.duplicate(true);
}

Dictionary SolersAgentSession::get_status() const {
	Dictionary status;
	status["running"] = running;
	status["turn_id"] = turn_id;
	status["message_count"] = messages.size();
	status["provider"] = active_provider.get("provider", String());
	status["model"] = active_provider.get("model", String());
	status["tool_iterations"] = tool_iterations;
	status["max_tool_iterations"] = max_tool_iterations;
	status["context_window"] = context_window;
	status["max_output_tokens"] = max_output_tokens;
	status["project_path"] = project_path;
	status["session_id"] = session_id;
	if (context_manager) {
		status["context_tokens"] = context_manager->get_last_estimated_tokens();
		status["prune_count"] = context_manager->get_prune_count();
		status["compaction_count"] = context_manager->get_compaction_count();
	}
	return status;
}

SolersAgentSession::SolersAgentSession() {
	session_id = _make_session_id();
	protocol_registry = memnew(SolersLLMProtocolRegistry);
	protocol_registry->register_builtin_protocols();
	provider_catalog = memnew(SolersLLMProviderCatalog);
	provider_catalog->register_builtin_profiles();
	client = memnew(SolersLLMClient);
	client->set_protocol_registry(protocol_registry);
	context_manager = memnew(SolersContextManager);
	models_dev = memnew(SolersModelsDev);
	models_dev->initialize();
	provider_catalog->set_models_dev(models_dev);

	Dictionary event;
	event["role"] = "session_start";
	event["pid"] = OS::get_singleton()->get_process_id();
	event["wall"] = Time::get_singleton()->get_unix_time_from_system();
	event["unique_id"] = OS::get_singleton()->get_unique_id();
	_stamp_transcript_event(event);
	solers_transcript_write(event);
}

SolersAgentSession::~SolersAgentSession() {
	abort();
	if (context_manager) {
		memdelete(context_manager);
		context_manager = nullptr;
	}
	if (client) {
		memdelete(client);
		client = nullptr;
	}
	if (provider_catalog) {
		memdelete(provider_catalog);
		provider_catalog = nullptr;
	}
	if (models_dev) {
		memdelete(models_dev);
		models_dev = nullptr;
	}
	if (protocol_registry) {
		memdelete(protocol_registry);
		protocol_registry = nullptr;
	}
}
