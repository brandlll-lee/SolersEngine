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

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/input/input_event.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/themes/editor_scale.h"
#include "modules/modules_enabled.gen.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_agent_runtime.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/editor/solers_rml_chat_surface.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "modules/solers_ai/protocol/solers_rpc_server.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_button.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/style_box.h"
#include "scene/resources/style_box_flat.h"

#ifdef MODULE_SVG_ENABLED
#include "modules/svg/image_loader_svg.h"
#endif

constexpr float SOLERS_COMPOSER_TEXT_MIN_HEIGHT = 58.0f;
constexpr float SOLERS_COMPOSER_TEXT_MAX_HEIGHT = 250.0f;
constexpr float SOLERS_COMPOSER_TOOLBAR_HEIGHT = 32.0f;
// Card padding (12 top + 8 bottom) plus the gap between the text area and toolbar.
constexpr float SOLERS_COMPOSER_VERTICAL_CHROME = 24.0f;

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

static Ref<Texture2D> solers_texture_from_svg(const String &p_svg, float p_scale = EDSCALE) {
#ifdef MODULE_SVG_ENABLED
	Ref<Image> image;
	image.instantiate();
	Error err = ImageLoaderSVG::create_image_from_string(image, p_svg, p_scale, false, HashMap<Color, Color>());
	if (err == OK && image.is_valid() && !image->is_empty()) {
		return ImageTexture::create_from_image(image);
	}
#endif
	return Ref<Texture2D>();
}

static String solers_micro_icon_svg(const StringName &p_icon, const Color &p_color) {
	const String stroke = p_color.to_html(false);
	// Slimmer, Codex-grade line weight for a more refined glyph silhouette.
	const String prefix = vformat("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#%s\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">", stroke);
	const String suffix = "</svg>";

	if (p_icon == SNAME("solers_plus")) {
		return prefix + "<path d=\"M12 5v14\"/><path d=\"M5 12h14\"/>" + suffix;
	}
	if (p_icon == SNAME("solers_send")) {
		// Slightly tighter arrow that sits visually centered inside the round send pill.
		return prefix + "<path d=\"M12 19V6\"/><path d=\"m6 12 6-6 6 6\"/>" + suffix;
	}
	if (p_icon == SNAME("solers_shield")) {
		return prefix + "<path d=\"M20 13c0 5-3.5 7.5-8 9-4.5-1.5-8-4-8-9V5l8-3 8 3z\"/><path d=\"M9.5 12.5 11 14l3.5-4\"/>" + suffix;
	}
	if (p_icon == SNAME("solers_chevron_down")) {
		return prefix + "<path d=\"m6 9 6 6 6-6\"/>" + suffix;
	}
	if (p_icon == SNAME("solers_mic")) {
		return prefix + "<path d=\"M12 19v3\"/><path d=\"M19 10v2a7 7 0 0 1-14 0v-2\"/><rect x=\"9\" y=\"2\" width=\"6\" height=\"13\" rx=\"3\"/>" + suffix;
	}
	if (p_icon == SNAME("solers_new_chat")) {
		return prefix + "<path d=\"M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h8\"/><path d=\"M19 3v6\"/><path d=\"M16 6h6\"/>" + suffix;
	}
	if (p_icon == SNAME("solers_more")) {
		return prefix + "<circle cx=\"12\" cy=\"6\" r=\"1\"/><circle cx=\"12\" cy=\"12\" r=\"1\"/><circle cx=\"12\" cy=\"18\" r=\"1\"/>" + suffix;
	}

	return String();
}

static StringName solers_builtin_icon_fallback(const StringName &p_icon) {
	if (p_icon == SNAME("solers_plus")) {
		return SNAME("Add");
	}
	if (p_icon == SNAME("solers_send")) {
		return SNAME("ArrowUp");
	}
	if (p_icon == SNAME("solers_shield")) {
		return SNAME("Lock");
	}
	if (p_icon == SNAME("solers_chevron_down")) {
		return SNAME("GuiTreeArrowDown");
	}
	if (p_icon == SNAME("solers_mic")) {
		return SNAME("AudioStreamMicrophone");
	}
	if (p_icon == SNAME("solers_new_chat")) {
		return SNAME("VisualShaderNodeComment");
	}
	if (p_icon == SNAME("solers_more")) {
		return SNAME("GuiTabMenuHl");
	}
	return p_icon;
}

static Ref<Texture2D> solers_load_micro_icon(const StringName &p_icon, const Color &p_color) {
	const String svg = solers_micro_icon_svg(p_icon, p_color);
	if (svg.is_empty()) {
		return Ref<Texture2D>();
	}
	return solers_texture_from_svg(svg, EDSCALE * 0.72f);
}

static Ref<Texture2D> solers_load_logo_texture(bool p_icon_only = true) {
	Vector<String> candidates;
	const String cwd = OS::get_singleton()->get_cwd();
	const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	const String source_root = exe_dir.get_base_dir();
	const String repo_root = source_root.get_base_dir();

	candidates.push_back(repo_root.path_join("branding/generated/solers02_icon_transparent_1024.png"));
	candidates.push_back(repo_root.path_join("branding/generated/solers_splash_icon_transparent_800x600.png"));
	if (!p_icon_only) {
		candidates.push_back(repo_root.path_join("branding/generated/solers_logo_white_transparent.png"));
	}
	candidates.push_back(cwd.path_join("icon.png"));
	candidates.push_back(cwd.path_join("main/app_icon.png"));
	candidates.push_back(source_root.path_join("icon.png"));
	candidates.push_back(source_root.path_join("main/app_icon.png"));
	candidates.push_back(exe_dir.path_join("icon.png"));
	candidates.push_back(cwd.path_join("logo.png"));
	candidates.push_back(source_root.path_join("logo.png"));

	for (const String &path : candidates) {
		if (!FileAccess::exists(path)) {
			continue;
		}

		Ref<Image> image = Image::load_from_file(path);
		if (image.is_valid() && !image->is_empty()) {
			return ImageTexture::create_from_image(image);
		}
	}

	return Ref<Texture2D>();
}

Label *SolersDock::_create_section_label(const String &p_text) {
	Label *label = memnew(Label(p_text));
	label->set_theme_type_variation("HeaderSmall");
	label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	return label;
}

PanelContainer *SolersDock::_create_panel_card(const Color &p_color, const Color &p_border_color, int p_radius, int p_padding) const {
	PanelContainer *panel = memnew(PanelContainer);
	panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	panel->add_theme_style_override("panel", solers_make_stylebox(p_color, p_border_color, p_radius, p_padding));
	return panel;
}

Label *SolersDock::_create_body_label(const String &p_text, bool p_bold) const {
	Label *label = memnew(Label(p_text));
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	if (p_bold) {
		label->set_theme_type_variation("HeaderSmall");
	}
	return label;
}

Button *SolersDock::_create_chip_button(const String &p_text) const {
	Button *button = memnew(Button(p_text));
	button->set_focus_mode(Control::FOCUS_ALL);
	button->set_custom_minimum_size(Size2(0, 30 * EDSCALE));
	button->set_flat(true);
	button->set_clip_text(true);
	button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	button->add_theme_style_override("normal", solers_make_stylebox(Color(0.16, 0.16, 0.17, 1), Color(0.32, 0.32, 0.34, 0.8), 8, 8));
	button->add_theme_style_override("hover", solers_make_stylebox(Color(0.20, 0.20, 0.22, 1), Color(0.40, 0.40, 0.44, 0.9), 8, 8));
	button->add_theme_style_override("pressed", solers_make_stylebox(Color(0.12, 0.20, 0.32, 1), Color(0.10, 0.48, 0.95, 1), 8, 8));
	return button;
}

Button *SolersDock::_create_composer_select(const String &p_text, const String &p_tooltip) const {
	Button *button = memnew(Button(p_text));
	button->set_focus_mode(Control::FOCUS_ALL);
	button->set_flat(true);
	button->set_clip_text(true);
	button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	button->set_custom_minimum_size(Size2(116 * EDSCALE, 30 * EDSCALE));
	button->set_tooltip_text(p_tooltip);
	button->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);
	button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	// Trailing chevron-down glyph, mirroring the Codex model/effort selectors.
	button->set_icon_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	button->set_vertical_icon_alignment(VERTICAL_ALIGNMENT_CENTER);
	button->set_meta(SNAME("solers_icon"), SNAME("solers_chevron_down"));
	button->set_meta(SNAME("solers_primary_icon"), false);
	button->add_theme_constant_override("h_separation", 5 * EDSCALE);
	button->add_theme_constant_override("icon_max_width", 10 * EDSCALE);
	button->add_theme_style_override("normal", solers_make_stylebox(Color(0, 0, 0, 0), Color(0, 0, 0, 0), 9, 7));
	button->add_theme_style_override("hover", solers_make_stylebox(Color(1, 1, 1, 0.055), Color(0, 0, 0, 0), 9, 7));
	button->add_theme_style_override("pressed", solers_make_stylebox(Color(1, 1, 1, 0.035), Color(0, 0, 0, 0), 9, 7));
	button->add_theme_color_override(SceneStringName(font_color), Color(0.73, 0.75, 0.80, 1));
	button->add_theme_color_override("font_hover_color", Color(0.85, 0.88, 0.93, 1));
	button->add_theme_color_override("font_pressed_color", Color(0.90, 0.93, 0.98, 1));
	button->add_theme_font_size_override(SceneStringName(font_size), 12 * EDSCALE);
	return button;
}

