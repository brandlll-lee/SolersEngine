/**************************************************************************/
/*  solers_tool_registry.h                                                */
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
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/variant/dictionary.h"

#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_tool.h"

class SolersActionTimeline;
class SolersAssetService;
class SolersFileCheckpoint;
class SolersObservationService;
class SolersReflectionService;
class SolersResourceService;
class SolersScriptService;

struct SolersPreparedToolCall {
	SolersTool *tool = nullptr;
	StringName name;
	Dictionary args;
	Dictionary timeline_payload;
	SolersToolContext context;
	SolersToolExecution execution = SolersToolExecution::MAIN_THREAD;
	SolersToolMutationPolicy mutation_policy = SolersToolMutationPolicy::READ_ONLY;
	Dictionary reversal_state;
	Dictionary journal_event;
};

class SolersToolRegistry : public Object {
	GDCLASS(SolersToolRegistry, Object);

	HashMap<StringName, SolersTool *> tools; // owned; freed on clear/destroy
	HashMap<StringName, StringName> model_name_index;
	Array tool_catalog;
	HashMap<StringName, Dictionary> tool_catalog_by_name;
	uint64_t tool_catalog_revision = 0;
	HashMap<String, Vector<Dictionary>> reversals_by_session;
	HashSet<String> delivered_addon_contracts;

	SolersObservationService *observation_service = nullptr;
	SolersAssetService *asset_service = nullptr;
	SolersFileCheckpoint *file_checkpoint = nullptr;
	SolersReflectionService *reflection_service = nullptr;
	SolersResourceService *resource_service = nullptr;
	SolersScriptService *script_service = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	SolersActionTimeline *action_timeline = nullptr;

	static String _make_model_tool_name(const StringName &p_name);
	static Dictionary _schema(const char *p_json);

	void _clear_tools();
	void _register(SolersTool *p_tool);
	void _rebuild_tool_catalog();
	void _add(const char *p_name, const char *p_description, const char *p_schema_json,
			SolersPermissionManager::Permission p_permission, SolersToolMutationPolicy p_mutation_policy,
			const Vector<String> &p_redact,
			SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler,
			SolersToolExecution p_execution = SolersToolExecution::MAIN_THREAD,
			std::function<Array(const Dictionary &)> p_resource_access = {},
			SolersFunctionTool::PollHandler p_poll_handler = {},
			SolersFunctionTool::ReadyHandler p_ready_handler = {},
			SolersFunctionTool::CompletionHandler p_completion_handler = {},
			std::function<SolersPermissionManager::Permission(const Dictionary &)> p_permission_resolver = {},
			std::function<SolersToolMutationPolicy(const Dictionary &)> p_mutation_policy_resolver = {},
			SolersToolUiKind p_ui_kind = SolersToolUiKind::DEFAULT);
	void _add_observe_exposed(const char *p_name, const char *p_description, const char *p_schema_json,
			SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler,
			std::function<Array(const Dictionary &)> p_resource_access = {},
			SolersFunctionTool::PollHandler p_poll_handler = {},
			SolersFunctionTool::ReadyHandler p_ready_handler = {},
			SolersToolUiKind p_ui_kind = SolersToolUiKind::OBSERVE,
			SolersToolExecution p_execution = SolersToolExecution::MAIN_THREAD);
	void _add_observe(const char *p_name, const char *p_description, const char *p_schema_json,
			SolersFunctionTool::Handler p_handler, std::function<Array(const Dictionary &)> p_resource_access = {},
			SolersFunctionTool::PollHandler p_poll_handler = {},
			SolersFunctionTool::ReadyHandler p_ready_handler = {},
			SolersToolUiKind p_ui_kind = SolersToolUiKind::OBSERVE);

