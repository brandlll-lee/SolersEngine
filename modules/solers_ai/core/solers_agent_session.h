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

#include "core/error/error_macros.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersContextManager;
class SolersLLMClient;
class SolersLLMProtocolRegistry;
class SolersModelCatalog;
class SolersPermissionManager;
class SolersProjectObservation;
class SolersRuntimeObservation;
class SolersSceneObservation;
class SolersSettingsService;
class SolersToolExecutor;
class SolersToolRegistry;

class SolersAgentSession : public Object {
	GDCLASS(SolersAgentSession, Object);

	enum Phase {
		PHASE_STREAMING,
		PHASE_COMPACTING,
		PHASE_TOOLS,
	};

	static constexpr int MAX_LLM_RETRY_ATTEMPTS = 3;
	static constexpr uint64_t TOOL_EXECUTION_TIMEOUT_MSEC = 600000;

	SolersToolRegistry *tool_registry = nullptr;
	SolersSettingsService *settings_service = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	SolersProjectObservation *project_observation = nullptr;
	SolersRuntimeObservation *runtime_observation = nullptr;
	SolersSceneObservation *scene_observation = nullptr;
	SolersLLMProtocolRegistry *protocol_registry = nullptr;
	SolersLLMClient *client = nullptr;
	SolersContextManager *context_manager = nullptr;
	SolersModelCatalog *model_catalog = nullptr;
	bool owns_model_catalog = true;

	int context_window = 0;
	int max_output_tokens = 8192;
	int cached_request_tool_tokens = 0;
	Array messages;
	mutable Array timeline_entries;
	mutable HashMap<String, Dictionary> open_timeline_tools;
	mutable int64_t transcript_event_sequence = 0;
	String system_prompt;
	String current_text;
	String current_reasoning;
	Dictionary current_provider_metadata;
	Array pending_tool_calls;
	Dictionary streamed_tool_calls;
	String last_stop_reason;
	Dictionary last_usage;
	Dictionary session_usage;
	Dictionary context_usage;
	Dictionary latest_compaction;
	int context_covered_message_count = 0;
	String last_outcome;
	Dictionary active_provider;
	bool running = false;
	Phase phase = PHASE_STREAMING;

	Array tool_queue;
	int tool_queue_index = 0;
	String active_tool_call_id;
	String active_tool_model_name;
	String active_tool_canonical_name;
	Dictionary active_tool_args;
	SolersToolExecutor *tool_executor = nullptr;
	uint64_t tool_queued_msec = 0;
	uint64_t tool_started_msec = 0;
	uint64_t tool_completed_msec = 0;
	bool approval_announced = false;
	String last_progress_call_id;
	uint64_t last_progress_msec = 0;

	int turn_id = 0;
	int retry_attempt = 0;
	int model_request_index = 0;
	int64_t turn_fresh_input_tokens = 0;
	int64_t turn_cache_read_tokens = 0;
	int64_t turn_cache_write_tokens = 0;
	int64_t turn_output_tokens = 0;
	int64_t turn_reasoning_tokens = 0;
	int64_t turn_wire_body_bytes = 0;
	int request_transient_tokens = 0;
	uint64_t retry_resume_msec = 0;
	int text_delta_count = 0;
	uint64_t last_text_delta_msec = 0;
	Array compaction_source_messages;
	int64_t compaction_id = 0;
	int64_t compaction_timeline_event_id = 0;
	int64_t compaction_first_kept_event_id = 0;
	int compaction_tokens_before = 0;
	Dictionary retry_request;
	Dictionary retry_profile;
	int overflow_compaction_attempts = 0;
	String project_path;
	String session_id;
	Array turn_attachments;
	Array turn_mentions;
	Array pending_steering_messages;

	bool godot_log_audit_installed = false;
	ErrorHandlerList godot_error_handler;
	bool godot_log_turn_active = false;
	mutable Mutex godot_log_mutex;
	int godot_log_error_count = 0;
	int godot_log_warning_count = 0;
	uint64_t runtime_observation_cursor = 0;
	Array pending_godot_diagnostics;
	HashMap<String, int> pending_godot_diagnostic_index;
	int pending_godot_diagnostics_overflow = 0;

