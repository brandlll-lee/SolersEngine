/**************************************************************************/
/*  solers_tool_history.cpp                                               */
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

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/templates/hash_set.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/run/editor_run_bar.h"
#include "editor/run/game_view_plugin.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_builtin_skills.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/plugins/solers_plugin.h"

bool SolersToolRegistry::_mutation_record_has_domain(const Dictionary &p_record, const String &p_domain) {
	return PackedStringArray(p_record.get("domains", PackedStringArray())).has(p_domain);
}

static UndoRedo *_current_scene_undo_redo(int &r_history_id) {
	r_history_id = EditorUndoRedoManager::INVALID_HISTORY;
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	if (!manager) {
		return nullptr;
	}
	r_history_id = EditorNode::get_singleton() && EditorNode::get_editor_data().get_edited_scene_count() > 0 ? EditorNode::get_editor_data().get_current_edited_scene_history_id() : EditorUndoRedoManager::GLOBAL_HISTORY;
	if (r_history_id == EditorUndoRedoManager::INVALID_HISTORY) {
		return nullptr;
	}
	return manager->get_or_create_history(r_history_id).undo_redo;
}

Dictionary SolersToolRegistry::_scene_state_receipt() {
	Dictionary receipt;
	EditorNode *editor = EditorNode::get_singleton();
	if (!editor || EditorNode::get_editor_data().get_edited_scene_count() == 0) {
		return receipt;
	}
	Node *root = editor->get_edited_scene();
	const String path = root ? root->get_scene_file_path() : String();
	const int history_id = EditorNode::get_editor_data().get_current_edited_scene_history_id();
	receipt["has_root"] = root != nullptr;
	if (root) {
		receipt["root_object_id"] = solers_object_id_to_string(root->get_instance_id());
	}
	receipt["scene_path"] = path;
	receipt["history_id"] = history_id;
	if (EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton()) {
		if (history_id != EditorUndoRedoManager::INVALID_HISTORY) {
			receipt["version"] = (int64_t)manager->get_or_create_history(history_id).undo_redo->get_version();
		}
	}
	if (!path.is_empty()) {
		const ResourceUID::ID uid = ResourceLoader::get_resource_uid(path);
		if (uid != ResourceUID::INVALID_ID) {
			receipt["resource_uid"] = ResourceUID::get_singleton()->id_to_text(uid);
		}
		if (FileAccess::exists(path)) {
			receipt["saved_sha256"] = FileAccess::get_sha256(path);
		}
	}
	return receipt;
}

static Dictionary _solers_checkpoint_target_state(const SolersFileCheckpoint *p_service, const String &p_path) {
	return p_service ? Dictionary(p_service->get_path_state(p_path).get("data", Dictionary())) : Dictionary();
}

Dictionary SolersToolRegistry::_resource_state_receipt(const SolersFileCheckpoint *p_service, const String &p_path) {
	const Dictionary state = _solers_checkpoint_target_state(p_service, p_path);
	Dictionary receipt;
	receipt["path"] = p_path;
	const bool exists = state.get("existed", false);
	receipt["exists"] = exists;
	if (exists) {
		receipt["sha256"] = state.get("content_sha256", String());
		receipt["directory"] = state.get("directory", false);
	}
	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(p_path);
	if (uid != ResourceUID::INVALID_ID) {
		receipt["resource_uid"] = ResourceUID::get_singleton()->id_to_text(uid);
	}
	return receipt;
}

static bool _solers_checkpoint_matches(const SolersFileCheckpoint *p_service, const Dictionary &p_checkpoint, bool p_after) {
	const Dictionary state = _solers_checkpoint_target_state(p_service, p_checkpoint.get("path", String()));
	const bool expected_exists = p_checkpoint.get(p_after ? "exists_after" : "existed", false);
	const String expected_sha = p_checkpoint.get(p_after ? "sha256_after" : "content_sha256", String());
	return (bool)state.get("existed", false) == expected_exists && (!expected_exists || String(state.get("content_sha256", String())) == expected_sha);
}

static Array _solers_operation_targets(const Dictionary &p_data) {
	Array targets;
	Dictionary target;
	for (const char *field : { "node_path", "object_id", "class_name", "native_facts", "path", "sha256" }) {
		if (p_data.has(field)) {
			target[field] = p_data[field];
		}
	}
	if (!target.is_empty()) {
		targets.push_back(target);
	}
	return targets;
}

static bool _solers_scene_state_matches(const Dictionary &p_expected, const Dictionary &p_actual) {
	if ((int64_t)p_expected.get("history_id", -1) != (int64_t)p_actual.get("history_id", -2) ||
			(int64_t)p_expected.get("version", -1) != (int64_t)p_actual.get("version", -2)) {
		return false;
	}
	return !p_expected.has("root_object_id") || p_expected.get("root_object_id", String()) == p_actual.get("root_object_id", String());
}

