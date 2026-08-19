/**************************************************************************/
/*  solers_editor_plugin.cpp                                              */
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

#include "solers_editor_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/compression.h"
#include "core/io/config_file.h"
#include "core/io/file_access_memory.h"
#include "core/io/translation_loader_po.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/string/translation_server.h"
#include "editor/debugger/editor_debugger_plugin.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"

#include "modules/solers_ai/editor/solers_agent_runtime.h"
#include "modules/solers_ai/editor/solers_dock.h"
#include "modules/solers_ai/editor/solers_ui_theme.h"
#include "modules/solers_ai/generated/solers_translations.gen.h"

static constexpr int SOLERS_WORKSPACE_LAYOUT_VERSION = 1;

class SolersRuntimeDebuggerCapture : public EditorDebuggerPlugin {
public:
	bool has_capture(const String &p_capture) const override { return p_capture == "solers"; }
	bool capture(const String &p_message, const Array &, int) override { return p_message.begins_with("solers:"); }
};

void solers_load_editor_translation() {
	Ref<TranslationDomain> domain = TranslationServer::get_singleton()->get_editor_domain();
	for (const EditorTranslationList *entry = _solers_translations; entry->data; entry++) {
		if (entry->lang != EditorSettings::get_singleton()->get_language()) {
			continue;
		}
		LocalVector<uint8_t> data;
		data.resize_uninitialized(entry->uncomp_size);
		ERR_FAIL_COND(Compression::decompress(data.ptr(), entry->uncomp_size, entry->data, entry->comp_size, Compression::MODE_DEFLATE) == -1);
		Ref<FileAccessMemory> file;
		file.instantiate();
		file->open_custom(data.ptr(), data.size());
		Ref<Translation> translation = TranslationLoaderPO::load_translation(file);
		if (translation.is_valid()) {
			translation->set_locale(entry->lang);
			domain->add_translation(translation);
		}
		return;
	}
}

void SolersEditorPlugin::_select_session(const String &p_session_id) {
	runtime->set_session(project_path, p_session_id);
	dock->load_chat_history(runtime->get_timeline_entries());
	dock->set_session_context(project_path, p_session_id);
}

void SolersEditorPlugin::_new_session() {
	dock->start_new_chat();
	dock->set_session_context(project_path, runtime->get_status().get("session_id", String()));
}

void SolersEditorPlugin::_translation_changed() {
	solers_load_editor_translation();
	if (dock) {
		dock->propagate_notification(NOTIFICATION_TRANSLATION_CHANGED);
	}
}

void SolersEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			add_debugger_plugin(runtime_debugger_capture);
			break;
		case NOTIFICATION_EXIT_TREE:
			remove_debugger_plugin(runtime_debugger_capture);
			break;
		case NOTIFICATION_PROCESS:
			runtime->poll();
			if (runtime->is_running()) {
				dock->queue_redraw();
			}
			break;
	}
}

void SolersEditorPlugin::set_window_layout(Ref<ConfigFile> p_layout) {
	if ((int)p_layout->get_value("Solers", "workspace_layout_version", 0) < SOLERS_WORKSPACE_LAYOUT_VERSION) {
		EditorDockManager::get_singleton()->consolidate_vertical_docks(EditorDock::DOCK_SLOT_RIGHT_UL);
		EditorNode::get_singleton()->save_editor_layout_delayed();
	}
}

void SolersEditorPlugin::get_window_layout(Ref<ConfigFile> p_layout) {
	p_layout->set_value("Solers", "workspace_layout_version", SOLERS_WORKSPACE_LAYOUT_VERSION);
}

SolersEditorPlugin::SolersEditorPlugin() {
	runtime_debugger_capture = Ref<EditorDebuggerPlugin>(memnew(SolersRuntimeDebuggerCapture));
	solers_load_editor_translation();
	EditorSettings::get_singleton()->connect(SNAME("_translation_changed"), callable_mp(this, &SolersEditorPlugin::_translation_changed));
	project_path = ProjectSettings::get_singleton()->get_resource_path();
	runtime = memnew(SolersAgentRuntime);
	dock = memnew(SolersDock);
	dock->set_theme(SolersUITheme::create());
	dock->set_name("SolersChat");
	dock->set_session_select_callback(callable_mp(this, &SolersEditorPlugin::_select_session));
	dock->set_new_session_callback(callable_mp(this, &SolersEditorPlugin::_new_session));
	runtime->bind_dock(dock);
	add_control_to_container(CONTAINER_EDITOR_SIDE_LEFT, dock);

	const String session_id = OS::get_singleton()->get_environment("SOLERS_SESSION_ID");
	if (session_id.is_empty()) {
		runtime->set_project_path(project_path);
	} else {
		runtime->set_session(project_path, session_id);
		dock->load_chat_history(runtime->get_timeline_entries());
		OS::get_singleton()->unset_environment("SOLERS_SESSION_ID");
	}
	dock->set_session_context(project_path, session_id);
	dock->make_visible();
	set_process(true);
}

SolersEditorPlugin::~SolersEditorPlugin() {
	memdelete(runtime);
	if (dock && dock->get_parent()) {
		remove_control_from_container(CONTAINER_EDITOR_SIDE_LEFT, dock);
	}
	memdelete(dock);
}
