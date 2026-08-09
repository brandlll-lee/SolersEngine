/**************************************************************************/
/*  solers_dock.cpp                                                       */
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

#include "solers_dock.h"

#include "solers_pm_ai_view.h"
#include "solers_ui_theme.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/input/input_event.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/callable_mp.h"
#include "core/os/time.h"
#include "core/templates/hash_set.h"
#include "core/version.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "main/app_icon.gen.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/font.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/style_box.h"
#include "scene/resources/style_box_flat.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_mention.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/editor/solers_chat_cells.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_models_dev.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "modules/solers_ai/protocol/solers_rpc_server.h"

constexpr float SOLERS_COMPOSER_TEXT_MIN_HEIGHT = 48.0f;
constexpr float SOLERS_COMPOSER_TEXT_MAX_HEIGHT = 220.0f;
constexpr float SOLERS_COMPOSER_TOOLBAR_HEIGHT = 30.0f;
// Top/bottom composer padding. Keep the toolbar visually attached to the prompt.
constexpr float SOLERS_COMPOSER_VERTICAL_CHROME = 20.0f;
constexpr int SOLERS_MENTION_VISIBLE_ROWS = 4;
static Ref<StyleBoxFlat> solers_row_styles[7];
static real_t solers_row_style_scale = 0;

// Codex-calibrated surface palette.
static const Color SOLERS_BG = Color(0.030, 0.030, 0.023);
// Composer / chip colors: shared authority in solers_chat_widgets.h
#define SOLERS_COMPOSER_BG solers_composer_bg()
static const Color SOLERS_POPUP_BG = Color(0.118, 0.118, 0.122);
#define SOLERS_COMPOSER_BORDER solers_composer_border()
// Primary text: high contrast for readability on dark backgrounds.
static const Color SOLERS_TEXT_PRIMARY = Color(0.961, 0.969, 0.984);
// Body text: comfortable reading with slightly reduced contrast for hierarchy.
static const Color SOLERS_TEXT_BODY = Color(0.918, 0.929, 0.945);
// Dim text: secondary info and status labels.
static const Color SOLERS_TEXT_DIM = Color(0.667, 0.690, 0.733);
static const Color SOLERS_TEXT_META = Color(0.735, 0.755, 0.790);
// Placeholder text: subtle cue in the input field.
static const Color SOLERS_TEXT_PLACEHOLDER = Color(0.345, 0.357, 0.388);

static Ref<StyleBoxFlat> solers_make_stylebox(const Color &p_bg, const Color &p_border, int p_radius, int p_padding, bool p_shadow = false) {
	Ref<StyleBoxFlat> style(memnew(StyleBoxFlat));
	style->set_bg_color(p_bg);
	style->set_border_color(p_border);
	style->set_border_width_all(p_border.a > 0.0 ? 1 : 0);
	style->set_corner_radius_all(p_radius * EDSCALE);
	style->set_content_margin_all(p_padding * EDSCALE);
	if (p_shadow) {
		style->set_shadow_color(Color(0, 0, 0, 0.22));
		style->set_shadow_size(7 * EDSCALE);
		style->set_shadow_offset(Point2(0, 2 * EDSCALE));
	}
	return style;
}

static Ref<StyleBoxFlat> solers_make_stylebox_margins(const Color &p_bg, int p_radius, int p_left, int p_top, int p_right, int p_bottom) {
	Ref<StyleBoxFlat> style = solers_make_stylebox(p_bg, Color(0, 0, 0, 0), p_radius, 0);
	style->set_content_margin_individual(p_left * EDSCALE, p_top * EDSCALE, p_right * EDSCALE, p_bottom * EDSCALE);
	return style;
}

static Label *solers_make_session_group(const String &p_title) {
	Label *label = memnew(Label);
	label->set_text(p_title);
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	label->set_custom_minimum_size(Size2(0, 30 * EDSCALE));
	label->set_vertical_alignment(VERTICAL_ALIGNMENT_BOTTOM);
	label->add_theme_font_size_override(SceneStringName(font_size), int(11 * EDSCALE));
	label->add_theme_color_override(SceneStringName(font_color), SOLERS_TEXT_DIM);
	return label;
}

// Display title: peel owned prompt_block, then leading @tokens so the rail
// shows human text (not agent JSON appendices).
static String solers_session_display_title(const String &p_raw) {
	String raw = SolersMention::strip_prompt_block(p_raw.strip_edges().replace("\r", " ").replace("\n", " ").strip_edges());
	String t = raw;
	String first_token;
	while (t.begins_with("@")) {
		int end = 1;
		while (end < t.length() && t[end] != ' ' && t[end] != '\t') {
			end++;
		}
		if (first_token.is_empty()) {
			first_token = t.substr(0, end);
		}
		t = t.substr(end).strip_edges();
	}
	if (!t.is_empty()) {
		return t;
	}
	if (!first_token.is_empty()) {
		const int colon = first_token.find_char(':');
		const String body = colon >= 0 ? first_token.substr(colon + 1) : first_token.substr(1);
		return body.contains("/") ? body.get_file() : body;
	}
	return raw;
}

static void solers_style_model_popup_row(Button *p_row, bool p_selected = false) {
	p_row->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	p_row->set_toggle_mode(true);
	p_row->set_pressed(p_selected);
	p_row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p_row->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	p_row->set_clip_text(true);
	p_row->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	p_row->set_custom_minimum_size(Size2(0, 28 * EDSCALE));
	p_row->add_theme_font_size_override("font_size", int(13 * EDSCALE));
	p_row->add_theme_color_override(SceneStringName(font_color), SOLERS_TEXT_BODY);
	p_row->add_theme_color_override("font_hover_color", SOLERS_TEXT_PRIMARY);
	p_row->add_theme_color_override("font_pressed_color", SOLERS_TEXT_PRIMARY);
	p_row->add_theme_color_override("font_hover_pressed_color", SOLERS_TEXT_PRIMARY);
	if (solers_row_styles[0].is_null() || !Math::is_equal_approx(solers_row_style_scale, EDSCALE)) {
		solers_row_style_scale = EDSCALE;
		solers_row_styles[0] = solers_make_stylebox_margins(Color(0, 0, 0, 0), 5, 8, 3, 8, 3);
		solers_row_styles[1] = solers_make_stylebox_margins(Color(1, 1, 1, 0.060), 5, 8, 3, 8, 3);
		solers_row_styles[2] = solers_make_stylebox_margins(Color(1, 1, 1, 0.045), 5, 8, 3, 8, 3);
		solers_row_styles[3] = solers_make_stylebox_margins(Color(1, 1, 1, 0.070), 5, 8, 3, 8, 3);
		solers_row_styles[4] = solers_make_stylebox_margins(Color(0, 0, 0, 0), 5, 8, 3, 8, 3);
		solers_row_styles[5] = solers_make_stylebox_margins(Color(1, 1, 1, 0.045), 7, 10, 5, 10, 5);
		solers_row_styles[6] = solers_make_stylebox_margins(Color(1, 1, 1, 0.085), 7, 10, 5, 10, 5);
	}
	p_row->add_theme_style_override("normal", solers_row_styles[0]);
	p_row->add_theme_style_override("hover", solers_row_styles[1]);
	p_row->add_theme_style_override("pressed", solers_row_styles[2]);
	p_row->add_theme_style_override("hover_pressed", solers_row_styles[3]);
	p_row->add_theme_style_override("focus", solers_row_styles[4]);
}

static void solers_style_session_button(Button *p_button, bool p_session_row) {
	solers_style_model_popup_row(p_button);
	p_button->set_focus_mode(Control::FOCUS_ALL);
	p_button->set_toggle_mode(p_session_row);
	p_button->set_custom_minimum_size(Size2(0, (p_session_row ? 60 : 38) * EDSCALE));
	p_button->add_theme_constant_override("h_separation", int(9 * EDSCALE));
	p_button->add_theme_style_override("hover", solers_row_styles[5]);
	p_button->add_theme_style_override("pressed", solers_row_styles[6]);
	p_button->add_theme_style_override("hover_pressed", solers_row_styles[6]);
}

static Button *solers_make_model_popup_group(const String &p_title, const Ref<Texture2D> &p_icon = Ref<Texture2D>()) {
	Button *label = memnew(Button);
	label->set_disabled(true);
	label->set_focus_mode(Control::FOCUS_NONE);
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	label->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	label->set_clip_text(true);
	label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	label->set_custom_minimum_size(Size2(0, 24 * EDSCALE));
	label->set_text(p_title);
	label->add_theme_font_size_override("font_size", int(12 * EDSCALE));
	label->add_theme_color_override("font_disabled_color", Color(0.50, 0.55, 0.58));
	label->add_theme_style_override("disabled", solers_make_stylebox_margins(Color(0, 0, 0, 0), 0, 8, 8, 8, 2));
	if (p_icon.is_valid()) {
		label->set_button_icon(p_icon);
		label->add_theme_color_override("icon_disabled_color", Color(1, 1, 1, 0.62));
		label->add_theme_constant_override("h_separation", int(6 * EDSCALE));
	}
	return label;
}

static Button *solers_make_model_popup_row(const String &p_label, const String &p_model_id, bool p_selected) {
	Button *row = memnew(Button);
	row->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
	solers_style_model_popup_row(row, p_selected);
	row->set_text(p_label.is_empty() ? p_model_id : p_label);
	row->set_tooltip_text(p_label == p_model_id || p_model_id.is_empty() ? String() : p_model_id);
	if (p_selected) {
		row->set_button_icon(SolersIcons::get(SNAME("check"), int(Math::round(12.0f * EDSCALE))));
		row->set_icon_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	}
	return row;
}

// Root cascade-menu row: [icon] Name .... value ›  (submenu opens beside it).
static Button *solers_make_model_menu_parent_row(const Ref<Texture2D> &p_icon, const String &p_label, const String &p_value) {
	Button *row = memnew(Button);
	solers_style_model_popup_row(row, false);
	row->set_custom_minimum_size(Size2(0, 30 * EDSCALE));

	HBoxContainer *box = memnew(HBoxContainer);
	box->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	box->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	box->set_offset(SIDE_LEFT, 8 * EDSCALE);
	box->set_offset(SIDE_RIGHT, -8 * EDSCALE);
	box->add_theme_constant_override("separation", int(6 * EDSCALE));
	row->add_child(box);

	if (p_icon.is_valid()) {
		TextureRect *icon = memnew(TextureRect);
		icon->set_texture(p_icon);
		icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
		icon->set_custom_minimum_size(Size2(14, 14) * EDSCALE);
		icon->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
		icon->set_self_modulate(Color(1, 1, 1, 0.85));
		box->add_child(icon);
	}

	Label *name = memnew(Label(p_label));
	name->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	name->add_theme_font_size_override(SceneStringName(font_size), int(13 * EDSCALE));
	name->add_theme_color_override(SceneStringName(font_color), SOLERS_TEXT_BODY);
	box->add_child(name);

	Label *value = memnew(Label(p_value));
	value->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
	value->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	value->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	value->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	value->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	value->add_theme_font_size_override(SceneStringName(font_size), int(12 * EDSCALE));
	value->add_theme_color_override(SceneStringName(font_color), SOLERS_TEXT_DIM);
	box->add_child(value);

	TextureRect *chevron = memnew(TextureRect);
	chevron->set_texture(SolersIcons::get(SNAME("chevron_right"), int(Math::round(10.0f * EDSCALE))));
	chevron->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	chevron->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	chevron->set_self_modulate(Color(0.50, 0.51, 0.55));
	box->add_child(chevron);

	// Buttons ignore child Controls when sizing; account for the row content.
	row->set_custom_minimum_size(Size2(box->get_combined_minimum_size().x + 16 * EDSCALE, 30 * EDSCALE));
	return row;
}

static void solers_add_unique_model(Array &r_models, HashSet<String> &r_seen, const String &p_model) {
	const String model = p_model.strip_edges();
	if (model.is_empty() || r_seen.has(model)) {
		return;
	}
	r_seen.insert(model);
	r_models.push_back(model);
}

static String solers_model_display_label(const String &p_model_id, const Dictionary &p_model_info, const Dictionary &p_model_labels) {
	const String catalog_name = String(p_model_info.get("name", String())).strip_edges();
	if (!catalog_name.is_empty()) {
		return catalog_name;
	}
	const String labeled = String(p_model_labels.get(p_model_id, String())).strip_edges();
	if (!labeled.is_empty()) {
		return labeled;
	}
	// Catalog is the authority; slug humanization is only the last resort.
	return p_model_id.replace("-", " ").replace("_", " ").strip_edges().capitalize();
}

static String solers_compact_label(const String &p_label) {
	const String label = p_label.strip_edges();
	if (label.length() <= 28) {
		return label;
	}
	return label.substr(0, 25) + "...";
}

static SolersModelsDev *solers_dock_models_dev(SolersSettingsService *p_settings) {
	return p_settings && p_settings->get_provider_registry() ? p_settings->get_provider_registry()->get_models_dev() : nullptr;
}

static String solers_resolve_model_display(SolersSettingsService *p_settings, const Dictionary &p_provider_data, const String &p_model) {
	const Dictionary profile = p_provider_data.get("profile", Dictionary());
	const String provider = String(p_provider_data.get("provider", String())).strip_edges();
	const String catalog_id = profile.get("catalog_provider", provider);
	SolersModelsDev *models_dev = solers_dock_models_dev(p_settings);
	const Dictionary model_info = models_dev ? models_dev->get_model(StringName(catalog_id), p_model) : Dictionary();
	return solers_model_display_label(p_model, model_info, profile.get("model_labels", Dictionary()));
}

static String solers_reasoning_effort_label(const String &p_effort) {
	return p_effort.is_empty() ? String("Default") : p_effort.capitalize();
}

static String solers_attachment_dir() {
	return solers_session_dir().path_join("attachments");
}

class SolersAttachmentThumb : public Control {
	GDCLASS(SolersAttachmentThumb, Control);

	TextureRect *image_rect = nullptr;
	SolersGlyphButton *remove_button = nullptr;

	bool _mouse_inside_tree() const {
		const Vector2 mouse = get_local_mouse_position();
		if (Rect2(Vector2(), get_size()).has_point(mouse)) {
			return true;
		}
		return remove_button && remove_button->is_visible() && Rect2(remove_button->get_position(), remove_button->get_size()).has_point(mouse);
	}

protected:
	void _notification(int p_what) {
		switch (p_what) {
			case NOTIFICATION_RESIZED: {
				if (image_rect) {
					image_rect->set_position(Point2(EDSCALE, EDSCALE));
					image_rect->set_size(get_size() - Size2(2 * EDSCALE, 2 * EDSCALE));
				}
				if (remove_button) {
					const Size2 close_size = remove_button->get_size();
					remove_button->set_position(Point2(get_size().x - close_size.x - 4 * EDSCALE, 4 * EDSCALE));
				}
			} break;
			case NOTIFICATION_MOUSE_ENTER: {
				if (remove_button) {
					remove_button->show();
				}
			} break;
			case NOTIFICATION_MOUSE_EXIT: {
				callable_mp(this, &SolersAttachmentThumb::_sync_hover).call_deferred();
			} break;
			case NOTIFICATION_DRAW: {
				draw_rect(Rect2(Point2(), get_size()), Color(0, 0, 0, 0.18f));
				draw_rect(Rect2(Point2(0.5f, 0.5f), get_size() - Size2(1, 1)), Color(1, 1, 1, 0.105f), false, MAX(1.0f, EDSCALE));
			} break;
		}
	}
	static void _bind_methods() {}

public:
	void _sync_hover() {
		if (remove_button && !_mouse_inside_tree()) {
			remove_button->hide();
		}
	}

	void setup(const Dictionary &p_attachment, const Callable &p_remove) {
		const float ed = EDSCALE;
		set_mouse_filter(MOUSE_FILTER_STOP);
		set_clip_contents(true);
		set_custom_minimum_size(Size2(58 * ed, 58 * ed));
		const String filename = String(p_attachment.get("filename", String())).strip_edges();
		set_tooltip_text(filename.is_empty() ? String(p_attachment.get("id", "image")) : filename);

		image_rect = memnew(TextureRect);
		image_rect->set_mouse_filter(MOUSE_FILTER_IGNORE);
		image_rect->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		image_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
		image_rect->set_texture(solers_attachment_texture(p_attachment));
		add_child(image_rect);

		remove_button = memnew(SolersGlyphButton);
		remove_button->configure(SNAME("cross"), SolersGlyphButton::SKIN_GHOST, TTR("Remove image"), 11);
		const Size2 close_size(20 * ed, 20 * ed);
		remove_button->set_custom_minimum_size(close_size);
		remove_button->set_size(close_size);
		remove_button->set_mouse_filter(MOUSE_FILTER_STOP);
		remove_button->set_pressed_callback(p_remove);
		remove_button->hide();
		add_child(remove_button);
	}
};