Dictionary SolersToolRegistry::_prepare_reversal(SolersPreparedToolCall &r_call) {
	if (r_call.mutation_domains == SolersToolMutationDomain::NONE) {
		return Dictionary();
	}

	Dictionary state;
	state["domains"] = _mutation_domain_names(r_call.mutation_domains);
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::IRREVERSIBLE)) {
		state["scene_state_before"] = _scene_state_receipt();
		r_call.reversal_state = state;
		return Dictionary();
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR)) {
		int history_id = EditorUndoRedoManager::INVALID_HISTORY;
		UndoRedo *undo_redo = _current_scene_undo_redo(history_id);
		if (!undo_redo) {
			return _tool_result_envelope(_error("UNDO_HISTORY_UNAVAILABLE", "The current edited scene has no UndoRedo history.", true), r_call.context.call_id);
		}
		state["history_id"] = history_id;
		state["version_before"] = (int64_t)undo_redo->get_version();
		state["scene_state_before"] = _scene_state_receipt();
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
		if (!file_checkpoint) {
			return _tool_result_envelope(_error("CHECKPOINT_SERVICE_UNAVAILABLE", "The file checkpoint service is not initialized.", false), r_call.context.call_id);
		}
		Array checkpoints;
		Array resource_states_before;
		HashSet<String> seen_paths;
		const Array accesses = resolve_resource_access(r_call.name, r_call.args);
		for (int i = 0; i < accesses.size(); i++) {
			const Dictionary access = accesses[i];
			const String key = access.get("key", String());
			if (String(access.get("mode", "write")) != "write" || !key.begins_with("project:res://")) {
				continue;
			}
			const String path = key.trim_prefix("project:");
			if (seen_paths.has(path)) {
				continue;
			}
			seen_paths.insert(path);
			const Dictionary checkpoint = file_checkpoint->create_checkpoint(path, vformat("Solers tool %s", r_call.name));
			if (!(bool)checkpoint.get("ok", false)) {
				for (const Variant &prepared : checkpoints) {
					file_checkpoint->discard_checkpoint_state(prepared);
				}
				return _tool_result_envelope(checkpoint, r_call.context.call_id);
			}
			checkpoints.push_back(checkpoint.get("data", Dictionary()));
			resource_states_before.push_back(_resource_state_receipt(file_checkpoint, path));
		}
		if (checkpoints.is_empty()) {
			return _tool_result_envelope(_error("CHECKPOINT_TARGET_UNDECLARED", vformat("Tool '%s' must declare concrete project file write targets.", r_call.name), false), r_call.context.call_id);
		}
		state["checkpoints"] = checkpoints;
		state["resource_states_before"] = resource_states_before;
	}
	r_call.reversal_state = state;
	return Dictionary();
}

void SolersToolRegistry::_discard_reversal(const Dictionary &p_record) {
	if (!file_checkpoint) {
		return;
	}
	const Array checkpoints = p_record.get("checkpoints", Array());
	for (int i = 0; i < checkpoints.size(); i++) {
		file_checkpoint->discard_checkpoint_state(checkpoints[i]);
	}
}

