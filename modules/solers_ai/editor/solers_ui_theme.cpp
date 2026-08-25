/**************************************************************************/
/*  solers_ui_theme.cpp                                                   */
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

#include "solers_ui_theme.h"

#include "core/os/os.h"
#include "core/string/translation_server.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/themes/builtin_fonts.gen.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/dialogs.h"
#include "scene/resources/font.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/style_box_line.h"

#include "modules/solers_ai/editor/solers_chat_widgets.h"

static void _solers_tune(const Ref<StyleBoxFlat> &p_sb, int p_radius_px) {
	if (p_sb.is_null()) {
		return;
	}
	p_sb->set_anti_aliased(true);
	p_sb->set_aa_size(0.65f); // Tighter feather than the 1.0 default.
	if (p_radius_px > 0) {
		// ~2 segments per pixel of radius keeps the arc smooth at any DPI.
		p_sb->set_corner_detail(CLAMP(p_radius_px * 2, 10, 24));
	}
}

// File-local: build a flat stylebox with the given (already DPI-scaled) metrics.
// A negative margin means "leave content margin untouched".
static Ref<StyleBoxFlat> _solers_flat(const Color &p_bg, int p_radius_px, const Color &p_border, int p_border_px, float p_margin_px) {
	Ref<StyleBoxFlat> sb;
	sb.instantiate();
	sb->set_bg_color(p_bg);
	if (p_radius_px > 0) {
		sb->set_corner_radius_all(p_radius_px);
	}
	if (p_border_px > 0 && p_border.a > 0.0f) {
		sb->set_border_color(p_border);
		sb->set_border_width_all(p_border_px);
		sb->set_border_blend(true); // Blend border→bg so the corner miter never spikes bright.
	}
	if (p_margin_px >= 0.0f) {
		sb->set_content_margin_all(p_margin_px);
	}
	_solers_tune(sb, p_radius_px);
	return sb;
}

struct SolersEmbeddedFont {
	const uint8_t *data;
	size_t size;
};

static Ref<FontFile> _solers_font(const SolersEmbeddedFont &p_source, TypedArray<Font> *r_fallbacks = nullptr) {
	Ref<FontFile> font;
	font.instantiate();
	font->set_data_ptr(p_source.data, p_source.size);
	font->set_force_autohinter(true);
	if (r_fallbacks) {
		r_fallbacks->push_back(font);
	}
	return font;
}

static PackedStringArray _solers_cjk_names() {
	String locale = TranslationServer::get_singleton()->get_tool_locale();
	if (!locale.begins_with("zh") && !locale.begins_with("ja") && !locale.begins_with("ko")) {
		locale = OS::get_singleton()->get_locale();
	}
	PackedStringArray suffixes;
	if (locale.begins_with("zh") && (locale.contains("Hans") || locale.contains("CN") || locale.contains("SG"))) {
		suffixes = { "SC", "TC", "HK", "JP", "KR" };
	} else if (locale.begins_with("zh") && locale.contains("HK")) {
		suffixes = { "HK", "TC", "SC", "JP", "KR" };
	} else if (locale.begins_with("zh")) {
		suffixes = { "TC", "HK", "SC", "JP", "KR" };
	} else if (locale.begins_with("ja")) {
		suffixes = { "JP", "SC", "TC", "HK", "KR" };
	} else if (locale.begins_with("ko")) {
		suffixes = { "KR", "SC", "TC", "HK", "JP" };
	} else {
		suffixes = { "SC", "TC", "HK", "JP", "KR" };
	}
	PackedStringArray names;
	for (const String &suffix : suffixes) {
		names.push_back("Noto Sans CJK " + suffix);
	}
	return names;
}

