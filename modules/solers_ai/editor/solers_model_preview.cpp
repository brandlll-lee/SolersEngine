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
#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/scene/3d/skeleton_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/gui/view_panner.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/environment.h"
#include "scene/resources/sky.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GLTF_ENABLED
#include "modules/gltf/extensions/gltf_document_extension_convert_importer_mesh.h"
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
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_preview_bounds(p_node->get_child(i), transform, r_bounds, r_has_bounds);
	}
}

static void _solers_preview_nodes(Node *p_node, Vector<Skeleton3D *> &r_skeletons, AnimationPlayer *&r_animation_player) {
	if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_node)) {
		r_skeletons.push_back(skeleton);
	}
	if (!r_animation_player) {
		r_animation_player = Object::cast_to<AnimationPlayer>(p_node);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_preview_nodes(p_node->get_child(i), r_skeletons, r_animation_player);
	}
}
#endif

void SolersModelPreview::_update_camera() {
	const float distance = MAX(model_radius * 2.8f * distance_scale, 0.25f);
	Vector3 direction = Basis(Vector3::UP, orbit_yaw).xform(Basis(Vector3::RIGHT, orbit_pitch).xform(Vector3(0, 0, 1)));
	camera->look_at_from_position(model_center + direction * distance, model_center, Vector3::UP);
	camera->set_perspective(42.0f, MAX(model_radius * 0.01f, 0.01f), MAX(distance + model_radius * 4.0f, 10.0f));
}

void SolersModelPreview::_pan_view(Vector2 p_delta, Ref<InputEvent>) {
	const float depth = camera->get_global_position().distance_to(model_center);
	const Vector2 center = get_size() * 0.5f;
	model_center += camera->project_position(center, depth) - camera->project_position(center + p_delta, depth);
	_update_camera();
}

void SolersModelPreview::_zoom_view(float p_factor, Vector2, Ref<InputEvent>) {
	distance_scale = CLAMP(distance_scale / p_factor, 0.35f, 4.0f);
	_update_camera();
}

void SolersModelPreview::clear_model() {
	stop_animation();
	animation_player = nullptr;
	skeletons.clear();
	for (MeshInstance3D *mesh : skeleton_meshes) {
		mesh->queue_free();
	}
	skeleton_meshes.clear();
	if (model_root) {
		model_root->queue_free();
		model_root = nullptr;
	}
}

PackedStringArray SolersModelPreview::get_animation_names() const {
	PackedStringArray result;
	if (!animation_player) {
		return result;
	}
	LocalVector<StringName> names;
	animation_player->get_animation_list(&names);
	for (const StringName &name : names) {
		if (name != StringName("RESET")) {
			result.push_back(name);
		}
	}
	return result;
}

bool SolersModelPreview::play_animation(const StringName &p_name) {
	if (!animation_player || !animation_player->has_animation(p_name)) {
		return false;
	}
	animation_player->play(p_name);
	return true;
}

void SolersModelPreview::pause_animation() {
	if (animation_player) {
		animation_player->pause();
	}
}

void SolersModelPreview::stop_animation() {
	if (animation_player) {
		animation_player->stop();
	}
}

void SolersModelPreview::set_skeleton_visible(bool p_visible) {
	if (p_visible && skeleton_meshes.is_empty() && Node3DEditor::get_singleton()) {
		for (Skeleton3D *skeleton : skeletons) {
			MeshInstance3D *bones = memnew(MeshInstance3D);
			bones->set_mesh(Skeleton3DGizmoPlugin::get_bones_mesh(skeleton, -1, true));
			bones->set_transform(skeleton->get_global_transform());
			bones->set_skeleton_path(skeleton->get_path());
			Ref<Skin> skin = skeleton->create_skin_from_rest_transforms();
			skeleton->register_skin(skin);
			bones->set_skin(skin);
			viewport->add_child(bones);
			skeleton_meshes.push_back(bones);
		}
	}
	for (MeshInstance3D *mesh : skeleton_meshes) {
		mesh->set_visible(p_visible);
	}
}

bool SolersModelPreview::is_animation_playing() const {
	return animation_player && animation_player->is_playing();
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
	if (panner->gui_input(p_event, get_global_rect())) {
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
	state->set_handle_binary_image_mode(GLTFState::HANDLE_BINARY_IMAGE_MODE_EMBED_AS_UNCOMPRESSED);
	const Error error = document->append_from_file(p_path, state);
	if (error != OK) {
		return error;
	}
	Node *generated = document->generate_scene(state);
	if (!generated) {
		return ERR_CANT_CREATE;
	}
	Ref<GLTFDocumentExtensionConvertImporterMesh> converter;
	converter.instantiate();
	const Error conversion_error = converter->import_post(state, generated);
	if (conversion_error != OK) {
		memdelete(generated);
		return conversion_error;
	}
	model_root = Object::cast_to<Node3D>(generated);
	if (!model_root) {
		if (generated) {
			memdelete(generated);
		}
		return ERR_INVALID_DATA;
	}
	viewport->add_child(model_root);
	_solers_preview_nodes(model_root, skeletons, animation_player);
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
	set_mouse_target(true);
	set_focus_mode(FOCUS_ALL);
	panner.instantiate();
	panner->set_callbacks(callable_mp(this, &SolersModelPreview::_pan_view), callable_mp(this, &SolersModelPreview::_zoom_view));
	panner->setup_warped_panning(this, true);
	viewport = memnew(SubViewport);
	Ref<World3D> world;
	world.instantiate();
	viewport->set_world_3d(world);
	viewport->set_disable_input(true);
	viewport->set_msaa_3d(Viewport::MSAA_4X);
	add_child(viewport);

	Ref<Environment> environment;
	environment.instantiate();
	Ref<Sky> sky;
	sky.instantiate();
	Ref<ProceduralSkyMaterial> sky_material = memnew(ProceduralSkyMaterial);
	sky->set_material(sky_material);
	environment->set_sky(sky);
	environment->set_background(Environment::BG_COLOR);
	environment->set_bg_color(Color(0.065f, 0.065f, 0.072f));
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
	environment->set_reflection_source(Environment::REFLECTION_SOURCE_SKY);
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