Dictionary SolersToolRegistry::_finalize_prepared_result(SolersPreparedToolCall &r_call, const Dictionary &p_result) {
	Dictionary result = p_result.duplicate(true);
	const Dictionary pending_data = result.get("data", Dictionary());
	if ((bool)result.get("ok", false) && String(pending_data.get("status", String())) == "pending") {
		return result;
	}
	if (r_call.mutation_domains == SolersToolMutationDomain::NONE) {
		return result;
	}

	auto rollback = [&]() -> bool {
		bool restored = true;
		if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR)) {
			const int history_id = r_call.reversal_state.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
			const uint64_t version_before = (int64_t)r_call.reversal_state.get("version_before", 0);
			EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
			UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
			while (manager && undo_redo && undo_redo->get_version() > version_before && manager->undo_history(history_id)) {
			}
			restored = restored && undo_redo && undo_redo->get_version() == version_before;
		}
		if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
			const Array checkpoints = r_call.reversal_state.get("checkpoints", Array());
			for (int i = checkpoints.size() - 1; i >= 0; i--) {
				const Dictionary checkpoint = checkpoints[i];
				if (!_solers_checkpoint_matches(file_checkpoint, checkpoint, false) && (!file_checkpoint || !(bool)file_checkpoint->restore_checkpoint_state(checkpoint).get("ok", false))) {
					restored = false;
				}
			}
			_discard_reversal(r_call.reversal_state);
		}
		return restored;
	};

	if (!(bool)result.get("ok", false)) {
		if (!rollback()) {
			Dictionary rollback_error = _error("TOOL_ROLLBACK_FAILED", vformat("Tool '%s' failed and its previous state could not be restored.", r_call.name), false);
			Dictionary error = rollback_error.get("error", Dictionary());
			error["original_error"] = result.get("error", Dictionary());
			rollback_error["error"] = error;
			return _tool_result_envelope(rollback_error, r_call.context.call_id);
		}
		return result;
	}
	Dictionary data = result.get("data", Dictionary());
	const bool checkpoint_consumed = data.get("checkpoint_consumed", false);
	if (checkpoint_consumed) {
		r_call.journal_event["event_type"] = "checkpoint_consumed";
		r_call.journal_event["reversal_id"] = data.get("reversal_id", String());
	}
	bool changed = solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::IRREVERSIBLE) && (bool)data.get("authored_state_changed", false);
	bool editor_changed = false;
	bool files_changed = false;
	Dictionary record = r_call.reversal_state.duplicate(true);
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR)) {
		const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
		const uint64_t version_before = (int64_t)record.get("version_before", 0);
		EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
		UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
		const uint64_t version_after = undo_redo ? undo_redo->get_version() : version_before;
		editor_changed = version_after != version_before;
		changed = changed || editor_changed;
		if (version_after != version_before + 1) {
			Dictionary failure = _error("TOOL_UNDO_CONTRACT_VIOLATION", vformat("Tool '%s' must commit exactly one UndoRedo action.", r_call.name), false);
			failure["data"] = Dictionary({ { "history_id", history_id }, { "version_before", (int64_t)version_before }, { "version_after", (int64_t)version_after }, { "scene_before", record.get("scene_state_before", Dictionary()) }, { "scene_after", _scene_state_receipt() } });
			rollback();
			return _tool_result_envelope(failure, r_call.context.call_id);
		}
		record["version_after"] = (int64_t)version_after;
	}
	if (editor_changed && affects_scene_state(r_call.name, r_call.args)) {
		EditorInterface *editor = EditorInterface::get_singleton();
		Node *root = editor ? editor->get_edited_scene_root() : nullptr;
		String scene_path = root ? root->get_scene_file_path() : String();
		if (scene_path.is_empty()) {
			scene_path = String(r_call.args.get("save_path", String())).strip_edges();
		}
		if (!root || !scene_path.begins_with("res://") || (scene_path.get_extension().to_lower() != "tscn" && scene_path.get_extension().to_lower() != "scn")) {
			rollback();
			return _tool_result_envelope(_error("SCENE_PATH_REQUIRED", "An unsaved scene transaction requires an explicit res://*.tscn or res://*.scn save_path.", false), r_call.context.call_id);
		}
		if (root->get_scene_file_path().is_empty()) {
			editor->save_scene_as(scene_path);
		} else {
			editor->save_scene();
		}
		const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
		EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
		const bool persisted = FileAccess::exists(scene_path) && root->get_scene_file_path() == scene_path && manager && !manager->is_history_unsaved(history_id);
		if (!persisted) {
			rollback();
			return _tool_result_envelope(_error("SCENE_SAVE_FAILED", "Godot did not confirm the scene path, disk file, and UndoRedo saved version; the action was rolled back.", false), r_call.context.call_id);
		}
		data["persisted"] = true;
		data["path"] = scene_path;
		data["saved_sha256"] = FileAccess::get_sha256(scene_path);
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
		Array checkpoints = record.get("checkpoints", Array());
		for (int i = 0; i < checkpoints.size(); i++) {
			Dictionary checkpoint = checkpoints[i];
			const Dictionary state_after = _solers_checkpoint_target_state(file_checkpoint, checkpoint.get("path", String()));
			const bool exists_after = state_after.get("existed", false);
			const String sha_after = state_after.get("content_sha256", String());
			checkpoint["exists_after"] = exists_after;
			checkpoint["sha256_after"] = sha_after;
			Dictionary settings_after;
			const Dictionary settings_before = checkpoint.get("project_settings", Dictionary());
			for (const Variant *setting = settings_before.next(nullptr); setting; setting = settings_before.next(setting)) {
				settings_after[*setting] = ProjectSettings::get_singleton()->get(*setting);
			}
			if (!settings_after.is_empty()) {
				checkpoint["project_settings_after"] = settings_after;
			}
			files_changed = files_changed || exists_after != (bool)checkpoint.get("existed", false) || (exists_after && sha_after != String(checkpoint.get("content_sha256", String())));
			checkpoints[i] = checkpoint;
		}
		record["checkpoints"] = checkpoints;
		changed = changed || files_changed;
	}
	if (!changed) {
		_discard_reversal(record);
		data["authored_state_changed"] = false;
		result["data"] = data;
		return result;
	}

	Dictionary receipt;
	receipt["call_id"] = r_call.context.call_id;
	receipt["domains"] = _mutation_domain_names(r_call.mutation_domains);
	const Array targets = _solers_operation_targets(data);
	if (!targets.is_empty()) {
		receipt["targets"] = targets;
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR)) {
		receipt["scene_before"] = r_call.reversal_state.get("scene_state_before", Dictionary());
		receipt["scene_after"] = _scene_state_receipt();
		data.erase("results");
		data.erase("state_before");
		data.erase("state_after");
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
		receipt["resources_before"] = r_call.reversal_state.get("resource_states_before", Array());
		Array resources_after;
		const Array checkpoints = record.get("checkpoints", Array());
		for (int i = 0; i < checkpoints.size(); i++) {
			resources_after.push_back(_resource_state_receipt(file_checkpoint, Dictionary(checkpoints[i]).get("path", String())));
		}
		receipt["resources_after"] = resources_after;
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::IRREVERSIBLE)) {
		receipt["scene_before"] = r_call.reversal_state.get("scene_state_before", Dictionary());
		receipt["scene_after"] = _scene_state_receipt();
	}
	record["receipt"] = receipt;

	data["authored_state_changed"] = true;
	Dictionary mutation;
	mutation["session_revision"] = (int64_t)(r_call.context.authored_revision + 1);
	mutation["domains"] = _mutation_domain_names(r_call.mutation_domains);
	mutation["receipt"] = receipt;
	const String session_key = r_call.context.session_id.is_empty() ? String("direct") : r_call.context.session_id;
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR) || solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
		Vector<Dictionary> &stack = reversals_by_session[session_key];
		const String reversal_id = (session_key + ":" + r_call.context.call_id + ":" + String::num_uint64(r_call.context.authored_revision + 1) + ":" + String::num_int64(stack.size() + 1)).sha256_text();
		record["id"] = reversal_id;
		record["session_id"] = session_key;
		record["session_revision"] = (int64_t)(r_call.context.authored_revision + 1);
		stack.push_back(record);
		mutation["reversal_id"] = reversal_id;
		r_call.journal_event["event_type"] = "checkpoint_created";
		r_call.journal_event["checkpoint"] = record;
		r_call.journal_event["note"] = "Protective checkpoint for history.revert; not a rollback of your edit.";
	} else if (!checkpoint_consumed) {
		Vector<Dictionary> &stack = reversals_by_session[session_key];
		for (const Dictionary &previous : stack) {
			_discard_reversal(previous);
		}
		stack.clear();
		Dictionary barrier = record;
		barrier["id"] = (session_key + ":barrier:" + String::num_uint64(r_call.context.authored_revision + 1)).sha256_text();
		barrier["session_id"] = session_key;
		barrier["session_revision"] = (int64_t)(r_call.context.authored_revision + 1);
		stack.push_back(barrier);
		r_call.journal_event["event_type"] = "checkpoint_cleared";
		r_call.journal_event["barrier"] = barrier;
	}
	data["mutation"] = mutation;
	result["data"] = data;
	return result;
}

