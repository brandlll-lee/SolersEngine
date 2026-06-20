/**************************************************************************/
/*  solers_agent_runtime.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"

class SolersActionTimeline;
class SolersAgentSession;
class SolersDock;
class SolersFileCheckpoint;
class SolersMCPAdapter;
class SolersObservationService;
class SolersPermissionManager;
class SolersProviderRegistry;
class SolersReflectionService;
class SolersResourceService;
class SolersRpcServer;
class SolersScriptService;
class SolersSettingsService;
class SolersToolRegistry;

class SolersAgentRuntime {
	SolersActionTimeline *action_timeline = nullptr;
	SolersAgentSession *agent_session = nullptr;
	SolersFileCheckpoint *file_checkpoint = nullptr;
	SolersMCPAdapter *mcp_adapter = nullptr;
	SolersObservationService *observation_service = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	SolersProviderRegistry *provider_registry = nullptr;
	SolersReflectionService *reflection_service = nullptr;
	SolersResourceService *resource_service = nullptr;
	SolersRpcServer *rpc_server = nullptr;
	SolersScriptService *script_service = nullptr;
	SolersSettingsService *settings_service = nullptr;
	SolersToolRegistry *tool_registry = nullptr;
	bool in_poll = false;

public:
	void bind_dock(SolersDock *p_dock);
	void poll();
	bool is_running() const;
	void set_project_path(const String &p_project_path);

	SolersAgentRuntime();
	~SolersAgentRuntime();
};
