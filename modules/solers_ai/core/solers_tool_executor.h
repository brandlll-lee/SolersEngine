/**************************************************************************/
/*  solers_tool_executor.h                                                */
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

#include "modules/solers_ai/core/solers_tool_registry.h"

class SolersPermissionManager;

// Drives one tool call from validated input to one terminal receipt. Hosts
// retain ordering policy; this object owns only the lifecycle of its slot.
class SolersToolExecutor {
public:
	enum State {
		STATE_IDLE,
		STATE_EXECUTING,
		STATE_AWAITING_APPROVAL,
		STATE_CONTINUING,
		STATE_TERMINAL,
	};

private:
	SolersToolRegistry *registry = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	State state = STATE_IDLE;
	StringName tool_name;
	Dictionary arguments;
	Dictionary continuation_arguments;
	SolersToolContext context;
	SolersPreparedToolCall prepared_call;
	bool has_prepared_call = false;
	int approval_id = 0;
	uint64_t deadline_msec = 0;
	SafeFlag cancel_requested;
	Dictionary terminal_result;

	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	bool _prepare(const Dictionary &p_arguments);
	void _accept_result(const Dictionary &p_result);
	void _finish(const Dictionary &p_result);
	void _reset();

public:
	void configure(SolersToolRegistry *p_registry, SolersPermissionManager *p_permission_manager);
	Dictionary start(const StringName &p_name, const Dictionary &p_arguments, const SolersToolContext &p_context, uint64_t p_timeout_msec);
	void poll();
	void cancel(const String &p_code = "TOOL_CANCELLED", const String &p_message = "The tool call was cancelled.");

	State get_state() const { return state; }
	bool is_idle() const { return state == STATE_IDLE; }
	bool is_active() const { return state != STATE_IDLE && state != STATE_TERMINAL; }
	bool is_terminal() const { return state == STATE_TERMINAL; }
	bool is_awaiting_approval() const { return state == STATE_AWAITING_APPROVAL; }
	int get_approval_id() const { return approval_id; }
	String get_call_id() const { return context.call_id; }
	StringName get_tool_name() const { return tool_name; }
	Dictionary take_result();

	~SolersToolExecutor();
};
