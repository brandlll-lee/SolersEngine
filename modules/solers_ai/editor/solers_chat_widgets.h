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
/* Icon glyphs are Lucide geometry (MIT, see UI_ICON_LICENSE.txt),        */
/* rasterized once per (name, size) as white strokes and tinted at draw   */
/* time via modulate — hover color blends never re-rasterize.             */
/**************************************************************************/

#pragma once

#include "core/variant/callable.h"
#include "scene/gui/control.h"
#include "scene/gui/margin_container.h"
#include "scene/resources/texture.h"

// One-shot Lucide glyph rasterizer with a process-lifetime texture cache.
class SolersChatGlyphs {
public:
	// Returns a white-stroke glyph texture at p_size_px physical pixels.
	// Tint it with CanvasItem draw modulate; never bake colors into the cache.
	static Ref<Texture2D> get(const StringName &p_name, int p_size_px, float p_stroke_width = 1.7f);
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
// Used for the access selector ("Full access", accent-tinted) and the
// model/effort selector ("5.5  Extra High"). Hugs its content width.
class SolersSelectChip : public Control {
	GDCLASS(SolersSelectChip, Control);

	StringName glyph; // Optional leading glyph (empty -> none).
	String strong_text;
	String muted_text;
	Color accent = Color(0, 0, 0, 0); // Transparent means neutral gray ramp.
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
	void set_accent(const Color &p_accent);
	void set_texts(const String &p_strong, const String &p_muted);
	void set_show_chevron(bool p_show);
	void set_pressed_callback(const Callable &p_cb) { pressed_callback = p_cb; }

	SolersSelectChip();
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
