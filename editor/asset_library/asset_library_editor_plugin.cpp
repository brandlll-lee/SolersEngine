/**************************************************************************/
/*  asset_library_editor_plugin.cpp                                       */
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

#include "asset_library_editor_plugin.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/stream_peer_tls.h"
#include "core/input/input_event.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/file_system/editor_paths.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/settings/editor_settings.h"
#include "editor/settings/project_settings_editor.h"
#include "editor/themes/editor_scale.h"
#include "modules/gltf/extensions/gltf_document_extension_convert_importer_mesh.h"
#include "modules/gltf/gltf_document.h"
#include "modules/gltf/gltf_state.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/importer_mesh_instance_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/animation/animation_player.h"
#include "scene/gui/color_rect.h"
#include "scene/gui/flow_container.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/separator.h"
#include "scene/gui/subviewport_container.h"
#include "scene/main/viewport.h"
#include "scene/resources/environment.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/animation.h"
#include "scene/resources/immediate_mesh.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/sky.h"
#include "scene/resources/style_box_flat.h"

static inline void setup_http_request(HTTPRequest *request) {
	request->set_use_threads(EDITOR_GET("asset_library/use_threads"));

	const String proxy_host = EDITOR_GET("network/http_proxy/host");
	const int proxy_port = EDITOR_GET("network/http_proxy/port");
	request->set_http_proxy(proxy_host, proxy_port);
	request->set_https_proxy(proxy_host, proxy_port);
}

enum {
	ASSET_SOURCE_PICKER,
	ASSET_SOURCE_GODOT,
	ASSET_SOURCE_SOLERS,
};

enum {
	SOLERS_KIND_FILTER_ALL,
	SOLERS_KIND_FILTER_3D,
	SOLERS_KIND_FILTER_SFX,
	SOLERS_KIND_FILTER_MUSIC,
};

enum {
	SOLERS_STATUS_FILTER_ALL,
	SOLERS_STATUS_FILTER_READY,
	SOLERS_STATUS_FILTER_DRAFT,
	SOLERS_STATUS_FILTER_GENERATING,
	SOLERS_STATUS_FILTER_FAILED,
};

enum {
	SOLERS_DETAIL_VIEW_FINAL,
	SOLERS_DETAIL_VIEW_LIGHTING,
	SOLERS_DETAIL_VIEW_UNSHADED,
	SOLERS_DETAIL_VIEW_WIREFRAME,
	SOLERS_DETAIL_VIEW_NORMALS,
	SOLERS_DETAIL_VIEW_SKELETON,
};

static String solers_kind_filter_value_from_id(int p_id) {
	switch (p_id) {
		case SOLERS_KIND_FILTER_3D:
			return "3d";
		case SOLERS_KIND_FILTER_SFX:
			return "sfx";
		case SOLERS_KIND_FILTER_MUSIC:
			return "music";
		default:
			return String();
	}
}

static int solers_kind_filter_id_from_value(const String &p_value) {
	if (p_value == "3d") {
		return SOLERS_KIND_FILTER_3D;
	}
	if (p_value == "sfx") {
		return SOLERS_KIND_FILTER_SFX;
	}
	if (p_value == "music") {
		return SOLERS_KIND_FILTER_MUSIC;
	}
	return SOLERS_KIND_FILTER_ALL;
}

static String solers_kind_filter_label(const String &p_value) {
	switch (solers_kind_filter_id_from_value(p_value)) {
		case SOLERS_KIND_FILTER_3D:
			return TTRC("3D Models");
		case SOLERS_KIND_FILTER_SFX:
			return TTRC("Sound Effects");
		case SOLERS_KIND_FILTER_MUSIC:
			return TTRC("Music");
		default:
			return TTRC("All Assets");
	}
}

static String solers_status_filter_value_from_id(int p_id) {
	switch (p_id) {
		case SOLERS_STATUS_FILTER_READY:
			return "ready";
		case SOLERS_STATUS_FILTER_DRAFT:
			return "draft";
		case SOLERS_STATUS_FILTER_GENERATING:
			return "generating";
		case SOLERS_STATUS_FILTER_FAILED:
			return "failed";
		default:
			return String();
	}
}

static int solers_status_filter_id_from_value(const String &p_value) {
	if (p_value == "ready") {
		return SOLERS_STATUS_FILTER_READY;
	}
	if (p_value == "draft") {
		return SOLERS_STATUS_FILTER_DRAFT;
	}
	if (p_value == "generating") {
		return SOLERS_STATUS_FILTER_GENERATING;
	}
	if (p_value == "failed") {
		return SOLERS_STATUS_FILTER_FAILED;
	}
	return SOLERS_STATUS_FILTER_ALL;
}

static String solers_status_filter_label(const String &p_value) {
	switch (solers_status_filter_id_from_value(p_value)) {
		case SOLERS_STATUS_FILTER_READY:
			return TTRC("Ready");
		case SOLERS_STATUS_FILTER_DRAFT:
			return TTRC("Draft");
		case SOLERS_STATUS_FILTER_GENERATING:
			return TTRC("Generating");
		case SOLERS_STATUS_FILTER_FAILED:
			return TTRC("Failed");
		default:
			return TTRC("All Status");
	}
}

static bool solers_asset_status_matches(const String &p_filter, const String &p_status) {
	if (p_filter.is_empty()) {
		return true;
	}
	if (p_filter == "generating") {
		return p_status == "queued" || p_status == "running";
	}
	return p_status == p_filter;
}

static bool solers_asset_is_draft(const Dictionary &p_manifest) {
	if (String(p_manifest.get("status", String())).to_lower() == "draft") {
		return true;
	}
	if (String(p_manifest.get("kind", String())).to_lower() != "3d" || String(p_manifest.get("provider", String())).to_lower() != "meshy") {
		return false;
	}
	if (p_manifest.has("refine_task_id")) {
		return false;
	}
	const Dictionary provider_response = p_manifest.get("provider_response", Dictionary());
	return String(provider_response.get("type", String())).to_lower() == "text-to-3d-preview";
}

static String solers_asset_display_status(const Dictionary &p_manifest) {
	if (solers_asset_is_draft(p_manifest)) {
		return "draft";
	}
	return String(p_manifest.get("status", "unknown")).to_lower();
}

static Color solers_asset_status_color(const String &p_status) {
	if (p_status == "ready") {
		return Color(0.35f, 0.88f, 0.58f);
	}
	if (p_status == "draft") {
		return Color(0.85f, 0.70f, 0.42f);
	}
	if (p_status == "failed") {
		return Color(0.95f, 0.36f, 0.36f);
	}
	return Color(0.35f, 0.67f, 1.0f);
}

static String solers_asset_group_status(const Array &p_asset_ids, const Dictionary &p_manifests_by_id) {
	bool has_ready = false;
	bool has_failed = false;
	for (int i = 0; i < p_asset_ids.size(); i++) {
		const Dictionary manifest = p_manifests_by_id.get(p_asset_ids[i], Dictionary());
		const String status = solers_asset_display_status(manifest);
		if (status == "queued" || status == "running") {
			return status;
		}
		has_ready = has_ready || status == "ready" || status == "draft";
		has_failed = has_failed || status == "failed";
	}
	if (has_ready) {
		return "ready";
	}
	if (has_failed) {
		return "failed";
	}
	return "unknown";
}

static String solers_asset_version_label(const String &p_asset_id, const Dictionary &p_manifest, const String &p_root_asset_id) {
	if (p_asset_id == p_root_asset_id) {
		return TTRC("Static");
	}
	const String label = String(p_manifest.get("operation_label", String())).strip_edges();
	if (!label.is_empty()) {
		const Dictionary options = p_manifest.get("provider_options", Dictionary());
		const Variant action = options.get("action_id", Variant());
		if (action.get_type() != Variant::NIL) {
			return vformat("%s %s", label, String(action));
		}
		return label;
	}
	return String(p_manifest.get("name", p_asset_id));
}

static Label *solers_make_label(const String &p_text, int p_font_size, const Color &p_color, bool p_wrap = false) {
	Label *label = memnew(Label(p_text));
	label->add_theme_font_size_override(SceneStringName(font_size), p_font_size * EDSCALE);
	label->add_theme_color_override(SceneStringName(font_color), p_color);
	label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	if (p_wrap) {
		label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		label->set_text_overrun_behavior(TextServer::OVERRUN_NO_TRIMMING);
	}
	return label;
}

static void solers_add_info_row(VBoxContainer *p_parent, const String &p_label, const String &p_value) {
	if (p_value.is_empty()) {
		return;
	}

	Label *caption = solers_make_label(p_label, 11, Color(0.58f, 0.64f, 0.74f));
	p_parent->add_child(caption);
	Label *value = solers_make_label(p_value, 13, Color(0.84f, 0.88f, 0.94f), true);
	p_parent->add_child(value);
}

static void solers_add_inspector_section(VBoxContainer *p_parent, const String &p_text) {
	Label *label = solers_make_label(p_text, 12, Color(0.94f, 0.95f, 0.92f));
	p_parent->add_child(label);
}

static Button *solers_make_viewer_button(const String &p_text) {
	Button *button = memnew(Button);
	button->set_text(p_text);
	button->set_theme_type_variation("SolersFlatPillButton");
	button->set_custom_minimum_size(Size2(72, 32) * EDSCALE);
	return button;
}

static Ref<StyleBoxFlat> solers_make_action_stylebox(const Color &p_bg, const Color &p_border, int p_radius, int p_padding) {
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_bg_color(p_bg);
	style->set_border_color(p_border);
	style->set_border_width_all(p_border.a > 0.0f ? 1 * EDSCALE : 0);
	style->set_corner_radius_all(p_radius * EDSCALE);
	style->set_content_margin_all(p_padding * EDSCALE);
	return style;
}

static void solers_style_segment_button(Button *p_button, bool p_selected) {
	p_button->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	p_button->set_custom_minimum_size(Size2(88, 30) * EDSCALE);
	p_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	p_button->add_theme_style_override(CoreStringName(normal), solers_make_action_stylebox(p_selected ? Color(0.105f, 0.112f, 0.118f) : Color(0.050f, 0.050f, 0.050f), Color(0.22f, 0.26f, 0.30f, 0.55f), 0, 8));
	p_button->add_theme_style_override(SceneStringName(hover), solers_make_action_stylebox(p_selected ? Color(0.130f, 0.140f, 0.150f) : Color(0.068f, 0.070f, 0.072f), Color(0.28f, 0.34f, 0.40f, 0.65f), 0, 8));
	p_button->add_theme_style_override(SceneStringName(pressed), solers_make_action_stylebox(Color(0.135f, 0.145f, 0.155f), Color(0.32f, 0.45f, 0.55f, 0.72f), 0, 8));
	p_button->add_theme_style_override("focus", solers_make_action_stylebox(Color(0, 0, 0, 0), Color(0.32f, 0.75f, 1.0f, 0.28f), 0, 8));
	p_button->add_theme_color_override(SceneStringName(font_color), p_selected ? Color(0.86f, 0.93f, 1.0f) : Color(0.62f, 0.67f, 0.73f));
	p_button->add_theme_color_override("font_hover_color", Color(0.88f, 0.94f, 1.0f));
	p_button->add_theme_color_override("font_pressed_color", Color(0.32f, 0.75f, 1.0f));
}

static void solers_style_action_button(Button *p_button, bool p_primary = false) {
	p_button->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	p_button->set_custom_minimum_size(Size2(p_primary ? 82 : 74, 28) * EDSCALE);
	p_button->set_h_size_flags(Control::SIZE_SHRINK_END);
	p_button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	p_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	p_button->add_theme_style_override(CoreStringName(normal), solers_make_action_stylebox(p_primary ? Color(0.145f, 0.112f, 0.060f) : Color(0.072f, 0.070f, 0.064f), Color(0, 0, 0, 0), 7, 6));
	p_button->add_theme_style_override(SceneStringName(hover), solers_make_action_stylebox(p_primary ? Color(0.190f, 0.140f, 0.070f) : Color(0.096f, 0.090f, 0.074f), Color(0, 0, 0, 0), 7, 6));
	p_button->add_theme_style_override(SceneStringName(pressed), solers_make_action_stylebox(p_primary ? Color(0.215f, 0.152f, 0.072f) : Color(0.118f, 0.102f, 0.070f), Color(0, 0, 0, 0), 7, 6));
	p_button->add_theme_style_override("hover_pressed", solers_make_action_stylebox(p_primary ? Color(0.215f, 0.152f, 0.072f) : Color(0.118f, 0.102f, 0.070f), Color(0, 0, 0, 0), 7, 6));
	p_button->add_theme_style_override("disabled", solers_make_action_stylebox(Color(0.052f, 0.050f, 0.046f), Color(0, 0, 0, 0), 7, 6));
	p_button->add_theme_style_override("focus", solers_make_action_stylebox(Color(0, 0, 0, 0), Color(0.95f, 0.78f, 0.50f, 0.18f), 7, 6));
	p_button->add_theme_color_override(SceneStringName(font_color), p_primary ? Color(0.96f, 0.94f, 0.86f) : Color(0.80f, 0.79f, 0.72f));
	p_button->add_theme_color_override("font_hover_color", Color(0.96f, 0.94f, 0.86f));
	p_button->add_theme_color_override("font_pressed_color", Color(0.98f, 0.84f, 0.58f));
	p_button->add_theme_color_override("font_hover_pressed_color", Color(0.98f, 0.84f, 0.58f));
	p_button->add_theme_color_override("font_disabled_color", Color(0.43f, 0.43f, 0.39f));
}

static void solers_style_action_input(LineEdit *p_line) {
	p_line->set_custom_minimum_size(Size2(0, 30) * EDSCALE);
	p_line->add_theme_style_override(CoreStringName(normal), solers_make_action_stylebox(Color(0.044f, 0.043f, 0.039f), Color(0.110f, 0.104f, 0.086f), 7, 8));
	p_line->add_theme_style_override("focus", solers_make_action_stylebox(Color(0.052f, 0.050f, 0.044f), Color(0.165f, 0.135f, 0.078f), 7, 8));
	p_line->add_theme_color_override(SceneStringName(font_color), Color(0.84f, 0.84f, 0.78f));
	p_line->add_theme_color_override("font_placeholder_color", Color(0.48f, 0.48f, 0.43f));
	p_line->add_theme_color_override("caret_color", Color(0.92f, 0.78f, 0.50f));
	p_line->add_theme_color_override("selection_color", Color(0.95f, 0.72f, 0.38f, 0.20f));
}

static PanelContainer *solers_make_action_panel() {
	PanelContainer *panel = memnew(PanelContainer);
	panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	panel->add_theme_style_override(SceneStringName(panel), solers_make_action_stylebox(Color(0.049f, 0.048f, 0.044f), Color(0, 0, 0, 0), 10, 0));
	return panel;
}

static HBoxContainer *solers_add_action_row(VBoxContainer *p_parent, bool p_separator = true) {
	if (p_separator) {
		ColorRect *separator = memnew(ColorRect);
		separator->set_color(Color(0.110f, 0.104f, 0.086f));
		separator->set_custom_minimum_size(Size2(0, 1) * EDSCALE);
		p_parent->add_child(separator);
	}
	MarginContainer *margin = memnew(MarginContainer);
	margin->add_theme_constant_override("margin_left", 12 * EDSCALE);
	margin->add_theme_constant_override("margin_right", 12 * EDSCALE);
	margin->add_theme_constant_override("margin_top", 8 * EDSCALE);
	margin->add_theme_constant_override("margin_bottom", 8 * EDSCALE);
	p_parent->add_child(margin);

	HBoxContainer *row = memnew(HBoxContainer);
	row->add_theme_constant_override("separation", 12 * EDSCALE);
	row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	margin->add_child(row);
	return row;
}

static void solers_add_action_copy(VBoxContainer *p_parent, const String &p_title, const String &p_description) {
	p_parent->add_child(solers_make_label(p_title, 13, Color(0.88f, 0.87f, 0.80f)));
	p_parent->add_child(solers_make_label(p_description, 11, Color(0.56f, 0.57f, 0.53f), true));
}

static void solers_mark_asset_action_creating(Button *p_button) {
	if (!p_button) {
		return;
	}
	p_button->set_text(TTRC("Creating..."));
	p_button->set_disabled(true);
}

static Control *solers_make_operation_option_control(const String &p_name, const Dictionary &p_property) {
	const String type = String(p_property.get("type", String()));
	const String label = String(p_property.get("label", p_name.capitalize()));
	const Array enum_values = p_property.get("enum", Array());
	Control *control = nullptr;
	if (!enum_values.is_empty()) {
		OptionButton *option = memnew(OptionButton);
		option->set_theme_type_variation("FlatMenuButton");
		const String default_value = String(p_property.get("default", String()));
		for (int i = 0; i < enum_values.size(); i++) {
			const String enum_value = String(enum_values[i]);
			option->add_item(enum_value);
			if (enum_value == default_value) {
				option->select(i);
			}
		}
		control = option;
	} else if (type == "boolean") {
		Button *toggle = memnew(Button);
		toggle->set_text(TTRC("Confirm"));
		toggle->set_toggle_mode(true);
		solers_style_action_button(toggle);
		control = toggle;
	} else {
		LineEdit *line = memnew(LineEdit);
		line->set_placeholder(label);
		if (type == "integer" && p_property.has("default")) {
			line->set_text(itos((int)p_property.get("default", 0)));
		}
		solers_style_action_input(line);
		control = line;
	}
	control->set_meta("solers_option_name", p_name);
	control->set_meta("solers_option_type", type);
	control->set_h_size_flags(type == "boolean" ? Control::SIZE_SHRINK_END : Control::SIZE_EXPAND_FILL);
	control->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	return control;
}

static void solers_collect_operation_options(Control *p_root, Dictionary &r_options) {
	if (!p_root) {
		return;
	}
	if (p_root->has_meta("solers_option_name")) {
		const String name = String(p_root->get_meta("solers_option_name"));
		const String type = String(p_root->get_meta("solers_option_type", String()));
		if (LineEdit *line = Object::cast_to<LineEdit>(p_root)) {
			const String text = line->get_text().strip_edges();
			if (!text.is_empty()) {
				r_options[name] = type == "integer" ? Variant(text.to_int()) : Variant(text);
			}
		} else if (OptionButton *option = Object::cast_to<OptionButton>(p_root)) {
			if (option->get_selected() >= 0) {
				r_options[name] = option->get_item_text(option->get_selected());
			}
		} else if (Button *button = Object::cast_to<Button>(p_root)) {
			r_options[name] = button->is_pressed();
		}
	}
	for (int i = 0; i < p_root->get_child_count(); i++) {
		if (Control *child = Object::cast_to<Control>(p_root->get_child(i))) {
			solers_collect_operation_options(child, r_options);
		}
	}
}

static void solers_convert_importer_meshes(Node *p_root) {
	List<Node *> queue;
	queue.push_back(p_root);
	List<Node *> delete_queue;
	while (!queue.is_empty()) {
		Node *node = queue.front()->get();
		queue.pop_front();

		ImporterMeshInstance3D *importer_mesh = Object::cast_to<ImporterMeshInstance3D>(node);
		if (importer_mesh) {
			delete_queue.push_back(importer_mesh);
			node = GLTFDocumentExtensionConvertImporterMesh::convert_importer_mesh_instance_3d(importer_mesh);
		}

		for (int i = 0; i < node->get_child_count(); i++) {
			queue.push_back(node->get_child(i));
		}
	}
	while (!delete_queue.is_empty()) {
		Node *node = delete_queue.front()->get();
		delete_queue.pop_front();
		memdelete(node);
	}
}