static TypedArray<Font> _solers_fallbacks(bool p_bold) {
	static const SolersEmbeddedFont regular[] = {
		{ _font_Vazirmatn_Regular, _font_Vazirmatn_Regular_size },
		{ _font_NotoSansBengali_Regular, _font_NotoSansBengali_Regular_size },
		{ _font_NotoSansDevanagari_Regular, _font_NotoSansDevanagari_Regular_size },
		{ _font_NotoSansGeorgian_Regular, _font_NotoSansGeorgian_Regular_size },
		{ _font_NotoSansHebrew_Regular, _font_NotoSansHebrew_Regular_size },
		{ _font_NotoSansMalayalamUI_Regular, _font_NotoSansMalayalamUI_Regular_size },
		{ _font_NotoSansOriya_Regular, _font_NotoSansOriya_Regular_size },
		{ _font_NotoSansSinhala_Regular, _font_NotoSansSinhala_Regular_size },
		{ _font_NotoSansTamilUI_Regular, _font_NotoSansTamilUI_Regular_size },
		{ _font_NotoSansTeluguUI_Regular, _font_NotoSansTeluguUI_Regular_size },
		{ _font_NotoSansThai_Regular, _font_NotoSansThai_Regular_size },
	};
	static const SolersEmbeddedFont bold[] = {
		{ _font_Vazirmatn_Bold, _font_Vazirmatn_Bold_size },
		{ _font_NotoSansBengali_Bold, _font_NotoSansBengali_Bold_size },
		{ _font_NotoSansDevanagari_Bold, _font_NotoSansDevanagari_Bold_size },
		{ _font_NotoSansGeorgian_Bold, _font_NotoSansGeorgian_Bold_size },
		{ _font_NotoSansHebrew_Bold, _font_NotoSansHebrew_Bold_size },
		{ _font_NotoSansMalayalamUI_Bold, _font_NotoSansMalayalamUI_Bold_size },
		{ _font_NotoSansOriya_Bold, _font_NotoSansOriya_Bold_size },
		{ _font_NotoSansSinhala_Bold, _font_NotoSansSinhala_Bold_size },
		{ _font_NotoSansTamilUI_Bold, _font_NotoSansTamilUI_Bold_size },
		{ _font_NotoSansTeluguUI_Bold, _font_NotoSansTeluguUI_Bold_size },
		{ _font_NotoSansThai_Bold, _font_NotoSansThai_Bold_size },
	};
	TypedArray<Font> fallbacks;
	const SolersEmbeddedFont *fonts = p_bold ? bold : regular;
	for (uint32_t i = 0; i < sizeof(regular) / sizeof(regular[0]); i++) {
		_solers_font(fonts[i], &fallbacks);
	}
	auto add_cjk = [&fallbacks, p_bold](const Ref<FontFile> &p_font) {
		if (!p_bold) {
			fallbacks.push_back(p_font);
			return;
		}
		Ref<FontVariation> bold_font;
		bold_font.instantiate();
		bold_font->set_base_font(p_font);
		bold_font->set_variation_embolden(0.6);
		fallbacks.push_back(bold_font);
	};

	Ref<SystemFont> cjk;
	cjk.instantiate();
	cjk->set_font_names(_solers_cjk_names());
	cjk->set_font_weight(p_bold ? 700 : 400);
	fallbacks.push_back(cjk);

	Ref<FontFile> common = _solers_font({ _font_DroidSansFallback, _font_DroidSansFallback_size });
	common->set_language_support_override("ja", false);
	common->set_language_support_override("zh", true);
	common->set_language_support_override("ko", true);
	common->set_language_support_override("*", false);
	add_cjk(common);
	Ref<FontFile> japanese = _solers_font({ _font_DroidSansJapanese, _font_DroidSansJapanese_size });
	japanese->set_language_support_override("ja", true);
	japanese->set_language_support_override("zh", false);
	japanese->set_language_support_override("ko", false);
	japanese->set_language_support_override("*", false);
	add_cjk(japanese);

	Ref<SystemFont> emoji;
	emoji.instantiate();
	const PackedStringArray emoji_names = { "Apple Color Emoji", "Segoe UI Emoji", "Noto Color Emoji", "Twitter Color Emoji", "OpenMoji" };
	emoji->set_font_names(emoji_names);
	fallbacks.push_back(emoji);
	return fallbacks;
}