static String solers_tool_ui_kind_for_name(const SolersToolRegistry *p_registry, const String &p_name) {
	const Dictionary tool = p_registry && !p_name.is_empty() ? p_registry->get_tool_definition(StringName(p_name)) : Dictionary();
	return tool.get("ui_kind", String());
}

PanelContainer *SolersDock::_create_panel_card(const Color &p_color, const Color &p_border_color, int p_radius, int p_padding) const {
	PanelContainer *panel = memnew(PanelContainer);
	panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	panel->add_theme_style_override("panel", solers_make_stylebox(p_color, p_border_color, p_radius, p_padding));
	return panel;
}

Control *SolersDock::_create_empty_state() const {
	VBoxContainer *state = memnew(VBoxContainer);
	state->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	state->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	state->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	state->add_theme_constant_override("separation", 10 * EDSCALE);

	Ref<Image> logo_image = memnew(Image(app_icon_png));
	TextureRect *logo = memnew(TextureRect);
	logo->set_name("SolersBrandMark");
	logo->set_texture(ImageTexture::create_from_image(logo_image));
	logo->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	logo->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	logo->set_custom_minimum_size(Size2(36, 36) * EDSCALE);
	logo->set_h_size_flags(Control::SIZE_SHRINK_CENTER);
	logo->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	logo->set_accessibility_name(TTR("Solers"));
	state->add_child(logo);

	Label *title = memnew(Label(TTR("What should we build?")));
	title->set_h_size_flags(Control::SIZE_SHRINK_CENTER);
	title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	title->add_theme_color_override(SceneStringName(font_color), SOLERS_TEXT_PRIMARY);
	title->add_theme_font_size_override(SceneStringName(font_size), 18 * EDSCALE);
	state->add_child(title);

	return state;
}

void SolersDock::_sync_layout_widths() {
	if (!composer_inset) {
		return;
	}
	const float width = chat_column ? chat_column->get_size().x : get_size().x;
	float margin = 20 * EDSCALE;
	if (width > 980 * EDSCALE) {
		const float target = MIN(width * 0.52f, 920 * EDSCALE);
		margin = MAX(margin, (width - target) * 0.5f);
	}
	const int margin_px = int(margin);
	if (composer_margin_px == margin_px) {
		return;
	}
	composer_margin_px = margin_px;
	composer_inset->add_theme_constant_override("margin_left", margin_px);
	composer_inset->add_theme_constant_override("margin_right", margin_px);
}

void SolersDock::_refresh_status() {
	// The only live status surfaces are the inline approval prompt and the
	// model chip; everything else here was wiring for the removed diagnostics.
	_sync_approval_panel();
	_refresh_model_chip();
}

void SolersDock::_refresh_model_chip() {
	if (!model_chip) {
		return;
	}

	if (!settings_service) {
		model_chip->set_texts(TTR("Model"), String());
		model_chip->set_tooltip_text(TTR("AI model settings are unavailable."));
		return;
	}

	const Dictionary provider_result = settings_service->get_provider_config();
	const Dictionary provider_data = provider_result.get("data", Dictionary());
	const String provider = String(provider_data.get("provider", String())).strip_edges();
	const String model = String(provider_data.get("model", String())).strip_edges();
	const String reasoning_effort = String(provider_data.get("reasoning_effort", String())).strip_edges();
	const String base_url = String(provider_data.get("base_url", String())).strip_edges();
	const Dictionary validation = provider_data.get("validation", Dictionary());
	const bool valid = validation.get("valid", false);
	const bool connected = provider_data.get("connected", false);
	const bool available = provider_data.get("available", false);

	if (!connected || model.is_empty()) {
		model_chip->set_leading_texture(Ref<Texture2D>());
		model_chip->set_texts(TTR("Model"), String());
		model_chip->set_tooltip_text(TTR("Connect a provider, then choose a model."));
		return;
	}

	const Dictionary chip_profile = provider_data.get("profile", Dictionary());
	const String catalog_id = chip_profile.get("catalog_provider", provider);
	model_chip->set_leading_texture(SolersIcons::provider_logo(catalog_id, int(Math::round(12.0f * EDSCALE))));

	if (!available) {
		model_chip->set_texts(solers_compact_label(solers_resolve_model_display(settings_service, provider_data, model)), TTR("Local only"));
		model_chip->set_tooltip_text(TTR("Local Models Only blocks this remote provider. Choose a local model or disable Local Models Only in Provider Settings."));
		return;
	}

	model_chip->set_texts(solers_compact_label(solers_resolve_model_display(settings_service, provider_data, model)), solers_reasoning_effort_label(reasoning_effort));

	String tooltip = vformat(TTR("Model: %s\nEffort: %s\nProvider: %s"), model, solers_reasoning_effort_label(reasoning_effort), provider.is_empty() ? TTR("unknown") : provider);
	if (!base_url.is_empty()) {
		tooltip += "\n" + vformat(TTR("Base URL: %s"), base_url);
	}
	tooltip += "\n" + String(valid ? TTR("Configuration is valid.") : TTR("Configuration needs attention in Provider Settings."));
	model_chip->set_tooltip_text(tooltip);
}

void SolersDock::_clear_empty_state() {
	if (empty_home) {
		empty_home->hide();
	}
	if (chat_scroll) {
		chat_scroll->show();
	}
	if (composer_inset && chat_column && composer_inset->get_parent() != chat_column) {
		if (composer_inset->get_parent()) {
			composer_inset->get_parent()->remove_child(composer_inset);
		}
		chat_column->add_child(composer_inset);
	}
	_sync_layout_widths();
}

void SolersDock::_show_empty_state() {
	if (!empty_home || !composer_inset) {
		return;
	}
	if (chat_scroll) {
		chat_scroll->hide();
	}
	if (composer_inset->get_parent() != empty_home) {
		if (composer_inset->get_parent()) {
			composer_inset->get_parent()->remove_child(composer_inset);
		}
		empty_home->add_child(composer_inset);
	}
	empty_home->show();
	_sync_layout_widths();
	_update_chat_input_height();
}

void SolersDock::_scroll_chat_to_bottom() {
	scroll_to_bottom_deferred = false;
	if (!chat_scroll) {
		return;
	}
	VScrollBar *bar = chat_scroll->get_v_scroll_bar();
	if (bar) {
		bar->set_value(bar->get_max());
	}
}

bool SolersDock::_is_scroll_pinned() const {
	if (!chat_scroll) {
		return true;
	}
	VScrollBar *bar = chat_scroll->get_v_scroll_bar();
	if (!bar || !bar->is_visible()) {
		return true;
	}
	// Follow the stream only while the user is at (or near) the bottom; a
	// reader who scrolled up keeps their place.
	return bar->get_value() + bar->get_page() >= bar->get_max() - 48.0 * EDSCALE;
}

void SolersDock::_on_cell_content_changed() {
	if (_is_scroll_pinned() && !scroll_to_bottom_deferred) {
		scroll_to_bottom_deferred = true;
		callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
	}
}

Control *SolersDock::_append_user_message(const String &p_message, const Array &p_attachments) {
	VBoxContainer *mount = _chat_mount();
	if (!mount) {
		return nullptr;
	}
	_clear_empty_state();

	SolersUserBubble *bubble = memnew(SolersUserBubble);
	bubble->set_meta("timeline_row", true);
	bubble->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
	mount->add_child(bubble);
	bubble->set_attachments(p_attachments);
	bubble->set_message(p_message);

	if (!history_mount) {
		callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
	}
	return bubble;
}

void SolersDock::_append_error_row(const String &p_text) {
	VBoxContainer *mount = _chat_mount();
	if (!mount) {
		return;
	}
	_clear_empty_state();

	Label *row = memnew(Label(p_text));
	row->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	row->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	row->add_theme_color_override("font_color", Color(0.875, 0.478, 0.420));
	row->add_theme_font_size_override(SceneStringName(font_size), 12 * EDSCALE);
	mount->add_child(row);

	callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
}

void SolersDock::_ensure_status_cell(const String &p_status) {
	VBoxContainer *mount = _chat_mount();
	if (!mount) {
		return;
	}
	_clear_empty_state();
	if (!status_cell) {
		status_cell = memnew(SolersStatusCell);
		mount->add_child(status_cell);
	}
	// The status row always trails the latest content.
	mount->move_child(status_cell, mount->get_child_count() - 1);
	status_cell->set_status(p_status);
	_on_cell_content_changed();
}

void SolersDock::_remove_status_cell() {
	if (status_cell) {
		status_cell->queue_free();
		status_cell = nullptr;
	}
}

void SolersDock::_settle_thinking_cell() {
	if (active_thinking_cell && active_thinking_cell->is_active()) {
		active_thinking_cell->set_done();
	}
}

void SolersDock::_settle_tool_group() {
	// Close the current "N actions" batch; the next tool call opens a new one.
	if (active_tool_group) {
		active_tool_group->settle();
		active_tool_group = nullptr;
		active_assistant_row = nullptr;
	}
}

VBoxContainer *SolersDock::_ensure_assistant_row() {
	VBoxContainer *mount = _chat_mount();
	if (!active_assistant_row && mount) {
		active_assistant_row = memnew(VBoxContainer);
		active_assistant_row->set_meta("timeline_row", true);
		if (pending_assistant_event_id > 0) {
			active_assistant_row->set_meta("timeline_event_id", pending_assistant_event_id);
			pending_assistant_event_id = -1;
		}
		active_assistant_row->add_theme_constant_override("separation", 8 * EDSCALE);
		mount->add_child(active_assistant_row);
	}
	return active_assistant_row;
}

SolersAssistantCell *SolersDock::_ensure_text_cell() {
	if (active_text_cell) {
		return active_text_cell;
	}
	VBoxContainer *mount = _ensure_assistant_row();
	if (!mount) {
		return nullptr;
	}
	_clear_empty_state();
	active_text_cell = memnew(SolersAssistantCell);
	active_text_cell->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
	mount->add_child(active_text_cell);
	return active_text_cell;
}

void SolersDock::_finish_turn_cells() {
	_settle_thinking_cell();
	_settle_tool_group();
	if (active_text_cell) {
		active_text_cell->finalize(String());
	}
	active_thinking_cell = nullptr;
	active_text_cell = nullptr;
	active_assistant_row = nullptr;
	pending_assistant_event_id = -1;
	tool_cells_by_id.clear();
	last_started_tool_cell = nullptr;
	_remove_status_cell();
}

void SolersDock::_clear_chat_view(bool p_show_empty) {
	scroll_to_bottom_deferred = false;
	active_thinking_cell = nullptr;
	active_text_cell = nullptr;
	active_assistant_row = nullptr;
	pending_assistant_event_id = -1;
	status_cell = nullptr;
	active_tool_group = nullptr;
	tool_cells_by_id.clear();
	last_started_tool_cell = nullptr;
	timeline_messages.clear();
	timeline_start = 0;
	timeline_rows_sorted = false;
	timeline_anchor_event_id = -1;
	if (plan_capsule) {
		plan_capsule->clear_plan();
	}
	if (message_list) {
		while (message_list->get_child_count() > 0) {
			Node *child = message_list->get_child(0);
			message_list->remove_child(child);
			child->queue_free();
		}
	}
	if (p_show_empty) {
		_show_empty_state();
	} else {
		_clear_empty_state();
	}
}

void SolersDock::_on_send_chat_pressed() {
	if (!chat_input) {
		return;
	}
	_hide_mention_popup();
	const String prompt = chat_input->get_text().strip_edges();
	if (agent_session && agent_session->is_running()) {
		// Typing while the agent works steers the running turn; an empty
		// composer keeps the button as the stop control.
		if (prompt.is_empty() && pending_attachments.is_empty()) {
			_on_stop_chat_pressed();
			return;
		}
		const Array attachments = pending_attachments.duplicate(true);
		chat_input->set_text("");
		_clear_attachments();
		_update_chat_input_height();
		_update_send_enabled();
		_submit_steering(prompt, attachments);
		return;
	}
	if (send_chat_button && !send_chat_button->is_enabled()) {
		return;
	}

	if (prompt.is_empty() && pending_attachments.is_empty()) {
		return;
	}
	const Array attachments = pending_attachments.duplicate(true);
	chat_input->set_text("");
	_clear_attachments();
	_update_chat_input_height();
	_update_send_enabled();
	_refresh_model_chip();
	_submit_chat_prompt(prompt, attachments);
}

void SolersDock::_on_stop_chat_pressed() {
	if (!agent_session || !agent_session->is_running()) {
		_update_send_enabled();
		return;
	}
	agent_session->abort();
	if (permission_manager) {
		const Array pending = permission_manager->list_pending_requests();
		for (int i = 0; i < pending.size(); i++) {
			const Dictionary request = pending[i];
			permission_manager->reject_request(request.get("id", 0));
		}
	}
	// Abort emits turn_completed(outcome=aborted); that handler shows the visible status.
	_refresh_status();
	_update_send_enabled();
}

void SolersDock::_toggle_session_sidebar() {
	if (!session_sidebar) {
		return;
	}
	const bool show = !session_sidebar->is_visible();
	session_sidebar->set_visible(show);
	_sync_session_button();
	if (EditorSettings::get_singleton()) {
		EditorSettings::get_singleton()->set_project_metadata("solers", "session_sidebar_visible", show);
	}
	// Show the shell this frame; fill the list next idle so a cold index rebuild
	// cannot stall the toggle click. solers_list_sessions rebuilds if needed.
	if (show && session_list && session_list->get_child_count() == 0) {
		_request_session_list_refresh();
	}
	_sync_layout_widths();
}

void SolersDock::_sync_session_button() {
	if (!session_button) {
		return;
	}
	const bool active = session_sidebar && session_sidebar->is_visible();
	session_button->set_accent(active ? session_button->get_theme_color(SNAME("accent_color"), EditorStringName(Editor)) : Color());
}

void SolersDock::_request_session_list_refresh() {
	callable_mp(this, &SolersDock::_refresh_session_list).call_deferred();
}

void SolersDock::_on_new_chat_pressed() {
	if (new_session_callback.is_valid()) {
		new_session_callback.call();
	} else {
		start_new_chat();
	}
	_refresh_session_list();
}

void SolersDock::_sync_session_selection() {
	if (session_button_group.is_null()) {
		return;
	}
	List<BaseButton *> rows;
	session_button_group->get_buttons(&rows);
	for (BaseButton *row : rows) {
		row->set_pressed_no_signal(String(row->get_meta("session_id", String())) == session_current_id);
	}
}

void SolersDock::_on_session_row_pressed(const String &p_session_id) {
	if (p_session_id.is_empty()) {
		return;
	}
	session_current_id = p_session_id;
	if (session_select_callback.is_valid()) {
		session_select_callback.call(p_session_id);
	}
}

static int64_t _solers_day_start(int64_t p_unix, int64_t p_time_zone_offset) {
	const Dictionary dt = Time::get_singleton()->get_datetime_dict_from_unix_time(p_unix + p_time_zone_offset);
	Dictionary day;
	day["year"] = dt.get("year", 1970);
	day["month"] = dt.get("month", 1);
	day["day"] = dt.get("day", 1);
	day["hour"] = 0;
	day["minute"] = 0;
	day["second"] = 0;
	return (int64_t)Time::get_singleton()->get_unix_time_from_datetime_dict(day) - p_time_zone_offset;
}

static String _solers_session_group_label(int64_t p_wall, int64_t p_now, int64_t p_time_zone_offset) {
	if (p_wall <= 0) {
		return TTR("Older");
	}
	const int64_t today = _solers_day_start(p_now, p_time_zone_offset);
	const int64_t yesterday = today - 86400;
	if (p_wall >= today) {
		return TTR("Today");
	}
	if (p_wall >= yesterday) {
		return TTR("Yesterday");
	}
	if (p_wall >= today - 7 * 86400) {
		return TTR("Last 7 Days");
	}
	if (p_wall >= today - 30 * 86400) {
		return TTR("Last 30 Days");
	}
	return TTR("Older");
}