static bool solers_mesh_array_has(const Array &p_arrays, Mesh::ArrayType p_type) {
	if (p_arrays.size() <= p_type || p_arrays[p_type].get_type() == Variant::NIL) {
		return false;
	}
	const Variant::Type type = p_arrays[p_type].get_type();
	if (type == Variant::PACKED_VECTOR2_ARRAY) {
		return PackedVector2Array(p_arrays[p_type]).size() > 0;
	}
	if (type == Variant::PACKED_VECTOR3_ARRAY) {
		return PackedVector3Array(p_arrays[p_type]).size() > 0;
	}
	if (type == Variant::PACKED_FLOAT32_ARRAY) {
		return PackedFloat32Array(p_arrays[p_type]).size() > 0;
	}
	if (type == Variant::PACKED_FLOAT64_ARRAY) {
		return PackedFloat64Array(p_arrays[p_type]).size() > 0;
	}
	return false;
}

static void solers_collect_preview_mesh_stats(Node *p_node, int &r_meshes, int &r_surfaces, int &r_triangles, int &r_materials, int &r_textured_surfaces, int &r_texture_slots, int &r_uv_surfaces, int &r_normal_surfaces) {
	if (p_node->has_meta("solers_preview_overlay")) {
		return;
	}
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node);
	if (mesh_instance) {
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_valid()) {
			r_meshes++;
			for (int i = 0; i < mesh->get_surface_count(); i++) {
				r_surfaces++;
				if (mesh->surface_get_primitive_type(i) == Mesh::PRIMITIVE_TRIANGLES) {
					const int len = (mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_INDEX) ? mesh->surface_get_array_index_len(i) : mesh->surface_get_array_len(i);
					r_triangles += len / 3;
				} else if (mesh->surface_get_primitive_type(i) == Mesh::PRIMITIVE_TRIANGLE_STRIP) {
					const int len = (mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_INDEX) ? mesh->surface_get_array_index_len(i) : mesh->surface_get_array_len(i);
					r_triangles += MAX(0, len - 2);
				}

				const Ref<Material> material = mesh_instance->get_active_material(i);
				if (material.is_valid()) {
					r_materials++;
					BaseMaterial3D *base_material = Object::cast_to<BaseMaterial3D>(material.ptr());
					if (base_material) {
						bool has_texture = false;
						for (int texture_slot = 0; texture_slot < BaseMaterial3D::TEXTURE_MAX; texture_slot++) {
							if (base_material->get_texture(BaseMaterial3D::TextureParam(texture_slot)).is_valid()) {
								r_texture_slots++;
								has_texture = true;
							}
						}
						if (has_texture) {
							r_textured_surfaces++;
						}
					}
				}

				const Array arrays = mesh->surface_get_arrays(i);
				if (solers_mesh_array_has(arrays, Mesh::ARRAY_TEX_UV) || solers_mesh_array_has(arrays, Mesh::ARRAY_TEX_UV2)) {
					r_uv_surfaces++;
				}
				if (solers_mesh_array_has(arrays, Mesh::ARRAY_NORMAL)) {
					r_normal_surfaces++;
				}
			}
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		solers_collect_preview_mesh_stats(p_node->get_child(i), r_meshes, r_surfaces, r_triangles, r_materials, r_textured_surfaces, r_texture_slots, r_uv_surfaces, r_normal_surfaces);
	}
}

static Skeleton3D *solers_find_skeleton(Node *p_node) {
	if (!p_node) {
		return nullptr;
	}
	if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_node)) {
		return skeleton->get_bone_count() > 0 ? skeleton : nullptr;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (Skeleton3D *skeleton = solers_find_skeleton(p_node->get_child(i))) {
			return skeleton;
		}
	}
	return nullptr;
}

static AnimationPlayer *solers_find_animation_player(Node *p_node) {
	if (!p_node) {
		return nullptr;
	}
	if (AnimationPlayer *player = Object::cast_to<AnimationPlayer>(p_node)) {
		List<StringName> animations;
		player->get_animation_list(&animations);
		return animations.is_empty() ? nullptr : player;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (AnimationPlayer *player = solers_find_animation_player(p_node->get_child(i))) {
			return player;
		}
	}
	return nullptr;
}

static void solers_apply_preview_fallback_material(Node *p_node, const Ref<Material> &p_material) {
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node);
	if (mesh_instance) {
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_valid()) {
			for (int i = 0; i < mesh->get_surface_count(); i++) {
				if (mesh_instance->get_active_material(i).is_null()) {
					mesh_instance->set_surface_override_material(i, p_material);
				}
			}
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		solers_apply_preview_fallback_material(p_node->get_child(i), p_material);
	}
}

void EditorAssetLibraryItem::configure(const String &p_title, int p_asset_id, const String &p_category, int p_category_id, const String &p_author, int p_author_id, const String &p_cost) {
	title_text = p_title;
	title->set_text(title_text);
	title->set_tooltip_text(title_text);
	asset_id = p_asset_id;
	category->set_text(p_category);
	category_id = p_category_id;
	author->set_text(p_author);
	author_id = p_author_id;
	price->set_text(p_cost);

	_calculate_misc_links_size();
}

void EditorAssetLibraryItem::set_image(int p_type, int p_index, const Ref<Texture2D> &p_image) {
	ERR_FAIL_COND(p_type != EditorAssetLibrary::IMAGE_QUEUE_ICON);
	ERR_FAIL_COND(p_index != 0);

	icon->set_texture_normal(p_image);
}

void EditorAssetLibraryItem::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			icon->set_texture_normal(get_editor_theme_icon(SNAME("ProjectIconLoading")));
			category->add_theme_color_override(SceneStringName(font_color), Color(0.5, 0.5, 0.5));
			author->add_theme_color_override(SceneStringName(font_color), Color(0.5, 0.5, 0.5));
			price->add_theme_color_override(SceneStringName(font_color), Color(0.5, 0.5, 0.5));

			if (author->get_default_cursor_shape() == CURSOR_ARROW) {
				// Disable visible feedback if author link isn't clickable.
				author->add_theme_color_override("font_pressed_color", Color(0.5, 0.5, 0.5));
				author->add_theme_color_override("font_hover_color", Color(0.5, 0.5, 0.5));
			}

			_calculate_misc_links_size();
		} break;

		case NOTIFICATION_TRANSLATION_CHANGED: {
			_calculate_misc_links_size();
		} break;

		case NOTIFICATION_RESIZED: {
			calculate_misc_links_ratio();
		} break;
	}
}

void EditorAssetLibraryItem::_calculate_misc_links_size() {
	Ref<TextLine> text_buf;
	text_buf.instantiate();
	text_buf->add_string(author->get_text(), author->get_button_font(), author->get_button_font_size());
	author_width = text_buf->get_line_width();

	text_buf->clear();
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	text_buf->add_string(price->get_text(), font, font_size);
	price_width = text_buf->get_line_width();

	calculate_misc_links_ratio();
}

void EditorAssetLibraryItem::calculate_misc_links_ratio() {
	const int separators_width = 15 * EDSCALE;
	const float total_width = author_price_hbox->get_size().width - (separator->get_size().width + separators_width);
	if (total_width <= 0) {
		return;
	}

	float ratio_left = 1;
	// Make the ratios a fraction bigger, to avoid unnecessary trimming.
	const float extra_ratio = 4.0 / total_width;

	const float author_ratio = MIN(1, author_width / total_width);
	author->set_stretch_ratio(author_ratio + extra_ratio);
	ratio_left -= author_ratio;

	const float price_ratio = MIN(1, price_width / total_width);
	price->set_stretch_ratio(price_ratio + extra_ratio);
	ratio_left -= price_ratio;

	spacer->set_stretch_ratio(ratio_left);
}

void EditorAssetLibraryItem::_asset_clicked() {
	emit_signal(SNAME("asset_selected"), asset_id);
}

void EditorAssetLibraryItem::_category_clicked() {
	emit_signal(SNAME("category_selected"), category_id);
}

void EditorAssetLibraryItem::_author_clicked() {
	emit_signal(SNAME("author_selected"), author->get_text());
}

void EditorAssetLibraryItem::_bind_methods() {
	ClassDB::bind_method("set_image", &EditorAssetLibraryItem::set_image);
	ADD_SIGNAL(MethodInfo("asset_selected"));
	ADD_SIGNAL(MethodInfo("category_selected"));
	ADD_SIGNAL(MethodInfo("author_selected"));
}

EditorAssetLibraryItem::EditorAssetLibraryItem(bool p_clickable) {
	Ref<StyleBoxEmpty> border;
	border.instantiate();
	border->set_content_margin_all(5 * EDSCALE);
	add_theme_style_override(SceneStringName(panel), border);

	HBoxContainer *hb = memnew(HBoxContainer);
	// Add some spacing to visually separate the icon from the asset details.
	hb->add_theme_constant_override("separation", 15 * EDSCALE);
	add_child(hb);

	icon = memnew(TextureButton);
	icon->set_accessibility_name(TTRC("Open asset details"));
	icon->set_custom_minimum_size(Size2(64, 64) * EDSCALE);
	hb->add_child(icon);

	VBoxContainer *vb = memnew(VBoxContainer);

	hb->add_child(vb);
	vb->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	title = memnew(LinkButton);
	title->set_accessibility_name(TTRC("Title"));
	title->set_auto_translate_mode(AutoTranslateMode::AUTO_TRANSLATE_MODE_DISABLED);
	title->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	title->set_underline_mode(LinkButton::UNDERLINE_MODE_ON_HOVER);
	vb->add_child(title);

	category = memnew(LinkButton);
	category->set_accessibility_name(TTRC("Category"));
	category->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	category->set_underline_mode(LinkButton::UNDERLINE_MODE_ON_HOVER);
	vb->add_child(category);

	author_price_hbox = memnew(HBoxContainer);
	author_price_hbox->add_theme_constant_override("separation", 5 * EDSCALE);
	vb->add_child(author_price_hbox);

	author = memnew(LinkButton);
	author->set_tooltip_text(TTRC("Author"));
	author->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	author->set_accessibility_name(TTRC("Author"));
	author->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	author_price_hbox->add_child(author);

	separator = memnew(HSeparator);
	author_price_hbox->add_child(separator);

	if (p_clickable) {
		author->set_underline_mode(LinkButton::UNDERLINE_MODE_ON_HOVER);
		icon->set_default_cursor_shape(CURSOR_POINTING_HAND);
		icon->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibraryItem::_asset_clicked));
		title->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibraryItem::_asset_clicked));
		category->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibraryItem::_category_clicked));
		author->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibraryItem::_author_clicked));
	} else {
		title->set_mouse_filter(MOUSE_FILTER_IGNORE);
		category->set_mouse_filter(MOUSE_FILTER_IGNORE);
		author->set_underline_mode(LinkButton::UNDERLINE_MODE_NEVER);
		author->set_default_cursor_shape(CURSOR_ARROW);
	}

	Ref<StyleBoxEmpty> label_margin;
	label_margin.instantiate();
	label_margin->set_content_margin_all(0);

	price = memnew(Label);
	price->set_focus_mode(FOCUS_ACCESSIBILITY);
	price->add_theme_style_override(CoreStringName(normal), label_margin);
	price->set_tooltip_text(TTRC("License"));
	price->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	price->set_accessibility_name(TTRC("License"));
	price->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	price->set_mouse_filter(MOUSE_FILTER_PASS);
	author_price_hbox->add_child(price);

	spacer = memnew(Control);
	spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	author_price_hbox->add_child(spacer);

	set_custom_minimum_size(Size2(250, 80) * EDSCALE);
	set_h_size_flags(Control::SIZE_EXPAND_FILL);
}

//////////////////////////////////////////////////////////////////////////////

void EditorAssetLibraryItemDescription::set_image(int p_type, int p_index, const Ref<Texture2D> &p_image) {
	switch (p_type) {
		case EditorAssetLibrary::IMAGE_QUEUE_ICON: {
			item->call("set_image", p_type, p_index, p_image);
			icon = p_image;
		} break;
		case EditorAssetLibrary::IMAGE_QUEUE_THUMBNAIL: {
			for (int i = 0; i < preview_images.size(); i++) {
				if (preview_images[i].id == p_index) {
					if (preview_images[i].is_video) {
						Ref<Image> overlay = previews->get_editor_theme_icon(SNAME("PlayOverlay"))->get_image();
						Ref<Image> thumbnail = p_image->get_image();
						thumbnail = thumbnail->duplicate();
						Point2i overlay_pos = Point2i((thumbnail->get_width() - overlay->get_width()) / 2, (thumbnail->get_height() - overlay->get_height()) / 2);

						// Overlay and thumbnail need the same format for `blend_rect` to work.
						thumbnail->convert(Image::FORMAT_RGBA8);
						thumbnail->blend_rect(overlay, overlay->get_used_rect(), overlay_pos);
						preview_images[i].button->set_button_icon(ImageTexture::create_from_image(thumbnail));

						// Make it clearer that clicking it will open an external link
						preview_images[i].button->set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
					} else {
						preview_images[i].button->set_button_icon(p_image);
					}
					break;
				}
			}
		} break;
		case EditorAssetLibrary::IMAGE_QUEUE_SCREENSHOT: {
			for (int i = 0; i < preview_images.size(); i++) {
				if (preview_images[i].id == p_index) {
					preview_images.write[i].image = p_image;
					if (preview_images[i].button->is_pressed()) {
						_preview_click(p_index);
					}
					break;
				}
			}
		} break;
	}
}

void EditorAssetLibraryItemDescription::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			previews_bg->add_theme_style_override(SceneStringName(panel), previews->get_theme_stylebox(CoreStringName(normal), SNAME("TextEdit")));
		} break;

		case NOTIFICATION_POST_POPUP: {
			callable_mp(item, &EditorAssetLibraryItem::calculate_misc_links_ratio).call_deferred();
		} break;
	}
}

void EditorAssetLibraryItemDescription::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_image"), &EditorAssetLibraryItemDescription::set_image);
}

void EditorAssetLibraryItemDescription::_link_click(const String &p_url) {
	ERR_FAIL_COND(!p_url.begins_with("http"));
	OS::get_singleton()->shell_open(p_url);
}

void EditorAssetLibraryItemDescription::_preview_click(int p_id) {
	for (int i = 0; i < preview_images.size(); i++) {
		if (preview_images[i].id == p_id) {
			preview_images[i].button->set_pressed(true);
			if (!preview_images[i].is_video) {
				if (preview_images[i].image.is_valid()) {
					preview->set_texture(preview_images[i].image);
					child_controls_changed();
				}
			} else {
				_link_click(preview_images[i].video_link);
			}
		} else {
			preview_images[i].button->set_pressed(false);
		}
	}
}

void EditorAssetLibraryItemDescription::configure(const String &p_title, int p_asset_id, const String &p_category, int p_category_id, const String &p_author, int p_author_id, const String &p_cost, int p_version, const String &p_version_string, const String &p_description, const String &p_download_url, const String &p_browse_url, const String &p_sha256_hash) {
	asset_id = p_asset_id;
	title = p_title;
	download_url = p_download_url;
	sha256 = p_sha256_hash;
	item->configure(p_title, p_asset_id, p_category, p_category_id, p_author, p_author_id, p_cost);
	description->clear();
	description->add_text(TTR("Version:") + " " + p_version_string + "\n");
	description->add_text(TTR("Contents:") + " ");
	description->push_meta(p_browse_url);
	description->add_text(TTR("View Files"));
	description->pop();
	description->add_text("\n" + TTR("Description:") + "\n\n");
	description->append_text(p_description);
	description->set_selection_enabled(true);
	description->set_context_menu_enabled(true);
	set_title(p_title);
}

void EditorAssetLibraryItemDescription::add_preview(int p_id, bool p_video, const String &p_url) {
	if (preview_images.is_empty()) {
		previews_vbox->show();
	}

	Preview new_preview;
	new_preview.id = p_id;
	new_preview.video_link = p_url;
	new_preview.is_video = p_video;
	new_preview.button = memnew(Button);
	new_preview.button->set_button_icon(previews->get_editor_theme_icon(SNAME("ThumbnailWait")));
	new_preview.button->set_toggle_mode(true);
	new_preview.button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibraryItemDescription::_preview_click).bind(p_id));
	preview_hb->add_child(new_preview.button);
	if (!p_video) {
		new_preview.image = previews->get_editor_theme_icon(SNAME("ThumbnailWait"));
	}
	preview_images.push_back(new_preview);
	if (preview_images.size() == 1 && !p_video) {
		_preview_click(p_id);
	}
}

EditorAssetLibraryItemDescription::EditorAssetLibraryItemDescription() {
	HBoxContainer *hbox = memnew(HBoxContainer);
	add_child(hbox);
	VBoxContainer *desc_vbox = memnew(VBoxContainer);
	hbox->add_child(desc_vbox);
	hbox->add_theme_constant_override("separation", 15 * EDSCALE);

	item = memnew(EditorAssetLibraryItem);

	desc_vbox->add_child(item);
	desc_vbox->set_custom_minimum_size(Size2(440 * EDSCALE, 440 * EDSCALE));

	description = memnew(RichTextLabel);
	desc_vbox->add_child(description);
	description->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	description->connect("meta_clicked", callable_mp(this, &EditorAssetLibraryItemDescription::_link_click));
	description->add_theme_constant_override(SceneStringName(line_separation), Math::round(5 * EDSCALE));

	previews_vbox = memnew(VBoxContainer);
	previews_vbox->hide(); // Will be shown if we add any previews later.

	hbox->add_child(previews_vbox);
	previews_vbox->add_theme_constant_override("separation", 15 * EDSCALE);
	previews_vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	previews_vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	preview = memnew(TextureRect);
	previews_vbox->add_child(preview);
	preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	preview->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	preview->set_custom_minimum_size(Size2(640 * EDSCALE, 345 * EDSCALE));
	preview->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	preview->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	previews_bg = memnew(PanelContainer);
	previews_vbox->add_child(previews_bg);
	previews_bg->set_custom_minimum_size(Size2(640 * EDSCALE, 101 * EDSCALE));

	previews = memnew(ScrollContainer);
	previews_bg->add_child(previews);
	previews->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	preview_hb = memnew(HBoxContainer);
	preview_hb->set_v_size_flags(Control::SIZE_EXPAND_FILL);

	previews->add_child(preview_hb);
	set_ok_button_text(TTRC("Download"));
	set_cancel_button_text(TTRC("Close"));
}

///////////////////////////////////////////////////////////////////////////////////

