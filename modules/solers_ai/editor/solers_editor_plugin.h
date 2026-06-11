/**************************************************************************/
/*  solers_editor_plugin.h                                                */
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

#include "editor/plugins/editor_plugin.h"

class SolersDock;
class SolersActionTimeline;
class SolersAgentOrchestrator;
class SolersAgentRuntime;
class SolersAgentSession;
class SolersEditorOperator;
class SolersFileCheckpoint;
class SolersMCPAdapter;
class SolersObservationService;
class SolersPermissionManager;
class SolersProviderGateway;
class SolersProviderRegistry;
class SolersResourceService;
class SolersRpcServer;
class SolersScriptService;
class SolersSettingsService;
class SolersToolRegistry;

class SolersEditorPlugin : public EditorPlugin {
	GDCLASS(SolersEditorPlugin, EditorPlugin);

	SolersDock *dock = nullptr;
	SolersActionTimeline *action_timeline = nullptr;
	SolersAgentOrchestrator *agent_orchestrator = nullptr;
	SolersAgentRuntime *agent_runtime = nullptr;
	SolersAgentSession *agent_session = nullptr;
	SolersEditorOperator *editor_operator = nullptr;
	SolersFileCheckpoint *file_checkpoint = nullptr;
	SolersMCPAdapter *mcp_adapter = nullptr;
	SolersObservationService *observation_service = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	SolersProviderGateway *provider_gateway = nullptr;
	SolersProviderRegistry *provider_registry = nullptr;
	SolersResourceService *resource_service = nullptr;
	SolersRpcServer *rpc_server = nullptr;
	SolersScriptService *script_service = nullptr;
	SolersSettingsService *settings_service = nullptr;
	SolersToolRegistry *tool_registry = nullptr;

protected:
	void _notification(int p_what);

public:
	String get_plugin_name() const override;
	void make_visible(bool p_visible) override;

	SolersEditorPlugin();
	~SolersEditorPlugin();
};
