/**************************************************************************/
/*  solers_chat_cells.cpp                                                 */
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

#include "solers_chat_cells.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/text_paragraph.h"
#include "scene/theme/theme_db.h"
#include "servers/display/display_server.h"

#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/editor/solers_markdown_view.h"

/* ------------------------------------------------------------------ */
/* Shared palette + draw helpers                                       */
/* ------------------------------------------------------------------ */

static const Color SOLERS_CELL_TEXT_PRIMARY = Color(0.961, 0.969, 0.984);
static const Color SOLERS_CELL_TEXT_DIM = Color(0.667, 0.690, 0.733);
static const Color SOLERS_CELL_TEXT_FAINT = Color(0.667, 0.690, 0.733, 0.78f);
static const Color SOLERS_CELL_ERROR = Color(0.875, 0.478, 0.420);
// User bubble: composer-family gray, slightly lighter (shared solers_composer_bg).
#define SOLERS_CELL_BUBBLE_BG solers_cell_bubble_bg()

// Shimmer sweep period for "Thinking"/status headers, seconds.
static constexpr float SOLERS_SHIMMER_PERIOD = 1.6f;

// Reasoning tail: wrapped lines kept visible while the model is thinking.
static constexpr int SOLERS_THINKING_TAIL_LINES = 4;

static void solers_cell_fill(Control *p_control, const Rect2 &p_rect, const Color &p_color, float p_radius, const Color &p_border = Color(0, 0, 0, 0)) {
	Ref<StyleBoxFlat> sb;
	sb.instantiate();
	sb->set_anti_aliased(true);
	const bool bordered = p_border.a > 0.003f;
	sb->set_bg_color(p_color);
	sb->set_corner_radius_all(int(p_radius));
	sb->set_border_width_all(bordered ? MAX(1, int(EDSCALE)) : 0);
	sb->set_border_color(bordered ? p_border : Color(0, 0, 0, 0));
	p_control->draw_style_box(sb, p_rect);
}

static Ref<Font> solers_cell_font(const Control *p_control) {
	return p_control->get_theme_default_font();
}

static Ref<Font> solers_cell_mono_font(const Control *p_control) {
	return p_control->get_theme_font(SceneStringName(font), SNAME("SolersMono"));
}

static String _clip_tool_fact(const String &p_value, int p_max = 48) {
	if (p_value.length() <= p_max) {
		return p_value;
	}
	return p_value.left(MAX(1, p_max - 3)) + "...";
}

static String _summarize_streaming_tool_args(const String &p_raw) {
	const int key_pos = p_raw.find("\"");
	if (key_pos >= 0) {
		// Prefer the first JSON string value in a partial object (usually path/query).
		const int colon_pos = p_raw.find(":", key_pos + 1);
		const int quote_pos = colon_pos >= 0 ? p_raw.find("\"", colon_pos + 1) : -1;
		const int end_pos = quote_pos >= 0 ? p_raw.find("\"", quote_pos + 1) : -1;
		if (end_pos > quote_pos) {
			return _clip_tool_fact(p_raw.substr(quote_pos + 1, end_pos - quote_pos - 1));
		}
	}
	return String::utf8("…");
}

// Primary inline tool-row fact: first non-empty string arg (data-driven).
String solers_summarize_tool_args(const String &p_arguments_json) {
	const String raw = p_arguments_json.strip_edges();
	if (raw.is_empty() || raw == "{}") {
		return String();
	}
	if (!raw.ends_with("}")) {
		return _summarize_streaming_tool_args(raw);
	}
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(raw) != OK) {
		return _summarize_streaming_tool_args(raw);
	}
	const Variant parsed = json->get_data();
	if (parsed.get_type() != Variant::DICTIONARY) {
		return _clip_tool_fact(raw);
	}
	const Dictionary args = parsed;
	for (const KeyValue<Variant, Variant> &kv : args) {
		if (kv.value.get_type() == Variant::STRING) {
			const String value = String(kv.value).strip_edges();
			if (!value.is_empty()) {
				return _clip_tool_fact(value);
			}
		}
	}
	for (const KeyValue<Variant, Variant> &kv : args) {
		switch (kv.value.get_type()) {
			case Variant::INT:
			case Variant::FLOAT:
			case Variant::BOOL:
				return _clip_tool_fact(kv.value.stringify());
			default:
				break;
		}
	}
	return String();
}

/* ------------------------------------------------------------------ */
/* SolersUserBubble                                                    */
/* ------------------------------------------------------------------ */

SolersUserBubble::SolersUserBubble() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_h_size_flags(SIZE_EXPAND_FILL);
}

void SolersUserBubble::set_message(const String &p_text) {
	text = p_text;
	shaped_for_width = -1.0f;
	_shape(get_size().x);
	queue_redraw();
}

