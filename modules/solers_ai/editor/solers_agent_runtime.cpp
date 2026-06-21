/**************************************************************************/
/*  solers_agent_runtime.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_agent_runtime.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/editor/solers_dock.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "modules/solers_ai/protocol/solers_rpc_server.h"

SolersAgentRuntime::SolersAgentRuntime() {
	action_timeline = memnew(SolersActionTimeline);
	agent_session = memnew(SolersAgentSession);
	file_checkpoint = memnew(SolersFileCheckpoint);
	mcp_adapter = memnew(SolersMCPAdapter);
	observation_service = memnew(SolersObservationService);
	permission_manager = memnew(SolersPermissionManager);
	provider_registry = memnew(SolersProviderRegistry);
	reflection_service = memnew(SolersReflectionService);
	resource_service = memnew(SolersResourceService);
	rpc_server = memnew(SolersRpcServer);
	script_service = memnew(SolersScriptService);
	settings_service = memnew(SolersSettingsService);
	tool_registry = memnew(SolersToolRegistry);

	file_checkpoint->set_action_timeline(action_timeline);
	script_service->set_action_timeline(action_timeline);
	script_service->set_file_checkpoint(file_checkpoint);
	settings_service->set_provider_registry(provider_registry);
	reflection_service->set_action_timeline(action_timeline);

	tool_registry->set_action_timeline(action_timeline);
	tool_registry->set_observation_service(observation_service);
	tool_registry->set_reflection_service(reflection_service);
	tool_registry->set_permission_manager(permission_manager);
	tool_registry->set_resource_service(resource_service);
	tool_registry->set_script_service(script_service);
	tool_registry->register_default_tools();

	agent_session->set_action_timeline(action_timeline);
	agent_session->set_settings_service(settings_service);
	agent_session->set_tool_registry(tool_registry);
	agent_session->set_permission_manager(permission_manager);

	mcp_adapter->set_action_timeline(action_timeline);
	mcp_adapter->set_observation_service(observation_service);
	mcp_adapter->set_tool_registry(tool_registry);

	rpc_server->set_mcp_adapter(mcp_adapter);
}

void SolersAgentRuntime::bind_dock(SolersDock *p_dock) {
	if (!p_dock) {
		return;
	}
	p_dock->set_services(observation_service, tool_registry, action_timeline, permission_manager, mcp_adapter, rpc_server, settings_service);
	p_dock->set_agent_session(agent_session);
}

void SolersAgentRuntime::poll() {
	if (in_poll) {
		SOLERS_TRACE("agent_runtime.poll", "re-entrant poll skipped");
		return;
	}
	in_poll = true;
	if (rpc_server) {
		rpc_server->poll();
	}
	if (agent_session) {
		agent_session->poll();
	}
	in_poll = false;
}

bool SolersAgentRuntime::is_running() const {
	return agent_session && agent_session->is_running();
}

void SolersAgentRuntime::set_project_path(const String &p_project_path) {
	if (agent_session) {
		agent_session->set_project_path(p_project_path);
	}
}

void SolersAgentRuntime::set_session(const String &p_project_path, const String &p_session_id) {
	if (agent_session) {
		agent_session->set_session(p_project_path, p_session_id);
	}
}

Dictionary SolersAgentRuntime::get_status() const {
	return agent_session ? agent_session->get_status() : Dictionary();
}

Array SolersAgentRuntime::get_messages() const {
	return agent_session ? agent_session->get_messages() : Array();
}

SolersAgentRuntime::~SolersAgentRuntime() {
	if (tool_registry) {
		memdelete(tool_registry);
	}
	if (script_service) {
		memdelete(script_service);
	}
	if (settings_service) {
		memdelete(settings_service);
	}
	if (permission_manager) {
		memdelete(permission_manager);
	}
	if (provider_registry) {
		memdelete(provider_registry);
	}
	if (resource_service) {
		memdelete(resource_service);
	}
	if (rpc_server) {
		rpc_server->stop();
		memdelete(rpc_server);
	}
	if (observation_service) {
		memdelete(observation_service);
	}
	if (mcp_adapter) {
		memdelete(mcp_adapter);
	}
	if (file_checkpoint) {
		memdelete(file_checkpoint);
	}
	if (reflection_service) {
		memdelete(reflection_service);
	}
	if (agent_session) {
		memdelete(agent_session);
	}
	if (action_timeline) {
		memdelete(action_timeline);
	}
}
