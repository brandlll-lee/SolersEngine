/**************************************************************************/
/*  solers_chat_cells.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_chat_cells.h"

#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "editor/themes/editor_scale.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/editor/solers_markdown_view.h"
#include "scene/gui/box_container.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/style_box_flat.h"
#include "scene/theme/theme_db.h"

/* ------------------------------------------------------------------ */
/* Shared palette + draw helpers                                       */
/* ------------------------------------------------------------------ */

static const Color SOLERS_CELL_TEXT_PRIMARY = Color(0.961, 0.969, 0.984);
static const Color SOLERS_CELL_TEXT_DIM = Color(0.667, 0.690, 0.733);
static const Color SOLERS_CELL_TEXT_FAINT = Color(0.667, 0.690, 0.733, 0.78f);
static const Color SOLERS_CELL_OK = Color(0.478, 0.772, 0.525);
static const Color SOLERS_CELL_ERROR = Color(0.875, 0.478, 0.420);
// User bubble: Cursor-style accent-blue (#3b82f6) wash over the dark canvas.
static const Color SOLERS_CELL_BUBBLE_BG = Color(0.231, 0.510, 0.965, 0.26f);

// Shimmer sweep period for "Thinking"/status headers, seconds.
static constexpr float SOLERS_SHIMMER_PERIOD = 1.6f;

// Reasoning tail: wrapped lines kept visible while the model is thinking.
static constexpr int SOLERS_THINKING_TAIL_LINES = 4;

static Ref<Texture2D> solers_attachment_texture(const Dictionary &p_attachment) {
	const String path = String(p_attachment.get("local_path", String())).strip_edges();
	if (path.is_empty()) {
		return Ref<Texture2D>();
	}
	if (!FileAccess::exists(path)) {
		return Ref<Texture2D>();
	}
	Ref<Image> image = Image::load_from_file(path);
	if (image.is_null() || image->is_empty()) {
		return Ref<Texture2D>();
	}
	return ImageTexture::create_from_image(image);
}

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
	return p_control->get_theme_font(SceneStringName(font), SNAME("Label"));
}

static Ref<Font> solers_cell_mono_font(const Control *p_control) {
	Ref<Font> mono = p_control->get_theme_font(SNAME("source"), SNAME("EditorFonts"));
	if (mono.is_valid()) {
		return mono;
	}
	return solers_cell_font(p_control);
}

// Draws a left-to-right shimmer sweep across `p_text` (the Codex "thinking"
// header treatment): every glyph sits at the dim base color, and a soft
// highlight band sweeps across the word. Returns the drawn width.
static float solers_draw_shimmer_text(Control *p_control, const Point2 &p_baseline_pos, const String &p_text, const Ref<Font> &p_font, int p_font_size, float p_phase, const Color &p_base, const Color &p_lit) {
	if (p_font.is_null() || p_text.is_empty()) {
		return 0.0f;
	}
	const float total_width = p_font->get_string_size(p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, p_font_size).x;
	// Sweep center runs past both edges so the band fully enters and exits.
	const float sweep_x = (p_phase * 1.5f - 0.25f) * total_width;
	const float band = MAX(10.0f * EDSCALE, total_width * 0.30f);

	const RID ci = p_control->get_canvas_item();
	Point2 pos = p_baseline_pos;
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		const float glyph_center = pos.x - p_baseline_pos.x + p_font->get_char_size(c, p_font_size).x * 0.5f;
		const float d = Math::abs(glyph_center - sweep_x) / band;
		const float w = CLAMP(1.0f - d, 0.0f, 1.0f);
		const Color color = p_base.lerp(p_lit, w * w);
		pos.x += p_font->draw_char(ci, pos, c, p_font_size, color);
	}
	return pos.x - p_baseline_pos.x;
}