Button *SolersDock::_create_icon_button(const StringName &p_icon, const String &p_tooltip, bool p_primary) const {
	Button *button = memnew(Button);
	button->set_focus_mode(Control::FOCUS_ALL);
	button->set_flat(false);
	button->set_clip_text(true);
	button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	button->set_custom_minimum_size(Size2(32 * EDSCALE, 32 * EDSCALE));
	button->set_tooltip_text(p_tooltip);
	button->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	button->set_vertical_icon_alignment(VERTICAL_ALIGNMENT_CENTER);
	button->set_meta(SNAME("solers_icon"), p_icon);
	button->set_meta(SNAME("solers_primary_icon"), p_primary);
	if (p_primary) {
		// Round, softly-lit send pill mirroring the Codex composer's primary action.
		button->add_theme_style_override("normal", solers_make_stylebox(Color(0.62, 0.63, 0.66, 1), Color(1, 1, 1, 0.10), 16, 0));
		button->add_theme_style_override("hover", solers_make_stylebox(Color(0.74, 0.75, 0.78, 1), Color(1, 1, 1, 0.16), 16, 0));
		button->add_theme_style_override("pressed", solers_make_stylebox(Color(0.50, 0.51, 0.55, 1), Color(1, 1, 1, 0.08), 16, 0));
		button->add_theme_style_override("focus", solers_make_stylebox(Color(0.62, 0.63, 0.66, 1), Color(1, 1, 1, 0.18), 16, 0));
		button->add_theme_color_override("icon_normal_color", Color(0.10, 0.11, 0.13, 1));
		button->add_theme_color_override("icon_hover_color", Color(0.07, 0.08, 0.10, 1));
		button->add_theme_color_override("icon_pressed_color", Color(0.14, 0.15, 0.18, 1));
	} else {
		button->set_flat(true);
		button->add_theme_style_override("normal", solers_make_stylebox(Color(0, 0, 0, 0), Color(0, 0, 0, 0), 8, 3));
		button->add_theme_style_override("hover", solers_make_stylebox(Color(1, 1, 1, 0.07), Color(0, 0, 0, 0), 8, 3));
		button->add_theme_style_override("pressed", solers_make_stylebox(Color(1, 1, 1, 0.04), Color(0, 0, 0, 0), 8, 3));
		button->add_theme_color_override("icon_normal_color", Color(0.60, 0.63, 0.69, 1));
		button->add_theme_color_override("icon_hover_color", Color(0.85, 0.88, 0.93, 1));
		button->add_theme_color_override("icon_pressed_color", Color(0.92, 0.95, 1.00, 1));
	}
	return button;
}

Control *SolersDock::_create_brand_mark() const {
	PanelContainer *mark = _create_panel_card(Color(0, 0, 0, 0), Color(0.26, 0.28, 0.34, 0.58), 8, 0);
	mark->set_custom_minimum_size(Size2(34 * EDSCALE, 34 * EDSCALE));
	mark->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);

	Ref<Texture2D> logo = solers_load_logo_texture();
	if (logo.is_valid()) {
		TextureRect *logo_rect = memnew(TextureRect);
		logo_rect->set_texture(logo);
		logo_rect->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		logo_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		logo_rect->set_custom_minimum_size(Size2(28 * EDSCALE, 28 * EDSCALE));
		logo_rect->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		logo_rect->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		mark->add_child(logo_rect);
	} else {
		Label *glyph = memnew(Label("S"));
		glyph->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		glyph->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		glyph->add_theme_color_override("font_color", Color(0.80, 0.87, 0.95, 1));
		glyph->add_theme_font_size_override(SceneStringName(font_size), 14 * EDSCALE);
		mark->add_child(glyph);
	}
	return mark;
}

