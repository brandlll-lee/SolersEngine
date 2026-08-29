/**************************************************************************/
/*  solers_geometry_facts.h                                               */
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

#include "core/templates/hash_set.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_CSG_ENABLED
#include "modules/csg/csg_shape.h"
#endif
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/mesh.h"

inline Array solers_vector3_data(const Vector3 &p_value) {
	Array data;
	data.push_back(p_value.x);
	data.push_back(p_value.y);
	data.push_back(p_value.z);
	return data;
}

inline Dictionary solers_aabb_data(const AABB &p_bounds) {
	Dictionary data;
	data["position"] = solers_vector3_data(p_bounds.position);
	data["size"] = solers_vector3_data(p_bounds.size);
	data["end"] = solers_vector3_data(p_bounds.get_end());
	data["center"] = solers_vector3_data(p_bounds.get_center());
	return data;
}

inline void solers_accumulate_mesh_facts(const Ref<Mesh> &p_mesh, HashSet<ObjectID> &r_seen, Dictionary &r_facts) {
	if (p_mesh.is_null() || r_seen.has(p_mesh->get_instance_id())) {
		return;
	}
	r_seen.insert(p_mesh->get_instance_id());
	int64_t triangles = r_facts.get("triangle_count", 0);
	int surfaces = r_facts.get("surface_count", 0);
	int uv2_surfaces = r_facts.get("uv2_surface_count", 0);
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
		surfaces++;
		uv2_surfaces += (p_mesh->surface_get_format(surface) & Mesh::ARRAY_FORMAT_TEX_UV2) ? 1 : 0;
		if (p_mesh->surface_get_primitive_type(surface) == Mesh::PRIMITIVE_TRIANGLES) {
			const int indices = p_mesh->surface_get_array_index_len(surface);
			triangles += (indices > 0 ? indices : p_mesh->surface_get_array_len(surface)) / 3;
		}
	}
	r_facts["unique_mesh_count"] = (int)r_seen.size();
	r_facts["surface_count"] = surfaces;
	r_facts["uv2_surface_count"] = uv2_surfaces;
	r_facts["triangle_count"] = triangles;
}

inline Dictionary solers_describe_mesh(const Ref<Mesh> &p_mesh) {
	Dictionary facts;
	HashSet<ObjectID> seen;
	solers_accumulate_mesh_facts(p_mesh, seen, facts);
	const int surfaces = facts.get("surface_count", 0);
	facts["mesh_count"] = facts.get("unique_mesh_count", 0);
	facts["uv2_complete"] = surfaces > 0 && (int)facts.get("uv2_surface_count", 0) == surfaces;
	if (p_mesh.is_valid()) {
		facts["aabb"] = solers_aabb_data(p_mesh->get_aabb());
		facts["lowest_y"] = p_mesh->get_aabb().position.y;
	}
	return facts;
}

inline void solers_accumulate_geometry_facts(Node *p_node, const Transform3D &p_parent_transform, HashSet<ObjectID> &r_seen_meshes, Dictionary &r_facts, AABB &r_bounds, bool &r_has_bounds) {
	if (!p_node) {
		return;
	}
	Transform3D transform = p_parent_transform;
	if (Node3D *node_3d = Object::cast_to<Node3D>(p_node)) {
		transform *= node_3d->get_transform();
	}
	if (VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(p_node)) {
		const AABB local_bounds = visual->get_aabb();
		if (local_bounds.has_volume()) {
			const AABB transformed = transform.xform(local_bounds);
			r_bounds = r_has_bounds ? r_bounds.merge(transformed) : transformed;
			r_has_bounds = true;
			r_facts["geometry_count"] = (int)r_facts.get("geometry_count", 0) + 1;
		}
	}
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node)) {
		r_facts["mesh_instance_count"] = (int)r_facts.get("mesh_instance_count", 0) + 1;
		solers_accumulate_mesh_facts(mesh_instance->get_mesh(), r_seen_meshes, r_facts);
	}
#ifdef MODULE_CSG_ENABLED
	if (CSGShape3D *csg = Object::cast_to<CSGShape3D>(p_node)) {
		const Array meshes = csg->get_meshes();
		for (int i = 1; i < meshes.size(); i += 2) {
			const Ref<Mesh> mesh = meshes[i];
			solers_accumulate_mesh_facts(mesh, r_seen_meshes, r_facts);
		}
	}
#endif
	for (int i = 0; i < p_node->get_child_count(); i++) {
		solers_accumulate_geometry_facts(p_node->get_child(i), transform, r_seen_meshes, r_facts, r_bounds, r_has_bounds);
	}
}

