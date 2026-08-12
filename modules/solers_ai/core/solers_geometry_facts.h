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
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/visual_instance_3d.h"
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