Control *SolersDock::_create_empty_state() const {
	VBoxContainer *state = memnew(VBoxContainer);
	state->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	state->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	state->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	state->add_theme_constant_override("separation", 12 * EDSCALE);

	Control *top_spacer = memnew(Control);
	top_spacer->set_custom_minimum_size(Size2(0, 12 * EDSCALE));
	state->add_child(top_spacer);

	Ref<Texture2D> logo = solers_load_logo_texture();
	if (logo.is_valid()) {
		TextureRect *logo_rect = memnew(TextureRect);
		logo_rect->set_texture(logo);
		logo_rect->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		logo_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		logo_rect->set_custom_minimum_size(Size2(68 * EDSCALE, 68 * EDSCALE));
		logo_rect->set_h_size_flags(Control::SIZE_SHRINK_CENTER);
		state->add_child(logo_rect);
	} else {
		state->add_child(_create_brand_mark());
	}

	Label *headline = memnew(Label(TTR("The AI-native game engine")));
	headline->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	headline->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	headline->set_custom_minimum_size(Size2(260 * EDSCALE, 0));
	headline->set_theme_type_variation("HeaderSmall");
	headline->add_theme_color_override("font_color", Color(0.91, 0.93, 0.96, 1));
	headline->add_theme_font_size_override(SceneStringName(font_size), 20 * EDSCALE);
	state->add_child(headline);

	Label *copy = memnew(Label(TTR("Describe a world, mechanic, or workflow. Solers turns intent into scenes, systems, and playable builds.")));
	copy->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	copy->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	copy->set_custom_minimum_size(Size2(350 * EDSCALE, 0));
	copy->add_theme_color_override("font_color", Color(0.62, 0.65, 0.70, 1));
	copy->add_theme_font_size_override(SceneStringName(font_size), 13 * EDSCALE);
	state->add_child(copy);

	return state;
}

Control *SolersDock::_create_icon_badge(const String &p_text, const Color &p_color, const Color &p_border_color) const {
	PanelContainer *badge = _create_panel_card(p_color, p_border_color, 7, 0);
	badge->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);
	badge->set_custom_minimum_size(Size2(24 * EDSCALE, 24 * EDSCALE));

	Label *glyph = memnew(Label(p_text));
	glyph->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	glyph->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	glyph->set_clip_text(true);
	glyph->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	glyph->set_max_lines_visible(1);
	glyph->add_theme_color_override("font_color", Color(0.64, 0.67, 0.72, 1));
	glyph->add_theme_font_size_override(SceneStringName(font_size), 12 * EDSCALE);
	badge->add_child(glyph);

	return badge;
}

HBoxContainer *SolersDock::_create_tool_dots(int p_count, const String &p_label) const {
	HBoxContainer *row = memnew(HBoxContainer);
	row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	row->add_theme_constant_override("separation", 6 * EDSCALE);

	for (int i = 0; i < p_count; i++) {
		row->add_child(_create_icon_badge("*", Color(0.12, 0.13, 0.15, 1), Color(0.25, 0.26, 0.30, 1)));
	}

	if (!p_label.is_empty()) {
		PanelContainer *chip = _create_panel_card(Color(0.12, 0.13, 0.15, 1), Color(0.25, 0.26, 0.30, 1), 7, 7);
		chip->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);
		chip->set_custom_minimum_size(Size2(72 * EDSCALE, 24 * EDSCALE));
		Label *label = memnew(Label(p_label));
		label->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
		label->set_clip_text(true);
		label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		label->set_max_lines_visible(1);
		label->add_theme_color_override("font_color", Color(0.64, 0.66, 0.70, 1));
		label->add_theme_font_size_override(SceneStringName(font_size), 13 * EDSCALE);
		chip->add_child(label);
		row->add_child(chip);
	}

	return row;
}

