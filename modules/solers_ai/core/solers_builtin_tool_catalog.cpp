/**************************************************************************/
/*  solers_builtin_tool_catalog.cpp                                       */
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

#include "solers_builtin_tool_catalog.h"

Vector<SolersBuiltinToolDefinition> SolersBuiltinToolCatalog::list_tools() {
	Vector<SolersBuiltinToolDefinition> tools;
	tools.push_back({ "project.get_info", "Read the current Godot project metadata and Solers engine distribution info." });
	tools.push_back({ "project.get_settings_summary", "Read a compact summary of high-signal project settings." });
	tools.push_back({ "project.list_files", "List project files under res:// with bounded count and hidden-cache filtering." });
	tools.push_back({ "project.search_files", "Search project file paths under res:// by case-insensitive substring." });
	tools.push_back({ "project.read_file", "Read a project file from res:// with project-root boundary and byte limits." });
	tools.push_back({ "project.write_file", "Write a project text file with path safety, file checkpointing, optional script validation, and EditorFileSystem refresh.", SolersPermissionManager::PERMISSION_EDIT_FILES, "file_write", true });
	tools.push_back({ "script.read", "Read a script file from res:// with project-root boundary and byte limits." });
	tools.push_back({ "script.write", "Write or overwrite a script file after language validation and file checkpointing.", SolersPermissionManager::PERMISSION_EDIT_FILES, "file_write", true });
	tools.push_back({ "script.patch", "Apply an exact text replacement to a script or text file with optional sha256 guard, checkpointing, and validation.", SolersPermissionManager::PERMISSION_EDIT_FILES, "file_patch", true });
	tools.push_back({ "script.create", "Create a script file after language validation and EditorFileSystem refresh.", SolersPermissionManager::PERMISSION_EDIT_FILES, "file_write", true });
	tools.push_back({ "script.validate", "Validate script source through Godot's registered ScriptLanguage implementation." });
	tools.push_back({ "script.open_in_editor", "Open a script resource in Godot's ScriptEditor at a requested line and column.", SolersPermissionManager::PERMISSION_OBSERVE, "editor_ui" });
	tools.push_back({ "scene.get_open_scenes", "Read open editor scenes and lightweight root-node summaries." });
	tools.push_back({ "scene.open", "Open an existing scene file in the Godot editor.", SolersPermissionManager::PERMISSION_EDIT_SCENE, "editor_scene_state", true });
	tools.push_back({ "scene.create", "Create a new edited scene with an instantiable Node root through Godot editor state.", SolersPermissionManager::PERMISSION_EDIT_SCENE, "editor_scene_state", true });
	tools.push_back({ "scene.get_tree", "Read the current edited scene tree with bounded depth and child limits." });
	tools.push_back({ "selection.get_nodes", "Read the current editor node selection." });
	tools.push_back({ "node.get_properties", "Read bounded editor/storage properties for a node in the edited scene tree." });
	tools.push_back({ "runtime.get_status", "Read whether the editor is currently playing a scene and which scene is active." });
	tools.push_back({ "runtime.get_logs", "Read recent Godot editor/runtime output log messages captured by the editor Output dock." });
	tools.push_back({ "runtime.capture_screenshot", "Capture the current Solers editor viewport to PNG for visual verification. In v0.1 this is editor-visible viewport capture.", SolersPermissionManager::PERMISSION_RUN_PROJECT, "runtime_artifact", true });
	tools.push_back({ "editor.capture_screenshot", "Capture the current Solers editor viewport to PNG.", SolersPermissionManager::PERMISSION_RUN_PROJECT, "runtime_artifact", true });
	tools.push_back({ "editor.get_snapshot", "Read a combined project, scene, selection, and runtime snapshot." });
	tools.push_back({ "editor.get_logs", "Read recent Solers/Godot editor log messages and severity counts." });
	tools.push_back({ "timeline.list_actions", "Read recent Solers tool and action timeline events." });
	tools.push_back({ "timeline.rollback_last", "Rollback the newest Godot editor UndoRedo action. v0.1 maps this to EditorUndoRedoManager::undo.", SolersPermissionManager::PERMISSION_EDIT_SCENE, "editor_undo_redo", true });
	tools.push_back({ "validation.validate_project_scripts", "Validate all project scripts supported by registered Godot ScriptLanguage implementations." });
	tools.push_back({ "validation.assert_no_errors", "Run the v0.1 validation baseline and report whether supported project scripts have no language errors." });
	tools.push_back({ "validation.read_editor_errors", "Read recent editor/runtime log messages filtered to errors and warnings for agent verification." });
	tools.push_back({ "validation.run_scene_smoke", "Run the v0.1 non-blocking scene smoke baseline: validate scripts, inspect current scene state, and report runtime readiness.", SolersPermissionManager::PERMISSION_RUN_PROJECT, "runtime_only", true });
	tools.push_back({ "resource.get_info", "Read resource type, UID, import state, and dependency metadata for a res:// resource." });
	tools.push_back({ "export.list_presets", "List Godot export platforms and export presets from the current project." });
	tools.push_back({ "export.validate_presets", "Validate configured export presets without exporting build artifacts." });
	tools.push_back({ "provider.get_config", "Read Solers BYOK provider configuration metadata with secrets redacted." });
	tools.push_back({ "provider.list_profiles", "List built-in Solers provider profiles, default endpoints, local/remote policy flags, and advertised feature families." });
	tools.push_back({ "provider.validate_config", "Validate Solers provider configuration without making a network request." });
	tools.push_back({ "provider.set_config", "Set Solers BYOK provider configuration in local EditorSettings. Secrets are not written to project files or timeline content.", SolersPermissionManager::PERMISSION_NETWORK, "editor_settings", true });
	tools.push_back({ "approvals.list_pending", "Read pending Solers user approval requests." });
	tools.push_back({ "rpc.get_status", "Read Solers local JSONL RPC loopback server status." });
	tools.push_back({ "rpc.start", "Start the explicit opt-in Solers JSONL RPC loopback server on 127.0.0.1 with session token authentication.", SolersPermissionManager::PERMISSION_NETWORK, "network_listener", true });
	tools.push_back({ "rpc.stop", "Stop the Solers local JSONL RPC loopback server and disconnect clients.", SolersPermissionManager::PERMISSION_OBSERVE, "network_listener" });
	tools.push_back({ "node.add", "Add a Node under the edited scene tree through EditorUndoRedoManager.", SolersPermissionManager::PERMISSION_EDIT_SCENE, "editor_undo_redo", true });
	tools.push_back({ "node.set_properties", "Set Node properties through EditorUndoRedoManager.", SolersPermissionManager::PERMISSION_EDIT_SCENE, "editor_undo_redo", true });
	tools.push_back({ "node.reparent", "Reparent a Node in the edited scene tree through EditorUndoRedoManager.", SolersPermissionManager::PERMISSION_EDIT_SCENE, "editor_undo_redo", true });
	tools.push_back({ "node.attach_script", "Attach an existing Script resource to a Node through EditorUndoRedoManager.", SolersPermissionManager::PERMISSION_EDIT_SCENE, "editor_undo_redo", true });
	tools.push_back({ "node.connect_signal", "Connect a Node signal to a target Node method with persistent connection flags.", SolersPermissionManager::PERMISSION_EDIT_SCENE, "editor_undo_redo", true });
	tools.push_back({ "node.list_signal_connections", "List persistent and runtime signal connections for a Node." });
	tools.push_back({ "node.remove", "Remove a Node from the edited scene tree through EditorUndoRedoManager.", SolersPermissionManager::PERMISSION_EDIT_SCENE, "editor_undo_redo", true });
	tools.push_back({ "scene.save", "Save the current edited scene through EditorInterface.", SolersPermissionManager::PERMISSION_EDIT_FILES, "editor_save", true });
	tools.push_back({ "scene.save_as", "Save the current edited scene to a new res:// scene file through EditorInterface.", SolersPermissionManager::PERMISSION_EDIT_FILES, "editor_save", true });
	tools.push_back({ "runtime.play_current_scene", "Run the current edited scene through EditorInterface.", SolersPermissionManager::PERMISSION_RUN_PROJECT, "runtime_only", true });
	tools.push_back({ "runtime.stop", "Stop scene playback through EditorInterface.", SolersPermissionManager::PERMISSION_RUN_PROJECT, "runtime_only", true });
	return tools;
}