const Dictionary *SolersToolRegistry::_find_reversal(const String &p_reversal_id) const {
	for (const KeyValue<String, Vector<Dictionary>> &session : reversals_by_session) {
		for (const Dictionary &record : session.value) {
			if (String(record.get("id", String())) == p_reversal_id) {
				return &record;
			}
		}
	}
	return nullptr;
}

Dictionary SolersToolRegistry::_revert_latest(const SolersToolContext &p_context, const Dictionary &p_args) {
	const String reversal_id = String(p_args.get("reversal_id", String())).strip_edges();
	const String session_key = p_context.session_id.is_empty() ? String("direct") : p_context.session_id;
	Vector<Dictionary> *stack = reversals_by_session.getptr(session_key);
	if (!stack || stack->is_empty()) {
		return _error("REVERSAL_NOT_FOUND", "The reversal id is unknown or has already been used.");
	}
	const Dictionary record = stack->get(stack->size() - 1);
	if (String(record.get("id", String())) != reversal_id || String(record.get("session_id", String())) != session_key) {
		return _error("STALE_REVERSAL", "Only the latest Agent mutation at the current revision can be reverted.");
	}

	const uint64_t target_revision = MAX((int64_t)record.get("session_revision", 1) - 1, (int64_t)0);
	const Dictionary prepared = prepare_session_rewind(session_key, target_revision);
	if (!(bool)prepared.get("ok", false)) {
		return prepared;
	}
	Dictionary transaction = prepared.get("data", Dictionary());
	transaction["records"] = Array({ record });
	const Dictionary applied = apply_session_rewind(transaction);
	if (!(bool)applied.get("ok", false)) {
		abort_session_rewind(transaction);
		return applied;
	}
	finish_session_rewind(transaction);
	Dictionary data;
	data["reversal_id"] = reversal_id;
	data["reverted_session_revision"] = record.get("session_revision", 0);
	data["checkpoint_consumed"] = true;
	data["authored_state_changed"] = true;
	return _ok(data);
}