void SolersDock::_refresh_status() {
	if (rml_chat_surface) {
		return;
	}

	if (!observation_service) {
		const String project_name = GLOBAL_GET("application/config/name");
		project_status_label->set_text(project_name.is_empty() ? TTR("No named project loaded") : vformat(TTR("Project: %s"), project_name));
		runtime_status_label->set_text(TTR("Runtime: idle"));
		return;
	}

	Dictionary snapshot = observation_service->get_editor_snapshot(2, 16);
	Dictionary project = snapshot.get("project", Dictionary());
	Dictionary open_scenes = snapshot.get("open_scenes", Dictionary());
	Dictionary scene_tree = snapshot.get("scene_tree", Dictionary());
	Dictionary selection = snapshot.get("selection", Dictionary());
	Dictionary runtime = snapshot.get("runtime", Dictionary());

	const String project_name = project.get("name", String());
	const String resource_path = project.get("resource_path", String());
	project_status_label->set_text(project_name.is_empty() ? vformat(TTR("Project path: %s"), resource_path) : vformat(TTR("Project: %s"), project_name));

	const bool is_playing = runtime.get("is_playing", false);
	const String playing_scene = runtime.get("playing_scene", String());
	runtime_status_label->set_text(is_playing ? vformat(TTR("Runtime: playing %s"), playing_scene) : TTR("Runtime: idle"));

	const bool has_scene = scene_tree.get("has_edited_scene", false);
	const String current_scene_path = open_scenes.get("current_scene_path", String());
	const int open_scene_count = open_scenes.get("count", 0);
	scene_status_label->set_text(has_scene ? vformat(TTR("Scene: %s (%d open)"), current_scene_path.is_empty() ? TTR("unsaved") : current_scene_path, open_scene_count) : TTR("Scene: no edited scene"));

	const int selected_count = selection.get("count", 0);
	selection_status_label->set_text(vformat(TTR("Selection: %d node(s)"), selected_count));

	const bool scene_mutations_enabled = permission_manager ? permission_manager->get_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_SCENE) : false;
	const bool file_saves_enabled = permission_manager ? permission_manager->get_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES) : false;
	const bool run_project_enabled = permission_manager ? permission_manager->get_auto_approve_permission(SolersPermissionManager::PERMISSION_RUN_PROJECT) : false;
	tool_status_label->set_text(tool_registry ? vformat(TTR("Tools: %d registered, scene edits %s, file writes %s, run %s"), tool_registry->get_tool_count(), scene_mutations_enabled ? TTR("on") : TTR("locked"), file_saves_enabled ? TTR("on") : TTR("locked"), run_project_enabled ? TTR("on") : TTR("locked")) : TTR("Tools: unavailable"));

	Dictionary agent_status = agent_runtime ? agent_runtime->get_status() : Dictionary();
	agent_status_label->set_text(agent_runtime ? vformat(TTR("Agent: %s (turn %d)"), String(agent_status.get("state", "unknown")), (int)agent_status.get("turn_id", 0)) : TTR("Agent: unavailable"));

	Dictionary protocol_status = mcp_adapter ? mcp_adapter->get_status() : Dictionary();
	Dictionary rpc_status = rpc_server ? rpc_server->get_status() : Dictionary();
	if (mcp_adapter && rpc_server) {
		const bool rpc_running = rpc_status.get("running", false);
		protocol_status_label->set_text(rpc_running ? vformat(TTR("Protocol: MCP adapter ready, RPC 127.0.0.1:%d (%d client(s)), %d tool(s)"), (int)rpc_status.get("port", 0), (int)rpc_status.get("clients", 0), (int)protocol_status.get("tools_available", 0)) : vformat(TTR("Protocol: MCP adapter ready, RPC stopped, %d tool(s)"), (int)protocol_status.get("tools_available", 0)));
	} else {
		protocol_status_label->set_text(TTR("Protocol: unavailable"));
	}

	if (settings_service && provider_status_label) {
		Dictionary provider_result = settings_service->get_provider_config();
		Dictionary provider_data = provider_result.get("data", Dictionary());
		Dictionary validation = provider_data.get("validation", Dictionary());
		const String provider = provider_data.get("provider", String("unknown"));
		const bool privacy_mode = provider_data.get("privacy_mode", true);
		const bool api_key_configured = provider_data.get("api_key_configured", false);
		const bool valid = validation.get("valid", false);
		Array warnings = validation.get("warnings", Array());
		Array blockers = validation.get("blockers", Array());
		provider_status_label->set_text(vformat(TTR("Provider: %s, privacy %s, key %s, config %s (%d warning(s), %d blocker(s))"), provider, privacy_mode ? TTR("local-only") : TTR("network-enabled"), api_key_configured ? TTR("set") : TTR("not set"), valid ? TTR("valid") : TTR("blocked"), warnings.size(), blockers.size()));
	} else if (provider_status_label) {
		provider_status_label->set_text(TTR("Provider: unavailable"));
	}

	if (rmlui_status_label) {
		const bool rmlui_available = Engine::get_singleton() && Engine::get_singleton()->has_singleton("RMLServer");
		rmlui_status_label->set_text(rmlui_available ? TTR("RmlUi: runtime bridge active") : TTR("RmlUi: runtime bridge missing, native fallback active"));
	}

	const int pending_approval_count = permission_manager ? permission_manager->get_pending_request_count() : 0;
	approval_status_label->set_text(permission_manager ? vformat(TTR("Approvals: %d pending"), pending_approval_count) : TTR("Approvals: unavailable"));

	timeline_status_label->set_text(action_timeline ? vformat(TTR("Timeline: %d event(s)"), action_timeline->get_action_count()) : TTR("Timeline: unavailable"));

	if (snapshot_preview) {
		Array tools = tool_registry ? tool_registry->list_tools() : Array();
		String preview = vformat(TTR("Snapshot ready. Open scenes: %d, selected nodes: %d, tools: %d."), open_scene_count, selected_count, tools.size());
		snapshot_preview->set_text(preview);
	}
}

void SolersDock::_on_refresh_pressed() {
	if (tool_registry) {
		Dictionary args;
		args["max_scene_depth"] = 2;
		args["max_children_per_node"] = 16;
		tool_registry->call_tool("editor.get_snapshot", args);
	}
	_refresh_status();
}

void SolersDock::_on_run_loopback_probe_pressed() {
	if (!agent_runtime) {
		return;
	}

	Array tool_calls;
	Dictionary snapshot_call;
	snapshot_call["name"] = "editor.get_snapshot";
	Dictionary snapshot_args;
	snapshot_args["max_scene_depth"] = 2;
	snapshot_args["max_children_per_node"] = 16;
	snapshot_call["arguments"] = snapshot_args;
	tool_calls.push_back(snapshot_call);

	Dictionary timeline_call;
	timeline_call["name"] = "timeline.list_actions";
	Dictionary timeline_args;
	timeline_args["limit"] = 20;
	timeline_call["arguments"] = timeline_args;
	tool_calls.push_back(timeline_call);

	Dictionary turn;
	turn["objective"] = "Solers v0.1 internal loopback probe";
	turn["mode"] = "tool_batch";
	turn["tool_calls"] = tool_calls;
	Dictionary result = agent_runtime->start_turn(turn);
	if (snapshot_preview) {
		snapshot_preview->set_text(JSON::stringify(result, "\t", false, true));
	}
	_refresh_status();
}

void SolersDock::_append_chat_message(const String &p_speaker, const String &p_message) {
	chat_log += vformat("%s%s\n%s\n", chat_log.is_empty() ? "" : "\n", p_speaker, p_message);
	if (rml_chat_surface) {
		rml_chat_surface->append_message(p_speaker, p_message);
		return;
	}
	if (!message_list) {
		return;
	}
	if (empty_state) {
		empty_state->queue_free();
		empty_state = nullptr;
	}

	const bool is_user = p_speaker == "You";
	HBoxContainer *row = memnew(HBoxContainer);
	row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	row->add_theme_constant_override("separation", 8 * EDSCALE);
	message_list->add_child(row);

	if (is_user) {
		Control *spacer = memnew(Control);
		spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		row->add_child(spacer);
	}

	PanelContainer *card = _create_panel_card(is_user ? Color(0.05, 0.17, 0.29, 1) : Color(0, 0, 0, 0), is_user ? Color(0.08, 0.42, 0.78, 0.52) : Color(0, 0, 0, 0), is_user ? 18 : 10, is_user ? 12 : 4);
	card->set_h_size_flags(is_user ? Control::SIZE_SHRINK_END : Control::SIZE_EXPAND_FILL);
	card->set_custom_minimum_size(Size2(is_user ? 220 * EDSCALE : 260 * EDSCALE, 0));
	row->add_child(card);

	VBoxContainer *card_body = memnew(VBoxContainer);
	card_body->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	card_body->add_theme_constant_override("separation", 6 * EDSCALE);
	card->add_child(card_body);

	if (!is_user) {
		HBoxContainer *kicker = _create_tool_dots(1, p_speaker);
		card_body->add_child(kicker);
	}

	Label *body = _create_body_label(p_message);
	body->set_custom_minimum_size(Size2(is_user ? 180 * EDSCALE : 220 * EDSCALE, 0));
	body->add_theme_color_override("font_color", is_user ? Color(0.86, 0.93, 1.0, 1) : Color(0.85, 0.86, 0.88, 1));
	body->add_theme_font_size_override(SceneStringName(font_size), is_user ? 15 * EDSCALE : 14 * EDSCALE);
	card_body->add_child(body);

	if (chat_scroll) {
		chat_scroll->queue_redraw();
	}
}

