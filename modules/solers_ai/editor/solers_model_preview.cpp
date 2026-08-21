/**************************************************************************/
/*  solers_model_preview.cpp                                              */
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

#include "solers_model_preview.h"

#include "core/input/input_event.h"
#include "core/math/math_funcs.h"
#include "editor/editor_node.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/importer_mesh_instance_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/environment.h"

#ifdef MODULE_GLTF_ENABLED
#include "modules/gltf/gltf_document.h"
#include "modules/gltf/gltf_state.h"
#endif

#ifdef MODULE_GLTF_ENABLED
static void _solers_preview_merge_aabb(const AABB &p_value, AABB &r_bounds, bool &r_has_bounds) {
	if (p_value.size.is_zero_approx()) {
		return;
	}
	if (r_has_bounds) {
		r_bounds = r_bounds.merge(p_value);
	} else {
		r_bounds = p_value;
		r_has_bounds = true;
	}
}

static void _solers_preview_bounds(Node *p_node, const Transform3D &p_parent, AABB &r_bounds, bool &r_has_bounds) {
	Transform3D transform = p_parent;
	if (Node3D *node_3d = Object::cast_to<Node3D>(p_node)) {
		transform *= node_3d->get_transform();
	}
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node)) {
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_valid()) {
			_solers_preview_merge_aabb(transform.xform(mesh->get_aabb()), r_bounds, r_has_bounds);
		}
	} else if (ImporterMeshInstance3D *importer_instance = Object::cast_to<ImporterMeshInstance3D>(p_node)) {
		const Ref<ImporterMesh> importer_mesh = importer_instance->get_mesh();
		if (importer_mesh.is_valid()) {
			const Ref<ArrayMesh> mesh = importer_mesh->get_mesh();
			if (mesh.is_valid()) {
				_solers_preview_merge_aabb(transform.xform(mesh->get_aabb()), r_bounds, r_has_bounds);
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_preview_bounds(p_node->get_child(i), transform, r_bounds, r_has_bounds);
	}
}
#endif

void SolersModelPreview::_update_camera() {
	const float distance = MAX(model_radius * 2.8f * distance_scale, 0.25f);
	Vector3 direction = Basis(Vector3::UP, orbit_yaw).xform(Basis(Vector3::RIGHT, orbit_pitch).xform(Vector3(0, 0, 1)));
	camera->look_at_from_position(model_center + direction * distance, model_center, Vector3::UP);
	camera->set_perspective(42.0f, MAX(model_radius * 0.01f, 0.01f), MAX(distance + model_radius * 4.0f, 10.0f));
}

void SolersModelPreview::clear_model() {
	if (model_root) {
		model_root->queue_free();
		model_root = nullptr;
	}
}

void SolersModelPreview::gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid() && motion->get_button_mask().has_flag(MouseButtonMask::LEFT)) {
		orbit_yaw -= motion->get_relative().x * 0.01f;
		orbit_pitch = CLAMP(orbit_pitch - motion->get_relative().y * 0.01f, -1.35f, 1.35f);
		_update_camera();
		accept_event();
		return;
	}
	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid() && button->is_pressed() && (button->get_button_index() == MouseButton::WHEEL_UP || button->get_button_index() == MouseButton::WHEEL_DOWN)) {
		distance_scale = CLAMP(distance_scale * (button->get_button_index() == MouseButton::WHEEL_UP ? 0.9f : 1.1f), 0.35f, 4.0f);
		_update_camera();
		accept_event();
	}
}

Error SolersModelPreview::load_model(const String &p_path) {
	clear_model();
#ifdef MODULE_GLTF_ENABLED
	Ref<GLTFDocument> document;
	document.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	const Error error = document->append_from_file(p_path, state);
	if (error != OK) {
		return error;
	}
	Node *generated = document->generate_scene(state);
	model_root = Object::cast_to<Node3D>(generated);
	if (!model_root) {
		if (generated) {
			memdelete(generated);
		}
		return ERR_INVALID_DATA;
	}
	viewport->add_child(model_root);
	AABB bounds;
	bool has_bounds = false;
	_solers_preview_bounds(model_root, Transform3D(), bounds, has_bounds);
	model_center = has_bounds ? bounds.get_center() : Vector3();
	model_radius = has_bounds ? MAX(bounds.get_longest_axis_size() * 0.5f, 0.1f) : 1.0f;
	distance_scale = 1.0f;
	orbit_yaw = 0.45f;
	orbit_pitch = -0.18f;
	_update_camera();
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

SolersModelPreview::SolersModelPreview() {
	set_stretch(true);
	set_focus_mode(FOCUS_ALL);
	viewport = memnew(SubViewport);
	Ref<World3D> world;
	world.instantiate();
	viewport->set_world_3d(world);
	viewport->set_disable_input(true);
	viewport->set_msaa_3d(Viewport::MSAA_4X);
	add_child(viewport);

	Ref<Environment> environment;
	environment.instantiate();
	environment->set_background(Environment::BG_COLOR);
	environment->set_bg_color(Color(0.065f, 0.065f, 0.072f));
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_COLOR);
	environment->set_ambient_light_color(Color(0.72f, 0.74f, 0.78f));
	environment->set_ambient_light_energy(0.55f);
	camera = memnew(Camera3D);
	camera->set_environment(environment);
	viewport->add_child(camera);

	DirectionalLight3D *key = memnew(DirectionalLight3D);
	key->set_transform(Transform3D().looking_at(Vector3(-1, -1, -1), Vector3::UP));
	viewport->add_child(key);
	DirectionalLight3D *fill = memnew(DirectionalLight3D);
	fill->set_transform(Transform3D().looking_at(Vector3(1, -0.25f, 0.5f), Vector3::UP));
	fill->set_color(Color(0.64f, 0.70f, 0.82f));
	fill->set_param(Light3D::PARAM_ENERGY, 0.45f);
	viewport->add_child(fill);
	_update_camera();
	if (EditorNode::get_singleton()) {
		EditorNode::get_singleton()->register_hdr_viewport(viewport);
	}
}
