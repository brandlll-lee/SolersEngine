/**************************************************************************/
/*  solers_asset_grid.h                                                   */
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

#include "core/variant/callable.h"
#include "core/variant/dictionary.h"
#include "scene/gui/scroll_container.h"

class GridContainer;
class Texture2D;

class SolersAssetGrid : public ScrollContainer {
	GDCLASS(SolersAssetGrid, ScrollContainer);

	GridContainer *grid = nullptr;
	Callable selected_callback;
	Callable menu_callback;
	String selected_id;
	int asset_count = 0;

	void _asset_pressed(const String &p_asset_id);
	void _asset_menu_pressed(const String &p_asset_id, Control *p_anchor);
	void _update_columns();

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	void clear_assets();
	void add_asset(const Dictionary &p_manifest, const Ref<Texture2D> &p_preview);
	void set_selected_asset(const String &p_asset_id);
	void set_callbacks(const Callable &p_selected, const Callable &p_menu);
	int get_asset_count() const { return asset_count; }

	SolersAssetGrid();
};
