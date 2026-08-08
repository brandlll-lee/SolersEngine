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
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersToolRegistry;
struct SolersPreparedToolCall;
class SolersActionTimeline;
class EditorLog;
class SolersSettingsService;
class SolersPermissionManager;
class SolersContextManager;
class SolersModelsDev;
class SolersLLMProtocolRegistry;
class SolersLLMClient;

class SolersAgentSession : public Object {
	GDCLASS(SolersAgentSession, Object);
	struct ToolThreadState;
	struct PendingToolExecution;

	enum Phase {
		PHASE_STREAMING,
		PHASE_COMPACTING,
		PHASE_TOOLS,
		PHASE_AWAITING_APPROVAL,
		PHASE_TOOL_EXECUTING,
		PHASE_WAITING,
	};

	static constexpr int MAX_LLM_RETRY_ATTEMPTS = 3;

	SolersToolRegistry *tool_registry = nullptr;
	SolersSettingsService *settings_service = nullptr;
	SolersActionTimeline *action_timeline = nullptr;
	SolersPermissionManager *permission_manager = nullptr;

	SolersLLMProtocolRegistry *protocol_registry = nullptr; // owned
	SolersLLMClient *client = nullptr; // owned
	SolersContextManager *context_manager = nullptr; // owned
	SolersModelsDev *models_dev = nullptr; // data-driven model registry
	bool owns_models_dev = true;

	int context_window = 0; // Unknown until provider/model metadata says otherwise.
	int max_output_tokens = 8192;
	Array messages; // active model projection; transcript is the full audit history
	mutable Array timeline_entries;
	mutable HashMap<String, Dictionary> open_timeline_tools;
	mutable int64_t transcript_event_sequence = 0;
	String system_prompt;
	String current_text; // assistant text accumulated this model turn
	String current_reasoning; // reasoning/thinking text accumulated this model turn
	Dictionary current_provider_metadata; // opaque wire state for provider continuation
	Array pending_tool_calls; // tool calls collected this model turn
	Dictionary streamed_tool_calls; // call id -> surfaced tool call state for this model step
	String last_stop_reason;
	Dictionary last_usage;
	Dictionary current_plan;
	String last_outcome;
	Dictionary active_provider; // { provider, model, base_url, api_key, features }
	bool running = false;
	Phase phase = PHASE_STREAMING;
	Array tool_queue; // provider-ordered tool calls for this step (MAIN_THREAD tools run serially)
	Array failed_resource_accesses;
	HashMap<String, Dictionary> readonly_cache;
	HashSet<StringName> task_deferred_tools;
	Array cached_request_tools;
	int cached_request_tool_tokens = 0;
	uint32_t cached_request_deferred_count = 0;
	uint64_t cached_tool_catalog_revision = 0;
	Array turn_attachments;
	HashSet<String> delivered_model_attachments;
	HashSet<String> pending_model_attachments;
	Array turn_mentions;
	int tool_queue_index = 0; // next provider-ordered call to start
	int tool_delivery_index = 0; // next provider-ordered terminal result to deliver
	HashMap<int, Dictionary> completed_tool_results;
	Vector<PendingToolExecution *> pending_tool_executions;
	bool tool_started_announced = false;
	uint64_t tool_queued_msec = 0;
	uint64_t tool_started_msec = 0;
	uint64_t tool_completed_msec = 0;
	int deferred_queue_index = -1;
	String deferred_call_id;
	String deferred_model_name;
	String deferred_canonical_name;
	Dictionary deferred_args;
	Dictionary deferred_initial_args;
	Array deferred_resource_accesses;
	Dictionary deferred_result;
	SolersPreparedToolCall *deferred_prepared_call = nullptr;
	bool deferred_done = false;
	bool deferred_polling = false;
	bool deferred_is_resume = false;
	uint64_t tool_exec_token = 0;
	// Tool execution is requested here and performed from the next poll()
	// tick. poll() runs on the plain main-loop stack (EditorNode process),
	// the same context as a user-triggered editor action — unlike
	// call_deferred, whose MessageQueue::flush() context forbids progress
	// dialogs and broke save/play tools.
	bool tool_exec_requested = false;
	String last_progress_call_id;
	uint64_t last_progress_msec = 0;
	Thread tool_thread;
	ToolThreadState *tool_thread_state = nullptr;
	SafeFlag tool_cancel_requested;
	Dictionary awaiting_call; // tool call parked on the approval gate
	int awaiting_approval_id = 0; // pending request id we are waiting on
	int turn_id = 0;
	int retry_attempt = 0;
	int model_request_index = 0;
	int64_t turn_fresh_input_tokens = 0;
	int64_t turn_cache_read_tokens = 0;
	int64_t turn_cache_write_tokens = 0;
	int64_t turn_output_tokens = 0;
	int64_t turn_wire_body_bytes = 0;
	int request_transient_tokens = 0;
	uint64_t retry_resume_msec = 0;
	int text_delta_count = 0;
	uint64_t last_text_delta_msec = 0;
	Array compaction_source_messages;
	int64_t compaction_event_id = 0;
	Dictionary retry_request;
	Dictionary retry_profile;
	int overflow_compaction_attempts = 0;
	bool turn_runtime_owned = false;
	SolersToolRegistry *session_tools_registry = nullptr;
	String project_path;
	String session_id;
	Array pending_steering_messages; // user input queued mid-turn, injected before the next model dispatch
	Array pending_background_assets;
	HashSet<String> delivered_background_assets;
	HashSet<String> waiting_background_asset_ids;
	bool godot_log_audit_installed = false;
	ObjectID godot_log_object_id;
	bool godot_log_turn_active = false;
	mutable Mutex godot_log_mutex;
	int godot_log_error_count = 0;
	int godot_log_warning_count = 0;
	HashMap<uint64_t, Dictionary> worker_tool_audits;
	Dictionary main_thread_tool_audit; // active only while a native handler is on-stack
	Dictionary deferred_window_audit; // the single parked tool whose continuation owns the main thread between polls
	Dictionary attributable_tool_errors; // call_id -> scoped Godot error evidence
	uint64_t authored_revision = 0; // Session-local ordering only; native receipts carry authority.
	uint64_t observed_revision = 0;
	uint64_t runtime_observation_cursor = 0;
	Dictionary render_artifacts; // artifact kind -> versioned native-tool result
	// Aggregated by (severity, message, call_id) at ingestion: a repeated
	// engine error is one entry with a count, so a per-frame error loop can
	// never flood the transcript or the model boundary.
	Array pending_godot_diagnostics;
	HashMap<String, int> pending_godot_diagnostic_index; // group key -> index into pending_godot_diagnostics
	int pending_godot_diagnostics_overflow = 0;
	bool background_resume_suppressed = false;