void SolersDock::_append_timeline_event(const Dictionary &p_event) {
	const String kind = p_event.get("kind", String());
	if (kind == "user") {
		_append_chat_message("You", p_event.get("text", String()));
		Label *meta = _create_body_label(p_event.get("meta", String()), false);
		meta->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
		meta->add_theme_color_override("font_color", Color(0.50, 0.51, 0.55, 1));
		message_list->add_child(meta);
		return;
	}

	if (kind == "output") {
		PanelContainer *card = _create_panel_card(Color(0.13, 0.14, 0.15, 1), Color(0.18, 0.19, 0.21, 1), 14, 12);
		card->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		message_list->add_child(card);

		HBoxContainer *row = memnew(HBoxContainer);
		row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		row->add_theme_constant_override("separation", 12 * EDSCALE);
		card->add_child(row);

		PanelContainer *icon = _create_panel_card(Color(0.06, 0.11, 0.18, 1), Color(0.02, 0.47, 0.95, 1), 10, 0);
		icon->set_custom_minimum_size(Size2(38 * EDSCALE, 38 * EDSCALE));
		Label *icon_label = memnew(Label("#"));
		icon_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		icon_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		icon_label->set_clip_text(true);
		icon_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		icon_label->set_max_lines_visible(1);
		icon_label->add_theme_color_override("font_color", Color(0.19, 0.58, 1.0, 1));
		icon->add_child(icon_label);
		row->add_child(icon);

		VBoxContainer *copy = memnew(VBoxContainer);
		copy->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		copy->add_theme_constant_override("separation", 2 * EDSCALE);
		row->add_child(copy);

		Label *output_title = _create_body_label(p_event.get("title", String()), true);
		output_title->add_theme_color_override("font_color", Color(0.91, 0.92, 0.94, 1));
		copy->add_child(output_title);

		Label *subtitle = _create_body_label(p_event.get("subtitle", String()), false);
		subtitle->add_theme_color_override("font_color", Color(0.59, 0.61, 0.66, 1));
		copy->add_child(subtitle);

		Button *open = _create_chip_button(p_event.get("action_label", String("Open")));
		open->set_custom_minimum_size(Size2(70 * EDSCALE, 38 * EDSCALE));
		open->set_h_size_flags(Control::SIZE_SHRINK_END);
		row->add_child(open);
		return;
	}

	HBoxContainer *kicker = _create_tool_dots(1, p_event.get("kicker", String()));
	message_list->add_child(kicker);

	const bool strong = p_event.get("strong", false);
	Label *body = _create_body_label(p_event.get("text", String()), strong);
	body->set_custom_minimum_size(Size2(260 * EDSCALE, 0));
	body->add_theme_color_override("font_color", strong ? Color(0.82, 0.83, 0.86, 1) : Color(0.84, 0.85, 0.88, 1));
	body->add_theme_font_size_override(SceneStringName(font_size), 14 * EDSCALE);
	message_list->add_child(body);

	const String action_label = p_event.get("action_label", String());
	if (!action_label.is_empty()) {
		message_list->add_child(_create_tool_dots(strong ? 7 : 2, action_label));
	}
}

void SolersDock::_populate_initial_timeline() {
	if (message_list) {
		empty_state = _create_empty_state();
		message_list->add_child(empty_state);
	}
}

void SolersDock::_append_orchestrator_result(const Dictionary &p_result) {
	if (!(bool)p_result.get("ok", false)) {
		Dictionary error = p_result.get("error", Dictionary());
		_append_chat_message("Solers", vformat("I could not start the agent turn.\n%s", String(error.get("message", "Unknown error."))));
		return;
	}

	Dictionary result_data = p_result.get("data", Dictionary());
	Array phases = result_data.get("phases", Array());
	String phase_text;
	for (int i = 0; i < phases.size(); i++) {
		phase_text += vformat("%s%s", i == 0 ? "" : " -> ", String(phases[i]));
	}

	const String state = result_data.get("state", "unknown");
	String message = vformat("State: %s\nFlow: %s", state, phase_text);
	if (result_data.has("executor_result")) {
		Dictionary executor = result_data["executor_result"];
		Array tool_results = executor.get("tool_results", Array());
		message += vformat("\nTool actions: %d", tool_results.size());
		if (executor.has("error")) {
			Dictionary error = executor["error"];
			message += vformat("\nExecutor blocked: %s", String(error.get("message", String())));
		}
	}
	_append_chat_message("Solers", message);
}

void SolersDock::_on_send_chat_pressed() {
	if (!chat_input) {
		return;
	}

	const String prompt = chat_input->get_text().strip_edges();
	if (prompt.is_empty()) {
		return;
	}
	chat_input->set_text("");
	_update_chat_input_height();
	_submit_chat_prompt(prompt);
}

void SolersDock::_on_rml_prompt_submitted(const String &p_prompt) {
	_submit_chat_prompt(p_prompt);
}

void SolersDock::_submit_chat_prompt(const String &p_prompt) {
	const String prompt = p_prompt.strip_edges();
	if (prompt.is_empty()) {
		return;
	}

	_append_chat_message("You", prompt);

	if (!mcp_adapter) {
		_append_chat_message("Solers", "MCP adapter is unavailable, so I cannot start an agent session yet.");
		return;
	}

	Array messages;
	Dictionary user_message;
	user_message["role"] = "user";
	user_message["content"] = prompt;
	messages.push_back(user_message);

	Dictionary params;
	params["provider"] = "mock";
	params["model"] = "solers-chat-session-v0";
	params["objective"] = prompt;
	params["messages"] = messages;

	Dictionary request;
	request["jsonrpc"] = "2.0";
	request["id"] = 1;
	request["method"] = "solers/agent/orchestrate";
	request["params"] = params;

	Dictionary response = mcp_adapter->handle_request(request);
	if (response.has("error")) {
		Dictionary error = response["error"];
		_append_chat_message("Solers", String(error.get("message", "Unknown JSON-RPC error.")));
		return;
	}
	_append_orchestrator_result(response.get("result", Dictionary()));
	_refresh_status();
}

