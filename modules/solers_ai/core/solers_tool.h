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
#include "core/templates/vector.h"
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

enum class SolersToolExposure {
	MODEL,
	DEFERRED,
	HIDDEN,
};

enum class SolersOperationDomain {
	NONE,
	EDITOR,
	RUNTIME,
	PIPELINE,
};

enum class SolersOperationMode {
	QUERY,
	APPLY,
};

enum class SolersToolExecution {
	MAIN_THREAD,
	WORKER_THREAD,
};

// How one successful tool mutation can be reversed. This is authoritative:
// the executor never infers reversibility from a tool name or permission.
enum class SolersToolMutationDomain : uint32_t {
	NONE = 0,
	EDITOR = 1 << 0,
	FILES = 1 << 1,
	IRREVERSIBLE = 1 << 2,
};

inline SolersToolMutationDomain operator|(SolersToolMutationDomain p_left, SolersToolMutationDomain p_right) {
	return SolersToolMutationDomain(uint32_t(p_left) | uint32_t(p_right));
}

inline bool solers_has_mutation_domain(SolersToolMutationDomain p_domains, SolersToolMutationDomain p_domain) {
	return (uint32_t(p_domains) & uint32_t(p_domain)) != 0;
}

// Closed UI presentation kind for chat tool rows. Declared at registration —
// the dock maps kind → glyph/verb and never matches tool name strings.
// DEFAULT means "derive from permission / mutation domains".
enum class SolersToolUiKind {
	DEFAULT,
	OBSERVE,
	READ,
	SEARCH,
	WRITE,
	SCENE,
	SHELL,
	RUN,
	NETWORK,
	ASSET,
	CAPTURE,
	THINK,
	SHIELD,
};

struct SolersToolHostPolicy {
	bool timeline_visible = true;
	PackedStringArray attachment_args;
	PackedStringArray required_model_inputs;
};

// Authoritative capability metadata. Permission/approval/redaction are decided
// from these structured facts — never from matching the tool name. Adding a
// new tool with new risk characteristics needs no change to the orchestrator.
struct SolersToolCapability {
	SolersPermissionManager::Permission permission = SolersPermissionManager::PERMISSION_OBSERVE;
	// Optional parameter-aware permission. This keeps exceptional trust
	// boundaries (for example, approved third-party code installation) in the
	// same authoritative tool definition as its schema and handler.
	std::function<SolersPermissionManager::Permission(const Dictionary &)> permission_resolver;
	SolersToolMutationDomain mutation_domains = SolersToolMutationDomain::NONE;
	std::function<SolersToolMutationDomain(const Dictionary &)> mutation_domain_resolver;
	std::function<Dictionary(const Dictionary &)> argument_validator;
	SolersOperationDomain operation_domain = SolersOperationDomain::NONE;
	SolersOperationMode operation_mode = SolersOperationMode::QUERY;
	StringName target_class;
	StringName target_kind;
	// Godot/editor APIs stay on the main thread. Handlers opt into a worker
	// only when they are explicitly thread-safe.
	SolersToolExecution execution = SolersToolExecution::MAIN_THREAD;
	std::function<SolersToolExecution(const Dictionary &)> execution_resolver;
	std::function<bool(const Dictionary &)> execution_ready;
	// Optional parameter-aware resolver. Each item is { mode: "read"|"write",
	// key: String }. Tools without one are conservatively treated as touching
	// the global resource; the scheduler never guesses side effects from names.
	std::function<Array(const Dictionary &)> resource_access;
	// Argument keys whose values must be redacted from the timeline/logs
	// (replaces the old per-tool-name `if` redaction in the dispatcher).
	Vector<String> redact_args;
	SolersToolUiKind ui_kind = SolersToolUiKind::DEFAULT;
	SolersToolHostPolicy host;
};

// Per-call context handed to every tool, mirroring opencode's `Tool.Context`.
// Carries identity and the approval token resolved by the orchestrator.
struct SolersToolContext {
	String call_id;
	String session_id;
	String project_path;
	Array mentions;
	uint64_t authored_revision = 0;
	int result_token_budget = SolersContextManager::TOOL_RESULT_MAX_TOKENS;
	int approval_id = 0;
	const SafeFlag *cancel_requested = nullptr;
};

// The tool interface. Faithful to codex `ToolExecutor` / opencode `Tool.Def`:
// the model-visible spec stays tied to the executable runtime, so routing,
// permissions and telemetry layer on top without reopening the spec/run split.
class SolersTool {
public:
	// Canonical, dotted tool id (e.g. "scene.create"). Stable across the host.
	virtual StringName name() const = 0;
	virtual String description() const = 0;
	// JSON-schema object describing the arguments. Lives next to the handler.
	virtual Dictionary parameters_schema() const = 0;
	virtual SolersToolExposure exposure() const { return SolersToolExposure::MODEL; }
	virtual const SolersToolCapability &capability() const = 0;
	// Execute the call. Returns the canonical { ok, data } / { ok, error }
	// envelope used everywhere in the module.
	virtual Dictionary execute(const SolersToolContext &p_ctx, const Dictionary &p_args) = 0;
	// Continue work started by execute(). The orchestrator calls this only when
	// execute() returned data.status=pending with data.poll_args. Keeping the
	// continuation separate guarantees that a pending call is started once.
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

// Data-driven concrete tool. One construction registers the handler, schema and
// metadata together — collapsing the former catalog-entry + dispatcher-branch +
// operator-method triple into a single call site (opencode's `Tool.define`).
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
	SolersToolExposure tool_exposure = SolersToolExposure::MODEL;
	SolersToolCapability tool_capability;
	Handler handler;
	PollHandler poll_handler;
	ReadyHandler ready_handler;
	CompletionHandler completion_handler;

public:
	StringName name() const override { return tool_name; }
	String description() const override { return tool_description; }
	Dictionary parameters_schema() const override { return schema; }
	SolersToolExposure exposure() const override { return tool_exposure; }
	const SolersToolCapability &capability() const override { return tool_capability; }

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
			SolersToolExposure p_exposure, const SolersToolCapability &p_capability, Handler p_handler, PollHandler p_poll_handler = {}, ReadyHandler p_ready_handler = {}, CompletionHandler p_completion_handler = {}) :
			tool_name(p_name),
			tool_description(p_description),
			schema(p_schema),
			tool_exposure(p_exposure),
			tool_capability(p_capability),
			handler(std::move(p_handler)),
			poll_handler(std::move(p_poll_handler)),
			ready_handler(std::move(p_ready_handler)),
			completion_handler(std::move(p_completion_handler)) {}
};
