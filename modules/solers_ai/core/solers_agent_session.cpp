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
#include "core/os/os.h"
#include "core/os/time.h"
#include "scene/main/node.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_project_observation.h"
#include "modules/solers_ai/core/solers_runtime_observation.h"
#include "modules/solers_ai/core/solers_scene_observation.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_executor.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_retry.h"
#include "modules/solers_ai/llm/solers_models_dev.h"

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
	_write_transcript_event(p_event, p_payload);
}

void SolersAgentSession::set_tool_registry(SolersToolRegistry *p_tool_registry) {
	tool_registry = p_tool_registry;
	if (tool_executor && tool_executor->is_idle()) {
		tool_executor->configure(tool_registry, permission_manager);
	}
}

void SolersAgentSession::set_permission_manager(SolersPermissionManager *p_permission_manager) {
	permission_manager = p_permission_manager;
	if (tool_executor && tool_executor->is_idle()) {
		tool_executor->configure(tool_registry, permission_manager);
	}
}

void SolersAgentSession::set_observations(SolersProjectObservation *p_project, SolersSceneObservation *p_scene, SolersRuntimeObservation *p_runtime) {
	project_observation = p_project;
	scene_observation = p_scene;
	runtime_observation = p_runtime;
}

void SolersAgentSession::set_models_dev(SolersModelsDev *p_models_dev, bool p_owned) {
	if (owns_models_dev && models_dev && models_dev != p_models_dev) {
		memdelete(models_dev);
	}
	models_dev = p_models_dev;
	owns_models_dev = p_owned && p_models_dev != nullptr;
}

static Array _solers_enrich_mentions(const Array &p_mentions, SolersProjectObservation *p_observation) {
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
	const Array enriched_mentions = _solers_enrich_mentions(mentions, project_observation);
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
			"- Godot live editor and runtime state are authoritative. Combine static project evidence with native facts when the task crosses both.\n"
			"- Choose the smallest available tool that advances the task. Search, read, inspect, capture, edit, and validate are independent operations; no observation unlocks an action.\n"
			"- The live Registry is the complete current tool surface. Read built-in skills through read(skill://name); skills provide domain knowledge and never grant capabilities.\n"
			"- Tool results are bounded pages with native identities, cursors, hashes, epochs, receipts, and errors. Continue from those facts when more detail is needed.\n"
			"- Mutations request permission from their concrete paths and side effects at execution time. Supply current native receipts; never invent state or a separate save step.\n"
			"- Prefer the smallest coherent native change. Use ClassDB metadata and native documentation for unfamiliar engine types.\n"
			"- Tool errors and Godot diagnostics are authoritative. Change the cause before retrying; never repeat an identical failed call.\n"
			"- For project-scale tasks, map relevant files, live objects, runtime state, and pixels as needed; "
			"choose the next observation from the evidence.\n"
			"- Before changing state, form a falsifiable hypothesis and compare native evidence after the change; "
			"do not declare success from intent alone.\n"
			"- Tool calls execute strictly in model-returned order. Text without tool calls ends the turn; finish with a clear summary.";
	if (tool_registry) {
		const String skill_catalog = tool_registry->get_skill_catalog_prompt();
		if (!skill_catalog.is_empty()) {
			prompt += "\n\n" + skill_catalog;
		}
	}
	return prompt;
}

