/**************************************************************************/
/*  solers_schema_form.h                                                  */
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
#include "scene/gui/box_container.h"
class Image;
class InputEvent;
class SolersSchemaForm : public VBoxContainer {
	GDCLASS(SolersSchemaForm, VBoxContainer);

	HashMap<StringName, Control *> fields;
	Callable image_stager;
	Control *_create_field(const StringName &p_name, const Dictionary &p_schema, const Dictionary &p_extras, const Dictionary &p_presentation, const Variant &p_default);
	void _image_gui_input(const Ref<InputEvent> &p_event, Control *p_field);
	bool _can_drop_image(const Point2 &, const Variant &p_data, Control *) const;
	void _drop_image(const Point2 &, const Variant &p_data, Control *p_field);
	void _replace_images(const PackedStringArray &p_files, Control *p_field);
	void _append_image(Control *p_field, const Ref<Image> &p_image);

protected:
	static void _bind_methods() {}

public:
	void set_image_stager(const Callable &p_stager) { image_stager = p_stager; }
	void set_schema(const Dictionary &p_schema, const Dictionary &p_extras = Dictionary(), const Dictionary &p_presentation = Dictionary());
	Dictionary get_values() const;
	void set_values(const Dictionary &p_values);
};
