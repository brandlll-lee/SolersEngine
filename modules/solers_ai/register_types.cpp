/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#ifdef TOOLS_ENABLED
#include "core/solers_action_timeline.h"
#include "core/solers_agent_orchestrator.h"
#include "core/solers_agent_runtime.h"
#include "core/solers_editor_operator.h"
#include "core/solers_file_checkpoint.h"
#include "core/solers_observation_service.h"
#include "core/solers_permission_manager.h"
#include "core/solers_provider_gateway.h"
#include "core/solers_provider_registry.h"
#include "core/solers_resource_service.h"
#include "core/solers_script_service.h"
#include "core/solers_settings_service.h"
#include "core/solers_tool_registry.h"
#include "editor/solers_dock.h"
#include "editor/solers_editor_plugin.h"
#include "editor/solers_rml_chat_surface.h"
#include "protocol/solers_mcp_adapter.h"
#include "protocol/solers_rpc_server.h"

#include "editor/plugins/editor_plugin.h"
#endif // TOOLS_ENABLED

void initialize_solers_ai_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(SolersActionTimeline);
		GDREGISTER_CLASS(SolersAgentOrchestrator);
		GDREGISTER_CLASS(SolersAgentRuntime);
		GDREGISTER_CLASS(SolersEditorOperator);
		GDREGISTER_CLASS(SolersFileCheckpoint);
		GDREGISTER_CLASS(SolersObservationService);
		GDREGISTER_CLASS(SolersPermissionManager);
		GDREGISTER_CLASS(SolersProviderGateway);
		GDREGISTER_CLASS(SolersProviderRegistry);
		GDREGISTER_CLASS(SolersResourceService);
		GDREGISTER_CLASS(SolersScriptService);
		GDREGISTER_CLASS(SolersSettingsService);
		GDREGISTER_CLASS(SolersToolRegistry);
		GDREGISTER_CLASS(SolersMCPAdapter);
		GDREGISTER_CLASS(SolersRpcServer);
		GDREGISTER_CLASS(SolersDock);
		GDREGISTER_CLASS(SolersRmlChatSurface);
		GDREGISTER_CLASS(SolersEditorPlugin);
		EditorPlugins::add_by_type<SolersEditorPlugin>();
	}
#endif // TOOLS_ENABLED
}

void uninitialize_solers_ai_module(ModuleInitializationLevel p_level) {
}
