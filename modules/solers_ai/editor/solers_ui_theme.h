/**************************************************************************/
/*  solers_ui_theme.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Solers-owned typography and visual tokens. Godot remains responsible   */
/* for shaping, BiDi, rasterization and native Control behavior.           */
/**************************************************************************/

#pragma once

#include "scene/resources/theme.h"

class AcceptDialog;

class SolersUITheme {
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
		Color hairline; // Cursor-flat pane/title edges (light on deep bg).
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

	static Tokens make_tokens();

	// Sparse subtree theme: explicit Solers fonts, ambient Godot styles.
	static Ref<Theme> create();

	// Cursor-flat pane edges for ANY Solers-themed tree (Editor + PM + Dock):
	// SplitContainer / HSeparator / VSeparator + Editor/hairline color.
	// New splits need zero call-site styling — theme is the authority.
	static void apply_chrome_edges(const Ref<Theme> &p_theme, const Color &p_hairline);

	// Retint Window embedded_border + AcceptDialog panel fill to p_chrome.
	// Title/body join is NOT here — see apply_chrome_edges (hairline token) +
	// EditorTitleBar / Viewport (one draw recipe, two Godot chrome hosts).
	static void apply_window_chrome(const Ref<Theme> &p_theme, const Color &p_chrome);

	// Settings host: variation + hide OK bar. Uses apply_window_chrome /
	// apply_chrome_edges — same tokens as every other AcceptDialog.
	static void configure_settings_host(AcceptDialog *p_dialog, const Ref<Theme> &p_theme);

};
