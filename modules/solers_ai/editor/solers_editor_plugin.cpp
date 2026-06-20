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

#include "solers_agent_runtime.h"
#include "solers_dock.h"
#include "editor/editor_node.h"

String SolersEditorPlugin::get_plugin_name() const {
	return "Solers";
}

void SolersEditorPlugin::make_visible(bool p_visible) {
	if (dock) {
		dock->set_visible(true);
		dock->make_visible();
	}
}

void SolersEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			runtime = memnew(SolersAgentRuntime);
			dock = memnew(SolersDock);
			runtime->bind_dock(dock);
			EditorNode::get_singleton()->set_solers_ai_panel(dock);
			set_process(true);
		} break;

		case NOTIFICATION_PROCESS: {
			if (runtime) {
				runtime->poll();
			}
			// S3: while a turn is active, request a repaint so the editor keeps
			// idle-ticking and poll() drains stream deltas every frame instead of
			// stalling until the next input event. (opencode renders on arrival;
			// this guarantees arrival is processed promptly.)
			if (runtime && runtime->is_running() && dock) {
				dock->queue_redraw();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			set_process(false);
			if (dock) {
				if (EditorNode::get_singleton()) {
					EditorNode::get_singleton()->set_solers_ai_panel(nullptr);
				}
				dock->queue_free();
				dock = nullptr;
			}
			if (runtime) {
				memdelete(runtime);
				runtime = nullptr;
			}
		} break;
	}
}

SolersEditorPlugin::SolersEditorPlugin() {
}

SolersEditorPlugin::~SolersEditorPlugin() {
}
