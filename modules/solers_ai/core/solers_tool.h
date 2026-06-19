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
	// Visible in the model tool list, but excluded from any nested/batch
	// (code-mode-equivalent) surface.
	DIRECT_MODEL_ONLY,
	// Dispatchable but never shown to the model.
	HIDDEN,
};

// Authoritative capability metadata. Permission/approval/redaction are decided
// from these structured facts — never from matching the tool name. Adding a
// new tool with new risk characteristics needs no change to the orchestrator.
struct SolersToolCapability {
	SolersPermissionManager::Permission permission = SolersPermissionManager::PERMISSION_OBSERVE;
	// Free-form mutation classifier surfaced to the timeline/UI (e.g.
	// "editor_undo_redo", "file_write", "none"). Descriptive only.
	String mutation_kind = "none";
	bool requires_approval = false;
	// True when the mutation is reversible through EditorUndoRedoManager. The
	// user can always Ctrl+Z an undoable tool — Solers' native safety net.
	bool undoable = false;
	// Argument keys whose values must be redacted from the timeline/logs
	// (replaces the old per-tool-name `if` redaction in the dispatcher).
	Vector<String> redact_args;
};

// Per-call context handed to every tool, mirroring opencode's `Tool.Context`.
// Carries identity and the approval token resolved by the orchestrator.
struct SolersToolContext {
	String call_id;
	int approval_id = 0;
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

	virtual ~SolersTool() {}
};

// Data-driven concrete tool. One construction registers the handler, schema and
// metadata together — collapsing the former catalog-entry + dispatcher-branch +
// operator-method triple into a single call site (opencode's `Tool.define`).
class SolersFunctionTool : public SolersTool {
public:
	using Handler = std::function<Dictionary(const SolersToolContext &, const Dictionary &)>;

private:
	StringName tool_name;
	String tool_description;
	Dictionary schema;
	SolersToolExposure tool_exposure = SolersToolExposure::DIRECT;
	SolersToolCapability tool_capability;
	Handler handler;

public:
	StringName name() const override { return tool_name; }
	String description() const override { return tool_description; }
	Dictionary parameters_schema() const override { return schema; }
	SolersToolExposure exposure() const override { return tool_exposure; }
	const SolersToolCapability &capability() const override { return tool_capability; }

	Dictionary execute(const SolersToolContext &p_ctx, const Dictionary &p_args) override {
		return handler(p_ctx, p_args);
	}

	SolersFunctionTool(const StringName &p_name, const String &p_description, const Dictionary &p_schema,
			SolersToolExposure p_exposure, const SolersToolCapability &p_capability, Handler p_handler) :
			tool_name(p_name),
			tool_description(p_description),
			schema(p_schema),
			tool_exposure(p_exposure),
			tool_capability(p_capability),
			handler(std::move(p_handler)) {}
};
