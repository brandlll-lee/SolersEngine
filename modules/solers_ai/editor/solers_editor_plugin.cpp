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
#include "core/object/callable_mp.h"
#include "core/os/os.h"

#include "modules/solers_ai/editor/solers_agent_runtime.h"
#include "modules/solers_ai/editor/solers_dock.h"

void SolersEditorPlugin::_select_session(const String &p_session_id) {
	runtime->set_session(project_path, p_session_id);
	dock->load_chat_history(runtime->get_timeline_entries());
	dock->set_session_context(project_path, p_session_id);
}

void SolersEditorPlugin::_new_session() {
	dock->start_new_chat();
	dock->set_session_context(project_path, runtime->get_status().get("session_id", String()));
}

void SolersEditorPlugin::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		runtime->poll();
		if (runtime->is_running()) {
			dock->queue_redraw();
		}
	}
}

SolersEditorPlugin::SolersEditorPlugin() {
	project_path = ProjectSettings::get_singleton()->get_resource_path();
	runtime = memnew(SolersAgentRuntime);
	dock = memnew(SolersDock);
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
