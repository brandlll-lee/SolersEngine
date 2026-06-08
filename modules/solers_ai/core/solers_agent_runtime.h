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

#include "core/object/object.h"
#include "core/variant/binder_common.h"
#include "core/variant/dictionary.h"

class SolersActionTimeline;
class SolersObservationService;
class SolersToolRegistry;

class SolersAgentRuntime : public Object {
	GDCLASS(SolersAgentRuntime, Object);

public:
	enum AgentState {
		STATE_IDLE,
		STATE_RUNNING,
		STATE_WAITING_FOR_APPROVAL,
		STATE_COMPLETED,
		STATE_FAILED,
		STATE_ABORTED,
	};

private:
	SolersToolRegistry *tool_registry = nullptr;
	SolersObservationService *observation_service = nullptr;
	SolersActionTimeline *action_timeline = nullptr;
	AgentState state = STATE_IDLE;
	int next_turn_id = 1;
	int active_turn_id = 0;
	bool abort_requested = false;
	Dictionary last_result;

	String _state_to_string(AgentState p_state) const;
	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;

protected:
	static void _bind_methods();

public:
	void set_tool_registry(SolersToolRegistry *p_tool_registry);
	void set_observation_service(SolersObservationService *p_observation_service);
	void set_action_timeline(SolersActionTimeline *p_action_timeline);

	Dictionary start_turn(const Dictionary &p_args);
	Dictionary run_tool_batch(const Array &p_tool_calls);
	void abort_current_turn();
	Dictionary get_status() const;
};

VARIANT_ENUM_CAST(SolersAgentRuntime::AgentState);
