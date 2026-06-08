/**************************************************************************/
/*  solers_editor_plugin.cpp                                              */
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

#include "solers_editor_plugin.h"

#include "solers_dock.h"
#include "editor/editor_node.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_agent_orchestrator.h"
#include "modules/solers_ai/core/solers_agent_runtime.h"
#include "modules/solers_ai/core/solers_editor_operator.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_provider_gateway.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "modules/solers_ai/protocol/solers_rpc_server.h"

String SolersEditorPlugin::get_plugin_name() const {
	return "Solers";
}

void SolersEditorPlugin::make_visible(bool p_visible) {
	if (dock) {
		dock->set_visible(true);
		dock->make_visible();
	}
}

void SolersEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			action_timeline = memnew(SolersActionTimeline);
			agent_orchestrator = memnew(SolersAgentOrchestrator);
			agent_runtime = memnew(SolersAgentRuntime);
			editor_operator = memnew(SolersEditorOperator);
			file_checkpoint = memnew(SolersFileCheckpoint);
			mcp_adapter = memnew(SolersMCPAdapter);
			observation_service = memnew(SolersObservationService);
			permission_manager = memnew(SolersPermissionManager);
			provider_gateway = memnew(SolersProviderGateway);
			provider_registry = memnew(SolersProviderRegistry);
			resource_service = memnew(SolersResourceService);
			rpc_server = memnew(SolersRpcServer);
			script_service = memnew(SolersScriptService);
			settings_service = memnew(SolersSettingsService);
			tool_registry = memnew(SolersToolRegistry);

			file_checkpoint->set_action_timeline(action_timeline);
			script_service->set_action_timeline(action_timeline);
			script_service->set_file_checkpoint(file_checkpoint);
			settings_service->set_provider_registry(provider_registry);

			tool_registry->set_action_timeline(action_timeline);
			tool_registry->set_editor_operator(editor_operator);
			tool_registry->set_observation_service(observation_service);
			tool_registry->set_permission_manager(permission_manager);
			tool_registry->set_resource_service(resource_service);
			tool_registry->set_script_service(script_service);
			tool_registry->set_settings_service(settings_service);
			tool_registry->register_default_tools();

			agent_runtime->set_action_timeline(action_timeline);
			agent_runtime->set_observation_service(observation_service);
			agent_runtime->set_tool_registry(tool_registry);

			agent_orchestrator->set_action_timeline(action_timeline);
			agent_orchestrator->set_provider_gateway(provider_gateway);
			agent_orchestrator->set_tool_registry(tool_registry);

			mcp_adapter->set_action_timeline(action_timeline);
			mcp_adapter->set_agent_orchestrator(agent_orchestrator);
			mcp_adapter->set_agent_runtime(agent_runtime);
			mcp_adapter->set_observation_service(observation_service);
			mcp_adapter->set_tool_registry(tool_registry);

			rpc_server->set_mcp_adapter(mcp_adapter);
			tool_registry->set_rpc_server(rpc_server);

			dock = memnew(SolersDock);
			dock->set_services(observation_service, tool_registry, action_timeline, permission_manager, agent_runtime, mcp_adapter, rpc_server, settings_service);
			EditorNode::get_singleton()->set_solers_ai_panel(dock);
			set_process(true);
		} break;

		case NOTIFICATION_PROCESS: {
			if (rpc_server) {
				rpc_server->poll();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			set_process(false);
			if (dock) {
				if (EditorNode::get_singleton()) {
					EditorNode::get_singleton()->set_solers_ai_panel(nullptr);
				}
				dock->queue_free();
				dock = nullptr;
			}
			if (tool_registry) {
				memdelete(tool_registry);
				tool_registry = nullptr;
			}
			if (script_service) {
				memdelete(script_service);
				script_service = nullptr;
			}
			if (settings_service) {
				memdelete(settings_service);
				settings_service = nullptr;
			}
			if (permission_manager) {
				memdelete(permission_manager);
				permission_manager = nullptr;
			}
			if (provider_gateway) {
				memdelete(provider_gateway);
				provider_gateway = nullptr;
			}
			if (provider_registry) {
				memdelete(provider_registry);
				provider_registry = nullptr;
			}
			if (resource_service) {
				memdelete(resource_service);
				resource_service = nullptr;
			}
			if (rpc_server) {
				rpc_server->stop();
				memdelete(rpc_server);
				rpc_server = nullptr;
			}
			if (observation_service) {
				memdelete(observation_service);
				observation_service = nullptr;
			}
			if (mcp_adapter) {
				memdelete(mcp_adapter);
				mcp_adapter = nullptr;
			}
			if (file_checkpoint) {
				memdelete(file_checkpoint);
				file_checkpoint = nullptr;
			}
			if (editor_operator) {
				memdelete(editor_operator);
				editor_operator = nullptr;
			}
			if (agent_runtime) {
				memdelete(agent_runtime);
				agent_runtime = nullptr;
			}
			if (agent_orchestrator) {
				memdelete(agent_orchestrator);
				agent_orchestrator = nullptr;
			}
			if (action_timeline) {
				memdelete(action_timeline);
				action_timeline = nullptr;
			}
		} break;
	}
}

SolersEditorPlugin::SolersEditorPlugin() {
}

SolersEditorPlugin::~SolersEditorPlugin() {
}
