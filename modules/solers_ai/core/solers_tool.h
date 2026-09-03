/**************************************************************************/
/*  solers_tool.h                                                         */
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

#include "core/object/object_id.h"
#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_permission_manager.h"

#include <functional>

inline String solers_object_id_to_string(ObjectID p_id) {
	return String::num_int64((int64_t)p_id);
}

inline bool solers_object_id_from_variant(const Variant &p_value, ObjectID &r_id) {
	if (p_value.get_type() != Variant::STRING) {
		return false;
	}
	const String encoded = p_value;
	if (encoded.is_empty() || encoded != encoded.strip_edges() || encoded.begins_with("+") || !encoded.is_valid_int()) {
		return false;
	}
	r_id = ObjectID(encoded.to_int());
	return r_id.is_valid();
}

struct SolersToolContext {
	String call_id;
	String session_id;
	String project_path;
	Array mentions;
	int result_token_budget = SolersContextManager::TOOL_RESULT_MAX_TOKENS;
	int approval_id = 0;
	const SafeFlag *cancel_requested = nullptr;
	std::function<Dictionary(SolersPermissionManager::Permission, const Dictionary &)> permission_gate;

	Dictionary require_permission(SolersPermissionManager::Permission p_permission, const Dictionary &p_details) const {
		if (permission_gate) {
			return permission_gate(p_permission, p_details);
		}
		Dictionary error;
		error["code"] = "PERMISSION_MANAGER_UNAVAILABLE";
		error["message"] = "Solers permission manager is not initialized.";
		error["recoverable"] = false;
		return Dictionary({ { "ok", false }, { "error", error } });
	}
};

class SolersTool {
public:
	virtual StringName name() const = 0;
	virtual String description() const = 0;
	virtual Dictionary parameters_schema() const = 0;
	virtual Dictionary execute(const SolersToolContext &p_ctx, const Dictionary &p_args) = 0;
	virtual Dictionary poll(const SolersToolContext &, const Dictionary &) {
		Dictionary error;
		error["code"] = "TOOL_CONTINUATION_UNAVAILABLE";
		error["message"] = "The tool returned pending without declaring a continuation callback.";
		error["recoverable"] = false;
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	virtual bool is_continuation_ready(const SolersToolContext &, const Dictionary &) const { return true; }
	virtual void complete(const SolersToolContext &, const Dictionary &, const Dictionary &) {}

	virtual ~SolersTool() {}
};

class SolersFunctionTool : public SolersTool {
public:
	using Handler = std::function<Dictionary(const SolersToolContext &, const Dictionary &)>;
	using PollHandler = std::function<Dictionary(const SolersToolContext &, const Dictionary &)>;
	using ReadyHandler = std::function<bool(const SolersToolContext &, const Dictionary &)>;
	using CompletionHandler = std::function<void(const SolersToolContext &, const Dictionary &, const Dictionary &)>;

private:
	StringName tool_name;
	String tool_description;
	Dictionary schema;
	Handler handler;
	PollHandler poll_handler;
	ReadyHandler ready_handler;
	CompletionHandler completion_handler;

public:
	StringName name() const override { return tool_name; }
	String description() const override { return tool_description; }
	Dictionary parameters_schema() const override { return schema; }

	Dictionary execute(const SolersToolContext &p_ctx, const Dictionary &p_args) override {
		return handler(p_ctx, p_args);
	}
	Dictionary poll(const SolersToolContext &p_ctx, const Dictionary &p_args) override {
		return poll_handler ? poll_handler(p_ctx, p_args) : SolersTool::poll(p_ctx, p_args);
	}
	bool is_continuation_ready(const SolersToolContext &p_ctx, const Dictionary &p_args) const override {
		return ready_handler ? ready_handler(p_ctx, p_args) : SolersTool::is_continuation_ready(p_ctx, p_args);
	}
	void complete(const SolersToolContext &p_ctx, const Dictionary &p_args, const Dictionary &p_result) override {
		if (completion_handler) {
			completion_handler(p_ctx, p_args, p_result);
		}
	}

	SolersFunctionTool(const StringName &p_name, const String &p_description, const Dictionary &p_schema,
			Handler p_handler, PollHandler p_poll_handler = {}, ReadyHandler p_ready_handler = {}, CompletionHandler p_completion_handler = {}) :
			tool_name(p_name),
			tool_description(p_description),
			schema(p_schema),
			handler(std::move(p_handler)),
			poll_handler(std::move(p_poll_handler)),
			ready_handler(std::move(p_ready_handler)),
			completion_handler(std::move(p_completion_handler)) {}
};
