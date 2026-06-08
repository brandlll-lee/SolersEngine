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

class SolersActionTimeline;
class SolersEditorOperator;
class SolersObservationService;
class SolersResourceService;
class SolersRpcServer;
class SolersScriptService;
class SolersSettingsService;

class SolersToolRegistry : public Object {
	GDCLASS(SolersToolRegistry, Object);

	struct ToolDefinition {
		StringName name;
		String description;
		SolersPermissionManager::Permission permission = SolersPermissionManager::PERMISSION_OBSERVE;
		String mutation_kind = "none";
		bool requires_approval = false;
		int timeout_ms = 10000;
		Dictionary input_schema;
		Dictionary output_schema;
	};

	HashMap<StringName, ToolDefinition> tools;
	SolersEditorOperator *editor_operator = nullptr;
	SolersObservationService *observation_service = nullptr;
	SolersResourceService *resource_service = nullptr;
	SolersScriptService *script_service = nullptr;
	SolersSettingsService *settings_service = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	SolersActionTimeline *action_timeline = nullptr;
	SolersRpcServer *rpc_server = nullptr;

	void _register_tool(const ToolDefinition &p_definition);
	Dictionary _tool_to_dictionary(const ToolDefinition &p_definition) const;
	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	Dictionary _object_schema() const;

protected:
	static void _bind_methods();

public:
	void set_editor_operator(SolersEditorOperator *p_editor_operator);
	void set_observation_service(SolersObservationService *p_observation_service);
	void set_resource_service(SolersResourceService *p_resource_service);
	void set_script_service(SolersScriptService *p_script_service);
	void set_settings_service(SolersSettingsService *p_settings_service);
	void set_permission_manager(SolersPermissionManager *p_permission_manager);
	void set_action_timeline(SolersActionTimeline *p_action_timeline);
	void set_rpc_server(SolersRpcServer *p_rpc_server);

	void register_default_tools();
	Array list_tools() const;
	Dictionary call_tool(const StringName &p_name, const Dictionary &p_args);
	int get_tool_count() const;
};