void EditorAssetLibraryItemDownload::_http_download_completed(int p_status, int p_code, const PackedStringArray &headers, const PackedByteArray &p_data) {
	String error_text;

	switch (p_status) {
		case HTTPRequest::RESULT_CHUNKED_BODY_SIZE_MISMATCH:
		case HTTPRequest::RESULT_CONNECTION_ERROR:
		case HTTPRequest::RESULT_BODY_SIZE_LIMIT_EXCEEDED: {
			error_text = TTR("Connection error, please try again.");
			status->set_text(TTRC("Can't connect."));
		} break;
		case HTTPRequest::RESULT_CANT_CONNECT:
		case HTTPRequest::RESULT_TLS_HANDSHAKE_ERROR: {
			error_text = TTR("Can't connect to host:") + " " + host;
			status->set_text(TTRC("Can't connect."));
		} break;
		case HTTPRequest::RESULT_NO_RESPONSE: {
			error_text = TTR("No response from host:") + " " + host;
			status->set_text(TTRC("No response."));
		} break;
		case HTTPRequest::RESULT_CANT_RESOLVE: {
			error_text = TTR("Can't resolve hostname:") + " " + host;
			status->set_text(TTRC("Can't resolve."));
		} break;
		case HTTPRequest::RESULT_REQUEST_FAILED: {
			error_text = TTR("Request failed, return code:") + " " + itos(p_code);
			status->set_text(TTRC("Request failed."));
		} break;
		case HTTPRequest::RESULT_DOWNLOAD_FILE_CANT_OPEN:
		case HTTPRequest::RESULT_DOWNLOAD_FILE_WRITE_ERROR: {
			error_text = TTR("Cannot save response to:") + " " + download->get_download_file();
			status->set_text(TTRC("Write error."));
		} break;
		case HTTPRequest::RESULT_REDIRECT_LIMIT_REACHED: {
			error_text = TTR("Request failed, too many redirects");
			status->set_text(TTRC("Redirect loop."));
		} break;
		case HTTPRequest::RESULT_TIMEOUT: {
			error_text = TTR("Request failed, timeout");
			status->set_text(TTRC("Timeout."));
		} break;
		default: {
			if (p_code != 200) {
				error_text = TTR("Request failed, return code:") + " " + itos(p_code);
				status->set_text(TTR("Failed:") + " " + itos(p_code));
			} else if (!sha256.is_empty()) {
				String download_sha256 = FileAccess::get_sha256(download->get_download_file());
				if (sha256 != download_sha256) {
					error_text = TTR("Bad download hash, assuming file has been tampered with.") + "\n";
					error_text += TTR("Expected:") + " " + sha256 + "\n" + TTR("Got:") + " " + download_sha256;
					status->set_text(TTRC("Failed SHA-256 hash check"));
				}
			}
		} break;
	}

	// Make the progress bar invisible but don't reflow other Controls around it.
	progress->set_modulate(Color(0, 0, 0, 0));
	progress->set_indeterminate(false);

	if (!error_text.is_empty()) {
		download_error->set_text(TTR("Asset Download Error:") + "\n" + error_text);
		download_error->popup_centered();
		// Let the user retry the download.
		retry_button->show();
		return;
	}

	install_button->set_disabled(false);
	status->set_text(TTRC("Ready to install!"));

	set_process(false);

	// Automatically prompt for installation once the download is completed.
	install();
}

void EditorAssetLibraryItemDownload::configure(const String &p_title, int p_asset_id, const Ref<Texture2D> &p_preview, const String &p_download_url, const String &p_sha256_hash) {
	title->set_text(p_title);
	icon->set_texture(p_preview);
	asset_id = p_asset_id;
	if (p_preview.is_null()) {
		icon->set_texture(get_editor_theme_icon(SNAME("FileBrokenBigThumb")));
	}
	host = p_download_url;
	sha256 = p_sha256_hash;
	_make_request();
}

void EditorAssetLibraryItemDownload::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			panel->add_theme_style_override(SceneStringName(panel), get_theme_stylebox(SceneStringName(panel), SNAME("AssetLib")));
			status->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("status_color"), SNAME("AssetLib")));
			dismiss_button->set_texture_normal(get_theme_icon(SNAME("dismiss"), SNAME("AssetLib")));
		} break;

		case NOTIFICATION_PROCESS: {
			// Make the progress bar visible again when retrying the download.
			progress->set_modulate(Color(1, 1, 1, 1));

			if (download->get_downloaded_bytes() > 0) {
				progress->set_max(download->get_body_size());
				progress->set_value(download->get_downloaded_bytes());
			}

			int cstatus = download->get_http_client_status();

			if (cstatus == HTTPClient::STATUS_BODY) {
				if (download->get_body_size() > 0) {
					progress->set_indeterminate(false);
					status->set_text(vformat(
							TTR("Downloading (%s / %s)..."),
							String::humanize_size(download->get_downloaded_bytes()),
							String::humanize_size(download->get_body_size())));
				} else {
					progress->set_indeterminate(true);
					status->set_text(vformat(
							TTR("Downloading...") + " (%s)",
							String::humanize_size(download->get_downloaded_bytes())));
				}
			}

			if (cstatus != prev_status) {
				switch (cstatus) {
					case HTTPClient::STATUS_RESOLVING: {
						status->set_text(TTRC("Resolving..."));
						progress->set_max(1);
						progress->set_value(0);
					} break;
					case HTTPClient::STATUS_CONNECTING: {
						status->set_text(TTRC("Connecting..."));
						progress->set_max(1);
						progress->set_value(0);
					} break;
					case HTTPClient::STATUS_REQUESTING: {
						status->set_text(TTRC("Requesting..."));
						progress->set_max(1);
						progress->set_value(0);
					} break;
					default: {
					}
				}
				prev_status = cstatus;
			}
		} break;
	}
}

void EditorAssetLibraryItemDownload::_close() {
	// Clean up downloaded file.
	DirAccess::remove_file_or_error(download->get_download_file());
	queue_free();
}

bool EditorAssetLibraryItemDownload::can_install() const {
	return !install_button->is_disabled();
}

void EditorAssetLibraryItemDownload::install() {
	String file = download->get_download_file();

	if (external_install) {
		emit_signal(SNAME("install_asset"), file, title->get_text());
		return;
	}

	asset_installer->set_asset_name(title->get_text());
	asset_installer->open_asset(file, true);
}

void EditorAssetLibraryItemDownload::_make_request() {
	// Hide the Retry button if we've just pressed it.
	retry_button->hide();

	download->cancel_request();
	download->set_download_file(EditorPaths::get_singleton()->get_cache_dir().path_join("tmp_asset_" + itos(asset_id)) + ".zip");

	Error err = download->request(host);
	if (err != OK) {
		status->set_text(TTRC("Error making request"));
	} else {
		progress->set_indeterminate(true);
		set_process(true);
	}
}

void EditorAssetLibraryItemDownload::_bind_methods() {
	ADD_SIGNAL(MethodInfo("install_asset", PropertyInfo(Variant::STRING, "zip_path"), PropertyInfo(Variant::STRING, "name")));
}

EditorAssetLibraryItemDownload::EditorAssetLibraryItemDownload() {
	panel = memnew(PanelContainer);
	add_child(panel);

	HBoxContainer *hb = memnew(HBoxContainer);
	panel->add_child(hb);
	icon = memnew(TextureRect);
	icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	icon->set_v_size_flags(0);
	hb->add_child(icon);

	VBoxContainer *vb = memnew(VBoxContainer);
	hb->add_child(vb);
	vb->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	HBoxContainer *title_hb = memnew(HBoxContainer);
	vb->add_child(title_hb);
	title = memnew(Label);
	title->set_focus_mode(FOCUS_ACCESSIBILITY);
	title_hb->add_child(title);
	title->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	dismiss_button = memnew(TextureButton);
	dismiss_button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibraryItemDownload::_close));
	dismiss_button->set_accessibility_name(TTRC("Close"));
	title_hb->add_child(dismiss_button);

	title->set_clip_text(true);

	vb->add_spacer();

	status = memnew(Label(TTRC("Idle")));
	vb->add_child(status);
	progress = memnew(ProgressBar);
	progress->set_editor_preview_indeterminate(true);
	vb->add_child(progress);

	HBoxContainer *hb2 = memnew(HBoxContainer);
	vb->add_child(hb2);
	hb2->add_spacer();

	install_button = memnew(Button);
	install_button->set_text(TTRC("Install..."));
	install_button->set_disabled(true);
	install_button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibraryItemDownload::install));

	retry_button = memnew(Button);
	retry_button->set_text(TTRC("Retry"));
	retry_button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibraryItemDownload::_make_request));
	// Only show the Retry button in case of a failure.
	retry_button->hide();

	hb2->add_child(retry_button);
	hb2->add_child(install_button);
	set_custom_minimum_size(Size2(310, 0) * EDSCALE);

	download = memnew(HTTPRequest);
	panel->add_child(download);
	download->connect("request_completed", callable_mp(this, &EditorAssetLibraryItemDownload::_http_download_completed));
	setup_http_request(download);

	download_error = memnew(AcceptDialog);
	panel->add_child(download_error);
	download_error->set_title(TTRC("Download Error"));

	asset_installer = memnew(EditorAssetInstaller);
	panel->add_child(asset_installer);
	asset_installer->connect(SceneStringName(confirmed), callable_mp(this, &EditorAssetLibraryItemDownload::_close));

	prev_status = -1;

	external_install = false;
}

////////////////////////////////////////////////////////////////////////////////
void EditorAssetLibrary::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			add_theme_style_override(SceneStringName(panel), get_theme_stylebox(SNAME("bg"), SNAME("AssetLib")));
			error_label->move_to_front();
		} break;

		case NOTIFICATION_TRANSLATION_CHANGED: {
			if (!initial_loading) {
				_rerun_search(-1);
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			error_tr->set_texture(get_editor_theme_icon(SNAME("Error")));
			filter->set_right_icon(get_editor_theme_icon(SNAME("Search")));
			library_scroll->add_theme_style_override(SceneStringName(panel), get_theme_stylebox(SceneStringName(panel), SNAME("Tree")));
			downloads_scroll->add_theme_style_override(SceneStringName(panel), get_theme_stylebox(SNAME("downloads"), SNAME("AssetLib")));
			error_label->add_theme_color_override("color", get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (is_visible()) {
#ifndef ANDROID_ENABLED
				// Focus the search box automatically when switching to the Templates tab (in the Project Manager)
				// or switching to the AssetLib tab (in the editor).
				// The Project Manager's project filter box is automatically focused in the project manager code.
				if (templates_only || source_mode == ASSET_SOURCE_GODOT) {
					filter->grab_focus();
				}
#endif

				if (initial_loading && (templates_only || source_mode == ASSET_SOURCE_GODOT)) {
					_repository_changed(0); // Update when shown for the first time.
				}
			}
		} break;

		case NOTIFICATION_PROCESS: {
			if (source_mode == ASSET_SOURCE_SOLERS) {
				if (solers_detail_visible) {
					_update_solers_detail_animation_time();
				}
				const uint64_t now = OS::get_singleton()->get_ticks_msec();
				if (now >= solers_library_next_refresh_msec) {
					solers_library_next_refresh_msec = now + 1000;
					const String signature = _solers_library_signature();
					if (signature != solers_library_signature) {
						if (solers_detail_visible) {
							solers_library_signature = signature;
							Dictionary manifest = _read_solers_asset_manifest(solers_detail_asset_id);
							if (!manifest.is_empty()) {
								_show_solers_asset_detail(solers_detail_asset_id, manifest);
							}
						} else {
							_show_solers_library();
						}
					}
				}
				break;
			}

			HTTPClient::Status s = request->get_http_client_status();
			const bool loading = s != HTTPClient::STATUS_DISCONNECTED;

			if (loading) {
				library_scroll->set_modulate(Color(1, 1, 1, 0.5));
			} else {
				library_scroll->set_modulate(Color(1, 1, 1, 1));
			}

			const bool no_downloads = downloads_hb->get_child_count() == 0;
			if (no_downloads == downloads_scroll->is_visible()) {
				downloads_scroll->set_visible(!no_downloads);

				library_mc->set_theme_type_variation(no_downloads ? (Engine::get_singleton()->is_project_manager_hint() ? "NoBorderAssetLibProjectManager" : "NoBorderAssetLib") : "NoBorderHorizontal");
				library_scroll->set_scroll_hint_mode(no_downloads ? ScrollContainer::SCROLL_HINT_MODE_TOP_AND_LEFT : ScrollContainer::SCROLL_HINT_MODE_ALL);
			}

		} break;

		case NOTIFICATION_RESIZED: {
			_update_asset_items_columns();
			if (solers_detail) {
				solers_detail->set_vertical(get_size().x < 980 * EDSCALE);
				_sync_solers_detail_fullscreen();
			}
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (!EditorSettings::get_singleton()->check_changed_settings_in_group("asset_library") &&
					!EditorSettings::get_singleton()->check_changed_settings_in_group("network")) {
				break;
			}

			_update_repository_options();
			setup_http_request(request);

			const bool loading_blocked_new = ((int)EDITOR_GET("network/connection/network_mode") == EditorSettings::NETWORK_OFFLINE);
			if (loading_blocked_new != loading_blocked) {
				loading_blocked = loading_blocked_new;

				if (!loading_blocked && is_visible()) {
					_request_current_config(); // Reload config now that the network is available.
				}
			}
		} break;
	}
}

void EditorAssetLibrary::_update_repository_options() {
	// TODO: Move to editor_settings.cpp
	Dictionary default_urls;
	default_urls["godotengine.org (Official)"] = "https://godotengine.org/asset-library/api";
	Dictionary available_urls = _EDITOR_DEF("asset_library/available_urls", default_urls, true);
	repository->clear();
	int i = 0;
	for (const KeyValue<Variant, Variant> &kv : available_urls) {
		repository->add_item(kv.key);
		repository->set_item_metadata(i, kv.value);
		i++;
	}
}

void EditorAssetLibrary::shortcut_input(const Ref<InputEvent> &p_event) {
	ERR_FAIL_COND(p_event.is_null());

	const Ref<InputEventKey> key = p_event;

	if (key.is_valid() && key->is_pressed()) {
		if (key->is_match(InputEventKey::create_reference(KeyModifierMask::CMD_OR_CTRL | Key::F)) && is_visible_in_tree()) {
			if (templates_only || source_mode == ASSET_SOURCE_GODOT) {
				filter->grab_focus();
				filter->select_all();
				accept_event();
			}
		}
	}
}

void EditorAssetLibrary::_install_asset() {
	ERR_FAIL_NULL(description);

	EditorAssetLibraryItemDownload *d = _get_asset_in_progress(description->get_asset_id());
	if (d) {
		d->install();
		return;
	}

	EditorAssetLibraryItemDownload *download = memnew(EditorAssetLibraryItemDownload);
	downloads_hb->add_child(download);
	download->configure(description->get_title(), description->get_asset_id(), description->get_preview_icon(), description->get_download_url(), description->get_sha256());

	if (templates_only) {
		download->set_external_install(true);
		download->connect("install_asset", callable_mp(this, &EditorAssetLibrary::_install_external_asset));
	}
}

const char *EditorAssetLibrary::sort_key[SORT_MAX] = {
	"updated",
	"updated",
	"name",
	"name",
	"cost",
	"cost",
};

const char *EditorAssetLibrary::sort_text[SORT_MAX] = {
	TTRC("Recently Updated"),
	TTRC("Least Recently Updated"),
	TTRC("Name (A-Z)"),
	TTRC("Name (Z-A)"),
	TTRC("License (A-Z)"), // "cost" stores the SPDX license name in the Godot Asset Library.
	TTRC("License (Z-A)"), // "cost" stores the SPDX license name in the Godot Asset Library.
};

const char *EditorAssetLibrary::support_key[SUPPORT_MAX] = {
	"official", // Former name for the Featured support level (still used on the API backend).
	"community",
	"testing",
};

const char *EditorAssetLibrary::support_text[SUPPORT_MAX] = {
	TTRC("Featured"),
	TTRC("Community"),
	TTRC("Testing"),
};

void EditorAssetLibrary::_select_author(const String &p_author) {
	if (!host.contains("godotengine.org")) {
		// Don't open the link for alternative repositories.
		return;
	}
	OS::get_singleton()->shell_open("https://godotengine.org/asset-library/asset?user=" + p_author.uri_encode());
}

void EditorAssetLibrary::_select_category(int p_id) {
	for (int i = 0; i < categories->get_item_count(); i++) {
		if (i == 0) {
			continue;
		}
		int id = categories->get_item_metadata(i);
		if (id == p_id) {
			categories->select(i);
			_search();
			break;
		}
	}
}

void EditorAssetLibrary::_select_asset(int p_id) {
	_api_request("asset/" + itos(p_id), REQUESTING_ASSET);
}

void EditorAssetLibrary::_image_update(bool p_use_cache, bool p_final, const PackedByteArray &p_data, int p_queue_id) {
	Object *obj = ObjectDB::get_instance(image_queue[p_queue_id].target);
	if (!obj) {
		return;
	}

	bool image_set = false;
	PackedByteArray image_data = p_data;

	if (p_use_cache) {
		String cache_filename_base = EditorPaths::get_singleton()->get_cache_dir().path_join("assetimage_" + image_queue[p_queue_id].image_url.md5_text());

		Ref<FileAccess> file = FileAccess::open(cache_filename_base + ".data", FileAccess::READ);
		if (file.is_valid()) {
			PackedByteArray cached_data;
			int len = file->get_32();
			cached_data.resize(len);

			uint8_t *w = cached_data.ptrw();
			file->get_buffer(w, len);

			image_data = cached_data;
		}
	}

	int len = image_data.size();
	const uint8_t *r = image_data.ptr();
	Ref<Image> image = memnew(Image);

	uint8_t png_signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	uint8_t jpg_signature[3] = { 255, 216, 255 };
	uint8_t webp_signature[4] = { 82, 73, 70, 70 };
	uint8_t bmp_signature[2] = { 66, 77 };

	if (r) {
		Ref<Image> parsed_image;

		if ((memcmp(&r[0], &png_signature[0], 8) == 0) && Image::_png_mem_loader_func) {
			parsed_image = Image::_png_mem_loader_func(r, len);
		} else if ((memcmp(&r[0], &jpg_signature[0], 3) == 0) && Image::_jpg_mem_loader_func) {
			parsed_image = Image::_jpg_mem_loader_func(r, len);
		} else if ((memcmp(&r[0], &webp_signature[0], 4) == 0) && Image::_webp_mem_loader_func) {
			parsed_image = Image::_webp_mem_loader_func(r, len);
		} else if ((memcmp(&r[0], &bmp_signature[0], 2) == 0) && Image::_bmp_mem_loader_func) {
			parsed_image = Image::_bmp_mem_loader_func(r, len);
		} else if (Image::_svg_scalable_mem_loader_func) {
			parsed_image = Image::_svg_scalable_mem_loader_func(r, len, 1.0);
		}

		if (parsed_image.is_null()) {
			if (is_print_verbose_enabled()) {
				ERR_PRINT(vformat("Asset Library: Invalid image downloaded from '%s' for asset # %d", image_queue[p_queue_id].image_url, image_queue[p_queue_id].asset_id));
			}
		} else {
			image->copy_internals_from(parsed_image);
		}
	}

	if (!image->is_empty()) {
		switch (image_queue[p_queue_id].image_type) {
			case IMAGE_QUEUE_ICON:
				image->resize(64 * EDSCALE, 64 * EDSCALE, Image::INTERPOLATE_LANCZOS);
				break;

			case IMAGE_QUEUE_THUMBNAIL: {
				float max_height = 85 * EDSCALE;

				float scale_ratio = max_height / (image->get_height() * EDSCALE);
				if (scale_ratio < 1) {
					image->resize(image->get_width() * EDSCALE * scale_ratio, image->get_height() * EDSCALE * scale_ratio, Image::INTERPOLATE_LANCZOS);
				}
			} break;

			case IMAGE_QUEUE_SCREENSHOT: {
				float max_height = 397 * EDSCALE;

				float scale_ratio = max_height / (image->get_height() * EDSCALE);
				if (scale_ratio < 1) {
					image->resize(image->get_width() * EDSCALE * scale_ratio, image->get_height() * EDSCALE * scale_ratio, Image::INTERPOLATE_LANCZOS);
				}
			} break;
		}

		Ref<ImageTexture> tex = ImageTexture::create_from_image(image);

		obj->call("set_image", image_queue[p_queue_id].image_type, image_queue[p_queue_id].image_index, tex);
		image_set = true;
	}

	if (!image_set && p_final) {
		obj->call("set_image", image_queue[p_queue_id].image_type, image_queue[p_queue_id].image_index, get_editor_theme_icon(SNAME("FileBrokenBigThumb")));
	}
}