Ref<Theme> SolersUITheme::create() {
	Ref<FontFile> regular = _solers_font({ _font_Inter_Regular, _font_Inter_Regular_size });
	Ref<FontFile> bold = _solers_font({ _font_Inter_Bold, _font_Inter_Bold_size });
	Ref<FontFile> mono_base = _solers_font({ _font_JetBrainsMono_Regular, _font_JetBrainsMono_Regular_size });
	regular->set_fallbacks(_solers_fallbacks(false));
	bold->set_fallbacks(_solers_fallbacks(true));
	mono_base->set_fallbacks(regular->get_fallbacks());
	mono_base->set_subpixel_positioning(TextServer::SUBPIXEL_POSITIONING_DISABLED);
	mono_base->set_keep_rounding_remainders(false);

	Dictionary body_features;
	body_features["calt"] = false;
	body_features["ss04"] = true;
	body_features["tnum"] = true;
	Ref<FontVariation> body;
	body.instantiate();
	body->set_base_font(regular);
	body->set_opentype_features(body_features);
	Ref<FontVariation> strong;
	strong.instantiate();
	strong->set_base_font(bold);
	strong->set_opentype_features(body_features);
	Ref<FontVariation> italic;
	italic.instantiate();
	italic->set_base_font(body);
	italic->set_variation_transform(Transform2D(1.0, 0.2, 0.0, 1.0, 0.0, 0.0));
	Ref<FontVariation> strong_italic;
	strong_italic.instantiate();
	strong_italic->set_base_font(strong);
	strong_italic->set_variation_transform(Transform2D(1.0, 0.2, 0.0, 1.0, 0.0, 0.0));
	Ref<FontVariation> mono;
	mono.instantiate();
	mono->set_base_font(mono_base);
	Dictionary mono_features;
	mono_features["calt"] = false;
	mono_features["liga"] = false;
	mono->set_opentype_features(mono_features);

	Ref<Theme> theme;
	theme.instantiate();
	theme->set_default_font(body);
	theme->set_font("normal_font", "RichTextLabel", body);
	theme->set_font("bold_font", "RichTextLabel", strong);
	theme->set_font("italics_font", "RichTextLabel", italic);
	theme->set_font("bold_italics_font", "RichTextLabel", strong_italic);
	theme->set_font("mono_font", "RichTextLabel", mono);
	theme->set_font(SceneStringName(font), "SolersMono", mono);
	theme->set_type_variation("SolersCodeText", "RichTextLabel");
	theme->set_font("normal_font", "SolersCodeText", mono);
	const Tokens tokens = make_tokens();
	theme->set_type_variation("SolersSessionTitle", "Label");
	theme->set_font(SceneStringName(font), "SolersSessionTitle", strong);
	theme->set_font_size(SceneStringName(font_size), "SolersSessionTitle", int(14 * EDSCALE));
	theme->set_type_variation("SolersSessionMeta", "Label");
	theme->set_font_size(SceneStringName(font_size), "SolersSessionMeta", int(11 * EDSCALE));
	theme->set_type_variation("SolersHeroTitle", "Label");
	theme->set_font(SceneStringName(font), "SolersHeroTitle", strong);
	theme->set_font_size(SceneStringName(font_size), "SolersHeroTitle", int(22 * EDSCALE));
	theme->set_color(SceneStringName(font_color), "SolersSessionTitle", tokens.text);
	theme->set_color(SceneStringName(font_color), "SolersSessionMeta", tokens.text_dim);
	theme->set_color(SceneStringName(font_color), "SolersHeroTitle", tokens.text);

	const int control_radius = int(tokens.radius_control * EDSCALE);
	const int card_radius = int(tokens.radius_list_thumb * EDSCALE);
	const int product_radius = int(tokens.radius_home_tile * EDSCALE);
	const int border_width = MAX(1, int(EDSCALE));
	const float control_margin = 8.0f * EDSCALE;
	const Ref<StyleBoxFlat> control_normal = _solers_flat(tokens.card, control_radius, tokens.border, border_width, control_margin);
	const Ref<StyleBoxFlat> control_hover = _solers_flat(tokens.card_hover, control_radius, tokens.border, border_width, control_margin);
	const Ref<StyleBoxFlat> control_pressed = _solers_flat(tokens.home_tile_pressed, control_radius, tokens.border_strong, border_width, control_margin);
	const Ref<StyleBoxFlat> control_disabled = _solers_flat(tokens.surface, control_radius, tokens.border, border_width, control_margin);
	const Ref<StyleBoxFlat> control_focus = _solers_flat(Color(0, 0, 0, 0), control_radius, tokens.hairline, border_width, control_margin);

	theme->set_stylebox(SceneStringName(panel), "PanelContainer", _solers_flat(tokens.bg, 0, Color(), 0, 0));
	for (const StringName &type : { SNAME("Button"), SNAME("OptionButton") }) {
		theme->set_stylebox(CoreStringName(normal), type, control_normal);
		theme->set_stylebox(SceneStringName(hover), type, control_hover);
		theme->set_stylebox(SceneStringName(pressed), type, control_pressed);
		theme->set_stylebox(SNAME("disabled"), type, control_disabled);
		theme->set_stylebox(SNAME("focus"), type, control_focus);
		theme->set_color(SceneStringName(font_color), type, tokens.text);
		theme->set_color(SNAME("font_hover_color"), type, tokens.text);
		theme->set_color(SNAME("font_pressed_color"), type, tokens.text);
		theme->set_color(SNAME("font_focus_color"), type, tokens.text);
		theme->set_color(SNAME("font_disabled_color"), type, tokens.text_dim);
	}
	theme->set_stylebox(CoreStringName(normal), "OptionButton", _solers_flat(tokens.card, product_radius, tokens.border, border_width, control_margin));
	theme->set_stylebox(SceneStringName(hover), "OptionButton", _solers_flat(tokens.card_hover, product_radius, tokens.border, border_width, control_margin));
	theme->set_stylebox(SceneStringName(pressed), "OptionButton", _solers_flat(tokens.home_tile_pressed, product_radius, tokens.border_strong, border_width, control_margin));
	theme->set_stylebox(SNAME("disabled"), "OptionButton", _solers_flat(tokens.surface, product_radius, tokens.border, border_width, control_margin));
	theme->set_stylebox(SNAME("focus"), "OptionButton", _solers_flat(Color(0, 0, 0, 0), product_radius, tokens.hairline, border_width, control_margin));
	theme->set_icon(SNAME("arrow"), "OptionButton", SolersIcons::get(SNAME("chevron_down"), int(12 * EDSCALE)));
	theme->set_constant(SNAME("arrow_margin"), "OptionButton", int(8 * EDSCALE));
	theme->set_constant(SNAME("modulate_arrow"), "OptionButton", 1);
	for (const StringName &type : { SNAME("LineEdit"), SNAME("TextEdit") }) {
		theme->set_stylebox(CoreStringName(normal), type, control_normal);
		theme->set_stylebox(SNAME("focus"), type, control_focus);
		theme->set_stylebox(SNAME("read_only"), type, control_disabled);
		theme->set_color(SceneStringName(font_color), type, tokens.text);
		theme->set_color(SNAME("font_placeholder_color"), type, tokens.text_dim);
		theme->set_color(SNAME("caret_color"), type, tokens.text);
		theme->set_color(SNAME("selection_color"), type, tokens.card_selected);
	}
	theme->set_type_variation(SNAME("SolersStudioPrompt"), SNAME("TextEdit"));
	theme->set_stylebox(CoreStringName(normal), "SolersStudioPrompt", _solers_flat(tokens.card, product_radius, tokens.border, border_width, 12 * EDSCALE));
	theme->set_stylebox(SNAME("focus"), "SolersStudioPrompt", _solers_flat(tokens.card, product_radius, tokens.hairline, border_width, 12 * EDSCALE));
	theme->set_stylebox(SNAME("read_only"), "SolersStudioPrompt", _solers_flat(tokens.surface, product_radius, tokens.border, border_width, 12 * EDSCALE));
	theme->set_font_size(SceneStringName(font_size), "SolersStudioPrompt", int(14 * EDSCALE));

	theme->set_stylebox(SceneStringName(panel), "ItemList", _solers_flat(tokens.surface, 0, Color(), 0, 8 * EDSCALE));
	theme->set_stylebox(SNAME("focus"), "ItemList", control_focus);
	theme->set_stylebox(SNAME("hovered"), "ItemList", _solers_flat(tokens.card_hover, card_radius, Color(), 0, 6 * EDSCALE));
	for (const StringName &name : { SNAME("selected"), SNAME("selected_focus"), SNAME("hovered_selected"), SNAME("hovered_selected_focus") }) {
		theme->set_stylebox(name, "ItemList", _solers_flat(tokens.card_selected, card_radius, Color(), 0, 6 * EDSCALE));
	}
	theme->set_stylebox(SNAME("cursor"), "ItemList", control_focus);
	theme->set_stylebox(SNAME("cursor_unfocused"), "ItemList", control_focus);
	theme->set_color(SceneStringName(font_color), "ItemList", tokens.text_dim);
	theme->set_color(SNAME("font_hovered_color"), "ItemList", tokens.text);
	theme->set_color(SNAME("font_selected_color"), "ItemList", tokens.text);
	theme->set_color(SNAME("font_hovered_selected_color"), "ItemList", tokens.text);
	theme->set_constant(SNAME("h_separation"), "ItemList", int(8 * EDSCALE));
	theme->set_constant(SNAME("v_separation"), "ItemList", int(8 * EDSCALE));
	theme->set_type_variation(SNAME("SolersStudioRail"), SNAME("ItemList"));
	theme->set_stylebox(SceneStringName(panel), "SolersStudioRail", _solers_flat(tokens.surface, 0, Color(), 0, 0));
	Ref<StyleBoxFlat> rail_hover = _solers_flat(tokens.card_hover, card_radius, Color(), 0, 4 * EDSCALE);
	rail_hover->set_expand_margin_all(-2 * EDSCALE);
	Ref<StyleBoxFlat> rail_selected = _solers_flat(tokens.card_selected, card_radius, Color(), 0, 4 * EDSCALE);
	rail_selected->set_expand_margin_all(-2 * EDSCALE);
	for (const StringName &name : { SNAME("hovered"), SNAME("hovered_selected"), SNAME("hovered_selected_focus") }) {
		theme->set_stylebox(name, "SolersStudioRail", rail_hover);
	}
	for (const StringName &name : { SNAME("selected"), SNAME("selected_focus") }) {
		theme->set_stylebox(name, "SolersStudioRail", rail_selected);
	}
	theme->set_constant(SNAME("h_separation"), "SolersStudioRail", int(8 * EDSCALE));
	theme->set_constant(SNAME("v_separation"), "SolersStudioRail", int(10 * EDSCALE));
	theme->set_type_variation(SNAME("SolersStudioScroll"), SNAME("VScrollBar"));
	const Ref<StyleBoxFlat> scroll_empty = _solers_flat(Color(0, 0, 0, 0), 3 * EDSCALE, Color(0, 0, 0, 0), 0, 0);
	theme->set_stylebox(SNAME("scroll"), "SolersStudioScroll", scroll_empty);
	theme->set_stylebox(SNAME("scroll_focus"), "SolersStudioScroll", scroll_empty);
	theme->set_stylebox(SNAME("grabber"), "SolersStudioScroll", scroll_empty);
	theme->set_stylebox(SNAME("grabber_highlight"), "SolersStudioScroll", _solers_flat(tokens.text_dim, 3 * EDSCALE, Color(), 0, 0));
	theme->set_stylebox(SNAME("grabber_pressed"), "SolersStudioScroll", _solers_flat(tokens.text, 3 * EDSCALE, Color(), 0, 0));

	const Ref<StyleBoxFlat> tab_selected = _solers_flat(tokens.card_hover, product_radius, tokens.border, border_width, 8 * EDSCALE);
	const Ref<StyleBoxFlat> tab_unselected = _solers_flat(tokens.surface, product_radius, Color(), 0, 8 * EDSCALE);
	const Ref<StyleBoxFlat> tab_hovered = _solers_flat(tokens.card, product_radius, Color(), 0, 8 * EDSCALE);
	for (const StringName &type : { SNAME("TabContainer"), SNAME("TabBar") }) {
		theme->set_stylebox(SNAME("tab_selected"), type, tab_selected);
		theme->set_stylebox(SNAME("tab_unselected"), type, tab_unselected);
		theme->set_stylebox(SNAME("tab_hovered"), type, tab_hovered);
		theme->set_stylebox(SNAME("tab_disabled"), type, tab_unselected);
		theme->set_stylebox(SNAME("tab_focus"), type, control_focus);
		theme->set_color(SNAME("font_selected_color"), type, tokens.text);
		theme->set_color(SNAME("font_hovered_color"), type, tokens.text);
		theme->set_color(SNAME("font_unselected_color"), type, tokens.text_dim);
	}
	theme->set_stylebox(SceneStringName(panel), "TabContainer", _solers_flat(tokens.surface, 0, Color(), 0, 0));
	theme->set_stylebox(SNAME("tabbar_background"), "TabContainer", _solers_flat(tokens.surface, 0, Color(), 0, 0));

	theme->set_stylebox(SNAME("background"), "ProgressBar", _solers_flat(tokens.card, card_radius, Color(), 0, 2 * EDSCALE));
	theme->set_stylebox(SNAME("fill"), "ProgressBar", _solers_flat(tokens.text, card_radius, Color(), 0, 2 * EDSCALE));
	theme->set_color(SceneStringName(font_color), "ProgressBar", tokens.on_primary);
	for (const StringName &type : { SNAME("CheckBox"), SNAME("CheckButton") }) {
		for (const StringName &name : { CoreStringName(normal), SceneStringName(hover), SceneStringName(pressed), SNAME("hover_pressed"), SNAME("disabled") }) {
			theme->set_stylebox(name, type, _solers_flat(Color(0, 0, 0, 0), 0, Color(), 0, 2 * EDSCALE));
		}
		theme->set_stylebox(SNAME("focus"), type, control_focus);
		theme->set_color(SceneStringName(font_color), type, tokens.text);
		theme->set_color(SNAME("font_hover_color"), type, tokens.text);
		theme->set_color(SNAME("font_pressed_color"), type, tokens.text);
	}

	theme->set_type_variation(SNAME("SolersPrimaryButton"), SNAME("Button"));
	theme->set_stylebox(CoreStringName(normal), "SolersPrimaryButton", _solers_flat(tokens.primary, product_radius, Color(), 0, 10 * EDSCALE));
	theme->set_stylebox(SceneStringName(hover), "SolersPrimaryButton", _solers_flat(tokens.primary_hover, product_radius, Color(), 0, 10 * EDSCALE));
	theme->set_stylebox(SceneStringName(pressed), "SolersPrimaryButton", _solers_flat(tokens.primary_pressed, product_radius, Color(), 0, 10 * EDSCALE));
	theme->set_stylebox(SNAME("disabled"), "SolersPrimaryButton", _solers_flat(tokens.primary_pressed, product_radius, Color(), 0, 10 * EDSCALE));
	theme->set_stylebox(SNAME("focus"), "SolersPrimaryButton", control_focus);
	for (const StringName &name : { SceneStringName(font_color), SNAME("font_hover_color"), SNAME("font_pressed_color"), SNAME("font_focus_color") }) {
		theme->set_color(name, "SolersPrimaryButton", tokens.on_primary);
	}
	theme->set_color(SNAME("font_disabled_color"), "SolersPrimaryButton", tokens.on_primary);
	theme->set_type_variation(SNAME("SolersStudioSegment"), SNAME("Button"));
	theme->set_stylebox(CoreStringName(normal), "SolersStudioSegment", _solers_flat(tokens.surface, card_radius, Color(), 0, 6 * EDSCALE));
	theme->set_stylebox(SceneStringName(hover), "SolersStudioSegment", _solers_flat(tokens.card_hover, card_radius, Color(), 0, 6 * EDSCALE));
	theme->set_stylebox(SceneStringName(pressed), "SolersStudioSegment", _solers_flat(tokens.card_selected, card_radius, tokens.hairline, border_width, 6 * EDSCALE));
	theme->set_stylebox(SNAME("disabled"), "SolersStudioSegment", _solers_flat(tokens.surface, card_radius, Color(), 0, 6 * EDSCALE));
	theme->set_stylebox(SNAME("focus"), "SolersStudioSegment", control_focus);
	for (const StringName &name : { SceneStringName(font_color), SNAME("font_hover_color"), SNAME("font_pressed_color"), SNAME("font_focus_color") }) {
		theme->set_color(name, "SolersStudioSegment", tokens.text);
	}
	theme->set_type_variation(SNAME("SolersStudioActionButton"), SNAME("Button"));
	theme->set_stylebox(CoreStringName(normal), "SolersStudioActionButton", _solers_flat(Color(0, 0, 0, 0), card_radius, Color(), 0, 6 * EDSCALE));
	theme->set_stylebox(SceneStringName(hover), "SolersStudioActionButton", _solers_flat(tokens.card_hover, card_radius, Color(), 0, 6 * EDSCALE));
	theme->set_stylebox(SceneStringName(pressed), "SolersStudioActionButton", _solers_flat(tokens.card_selected, card_radius, Color(), 0, 6 * EDSCALE));
	theme->set_stylebox(SNAME("disabled"), "SolersStudioActionButton", _solers_flat(Color(0, 0, 0, 0), card_radius, Color(), 0, 6 * EDSCALE));
	theme->set_stylebox(SNAME("focus"), "SolersStudioActionButton", control_focus);
	const Color editor_accent = EditorNode::get_singleton() ? EditorNode::get_singleton()->get_editor_theme()->get_color(SNAME("accent_color"), EditorStringName(Editor)) : tokens.text;
	const Color editor_error = EditorNode::get_singleton() ? EditorNode::get_singleton()->get_editor_theme()->get_color(SNAME("error_color"), EditorStringName(Editor)) : Color(0.9, 0.3, 0.3);
	theme->set_color(SNAME("icon_normal_color"), "SolersStudioActionButton", tokens.text_dim);
	theme->set_color(SNAME("icon_hover_color"), "SolersStudioActionButton", editor_accent);
	theme->set_color(SNAME("icon_pressed_color"), "SolersStudioActionButton", editor_accent);
	theme->set_color(SNAME("icon_focus_color"), "SolersStudioActionButton", editor_accent);
	theme->set_color(SNAME("icon_disabled_color"), "SolersStudioActionButton", tokens.text_dim);
	theme->set_type_variation(SNAME("SolersAssetCard"), SNAME("Button"));
	const Ref<StyleBoxFlat> asset_card_idle = _solers_flat(Color(0, 0, 0, 0), card_radius, Color(), 0, 0);
	const Ref<StyleBoxFlat> asset_card_active = _solers_flat(Color(0, 0, 0, 0), card_radius, editor_accent, border_width, 0);
	for (const StringName &name : { CoreStringName(normal), SNAME("disabled") }) {
		theme->set_stylebox(name, "SolersAssetCard", asset_card_idle);
	}
	for (const StringName &name : { SceneStringName(hover), SceneStringName(pressed), SNAME("hover_pressed"), SNAME("focus") }) {
		theme->set_stylebox(name, "SolersAssetCard", asset_card_active);
	}
	theme->set_color(SNAME("failure_color"), "SolersAssetCard", editor_error);
	theme->set_color(SNAME("placeholder_color"), "SolersAssetCard", tokens.text_dim);
	theme->set_type_variation(SNAME("SolersPopupPanel"), SNAME("PanelContainer"));
	theme->set_stylebox(SceneStringName(panel), "SolersPopupPanel", _solers_flat(tokens.surface, product_radius, tokens.border, border_width, 6 * EDSCALE));
	for (const StringName &variation : { SNAME("SolersPopupItem"), SNAME("SolersPopupDangerItem") }) {
		theme->set_type_variation(variation, SNAME("Button"));
		theme->set_stylebox(CoreStringName(normal), variation, _solers_flat(Color(0, 0, 0, 0), card_radius, Color(), 0, 8 * EDSCALE));
		theme->set_stylebox(SceneStringName(hover), variation, _solers_flat(tokens.card_hover, card_radius, Color(), 0, 8 * EDSCALE));
		theme->set_stylebox(SceneStringName(pressed), variation, _solers_flat(tokens.card_selected, card_radius, Color(), 0, 8 * EDSCALE));
		theme->set_stylebox(SNAME("hover_pressed"), variation, _solers_flat(tokens.card_selected, card_radius, Color(), 0, 8 * EDSCALE));
		theme->set_stylebox(SNAME("focus"), variation, control_focus);
		theme->set_color(SceneStringName(font_color), variation, variation == SNAME("SolersPopupDangerItem") ? editor_error : tokens.text);
		theme->set_color(SNAME("font_hover_color"), variation, variation == SNAME("SolersPopupDangerItem") ? editor_error : tokens.text);
		theme->set_color(SNAME("font_pressed_color"), variation, variation == SNAME("SolersPopupDangerItem") ? editor_error : tokens.text);
	}
	theme->set_stylebox(SceneStringName(panel), "PopupMenu", _solers_flat(tokens.surface, product_radius, Color(), 0, 6 * EDSCALE));
	theme->set_stylebox(SceneStringName(hover), "PopupMenu", _solers_flat(tokens.card_hover, card_radius, Color(), 0, 6 * EDSCALE));
	theme->set_color(SceneStringName(font_color), "PopupMenu", tokens.text);
	theme->set_color(SNAME("font_hover_color"), "PopupMenu", tokens.text);
	theme->set_color(SNAME("font_disabled_color"), "PopupMenu", tokens.text_dim);
	theme->set_color(SNAME("font_accelerator_color"), "PopupMenu", tokens.text_dim);
	theme->set_constant(SNAME("item_start_padding"), "PopupMenu", int(8 * EDSCALE));
	theme->set_constant(SNAME("item_end_padding"), "PopupMenu", int(8 * EDSCALE));
	theme->set_constant(SNAME("v_separation"), "PopupMenu", int(4 * EDSCALE));
	const Ref<Texture2D> empty_mark = ImageTexture::create_from_image(Image::create_empty(MAX(1, int(14 * EDSCALE)), MAX(1, int(14 * EDSCALE)), false, Image::FORMAT_RGBA8));
	const Ref<Texture2D> check_mark = SolersIcons::get(SNAME("check"), int(14 * EDSCALE));
	theme->set_icon(SNAME("radio_checked"), "PopupMenu", check_mark);
	theme->set_icon(SNAME("radio_unchecked"), "PopupMenu", empty_mark);
	apply_chrome_edges(theme, tokens.hairline);
	return theme;
}