Dictionary SolersAgentSession::_environment_context_message() {
	if (!project_observation || !scene_observation || !runtime_observation) {
		return Dictionary();
	}
	Dictionary context;
	context["project"] = project_observation->get_project_info();
	const Dictionary runtime_status = runtime_observation->get_runtime_status();
	context["runtime"] = runtime_status;
	context.merge(scene_observation->get_editor_state(), true);

	Dictionary observe_args;
	observe_args["target"] = "events";
	observe_args["since_cursor"] = (int64_t)runtime_observation_cursor;
	observe_args["include_events"] = true;
	const Dictionary runtime_observations = runtime_observation->observe_runtime(observe_args);
	runtime_observation_cursor = (int64_t)runtime_observations.get("cursor", runtime_observation_cursor);
	const Dictionary error_digest = runtime_observations.get("error_digest", Dictionary());
	if (!error_digest.is_empty()) {
		context["runtime_error_digest"] = error_digest;
		context["runtime_epoch_error_count"] = runtime_observations.get("epoch_error_count", 0);
	}
	const Array runtime_events = runtime_observations.get("events", Array());
	if (!runtime_events.is_empty()) {
		context["runtime_events"] = runtime_events;
		context["runtime_events_truncated"] = runtime_observations.get("truncated", false);
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
	for (const Variant &value : tool_registry->list_tools()) {
		const Dictionary definition = value;
		Dictionary tool;
		tool["name"] = definition.get("model_name", definition.get("name", String()));
		tool["canonical_name"] = definition.get("name", String());
		tool["description"] = definition.get("description", String());
		tool["parameters"] = definition.get("input_schema", Dictionary());
		out.push_back(tool);
	}
	cached_request_tool_tokens = SolersContextManager::estimate_tokens(JSON::stringify(out, "", false, true));
	return out;
}

bool SolersAgentSession::_refresh_active_model_limits() {
	if (!settings_service || !models_dev) {
		return false;
	}
	const String provider_id = active_provider.get("provider", String());
	const String model_id = active_provider.get("model", String());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, active_provider.get("base_url", String()), model_id);
	const StringName catalog_provider = StringName(profile.get("catalog_provider", provider_id));
	const Dictionary model = models_dev->get_model(catalog_provider, model_id);

	const bool explicit_context = active_provider.has("context_window");
	const bool explicit_output = active_provider.has("max_tokens");
	int resolved_context = (int)profile.get("context_window", 0);
	if ((int)model.get("context", 0) > 0) {
		resolved_context = model.get("context", 0);
	}
	if (explicit_context) {
		resolved_context = (int)active_provider["context_window"];
	}
	int resolved_output = SolersContextManager::DEFAULT_OUTPUT_TOKENS;
	resolved_output = (int)profile.get("max_output_tokens", resolved_output);
	if ((int)model.get("output", 0) > 0) {
		resolved_output = model.get("output", 0);
	}
	if (explicit_output) {
		resolved_output = (int)active_provider["max_tokens"];
	}

	const int previous_context = context_window;
	const int previous_output = max_output_tokens;
	context_window = resolved_context > 0 ? resolved_context : 0;
	// Keep catalog output from consuming the usable input budget.
	static constexpr int SOLERS_OUTPUT_TOKEN_MAX = 32000;
	if (resolved_output <= 0) {
		resolved_output = SolersContextManager::DEFAULT_OUTPUT_TOKENS;
	}
	resolved_output = MIN(resolved_output, SOLERS_OUTPUT_TOKEN_MAX);
	max_output_tokens = resolved_output;
	return context_window != previous_context || max_output_tokens != previous_output;
}

int SolersAgentSession::_active_model_input_support(const String &p_modality) const {
	if (!settings_service || !models_dev) {
		return -1;
	}
	const String provider_id = active_provider.get("provider", String());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, active_provider.get("base_url", String()), active_provider.get("model", String()));
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
		const Dictionary active_profile = settings_service ? settings_service->resolve_provider_profile(provider_id, active_provider.get("base_url", String()), active_provider.get("model", String())) : Dictionary();
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

Error SolersAgentSession::_dispatch_model_request() {
	const Dictionary availability_error = _provider_dispatch_error();
	if (!availability_error.is_empty()) {
		const Dictionary error = availability_error.get("error", Dictionary());
		_finish_turn("failed", error.get("message", "The selected provider is unavailable."), error);
		return ERR_UNAVAILABLE;
	}
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
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url, active_provider.get("model", String()));
	if (system_prompt.is_empty()) {
		system_prompt = _default_system_prompt();
	}
	const Array tools = _collect_tools();
	Array request_messages = context_manager ? context_manager->project_tool_evidence(messages) : messages.duplicate(true);
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
	if (context_manager && context_manager->should_compact(request_messages, system_prompt, cached_request_tool_tokens, context_window, max_output_tokens, request_transient_tokens)) {
		if (phase == PHASE_COMPACTING) {
			const Dictionary error = _error("COMPACTION_TARGET_NOT_REACHED", "The accepted context projection no longer fits the current engine state.").get("error", Dictionary());
			_finish_turn("failed", error.get("message", String()), error);
			return ERR_OUT_OF_MEMORY;
		}
		return _begin_compaction(false);
	}
	Dictionary request = _build_request(request_messages, system_prompt, tools);
	phase = PHASE_STREAMING;
	streamed_tool_calls.clear();
	return _begin_provider_request(request, profile);
}

