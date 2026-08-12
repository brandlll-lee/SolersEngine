/**************************************************************************/
/*  solers_agent_runtime.h                                                */
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

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersActionTimeline;
class SolersAgentSession;
class SolersAssetService;
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
	SolersAssetService *asset_service = nullptr;
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
	bool shutting_down = false;

public:
	void bind_dock(SolersDock *p_dock);
	void poll();
	void shutdown();
	bool is_running() const;
	void set_project_path(const String &p_project_path);
	void set_session(const String &p_project_path, const String &p_session_id);
	Dictionary get_status() const;
	Array get_timeline_entries() const;

	SolersAgentRuntime();
	~SolersAgentRuntime();
};