	void _reset_session_derived_state();
	String _default_system_prompt() const;
	Dictionary _environment_context_message();
	String _make_session_id() const;
	Dictionary _read_transcript_state(const String &p_project_path, const String &p_session_id) const;
	void _stamp_transcript_event(Dictionary &r_event) const;
	int64_t _write_transcript_event(const String &p_type, const Dictionary &p_payload = Dictionary()) const;
	Error _write_transcript_event_durable(const String &p_type, const Dictionary &p_payload, int64_t &r_event_id) const;
	void _ensure_godot_log_audit(bool p_turn_active);
	void _release_godot_log_audit();
	static void _godot_error_callback(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type);
	void _on_godot_error(const String &p_message, ErrorHandlerType p_type, int64_t p_source_thread, const String &p_function, const String &p_file, int p_line);
	Dictionary _take_godot_diagnostics();

	Array _collect_tools();
	bool _refresh_active_model_limits();
	int _active_model_input_support(const String &p_modality) const;
	Dictionary _build_request(const Array &p_messages, const String &p_request_system_prompt, const Array &p_tools) const;
	Dictionary _provider_dispatch_error() const;
	Error _begin_provider_request(const Dictionary &p_request, const Dictionary &p_profile);
	void _record_request_usage(const Dictionary &p_usage);
	double _usage_cost(const Dictionary &p_usage) const;
	void _accumulate_usage(const Dictionary &p_usage);
	Error _dispatch_model_request();
	Error _dispatch_compaction_request();
	Error _begin_compaction(bool p_from_overflow);
	void _poll_compaction();
	void _on_compaction_complete();
	bool _is_context_overflow(const Dictionary &p_error) const;
	bool _schedule_llm_retry(const Dictionary &p_error);
	Dictionary _tool_call_from_event(const Dictionary &p_event) const;
	Dictionary _merge_streamed_tool_call(const Dictionary &p_call);
	Dictionary _surface_tool_call(const Dictionary &p_call);
	Array _attachments_for_ids(const Array &p_ids) const;
	void _on_model_turn_complete();
	void _finish_turn(const String &p_outcome, const String &p_message, const Dictionary &p_error = Dictionary());

	void _poll_tool_queue();
	void _poll_tool_executing();
	void _start_tool_execution(const Dictionary &p_call, const Dictionary &p_args);
	void _finish_active_tool(const Dictionary &p_result);
	void _cancel_undelivered_tools();
	void _deliver_tool_result(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, uint64_t p_started_msec);
	Dictionary _tool_denied_result(const String &p_code, const String &p_message) const;
	bool _flush_pending_steering();

	int64_t _write_transcript_message(const String &p_role, const String &p_content, const Array &p_mentions = Array(), const Array &p_tool_calls = Array(), const String &p_reasoning = String(), const Array &p_attachments = Array(), int64_t p_event_id = 0) const;
	int64_t _write_transcript_tool(const String &p_call_id, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, const String &p_delivered_content, const Array &p_added_tool_names = Array()) const;
	int64_t _write_transcript_compaction(const String &p_phase, const Dictionary &p_payload) const;
	void _record(const String &p_event, const Dictionary &p_payload) const;
	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message) const;

protected:
	static void _bind_methods();

public:
	void set_tool_registry(SolersToolRegistry *p_tool_registry);
	SolersToolExecutor *get_tool_executor() const { return tool_executor; }
	void set_settings_service(SolersSettingsService *p_settings_service) { settings_service = p_settings_service; }
	void set_model_catalog(SolersModelCatalog *p_model_catalog, bool p_owned = false);
	void set_permission_manager(SolersPermissionManager *p_permission_manager);
	void set_observations(SolersProjectObservation *p_project, SolersSceneObservation *p_scene, SolersRuntimeObservation *p_runtime);
	Dictionary start_turn(const Dictionary &p_args);
	Dictionary preview_rewind_to_event(int64_t p_event_id) const;
	Dictionary rewind_to_event(int64_t p_event_id);
	Dictionary queue_user_message(const Dictionary &p_args);
	void poll();
	void shutdown();
	void abort();
	void reset_conversation();
	void set_project_path(const String &p_project_path);
	void set_session(const String &p_project_path, const String &p_session_id);
	Array get_messages() const;
	Array get_timeline_entries() const { return timeline_entries; }
	Dictionary get_status() const;
	bool is_running() const { return running; }
	bool is_executing_tool() const;

	SolersAgentSession();
	~SolersAgentSession();
};
