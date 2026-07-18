/**************************************************************************/
/*  solers_tool_registry.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_tool.h"

class SolersActionTimeline;
class SolersAssetService;
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
	String failure_cache_key;
	SolersToolExecution execution = SolersToolExecution::MAIN_THREAD;
};

class SolersToolRegistry : public Object {
	GDCLASS(SolersToolRegistry, Object);

	HashMap<StringName, SolersTool *> tools; // owned; freed on clear/destroy
	HashMap<StringName, StringName> model_name_index;
	HashMap<String, Dictionary> failed_calls;

	SolersObservationService *observation_service = nullptr;
	SolersAssetService *asset_service = nullptr;
	SolersReflectionService *reflection_service = nullptr;
	SolersResourceService *resource_service = nullptr;
	SolersScriptService *script_service = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	SolersActionTimeline *action_timeline = nullptr;

	static String _make_model_tool_name(const StringName &p_name);
	static Dictionary _schema(const char *p_json);

	void _clear_tools();
	void _register(SolersTool *p_tool);
	void _add(const char *p_name, const char *p_description, const char *p_schema_json,
			SolersPermissionManager::Permission p_permission, const char *p_mutation_kind,
			bool p_requires_approval, bool p_undoable, const Vector<String> &p_redact,
			SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler, bool p_ephemeral_result = false,
			SolersToolExecution p_execution = SolersToolExecution::MAIN_THREAD,
			std::function<Array(const Dictionary &)> p_resource_access = {},
			bool p_cache_across_revisions = false,
			SolersFunctionTool::PollHandler p_poll_handler = {},
			bool p_produces_scene_validation = false,
			SolersFunctionTool::ReadyHandler p_ready_handler = {},
			SolersFunctionTool::CompletionHandler p_completion_handler = {});
	void _add_observe_exposed(const char *p_name, const char *p_description, const char *p_schema_json,
			SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler, bool p_ephemeral_result = true,
			std::function<Array(const Dictionary &)> p_resource_access = {},
			bool p_cache_across_revisions = false,
			SolersFunctionTool::PollHandler p_poll_handler = {},
			SolersFunctionTool::ReadyHandler p_ready_handler = {});
	void _add_observe(const char *p_name, const char *p_description, const char *p_schema_json,
			SolersFunctionTool::Handler p_handler, std::function<Array(const Dictionary &)> p_resource_access = {},
			SolersFunctionTool::PollHandler p_poll_handler = {},
			SolersFunctionTool::ReadyHandler p_ready_handler = {});

	void _register_observation_tools();
	void _register_script_tools();
	void _register_runtime_tools();
	void _register_asset_tools();
	void _register_modeling_tools();
	void _register_reflection_tools();
	void _register_skill_tools();
	void _register_search_tools();
	Dictionary _run_control(const Dictionary &p_args) const;
	Dictionary _preflight_tool_call(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context, Dictionary &r_args, String &r_failure_cache_key);
	Dictionary _prepare_tool_call(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context, SolersPreparedToolCall &r_call);
	Dictionary _execute_prepared_tool(const SolersPreparedToolCall &p_call) const;
	Dictionary _poll_prepared_tool(const SolersPreparedToolCall &p_call, const Dictionary &p_args) const;
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
	void set_reflection_service(SolersReflectionService *p_reflection_service);
	void set_resource_service(SolersResourceService *p_resource_service);
	void set_script_service(SolersScriptService *p_script_service);
	void set_permission_manager(SolersPermissionManager *p_permission_manager);
	void set_action_timeline(SolersActionTimeline *p_action_timeline);

	void register_default_tools();
	void register_tool(SolersTool *p_tool);
	Array list_tools() const;
	String get_skill_catalog_prompt() const;
	String get_model_tool_name(const StringName &p_name) const;
	StringName resolve_model_tool_name(const String &p_model_name) const;
	bool is_result_ephemeral(const StringName &p_name) const;
	bool caches_across_revisions(const StringName &p_name) const;
	bool affects_authored_state(const StringName &p_name) const;
	bool affects_scene_state(const StringName &p_name) const;
	bool produces_scene_validation(const StringName &p_name) const;
	bool is_read_only(const StringName &p_name, const Dictionary &p_args = Dictionary()) const;
	Array resolve_resource_access(const StringName &p_name, const Dictionary &p_args) const;
	static bool has_write_conflict(const Array &p_left, const Array &p_right);
	Dictionary normalize_tool_args(const StringName &p_name, const Dictionary &p_args) const;
	Dictionary redact_tool_args_for_audit(const StringName &p_name, const Dictionary &p_args) const;
	Dictionary protect_tool_args_for_replay(const StringName &p_name, const Dictionary &p_args) const;
	Dictionary restore_tool_args_from_replay(const StringName &p_name, const Dictionary &p_args) const;
	Dictionary summarize_tool_args_for_audit(const StringName &p_name, const Dictionary &p_args) const;
	String summarize_tool_result_for_audit(const Dictionary &p_result) const;
	Dictionary call_tool(const StringName &p_name, const Dictionary &p_args);
	Dictionary call_tool_with_context(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context);
	void clear_task_state(const String &p_session_id);
	int get_tool_count() const;

	SolersToolRegistry();
	~SolersToolRegistry();
};
