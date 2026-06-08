/**************************************************************************/
/*  solers_rml_chat_surface.h                                             */
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

#include "core/input/input_event.h"
#include "scene/gui/control.h"

class SolersRmlTextInputHandler;
class SolersRmlComposerChangeListener;

class SolersRmlChatSurface : public Control {
	GDCLASS(SolersRmlChatSurface, Control);
	friend class SolersRmlTextInputHandler;
	friend class SolersRmlComposerChangeListener;

	struct Impl;
	Impl *impl = nullptr;

	void _ensure_runtime();
	void _release_runtime();
	void _reload_document();
	void _update_context_size();
	void _set_messages_rml(const String &p_rml);
	void _sync_composer_layout(bool p_force_context_update);
	void _mark_composer_layout_dirty();
	void _set_composer_focused(bool p_focused);
	void _open_ime_window();
	void _close_ime_window();
	void _update_ime_window_position();
	void _request_rml_update();
	void _schedule_next_rml_update();

protected:
	static void _bind_methods();
	void _notification(int p_what);
	void gui_input(const Ref<InputEvent> &p_event) override;

public:
	void set_animation_suspended(bool p_suspended);
	void submit_current_prompt();
	void append_message(const String &p_speaker, const String &p_message);
	bool is_runtime_ready() const;

	SolersRmlChatSurface();
	~SolersRmlChatSurface();
};