static String _solers_session_time_label(int64_t p_wall, int64_t p_time_zone_offset) {
	if (p_wall <= 0) {
		return TTR("No completed turns");
	}
	const Dictionary dt = Time::get_singleton()->get_datetime_dict_from_unix_time(p_wall + p_time_zone_offset);
	return vformat("%d.%d.%02d %02d:%02d", dt.get("year", 0), dt.get("month", 0), dt.get("day", 0), dt.get("hour", 0), dt.get("minute", 0));
}

void SolersDock::_refresh_session_list() {
	if (!session_list) {
		return;
	}
	while (session_list->get_child_count() > 0) {
		Node *child = session_list->get_child(0);
		session_list->remove_child(child);
		child->queue_free();
	}
	session_button_group.instantiate();

	Vector<SolersSessionInfo> sessions = solers_list_sessions(session_project_path);
	struct WallSort {
		bool operator()(const SolersSessionInfo &a, const SolersSessionInfo &b) const {
			return a.wall > b.wall;
		}
	};
	sessions.sort_custom<WallSort>();

	const int64_t now = (int64_t)Time::get_singleton()->get_unix_time_from_system();
	const int64_t time_zone_offset = (int64_t)Time::get_singleton()->get_time_zone_from_system().get("bias", 0) * 60;
	String last_group;
	for (const SolersSessionInfo &session : sessions) {
		const String group = _solers_session_group_label(session.wall, now, time_zone_offset);
		if (group != last_group) {
			last_group = group;
			Label *header = solers_make_session_group(group);
			session_list->add_child(header);
		}
		const String title = solers_session_display_title(session.title);
		const String time = _solers_session_time_label(session.wall, time_zone_offset);
		Button *row = memnew(Button);
		solers_style_session_button(row, true);
		row->set_button_group(session_button_group);
		row->set_pressed_no_signal(session.session_id == session_current_id);
		row->set_tooltip_text(session.title);
		row->set_accessibility_name(title + ", " + time);
		row->set_meta("session_id", session.session_id);
		row->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_session_row_pressed).bind(session.session_id));
		session_list->add_child(row);

		MarginContainer *inset = memnew(MarginContainer);
		inset->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		inset->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		inset->add_theme_constant_override("margin_left", 10 * EDSCALE);
		inset->add_theme_constant_override("margin_right", 10 * EDSCALE);
		inset->add_theme_constant_override("margin_top", 7 * EDSCALE);
		inset->add_theme_constant_override("margin_bottom", 7 * EDSCALE);
		row->add_child(inset);
		VBoxContainer *content = memnew(VBoxContainer);
		content->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		content->add_theme_constant_override("separation", 3 * EDSCALE);
		inset->add_child(content);
		Label *title_label = memnew(Label(title));
		title_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		title_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		title_label->set_theme_type_variation("SolersSessionTitle");
		content->add_child(title_label);
		HBoxContainer *time_row = memnew(HBoxContainer);
		time_row->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		time_row->add_theme_constant_override("separation", 5 * EDSCALE);
		content->add_child(time_row);
		TextureRect *calendar = memnew(TextureRect);
		calendar->set_name("SessionCalendar");
		calendar->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		calendar->set_texture(SolersIcons::get(SNAME("calendar"), int(Math::round(12.0f * EDSCALE))));
		calendar->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
		calendar->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
		calendar->set_self_modulate(SOLERS_TEXT_META);
		time_row->add_child(calendar);
		Label *time_label = memnew(Label(time));
		time_label->set_name("SessionTime");
		time_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		time_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		time_label->set_theme_type_variation("SolersSessionMeta");
		time_row->add_child(time_label);
	}
	if (sessions.is_empty()) {
		Label *empty = memnew(Label);
		empty->set_text(TTR("No sessions yet."));
		empty->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		empty->add_theme_color_override(SceneStringName(font_color), SOLERS_TEXT_DIM);
		session_list->add_child(empty);
	}
}

void SolersDock::set_session_select_callback(const Callable &p_callback) {
	session_select_callback = p_callback;
}

void SolersDock::set_new_session_callback(const Callable &p_callback) {
	new_session_callback = p_callback;
}

void SolersDock::set_session_context(const String &p_project_path, const String &p_session_id) {
	const bool project_changed = session_project_path != p_project_path;
	session_project_path = p_project_path;
	session_current_id = p_session_id;
	if (!session_sidebar || !session_sidebar->is_visible()) {
		return;
	}
	// Only a project switch changes which sessions belong in the list.
	// Same-project session switches only need the selected-row accent.
	if (project_changed || (session_list && session_list->get_child_count() == 0)) {
		_request_session_list_refresh();
	} else {
		_sync_session_selection();
	}
}

void SolersDock::notify_sessions_changed() {
	if (session_sidebar && session_sidebar->is_visible()) {
		_request_session_list_refresh();
	}
}

// Submenu kinds for the cascading model menu.
enum {
	SOLERS_SUBMENU_NONE = 0,
	SOLERS_SUBMENU_MODEL = 1,
	SOLERS_SUBMENU_EFFORT = 2,
};

static void solers_clear_children(Node *p_node) {
	while (p_node->get_child_count() > 0) {
		Node *child = p_node->get_child(0);
		p_node->remove_child(child);
		child->queue_free();
	}
}

void SolersDock::_on_model_chip_pressed() {
	if (!model_popup_overlay || !model_menu || !model_menu_box || !model_chip) {
		return;
	}

	if (model_popup_overlay->is_visible()) {
		_hide_model_popup();
		return;
	}

	_close_model_submenu();
	solers_clear_children(model_menu_box);
	model_menu_model_row = nullptr;
	model_menu_effort_row = nullptr;

	const Dictionary provider_data = settings_service ? Dictionary(settings_service->get_provider_config().get("data", Dictionary())) : Dictionary();
	const String active_provider = String(provider_data.get("provider", String())).strip_edges();
	const String active_model = String(provider_data.get("model", String())).strip_edges();
	const String active_effort = String(provider_data.get("reasoning_effort", String())).strip_edges();
	const Dictionary active_profile = provider_data.get("profile", Dictionary());
	const String active_catalog_id = active_profile.get("catalog_provider", active_provider);

	model_menu_model_row = solers_make_model_menu_parent_row(
			active_provider.is_empty() ? Ref<Texture2D>() : SolersIcons::provider_logo(active_catalog_id, int(Math::round(14.0f * EDSCALE))),
			TTR("Model"),
			active_model.is_empty() ? TTR("None") : solers_compact_label(solers_resolve_model_display(settings_service, provider_data, active_model)));
	model_menu_model_row->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_open_model_submenu).bind(SOLERS_SUBMENU_MODEL));
	model_menu_box->add_child(model_menu_model_row);

	SolersModelsDev *models_dev = solers_dock_models_dev(settings_service);
	const Dictionary active_model_info = models_dev ? models_dev->get_model(StringName(active_catalog_id), active_model) : Dictionary();
	const Array efforts = SolersModelsDev::reasoning_efforts(active_model_info);
	if (provider_data.get("available", false) && !active_model.is_empty() && !efforts.is_empty()) {
		model_menu_effort_row = solers_make_model_menu_parent_row(Ref<Texture2D>(), TTR("Effort"), solers_reasoning_effort_label(active_effort));
		model_menu_effort_row->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_open_model_submenu).bind(SOLERS_SUBMENU_EFFORT));
		model_menu_effort_row->connect(SceneStringName(mouse_entered), callable_mp(this, &SolersDock::_open_model_submenu).bind(SOLERS_SUBMENU_EFFORT));
		model_menu_box->add_child(model_menu_effort_row);
	}

	model_menu_box->add_child(memnew(HSeparator));

	Button *reset_row = solers_make_model_popup_row(TTR("Reset to default"), String(), false);
	reset_row->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_reset_model_defaults_from_popup));
	reset_row->connect(SceneStringName(mouse_entered), callable_mp(this, &SolersDock::_close_model_submenu));
	model_menu_box->add_child(reset_row);

	Button *settings_row = solers_make_model_popup_row(TTR("Manage providers..."), String(), false);
	settings_row->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_open_model_settings_from_popup));
	settings_row->connect(SceneStringName(mouse_entered), callable_mp(this, &SolersDock::_close_model_submenu));
	model_menu_box->add_child(settings_row);

	model_popup_overlay->show();
	model_popup_overlay->move_to_front();
	model_menu->show();
	_position_model_menu();
}

void SolersDock::_position_model_menu() {
	if (!model_popup_overlay || !model_popup_overlay->is_visible() || !model_menu || !model_chip) {
		return;
	}

	const Rect2 anchor = model_chip->get_screen_rect();
	const Point2 base_screen_pos = model_popup_overlay->get_screen_position();
	const Size2 overlay_size = model_popup_overlay->get_size();
	const int margin = int(8 * EDSCALE);
	const int gap = int(6 * EDSCALE);

	const Size2 natural = model_menu->get_combined_minimum_size();
	const int max_w = MAX(1, int(overlay_size.x) - margin * 2);
	const int menu_w = CLAMP(MAX(int(natural.x), int(210 * EDSCALE)), 1, max_w);
	const int max_h = MAX(1, int(overlay_size.y) - margin * 2);
	const int menu_h = CLAMP(int(natural.y), 1, max_h);

	const int below_h = MAX(0, int(base_screen_pos.y + overlay_size.y - (anchor.position.y + anchor.size.y) - gap - margin));
	const bool open_above = below_h < menu_h;

	Point2 menu_pos(anchor.position.x + anchor.size.x - menu_w, open_above ? anchor.position.y - menu_h - gap : anchor.position.y + anchor.size.y + gap);
	menu_pos.x = CLAMP(menu_pos.x, base_screen_pos.x + margin, base_screen_pos.x + overlay_size.x - menu_w - margin);
	menu_pos.y = CLAMP(menu_pos.y, base_screen_pos.y + margin, base_screen_pos.y + overlay_size.y - menu_h - margin);

	model_menu->set_size(Size2(menu_w, menu_h));
	model_menu->set_position(menu_pos - base_screen_pos);
}

void SolersDock::_open_model_submenu(int p_kind) {
	if (!model_submenu || !model_submenu_scroll || !model_submenu_list) {
		return;
	}
	if (model_submenu_kind == p_kind && model_submenu->is_visible()) {
		return;
	}
	model_submenu_kind = p_kind;
	model_submenu_entries.clear();
	if (model_submenu_search) {
		model_submenu_search->set_visible(p_kind == SOLERS_SUBMENU_MODEL);
		model_submenu_search->set_text(String());
	}

	const Dictionary provider_data = settings_service ? Dictionary(settings_service->get_provider_config().get("data", Dictionary())) : Dictionary();
	const String active_provider = String(provider_data.get("provider", String())).strip_edges();
	const String active_model = String(provider_data.get("model", String())).strip_edges();
	const String active_effort = String(provider_data.get("reasoning_effort", String())).strip_edges();

	if (p_kind == SOLERS_SUBMENU_MODEL) {
		const Dictionary connected_data = settings_service ? Dictionary(settings_service->list_connected_provider_configs().get("data", Dictionary())) : Dictionary();
		const Array connected = connected_data.get("providers", Array());
		SolersModelsDev *models_dev = solers_dock_models_dev(settings_service);

		for (const Variant &config_value : connected) {
			const Dictionary config = config_value;
			const bool available = config.get("available", false);
			const Dictionary profile = config.get("profile", Dictionary());
			const String provider = config.get("provider", String());
			const bool selected = active_provider == provider;
			const String catalog_id = profile.get("catalog_provider", provider);
			const StringName catalog_name = StringName(catalog_id);
			const Array allowed_models = profile.get("allowed_models", Array());
			const bool has_allowlist = !allowed_models.is_empty();

			Array models;
			HashSet<String> seen_models;
			solers_add_unique_model(models, seen_models, config.get("model", String()));
			for (const Variant &profile_model : allowed_models) {
				solers_add_unique_model(models, seen_models, profile_model);
			}
			if (models_dev) {
				for (const Variant &model_id_value : models_dev->list_model_ids(catalog_name)) {
					const String model_id = model_id_value;
					if (has_allowlist && !allowed_models.has(model_id)) {
						continue;
					}
					const Dictionary model_info = models_dev->get_model(catalog_name, model_id);
					if (model_info.has("tool_call") && !(bool)model_info["tool_call"]) {
						continue;
					}
					solers_add_unique_model(models, seen_models, model_id);
				}
			}
			solers_add_unique_model(models, seen_models, profile.get("default_model", String()));
			if (models.is_empty()) {
				continue;
			}

			const String provider_label = profile.get("label", provider);
			Dictionary group;
			group["kind"] = "group";
			group["provider"] = provider;
			group["label"] = available ? provider_label : vformat(TTR("%s — blocked by Local Models Only"), provider_label);
			group["catalog_id"] = catalog_id;
			model_submenu_entries.push_back(group);

			const Dictionary model_labels = profile.get("model_labels", Dictionary());
			for (const Variant &model_value : models) {
				const String model_id = model_value;
				if (has_allowlist && !allowed_models.has(model_id)) {
					continue;
				}
				const Dictionary model_info = models_dev ? models_dev->get_model(catalog_name, model_id) : Dictionary();
				Dictionary entry;
				entry["kind"] = "model";
				entry["provider"] = provider;
				entry["model_id"] = model_id;
				entry["label"] = model_info.get("name", model_labels.get(model_id, model_id));
				entry["available"] = available;
				entry["selected"] = selected && model_id == active_model;
				model_submenu_entries.push_back(entry);
			}
		}
		_rebuild_model_submenu_list();
		_position_model_submenu(model_menu_model_row);
	} else if (p_kind == SOLERS_SUBMENU_EFFORT) {
		solers_clear_children(model_submenu_list);
		const Dictionary active_profile = provider_data.get("profile", Dictionary());
		const String active_catalog_id = active_profile.get("catalog_provider", active_provider);
		SolersModelsDev *models_dev = solers_dock_models_dev(settings_service);
		const Dictionary active_model_info = models_dev ? models_dev->get_model(StringName(active_catalog_id), active_model) : Dictionary();
		const Array efforts = SolersModelsDev::reasoning_efforts(active_model_info);

		Button *default_effort = solers_make_model_popup_row("Default", String(), active_effort.is_empty());
		default_effort->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_set_reasoning_effort_from_popup).bind(String()));
		model_submenu_list->add_child(default_effort);
		for (const Variant &effort_value : efforts) {
			const String effort = effort_value;
			Button *row = solers_make_model_popup_row(solers_reasoning_effort_label(effort), effort, effort == active_effort);
			row->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_set_reasoning_effort_from_popup).bind(effort));
			model_submenu_list->add_child(row);
		}
		_position_model_submenu(model_menu_effort_row);
	}
}

void SolersDock::_on_model_submenu_search(const String &p_text) {
	if (model_submenu_kind == SOLERS_SUBMENU_MODEL) {
		_rebuild_model_submenu_list(p_text);
	}
}

void SolersDock::_rebuild_model_submenu_list(const String &p_filter) {
	if (!model_submenu_list) {
		return;
	}
	solers_clear_children(model_submenu_list);
	const String filter = p_filter.strip_edges().to_lower();
	int visible_models = 0;
	HashSet<String> emitted_groups;

	for (int i = 0; i < model_submenu_entries.size(); i++) {
		const Dictionary entry = model_submenu_entries[i];
		if (String(entry.get("kind", String())) != "model") {
			continue;
		}
		const String model_id = entry.get("model_id", String());
		const String label = entry.get("label", model_id);
		if (!filter.is_empty() && !model_id.to_lower().contains(filter) && !label.to_lower().contains(filter)) {
			continue;
		}
		const String provider = entry.get("provider", String());
		if (!emitted_groups.has(provider)) {
			for (const Variant &g_v : model_submenu_entries) {
				const Dictionary g = g_v;
				if (g.get("kind", String()) == "group" && String(g.get("provider", String())) == provider) {
					model_submenu_list->add_child(solers_make_model_popup_group(
							g.get("label", provider),
							SolersIcons::provider_logo(g.get("catalog_id", provider), int(Math::round(13.0f * EDSCALE)))));
					emitted_groups.insert(provider);
					break;
				}
			}
		}
		const bool available = entry.get("available", false);
		Button *row = solers_make_model_popup_row(label, model_id, entry.get("selected", false));
		if (available) {
			row->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_set_model_provider_from_popup).bind(provider, model_id));
		} else {
			row->set_disabled(true);
			row->add_theme_color_override("font_disabled_color", SOLERS_TEXT_DIM);
			row->set_tooltip_text(TTR("Disable Local Models Only to use this remote model."));
		}
		model_submenu_list->add_child(row);
		visible_models++;
	}

	if (visible_models == 0) {
		Label *empty = memnew(Label(model_submenu_entries.is_empty()
						? TTR("No connected model provider. Connect one in Provider Settings.")
						: (filter.is_empty() ? TTR("Connected providers do not expose any selectable models.") : TTR("No models match this search."))));
		empty->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		empty->add_theme_color_override("font_color", SOLERS_TEXT_DIM);
		empty->set_custom_minimum_size(Size2(0, 52 * EDSCALE));
		model_submenu_list->add_child(empty);
	}
}

