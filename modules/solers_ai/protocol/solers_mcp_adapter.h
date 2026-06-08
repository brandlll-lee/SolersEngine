/**************************************************************************/
/*  solers_mcp_adapter.h                                                  */
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
#include "core/variant/dictionary.h"

class SolersActionTimeline;
class SolersAgentOrchestrator;
class SolersAgentRuntime;
class SolersObservationService;
class SolersToolRegistry;

class SolersMCPAdapter : public Object {
	GDCLASS(SolersMCPAdapter, Object);

	SolersToolRegistry *tool_registry = nullptr;
	SolersObservationService *observation_service = nullptr;
	SolersActionTimeline *action_timeline = nullptr;
	SolersAgentRuntime *agent_runtime = nullptr;
	SolersAgentOrchestrator *agent_orchestrator = nullptr;

	Dictionary _jsonrpc_result(const Variant &p_id, const Variant &p_result) const;
	Dictionary _jsonrpc_error(const Variant &p_id, int p_code, const String &p_message, const Variant &p_data = Variant()) const;
	Dictionary _content_text(const String &p_text) const;
	Array _tool_definitions_for_mcp() const;
	Dictionary _resource(const String &p_uri, const String &p_name, const String &p_description, const String &p_mime_type = "application/json") const;

protected:
	static void _bind_methods();

public:
	void set_tool_registry(SolersToolRegistry *p_tool_registry);
	void set_observation_service(SolersObservationService *p_observation_service);
	void set_action_timeline(SolersActionTimeline *p_action_timeline);
	void set_agent_runtime(SolersAgentRuntime *p_agent_runtime);
	void set_agent_orchestrator(SolersAgentOrchestrator *p_agent_orchestrator);

	Dictionary handle_request(const Dictionary &p_request);
	Dictionary initialize(const Dictionary &p_params) const;
	Dictionary list_tools() const;
	Dictionary call_tool(const Dictionary &p_params);
	Dictionary list_resources() const;
	Dictionary read_resource(const Dictionary &p_params) const;
	Dictionary list_prompts() const;
	Dictionary get_status() const;
};