Dictionary SolersToolRegistry::_validate_expected_state(const SolersTool *p_tool, const Dictionary &p_args) const {
	const SolersToolCapability &capability = p_tool->capability();
	const SolersToolMutationDomain domains = capability.mutation_domain_resolver ? capability.mutation_domain_resolver(p_args) : capability.mutation_domains;
	const Dictionary expected = p_args.get("expected_state", Dictionary());
	if (expected.is_empty()) {
		return Dictionary();
	}
	if (solers_has_mutation_domain(domains, SolersToolMutationDomain::EDITOR)) {
		const Dictionary actual = _scene_state_receipt();
		if (!_solers_scene_state_matches(expected, actual)) {
			Dictionary failure = _error("SCENE_STATE_CONFLICT", "The edited scene changed since it was inspected.");
			failure["data"] = Dictionary({ { "expected_state", expected }, { "actual_state", actual } });
			return failure;
		}
	}
	if (!solers_has_mutation_domain(domains, SolersToolMutationDomain::FILES)) {
		return Dictionary();
	}
	const String scene_path = expected.get("scene_path", String());
	const String saved_sha256 = expected.get("saved_sha256", String());
	const bool has_scene_file_receipt = !scene_path.is_empty() && !saved_sha256.is_empty();
	if (!expected.has("resources") && !has_scene_file_receipt) {
		return _error("RESOURCE_STATE_INVALID", "expected_state.resources must contain native file receipts.");
	}
	Dictionary expected_resources;
	for (const Variant &item : Array(expected.get("resources", Array()))) {
		const Dictionary receipt = item;
		const String path = receipt.get("path", String());
		if (path.is_empty() || expected_resources.has(path)) {
			return _error("RESOURCE_STATE_INVALID", "expected_state.resources must contain unique resource paths.");
		}
		expected_resources[path] = receipt;
	}
	if (has_scene_file_receipt && !expected_resources.has(scene_path)) {
		expected_resources[scene_path] = Dictionary({ { "path", scene_path }, { "exists", true }, { "sha256", saved_sha256 } });
	}
	for (const Variant &item : resolve_resource_access(p_tool->name(), p_args)) {
		const Dictionary access = item;
		const String key = access.get("key", String());
		if (String(access.get("mode", "write")) != "write" || !key.begins_with("project:res://")) {
			continue;
		}
		const String path = key.trim_prefix("project:");
		if (!expected_resources.has(path)) {
			return _error("RESOURCE_STATE_INVALID", vformat("expected_state.resources is missing the native receipt for %s.", path));
		}
		const Dictionary expected_resource = expected_resources[path];
		const Dictionary actual_resource = _resource_state_receipt(file_checkpoint, path);
		const bool exists = expected_resource.get("exists", false);
		if (exists != (bool)actual_resource.get("exists", false) ||
				(exists && expected_resource.get("sha256", String()) != actual_resource.get("sha256", String()))) {
			Dictionary failure = _error("RESOURCE_STATE_CONFLICT", vformat("Resource changed since it was inspected: %s", path));
			failure["data"] = Dictionary({ { "expected_state", expected_resource }, { "actual_state", actual_resource } });
			return failure;
		}
	}
	return Dictionary();
}

void SolersToolRegistry::clear_task_state(const String &p_session_id) {
	if (p_session_id.is_empty()) {
		return;
	}
	Vector<String> delivery_keys;
	const String delivery_prefix = p_session_id + ":";
	for (const String &key : delivered_addon_contracts) {
		if (key.begins_with(delivery_prefix)) {
			delivery_keys.push_back(key);
		}
	}
	for (const String &key : delivery_keys) {
		delivered_addon_contracts.erase(key);
	}
}