void SolersDock::_position_model_submenu(Button *p_anchor_row) {
	if (!model_popup_overlay || !model_menu || !model_submenu || !model_submenu_scroll || !model_submenu_list || !p_anchor_row) {
		return;
	}

	const Point2 base_screen_pos = model_popup_overlay->get_screen_position();
	const Size2 overlay_size = model_popup_overlay->get_size();
	const Rect2 menu_rect(model_menu->get_position(), model_menu->get_size());
	const Point2 row_pos = p_anchor_row->get_screen_position() - base_screen_pos;
	const int margin = int(8 * EDSCALE);
	const int gap = int(4 * EDSCALE);
	const int popup_pad = int(8 * EDSCALE);

	const int max_w = MAX(1, int(overlay_size.x) - margin * 2);
	const int submenu_w = CLAMP(int(260 * EDSCALE), 1, max_w);
	const float search_h = (model_submenu_search && model_submenu_search->is_visible()) ? model_submenu_search->get_combined_minimum_size().y + 4 * EDSCALE : 0;
	const float natural_h = model_submenu_list->get_combined_minimum_size().y + search_h + popup_pad * 2;
	const int max_h = MIN(int(360 * EDSCALE), MAX(1, int(overlay_size.y) - margin * 2));
	const int submenu_h = int(CLAMP(natural_h, float(MIN(int(60 * EDSCALE), max_h)), float(max_h)));

	const int content_w = MAX(1, submenu_w - popup_pad * 2);
	const int content_h = MAX(1, submenu_h - popup_pad * 2 - int(search_h));
	model_submenu_scroll->set_custom_minimum_size(Size2(content_w, content_h));
	model_submenu_list->set_custom_minimum_size(Size2(content_w, 0));
	model_submenu_scroll->get_v_scroll_bar()->set_value(0);

	// Beside the root menu: prefer the right edge, fall back to the left.
	Point2 submenu_pos(menu_rect.position.x + menu_rect.size.x + gap, row_pos.y - popup_pad);
	if (submenu_pos.x + submenu_w > overlay_size.x - margin) {
		submenu_pos.x = menu_rect.position.x - submenu_w - gap;
	}
	submenu_pos.x = CLAMP(submenu_pos.x, float(margin), MAX(float(margin), overlay_size.x - submenu_w - margin));
	submenu_pos.y = CLAMP(submenu_pos.y, float(margin), MAX(float(margin), overlay_size.y - submenu_h - margin));

	model_submenu->set_size(Size2(submenu_w, submenu_h));
	model_submenu->set_position(submenu_pos);
	model_submenu->show();
}

void SolersDock::_close_model_submenu() {
	model_submenu_kind = SOLERS_SUBMENU_NONE;
	if (model_submenu) {
		model_submenu->hide();
	}
}

void SolersDock::_hide_model_popup() {
	_close_model_submenu();
	if (model_menu) {
		model_menu->hide();
	}
	if (model_popup_overlay) {
		model_popup_overlay->hide();
	}
}

void SolersDock::_on_model_popup_overlay_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mouse_button = p_event;
	if (mouse_button.is_valid() && mouse_button->is_pressed() && mouse_button->get_button_index() == MouseButton::LEFT) {
		_hide_model_popup();
		model_popup_overlay->accept_event();
	}
}

void SolersDock::_set_model_provider_from_popup(const String &p_provider, const String &p_model) {
	_hide_model_popup();
	if (!settings_service) {
		return;
	}
	Dictionary args;
	args["provider"] = p_provider;
	if (!p_model.is_empty()) {
		args["model"] = p_model;
	}
	settings_service->set_provider_config(args);
	_refresh_model_chip();
}

void SolersDock::_set_reasoning_effort_from_popup(const String &p_effort) {
	_hide_model_popup();
	if (!settings_service) {
		return;
	}
	Dictionary args;
	args["reasoning_effort"] = p_effort;
	settings_service->set_provider_config(args);
	_refresh_model_chip();
}

void SolersDock::_reset_model_defaults_from_popup() {
	_hide_model_popup();
	if (!settings_service) {
		return;
	}
	const Dictionary provider_data = settings_service->get_provider_config().get("data", Dictionary());
	const Dictionary profile = provider_data.get("profile", Dictionary());
	Dictionary args;
	args["reasoning_effort"] = String();
	const String default_model = String(profile.get("default_model", String())).strip_edges();
	if (!default_model.is_empty()) {
		args["model"] = default_model;
	}
	settings_service->set_provider_config(args);
	_refresh_model_chip();
}

void SolersDock::_open_model_settings_from_popup() {
	_hide_model_popup();
	open_provider_settings("llm");
}

void SolersDock::open_provider_settings(const String &p_category) {
	if (!provider_settings_dialog || !provider_settings_view) {
		return;
	}
	provider_settings_view->refresh();
	if (!p_category.is_empty()) {
		provider_settings_view->select_category(p_category);
	}
	provider_settings_dialog->popup_centered(Size2(980, 640) * EDSCALE);
}

SolersPMAIView *SolersDock::get_provider_settings_view() const {
	return provider_settings_view;
}

void SolersDock::start_new_chat() {
	if (agent_session) {
		agent_session->reset_conversation();
	}
	_clear_chat_view(true);
	if (chat_input) {
		_hide_mention_popup();
		chat_input->set_text("");
		_update_chat_input_height();
		_update_send_enabled();
		chat_input->grab_focus();
	}
	_refresh_status();
	notify_sessions_changed();
}

static constexpr int SOLERS_TIMELINE_WINDOW = 24;

void SolersDock::load_chat_history(const Array &p_messages) {
	_clear_chat_view(false);
	timeline_messages = p_messages.duplicate(true);
	_render_timeline(MAX(0, timeline_messages.size() - SOLERS_TIMELINE_WINDOW));
	callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
}

VBoxContainer *SolersDock::_chat_mount() const {
	return history_mount ? history_mount : message_list;
}

static Control *_solers_timeline_row(VBoxContainer *p_list, int64_t p_id) {
	for (int i = 0; p_list && i < p_list->get_child_count(); i++) {
		Control *row = Object::cast_to<Control>(p_list->get_child(i));
		if (row && (int64_t)row->get_meta("timeline_event_id", -1) == p_id) {
			return row;
		}
	}
	return nullptr;
}

void SolersDock::_bind_layout_splits() {
	const Callable started = callable_mp(this, &SolersDock::_set_layout_dragging).bind(true);
	const Callable ended = callable_mp(this, &SolersDock::_set_layout_dragging).bind(false);
	for (Node *ancestor = get_parent(); ancestor; ancestor = ancestor->get_parent()) {
		SplitContainer *split = Object::cast_to<SplitContainer>(ancestor);
		if (split && !split->is_connected(SNAME("drag_started"), started)) {
			split->connect(SNAME("drag_started"), started);
			split->connect(SNAME("drag_ended"), ended);
		}
	}
}

void SolersDock::_set_layout_dragging(bool p_active) {
	if (p_active) {
		if (layout_drag_depth++ > 0 || !timeline_inset || !chat_scroll || !chat_scroll->is_visible()) {
			return;
		}
		const float width = timeline_inset->get_size().x;
		if (width <= 0.0f) {
			return;
		}
		const Rect2 viewport = chat_scroll->get_global_rect();
		for (int i = 0; timeline_anchor_event_id < 0 && i < message_list->get_child_count(); i++) {
			Control *anchor = Object::cast_to<Control>(message_list->get_child(i));
			if (anchor && (int64_t)anchor->get_meta("timeline_event_id", -1) >= 0 && anchor->get_global_rect().intersects(viewport)) {
				timeline_rows_sorted = false;
				timeline_anchor_event_id = anchor->get_meta("timeline_event_id", -1);
				timeline_anchor_screen_y = anchor->get_global_position().y;
			}
		}
		Size2 minimum = timeline_inset->get_custom_minimum_size();
		Size2 maximum = timeline_inset->get_custom_maximum_size();
		minimum.x = maximum.x = width;
		timeline_inset->set_custom_minimum_size(minimum);
		timeline_inset->set_custom_maximum_size(maximum);
	} else {
		layout_drag_depth = MAX(0, layout_drag_depth - 1);
		if (layout_drag_depth != 0 || !timeline_inset || timeline_inset->get_custom_maximum_size().x < 0) {
			return;
		}
		Size2 minimum = timeline_inset->get_custom_minimum_size();
		Size2 maximum = timeline_inset->get_custom_maximum_size();
		minimum.x = 0;
		maximum.x = -1;
		timeline_inset->set_custom_maximum_size(maximum);
		timeline_inset->set_custom_minimum_size(minimum);
		if (timeline_anchor_event_id >= 0) {
			message_list->call(SNAME("queue_sort"));
		}
	}
}

void SolersDock::_render_timeline(int p_start) {
	if (!message_list || timeline_anchor_event_id >= 0) {
		return;
	}
	timeline_rows_sorted = false;
	_clear_empty_state();
	const int next_start = CLAMP(p_start, 0, MAX(0, timeline_messages.size() - SOLERS_TIMELINE_WINDOW));
	const int next_end = MIN(timeline_messages.size(), next_start + SOLERS_TIMELINE_WINDOW);
	if (timeline_messages.is_empty()) {
		_show_empty_state();
		return;
	}
	HashSet<int64_t> desired;
	HashMap<int64_t, Control *> mounted;
	for (int index = next_start; index < next_end; index++) {
		desired.insert(Dictionary(timeline_messages[index]).get("event_id", -1));
	}
	for (int i = 0; i < message_list->get_child_count(); i++) {
		Control *row = Object::cast_to<Control>(message_list->get_child(i));
		const int64_t id = row ? (int64_t)row->get_meta("timeline_event_id", -1) : -1;
		if (id >= 0) {
			mounted[id] = row;
		}
	}
	Control *anchor = nullptr;
	for (int index = next_start; index < next_end && !anchor; index++) {
		Control **row = mounted.getptr(Dictionary(timeline_messages[index]).get("event_id", -1));
		anchor = row ? *row : nullptr;
	}
	if (anchor) {
		timeline_anchor_event_id = anchor->get_meta("timeline_event_id", -1);
		timeline_anchor_screen_y = anchor->get_global_position().y;
	}
	bool layout_pending = false;
	for (int i = message_list->get_child_count() - 1; i >= 0; i--) {
		Control *row = Object::cast_to<Control>(message_list->get_child(i));
		const int64_t id = row ? (int64_t)row->get_meta("timeline_event_id", -1) : -1;
		const bool live = row && agent_session && agent_session->is_running() && (id < 0 || (bool)row->get_meta("timeline_pending", false));
		const bool keep = desired.has(id) || live;
		if (!keep && row && (bool)row->get_meta("timeline_row", false)) {
			mounted.erase(id);
			message_list->remove_child(row);
			row->queue_free();
			layout_pending = true;
		}
	}
	for (int index = next_start; index < next_end; index++) {
		const int64_t id = Dictionary(timeline_messages[index]).get("event_id", -1);
		Control **found = mounted.getptr(id);
		Control *row = found ? *found : nullptr;
		if (!row && timeline_messages[index].get_type() == Variant::DICTIONARY) {
			row = _create_history_entry(timeline_messages[index]);
			if (row) {
				row->set_meta("timeline_row", true);
				row->set_meta("timeline_event_id", id);
				layout_pending = true;
			}
		}
		if (row && row->get_index() != index - next_start) {
			layout_pending = true;
			message_list->move_child(row, index - next_start);
		}
	}
	timeline_start = next_start;
	if (!anchor || !layout_pending) {
		timeline_anchor_event_id = -1;
	}
}

void SolersDock::_queue_timeline_layout_commit() {
	if (layout_drag_depth > 0 || timeline_anchor_event_id < 0 || !chat_scroll) {
		return;
	}
	timeline_rows_sorted = true;
	chat_scroll->call(SNAME("queue_sort"));
}

void SolersDock::_commit_timeline_layout() {
	if (!timeline_rows_sorted || timeline_anchor_event_id < 0) {
		return;
	}
	Control *anchor = _solers_timeline_row(message_list, timeline_anchor_event_id);
	if (anchor) {
		chat_scroll->set_v_scroll(chat_scroll->get_v_scroll() + Math::round(anchor->get_global_position().y - timeline_anchor_screen_y));
	}
	timeline_anchor_event_id = -1;
	timeline_rows_sorted = false;
}

Control *SolersDock::_create_history_entry(const Dictionary &p_message) {
	VBoxContainer *entry = memnew(VBoxContainer);
	message_list->add_child(entry);
	history_mount = entry;
	_append_history_message(p_message);
	_finish_turn_cells();
	history_mount = nullptr;
	if (entry->get_child_count() == 0) {
		memdelete(entry);
		return nullptr;
	}
	entry->set_meta("timeline_event_id", p_message.get("event_id", -1));
	return entry;
}

void SolersDock::_append_history_message(const Dictionary &p_message) {
	const String role = p_message.get("role", String());
	const String content = p_message.get("content", String());
	if (role == SolersLLMRole::USER) {
		if (content.is_empty() && Array(p_message.get("attachments", Array())).is_empty()) {
			return;
		}
		_settle_tool_group();
		_append_user_message(SolersMention::strip_prompt_block(content), p_message.get("attachments", Array()));
	} else if (role == SolersLLMRole::ASSISTANT) {
		_settle_tool_group();
		const String reasoning = String(p_message.get("reasoning", String())).strip_edges();
		if (!reasoning.is_empty()) {
			SolersThinkingCell *thinking = memnew(SolersThinkingCell);
			thinking->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
			_ensure_assistant_row()->add_child(thinking);
			thinking->set_settled_reasoning(reasoning);
		}
		if (!content.is_empty()) {
			_on_agent_assistant_message(content);
		}
		const Array calls = p_message.get("tool_calls", Array());
		for (const Variant &item : calls) {
			const Dictionary call = item;
			const String id = call.get("id", String());
			const String name = call.get("canonical_name", call.get("name", String()));
			_on_agent_tool_started(id, name, call.get("arguments", String("{}")));
			if ((bool)call.get("finished", false)) {
				Dictionary result;
				result["ok"] = call.get("ok", false);
				Dictionary error;
				error["message"] = call.get("error_message", String());
				result["error"] = error;
				_on_agent_tool_finished(id, name, result, call.get("duration_msec", 0));
			}
		}
	} else if (role == "context_compaction") {
		SolersStatusCell *cell = memnew(SolersStatusCell);
		const String phase = p_message.get("phase", "completed");
		cell->set_status(phase == "started" ? TTR("Context automatically compacting") : phase == "failed" ? TTR("Context compaction failed")
						: phase == "cancelled"															  ? TTR("Context compaction cancelled")
																										  : TTR("Context automatically compacted"));
		cell->set_active(phase == "started");
		_chat_mount()->add_child(cell);
	} else if (role == "turn_outcome") {
		_append_error_row(content);
	}
}

void SolersDock::_on_timeline_scrolled() {
	if (timeline_anchor_event_id >= 0 || !chat_scroll) {
		return;
	}
	VScrollBar *bar = chat_scroll->get_v_scroll_bar();
	const double p_value = bar->get_value();
	if (timeline_start > 0 && Math::is_equal_approx(p_value, bar->get_min())) {
		_render_timeline(MAX(0, timeline_start - SOLERS_TIMELINE_WINDOW / 2));
	} else if (timeline_start + SOLERS_TIMELINE_WINDOW < timeline_messages.size() && Math::is_equal_approx(p_value, bar->get_max() - bar->get_page())) {
		_render_timeline(MIN(timeline_messages.size() - SOLERS_TIMELINE_WINDOW, timeline_start + SOLERS_TIMELINE_WINDOW / 2));
	}
}