void SolersUserBubble::set_attachments(const Array &p_attachments) {
	attachment_textures.clear();
	for (int i = 0; i < p_attachments.size(); i++) {
		const Variant item = p_attachments[i];
		if (item.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Ref<Texture2D> texture = solers_attachment_texture(item);
		if (texture.is_valid()) {
			attachment_textures.push_back(texture);
		}
	}
	shaped_for_width = -1.0f;
	_shape(get_size().x);
	queue_redraw();
}

void SolersUserBubble::_shape(float p_cell_width) {
	const float ed = EDSCALE;
	const float cell_width = MAX(p_cell_width, 60.0f * ed);
	if (Math::is_equal_approx(shaped_for_width, cell_width)) {
		return;
	}
	shaped_for_width = cell_width;

	const Ref<Font> font = solers_cell_font(this);
	const int font_size = int(14 * ed);
	line_height = font.is_valid() ? font->get_height(font_size) + 3.0f * ed : float(font_size) + 3.0f * ed;
	// Parent cell width is the only authority. Padding is reserved up front so
	// shaped content never asks the draw path for more than the cell owns.
	const float pad_h = 12.0f * ed;
	const float max_content = MAX(cell_width - pad_h * 2.0f, 40.0f * ed);
	const float max_text_width = MIN(max_content, 380.0f * ed);
	const int icon_px = int(Math::round(13.0f * ed));

	if (paragraph.is_null()) {
		paragraph.instantiate();
	} else {
		paragraph->clear();
	}
	mention_objects.clear();
	text_size = Size2();

	const String display = SolersMention::strip_prompt_block(text);
	if (font.is_valid() && !display.is_empty()) {
		const PackedStringArray paragraphs = display.split("\n", true);
		for (int p = 0; p < paragraphs.size(); p++) {
			const String paragraph_text = paragraphs[p];
			const Array spans = SolersMention::scan_line_spans(paragraph_text);
			int cursor = 0;
			for (int i = 0; i < spans.size(); i++) {
				const Dictionary span = spans[i];
				const int start = int(span.get("column", -1));
				const int len = int(span.get("length", 0));
				if (start < cursor || len <= 0) {
					continue;
				}
				if (start > cursor) {
					this->paragraph->add_string(paragraph_text.substr(cursor, start - cursor), font, font_size);
				}
				const Dictionary mention = span.get("mention", Dictionary());
				const String label = solers_mention_chip_label(mention);
				if (!label.is_empty()) {
					const Ref<Texture2D> icon = solers_mention_chip_icon(mention, icon_px);
					Dictionary object;
					object["key"] = mention_objects.size();
					object["label"] = label;
					object["mention"] = mention;
					const float width = MIN(solers_mention_chip_width(label, font, font_size, icon.is_valid()), max_text_width);
					this->paragraph->add_object(object["key"], Size2(width, line_height));
					mention_objects.push_back(object);
				}
				cursor = start + len;
			}
			if (cursor < paragraph_text.length()) {
				this->paragraph->add_string(paragraph_text.substr(cursor), font, font_size);
			}
			if (p + 1 < paragraphs.size()) {
				this->paragraph->add_string("\n", font, font_size);
			}
		}
		this->paragraph->set_break_flags(TextServer::BREAK_MANDATORY | TextServer::BREAK_WORD_BOUND | TextServer::BREAK_GRAPHEME_BOUND);
		this->paragraph->set_width(max_text_width);
		this->paragraph->set_line_spacing(3.0f * ed);
		text_size = this->paragraph->get_size();
		text_size.x = MIN(this->paragraph->get_non_wrapped_size().x, max_text_width);
	}

	const float pad_v = 8.0f * ed;
	const float thumb = 56.0f * ed;
	const float gap = 8.0f * ed;
	const int attachment_count = attachment_textures.size();
	const int per_row = MAX(1, int((max_content + gap) / (thumb + gap)));
	const int attachment_rows = attachment_count > 0 ? (attachment_count + per_row - 1) / per_row : 0;
	const float attachments_height = attachment_rows > 0 ? attachment_rows * thumb + (attachment_rows - 1) * gap + (text_size.y > 0.0f ? gap : 0.0f) : 0.0f;
	text_size.x = MIN(text_size.x, max_text_width);
	const float old_height = cell_height;
	cell_height = attachments_height + text_size.y + pad_v * 2.0f;
	if (!Math::is_equal_approx(old_height, cell_height)) {
		update_minimum_size();
		if (content_changed.is_valid()) {
			content_changed.call();
		}
	}
}

Size2 SolersUserBubble::get_minimum_size() const {
	return Size2(0, cell_height);
}

void SolersUserBubble::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_RESIZED: {
			_shape(get_size().x);
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			shaped_for_width = -1.0f;
			_shape(get_size().x);
			queue_redraw();
		} break;
		case NOTIFICATION_DRAW: {
			if (text.is_empty() && attachment_textures.is_empty()) {
				break;
			}
			_shape(get_size().x);
			const float ed = EDSCALE;
			const float pad_h = 12.0f * ed;
			const float pad_v = 8.0f * ed;
			const float thumb = 56.0f * ed;
			const float gap = 8.0f * ed;
			const int font_size = int(14 * ed);
			const int icon_px = int(Math::round(13.0f * ed));
			const Ref<Font> font = solers_cell_font(this);
			const int attachment_count = attachment_textures.size();
			const float max_content = MAX(get_size().x - pad_h * 2.0f, 40.0f * ed);
			const int per_row = MAX(1, int((max_content + gap) / (thumb + gap)));
			const float attachments_width = attachment_count > 0 ? MIN(attachment_count, per_row) * thumb + MAX(0, MIN(attachment_count, per_row) - 1) * gap : 0.0f;
			const int attachment_rows = attachment_count > 0 ? (attachment_count + per_row - 1) / per_row : 0;
			const float attachments_height = attachment_rows > 0 ? attachment_rows * thumb + (attachment_rows - 1) * gap + (text_size.y > 0.0f ? gap : 0.0f) : 0.0f;
			const float ideal_w = MAX(text_size.x, attachments_width) + pad_h * 2.0f;
			const Size2 bubble_size(MIN(ideal_w, get_size().x), attachments_height + text_size.y + pad_v * 2.0f);
			const Rect2 bubble(Point2(get_size().x - bubble_size.x, 0), bubble_size);
			solers_cell_fill(this, bubble, SOLERS_CELL_BUBBLE_BG, 14.0f * ed);
			float y = bubble.position.y + pad_v;
			if (attachment_count > 0) {
				int drawn = 0;
				while (drawn < attachment_count) {
					float x = bubble.position.x + pad_h;
					const int row_count = MIN(per_row, attachment_count - drawn);
					for (int i = 0; i < row_count; i++) {
						const Rect2 rect(Point2(x, y), Size2(thumb, thumb));
						solers_cell_fill(this, rect, Color(0, 0, 0, 0.18f), 8.0f * ed, Color(1, 1, 1, 0.11f));
						const Ref<Texture2D> texture = attachment_textures[drawn + i];
						if (texture.is_valid()) {
							const Size2 source = texture->get_size();
							const float crop = MIN(source.x, source.y);
							const Rect2 source_rect(Point2((source.x - crop) * 0.5f, (source.y - crop) * 0.5f), Size2(crop, crop));
							draw_texture_rect_region(texture, rect, source_rect);
						}
						x += thumb + gap;
					}
					drawn += row_count;
					y += thumb + (drawn < attachment_count ? gap : (text_size.y > 0.0f ? gap : 0.0f));
				}
			}
			if (paragraph.is_valid() && font.is_valid()) {
				const RID ci = get_canvas_item();
				const Point2 origin(bubble.position.x + pad_h, y);
				paragraph->draw(ci, origin, SOLERS_CELL_TEXT_PRIMARY);
				for (int line = 0; line < paragraph->get_line_count(); line++) {
					const Array objects = paragraph->get_line_objects(line);
					for (int i = 0; i < objects.size(); i++) {
						const int key = objects[i];
						if (key < 0 || key >= mention_objects.size()) {
							continue;
						}
						const Dictionary object = mention_objects[key];
						Rect2 pill = paragraph->get_line_object_rect(line, key);
						pill.position += origin;
						const Dictionary mention = object["mention"];
						solers_draw_mention_chip(ci, pill, object["label"], font, font_size, solers_mention_chip_icon(mention, icon_px), get_theme_color(SNAME("accent_color"), SNAME("Editor")));
					}
				}
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* SolersUserMessageCell                                               */
/* ------------------------------------------------------------------ */

SolersUserMessageCell::SolersUserMessageCell() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_mouse_filter(MOUSE_FILTER_PASS);
	add_theme_constant_override("separation", 2 * EDSCALE);
	bubble = memnew(SolersUserBubble);
	bubble->set_name("UserMessageBubble");
	add_child(bubble);
	footer = memnew(HBoxContainer);
	footer->set_name("UserMessageFooter");
	footer->set_h_size_flags(SIZE_EXPAND_FILL);
	footer->set_mouse_filter(MOUSE_FILTER_PASS);
	footer->set_custom_minimum_size(Size2(0, 24 * EDSCALE));
	footer->set_alignment(BoxContainer::ALIGNMENT_END);
	footer->add_theme_constant_override("separation", 2 * EDSCALE);
	add_child(footer);
	time_label = memnew(Label);
	time_label->set_v_size_flags(SIZE_SHRINK_CENTER);
	time_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
	time_label->add_theme_color_override("font_color", SOLERS_CELL_TEXT_FAINT);
	time_label->add_theme_font_size_override(SceneStringName(font_size), 11 * EDSCALE);
	footer->add_child(time_label);
	copy_button = memnew(SolersGlyphButton);
	copy_button->configure(SNAME("copy"), SolersGlyphButton::SKIN_GHOST, TTR("Copy message"), 14);
	copy_button->set_mouse_filter(MOUSE_FILTER_PASS);
	copy_button->set_custom_minimum_size(Size2(24, 24) * EDSCALE);
	copy_button->set_pressed_callback(callable_mp(this, &SolersUserMessageCell::_copy_message));
	footer->add_child(copy_button);
	edit_button = memnew(SolersGlyphButton);
	edit_button->configure(SNAME("tool_file"), SolersGlyphButton::SKIN_GHOST, TTR("Edit message"), 14);
	edit_button->set_mouse_filter(MOUSE_FILTER_PASS);
	edit_button->set_custom_minimum_size(Size2(24, 24) * EDSCALE);
	edit_button->set_pressed_callback(callable_mp(this, &SolersUserMessageCell::_begin_edit));
	footer->add_child(edit_button);
	editor_surface = memnew(SolersSurface);
	editor_surface->set_name("HistoryMessageEditorSurface");
	solers_configure_prompt_surface(editor_surface);
	editor_surface->hide();
	add_child(editor_surface);

	VBoxContainer *editor_stack = memnew(VBoxContainer);
	editor_stack->set_h_size_flags(SIZE_EXPAND_FILL);
	editor_stack->add_theme_constant_override("separation", 6 * EDSCALE);
	editor_surface->add_child(editor_stack);

	editor = memnew(TextEdit);
	editor->set_name("HistoryMessageEditor");
	solers_configure_prompt_text_edit(editor);
	editor_stack->add_child(editor);

	editor_attachments = memnew(HBoxContainer);
	editor_attachments->add_theme_constant_override("separation", 6 * EDSCALE);
	editor_attachments->hide();
	editor_stack->add_child(editor_attachments);

	HBoxContainer *actions = memnew(HBoxContainer);
	actions->set_h_size_flags(SIZE_EXPAND_FILL);
	actions->set_alignment(BoxContainer::ALIGNMENT_END);
	actions->add_theme_constant_override("separation", 6 * EDSCALE);
	editor_stack->add_child(actions);

	Button *cancel = memnew(Button);
	cancel->set_translation_domain(SNAME("godot.editor"));
	cancel->set_text("Cancel");
	cancel->connect(SceneStringName(pressed), callable_mp(this, &SolersUserMessageCell::_cancel_edit));
	actions->add_child(cancel);

	Button *send = memnew(Button);
	send->set_translation_domain(SNAME("godot.editor"));
	send->set_text("Send");
	send->connect(SceneStringName(pressed), callable_mp(this, &SolersUserMessageCell::_send_edit));
	actions->add_child(send);

	_set_footer_active(false);
}

void SolersUserMessageCell::configure(int64_t p_event_id, const String &p_message, const Array &p_attachments, const String &p_time_label, const Callable &p_edit_requested, const Callable &p_content_changed) {
	event_id = p_event_id;
	message = p_message;
	attachments = p_attachments.duplicate(true);
	edit_requested = p_edit_requested;
	content_changed = p_content_changed;
	time_label->set_text(p_time_label);
	bubble->set_message(message);
	bubble->set_attachments(attachments);
	bubble->set_content_changed_callback(content_changed);
	set_event_id(event_id);

	while (editor_attachments->get_child_count() > 0) {
		Node *child = editor_attachments->get_child(0);
		editor_attachments->remove_child(child);
		memdelete(child);
	}
	for (const Variant &item : attachments) {
		if (item.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Ref<Texture2D> texture = solers_attachment_texture(item);
		if (texture.is_null()) {
			continue;
		}
		TextureRect *preview = memnew(TextureRect);
		preview->set_texture(texture);
		preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		preview->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
		preview->set_custom_minimum_size(Size2(42, 42) * EDSCALE);
		preview->set_tooltip_text(TTR("This attachment will be verified and sent unchanged."));
		editor_attachments->add_child(preview);
	}
	editor_attachments->set_visible(editor_attachments->get_child_count() > 0);
}

void SolersUserMessageCell::set_event_id(int64_t p_event_id) {
	event_id = p_event_id;
	edit_button->set_enabled(event_id >= 0);
}

void SolersUserMessageCell::set_inline_object_handlers(const Callable &p_parse, const Callable &p_draw, const Callable &p_click) {
	editor->set_inline_object_handlers(p_parse, p_draw, p_click);
}

void SolersUserMessageCell::_set_footer_active(bool p_active) {
	footer->set_modulate(Color(1, 1, 1, p_active ? 1.0f : 0.0f));
}

void SolersUserMessageCell::_copy_message() {
	DisplayServer::get_singleton()->clipboard_set(message);
}

void SolersUserMessageCell::_begin_edit() {
	if (event_id < 0) {
		return;
	}
	editor->set_text(message);
	bubble->hide();
	footer->hide();
	editor_surface->show();
	editor->grab_focus();
	editor->set_caret_line(editor->get_line_count() - 1);
	editor->set_caret_column(editor->get_line(editor->get_caret_line()).length());
	if (content_changed.is_valid()) {
		content_changed.call();
	}
}

void SolersUserMessageCell::_cancel_edit() {
	cancel_edit();
}

void SolersUserMessageCell::cancel_edit() {
	editor_surface->hide();
	bubble->show();
	footer->show();
	_set_footer_active(false);
	if (content_changed.is_valid()) {
		content_changed.call();
	}
}

void SolersUserMessageCell::_send_edit() {
	const String edited = editor->get_text().strip_edges();
	if ((edited.is_empty() && attachments.is_empty()) || !edit_requested.is_valid()) {
		return;
	}
	edit_requested.call(event_id, edited, attachments.duplicate(true));
}

void SolersUserMessageCell::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_ENTER:
			if (!editor_surface->is_visible()) {
				_set_footer_active(true);
			}
			break;
		case NOTIFICATION_MOUSE_EXIT:
			if (!editor_surface->is_visible()) {
				_set_footer_active(false);
			}
			break;
	}
}

/* ------------------------------------------------------------------ */
/* SolersAssistantCell                                                 */
/* ------------------------------------------------------------------ */

SolersAssistantCell::SolersAssistantCell() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_h_size_flags(SIZE_EXPAND_FILL);

	markdown_view = memnew(SolersMarkdownView);
	add_child(markdown_view);
}