void SolersToolRegistry::restore_session_reversals(const String &p_session_id, const Array &p_records) {
	if (p_session_id.is_empty()) {
		return;
	}
	reversals_by_session.erase(p_session_id);
	Vector<Dictionary> restored;
	for (const Variant &item : p_records) {
		if (item.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary record = item;
		if (!String(record.get("id", String())).is_empty() && String(record.get("session_id", String())) == p_session_id) {
			restored.push_back(record.duplicate(true));
		}
	}
	if (!restored.is_empty()) {
		reversals_by_session[p_session_id] = restored;
	}
}
Dictionary SolersToolRegistry::preview_session_rewind(const String &p_session_id, uint64_t p_target_revision) const {
	const Vector<Dictionary> *stack = reversals_by_session.getptr(p_session_id);
	Array records;
	if (stack) {
		for (const Dictionary &record : *stack) {
			if ((uint64_t)(int64_t)record.get("session_revision", 0) > p_target_revision) {
				records.push_back(record.duplicate(true));
			}
		}
	}
	HashMap<int, int64_t> expected_history_versions;
	HashMap<String, Dictionary> expected_file_states;
	HashMap<StringName, Variant> expected_project_settings;
	HashSet<String> files;
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	for (int i = records.size() - 1; i >= 0; i--) {
		const Dictionary record = records[i];
		const bool editor_domain = _mutation_record_has_domain(record, "editor");
		const bool files_domain = _mutation_record_has_domain(record, "files");
		if (_mutation_record_has_domain(record, "irreversible")) {
			return _error("REWIND_IRREVERSIBLE_BOUNDARY", "An irreversible Agent mutation exists after this message.", false);
		}
		if (!editor_domain && !files_domain) {
			return _error("REWIND_DOMAIN_UNSUPPORTED", "The mutation receipt has no supported reversal domain.", false);
		}
		if (editor_domain) {
			const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
			const int64_t before = record.get("version_before", 0);
			const int64_t after = record.get("version_after", 0);
			const int64_t *expected = expected_history_versions.getptr(history_id);
			UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
			if ((!expected && (!undo_redo || (int64_t)undo_redo->get_version() != after)) || (expected && *expected != after)) {
				return _error("REWIND_NATIVE_HISTORY_CONFLICT", "Godot's UndoRedo history changed after the selected message.", false);
			}
			if (!expected) {
				const Dictionary expected_scene = Dictionary(record.get("receipt", Dictionary())).get("scene_after", Dictionary());
				const Dictionary current_scene = _scene_state_receipt();
				for (const char *field : { "root_object_id", "scene_path", "history_id", "version" }) {
					if (expected_scene.has(field) && current_scene.get(field, Variant()) != expected_scene[field]) {
						return _error("REWIND_SCENE_CONFLICT", vformat("Godot scene fact changed: %s", field), false);
					}
				}
				const String scene_path = expected_scene.get("scene_path", String());
				if (!scene_path.is_empty()) {
					files.insert(scene_path);
				}
				const Dictionary *projected_file = expected_file_states.getptr(scene_path);
				const String expected_sha = expected_scene.get("saved_sha256", String());
				const String observed_sha = projected_file ? String(projected_file->get("sha256", String())) : String(current_scene.get("saved_sha256", String()));
				if (!expected_sha.is_empty() && observed_sha != expected_sha) {
					return _error("REWIND_SCENE_CONFLICT", "The saved scene SHA-256 changed after the selected message.", false);
				}
			}
			expected_history_versions[history_id] = before;
		}
		const Array checkpoints = files_domain ? Array(record.get("checkpoints", Array())) : Array();
		for (const Variant &item : checkpoints) {
			const Dictionary checkpoint = item;
			const String path = checkpoint.get("path", String());
			const bool after_exists = checkpoint.get("exists_after", false);
			const String after_sha = checkpoint.get("sha256_after", String());
			const Dictionary *expected = expected_file_states.getptr(path);
			if (expected) {
				if ((bool)expected->get("exists", false) != after_exists || (after_exists && String(expected->get("sha256", String())) != after_sha)) {
					return _error("REWIND_CHECKPOINT_CHAIN_BROKEN", vformat("The recorded file history is not contiguous: %s", path), false);
				}
			} else {
				const Dictionary current = _solers_checkpoint_target_state(file_checkpoint, path);
				if ((bool)current.get("existed", false) != after_exists || (after_exists && String(current.get("content_sha256", String())) != after_sha)) {
					return _error("REWIND_FILE_CONFLICT", vformat("File changed outside the recorded Agent history: %s", path), false);
				}
			}
			const String checkpoint_path = checkpoint.get("checkpoint_path", String());
			if ((bool)checkpoint.get("existed", false) && ((bool)checkpoint.get("directory", false) ? !DirAccess::exists(checkpoint_path) : !FileAccess::exists(checkpoint_path))) {
				return _error("REWIND_CHECKPOINT_MISSING", vformat("A required checkpoint is missing: %s", path), false);
			}
			const Dictionary settings_after = checkpoint.get("project_settings_after", Dictionary());
			for (const Variant *setting = settings_after.next(nullptr); setting; setting = settings_after.next(setting)) {
				const StringName name = *setting;
				const Variant *expected_setting = expected_project_settings.getptr(name);
				const Variant actual = expected_setting ? *expected_setting : ProjectSettings::get_singleton()->get(name);
				if (actual != settings_after[*setting]) {
					return _error("REWIND_PROJECT_SETTINGS_CONFLICT", vformat("Project setting changed after the Agent mutation: %s", name), false);
				}
			}
			Dictionary before_state;
			before_state["exists"] = checkpoint.get("existed", false);
			before_state["sha256"] = checkpoint.get("content_sha256", String());
			expected_file_states[path] = before_state;
			const Dictionary settings_before = checkpoint.get("project_settings", Dictionary());
			for (const Variant *setting = settings_before.next(nullptr); setting; setting = settings_before.next(setting)) {
				expected_project_settings[StringName(*setting)] = settings_before[*setting];
			}
			files.insert(path);
		}
	}
	Dictionary data;
	data["session_id"] = p_session_id;
	data["target_revision"] = (int64_t)p_target_revision;
	data["records"] = records;
	data["action_count"] = records.size();
	data["file_count"] = files.size();
	return _ok(data);
}

Dictionary SolersToolRegistry::prepare_session_rewind(const String &p_session_id, uint64_t p_target_revision) {
	const Dictionary preview = preview_session_rewind(p_session_id, p_target_revision);
	if (!(bool)preview.get("ok", false)) {
		return preview;
	}
	if (!file_checkpoint) {
		return _error("CHECKPOINT_SERVICE_UNAVAILABLE", "The file checkpoint service is not initialized.", false);
	}
	const Dictionary preview_data = preview.get("data", Dictionary());
	const Array records = preview_data.get("records", Array());
	HashSet<String> paths;
	for (const Variant &item : records) {
		const Dictionary record = item;
		if (_mutation_record_has_domain(record, "files")) {
			for (const Variant &checkpoint_item : Array(record.get("checkpoints", Array()))) {
				paths.insert(String(Dictionary(checkpoint_item).get("path", String())));
			}
		}
		if (_mutation_record_has_domain(record, "editor")) {
			const Dictionary scene_after = Dictionary(record.get("receipt", Dictionary())).get("scene_after", Dictionary());
			const String scene_path = scene_after.get("scene_path", String());
			if (!scene_path.is_empty()) {
				paths.insert(scene_path);
			}
		}
	}
	Array recovery;
	for (const String &path : paths) {
		const Dictionary checkpoint = file_checkpoint->create_checkpoint(path, "Solers historical-message rewind recovery");
		if (!(bool)checkpoint.get("ok", false)) {
			for (const Variant &created : recovery) {
				file_checkpoint->discard_checkpoint_state(created);
			}
			return checkpoint;
		}
		Dictionary recovery_state = checkpoint.get("data", Dictionary());
		Dictionary settings;
		for (const Variant &record_item : records) {
			for (const Variant &checkpoint_item : Array(Dictionary(record_item).get("checkpoints", Array()))) {
				const Dictionary recorded = checkpoint_item;
				if (String(recorded.get("path", String())) != path) {
					continue;
				}
				const Dictionary recorded_settings = recorded.get("project_settings", Dictionary());
				for (const Variant *setting = recorded_settings.next(nullptr); setting; setting = recorded_settings.next(setting)) {
					settings[*setting] = ProjectSettings::get_singleton()->get(*setting);
				}
			}
		}
		if (!settings.is_empty()) {
			recovery_state["project_settings"] = settings;
		}
		recovery.push_back(recovery_state);
	}
	Dictionary transaction = preview_data.duplicate(true);
	transaction["transaction_id"] = (p_session_id + ":" + String::num_uint64(p_target_revision) + ":" + String::num_uint64(Time::get_singleton()->get_ticks_usec())).sha256_text();
	transaction["recovery_checkpoints"] = recovery;
	return _ok(transaction);
}

Dictionary SolersToolRegistry::abort_session_rewind(const Dictionary &p_transaction) {
	const Array records = p_transaction.get("records", Array());
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	HashMap<int, int64_t> latest_history_versions;
	for (const Variant &item : records) {
		const Dictionary record = item;
		if (_mutation_record_has_domain(record, "editor")) {
			latest_history_versions[record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY)] = record.get("version_after", 0);
		}
	}
	for (const KeyValue<int, int64_t> &expected : latest_history_versions) {
		UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(expected.key) : nullptr;
		if (!undo_redo || (int64_t)undo_redo->get_version() > expected.value) {
			return _error("REWIND_COMPENSATION_CONFLICT", "Godot's UndoRedo history changed after the rewind transaction.", false);
		}
	}
	const Array recovery = p_transaction.get("recovery_checkpoints", Array());
	for (const Variant &recovery_item : recovery) {
		const Dictionary recovery_state = recovery_item;
		const String path = recovery_state.get("path", String());
		const Dictionary current = _solers_checkpoint_target_state(file_checkpoint, path);
		const bool exists = current.get("existed", false);
		const String sha = current.get("content_sha256", String());
		bool known = exists == (bool)recovery_state.get("existed", false) && (!exists || sha == String(recovery_state.get("content_sha256", String())));
		for (int i = records.size() - 1; i >= 0 && !known; i--) {
			const Dictionary record = records[i];
			if (_mutation_record_has_domain(record, "files")) {
				for (const Variant &checkpoint_item : Array(record.get("checkpoints", Array()))) {
					const Dictionary checkpoint = checkpoint_item;
					known = String(checkpoint.get("path", String())) == path && exists == (bool)checkpoint.get("existed", false) && (!exists || sha == String(checkpoint.get("content_sha256", String())));
					if (known) {
						break;
					}
				}
			}
			if (!known && _mutation_record_has_domain(record, "editor")) {
				const Dictionary receipt = record.get("receipt", Dictionary());
				for (const char *field : { "scene_before", "scene_after" }) {
					const Dictionary state = receipt.get(field, Dictionary());
					known = exists && String(state.get("scene_path", String())) == path && sha == String(state.get("saved_sha256", String()));
					if (known) {
						break;
					}
				}
			}
		}
		if (!known) {
			return _error("REWIND_COMPENSATION_CONFLICT", vformat("File changed outside the interrupted rewind: %s", path), false);
		}
	}
	bool scene_changed = false;
	for (const Variant &item : records) {
		const Dictionary record = item;
		if (!_mutation_record_has_domain(record, "editor")) {
			continue;
		}
		scene_changed = true;
		const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
		const uint64_t before = (int64_t)record.get("version_before", 0);
		const uint64_t after = (int64_t)record.get("version_after", 0);
		UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
		if (!undo_redo) {
			return _error("REWIND_COMPENSATION_CONFLICT", "Godot's UndoRedo history is unavailable during compensation.", false);
		}
		const uint64_t current = undo_redo->get_version();
		if (current == before && (!manager->redo_history(history_id) || undo_redo->get_version() != after)) {
			return _error("REWIND_COMPENSATION_FAILED", "Godot could not redo a compensated editor action.", false);
		}
		if (current < before || (current > before && current < after)) {
			return _error("REWIND_COMPENSATION_CONFLICT", "Godot's UndoRedo history changed during compensation.", false);
		}
	}
	if (scene_changed && EditorInterface::get_singleton()) {
		EditorInterface::get_singleton()->save_scene();
	}
	for (int i = recovery.size() - 1; i >= 0; i--) {
		if (!file_checkpoint || !(bool)file_checkpoint->restore_checkpoint_state(recovery[i]).get("ok", false)) {
			return _error("REWIND_COMPENSATION_FAILED", "A recovery checkpoint could not be restored.", false);
		}
	}
	for (const Variant &checkpoint : recovery) {
		file_checkpoint->discard_checkpoint_state(checkpoint);
	}
	Dictionary data;
	data["transaction_id"] = p_transaction.get("transaction_id", String());
	data["compensated"] = true;
	return _ok(data);
}