void SolersDock::_on_chat_input_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || key->is_echo()) {
		return;
	}

	const Key keycode = key->get_keycode();
	if (keycode != Key::ENTER && keycode != Key::KP_ENTER) {
		return;
	}

	if (key->is_shift_pressed()) {
		chat_input->insert_text_at_caret("\n");
		_update_chat_input_height();
		chat_input->accept_event();
		return;
	}

	_on_send_chat_pressed();
	chat_input->accept_event();
}

void SolersDock::_on_chat_input_text_changed() {
	_update_chat_input_height();
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

void SolersDock::_on_abort_agent_pressed() {
	if (agent_runtime) {
		agent_runtime->abort_current_turn();
	}
	_refresh_status();
}

void SolersDock::_on_approve_next_pressed() {
	if (!permission_manager) {
		return;
	}
	Array pending = permission_manager->list_pending_requests();
	if (pending.is_empty()) {
		_refresh_status();
		return;
	}
	Dictionary request = pending[0];
	permission_manager->approve_request(request.get("id", 0));
	if (snapshot_preview) {
		snapshot_preview->set_text(vformat(TTR("Approved Solers request %d for one retry."), (int)request.get("id", 0)));
	}
	_refresh_status();
}

void SolersDock::_on_reject_next_pressed() {
	if (!permission_manager) {
		return;
	}
	Array pending = permission_manager->list_pending_requests();
	if (pending.is_empty()) {
		_refresh_status();
		return;
	}
	Dictionary request = pending[0];
	permission_manager->reject_request(request.get("id", 0));
	if (snapshot_preview) {
		snapshot_preview->set_text(vformat(TTR("Rejected Solers request %d."), (int)request.get("id", 0)));
	}
	_refresh_status();
}

void SolersDock::_on_allow_scene_mutations_toggled(bool p_enabled) {
	if (permission_manager) {
		permission_manager->set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_SCENE, p_enabled);
	}
	_refresh_status();
}

void SolersDock::_on_allow_file_saves_toggled(bool p_enabled) {
	if (permission_manager) {
		permission_manager->set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, p_enabled);
	}
	_refresh_status();
}

void SolersDock::_on_allow_run_project_toggled(bool p_enabled) {
	if (permission_manager) {
		permission_manager->set_auto_approve_permission(SolersPermissionManager::PERMISSION_RUN_PROJECT, p_enabled);
	}
	_refresh_status();
}

void SolersDock::_refresh_icon_buttons() {
	Button *buttons[] = {
		add_context_button,
		access_status_button,
		model_select_button,
		effort_select_button,
		send_chat_button,
		new_chat_button,
		more_button,
	};

	for (Button *button : buttons) {
		if (!button || !button->has_meta(SNAME("solers_icon"))) {
			continue;
		}

		const StringName solers_icon_name = button->get_meta(SNAME("solers_icon"));
		const bool primary = button->has_meta(SNAME("solers_primary_icon")) && bool(button->get_meta(SNAME("solers_primary_icon")));
		Color icon_color = primary ? Color(0.10, 0.11, 0.13, 1) : Color(0.62, 0.65, 0.71, 1);
		if (button->has_meta(SNAME("solers_icon_color"))) {
			icon_color = button->get_meta(SNAME("solers_icon_color"));
		}
		Ref<Texture2D> icon = solers_load_micro_icon(solers_icon_name, icon_color);
		if (icon.is_null()) {
			icon = get_editor_theme_icon(solers_builtin_icon_fallback(solers_icon_name));
		}
		button->set_button_icon(icon);
	}
}

void SolersDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_refresh_icon_buttons();
			_update_chat_input_height();
			_refresh_status();
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			_refresh_icon_buttons();
			_update_chat_input_height();
		} break;
		case NOTIFICATION_TRANSLATION_CHANGED: {
			_refresh_status();
		} break;
	}
}

void SolersDock::set_services(SolersObservationService *p_observation_service, SolersToolRegistry *p_tool_registry, SolersActionTimeline *p_action_timeline, SolersPermissionManager *p_permission_manager, SolersAgentRuntime *p_agent_runtime, SolersMCPAdapter *p_mcp_adapter, SolersRpcServer *p_rpc_server, SolersSettingsService *p_settings_service) {
	observation_service = p_observation_service;
	tool_registry = p_tool_registry;
	action_timeline = p_action_timeline;
	permission_manager = p_permission_manager;
	agent_runtime = p_agent_runtime;
	mcp_adapter = p_mcp_adapter;
	rpc_server = p_rpc_server;
	settings_service = p_settings_service;
	_refresh_status();
}

void SolersDock::make_visible() {
	_refresh_status();
}