void SolersAssistantCell::append_delta(const String &p_text) {
	if (p_text.is_empty()) {
		return;
	}
	pending_delta += p_text;
	set_process_internal(true);
}

void SolersAssistantCell::_flush_pending_delta() {
	if (pending_delta.is_empty()) {
		return;
	}
	full_text += pending_delta;
	pending_delta = String();
	_update_markdown();
}

void SolersAssistantCell::finalize(const String &p_full_text) {
	_flush_pending_delta();
	set_process_internal(false);
	if (!p_full_text.is_empty() && p_full_text != full_text) {
		full_text = p_full_text;
		rendered_chars = -1;
	}
	stream_done = true;
	_update_markdown();
}

void SolersAssistantCell::set_full_text_immediate(const String &p_text) {
	pending_delta = String();
	set_process_internal(false);
	full_text = p_text;
	stream_done = true;
	rendered_chars = -1;
	_update_markdown();
}

void SolersAssistantCell::_update_markdown() {
	if (!markdown_view) {
		return;
	}
	const int len = full_text.length();
	const bool caret = !stream_done; // caret only while the stream is open
	if (rendered_chars == len && rendered_caret == caret) {
		return;
	}
	const int previous_rendered_chars = rendered_chars;
	const bool can_append = !stream_done && previous_rendered_chars >= 0 && len > previous_rendered_chars && rendered_caret;
	rendered_chars = len;
	rendered_caret = caret;

	if (can_append) {
		markdown_view->append_markdown_delta(full_text.substr(previous_rendered_chars), caret);
	} else {
		markdown_view->set_markdown(full_text, caret);
	}
	if (content_changed.is_valid()) {
		content_changed.call();
	}
}

