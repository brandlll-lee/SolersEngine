/**************************************************************************/
/*  solers_model_operation.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_model_operation.h"

#include "core/io/json.h"
#include "core/math/geometry_3d.h"

static Dictionary _operation_schema(const char *p_json) {
	const Variant parsed = JSON::parse_string(p_json);
	ERR_FAIL_COND_V_MSG(parsed.get_type() != Variant::DICTIONARY, Dictionary(), "Invalid Solers modeling operation schema.");
	return parsed;
}

static Vector3 _vector3_arg(const Variant &p_value, const Vector3 &p_default = Vector3()) {
	if (p_value.get_type() == Variant::VECTOR3) {
		return p_value;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary value = p_value;
		return Vector3(value.get("x", p_default.x), value.get("y", p_default.y), value.get("z", p_default.z));
	}
	if (p_value.get_type() == Variant::ARRAY) {
		const Array value = p_value;
		if (value.size() >= 3) {
			return Vector3(value[0], value[1], value[2]);
		}
	}
	return p_default;
}

static Vector2 _vector2_arg(const Variant &p_value, const Vector2 &p_default = Vector2()) {
	if (p_value.get_type() == Variant::VECTOR2) {
		return p_value;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary value = p_value;
		return Vector2(value.get("x", p_default.x), value.get("y", p_default.y));
	}
	if (p_value.get_type() == Variant::ARRAY) {
		const Array value = p_value;
		if (value.size() >= 2) {
			return Vector2(value[0], value[1]);
		}
	}
	return p_default;
}

static Vector<int64_t> _id_array(const Variant &p_value) {
	Vector<int64_t> ids;
	if (p_value.get_type() == Variant::ARRAY) {
		for (const Variant &value : (Array)p_value) {
			ids.push_back(value);
		}
	} else if (p_value.get_type() == Variant::PACKED_INT64_ARRAY) {
		for (int64_t value : (PackedInt64Array)p_value) {
			ids.push_back(value);
		}
	} else if (p_value.get_type() == Variant::PACKED_INT32_ARRAY) {
		for (int32_t value : (PackedInt32Array)p_value) {
			ids.push_back(value);
		}
	}
	return ids;
}

static Array _id_result(const Vector<int64_t> &p_ids) {
	Array result;
	for (int64_t id : p_ids) {
		result.push_back(id);
	}
	return result;
}

static Vector3 _face_normal(const SolersEditableMesh &p_mesh, int64_t p_face_id) {
	const Vector<int64_t> vertex_ids = p_mesh.get_face_vertices(p_face_id);
	Vector3 normal;
	for (int i = 0; i < vertex_ids.size(); i++) {
		const Vector3 a = p_mesh.get_vertex(vertex_ids[i])->position;
		const Vector3 b = p_mesh.get_vertex(vertex_ids[(i + 1) % vertex_ids.size()])->position;
		normal.x += (a.y - b.y) * (a.z + b.z);
		normal.y += (a.z - b.z) * (a.x + b.x);
		normal.z += (a.x - b.x) * (a.y + b.y);
	}
	return normal.normalized();
}

static Vector<int64_t> _face_selection(const SolersEditableMesh &p_mesh, const Dictionary &p_args) {
	Vector<int64_t> face_ids = _id_array(p_args.get("face_ids", Variant()));
	return face_ids.is_empty() ? p_mesh.get_selected_faces() : face_ids;
}

static Vector<int64_t> _edge_selection(const SolersEditableMesh &p_mesh, const Dictionary &p_args) {
	Vector<int64_t> edge_ids = _id_array(p_args.get("edge_ids", Variant()));
	return edge_ids.is_empty() ? p_mesh.get_selected_edges() : edge_ids;
}

static Vector<int64_t> _vertex_selection(const SolersEditableMesh &p_mesh, const Dictionary &p_args) {
	Vector<int64_t> vertex_ids = _id_array(p_args.get("vertex_ids", Variant()));
	return vertex_ids.is_empty() ? p_mesh.expand_vertices_from_selection() : vertex_ids;
}

static Dictionary _created_result(const Vector<int64_t> &p_vertices, const Vector<int64_t> &p_edges, const Vector<int64_t> &p_faces) {
	Dictionary data;
	data["created_vertices"] = _id_result(p_vertices);
	data["created_edges"] = _id_result(p_edges);
	data["created_faces"] = _id_result(p_faces);
	data["selected_vertices"] = _id_result(p_vertices);
	data["selected_faces"] = _id_result(p_faces);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _create_box(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector3 size = _vector3_arg(p_args.get("size", Variant()), Vector3(1, 1, 1));
	const Vector3 center = _vector3_arg(p_args.get("center", Variant()));
	if (!size.is_finite() || size.x <= 0 || size.y <= 0 || size.z <= 0 || !center.is_finite()) {
		Dictionary error;
		error["code"] = "MODEL_ARGUMENT_INVALID";
		error["message"] = "Box dimensions must be finite and positive.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	const Vector3 half = size * 0.5;
	Vector<int64_t> vertices;
	vertices.push_back(r_mesh.add_vertex(center + Vector3(-half.x, -half.y, -half.z)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(half.x, -half.y, -half.z)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(half.x, half.y, -half.z)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(-half.x, half.y, -half.z)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(-half.x, -half.y, half.z)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(half.x, -half.y, half.z)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(half.x, half.y, half.z)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(-half.x, half.y, half.z)));
	const int face_indices[6][4] = {
		{ 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
		{ 3, 7, 6, 2 }, { 0, 4, 7, 3 }, { 1, 2, 6, 5 }
	};
	Vector<int64_t> faces;
	for (const auto &indices : face_indices) {
		Vector<int64_t> face_vertices;
		for (int index : indices) {
			face_vertices.push_back(vertices[index]);
		}
		const int64_t face_id = r_mesh.add_face(face_vertices);
		faces.push_back(face_id);
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		for (int i = 0; i < 4; i++) {
			r_mesh.set_loop_uv(face->loops[i], Vector2((i == 1 || i == 2) ? 1 : 0, (i >= 2) ? 1 : 0));
		}
	}
	r_mesh.select_faces(faces);
	return _created_result(vertices, r_mesh.get_edge_ids(), faces);
}

static Dictionary _create_plane(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector2 size = _vector2_arg(p_args.get("size", Variant()), Vector2(1, 1));
	const Vector3 center = _vector3_arg(p_args.get("center", Variant()));
	if (!size.is_finite() || size.x <= 0 || size.y <= 0 || !center.is_finite()) {
		Dictionary error;
		error["code"] = "MODEL_ARGUMENT_INVALID";
		error["message"] = "Plane size must be finite and positive.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	Vector<int64_t> vertices;
	vertices.push_back(r_mesh.add_vertex(center + Vector3(-size.x * 0.5, 0, -size.y * 0.5)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(-size.x * 0.5, 0, size.y * 0.5)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(size.x * 0.5, 0, size.y * 0.5)));
	vertices.push_back(r_mesh.add_vertex(center + Vector3(size.x * 0.5, 0, -size.y * 0.5)));
	const int64_t face_id = r_mesh.add_face(vertices);
	const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
	for (int i = 0; i < 4; i++) {
		r_mesh.set_loop_uv(face->loops[i], Vector2((i >= 2) ? 1 : 0, (i == 1 || i == 2) ? 1 : 0));
	}
	Vector<int64_t> faces;
	faces.push_back(face_id);
	r_mesh.select_faces(faces);
	return _created_result(vertices, r_mesh.get_edge_ids(), faces);
}

static Dictionary _create_cylinder(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const int segments = CLAMP((int)p_args.get("segments", 32), 3, 256);
	const double radius = p_args.get("radius", 0.5);
	const double depth = p_args.get("depth", 1.0);
	const Vector3 center = _vector3_arg(p_args.get("center", Variant()));
	if (!Math::is_finite(radius) || !Math::is_finite(depth) || radius <= 0 || depth <= 0 || !center.is_finite()) {
		Dictionary error;
		error["code"] = "MODEL_ARGUMENT_INVALID";
		error["message"] = "Cylinder radius and depth must be finite and positive.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	Vector<int64_t> bottom;
	Vector<int64_t> top;
	Vector<int64_t> created_vertices;
	for (int i = 0; i < segments; i++) {
		const double angle = Math::TAU * (double)i / segments;
		const Vector3 radial(Math::cos(angle) * radius, 0, Math::sin(angle) * radius);
		bottom.push_back(r_mesh.add_vertex(center + radial + Vector3(0, -depth * 0.5, 0)));
		top.push_back(r_mesh.add_vertex(center + radial + Vector3(0, depth * 0.5, 0)));
		created_vertices.push_back(bottom[i]);
		created_vertices.push_back(top[i]);
	}
	Vector<int64_t> created_faces;
	Vector<int64_t> bottom_reversed;
	for (int i = segments - 1; i >= 0; i--) {
		bottom_reversed.push_back(bottom[i]);
	}
	created_faces.push_back(r_mesh.add_face(bottom_reversed));
	created_faces.push_back(r_mesh.add_face(top));
	for (int i = 0; i < segments; i++) {
		Vector<int64_t> side;
		side.push_back(bottom[i]);
		side.push_back(bottom[(i + 1) % segments]);
		side.push_back(top[(i + 1) % segments]);
		side.push_back(top[i]);
		const int64_t face_id = r_mesh.add_face(side, 0, true);
		created_faces.push_back(face_id);
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		const double u0 = (double)i / segments;
		const double u1 = (double)(i + 1) / segments;
		r_mesh.set_loop_uv(face->loops[0], Vector2(u0, 0));
		r_mesh.set_loop_uv(face->loops[1], Vector2(u1, 0));
		r_mesh.set_loop_uv(face->loops[2], Vector2(u1, 1));
		r_mesh.set_loop_uv(face->loops[3], Vector2(u0, 1));
	}
	r_mesh.select_faces(created_faces);
	return _created_result(created_vertices, r_mesh.get_edge_ids(), created_faces);
}

static Dictionary _select_elements(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const String domain = String(p_args.get("domain", "face")).to_lower();
	const String mode = String(p_args.get("mode", "replace")).to_lower();
	const bool replace = mode == "replace";
	Vector<int64_t> ids = _id_array(p_args.get("ids", Array()));
	if ((bool)p_args.get("all", false)) {
		ids = domain == "vertex" ? r_mesh.get_vertex_ids() : (domain == "edge" ? r_mesh.get_edge_ids() : r_mesh.get_face_ids());
	}
	if ((bool)p_args.get("boundary", false) && domain == "edge") {
		ids = r_mesh.get_boundary_edges();
	}
	if (mode == "subtract") {
		for (int64_t id : ids) {
			if (domain == "vertex" && r_mesh.get_vertex(id)) {
				r_mesh.get_vertex(id)->selected = false;
			} else if (domain == "edge" && r_mesh.get_edge(id)) {
				r_mesh.get_edge(id)->selected = false;
			} else if (domain == "face" && r_mesh.get_face(id)) {
				r_mesh.get_face(id)->selected = false;
			}
		}
	} else if (domain == "vertex") {
		r_mesh.select_vertices(ids, replace);
	} else if (domain == "edge") {
		r_mesh.select_edges(ids, replace);
	} else if (domain == "face") {
		r_mesh.select_faces(ids, replace);
	} else {
		Dictionary error;
		error["code"] = "MODEL_ARGUMENT_INVALID";
		error["message"] = "Selection domain must be vertex, edge, or face.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	Dictionary data;
	data["selected_vertices"] = _id_result(r_mesh.get_selected_vertices());
	data["selected_edges"] = _id_result(r_mesh.get_selected_edges());
	data["selected_faces"] = _id_result(r_mesh.get_selected_faces());
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _transform_selection(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> vertex_ids = _vertex_selection(r_mesh, p_args);
	if (vertex_ids.is_empty()) {
		Dictionary error;
		error["code"] = "MODEL_SELECTION_EMPTY";
		error["message"] = "Transform requires selected vertices, edges, or faces.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	Vector3 translation = _vector3_arg(p_args.get("translation", Variant()));
	Vector3 rotation = _vector3_arg(p_args.get("rotation_degrees", Variant())) * Math::PI / 180.0;
	Vector3 scale = _vector3_arg(p_args.get("scale", Variant()), Vector3(1, 1, 1));
	const Vector3 pivot = _vector3_arg(p_args.get("pivot", Variant()));
	const String axis = String(p_args.get("axis", "all")).to_lower();
	if (axis == "x" || axis == "y" || axis == "z") {
		const int component = axis == "x" ? 0 : (axis == "y" ? 1 : 2);
		for (int i = 0; i < 3; i++) {
			if (i != component) {
				translation[i] = 0;
				rotation[i] = 0;
				scale[i] = 1;
			}
		}
	}
	const double snap = p_args.get("snap", 0.0);
	if (snap > 0) {
		translation.x = Math::snapped(translation.x, snap);
		translation.y = Math::snapped(translation.y, snap);
		translation.z = Math::snapped(translation.z, snap);
	}
	if (!translation.is_finite() || !rotation.is_finite() || !scale.is_finite() || scale.x == 0 || scale.y == 0 || scale.z == 0) {
		Dictionary error;
		error["code"] = "MODEL_ARGUMENT_INVALID";
		error["message"] = "Transform values must be finite and scale components cannot be zero.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	const Basis basis = Basis::from_euler(rotation).scaled(scale);
	const Transform3D transform(basis, pivot + translation - basis.xform(pivot));
	String error_message;
	if (!r_mesh.transform_vertices(vertex_ids, transform, &error_message)) {
		Dictionary error;
		error["code"] = "MODEL_TRANSFORM_FAILED";
		error["message"] = error_message;
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	Dictionary data;
	data["modified_vertices"] = _id_result(vertex_ids);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

struct _ExtrudeFaceData {
	Vector<int64_t> vertices;
	int material = 0;
	bool smooth = false;
};

static Dictionary _extrude_faces(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> face_ids = _face_selection(r_mesh, p_args);
	if (face_ids.is_empty()) {
		Dictionary error;
		error["code"] = "MODEL_SELECTION_EMPTY";
		error["message"] = "Extrude requires selected faces.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	HashSet<int64_t> selected_faces;
	HashMap<int64_t, Vector3> vertex_normals;
	HashMap<int64_t, int> normal_counts;
	HashMap<int64_t, _ExtrudeFaceData> face_data;
	HashMap<int64_t, int> selected_edge_uses;
	for (int64_t face_id : face_ids) {
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		if (!face) {
			Dictionary error;
			error["code"] = "MODEL_ELEMENT_NOT_FOUND";
			error["message"] = vformat("Face %d does not exist.", face_id);
			Dictionary result;
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		selected_faces.insert(face_id);
		_ExtrudeFaceData data;
		data.vertices = r_mesh.get_face_vertices(face_id);
		data.material = face->material;
		data.smooth = face->smooth;
		face_data.insert(face_id, data);
		const Vector3 normal = _face_normal(r_mesh, face_id);
		for (int64_t vertex_id : data.vertices) {
			vertex_normals[vertex_id] += normal;
			normal_counts[vertex_id] += 1;
		}
		for (int64_t loop_id : face->loops) {
			selected_edge_uses[r_mesh.get_loop(loop_id)->edge] += 1;
		}
	}
	const bool has_offset = p_args.has("offset");
	const Vector3 explicit_offset = _vector3_arg(p_args.get("offset", Variant()));
	const double distance = p_args.get("distance", 1.0);
	if ((has_offset && !explicit_offset.is_finite()) || (!has_offset && !Math::is_finite(distance))) {
		Dictionary error;
		error["code"] = "MODEL_ARGUMENT_INVALID";
		error["message"] = "Extrude offset and distance must be finite.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	HashMap<int64_t, int64_t> duplicate_vertices;
	Vector<int64_t> created_vertices;
	for (const KeyValue<int64_t, Vector3> &entry : vertex_normals) {
		const Vector3 offset = has_offset ? explicit_offset : entry.value.normalized() * distance;
		const int64_t new_id = r_mesh.add_vertex(r_mesh.get_vertex(entry.key)->position + offset);
		duplicate_vertices.insert(entry.key, new_id);
		created_vertices.push_back(new_id);
	}
	Vector<Vector<int64_t>> side_faces;
	for (int64_t face_id : face_ids) {
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		for (int64_t loop_id : face->loops) {
			const SolersEditableMesh::Loop *loop = r_mesh.get_loop(loop_id);
			if (selected_edge_uses[loop->edge] == 1) {
				const int64_t a = loop->vertex;
				const int64_t b = r_mesh.get_loop(loop->next)->vertex;
				Vector<int64_t> side;
				side.push_back(a);
				side.push_back(b);
				side.push_back(duplicate_vertices[b]);
				side.push_back(duplicate_vertices[a]);
				side_faces.push_back(side);
			}
		}
	}
	for (int64_t face_id : face_ids) {
		r_mesh.remove_face(face_id, false);
	}
	Vector<int64_t> created_faces;
	for (int64_t face_id : face_ids) {
		const _ExtrudeFaceData &old_face = face_data[face_id];
		Vector<int64_t> top;
		for (int64_t old_vertex : old_face.vertices) {
			top.push_back(duplicate_vertices[old_vertex]);
		}
		created_faces.push_back(r_mesh.add_face(top, old_face.material, old_face.smooth));
	}
	for (const Vector<int64_t> &side : side_faces) {
		created_faces.push_back(r_mesh.add_face(side));
	}
	r_mesh.remove_orphan_elements();
	r_mesh.select_faces(created_faces);
	return _created_result(created_vertices, Vector<int64_t>(), created_faces);
}

static Dictionary _inset_faces(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> face_ids = _face_selection(r_mesh, p_args);
	const double factor = p_args.get("factor", 0.15);
	if (face_ids.is_empty() || !Math::is_finite(factor) || factor <= 0 || factor >= 1) {
		Dictionary error;
		error["code"] = face_ids.is_empty() ? "MODEL_SELECTION_EMPTY" : "MODEL_ARGUMENT_INVALID";
		error["message"] = face_ids.is_empty() ? "Inset requires selected faces." : "Inset factor must be between zero and one.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	Vector<int64_t> created_vertices;
	Vector<int64_t> created_faces;
	for (int64_t face_id : face_ids) {
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		if (!face) {
			continue;
		}
		const Vector<int64_t> outer = r_mesh.get_face_vertices(face_id);
		Vector3 center;
		for (int64_t vertex_id : outer) {
			center += r_mesh.get_vertex(vertex_id)->position;
		}
		center /= outer.size();
		const int material = face->material;
		const bool smooth = face->smooth;
		Vector<int64_t> inner;
		for (int64_t vertex_id : outer) {
			const Vector3 position = r_mesh.get_vertex(vertex_id)->position.lerp(center, factor);
			const int64_t inner_id = r_mesh.add_vertex(position);
			inner.push_back(inner_id);
			created_vertices.push_back(inner_id);
		}
		r_mesh.remove_face(face_id, false);
		const int64_t inner_face = r_mesh.add_face(inner, material, smooth);
		created_faces.push_back(inner_face);
		for (int i = 0; i < outer.size(); i++) {
			Vector<int64_t> ring_face;
			ring_face.push_back(outer[i]);
			ring_face.push_back(outer[(i + 1) % outer.size()]);
			ring_face.push_back(inner[(i + 1) % inner.size()]);
			ring_face.push_back(inner[i]);
			created_faces.push_back(r_mesh.add_face(ring_face, material, smooth));
		}
	}
	r_mesh.remove_orphan_elements();
	r_mesh.select_faces(created_faces);
	return _created_result(created_vertices, Vector<int64_t>(), created_faces);
}

static Dictionary _delete_elements(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const String domain = String(p_args.get("domain", "face")).to_lower();
	Vector<int64_t> ids = _id_array(p_args.get("ids", Variant()));
	if (ids.is_empty()) {
		ids = domain == "vertex" ? r_mesh.get_selected_vertices() : (domain == "edge" ? r_mesh.get_selected_edges() : r_mesh.get_selected_faces());
	}
	for (int64_t id : ids) {
		if (domain == "vertex") {
			r_mesh.remove_vertex(id, true);
		} else if (domain == "edge") {
			r_mesh.remove_edge(id, true);
		} else if (domain == "face") {
			r_mesh.remove_face(id, false);
		} else {
			Dictionary error;
			error["code"] = "MODEL_ARGUMENT_INVALID";
			error["message"] = "Delete domain must be vertex, edge, or face.";
			Dictionary result;
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
	}
	r_mesh.remove_orphan_elements();
	Dictionary data;
	data["deleted"] = _id_result(ids);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _flip_faces(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> face_ids = _face_selection(r_mesh, p_args);
	for (int64_t face_id : face_ids) {
		String error_message;
		if (!r_mesh.reverse_face(face_id, &error_message)) {
			Dictionary error;
			error["code"] = "MODEL_ELEMENT_NOT_FOUND";
			error["message"] = error_message;
			Dictionary result;
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
	}
	Dictionary data;
	data["modified_faces"] = _id_result(face_ids);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _set_face_smoothing(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> face_ids = _face_selection(r_mesh, p_args);
	r_mesh.set_faces_smooth(face_ids, p_args.get("enabled", true));
	Dictionary data;
	data["modified_faces"] = _id_result(face_ids);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _set_edge_flag(SolersEditableMesh &r_mesh, const Dictionary &p_args, bool p_seam) {
	const Vector<int64_t> edge_ids = _edge_selection(r_mesh, p_args);
	const bool enabled = p_args.get("enabled", true);
	if (p_seam) {
		r_mesh.set_edge_seam(edge_ids, enabled);
	} else {
		r_mesh.set_edge_sharp(edge_ids, enabled);
	}
	Dictionary data;
	data["modified_edges"] = _id_result(edge_ids);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _set_material(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> face_ids = _face_selection(r_mesh, p_args);
	int material = p_args.get("material_index", -1);
	const String path = p_args.get("material_path", String());
	if (!path.is_empty()) {
		if (!path.begins_with("res://")) {
			Dictionary error;
			error["code"] = "MODEL_ARGUMENT_INVALID";
			error["message"] = "Material paths must use res://.";
			Dictionary result;
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		material = r_mesh.add_material_path(path);
	}
	if (material < 0) {
		Dictionary error;
		error["code"] = "MODEL_ARGUMENT_INVALID";
		error["message"] = "Provide material_index or material_path.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	r_mesh.set_face_material(face_ids, material);
	Dictionary data;
	data["material_index"] = material;
	data["modified_faces"] = _id_result(face_ids);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _project_uv(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	Vector<int64_t> face_ids = _face_selection(r_mesh, p_args);
	if (face_ids.is_empty()) {
		face_ids = r_mesh.get_face_ids();
	}
	const String axis = String(p_args.get("axis", "auto")).to_lower();
	const Vector2 scale = _vector2_arg(p_args.get("scale", Variant()), Vector2(1, 1));
	const Vector2 offset = _vector2_arg(p_args.get("offset", Variant()));
	for (int64_t face_id : face_ids) {
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		const Vector3 normal = _face_normal(r_mesh, face_id).abs();
		String projection = axis;
		if (projection == "auto") {
			projection = normal.x >= normal.y && normal.x >= normal.z ? "x" : (normal.y >= normal.z ? "y" : "z");
		}
		for (int64_t loop_id : face->loops) {
			const Vector3 point = r_mesh.get_vertex(r_mesh.get_loop(loop_id)->vertex)->position;
			Vector2 uv;
			if (projection == "x") {
				uv = Vector2(point.z, point.y);
			} else if (projection == "y") {
				uv = Vector2(point.x, point.z);
			} else {
				uv = Vector2(point.x, point.y);
			}
			r_mesh.set_loop_uv(loop_id, uv * scale + offset);
		}
	}
	Dictionary data;
	data["modified_faces"] = _id_result(face_ids);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static bool _boundary_cycle(const SolersEditableMesh &p_mesh, const Vector<int64_t> &p_edge_ids, Vector<int64_t> &r_vertices) {
	HashMap<int64_t, Vector<int64_t>> adjacency;
	for (int64_t edge_id : p_edge_ids) {
		const SolersEditableMesh::Edge *edge = p_mesh.get_edge(edge_id);
		if (!edge || edge->loops.size() > 1) {
			return false;
		}
		adjacency[edge->vertex_a].push_back(edge->vertex_b);
		adjacency[edge->vertex_b].push_back(edge->vertex_a);
	}
	if (adjacency.is_empty()) {
		return false;
	}
	for (const KeyValue<int64_t, Vector<int64_t>> &entry : adjacency) {
		if (entry.value.size() != 2) {
			return false;
		}
	}
	const int64_t start = adjacency.begin()->key;
	int64_t previous = 0;
	int64_t current = start;
	for (uint32_t i = 0; i < adjacency.size(); i++) {
		r_vertices.push_back(current);
		const Vector<int64_t> &neighbors = adjacency[current];
		const int64_t next = neighbors[0] == previous ? neighbors[1] : neighbors[0];
		previous = current;
		current = next;
	}
	return current == start;
}

static Dictionary _fill_hole(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> edge_ids = _edge_selection(r_mesh, p_args);
	Vector<int64_t> boundary;
	if (!_boundary_cycle(r_mesh, edge_ids, boundary)) {
		Dictionary error;
		error["code"] = "MODEL_BOUNDARY_INVALID";
		error["message"] = "Fill Hole requires one closed loop of boundary edges.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	const int64_t face_id = r_mesh.add_face(boundary, p_args.get("material", 0));
	Vector<int64_t> faces;
	faces.push_back(face_id);
	r_mesh.select_faces(faces);
	return _created_result(Vector<int64_t>(), Vector<int64_t>(), faces);
}

static Dictionary _bridge_loops(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> first = _id_array(p_args.get("first", Variant()));
	const Vector<int64_t> second = _id_array(p_args.get("second", Variant()));
	if (first.size() < 2 || first.size() != second.size()) {
		Dictionary error;
		error["code"] = "MODEL_ARGUMENT_INVALID";
		error["message"] = "Bridge requires two ordered vertex loops of equal length.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	Vector<int64_t> created_faces;
	for (int i = 0; i < first.size(); i++) {
		Vector<int64_t> face;
		face.push_back(first[i]);
		face.push_back(first[(i + 1) % first.size()]);
		face.push_back(second[(i + 1) % second.size()]);
		face.push_back(second[i]);
		String error_message;
		const int64_t face_id = r_mesh.add_face(face, p_args.get("material", 0), false, &error_message);
		if (face_id == 0) {
			Dictionary error;
			error["code"] = "MODEL_BRIDGE_FAILED";
			error["message"] = error_message;
			Dictionary result;
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		created_faces.push_back(face_id);
	}
	r_mesh.select_faces(created_faces);
	return _created_result(Vector<int64_t>(), Vector<int64_t>(), created_faces);
}

static Dictionary _add_modifier(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const StringName type = StringName(String(p_args.get("type", String())).to_lower());
	static const HashSet<StringName> allowed = { SNAME("mirror"), SNAME("array"), SNAME("solidify"), SNAME("bevel"), SNAME("boolean") };
	if (!allowed.has(type)) {
		Dictionary error;
		error["code"] = "MODEL_MODIFIER_UNSUPPORTED";
		error["message"] = "Modifier type must be mirror, array, solidify, bevel, or boolean.";
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	const int64_t id = r_mesh.add_modifier(type, p_args.get("parameters", Dictionary()));
	Dictionary data;
	data["modifier_id"] = id;
	data["type"] = type;
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _remove_modifier(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const int64_t id = p_args.get("modifier_id", 0);
	if (!r_mesh.remove_modifier(id)) {
		Dictionary error;
		error["code"] = "MODEL_ELEMENT_NOT_FOUND";
		error["message"] = vformat("Modifier %d does not exist.", id);
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	Dictionary data;
	data["removed_modifier"] = id;
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

void SolersModelOperationRegistry::_add(const StringName &p_id, const String &p_description, const Dictionary &p_schema, bool p_changes_topology, bool p_modifier_operation, SolersModelOperationDefinition::Handler p_handler) {
	ERR_FAIL_COND_MSG(operation_index.has(p_id), vformat("Duplicate Solers modeling operation: %s", p_id));
	SolersModelOperationDefinition definition;
	definition.id = p_id;
	definition.description = p_description;
	definition.parameters_schema = p_schema;
	definition.changes_topology = p_changes_topology;
	definition.modifier_operation = p_modifier_operation;
	definition.handler = p_handler;
	operation_index.insert(p_id, operations.size());
	operations.push_back(definition);
}

Dictionary SolersModelOperationRegistry::_ok(const Dictionary &p_data) {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersModelOperationRegistry::_error(const String &p_code, const String &p_message, bool p_recoverable) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

SolersModelOperationRegistry::SolersModelOperationRegistry() {
	const Dictionary empty = _operation_schema(R"({"type":"object","properties":{},"additionalProperties":false})");
	_add(SNAME("create_box"), "Create a dimensionally exact box primitive.", _operation_schema(R"({"type":"object","properties":{"size":{"description":"Box dimensions in meters.","type":"object"},"center":{"description":"Primitive center.","type":"object"}},"additionalProperties":false})"), true, false, _create_box);
	_add(SNAME("create_plane"), "Create a planar quad primitive.", _operation_schema(R"({"type":"object","properties":{"size":{"description":"Plane width and depth.","type":"object"},"center":{"type":"object"}},"additionalProperties":false})"), true, false, _create_plane);
	_add(SNAME("create_cylinder"), "Create a capped cylinder with controlled radial topology.", _operation_schema(R"({"type":"object","properties":{"segments":{"type":"integer","minimum":3,"maximum":256},"radius":{"type":"number","exclusiveMinimum":0},"depth":{"type":"number","exclusiveMinimum":0},"center":{"type":"object"}},"additionalProperties":false})"), true, false, _create_cylinder);
	_add(SNAME("select"), "Select vertices, edges, faces, or boundary edges by persistent ID.", _operation_schema(R"({"type":"object","properties":{"domain":{"type":"string","enum":["vertex","edge","face"]},"ids":{"type":"array","items":{"type":"integer"}},"mode":{"type":"string","enum":["replace","add","subtract"]},"all":{"type":"boolean"},"boundary":{"type":"boolean"}},"required":["domain"],"additionalProperties":false})"), false, false, _select_elements);
	_add(SNAME("transform"), "Move, rotate, and scale selected topology with exact numeric values, axis constraints, and snapping.", _operation_schema(R"({"type":"object","properties":{"vertex_ids":{"type":"array","items":{"type":"integer"}},"translation":{"type":"object"},"rotation_degrees":{"type":"object"},"scale":{"type":"object"},"pivot":{"type":"object"},"axis":{"type":"string","enum":["all","x","y","z"]},"snap":{"type":"number","minimum":0}},"additionalProperties":false})"), false, false, _transform_selection);
	_add(SNAME("extrude_faces"), "Extrude a connected face region while preserving boundary adjacency.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"distance":{"type":"number"},"offset":{"type":"object"}},"additionalProperties":false})"), true, false, _extrude_faces);
	_add(SNAME("inset_faces"), "Inset selected polygon faces and create a clean surrounding ring.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"factor":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":1}},"additionalProperties":false})"), true, false, _inset_faces);
	_add(SNAME("delete"), "Delete selected topology with deterministic incident-face cleanup.", _operation_schema(R"({"type":"object","properties":{"domain":{"type":"string","enum":["vertex","edge","face"]},"ids":{"type":"array","items":{"type":"integer"}}},"required":["domain"],"additionalProperties":false})"), true, false, _delete_elements);
	_add(SNAME("flip_faces"), "Reverse selected face winding and normals.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}}},"additionalProperties":false})"), true, false, _flip_faces);
	_add(SNAME("set_smooth"), "Set smooth or flat shading on selected faces.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"enabled":{"type":"boolean"}},"additionalProperties":false})"), false, false, _set_face_smoothing);
	_add(SNAME("set_sharp"), "Mark selected edges as hard or smooth boundaries.", _operation_schema(R"({"type":"object","properties":{"edge_ids":{"type":"array","items":{"type":"integer"}},"enabled":{"type":"boolean"}},"additionalProperties":false})"), false, false, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _set_edge_flag(r_mesh, p_args, false); });
	_add(SNAME("set_uv_seam"), "Mark selected edges as UV seams.", _operation_schema(R"({"type":"object","properties":{"edge_ids":{"type":"array","items":{"type":"integer"}},"enabled":{"type":"boolean"}},"additionalProperties":false})"), false, false, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _set_edge_flag(r_mesh, p_args, true); });
	_add(SNAME("project_uv"), "Project UV coordinates on selected faces using a precise world axis or automatic face axis.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"axis":{"type":"string","enum":["auto","x","y","z"]},"scale":{"type":"object"},"offset":{"type":"object"}},"additionalProperties":false})"), false, false, _project_uv);
	_add(SNAME("set_material"), "Assign a material resource or existing material slot to selected faces.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"material_index":{"type":"integer","minimum":0},"material_path":{"type":"string"}},"additionalProperties":false})"), false, false, _set_material);
	_add(SNAME("fill_hole"), "Fill one closed boundary edge loop with an n-gon.", _operation_schema(R"({"type":"object","properties":{"edge_ids":{"type":"array","items":{"type":"integer"}},"material":{"type":"integer","minimum":0}},"additionalProperties":false})"), true, false, _fill_hole);
	_add(SNAME("bridge"), "Bridge two ordered vertex loops with quad topology.", _operation_schema(R"({"type":"object","properties":{"first":{"type":"array","items":{"type":"integer"},"minItems":2},"second":{"type":"array","items":{"type":"integer"},"minItems":2},"material":{"type":"integer","minimum":0}},"required":["first","second"],"additionalProperties":false})"), true, false, _bridge_loops);
	_add(SNAME("add_modifier"), "Add a non-destructive Mirror, Array, Solidify, Bevel, or Boolean modifier.", _operation_schema(R"({"type":"object","properties":{"type":{"type":"string","enum":["mirror","array","solidify","bevel","boolean"]},"parameters":{"type":"object"}},"required":["type"],"additionalProperties":false})"), false, true, _add_modifier);
	_add(SNAME("remove_modifier"), "Remove a modifier by persistent ID.", _operation_schema(R"({"type":"object","properties":{"modifier_id":{"type":"integer"}},"required":["modifier_id"],"additionalProperties":false})"), false, true, _remove_modifier);
}

SolersModelOperationRegistry *SolersModelOperationRegistry::get_singleton() {
	static SolersModelOperationRegistry singleton;
	return &singleton;
}

const SolersModelOperationDefinition *SolersModelOperationRegistry::get_operation(const StringName &p_id) const {
	const int *index = operation_index.getptr(p_id);
	return index ? &operations[*index] : nullptr;
}

Dictionary SolersModelOperationRegistry::execute(SolersEditableMesh &r_mesh, const StringName &p_id, const Dictionary &p_parameters) const {
	const SolersModelOperationDefinition *operation = get_operation(p_id);
	if (!operation) {
		return _error("MODEL_OPERATION_NOT_FOUND", vformat("Unknown modeling operation: %s", p_id), true);
	}
	Dictionary result = operation->handler(r_mesh, p_parameters);
	if (!(bool)result.get("ok", false)) {
		return result;
	}
	String validation_error;
	if (r_mesh.validate(&validation_error) != OK) {
		return _error("MODEL_TOPOLOGY_INVALID", validation_error, false);
	}
	return result;
}