void SolersDock::_submit_chat_prompt(const String &p_prompt, const Array &p_attachments) {
	const String prompt = p_prompt.strip_edges();
	if (prompt.is_empty() && p_attachments.is_empty()) {
		return;
	}

	Control *user_row = _append_user_message(prompt, p_attachments);

	if (!agent_session) {
		_append_error_row(TTR("Agent session is unavailable."));
		return;
	}

	// Real BYOK end-to-end: hand the prompt to the single agent loop. The
	// session streams assistant text, tool calls and results back through the
	// signals wired in set_agent_session(); no mock, no hardcoded provider.
	Dictionary args;
	args["prompt"] = prompt;
	const Array mentions = SolersMention::parse(prompt);
	if (!mentions.is_empty()) {
		args["mentions"] = mentions;
	}
	if (!p_attachments.is_empty()) {
		args["attachments"] = p_attachments.duplicate(true);
	}
	const Dictionary result = agent_session->start_turn(args);
	if (!(bool)result.get("ok", false)) {
		_remove_status_cell();
		const Dictionary error = result.get("error", Dictionary());
		_append_error_row(String::utf8("\u26a0 ") + String(error.get("message", "Could not start the agent turn.")));
	} else {
		if (user_row) {
			user_row->set_meta("timeline_event_id", Dictionary(result.get("data", Dictionary())).get("event_id", -1));
		}
		timeline_messages = agent_session->get_timeline_entries();
		session_current_id = agent_session->get_status().get("session_id", session_current_id);
		notify_sessions_changed();
	}
	_refresh_status();
}

void SolersDock::_submit_steering(const String &p_prompt, const Array &p_attachments) {
	Dictionary args;
	args["prompt"] = p_prompt;
	const Array mentions = SolersMention::parse(p_prompt);
	if (!mentions.is_empty()) {
		args["mentions"] = mentions;
	}
	if (!p_attachments.is_empty()) {
		args["attachments"] = p_attachments.duplicate(true);
	}
	const Dictionary result = agent_session ? agent_session->queue_user_message(args) : Dictionary();
	if ((bool)result.get("ok", false)) {
		Control *row = _append_user_message(p_prompt, p_attachments);
		if (row) {
			row->set_meta("timeline_event_id", Dictionary(result.get("data", Dictionary())).get("event_id", -1));
			row->set_meta("timeline_pending", true);
		}
		callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
		return;
	}
	// The turn ended between typing and sending: start an ordinary turn.
	_submit_chat_prompt(p_prompt, p_attachments);
}

void SolersDock::_on_chat_input_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || key->is_echo()) {
		return;
	}

	const Key keycode = key->get_keycode();
	const bool search_focused = plugin_mention_search && plugin_mention_search->has_focus();
	if (!search_focused && keycode == Key::BACKSPACE && _try_delete_mention_span(-1)) {
		chat_input->accept_event();
		return;
	}
	if (!search_focused && keycode == Key::KEY_DELETE && _try_delete_mention_span(1)) {
		chat_input->accept_event();
		return;
	}

	if (plugin_mention_popup && plugin_mention_popup->is_visible()) {
		if (keycode == Key::ESCAPE) {
			if (!mention_section.is_empty()) {
				mention_section = String();
				_refresh_mention_popup();
			} else {
				_hide_mention_popup();
				if (search_focused) {
					chat_input->grab_focus();
				}
			}
			chat_input->accept_event();
			return;
		}
		if (keycode == Key::UP || keycode == Key::DOWN) {
			_move_mention_selection(keycode == Key::UP ? -1 : 1);
			chat_input->accept_event();
			return;
		}
		if (keycode == Key::TAB || keycode == Key::ENTER || keycode == Key::KP_ENTER) {
			_activate_mention_selection();
			chat_input->accept_event();
			return;
		}
	}
	if (search_focused) {
		return;
	}
	if ((key->is_command_or_control_pressed() || key->is_meta_pressed()) && keycode == Key::V && _add_image_attachment_from_clipboard()) {
		chat_input->accept_event();
		return;
	}

	if (keycode != Key::ENTER && keycode != Key::KP_ENTER) {
		return;
	}

	if (key->is_shift_pressed()) {
		chat_input->insert_text_at_caret("\n");
		_update_chat_input_height();
		chat_input->accept_event();
		return;
	}

	if (permission_manager && permission_manager->get_pending_request_count() > 0) {
		_submit_current_approval();
		chat_input->accept_event();
		return;
	}
	_on_send_chat_pressed();
	chat_input->accept_event();
}

void SolersDock::_on_chat_input_text_changed() {
	_update_chat_input_height();
	_update_send_enabled();
	_refresh_mention_popup();
}

static Ref<Texture2D> solers_mention_section_icon(Control *p_owner, const String &p_section_id) {
	const int size = int(Math::round(13.0f * EDSCALE));
	if (p_section_id == "solers") {
		return SolersIcons::get(SNAME("tool_export"), size);
	}
	if (p_section_id == "addons") {
		return SolersIcons::get(SNAME("plugin"), size);
	}
	if (p_section_id == "files") {
		return p_owner->get_theme_icon(SNAME("File"), EditorStringName(EditorIcons));
	}
	if (p_section_id == "folders") {
		return p_owner->get_theme_icon(SNAME("folder"), SNAME("FileDialog"));
	}
	if (p_section_id == "scenes") {
		return p_owner->get_theme_icon(SNAME("PackedScene"), EditorStringName(EditorIcons));
	}
	if (p_section_id == "selection") {
		return SolersIcons::get(SNAME("tool_observe"), size);
	}
	return SolersIcons::get(SNAME("plus"), size);
}

static Ref<Texture2D> solers_mention_path_icon(Control *p_owner, const Dictionary &p_mention) {
	const String source = String(p_mention.get("source", String())).strip_edges().to_lower();
	String path = ResourceUID::ensure_path(String(p_mention.get("path", p_mention.get("id", String()))).strip_edges());
	if (path.is_empty()) {
		return Ref<Texture2D>();
	}
	if (source == "folder" || path.ends_with("/")) {
		return p_owner->get_theme_icon(SNAME("folder"), SNAME("FileDialog"));
	}

	if (EditorResourcePreview *previewer = EditorResourcePreview::get_singleton()) {
		const EditorResourcePreview::PreviewItem item = previewer->get_resource_preview_if_available(path);
		if (item.small_preview.is_valid()) {
			return item.small_preview;
		}
		if (item.preview.is_valid()) {
			return item.preview;
		}
	}

	String type = p_mention.get("resource_type", String());
	String icon_path = p_mention.get("icon_path", String());
	bool import_ok = p_mention.get("import_valid", true);
	if (type.is_empty() && EditorFileSystem::get_singleton()) {
		int idx = -1;
		if (EditorFileSystemDirectory *dir = EditorFileSystem::get_singleton()->find_file(path, &idx)) {
			type = dir->get_file_type(idx);
			icon_path = dir->get_file_icon_path(idx);
			import_ok = dir->get_file_import_is_valid(idx);
		}
	}
	if (!icon_path.is_empty()) {
		const Ref<Texture2D> custom = ResourceLoader::load(icon_path);
		if (custom.is_valid()) {
			return custom;
		}
	}
	if (!import_ok) {
		return p_owner->get_theme_icon(SNAME("ImportFail"), EditorStringName(EditorIcons));
	}
	const StringName type_name(type);
	return p_owner->has_theme_icon(type_name, EditorStringName(EditorIcons)) ? p_owner->get_theme_icon(type_name, EditorStringName(EditorIcons)) : p_owner->get_theme_icon(SNAME("File"), EditorStringName(EditorIcons));
}

static Ref<Texture2D> solers_mention_row_icon(Control *p_owner, const Dictionary &p_mention, int p_px) {
	const String source = String(p_mention.get("source", "plugin")).strip_edges().to_lower();
	if (source == "plugin") {
		const String id = String(p_mention.get("id", String())).strip_edges().to_lower();
		const Ref<Texture2D> color = SolersIcons::provider_logo_color(id, p_px);
		return color.is_valid() ? color : SolersIcons::provider_logo(id, p_px);
	}
	if (source == "addon") {
		return SolersIcons::get(SNAME("plugin"), p_px);
	}
	if (source == "node") {
		return SolersIcons::get(SNAME("node"), p_px);
	}
	return solers_mention_path_icon(p_owner, p_mention);
}

static String solers_mention_item_label(const Dictionary &p_mention) {
	const String source = String(p_mention.get("source", "plugin")).strip_edges().to_lower();
	const String id = String(p_mention.get("id", String())).strip_edges();
	const String label = String(p_mention.get("label", id)).strip_edges();
	if (source == "plugin") {
		return "@" + id;
	}
	return label.is_empty() ? id.get_file() : label;
}

Array SolersDock::_mention_inline_parse(const String &p_line_text) {
	Array objects;
	if (!chat_input) {
		return objects;
	}
	const Ref<Font> font = chat_input->get_theme_default_font();
	const int font_size = chat_input->get_theme_font_size(SceneStringName(font_size), SNAME("TextEdit"));
	// width_ratio is relative to the TextEdit line font height (object box height).
	const float line_font_h = font.is_valid() ? font->get_height(font_size) : float(MAX(1, font_size));
	const int icon_px = int(Math::round(13.0f * EDSCALE));

	const Array spans = SolersMention::scan_line_spans(p_line_text);
	for (int i = 0; i < spans.size(); i++) {
		const Dictionary span = spans[i];
		const Dictionary mention = span.get("mention", Dictionary());
		const String label = solers_mention_chip_label(mention);
		if (label.is_empty()) {
			continue;
		}
		const Ref<Texture2D> icon = solers_mention_row_icon(chat_input, mention, icon_px);
		const float chip_w = solers_mention_chip_width(label, font, font_size, icon.is_valid());

		Dictionary info = span.duplicate();
		info["width_ratio"] = chip_w / MAX(1.0f, line_font_h);
		info["chip_label"] = label;
		objects.push_back(info);
	}
	return objects;
}

void SolersDock::_mention_inline_draw(const Dictionary &p_info, const Rect2 &p_rect) {
	if (!chat_input || !p_info.has("mention")) {
		return;
	}
	const Dictionary mention = p_info.get("mention", Dictionary());
	const String label = String(p_info.get("chip_label", solers_mention_chip_label(mention)));
	if (label.is_empty()) {
		return;
	}

	const RID ci = chat_input->get_text_canvas_item();
	const int icon_px = int(Math::round(13.0f * EDSCALE));
	const Rect2 pill = p_rect.grow_individual(-1.0f * EDSCALE, -2.0f * EDSCALE, -1.0f * EDSCALE, -2.0f * EDSCALE);
	const Ref<Font> font = chat_input->get_theme_default_font();
	const int font_size = chat_input->get_theme_font_size(SceneStringName(font_size), SNAME("TextEdit"));
	// Prefer richer path preview when available; fall back to shared chip icon.
	Ref<Texture2D> icon = solers_mention_row_icon(chat_input, mention, icon_px);
	if (icon.is_null()) {
		icon = solers_mention_chip_icon(mention, icon_px);
	}
	solers_draw_mention_chip(ci, pill, label, font, font_size, icon, chat_input->get_theme_color(SNAME("accent_color"), EditorStringName(Editor)));
}

void SolersDock::_mention_inline_click(const Dictionary &p_info, const Rect2 &p_rect) {
	if (!chat_input || !p_info.has("column") || !p_info.has("length")) {
		return;
	}
	const int line = chat_input->get_caret_line();
	const int start = int(p_info["column"]);
	int end = start + int(p_info["length"]);
	const String text = chat_input->get_line(line);
	if (end < text.length() && text[end] == ' ') {
		end++;
	}
	chat_input->select(line, start, line, end);
	chat_input->grab_focus();
}

bool SolersDock::_try_delete_mention_span(int p_direction) {
	if (!chat_input || chat_input->has_selection() || p_direction == 0) {
		return false;
	}
	const int line = chat_input->get_caret_line();
	const int col = chat_input->get_caret_column();
	const String text = chat_input->get_line(line);
	const Array spans = SolersMention::scan_line_spans(text);
	for (int i = 0; i < spans.size(); i++) {
		const Dictionary span = spans[i];
		const int start = int(span.get("column", -1));
		const int len = int(span.get("length", 0));
		if (start < 0 || len <= 0) {
			continue;
		}
		int extent = start + len;
		if (extent < text.length() && text[extent] == ' ') {
			extent++;
		}
		const bool hit = (p_direction < 0) ? (col > start && col <= extent) : (col >= start && col < extent);
		if (!hit) {
			continue;
		}
		chat_input->select(line, start, line, extent);
		chat_input->delete_selection();
		_update_chat_input_height();
		_update_send_enabled();
		_refresh_mention_popup();
		return true;
	}
	return false;
}

void SolersDock::_refresh_mention_popup() {
	if (!chat_input || !plugin_mention_popup || !plugin_mention_list) {
		return;
	}

	const int line = mention_picker_explicit ? plugin_mention_line : chat_input->get_caret_line();
	const int column = chat_input->get_caret_column();
	int mention_start = mention_picker_explicit ? plugin_mention_start_column : -1;
	const String at_query = mention_picker_explicit ? String() : SolersMention::query_at(chat_input->get_line(line), column, mention_start);
	if (mention_start < 0) {
		_hide_mention_popup();
		return;
	}

	const bool was_visible = plugin_mention_popup->is_visible();
	if (plugin_mention_search && !was_visible) {
		plugin_mention_search->set_text(String());
	}

	// Popup LineEdit is the section-local filter; @ query seeds cross-source find
	// only when the search box is empty. Never clear mention_section on filter.
	String filter;
	if (plugin_mention_search) {
		filter = plugin_mention_search->get_text().strip_edges();
	}
	if (filter.is_empty()) {
		filter = at_query;
	}

	plugin_mention_list->clear();
	mention_generation++;
	Array entries;

	if (!mention_section.is_empty()) {
		entries = SolersMention::collect_section_items(mention_section, observation_service, filter);
	} else if (!filter.is_empty()) {
		entries = SolersMention::collect_section_items(String(), observation_service, filter, SolersMention::COLLECT_LIMIT);
	} else {
		entries = SolersMention::collect_root_sections();
	}

	const int icon_px = int(Math::round(14.0f * EDSCALE));
	for (int i = 0; i < entries.size(); i++) {
		const Dictionary value = entries[i];
		const bool section = value.has("id") && !value.has("source");
		Dictionary item_data;
		Ref<Texture2D> icon;
		String label;
		if (section) {
			const String id = value.get("id", String());
			item_data["kind"] = "section";
			item_data["section_id"] = id;
			label = value.get("label", id);
			icon = solers_mention_section_icon(plugin_mention_list, id);
		} else {
			item_data["kind"] = "item";
			item_data["mention"] = value;
			label = solers_mention_item_label(value);
			icon = solers_mention_row_icon(plugin_mention_list, value, icon_px);
		}
		const int index = plugin_mention_list->add_item(label, icon);
		plugin_mention_list->set_item_metadata(index, item_data);
		const String path = value.get("path", String());
		if (!path.is_empty()) {
			plugin_mention_list->set_item_tooltip(index, path);
		}
		const String source = value.get("source", String());
		if (source == "folder") {
			const Color fallback = plugin_mention_list->get_theme_color(SNAME("folder_icon_color"), SNAME("FileDialog"));
			plugin_mention_list->set_item_icon_modulate(index, FileSystemDock::get_dir_icon_color(path, fallback));
		} else if ((source == "file" || source == "scene") && value.get("import_valid", true) && EditorResourcePreview::get_singleton()) {
			EditorResourcePreview::get_singleton()->queue_resource_preview(path, callable_mp(this, &SolersDock::_on_mention_preview_ready).bind(mention_generation, index));
		}
	}

	if (plugin_mention_list->get_item_count() == 0) {
		_hide_mention_popup();
		return;
	}

	plugin_mention_line = line;
	plugin_mention_start_column = mention_start;
	plugin_mention_list->select(0);
	plugin_mention_list->set_current(0);

	const Rect2 input_rect = chat_input->get_global_rect();
	const Rect2 dock_rect = get_global_rect();
	const float width = MIN(360.0f * EDSCALE, MAX(220.0f * EDSCALE, input_rect.size.x));
	const float max_height = MIN(320.0f * EDSCALE, MAX(120.0f * EDSCALE, get_size().y * 0.45f));
	plugin_mention_popup->set_size(Size2(width, max_height));
	plugin_mention_list->set_custom_minimum_size(Size2(0, max_height));
	plugin_mention_list->force_update_list_size();
	const int last_visible = MIN(SOLERS_MENTION_VISIBLE_ROWS, plugin_mention_list->get_item_count()) - 1;
	plugin_mention_list->set_custom_minimum_size(Size2(0, plugin_mention_list->get_item_rect(last_visible).get_end().y));
	const float height = MIN(max_height, plugin_mention_popup->get_combined_minimum_size().y);
	const float x = CLAMP(input_rect.position.x, dock_rect.position.x + 6.0f * EDSCALE, MAX(dock_rect.position.x + 6.0f * EDSCALE, dock_rect.position.x + dock_rect.size.x - width - 6.0f * EDSCALE));
	const float y = MAX(dock_rect.position.y + 6.0f * EDSCALE, input_rect.position.y - height - 6.0f * EDSCALE);
	plugin_mention_popup->set_size(Size2(width, height));
	plugin_mention_popup->set_global_position(Point2(x, y));
	plugin_mention_popup->show();
	plugin_mention_popup->move_to_front();
	plugin_mention_list->ensure_current_is_visible();
}

