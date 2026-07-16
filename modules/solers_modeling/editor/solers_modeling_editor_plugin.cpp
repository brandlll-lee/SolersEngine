/**************************************************************************/
/*  solers_modeling_editor_plugin.cpp                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_modeling_editor_plugin.h"

#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/math/geometry_2d.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "modules/solers_modeling/core/solers_model_operation.h"
#include "modules/solers_modeling/core/solers_model_source.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/text_edit.h"
#include "scene/resources/material.h"

static String _model_source_path(MeshInstance3D *p_instance) {
	if (!p_instance || p_instance->get_mesh().is_null()) {
		return String();
	}
	const String path = p_instance->get_mesh()->get_path();
	return path.get_extension().to_lower() == "smodel" ? path : String();
}

static Vector<int64_t> _domain_ids(const SolersEditableMesh &p_mesh, SolersModelingEditorPlugin::SelectionDomain p_domain) {
	return p_domain == SolersModelingEditorPlugin::DOMAIN_VERTEX ? p_mesh.get_vertex_ids() :
			(p_domain == SolersModelingEditorPlugin::DOMAIN_EDGE ? p_mesh.get_edge_ids() : p_mesh.get_face_ids());
}

static Vector<int64_t> _element_vertices(const SolersEditableMesh &p_mesh, SolersModelingEditorPlugin::SelectionDomain p_domain, int64_t p_id) {
	Vector<int64_t> vertices;
	if (p_domain == SolersModelingEditorPlugin::DOMAIN_VERTEX) {
		if (p_mesh.get_vertex(p_id)) {
			vertices.push_back(p_id);
		}
	} else if (p_domain == SolersModelingEditorPlugin::DOMAIN_EDGE) {
		const SolersEditableMesh::Edge *edge = p_mesh.get_edge(p_id);
		if (edge) {
			vertices.push_back(edge->vertex_a);
			vertices.push_back(edge->vertex_b);
		}
	} else {
		vertices = p_mesh.get_face_vertices(p_id);
	}
	return vertices;
}

static Vector3 _element_center(const SolersEditableMesh &p_mesh, SolersModelingEditorPlugin::SelectionDomain p_domain, int64_t p_id) {
	const Vector<int64_t> vertices = _element_vertices(p_mesh, p_domain, p_id);
	Vector3 center;
	for (int64_t vertex_id : vertices) {
		center += p_mesh.get_vertex(vertex_id)->position;
	}
	return vertices.is_empty() ? center : center / vertices.size();
}

SolersModelingGizmoPlugin::SolersModelingGizmoPlugin(SolersModelingEditorPlugin *p_editor) {
	editor = p_editor;
	create_material("topology", Color(0.24, 0.72, 1.0), false, true);
	create_material("elements", Color(0.9, 0.92, 0.96), false, true);
	create_material("selected", Color(1.0, 0.55, 0.12), false, true);
}

String SolersModelingGizmoPlugin::_source_path(MeshInstance3D *p_instance) const {
	if (const Preview *preview = previews.getptr(p_instance->get_instance_id())) {
		return preview->source_path;
	}
	return _model_source_path(p_instance);
}

Vector<int64_t> SolersModelingGizmoPlugin::_element_vertices(const SolersEditableMesh &p_mesh, int p_handle) const {
	const Vector<int64_t> ids = _domain_ids(p_mesh, editor->get_selection_domain());
	return p_handle >= 0 && p_handle < ids.size() ? ::_element_vertices(p_mesh, editor->get_selection_domain(), ids[p_handle]) : Vector<int64_t>();
}

Vector3 SolersModelingGizmoPlugin::_element_center(const SolersEditableMesh &p_mesh, int p_handle) const {
	const Vector<int64_t> ids = _domain_ids(p_mesh, editor->get_selection_domain());
	return p_handle >= 0 && p_handle < ids.size() ? ::_element_center(p_mesh, editor->get_selection_domain(), ids[p_handle]) : Vector3();
}

bool SolersModelingGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_spatial);
	return editor && editor->is_edit_mode() && instance && !_source_path(instance).is_empty();
}

void SolersModelingGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_gizmo->get_node_3d());
	SolersEditableMesh mesh;
	if (!instance || SolersModelSource::load(_source_path(instance), mesh) != OK) {
		return;
	}
	Vector<Vector3> lines;
	for (int64_t edge_id : mesh.get_edge_ids()) {
		const SolersEditableMesh::Edge *edge = mesh.get_edge(edge_id);
		lines.push_back(mesh.get_vertex(edge->vertex_a)->position);
		lines.push_back(mesh.get_vertex(edge->vertex_b)->position);
	}
	if (!lines.is_empty()) {
		p_gizmo->add_lines(lines, get_material("topology", Ref<EditorNode3DGizmo>(p_gizmo)));
		p_gizmo->add_collision_segments(lines);
	}
	Vector<Vector3> elements;
	Vector<Vector3> selected;
	const Vector<int64_t> ids = _domain_ids(mesh, editor->get_selection_domain());
	for (int i = 0; i < ids.size(); i++) {
		(p_gizmo->is_subgizmo_selected(i) ? selected : elements).push_back(::_element_center(mesh, editor->get_selection_domain(), ids[i]));
	}
	if (!elements.is_empty()) {
		p_gizmo->add_vertices(elements, get_material("elements", Ref<EditorNode3DGizmo>(p_gizmo)), Mesh::PRIMITIVE_POINTS);
	}
	if (!selected.is_empty()) {
		p_gizmo->add_vertices(selected, get_material("selected", Ref<EditorNode3DGizmo>(p_gizmo)), Mesh::PRIMITIVE_POINTS);
	}
}

int SolersModelingGizmoPlugin::subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const {
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_gizmo->get_node_3d());
	SolersEditableMesh mesh;
	if (!instance || SolersModelSource::load(_source_path(instance), mesh) != OK) {
		return -1;
	}
	const Transform3D global = instance->get_global_transform();
	const Vector<int64_t> ids = _domain_ids(mesh, editor->get_selection_domain());
	double closest = 14.0;
	int result = -1;
	for (int i = 0; i < ids.size(); i++) {
		double distance = 0.0;
		if (editor->get_selection_domain() == SolersModelingEditorPlugin::DOMAIN_EDGE) {
			const SolersEditableMesh::Edge *edge = mesh.get_edge(ids[i]);
			const Vector3 a = global.xform(mesh.get_vertex(edge->vertex_a)->position);
			const Vector3 b = global.xform(mesh.get_vertex(edge->vertex_b)->position);
			if (p_camera->is_position_behind(a) && p_camera->is_position_behind(b)) {
				continue;
			}
			distance = Geometry2D::get_closest_point_to_segment(p_point, p_camera->unproject_position(a), p_camera->unproject_position(b)).distance_to(p_point);
		} else {
			const Vector3 center = global.xform(::_element_center(mesh, editor->get_selection_domain(), ids[i]));
			if (p_camera->is_position_behind(center)) {
				continue;
			}
			distance = p_camera->unproject_position(center).distance_to(p_point);
		}
		if (distance < closest) {
			closest = distance;
			result = i;
		}
	}
	return result;
}

Vector<int> SolersModelingGizmoPlugin::subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const {
	Vector<int> result;
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_gizmo->get_node_3d());
	SolersEditableMesh mesh;
	if (!instance || SolersModelSource::load(_source_path(instance), mesh) != OK) {
		return result;
	}
	const Vector<int64_t> ids = _domain_ids(mesh, editor->get_selection_domain());
	for (int i = 0; i < ids.size(); i++) {
		const Vector3 center = instance->get_global_transform().xform(::_element_center(mesh, editor->get_selection_domain(), ids[i]));
		bool contained = true;
		for (const Plane &plane : p_frustum) {
			contained = contained && plane.distance_to(center) <= 0;
		}
		if (contained) {
			result.push_back(i);
		}
	}
	return result;
}

Transform3D SolersModelingGizmoPlugin::get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const {
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_gizmo->get_node_3d());
	SolersEditableMesh mesh;
	if (!instance || SolersModelSource::load(_source_path(instance), mesh) != OK) {
		return Transform3D();
	}
	return Transform3D(Basis(), _element_center(mesh, p_id));
}

void SolersModelingGizmoPlugin::set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) {
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(instance);
	Preview *preview = previews.getptr(instance->get_instance_id());
	if (!preview) {
		Preview state;
		state.source_path = _model_source_path(instance);
		ERR_FAIL_COND(state.source_path.is_empty() || SolersModelSource::load(state.source_path, state.source) != OK);
		state.revision = state.source.get_revision();
		state.original_mesh = instance->get_mesh();
		previews.insert(instance->get_instance_id(), state);
		preview = previews.getptr(instance->get_instance_id());
	}
	preview->handles = p_gizmo->get_subgizmo_selection();
	if (!preview->handles.has(p_id)) {
		preview->handles.push_back(p_id);
	}
	preview->delta = p_transform * Transform3D(Basis(), _element_center(preview->source, p_id)).affine_inverse();
	HashSet<int64_t> vertex_set;
	for (int handle : preview->handles) {
		for (int64_t vertex_id : _element_vertices(preview->source, handle)) {
			vertex_set.insert(vertex_id);
		}
	}
	Vector<int64_t> vertices;
	for (int64_t vertex_id : vertex_set) {
		vertices.push_back(vertex_id);
	}
	SolersEditableMesh working = preview->source;
	if (!working.transform_vertices(vertices, preview->delta)) {
		return;
	}
	Ref<ArrayMesh> mesh = working.compile();
	if (mesh.is_valid()) {
		instance->set_mesh(mesh);
	}
}

void SolersModelingGizmoPlugin::commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel) {
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(instance);
	Preview *preview = previews.getptr(instance->get_instance_id());
	if (!preview) {
		return;
	}
	const String source_path = preview->source_path;
	if (!p_cancel) {
		HashSet<int64_t> vertex_set;
		for (int handle : preview->handles) {
			for (int64_t vertex_id : _element_vertices(preview->source, handle)) {
				vertex_set.insert(vertex_id);
			}
		}
		Array vertex_ids;
		for (int64_t vertex_id : vertex_set) {
			vertex_ids.push_back(vertex_id);
		}
		Dictionary parameters;
		parameters["vertex_ids"] = vertex_ids;
		parameters["translation"] = preview->delta.origin;
		parameters["rotation_degrees"] = preview->delta.basis.get_rotation_quaternion().get_euler() * (180.0 / Math::PI);
		parameters["scale"] = preview->delta.basis.get_scale();
		const Dictionary result = SolersModelingService::get_singleton()->apply(source_path, SNAME("transform"), parameters, preview->revision);
		if (!(bool)result.get("ok", false)) {
			EditorNode::get_singleton()->show_warning(JSON::stringify(result), TTR("Modeling operation failed"));
		}
	}
	instance->set_mesh(preview->original_mesh);
	previews.erase(instance->get_instance_id());
	editor->finish_preview(instance, source_path);
}

SolersModelingEditorPlugin::SolersModelingEditorPlugin() {
	modeling_gizmo.instantiate(this);
	add_node_3d_gizmo_plugin(modeling_gizmo);

	toolbar = memnew(HBoxContainer);
	mode = memnew(OptionButton);
	mode->add_item(TTR("Object"), 0);
	mode->add_item(TTR("Edit"), 1);
	mode->set_tooltip_text(TTR("Switch between object and editable topology modes."));
	mode->connect(SceneStringName(item_selected), callable_mp(this, &SolersModelingEditorPlugin::_mode_changed));
	toolbar->add_child(mode);
	selection_domain = memnew(OptionButton);
	selection_domain->add_item(TTR("Vertex"), DOMAIN_VERTEX);
	selection_domain->add_item(TTR("Edge"), DOMAIN_EDGE);
	selection_domain->add_item(TTR("Face"), DOMAIN_FACE);
	selection_domain->set_disabled(true);
	selection_domain->connect(SceneStringName(item_selected), callable_mp(this, &SolersModelingEditorPlugin::_selection_domain_changed));
	toolbar->add_child(selection_domain);
	operation_filter = memnew(LineEdit);
	operation_filter->set_placeholder(TTR("Find operation"));
	operation_filter->set_custom_minimum_size(Size2(120 * EDSCALE, 0));
	operation_filter->connect(SceneStringName(text_changed), callable_mp(this, &SolersModelingEditorPlugin::_refresh_operations));
	toolbar->add_child(operation_filter);
	operation_menu = memnew(MenuButton);
	operation_menu->set_text(TTR("Operations"));
	operation_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &SolersModelingEditorPlugin::_operation_chosen));
	toolbar->add_child(operation_menu);
	uv_menu = memnew(MenuButton);
	uv_menu->set_text(TTR("UV"));
	uv_menu->get_popup()->add_item(TTR("Unwrap"), 0);
	uv_menu->get_popup()->add_item(TTR("Pack"), 1);
	uv_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &SolersModelingEditorPlugin::_quick_uv));
	toolbar->add_child(uv_menu);
	_refresh_operations();
	Node3DEditor::get_singleton()->add_control_to_menu_panel(toolbar);
	toolbar->hide();

	modifier_panel = memnew(VBoxContainer);
	modifier_panel->set_custom_minimum_size(Size2(220 * EDSCALE, 0));
	Label *title = memnew(Label);
	title->set_text(TTR("Modifiers"));
	modifier_panel->add_child(title);
	modifier_list = memnew(ItemList);
	modifier_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	modifier_panel->add_child(modifier_list);
	HBoxContainer *modifier_actions = memnew(HBoxContainer);
	add_modifier = memnew(MenuButton);
	add_modifier->set_text(TTR("Add"));
	const char *modifier_names[] = { "Mirror", "Array", "Solidify", "Bevel", "Boolean" };
	for (int i = 0; i < 5; i++) {
		add_modifier->get_popup()->add_item(TTR(modifier_names[i]), i);
	}
	add_modifier->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &SolersModelingEditorPlugin::_add_modifier));
	modifier_actions->add_child(add_modifier);
	Button *remove = memnew(Button);
	remove->set_text(TTR("Remove"));
	remove->connect(SceneStringName(pressed), callable_mp(this, &SolersModelingEditorPlugin::_remove_modifier));
	modifier_actions->add_child(remove);
	Button *apply = memnew(Button);
	apply->set_text(TTR("Apply"));
	apply->connect(SceneStringName(pressed), callable_mp(this, &SolersModelingEditorPlugin::_apply_modifiers));
	modifier_actions->add_child(apply);
	modifier_panel->add_child(modifier_actions);
	Node3DEditor::get_singleton()->add_control_to_right_panel(modifier_panel);
	modifier_panel->hide();

	operation_dialog = memnew(ConfirmationDialog);
	operation_dialog->set_title(TTR("Modeling Operation"));
	operation_dialog->set_ok_button_text(TTR("Apply"));
	operation_dialog->connect(SceneStringName(confirmed), callable_mp(this, &SolersModelingEditorPlugin::_confirm_operation));
	VBoxContainer *dialog_content = memnew(VBoxContainer);
	operation_dialog->add_child(dialog_content);
	operation_description = memnew(Label);
	operation_description->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	dialog_content->add_child(operation_description);
	operation_parameters = memnew(TextEdit);
	operation_parameters->set_custom_minimum_size(Size2(520 * EDSCALE, 180 * EDSCALE));
	operation_parameters->set_placeholder(TTR("JSON parameters"));
	dialog_content->add_child(operation_parameters);
	add_child(operation_dialog);
}

SolersModelingEditorPlugin::~SolersModelingEditorPlugin() {
	remove_node_3d_gizmo_plugin(modeling_gizmo);
	if (Node3DEditor::get_singleton()) {
		Node3DEditor::get_singleton()->remove_control_from_menu_panel(toolbar);
		Node3DEditor::get_singleton()->remove_control_from_right_panel(modifier_panel);
	}
	memdelete(toolbar);
	memdelete(modifier_panel);
}

const Ref<Texture2D> SolersModelingEditorPlugin::get_plugin_icon() const {
	return EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("MeshInstance3D"), EditorStringName(EditorIcons));
}

void SolersModelingEditorPlugin::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		_refresh_modifiers();
	}
}

void SolersModelingEditorPlugin::make_visible(bool p_visible) {
	active = p_visible;
	Node3DEditor *editor = Node3DEditor::get_singleton();
	toolbar->set_visible(p_visible);
	modifier_panel->set_visible(p_visible);
	if (p_visible) {
		editor->show();
		editor->set_process(true);
		editor->set_physics_process(true);
		editor->refresh_dirty_gizmos();
		_refresh_modifiers();
	} else {
		editor->hide();
		editor->set_process(false);
		editor->set_physics_process(false);
	}
	editor->update_all_gizmos();
}

bool SolersModelingEditorPlugin::handles(Object *p_object) const {
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_object);
	return instance && !_model_source_path(instance).is_empty();
}

void SolersModelingEditorPlugin::edit(Object *p_object) {
	_refresh_modifiers();
}

MeshInstance3D *SolersModelingEditorPlugin::_selected_model() const {
	return Object::cast_to<MeshInstance3D>(Node3DEditor::get_singleton()->get_single_selected_node());
}

String SolersModelingEditorPlugin::_selected_source_path() const {
	return _model_source_path(_selected_model());
}

Vector<int> SolersModelingEditorPlugin::_selected_handles() const {
	Vector<int> result;
	MeshInstance3D *instance = _selected_model();
	if (!instance) {
		return result;
	}
	for (const Ref<Node3DGizmo> &base_gizmo : instance->get_gizmos()) {
		Ref<EditorNode3DGizmo> gizmo = base_gizmo;
		if (gizmo.is_valid() && gizmo->get_plugin() == modeling_gizmo) {
			return gizmo->get_subgizmo_selection();
		}
	}
	return result;
}

Vector<int64_t> SolersModelingEditorPlugin::_selected_element_ids(const SolersEditableMesh &p_mesh) const {
	const Vector<int64_t> domain_ids = _domain_ids(p_mesh, get_selection_domain());
	Vector<int64_t> result;
	for (int handle : _selected_handles()) {
		if (handle >= 0 && handle < domain_ids.size()) {
			result.push_back(domain_ids[handle]);
		}
	}
	return result;
}

Vector<int64_t> SolersModelingEditorPlugin::_selected_vertex_ids(const SolersEditableMesh &p_mesh) const {
	HashSet<int64_t> unique;
	const Vector<int64_t> ids = _selected_element_ids(p_mesh);
	for (int64_t id : ids) {
		for (int64_t vertex_id : ::_element_vertices(p_mesh, get_selection_domain(), id)) {
			unique.insert(vertex_id);
		}
	}
	Vector<int64_t> result;
	for (int64_t id : unique) {
		result.push_back(id);
	}
	result.sort();
	return result;
}

void SolersModelingEditorPlugin::_inject_selection(const StringName &p_operation, const SolersEditableMesh &p_mesh, Dictionary &r_parameters) const {
	const Vector<int64_t> selected = _selected_element_ids(p_mesh);
	if (selected.is_empty() || p_operation == SNAME("select")) {
		return;
	}
	Array ids;
	for (int64_t id : selected) {
		ids.push_back(id);
	}
	if (p_operation == SNAME("delete")) {
		r_parameters["domain"] = get_selection_domain() == DOMAIN_VERTEX ? "vertex" : (get_selection_domain() == DOMAIN_EDGE ? "edge" : "face");
		r_parameters["ids"] = ids;
		return;
	}
	const Dictionary schema = SolersModelOperationRegistry::get_singleton()->get_operation(p_operation)->parameters_schema;
	const Dictionary properties = schema.get("properties", Dictionary());
	const StringName domain_key = get_selection_domain() == DOMAIN_VERTEX ? SNAME("vertex_ids") : (get_selection_domain() == DOMAIN_EDGE ? SNAME("edge_ids") : SNAME("face_ids"));
	if (properties.has(domain_key) && !r_parameters.has(domain_key)) {
		r_parameters[domain_key] = ids;
	}
	if ((p_operation == SNAME("transform") || p_operation == SNAME("weld")) && !r_parameters.has("vertex_ids")) {
		Array vertices;
		for (int64_t vertex_id : _selected_vertex_ids(p_mesh)) {
			vertices.push_back(vertex_id);
		}
		r_parameters["vertex_ids"] = vertices;
	}
}

Dictionary SolersModelingEditorPlugin::_apply(const StringName &p_operation, Dictionary p_parameters) {
	const String path = _selected_source_path();
	if (path.is_empty()) {
		EditorNode::get_singleton()->show_warning(TTR("Select a MeshInstance3D backed by a .smodel source."));
		return Dictionary();
	}
	SolersEditableMesh mesh;
	String error;
	if (SolersModelSource::load(path, mesh, &error) != OK) {
		EditorNode::get_singleton()->show_warning(error);
		return Dictionary();
	}
	_inject_selection(p_operation, mesh, p_parameters);
	const Dictionary result = SolersModelingService::get_singleton()->apply(path, p_operation, p_parameters, mesh.get_revision());
	if (!(bool)result.get("ok", false)) {
		EditorNode::get_singleton()->show_warning(JSON::stringify(result), TTR("Modeling operation failed"));
		return result;
	}
	_reload_selected_model(path);
	_refresh_modifiers();
	return result;
}

void SolersModelingEditorPlugin::_reload_selected_model(const String &p_path) {
	MeshInstance3D *instance = _selected_model();
	if (!instance) {
		return;
	}
	Ref<ArrayMesh> mesh = ResourceLoader::load(p_path, "ArrayMesh", ResourceFormatLoader::CACHE_MODE_REPLACE);
	if (mesh.is_valid()) {
		instance->set_mesh(mesh);
	}
	Node3DEditor::get_singleton()->update_all_gizmos(instance);
}

void SolersModelingEditorPlugin::finish_preview(MeshInstance3D *p_instance, const String &p_path) {
	if (p_instance == _selected_model()) {
		_reload_selected_model(p_path);
	}
	_refresh_modifiers();
}

void SolersModelingEditorPlugin::_refresh_operations(const String &p_filter) {
	PopupMenu *popup = operation_menu->get_popup();
	popup->clear();
	const String filter = p_filter.strip_edges().to_lower();
	int id = 0;
	for (const SolersModelOperationDefinition &operation : SolersModelOperationRegistry::get_singleton()->get_operations()) {
		const String name = String(operation.id);
		if (!filter.is_empty() && !name.to_lower().contains(filter) && !operation.description.to_lower().contains(filter)) {
			continue;
		}
		popup->add_item(name.replace("_", " ").capitalize(), id);
		popup->set_item_metadata(popup->get_item_count() - 1, name);
		id++;
	}
}

void SolersModelingEditorPlugin::_refresh_modifiers() {
	if (!modifier_list) {
		return;
	}
	modifier_list->clear();
	SolersEditableMesh mesh;
	if (_selected_source_path().is_empty() || SolersModelSource::load(_selected_source_path(), mesh) != OK) {
		return;
	}
	for (const SolersEditableMesh::Modifier &modifier : mesh.get_modifiers()) {
		const int item = modifier_list->add_item(String(modifier.type).capitalize() + (modifier.enabled ? String() : TTR(" (disabled)")));
		modifier_list->set_item_metadata(item, modifier.id);
	}
}

void SolersModelingEditorPlugin::_mode_changed(int p_index) {
	selection_domain->set_disabled(p_index == 0);
	Node3DEditor::get_singleton()->clear_subgizmo_selection();
	Node3DEditor::get_singleton()->update_all_gizmos();
}

void SolersModelingEditorPlugin::_selection_domain_changed(int p_index) {
	Node3DEditor::get_singleton()->clear_subgizmo_selection();
	Node3DEditor::get_singleton()->update_all_gizmos();
}

void SolersModelingEditorPlugin::_operation_chosen(int p_id) {
	const int index = operation_menu->get_popup()->get_item_index(p_id);
	if (index >= 0) {
		_show_operation(StringName(operation_menu->get_popup()->get_item_metadata(index)), last_operation == StringName(operation_menu->get_popup()->get_item_metadata(index)) ? last_parameters : "{}");
	}
}

void SolersModelingEditorPlugin::_show_operation(const StringName &p_operation, const String &p_parameters) {
	const SolersModelOperationDefinition *operation = SolersModelOperationRegistry::get_singleton()->get_operation(p_operation);
	ERR_FAIL_NULL(operation);
	pending_operation = p_operation;
	operation_dialog->set_title(String(p_operation).replace("_", " ").capitalize());
	operation_description->set_text(operation->description + "\n\n" + JSON::stringify(operation->parameters_schema, "  "));
	operation_parameters->set_text(p_parameters);
	operation_dialog->popup_centered();
}

void SolersModelingEditorPlugin::_confirm_operation() {
	Ref<JSON> json;
	json.instantiate();
	const Error error = json->parse(operation_parameters->get_text());
	if (error != OK || json->get_data().get_type() != Variant::DICTIONARY) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Parameters must be a JSON object.\n%s"), json->get_error_message()));
		return;
	}
	last_operation = pending_operation;
	last_parameters = operation_parameters->get_text();
	_apply(pending_operation, json->get_data());
}

void SolersModelingEditorPlugin::_quick_uv(int p_id) {
	_apply(p_id == 0 ? SNAME("unwrap_uv") : SNAME("pack_uv"), Dictionary());
}

void SolersModelingEditorPlugin::_add_modifier(int p_id) {
	static const char *types[] = { "mirror", "array", "solidify", "bevel", "boolean" };
	ERR_FAIL_INDEX(p_id, 5);
	Dictionary parameters;
	if (p_id == 1) {
		parameters["count"] = 2;
		parameters["offset"] = Vector3(1, 0, 0);
	} else if (p_id == 2) {
		parameters["thickness"] = 0.1;
	} else if (p_id == 3) {
		parameters["width"] = 0.05;
		parameters["segments"] = 2;
	} else if (p_id == 4) {
		_show_operation(SNAME("add_modifier"), R"({"type":"boolean","parameters":{"operand":"res://operand.smodel","operation":"subtract"}})");
		return;
	}
	Dictionary operation;
	operation["type"] = types[p_id];
	operation["parameters"] = parameters;
	_apply(SNAME("add_modifier"), operation);
}

void SolersModelingEditorPlugin::_remove_modifier() {
	const PackedInt32Array selected = modifier_list->get_selected_items();
	if (selected.is_empty()) {
		return;
	}
	Dictionary parameters;
	parameters["modifier_id"] = modifier_list->get_item_metadata(selected[0]);
	_apply(SNAME("remove_modifier"), parameters);
}

void SolersModelingEditorPlugin::_apply_modifiers() {
	_apply(SNAME("apply_modifiers"), Dictionary());
}
