/**************************************************************************/
/*  solers_resource_service.h                                             */
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

#include "core/io/resource.h"
#include "core/object/object.h"
#include "core/variant/dictionary.h"

bool solers_coerce_property_value(Object *p_object, const StringName &p_property, const Variant &p_value, Variant &r_out, String &r_error);
bool solers_coerce_variant_value(const PropertyInfo &p_info, const Variant &p_value, Variant &r_out, String &r_error);
bool solers_decode_wire_variant(const Variant &p_value, Variant &r_out, String &r_error);
Dictionary solers_variant_wire_shape(Variant::Type p_type);
Dictionary solers_native_object_handle(Object *p_object);
Variant solers_summarize_display_value(const Variant &p_value);
PackedStringArray solers_nearest_names(const String &p_needle, const PackedStringArray &p_candidates, int p_max = 5);
String solers_property_suggestions(Object *p_object, const String &p_property);
String solers_class_suggestions(const String &p_class_name);
String solers_file_suggestions(const String &p_res_path);

class SolersResourceService : public Object {
	GDCLASS(SolersResourceService, Object);

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	bool _normalize_project_path(const String &p_path, String &r_res_path, String &r_error) const;
	bool _resolve_native_object(const Variant &p_object_id, Object *&r_object, String &r_error) const;
	Dictionary _read_method(Object *p_object, const StringName &p_method) const;
	String _export_filter_to_string(int p_filter) const;
	String _script_export_mode_to_string(int p_mode) const;
	String _export_message_type_to_string(int p_type) const;

protected:
	static void _bind_methods();

public:
	Dictionary get_resource_info(const Dictionary &p_args) const;
	Dictionary inspect_resource(const Dictionary &p_args) const;
	Dictionary create_resource(const Dictionary &p_args) const;
	Dictionary set_resource_property(const Dictionary &p_args) const;
	Dictionary native_list_properties(const Dictionary &p_args) const;
	Dictionary native_get(const Dictionary &p_args) const;
	Dictionary list_export_presets(const Dictionary &p_args) const;
	Dictionary validate_export_presets(const Dictionary &p_args) const;
	Dictionary run_export_preset(const Dictionary &p_args) const;
};
