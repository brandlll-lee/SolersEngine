/**************************************************************************/
/*  solers_tool_registry.cpp                                              */
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

#include "solers_tool_registry.h"

#include "core/object/class_db.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_editor_operator.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/protocol/solers_rpc_server.h"

void SolersToolRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_editor_operator", "editor_operator"), &SolersToolRegistry::set_editor_operator);
	ClassDB::bind_method(D_METHOD("set_observation_service", "observation_service"), &SolersToolRegistry::set_observation_service);
	ClassDB::bind_method(D_METHOD("set_resource_service", "resource_service"), &SolersToolRegistry::set_resource_service);
	ClassDB::bind_method(D_METHOD("set_script_service", "script_service"), &SolersToolRegistry::set_script_service);
	ClassDB::bind_method(D_METHOD("set_settings_service", "settings_service"), &SolersToolRegistry::set_settings_service);
	ClassDB::bind_method(D_METHOD("set_permission_manager", "permission_manager"), &SolersToolRegistry::set_permission_manager);
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersToolRegistry::set_action_timeline);
	ClassDB::bind_method(D_METHOD("set_rpc_server", "rpc_server"), &SolersToolRegistry::set_rpc_server);
	ClassDB::bind_method(D_METHOD("register_default_tools"), &SolersToolRegistry::register_default_tools);
	ClassDB::bind_method(D_METHOD("list_tools"), &SolersToolRegistry::list_tools);
	ClassDB::bind_method(D_METHOD("call_tool", "name", "args"), &SolersToolRegistry::call_tool);
	ClassDB::bind_method(D_METHOD("get_tool_count"), &SolersToolRegistry::get_tool_count);
}

Dictionary SolersToolRegistry::_object_schema() const {
	Dictionary schema;
	schema["type"] = "object";
	schema["additionalProperties"] = true;
	return schema;
}

void SolersToolRegistry::_register_tool(const ToolDefinition &p_definition) {
	tools[p_definition.name] = p_definition;
}

Dictionary SolersToolRegistry::_tool_to_dictionary(const ToolDefinition &p_definition) const {
	Dictionary tool;
	tool["name"] = p_definition.name;
	tool["description"] = p_definition.description;
	tool["permission"] = permission_manager ? permission_manager->get_permission_name(p_definition.permission) : "observe";
	tool["mutation_kind"] = p_definition.mutation_kind;
	tool["requires_approval"] = p_definition.requires_approval;
	tool["timeout_ms"] = p_definition.timeout_ms;
	tool["input_schema"] = p_definition.input_schema;
	tool["output_schema"] = p_definition.output_schema;
	return tool;
}

Dictionary SolersToolRegistry::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersToolRegistry::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

void SolersToolRegistry::set_editor_operator(SolersEditorOperator *p_editor_operator) {
	editor_operator = p_editor_operator;
}

void SolersToolRegistry::set_observation_service(SolersObservationService *p_observation_service) {
	observation_service = p_observation_service;
}

void SolersToolRegistry::set_resource_service(SolersResourceService *p_resource_service) {
	resource_service = p_resource_service;
}

void SolersToolRegistry::set_script_service(SolersScriptService *p_script_service) {
	script_service = p_script_service;
}

void SolersToolRegistry::set_settings_service(SolersSettingsService *p_settings_service) {
	settings_service = p_settings_service;
}

void SolersToolRegistry::set_permission_manager(SolersPermissionManager *p_permission_manager) {
	permission_manager = p_permission_manager;
}

void SolersToolRegistry::set_action_timeline(SolersActionTimeline *p_action_timeline) {
	action_timeline = p_action_timeline;
}

void SolersToolRegistry::set_rpc_server(SolersRpcServer *p_rpc_server) {
	rpc_server = p_rpc_server;
}