SolersUITheme::Tokens SolersUITheme::make_tokens() {
	Tokens t;

	// Palette sampled against the Unreal Engine 5 Project Browser itself (not the Slate JSON,
	// whose values are linear-space and read too bright once treated as sRGB).
	// Four-layer neutral darks, deepest-to-lightest: window backdrop → content
	// panel → tile surface → hover. Unreal sits everything noticeably *deeper* than
	// stock Godot; the gap between layers is what makes the chrome read "Unreal".
	t.bg = Color(0.055f, 0.055f, 0.062f); // ~#0E0E10 — window backdrop (deepest).
	t.surface = Color(0.082f, 0.082f, 0.090f); // ~#151517 — content panel / grid backdrop.
	t.card = Color(0.118f, 0.118f, 0.128f); // ~#1E1E21 — tile / card surface.
	t.card_hover = Color(0.165f, 0.165f, 0.180f); // ~#2A2A2E — hover.
	t.card_selected = Color(0.028f, 0.165f, 0.318f); // ~#072A51 — muted selection navy.
	// Unreal separates surfaces with *dark recess* lines, not bright outlines. Dark,
	// low-alpha edges read as depth and — crucially — never produce visible white
	// corner ticks the way light borders do on rounded boxes.
	t.border = Color(0, 0, 0, 0.45f); // Hairline recess separators.
	t.border_strong = Color(0, 0, 0, 0.62f); // Emphasized edges.
	// Cursor-flat chrome edges: light hairline on deep bg (not dark recess).
	t.hairline = Color(0.95f, 0.95f, 0.97f, 0.12f);
	t.text = Color(0.886f, 0.890f, 0.902f); // ~#E2E3E6 — primary text.
	t.text_dim = Color(0.886f, 0.890f, 0.902f, 0.55f); // Muted text.
	t.primary = Color(0.94f, 0.94f, 0.92f);
	t.primary_hover = Color(1.0f, 1.0f, 0.98f);
	t.primary_pressed = Color(0.82f, 0.82f, 0.80f);
	t.on_primary = Color(0.055f, 0.055f, 0.052f);

	t.home_tile = Color(0.090f, 0.090f, 0.096f); // ~#171718
	t.home_tile_hover = Color(0.125f, 0.125f, 0.132f);
	t.home_tile_pressed = Color(0.070f, 0.070f, 0.076f);

	return t;
}