void SolersAssistantCell::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			queue_redraw();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_flush_pending_delta();
			if (pending_delta.is_empty() && rendered_chars == full_text.length()) {
				set_process_internal(false);
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* SolersThinkingCell                                                  */
/* ------------------------------------------------------------------ */

SolersThinkingCell::SolersThinkingCell() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	body.instantiate();
	body->set_break_flags(TextServer::BREAK_MANDATORY | TextServer::BREAK_WORD_BOUND | TextServer::BREAK_ADAPTIVE);
	started_msec = OS::get_singleton()->get_ticks_msec();
	set_process_internal(true);
	set_default_cursor_shape(CURSOR_POINTING_HAND);
}

String SolersThinkingCell::_header_text() const {
	if (active) {
		return TTR("Thinking");
	}
	const float secs = float(thought_msec) / 1000.0f;
	if (secs < 0.95f) {
		return TTR("Thought briefly");
	}
	return vformat(TTR("Thought for %s s"), String::num(secs, secs < 10.0f ? 1 : 0));
}

float SolersThinkingCell::_header_height() const {
	return 20.0f * EDSCALE;
}

void SolersThinkingCell::append_reasoning(const String &p_text) {
	reasoning += p_text;
	shaped_chars = -1;
	_shape(get_size().x);
	queue_redraw();
}

