/**************************************************************************/
/*  solers_pm_theme.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* This file introduces a dedicated, self-contained design-token theme    */
/* layer for the Project Manager. It is applied as an *overlay* on top of  */
/* the theme produced by EditorThemeManager::generate_theme(), so the      */
/* shared editor theme generator is never modified. The overlay restyles   */
/* only Project-Manager-scoped theme entries (its own backdrop, panels,    */
/* list rows and top view toggles) to achieve an Unreal-Engine-grade       */
/* dark, minimal and textured look while preserving all behavior.          */
/**************************************************************************/

#pragma once

#include "scene/resources/texture.h"
#include "scene/resources/theme.h"

class AcceptDialog;

// Design-token + StyleBox set for the Solers Project Manager.
//
// All values are derived from the *already generated* editor theme (so the
// user's accent color and light/dark preference are respected), then biased
// toward a deep, neutral, UE-style graphite palette with subtle borders and
// rounded surfaces. Nothing here touches application logic.
class SolersPMTheme {
public:
	// Resolved palette + geometry for one theme generation.
	struct Tokens {
		Color bg; // Window backdrop (deepest).
		Color surface; // Outer content panel.
		Color card; // Elevated surfaces.
		Color card_hover;
		Color card_selected;
		Color border;
		Color border_strong;
		Color accent;
		Color text;
		Color text_dim;

		Color home_tile; // PM home action tiles.
		Color home_tile_hover;
		Color home_tile_pressed;

		int radius_panel = 0;
		int radius_control = 2;
		int radius_home_tile = 12;
		int radius_list_thumb = 6;
	};

	// Compute the token set from a generated theme (pure, no side effects).
	static Tokens make_tokens(const Ref<Theme> &p_theme);

	// Apply the Solers Project Manager theme overlay in-place.
	// Safe to call repeatedly (idempotent) after every theme (re)generation.
	static void apply(const Ref<Theme> &p_theme);

	// Flat settings host: one Solers fill, no AcceptDialog picture-frame margins.
	static void configure_settings_host(AcceptDialog *p_dialog);

	// Convert a (possibly colored) editor icon into a neutral monochrome glyph.
	// Unreal's chrome is strictly grayscale; stock editor SVGs like the red
	// Heart or the gold Favorites star would inject cartoon color into it.
	static Ref<Texture2D> mono_icon(const Ref<Texture2D> &p_icon);

	// Rasterize a Lucide glyph (24x24 viewBox inner SVG body) as a white stroke
	// icon at p_size_px logical pixels. Drawn white so callers tint it freely
	// via modulate (idle gray / selected white). Returns null without MODULE_SVG.
	static Ref<Texture2D> lucide_icon(const char *p_svg_body, int p_size_px = 18, float p_stroke_width = 1.8f);
};