Error SolersAgentSession::_begin_provider_request(const Dictionary &p_request, const Dictionary &p_profile) {
	retry_request = p_request.duplicate(true);
	retry_profile = p_profile.duplicate(true);
	current_provider_metadata.clear();
	model_request_index++;
	const Dictionary auth = active_provider.get("auth", Dictionary());
	const String provider = active_provider.get("provider", String());
	const Error err = client->begin(retry_request, retry_profile, auth, settings_service->get_auth_method(provider, retry_profile, auth));
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
	payload["tokens_before"] = context_manager->get_token_count_with_pending(messages, system_prompt, cached_request_tool_tokens, request_transient_tokens);
	compaction_id = ++transcript_event_sequence;
	compaction_timeline_event_id = _write_transcript_compaction("started", payload);
	emit_signal(SNAME("timeline_entry_committed"), compaction_timeline_event_id, "context_compaction");
	return _dispatch_compaction_request();
}

void SolersAgentSession::_record_request_usage(const Dictionary &p_usage) {
	last_usage = p_usage;
	last_usage["context_window"] = context_window;
	last_usage["provider"] = active_provider.get("provider", String());
	last_usage["model"] = active_provider.get("model", String());
	const Array request_messages = retry_request.get("messages", Array());
	int media_reference_count = 0;
	for (const Variant &message_variant : request_messages) {
		media_reference_count += Array(Dictionary(message_variant).get("attachments", Array())).size();
	}
	last_usage["message_count"] = request_messages.size();
	last_usage["media_reference_count"] = media_reference_count;
	last_request_usage = last_usage.duplicate(true);
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
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url, active_provider.get("model", String()));

	const String compaction_system_prompt = R"(Update one continuation for another agent using exactly these headings:
