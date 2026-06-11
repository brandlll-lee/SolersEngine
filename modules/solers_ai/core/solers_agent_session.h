/**************************************************************************/
/*  solers_agent_session.h                                                */
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
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersToolRegistry;
class SolersActionTimeline;
class SolersSettingsService;
class SolersLLMProtocolRegistry;
class SolersLLMProviderCatalog;
class SolersLLMClient;

// ---------------------------------------------------------------------------
// SolersAgentSession — the single agent loop.
//
// Replaces the old two-track design (SolersAgentRuntime executed tool batches,
// SolersAgentOrchestrator ran a separate plan/execute loop that never reached a
// real model). One session owns one conversation and runs the canonical loop:
//
//   user prompt
//     -> request model (canonical LLMRequest with the tool catalogue)
//     -> stream assistant text + tool calls (LLMEvents, via SolersLLMClient)
//     -> if the model asked for tools: execute them through the tool registry
//        (permissions, checkpoints and timeline all still apply), append the
//        results, and request the model again
//     -> repeat until the model finishes without requesting tools
//
// It is non-blocking: drive it by calling `poll()` each frame. Streaming and
// lifecycle are surfaced as signals so the dock can render incrementally.
// ---------------------------------------------------------------------------
class SolersAgentSession : public Object {
	GDCLASS(SolersAgentSession, Object);

	SolersToolRegistry *tool_registry = nullptr;
	SolersSettingsService *settings_service = nullptr;
	SolersActionTimeline *action_timeline = nullptr;

	SolersLLMProtocolRegistry *protocol_registry = nullptr; // owned
	SolersLLMProviderCatalog *provider_catalog = nullptr; // owned
	SolersLLMClient *client = nullptr; // owned

	Array messages; // canonical conversation history
	String system_prompt;
	String current_text; // assistant text accumulated this model turn
	String current_reasoning; // reasoning/thinking text accumulated this model turn
	Array pending_tool_calls; // tool calls collected this model turn
	String last_stop_reason;
	Dictionary last_usage;
	Dictionary active_provider; // { provider, model, base_url, api_key }
	bool running = false;
	int turn_id = 0;
	int tool_iterations = 0;
	int max_tool_iterations = 8;

	String _default_system_prompt() const;
	Array _collect_tools() const;
	Dictionary _build_request() const;
	Error _dispatch_model_request();
	void _on_model_turn_complete();
	void _record(const String &p_event, const Dictionary &p_payload) const;
	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message) const;

protected:
	static void _bind_methods();

public:
	void set_tool_registry(SolersToolRegistry *p_tool_registry) { tool_registry = p_tool_registry; }
	void set_settings_service(SolersSettingsService *p_settings_service) { settings_service = p_settings_service; }
	void set_action_timeline(SolersActionTimeline *p_action_timeline) { action_timeline = p_action_timeline; }

	Dictionary start_turn(const Dictionary &p_args); // { prompt: String }
	void poll();
	void abort();
	void reset_conversation();
	Dictionary get_status() const;
	bool is_running() const { return running; }

	SolersAgentSession();
	~SolersAgentSession();
};