	void _register_observation_tools();
	void _register_script_tools();
	void _register_runtime_tools();
	void _register_asset_tools();
	void _register_addon_tools();
	void _register_reflection_tools();
	void _register_skill_tools();
	void _register_search_tools();
	Dictionary _inspect_engine(const Dictionary &p_args);
	Dictionary _transact_objects(const Dictionary &p_args);
	Dictionary _compact_addon_contract(const SolersToolContext &p_context, const Dictionary &p_result);
	Dictionary _run_control(const Dictionary &p_args) const;
	Dictionary _poll_runtime_control(const Dictionary &p_args) const;
	bool _is_runtime_control_ready(const Dictionary &p_args) const;
	Dictionary _revert_latest(const SolersToolContext &p_context, const Dictionary &p_args);
	const Dictionary *_find_reversal(const String &p_reversal_id) const;
	Dictionary _prepare_reversal(SolersPreparedToolCall &r_call);
	void _discard_reversal(const Dictionary &p_record);
	Dictionary _finalize_prepared_result(SolersPreparedToolCall &r_call, const Dictionary &p_result);
	Dictionary _preflight_tool_call(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context, Dictionary &r_args);
	Dictionary _prepare_tool_call(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context, SolersPreparedToolCall &r_call);
	Dictionary _execute_prepared_tool(SolersPreparedToolCall &p_call);
	Dictionary _poll_prepared_tool(SolersPreparedToolCall &p_call, const Dictionary &p_args);
	bool _is_prepared_tool_ready(const SolersPreparedToolCall &p_call, const Dictionary &p_args) const;
	void _complete_prepared_tool(const SolersPreparedToolCall &p_call, const Dictionary &p_result);

	Dictionary _tool_to_dictionary(const SolersTool *p_tool) const;
	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;

protected:
	static void _bind_methods();

public:
	friend class SolersAgentSession;
	void set_observation_service(SolersObservationService *p_observation_service);
	void set_asset_service(SolersAssetService *p_asset_service);
	void set_file_checkpoint(SolersFileCheckpoint *p_file_checkpoint);
	void set_reflection_service(SolersReflectionService *p_reflection_service);
	void set_resource_service(SolersResourceService *p_resource_service);
	void set_script_service(SolersScriptService *p_script_service);
	void set_permission_manager(SolersPermissionManager *p_permission_manager);
	void set_action_timeline(SolersActionTimeline *p_action_timeline);

	void register_default_tools();
	void register_tool(SolersTool *p_tool);
	Array list_tools() const;
	Dictionary get_tool_definition(const StringName &p_name) const;
	uint64_t get_tool_catalog_revision() const { return tool_catalog_revision; }
	String get_skill_catalog_prompt() const;
	String get_model_tool_name(const StringName &p_name) const;
	StringName resolve_model_tool_name(const String &p_model_name) const;
	bool affects_scene_state(const StringName &p_name, const Dictionary &p_args = Dictionary()) const;
	bool is_read_only(const StringName &p_name, const Dictionary &p_args = Dictionary()) const;
	Array resolve_resource_access(const StringName &p_name, const Dictionary &p_args) const;
	static bool has_write_conflict(const Array &p_left, const Array &p_right);
	Dictionary normalize_tool_args(const StringName &p_name, const Dictionary &p_args) const;
	Dictionary redact_tool_args_for_audit(const StringName &p_name, const Dictionary &p_args) const;
	Dictionary summarize_tool_args_for_audit(const StringName &p_name, const Dictionary &p_args) const;
	String summarize_tool_result_for_audit(const Dictionary &p_result) const;
	Dictionary call_tool(const StringName &p_name, const Dictionary &p_args);
	Dictionary call_tool_with_context(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context);
	void clear_task_state(const String &p_session_id);
	void restore_session_reversals(const String &p_session_id, const Array &p_records);
	Dictionary preview_session_rewind(const String &p_session_id, uint64_t p_target_revision) const;
	Dictionary prepare_session_rewind(const String &p_session_id, uint64_t p_target_revision);
	Dictionary apply_session_rewind(const Dictionary &p_transaction);
	Dictionary abort_session_rewind(const Dictionary &p_transaction);
	Dictionary finish_session_rewind(const Dictionary &p_transaction);
	int get_tool_count() const;

	SolersToolRegistry();
	~SolersToolRegistry();
};