void SolersToolRegistry::register_default_tools() {
	tools.clear();

	const Dictionary object_schema = _object_schema();

	ToolDefinition project_info;
	project_info.name = "project.get_info";
	project_info.description = "Read the current Godot project metadata and Solers engine distribution info.";
	project_info.input_schema = object_schema;
	project_info.output_schema = object_schema;
	_register_tool(project_info);

	ToolDefinition project_settings;
	project_settings.name = "project.get_settings_summary";
	project_settings.description = "Read a compact summary of high-signal project settings.";
	project_settings.input_schema = object_schema;
	project_settings.output_schema = object_schema;
	_register_tool(project_settings);

	ToolDefinition project_files;
	project_files.name = "project.list_files";
	project_files.description = "List project files under res:// with bounded count and hidden-cache filtering.";
	project_files.input_schema = object_schema;
	project_files.output_schema = object_schema;
	_register_tool(project_files);

	ToolDefinition project_search;
	project_search.name = "project.search_files";
	project_search.description = "Search project file paths under res:// by case-insensitive substring.";
	project_search.input_schema = object_schema;
	project_search.output_schema = object_schema;
	_register_tool(project_search);

	ToolDefinition project_read;
	project_read.name = "project.read_file";
	project_read.description = "Read a project file from res:// with project-root boundary and byte limits.";
	project_read.input_schema = object_schema;
	project_read.output_schema = object_schema;
	_register_tool(project_read);

	ToolDefinition project_write;
	project_write.name = "project.write_file";
	project_write.description = "Write a project text file with path safety, file checkpointing, optional script validation, and EditorFileSystem refresh.";
	project_write.permission = SolersPermissionManager::PERMISSION_EDIT_FILES;
	project_write.mutation_kind = "file_write";
	project_write.requires_approval = true;
	project_write.input_schema = object_schema;
	project_write.output_schema = object_schema;
	_register_tool(project_write);

	ToolDefinition script_read;
	script_read.name = "script.read";
	script_read.description = "Read a script file from res:// with project-root boundary and byte limits.";
	script_read.input_schema = object_schema;
	script_read.output_schema = object_schema;
	_register_tool(script_read);

	ToolDefinition script_write;
	script_write.name = "script.write";
	script_write.description = "Write or overwrite a script file after language validation and file checkpointing.";
	script_write.permission = SolersPermissionManager::PERMISSION_EDIT_FILES;
	script_write.mutation_kind = "file_write";
	script_write.requires_approval = true;
	script_write.input_schema = object_schema;
	script_write.output_schema = object_schema;
	_register_tool(script_write);

	ToolDefinition script_patch;
	script_patch.name = "script.patch";
	script_patch.description = "Apply an exact text replacement to a script or text file with optional sha256 guard, checkpointing, and validation.";
	script_patch.permission = SolersPermissionManager::PERMISSION_EDIT_FILES;
	script_patch.mutation_kind = "file_patch";
	script_patch.requires_approval = true;
	script_patch.input_schema = object_schema;
	script_patch.output_schema = object_schema;
	_register_tool(script_patch);

	ToolDefinition script_create;
	script_create.name = "script.create";
	script_create.description = "Create a script file after language validation and EditorFileSystem refresh.";
	script_create.permission = SolersPermissionManager::PERMISSION_EDIT_FILES;
	script_create.mutation_kind = "file_write";
	script_create.requires_approval = true;
	script_create.input_schema = object_schema;
	script_create.output_schema = object_schema;
	_register_tool(script_create);

	ToolDefinition script_validate;
	script_validate.name = "script.validate";
	script_validate.description = "Validate script source through Godot's registered ScriptLanguage implementation.";
	script_validate.input_schema = object_schema;
	script_validate.output_schema = object_schema;
	_register_tool(script_validate);

	ToolDefinition script_open;
	script_open.name = "script.open_in_editor";
	script_open.description = "Open a script resource in Godot's ScriptEditor at a requested line and column.";
	script_open.mutation_kind = "editor_ui";
	script_open.input_schema = object_schema;
	script_open.output_schema = object_schema;
	_register_tool(script_open);

	ToolDefinition open_scenes;
	open_scenes.name = "scene.get_open_scenes";
	open_scenes.description = "Read open editor scenes and lightweight root-node summaries.";
	open_scenes.input_schema = object_schema;
	open_scenes.output_schema = object_schema;
	_register_tool(open_scenes);

	ToolDefinition scene_open;
	scene_open.name = "scene.open";
	scene_open.description = "Open an existing scene file in the Godot editor.";
	scene_open.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	scene_open.mutation_kind = "editor_scene_state";
	scene_open.requires_approval = true;
	scene_open.input_schema = object_schema;
	scene_open.output_schema = object_schema;
	_register_tool(scene_open);

	ToolDefinition scene_create;
	scene_create.name = "scene.create";
	scene_create.description = "Create a new edited scene with an instantiable Node root through Godot editor state.";
	scene_create.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	scene_create.mutation_kind = "editor_scene_state";
	scene_create.requires_approval = true;
	scene_create.input_schema = object_schema;
	scene_create.output_schema = object_schema;
	_register_tool(scene_create);

	ToolDefinition scene_tree;
	scene_tree.name = "scene.get_tree";
	scene_tree.description = "Read the current edited scene tree with bounded depth and child limits.";
	scene_tree.input_schema = object_schema;
	scene_tree.output_schema = object_schema;
	_register_tool(scene_tree);

	ToolDefinition selection;
	selection.name = "selection.get_nodes";
	selection.description = "Read the current editor node selection.";
	selection.input_schema = object_schema;
	selection.output_schema = object_schema;
	_register_tool(selection);

	ToolDefinition node_properties;
	node_properties.name = "node.get_properties";
	node_properties.description = "Read bounded editor/storage properties for a node in the edited scene tree.";
	node_properties.input_schema = object_schema;
	node_properties.output_schema = object_schema;
	_register_tool(node_properties);

	ToolDefinition runtime_status;
	runtime_status.name = "runtime.get_status";
	runtime_status.description = "Read whether the editor is currently playing a scene and which scene is active.";
	runtime_status.input_schema = object_schema;
	runtime_status.output_schema = object_schema;
	_register_tool(runtime_status);

	ToolDefinition runtime_logs;
	runtime_logs.name = "runtime.get_logs";
	runtime_logs.description = "Read recent Godot editor/runtime output log messages captured by the editor Output dock.";
	runtime_logs.input_schema = object_schema;
	runtime_logs.output_schema = object_schema;
	_register_tool(runtime_logs);

	ToolDefinition runtime_screenshot;
	runtime_screenshot.name = "runtime.capture_screenshot";
	runtime_screenshot.description = "Capture the current Solers editor viewport to PNG for visual verification. In v0.1 this is editor-visible viewport capture.";
	runtime_screenshot.permission = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	runtime_screenshot.mutation_kind = "runtime_artifact";
	runtime_screenshot.requires_approval = true;
	runtime_screenshot.input_schema = object_schema;
	runtime_screenshot.output_schema = object_schema;
	_register_tool(runtime_screenshot);

	ToolDefinition editor_screenshot;
	editor_screenshot.name = "editor.capture_screenshot";
	editor_screenshot.description = "Capture the current Solers editor viewport to PNG.";
	editor_screenshot.permission = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	editor_screenshot.mutation_kind = "runtime_artifact";
	editor_screenshot.requires_approval = true;
	editor_screenshot.input_schema = object_schema;
	editor_screenshot.output_schema = object_schema;
	_register_tool(editor_screenshot);

	ToolDefinition editor_snapshot;
	editor_snapshot.name = "editor.get_snapshot";
	editor_snapshot.description = "Read a combined project, scene, selection, and runtime snapshot.";
	editor_snapshot.input_schema = object_schema;
	editor_snapshot.output_schema = object_schema;
	_register_tool(editor_snapshot);

	ToolDefinition editor_logs;
	editor_logs.name = "editor.get_logs";
	editor_logs.description = "Read recent Solers/Godot editor log messages and severity counts.";
	editor_logs.input_schema = object_schema;
	editor_logs.output_schema = object_schema;
	_register_tool(editor_logs);

	ToolDefinition timeline_list;
	timeline_list.name = "timeline.list_actions";
	timeline_list.description = "Read recent Solers tool and action timeline events.";
	timeline_list.input_schema = object_schema;
	timeline_list.output_schema = object_schema;
	_register_tool(timeline_list);

	ToolDefinition timeline_rollback;
	timeline_rollback.name = "timeline.rollback_last";
	timeline_rollback.description = "Rollback the newest Godot editor UndoRedo action. v0.1 maps this to EditorUndoRedoManager::undo.";
	timeline_rollback.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	timeline_rollback.mutation_kind = "editor_undo_redo";
	timeline_rollback.requires_approval = true;
	timeline_rollback.input_schema = object_schema;
	timeline_rollback.output_schema = object_schema;
	_register_tool(timeline_rollback);

	ToolDefinition validation_project_scripts;
	validation_project_scripts.name = "validation.validate_project_scripts";
	validation_project_scripts.description = "Validate all project scripts supported by registered Godot ScriptLanguage implementations.";
	validation_project_scripts.input_schema = object_schema;
	validation_project_scripts.output_schema = object_schema;
	_register_tool(validation_project_scripts);

	ToolDefinition validation_assert_no_errors;
	validation_assert_no_errors.name = "validation.assert_no_errors";
	validation_assert_no_errors.description = "Run the v0.1 validation baseline and report whether supported project scripts have no language errors.";
	validation_assert_no_errors.input_schema = object_schema;
	validation_assert_no_errors.output_schema = object_schema;
	_register_tool(validation_assert_no_errors);

	ToolDefinition validation_read_errors;
	validation_read_errors.name = "validation.read_editor_errors";
	validation_read_errors.description = "Read recent editor/runtime log messages filtered to errors and warnings for agent verification.";
	validation_read_errors.input_schema = object_schema;
	validation_read_errors.output_schema = object_schema;
	_register_tool(validation_read_errors);

	ToolDefinition validation_scene_smoke;
	validation_scene_smoke.name = "validation.run_scene_smoke";
	validation_scene_smoke.description = "Run the v0.1 non-blocking scene smoke baseline: validate scripts, inspect current scene state, and report runtime readiness.";
	validation_scene_smoke.permission = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	validation_scene_smoke.mutation_kind = "runtime_only";
	validation_scene_smoke.requires_approval = true;
	validation_scene_smoke.input_schema = object_schema;
	validation_scene_smoke.output_schema = object_schema;
	_register_tool(validation_scene_smoke);

	ToolDefinition resource_info;
	resource_info.name = "resource.get_info";
	resource_info.description = "Read resource type, UID, import state, and dependency metadata for a res:// resource.";
	resource_info.input_schema = object_schema;
	resource_info.output_schema = object_schema;
	_register_tool(resource_info);

	ToolDefinition export_list_presets;
	export_list_presets.name = "export.list_presets";
	export_list_presets.description = "List Godot export platforms and export presets from the current project.";
	export_list_presets.input_schema = object_schema;
	export_list_presets.output_schema = object_schema;
	_register_tool(export_list_presets);

	ToolDefinition export_validate_presets;
	export_validate_presets.name = "export.validate_presets";
	export_validate_presets.description = "Validate configured export presets without exporting build artifacts.";
	export_validate_presets.input_schema = object_schema;
	export_validate_presets.output_schema = object_schema;
	_register_tool(export_validate_presets);

	ToolDefinition provider_get;
	provider_get.name = "provider.get_config";
	provider_get.description = "Read Solers BYOK provider configuration metadata with secrets redacted.";
	provider_get.input_schema = object_schema;
	provider_get.output_schema = object_schema;
	_register_tool(provider_get);

	ToolDefinition provider_list;
	provider_list.name = "provider.list_profiles";
	provider_list.description = "List built-in Solers provider profiles, default endpoints, local/remote policy flags, and advertised feature families.";
	provider_list.input_schema = object_schema;
	provider_list.output_schema = object_schema;
	_register_tool(provider_list);

	ToolDefinition provider_validate;
	provider_validate.name = "provider.validate_config";
	provider_validate.description = "Validate Solers provider configuration without making a network request.";
	provider_validate.input_schema = object_schema;
	provider_validate.output_schema = object_schema;
	_register_tool(provider_validate);

	ToolDefinition provider_set;
	provider_set.name = "provider.set_config";
	provider_set.description = "Set Solers BYOK provider configuration in local EditorSettings. Secrets are not written to project files or timeline content.";
	provider_set.permission = SolersPermissionManager::PERMISSION_NETWORK;
	provider_set.mutation_kind = "editor_settings";
	provider_set.requires_approval = true;
	provider_set.input_schema = object_schema;
	provider_set.output_schema = object_schema;
	_register_tool(provider_set);

	ToolDefinition approvals_list;
	approvals_list.name = "approvals.list_pending";
	approvals_list.description = "Read pending Solers user approval requests.";
	approvals_list.input_schema = object_schema;
	approvals_list.output_schema = object_schema;
	_register_tool(approvals_list);

	ToolDefinition rpc_status;
	rpc_status.name = "rpc.get_status";
	rpc_status.description = "Read Solers local JSONL RPC loopback server status.";
	rpc_status.input_schema = object_schema;
	rpc_status.output_schema = object_schema;
	_register_tool(rpc_status);

	ToolDefinition rpc_start;
	rpc_start.name = "rpc.start";
	rpc_start.description = "Start the explicit opt-in Solers JSONL RPC loopback server on 127.0.0.1 with session token authentication.";
	rpc_start.permission = SolersPermissionManager::PERMISSION_NETWORK;
	rpc_start.mutation_kind = "network_listener";
	rpc_start.requires_approval = true;
	rpc_start.input_schema = object_schema;
	rpc_start.output_schema = object_schema;
	_register_tool(rpc_start);

	ToolDefinition rpc_stop;
	rpc_stop.name = "rpc.stop";
	rpc_stop.description = "Stop the Solers local JSONL RPC loopback server and disconnect clients.";
	rpc_stop.mutation_kind = "network_listener";
	rpc_stop.input_schema = object_schema;
	rpc_stop.output_schema = object_schema;
	_register_tool(rpc_stop);

	ToolDefinition node_add;
	node_add.name = "node.add";
	node_add.description = "Add a Node under the edited scene tree through EditorUndoRedoManager.";
	node_add.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	node_add.mutation_kind = "editor_undo_redo";
	node_add.requires_approval = true;
	node_add.input_schema = object_schema;
	node_add.output_schema = object_schema;
	_register_tool(node_add);

	ToolDefinition node_set_properties;
	node_set_properties.name = "node.set_properties";
	node_set_properties.description = "Set Node properties through EditorUndoRedoManager.";
	node_set_properties.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	node_set_properties.mutation_kind = "editor_undo_redo";
	node_set_properties.requires_approval = true;
	node_set_properties.input_schema = object_schema;
	node_set_properties.output_schema = object_schema;
	_register_tool(node_set_properties);

	ToolDefinition node_reparent;
	node_reparent.name = "node.reparent";
	node_reparent.description = "Reparent a Node in the edited scene tree through EditorUndoRedoManager.";
	node_reparent.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	node_reparent.mutation_kind = "editor_undo_redo";
	node_reparent.requires_approval = true;
	node_reparent.input_schema = object_schema;
	node_reparent.output_schema = object_schema;
	_register_tool(node_reparent);

	ToolDefinition node_attach_script;
	node_attach_script.name = "node.attach_script";
	node_attach_script.description = "Attach an existing Script resource to a Node through EditorUndoRedoManager.";
	node_attach_script.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	node_attach_script.mutation_kind = "editor_undo_redo";
	node_attach_script.requires_approval = true;
	node_attach_script.input_schema = object_schema;
	node_attach_script.output_schema = object_schema;
	_register_tool(node_attach_script);

	ToolDefinition node_connect_signal;
	node_connect_signal.name = "node.connect_signal";
	node_connect_signal.description = "Connect a Node signal to a target Node method with persistent connection flags.";
	node_connect_signal.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	node_connect_signal.mutation_kind = "editor_undo_redo";
	node_connect_signal.requires_approval = true;
	node_connect_signal.input_schema = object_schema;
	node_connect_signal.output_schema = object_schema;
	_register_tool(node_connect_signal);

	ToolDefinition node_list_connections;
	node_list_connections.name = "node.list_signal_connections";
	node_list_connections.description = "List persistent and runtime signal connections for a Node.";
	node_list_connections.input_schema = object_schema;
	node_list_connections.output_schema = object_schema;
	_register_tool(node_list_connections);

	ToolDefinition node_remove;
	node_remove.name = "node.remove";
	node_remove.description = "Remove a Node from the edited scene tree through EditorUndoRedoManager.";
	node_remove.permission = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	node_remove.mutation_kind = "editor_undo_redo";
	node_remove.requires_approval = true;
	node_remove.input_schema = object_schema;
	node_remove.output_schema = object_schema;
	_register_tool(node_remove);

	ToolDefinition scene_save;
	scene_save.name = "scene.save";
	scene_save.description = "Save the current edited scene through EditorInterface.";
	scene_save.permission = SolersPermissionManager::PERMISSION_EDIT_FILES;
	scene_save.mutation_kind = "editor_save";
	scene_save.requires_approval = true;
	scene_save.input_schema = object_schema;
	scene_save.output_schema = object_schema;
	_register_tool(scene_save);

	ToolDefinition scene_save_as;
	scene_save_as.name = "scene.save_as";
	scene_save_as.description = "Save the current edited scene to a new res:// scene file through EditorInterface.";
	scene_save_as.permission = SolersPermissionManager::PERMISSION_EDIT_FILES;
	scene_save_as.mutation_kind = "editor_save";
	scene_save_as.requires_approval = true;
	scene_save_as.input_schema = object_schema;
	scene_save_as.output_schema = object_schema;
	_register_tool(scene_save_as);

	ToolDefinition runtime_play;
	runtime_play.name = "runtime.play_current_scene";
	runtime_play.description = "Run the current edited scene through EditorInterface.";
	runtime_play.permission = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	runtime_play.mutation_kind = "runtime_only";
	runtime_play.requires_approval = true;
	runtime_play.input_schema = object_schema;
	runtime_play.output_schema = object_schema;
	_register_tool(runtime_play);

	ToolDefinition runtime_stop;
	runtime_stop.name = "runtime.stop";
	runtime_stop.description = "Stop scene playback through EditorInterface.";
	runtime_stop.permission = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	runtime_stop.mutation_kind = "runtime_only";
	runtime_stop.requires_approval = true;
	runtime_stop.input_schema = object_schema;
	runtime_stop.output_schema = object_schema;
	_register_tool(runtime_stop);
}