SolersDock::SolersDock() {
	set_name(TTRC("Solers"));
	set_custom_minimum_size(Size2(520 * EDSCALE, 0));
	set_h_size_flags(Control::SIZE_FILL);
	set_v_size_flags(Control::SIZE_EXPAND_FILL);
	add_theme_style_override("panel", solers_make_stylebox(Color(0.070, 0.073, 0.078, 1), Color(0.16, 0.16, 0.17, 1), 0, 0));

#ifdef SOLERS_RMLUI_ENABLED
	rml_chat_surface = memnew(SolersRmlChatSurface);
	rml_chat_surface->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	rml_chat_surface->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	rml_chat_surface->connect(SNAME("prompt_submitted"), callable_mp(this, &SolersDock::_on_rml_prompt_submitted));
	add_child(rml_chat_surface);
	return;
#endif

	PanelContainer *surface = _create_panel_card(Color(0.070, 0.073, 0.078, 1), Color(0, 0, 0, 0), 0, 0);
	surface->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	surface->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	add_child(surface);

	VBoxContainer *root = memnew(VBoxContainer);
	root->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	root->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	root->add_theme_constant_override("separation", 0);
	surface->add_child(root);

	MarginContainer *topbar_inset = memnew(MarginContainer);
	topbar_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_inset->set_custom_minimum_size(Size2(0, 40 * EDSCALE));
	topbar_inset->add_theme_constant_override("margin_left", 12 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_right", 12 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_top", 5 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_bottom", 5 * EDSCALE);
	root->add_child(topbar_inset);

	HBoxContainer *topbar_content = memnew(HBoxContainer);
	topbar_content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_content->add_theme_constant_override("separation", 6 * EDSCALE);
	topbar_inset->add_child(topbar_content);

	Control *topbar_spacer = memnew(Control);
	topbar_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_content->add_child(topbar_spacer);

	new_chat_button = _create_icon_button(SNAME("solers_new_chat"), TTR("New chat"));
	new_chat_button->set_custom_minimum_size(Size2(30 * EDSCALE, 30 * EDSCALE));
	topbar_content->add_child(new_chat_button);

	more_button = _create_icon_button(SNAME("solers_more"), TTR("More"));
	more_button->set_custom_minimum_size(Size2(30 * EDSCALE, 30 * EDSCALE));
	topbar_content->add_child(more_button);

	project_status_label = memnew(Label);
	project_status_label->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
	project_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	project_status_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	project_status_label->set_visible(false);
	root->add_child(project_status_label);

	runtime_status_label = memnew(Label);
	runtime_status_label->set_visible(false);
	runtime_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	root->add_child(runtime_status_label);

	agent_status_label = memnew(Label);
	agent_status_label->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
	agent_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	agent_status_label->set_visible(false);
	root->add_child(agent_status_label);

	provider_status_label = memnew(Label);
	provider_status_label->set_visible(false);
	provider_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	root->add_child(provider_status_label);

	rmlui_status_label = memnew(Label);
	rmlui_status_label->set_visible(false);
	rmlui_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	root->add_child(rmlui_status_label);

	chat_scroll = memnew(ScrollContainer);
	chat_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	chat_scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	root->add_child(chat_scroll);

	message_list = memnew(VBoxContainer);
	message_list->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	message_list->add_theme_constant_override("separation", 16 * EDSCALE);

	MarginContainer *timeline_inset = memnew(MarginContainer);
	timeline_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	timeline_inset->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	timeline_inset->add_theme_constant_override("margin_left", 24 * EDSCALE);
	timeline_inset->add_theme_constant_override("margin_right", 24 * EDSCALE);
	timeline_inset->add_theme_constant_override("margin_top", 10 * EDSCALE);
	timeline_inset->add_theme_constant_override("margin_bottom", 12 * EDSCALE);
	timeline_inset->add_child(message_list);
	chat_scroll->add_child(timeline_inset);

	_populate_initial_timeline();

	MarginContainer *composer_inset = memnew(MarginContainer);
	composer_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_inset->add_theme_constant_override("margin_left", 18 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_right", 18 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_top", 6 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_bottom", 18 * EDSCALE);
	root->add_child(composer_inset);

	PanelContainer *composer_card = memnew(PanelContainer);
	composer_card->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	// ChatGPT-style floating composer: one rounded surface owns both text entry and actions.
	composer_card->add_theme_style_override("panel", solers_make_stylebox(Color(0.095, 0.096, 0.100, 1.0), Color(0, 0, 0, 0), 20, 12, true));
	composer_card->set_custom_minimum_size(Size2(0, (SOLERS_COMPOSER_TEXT_MIN_HEIGHT + SOLERS_COMPOSER_TOOLBAR_HEIGHT + SOLERS_COMPOSER_VERTICAL_CHROME) * EDSCALE));
	composer_inset->add_child(composer_card);

	VBoxContainer *composer = memnew(VBoxContainer);
	composer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer->add_theme_constant_override("separation", 2 * EDSCALE);
	composer_card->add_child(composer);

	chat_input = memnew(TextEdit);
	chat_input->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	chat_input->set_custom_minimum_size(Size2(0, SOLERS_COMPOSER_TEXT_MIN_HEIGHT * EDSCALE));
	chat_input->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	chat_input->set_placeholder(TTR("Ask for follow-up changes"));
	chat_input->set_smooth_scroll_enabled(true);
	chat_input->set_scroll_past_end_of_file_enabled(false);
	chat_input->set_fit_content_height_enabled(false);
	chat_input->set_indent_wrapped_lines(false);
	chat_input->set_highlight_current_line(false);
	chat_input->set_draw_minimap(false);
	chat_input->add_theme_style_override("normal", memnew(StyleBoxEmpty));
	chat_input->add_theme_style_override("focus", memnew(StyleBoxEmpty));
	chat_input->add_theme_style_override("read_only", memnew(StyleBoxEmpty));
	chat_input->add_theme_color_override("font_color", Color(0.89, 0.91, 0.94, 1));
	chat_input->add_theme_color_override("font_placeholder_color", Color(0.40, 0.41, 0.44, 1));
	chat_input->add_theme_color_override("background_color", Color(0, 0, 0, 0));
	chat_input->add_theme_color_override("caret_color", Color(0.86, 0.91, 0.98, 1));
	chat_input->add_theme_color_override("selection_color", Color(0.10, 0.42, 0.62, 0.56));
	chat_input->add_theme_constant_override("line_spacing", 5 * EDSCALE);
	chat_input->add_theme_font_size_override(SceneStringName(font_size), 14 * EDSCALE);
	chat_input->connect(SceneStringName(gui_input), callable_mp(this, &SolersDock::_on_chat_input_gui_input));
	chat_input->connect(SceneStringName(text_changed), callable_mp(this, &SolersDock::_on_chat_input_text_changed));
	composer->add_child(chat_input);

	HBoxContainer *composer_toolbar = memnew(HBoxContainer);
	composer_toolbar->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_toolbar->set_custom_minimum_size(Size2(0, SOLERS_COMPOSER_TOOLBAR_HEIGHT * EDSCALE));
	composer_toolbar->set_alignment(BoxContainer::ALIGNMENT_BEGIN);
	composer_toolbar->add_theme_constant_override("separation", 6 * EDSCALE);
	composer->add_child(composer_toolbar);

	add_context_button = _create_icon_button(SNAME("solers_plus"), TTR("Attach context"));
	add_context_button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	composer_toolbar->add_child(add_context_button);

	access_status_button = _create_icon_button(SNAME("solers_shield"), TTR("Agent access"));
	access_status_button->set_custom_minimum_size(Size2(24 * EDSCALE, 30 * EDSCALE));
	access_status_button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	access_status_button->set_meta(SNAME("solers_icon_color"), Color(1.00, 0.49, 0.20, 1));
	composer_toolbar->add_child(access_status_button);

	model_select_button = _create_composer_select(TTR("Full access"), TTR("Agent access"));
	model_select_button->set_custom_minimum_size(Size2(92 * EDSCALE, 30 * EDSCALE));
	model_select_button->set_meta(SNAME("solers_icon_color"), Color(1.00, 0.49, 0.20, 1));
	model_select_button->add_theme_color_override(SceneStringName(font_color), Color(1.00, 0.49, 0.20, 1));
	model_select_button->add_theme_color_override("font_hover_color", Color(1.00, 0.62, 0.36, 1));
	model_select_button->add_theme_color_override("font_pressed_color", Color(1.00, 0.55, 0.28, 1));
	composer_toolbar->add_child(model_select_button);

	Control *toolbar_spacer = memnew(Control);
	toolbar_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_toolbar->add_child(toolbar_spacer);

	effort_select_button = _create_composer_select(TTR("5.5  Extra High"), TTR("Model and effort"));
	effort_select_button->set_custom_minimum_size(Size2(126 * EDSCALE, 30 * EDSCALE));
	composer_toolbar->add_child(effort_select_button);

	send_chat_button = _create_icon_button(SNAME("solers_send"), TTR("Send"), true);
	send_chat_button->set_focus_mode(Control::FOCUS_ALL);
	send_chat_button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	send_chat_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_send_chat_pressed));
	composer_toolbar->add_child(send_chat_button);

	_update_chat_input_height();

	HSeparator *debug_separator = memnew(HSeparator);
	debug_separator->set_visible(false);
	root->add_child(debug_separator);

	Label *debug_title = _create_section_label(TTR("Tools & approvals"));
	debug_title->set_visible(false);
	root->add_child(debug_title);

	ScrollContainer *debug_scroll = memnew(ScrollContainer);
	debug_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	debug_scroll->set_custom_minimum_size(Size2(0, 220 * EDSCALE));
	debug_scroll->set_visible(false);
	root->add_child(debug_scroll);

	debug_panel = memnew(VBoxContainer);
	debug_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	debug_panel->add_theme_constant_override("separation", 6 * EDSCALE);
	debug_scroll->add_child(debug_panel);

	scene_status_label = memnew(Label);
	scene_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	scene_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(scene_status_label);

	selection_status_label = memnew(Label);
	selection_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	selection_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(selection_status_label);

	tool_status_label = memnew(Label);
	tool_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	tool_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(tool_status_label);

	timeline_status_label = memnew(Label);
	timeline_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	timeline_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(timeline_status_label);

	protocol_status_label = memnew(Label);
	protocol_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	protocol_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(protocol_status_label);

	approval_status_label = memnew(Label);
	approval_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	approval_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(approval_status_label);

	Button *refresh_button = memnew(Button(TTR("Refresh Snapshot")));
	refresh_button->set_focus_mode(Control::FOCUS_ALL);
	refresh_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_refresh_pressed));
	debug_panel->add_child(refresh_button);

	Button *loopback_probe_button = memnew(Button(TTR("Run Runtime Probe")));
	loopback_probe_button->set_focus_mode(Control::FOCUS_ALL);
	loopback_probe_button->set_tooltip_text(TTR("Run a local Solers Agent Runtime tool batch without contacting any model provider."));
	loopback_probe_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_run_loopback_probe_pressed));
	debug_panel->add_child(loopback_probe_button);

	Button *abort_button = memnew(Button(TTR("Abort Agent Turn")));
	abort_button->set_focus_mode(Control::FOCUS_ALL);
	abort_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_abort_agent_pressed));
	debug_panel->add_child(abort_button);

	HBoxContainer *approval_buttons = memnew(HBoxContainer);
	approval_buttons->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	debug_panel->add_child(approval_buttons);

	Button *approve_next_button = memnew(Button(TTR("Approve Next")));
	approve_next_button->set_focus_mode(Control::FOCUS_ALL);
	approve_next_button->set_tooltip_text(TTR("Approve the oldest pending Solers tool request for one retry."));
	approve_next_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_approve_next_pressed));
	approval_buttons->add_child(approve_next_button);

	Button *reject_next_button = memnew(Button(TTR("Reject Next")));
	reject_next_button->set_focus_mode(Control::FOCUS_ALL);
	reject_next_button->set_tooltip_text(TTR("Reject the oldest pending Solers tool request."));
	reject_next_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_reject_next_pressed));
	approval_buttons->add_child(reject_next_button);

	allow_scene_mutation_toggle = memnew(CheckButton(TTR("Allow Scene Mutations")));
	allow_scene_mutation_toggle->set_pressed(false);
	allow_scene_mutation_toggle->set_tooltip_text(TTR("Temporarily allow Solers edit_scene tools such as node.add and node.set_properties."));
	allow_scene_mutation_toggle->connect(SceneStringName(toggled), callable_mp(this, &SolersDock::_on_allow_scene_mutations_toggled));
	debug_panel->add_child(allow_scene_mutation_toggle);

	allow_file_save_toggle = memnew(CheckButton(TTR("Allow File Writes")));
	allow_file_save_toggle->set_pressed(false);
	allow_file_save_toggle->set_tooltip_text(TTR("Temporarily allow Solers edit_files tools such as scene.save and script.write."));
	allow_file_save_toggle->connect(SceneStringName(toggled), callable_mp(this, &SolersDock::_on_allow_file_saves_toggled));
	debug_panel->add_child(allow_file_save_toggle);

	allow_run_project_toggle = memnew(CheckButton(TTR("Allow Run Controls")));
	allow_run_project_toggle->set_pressed(false);
	allow_run_project_toggle->set_tooltip_text(TTR("Temporarily allow Solers run_project tools such as runtime.play_current_scene and runtime.stop."));
	allow_run_project_toggle->connect(SceneStringName(toggled), callable_mp(this, &SolersDock::_on_allow_run_project_toggled));
	debug_panel->add_child(allow_run_project_toggle);

	debug_panel->add_child(_create_section_label(TTR("v0.1 Runtime Spine")));

	RichTextLabel *spine = memnew(RichTextLabel);
	spine->set_fit_content(true);
	spine->set_scroll_active(false);
	spine->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	spine->set_text(TTR("- Observation service\n- Tool registry\n- Permission policy\n- File checkpoints\n- Script validation\n- MCP adapter\n- JSONL RPC loopback server\n- Agent tool loop\n- Action timeline"));
	debug_panel->add_child(spine);

	debug_panel->add_child(_create_section_label(TTR("Observation Preview")));

	snapshot_preview = memnew(RichTextLabel);
	snapshot_preview->set_fit_content(true);
	snapshot_preview->set_scroll_active(false);
	snapshot_preview->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	snapshot_preview->set_text(TTR("Snapshot not captured yet."));
	debug_panel->add_child(snapshot_preview);
}