void EditorAssetLibrary::_image_request_completed(int p_status, int p_code, const PackedStringArray &headers, const PackedByteArray &p_data, int p_queue_id) {
	ERR_FAIL_COND(!image_queue.has(p_queue_id));

	if (p_status == HTTPRequest::RESULT_SUCCESS && p_code < HTTPClient::RESPONSE_BAD_REQUEST) {
		if (p_code != HTTPClient::RESPONSE_NOT_MODIFIED) {
			for (int i = 0; i < headers.size(); i++) {
				if (headers[i].findn("ETag:") == 0) { // Save etag
					String cache_filename_base = EditorPaths::get_singleton()->get_cache_dir().path_join("assetimage_" + image_queue[p_queue_id].image_url.md5_text());
					String new_etag = headers[i].substr(headers[i].find_char(':') + 1).strip_edges();
					Ref<FileAccess> file = FileAccess::open(cache_filename_base + ".etag", FileAccess::WRITE);
					if (file.is_valid()) {
						file->store_line(new_etag);
					}

					int len = p_data.size();
					const uint8_t *r = p_data.ptr();
					file = FileAccess::open(cache_filename_base + ".data", FileAccess::WRITE);
					if (file.is_valid()) {
						file->store_32(len);
						file->store_buffer(r, len);
					}

					break;
				}
			}
		}
		_image_update(p_code == HTTPClient::RESPONSE_NOT_MODIFIED, true, p_data, p_queue_id);

	} else {
		if (is_print_verbose_enabled()) {
			WARN_PRINT(vformat("Asset Library: Error getting image from '%s' for asset # %d.", image_queue[p_queue_id].image_url, image_queue[p_queue_id].asset_id));
		}

		Object *obj = ObjectDB::get_instance(image_queue[p_queue_id].target);
		if (obj) {
			obj->call("set_image", image_queue[p_queue_id].image_type, image_queue[p_queue_id].image_index, get_editor_theme_icon(SNAME("FileBrokenBigThumb")));
		}
	}

	image_queue[p_queue_id].request->queue_free();
	image_queue.erase(p_queue_id);

	_update_image_queue();
}

void EditorAssetLibrary::_update_image_queue() {
	const int max_images = 6;
	int current_images = 0;

	List<int> to_delete;
	for (KeyValue<int, ImageQueue> &E : image_queue) {
		if (!E.value.active && current_images < max_images) {
			String cache_filename_base = EditorPaths::get_singleton()->get_cache_dir().path_join("assetimage_" + E.value.image_url.md5_text());
			Vector<String> headers;

			if (FileAccess::exists(cache_filename_base + ".etag") && FileAccess::exists(cache_filename_base + ".data")) {
				Ref<FileAccess> file = FileAccess::open(cache_filename_base + ".etag", FileAccess::READ);
				if (file.is_valid()) {
					headers.push_back("If-None-Match: " + file->get_line());
				}
			}

			Error err = E.value.request->request(E.value.image_url, headers);
			if (err != OK) {
				to_delete.push_back(E.key);
			} else {
				E.value.active = true;
			}
			current_images++;
		} else if (E.value.active) {
			current_images++;
		}
	}

	while (to_delete.size()) {
		image_queue[to_delete.front()->get()].request->queue_free();
		image_queue.erase(to_delete.front()->get());
		to_delete.pop_front();
	}
}

void EditorAssetLibrary::_request_image(ObjectID p_for, int p_asset_id, String p_image_url, ImageType p_type, int p_image_index) {
	// Remove extra spaces around the URL. This isn't strictly valid, but recoverable.
	String trimmed_url = p_image_url.strip_edges();
	if (trimmed_url != p_image_url && is_print_verbose_enabled()) {
		WARN_PRINT(vformat("Asset Library: Badly formatted image URL '%s' for asset # %d.", p_image_url, p_asset_id));
	}

	// Validate the image URL first.
	{
		String url_scheme;
		String url_host;
		int url_port;
		String url_path;
		String url_fragment;
		Error err = trimmed_url.parse_url(url_scheme, url_host, url_port, url_path, url_fragment);
		if (err != OK) {
			if (is_print_verbose_enabled()) {
				ERR_PRINT(vformat("Asset Library: Invalid image URL '%s' for asset # %d.", trimmed_url, p_asset_id));
			}

			Object *obj = ObjectDB::get_instance(p_for);
			if (obj) {
				obj->call("set_image", p_type, p_image_index, get_editor_theme_icon(SNAME("FileBrokenBigThumb")));
			}
			return;
		}
	}

	ImageQueue iq;
	iq.image_url = trimmed_url;
	iq.image_index = p_image_index;
	iq.image_type = p_type;
	iq.request = memnew(HTTPRequest);
	setup_http_request(iq.request);

	iq.target = p_for;
	iq.asset_id = p_asset_id;
	iq.queue_id = ++last_queue_id;
	iq.active = false;

	iq.request->connect("request_completed", callable_mp(this, &EditorAssetLibrary::_image_request_completed).bind(iq.queue_id));

	image_queue[iq.queue_id] = iq;
	add_child(iq.request);

	_image_update(true, false, PackedByteArray(), iq.queue_id);
	_update_image_queue();
}

void EditorAssetLibrary::_repository_changed(int p_repository_id) {
	_set_library_message(TTRC("Loading..."));

	asset_top_page->hide();
	asset_bottom_page->hide();
	asset_items->hide();

	filter->set_editable(false);
	sort->set_disabled(true);
	categories->set_disabled(true);
	support->set_disabled(true);

	host = repository->get_item_metadata(p_repository_id);
	if (templates_only) {
		_api_request("configure", REQUESTING_CONFIG, "?type=project");
	} else {
		_api_request("configure", REQUESTING_CONFIG);
	}
}

void EditorAssetLibrary::_support_toggled(int p_support) {
	support->get_popup()->set_item_checked(p_support, !support->get_popup()->is_item_checked(p_support));
	_search();
}

void EditorAssetLibrary::_rerun_search(int p_ignore) {
	_search();
}

void EditorAssetLibrary::_search(int p_page) {
	String args;

	if (templates_only) {
		args += "?type=project&";
	} else {
		args += "?";
	}
	args += String() + "sort=" + sort_key[sort->get_selected()];

	// We use the "branch" version, i.e. major.minor, as patch releases should be compatible
	args += "&godot_version=" + String(GODOT_VERSION_BRANCH);

	String support_list;
	for (int i = 0; i < SUPPORT_MAX; i++) {
		if (support->get_popup()->is_item_checked(i)) {
			support_list += String(support_key[i]) + "+";
		}
	}
	if (!support_list.is_empty()) {
		args += "&support=" + support_list.substr(0, support_list.length() - 1);
	}

	if (categories->get_selected() > 0) {
		args += "&category=" + itos(categories->get_item_metadata(categories->get_selected()));
	}

	// Sorting options with an odd index are always the reverse of the previous one
	if (sort->get_selected() % 2 == 1) {
		args += "&reverse=true";
	}

	if (!filter->get_text().is_empty()) {
		args += "&filter=" + filter->get_text().uri_encode();
	}

	if (p_page > 0) {
		args += "&page=" + itos(p_page);
	}

	_api_request("asset", REQUESTING_SEARCH, args);
}

void EditorAssetLibrary::_search_text_changed(const String &p_text) {
	filter_debounce_timer->start();
}

void EditorAssetLibrary::_filter_debounce_timer_timeout() {
	_search();
}

void EditorAssetLibrary::_request_current_config() {
	_repository_changed(repository->get_selected());
}

HBoxContainer *EditorAssetLibrary::_make_pages(int p_page, int p_page_count, int p_page_len, int p_total_items, int p_current_items) {
	HBoxContainer *hbc = memnew(HBoxContainer);

	if (p_page_count < 2) {
		return hbc;
	}

	//do the mario
	int from = p_page - (5 / EDSCALE);
	if (from < 0) {
		from = 0;
	}
	int to = from + (10 / EDSCALE);
	if (to > p_page_count) {
		to = p_page_count;
	}

	hbc->add_spacer();
	hbc->add_theme_constant_override("separation", 5 * EDSCALE);

	Button *first = memnew(Button);
	first->set_text(TTR("First", "Pagination"));
	first->set_theme_type_variation("PanelBackgroundButton");
	if (p_page != 0) {
		first->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_search).bind(0));
	} else {
		first->set_disabled(true);
		first->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	}
	hbc->add_child(first);

	Button *prev = memnew(Button);
	prev->set_text(TTR("Previous", "Pagination"));
	prev->set_theme_type_variation("PanelBackgroundButton");
	if (p_page > 0) {
		prev->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_search).bind(p_page - 1));
	} else {
		prev->set_disabled(true);
		prev->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	}
	hbc->add_child(prev);
	hbc->add_child(memnew(VSeparator));

	for (int i = from; i < to; i++) {
		Button *current = memnew(Button);
		// Add padding to make page number buttons easier to click.
		current->set_text(vformat(" %d ", i + 1));
		current->set_theme_type_variation("PanelBackgroundButton");
		if (i == p_page) {
			current->set_disabled(true);
			current->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
		} else {
			current->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_search).bind(i));
		}
		hbc->add_child(current);
	}

	Button *next = memnew(Button);
	next->set_text(TTR("Next", "Pagination"));
	next->set_theme_type_variation("PanelBackgroundButton");
	if (p_page < p_page_count - 1) {
		next->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_search).bind(p_page + 1));
	} else {
		next->set_disabled(true);
		next->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	}
	hbc->add_child(memnew(VSeparator));
	hbc->add_child(next);

	Button *last = memnew(Button);
	last->set_text(TTR("Last", "Pagination"));
	last->set_theme_type_variation("PanelBackgroundButton");
	if (p_page != p_page_count - 1) {
		last->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_search).bind(p_page_count - 1));
	} else {
		last->set_disabled(true);
		last->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	}
	hbc->add_child(last);

	hbc->add_spacer();

	return hbc;
}

void EditorAssetLibrary::_api_request(const String &p_request, RequestType p_request_type, const String &p_arguments) {
	if (requesting != REQUESTING_NONE) {
		request->cancel_request();
	}
	error_hb->hide();

	if (loading_blocked) {
		_set_library_message_with_action(TTRC("The Asset Library requires an online connection and involves sending data over the internet."), TTRC("Go Online"), callable_mp(this, &EditorAssetLibrary::_force_online_mode));
		return;
	}

	requesting = p_request_type;
	request->request(host + "/" + p_request + p_arguments);
}

void EditorAssetLibrary::_http_request_completed(int p_status, int p_code, const PackedStringArray &headers, const PackedByteArray &p_data) {
	String str = String::utf8((const char *)p_data.ptr(), (int)p_data.size());
	bool error_abort = true;

	switch (p_status) {
		case HTTPRequest::RESULT_CANT_RESOLVE: {
			error_label->set_text(TTR("Can't resolve hostname:") + " " + host);
		} break;
		case HTTPRequest::RESULT_BODY_SIZE_LIMIT_EXCEEDED:
		case HTTPRequest::RESULT_CONNECTION_ERROR:
		case HTTPRequest::RESULT_CHUNKED_BODY_SIZE_MISMATCH: {
			error_label->set_text(TTR("Connection error, please try again."));
		} break;
		case HTTPRequest::RESULT_TLS_HANDSHAKE_ERROR:
		case HTTPRequest::RESULT_CANT_CONNECT: {
			error_label->set_text(TTR("Can't connect to host:") + " " + host);
		} break;
		case HTTPRequest::RESULT_NO_RESPONSE: {
			error_label->set_text(TTR("No response from host:") + " " + host);
		} break;
		case HTTPRequest::RESULT_REQUEST_FAILED: {
			error_label->set_text(TTR("Request failed, return code:") + " " + itos(p_code));
		} break;
		case HTTPRequest::RESULT_REDIRECT_LIMIT_REACHED: {
			error_label->set_text(TTRC("Request failed, too many redirects"));

		} break;
		default: {
			if (p_code != 200) {
				error_label->set_text(TTR("Request failed, return code:") + " " + itos(p_code));
			} else {
				error_abort = false;
			}
		} break;
	}

	if (error_abort) {
		if (requesting == REQUESTING_CONFIG) {
			_set_library_message_with_action(TTRC("Failed to get repository configuration."), TTRC("Retry"), callable_mp(this, &EditorAssetLibrary::_request_current_config));
		}
		error_hb->show();
		return;
	}

	Dictionary d;
	{
		JSON json;
		json.parse(str);
		d = json.get_data();
	}

	RequestType requested = requesting;
	requesting = REQUESTING_NONE;

	switch (requested) {
		case REQUESTING_CONFIG: {
			categories->clear();
			categories->add_item(TTRC("All"));
			categories->set_item_metadata(0, 0);
			if (d.has("categories")) {
				Array clist = d["categories"];
				for (int i = 0; i < clist.size(); i++) {
					Dictionary cat = clist[i];
					if (!cat.has("name") || !cat.has("id")) {
						continue;
					}
					String name = cat["name"];
					int id = cat["id"];
					categories->add_item(name);
					categories->set_item_metadata(-1, id);
					category_map[cat["id"]] = name;
				}
			}

			filter->set_editable(true);
			sort->set_disabled(false);
			categories->set_disabled(false);
			support->set_disabled(false);

			_search();
		} break;
		case REQUESTING_SEARCH: {
			initial_loading = false;

			if (asset_items) {
				memdelete(asset_items);
			}

			if (asset_top_page) {
				memdelete(asset_top_page);
			}

			if (asset_bottom_page) {
				memdelete(asset_bottom_page);
			}

			int page = 0;
			int pages = 1;
			int page_len = 10;
			int total_items = 1;
			Array result;

			if (d.has("page")) {
				page = d["page"];
			}
			if (d.has("pages")) {
				pages = d["pages"];
			}
			if (d.has("page_length")) {
				page_len = d["page_length"];
			}
			if (d.has("total")) {
				total_items = d["total"];
			}
			if (d.has("result")) {
				result = d["result"];
			}

			asset_top_page = _make_pages(page, pages, page_len, total_items, result.size());
			library_vb->add_child(asset_top_page);

			asset_items = memnew(GridContainer);
			asset_items->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			_update_asset_items_columns();
			asset_items->add_theme_constant_override("h_separation", 10 * EDSCALE);
			asset_items->add_theme_constant_override("v_separation", 10 * EDSCALE);

			contents->add_child(asset_items);

			asset_bottom_page = _make_pages(page, pages, page_len, total_items, result.size());
			library_vb->add_child(asset_bottom_page);

			if (result.is_empty()) {
				String support_list;
				for (int i = 0; i < SUPPORT_MAX; i++) {
					if (support->get_popup()->is_item_checked(i)) {
						if (!support_list.is_empty()) {
							support_list += ", ";
						}
						support_list += TTRGET(support_text[i]);
					}
				}
				if (support_list.is_empty()) {
					support_list = "-";
				}

				if (!filter->get_text().is_empty()) {
					_set_library_message(
							vformat(TTR("No results for \"%s\" for support level(s): %s."), filter->get_text(), support_list));
				} else {
					// No results, even though the user didn't search for anything specific.
					// This is typically because the version number changed recently
					// and no assets compatible with the new version have been published yet.
					_set_library_message(
							vformat(TTR("No results compatible with %s %s for support level(s): %s.\nCheck the enabled support levels using the 'Support' button in the top-right corner."), String(GODOT_VERSION_SHORT_NAME).capitalize(), String(GODOT_VERSION_BRANCH), support_list));
				}
			} else {
				library_message_box->hide();
			}

			for (int i = 0; i < result.size(); i++) {
				Dictionary r = result[i];

				ERR_CONTINUE(!r.has("title"));
				ERR_CONTINUE(!r.has("asset_id"));
				ERR_CONTINUE(!r.has("author"));
				ERR_CONTINUE(!r.has("author_id"));
				ERR_CONTINUE(!r.has("category_id"));
				ERR_FAIL_COND(!category_map.has(r["category_id"]));
				ERR_CONTINUE(!r.has("cost"));

				EditorAssetLibraryItem *item = memnew(EditorAssetLibraryItem(true));
				asset_items->add_child(item);
				asset_items->connect(SceneStringName(sort_children), callable_mp(item, &EditorAssetLibraryItem::calculate_misc_links_ratio));
				item->configure(r["title"], r["asset_id"], category_map[r["category_id"]], r["category_id"], r["author"], r["author_id"], r["cost"]);
				item->connect("asset_selected", callable_mp(this, &EditorAssetLibrary::_select_asset));
				item->connect("author_selected", callable_mp(this, &EditorAssetLibrary::_select_author));
				item->connect("category_selected", callable_mp(this, &EditorAssetLibrary::_select_category));

				if (r.has("icon_url") && !r["icon_url"].operator String().is_empty()) {
					_request_image(item->get_instance_id(), r["asset_id"], r["icon_url"], IMAGE_QUEUE_ICON, 0);
				}
			}

			if (!result.is_empty()) {
				library_scroll->set_v_scroll(0);
			}
		} break;
		case REQUESTING_ASSET: {
			Dictionary r = d;

			ERR_FAIL_COND(!r.has("title"));
			ERR_FAIL_COND(!r.has("asset_id"));
			ERR_FAIL_COND(!r.has("author"));
			ERR_FAIL_COND(!r.has("author_id"));
			ERR_FAIL_COND(!r.has("version"));
			ERR_FAIL_COND(!r.has("version_string"));
			ERR_FAIL_COND(!r.has("category_id"));
			ERR_FAIL_COND(!category_map.has(r["category_id"]));
			ERR_FAIL_COND(!r.has("cost"));
			ERR_FAIL_COND(!r.has("description"));
			ERR_FAIL_COND(!r.has("download_url"));
			ERR_FAIL_COND(!r.has("download_hash"));
			ERR_FAIL_COND(!r.has("browse_url"));

			if (description) {
				memdelete(description);
			}

			description = memnew(EditorAssetLibraryItemDescription);
			add_child(description);
			description->connect(SceneStringName(confirmed), callable_mp(this, &EditorAssetLibrary::_install_asset));

			description->configure(r["title"], r["asset_id"], category_map[r["category_id"]], r["category_id"], r["author"], r["author_id"], r["cost"], r["version"], r["version_string"], r["description"], r["download_url"], r["browse_url"], r["download_hash"]);

			EditorAssetLibraryItemDownload *download_item = _get_asset_in_progress(description->get_asset_id());
			if (download_item) {
				if (download_item->can_install()) {
					description->set_ok_button_text(TTRC("Install"));
					description->get_ok_button()->set_disabled(false);
				} else {
					description->set_ok_button_text(TTRC("Downloading..."));
					description->get_ok_button()->set_disabled(true);
				}
			} else {
				description->set_ok_button_text(TTRC("Download"));
				description->get_ok_button()->set_disabled(false);
			}

			if (r.has("icon_url") && !r["icon_url"].operator String().is_empty()) {
				_request_image(description->get_instance_id(), r["asset_id"], r["icon_url"], IMAGE_QUEUE_ICON, 0);
			}

			if (d.has("previews")) {
				Array previews = d["previews"];

				for (int i = 0; i < previews.size(); i++) {
					Dictionary p = previews[i];

					ERR_CONTINUE(!p.has("type"));
					ERR_CONTINUE(!p.has("link"));

					bool is_video = p.has("type") && String(p["type"]) == "video";
					String video_url;
					if (is_video && p.has("link")) {
						video_url = p["link"];
					}

					description->add_preview(i, is_video, video_url);

					if (p.has("thumbnail")) {
						_request_image(description->get_instance_id(), r["asset_id"], p["thumbnail"], IMAGE_QUEUE_THUMBNAIL, i);
					}

					if (!is_video) {
						_request_image(description->get_instance_id(), r["asset_id"], p["link"], IMAGE_QUEUE_SCREENSHOT, i);
					}
				}
			}

			description->popup_centered();
		} break;
		default:
			break;
	}
}

void EditorAssetLibrary::_asset_file_selected(const String &p_file) {
	if (asset_installer) {
		memdelete(asset_installer);
		asset_installer = nullptr;
	}

	asset_installer = memnew(EditorAssetInstaller);
	asset_installer->set_asset_name(p_file);
	add_child(asset_installer);
	asset_installer->open_asset(p_file);
}