void SolersDock::_on_mention_search_changed(const String &) {
	if (plugin_mention_popup && plugin_mention_popup->is_visible()) {
		_refresh_mention_popup();
	}
}

void SolersDock::_on_mention_item_clicked(int p_index, const Vector2 &, MouseButton p_button) {
	if (p_button == MouseButton::LEFT) {
		plugin_mention_list->set_current(p_index);
		_activate_mention_selection();
	}
}

void SolersDock::_on_mention_preview_ready(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, uint64_t p_generation, int p_index) {
	if (p_generation != mention_generation || !plugin_mention_list || p_index < 0 || p_index >= plugin_mention_list->get_item_count()) {
		return;
	}
	const Dictionary item_data = plugin_mention_list->get_item_metadata(p_index);
	const Dictionary mention = item_data.get("mention", Dictionary());
	if (String(mention.get("path", String())) == p_path && (p_small_preview.is_valid() || p_preview.is_valid())) {
		plugin_mention_list->set_item_icon(p_index, p_small_preview.is_valid() ? p_small_preview : p_preview);
	}
}

void SolersDock::_hide_mention_popup() {
	if (plugin_mention_popup) {
		plugin_mention_popup->hide();
	}
	if (plugin_mention_search) {
		plugin_mention_search->set_text(String());
	}
	if (plugin_mention_list) {
		plugin_mention_list->clear();
	}
	mention_generation++;
	plugin_mention_line = -1;
	plugin_mention_start_column = -1;
	mention_picker_explicit = false;
	mention_section = String();
}

void SolersDock::_select_mention(const Dictionary &p_mention) {
	if (!chat_input || plugin_mention_line < 0 || plugin_mention_start_column < 0 || chat_input->get_caret_line() != plugin_mention_line) {
		_hide_mention_popup();
		return;
	}
	const String token = SolersMention::format_token(p_mention);
	if (token.is_empty()) {
		_hide_mention_popup();
		return;
	}
	chat_input->select(plugin_mention_line, plugin_mention_start_column, plugin_mention_line, chat_input->get_caret_column());
	chat_input->insert_text_at_caret(token + " ");
	_hide_mention_popup();
	chat_input->grab_focus();
	_update_chat_input_height();
	_update_send_enabled();
}

void SolersDock::_activate_mention_selection() {
	const int selected = plugin_mention_list ? plugin_mention_list->get_current() : -1;
	if (selected < 0 || selected >= plugin_mention_list->get_item_count()) {
		return;
	}
	const Dictionary item_data = plugin_mention_list->get_item_metadata(selected);
	const String kind = item_data.get("kind", String());
	if (kind == "section") {
		mention_section = item_data.get("section_id", String());
		_refresh_mention_popup();
		return;
	}
	if (kind == "item") {
		_select_mention(item_data.get("mention", Dictionary()));
	}
}

void SolersDock::_move_mention_selection(int p_delta) {
	const int count = plugin_mention_list ? plugin_mention_list->get_item_count() : 0;
	if (count == 0) {
		return;
	}
	const int selected = (plugin_mention_list->get_current() + p_delta + count) % count;
	plugin_mention_list->select(selected);
	plugin_mention_list->set_current(selected);
	plugin_mention_list->ensure_current_is_visible();
}

void SolersDock::_on_add_context_pressed() {
	_hide_mention_popup();
	plugin_mention_line = chat_input->get_caret_line();
	plugin_mention_start_column = chat_input->get_caret_column();
	mention_picker_explicit = true;
	_refresh_mention_popup();
	plugin_mention_search->grab_focus();
}

bool SolersDock::_add_image_attachment_from_clipboard() {
	DisplayServer *ds = DisplayServer::get_singleton();
	if (!ds || !ds->clipboard_has_image() || pending_attachments.size() >= 4) {
		return false;
	}
	Ref<Image> image = ds->clipboard_get_image();
	if (image.is_null() || image->is_empty()) {
		return false;
	}
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	if (dir.is_null() || dir->make_dir_recursive(solers_attachment_dir()) != OK) {
		return false;
	}
	const String id = vformat("img_%s_%d", itos((int64_t)Time::get_singleton()->get_ticks_usec()), pending_attachments.size() + 1);
	const String stored_path = solers_attachment_dir().path_join(id + ".png");
	const PackedByteArray bytes = image->save_png_to_buffer();
	Ref<FileAccess> file = FileAccess::open(stored_path, FileAccess::WRITE);
	if (file.is_null() || bytes.is_empty()) {
		return false;
	}
	file->store_buffer(bytes);
	file->close();
	const String sha256 = FileAccess::get_sha256(stored_path);
	const String content_path = solers_attachment_dir().path_join(sha256 + ".png");
	if (FileAccess::exists(content_path)) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(stored_path));
	} else if (DirAccess::rename_absolute(ProjectSettings::get_singleton()->globalize_path(stored_path), ProjectSettings::get_singleton()->globalize_path(content_path)) != OK) {
		return false;
	}

	Dictionary attachment;
	attachment["id"] = sha256;
	attachment["type"] = "image";
	attachment["mime_type"] = "image/png";
	attachment["filename"] = sha256 + ".png";
	attachment["local_path"] = content_path;
	attachment["content_sha256"] = sha256;
	pending_attachments.push_back(attachment);
	_refresh_attachment_bar();
	_update_send_enabled();
	return true;
}

void SolersDock::_refresh_attachment_bar() {
	if (!attachment_bar) {
		return;
	}
	while (attachment_bar->get_child_count() > 0) {
		Node *child = attachment_bar->get_child(0);
		attachment_bar->remove_child(child);
		child->queue_free();
	}
	attachment_bar->set_visible(!pending_attachments.is_empty());
	for (int i = 0; i < pending_attachments.size(); i++) {
		const Dictionary attachment = pending_attachments[i];
		SolersAttachmentThumb *thumb = memnew(SolersAttachmentThumb);
		thumb->setup(attachment, callable_mp(this, &SolersDock::_remove_attachment).bind(i));
		attachment_bar->add_child(thumb);
	}
}

void SolersDock::_remove_attachment(int p_index) {
	if (p_index >= 0 && p_index < pending_attachments.size()) {
		pending_attachments.remove_at(p_index);
	}
	_refresh_attachment_bar();
	_update_send_enabled();
}

void SolersDock::_clear_attachments() {
	pending_attachments.clear();
	_refresh_attachment_bar();
	_update_send_enabled();
}

void SolersDock::_update_send_enabled() {
	if (send_chat_button && chat_input) {
		const bool blocked = permission_manager && permission_manager->get_pending_request_count() > 0;
		const bool running = agent_session && agent_session->is_running();
		const Dictionary provider = settings_service ? Dictionary(settings_service->get_provider_config().get("data", Dictionary())) : Dictionary();
		const bool provider_available = provider.get("connected", false) && provider.get("available", false);
		chat_input->set_editable(!blocked);
		if (running) {
			const bool has_steering_input = !chat_input->get_text().strip_edges().is_empty() || !pending_attachments.is_empty();
			if (has_steering_input) {
				send_chat_button->configure(SNAME("send_up"), SolersGlyphButton::SKIN_PRIMARY, TTR("Send to the running turn"), 16);
			} else {
				send_chat_button->configure(SNAME("stop"), SolersGlyphButton::SKIN_PRIMARY, TTR("Stop generation"), 14);
			}
			send_chat_button->set_pressed_callback(callable_mp(this, &SolersDock::_on_send_chat_pressed));
			send_chat_button->set_enabled(!blocked);
		} else {
			const String tooltip = provider_available ? TTR("Send") : (provider.get("connected", false) ? TTR("Local Models Only blocks the selected remote provider.") : TTR("Connect a model provider before sending."));
			send_chat_button->configure(SNAME("send_up"), SolersGlyphButton::SKIN_PRIMARY, tooltip, 16);
			send_chat_button->set_pressed_callback(callable_mp(this, &SolersDock::_on_send_chat_pressed));
			send_chat_button->set_enabled(!blocked && provider_available && (!chat_input->get_text().strip_edges().is_empty() || !pending_attachments.is_empty()));
		}
	}
}

void SolersDock::_update_chat_input_height() {
	if (!chat_input || !chat_input->is_inside_tree()) {
		return;
	}

	const int line_height = MAX(1, chat_input->get_line_height());
	const int visible_rows = MAX(1, chat_input->get_total_visible_line_count());
	const float text_height = CLAMP(float(visible_rows * line_height) + 20.0f * EDSCALE, SOLERS_COMPOSER_TEXT_MIN_HEIGHT * EDSCALE, SOLERS_COMPOSER_TEXT_MAX_HEIGHT * EDSCALE);
	chat_input->set_custom_minimum_size(Size2(0, text_height));

	Control *composer_card = Object::cast_to<Control>(chat_input->get_parent() ? chat_input->get_parent()->get_parent() : nullptr);
	if (composer_card) {
		composer_card->set_custom_minimum_size(Size2(0, text_height + SOLERS_COMPOSER_TOOLBAR_HEIGHT * EDSCALE + SOLERS_COMPOSER_VERTICAL_CHROME * EDSCALE));
	}

	const int max_visible_rows = MAX(1, int((SOLERS_COMPOSER_TEXT_MAX_HEIGHT * EDSCALE) / line_height));
	if (visible_rows > max_visible_rows) {
		chat_input->set_v_scroll(MAX(0, chat_input->get_total_visible_line_count() - chat_input->get_visible_line_count()));
	}
}

void SolersDock::_sync_approval_panel() {
	if (!approval_overlay_inset) {
		return;
	}
	if (!permission_manager) {
		approval_overlay_inset->set_visible(false);
		_update_send_enabled();
		return;
	}

	Array pending = permission_manager->list_pending_requests();
	if (permission_manager->is_auto_approve_all()) {
		for (int i = 0; i < pending.size(); i++) {
			const Dictionary request = pending[i];
			permission_manager->approve_request(request.get("id", 0));
		}
		pending = permission_manager->list_pending_requests();
	}
	if (pending.is_empty()) {
		active_approval_id = 0;
		approval_overlay_inset->set_visible(false);
		_update_send_enabled();
		return;
	}

	const Dictionary request = pending[0];
	const int request_id = request.get("id", 0);
	if (active_approval_id != request_id) {
		active_approval_id = request_id;
		approval_choice = "once";
		approval_always_confirming = false;
	}
	approval_overlay_inset->set_visible(true);

	const String tool = String(request.get("tool", String()));
	const String permission = String(request.get("permission", String()));
	const Dictionary args = request.get("args", Dictionary());

	if (approval_tool_label) {
		approval_tool_label->set_text(tool);
	}
	if (approval_summary_label) {
		String summary = solers_summarize_tool_args(JSON::stringify(args, "", false, true));
		if (summary.is_empty()) {
			summary = permission;
		} else {
			summary = vformat("%s - %s", permission, summary);
		}
		approval_summary_label->set_text(summary);
	}
	_set_approval_choice(approval_choice);
	_update_send_enabled();
}

void SolersDock::_set_approval_choice(const String &p_choice) {
	approval_choice = p_choice;
	if (p_choice != "always") {
		approval_always_confirming = false;
	}
	if (approval_once_button) {
		approval_once_button->set_text(p_choice == "once" ? TTR("1  Allow once *") : TTR("1  Allow once"));
	}
	if (approval_always_button) {
		approval_always_button->set_text(p_choice == "always" ? TTR("2  Allow always *") : TTR("2  Allow always"));
	}
	if (approval_reject_button) {
		approval_reject_button->set_text(p_choice == "reject" ? TTR("3  Deny *") : TTR("3  Deny"));
	}
	if (approval_submit_button) {
		approval_submit_button->set_text(approval_always_confirming ? TTR("Confirm") : TTR("Submit"));
	}
}

void SolersDock::_submit_current_approval() {
	if (!permission_manager) {
		return;
	}
	Array pending = permission_manager->list_pending_requests();
	if (pending.is_empty()) {
		_refresh_status();
		return;
	}
	Dictionary request = pending[0];
	const int request_id = request.get("id", 0);
	if (approval_choice == "reject") {
		permission_manager->reject_request(request_id);
	} else if (approval_choice == "always") {
		if (!approval_always_confirming) {
			approval_always_confirming = true;
			_set_approval_choice("always");
			return;
		}
		const int permission_id = request.get("permission_id", (int)SolersPermissionManager::PERMISSION_OBSERVE);
		permission_manager->set_auto_approve_permission((SolersPermissionManager::Permission)permission_id, true);
		permission_manager->approve_request(request_id);
	} else {
		permission_manager->approve_request(request_id);
	}
	approval_always_confirming = false;
	_refresh_status();
	_sync_approval_panel();
}

void SolersDock::_set_auto_approve_mode(bool p_enabled, bool p_persist) {
	if (!permission_manager) {
		return;
	}
	permission_manager->set_auto_approve_all(p_enabled);
	if (approval_mode_chip) {
		approval_mode_chip->set_texts(p_enabled ? TTR("Auto") : TTR("Manual"), String());
		approval_mode_chip->set_tooltip_text(p_enabled ? TTR("Auto-approve each pending tool call once.") : TTR("Ask before mutating tool calls."));
	}
	if (p_persist && EditorSettings::get_singleton()) {
		EditorSettings::get_singleton()->set_project_metadata("solers", "auto_approve_mode", p_enabled);
	}
}

void SolersDock::_on_auto_approve_chip_pressed() {
	const bool enabled = permission_manager && permission_manager->is_auto_approve_all();
	_set_auto_approve_mode(!enabled, true);
	_sync_approval_panel();
}

void SolersDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_bind_layout_splits();
			_sync_layout_widths();
			_update_chat_input_height();
			_update_send_enabled();
			_refresh_status();
		} break;
		case NOTIFICATION_RESIZED: {
			_sync_layout_widths();
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			_update_chat_input_height();
			_refresh_model_chip();
			_sync_session_button();
			SolersUITheme::configure_settings_host(provider_settings_dialog, get_theme());
		} break;
		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			_refresh_status();
			_sync_session_button();
		} break;
		case NOTIFICATION_TRANSLATION_CHANGED: {
			_refresh_status();
			_refresh_session_list();
		} break;
	}
}

void SolersDock::set_services(SolersObservationService *p_observation_service, SolersToolRegistry *p_tool_registry, SolersActionTimeline *p_action_timeline, SolersPermissionManager *p_permission_manager, SolersMCPAdapter *p_mcp_adapter, SolersRpcServer *p_rpc_server, SolersSettingsService *p_settings_service) {
	observation_service = p_observation_service;
	tool_registry = p_tool_registry;
	action_timeline = p_action_timeline;
	permission_manager = p_permission_manager;
	mcp_adapter = p_mcp_adapter;
	rpc_server = p_rpc_server;
	settings_service = p_settings_service;
	if (provider_settings_view && p_settings_service) {
		provider_settings_view->bind_services(p_settings_service);
	}
	const bool auto_mode = EditorSettings::get_singleton() ? (bool)EditorSettings::get_singleton()->get_project_metadata("solers", "auto_approve_mode", false) : false;
	_set_auto_approve_mode(auto_mode, false);
	_refresh_status();
	_sync_approval_panel();
}

