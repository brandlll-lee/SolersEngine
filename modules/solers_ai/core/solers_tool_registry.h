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
#include "core/variant/dictionary.h"

#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_tool.h"

class SolersProjectObservation;
class SolersReflectionService;
class SolersResourceService;
class SolersRuntimeObservation;
class SolersSceneObservation;
class SolersScriptService;

struct SolersPreparedToolCall {
	SolersTool *tool = nullptr;
	StringName name;
	Dictionary args;
	SolersToolContext context;
};

class SolersToolRegistry : public Object {
	GDCLASS(SolersToolRegistry, Object);

	HashMap<StringName, SolersTool *> tools;
	HashMap<StringName, StringName> model_name_index;
	Array tool_catalog;
	HashMap<StringName, Dictionary> tool_catalog_by_name;
	uint64_t tool_catalog_revision = 0;

	SolersProjectObservation *project_observation = nullptr;
	SolersReflectionService *reflection_service = nullptr;
	SolersResourceService *resource_service = nullptr;
	SolersRuntimeObservation *runtime_observation = nullptr;
	SolersSceneObservation *scene_observation = nullptr;
	SolersScriptService *script_service = nullptr;
	SolersPermissionManager *permission_manager = nullptr;

	static String _make_model_tool_name(const StringName &p_name);
	static Dictionary _schema(const char *p_json);
	static Variant _redact_schema_value(const Dictionary &p_schema, const Variant &p_value);

	void _clear_tools();
	void _register(SolersTool *p_tool);
	void _rebuild_tool_catalog();
	void _add(const char *p_name, const char *p_description, const char *p_schema_json,
			SolersFunctionTool::Handler p_handler,
			SolersFunctionTool::PollHandler p_poll_handler = {},
			SolersFunctionTool::ReadyHandler p_ready_handler = {},
			SolersFunctionTool::CompletionHandler p_completion_handler = {});
	void _register_builtin_tools();
	void _register_observation_tools();
	void _register_reflection_tools();
	void _register_runtime_tools();
	void _register_script_tools();

	Dictionary _inspect_engine(const Dictionary &p_args);
	Dictionary _run_control(const Dictionary &p_args, const String &p_call_id, const SolersToolContext *p_context = nullptr) const;
	Dictionary _poll_runtime_control(const Dictionary &p_args, const SolersToolContext *p_context = nullptr) const;
	bool _is_runtime_control_ready(const Dictionary &p_args) const;
	Dictionary _tool_to_dictionary(const SolersTool *p_tool) const;
	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;

protected:
	static void _bind_methods();

public:
	void set_project_observation(SolersProjectObservation *p_project_observation);
	void set_reflection_service(SolersReflectionService *p_reflection_service);
	void set_resource_service(SolersResourceService *p_resource_service);
	void set_runtime_observation(SolersRuntimeObservation *p_runtime_observation);
	void set_scene_observation(SolersSceneObservation *p_scene_observation);
	void set_script_service(SolersScriptService *p_script_service);
	void set_permission_manager(SolersPermissionManager *p_permission_manager);

	void register_default_tools();
	void register_tool(SolersTool *p_tool);
	Array list_tools() const;
	Dictionary get_tool_definition(const StringName &p_name) const;
	uint64_t get_tool_catalog_revision() const { return tool_catalog_revision; }
	String get_skill_catalog_prompt() const;
	String get_model_tool_name(const StringName &p_name) const;
	StringName resolve_model_tool_name(const String &p_model_name) const;
	Dictionary normalize_tool_args(const StringName &p_name, const Dictionary &p_args) const;
	Dictionary redact_tool_args_for_audit(const StringName &p_name, const Dictionary &p_args) const;
	String summarize_tool_result_for_audit(const Dictionary &p_result) const;

	Dictionary prepare_call(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context, SolersPreparedToolCall &r_call);
	Dictionary execute_call(SolersPreparedToolCall &p_call);
	Dictionary poll_call(SolersPreparedToolCall &p_call, const Dictionary &p_args);
	bool is_call_ready(const SolersPreparedToolCall &p_call, const Dictionary &p_args) const;
	void complete_call(const SolersPreparedToolCall &p_call, const Dictionary &p_result);
	Dictionary call_tool(const StringName &p_name, const Dictionary &p_args);
	Dictionary call_tool_with_context(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context);
	int get_tool_count() const;

	SolersToolRegistry();
	~SolersToolRegistry();
};
