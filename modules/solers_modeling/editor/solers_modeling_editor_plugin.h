/**************************************************************************/
/*  solers_modeling_editor_plugin.h                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"
#include "modules/solers_modeling/core/solers_editable_mesh.h"
#include "scene/gui/option_button.h"

class ConfirmationDialog;
class HBoxContainer;
class ItemList;
class Label;
class LineEdit;
class MenuButton;
class MeshInstance3D;
class TextEdit;
class VBoxContainer;

class SolersModelingEditorPlugin;

class SolersModelingGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(SolersModelingGizmoPlugin, EditorNode3DGizmoPlugin);

	struct Preview {
		String source_path;
		int64_t revision = 0;
		SolersEditableMesh source;
		Ref<ArrayMesh> original_mesh;
		Transform3D delta;
		Vector<int> handles;
	};

	SolersModelingEditorPlugin *editor = nullptr;
	HashMap<ObjectID, Preview> previews;

	String _source_path(MeshInstance3D *p_instance) const;
	Vector<int64_t> _element_vertices(const SolersEditableMesh &p_mesh, int p_handle) const;
	Vector3 _element_center(const SolersEditableMesh &p_mesh, int p_handle) const;

protected:
	static void _bind_methods() {}
	bool has_gizmo(Node3D *p_spatial) override;

public:
	String get_gizmo_name() const override { return "Solers Modeling"; }
	int get_priority() const override { return -2; }
	void redraw(EditorNode3DGizmo *p_gizmo) override;
	int subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const override;
	Vector<int> subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const override;
	Transform3D get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const override;
	void set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) override;
	void commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel = false) override;

	SolersModelingGizmoPlugin(SolersModelingEditorPlugin *p_editor = nullptr);
};

class SolersModelingEditorPlugin : public EditorPlugin {
	GDCLASS(SolersModelingEditorPlugin, EditorPlugin);

public:
	enum SelectionDomain {
		DOMAIN_VERTEX,
		DOMAIN_EDGE,
		DOMAIN_FACE,
	};

private:
	Ref<SolersModelingGizmoPlugin> modeling_gizmo;
	HBoxContainer *toolbar = nullptr;
	OptionButton *mode = nullptr;
	OptionButton *selection_domain = nullptr;
	LineEdit *operation_filter = nullptr;
	MenuButton *operation_menu = nullptr;
	MenuButton *uv_menu = nullptr;
	VBoxContainer *modifier_panel = nullptr;
	ItemList *modifier_list = nullptr;
	MenuButton *add_modifier = nullptr;
	ConfirmationDialog *operation_dialog = nullptr;
	Label *operation_description = nullptr;
	TextEdit *operation_parameters = nullptr;
	StringName pending_operation;
	StringName last_operation;
	String last_parameters = "{}";
	bool active = false;

	MeshInstance3D *_selected_model() const;
	String _selected_source_path() const;
	Vector<int> _selected_handles() const;
	Vector<int64_t> _selected_element_ids(const SolersEditableMesh &p_mesh) const;
	Vector<int64_t> _selected_vertex_ids(const SolersEditableMesh &p_mesh) const;
	void _inject_selection(const StringName &p_operation, const SolersEditableMesh &p_mesh, Dictionary &r_parameters) const;
	Dictionary _apply(const StringName &p_operation, Dictionary p_parameters);
	void _reload_selected_model(const String &p_path);
	void _refresh_operations(const String &p_filter = String());
	void _refresh_modifiers();
	void _mode_changed(int p_index);
	void _selection_domain_changed(int p_index);
	void _operation_chosen(int p_id);
	void _show_operation(const StringName &p_operation, const String &p_parameters = "{}");
	void _confirm_operation();
	void _quick_uv(int p_id);
	void _add_modifier(int p_id);
	void _remove_modifier();
	void _apply_modifiers();

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	String get_plugin_name() const override { return TTRC("Modeling"); }
	const Ref<Texture2D> get_plugin_icon() const override;
	bool has_main_screen() const override { return true; }
	void make_visible(bool p_visible) override;
	bool handles(Object *p_object) const override;
	void edit(Object *p_object) override;
	SelectionDomain get_selection_domain() const { return (SelectionDomain)selection_domain->get_selected_id(); }
	bool is_edit_mode() const { return active && mode->get_selected_id() == 1; }
	void finish_preview(MeshInstance3D *p_instance, const String &p_path);

	SolersModelingEditorPlugin();
	~SolersModelingEditorPlugin();
};
