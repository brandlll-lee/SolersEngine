/**************************************************************************/
/*  solers_agent_session.h                                                */
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

#pragma once

#include "core/object/object.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersToolRegistry;
class SolersActionTimeline;
class SolersSettingsService;
class SolersPermissionManager;
class SolersContextManager;
class SolersModelsDev;
class SolersLLMProtocolRegistry;
class SolersLLMProviderCatalog;
class SolersLLMClient;

class SolersAgentSession : public Object {
	GDCLASS(SolersAgentSession, Object);

	enum Phase {
		PHASE_STREAMING,
		PHASE_TOOLS,
		PHASE_AWAITING_APPROVAL,
		PHASE_TOOL_EXECUTING,
	};

	SolersToolRegistry *tool_registry = nullptr;
	SolersSettingsService *settings_service = nullptr;
	SolersActionTimeline *action_timeline = nullptr;
	SolersPermissionManager *permission_manager = nullptr;

	SolersLLMProtocolRegistry *protocol_registry = nullptr; // owned
	SolersLLMProviderCatalog *provider_catalog = nullptr; // owned
	SolersLLMClient *client = nullptr; // owned
	SolersContextManager *context_manager = nullptr; // owned
	SolersModelsDev *models_dev = nullptr; // owned; data-driven model registry

	int context_window = 128000;
	int max_output_tokens = 8192;

	Array messages; // canonical conversation history
	String system_prompt;
	String current_text; // assistant text accumulated this model turn
	String current_reasoning; // reasoning/thinking text accumulated this model turn
	Array pending_tool_calls; // tool calls collected this model turn
	Dictionary streamed_tool_calls; // call id -> surfaced tool call state for this model step
	String last_stop_reason;
	Dictionary last_usage;
	Dictionary active_provider; // { provider, model, base_url, api_key }
	bool running = false;
	Phase phase = PHASE_STREAMING;
	Array tool_queue; // calls queued for paced execution this step
	int tool_queue_index = 0;
	bool tool_started_announced = false;
	uint64_t tool_started_msec = 0;
	String deferred_call_id;
	String deferred_model_name;
	String deferred_canonical_name;
	Dictionary deferred_args;
	Dictionary deferred_result;
	bool deferred_done = false;
	bool deferred_is_resume = false;
	uint64_t tool_exec_token = 0;
	Dictionary awaiting_call; // tool call parked on the approval gate
	int awaiting_approval_id = 0; // pending request id we are waiting on
	int turn_id = 0;
	int tool_iterations = 0;
	int retry_attempt = 0;
	uint64_t retry_resume_msec = 0;
	int text_delta_count = 0;
	uint64_t last_text_delta_msec = 0;
	int max_tool_iterations = 12;
	bool force_final_answer = false;

	String _default_system_prompt() const;
	Array _collect_tools() const;
	Dictionary _build_request() const;
	Dictionary _redacted_request_graph(const Dictionary &p_request, const Dictionary &p_profile) const;
	Error _dispatch_model_request();
	Dictionary _tool_call_from_event(const Dictionary &p_event) const;
	Dictionary _merge_streamed_tool_call(const Dictionary &p_call);
	Dictionary _surface_tool_call(const Dictionary &p_call);
	void _on_model_turn_complete();
	void _poll_tool_queue();
	void _poll_awaiting_approval();
	void _poll_tool_executing();
	void _schedule_tool_execution(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, bool p_is_resume);
	void _execute_deferred_tool(uint64_t p_token);
	void _deliver_tool_result(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_result);
	bool _is_awaiting_approval_result(const Dictionary &p_result) const;
	Dictionary _commit_dirty_scene_if_needed();
	void _write_transcript_message(const String &p_role, const String &p_content) const;
	void _write_transcript_tool(const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result) const;
	Dictionary _tool_denied_result(const String &p_code, const String &p_message) const;
	void _record(const String &p_event, const Dictionary &p_payload) const;
	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message) const;

protected:
	static void _bind_methods();

public:
	void set_tool_registry(SolersToolRegistry *p_tool_registry) { tool_registry = p_tool_registry; }
	void set_settings_service(SolersSettingsService *p_settings_service) { settings_service = p_settings_service; }
	void set_action_timeline(SolersActionTimeline *p_action_timeline) { action_timeline = p_action_timeline; }
	void set_permission_manager(SolersPermissionManager *p_permission_manager) { permission_manager = p_permission_manager; }

	Dictionary start_turn(const Dictionary &p_args); // { prompt: String }
	void poll();
	void abort();
	void reset_conversation();
	Dictionary get_status() const;
	bool is_running() const { return running; }
	bool is_executing_tool() const { return running && phase == PHASE_TOOL_EXECUTING; }

	SolersAgentSession();
	~SolersAgentSession();
};