void EditorAssetLibrary::_asset_open() {
	asset_open->popup_file_dialog();
}

void EditorAssetLibrary::_manage_plugins() {
	ProjectSettingsEditor::get_singleton()->popup_project_settings(true);
	ProjectSettingsEditor::get_singleton()->set_plugins_page();
}

EditorAssetLibraryItemDownload *EditorAssetLibrary::_get_asset_in_progress(int p_asset_id) const {
	for (int i = 0; i < downloads_hb->get_child_count(); i++) {
		EditorAssetLibraryItemDownload *d = Object::cast_to<EditorAssetLibraryItemDownload>(downloads_hb->get_child(i));
		if (d && d->get_asset_id() == p_asset_id) {
			return d;
		}
	}

	return nullptr;
}

void EditorAssetLibrary::_install_external_asset(String p_zip_path, String p_title) {
	emit_signal(SNAME("install_asset"), p_zip_path, p_title);
}

void EditorAssetLibrary::_update_asset_items_columns() {
	if (!asset_items) {
		return;
	}

	const float item_width = source_mode == ASSET_SOURCE_SOLERS ? 210.0 : 450.0;
	float available_width = asset_items->get_size().x;
	if (available_width <= 1.0 && library_scroll) {
		available_width = library_scroll->get_size().x;
	}
	if (available_width <= 1.0) {
		available_width = get_size().x;
	}

	int new_columns = available_width / (item_width * EDSCALE);
	new_columns = MAX(1, new_columns);

	if (new_columns != asset_items->get_columns()) {
		asset_items->set_columns(new_columns);
	}
}

void EditorAssetLibrary::_set_library_message(const String &p_message) {
	library_message->set_text(p_message);

	if (library_message_action.is_valid()) {
		library_message_button->disconnect(SceneStringName(pressed), library_message_action);
		library_message_action = Callable();
	}
	library_message_button->hide();

	library_message_box->show();
}

void EditorAssetLibrary::_set_library_message_with_action(const String &p_message, const String &p_action_text, const Callable &p_action) {
	library_message->set_text(p_message);

	library_message_button->set_text(p_action_text);
	if (library_message_action.is_valid()) {
		library_message_button->disconnect(SceneStringName(pressed), library_message_action);
		library_message_action = Callable();
	}
	library_message_action = p_action;
	library_message_button->connect(SceneStringName(pressed), library_message_action);
	library_message_button->show();

	library_message_box->show();
}

void EditorAssetLibrary::_force_online_mode() {
	EditorSettings::get_singleton()->set_setting("network/connection/network_mode", EditorSettings::NETWORK_ONLINE);
	EditorSettings::get_singleton()->notify_changes();
	EditorSettings::get_singleton()->save();
}

String EditorAssetLibrary::_solers_library_signature() const {
	String signature;
	const PackedStringArray dirs = DirAccess::get_directories_at("user://solers_library/assets");
	for (int i = 0; i < dirs.size(); i++) {
		const String manifest_path = String("user://solers_library/assets").path_join(dirs[i]).path_join("manifest.json");
		if (FileAccess::exists(manifest_path)) {
			signature += dirs[i] + ":" + String::num_uint64(FileAccess::get_modified_time(manifest_path)) + ";";
		}
	}
	return signature;
}

Dictionary EditorAssetLibrary::_read_solers_asset_manifest(const String &p_asset_id) const {
	const String manifest_path = String("user://solers_library/assets").path_join(p_asset_id).path_join("manifest.json");
	if (!FileAccess::exists(manifest_path)) {
		return Dictionary();
	}

	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(manifest_path));
	if (parsed.get_type() != Variant::DICTIONARY) {
		return Dictionary();
	}
	return parsed;
}

String EditorAssetLibrary::_solers_asset_root_id(const String &p_asset_id, const Dictionary &p_manifests_by_id) const {
	String asset_id = p_asset_id;
	for (int i = 0; i < 64; i++) {
		const Dictionary manifest = p_manifests_by_id.get(asset_id, Dictionary());
		const String parent_id = String(manifest.get("parent_asset_id", String()));
		if (parent_id.is_empty() || !p_manifests_by_id.has(parent_id)) {
			return asset_id;
		}
		asset_id = parent_id;
	}
	return p_asset_id;
}

Array EditorAssetLibrary::_solers_asset_group_ids(const String &p_root_asset_id) const {
	Dictionary manifests_by_id;
	const PackedStringArray dirs = DirAccess::get_directories_at("user://solers_library/assets");
	for (int i = 0; i < dirs.size(); i++) {
		const Dictionary manifest = _read_solers_asset_manifest(dirs[i]);
		if (!manifest.is_empty()) {
			manifests_by_id[dirs[i]] = manifest;
		}
	}

	Array group_ids;
	for (int i = 0; i < dirs.size(); i++) {
		if (manifests_by_id.has(dirs[i]) && _solers_asset_root_id(dirs[i], manifests_by_id) == p_root_asset_id) {
			group_ids.push_back(dirs[i]);
		}
	}
	return group_ids;
}

String EditorAssetLibrary::_solers_asset_preview_path(const String &p_asset_id, const Dictionary &p_manifest) const {
	const String preview_file = p_manifest.get("preview_file", String());
	if (!preview_file.is_empty() && FileAccess::exists(preview_file)) {
		return preview_file;
	}

	const String asset_dir = String("user://solers_library/assets").path_join(p_asset_id);
	const PackedStringArray files = DirAccess::get_files_at(asset_dir);
	for (int i = 0; i < files.size(); i++) {
		if (files[i].get_basename() == "preview") {
			const String path = asset_dir.path_join(files[i]);
			return path;
		}
	}
	return String();
}

String EditorAssetLibrary::_solers_asset_source_path(const Dictionary &p_manifest) const {
	const Array files = p_manifest.get("files", Array());
	if (files.is_empty()) {
		return String();
	}
	return String(files[0]);
}

void EditorAssetLibrary::_show_solers_library() {
	solers_detail_visible = false;
	solers_detail_asset_id = String();
	solers_detail_manifest.clear();
	_clear_solers_detail_preview();
	_sync_solers_filter_menus();
	solers_library_signature = _solers_library_signature();
	library_message_box->hide();
	asset_top_page->hide();
	asset_bottom_page->hide();
	asset_items->show();
	contents->show();
	if (solers_detail) {
		solers_detail->hide();
	}
	if (solers_detail_version_row) {
		solers_detail_version_row->hide();
	}
	_update_asset_items_columns();
	for (int i = asset_items->get_child_count() - 1; i >= 0; i--) {
		memdelete(asset_items->get_child(i));
	}

	const PackedStringArray dirs = DirAccess::get_directories_at("user://solers_library/assets");
	Dictionary manifests_by_id;
	Dictionary groups_by_root;
	Array root_order;
	for (int i = 0; i < dirs.size(); i++) {
		const Dictionary manifest = _read_solers_asset_manifest(dirs[i]);
		if (manifest.is_empty()) {
			continue;
		}
		manifests_by_id[dirs[i]] = manifest;
	}
	for (int i = 0; i < dirs.size(); i++) {
		if (!manifests_by_id.has(dirs[i])) {
			continue;
		}
		const String root_id = _solers_asset_root_id(dirs[i], manifests_by_id);
		Array group_ids = groups_by_root.get(root_id, Array());
		if (group_ids.is_empty()) {
			root_order.push_back(root_id);
		}
		group_ids.push_back(dirs[i]);
		groups_by_root[root_id] = group_ids;
	}

	for (int i = 0; i < root_order.size(); i++) {
		const String root_id = root_order[i];
		const Dictionary manifest = manifests_by_id.get(root_id, Dictionary());
		if (manifest.is_empty()) {
			continue;
		}
		const Array group_ids = groups_by_root.get(root_id, Array());
		bool group_matches = false;
		for (int j = 0; j < group_ids.size(); j++) {
			const Dictionary item_manifest = manifests_by_id.get(group_ids[j], Dictionary());
			const String item_kind = String(item_manifest.get("kind", "asset")).to_lower();
			const String item_status = solers_asset_display_status(item_manifest);
			if ((solers_kind_filter.is_empty() || item_kind == solers_kind_filter) && solers_asset_status_matches(solers_status_filter, item_status)) {
				group_matches = true;
				break;
			}
		}
		if (!group_matches) {
			continue;
		}
		const String manifest_kind = String(manifest.get("kind", "asset")).to_lower();
		const String manifest_status = solers_asset_group_status(group_ids, manifests_by_id);
		PanelContainer *card = memnew(PanelContainer);
		card->set_custom_minimum_size(Size2(196, 210) * EDSCALE);
		card->add_theme_style_override(SceneStringName(panel), get_theme_stylebox(SNAME("solers_asset_card"), SNAME("AssetLib")));
		if (manifest_kind == "3d") {
			card->set_mouse_filter(Control::MOUSE_FILTER_STOP);
			card->connect(SceneStringName(gui_input), callable_mp(this, &EditorAssetLibrary::_solers_asset_card_input).bind(root_id));
			card->set_tooltip_text(TTR("Open asset details"));
		}
		asset_items->add_child(card);

		VBoxContainer *content = memnew(VBoxContainer);
		content->add_theme_constant_override("separation", 6 * EDSCALE);
		card->add_child(content);

		const String preview_path = _solers_asset_preview_path(root_id, manifest);
		TextureRect *preview = memnew(TextureRect);
		preview->set_custom_minimum_size(Size2(180, 102) * EDSCALE);
		preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		preview->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
		if (FileAccess::exists(preview_path)) {
			Ref<Image> image = Image::load_from_file(preview_path);
			if (image.is_valid()) {
				preview->set_texture(ImageTexture::create_from_image(image));
			}
		} else {
			Ref<Image> image;
			image.instantiate();
			image->initialize_data(80, 45, false, Image::FORMAT_RGBA8);
			const Color base = manifest_kind == "music" ? Color(0.15f, 0.23f, 0.20f) : (manifest_kind == "sfx" ? Color(0.22f, 0.17f, 0.23f) : Color(0.12f, 0.16f, 0.23f));
			const Color line = Color(0.53f, 0.65f, 0.86f, 0.42f);
			for (int y = 0; y < 45; y++) {
				for (int x = 0; x < 80; x++) {
					image->set_pixel(x, y, ((x / 8 + y / 8) % 2) ? base : base.lightened(0.08f));
				}
			}
			for (int x = 0; x < 80; x += 8) {
				for (int y = 0; y < 45; y++) {
					image->set_pixel(x, y, line);
				}
			}
			for (int y = 0; y < 45; y += 8) {
				for (int x = 0; x < 80; x++) {
					image->set_pixel(x, y, line);
				}
			}
			preview->set_texture(ImageTexture::create_from_image(image));
		}
		content->add_child(preview);

		Label *title = memnew(Label(String(manifest.get("name", root_id))));
		title->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		title->add_theme_font_size_override(SceneStringName(font_size), 14 * EDSCALE);
		title->add_theme_color_override(SceneStringName(font_color), Color(0.91f, 0.94f, 0.98f));
		content->add_child(title);

		HBoxContainer *meta_row = memnew(HBoxContainer);
		meta_row->add_theme_constant_override("separation", 6 * EDSCALE);
		content->add_child(meta_row);

		const Color status_color = solers_asset_status_color(manifest_status);
		Label *status_label = memnew(Label(manifest_status.to_upper()));
		status_label->add_theme_font_size_override(SceneStringName(font_size), 11 * EDSCALE);
		status_label->add_theme_color_override(SceneStringName(font_color), status_color);
		meta_row->add_child(status_label);

		int rig_count = 0;
		int animation_count = 0;
		for (int j = 0; j < group_ids.size(); j++) {
			const Dictionary item_manifest = manifests_by_id.get(group_ids[j], Dictionary());
			const String operation = String(item_manifest.get("operation", String()));
			rig_count += operation == "rig_humanoid" ? 1 : 0;
			animation_count += operation == "animate_humanoid" ? 1 : 0;
		}
		String kind_text = manifest_kind.to_upper();
		if (rig_count > 0 || animation_count > 0) {
			kind_text = vformat("%s%s%s", TTRC("Static"), rig_count > 0 ? String(" · ") + TTRC("Rig") : String(), animation_count > 0 ? vformat(" · %d %s", animation_count, TTRC("Animations")) : String());
		}
		Label *kind = memnew(Label(kind_text));
		kind->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		kind->add_theme_font_size_override(SceneStringName(font_size), 11 * EDSCALE);
		kind->add_theme_color_override(SceneStringName(font_color), Color(0.66f, 0.74f, 0.86f));
		meta_row->add_child(kind);

		Label *prompt = memnew(Label(String(manifest.get("prompt", String()))));
		prompt->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		prompt->add_theme_font_size_override(SceneStringName(font_size), 12 * EDSCALE);
		prompt->add_theme_color_override(SceneStringName(font_color), Color(0.70f, 0.76f, 0.86f, 0.78f));
		content->add_child(prompt);
	}
	if (asset_items->get_child_count() == 0) {
		PanelContainer *empty_card = memnew(PanelContainer);
		empty_card->set_custom_minimum_size(Size2(196, 120) * EDSCALE);
		empty_card->add_theme_style_override(SceneStringName(panel), get_theme_stylebox(SNAME("solers_asset_card"), SNAME("AssetLib")));
		asset_items->add_child(empty_card);

		Label *empty = memnew(Label(TTRC("No Solers assets match the current filters.")));
		empty->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		empty->add_theme_color_override(SceneStringName(font_color), Color(0.70f, 0.76f, 0.86f, 0.78f));
		empty_card->add_child(empty);
	}
	callable_mp(this, &EditorAssetLibrary::_update_asset_items_columns).call_deferred();
}

void EditorAssetLibrary::_solers_asset_card_input(const Ref<InputEvent> &p_input, const String &p_asset_id) {
	Ref<InputEventMouseButton> mb = p_input;
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
		_open_solers_asset_detail(p_asset_id);
	}
}

void EditorAssetLibrary::_open_solers_asset_detail(const String &p_asset_id) {
	const Dictionary manifest = _read_solers_asset_manifest(p_asset_id);
	if (manifest.is_empty()) {
		return;
	}
	if (String(manifest.get("kind", String())).to_lower() != "3d") {
		return;
	}
	_show_solers_asset_detail(p_asset_id, manifest);
}

void EditorAssetLibrary::_populate_solers_detail_versions(const String &p_root_asset_id, const String &p_selected_asset_id) {
	if (!solers_detail_version_row) {
		return;
	}
	for (int i = solers_detail_version_row->get_child_count() - 1; i >= 0; i--) {
		Node *child = solers_detail_version_row->get_child(i);
		solers_detail_version_row->remove_child(child);
		child->queue_free();
	}

	const Array group_ids = _solers_asset_group_ids(p_root_asset_id);
	if (group_ids.size() <= 1) {
		solers_detail_version_row->hide();
		return;
	}
	for (int i = 0; i < group_ids.size(); i++) {
		const String asset_id = group_ids[i];
		const Dictionary manifest = _read_solers_asset_manifest(asset_id);
		if (String(manifest.get("kind", String())).to_lower() != "3d") {
			continue;
		}
		Button *button = memnew(Button);
		button->set_text(solers_asset_version_label(asset_id, manifest, p_root_asset_id));
		button->set_tooltip_text(String(manifest.get("name", asset_id)));
		solers_style_segment_button(button, asset_id == p_selected_asset_id);
		button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_open_solers_asset_detail).bind(asset_id), CONNECT_DEFERRED);
		solers_detail_version_row->add_child(button);
	}
	solers_detail_version_row->set_visible(solers_detail_version_row->get_child_count() > 1);
}