static String _summarize_streaming_tool_args(const String &p_raw) {
	const int key_pos = p_raw.find("\"path\"");
	if (key_pos >= 0) {
		const int colon_pos = p_raw.find(":", key_pos + 6);
		const int quote_pos = colon_pos >= 0 ? p_raw.find("\"", colon_pos + 1) : -1;
		const int end_pos = quote_pos >= 0 ? p_raw.find("\"", quote_pos + 1) : -1;
		if (end_pos > quote_pos) {
			return "path: " + p_raw.substr(quote_pos + 1, end_pos - quote_pos - 1) + ", (streaming...)";
		}
	}
	return "(streaming...)";
}

// Compact, human-scannable summary of a tool-call argument object:
//   {"path":"res://main.tscn","type":"Node2D"} -> path: res://main.tscn, type: Node2D
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
	const Error parse_err = json->parse(raw);
	if (parse_err != OK) {
		return _summarize_streaming_tool_args(raw);
	}
	const Variant parsed = json->get_data();
	if (parsed.get_type() != Variant::DICTIONARY) {
		return raw.left(96);
	}
	const Dictionary args = parsed;
	String out;
	int listed = 0;
	for (const KeyValue<Variant, Variant> &kv : args) {
		if (listed >= 4 || out.length() > 88) {
			out += ", ...";
			break;
		}
		String value;
		switch (kv.value.get_type()) {
			case Variant::STRING: {
				value = String(kv.value);
				if (value.length() > 40) {
					value = value.left(37) + "...";
				}
			} break;
			case Variant::DICTIONARY:
			case Variant::ARRAY: {
				value = JSON::stringify(kv.value, "", false, true);
				if (value.length() > 40) {
					value = value.left(37) + "...";
				}
			} break;
			default: {
				value = kv.value.stringify();
			} break;
		}
		if (!out.is_empty()) {
			out += ", ";
		}
		out += String(kv.key) + ": " + value;
		listed++;
	}
	return out;
}

/* ------------------------------------------------------------------ */
/* SolersUserBubble                                                    */
/* ------------------------------------------------------------------ */

SolersUserBubble::SolersUserBubble() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_h_size_flags(SIZE_EXPAND_FILL);
	paragraph.instantiate();
	paragraph->set_break_flags(TextServer::BREAK_MANDATORY | TextServer::BREAK_WORD_BOUND | TextServer::BREAK_ADAPTIVE);
}

void SolersUserBubble::set_message(const String &p_text) {
	text = p_text;
	shaped_for_width = -1.0f;
	_shape(get_size().x);
	queue_redraw();
}