void SolersDock::make_visible() {
	_refresh_status();
	if (session_sidebar && EditorSettings::get_singleton()) {
		const bool show = EditorSettings::get_singleton()->get_project_metadata("solers", "session_sidebar_visible", false);
		session_sidebar->set_visible(show);
		_sync_session_button();
		if (show) {
			_request_session_list_refresh();
		}
	}
}

SolersDock::SolersDock() {
	set_h_size_flags(Control::SIZE_FILL);
	set_v_size_flags(Control::SIZE_EXPAND_FILL);
	add_theme_style_override("panel", solers_make_stylebox(SOLERS_BG, Color(0, 0, 0, 0), 0, 0));

	root_box = memnew(VBoxContainer);
	root_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	root_box->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	root_box->add_theme_constant_override("separation", 0);
	add_child(root_box);

	body_split = memnew(HSplitContainer);
	body_split->set_name("ConversationSplit");
	body_split->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	body_split->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	body_split->set_dragger_visibility(SplitContainer::DRAGGER_HIDDEN);
	body_split->connect(SNAME("drag_started"), callable_mp(this, &SolersDock::_set_layout_dragging).bind(true));
	body_split->connect(SNAME("drag_ended"), callable_mp(this, &SolersDock::_set_layout_dragging).bind(false));
	root_box->add_child(body_split);

	session_sidebar = memnew(PanelContainer);
	session_sidebar->set_custom_minimum_size(Size2(272, 0) * EDSCALE);
	session_sidebar->set_h_size_flags(Control::SIZE_FILL);
	session_sidebar->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	session_sidebar->add_theme_style_override(SceneStringName(panel), solers_make_stylebox(SOLERS_BG, Color(0, 0, 0, 0), 0, 0));
	session_sidebar->hide();
	body_split->add_child(session_sidebar);

	VBoxContainer *sidebar_root = memnew(VBoxContainer);
	sidebar_root->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	sidebar_root->add_theme_constant_override("separation", 0);
	session_sidebar->add_child(sidebar_root);

	MarginContainer *sidebar_pad = memnew(MarginContainer);
	sidebar_pad->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	sidebar_pad->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	sidebar_pad->add_theme_constant_override("margin_left", 12 * EDSCALE);
	sidebar_pad->add_theme_constant_override("margin_right", 12 * EDSCALE);
	sidebar_pad->add_theme_constant_override("margin_top", 10 * EDSCALE);
	sidebar_pad->add_theme_constant_override("margin_bottom", 10 * EDSCALE);
	sidebar_root->add_child(sidebar_pad);

	VBoxContainer *sidebar_box = memnew(VBoxContainer);
	sidebar_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	sidebar_box->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	sidebar_box->add_theme_constant_override("separation", 8 * EDSCALE);
	sidebar_pad->add_child(sidebar_box);

	Button *new_chat = memnew(Button);
	new_chat->set_name("NewChat");
	new_chat->set_text("New chat");
	new_chat->set_button_icon(SolersIcons::get(SNAME("new_chat"), int(Math::round(16.0f * EDSCALE))));
	new_chat->set_icon_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	solers_style_session_button(new_chat, false);
	new_chat->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_new_chat_pressed));
	sidebar_box->add_child(new_chat);

	session_scroll = memnew(ScrollContainer);
	session_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	session_scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	session_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	session_scroll->set_follow_focus(true);
	session_scroll->get_v_scroll_bar()->set_self_modulate(Color(1, 1, 1, 0.45));
	sidebar_box->add_child(session_scroll);

	session_list = memnew(VBoxContainer);
	session_list->set_name("SessionList");
	session_list->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	session_list->add_theme_constant_override("separation", 4 * EDSCALE);
	session_scroll->add_child(session_list);

	chat_column = memnew(VBoxContainer);
	chat_column->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	chat_column->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	chat_column->add_theme_constant_override("separation", 0);
	body_split->add_child(chat_column);

	MarginContainer *topbar_inset = memnew(MarginContainer);
	topbar_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_inset->set_custom_minimum_size(Size2(0, 40 * EDSCALE));
	topbar_inset->add_theme_constant_override("margin_left", 10 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_right", 10 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_top", 5 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_bottom", 5 * EDSCALE);
	chat_column->add_child(topbar_inset);

	HBoxContainer *topbar_content = memnew(HBoxContainer);
	topbar_content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_content->add_theme_constant_override("separation", 4 * EDSCALE);
	topbar_inset->add_child(topbar_content);

	// Same panel-left glyph as the editor Side Panel fold control.
	session_button = memnew(SolersGlyphButton);
	session_button->configure(SNAME("panel"), SolersGlyphButton::SKIN_GHOST, TTR("Sessions"), 15);
	session_button->set_pressed_callback(callable_mp(this, &SolersDock::_toggle_session_sidebar));
	topbar_content->add_child(session_button);

	empty_home = memnew(VBoxContainer);
	empty_home->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	empty_home->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	empty_home->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	empty_home->add_theme_constant_override("separation", 28 * EDSCALE);
	chat_column->add_child(empty_home);

	empty_state = _create_empty_state();
	empty_home->add_child(empty_state);

	/* Conversation timeline. */

	chat_scroll = memnew(ScrollContainer);
	chat_scroll->set_name("ChatTimelineScroll");
	chat_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	chat_scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	chat_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_SHOW_NEVER);
	chat_scroll->get_v_scroll_bar()->connect(SNAME("scrolling"), callable_mp(this, &SolersDock::_on_timeline_scrolled));
	chat_scroll->connect(SceneStringName(sort_children), callable_mp(this, &SolersDock::_commit_timeline_layout), CONNECT_DEFERRED);
	chat_scroll->hide();
	chat_column->add_child(chat_scroll);

	message_list = memnew(VBoxContainer);
	message_list->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	message_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	message_list->add_theme_constant_override("separation", 14 * EDSCALE);
	message_list->connect(SceneStringName(sort_children), callable_mp(this, &SolersDock::_queue_timeline_layout_commit));

	timeline_inset = memnew(MarginContainer);
	timeline_inset->set_name("TimelineInset");
	timeline_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	timeline_inset->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	timeline_inset->add_theme_constant_override("margin_left", 20 * EDSCALE);
	timeline_inset->add_theme_constant_override("margin_right", 20 * EDSCALE);
	timeline_inset->add_theme_constant_override("margin_top", 10 * EDSCALE);
	timeline_inset->add_theme_constant_override("margin_bottom", 12 * EDSCALE);
	timeline_inset->add_child(message_list);
	chat_scroll->add_child(timeline_inset);

	/* Approval prompt — shown inline above the composer when a tool is blocked. */

	approval_overlay_inset = memnew(MarginContainer);
	approval_overlay_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_overlay_inset->add_theme_constant_override("margin_left", 20 * EDSCALE);
	approval_overlay_inset->add_theme_constant_override("margin_right", 20 * EDSCALE);
	approval_overlay_inset->add_theme_constant_override("margin_top", 0);
	approval_overlay_inset->add_theme_constant_override("margin_bottom", 8 * EDSCALE);
	approval_overlay_inset->set_visible(false);
	chat_column->add_child(approval_overlay_inset);

	approval_overlay_card = _create_panel_card(Color(0.104, 0.106, 0.112), Color(1.0, 0.49, 0.20, 0.34), 14, 12);
	approval_overlay_card->set_custom_minimum_size(Size2(0, 118 * EDSCALE));
	approval_overlay_inset->add_child(approval_overlay_card);

	VBoxContainer *approval_box = memnew(VBoxContainer);
	approval_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_box->add_theme_constant_override("separation", 4 * EDSCALE);
	approval_overlay_card->add_child(approval_box);

	HBoxContainer *approval_header = memnew(HBoxContainer);
	approval_header->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_box->add_child(approval_header);

	Label *approval_title = memnew(Label(TTR("Allow using this tool?")));
	approval_header->add_child(approval_title);

	approval_tool_label = memnew(Label);
	approval_tool_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_tool_label->set_clip_text(true);
	approval_tool_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	approval_tool_label->add_theme_color_override("font_color", SOLERS_TEXT_BODY);
	approval_header->add_child(approval_tool_label);

	approval_summary_label = memnew(Label);
	approval_summary_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_summary_label->set_clip_text(true);
	approval_summary_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	approval_summary_label->add_theme_color_override("font_color", SOLERS_TEXT_DIM);
	approval_box->add_child(approval_summary_label);

	HBoxContainer *approval_actions = memnew(HBoxContainer);
	approval_actions->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_box->add_child(approval_actions);

	approval_once_button = memnew(Button(TTR("Allow once")));
	approval_once_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_once_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_set_approval_choice).bind("once"));
	approval_actions->add_child(approval_once_button);

	approval_always_button = memnew(Button(TTR("Allow always")));
	approval_always_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_always_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_set_approval_choice).bind("always"));
	approval_actions->add_child(approval_always_button);

	approval_reject_button = memnew(Button(TTR("Deny")));
	approval_reject_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_reject_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_set_approval_choice).bind("reject"));
	approval_actions->add_child(approval_reject_button);

	approval_submit_button = memnew(Button(TTR("Submit")));
	approval_submit_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_submit_current_approval));
	approval_box->add_child(approval_submit_button);
	/* Composer — one floating rounded card owns text entry and actions. */

	composer_inset = memnew(MarginContainer);
	composer_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_inset->add_theme_constant_override("margin_left", 20 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_right", 20 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_top", 4 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_bottom", 13 * EDSCALE);
	empty_home->add_child(composer_inset);

	VBoxContainer *composer_stack = memnew(VBoxContainer);
	composer_stack->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_stack->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	composer_stack->add_theme_constant_override("separation", 8 * EDSCALE);
	composer_inset->add_child(composer_stack);

	plan_capsule = memnew(SolersPlanCapsule);
	composer_stack->add_child(plan_capsule);

	SolersSurface *composer_card = memnew(SolersSurface);
	composer_card->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_card->configure(SOLERS_COMPOSER_BG, SOLERS_COMPOSER_BORDER, 19, 14, true);
	composer_card->set_custom_minimum_size(Size2(0, (SOLERS_COMPOSER_TEXT_MIN_HEIGHT + SOLERS_COMPOSER_TOOLBAR_HEIGHT + SOLERS_COMPOSER_VERTICAL_CHROME) * EDSCALE));
	composer_stack->add_child(composer_card);

	VBoxContainer *composer = memnew(VBoxContainer);
	composer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer->add_theme_constant_override("separation", 0);
	composer_card->add_child(composer);

	chat_input = memnew(TextEdit);
	chat_input->set_name("ComposerInput");
	chat_input->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	chat_input->set_custom_minimum_size(Size2(0, SOLERS_COMPOSER_TEXT_MIN_HEIGHT * EDSCALE));
	chat_input->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	chat_input->set_placeholder(TTR("Ask Solers to create..."));
	chat_input->set_smooth_scroll_enabled(true);
	chat_input->set_scroll_past_end_of_file_enabled(false);
	chat_input->set_fit_content_height_enabled(false);
	chat_input->set_indent_wrapped_lines(false);
	chat_input->set_highlight_current_line(false);
	chat_input->set_draw_minimap(false);
	chat_input->set_caret_blink_enabled(true);
	chat_input->add_theme_style_override("normal", memnew(StyleBoxEmpty));
	chat_input->add_theme_style_override("focus", memnew(StyleBoxEmpty));
	chat_input->add_theme_style_override("read_only", memnew(StyleBoxEmpty));
	chat_input->add_theme_color_override("font_color", SOLERS_TEXT_PRIMARY);
	chat_input->add_theme_color_override("font_placeholder_color", SOLERS_TEXT_PLACEHOLDER);
	chat_input->add_theme_color_override("background_color", Color(0, 0, 0, 0));
	chat_input->add_theme_color_override("caret_color", Color(0.86, 0.91, 0.98, 1));
	chat_input->add_theme_color_override("selection_color", Color(0.10, 0.42, 0.62, 0.56));
	chat_input->add_theme_constant_override("line_spacing", 4 * EDSCALE);
	chat_input->add_theme_font_size_override(SceneStringName(font_size), 14 * EDSCALE);
	chat_input->connect(SceneStringName(gui_input), callable_mp(this, &SolersDock::_on_chat_input_gui_input));
	chat_input->connect(SceneStringName(text_changed), callable_mp(this, &SolersDock::_on_chat_input_text_changed));
	chat_input->connect(SNAME("caret_changed"), callable_mp(this, &SolersDock::_refresh_mention_popup));
	chat_input->set_inline_object_handlers(
			callable_mp(this, &SolersDock::_mention_inline_parse),
			callable_mp(this, &SolersDock::_mention_inline_draw),
			callable_mp(this, &SolersDock::_mention_inline_click));
	composer->add_child(chat_input);

	attachment_bar = memnew(HBoxContainer);
	attachment_bar->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	attachment_bar->add_theme_constant_override("separation", 6 * EDSCALE);
	attachment_bar->hide();
	composer->add_child(attachment_bar);

	HBoxContainer *composer_toolbar = memnew(HBoxContainer);
	composer_toolbar->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_toolbar->set_custom_minimum_size(Size2(0, SOLERS_COMPOSER_TOOLBAR_HEIGHT * EDSCALE));
	composer_toolbar->set_alignment(BoxContainer::ALIGNMENT_BEGIN);
	composer_toolbar->add_theme_constant_override("separation", 6 * EDSCALE);
	composer->add_child(composer_toolbar);

	add_context_button = memnew(SolersGlyphButton);
	add_context_button->set_name("AddContextButton");
	add_context_button->configure(SNAME("plus"), SolersGlyphButton::SKIN_GHOST, TTR("Add context"), 15);
	add_context_button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	add_context_button->set_pressed_callback(callable_mp(this, &SolersDock::_on_add_context_pressed));
	composer_toolbar->add_child(add_context_button);

	approval_mode_chip = memnew(SolersSelectChip);
	approval_mode_chip->configure(SNAME("shield"), TTR("Manual"), String(), TTR("Ask before mutating tool calls."));
	approval_mode_chip->set_pressed_callback(callable_mp(this, &SolersDock::_on_auto_approve_chip_pressed));
	composer_toolbar->add_child(approval_mode_chip);

	Control *toolbar_spacer = memnew(Control);
	toolbar_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_toolbar->add_child(toolbar_spacer);

	model_chip = memnew(SolersSelectChip);
	model_chip->configure(StringName(), TTR("Model"), String(), TTR("Model and provider"));
	model_chip->set_pressed_callback(callable_mp(this, &SolersDock::_on_model_chip_pressed));
	composer_toolbar->add_child(model_chip);

	send_chat_button = memnew(SolersGlyphButton);
	send_chat_button->configure(SNAME("send_up"), SolersGlyphButton::SKIN_PRIMARY, TTR("Send"), 16);
	send_chat_button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	send_chat_button->set_pressed_callback(callable_mp(this, &SolersDock::_on_send_chat_pressed));
	send_chat_button->set_enabled(false);
	composer_toolbar->add_child(send_chat_button);

	plugin_mention_popup = memnew(PanelContainer);
	plugin_mention_popup->set_name("MentionPopup");
	plugin_mention_popup->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	// Escape PanelContainer's SORT_CHILDREN fit_child_in_rect — otherwise the
	// mention sheet is stretched to the entire SolersDock on every layout pass.
	plugin_mention_popup->set_as_top_level(true);
	plugin_mention_popup->add_theme_style_override(SceneStringName(panel), solers_make_stylebox(SOLERS_POPUP_BG, Color(0, 0, 0, 0), 10, 6, true));
	plugin_mention_popup->hide();
	add_child(plugin_mention_popup);

	plugin_mention_box = memnew(VBoxContainer);
	plugin_mention_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	plugin_mention_box->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	plugin_mention_box->add_theme_constant_override("separation", 4 * EDSCALE);
	plugin_mention_popup->add_child(plugin_mention_box);

	plugin_mention_search = memnew(LineEdit);
	plugin_mention_search->set_name("MentionSearch");
	plugin_mention_search->set_placeholder(TTR("Search..."));
	plugin_mention_search->set_clear_button_enabled(true);
	solers_style_bare_search_line_edit(plugin_mention_search);
	plugin_mention_search->connect(SceneStringName(text_changed), callable_mp(this, &SolersDock::_on_mention_search_changed));
	plugin_mention_search->connect(SceneStringName(gui_input), callable_mp(this, &SolersDock::_on_chat_input_gui_input));
	plugin_mention_box->add_child(plugin_mention_search);

	plugin_mention_list = memnew(ItemList);
	plugin_mention_list->set_name("MentionList");
	plugin_mention_list->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	plugin_mention_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	plugin_mention_list->set_icon_mode(ItemList::ICON_MODE_LEFT);
	plugin_mention_list->set_max_columns(1);
	plugin_mention_list->set_max_text_lines(1);
	plugin_mention_list->set_fixed_icon_size(Size2i(int(16 * EDSCALE), int(16 * EDSCALE)));
	plugin_mention_list->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	plugin_mention_list->set_allow_search(false);
	plugin_mention_list->add_theme_font_size_override(SceneStringName(font_size), int(13 * EDSCALE));
	plugin_mention_list->add_theme_color_override(SceneStringName(font_color), SOLERS_TEXT_BODY);
	plugin_mention_list->add_theme_style_override(SceneStringName(panel), memnew(StyleBoxEmpty));
	plugin_mention_list->connect(SNAME("item_clicked"), callable_mp(this, &SolersDock::_on_mention_item_clicked));
	plugin_mention_box->add_child(plugin_mention_list);

	model_popup_overlay = memnew(Control);
	model_popup_overlay->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	model_popup_overlay->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	model_popup_overlay->connect(SceneStringName(gui_input), callable_mp(this, &SolersDock::_on_model_popup_overlay_gui_input));
	model_popup_overlay->connect(SceneStringName(resized), callable_mp(this, &SolersDock::_hide_model_popup));
	model_popup_overlay->hide();
	add_child(model_popup_overlay);

	provider_settings_dialog = memnew(AcceptDialog);
	provider_settings_dialog->set_title(TTR("Settings"));
	provider_settings_dialog->set_min_size(Size2(980, 640) * EDSCALE);
	add_child(provider_settings_dialog);

	provider_settings_view = memnew(SolersPMAIView);
	provider_settings_view->set_name("ProviderSettings");
	provider_settings_view->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	provider_settings_view->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	provider_settings_dialog->add_child(provider_settings_view);

	model_menu = memnew(PanelContainer);
	model_menu->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	model_menu->hide();
	model_menu->add_theme_style_override(SceneStringName(panel), solers_make_stylebox(SOLERS_POPUP_BG, Color(0, 0, 0, 0), 14, 0, true));
	model_popup_overlay->add_child(model_menu);
	MarginContainer *model_menu_margin = memnew(MarginContainer);
	model_menu_margin->add_theme_constant_override("margin_left", 6 * EDSCALE);
	model_menu_margin->add_theme_constant_override("margin_right", 6 * EDSCALE);
	model_menu_margin->add_theme_constant_override("margin_top", 6 * EDSCALE);
	model_menu_margin->add_theme_constant_override("margin_bottom", 6 * EDSCALE);
	model_menu->add_child(model_menu_margin);
	model_menu_box = memnew(VBoxContainer);
	model_menu_box->add_theme_constant_override("separation", 2 * EDSCALE);
	model_menu_margin->add_child(model_menu_box);

	model_submenu = memnew(PanelContainer);
	model_submenu->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	model_submenu->hide();
	model_submenu->add_theme_style_override(SceneStringName(panel), solers_make_stylebox(SOLERS_POPUP_BG, Color(0, 0, 0, 0), 14, 0, true));
	model_popup_overlay->add_child(model_submenu);
	MarginContainer *model_submenu_margin = memnew(MarginContainer);
	model_submenu_margin->add_theme_constant_override("margin_left", 8 * EDSCALE);
	model_submenu_margin->add_theme_constant_override("margin_right", 8 * EDSCALE);
	model_submenu_margin->add_theme_constant_override("margin_top", 8 * EDSCALE);
	model_submenu_margin->add_theme_constant_override("margin_bottom", 8 * EDSCALE);
	model_submenu->add_child(model_submenu_margin);
	model_submenu_box = memnew(VBoxContainer);
	model_submenu_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	model_submenu_box->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	model_submenu_box->add_theme_constant_override("separation", 4 * EDSCALE);
	model_submenu_margin->add_child(model_submenu_box);

	model_submenu_search = memnew(LineEdit);
	model_submenu_search->set_placeholder(TTR("Search models..."));
	model_submenu_search->set_clear_button_enabled(true);
	solers_style_bare_search_line_edit(model_submenu_search);
	model_submenu_search->connect(SceneStringName(text_changed), callable_mp(this, &SolersDock::_on_model_submenu_search));
	model_submenu_box->add_child(model_submenu_search);

	model_submenu_scroll = memnew(ScrollContainer);
	model_submenu_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	model_submenu_scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	model_submenu_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	model_submenu_scroll->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_AUTO);
	model_submenu_scroll->add_theme_style_override(SceneStringName(panel), memnew(StyleBoxEmpty));
	VScrollBar *model_scroll_bar = model_submenu_scroll->get_v_scroll_bar();
	model_scroll_bar->add_theme_style_override("scroll", memnew(StyleBoxEmpty));
	model_scroll_bar->add_theme_style_override("grabber", solers_make_stylebox(Color(1, 1, 1, 0.14), Color(0, 0, 0, 0), 3, 0));
	model_scroll_bar->add_theme_style_override("grabber_highlight", solers_make_stylebox(Color(1, 1, 1, 0.22), Color(0, 0, 0, 0), 3, 0));
	model_scroll_bar->add_theme_style_override("grabber_pressed", solers_make_stylebox(Color(1, 1, 1, 0.30), Color(0, 0, 0, 0), 3, 0));
	model_submenu_list = memnew(VBoxContainer);
	model_submenu_list->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	model_submenu_list->add_theme_constant_override("separation", 2 * EDSCALE);
	model_submenu_scroll->add_child(model_submenu_list);
	model_submenu_box->add_child(model_submenu_scroll);

	_update_chat_input_height();
}

