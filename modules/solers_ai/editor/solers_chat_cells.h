/**************************************************************************/
/*  solers_chat_cells.h                                                   */
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

#include "core/variant/callable.h"
#include "scene/gui/box_container.h"
#include "scene/gui/control.h"
#include "scene/resources/text_paragraph.h"
#include "scene/resources/texture.h"

class SolersMarkdownView;
class SolersGlyphButton;
class SolersSurface;
class HBoxContainer;
class Label;
class TextEdit;
class TextParagraph;
class VBoxContainer;

// Right-aligned, content-sized user bubble with bounded readable wrapping.
class SolersUserBubble : public Control {
	GDCLASS(SolersUserBubble, Control);

	String text;
	Vector<Ref<Texture2D>> attachment_textures;
	Ref<TextParagraph> paragraph;
	Array mention_objects;
	float shaped_for_width = -1.0f;
	Size2 text_size;
	float line_height = 0.0f;
	float cell_height = 0.0f;

	Callable content_changed;

	void _shape(float p_cell_width);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;

	void set_message(const String &p_text);
	void set_attachments(const Array &p_attachments);
	void set_content_changed_callback(const Callable &p_cb) { content_changed = p_cb; }

	SolersUserBubble();
};

class SolersUserMessageCell : public VBoxContainer {
	GDCLASS(SolersUserMessageCell, VBoxContainer);

	int64_t event_id = -1;
	String message;
	Array attachments;
	Callable edit_requested;
	Callable content_changed;

	SolersUserBubble *bubble = nullptr;
	HBoxContainer *footer = nullptr;
	Label *time_label = nullptr;
	SolersGlyphButton *copy_button = nullptr;
	SolersGlyphButton *edit_button = nullptr;
	SolersSurface *editor_surface = nullptr;
	TextEdit *editor = nullptr;
	HBoxContainer *editor_attachments = nullptr;

	void _set_footer_active(bool p_active);
	void _copy_message();
	void _begin_edit();
	void _cancel_edit();
	void _send_edit();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	void configure(int64_t p_event_id, const String &p_message, const Array &p_attachments, const String &p_time_label, const Callable &p_edit_requested, const Callable &p_content_changed);
	void set_event_id(int64_t p_event_id);
	void set_inline_object_handlers(const Callable &p_parse, const Callable &p_draw, const Callable &p_click);
	void cancel_edit();

	SolersUserMessageCell();
};

class SolersAssistantCell : public VBoxContainer {
	GDCLASS(SolersAssistantCell, VBoxContainer);

	String full_text;
	bool stream_done = false;
	SolersMarkdownView *markdown_view = nullptr;
	String pending_delta;

	void _flush_pending_delta();
	void _update_markdown();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	void append_delta(const String &p_text);
	// Authoritative full text at the end of the model step.
	void finalize(const String &p_full_text);
	// Immediate, non-streamed content (errors, providers without streaming).
	void set_full_text_immediate(const String &p_text);

	void set_content_changed_callback(const Callable &p_cb);

	SolersAssistantCell();
};

// Live reasoning view. While the model thinks: a shimmer "Thinking" header
// and the tail (last few wrapped lines) of the streamed reasoning in dim
// text. When the model moves on: collapses to "Thought for N s", expandable
// on click to the full reasoning transcript.
class SolersThinkingCell : public Control {
	GDCLASS(SolersThinkingCell, Control);

	String reasoning;
	bool active = true;
	bool expanded = false;
	uint64_t started_msec = 0;
	uint64_t thought_msec = 0;

	Ref<TextParagraph> body;
	float shaped_for_width = -1.0f;
	int shaped_chars = -1;
	bool shaped_expanded = false;
	float cell_height = 0.0f;
	int first_visible_line = 0; // tail clipping while active

	float shimmer_phase = 0.0f;
	bool hovering = false;

	void _shape(float p_cell_width);
	String _header_text() const;
	float _header_height() const;

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

	void append_reasoning(const String &p_text);
	void set_settled_reasoning(const String &p_text);
	void set_done();
	bool is_active() const { return active; }

	SolersThinkingCell();
};

// One tool invocation, updated in place across its lifecycle. Its tool
// definition supplies the verb and subject; the cell only renders that data.
class SolersToolCell : public Control {
	GDCLASS(SolersToolCell, Control);

public:
	enum Status {
		STATUS_RUNNING,
		STATUS_OK,
		STATUS_ERROR,
	};

private:
	String tool_name;
	String arguments_json;
	String subject;
	Dictionary presentation;
	Status status = STATUS_RUNNING;
	int duration_msec = -1;
	bool expanded = false;
	bool hovering = false;

	Ref<TextParagraph> detail_paragraph;
	float shaped_for_width = -1.0f;
	float cell_height = 0.0f;

	String _detail_text() const;
	void _shape(float p_cell_width);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

	void start(const String &p_tool_name, const String &p_arguments_json, const Dictionary &p_presentation, const String &p_subject);
	void update(const String &p_tool_name, const String &p_arguments_json, const Dictionary &p_presentation, const String &p_subject);
	void finish(const Dictionary &p_result, int p_duration_msec);
	String get_status_text() const;
	String get_detail_text() const { return _detail_text(); }
	Status get_status() const { return status; }
	bool is_expanded() const { return expanded; }

	SolersToolCell();
};

// Transient turn status row: shimmer label ("Thinking").
// Lives at the tail of the timeline while the agent is between visible
// outputs; the dock retargets or removes it as the turn progresses.
class SolersStatusCell : public Control {
	GDCLASS(SolersStatusCell, Control);

	String status_text;
	float shimmer_phase = 0.0f;

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;

	void set_status(const String &p_text);
	void set_active(bool p_active);

	SolersStatusCell();
};