void SolersUserBubble::set_attachments(const Array &p_attachments) {
	attachments = p_attachments.duplicate(true);
	attachment_textures.clear();
	for (int i = 0; i < attachments.size(); i++) {
		const Variant item = attachments[i];
		if (item.get_type() != Variant::DICTIONARY) {
			attachment_textures.push_back(Ref<Texture2D>());
			continue;
		}
		attachment_textures.push_back(solers_attachment_texture(item));
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
	paragraph->clear();
	if (font.is_valid() && !text.is_empty()) {
		paragraph->set_line_spacing(3.0f * ed);
		// Measure unwrapped first; only cap the width when the prompt is
		// actually wider than the readable column.
		paragraph->set_width(-1);
		paragraph->add_string(text, font, font_size);
		const float natural_width = paragraph->get_non_wrapped_size().x;
		const float max_text_width = MIN(cell_width * 0.78f, 380.0f * ed);
		paragraph->set_width(MIN(natural_width + 1.0f, max_text_width));
		text_size = paragraph->get_size();
	} else {
		text_size = Size2();
	}

	const float pad_v = 8.0f * ed;
	const float thumb = 56.0f * ed;
	const float gap = 8.0f * ed;
	const int attachment_count = attachment_textures.size();
	const float attachments_height = attachment_count > 0 ? thumb + (text_size.y > 0.0f ? gap : 0.0f) : 0.0f;
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
			const int attachment_count = attachment_textures.size();
			const float attachments_width = attachment_count > 0 ? attachment_count * thumb + MAX(0, attachment_count - 1) * gap : 0.0f;
			const float attachments_height = attachment_count > 0 ? thumb + (text_size.y > 0.0f ? gap : 0.0f) : 0.0f;
			const Size2 bubble_size(MAX(text_size.x, attachments_width) + pad_h * 2.0f, attachments_height + text_size.y + pad_v * 2.0f);
			const Rect2 bubble(Point2(get_size().x - bubble_size.x, 0), bubble_size);
			solers_cell_fill(this, bubble, SOLERS_CELL_BUBBLE_BG, 14.0f * ed);
			float y = bubble.position.y + pad_v;
			if (attachment_count > 0) {
				float x = bubble.position.x + pad_h;
				for (int i = 0; i < attachment_count; i++) {
					const Rect2 rect(Point2(x, y), Size2(thumb, thumb));
					solers_cell_fill(this, rect, Color(0, 0, 0, 0.18f), 8.0f * ed, Color(1, 1, 1, 0.11f));
					const Ref<Texture2D> texture = attachment_textures[i];
					if (texture.is_valid()) {
						const Size2 source = texture->get_size();
						const float crop = MIN(source.x, source.y);
						const Rect2 source_rect(Point2((source.x - crop) * 0.5f, (source.y - crop) * 0.5f), Size2(crop, crop));
						draw_texture_rect_region(texture, rect, source_rect);
					}
					x += thumb + gap;
				}
				y += attachments_height;
			}
			if (!text.is_empty()) {
				paragraph->draw(get_canvas_item(), Point2(bubble.position.x + pad_h, y), SOLERS_CELL_TEXT_PRIMARY);
			}
		} break;
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
	float width = get_size().x;
	if (width <= 60.0f * EDSCALE) {
		if (Control *parent = get_parent_control()) {
			width = parent->get_size().x;
		}
	}
	if (width <= 60.0f * EDSCALE) {
		set_process_internal(true);
		return;
	}
	if (rendered_chars == len && Math::is_equal_approx(rendered_width, width)) {
		if (rendered_caret == caret) {
			return;
		}
		rendered_caret = caret;
		markdown_view->set_position(Point2());
		markdown_view->set_size(Size2(width, markdown_view->get_size().y));
		markdown_view->append_markdown_delta(String(), caret);
		const float old_height = cell_height;
		cell_height = MAX(markdown_view->get_content_height(), 18.0f * EDSCALE);
		markdown_view->set_size(Size2(width, cell_height));
		if (!Math::is_equal_approx(old_height, cell_height)) {
			update_minimum_size();
			if (content_changed.is_valid()) {
				content_changed.call();
			}
		}
		return;
	}
	const int previous_rendered_chars = rendered_chars;
	const bool can_append = !stream_done && previous_rendered_chars >= 0 && len > previous_rendered_chars && rendered_caret && Math::is_equal_approx(rendered_width, width);
	rendered_chars = len;
	rendered_caret = caret;
	rendered_width = width;

	markdown_view->set_position(Point2());
	markdown_view->set_size(Size2(width, markdown_view->get_size().y));
	if (can_append) {
		markdown_view->append_markdown_delta(full_text.substr(previous_rendered_chars), caret);
	} else {
		markdown_view->set_markdown(full_text, caret);
	}

	const float old_height = cell_height;
	cell_height = MAX(markdown_view->get_content_height(), 18.0f * EDSCALE);
	markdown_view->set_size(Size2(width, cell_height));
	if (!Math::is_equal_approx(old_height, cell_height)) {
		update_minimum_size();
		if (content_changed.is_valid()) {
			content_changed.call();
		}
	}
}

Size2 SolersAssistantCell::get_minimum_size() const {
	return Size2(0, cell_height);
}