static Ref<StyleBoxFlat> _solers_window_chrome_style(const Ref<Theme> &p_theme, const Color &p_chrome) {
	if (p_theme.is_valid() && p_theme->has_stylebox(SNAME("embedded_border"), SNAME("Window"))) {
		Ref<StyleBoxFlat> box = p_theme->get_stylebox(SNAME("embedded_border"), SNAME("Window"));
		if (box.is_valid()) {
			box = box->duplicate();
			box->set_bg_color(p_chrome);
			box->set_border_color(p_chrome);
			return box;
		}
	}
	// Fallback: must set border_color (default ~0.8 gray breaks flat title chrome).
	Ref<StyleBoxFlat> box = _solers_flat(p_chrome, 0, Color(), 0, 0);
	box->set_border_width(SIDE_TOP, 24 * EDSCALE);
	box->set_expand_margin(SIDE_TOP, 24 * EDSCALE);
	box->set_border_color(p_chrome);
	box->set_bg_color(p_chrome);
	return box;
}

void SolersUITheme::apply_window_chrome(const Ref<Theme> &p_theme, const Color &p_chrome) {
	ERR_FAIL_COND(p_theme.is_null());

	// Fill only. Title/body join is NOT a dialog StyleBox — it is the same
	// Editor/hairline token EditorTitleBar uses, drawn for embedded Windows in
	// Viewport::_sub_window_update (engine title chrome, not a per-dialog UI).
	Ref<StyleBoxFlat> window_chrome = _solers_window_chrome_style(p_theme, p_chrome);
	p_theme->set_stylebox(SNAME("embedded_border"), SNAME("Window"), window_chrome);
	p_theme->set_stylebox(SNAME("embedded_unfocused_border"), SNAME("Window"), window_chrome);

	Ref<StyleBoxFlat> dialog_panel;
	if (p_theme->has_stylebox(SNAME("panel"), SNAME("AcceptDialog"))) {
		dialog_panel = p_theme->get_stylebox(SNAME("panel"), SNAME("AcceptDialog"));
		if (dialog_panel.is_valid()) {
			dialog_panel = dialog_panel->duplicate();
		}
	}
	if (dialog_panel.is_null()) {
		dialog_panel = _solers_flat(p_chrome, 0, Color(), 0, 0);
	} else {
		dialog_panel->set_bg_color(p_chrome);
		dialog_panel->set_border_width_all(0); // drop any prior join-border patch
	}
	p_theme->set_stylebox(SNAME("panel"), SNAME("AcceptDialog"), dialog_panel);

	// Stock forks share the same fill Ref (schema sync).
	for (const StringName &type : { StringName("EditorSettingsDialog"), StringName("ProjectSettingsEditor") }) {
		if (p_theme->has_stylebox(SNAME("panel"), type)) {
			p_theme->set_stylebox(SNAME("panel"), type, dialog_panel);
		}
	}
}

