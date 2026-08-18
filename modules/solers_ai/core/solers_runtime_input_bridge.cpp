/**************************************************************************/
/*  solers_runtime_input_bridge.cpp                                       */
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

#include "core/debugger/engine_debugger.h"
#include "core/input/input.h"
#include "core/input/input_map.h"
#include "core/math/math_funcs.h"
#include "core/templates/hash_set.h"

static HashSet<StringName> solers_owned_input_actions;

static void _solers_send_input_result(const String &p_call_id, int64_t p_runtime_epoch, bool p_ok, const String &p_code = String(), const String &p_message = String()) {
	Dictionary result;
	result["call_id"] = p_call_id;
	result["runtime_epoch"] = p_runtime_epoch;
	result["ok"] = p_ok;
	if (!p_ok) {
		result["code"] = p_code;
		result["message"] = p_message;
	}
	if (EngineDebugger::is_active()) {
		EngineDebugger::get_singleton()->send_message("solers:input_result", { result });
	}
}

static Error _solers_capture_runtime_input(void *, const String &p_message, const Array &p_args, bool &r_captured) {
	r_captured = p_message == "set_input_actions";
	if (!r_captured) {
		return OK;
	}
	const String call_id = p_args.size() > 0 ? String(p_args[0]) : String();
	const int64_t runtime_epoch = p_args.size() > 1 ? (int64_t)p_args[1] : 0;
	if (p_args.size() != 3 || call_id.is_empty() || runtime_epoch <= 0 || p_args[2].get_type() != Variant::ARRAY) {
		_solers_send_input_result(call_id, runtime_epoch, false, "INVALID_INPUT_REQUEST", "set_input_actions requires call_id, runtime_epoch, and an actions array.");
		return OK;
	}

	Input *input = Input::get_singleton();
	InputMap *input_map = InputMap::get_singleton();
	if (!input || !input_map) {
		_solers_send_input_result(call_id, runtime_epoch, false, "INPUT_UNAVAILABLE", "Godot Input and InputMap must be initialized.");
		return OK;
	}

	const Array actions = p_args[2];
	HashSet<StringName> next_actions;
	for (int i = 0; i < actions.size(); i++) {
		if (actions[i].get_type() != Variant::DICTIONARY) {
			_solers_send_input_result(call_id, runtime_epoch, false, "INVALID_INPUT_ACTION", "Every input action must be an object with name and strength.");
			return OK;
		}
		const Dictionary action = actions[i];
		const StringName name = action.get("name", StringName());
		const Variant strength_value = action.get("strength", Variant());
		if (name.is_empty() || (strength_value.get_type() != Variant::INT && strength_value.get_type() != Variant::FLOAT)) {
			_solers_send_input_result(call_id, runtime_epoch, false, "INVALID_INPUT_ACTION", "Every input action requires a non-empty name and numeric strength.");
			return OK;
		}
		const double strength = strength_value;
		if (!Math::is_finite(strength) || strength <= 0.0 || strength > 1.0 || next_actions.has(name) || !input_map->has_action(name)) {
			_solers_send_input_result(call_id, runtime_epoch, false, "INVALID_INPUT_ACTION", vformat("Input action '%s' is unknown, duplicated, or has strength outside (0, 1].", name));
			return OK;
		}
		next_actions.insert(name);
	}

	for (const StringName &action : solers_owned_input_actions) {
		if (!next_actions.has(action) && input_map->has_action(action)) {
			input->action_release(action);
		}
	}
	for (int i = 0; i < actions.size(); i++) {
		const Dictionary action = actions[i];
		input->action_press(action.get("name", StringName()), (double)action.get("strength", 0.0));
	}
	solers_owned_input_actions = next_actions;
	_solers_send_input_result(call_id, runtime_epoch, true);
	return OK;
}

void solers_runtime_input_bridge_initialize() {
	if (!EngineDebugger::has_capture(SNAME("solers"))) {
		EngineDebugger::register_message_capture(SNAME("solers"), EngineDebugger::Capture(nullptr, _solers_capture_runtime_input));
	}
}

void solers_runtime_input_bridge_uninitialize() {
	Input *input = Input::get_singleton();
	InputMap *input_map = InputMap::get_singleton();
	for (const StringName &action : solers_owned_input_actions) {
		if (input && input_map && input_map->has_action(action)) {
			input->action_release(action);
		}
	}
	solers_owned_input_actions.clear();
	if (EngineDebugger::has_capture(SNAME("solers"))) {
		EngineDebugger::unregister_message_capture(SNAME("solers"));
	}
}
