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

#include "solers_chat_widgets.h"
#include "solers_ui_theme.h"

#include "core/input/input_event.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/object.h"
#include "core/string/translation_server.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/check_button.h"
#include "scene/gui/file_dialog.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/slider.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/text_edit.h"
#include "scene/resources/image_texture.h"
#include "servers/display/display_server.h"

void SolersSchemaForm::_image_staged(const Dictionary &p_result, const Ref<Image> &p_image, ObjectID p_field_id, ObjectID p_pick_id, uint64_t p_generation, int p_slot) {
	Control *field = Object::cast_to<Control>(ObjectDB::get_instance(p_field_id));
	Button *pick = Object::cast_to<Button>(ObjectDB::get_instance(p_pick_id));
	if (!field || !pick || (uint64_t)field->get_meta("image_generation", 0) != p_generation) {
		return;
	}
	const String path = Dictionary(p_result.get("data", Dictionary())).get("local_path", String());
	if (path.is_empty()) {
		return;
	}
	Array values = field->get_meta("image_values", Array());
	if (values.size() <= p_slot) {
		values.resize(p_slot + 1);
	}
	values[p_slot] = path;
	field->set_meta("image_values", values);
	int count = 0;
	for (const Variant &value : values) {
		count += !String(value).is_empty();
	}
	pick->set_text(count == 1 ? TTRC("1 reference image") : vformat(TTRC("%d reference images"), count));
	pick->set_expand_icon(true);
	pick->set_button_icon(ImageTexture::create_from_image(p_image));
}

void SolersSchemaForm::_replace_images(const PackedStringArray &p_files, Control *p_field, Button *p_pick) {
	const uint64_t generation = (uint64_t)p_field->get_meta("image_generation", 0) + 1;
	p_field->set_meta("image_generation", generation);
	p_field->set_meta("image_values", Array());
	p_pick->set_text(TTRC("Add reference image"));
	p_pick->set_expand_icon(false);
	p_pick->set_button_icon(SolersIcons::get(SNAME("tool_capture"), int(24 * EDSCALE)));
	const Dictionary schema = p_field->get_meta("schema", Dictionary());
	const int limit = schema.get("type", "string") == "array" ? (int)schema.get("maxItems", 4) : 1;
	for (int i = 0; i < MIN(p_files.size(), limit); i++) {
		image_stager.call(p_files[i], callable_mp(this, &SolersSchemaForm::_image_staged).bind(p_field->get_instance_id(), p_pick->get_instance_id(), generation, i));
	}
}

void SolersSchemaForm::_image_gui_input(const Ref<InputEvent> &p_event, Control *p_field, Button *p_pick) {
	const Ref<InputEventKey> key = p_event;
	DisplayServer *display = DisplayServer::get_singleton();
	if (p_pick->is_hovered() && key.is_valid() && key->is_pressed() && !key->is_echo() && key->get_keycode() == Key::V && key->is_command_or_control_pressed() && display && display->clipboard_has_image()) {
		const uint64_t generation = (uint64_t)p_field->get_meta("image_generation", 0) + 1;
		p_field->set_meta("image_generation", generation);
		p_field->set_meta("image_values", Array());
		image_stager.call(display->clipboard_get_image(), callable_mp(this, &SolersSchemaForm::_image_staged).bind(p_field->get_instance_id(), p_pick->get_instance_id(), generation, 0));
		accept_event();
	}
}

bool SolersSchemaForm::_can_drop_image(const Point2 &, const Variant &p_data, Control *) const {
	return String(Dictionary(p_data).get("type", String())) == "files";
}

void SolersSchemaForm::_drop_image(const Point2 &, const Variant &p_data, Control *p_field, Button *p_pick) {
	_replace_images(Dictionary(p_data).get("files", PackedStringArray()), p_field, p_pick);
}