Array SolersToolRegistry::list_tools() const {
	Array result;
	for (const KeyValue<StringName, ToolDefinition> &E : tools) {
		result.push_back(_tool_to_dictionary(E.value));
	}
	return result;
}

Dictionary SolersToolRegistry::call_tool(const StringName &p_name, const Dictionary &p_args) {
	ToolDefinition *definition = tools.getptr(p_name);
	if (!definition) {
		return _error("TOOL_NOT_FOUND", vformat("Solers tool not found: %s", p_name), true);
	}

	Dictionary timeline_payload;
	timeline_payload["tool"] = p_name;
	Dictionary timeline_args = p_args;
	if (p_name == StringName("provider.set_config") && timeline_args.has("api_key")) {
		timeline_args["api_key"] = "<redacted>";
	}
	if (p_name == StringName("rpc.start") && timeline_args.has("session_token")) {
		timeline_args["session_token"] = "<redacted>";
	}
	timeline_payload["args"] = timeline_args;
	timeline_payload["permission"] = permission_manager ? permission_manager->get_permission_name(definition->permission) : "observe";
	if (action_timeline) {
		action_timeline->record_event("tool_call_started", timeline_payload);
	}

	if (!permission_manager) {
		return _error("PERMISSION_MANAGER_UNAVAILABLE", "Solers permission manager is not initialized.", false);
	}

	const int approval_id = p_args.get("approval_id", 0);
	const bool has_approval = permission_manager->is_auto_approved(definition->permission) || permission_manager->consume_approval(approval_id, p_name);
	if (!has_approval) {
		Dictionary approval_request = permission_manager->request_user_approval(p_name, timeline_args, definition->permission);
		Dictionary denied = _error("USER_APPROVAL_REQUIRED", vformat("Tool requires approval before execution: %s", p_name), true);
		Dictionary error = denied.get("error", Dictionary());
		error["approval_request"] = approval_request;
		error["approval_id"] = approval_request.get("id", 0);
		denied["error"] = error;
		if (action_timeline) {
			Dictionary denied_payload = timeline_payload;
			denied_payload["result"] = denied;
			denied_payload["approval_request"] = approval_request;
			action_timeline->record_event("tool_call_blocked", denied_payload);
		}
		return denied;
	}

	const String tool_name = String(p_name);
	const bool editor_operator_tool = tool_name.begins_with("node.") || p_name == StringName("scene.open") || p_name == StringName("scene.create") || p_name == StringName("scene.save") || p_name == StringName("scene.save_as") || p_name == StringName("runtime.play_current_scene") || p_name == StringName("runtime.stop") || p_name == StringName("runtime.capture_screenshot") || p_name == StringName("editor.capture_screenshot") || p_name == StringName("timeline.rollback_last");
	const bool script_tool = p_name == StringName("project.write_file") || tool_name.begins_with("script.") || tool_name.begins_with("validation.");
	const bool settings_tool = tool_name.begins_with("provider.");
	const bool approval_tool = tool_name.begins_with("approvals.");
	const bool rpc_tool = tool_name.begins_with("rpc.");
	const bool resource_tool = tool_name.begins_with("resource.") || tool_name.begins_with("export.");
	const bool observation_tool = p_name != StringName("timeline.list_actions") && !editor_operator_tool && !script_tool && !settings_tool && !approval_tool && !rpc_tool && !resource_tool;
	if (observation_tool && !observation_service) {
		return _error("OBSERVATION_SERVICE_UNAVAILABLE", "Solers observation service is not initialized.", false);
	}
	if (editor_operator_tool && !editor_operator) {
		return _error("EDITOR_OPERATOR_UNAVAILABLE", "Solers editor operator is not initialized.", false);
	}
	if (script_tool && !script_service && p_name != StringName("script.read")) {
		return _error("SCRIPT_SERVICE_UNAVAILABLE", "Solers script service is not initialized.", false);
	}
	if (settings_tool && !settings_service) {
		return _error("SETTINGS_SERVICE_UNAVAILABLE", "Solers settings service is not initialized.", false);
	}
	if (resource_tool && !resource_service) {
		return _error("RESOURCE_SERVICE_UNAVAILABLE", "Solers resource service is not initialized.", false);
	}
	if (rpc_tool && !rpc_server) {
		return _error("RPC_SERVER_UNAVAILABLE", "Solers RPC server is not initialized.", false);
	}

	Dictionary result;
	if (p_name == StringName("project.get_info")) {
		result = _ok(observation_service->get_project_info());
	} else if (p_name == StringName("project.get_settings_summary")) {
		result = _ok(observation_service->get_project_settings_summary());
	} else if (p_name == StringName("project.list_files")) {
		const int max_files = p_args.get("max_files", 512);
		result = _ok(observation_service->list_project_files(max_files));
	} else if (p_name == StringName("project.search_files")) {
		const String query = p_args.get("query", String());
		const int max_files = p_args.get("max_files", 128);
		result = _ok(observation_service->search_project_files(query, max_files));
	} else if (p_name == StringName("project.read_file")) {
		const String path = p_args.get("path", String());
		const int max_bytes = p_args.get("max_bytes", 262144);
		result = _ok(observation_service->read_project_file(path, max_bytes));
	} else if (p_name == StringName("project.write_file")) {
		result = script_service->write_file(p_args);
	} else if (p_name == StringName("script.read")) {
		if (!observation_service) {
			result = _error("OBSERVATION_SERVICE_UNAVAILABLE", "Solers observation service is not initialized.", false);
		} else {
			const String path = p_args.get("path", String());
			const int max_bytes = p_args.get("max_bytes", 262144);
			result = _ok(observation_service->read_project_file(path, max_bytes));
		}
	} else if (p_name == StringName("script.write")) {
		result = script_service->write_file(p_args);
	} else if (p_name == StringName("script.patch")) {
		result = script_service->patch_file(p_args);
	} else if (p_name == StringName("script.create")) {
		Dictionary args = p_args;
		args["create"] = true;
		args["overwrite"] = false;
		result = script_service->write_file(args);
	} else if (p_name == StringName("script.validate")) {
		result = script_service->validate_script(p_args);
	} else if (p_name == StringName("script.open_in_editor")) {
		result = script_service->open_script(p_args);
	} else if (p_name == StringName("validation.validate_project_scripts")) {
		result = script_service->validate_project_scripts(p_args);
	} else if (p_name == StringName("validation.assert_no_errors")) {
		Dictionary scripts = script_service->validate_project_scripts(p_args);
		Dictionary scripts_data = scripts.get("data", Dictionary());
		Dictionary logs = observation_service ? observation_service->get_editor_logs((int)p_args.get("max_log_messages", 200)) : Dictionary();
		Dictionary counts = logs.get("counts", Dictionary());
		const int script_error_count = scripts_data.get("error_count", 0);
		const int log_error_count = counts.get("errors", 0);
		Dictionary data;
		data["valid"] = script_error_count == 0 && log_error_count == 0;
		data["script_validation"] = scripts;
		data["editor_log"] = logs;
		data["script_error_count"] = script_error_count;
		data["log_error_count"] = log_error_count;
		result = _ok(data);
	} else if (p_name == StringName("validation.read_editor_errors")) {
		Dictionary logs = observation_service ? observation_service->get_editor_logs((int)p_args.get("max_messages", 200)) : Dictionary();
		Array messages = logs.get("messages", Array());
		Array filtered;
		for (int i = 0; i < messages.size(); i++) {
			Dictionary item = messages[i];
			const int type = item.get("type", 0);
			if (type == 1 || type == 3) {
				filtered.push_back(item);
			}
		}
		Dictionary data;
		data["messages"] = filtered;
		data["count"] = filtered.size();
		data["counts"] = logs.get("counts", Dictionary());
		result = _ok(data);
	} else if (p_name == StringName("validation.run_scene_smoke")) {
		Dictionary scripts = script_service->validate_project_scripts(p_args);
		Dictionary scene = observation_service ? observation_service->get_scene_tree((int)p_args.get("max_scene_depth", 3), (int)p_args.get("max_children_per_node", 64)) : Dictionary();
		Dictionary runtime = observation_service ? observation_service->get_runtime_status() : Dictionary();
		Dictionary logs = observation_service ? observation_service->get_editor_logs((int)p_args.get("max_log_messages", 100)) : Dictionary();
		Dictionary scripts_data = scripts.get("data", Dictionary());
		Dictionary log_counts = logs.get("counts", Dictionary());
		const bool has_scene = scene.get("has_edited_scene", false);
		const int script_error_count = scripts_data.get("error_count", 0);
		const int log_error_count = log_counts.get("errors", 0);
		Dictionary data;
		data["passed"] = has_scene && script_error_count == 0 && log_error_count == 0;
		data["has_edited_scene"] = has_scene;
		data["script_validation"] = scripts;
		data["scene_tree"] = scene;
		data["runtime"] = runtime;
		data["editor_log"] = logs;
		result = _ok(data);
	} else if (p_name == StringName("resource.get_info")) {
		result = resource_service->get_resource_info(p_args);
	} else if (p_name == StringName("export.list_presets")) {
		result = resource_service->list_export_presets(p_args);
	} else if (p_name == StringName("export.validate_presets")) {
		result = resource_service->validate_export_presets(p_args);
	} else if (p_name == StringName("provider.get_config")) {
		result = settings_service->get_provider_config();
	} else if (p_name == StringName("provider.list_profiles")) {
		result = settings_service->list_provider_profiles();
	} else if (p_name == StringName("provider.validate_config")) {
		result = settings_service->validate_provider_config(p_args);
	} else if (p_name == StringName("provider.set_config")) {
		result = settings_service->set_provider_config(p_args);
	} else if (p_name == StringName("approvals.list_pending")) {
		Dictionary data;
		data["pending"] = permission_manager->list_pending_requests();
		data["count"] = permission_manager->get_pending_request_count();
		result = _ok(data);
	} else if (p_name == StringName("rpc.get_status")) {
		result = _ok(rpc_server->get_status());
	} else if (p_name == StringName("rpc.start")) {
		result = rpc_server->start(p_args);
	} else if (p_name == StringName("rpc.stop")) {
		result = rpc_server->stop();
	} else if (p_name == StringName("scene.get_open_scenes")) {
		const int max_depth = p_args.get("max_depth", 1);
		const int max_children = p_args.get("max_children_per_node", 16);
		result = _ok(observation_service->get_open_scenes(max_depth, max_children));
	} else if (p_name == StringName("scene.open")) {
		result = editor_operator->open_scene(p_args);
	} else if (p_name == StringName("scene.create")) {
		result = editor_operator->create_scene(p_args);
	} else if (p_name == StringName("scene.get_tree")) {
		const int max_depth = p_args.get("max_depth", 8);
		const int max_children = p_args.get("max_children_per_node", 128);
		result = _ok(observation_service->get_scene_tree(max_depth, max_children));
	} else if (p_name == StringName("selection.get_nodes")) {
		const int max_depth = p_args.get("max_depth", 1);
		const int max_children = p_args.get("max_children_per_node", 16);
		result = _ok(observation_service->get_selection(max_depth, max_children));
	} else if (p_name == StringName("node.get_properties")) {
		result = editor_operator->get_node_properties(p_args);
	} else if (p_name == StringName("runtime.get_status")) {
		result = _ok(observation_service->get_runtime_status());
	} else if (p_name == StringName("runtime.get_logs") || p_name == StringName("editor.get_logs")) {
		result = _ok(observation_service->get_editor_logs((int)p_args.get("max_messages", 200)));
	} else if (p_name == StringName("runtime.capture_screenshot") || p_name == StringName("editor.capture_screenshot")) {
		result = editor_operator->capture_editor_screenshot(p_args);
	} else if (p_name == StringName("editor.get_snapshot")) {
		const int max_depth = p_args.get("max_scene_depth", 4);
		const int max_children = p_args.get("max_children_per_node", 64);
		result = _ok(observation_service->get_editor_snapshot(max_depth, max_children));
	} else if (p_name == StringName("timeline.list_actions")) {
		const int limit = p_args.get("limit", 100);
		result = _ok(action_timeline ? action_timeline->list_actions(limit) : Array());
	} else if (p_name == StringName("timeline.rollback_last")) {
		result = editor_operator->rollback_last_editor_action(p_args);
	} else if (p_name == StringName("node.add")) {
		result = editor_operator->add_node(p_args);
	} else if (p_name == StringName("node.reparent")) {
		result = editor_operator->reparent_node(p_args);
	} else if (p_name == StringName("node.attach_script")) {
		result = editor_operator->attach_script(p_args);
	} else if (p_name == StringName("node.connect_signal")) {
		result = editor_operator->connect_signal(p_args);
	} else if (p_name == StringName("node.list_signal_connections")) {
		result = editor_operator->list_signal_connections(p_args);
	} else if (p_name == StringName("node.set_properties")) {
		result = editor_operator->set_node_properties(p_args);
	} else if (p_name == StringName("node.remove")) {
		result = editor_operator->remove_node(p_args);
	} else if (p_name == StringName("scene.save")) {
		result = editor_operator->save_current_scene(p_args);
	} else if (p_name == StringName("scene.save_as")) {
		result = editor_operator->save_scene_as(p_args);
	} else if (p_name == StringName("runtime.play_current_scene")) {
		result = editor_operator->play_current_scene(p_args);
	} else if (p_name == StringName("runtime.stop")) {
		result = editor_operator->stop_playing_scene(p_args);
	} else {
		result = _error("TOOL_HANDLER_NOT_FOUND", vformat("Solers tool has no handler: %s", p_name), false);
	}

	if (action_timeline) {
		Dictionary completed_payload = timeline_payload;
		completed_payload["ok"] = result.get("ok", false);
		action_timeline->record_event("tool_call_completed", completed_payload);
	}

	return result;
}

int SolersToolRegistry::get_tool_count() const {
	return tools.size();
}
