/**************************************************************************/
/*  solers_editor_operator.h                                              */
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

#include "core/object/object.h"
#include "core/variant/dictionary.h"

class Node;

class SolersEditorOperator : public Object {
	GDCLASS(SolersEditorOperator, Object);

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	Node *_resolve_node(const String &p_node_path, String &r_error) const;

protected:
	static void _bind_methods();

public:
	Dictionary get_node_properties(const Dictionary &p_args);
	Dictionary add_node(const Dictionary &p_args);
	Dictionary reparent_node(const Dictionary &p_args);
	Dictionary set_node_properties(const Dictionary &p_args);
	Dictionary remove_node(const Dictionary &p_args);
	Dictionary attach_script(const Dictionary &p_args);
	Dictionary connect_signal(const Dictionary &p_args);
	Dictionary list_signal_connections(const Dictionary &p_args);
	Dictionary create_scene(const Dictionary &p_args);
	Dictionary open_scene(const Dictionary &p_args);
	Dictionary save_current_scene(const Dictionary &p_args);
	Dictionary save_scene_as(const Dictionary &p_args);
	Dictionary play_current_scene(const Dictionary &p_args);
	Dictionary stop_playing_scene(const Dictionary &p_args);
	Dictionary capture_editor_screenshot(const Dictionary &p_args);
	Dictionary rollback_last_editor_action(const Dictionary &p_args);
};