inline void solers_accumulate_world_aabb(Node *p_node, AABB &r_bounds, bool &r_found, int *r_visual_count = nullptr) {
	if (!p_node) {
		return;
	}
	if (VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(p_node)) {
		const AABB bounds = visual->get_global_transform().xform(visual->get_aabb());
		if (visual->is_visible_in_tree() && bounds.has_volume()) {
			r_bounds = r_found ? r_bounds.merge(bounds) : bounds;
			r_found = true;
			if (r_visual_count) {
				(*r_visual_count)++;
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		solers_accumulate_world_aabb(p_node->get_child(i), r_bounds, r_found, r_visual_count);
	}
}

inline Array solers_rect2_data(const Rect2 &p_rect) {
	return Array({ p_rect.position.x, p_rect.position.y, p_rect.size.x, p_rect.size.y });
}

inline Dictionary solers_project_aabb(Camera3D *p_camera, const AABB &p_bounds) {
	Dictionary facts;
	Viewport *viewport = p_camera ? p_camera->get_viewport() : nullptr;
	facts["available"] = false;
	if (!viewport || !p_bounds.has_volume()) {
		return facts;
	}

	Rect2 projected;
	bool has_projection = false;
	int corners_in_front = 0;
	int corners_in_depth_range = 0;
	const Transform3D camera_inverse = p_camera->get_global_transform().affine_inverse();
	for (int i = 0; i < 8; i++) {
		const Vector3 corner = p_bounds.position + Vector3((i & 1) ? p_bounds.size.x : 0.0, (i & 2) ? p_bounds.size.y : 0.0, (i & 4) ? p_bounds.size.z : 0.0);
		if (p_camera->is_position_behind(corner)) {
			continue;
		}
		const real_t depth = -camera_inverse.xform(corner).z;
		corners_in_depth_range += depth >= p_camera->get_near() && depth <= p_camera->get_far() ? 1 : 0;
		const Vector2 screen = p_camera->unproject_position(corner);
		projected = has_projection ? projected.expand(screen) : Rect2(screen, Vector2());
		has_projection = true;
		corners_in_front++;
	}
	facts["in_front"] = corners_in_front > 0;
	facts["fully_in_front"] = corners_in_front == 8;
	facts["in_depth_range"] = corners_in_depth_range > 0;
	facts["fully_in_depth_range"] = corners_in_depth_range == 8;
	if (!has_projection) {
		return facts;
	}

	const Rect2 viewport_rect(Vector2(), viewport->get_visible_rect().size);
	const Rect2 clipped = projected.intersection(viewport_rect);
	const real_t viewport_area = viewport_rect.get_area();
	const real_t projected_area = projected.get_area();
	facts["available"] = true;
	facts["projected_rect"] = solers_rect2_data(projected);
	facts["clipped_rect"] = solers_rect2_data(clipped);
	facts["in_frame"] = corners_in_depth_range > 0 && clipped.has_area();
	facts["fully_in_frame"] = corners_in_front == 8 && corners_in_depth_range == 8 && viewport_rect.encloses(projected);
	facts["viewport_fraction"] = viewport_area > 0.0 ? clipped.get_area() / viewport_area : 0.0;
	facts["clipped_fraction"] = projected_area > 0.0 ? 1.0 - clipped.get_area() / projected_area : 1.0;
	return facts;
}

inline Transform3D solers_frame_aabb(const Transform3D &p_view, const AABB &p_bounds, real_t p_fov_degrees, real_t p_aspect, Camera3D::KeepAspect p_keep_aspect, real_t p_near) {
	const real_t half_fov = Math::deg_to_rad(p_fov_degrees) * 0.5;
	real_t half_vertical = half_fov;
	real_t half_horizontal = Math::atan(Math::tan(half_vertical) * p_aspect);
	if (p_keep_aspect == Camera3D::KEEP_WIDTH) {
		half_horizontal = half_fov;
		half_vertical = Math::atan(Math::tan(half_horizontal) / p_aspect);
	}
	const real_t limiting_angle = MIN(half_vertical, half_horizontal);
	const real_t radius = p_bounds.size.length() * 0.5;
	const real_t distance = MAX(radius / Math::sin(limiting_angle), radius + p_near);
	const Basis basis = p_view.basis.orthonormalized();
	return Transform3D(basis, p_bounds.get_center() + basis.get_column(2) * distance);
}

inline Dictionary solers_describe_geometry(Node *p_root, bool p_world_space = false) {
	Dictionary facts;
	if (!p_root) {
		return facts;
	}
	Transform3D base;
	if (p_world_space) {
		if (Node3D *root_3d = Object::cast_to<Node3D>(p_root)) {
			base = root_3d->get_global_transform() * root_3d->get_transform().affine_inverse();
		}
	}
	HashSet<ObjectID> seen_meshes;
	AABB bounds;
	bool has_bounds = false;
	solers_accumulate_geometry_facts(p_root, base, seen_meshes, facts, bounds, has_bounds);
	const int surfaces = facts.get("surface_count", 0);
	facts["mesh_count"] = facts.get("unique_mesh_count", 0);
	facts["uv2_complete"] = surfaces > 0 && (int)facts.get("uv2_surface_count", 0) == surfaces;
	facts["coordinate_space"] = p_world_space ? "world" : "root_relative";
	if (has_bounds) {
		facts["aabb"] = solers_aabb_data(bounds);
		facts["lowest_y"] = bounds.position.y;
	}
	if (Node3D *root_3d = Object::cast_to<Node3D>(p_root)) {
		Dictionary transform;
		transform["position"] = solers_vector3_data(p_world_space ? root_3d->get_global_position() : root_3d->get_position());
		transform["rotation_degrees"] = solers_vector3_data(p_world_space ? root_3d->get_global_rotation_degrees() : root_3d->get_rotation_degrees());
		transform["scale"] = solers_vector3_data(p_world_space ? root_3d->get_global_basis().get_scale() : root_3d->get_scale());
		facts["root_transform"] = transform;
	}
	return facts;
}