	void _reset_session_derived_state();
	String _default_system_prompt() const;
	// Fresh engine facts for one request, carried as the final projected user
	// message (origin solers_state) so the system prompt stays byte-stable
	// and provider prompt caching keeps its prefix hits.
	Dictionary _environment_context_message();
	String _make_session_id() const;
	Dictionary _read_transcript_state(const String &p_project_path, const String &p_session_id) const;
	void _stamp_transcript_event(Dictionary &r_event) const;
	void _write_transcript_event(const String &p_type, const Dictionary &p_payload = Dictionary()) const;
	void _write_prepared_journal_event(SolersPreparedToolCall *p_call) const;
	void _ensure_godot_log_audit(bool p_turn_active);
	void _release_godot_log_audit();
	void _on_godot_log_message(const String &p_message, int p_type, int64_t p_source_thread);
	void _begin_main_thread_tool_audit();
	void _end_main_thread_tool_audit();
	void _refresh_deferred_window_audit();
	void _register_worker_tool_audit(uint64_t p_thread_id, const String &p_call_id, const String &p_tool, const Array &p_resource_accesses);
	Dictionary _consume_attributable_tool_error(const String &p_call_id);
	Dictionary _take_godot_diagnostics();
	String _readonly_cache_key(const StringName &p_name, const Dictionary &p_args) const;
	Array _collect_tools();
	bool _refresh_active_model_limits();
	int _active_model_input_support(const String &p_modality) const;
	Dictionary _build_request(const Array &p_messages, const String &p_request_system_prompt, const Array &p_tools) const;
	Dictionary _provider_dispatch_error() const;
	Error _begin_provider_request(const Dictionary &p_request, const Dictionary &p_profile);
	Error _dispatch_model_request(bool p_skip_compaction = false);
	Error _dispatch_compaction_request();
	void _commit_attachment_projection();
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
	void _poll_awaiting_approval();
	void _poll_tool_executing();
	void _schedule_tool_execution(int p_queue_index, const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, bool p_is_resume);
	void _park_pending_tool(const Dictionary &p_poll_args);
	bool _resume_ready_pending_tool();
	bool _conflicts_with_pending(const Array &p_accesses) const;
	void _queue_tool_result(int p_queue_index, const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, uint64_t p_started_msec);
	bool _flush_tool_results();
	void _clear_pending_tools();
	void _cancel_undelivered_tools();
	void _execute_deferred_tool(uint64_t p_token);
	static void _tool_thread_func(void *p_userdata);
	bool _collect_tool_thread_result(bool p_wait);
	void _deliver_tool_result(const String &p_id, const String &p_model_name, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, int p_budget = INT32_MAX);
	bool _is_awaiting_approval_result(const Dictionary &p_result) const;
	void _register_session_tools();
	Dictionary _handle_update_plan(const Dictionary &p_args);
	bool _flush_pending_steering();
	bool _append_background_asset_deltas(bool p_waited_only);
	void _resume_next_background_asset();
	int64_t _write_transcript_message(const String &p_role, const String &p_content, const Array &p_mentions = Array(), const Array &p_tool_calls = Array(), const String &p_reasoning = String(), const Array &p_attachments = Array(), int64_t p_event_id = 0) const;
	void _write_transcript_tool(const String &p_call_id, const String &p_canonical_name, const Dictionary &p_args, const Dictionary &p_result, const String &p_delivered_content) const;
	void _write_transcript_plan() const;
	void _write_transcript_compaction(const String &p_phase, const Dictionary &p_payload) const;
	Dictionary _tool_denied_result(const String &p_code, const String &p_message) const;
	void _record(const String &p_event, const Dictionary &p_payload) const;
	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message) const;

