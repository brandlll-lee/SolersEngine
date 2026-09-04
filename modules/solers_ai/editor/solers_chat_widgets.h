/**************************************************************************/
/*  solers_chat_widgets.h                                                 */
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

class TextEdit;

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
	static Ref<Texture2D> brand_mark();
	static void clear_cache();
};

class SolersActivityIndicator : public Control {
	GDCLASS(SolersActivityIndicator, Control);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	SolersActivityIndicator();
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

class SolersContextRing : public Control {
	GDCLASS(SolersContextRing, Control);

	int64_t used_tokens = 0;
	int64_t total_tokens = 0;
	float usage_ratio = -1.0f;
	bool hovering = false;

	static String _format_tokens(int64_t p_tokens);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	void set_usage(const Dictionary &p_usage);
	float get_usage_ratio() const { return usage_ratio; }

	SolersContextRing();
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
	bool dashed_border = false;
	bool hover_accent = false;
	bool hovered = false;
	bool shadow = false;
	float radius = 16.0f;
	float border_w = 1.0f;

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	void configure(const Color &p_bg, const Color &p_border, float p_radius, int p_padding, bool p_shadow = false, float p_border_width = 1.0f);
	void set_dashed_border(bool p_enabled) {
		dashed_border = p_enabled;
		queue_redraw();
	}
	void set_hover_accent(bool p_enabled) { hover_accent = p_enabled; }
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

void solers_configure_prompt_surface(SolersSurface *p_surface);
void solers_configure_prompt_text_edit(TextEdit *p_edit);

// Bare search field (mention / model / provider pickers) — Cursor-flat, no chrome.
void solers_style_bare_search_line_edit(LineEdit *p_edit);

String solers_mention_chip_label(const Dictionary &p_mention);
float solers_mention_chip_width(const String &p_label, const Ref<Font> &p_font, int p_font_size, bool p_has_icon);
void solers_draw_mention_chip(RID p_ci, const Rect2 &p_pill, const String &p_label, const Ref<Font> &p_font, int p_font_size, const Ref<Texture2D> &p_icon, const Color &p_text_color);
// Compact chip icon (provider logos + Solers UI fallbacks). Composer popup may
// still use richer path previews locally.
Ref<Texture2D> solers_mention_chip_icon(const Dictionary &p_mention, int p_px);
Ref<Texture2D> solers_attachment_texture(const Dictionary &p_attachment);
