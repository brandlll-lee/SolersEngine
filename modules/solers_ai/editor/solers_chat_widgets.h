/**************************************************************************/
/*  solers_chat_widgets.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Self-drawn widget kit for the Solers AI chat dock. Follows the same    */
/* philosophy as SolersCategoryCard in the Project Manager: fully         */
/* custom-drawn Controls (no stock Button/Panel skeleton) so the chat     */
/* chrome escapes the recognizable Godot look and matches the refined,    */
/* low-contrast Codex composer aesthetic.                                 */
/*                                                                        */
/* All widgets are event-driven: hover/press transitions run a short,     */
/* bounded internal process for the highlight blend, then processing      */
/* stops. Steady state costs zero CPU and zero redraws.                   */
/*                                                                        */
/* Functional icons are pinned Tabler SVG assets (see UI_ICON_LICENSE),   */
/* rasterized once per (name, size) and tinted at draw time.               */
/**************************************************************************/

#pragma once

#include "core/variant/array.h"
#include "core/variant/callable.h"
#include "core/variant/dictionary.h"
#include "scene/gui/box_container.h"
#include "scene/gui/control.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/panel_container.h"
#include "scene/resources/font.h"
#include "scene/resources/texture.h"

// Module-owned SVG catalog with one process-lifetime texture cache.
class SolersIcons {
public:
	// Returns a white glyph texture at p_size_px physical pixels.
	// Tint it with CanvasItem draw modulate; never bake colors into the cache.
	static Ref<Texture2D> get(const StringName &p_name, int p_size_px);
	// White provider mark for a models.dev catalog id (profile field
	// `catalog_provider`), from the build-time vendored logo table. Unknown
	// ids fall back to the generic "synthetic" mark, mirroring opencode.
	static Ref<Texture2D> provider_logo(const String &p_catalog_id, int p_size_px);
	// Multicolor official mark when `{id}.color.svg` exists. Empty Ref if the
	// color track is absent — callers fall back to provider_logo and may tint.
	// Color textures must be drawn without Button/theme icon modulate.
	static Ref<Texture2D> provider_logo_color(const String &p_catalog_id, int p_size_px);
	static void clear_cache();
};

// Self-drawn icon button. Two skins:
//  - GHOST: transparent at rest, soft rounded wash on hover (topbar/composer icons).
//  - PRIMARY: filled circle pill (the Codex-style send action), with a
//    disabled state that dims to a faint ring while the composer is empty.
class SolersGlyphButton : public Control {
	GDCLASS(SolersGlyphButton, Control);

public:
	enum Skin {
		SKIN_GHOST,
		SKIN_PRIMARY,
	};

private:
	StringName glyph;
	Skin skin = SKIN_GHOST;
	int glyph_px = 15; // Logical pixels; multiplied by EDSCALE at draw time.
	Color accent = Color(0, 0, 0, 0); // Optional glyph tint override.
	bool enabled_state = true;

	bool hovering = false;
	bool pressing = false;
	float anim = 0.0f;
	float anim_target = 0.0f;

	Callable pressed_callback;

	void _update_anim_target();
	void _activate();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

	void configure(const StringName &p_glyph, Skin p_skin, const String &p_tooltip, int p_glyph_px = 15);
	void set_accent(const Color &p_accent);
	void set_pressed_callback(const Callable &p_cb) { pressed_callback = p_cb; }
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled_state; }

	SolersGlyphButton();
};

// Self-drawn select chip: [glyph] strong-text muted-text chevron.
class SolersSelectChip : public Control {
	GDCLASS(SolersSelectChip, Control);

	StringName glyph; // Optional leading glyph (empty -> none).
	Ref<Texture2D> leading_texture; // Optional leading mark (e.g. provider logo); wins over glyph.
	String strong_text;
	String muted_text;
	bool show_chevron = true; // Trailing dropdown chevron (false for static pills).

	bool hovering = false;
	bool pressing = false;
	float anim = 0.0f;
	float anim_target = 0.0f;

	Callable pressed_callback;