void EditorAssetLibrary::_ensure_solers_detail_view() {
	if (solers_detail) {
		return;
	}

	solers_detail = memnew(BoxContainer);
	solers_detail->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	solers_detail->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	solers_detail->add_theme_constant_override("separation", 22 * EDSCALE);
	solers_detail->hide();
	library_vb->add_child(solers_detail);

	solers_detail_content = memnew(VBoxContainer);
	solers_detail_content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	solers_detail_content->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	solers_detail_content->add_theme_constant_override("separation", 10 * EDSCALE);
	solers_detail->add_child(solers_detail_content);

	solers_detail_version_row = memnew(HFlowContainer);
	solers_detail_version_row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	solers_detail_version_row->set_alignment(FlowContainer::ALIGNMENT_BEGIN);
	solers_detail_version_row->add_theme_constant_override("h_separation", 0 * EDSCALE);
	solers_detail_version_row->add_theme_constant_override("v_separation", 6 * EDSCALE);
	solers_detail_version_row->hide();
	solers_detail_content->add_child(solers_detail_version_row);

	solers_detail_viewer_panel = memnew(PanelContainer);
	solers_detail_viewer_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	solers_detail_viewer_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	solers_detail_viewer_panel->set_custom_minimum_size(Size2(560, 560) * EDSCALE);
	Ref<StyleBoxFlat> viewer_style;
	viewer_style.instantiate();
	viewer_style->set_bg_color(Color(0.045f, 0.045f, 0.047f));
	viewer_style->set_border_color(Color(1, 1, 1, 0.08f));
	viewer_style->set_border_width_all(1 * EDSCALE);
	viewer_style->set_corner_radius_all(12 * EDSCALE);
	solers_detail_viewer_panel->add_theme_style_override(SceneStringName(panel), viewer_style);
	solers_detail_content->add_child(solers_detail_viewer_panel);

	Control *viewer_root = memnew(Control);
	viewer_root->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	solers_detail_viewer_panel->add_child(viewer_root);

	solers_detail_viewport_container = memnew(SubViewportContainer);
	solers_detail_viewport_container->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	solers_detail_viewport_container->set_stretch(true);
	solers_detail_viewport_container->set_force_pass_scroll_events(false);
	solers_detail_viewport_container->connect(SceneStringName(gui_input), callable_mp(this, &EditorAssetLibrary::_solers_detail_viewport_input));
	viewer_root->add_child(solers_detail_viewport_container);

	solers_detail_viewport = memnew(SubViewport);
	solers_detail_viewport->set_use_own_world_3d(true);
	solers_detail_viewport->set_update_mode(SubViewport::UPDATE_WHEN_VISIBLE);
	solers_detail_viewport->set_transparent_background(false);
	solers_detail_viewport_container->add_child(solers_detail_viewport);

	WorldEnvironment *world_environment = memnew(WorldEnvironment);
	Ref<ProceduralSkyMaterial> sky_material;
	sky_material.instantiate();
	sky_material->set_sky_top_color(Color(0.12f, 0.12f, 0.13f));
	sky_material->set_sky_horizon_color(Color(0.06f, 0.06f, 0.065f));
	sky_material->set_ground_horizon_color(Color(0.045f, 0.045f, 0.047f));
	sky_material->set_ground_bottom_color(Color(0.025f, 0.025f, 0.027f));
	sky_material->set_sun_angle_max(0.0);
	Ref<Sky> sky;
	sky.instantiate();
	sky->set_material(sky_material);
	sky->set_radiance_size(Sky::RADIANCE_SIZE_512);
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_background(Environment::BG_COLOR);
	environment->set_bg_color(Color(0.135f, 0.137f, 0.140f));
	environment->set_sky(sky);
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
	environment->set_ambient_light_energy(0.72);
	environment->set_reflection_source(Environment::REFLECTION_SOURCE_SKY);
	environment->set_tonemapper(Environment::TONE_MAPPER_AGX);
	environment->set_tonemap_exposure(1.02);
	environment->set_tonemap_agx_white(16.0);
	environment->set_tonemap_agx_contrast(1.02);
	world_environment->set_environment(environment);
	solers_detail_viewport->add_child(world_environment);

	solers_detail_scene_root = memnew(Node3D);
	solers_detail_viewport->add_child(solers_detail_scene_root);

	solers_detail_camera = memnew(Camera3D);
	solers_detail_camera->set_perspective(42.0, 0.01, 4000.0);
	solers_detail_camera->make_current();
	solers_detail_viewport->add_child(solers_detail_camera);

	solers_detail_light_1 = memnew(DirectionalLight3D);
	solers_detail_light_1->set_color(Color(1.0f, 0.96f, 0.90f));
	solers_detail_light_1->set_param(Light3D::PARAM_ENERGY, 1.80);
	solers_detail_light_1->set_param(Light3D::PARAM_SPECULAR, 0.30);
	solers_detail_light_1->set_transform(Transform3D(Basis::looking_at(Vector3(-0.75f, -0.85f, -0.45f))));
	solers_detail_camera->add_child(solers_detail_light_1);

	solers_detail_light_2 = memnew(DirectionalLight3D);
	solers_detail_light_2->set_color(Color(0.68f, 0.74f, 0.82f));
	solers_detail_light_2->set_param(Light3D::PARAM_ENERGY, 0.52);
	solers_detail_light_2->set_param(Light3D::PARAM_SPECULAR, 0.10);
	solers_detail_light_2->set_transform(Transform3D(Basis::looking_at(Vector3(0.8f, -0.45f, 0.55f))));
	solers_detail_camera->add_child(solers_detail_light_2);

	solers_detail_light_3 = memnew(DirectionalLight3D);
	solers_detail_light_3->set_color(Color(0.74f, 0.82f, 1.0f));
	solers_detail_light_3->set_param(Light3D::PARAM_ENERGY, 0.95);
	solers_detail_light_3->set_param(Light3D::PARAM_SPECULAR, 0.16);
	solers_detail_light_3->set_transform(Transform3D(Basis::looking_at(Vector3(0.25f, -0.55f, 1.0f))));
	solers_detail_camera->add_child(solers_detail_light_3);

	solers_detail_expand_button = memnew(Button);
	solers_detail_expand_button->set_text(TTRC("Expand"));
	solers_detail_expand_button->set_theme_type_variation("SolersFlatPillButton");
	solers_detail_expand_button->set_anchors_and_offsets_preset(Control::PRESET_TOP_RIGHT, Control::PRESET_MODE_MINSIZE, 12 * EDSCALE);
	solers_detail_expand_button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_toggle_solers_detail_fullscreen));
	viewer_root->add_child(solers_detail_expand_button);

	solers_detail_inspector_panel = memnew(PanelContainer);
	solers_detail_inspector_panel->set_anchor(SIDE_LEFT, Control::ANCHOR_BEGIN);
	solers_detail_inspector_panel->set_anchor(SIDE_TOP, Control::ANCHOR_BEGIN);
	solers_detail_inspector_panel->set_anchor(SIDE_RIGHT, Control::ANCHOR_BEGIN);
	solers_detail_inspector_panel->set_anchor(SIDE_BOTTOM, Control::ANCHOR_BEGIN);
	solers_detail_inspector_panel->set_offset(SIDE_LEFT, 16 * EDSCALE);
	solers_detail_inspector_panel->set_offset(SIDE_TOP, 72 * EDSCALE);
	solers_detail_inspector_panel->set_offset(SIDE_RIGHT, 292 * EDSCALE);
	solers_detail_inspector_panel->set_offset(SIDE_BOTTOM, 338 * EDSCALE);
	solers_detail_inspector_panel->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	Ref<StyleBoxFlat> inspector_style;
	inspector_style.instantiate();
	inspector_style->set_bg_color(Color(0.012f, 0.012f, 0.014f, 0.94f));
	inspector_style->set_border_color(Color(1, 1, 1, 0.12f));
	inspector_style->set_border_width_all(1 * EDSCALE);
	inspector_style->set_corner_radius_all(8 * EDSCALE);
	inspector_style->set_content_margin_all(10 * EDSCALE);
	solers_detail_inspector_panel->add_theme_style_override(SceneStringName(panel), inspector_style);
	viewer_root->add_child(solers_detail_inspector_panel);

	solers_detail_inspector_content = memnew(VBoxContainer);
	solers_detail_inspector_content->add_theme_constant_override("separation", 8 * EDSCALE);
	solers_detail_inspector_panel->add_child(solers_detail_inspector_content);
	solers_detail_inspector_panel->hide();

	HBoxContainer *tools = memnew(HBoxContainer);
	tools->add_theme_constant_override("separation", 6 * EDSCALE);
	tools->set_anchors_and_offsets_preset(Control::PRESET_CENTER_BOTTOM, Control::PRESET_MODE_MINSIZE, 12 * EDSCALE);
	viewer_root->add_child(tools);

	solers_detail_inspector_button = solers_make_viewer_button(TTRC("View"));
	solers_detail_inspector_button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_toggle_solers_detail_inspector));
	tools->add_child(solers_detail_inspector_button);

	Button *orbit = solers_make_viewer_button(TTRC("Orbit"));
	orbit->set_disabled(true);
	tools->add_child(orbit);

	Button *reset = solers_make_viewer_button(TTRC("Reset"));
	reset->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_reset_solers_detail_camera));
	tools->add_child(reset);

	solers_detail_message = solers_make_label(String(), 13, Color(0.76f, 0.80f, 0.88f), true);
	solers_detail_message->set_anchors_and_offsets_preset(Control::PRESET_CENTER, Control::PRESET_MODE_MINSIZE);
	solers_detail_message->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	viewer_root->add_child(solers_detail_message);

	solers_detail_info_panel = memnew(PanelContainer);
	solers_detail_info_panel->set_custom_minimum_size(Size2(340, 420) * EDSCALE);
	solers_detail_info_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	solers_detail_info_panel->add_theme_style_override(SceneStringName(panel), get_theme_stylebox(SNAME("solers_asset_card"), SNAME("AssetLib")));
	solers_detail->add_child(solers_detail_info_panel);

	solers_detail_info_content = memnew(VBoxContainer);
	solers_detail_info_content->add_theme_constant_override("separation", 10 * EDSCALE);
	solers_detail_info_panel->add_child(solers_detail_info_content);
}

void EditorAssetLibrary::_show_solers_asset_detail(const String &p_asset_id, const Dictionary &p_manifest) {
	_ensure_solers_detail_view();
	solers_detail_visible = true;
	solers_detail_asset_id = p_asset_id;
	solers_detail_manifest = p_manifest;
	Dictionary manifests_by_id;
	const PackedStringArray dirs = DirAccess::get_directories_at("user://solers_library/assets");
	for (int i = 0; i < dirs.size(); i++) {
		const Dictionary manifest = _read_solers_asset_manifest(dirs[i]);
		if (!manifest.is_empty()) {
			manifests_by_id[dirs[i]] = manifest;
		}
	}
	solers_detail_root_asset_id = _solers_asset_root_id(p_asset_id, manifests_by_id);
	solers_library_signature = _solers_library_signature();

	asset_items->hide();
	contents->hide();
	asset_top_page->hide();
	asset_bottom_page->hide();
	library_message_box->hide();
	solers_filter_row->hide();
	solers_detail_fullscreen = false;
	solers_detail_view_mode = SOLERS_DETAIL_VIEW_FINAL;
	solers_detail_show_skeleton = false;
	solers_detail_animation_loop = true;
	solers_detail_delete_confirm = false;
	if (solers_detail_viewport) {
		solers_detail_viewport->set_debug_draw(Viewport::DEBUG_DRAW_DISABLED);
	}
	if (solers_detail_inspector_panel) {
		solers_detail_inspector_panel->hide();
	}
	solers_detail->show();
	solers_detail->set_vertical(get_size().x < 980 * EDSCALE);
	_sync_solers_detail_fullscreen();
	source_title->set_text(String(p_manifest.get("name", p_asset_id)));
	_populate_solers_detail_versions(solers_detail_root_asset_id, p_asset_id);

	for (int i = solers_detail_info_content->get_child_count() - 1; i >= 0; i--) {
		memdelete(solers_detail_info_content->get_child(i));
	}

	HBoxContainer *badge_row = memnew(HBoxContainer);
	badge_row->add_theme_constant_override("separation", 8 * EDSCALE);
	solers_detail_info_content->add_child(badge_row);
	badge_row->add_child(solers_make_label(TTRC("Solers"), 13, Color(0.92f, 0.92f, 0.88f)));
	const String display_status = solers_asset_display_status(p_manifest);
	const Color detail_status_color = display_status == "ready" ? Color(0.35f, 0.88f, 0.58f) : Color(0.85f, 0.70f, 0.42f);
	badge_row->add_child(solers_make_label(display_status.to_upper(), 12, detail_status_color));

	Label *title = solers_make_label(String(p_manifest.get("name", p_asset_id)), 24, Color(0.96f, 0.97f, 0.99f), true);
	solers_detail_info_content->add_child(title);
	solers_detail_info_content->add_child(solers_make_label(TTRC("3D Model"), 13, Color(0.32f, 0.75f, 1.0f)));

	solers_add_info_row(solers_detail_info_content, TTRC("Prompt"), String(p_manifest.get("prompt", String())));
	solers_add_info_row(solers_detail_info_content, TTRC("Provider"), String(p_manifest.get("provider", String())));
	solers_add_info_row(solers_detail_info_content, TTRC("Profile"), String(p_manifest.get("profile", String())));
	solers_add_info_row(solers_detail_info_content, TTRC("Updated"), String(p_manifest.get("updated_at", p_manifest.get("created_at", String()))));
	if (display_status == "failed") {
		const Dictionary error = p_manifest.get("error", Dictionary());
		solers_add_info_row(solers_detail_info_content, TTRC("Error"), String(error.get("message", TTRC("Unknown failure"))));
	}

	const Array files = p_manifest.get("files", Array());
	solers_add_info_row(solers_detail_info_content, TTRC("Files"), itos(files.size()));
	const String parent_asset_id = String(p_manifest.get("parent_asset_id", String()));
	if (!parent_asset_id.is_empty()) {
		Dictionary parent = _read_solers_asset_manifest(parent_asset_id);
		solers_add_info_row(solers_detail_info_content, TTRC("Derived From"), String(parent.get("name", parent_asset_id)));
	}
	const Dictionary animation_data = p_manifest.get("animations", Dictionary());
	const Dictionary basic_animations = animation_data.get("basic", Dictionary());
	if (!basic_animations.is_empty()) {
		solers_detail_info_content->add_spacer();
		solers_detail_info_content->add_child(solers_make_label(TTRC("Basic Animations"), 13, Color(0.92f, 0.92f, 0.88f)));
		PanelContainer *animation_panel = solers_make_action_panel();
		VBoxContainer *animation_list = memnew(VBoxContainer);
		animation_list->add_theme_constant_override("separation", 0);
		animation_panel->add_child(animation_list);
		solers_detail_info_content->add_child(animation_panel);

		Array names = basic_animations.keys();
		names.sort();
		for (int i = 0; i < names.size(); i++) {
			const Dictionary item = basic_animations[names[i]];
			const String path = String(item.get("glb_file", String()));
			HBoxContainer *row = solers_add_action_row(animation_list, i > 0);
			VBoxContainer *copy = memnew(VBoxContainer);
			copy->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			copy->add_theme_constant_override("separation", 3 * EDSCALE);
			solers_add_action_copy(copy, String(item.get("name", String(names[i]).capitalize())), TTRC("Meshy rigging basic animation."));
			row->add_child(copy);

			Button *view = memnew(Button);
			view->set_text(TTRC("View"));
			solers_style_action_button(view);
			view->set_disabled(path.is_empty() || !FileAccess::exists(path));
			view->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_view_solers_basic_animation).bind(path));
			row->add_child(view);

			Button *import = memnew(Button);
			import->set_text(TTRC("Import"));
			solers_style_action_button(import);
			import->set_disabled(path.is_empty() || !FileAccess::exists(path));
			import->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_import_solers_basic_animation).bind(path));
			row->add_child(import);
		}
	}

	solers_detail_info_content->add_spacer();
	solers_detail_info_content->add_child(solers_make_label(TTRC("Actions"), 13, Color(0.92f, 0.92f, 0.88f)));
	PanelContainer *action_panel = solers_make_action_panel();
	VBoxContainer *action_list = memnew(VBoxContainer);
	action_list->add_theme_constant_override("separation", 0);
	action_panel->add_child(action_list);
	solers_detail_info_content->add_child(action_panel);

	HBoxContainer *import_row = solers_add_action_row(action_list, false);
	VBoxContainer *import_copy = memnew(VBoxContainer);
	import_copy->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	import_copy->add_theme_constant_override("separation", 3 * EDSCALE);
	solers_add_action_copy(import_copy, TTRC("Project Import"), display_status == "draft" ? TTRC("Add this draft to the current project.") : TTRC("Add this asset to the current project."));
	import_row->add_child(import_copy);
	Button *import_button = memnew(Button);
	import_button->set_text(display_status == "draft" ? TTRC("Import Draft") : TTRC("Import"));
	solers_style_action_button(import_button, true);
	import_button->set_disabled(files.is_empty());
	import_button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_import_solers_detail_asset));
	import_row->add_child(import_button);

	Dictionary capability_args;
	capability_args["asset_id"] = p_asset_id;
	const Dictionary capability_result = solers_asset_service->capabilities(capability_args);
	if ((bool)capability_result.get("ok", false)) {
		const Dictionary capability_data = capability_result.get("data", Dictionary());
		const Array operations = capability_data.get("available_operations", Array());
		for (int i = 0; i < operations.size(); i++) {
			const Dictionary operation = operations[i];
			if (!(bool)operation.get("ui_supported", false)) {
				continue;
			}
			const String operation_id = String(operation.get("operation_id", String()));
			HBoxContainer *operation_row = solers_add_action_row(action_list);
			VBoxContainer *operation_copy = memnew(VBoxContainer);
			operation_copy->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			operation_copy->add_theme_constant_override("separation", 5 * EDSCALE);
			solers_add_action_copy(operation_copy, String(operation.get("label", operation_id)), String(operation.get("description", TTRC("Create a new asset version."))));

			Button *operation_button = memnew(Button);
			operation_button->set_text(String(operation.get("label", operation_id)));
			solers_style_action_button(operation_button);
			operation_button->set_meta("solers_operation_id", operation_id);
			operation_button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_run_solers_detail_operation).bind(operation_button));

			const Dictionary schema = operation.get("options_schema", Dictionary());
			const Dictionary properties = schema.get("properties", Dictionary());
			Array option_names = schema.get("ui_order", Array());
			if (option_names.is_empty()) {
				option_names = schema.get("required", Array());
			}
			for (int j = 0; j < option_names.size(); j++) {
				const String option_name = String(option_names[j]);
				const Dictionary property = properties.get(option_name, Dictionary());
				if (property.is_empty()) {
					continue;
				}
				Control *option_control = solers_make_operation_option_control(option_name, property);
				if (String(property.get("type", String())) == "boolean") {
					HBoxContainer *toggle_row = memnew(HBoxContainer);
					toggle_row->add_theme_constant_override("separation", 10 * EDSCALE);
					Label *toggle_label = solers_make_label(String(property.get("label", option_name.capitalize())), 12, Color(0.70f, 0.70f, 0.65f));
					toggle_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
					toggle_row->add_child(toggle_label);
					if (Button *toggle = Object::cast_to<Button>(option_control)) {
						toggle->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_set_solers_bool_option).bind(toggle, operation_button));
						toggle_row->add_child(toggle);
						operation_button->set_disabled(true);
					}
					operation_copy->add_child(toggle_row);
				} else {
					operation_copy->add_child(solers_make_label(String(property.get("label", option_name.capitalize())), 11, Color(0.58f, 0.58f, 0.53f)));
					operation_copy->add_child(option_control);
				}
			}
			operation_row->add_child(operation_copy);
			operation_row->add_child(operation_button);
		}
	}

	HBoxContainer *delete_row = solers_add_action_row(action_list);
	VBoxContainer *delete_copy = memnew(VBoxContainer);
	delete_copy->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	delete_copy->add_theme_constant_override("separation", 3 * EDSCALE);
	solers_add_action_copy(delete_copy, TTRC("Delete from Library"), TTRC("Permanently remove this local Solers Library asset."));
	delete_row->add_child(delete_copy);
	Button *delete_button = memnew(Button);
	delete_button->set_text(TTRC("Delete"));
	solers_style_action_button(delete_button);
	delete_button->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_delete_solers_detail_asset).bind(delete_button));
	delete_row->add_child(delete_button);

	solers_detail_import_status = solers_make_label(String(), 12, Color(0.66f, 0.74f, 0.86f), true);
	solers_detail_info_content->add_child(solers_detail_import_status);

	const String model_path = _solers_asset_source_path(p_manifest);
	if (model_path.is_empty()) {
		_clear_solers_detail_preview();
		solers_detail_message->set_text(TTRC("No source model file is available for preview."));
		solers_detail_message->show();
		return;
	}
	if (!_load_solers_detail_preview(model_path)) {
		solers_detail_message->set_text(TTRC("Godot could not load this model for preview."));
		solers_detail_message->show();
	}
}

void EditorAssetLibrary::_clear_solers_detail_preview() {
	if (!solers_detail_scene_root) {
		return;
	}
	solers_detail_skeleton_overlay = nullptr;
	solers_detail_animation_player = nullptr;
	solers_detail_animation_picker = nullptr;
	solers_detail_animation_time = nullptr;
	for (int i = solers_detail_scene_root->get_child_count() - 1; i >= 0; i--) {
		memdelete(solers_detail_scene_root->get_child(i));
	}
}

bool EditorAssetLibrary::_load_solers_detail_preview(const String &p_model_path) {
	_clear_solers_detail_preview();
	if (solers_detail_message) {
		solers_detail_message->hide();
	}

	Ref<GLTFDocument> document;
	document.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	state->set_handle_binary_image_mode(GLTFState::HANDLE_BINARY_IMAGE_MODE_EMBED_AS_UNCOMPRESSED);
	const Error err = document->append_from_file(p_model_path, state);
	if (err != OK) {
		return false;
	}

	Node *scene = document->generate_scene(state);
	if (!scene) {
		return false;
	}

	solers_convert_importer_meshes(scene);
	Ref<StandardMaterial3D> fallback_material;
	fallback_material.instantiate();
	fallback_material->set_albedo(Color(0.44f, 0.46f, 0.48f));
	fallback_material->set_roughness(0.88f);
	fallback_material->set_metallic(0.0f);
	fallback_material->set_specular(0.16f);
	solers_apply_preview_fallback_material(scene, fallback_material);
	solers_detail_scene_root->add_child(scene);
	solers_detail_aabb = AABB();
	_update_solers_detail_aabb(scene);
	if (solers_detail_aabb.size == Vector3()) {
		solers_detail_scene_root->remove_child(scene);
		memdelete(scene);
		return false;
	}
	solers_detail_animation_player = solers_find_animation_player(scene);
	if (solers_detail_animation_player) {
		solers_detail_animation_player->set_active(true);
		solers_detail_animation_player->set_callback_mode_process(AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_IDLE);
	}
	_reset_solers_detail_camera();
	_refresh_solers_detail_skeleton_overlay();
	return true;
}

