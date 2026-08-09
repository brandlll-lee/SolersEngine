/**************************************************************************/
/*  solers_test_state.h                                                   */
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

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"

inline Dictionary solers_test_find_dictionary(const Array &p_items, const StringName &p_field, const String &p_value) {
	for (const Variant &item_variant : p_items) {
		const Dictionary item = item_variant;
		if (String(item.get(p_field, String())) == p_value) {
			return item;
		}
	}
	return Dictionary();
}

class SolersTestPaths {
	Vector<String> paths;

	static void _remove(const String &p_path) {
		const String absolute = ProjectSettings::get_singleton()->globalize_path(p_path);
		if (DirAccess::dir_exists_absolute(absolute)) {
			if (Ref<DirAccess> dir = DirAccess::open(absolute); dir.is_valid()) {
				dir->erase_contents_recursive();
			}
		}
		DirAccess::remove_absolute(absolute);
	}

public:
	~SolersTestPaths() {
		for (int i = paths.size() - 1; i >= 0; i--) {
			_remove(paths[i]);
		}
	}

	void add(const String &p_path) {
		_remove(p_path);
		paths.push_back(p_path);
	}
};