void SolersAssistantCell::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_RESIZED: {
			rendered_chars = -1;
			_update_markdown();
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			rendered_chars = -1;
			_update_markdown();
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
				header_w = solers_draw_shimmer_text(this, Point2(0, baseline), _header_text(), font, header_size, shimmer_phase, SOLERS_CELL_TEXT_FAINT, Color(0.95f, 0.96f, 0.98f));
			} else {
				const Color header_color = hovering ? SOLERS_CELL_TEXT_DIM.lerp(Color(1, 1, 1), 0.25f) : SOLERS_CELL_TEXT_DIM;
				draw_string(font, Point2(0, baseline).floor(), _header_text(), HORIZONTAL_ALIGNMENT_LEFT, -1, header_size, header_color);
				header_w = font->get_string_size(_header_text(), HORIZONTAL_ALIGNMENT_LEFT, -1, header_size).x;
				if (!reasoning.strip_edges().is_empty()) {
					Ref<Texture2D> chevron = SolersChatGlyphs::get(expanded ? SNAME("chevron_down") : SNAME("chevron_right"), int(Math::round(9.0f * ed)), 2.2f);
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
/* SolersPlanCell                                                      */
/* ------------------------------------------------------------------ */

SolersPlanCell::SolersPlanCell() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_h_size_flags(SIZE_EXPAND_FILL);
	body.instantiate();
	body->set_break_flags(TextServer::BREAK_MANDATORY | TextServer::BREAK_WORD_BOUND | TextServer::BREAK_ADAPTIVE);
}

String SolersPlanCell::_body_text() const {
	return format_plan_text(explanation, plan);
}

String SolersPlanCell::format_plan_text(const String &p_explanation, const Array &p_plan) {
	String text = p_explanation.strip_edges();
	for (int i = 0; i < p_plan.size(); i++) {
		const Dictionary item = p_plan[i];
		const String status = item.get("status", "pending");
		const String marker = status == "completed" ? String::utf8("✓ ") :
				status == "in_progress" ? String::utf8("→ ") :
										  String::utf8("○ ");
		if (!text.is_empty()) {
			text += "\n";
		}
		text += marker + String(item.get("step", String()));
	}
	return text;
}

void SolersPlanCell::set_plan(const String &p_explanation, const Array &p_plan) {
	explanation = p_explanation.strip_edges();
	plan = p_plan.duplicate(true);
	shaped_for_width = -1.0f;
	_shape(get_size().x);
	queue_redraw();
}

void SolersPlanCell::_shape(float p_cell_width) {
	const float ed = EDSCALE;
	const float width = MAX(p_cell_width, 60.0f * ed);
	if (Math::is_equal_approx(shaped_for_width, width)) {
		return;
	}
	shaped_for_width = width;

	body->clear();
	const String text = _body_text();
	const Ref<Font> font = solers_cell_font(this);
	float body_height = 0.0f;
	if (font.is_valid() && !text.is_empty()) {
		body->set_line_spacing(3.0f * ed);
		body->set_width(MAX(20.0f * ed, width - 24.0f * ed));
		body->add_string(text, font, int(12 * ed));
		body_height = body->get_size().y;
	} else if (!text.is_empty()) {
		body_height = (plan.size() + (explanation.is_empty() ? 0 : 1)) * 18.0f * ed;
	}

	const float old_height = cell_height;
	cell_height = 36.0f * ed + body_height + 10.0f * ed;
	if (!Math::is_equal_approx(old_height, cell_height)) {
		update_minimum_size();
		if (content_changed.is_valid()) {
			content_changed.call();
		}
	}
}

Size2 SolersPlanCell::get_minimum_size() const {
	return Size2(0, cell_height);
}

void SolersPlanCell::_notification(int p_what) {
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
		case NOTIFICATION_DRAW: {
			_shape(get_size().x);
			const float ed = EDSCALE;
			const Rect2 card(Point2(), get_size());
			solers_cell_fill(this, card, Color(0.12f, 0.14f, 0.18f, 0.72f), 10.0f * ed, Color(1, 1, 1, 0.08f));
			const Ref<Font> font = solers_cell_font(this);
			if (font.is_null()) {
				break;
			}
			const int header_size = int(12 * ed);
			const float baseline = 12.0f * ed + font->get_ascent(header_size);
			draw_string(font, Point2(12.0f * ed, baseline).floor(), TTR("Plan"), HORIZONTAL_ALIGNMENT_LEFT, -1, header_size, SOLERS_CELL_TEXT_PRIMARY);
			if (body->get_line_count() > 0) {
				body->draw(get_canvas_item(), Point2(12.0f * ed, 34.0f * ed), SOLERS_CELL_TEXT_DIM);
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* SolersToolCell                                                      */
/* ------------------------------------------------------------------ */

SolersToolCell::SolersToolCell() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_h_size_flags(SIZE_EXPAND_FILL);
	tool_glyph = SNAME("sparkle");
	error_paragraph.instantiate();
	error_paragraph->set_break_flags(TextServer::BREAK_MANDATORY | TextServer::BREAK_WORD_BOUND | TextServer::BREAK_ADAPTIVE);
}

void SolersToolCell::start(const String &p_tool_name, const String &p_arguments_json, const StringName &p_tool_glyph) {
	tool_name = p_tool_name.is_empty() ? String("tool") : p_tool_name;
	tool_glyph = p_tool_glyph == StringName() ? SNAME("sparkle") : p_tool_glyph;
	args_summary = solers_summarize_tool_args(p_arguments_json);
	status = STATUS_RUNNING;
	shaped_for_width = -1.0f;
	_shape(get_size().x);
	set_process_internal(true);
	queue_redraw();
}

void SolersToolCell::update(const String &p_tool_name, const String &p_arguments_json, const StringName &p_tool_glyph) {
	const String next_name = p_tool_name.is_empty() ? tool_name : p_tool_name;
	const String next_summary = solers_summarize_tool_args(p_arguments_json);
	const StringName next_glyph = p_tool_glyph == StringName() ? tool_glyph : p_tool_glyph;
	if (tool_name == next_name && args_summary == next_summary && tool_glyph == next_glyph) {
		return;
	}
	const bool glyph_changed = tool_glyph != next_glyph;
	tool_name = next_name.is_empty() ? String("tool") : next_name;
	tool_glyph = next_glyph;
	args_summary = next_summary;
	if (glyph_changed && content_changed.is_valid()) {
		content_changed.call();
	}
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
			const float type_cx = 14.0f * ed;
			const float icon_cy = header_h * 0.5f;

			Ref<Texture2D> type_icon = SolersChatGlyphs::get(tool_glyph, int(Math::round(12.0f * ed)), 2.1f);
			if (type_icon.is_valid()) {
				const Color icon_color = status == STATUS_ERROR ? SOLERS_CELL_ERROR : SOLERS_CELL_TEXT_FAINT;
				draw_texture(type_icon, Point2(type_cx - type_icon->get_width() * 0.5f, icon_cy - type_icon->get_height() * 0.5f).floor(), icon_color);
			}

			// Name + argument summary + duration.
			const int name_size = int(12 * ed);
			const int args_size = int(11 * ed);
			float x = 28.0f * ed;
			const float name_baseline = (header_h - font->get_height(name_size)) * 0.5f + font->get_ascent(name_size);
			if (status == STATUS_RUNNING) {
				solers_draw_shimmer_text(this, Point2(x, name_baseline).floor(), tool_name, font, name_size, Math::fmod(float(OS::get_singleton()->get_ticks_msec()) / (SOLERS_SHIMMER_PERIOD * 1000.0f), 1.0f), SOLERS_CELL_TEXT_FAINT, Color(0.95f, 0.96f, 0.98f));
			} else {
				draw_string(font, Point2(x, name_baseline).floor(), tool_name, HORIZONTAL_ALIGNMENT_LEFT, -1, name_size, status == STATUS_ERROR ? SOLERS_CELL_ERROR : Color(0.90f, 0.91f, 0.94f));
			}
			x += font->get_string_size(tool_name, HORIZONTAL_ALIGNMENT_LEFT, -1, name_size).x + 8.0f * ed;

			String trail;
			if (status != STATUS_RUNNING && duration_msec >= 0) {
				trail = duration_msec >= 1000 ? vformat("%s s", String::num(double(duration_msec) / 1000.0, 1)) : vformat("%d ms", duration_msec);
			}
			float trail_w = 0.0f;
			if (!trail.is_empty() && mono.is_valid()) {
				trail_w = mono->get_string_size(trail, HORIZONTAL_ALIGNMENT_LEFT, -1, args_size).x;
				const float trail_baseline = (header_h - mono->get_height(args_size)) * 0.5f + mono->get_ascent(args_size);
				draw_string(mono, Point2(get_size().x - trail_w - 10.0f * ed, trail_baseline).floor(), trail, HORIZONTAL_ALIGNMENT_LEFT, -1, args_size, SOLERS_CELL_TEXT_FAINT);
				trail_w += 16.0f * ed;
			}

			if (!args_summary.is_empty() && mono.is_valid()) {
				const float args_avail = get_size().x - x - trail_w - 10.0f * ed;
				if (args_avail > 24.0f * ed) {
					String shown = args_summary;
					while (shown.length() > 4 && mono->get_string_size(shown, HORIZONTAL_ALIGNMENT_LEFT, -1, args_size).x > args_avail) {
						shown = shown.left(shown.length() - 4) + "...";
					}
					const float args_baseline = (header_h - mono->get_height(args_size)) * 0.5f + mono->get_ascent(args_size);
					draw_string(mono, Point2(x, args_baseline).floor(), shown, HORIZONTAL_ALIGNMENT_LEFT, -1, args_size, SOLERS_CELL_TEXT_FAINT);
				}
			}

			// Error detail block.
			if (status == STATUS_ERROR && error_paragraph->get_line_count() > 0) {
				error_paragraph->draw(get_canvas_item(), Point2(28.0f * ed, header_h + 2.0f * ed), Color(SOLERS_CELL_ERROR, 0.92f));
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* SolersToolGroupCell                                                 */
/* ------------------------------------------------------------------ */

SolersToolGroupCell::SolersToolGroupCell() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_default_cursor_shape(CURSOR_POINTING_HAND);

	body = memnew(VBoxContainer);
	body->set_mouse_filter(MOUSE_FILTER_IGNORE); // The whole group is one click surface.
	body->add_theme_constant_override("separation", int(8 * EDSCALE));
	body->set_visible(false);
	add_child(body);
}

float SolersToolGroupCell::_header_height() const {
	return 28.0f * EDSCALE;
}

SolersToolCell *SolersToolGroupCell::add_tool() {
	SolersToolCell *cell = memnew(SolersToolCell);
	cell->set_content_changed_callback(callable_mp(this, &SolersToolGroupCell::_on_child_changed));
	body->add_child(cell);
	total++;
	running++;
	set_process_internal(true);
	_relayout();
	queue_redraw();
	return cell;
}

void SolersToolGroupCell::note_finished(bool p_ok) {
	running = MAX(0, running - 1);
	if (!p_ok) {
		error_count++;
	}
	if (running == 0) {
		set_process_internal(false);
	}
	queue_redraw();
}

void SolersToolGroupCell::settle() {
	running = 0;
	set_process_internal(false);
	queue_redraw();
}

void SolersToolGroupCell::_on_child_changed() {
	_relayout();
	queue_redraw();
	if (content_changed.is_valid()) {
		content_changed.call();
	}
}

void SolersToolGroupCell::_relayout() {
	const float ed = EDSCALE;
	const float width = MAX(get_size().x, 60.0f * ed);
	const float header_h = _header_height();
	const float gap = 6.0f * ed;

	body->set_visible(expanded);
	float body_h = 0.0f;
	if (expanded) {
		const float indent = 8.0f * ed;
		body_h = body->get_combined_minimum_size().y;
		body->set_position(Point2(indent, header_h + gap));
		body->set_size(Size2(width - indent, body_h));
	}

	const float old_height = cell_height;
	cell_height = header_h + (expanded ? gap + body_h : 0.0f);
	if (!Math::is_equal_approx(old_height, cell_height)) {
		update_minimum_size();
	}
}

Size2 SolersToolGroupCell::get_minimum_size() const {
	return Size2(0, cell_height);
}

void SolersToolGroupCell::gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT && mb->is_pressed() &&
			mb->get_position().y <= _header_height()) {
		expanded = !expanded;
		_relayout();
		if (content_changed.is_valid()) {
			content_changed.call();
		}
		queue_redraw();
		accept_event();
	}
}

void SolersToolGroupCell::_notification(int p_what) {
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
			_relayout();
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			_relayout();
			queue_redraw();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			queue_redraw();
		} break;
		case NOTIFICATION_DRAW: {
			const float ed = EDSCALE;
			const float header_h = _header_height();
			const Rect2 header_rect(Point2(), Size2(get_size().x, header_h));
			if (hovering) {
				solers_cell_fill(this, header_rect, Color(1, 1, 1, 0.045f), 7.0f * ed);
			}

			const Ref<Font> font = solers_cell_font(this);
			if (font.is_null()) {
				break;
			}

			const int font_size = int(12 * ed);
			const float baseline = (header_h - font->get_height(font_size)) * 0.5f + font->get_ascent(font_size);
			float x = 8.0f * ed;
			const float icon_step = 17.0f * ed;
			for (int i = 0; i < body->get_child_count() && i < 3; i++) {
				SolersToolCell *tool_cell = Object::cast_to<SolersToolCell>(body->get_child(i));
				if (!tool_cell) {
					continue;
				}
				const Color tint = tool_cell->get_status() == SolersToolCell::STATUS_ERROR ? SOLERS_CELL_ERROR : SOLERS_CELL_TEXT_DIM;
				Ref<Texture2D> icon = SolersChatGlyphs::get(tool_cell->get_tool_glyph(), int(Math::round(12.0f * ed)), 2.1f);
				if (icon.is_valid()) {
					draw_texture(icon, Point2(x, (header_h - icon->get_height()) * 0.5f).floor(), tint);
					x += icon_step;
				}
			}
			const String label = vformat(String::utf8("已运行 %d 条命令"), total);
			x += 4.0f * ed;
			const float phase = Math::fmod(float(OS::get_singleton()->get_ticks_msec()) / (SOLERS_SHIMMER_PERIOD * 1000.0f), 1.0f);
			const float label_w = running > 0 ?
					solers_draw_shimmer_text(this, Point2(x, baseline).floor(), label, font, font_size, phase, SOLERS_CELL_TEXT_FAINT, Color(0.95f, 0.96f, 0.98f)) :
					font->get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
			if (running == 0) {
				draw_string(font, Point2(x, baseline).floor(), label, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, SOLERS_CELL_TEXT_DIM);
			}
			x += label_w + 6.0f * ed;
			Ref<Texture2D> chevron = SolersChatGlyphs::get(expanded ? SNAME("chevron_down") : SNAME("chevron_right"), int(Math::round(9.0f * ed)), 2.2f);
			if (chevron.is_valid()) {
				draw_texture(chevron, Point2(x, (header_h - chevron->get_height()) * 0.5f).floor(), SOLERS_CELL_TEXT_DIM);
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
			solers_draw_shimmer_text(this, Point2(0, baseline), status_text, font, font_size, shimmer_phase, SOLERS_CELL_TEXT_FAINT, Color(0.95f, 0.96f, 0.98f));
		} break;
	}
}