## Goal
## Constraints & Preferences
## Progress
### Done
### In Progress
### Blocked
## Key Decisions
## Next Steps
## Critical Context
Current user instructions override conflicting history. Preserve exact paths, API names, errors, and attachment ids. The structured Godot checkpoint is authoritative: never invent engine state or rewrite hashes, ObjectIDs, revisions, or receipts. Return text only.)";
	Array history;
	const Array projection = SolersContextManager::project_completed_turns(compaction_source_messages);
	for (int i = 0; i < projection.size(); i++) {
		const Dictionary message = projection[i];
		const String origin = message.get("origin", String());
		const String role = message.get("role", String());
		if (origin == "turn_checkpoint" || origin == "compaction_summary") {
			history.push_back(SolersLLMMessage::user((origin == "turn_checkpoint" ? "Authoritative Godot checkpoint:\n" : "Previous continuation:\n") + String(message.get("content", String()))));
		} else if (role == SolersLLMRole::USER) {
			String content = SolersMention::strip_prompt_block(message.get("content", String()));
			PackedStringArray attachment_ids;
			for (const Variant &attachment : Array(message.get("attachments", Array()))) {
				attachment_ids.push_back(SolersLLMMessage::attachment_identity(attachment));
			}
			if (!attachment_ids.is_empty()) {
				content += "\n[Attachment ids: " + String(", ").join(attachment_ids) + "]";
			}
			history.push_back(SolersLLMMessage::user(content));
		} else if (role == SolersLLMRole::ASSISTANT && !String(message.get("content", String())).strip_edges().is_empty()) {
			history.push_back(SolersLLMMessage::assistant(message.get("content", String()), Array()));
		}
	}
	ERR_FAIL_COND_V(history.is_empty(), ERR_INVALID_DATA);
	const int intent_tokens = SolersContextManager::estimate_messages_tokens(history);
	history.push_back(SolersLLMMessage::user("Update the structured continuation from this history. Do not call tools."));
	Dictionary request = _build_request(history, compaction_system_prompt, Array());
	const int continuation_budget = MAX(1, MIN(intent_tokens, context_window > 0 ? context_window - max_output_tokens - SolersContextManager::estimate_tokens(compaction_system_prompt) : max_output_tokens));
	if (request.has("max_tokens")) {
		request["max_tokens"] = MIN((int)request["max_tokens"], continuation_budget);
	}
	phase = PHASE_COMPACTING;
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
			_record_request_usage(event);
			turn_fresh_input_tokens += MAX(0, (int)event.get("input_tokens", 0));
			turn_cache_read_tokens += MAX(0, (int)event.get("cache_read_tokens", 0));
			turn_cache_write_tokens += MAX(0, (int)event.get("cache_write_tokens", 0));
			turn_output_tokens += MAX(0, (int)event.get("output_tokens", 0));
			turn_reasoning_tokens += MAX(0, (int)event.get("reasoning_tokens", 0));
		} else if (kind == SolersLLMEventKind::FINISH) {
			last_stop_reason = event.get("stop_reason", String());
		}
	}

	if (client->is_failed()) {
		turn_wire_body_bytes += client->get_request_body_bytes();
		Dictionary error = client->get_error();
		if (String(error.get("message", String())).strip_edges().is_empty()) {
			error = _error("COMPACTION_FAILED", "The context compaction request failed without provider error details.").get("error", Dictionary());
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
	if (current_text.strip_edges().is_empty()) {
		const Dictionary error = _error("COMPACTION_FAILED", "The model did not produce a usable context summary.").get("error", Dictionary());
		_finish_turn("failed", String(error.get("message", "Context compaction failed.")), error);
		return;
	}
	turn_wire_body_bytes += client->get_request_body_bytes();
	_on_compaction_complete();
}

void SolersAgentSession::_on_compaction_complete() {
	const int projection_budget = context_window > 0 ? context_window - max_output_tokens - SolersContextManager::estimate_tokens(system_prompt) - cached_request_tool_tokens - request_transient_tokens - SolersContextManager::TOOL_RESULT_MAX_TOKENS : 0;
	if (context_window > 0 && projection_budget <= 0) {
		const Dictionary error = _error("MODEL_CONTEXT_BUDGET_INVALID", "The model context cannot fit the system prompt, tools, output, engine state, and one observation.").get("error", Dictionary());
		_finish_turn("failed", error.get("message", String()), error);
		return;
	}
	const Dictionary result = context_manager->apply_compaction(compaction_source_messages, current_text, projection_budget);
	if (!(bool)result.get("accepted", false)) {
		Dictionary error = _error("COMPACTION_TARGET_NOT_REACHED", "Context compaction did not produce a smaller projection within the request budget.").get("error", Dictionary());
		error["tokens_before"] = result.get("tokens_before", 0);
		error["tokens_after"] = result.get("tokens_after", 0);
		error["target_tokens"] = result.get("target_tokens", 0);
		_finish_turn("failed", error.get("message", String()), error);
		return;
	}
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
	_dispatch_model_request();
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

	const String canonical_name = call.get("canonical_name", String());
	const bool already_announced = call.get("ui_announced", false);
	call["ui_announced"] = true;
	streamed_tool_calls[id] = call;
	if (!already_announced) {
		const String arguments = call.get("arguments", String());
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
	active_provider = settings_service->resolve_active_provider();
	const String provider_id = active_provider.get("provider", String());
	const String model = active_provider.get("model", String());
	const String base_url = active_provider.get("base_url", String());
	const Dictionary auth = active_provider.get("auth", Dictionary());
	const Dictionary profile = settings_service->resolve_provider_profile(provider_id, base_url, model);
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
	turn_mentions = _solers_enrich_mentions(turn_mentions, project_observation);
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
	if (context_manager) {
		context_manager->reset();
	}
	messages.push_back(user_message);
	current_text = String();
	current_reasoning = String();
	current_provider_metadata.clear();
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
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
	turn_reasoning_tokens = 0;
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
	if (phase == PHASE_TOOLS) {
		_poll_tool_queue();
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
			_merge_streamed_tool_call(_tool_call_from_event(e));
		} else if (kind == SolersLLMEventKind::TOOL_CALL) {
			Dictionary call = _surface_tool_call(_tool_call_from_event(e));
			pending_tool_calls.push_back(call);
		} else if (kind == SolersLLMEventKind::USAGE) {
			_record_request_usage(e);
			turn_output_tokens += MAX(0, (int)e.get("output_tokens", 0));
			turn_reasoning_tokens += MAX(0, (int)e.get("reasoning_tokens", 0));
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
	retry_attempt = 0;
	Dictionary response_event;
	response_event["request_index"] = model_request_index;
	response_event["stop_reason"] = last_stop_reason;
	response_event["tool_call_count"] = pending_tool_calls.size();
	response_event["text_bytes"] = current_text.utf8().length();
	response_event["wire_body_bytes"] = wire_body_bytes;
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

	tool_queue = pending_tool_calls.duplicate(true);
	const bool response_truncated = last_stop_reason == SolersLLMStopReason::MAX_TOKENS;
	const uint64_t queued_msec = OS::get_singleton()->get_ticks_msec();
	for (int i = 0; i < tool_queue.size(); i++) {
		Dictionary call = tool_queue[i];
		call["queued_msec"] = (int64_t)queued_msec;
		Dictionary parsed_args;
		if (response_truncated) {
			call["preflight_result"] = _tool_denied_result("TOOL_CALL_TRUNCATED", "Tool call was not executed because the model response reached its output token limit. Re-issue the call with complete arguments.");
		} else {
			Ref<JSON> json;
			json.instantiate();
			const String arguments = call.get("arguments", "{}");
			if (json->parse(arguments.is_empty() ? "{}" : arguments) == OK && json->get_data().get_type() == Variant::DICTIONARY) {
				parsed_args = json->get_data();
			} else {
				call["preflight_result"] = _tool_denied_result("TOOL_ARGUMENT_INVALID", "Tool arguments must be a complete JSON object.");
			}
		}
		call["parsed_args"] = parsed_args;
		tool_queue[i] = call;
	}
	tool_queue_index = 0;
	active_tool_call_id = String();
	active_tool_model_name = String();
	active_tool_canonical_name = String();
	active_tool_args.clear();
	approval_announced = false;
	pending_tool_calls.clear();
	streamed_tool_calls.clear();
	current_text = String();
	current_reasoning = String();
	phase = PHASE_TOOLS;
	SOLERS_TRACE("session.tools", vformat("entering serial tool queue (%d call(s))", tool_queue.size()));
}

void SolersAgentSession::_finish_turn(const String &p_outcome, const String &p_message, const Dictionary &p_error) {
	if (compaction_id > 0) {
		_write_transcript_compaction(p_outcome == "aborted" ? "cancelled" : "failed", p_error);
		emit_signal(SNAME("timeline_entry_committed"), compaction_timeline_event_id, "context_compaction");
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
	Dictionary usage;
	usage["model_requests"] = model_request_index;
	usage["fresh_input_tokens"] = turn_fresh_input_tokens;
	usage["cache_read_tokens"] = turn_cache_read_tokens;
	usage["cache_write_tokens"] = turn_cache_write_tokens;
	usage["output_tokens"] = turn_output_tokens;
	usage["reasoning_tokens"] = turn_reasoning_tokens;
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
	tool_queue_index = 0;
	tool_queued_msec = 0;
	tool_started_msec = 0;
	tool_completed_msec = 0;
	approval_announced = false;
	retry_attempt = 0;
	retry_resume_msec = 0;
	retry_request.clear();
	retry_profile.clear();
	compaction_source_messages.clear();
	compaction_id = 0;
	compaction_timeline_event_id = 0;
	overflow_compaction_attempts = 0;
	last_progress_call_id = String();
	last_progress_msec = 0;
	active_tool_call_id = String();
	active_tool_model_name = String();
	active_tool_canonical_name = String();
	active_tool_args.clear();
	current_text = String();
	current_reasoning = String();
	turn_attachments.clear();
	turn_mentions.clear();
	if (outcome == "failed") {
		_record("agent_turn_failed", data);
		emit_signal(SNAME("turn_failed"), error);
	} else {
		_record("agent_turn_completed", data);
		emit_signal(SNAME("turn_completed"), data);
	}
}

void SolersAgentSession::abort() {
	if (!running) {
		return;
	}
	if (client) {
		client->abort();
	}
	if (tool_executor && tool_executor->is_active()) {
		tool_executor->cancel();
	}
	_finish_turn("aborted", "Turn aborted.");
}

void SolersAgentSession::shutdown() {
	abort();
	_release_godot_log_audit();
}

void SolersAgentSession::_reset_session_derived_state() {
	runtime_observation_cursor = 0;
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
	pending_steering_messages.clear();
	last_outcome = String();
	last_stop_reason = String();
	last_usage.clear();
	last_request_usage.clear();
	if (context_manager) {
		context_manager->reset();
	}
	session_id = _make_session_id();
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
	project_path = p_project_path;
	_reset_session_derived_state();
	if (!p_session_id.is_empty()) {
		session_id = p_session_id;
	}
	transcript_event_sequence = 0;
	const Dictionary state = _read_transcript_state(project_path, session_id);
	messages = state.get("messages", Array());
	timeline_entries = state.get("timeline_entries", Array());
	open_timeline_tools.clear();
	last_request_usage = state.get("last_request_usage", Dictionary());
	last_outcome = state.get("outcome", String());
	turn_id = state.get("turn_id", 0);
	if (context_manager) {
		context_manager->reset();
		context_manager->set_compaction_count(state.get("compaction_count", 0));
	}
	if (!project_path.is_empty()) {
		_ensure_godot_log_audit(false);
	}
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
	status["reasoning_tokens"] = turn_reasoning_tokens;
	status["wire_body_bytes"] = turn_wire_body_bytes;
	status["context_window"] = context_window;
	status["max_output_tokens"] = max_output_tokens;
	status["model_limits_known"] = context_window > 0;
	Dictionary usage = last_request_usage.duplicate(true);
	if (!usage.is_empty()) {
		usage["used_tokens"] = MAX(0, (int64_t)usage.get("input_tokens", 0)) + MAX(0, (int64_t)usage.get("output_tokens", 0)) + MAX(0, (int64_t)usage.get("reasoning_tokens", 0)) + MAX(0, (int64_t)usage.get("cache_read_tokens", 0)) + MAX(0, (int64_t)usage.get("cache_write_tokens", 0));
	}
	status["last_request_usage"] = usage;
	status["project_path"] = project_path;
	status["session_id"] = session_id;
	status["compacting"] = phase == PHASE_COMPACTING;
	status["last_outcome"] = last_outcome;
	{
		MutexLock lock(godot_log_mutex);
		status["godot_log_errors"] = godot_log_error_count;
		status["godot_log_warnings"] = godot_log_warning_count;
	}
	status["runtime_epoch"] = runtime_observation ? runtime_observation->get_runtime_status().get("runtime_epoch", 0) : Variant((int64_t)0);
	if (context_manager) {
		status["context_tokens"] = context_manager->get_last_estimated_tokens();
		status["compaction_count"] = context_manager->get_compaction_count();
	}
	return status;
}

bool SolersAgentSession::is_executing_tool() const {
	return running && phase == PHASE_TOOLS && tool_executor && tool_executor->is_active();
}

SolersAgentSession::SolersAgentSession() {
	session_id = _make_session_id();
	tool_executor = memnew(SolersToolExecutor);
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
	if (tool_executor) {
		memdelete(tool_executor);
		tool_executor = nullptr;
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