void SolersUITheme::apply_chrome_edges(const Ref<Theme> &p_theme, const Color &p_hairline) {
	ERR_FAIL_COND(p_theme.is_null());

	const int hair = MAX(1, (int)Math::round(EDSCALE));
	// One token: EditorTitleBar (shell) + Viewport embedded title join + Split/Separator.
	p_theme->set_color(SNAME("hairline"), EditorStringName(Editor), p_hairline);
	p_theme->set_color(SNAME("hairline"), SNAME("Window"), p_hairline);

	auto make_line = [&](bool p_vertical) -> Ref<StyleBoxLine> {
		Ref<StyleBoxLine> line;
		line.instantiate();
		line->set_color(p_hairline);
		line->set_thickness(hair);
		line->set_vertical(p_vertical);
		line->set_grow_begin(0);
		line->set_grow_end(0);
		return line;
	};

	// Pane splits (HSplit = vertical bar). DockSplitContainer inherits SplitContainer.
	p_theme->set_stylebox("split_bar_background", "HSplitContainer", make_line(true));
	p_theme->set_stylebox("split_bar_background", "SplitContainer", make_line(true));
	p_theme->set_stylebox("split_bar_background", "VSplitContainer", make_line(false));
	for (const StringName &type : { StringName("HSplitContainer"), StringName("VSplitContainer"), StringName("SplitContainer") }) {
		p_theme->set_constant("separation", type, hair);
		p_theme->set_constant("autohide", type, 1);
	}

	// Explicit HSeparator / VSeparator — and base Separator.separation (what
	// Separator::get_minimum_size actually caches via class hierarchy).
	p_theme->set_stylebox(SNAME("separator"), "HSeparator", make_line(false));
	p_theme->set_stylebox(SNAME("separator"), "VSeparator", make_line(true));
	p_theme->set_constant("separation", "HSeparator", hair);
	p_theme->set_constant("separation", "VSeparator", hair);
	p_theme->set_constant("separation", "Separator", hair);
}

void SolersUITheme::configure_settings_host(AcceptDialog *p_dialog, const Ref<Theme> &p_theme) {
	ERR_FAIL_NULL(p_dialog);
	p_dialog->set_theme_type_variation("PMSettingsDialog");
	// Same window chrome as every other AcceptDialog — no Settings-only line.
	const Tokens t = make_tokens();
	Ref<Theme> host_theme = p_theme->duplicate();
	host_theme->set_color(SNAME("background"), EditorStringName(Editor), t.bg);
	apply_window_chrome(host_theme, t.bg);
	apply_chrome_edges(host_theme, t.hairline);
	host_theme->set_type_variation("PMSettingsDialog", "AcceptDialog");
	host_theme->set_constant(SNAME("buttons_separation"), "PMSettingsDialog", 0);
	p_dialog->set_theme(host_theme);
	if (Button *ok = p_dialog->get_ok_button()) {
		ok->hide();
		if (CanvasItem *bar = Object::cast_to<CanvasItem>(ok->get_parent())) {
			bar->hide();
		}
	}
}
