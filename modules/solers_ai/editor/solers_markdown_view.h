/**************************************************************************/
/*  solers_markdown_view.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Markdown rendering for the Solers chat timeline, built for streaming:  */
/*                                                                        */
/*  - The message source is split into top-level segments (prose chunks   */
/*    separated by blank lines, plus fenced code blocks). Each segment    */
/*    renders into its own child control. Settled segments are never      */
/*    re-parsed; only the trailing "open" segment re-renders as deltas    */
/*    arrive, so cost stays proportional to the active block, not the     */
/*    whole message.                                                      */
/*  - Prose segments are parsed by md4c (thirdparty, CommonMark + GFM     */
/*    tables/strikethrough/task lists) and rendered straight through      */
/*    RichTextLabel's push_* API - no intermediate BBCode string, no      */
/*    escaping pitfalls.                                                  */
/*  - Fenced code becomes SolersCodeBlock: a dark panel with language     */
/*    tag, copy button, and lightweight syntax highlighting.              */
/*  - Links go through meta_clicked: res:// paths open in the editor     */
/*    (scripts jump to line), http(s) opens the browser.                  */
/**************************************************************************/

#pragma once

#include "core/templates/local_vector.h"
#include "scene/gui/control.h"

class Button;
class RichTextLabel;

// Fenced code block panel: header row (language tag + copy button) above a
// syntax-highlighted, selectable code body on a dark rounded card.
class SolersCodeBlock : public Control {
	GDCLASS(SolersCodeBlock, Control);

	String language;
	String code;
	bool caret = false;

	RichTextLabel *body = nullptr;
	Button *copy_button = nullptr;

	String rendered_code;
	bool rendered_caret = false;
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

	void set_code(const String &p_language, const String &p_code, bool p_caret);
	// Lays the panel out for `p_width` and returns the resulting height.
	float measure(float p_width);

	SolersCodeBlock();
};

// Streaming markdown view: owns one child control per top-level segment and
// reconciles them against the current message text on every update.
class SolersMarkdownView : public Control {
	GDCLASS(SolersMarkdownView, Control);

	struct Segment {
		bool is_code = false;
		String lang;
		String text;
	};

	struct Block {
		bool is_code = false;
		String lang;
		String source;
		bool rendered_caret = false;
		Control *control = nullptr;
	};

	LocalVector<Block> blocks;
	float layout_width = -1.0f;
	float content_height = 0.0f;
	// Render cache: skip the segment split + reconcile when nothing that affects
	// output changed (identical text, streaming flag, and laid-out width). This
	// drops redundant work from resize/theme double-calls and from the dock
	// re-rendering the same committed text.
	String rendered_md;
	bool rendered_streaming = false;
	bool rendered_md_valid = false;

	static Vector<Segment> _split_segments(const String &p_markdown);
	RichTextLabel *_make_paragraph_label();
	void _render_segment(int p_index, const Segment &p_segment, bool p_open);
	void _render_paragraph(RichTextLabel *p_label, const String &p_source, bool p_caret);
	void _relayout();
	void _on_meta_clicked(const Variant &p_meta);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;

	// Full current message text. While `p_streaming` the trailing segment is
	// considered open: it gets a caret glyph and unbalanced inline markers
	// are healed before parsing so the stream never flashes raw syntax.
	void set_markdown(const String &p_markdown, bool p_streaming);
	void append_markdown_delta(const String &p_delta, bool p_streaming);
	float get_content_height() const { return content_height; }

	SolersMarkdownView();
};
