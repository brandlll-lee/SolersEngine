/**************************************************************************/
/*  solers_schema_form.cpp                                                */
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

#include "solers_schema_form.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/string/translation_server.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/slider.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/text_edit.h"
Control *SolersSchemaForm::_create_field(const StringName &p_name, const Dictionary &p_schema, const Dictionary &p_extras, const Dictionary &p_presentation, const Variant &p_default) {
	const String type = p_schema.get("type", "string");
	const String enum_source = p_schema.get("enum_source", String());
	const Array enum_values = enum_source.is_empty() ? p_schema.get("enum", Array()) : p_extras.get(enum_source, Array());
	const String control_type = p_presentation.get("control", String());
	Control *field = nullptr;
	if (!enum_values.is_empty() && control_type == "segmented") {
		HBoxContainer *segments = memnew(HBoxContainer);
		Ref<ButtonGroup> group;
		group.instantiate();
		const String value_key = p_schema.get("enum_value", "id");
		const String label_key = p_schema.get("enum_label", "label");
		const Dictionary labels = p_presentation.get("labels", Dictionary());
		for (const Variant &value : enum_values) {
			Variant id = value;
			String label = labels.get(String(value), String(value).capitalize());
			if (value.get_type() == Variant::DICTIONARY) {
				const Dictionary item = value;
				id = item.get(value_key, item.get("value", Variant()));
				label = item.get(label_key, item.get("name", String(id)));
			}
			Button *segment = memnew(Button(label));
			segment->set_theme_type_variation(SNAME("SolersStudioSegment"));
			segment->set_toggle_mode(true);
			segment->set_button_group(group);
			segment->set_h_size_flags(SIZE_EXPAND_FILL);
			segment->set_meta("value", id);
			segment->set_pressed(id == p_default);
			segments->add_child(segment);
		}
		segments->set_meta("button_group", group);
		segments->set_meta("field_kind", "segmented");
		field = segments;
	} else if (!enum_values.is_empty()) {
		OptionButton *options = memnew(OptionButton);
		const String value_key = p_schema.get("enum_value", "id");
		const String label_key = p_schema.get("enum_label", "label");
		for (const Variant &value : enum_values) {
			Variant id = value;
			String label = String(value);
			if (value.get_type() == Variant::DICTIONARY) {
				const Dictionary item = value;
				id = item.get(value_key, item.get("value", Variant()));
				label = item.get(label_key, item.get("name", String(id)));
			}
			const int index = options->get_item_count();
			options->add_item(label);
			options->set_item_metadata(index, id);
			if (id == p_default) {
				options->select(index);
			}
		}
		if (p_default.get_type() == Variant::NIL) {
			options->select(-1);
		}
		field = options;
	} else if ((type == "integer" || type == "number") && control_type == "slider") {
		HBoxContainer *row = memnew(HBoxContainer);
		HSlider *slider = memnew(HSlider);
		SpinBox *number = memnew(SpinBox);
		const double minimum = p_presentation.get("minimum", p_schema.get("minimum", 0.0));
		const double maximum = p_presentation.get("maximum", p_schema.get("maximum", 100.0));
		const double step = type == "integer" ? 1.0 : (double)p_schema.get("multipleOf", 0.01);
		slider->set_min(minimum);
		slider->set_max(maximum);
		slider->set_step(step);
		slider->set_value(p_default.get_type() == Variant::NIL ? minimum : (double)p_default);
		slider->set_h_size_flags(SIZE_EXPAND_FILL);
		slider->share(number);
		number->set_custom_minimum_size(Size2(96, 0));
		row->add_child(slider);
		row->add_child(number);
		row->set_meta("field_kind", "slider");
		field = row;
	} else if (type == "boolean") {
		CheckBox *toggle = memnew(CheckBox);
		toggle->set_pressed((bool)p_default);
		field = toggle;
	} else if ((type == "integer" || type == "number") && p_default.get_type() != Variant::NIL) {
		SpinBox *number = memnew(SpinBox);
		number->set_min(p_schema.get("minimum", -1000000.0));
		number->set_max(p_schema.get("maximum", 1000000.0));
		const double step = type == "integer" ? 1.0 : (double)p_schema.get("multipleOf", 0.01);
		number->set_step(step);
		number->set_value(p_default);
		field = number;
	} else if (type == "object" || type == "array") {
		TextEdit *text = memnew(TextEdit);
		text->set_custom_minimum_size(Size2(0, 64));
		if (p_default.get_type() != Variant::NIL) {
			text->set_text(JSON::stringify(p_default));
		}
		field = text;
	} else {
		LineEdit *line = memnew(LineEdit);
		if (p_default.get_type() != Variant::NIL) {
			line->set_text(p_default);
		}
		if (type == "integer" || type == "number") {
			line->set_placeholder(type == "integer" ? TTRC("Integer") : TTRC("Number"));
		}
		field = line;
	}
	field->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	field->set_meta("schema", p_schema);
	return field;
}
void SolersSchemaForm::set_schema(const Dictionary &p_schema, const Dictionary &p_extras, const Dictionary &p_presentation) {
	fields.clear();
	while (get_child_count() > 0) {
		Node *child = get_child(0);
		remove_child(child);
		memdelete(child);
	}
	const Dictionary properties = p_schema.has("properties") ? Dictionary(p_schema["properties"]) : p_schema;
	Array names = p_schema.get("ui_order", p_presentation.get("order", Array()));
	Array remaining = properties.keys();
	remaining.sort();
	for (const Variant &name : remaining) {
		if (!names.has(name)) {
			names.push_back(name);
		}
	}
	const Dictionary controls = p_presentation.get("controls", Dictionary());
	for (const Variant &name_value : names) {
		const StringName name = name_value;
		if (!properties.has(name)) {
			continue;
		}
		const Dictionary property = properties[name];
		Label *label = memnew(Label);
		label->set_text(property.get("label", property.get("title", String(name).capitalize())));
		label->set_theme_type_variation(SNAME("SolersSessionMeta"));
		label->set_tooltip_text(property.get("description", String()));
		add_child(label);
		Control *field = _create_field(name, property, p_extras, controls.get(name, Dictionary()), property.get("default", Variant()));
		field->set_tooltip_text(label->get_tooltip_text());
		add_child(field);
		fields[name] = field;
	}
}
Dictionary SolersSchemaForm::get_values() const {
	Dictionary values;
	for (const KeyValue<StringName, Control *> &entry : fields) {
		Control *field = entry.value;
		const Dictionary property = field->get_meta("schema", Dictionary());
		const String type = property.get("type", "string");
		Variant value;
		const String field_kind = field->get_meta("field_kind", String());
		if (field_kind == "segmented") {
			const Ref<ButtonGroup> group = field->get_meta("button_group", Ref<ButtonGroup>());
			Button *pressed = group.is_valid() ? Object::cast_to<Button>(group->get_pressed_button()) : nullptr;
			if (!pressed) {
				continue;
			}
			value = pressed->get_meta("value", Variant());
		} else if (field_kind == "slider") {
			const HSlider *slider = Object::cast_to<HSlider>(field->get_child(0));
			value = type == "integer" ? Variant((int64_t)slider->get_value()) : Variant(slider->get_value());
		} else if (OptionButton *options = Object::cast_to<OptionButton>(field)) {
			value = options->get_selected_metadata();
		} else if (CheckBox *toggle = Object::cast_to<CheckBox>(field)) {
			value = toggle->is_pressed();
		} else if (SpinBox *number = Object::cast_to<SpinBox>(field)) {
			value = type == "integer" ? Variant((int64_t)number->get_value()) : Variant(number->get_value());
		} else if (TextEdit *text = Object::cast_to<TextEdit>(field)) {
			if (text->get_text().strip_edges().is_empty()) {
				continue;
			}
			value = JSON::parse_string(text->get_text());
			if (value.get_type() != (type == "array" ? Variant::ARRAY : Variant::DICTIONARY)) {
				continue;
			}
		} else if (LineEdit *line = Object::cast_to<LineEdit>(field)) {
			const String text = line->get_text().strip_edges();
			if (text.is_empty()) {
				continue;
			}
			value = type == "integer" ? Variant(text.to_int()) : type == "number" ? Variant(text.to_float())
																				  : Variant(text);
		}
		if (value.get_type() != Variant::NIL) {
			values[entry.key] = value;
		}
	}
	return values;
}
void SolersSchemaForm::set_values(const Dictionary &p_values) {
	for (const Variant *key = p_values.next(nullptr); key; key = p_values.next(key)) {
		Control *const *field_ptr = fields.getptr(StringName(*key));
		if (!field_ptr) {
			continue;
		}
		Control *field = *field_ptr;
		const Variant value = p_values[*key];
		const String field_kind = field->get_meta("field_kind", String());
		if (field_kind == "segmented") {
			for (int i = 0; i < field->get_child_count(); i++) {
				Button *segment = Object::cast_to<Button>(field->get_child(i));
				if (segment && segment->get_meta("value", Variant()) == value) {
					segment->set_pressed(true);
					break;
				}
			}
		} else if (field_kind == "slider") {
			Object::cast_to<HSlider>(field->get_child(0))->set_value(value);
		} else if (OptionButton *options = Object::cast_to<OptionButton>(field)) {
			for (int i = 0; i < options->get_item_count(); i++) {
				if (options->get_item_metadata(i) == value) {
					options->select(i);
					break;
				}
			}
		} else if (CheckBox *toggle = Object::cast_to<CheckBox>(field)) {
			toggle->set_pressed(value);
		} else if (SpinBox *number = Object::cast_to<SpinBox>(field)) {
			number->set_value(value);
		} else if (TextEdit *text = Object::cast_to<TextEdit>(field)) {
			text->set_text(JSON::stringify(value));
		} else if (LineEdit *line = Object::cast_to<LineEdit>(field)) {
			line->set_text(value);
		}
	}
}