protected:
	static void _bind_methods();

public:
	void set_tool_registry(SolersToolRegistry *p_tool_registry);
	void set_settings_service(SolersSettingsService *p_settings_service) { settings_service = p_settings_service; }
	void set_models_dev(SolersModelsDev *p_models_dev, bool p_owned = false);
	void set_action_timeline(SolersActionTimeline *p_action_timeline) { action_timeline = p_action_timeline; }
	void set_permission_manager(SolersPermissionManager *p_permission_manager) { permission_manager = p_permission_manager; }

	static Dictionary validate_plan(const Dictionary &p_args);
	// Exactly what a tool result contributes to the conversation: the whole
	// serialized result, or — when it exceeds the budget — an envelope naming
	// what was elided and where the complete body lives. Always valid JSON, so
	// no measurement the engine made reaches the model as unparsable text.
	static String deliverable_tool_result(const String &p_call_id, const Dictionary &p_result, int p_budget);
	Dictionary start_turn(const Dictionary &p_args); // { prompt: String }
	// Steer the running turn: the message is queued and joins the
	// conversation after the current tool batch, before the next model
	// dispatch. Fails with AGENT_IDLE when no turn is running.
	Dictionary queue_user_message(const Dictionary &p_args);
	void poll();
	void shutdown();
	void abort();
	void reset_conversation();
	void set_project_path(const String &p_project_path);
	void set_session(const String &p_project_path, const String &p_session_id);
	bool enqueue_background_asset(const Dictionary &p_manifest);
	void resume_background_assets();
	bool is_waiting_for_background_assets() const { return running && phase == PHASE_WAITING && !waiting_background_asset_ids.is_empty(); }
	Array get_messages() const;
	Array get_timeline_entries() const { return timeline_entries; }
	Dictionary get_plan() const { return current_plan.duplicate(true); }
	Dictionary get_status() const;
	bool is_running() const { return running; }
	bool is_executing_tool() const { return running && phase == PHASE_TOOL_EXECUTING; }
	bool is_admitting_tool_calls() const;

	SolersAgentSession();
	~SolersAgentSession();
};