	void _update_anim_target();
	void _activate();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual void gui_input(const Ref<InputEvent> &p_event) override;
	virtual Size2 get_minimum_size() const override;

	void configure(const StringName &p_glyph, const String &p_strong, const String &p_muted, const String &p_tooltip);
	void set_texts(const String &p_strong, const String &p_muted);
	void set_leading_texture(const Ref<Texture2D> &p_texture);
	void set_show_chevron(bool p_show);
	void set_pressed_callback(const Callable &p_cb) { pressed_callback = p_cb; }

	SolersSelectChip();
};

// Composer-adjacent plan chip: "Step N / M" capsule; hover reveals the todo list.
class SolersPlanCapsule : public Control {
	GDCLASS(SolersPlanCapsule, Control);

	String explanation;
	Array plan;
	bool hovering = false;
	bool detail_hovering = false;
	PanelContainer *detail_panel = nullptr;
	VBoxContainer *detail_list = nullptr;

	void _rebuild_detail();
	void _sync_detail_visibility();
	int _current_step_index() const;
	bool _has_open_work() const;
	Size2 _chip_size() const;
	void _on_detail_mouse(bool p_entered);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;

	void set_plan(const String &p_explanation, const Array &p_plan);
	void clear_plan();
	bool is_visible_for_plan() const { return _has_open_work(); }
	Array get_plan() const { return plan.duplicate(true); }

	SolersPlanCapsule();
};

// Self-drawn 1px hairline vertical divider used between composer toolbar groups.
class SolersToolbarDivider : public Control {
	GDCLASS(SolersToolbarDivider, Control);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual Size2 get_minimum_size() const override;
	SolersToolbarDivider();
};

// Self-drawn rounded surface (background + crisp hairline border + optional
// soft shadow). The border is rendered with the "two stacked fills" technique
// (opaque outer fill, inset background fill) so the corners stay perfectly
// even — no white anti-aliasing bloom like a StyleBoxFlat border ring. Acts as
// a MarginContainer so a single child lays out inside the padding.
class SolersSurface : public MarginContainer {
	GDCLASS(SolersSurface, MarginContainer);

	Color bg = Color(0, 0, 0, 0);
	Color border_opaque = Color(0, 0, 0, 0);
	bool has_border = false;
	bool shadow = false;
	float radius = 16.0f;
	float border_w = 1.0f;

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	void configure(const Color &p_bg, const Color &p_border, float p_radius, int p_padding, bool p_shadow = false, float p_border_width = 1.0f);
	void set_surface_colors(const Color &p_bg, const Color &p_border);

	SolersSurface();
};

// Shared Solers chrome: composer / chip / user-bubble (one authority for colors).
inline Color solers_composer_bg() {
	return Color(0.086f, 0.088f, 0.092f);
}
inline Color solers_cell_bubble_bg() {
	return solers_composer_bg().lightened(0.04f);
}
// Same RGB as Tokens.hairline; slightly stronger alpha for the composer card edge.
inline Color solers_composer_border() {
	return Color(0.95f, 0.95f, 0.97f, 0.16f);
}

// One UI table: ui_kind wire string → glyph + localized verb (TTR).
StringName solers_tool_glyph_for_ui_kind(const String &p_ui_kind);
String solers_tool_verb_for_ui_kind(const String &p_ui_kind);
String solers_tool_verb_for_glyph(const StringName &p_glyph);

// Bare search field (mention / model / provider pickers) — Cursor-flat, no chrome.
void solers_style_bare_search_line_edit(LineEdit *p_edit);

String solers_mention_chip_label(const Dictionary &p_mention);
float solers_mention_chip_width(const String &p_label, const Ref<Font> &p_font, int p_font_size, bool p_has_icon);
void solers_draw_mention_chip(RID p_ci, const Rect2 &p_pill, const String &p_label, const Ref<Font> &p_font, int p_font_size, const Ref<Texture2D> &p_icon, const Color &p_text_color);
// Compact chip icon (provider logos + Solers UI fallbacks). Composer popup may
// still use richer path previews locally.
Ref<Texture2D> solers_mention_chip_icon(const Dictionary &p_mention, int p_px);