void EditorAssetLibrary::_update_solers_detail_aabb(Node *p_node, const Transform3D &p_transform) {
	Node3D *node_3d = Object::cast_to<Node3D>(p_node);
	const Transform3D current = node_3d ? p_transform * node_3d->get_transform() : p_transform;

	VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(p_node);
	if (visual) {
		const AABB aabb = current.xform(visual->get_aabb());
		if (solers_detail_aabb.size == Vector3()) {
			solers_detail_aabb = aabb;
		} else {
			solers_detail_aabb.merge_with(aabb);
		}
	}

	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_node);
	if (skeleton && skeleton->get_bone_count() > 0) {
		AABB skeleton_aabb;
		for (int i = 0; i < skeleton->get_bone_count(); i++) {
			const Vector3 point = current.xform(skeleton->get_bone_global_pose(i).origin);
			if (i == 0) {
				skeleton_aabb = AABB(point, Vector3());
			} else {
				skeleton_aabb.expand_to(point);
			}
		}
		const float padding = MAX(0.01f, skeleton_aabb.get_longest_axis_size() * 0.04f);
		skeleton_aabb = skeleton_aabb.grow(padding);
		if (solers_detail_aabb.size == Vector3()) {
			solers_detail_aabb = skeleton_aabb;
		} else {
			solers_detail_aabb.merge_with(skeleton_aabb);
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_update_solers_detail_aabb(p_node->get_child(i), current);
	}
}

void EditorAssetLibrary::_reset_solers_detail_camera() {
	solers_detail_cam_rot_x = -Math::PI / 5;
	solers_detail_cam_rot_y = Math::PI / 4;
	solers_detail_cam_zoom = 1.0f;
	solers_detail_cam_pan = Vector3();
	_update_solers_detail_camera();
}

void EditorAssetLibrary::_update_solers_detail_camera() {
	if (!solers_detail_camera) {
		return;
	}

	const Vector3 center = solers_detail_aabb.get_center() + solers_detail_cam_pan;
	const float size = MAX(0.01f, solers_detail_aabb.get_longest_axis_size());
	Transform3D xf;
	xf.basis = Basis(Vector3(0, 1, 0), solers_detail_cam_rot_y) * Basis(Vector3(1, 0, 0), solers_detail_cam_rot_x);
	xf.origin = center;
	xf.translate_local(0, 0, size * 2.2f * solers_detail_cam_zoom);
	solers_detail_camera->set_transform(xf);
	solers_detail_camera->set_near(MAX(0.001f, size * 0.001f));
	solers_detail_camera->set_far(MAX(1000.0f, size * 10.0f));
}

void EditorAssetLibrary::_solers_detail_viewport_input(const Ref<InputEvent> &p_input) {
	bool handled = false;

	Ref<InputEventMouseMotion> mm = p_input;
	if (mm.is_valid() && mm->get_button_mask().has_flag(MouseButtonMask::LEFT)) {
		solers_detail_cam_rot_x -= mm->get_relative().y * 0.01 * EDSCALE;
		solers_detail_cam_rot_y -= mm->get_relative().x * 0.01 * EDSCALE;
		solers_detail_cam_rot_x = CLAMP(solers_detail_cam_rot_x, -Math::PI / 2, Math::PI / 2);
		_update_solers_detail_camera();
		handled = true;
	} else if (mm.is_valid() && mm->get_button_mask().has_flag(MouseButtonMask::MIDDLE)) {
		const float size = MAX(0.01f, solers_detail_aabb.get_longest_axis_size());
		const float pan_speed = size * solers_detail_cam_zoom * 0.0015f;
		const Basis basis = solers_detail_camera ? solers_detail_camera->get_global_transform().basis : Basis();
		solers_detail_cam_pan += (-basis.get_column(Vector3::AXIS_X) * mm->get_relative().x + basis.get_column(Vector3::AXIS_Y) * mm->get_relative().y) * pan_speed;
		_update_solers_detail_camera();
		handled = true;
	}

	Ref<InputEventMouseButton> mb = p_input;
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MouseButton::WHEEL_DOWN) {
		solers_detail_cam_zoom = MIN(10.0f, solers_detail_cam_zoom * 1.1f);
		_update_solers_detail_camera();
		handled = true;
	} else if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MouseButton::WHEEL_UP) {
		solers_detail_cam_zoom = MAX(0.1f, solers_detail_cam_zoom / 1.1f);
		_update_solers_detail_camera();
		handled = true;
	}

	Ref<InputEventMagnifyGesture> mg = p_input;
	if (mg.is_valid()) {
		const real_t factor = mg->get_factor() == 0.0 ? 1.0 : mg->get_factor();
		solers_detail_cam_zoom = CLAMP(solers_detail_cam_zoom / factor, 0.1f, 10.0f);
		_update_solers_detail_camera();
		handled = true;
	}

	if (handled && solers_detail_viewport_container) {
		solers_detail_viewport_container->accept_event();
	}
}

void EditorAssetLibrary::_sync_solers_detail_fullscreen() {
	if (!solers_detail_viewer_panel || !solers_detail_info_panel || !solers_detail_expand_button) {
		return;
	}
	solers_detail_info_panel->set_visible(!solers_detail_fullscreen);
	const float expanded_height = library_scroll ? MAX(560.0f * EDSCALE, library_scroll->get_size().y - 16.0f * EDSCALE) : 560.0f * EDSCALE;
	solers_detail_viewer_panel->set_custom_minimum_size(Size2(560.0f * EDSCALE, solers_detail_fullscreen ? expanded_height : 560.0f * EDSCALE));
	solers_detail_expand_button->set_text(solers_detail_fullscreen ? TTRC("Details") : TTRC("Expand"));
}

void EditorAssetLibrary::_toggle_solers_detail_inspector() {
	if (!solers_detail_inspector_panel) {
		return;
	}
	solers_detail_inspector_panel->set_visible(!solers_detail_inspector_panel->is_visible());
	if (solers_detail_inspector_panel->is_visible()) {
		_refresh_solers_detail_inspector();
	}
}

void EditorAssetLibrary::_set_solers_detail_view_mode(int p_mode) {
	solers_detail_view_mode = p_mode;
	if (!solers_detail_viewport) {
		return;
	}

	_set_solers_detail_skeleton_visible(p_mode == SOLERS_DETAIL_VIEW_SKELETON);
	switch (p_mode) {
		case SOLERS_DETAIL_VIEW_LIGHTING:
			solers_detail_viewport->set_debug_draw(Viewport::DEBUG_DRAW_LIGHTING);
			break;
		case SOLERS_DETAIL_VIEW_UNSHADED:
			solers_detail_viewport->set_debug_draw(Viewport::DEBUG_DRAW_UNSHADED);
			break;
		case SOLERS_DETAIL_VIEW_WIREFRAME:
			solers_detail_viewport->set_debug_draw(Viewport::DEBUG_DRAW_WIREFRAME);
			break;
		case SOLERS_DETAIL_VIEW_NORMALS:
			solers_detail_viewport->set_debug_draw(Viewport::DEBUG_DRAW_NORMAL_BUFFER);
			break;
		case SOLERS_DETAIL_VIEW_SKELETON:
			solers_detail_viewport->set_debug_draw(Viewport::DEBUG_DRAW_DISABLED);
			break;
		default:
			solers_detail_viewport->set_debug_draw(Viewport::DEBUG_DRAW_DISABLED);
			break;
	}
}

void EditorAssetLibrary::_set_solers_detail_skeleton_visible(bool p_visible) {
	solers_detail_show_skeleton = p_visible && solers_find_skeleton(solers_detail_scene_root);
	_refresh_solers_detail_skeleton_overlay();
}

void EditorAssetLibrary::_refresh_solers_detail_skeleton_overlay() {
	if (solers_detail_skeleton_overlay) {
		if (Node *parent = solers_detail_skeleton_overlay->get_parent()) {
			parent->remove_child(solers_detail_skeleton_overlay);
		}
		memdelete(solers_detail_skeleton_overlay);
		solers_detail_skeleton_overlay = nullptr;
	}
	if (!solers_detail_show_skeleton || !solers_detail_scene_root) {
		return;
	}

	Skeleton3D *skeleton = solers_find_skeleton(solers_detail_scene_root);
	if (!skeleton) {
		return;
	}

	Ref<StandardMaterial3D> line_material;
	line_material.instantiate();
	line_material->set_albedo(Color(0.92f, 0.76f, 0.42f, 0.92f));
	line_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	line_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	line_material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);

	Ref<ImmediateMesh> mesh;
	mesh.instantiate();
	mesh->surface_begin(Mesh::PRIMITIVE_LINES, line_material);
	const Transform3D root_inv = solers_detail_scene_root->get_global_transform().affine_inverse();
	int lines = 0;
	for (int i = 0; i < skeleton->get_bone_count(); i++) {
		const int parent = skeleton->get_bone_parent(i);
		if (parent < 0) {
			continue;
		}
		const Vector3 from = root_inv.xform(skeleton->get_global_transform().xform(skeleton->get_bone_global_pose(parent).origin));
		const Vector3 to = root_inv.xform(skeleton->get_global_transform().xform(skeleton->get_bone_global_pose(i).origin));
		mesh->surface_add_vertex(from);
		mesh->surface_add_vertex(to);
		lines++;
	}
	mesh->surface_end();
	if (lines == 0) {
		return;
	}

	solers_detail_skeleton_overlay = memnew(MeshInstance3D);
	solers_detail_skeleton_overlay->set_name("__SolersSkeletonOverlay");
	solers_detail_skeleton_overlay->set_meta("solers_preview_overlay", true);
	solers_detail_skeleton_overlay->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	solers_detail_skeleton_overlay->set_mesh(mesh);
	solers_detail_scene_root->add_child(solers_detail_skeleton_overlay);
}

void EditorAssetLibrary::_set_solers_detail_animation(int p_index) {
	if (!solers_detail_animation_player || !solers_detail_animation_picker || p_index < 0) {
		return;
	}
	const StringName animation(solers_detail_animation_picker->get_item_text(p_index));
	if (solers_detail_animation_player->has_animation(animation)) {
		solers_detail_animation_player->set_assigned_animation(animation);
		solers_detail_animation_player->seek(0.0, true);
		_refresh_solers_detail_skeleton_overlay();
		_update_solers_detail_animation_time();
	}
}

void EditorAssetLibrary::_play_solers_detail_animation() {
	if (!solers_detail_animation_player || !solers_detail_animation_picker || solers_detail_animation_picker->get_selected() < 0) {
		return;
	}
	const StringName animation(solers_detail_animation_picker->get_item_text(solers_detail_animation_picker->get_selected()));
	Ref<Animation> resource = solers_detail_animation_player->get_animation(animation);
	if (resource.is_valid()) {
		resource->set_loop_mode(solers_detail_animation_loop ? Animation::LOOP_LINEAR : Animation::LOOP_NONE);
	}
	solers_detail_animation_player->play(animation);
	_update_solers_detail_animation_time();
}

void EditorAssetLibrary::_pause_solers_detail_animation() {
	if (solers_detail_animation_player) {
		solers_detail_animation_player->pause();
		_update_solers_detail_animation_time();
	}
}

void EditorAssetLibrary::_stop_solers_detail_animation() {
	if (solers_detail_animation_player) {
		solers_detail_animation_player->stop(true);
		solers_detail_animation_player->seek(0.0, true);
		_refresh_solers_detail_skeleton_overlay();
		_update_solers_detail_animation_time();
	}
}

void EditorAssetLibrary::_toggle_solers_detail_animation_loop(Button *p_toggle) {
	if (!p_toggle) {
		return;
	}
	solers_detail_animation_loop = p_toggle->is_pressed();
	p_toggle->set_text(solers_detail_animation_loop ? TTRC("Loop On") : TTRC("Loop Off"));
}

void EditorAssetLibrary::_update_solers_detail_animation_time() {
	if (!solers_detail_animation_player || !solers_detail_animation_time) {
		return;
	}
	const double pos = solers_detail_animation_player->get_current_animation_position();
	const double len = solers_detail_animation_player->get_current_animation_length();
	solers_detail_animation_time->set_text(vformat("%.1fs / %.1fs", pos, len));
	if (solers_detail_show_skeleton && solers_detail_animation_player->is_playing()) {
		_refresh_solers_detail_skeleton_overlay();
	}
}

void EditorAssetLibrary::_refresh_solers_detail_inspector() {
	if (!solers_detail_inspector_content) {
		return;
	}

	solers_detail_animation_picker = nullptr;
	solers_detail_animation_time = nullptr;
	for (int i = solers_detail_inspector_content->get_child_count() - 1; i >= 0; i--) {
		memdelete(solers_detail_inspector_content->get_child(i));
	}

	Label *title = solers_make_label(TTRC("Viewer"), 14, Color(0.96f, 0.97f, 0.99f));
	solers_detail_inspector_content->add_child(title);

	solers_add_inspector_section(solers_detail_inspector_content, TTRC("Render"));
	OptionButton *render_mode = memnew(OptionButton);
	render_mode->set_theme_type_variation("FlatMenuButton");
	render_mode->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	render_mode->add_item(TTRC("Final"), SOLERS_DETAIL_VIEW_FINAL);
	render_mode->add_item(TTRC("Lighting"), SOLERS_DETAIL_VIEW_LIGHTING);
	render_mode->add_item(TTRC("Unshaded"), SOLERS_DETAIL_VIEW_UNSHADED);
	render_mode->add_item(TTRC("Wireframe"), SOLERS_DETAIL_VIEW_WIREFRAME);
	render_mode->add_item(TTRC("Normals"), SOLERS_DETAIL_VIEW_NORMALS);
	if (solers_find_skeleton(solers_detail_scene_root)) {
		render_mode->add_item(TTRC("Skeleton"), SOLERS_DETAIL_VIEW_SKELETON);
	}
	render_mode->select(solers_detail_view_mode);
	render_mode->connect(SceneStringName(item_selected), callable_mp(this, &EditorAssetLibrary::_set_solers_detail_view_mode));
	solers_detail_inspector_content->add_child(render_mode);

	solers_detail_animation_player = solers_find_animation_player(solers_detail_scene_root);
	if (solers_detail_animation_player) {
		solers_add_inspector_section(solers_detail_inspector_content, TTRC("Animation"));
		solers_detail_animation_picker = memnew(OptionButton);
		solers_detail_animation_picker->set_theme_type_variation("FlatMenuButton");
		solers_detail_animation_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		List<StringName> animations;
		solers_detail_animation_player->get_animation_list(&animations);
		for (const StringName &E : animations) {
			solers_detail_animation_picker->add_item(String(E));
		}
		solers_detail_animation_picker->connect(SceneStringName(item_selected), callable_mp(this, &EditorAssetLibrary::_set_solers_detail_animation));
		solers_detail_inspector_content->add_child(solers_detail_animation_picker);

		HBoxContainer *animation_controls = memnew(HBoxContainer);
		animation_controls->add_theme_constant_override("separation", 6 * EDSCALE);
		Button *play = memnew(Button);
		play->set_text(TTRC("Play"));
		solers_style_action_button(play);
		play->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_play_solers_detail_animation));
		animation_controls->add_child(play);
		Button *pause = memnew(Button);
		pause->set_text(TTRC("Pause"));
		solers_style_action_button(pause);
		pause->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_pause_solers_detail_animation));
		animation_controls->add_child(pause);
		Button *stop = memnew(Button);
		stop->set_text(TTRC("Stop"));
		solers_style_action_button(stop);
		stop->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_stop_solers_detail_animation));
		animation_controls->add_child(stop);
		Button *loop = memnew(Button);
		loop->set_text(TTRC("Loop On"));
		loop->set_toggle_mode(true);
		loop->set_pressed(solers_detail_animation_loop);
		solers_style_action_button(loop);
		loop->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_toggle_solers_detail_animation_loop).bind(loop));
		animation_controls->add_child(loop);
		solers_detail_inspector_content->add_child(animation_controls);

		solers_detail_animation_time = solers_make_label(String(), 12, Color(0.68f, 0.74f, 0.84f), true);
		solers_detail_inspector_content->add_child(solers_detail_animation_time);
		if (solers_detail_animation_picker->get_item_count() > 0) {
			_set_solers_detail_animation(0);
		}
	}

	int meshes = 0;
	int surfaces = 0;
	int triangles = 0;
	int materials = 0;
	int textured_surfaces = 0;
	int texture_slots = 0;
	int uv_surfaces = 0;
	int normal_surfaces = 0;
	if (solers_detail_scene_root) {
		solers_collect_preview_mesh_stats(solers_detail_scene_root, meshes, surfaces, triangles, materials, textured_surfaces, texture_slots, uv_surfaces, normal_surfaces);
	}

	solers_add_inspector_section(solers_detail_inspector_content, TTRC("Material"));
	solers_detail_inspector_content->add_child(solers_make_label(vformat("%s: %s | %s: %s | %s: %s", TTRC("Meshes"), itos(meshes), TTRC("Surfaces"), itos(surfaces), TTRC("Triangles"), itos(triangles)), 12, Color(0.68f, 0.74f, 0.84f), true));
	solers_detail_inspector_content->add_child(solers_make_label(vformat("%s: %s | %s: %s | %s: %s", TTRC("Materials"), itos(materials), TTRC("Textured Surfaces"), itos(textured_surfaces), TTRC("Texture Slots"), itos(texture_slots)), 12, Color(0.68f, 0.74f, 0.84f), true));

	solers_add_inspector_section(solers_detail_inspector_content, TTRC("Geometry"));
	solers_detail_inspector_content->add_child(solers_make_label(vformat("%s: %s / %s | %s: %s / %s", TTRC("Surfaces with UV"), itos(uv_surfaces), itos(surfaces), TTRC("Surfaces with Normals"), itos(normal_surfaces), itos(surfaces)), 12, Color(0.68f, 0.74f, 0.84f), true));
}

void EditorAssetLibrary::_toggle_solers_detail_fullscreen() {
	solers_detail_fullscreen = !solers_detail_fullscreen;
	_sync_solers_detail_fullscreen();
}

void EditorAssetLibrary::_import_solers_detail_asset() {
	if (solers_detail_asset_id.is_empty()) {
		return;
	}

	Dictionary args;
	args["asset_id"] = solers_detail_asset_id;
	const Dictionary result = solers_asset_service->import_to_project(args);
	if ((bool)result.get("ok", false)) {
		solers_detail_import_status->set_text(TTRC("Imported into the project."));
	} else {
		const Dictionary error = result.get("error", Dictionary());
		solers_detail_import_status->set_text(String(error.get("message", TTRC("Import failed."))));
	}
}

void EditorAssetLibrary::_view_solers_basic_animation(const String &p_path) {
	if (p_path.is_empty() || !FileAccess::exists(p_path)) {
		solers_detail_import_status->set_text(TTRC("Animation file is missing."));
		return;
	}
	if (!_load_solers_detail_preview(p_path)) {
		solers_detail_import_status->set_text(TTRC("Godot could not load this animation."));
		return;
	}
	_refresh_solers_detail_inspector();
	solers_detail_import_status->set_text(TTRC("Animation loaded in viewer."));
}

void EditorAssetLibrary::_import_solers_basic_animation(const String &p_path) {
	if (p_path.is_empty() || !FileAccess::exists(p_path)) {
		solers_detail_import_status->set_text(TTRC("Animation file is missing."));
		return;
	}
	const String target_dir = String("res://solers_assets/3d").path_join(solers_detail_asset_id).path_join("animations");
	if (DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(target_dir)) != OK) {
		solers_detail_import_status->set_text(TTRC("Could not create animation import folder."));
		return;
	}
	const String dst = target_dir.path_join(p_path.get_file());
	if (DirAccess::copy_absolute(ProjectSettings::get_singleton()->globalize_path(p_path), ProjectSettings::get_singleton()->globalize_path(dst)) != OK) {
		solers_detail_import_status->set_text(TTRC("Animation import failed."));
		return;
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(dst);
	}
	solers_detail_import_status->set_text(TTRC("Animation imported into the project."));
}

