/**************************************************************************/
/*  solers_agent_runtime.cpp                                              */
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

#include "solers_agent_runtime.h"

#include "editor/editor_interface.h"

#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_project_observation.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_runtime_observation.h"
#include "modules/solers_ai/core/solers_scene_observation.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/editor/solers_dock.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "modules/solers_ai/protocol/solers_rpc_server.h"

SolersAgentRuntime::SolersAgentRuntime() {
	asset_service = memnew(SolersAssetService);
	agent_session = memnew(SolersAgentSession);
	mcp_adapter = memnew(SolersMCPAdapter);
	permission_manager = memnew(SolersPermissionManager);
	project_observation = memnew(SolersProjectObservation);
	provider_registry = memnew(SolersProviderRegistry);
	reflection_service = memnew(SolersReflectionService);
	resource_service = memnew(SolersResourceService);
	rpc_server = memnew(SolersRpcServer);
	runtime_observation = memnew(SolersRuntimeObservation);
	scene_observation = memnew(SolersSceneObservation);
	script_service = memnew(SolersScriptService);
	settings_service = memnew(SolersSettingsService);
	tool_registry = memnew(SolersToolRegistry);

	settings_service->set_provider_registry(provider_registry);
	agent_session->set_models_dev(provider_registry->get_models_dev(), false);
	tool_registry->set_project_observation(project_observation);
	tool_registry->set_runtime_observation(runtime_observation);
	tool_registry->set_scene_observation(scene_observation);
	tool_registry->set_reflection_service(reflection_service);
	tool_registry->set_permission_manager(permission_manager);
	tool_registry->set_resource_service(resource_service);
	tool_registry->set_script_service(script_service);
	tool_registry->register_default_tools();

	agent_session->set_settings_service(settings_service);
	agent_session->set_tool_registry(tool_registry);
	agent_session->set_permission_manager(permission_manager);
	agent_session->set_observations(project_observation, scene_observation, runtime_observation);
	agent_session->set_runtime_stop_handler([]() {
		EditorInterface *editor = EditorInterface::get_singleton();
		if (!editor || !editor->is_playing_scene()) {
			return false;
		}
		editor->stop_playing_scene();
		return true;
	});

	scene_observation->set_runtime_observation(runtime_observation);
	mcp_adapter->set_observations(project_observation, scene_observation, runtime_observation);
	mcp_adapter->set_tool_registry(tool_registry);
	mcp_adapter->set_tool_executor(agent_session->get_tool_executor());

	rpc_server->set_mcp_adapter(mcp_adapter);
}

Dictionary SolersAgentRuntime::start_rpc(const Dictionary &p_args) {
	if (shutting_down || !rpc_server) {
		Dictionary error;
		error["code"] = "RUNTIME_SHUTTING_DOWN";
		error["message"] = "Solers runtime is shutting down.";
		return Dictionary({ { "ok", false }, { "error", error } });
	}
	return rpc_server->start(p_args);
}

Dictionary SolersAgentRuntime::stop_rpc() {
	return rpc_server ? rpc_server->stop() : Dictionary({ { "ok", true }, { "data", Dictionary({ { "stopped", true } }) } });
}

Dictionary SolersAgentRuntime::get_rpc_status() const {
	return rpc_server ? rpc_server->get_status() : Dictionary();
}

void SolersAgentRuntime::bind_dock(SolersDock *p_dock) {
	if (!p_dock) {
		return;
	}
	p_dock->set_services(scene_observation, tool_registry, permission_manager, settings_service);
	p_dock->set_agent_session(agent_session);
}

void SolersAgentRuntime::poll() {
	if (shutting_down) {
		return;
	}
	const bool rpc_running = rpc_server && rpc_server->is_running();
	if (!rpc_running && !agent_session && !asset_service) {
		return;
	}
	if (in_poll) {
		SOLERS_TRACE("agent_runtime.poll", "re-entrant poll skipped");
		return;
	}
	in_poll = true;
	if (runtime_observation) {
		runtime_observation->poll();
	}
	if (settings_service) {
		settings_service->poll_auth();
	}
	if (rpc_running) {
		rpc_server->poll();
	}
	if (agent_session && agent_session->is_running()) {
		agent_session->poll();
	}
	if (asset_service) {
		asset_service->poll();
	}
	in_poll = false;
}

bool SolersAgentRuntime::is_running() const {
	return !shutting_down && agent_session && agent_session->is_running();
}

void SolersAgentRuntime::set_project_path(const String &p_project_path) {
	if (!shutting_down && agent_session) {
		agent_session->set_project_path(p_project_path);
	}
}

void SolersAgentRuntime::set_session(const String &p_project_path, const String &p_session_id) {
	if (!shutting_down && agent_session) {
		agent_session->set_session(p_project_path, p_session_id);
	}
}

Dictionary SolersAgentRuntime::get_status() const {
	return agent_session ? agent_session->get_status() : Dictionary();
}

Array SolersAgentRuntime::get_timeline_entries() const {
	return agent_session ? agent_session->get_timeline_entries() : Array();
}

void SolersAgentRuntime::shutdown() {
	if (shutting_down) {
		return;
	}
	shutting_down = true;
	if (rpc_server) {
		rpc_server->stop();
	}
	if (agent_session) {
		agent_session->shutdown();
	}
	if (asset_service) {
		memdelete(asset_service);
		asset_service = nullptr;
	}
}

SolersAgentRuntime::~SolersAgentRuntime() {
	shutdown();
	if (rpc_server) {
		memdelete(rpc_server);
	}
	// Tear down in reverse dependency order after shutdown has stopped
	// admission, released editor subscriptions, and joined asset workers.
	if (agent_session) {
		memdelete(agent_session);
	}
	if (mcp_adapter) {
		memdelete(mcp_adapter);
	}
	if (tool_registry) {
		memdelete(tool_registry);
	}
	if (settings_service) {
		memdelete(settings_service);
	}
	if (script_service) {
		memdelete(script_service);
	}
	if (resource_service) {
		memdelete(resource_service);
	}
	if (reflection_service) {
		memdelete(reflection_service);
	}
	if (provider_registry) {
		memdelete(provider_registry);
	}
	if (permission_manager) {
		memdelete(permission_manager);
	}
	if (scene_observation) {
		memdelete(scene_observation);
	}
	if (runtime_observation) {
		memdelete(runtime_observation);
	}
	if (project_observation) {
		memdelete(project_observation);
	}
}
