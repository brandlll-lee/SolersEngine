/**************************************************************************/
/*  solers_model_modifier.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_model_modifier.h"

#include "core/math/geometry_2d.h"
#include "modules/solers_modeling/core/solers_model_source.h"

#include <manifold/manifold.h>

struct _ModelCornerKey {
	int64_t face = 0;
	int64_t vertex = 0;

	static uint32_t hash(const _ModelCornerKey &p_key) {
		return hash_one_uint64((uint64_t)p_key.face) ^ hash_murmur3_one_64((uint64_t)p_key.vertex);
	}

	bool operator==(const _ModelCornerKey &p_other) const {
		return face == p_other.face && vertex == p_other.vertex;
	}
};

struct _ModelAngleVertex {
	double angle = 0.0;
	int64_t vertex = 0;

	bool operator<(const _ModelAngleVertex &p_other) const {
		return angle < p_other.angle;
	}
};

static void _modifier_error(String *r_error, const String &p_message) {
	if (r_error) {
		*r_error = p_message;
	}
}

static Vector3 _modifier_vector3(const Variant &p_value, const Vector3 &p_default = Vector3()) {
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

static Vector3 _modifier_face_normal(const SolersEditableMesh &p_mesh, int64_t p_face_id) {
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

static void _copy_materials(const SolersEditableMesh &p_source, SolersEditableMesh &r_target) {
	for (const String &path : p_source.get_material_paths()) {
		r_target.add_material_path(path);
	}
}

static int64_t _copy_face(const SolersEditableMesh &p_source, int64_t p_face_id, const HashMap<int64_t, int64_t> &p_vertex_map, SolersEditableMesh &r_target, bool p_reverse = false) {
	const SolersEditableMesh::Face *source_face = p_source.get_face(p_face_id);
	Vector<int64_t> mapped;
	Vector<Vector2> uvs;
	for (int64_t loop_id : source_face->loops) {
		const SolersEditableMesh::Loop *loop = p_source.get_loop(loop_id);
		mapped.push_back(p_vertex_map[loop->vertex]);
		uvs.push_back(loop->uv);
	}
	if (p_reverse) {
		mapped.reverse();
		uvs.reverse();
	}
	const int64_t face_id = r_target.add_face(mapped, source_face->material, source_face->smooth);
	const SolersEditableMesh::Face *target_face = r_target.get_face(face_id);
	for (int i = 0; i < target_face->loops.size(); i++) {
		r_target.set_loop_uv(target_face->loops[i], uvs[i]);
	}
	return face_id;
}

static Error _apply_mirror(SolersEditableMesh &r_mesh, const Dictionary &p_parameters, String *r_error) {
	const String axis_name = String(p_parameters.get("axis", "x")).to_lower();
	const int axis = axis_name == "x" ? 0 : (axis_name == "y" ? 1 : (axis_name == "z" ? 2 : -1));
	if (axis < 0) {
		_modifier_error(r_error, "Mirror axis must be x, y, or z.");
		return ERR_INVALID_PARAMETER;
	}
	const Vector3 origin = _modifier_vector3(p_parameters.get("origin", Variant()));
	const bool merge = p_parameters.get("merge", true);
	const double merge_distance = p_parameters.get("merge_distance", 0.0001);
	if (!origin.is_finite() || !Math::is_finite(merge_distance) || merge_distance < 0) {
		_modifier_error(r_error, "Mirror origin and merge distance must be finite.");
		return ERR_INVALID_PARAMETER;
	}
	const Vector<int64_t> source_vertices = r_mesh.get_vertex_ids();
	const Vector<int64_t> source_faces = r_mesh.get_face_ids();
	HashMap<int64_t, int64_t> mirrored;
	for (int64_t vertex_id : source_vertices) {
		Vector3 position = r_mesh.get_vertex(vertex_id)->position;
		const double plane_distance = position[axis] - origin[axis];
		if (merge && Math::abs(plane_distance) <= merge_distance) {
			mirrored.insert(vertex_id, vertex_id);
		} else {
			position[axis] = origin[axis] - plane_distance;
			mirrored.insert(vertex_id, r_mesh.add_vertex(position));
		}
	}
	for (int64_t face_id : source_faces) {
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		Vector<int64_t> vertices;
		Vector<Vector2> uvs;
		for (int i = face->loops.size() - 1; i >= 0; i--) {
			const SolersEditableMesh::Loop *loop = r_mesh.get_loop(face->loops[i]);
			vertices.push_back(mirrored[loop->vertex]);
			uvs.push_back(loop->uv);
		}
		HashSet<int64_t> unique;
		for (int64_t vertex_id : vertices) {
			unique.insert(vertex_id);
		}
		if (unique.size() < 3) {
			continue;
		}
		String face_error;
		const int64_t mirrored_face = r_mesh.add_face(vertices, face->material, face->smooth, &face_error);
		if (mirrored_face == 0) {
			_modifier_error(r_error, face_error);
			return ERR_INVALID_DATA;
		}
		const SolersEditableMesh::Face *new_face = r_mesh.get_face(mirrored_face);
		for (int i = 0; i < new_face->loops.size(); i++) {
			r_mesh.set_loop_uv(new_face->loops[i], uvs[i]);
		}
	}
	return OK;
}

static Error _apply_array(SolersEditableMesh &r_mesh, const Dictionary &p_parameters, String *r_error) {
	const int count = p_parameters.get("count", 2);
	const Vector3 offset = _modifier_vector3(p_parameters.get("offset", Variant()), Vector3(1, 0, 0));
	if (count < 1 || count > 10000 || !offset.is_finite()) {
		_modifier_error(r_error, "Array count must be 1..10000 and offset must be finite.");
		return ERR_INVALID_PARAMETER;
	}
	const Vector<int64_t> source_vertices = r_mesh.get_vertex_ids();
	const Vector<int64_t> source_faces = r_mesh.get_face_ids();
	for (int copy = 1; copy < count; copy++) {
		HashMap<int64_t, int64_t> vertex_map;
		for (int64_t vertex_id : source_vertices) {
			vertex_map.insert(vertex_id, r_mesh.add_vertex(r_mesh.get_vertex(vertex_id)->position + offset * copy));
		}
		for (int64_t face_id : source_faces) {
			_copy_face(r_mesh, face_id, vertex_map, r_mesh);
		}
	}
	return OK;
}

static Error _apply_solidify(SolersEditableMesh &r_mesh, const Dictionary &p_parameters, String *r_error) {
	const double thickness = p_parameters.get("thickness", 0.1);
	if (!Math::is_finite(thickness) || Math::is_zero_approx(thickness)) {
		_modifier_error(r_error, "Solidify thickness must be finite and non-zero.");
		return ERR_INVALID_PARAMETER;
	}
	const Vector<int64_t> source_vertices = r_mesh.get_vertex_ids();
	const Vector<int64_t> source_faces = r_mesh.get_face_ids();
	HashMap<int64_t, Vector3> normal_sums;
	for (int64_t face_id : source_faces) {
		const Vector3 normal = _modifier_face_normal(r_mesh, face_id);
		for (int64_t vertex_id : r_mesh.get_face_vertices(face_id)) {
			normal_sums[vertex_id] += normal;
		}
	}
	HashMap<int64_t, int64_t> outer;
	for (int64_t vertex_id : source_vertices) {
		outer.insert(vertex_id, r_mesh.add_vertex(r_mesh.get_vertex(vertex_id)->position + normal_sums[vertex_id].normalized() * thickness));
	}
	for (int64_t face_id : source_faces) {
		_copy_face(r_mesh, face_id, outer, r_mesh);
		r_mesh.reverse_face(face_id);
	}
	const Vector<int64_t> boundary_edges = r_mesh.get_boundary_edges();
	for (int64_t edge_id : boundary_edges) {
		const SolersEditableMesh::Edge *edge = r_mesh.get_edge(edge_id);
		if (!outer.has(edge->vertex_a) || !outer.has(edge->vertex_b)) {
			continue;
		}
		Vector<int64_t> side;
		side.push_back(edge->vertex_a);
		side.push_back(edge->vertex_b);
		side.push_back(outer[edge->vertex_b]);
		side.push_back(outer[edge->vertex_a]);
		r_mesh.add_face(side);
	}
	return OK;
}

static Error _apply_bevel(SolersEditableMesh &r_mesh, const Dictionary &p_parameters, String *r_error) {
	const double width = p_parameters.get("width", 0.05);
	const int segments = CLAMP((int)p_parameters.get("segments", 1), 1, 12);
	if (!Math::is_finite(width) || width <= 0) {
		_modifier_error(r_error, "Bevel width must be finite and positive.");
		return ERR_INVALID_PARAMETER;
	}
	for (int64_t edge_id : r_mesh.get_edge_ids()) {
		if (r_mesh.get_edge(edge_id)->loops.size() != 2) {
			_modifier_error(r_error, "The Bevel modifier currently requires a closed manifold mesh.");
			return ERR_INVALID_DATA;
		}
	}

	SolersEditableMesh beveled;
	_copy_materials(r_mesh, beveled);
	HashMap<_ModelCornerKey, int64_t, _ModelCornerKey> corners;
	for (int64_t face_id : r_mesh.get_face_ids()) {
		const Vector<int64_t> face_vertices = r_mesh.get_face_vertices(face_id);
		Vector<int64_t> inset_face;
		for (int i = 0; i < face_vertices.size(); i++) {
			const Vector3 previous = r_mesh.get_vertex(face_vertices[(i + face_vertices.size() - 1) % face_vertices.size()])->position;
			const Vector3 current = r_mesh.get_vertex(face_vertices[i])->position;
			const Vector3 next = r_mesh.get_vertex(face_vertices[(i + 1) % face_vertices.size()])->position;
			const Vector3 to_previous = (previous - current).normalized();
			const Vector3 to_next = (next - current).normalized();
			const double sin_half = Math::sqrt(MAX(0.0, (1.0 - to_previous.dot(to_next)) * 0.5));
			const double max_distance = MIN(current.distance_to(previous), current.distance_to(next)) * 0.45;
			const double distance = MIN(width / MAX(0.000001, sin_half), max_distance);
			const Vector3 bisector = (to_previous + to_next).normalized();
			const int64_t corner = beveled.add_vertex(current + bisector * distance);
			corners.insert({ face_id, face_vertices[i] }, corner);
			inset_face.push_back(corner);
		}
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		beveled.add_face(inset_face, face->material, face->smooth);
	}

	HashMap<int64_t, Vector<int64_t>> cap_vertices;
	for (int64_t edge_id : r_mesh.get_edge_ids()) {
		const SolersEditableMesh::Edge *edge = r_mesh.get_edge(edge_id);
		const int64_t face_a = r_mesh.get_loop(edge->loops[0])->face;
		const int64_t face_b = r_mesh.get_loop(edge->loops[1])->face;
		Vector<int64_t> curve_a;
		Vector<int64_t> curve_b;
		const int64_t endpoints[2] = { edge->vertex_a, edge->vertex_b };
		for (int endpoint_index = 0; endpoint_index < 2; endpoint_index++) {
			const int64_t endpoint = endpoints[endpoint_index];
			const Vector3 start = beveled.get_vertex(corners[{ face_a, endpoint }])->position;
			const Vector3 control = r_mesh.get_vertex(endpoint)->position;
			const Vector3 end = beveled.get_vertex(corners[{ face_b, endpoint }])->position;
			Vector<int64_t> *curve = endpoint_index == 0 ? &curve_a : &curve_b;
			curve->push_back(corners[{ face_a, endpoint }]);
			for (int segment = 1; segment < segments; segment++) {
				const double t = (double)segment / segments;
				const Vector3 point = start * ((1.0 - t) * (1.0 - t)) + control * (2.0 * (1.0 - t) * t) + end * (t * t);
				curve->push_back(beveled.add_vertex(point));
			}
			curve->push_back(corners[{ face_b, endpoint }]);
			for (int64_t vertex_id : *curve) {
				cap_vertices[endpoint].push_back(vertex_id);
			}
		}
		for (int segment = 0; segment < segments; segment++) {
			Vector<int64_t> strip;
			strip.push_back(curve_a[segment]);
			strip.push_back(curve_b[segment]);
			strip.push_back(curve_b[segment + 1]);
			strip.push_back(curve_a[segment + 1]);
			beveled.add_face(strip);
		}
	}

	for (int64_t vertex_id : r_mesh.get_vertex_ids()) {
		HashSet<int64_t> unique;
		for (int64_t cap_vertex : cap_vertices[vertex_id]) {
			unique.insert(cap_vertex);
		}
		if (unique.size() < 3) {
			continue;
		}
		Vector3 normal;
		for (int64_t face_id : r_mesh.get_vertex_faces(vertex_id)) {
			normal += _modifier_face_normal(r_mesh, face_id);
		}
		normal.normalize();
		Vector3 tangent = normal.cross(Vector3(0, 1, 0));
		if (tangent.is_zero_approx()) {
			tangent = normal.cross(Vector3(1, 0, 0));
		}
		tangent.normalize();
		const Vector3 bitangent = normal.cross(tangent).normalized();
		const Vector3 center = r_mesh.get_vertex(vertex_id)->position;
		Vector<_ModelAngleVertex> angular;
		for (int64_t cap_vertex : unique) {
			const Vector3 direction = beveled.get_vertex(cap_vertex)->position - center;
			angular.push_back({ Math::atan2(direction.dot(bitangent), direction.dot(tangent)), cap_vertex });
		}
		angular.sort();
		Vector<int64_t> cap;
		for (const _ModelAngleVertex &entry : angular) {
			cap.push_back(entry.vertex);
		}
		beveled.add_face(cap);
	}

	String validation_error;
	if (beveled.validate(&validation_error) != OK) {
		_modifier_error(r_error, validation_error);
		return ERR_INVALID_DATA;
	}
	r_mesh = beveled;
	return OK;
}

static Error _editable_to_manifold(const SolersEditableMesh &p_mesh, manifold::Manifold &r_manifold, String *r_error) {
	manifold::MeshGL64 mesh;
	mesh.numProp = 3;
	HashMap<int64_t, uint64_t> vertex_index;
	for (int64_t vertex_id : p_mesh.get_vertex_ids()) {
		const Vector3 position = p_mesh.get_vertex(vertex_id)->position;
		vertex_index.insert(vertex_id, mesh.vertProperties.size() / 3);
		mesh.vertProperties.push_back(position.x);
		mesh.vertProperties.push_back(position.y);
		mesh.vertProperties.push_back(position.z);
	}
	for (int64_t face_id : p_mesh.get_face_ids()) {
		const Vector<int64_t> face_vertices = p_mesh.get_face_vertices(face_id);
		const Vector3 normal = _modifier_face_normal(p_mesh, face_id).abs();
		PackedVector2Array projected;
		for (int64_t vertex_id : face_vertices) {
			const Vector3 point = p_mesh.get_vertex(vertex_id)->position;
			projected.push_back(normal.x >= normal.y && normal.x >= normal.z ? Vector2(point.y, point.z) : (normal.y >= normal.z ? Vector2(point.x, point.z) : Vector2(point.x, point.y)));
		}
		const PackedInt32Array triangles = Geometry2D::triangulate_polygon(projected);
		if (triangles.size() < 3) {
			_modifier_error(r_error, vformat("Face %d could not be triangulated for Boolean.", face_id));
			return ERR_INVALID_DATA;
		}
		for (int index : triangles) {
			mesh.triVerts.push_back(vertex_index[face_vertices[index]]);
		}
	}
	mesh.Merge();
	r_manifold = manifold::Manifold(mesh);
	if (r_manifold.Status() != manifold::Manifold::Error::NoError) {
		_modifier_error(r_error, "Boolean input must be a closed, consistently oriented manifold mesh.");
		return ERR_INVALID_DATA;
	}
	return OK;
}

static Error _manifold_to_editable(const manifold::Manifold &p_manifold, SolersEditableMesh &r_mesh, String *r_error) {
	if (p_manifold.Status() != manifold::Manifold::Error::NoError) {
		_modifier_error(r_error, "Manifold Boolean failed.");
		return ERR_INVALID_DATA;
	}
	const manifold::MeshGL64 output = p_manifold.GetMeshGL64();
	if (output.numProp < 3 || output.vertProperties.size() % output.numProp != 0 || output.triVerts.size() % 3 != 0) {
		_modifier_error(r_error, "Manifold Boolean returned malformed geometry.");
		return ERR_INVALID_DATA;
	}
	r_mesh.clear();
	Vector<int64_t> vertices;
	for (size_t i = 0; i < output.vertProperties.size(); i += output.numProp) {
		vertices.push_back(r_mesh.add_vertex(Vector3(output.vertProperties[i], output.vertProperties[i + 1], output.vertProperties[i + 2])));
	}
	for (size_t i = 0; i < output.triVerts.size(); i += 3) {
		Vector<int64_t> face;
		face.push_back(vertices[output.triVerts[i]]);
		face.push_back(vertices[output.triVerts[i + 1]]);
		face.push_back(vertices[output.triVerts[i + 2]]);
		r_mesh.add_face(face);
	}
	return r_mesh.validate(r_error);
}

static Error _apply_boolean(SolersEditableMesh &r_mesh, const Dictionary &p_parameters, String *r_error) {
	const String operand_path = p_parameters.get("operand", String());
	if (!operand_path.begins_with("res://") || operand_path.get_extension().to_lower() != "smodel") {
		_modifier_error(r_error, "Boolean operand must be a res:// .smodel source path.");
		return ERR_INVALID_PARAMETER;
	}
	SolersEditableMesh operand;
	if (SolersModelSource::load(operand_path, operand, r_error) != OK) {
		return ERR_CANT_OPEN;
	}
	operand.get_modifiers().clear();
	manifold::Manifold first;
	manifold::Manifold second;
	Error error = _editable_to_manifold(r_mesh, first, r_error);
	if (error != OK) {
		return error;
	}
	error = _editable_to_manifold(operand, second, r_error);
	if (error != OK) {
		return error;
	}
	const Vector3 translation = _modifier_vector3(p_parameters.get("translation", Variant()));
	const Vector3 rotation = _modifier_vector3(p_parameters.get("rotation_degrees", Variant()));
	const Vector3 scale = _modifier_vector3(p_parameters.get("scale", Variant()), Vector3(1, 1, 1));
	second = second.Scale({ scale.x, scale.y, scale.z }).Rotate(rotation.x, rotation.y, rotation.z).Translate({ translation.x, translation.y, translation.z });
	const String operation = String(p_parameters.get("operation", "union")).to_lower();
	const manifold::OpType type = operation == "subtract" ? manifold::OpType::Subtract : (operation == "intersect" ? manifold::OpType::Intersect : manifold::OpType::Add);
	return _manifold_to_editable(first.Boolean(second, type), r_mesh, r_error);
}

Error SolersModelModifierEvaluator::evaluate(const SolersEditableMesh &p_source, SolersEditableMesh &r_result, String *r_error) {
	r_result = p_source;
	const Vector<SolersEditableMesh::Modifier> modifiers = p_source.get_modifiers();
	r_result.get_modifiers().clear();
	for (const SolersEditableMesh::Modifier &modifier : modifiers) {
		if (!modifier.enabled) {
			continue;
		}
		Error error = ERR_INVALID_PARAMETER;
		if (modifier.type == SNAME("mirror")) {
			error = _apply_mirror(r_result, modifier.parameters, r_error);
		} else if (modifier.type == SNAME("array")) {
			error = _apply_array(r_result, modifier.parameters, r_error);
		} else if (modifier.type == SNAME("solidify")) {
			error = _apply_solidify(r_result, modifier.parameters, r_error);
		} else if (modifier.type == SNAME("bevel")) {
			error = _apply_bevel(r_result, modifier.parameters, r_error);
		} else if (modifier.type == SNAME("boolean")) {
			error = _apply_boolean(r_result, modifier.parameters, r_error);
		} else {
			_modifier_error(r_error, vformat("Unknown modifier type: %s", modifier.type));
		}
		if (error != OK) {
			return error;
		}
		String validation_error;
		if (r_result.validate(&validation_error) != OK) {
			_modifier_error(r_error, validation_error);
			return ERR_INVALID_DATA;
		}
	}
	r_result.get_build_settings() = p_source.get_build_settings().duplicate(true);
	r_result.set_revision(p_source.get_revision());
	return OK;
}