void EditorAssetLibrary::_delete_solers_detail_asset(Button *p_button) {
	if (solers_detail_asset_id.is_empty() || !solers_asset_service || !p_button) {
		return;
	}
	if (!solers_detail_delete_confirm) {
		solers_detail_delete_confirm = true;
		p_button->set_text(TTRC("Confirm Delete"));
		if (solers_detail_import_status) {
			solers_detail_import_status->set_text(TTRC("Click Confirm Delete to permanently remove this Library asset."));
		}
		return;
	}
	Dictionary args;
	args["asset_id"] = solers_detail_asset_id;
	const Dictionary result = solers_asset_service->delete_asset(args);
	if ((bool)result.get("ok", false)) {
		_show_solers_library();
		return;
	}
	solers_detail_delete_confirm = false;
	p_button->set_text(TTRC("Delete"));
	const Dictionary error = result.get("error", Dictionary());
	if (solers_detail_import_status) {
		solers_detail_import_status->set_text(String(error.get("message", TTRC("Delete failed."))));
	}
}

void EditorAssetLibrary::_set_solers_bool_option(Button *p_toggle, Button *p_action_button) {
	if (!p_toggle) {
		return;
	}
	const bool confirmed = p_toggle->is_pressed();
	p_toggle->set_text(confirmed ? TTRC("Confirmed") : TTRC("Confirm"));
	if (p_action_button) {
		p_action_button->set_disabled(!confirmed);
	}
}

void EditorAssetLibrary::_run_solers_detail_operation(Button *p_button) {
	if (solers_detail_asset_id.is_empty()) {
		return;
	}
	const String operation_id = p_button ? String(p_button->get_meta("solers_operation_id", String())) : String();
	if (operation_id.is_empty()) {
		return;
	}
	Dictionary options;
	if (Control *parent = Object::cast_to<Control>(p_button->get_parent())) {
		solers_collect_operation_options(parent, options);
	}
	Dictionary args;
	args["asset_id"] = solers_detail_asset_id;
	args["operation_id"] = operation_id;
	args["options"] = options;
	const Dictionary result = solers_asset_service->run_operation(args);
	if ((bool)result.get("ok", false)) {
		solers_mark_asset_action_creating(p_button);
		solers_detail_import_status->set_text(String());
		solers_library_signature = String();
	} else {
		const Dictionary error = result.get("error", Dictionary());
		solers_detail_import_status->set_text(String(error.get("message", TTRC("Operation failed."))));
	}
}

void EditorAssetLibrary::_source_back_pressed() {
	if (source_mode == ASSET_SOURCE_SOLERS && solers_detail_visible) {
		source_title->set_text(TTRC("Solers Library"));
		solers_filter_row->show();
		if (solers_detail_version_row) {
			solers_detail_version_row->hide();
		}
		_show_solers_library();
		return;
	}
	_set_source_mode(ASSET_SOURCE_PICKER);
}

void EditorAssetLibrary::_set_source_mode(int p_mode) {
	source_mode = p_mode;
	const bool picker = source_mode == ASSET_SOURCE_PICKER && !templates_only;
	const bool godot = source_mode == ASSET_SOURCE_GODOT || templates_only;
	const bool solers = source_mode == ASSET_SOURCE_SOLERS;

	if (source_choice) {
		source_choice->set_visible(picker);
	}
	source_header->set_visible(!picker && !templates_only);
	if (source_title) {
		source_title->set_text(solers ? TTRC("Solers Library") : TTRC("Godot Assets"));
	}
	solers_filter_row->set_visible(solers);
	if (solers_detail_version_row) {
		solers_detail_version_row->hide();
	}
	search_hb->set_visible(godot);
	search_hb2->set_visible(godot);
	library_mc->set_visible(godot || solers);
	downloads_scroll->set_visible(godot && downloads_hb->get_child_count() > 0);
	error_hb->hide();
	if (contents) {
		contents->set_visible(godot || solers);
	}
	if (!solers && solers_detail) {
		solers_detail_visible = false;
		solers_detail->hide();
		_clear_solers_detail_preview();
	}

	if (godot && initial_loading) {
		_repository_changed(0);
	} else if (solers) {
		_show_solers_library();
	}
}

void EditorAssetLibrary::_set_solers_kind_filter(const String &p_kind) {
	solers_kind_filter = p_kind;
	_show_solers_library();
}

void EditorAssetLibrary::_set_solers_status_filter(const String &p_status) {
	solers_status_filter = p_status;
	_show_solers_library();
}

void EditorAssetLibrary::_set_solers_kind_filter_id(int p_id) {
	_set_solers_kind_filter(solers_kind_filter_value_from_id(p_id));
}

void EditorAssetLibrary::_set_solers_status_filter_id(int p_id) {
	_set_solers_status_filter(solers_status_filter_value_from_id(p_id));
}

void EditorAssetLibrary::_sync_solers_filter_menus() {
	if (solers_kind_filter_menu) {
		solers_kind_filter_menu->set_text(vformat("%s: %s", TTRC("Asset Type"), solers_kind_filter_label(solers_kind_filter)));
		PopupMenu *popup = solers_kind_filter_menu->get_popup();
		const int selected_id = solers_kind_filter_id_from_value(solers_kind_filter);
		for (int i = 0; i < popup->get_item_count(); i++) {
			popup->set_item_checked(i, popup->get_item_id(i) == selected_id);
		}
	}
	if (solers_status_filter_menu) {
		solers_status_filter_menu->set_text(vformat("%s: %s", TTRC("Status"), solers_status_filter_label(solers_status_filter)));
		PopupMenu *popup = solers_status_filter_menu->get_popup();
		const int selected_id = solers_status_filter_id_from_value(solers_status_filter);
		for (int i = 0; i < popup->get_item_count(); i++) {
			popup->set_item_checked(i, popup->get_item_id(i) == selected_id);
		}
	}
}

void EditorAssetLibrary::disable_community_support() {
	support->get_popup()->set_item_checked(SUPPORT_COMMUNITY, false);
}

void EditorAssetLibrary::_bind_methods() {
	ADD_SIGNAL(MethodInfo("install_asset", PropertyInfo(Variant::STRING, "zip_path"), PropertyInfo(Variant::STRING, "name")));
}

EditorAssetLibrary::EditorAssetLibrary(bool p_templates_only) {
	requesting = REQUESTING_NONE;
	templates_only = p_templates_only;
	loading_blocked = ((int)EDITOR_GET("network/connection/network_mode") == EditorSettings::NETWORK_OFFLINE);

	VBoxContainer *library_main = memnew(VBoxContainer);
	add_child(library_main);

	if (!p_templates_only) {
		source_choice = memnew(VBoxContainer);
		source_choice->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		source_choice->set_alignment(BoxContainer::ALIGNMENT_CENTER);
		source_choice->add_theme_constant_override("separation", 18 * EDSCALE);
		library_main->add_child(source_choice);

		Label *source_title_label = memnew(Label(TTRC("Asset Library")));
		source_title_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		source_title_label->add_theme_font_size_override(SceneStringName(font_size), 24 * EDSCALE);
		source_title_label->add_theme_color_override(SceneStringName(font_color), Color(0.91f, 0.94f, 0.98f));
		source_choice->add_child(source_title_label);

		HFlowContainer *source_cards = memnew(HFlowContainer);
		source_cards->set_h_size_flags(Control::SIZE_SHRINK_CENTER);
		source_cards->set_alignment(FlowContainer::ALIGNMENT_CENTER);
		source_cards->add_theme_constant_override("separation", 18 * EDSCALE);
		source_choice->add_child(source_cards);

		Button *godot_assets = memnew(Button);
		godot_assets->set_text(TTRC("Godot Assets"));
		godot_assets->set_theme_type_variation("SolersFlatPillButton");
		godot_assets->set_custom_minimum_size(Size2(190, 42) * EDSCALE);
		godot_assets->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_set_source_mode).bind(ASSET_SOURCE_GODOT));
		source_cards->add_child(godot_assets);

		Button *solers_library = memnew(Button);
		solers_library->set_text(TTRC("Solers Library"));
		solers_library->set_theme_type_variation("SolersFlatPillButton");
		solers_library->set_custom_minimum_size(Size2(190, 42) * EDSCALE);
		solers_library->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_set_source_mode).bind(ASSET_SOURCE_SOLERS));
		source_cards->add_child(solers_library);
	}

	source_header = memnew(VBoxContainer);
	source_header->add_theme_constant_override("separation", 8 * EDSCALE);
	source_header->hide();
	library_main->add_child(source_header);

	HBoxContainer *source_title_row = memnew(HBoxContainer);
	source_title_row->add_theme_constant_override("separation", 8 * EDSCALE);
	source_header->add_child(source_title_row);

	source_back = memnew(Button);
	source_back->set_text(TTRC("Back"));
	source_back->set_theme_type_variation("SolersFlatPillButton");
	source_back->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_source_back_pressed));
	source_title_row->add_child(source_back);

	source_title = memnew(Label);
	source_title->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	source_title->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	source_title->add_theme_font_size_override(SceneStringName(font_size), 16 * EDSCALE);
	source_title->add_theme_color_override(SceneStringName(font_color), Color(0.86f, 0.90f, 0.96f));
	source_title_row->add_child(source_title);

	solers_filter_row = memnew(HFlowContainer);
	solers_filter_row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	solers_filter_row->set_alignment(FlowContainer::ALIGNMENT_BEGIN);
	solers_filter_row->add_theme_constant_override("h_separation", 10 * EDSCALE);
	solers_filter_row->add_theme_constant_override("v_separation", 8 * EDSCALE);
	solers_filter_row->hide();
	source_header->add_child(solers_filter_row);

	solers_kind_filter_menu = memnew(MenuButton);
	solers_kind_filter_menu->set_theme_type_variation("FlatMenuButton");
	solers_kind_filter_menu->get_popup()->add_radio_check_item(TTRC("All Assets"), SOLERS_KIND_FILTER_ALL);
	solers_kind_filter_menu->get_popup()->add_radio_check_item(TTRC("3D Models"), SOLERS_KIND_FILTER_3D);
	solers_kind_filter_menu->get_popup()->add_radio_check_item(TTRC("Sound Effects"), SOLERS_KIND_FILTER_SFX);
	solers_kind_filter_menu->get_popup()->add_radio_check_item(TTRC("Music"), SOLERS_KIND_FILTER_MUSIC);
	solers_kind_filter_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &EditorAssetLibrary::_set_solers_kind_filter_id));
	solers_filter_row->add_child(solers_kind_filter_menu);

	solers_status_filter_menu = memnew(MenuButton);
	solers_status_filter_menu->set_theme_type_variation("FlatMenuButton");
	solers_status_filter_menu->get_popup()->add_radio_check_item(TTRC("All Status"), SOLERS_STATUS_FILTER_ALL);
	solers_status_filter_menu->get_popup()->add_radio_check_item(TTRC("Ready"), SOLERS_STATUS_FILTER_READY);
	solers_status_filter_menu->get_popup()->add_radio_check_item(TTRC("Draft"), SOLERS_STATUS_FILTER_DRAFT);
	solers_status_filter_menu->get_popup()->add_radio_check_item(TTRC("Generating"), SOLERS_STATUS_FILTER_GENERATING);
	solers_status_filter_menu->get_popup()->add_radio_check_item(TTRC("Failed"), SOLERS_STATUS_FILTER_FAILED);
	solers_status_filter_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &EditorAssetLibrary::_set_solers_status_filter_id));
	solers_filter_row->add_child(solers_status_filter_menu);
	_sync_solers_filter_menus();

	search_hb = memnew(HBoxContainer);

	library_main->add_child(search_hb);
	library_main->add_theme_constant_override("separation", 10 * EDSCALE);

	filter = memnew(LineEdit);
	if (templates_only) {
		filter->set_placeholder(TTRC("Search Templates, Projects, and Demos"));
	} else {
		filter->set_placeholder(TTRC("Search Assets (Excluding Templates, Projects, and Demos)"));
	}
	filter->set_clear_button_enabled(true);
	search_hb->add_child(filter);
	filter->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	filter->connect(SceneStringName(text_changed), callable_mp(this, &EditorAssetLibrary::_search_text_changed));

	// Perform a search automatically if the user hasn't entered any text for a certain duration.
	// This way, the user doesn't need to press Enter to initiate their search.
	filter_debounce_timer = memnew(Timer);
	filter_debounce_timer->set_one_shot(true);
	filter_debounce_timer->set_wait_time(0.25);
	filter_debounce_timer->connect("timeout", callable_mp(this, &EditorAssetLibrary::_filter_debounce_timer_timeout));
	search_hb->add_child(filter_debounce_timer);

	if (!p_templates_only) {
		search_hb->add_child(memnew(VSeparator));
	}

	Button *open_asset = memnew(Button);
	open_asset->set_text(TTRC("Import..."));
	search_hb->add_child(open_asset);
	open_asset->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_asset_open));

	Button *plugins = memnew(Button);
	plugins->set_text(TTRC("Plugins..."));
	search_hb->add_child(plugins);
	plugins->connect(SceneStringName(pressed), callable_mp(this, &EditorAssetLibrary::_manage_plugins));

	if (p_templates_only) {
		open_asset->hide();
		plugins->hide();
	}

	search_hb2 = memnew(HBoxContainer);
	library_main->add_child(search_hb2);

	search_hb2->add_child(memnew(Label(TTRC("Sort:"))));
	sort = memnew(OptionButton);
	for (int i = 0; i < SORT_MAX; i++) {
		sort->add_item(sort_text[i]);
	}

	search_hb2->add_child(sort);

	sort->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	sort->set_clip_text(true);
	sort->connect(SceneStringName(item_selected), callable_mp(this, &EditorAssetLibrary::_rerun_search));

	search_hb2->add_child(memnew(VSeparator));

	search_hb2->add_child(memnew(Label(TTRC("Category:"))));
	categories = memnew(OptionButton);
	categories->add_item(TTRC("All"));
	search_hb2->add_child(categories);
	categories->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	categories->set_clip_text(true);
	categories->connect(SceneStringName(item_selected), callable_mp(this, &EditorAssetLibrary::_rerun_search));

	search_hb2->add_child(memnew(VSeparator));

	search_hb2->add_child(memnew(Label(TTRC("Site:"))));
	repository = memnew(OptionButton);

	_update_repository_options();

	repository->connect(SceneStringName(item_selected), callable_mp(this, &EditorAssetLibrary::_repository_changed));

	search_hb2->add_child(repository);
	repository->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	repository->set_clip_text(true);

	search_hb2->add_child(memnew(VSeparator));

	support = memnew(MenuButton);
	search_hb2->add_child(support);
	support->set_text(TTRC("Support"));
	support->get_popup()->set_hide_on_checkable_item_selection(false);
	support->get_popup()->add_check_item(support_text[SUPPORT_FEATURED], SUPPORT_FEATURED);
	support->get_popup()->add_check_item(support_text[SUPPORT_COMMUNITY], SUPPORT_COMMUNITY);
	support->get_popup()->add_check_item(support_text[SUPPORT_TESTING], SUPPORT_TESTING);
	support->get_popup()->set_item_checked(SUPPORT_FEATURED, true);
	support->get_popup()->set_item_checked(SUPPORT_COMMUNITY, true);
	support->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &EditorAssetLibrary::_support_toggled));

	/////////

	library_mc = memnew(MarginContainer);
	library_mc->set_theme_type_variation(Engine::get_singleton()->is_project_manager_hint() ? "NoBorderAssetLibProjectManager" : "NoBorderAssetLib");
	library_mc->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	library_mc->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	library_main->add_child(library_mc);

	library_scroll = memnew(ScrollContainer);
	library_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	library_scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	library_scroll->set_scroll_hint_mode(ScrollContainer::SCROLL_HINT_MODE_TOP_AND_LEFT);
	library_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	library_mc->add_child(library_scroll);

	Ref<StyleBoxEmpty> border2;
	border2.instantiate();
	border2->set_content_margin_individual(15 * EDSCALE, 15 * EDSCALE, 15 * EDSCALE, 15 * EDSCALE);

	PanelContainer *library_vb_border = memnew(PanelContainer);
	library_scroll->add_child(library_vb_border);
	library_vb_border->add_theme_style_override(SceneStringName(panel), border2);
	library_vb_border->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	library_vb = memnew(VBoxContainer);
	library_vb->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	library_vb_border->add_child(library_vb);

	library_message_box = memnew(VBoxContainer);
	library_message_box->hide();
	library_vb->add_child(library_message_box);

	library_message = memnew(Label);
	library_message->set_focus_mode(FOCUS_ACCESSIBILITY);
	library_message->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	library_message_box->add_child(library_message);

	library_message_button = memnew(Button);
	library_message_button->set_h_size_flags(SIZE_SHRINK_CENTER);
	library_message_button->set_theme_type_variation("PanelBackgroundButton");
	library_message_box->add_child(library_message_button);

	asset_top_page = memnew(HBoxContainer);
	library_vb->add_child(asset_top_page);

	contents = memnew(HBoxContainer);
	contents->add_theme_constant_override("separation", 18 * EDSCALE);
	contents->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	library_vb->add_child(contents);

	asset_items = memnew(GridContainer);
	asset_items->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	_update_asset_items_columns();
	asset_items->add_theme_constant_override("h_separation", 10 * EDSCALE);
	asset_items->add_theme_constant_override("v_separation", 10 * EDSCALE);

	contents->add_child(asset_items);

	asset_bottom_page = memnew(HBoxContainer);
	library_vb->add_child(asset_bottom_page);

	request = memnew(HTTPRequest);
	add_child(request);
	setup_http_request(request);
	request->connect("request_completed", callable_mp(this, &EditorAssetLibrary::_http_request_completed));

	last_queue_id = 0;

	library_vb->add_theme_constant_override("separation", 20 * EDSCALE);

	error_hb = memnew(HBoxContainer);
	library_main->add_child(error_hb);
	error_label = memnew(Label);
	error_label->set_focus_mode(FOCUS_ACCESSIBILITY);
	error_hb->add_child(error_label);
	error_tr = memnew(TextureRect);
	error_tr->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	error_hb->add_child(error_tr);

	description = nullptr;

	set_process(true);
	set_process_shortcut_input(true); // Global shortcuts since there is no main element to be focused.

	downloads_scroll = memnew(ScrollContainer);
	downloads_scroll->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	downloads_scroll->set_theme_type_variation("ScrollContainerSecondary");
	library_main->add_child(downloads_scroll);
	downloads_hb = memnew(HBoxContainer);
	downloads_scroll->add_child(downloads_hb);

	asset_open = memnew(EditorFileDialog);

	asset_open->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	asset_open->add_filter("*.zip", TTRC("Assets ZIP File"));
	asset_open->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	add_child(asset_open);
	asset_open->connect("file_selected", callable_mp(this, &EditorAssetLibrary::_asset_file_selected));

	asset_installer = nullptr;
	solers_asset_service = memnew(SolersAssetService);

	if (!p_templates_only) {
		_set_source_mode(ASSET_SOURCE_PICKER);
	}
}

EditorAssetLibrary::~EditorAssetLibrary() {
	if (solers_asset_service) {
		memdelete(solers_asset_service);
	}
}

///////

bool AssetLibraryEditorPlugin::is_available() {
#ifdef WEB_ENABLED
	// Asset Library can't work on Web editor for now as most assets are sourced
	// directly from GitHub which does not set CORS.
	return false;
#else
	return StreamPeerTLS::is_available() && !Engine::get_singleton()->is_recovery_mode_hint();
#endif
}

void AssetLibraryEditorPlugin::make_visible(bool p_visible) {
	if (p_visible) {
		addon_library->show();
	} else {
		addon_library->hide();
	}
}

AssetLibraryEditorPlugin::AssetLibraryEditorPlugin() {
	addon_library = memnew(EditorAssetLibrary);
	addon_library->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	EditorNode::get_singleton()->get_editor_main_screen()->get_control()->add_child(addon_library);
	addon_library->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	addon_library->hide();
}
