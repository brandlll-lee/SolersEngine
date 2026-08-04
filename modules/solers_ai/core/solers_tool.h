/**************************************************************************/
/*  solers_tool.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Tool contract — the single capability interface every agent tool      */
/* implements. Modeled directly on codex's `ToolExecutor` trait and       */
/* opencode's `Tool.define`: a tool owns its name, description, parameter  */
/* schema, model-visibility (exposure), capability metadata, and its       */
/* execution. Registration is data-driven (see SolersFunctionTool), so a   */
/* new capability is one self-contained registration — never a new branch  */
/* in a dispatcher, a new entry in a static catalog, and a new operator    */
/* method (the old three-place pattern this layer replaces).               */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "modules/solers_ai/core/solers_permission_manager.h"

#include <functional>

// Where a tool is exposed to the model. Faithful to codex `ToolExposure`:
// visibility is an authoritative property the tool declares, never inferred
// from its name.
enum class SolersToolExposure {
	// Included in the initial model-visible tool list.
	DIRECT,
	// Registered for later discovery (via tool_search) but omitted from the
	// initial list. Keeps the long tail out of the prompt until needed.
	DEFERRED,
	// Dispatchable but never shown to the model.
	HIDDEN,
};

enum class SolersToolExecution {
	MAIN_THREAD,
	WORKER_THREAD,
};

// How one successful tool mutation can be reversed. This is authoritative:
// the executor never infers reversibility from a tool name or permission.
enum class SolersToolMutationPolicy {
	READ_ONLY,
	EDITOR_UNDO,
	FILE_CHECKPOINT,
	IRREVERSIBLE,
};

// Closed UI presentation kind for chat tool rows. Declared at registration —
// the dock maps kind → glyph/verb and never matches tool name strings.
// DEFAULT means "derive from permission / mutation_policy".
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

// Authoritative capability metadata. Permission/approval/redaction are decided
// from these structured facts — never from matching the tool name. Adding a
// new tool with new risk characteristics needs no change to the orchestrator.
struct SolersToolCapability {
	SolersPermissionManager::Permission permission = SolersPermissionManager::PERMISSION_OBSERVE;
	// Optional parameter-aware permission. This keeps exceptional trust
	// boundaries (for example, approved third-party code installation) in the
	// same authoritative tool definition as its schema and handler.
	std::function<SolersPermissionManager::Permission(const Dictionary &)> permission_resolver;
	SolersToolMutationPolicy mutation_policy = SolersToolMutationPolicy::READ_ONLY;
	// Resolve the reversal policy for a domain tool whose validated operations
	// use different native persistence mechanisms.
	std::function<SolersToolMutationPolicy(const Dictionary &)> mutation_policy_resolver;
	// When true, identical read-only calls reuse the session cache across
	// authored revision bumps (ClassDB/catalog facts do not change with edits).
	bool cache_across_revisions = false;
	// Godot/editor APIs stay on the main thread. Handlers opt into a worker
	// only when they are explicitly thread-safe.
	SolersToolExecution execution = SolersToolExecution::MAIN_THREAD;
	// Optional parameter-aware resolver. Each item is { mode: "read"|"write",
	// key: String }. Tools without one are conservatively treated as touching
	// the global resource; the scheduler never guesses side effects from names.
	std::function<Array(const Dictionary &)> resource_access;
	// Argument keys whose values must be redacted from the timeline/logs
	// (replaces the old per-tool-name `if` redaction in the dispatcher).
	Vector<String> redact_args;
	SolersToolUiKind ui_kind = SolersToolUiKind::DEFAULT;
};

// Per-call context handed to every tool, mirroring opencode's `Tool.Context`.
// Carries identity and the approval token resolved by the orchestrator.
struct SolersToolContext {
	String call_id;
	String session_id;
	String project_path;
	Array mentions;
	uint64_t authored_revision = 0;
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
	virtual SolersToolExposure exposure() const { return SolersToolExposure::DIRECT; }
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
	SolersToolExposure tool_exposure = SolersToolExposure::DIRECT;
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
