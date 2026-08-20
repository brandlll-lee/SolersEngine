/**************************************************************************/
/*  solers_ui_theme.h                                                     */
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
		Color primary;
		Color primary_hover;
		Color primary_pressed;
		Color on_primary;

		Color home_tile; // PM home action tiles.
		Color home_tile_hover;
		Color home_tile_pressed;

		int radius_panel = 0;
		int radius_control = 2;
		int radius_home_tile = 12;
		int radius_list_thumb = 6;
	};

	static Tokens make_tokens();

	// Complete Solers subtree theme. Godot owns control behavior; this theme
	// owns the visual contract without changing the editor's global theme.
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
