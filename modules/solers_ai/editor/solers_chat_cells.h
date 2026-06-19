/**************************************************************************/
/*  solers_chat_cells.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Self-drawn message cells for the Solers chat timeline. The design      */
/* follows the streaming architectures studied in OpenCode and Codex:     */
/*                                                                        */
/*  - The agent session is the event source (deltas, tool lifecycle).     */
/*  - Each cell owns one visual unit of the conversation and is updated   */
/*    in place as events arrive: user bubble, streaming assistant prose   */
/*    (typewriter pacing with adaptive catch-up, like Codex's commit      */
/*    animation), a collapsible thinking cell (shimmer header + tail of   */
/*    the live reasoning), tool-call cards (spinner -> check/cross with   */
/*    duration), and a transient status row ("Waiting for model...").     */
/*  - Settled cells stop processing entirely: like the rest of the        */
/*    Solers chat chrome, steady state costs zero CPU and zero redraws.   */
/*                                                                        */
/* All text is shaped with TextParagraph (TextServer shaping, so CJK,     */
/* BiDi and emoji behave), and every chrome pixel is custom-drawn.        */
/**************************************************************************/

#pragma once

#include "core/variant/callable.h"
#include "scene/gui/control.h"
#include "scene/resources/text_paragraph.h"

class SolersMarkdownView;
class VBoxContainer;

String solers_summarize_tool_args(const String &p_arguments_json);

// Right-aligned user message bubble. Shrinks to fit its content, caps its
// width for readability, and wraps long prompts. Never degenerates into the
// one-character-per-line column a min-width autowrap Label produces.
class SolersUserBubble : public Control {
	GDCLASS(SolersUserBubble, Control);

	String text;
	Ref<TextParagraph> paragraph;
	float shaped_for_width = -1.0f;
	Size2 text_size;
	float cell_height = 0.0f;

	Callable content_changed;

	void _shape(float p_cell_width);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;

	void set_message(const String &p_text);
	void set_content_changed_callback(const Callable &p_cb) { content_changed = p_cb; }

	SolersUserBubble();
};

// Full-width assistant prose, rendered on delta arrival (opencode's model:
// `text-delta` appends and the UI renders the committed text immediately —
// no client-side typewriter timer). Each delta appends to `full_text` and
// re-renders through SolersMarkdownView (md4c + block-level streaming); while
// streaming, the open block carries a caret glyph. There is no per-frame
// reveal loop, so display never depends on the editor's idle tick rate.
class SolersAssistantCell : public Control {
	GDCLASS(SolersAssistantCell, Control);

	String full_text;
	bool stream_done = false;
	SolersMarkdownView *markdown_view = nullptr;
	int rendered_chars = -1; // length of full_text at the last render
	bool rendered_caret = false;
	float rendered_width = -1.0f;
	float cell_height = 0.0f;

	Callable content_changed;

	void _update_markdown();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;

	void append_delta(const String &p_text);
	// Authoritative full text at the end of the model step; renders it whole,
	// drops the caret, and settles.
	void finalize(const String &p_full_text);
	// Immediate, non-streamed content (errors, providers without streaming).
	void set_full_text_immediate(const String &p_text);

	void set_content_changed_callback(const Callable &p_cb) { content_changed = p_cb; }

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

	Callable content_changed;

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
	void set_done();
	bool is_active() const { return active; }

	void set_content_changed_callback(const Callable &p_cb) { content_changed = p_cb; }

	SolersThinkingCell();
};

// One tool invocation, updated in place across its lifecycle:
//   running (spinner + tool name + compact argument summary)
//   -> ok    (check, duration)
//   -> error (cross, wrapped error message).
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
	StringName tool_glyph;
	String args_summary;
	String error_text;
	Status status = STATUS_RUNNING;
	int duration_msec = -1;

	Ref<TextParagraph> error_paragraph;
	float shaped_for_width = -1.0f;
	float cell_height = 0.0f;

	float spin_phase = 0.0f;

	Callable content_changed;

	void _shape(float p_cell_width);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;

	void start(const String &p_tool_name, const String &p_arguments_json, const StringName &p_tool_glyph);
	void update(const String &p_tool_name, const String &p_arguments_json, const StringName &p_tool_glyph);
	void finish(bool p_ok, const String &p_error_message, int p_duration_msec);
	StringName get_tool_glyph() const { return tool_glyph; }
	Status get_status() const { return status; }

	void set_content_changed_callback(const Callable &p_cb) { content_changed = p_cb; }

	SolersToolCell();
};

// A folded batch of tool calls, mirroring Cursor's "N actions" row. Collapsed
// by default to one header (per-tool icons + count + chevron). Click the header
// to expand the full list of SolersToolCells; click again to collapse.
class SolersToolGroupCell : public Control {
	GDCLASS(SolersToolGroupCell, Control);

	VBoxContainer *body = nullptr;
	int total = 0;
	int running = 0;
	int error_count = 0;
	bool expanded = false;
	bool hovering = false;
	float spin_phase = 0.0f;
	float cell_height = 0.0f;

	Callable content_changed;

	float _header_height() const;
	void _relayout();
	void _on_child_changed();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

	// Appends a fresh tool row and returns it; the caller drives it via start().
	SolersToolCell *add_tool();
	void note_finished(bool p_ok);
	void settle();

	void set_content_changed_callback(const Callable &p_cb) { content_changed = p_cb; }

	SolersToolGroupCell();
};

// Transient turn status row: spinner + shimmer label ("Waiting for model...").
// Lives at the tail of the timeline while the agent is between visible
// outputs; the dock retargets or removes it as the turn progresses.
class SolersStatusCell : public Control {
	GDCLASS(SolersStatusCell, Control);

	String status_text;
	float shimmer_phase = 0.0f;
	float spin_phase = 0.0f;

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;

	void set_status(const String &p_text);

	SolersStatusCell();
};