void SolersThinkingCell::set_settled_reasoning(const String &p_text) {
	reasoning = p_text;
	active = false;
	thought_msec = 0;
	set_process_internal(false);
	shaped_chars = -1;
	shaped_for_width = -1.0f;
	_shape(get_size().x);
	queue_redraw();
}

void SolersThinkingCell::set_done() {
	if (!active) {
		return;
	}
	active = false;
	thought_msec = OS::get_singleton()->get_ticks_msec() - started_msec;
	set_process_internal(false);
	shaped_chars = -1;
	shaped_for_width = -1.0f;
	_shape(get_size().x);
	queue_redraw();
}

void SolersThinkingCell::gui_input(const Ref<InputEvent> &p_event) {
	if (active || reasoning.strip_edges().is_empty()) {
		return;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT && mb->is_pressed()) {
		expanded = !expanded;
		shaped_for_width = -1.0f;
		_shape(get_size().x);
		queue_redraw();
		accept_event();
	}
}

void SolersThinkingCell::_shape(float p_cell_width) {
	const float ed = EDSCALE;
	const float cell_width = MAX(p_cell_width, 60.0f * ed);
	const int chars = reasoning.length();
	if (Math::is_equal_approx(shaped_for_width, cell_width) && shaped_chars == chars && shaped_expanded == expanded) {
		return;
	}
	shaped_for_width = cell_width;
	shaped_chars = chars;
	shaped_expanded = expanded;

	const Ref<Font> font = solers_cell_font(this);
	const int font_size = int(12 * ed);
	const String trimmed = reasoning.strip_edges();

	body->clear();
	first_visible_line = 0;
	float body_height = 0.0f;
	const bool body_visible = !trimmed.is_empty() && (active || expanded);
	if (font.is_valid() && body_visible) {
		body->set_line_spacing(3.0f * ed);
		body->set_width(cell_width - 14.0f * ed);
		body->add_string(trimmed, font, font_size);
		const int line_count = body->get_line_count();
		if (active && line_count > SOLERS_THINKING_TAIL_LINES) {
			first_visible_line = line_count - SOLERS_THINKING_TAIL_LINES;
		}
		for (int i = first_visible_line; i < line_count; i++) {
			body_height += body->get_line_ascent(i) + body->get_line_descent(i) + body->get_line_spacing();
		}
		body_height += 2.0f * ed;
	}

	const float old_height = cell_height;
	cell_height = _header_height() + body_height;
	if (!Math::is_equal_approx(old_height, cell_height)) {
		update_minimum_size();
		if (content_changed.is_valid()) {
			content_changed.call();
		}
	}
}