SolersDock::~SolersDock() {
	// The dock is the sole consumer of the glyph cache; release the textures
	// with it so nothing lives past renderer teardown.
	SolersIcons::clear_cache();
	for (Ref<StyleBoxFlat> &style : solers_row_styles) {
		style.unref();
	}
}

void SolersDock::set_agent_session(SolersAgentSession *p_agent_session) {
	agent_session = p_agent_session;
	if (!agent_session) {
		return;
	}
	agent_session->connect(SNAME("model_request_started"), callable_mp(this, &SolersDock::_on_agent_model_request_started));
	agent_session->connect(SNAME("timeline_entry_committed"), callable_mp(this, &SolersDock::_on_agent_timeline_entry_committed));
	agent_session->connect(SNAME("assistant_delta"), callable_mp(this, &SolersDock::_on_agent_assistant_delta));
	agent_session->connect(SNAME("reasoning_delta"), callable_mp(this, &SolersDock::_on_agent_reasoning_delta));
	agent_session->connect(SNAME("assistant_message"), callable_mp(this, &SolersDock::_on_agent_assistant_message));
	agent_session->connect(SNAME("tool_call_started"), callable_mp(this, &SolersDock::_on_agent_tool_started));
	agent_session->connect(SNAME("tool_call_updated"), callable_mp(this, &SolersDock::_on_agent_tool_updated));
	agent_session->connect(SNAME("tool_call_awaiting_approval"), callable_mp(this, &SolersDock::_on_agent_tool_awaiting_approval));
	agent_session->connect(SNAME("tool_call_finished"), callable_mp(this, &SolersDock::_on_agent_tool_finished));
	agent_session->connect(SNAME("turn_completed"), callable_mp(this, &SolersDock::_on_agent_turn_completed));
	agent_session->connect(SNAME("turn_failed"), callable_mp(this, &SolersDock::_on_agent_turn_failed));
	agent_session->connect(SNAME("turn_retrying"), callable_mp(this, &SolersDock::_on_agent_turn_retrying));
	agent_session->connect(SNAME("turn_waiting"), callable_mp(this, &SolersDock::_on_agent_turn_waiting));
	agent_session->connect(SNAME("plan_updated"), callable_mp(this, &SolersDock::_on_agent_plan_updated));
}

void SolersDock::_on_agent_model_request_started() {
	// Covers both the first request and every follow-up after a tool batch.
	_settle_tool_group();
	_ensure_status_cell(TTR("Thinking"));
	_update_send_enabled();
}

void SolersDock::_on_agent_timeline_entry_committed(int64_t p_event_id, const String &p_role) {
	timeline_messages = agent_session->get_timeline_entries();
	if (p_role == SolersLLMRole::ASSISTANT) {
		if (active_assistant_row) {
			active_assistant_row->set_meta("timeline_event_id", p_event_id);
		} else {
			pending_assistant_event_id = p_event_id;
		}
		return;
	}
	if (p_role == "context_compaction") {
		Control *row = _solers_timeline_row(message_list, p_event_id);
		if (row) {
			message_list->remove_child(row);
			row->queue_free();
		}
		_render_timeline(MAX(0, timeline_messages.size() - SOLERS_TIMELINE_WINDOW));
		return;
	}
	if (!message_list) {
		return;
	}
	for (int i = 0; i < message_list->get_child_count(); i++) {
		Control *row = Object::cast_to<Control>(message_list->get_child(i));
		if (row && (int64_t)row->get_meta("timeline_event_id", -1) == p_event_id) {
			row->remove_meta("timeline_pending");
			return;
		}
	}
}

void SolersDock::_on_agent_reasoning_delta(const String &p_text) {
	if (p_text.is_empty() || !message_list) {
		return;
	}
	_clear_empty_state();
	_remove_status_cell();
	_settle_tool_group();
	VBoxContainer *mount = _ensure_assistant_row();
	if (!active_thinking_cell || !active_thinking_cell->is_active()) {
		active_thinking_cell = memnew(SolersThinkingCell);
		active_thinking_cell->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
		mount->add_child(active_thinking_cell);
	}
	active_thinking_cell->append_reasoning(p_text);
	_on_cell_content_changed();
}

void SolersDock::_on_agent_assistant_delta(const String &p_text) {
	if (p_text.is_empty() || !_chat_mount()) {
		return;
	}
	// The model moved from thinking to answering.
	_settle_thinking_cell();
	_settle_tool_group();
	_remove_status_cell();
	_ensure_text_cell()->append_delta(p_text);
}

void SolersDock::_on_agent_assistant_message(const String &p_text) {
	const String text = p_text.strip_edges();
	_settle_tool_group();
	if (active_text_cell) {
		// Authoritative final text for this model step; unchanged streams only
		// drop the caret, avoiding a second full markdown layout.
		active_text_cell->finalize(p_text);
		active_text_cell = nullptr;
		return;
	}
	VBoxContainer *mount = _ensure_assistant_row();
	if (text.is_empty() || !mount) {
		active_assistant_row = nullptr;
		return;
	}
	// Provider without streaming: materialize the step in one piece.
	_settle_thinking_cell();
	_remove_status_cell();
	_clear_empty_state();
	SolersAssistantCell *cell = memnew(SolersAssistantCell);
	cell->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
	mount->add_child(cell);
	cell->set_full_text_immediate(p_text);
	_on_cell_content_changed();
}

void SolersDock::_on_agent_tool_started(const String &p_id, const String &p_name, const String &p_arguments) {
	if (!p_id.is_empty()) {
		SolersToolCell **found = tool_cells_by_id.getptr(p_id);
		if (found && *found) {
			(*found)->update(p_name, p_arguments, solers_tool_ui_kind_for_name(tool_registry, p_name));
			last_started_tool_cell = *found;
			_on_cell_content_changed();
			return;
		}
	}
	_settle_thinking_cell();
	_remove_status_cell();
	_clear_empty_state();
	VBoxContainer *mount = _ensure_assistant_row();
	if (!mount) {
		return;
	}
	if (!active_tool_group) {
		active_tool_group = memnew(SolersToolGroupCell);
		active_tool_group->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
		mount->add_child(active_tool_group);
	}
	SolersToolCell *cell = active_tool_group->add_tool();
	cell->start(p_name, p_arguments, solers_tool_ui_kind_for_name(tool_registry, p_name));
	if (!p_id.is_empty()) {
		tool_cells_by_id.insert(p_id, cell);
	}
	last_started_tool_cell = cell;
	_on_cell_content_changed();
}

void SolersDock::_on_agent_tool_updated(const String &p_id, const String &p_name, const String &p_arguments) {
	if (!p_id.is_empty()) {
		SolersToolCell **found = tool_cells_by_id.getptr(p_id);
		if (found && *found) {
			(*found)->update(p_name, p_arguments, solers_tool_ui_kind_for_name(tool_registry, p_name));
			_on_cell_content_changed();
			return;
		}
	}
	_on_agent_tool_started(p_id, p_name, p_arguments);
}

void SolersDock::_on_agent_tool_awaiting_approval(const String &p_id, const String &p_name) {
	// The call is parked on the permission gate. Its tool cell keeps spinning
	// (the call really is in progress) while we surface the approval prompt;
	// the session resolves the same call in place the moment the user decides.
	_ensure_status_cell(TTR("Awaiting approval"));
	_refresh_status();
	_update_send_enabled();
	_on_cell_content_changed();
}

void SolersDock::_on_agent_tool_finished(const String &p_id, const String &p_name, const Dictionary &p_result, int p_duration_msec) {
	SolersToolCell *cell = nullptr;
	if (!p_id.is_empty()) {
		SolersToolCell **found = tool_cells_by_id.getptr(p_id);
		if (found) {
			cell = *found;
			tool_cells_by_id.erase(p_id);
		}
	}
	if (!cell) {
		cell = last_started_tool_cell;
	}

	const bool ok = p_result.get("ok", false);
	String error_message;
	if (!ok) {
		const Dictionary error = p_result.get("error", Dictionary());
		error_message = error.get("message", String());
	}

	if (cell) {
		cell->finish(ok, error_message, p_duration_msec);
	}
	_remove_status_cell();
	if (active_tool_group) {
		active_tool_group->note_finished(ok);
	}
	if (agent_session) {
		timeline_messages = agent_session->get_timeline_entries();
	}
	if (cell == last_started_tool_cell) {
		last_started_tool_cell = nullptr;
	}
	_refresh_status();
	_on_cell_content_changed();
}

void SolersDock::_on_agent_turn_completed(const Dictionary &) {
	const bool follow_tail = _is_scroll_pinned();
	_finish_turn_cells();
	if (agent_session) {
		timeline_messages = agent_session->get_timeline_entries();
		_render_timeline(follow_tail ? MAX(0, timeline_messages.size() - SOLERS_TIMELINE_WINDOW) : timeline_start);
		const Dictionary plan = agent_session->get_plan();
		_on_agent_plan_updated(plan.get("explanation", String()), plan.get("plan", Array()));
	}
	_refresh_status();
	_update_send_enabled();
	notify_sessions_changed();
}

void SolersDock::_on_agent_turn_failed(const Dictionary &) {
	_finish_turn_cells();
	if (agent_session) {
		timeline_messages = agent_session->get_timeline_entries();
		_render_timeline(timeline_start);
	}
	_refresh_status();
	_update_send_enabled();
}

void SolersDock::_on_agent_turn_retrying(int p_attempt, const String &p_message) {
	// Context/observation is already ready; this is transport retry. Show the native cause.
	_settle_thinking_cell();
	const String detail = p_message.strip_edges();
	if (detail.is_empty()) {
		_ensure_status_cell(vformat(TTR("Reconnecting (attempt %d)..."), p_attempt));
	} else {
		_ensure_status_cell(vformat(TTR("Reconnecting (attempt %d): %s"), p_attempt, detail));
	}
	_update_send_enabled();
}

void SolersDock::_on_agent_turn_waiting(const Dictionary &p_waiting) {
	_settle_thinking_cell();
	_settle_tool_group();
	const Array pending_ids = p_waiting.get("pending_ids", Array());
	if (pending_ids.size() <= 1) {
		_ensure_status_cell(TTR("Waiting for background job..."));
	} else {
		_ensure_status_cell(vformat(TTR("Waiting for %d background jobs..."), pending_ids.size()));
	}
	_update_send_enabled();
}

void SolersDock::_on_agent_plan_updated(const String &p_explanation, const Array &p_plan) {
	if (!plan_capsule) {
		return;
	}
	if (p_plan.is_empty()) {
		plan_capsule->clear_plan();
		return;
	}
	plan_capsule->set_plan(p_explanation, p_plan);
}
