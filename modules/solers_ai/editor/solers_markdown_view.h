/**************************************************************************/
/*  solers_markdown_view.h                                                */
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

#include "core/templates/local_vector.h"
#include "scene/gui/box_container.h"

class Button;
class RichTextLabel;

// Fenced code block panel: header row (language tag + copy button) above a
// syntax-highlighted, selectable code body on a dark rounded card.
class SolersCodeBlock : public Control {
	GDCLASS(SolersCodeBlock, Control);

	String language;
	String code;
	bool streaming = false;

	RichTextLabel *body = nullptr;
	Button *copy_button = nullptr;

	String rendered_code;
	bool rendered_streaming = false;
	float layout_width = -1.0f;
	float block_height = 0.0f;
	uint64_t copied_until_msec = 0;

	void _render_body();
	void _copy_pressed();
	void _restore_copy_label();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;

	void set_code(const String &p_language, const String &p_code, bool p_streaming);
	// Lays the panel out for `p_width` and returns the resulting height.
	float measure(float p_width);

	SolersCodeBlock();
};

// Streaming markdown view: owns one child control per top-level segment and
// reconciles them against the current message text on every update.
class SolersMarkdownView : public VBoxContainer {
	GDCLASS(SolersMarkdownView, VBoxContainer);

	struct Segment {
		bool is_code = false;
		String lang;
		String text;
	};

	struct Block {
		bool is_code = false;
		String lang;
		String source;
		bool rendered_streaming = false;
		Control *control = nullptr;
	};

	LocalVector<Block> blocks;
	String target_md;
	bool target_streaming = false;
	String rendered_md;
	bool rendered_streaming = false;
	bool rendered_md_valid = false;

	static Vector<Segment> _split_segments(const String &p_markdown);
	RichTextLabel *_make_paragraph_label();
	void _render_target();
	void _render_segment(int p_index, const Segment &p_segment, bool p_streaming);
	void _render_paragraph(RichTextLabel *p_label, const String &p_source);
	void _on_meta_clicked(const Variant &p_meta);

protected:
	static void _bind_methods() {}

public:
	void set_markdown(const String &p_markdown, bool p_streaming);

	SolersMarkdownView();
};