Size2 SolersThinkingCell::get_minimum_size() const {
	return Size2(0, cell_height);
}

void SolersThinkingCell::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_ENTER: {
			hovering = true;
			queue_redraw();
		} break;
		case NOTIFICATION_MOUSE_EXIT: {
			hovering = false;
			queue_redraw();
		} break;
		case NOTIFICATION_RESIZED: {
			shaped_for_width = -1.0f;
			_shape(get_size().x);
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			shaped_for_width = -1.0f;
			shaped_chars = -1;
			_shape(get_size().x);
			queue_redraw();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			shimmer_phase = Math::fmod(shimmer_phase + float(get_process_delta_time()) / SOLERS_SHIMMER_PERIOD, 1.0f);
			queue_redraw();
		} break;
		case NOTIFICATION_DRAW: {
			_shape(get_size().x);
			const float ed = EDSCALE;
			const Ref<Font> font = solers_cell_font(this);
			if (font.is_null()) {
				break;
			}
			const int header_size = int(12 * ed);
			const float header_h = _header_height();
			const float baseline = (header_h - font->get_height(header_size)) * 0.5f + font->get_ascent(header_size);

			float header_w = 0.0f;
			if (active) {
				draw_string(font, Point2(0, baseline).floor(), _header_text(), HORIZONTAL_ALIGNMENT_LEFT, -1, header_size, SOLERS_CELL_TEXT_FAINT.lerp(Color(0.95f, 0.96f, 0.98f), 0.35f + 0.25f * Math::sin(shimmer_phase * Math::TAU)));
				header_w = font->get_string_size(_header_text(), HORIZONTAL_ALIGNMENT_LEFT, -1, header_size).x;
			} else {
				const Color header_color = hovering ? SOLERS_CELL_TEXT_DIM.lerp(Color(1, 1, 1), 0.25f) : SOLERS_CELL_TEXT_DIM;
				draw_string(font, Point2(0, baseline).floor(), _header_text(), HORIZONTAL_ALIGNMENT_LEFT, -1, header_size, header_color);
				header_w = font->get_string_size(_header_text(), HORIZONTAL_ALIGNMENT_LEFT, -1, header_size).x;
				if (!reasoning.strip_edges().is_empty()) {
					Ref<Texture2D> chevron = SolersIcons::get(expanded ? SNAME("chevron_down") : SNAME("chevron_right"), int(Math::round(9.0f * ed)));
					if (chevron.is_valid()) {
						draw_texture(chevron, Point2(header_w + 5.0f * ed, (header_h - chevron->get_height()) * 0.5f).floor(), header_color);
					}
				}
			}

			// Reasoning body: tail while live, full transcript when expanded.
			const bool body_visible = body->get_line_count() > 0 && (active || expanded);
			if (body_visible) {
				const float indent = 14.0f * ed;
				float y = header_h + 2.0f * ed;
				const int line_count = body->get_line_count();
				for (int i = first_visible_line; i < line_count; i++) {
					// Older tail lines fade slightly toward the top.
					float alpha = 0.62f;
					if (active && line_count > 1) {
						const float t = float(i - first_visible_line) / float(line_count - first_visible_line);
						alpha = 0.34f + 0.30f * t;
					}
					body->draw_line(get_canvas_item(), Point2(indent, y), i, Color(SOLERS_CELL_TEXT_DIM, alpha));
					y += body->get_line_ascent(i) + body->get_line_descent(i) + body->get_line_spacing();
				}
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* Plan text formatter                                                 */
/* ------------------------------------------------------------------ */

String solers_format_plan_text(const String &p_explanation, const Array &p_plan) {
	String text = p_explanation.strip_edges();
	for (int i = 0; i < p_plan.size(); i++) {
		const Dictionary item = p_plan[i];
		const String status = item.get("status", "pending");
		const String marker = status == "completed" ? String::utf8("✓ ") : status == "in_progress" ? String::utf8("→ ")
																								   : String::utf8("○ ");
		if (!text.is_empty()) {
			text += "\n";
		}
		text += marker + String(item.get("step", String()));
	}
	return text;
}

/* ------------------------------------------------------------------ */
/* SolersToolCell                                                      */
/* ------------------------------------------------------------------ */

SolersToolCell::SolersToolCell() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_h_size_flags(SIZE_EXPAND_FILL);
	error_paragraph.instantiate();
	error_paragraph->set_break_flags(TextServer::BREAK_MANDATORY | TextServer::BREAK_WORD_BOUND | TextServer::BREAK_ADAPTIVE);
}

void SolersToolCell::start(const String &p_tool_name, const String &p_arguments_json, const String &p_ui_kind) {
	tool_name = p_tool_name.is_empty() ? String("tool") : p_tool_name;
	tool_icon = solers_tool_icon_for_ui_kind(p_ui_kind);
	tool_verb = tool_name.begins_with("tool.") ? tool_name : "tool." + tool_name;
	args_summary = solers_summarize_tool_args(p_arguments_json);
	set_tooltip_text(tool_name);
	status = STATUS_RUNNING;
	shaped_for_width = -1.0f;
	_shape(get_size().x);
	set_process_internal(true);
	queue_redraw();
}

void SolersToolCell::update(const String &p_tool_name, const String &p_arguments_json, const String &p_ui_kind) {
	const String next_name = p_tool_name.is_empty() ? tool_name : p_tool_name;
	const String next_summary = solers_summarize_tool_args(p_arguments_json);
	const StringName next_icon = solers_tool_icon_for_ui_kind(p_ui_kind);
	const String next_verb = next_name.begins_with("tool.") ? next_name : "tool." + next_name;
	if (tool_name == next_name && args_summary == next_summary && tool_icon == next_icon && tool_verb == next_verb) {
		return;
	}
	tool_name = next_name.is_empty() ? String("tool") : next_name;
	tool_icon = next_icon;
	tool_verb = next_verb;
	args_summary = next_summary;
	set_tooltip_text(tool_name);
	queue_redraw();
}

void SolersToolCell::finish(bool p_ok, const String &p_error_message, int p_duration_msec) {
	status = p_ok ? STATUS_OK : STATUS_ERROR;
	error_text = p_ok ? String() : p_error_message.strip_edges();
	duration_msec = p_duration_msec;
	set_process_internal(false);
	shaped_for_width = -1.0f;
	_shape(get_size().x);
	queue_redraw();
}

String SolersToolCell::get_status_text() const {
	String text = status == STATUS_RUNNING ? "Running " : "Ran ";
	text += tool_verb;
	return args_summary.is_empty() ? text : text + " " + args_summary;
}

void SolersToolCell::_shape(float p_cell_width) {
	const float ed = EDSCALE;
	const float cell_width = MAX(p_cell_width, 60.0f * ed);
	if (Math::is_equal_approx(shaped_for_width, cell_width)) {
		return;
	}
	shaped_for_width = cell_width;

	float height = 28.0f * ed; // Header row.
	error_paragraph->clear();
	if (!error_text.is_empty()) {
		const Ref<Font> font = solers_cell_font(this);
		if (font.is_valid()) {
			error_paragraph->set_line_spacing(2.0f * ed);
			error_paragraph->set_width(cell_width - 30.0f * ed);
			error_paragraph->add_string(error_text, font, int(11 * ed));
			height += error_paragraph->get_size().y + 6.0f * ed;
		}
	}

	const float old_height = cell_height;
	cell_height = height;
	if (!Math::is_equal_approx(old_height, cell_height)) {
		update_minimum_size();
		if (content_changed.is_valid()) {
			content_changed.call();
		}
	}
}

Size2 SolersToolCell::get_minimum_size() const {
	return Size2(0, cell_height);
}

void SolersToolCell::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_RESIZED: {
			shaped_for_width = -1.0f;
			_shape(get_size().x);
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			shaped_for_width = -1.0f;
			_shape(get_size().x);
			queue_redraw();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			queue_redraw();
		} break;
		case NOTIFICATION_DRAW: {
			_shape(get_size().x);
			const float ed = EDSCALE;

			const Ref<Font> font = solers_cell_font(this);
			const Ref<Font> mono = solers_cell_mono_font(this);
			if (font.is_null()) {
				break;
			}

			const float header_h = 28.0f * ed;
			const float type_cx = 7.0f * ed;
			const float icon_cy = header_h * 0.5f;
			Ref<Texture2D> type_icon = SolersIcons::get(tool_icon, int(Math::round(13.0f * ed)));
			if (type_icon.is_valid()) {
				const Color icon_color = status == STATUS_ERROR ? SOLERS_CELL_ERROR : SOLERS_CELL_TEXT_DIM;
				draw_texture(type_icon, Point2(type_cx - type_icon->get_width() * 0.5f, icon_cy - type_icon->get_height() * 0.5f).floor(), icon_color);
			}

			const int verb_size = int(12 * ed);
			const int fact_size = int(11 * ed);
			const float x = 20.0f * ed;
			const float verb_baseline = (header_h - font->get_height(verb_size)) * 0.5f + font->get_ascent(verb_size);
			const Color verb_color = status == STATUS_ERROR ? SOLERS_CELL_ERROR : Color(0.90f, 0.91f, 0.94f);
			String trail;
			if (status != STATUS_RUNNING && duration_msec >= 0) {
				trail = duration_msec >= 1000 ? vformat("%s s", String::num(double(duration_msec) / 1000.0, 1)) : vformat("%d ms", duration_msec);
			}
			float trail_w = 0.0f;
			if (!trail.is_empty() && mono.is_valid()) {
				trail_w = mono->get_string_size(trail, HORIZONTAL_ALIGNMENT_LEFT, -1, fact_size).x;
				const float trail_baseline = (header_h - mono->get_height(fact_size)) * 0.5f + mono->get_ascent(fact_size);
				draw_string(mono, Point2(get_size().x - trail_w - 10.0f * ed, trail_baseline).floor(), trail, HORIZONTAL_ALIGNMENT_LEFT, -1, fact_size, SOLERS_CELL_TEXT_FAINT);
				trail_w += 14.0f * ed;
			}
			const String label = get_status_text();
			if (status == STATUS_RUNNING) {
				draw_string(font, Point2(x, verb_baseline).floor(), label, HORIZONTAL_ALIGNMENT_LEFT, get_size().x - x - trail_w, verb_size, SOLERS_CELL_TEXT_FAINT.lerp(Color(0.95f, 0.96f, 0.98f), 0.35f + 0.25f * Math::sin(float(OS::get_singleton()->get_ticks_msec()) * Math::TAU / (SOLERS_SHIMMER_PERIOD * 1000.0f))));
			} else {
				draw_string(font, Point2(x, verb_baseline).floor(), label, HORIZONTAL_ALIGNMENT_LEFT, get_size().x - x - trail_w, verb_size, verb_color);
			}

			if (status == STATUS_ERROR && error_paragraph->get_line_count() > 0) {
				error_paragraph->draw(get_canvas_item(), Point2(20.0f * ed, header_h + 2.0f * ed), Color(SOLERS_CELL_ERROR, 0.92f));
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* SolersStatusCell                                                    */
/* ------------------------------------------------------------------ */

SolersStatusCell::SolersStatusCell() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_process_internal(true);
}

void SolersStatusCell::set_status(const String &p_text) {
	if (status_text == p_text) {
		return;
	}
	status_text = p_text;
	queue_redraw();
}

void SolersStatusCell::set_active(bool p_active) {
	set_process_internal(p_active);
	queue_redraw();
}

Size2 SolersStatusCell::get_minimum_size() const {
	return Size2(0, 22.0f * EDSCALE);
}

void SolersStatusCell::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			const float dt = float(get_process_delta_time());
			shimmer_phase = Math::fmod(shimmer_phase + dt / SOLERS_SHIMMER_PERIOD, 1.0f);
			queue_redraw();
		} break;
		case NOTIFICATION_DRAW: {
			const float ed = EDSCALE;
			const Ref<Font> font = solers_cell_font(this);
			if (font.is_null()) {
				break;
			}
			const float h = get_size().y;
			const int font_size = int(12 * ed);
			const float baseline = (h - font->get_height(font_size)) * 0.5f + font->get_ascent(font_size);
			const Color color = is_processing_internal() ? SOLERS_CELL_TEXT_FAINT.lerp(Color(0.95f, 0.96f, 0.98f), 0.35f + 0.25f * Math::sin(shimmer_phase * Math::TAU)) : SOLERS_CELL_TEXT_DIM;
			draw_string(font, Point2(0, baseline).floor(), status_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, color);
		} break;
	}
}