Dictionary SolersToolRegistry::apply_session_rewind(const Dictionary &p_transaction) {
	const String session = p_transaction.get("session_id", String());
	const uint64_t target = (int64_t)p_transaction.get("target_revision", 0);
	const Dictionary checked = preview_session_rewind(session, target);
	if (!(bool)checked.get("ok", false)) {
		return checked;
	}
	const Array records = p_transaction.get("records", Array());
	const Array current_records = Dictionary(checked.get("data", Dictionary())).get("records", Array());
	if (records.size() > current_records.size()) {
		return _error("REWIND_PLAN_STALE", "The reversible mutation stack changed after confirmation.", false);
	}
	const int record_offset = current_records.size() - records.size();
	for (int i = 0; i < records.size(); i++) {
		if (String(Dictionary(records[i]).get("id", String())) != String(Dictionary(current_records[record_offset + i]).get("id", String()))) {
			return _error("REWIND_PLAN_STALE", "The reversible mutation stack changed after confirmation.", false);
		}
	}
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	for (int i = records.size() - 1; i >= 0; i--) {
		const Dictionary record = records[i];
		bool applied = true;
		if (_mutation_record_has_domain(record, "editor")) {
			const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
			const uint64_t before = (int64_t)record.get("version_before", 0);
			const uint64_t after = (int64_t)record.get("version_after", 0);
			UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
			applied = undo_redo && undo_redo->get_version() == after && manager->undo_history(history_id) && undo_redo->get_version() == before;
			if (applied && EditorInterface::get_singleton()) {
				EditorInterface::get_singleton()->save_scene();
				applied = !manager->is_history_unsaved(history_id);
			}
		}
		if (applied && _mutation_record_has_domain(record, "files")) {
			const Array checkpoints = record.get("checkpoints", Array());
			for (int checkpoint_index = checkpoints.size() - 1; checkpoint_index >= 0; checkpoint_index--) {
				const Dictionary checkpoint = checkpoints[checkpoint_index];
				applied = file_checkpoint && (bool)file_checkpoint->restore_checkpoint_state(checkpoint).get("ok", false) && _solers_checkpoint_matches(file_checkpoint, checkpoint, false);
				if (!applied) {
					break;
				}
			}
		}
		if (!applied) {
			return _error("REWIND_APPLY_FAILED", "The project rewind stopped before every native state could be restored.", false);
		}
	}
	Dictionary data = p_transaction.duplicate(true);
	data["applied"] = true;
	return _ok(data);
}
Dictionary SolersToolRegistry::finish_session_rewind(const Dictionary &p_transaction) {
	const String session = p_transaction.get("session_id", String());
	Vector<Dictionary> *stack = reversals_by_session.getptr(session);
	const Array records = p_transaction.get("records", Array());
	if (stack && records.size() <= stack->size()) {
		bool suffix_matches = true;
		const int stack_offset = stack->size() - records.size();
		for (int i = 0; i < records.size(); i++) {
			suffix_matches = suffix_matches && String(stack->get(stack_offset + i).get("id", String())) == String(Dictionary(records[i]).get("id", String()));
		}
		if (suffix_matches) {
			for (int i = records.size() - 1; i >= 0; i--) {
				_discard_reversal(stack->get(stack->size() - 1));
				stack->remove_at(stack->size() - 1);
			}
		}
	}
	for (const Variant &checkpoint : Array(p_transaction.get("recovery_checkpoints", Array()))) {
		if (file_checkpoint) {
			file_checkpoint->discard_checkpoint_state(checkpoint);
		}
	}
	Dictionary data;
	data["transaction_id"] = p_transaction.get("transaction_id", String());
	data["target_revision"] = p_transaction.get("target_revision", 0);
	return _ok(data);
}