Control *SolersSchemaForm::_create_field(const StringName &p_name, const Dictionary &p_schema, const Dictionary &p_extras, const Dictionary &p_presentation, const Variant &p_default) {
	const String type = p_schema.get("type", "string");
	const String enum_source = p_schema.get("enum_source", String());
	const String control_type = p_presentation.get("control", String());
	Array enum_values = enum_source.is_empty() ? p_schema.get("enum", Array()) : p_extras.get(enum_source, Array());
	if (control_type == "multi_select") {
		enum_values = Dictionary(p_schema.get("items", Dictionary())).get("enum", enum_values);
	}
	Control *field = nullptr;
	if (!enum_values.is_empty() && (control_type == "segmented" || control_type == "multi_select")) {
		HBoxContainer *segments = memnew(HBoxContainer);
		Ref<ButtonGroup> group;
		if (control_type == "segmented") {
			group.instantiate();
		}
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
			if (group.is_valid()) {
				segment->set_button_group(group);
			}
			segment->set_h_size_flags(SIZE_EXPAND_FILL);
			segment->set_meta("value", id);
			segment->set_pressed(control_type == "multi_select" ? p_default.get_type() == Variant::ARRAY && Array(p_default).has(id) : id == p_default);
			segments->add_child(segment);
		}
		segments->set_meta("button_group", group);
		segments->set_meta("field_kind", control_type);
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
		CheckButton *toggle = memnew(CheckButton);
		toggle->set_text(p_schema.get("label", p_schema.get("title", String(p_name).capitalize())));
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
	} else if (control_type == "image") {
		const SolersUITheme::Tokens tokens = SolersUITheme::make_tokens();
		SolersSurface *well = memnew(SolersSurface);
		well->configure(tokens.card, tokens.border, tokens.radius_home_tile, 0, false);
		well->set_dashed_border(true);
		well->set_hover_accent(true);
		well->set_custom_minimum_size(Size2(0, 96 * EDSCALE));
		Button *pick = memnew(Button(TTRC("Add reference image")));
		FileDialog *dialog = memnew(FileDialog);
		dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
		dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILES);
		well->add_child(dialog, false, INTERNAL_MODE_FRONT);
		dialog->connect(SNAME("files_selected"), callable_mp(this, &SolersSchemaForm::_replace_images).bind(well, pick));
		pick->set_flat(true);
		pick->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		pick->set_button_icon(SolersIcons::get(SNAME("tool_capture"), int(24 * EDSCALE)));
		pick->connect(SceneStringName(pressed), callable_mp(dialog, &FileDialog::popup_file_dialog));
		pick->connect(SceneStringName(mouse_entered), callable_mp((Control *)pick, &Control::grab_focus).bind(true));
		pick->connect(SceneStringName(gui_input), callable_mp(this, &SolersSchemaForm::_image_gui_input).bind(well, pick));
		pick->set_drag_forwarding(Callable(), callable_mp(this, &SolersSchemaForm::_can_drop_image).bind(well), callable_mp(this, &SolersSchemaForm::_drop_image).bind(well, pick));
		well->add_child(pick);
		well->set_meta("field_kind", "image");
		field = well;
	} else if (control_type == "multiline" || type == "object" || type == "array") {
		TextEdit *text = memnew(TextEdit);
		text->set_custom_minimum_size(Size2(0, control_type == "multiline" ? 92 : 64));
		if (control_type == "multiline") {
			text->set_theme_type_variation(SNAME("SolersStudioPrompt"));
			text->set_meta("field_kind", "multiline");
		}
		if (p_default.get_type() != Variant::NIL) {
			text->set_text(control_type == "multiline" ? String(p_default) : JSON::stringify(p_default));
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
		Control *field = _create_field(name, property, p_extras, controls.get(name, Dictionary()), property.get("default", Variant()));
		const String description = property.get("description", String());
		field->set_tooltip_text(description);
		if (String(property.get("type", "string")) != "boolean") {
			Label *label = memnew(Label(property.get("label", property.get("title", String(name).capitalize()))));
			label->set_theme_type_variation(SNAME("SolersSessionMeta"));
			label->set_tooltip_text(description);
			add_child(label);
		}
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
		} else if (field_kind == "multi_select") {
			Array selected;
			for (int i = 0; i < field->get_child_count(); i++) {
				Button *segment = Object::cast_to<Button>(field->get_child(i));
				if (segment && segment->is_pressed()) {
					selected.push_back(segment->get_meta("value", Variant()));
				}
			}
			value = selected;
		} else if (field_kind == "image") {
			Array images;
			for (const Variant &image : Array(field->get_meta("image_values", Array()))) {
				if (!String(image).is_empty()) {
					images.push_back(image);
				}
			}
			if (images.is_empty()) {
				continue;
			}
			value = type == "array" ? Variant(images) : images[0];
		} else if (field_kind == "slider") {
			const HSlider *slider = Object::cast_to<HSlider>(field->get_child(0));
			value = type == "integer" ? Variant((int64_t)slider->get_value()) : Variant(slider->get_value());
		} else if (OptionButton *options = Object::cast_to<OptionButton>(field)) {
			value = options->get_selected_metadata();
		} else if (CheckButton *toggle = Object::cast_to<CheckButton>(field)) {
			value = toggle->is_pressed();
		} else if (SpinBox *number = Object::cast_to<SpinBox>(field)) {
			value = type == "integer" ? Variant((int64_t)number->get_value()) : Variant(number->get_value());
		} else if (TextEdit *text = Object::cast_to<TextEdit>(field)) {
			if (text->get_text().strip_edges().is_empty()) {
				continue;
			}
			value = field_kind == "multiline" ? Variant(text->get_text().strip_edges()) : JSON::parse_string(text->get_text());
			if (field_kind != "multiline" && value.get_type() != (type == "array" ? Variant::ARRAY : Variant::DICTIONARY)) {
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
		} else if (field_kind == "multi_select") {
			for (int i = 0; i < field->get_child_count(); i++) {
				Button *segment = Object::cast_to<Button>(field->get_child(i));
				segment->set_pressed(Array(value).has(segment->get_meta("value", Variant())));
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
		} else if (CheckButton *toggle = Object::cast_to<CheckButton>(field)) {
			toggle->set_pressed(value);
		} else if (SpinBox *number = Object::cast_to<SpinBox>(field)) {
			number->set_value(value);
		} else if (TextEdit *text = Object::cast_to<TextEdit>(field)) {
			text->set_text(field_kind == "multiline" ? String(value) : JSON::stringify(value));
		} else if (LineEdit *line = Object::cast_to<LineEdit>(field)) {
			line->set_text(value);
		}
	}
}
