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
#include "modules/solers_modeling/core/solers_model_modifier.h"
#include "modules/solers_modeling/core/solers_model_uv.h"

static void _expand_vector_schemas(Dictionary &r_schema) {
	const String type = r_schema.get("type", String());
	if (type == "object") {
		if (!r_schema.has("properties")) {
			Dictionary component;
			component["type"] = "number";
			Dictionary properties;
			properties["x"] = component;
			properties["y"] = component;
			properties["z"] = component;
			r_schema["properties"] = properties;
			r_schema["additionalProperties"] = false;
			return;
		}
		Dictionary properties = r_schema["properties"];
		for (const Variant &key : properties.keys()) {
			Dictionary property = properties[key];
			_expand_vector_schemas(property);
			properties[key] = property;
		}
		r_schema["properties"] = properties;
	} else if (type == "array" && r_schema.has("items")) {
		Dictionary items = r_schema["items"];
		_expand_vector_schemas(items);
		r_schema["items"] = items;
	}
}

static Dictionary _operation_schema(const char *p_json) {
	const Variant parsed = JSON::parse_string(p_json);
	ERR_FAIL_COND_V_MSG(parsed.get_type() != Variant::DICTIONARY, Dictionary(), "Invalid Solers modeling operation schema.");
	Dictionary schema = parsed;
	_expand_vector_schemas(schema);
	return schema;
}

static bool _schema_type_matches(const Variant &p_value, const String &p_type) {
	if (p_type == "object") {
		return p_value.get_type() == Variant::DICTIONARY || p_value.get_type() == Variant::VECTOR2 || p_value.get_type() == Variant::VECTOR3;
	}
	if (p_type == "array") {
		return p_value.get_type() == Variant::ARRAY;
	}
	if (p_type == "string") {
		return p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME;
	}
	if (p_type == "integer") {
		return p_value.get_type() == Variant::INT || (p_value.get_type() == Variant::FLOAT && Math::is_equal_approx((double)p_value, Math::round((double)p_value)));
	}
	if (p_type == "number") {
		return p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT;
	}
	if (p_type == "boolean") {
		return p_value.get_type() == Variant::BOOL;
	}
	return true;
}

static bool _validate_schema_value(const Variant &p_value, const Dictionary &p_schema, const String &p_path, String &r_error) {
	const String type = p_schema.get("type", String());
	if (!type.is_empty() && !_schema_type_matches(p_value, type)) {
		r_error = vformat("%s must be %s.", p_path, type);
		return false;
	}
	const Array allowed = p_schema.get("enum", Array());
	if (!allowed.is_empty() && !allowed.has(p_value)) {
		r_error = vformat("%s must be one of %s.", p_path, JSON::stringify(allowed));
		return false;
	}
	if (type == "number" || type == "integer") {
		const double value = p_value;
		if (!Math::is_finite(value)) {
			r_error = vformat("%s must be finite.", p_path);
			return false;
		}
		if (p_schema.has("minimum") && value < (double)p_schema["minimum"]) {
			r_error = vformat("%s must be at least %s.", p_path, p_schema["minimum"]);
			return false;
		}
		if (p_schema.has("maximum") && value > (double)p_schema["maximum"]) {
			r_error = vformat("%s must be at most %s.", p_path, p_schema["maximum"]);
			return false;
		}
		if (p_schema.has("exclusiveMinimum") && value <= (double)p_schema["exclusiveMinimum"]) {
			r_error = vformat("%s must be greater than %s.", p_path, p_schema["exclusiveMinimum"]);
			return false;
		}
		if (p_schema.has("exclusiveMaximum") && value >= (double)p_schema["exclusiveMaximum"]) {
			r_error = vformat("%s must be less than %s.", p_path, p_schema["exclusiveMaximum"]);
			return false;
		}
	}
	if (type == "array") {
		const Array values = p_value;
		if (p_schema.has("minItems") && values.size() < (int)p_schema["minItems"]) {
			r_error = vformat("%s requires at least %d item(s).", p_path, (int)p_schema["minItems"]);
			return false;
		}
		if (p_schema.has("maxItems") && values.size() > (int)p_schema["maxItems"]) {
			r_error = vformat("%s allows at most %d item(s).", p_path, (int)p_schema["maxItems"]);
			return false;
		}
		const Dictionary item_schema = p_schema.get("items", Dictionary());
		for (int i = 0; i < values.size() && !item_schema.is_empty(); i++) {
			if (!_validate_schema_value(values[i], item_schema, vformat("%s[%d]", p_path, i), r_error)) {
				return false;
			}
		}
	}
	if (type == "object" && p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary value = p_value;
		const Dictionary properties = p_schema.get("properties", Dictionary());
		const Array required = p_schema.get("required", Array());
		for (const Variant &key : required) {
			if (!value.has(key)) {
				r_error = vformat("%s.%s is required.", p_path, key);
				return false;
			}
		}
		const bool allow_additional = p_schema.get("additionalProperties", true);
		const Variant *key = nullptr;
		while ((key = value.next(key))) {
			if (!properties.has(*key)) {
				if (!allow_additional) {
					r_error = vformat("%s.%s is not a supported parameter.", p_path, *key);
					return false;
				}
				continue;
			}
			if (!_validate_schema_value(value[*key], properties[*key], vformat("%s.%s", p_path, *key), r_error)) {
				return false;
			}
		}
	}
	return true;
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

static Dictionary _operation_ok(const Dictionary &p_data = Dictionary()) {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

static Dictionary _operation_error(const String &p_code, const String &p_message, bool p_recoverable = true) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
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

static Vector<int64_t> _face_edges(const SolersEditableMesh &p_mesh, const Vector<int64_t> &p_faces) {
	HashSet<int64_t> unique;
	for (int64_t face_id : p_faces) {
		const SolersEditableMesh::Face *face = p_mesh.get_face(face_id);
		if (!face) {
			continue;
		}
		for (int64_t loop_id : face->loops) {
			const SolersEditableMesh::Loop *loop = p_mesh.get_loop(loop_id);
			if (loop) {
				unique.insert(loop->edge);
			}
		}
	}
	Vector<int64_t> result;
	for (int64_t edge_id : unique) {
		result.push_back(edge_id);
	}
	result.sort();
	return result;
}

static Dictionary _add_box(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
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
	return _created_result(vertices, _face_edges(r_mesh, faces), faces);
}

static Dictionary _add_plane(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
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
	return _created_result(vertices, _face_edges(r_mesh, faces), faces);
}

static Dictionary _add_cylinder(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
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
	return _created_result(created_vertices, _face_edges(r_mesh, created_faces), created_faces);
}

static Vector<int64_t> _domain_selection(const SolersEditableMesh &p_mesh, const String &p_domain) {
	return p_domain == "vertex" ? p_mesh.get_selected_vertices() : (p_domain == "edge" ? p_mesh.get_selected_edges() : p_mesh.get_selected_faces());
}

static Vector<int64_t> _domain_ids(const SolersEditableMesh &p_mesh, const String &p_domain) {
	return p_domain == "vertex" ? p_mesh.get_vertex_ids() : (p_domain == "edge" ? p_mesh.get_edge_ids() : p_mesh.get_face_ids());
}

static Vector<int64_t> _connected_elements(const SolersEditableMesh &p_mesh, const String &p_domain, const Vector<int64_t> &p_seeds) {
	HashSet<int64_t> visited;
	Vector<int64_t> pending = p_seeds;
	for (int64_t seed : p_seeds) {
		visited.insert(seed);
	}
	while (!pending.is_empty()) {
		const int64_t id = pending[pending.size() - 1];
		pending.remove_at(pending.size() - 1);
		Vector<int64_t> neighbors;
		if (p_domain == "vertex") {
			for (int64_t edge_id : p_mesh.get_vertex_edges(id)) {
				const SolersEditableMesh::Edge *edge = p_mesh.get_edge(edge_id);
				neighbors.push_back(edge->vertex_a == id ? edge->vertex_b : edge->vertex_a);
			}
		} else if (p_domain == "edge") {
			const SolersEditableMesh::Edge *edge = p_mesh.get_edge(id);
			if (edge) {
				neighbors.append_array(p_mesh.get_vertex_edges(edge->vertex_a));
				neighbors.append_array(p_mesh.get_vertex_edges(edge->vertex_b));
			}
		} else {
			const SolersEditableMesh::Face *face = p_mesh.get_face(id);
			if (face) {
				for (int64_t loop_id : face->loops) {
					for (int64_t radial_loop : p_mesh.get_edge(p_mesh.get_loop(loop_id)->edge)->loops) {
						neighbors.push_back(p_mesh.get_loop(radial_loop)->face);
					}
				}
			}
		}
		for (int64_t neighbor : neighbors) {
			if (!visited.has(neighbor)) {
				visited.insert(neighbor);
				pending.push_back(neighbor);
			}
		}
	}
	Vector<int64_t> result;
	for (int64_t id : visited) {
		result.push_back(id);
	}
	result.sort();
	return result;
}

static bool _quad_edge_ring(const SolersEditableMesh &p_mesh, const Vector<int64_t> &p_seeds, Vector<int64_t> &r_edges, bool p_require_quads, String *r_error = nullptr) {
	if (p_seeds.size() != 1 || !p_mesh.get_edge(p_seeds[0])) {
		if (r_error) {
			*r_error = "An edge loop requires exactly one valid seed edge.";
		}
		return false;
	}
	HashSet<int64_t> visited;
	Vector<int64_t> pending = p_seeds;
	visited.insert(p_seeds[0]);
	while (!pending.is_empty()) {
		const int64_t edge_id = pending[pending.size() - 1];
		pending.remove_at(pending.size() - 1);
		for (int64_t loop_id : p_mesh.get_edge(edge_id)->loops) {
			const SolersEditableMesh::Loop *loop = p_mesh.get_loop(loop_id);
			const SolersEditableMesh::Face *face = p_mesh.get_face(loop->face);
			if (face->loops.size() != 4) {
				if (p_require_quads) {
					if (r_error) {
						*r_error = "The edge ring crosses a non-quad face.";
					}
					return false;
				}
				continue;
			}
			const int loop_index = face->loops.find(loop_id);
			const int64_t opposite = p_mesh.get_loop(face->loops[(loop_index + 2) % 4])->edge;
			if (!visited.has(opposite)) {
				visited.insert(opposite);
				pending.push_back(opposite);
			}
		}
	}
	for (int64_t edge_id : visited) {
		r_edges.push_back(edge_id);
	}
	r_edges.sort();
	return true;
}

static Vector3 _element_center(const SolersEditableMesh &p_mesh, const String &p_domain, int64_t p_id) {
	if (p_domain == "vertex") {
		return p_mesh.get_vertex(p_id)->position;
	}
	if (p_domain == "edge") {
		const SolersEditableMesh::Edge *edge = p_mesh.get_edge(p_id);
		return (p_mesh.get_vertex(edge->vertex_a)->position + p_mesh.get_vertex(edge->vertex_b)->position) * 0.5;
	}
	Vector3 center;
	const Vector<int64_t> vertices = p_mesh.get_face_vertices(p_id);
	for (int64_t vertex_id : vertices) {
		center += p_mesh.get_vertex(vertex_id)->position;
	}
	return vertices.is_empty() ? center : center / vertices.size();
}

static bool _spatial_filter(const SolersEditableMesh &p_mesh, const String &p_domain, const Dictionary &p_spatial, const Vector<int64_t> &p_candidates, Vector<int64_t> &r_matches, String &r_error) {
	const bool sphere = p_spatial.has("center") || p_spatial.has("radius");
	const Vector3 center = _vector3_arg(p_spatial.get("center", Variant()));
	const double radius = p_spatial.get("radius", -1.0);
	const Vector3 minimum = _vector3_arg(p_spatial.get("min", Variant()), Vector3(-Math::INF, -Math::INF, -Math::INF));
	const Vector3 maximum = _vector3_arg(p_spatial.get("max", Variant()), Vector3(Math::INF, Math::INF, Math::INF));
	if ((sphere && (!center.is_finite() || !Math::is_finite(radius) || radius < 0)) || (!sphere && (minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z))) {
		r_error = "Spatial selection requires a finite center and non-negative radius, or valid min/max bounds.";
		return false;
	}
	for (int64_t id : p_candidates) {
		const Vector3 point = _element_center(p_mesh, p_domain, id);
		const bool matches = sphere ? point.distance_squared_to(center) <= radius * radius :
				(point.x >= minimum.x && point.y >= minimum.y && point.z >= minimum.z && point.x <= maximum.x && point.y <= maximum.y && point.z <= maximum.z);
		if (matches) {
			r_matches.push_back(id);
		}
	}
	return true;
}

static Dictionary _select_elements(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const String domain = String(p_args.get("domain", "face")).to_lower();
	const String mode = String(p_args.get("mode", "replace")).to_lower();
	if ((domain != "vertex" && domain != "edge" && domain != "face") || (mode != "replace" && mode != "add" && mode != "subtract")) {
		return _operation_error("MODEL_ARGUMENT_INVALID", "Selection domain must be vertex, edge, or face and mode must be replace, add, or subtract.");
	}
	const bool replace = mode == "replace";
	Vector<int64_t> ids = _id_array(p_args.get("ids", Array()));
	if ((bool)p_args.get("all", false)) {
		ids = _domain_ids(r_mesh, domain);
	}
	if ((bool)p_args.get("boundary", false) && domain == "edge") {
		ids = r_mesh.get_boundary_edges();
	}
	if ((bool)p_args.get("edge_loop", false)) {
		if (domain != "edge") {
			return _operation_error("MODEL_ARGUMENT_INVALID", "Edge-loop selection requires the edge domain.");
		}
		if (ids.is_empty()) {
			ids = r_mesh.get_selected_edges();
		}
		Vector<int64_t> edge_loop;
		String loop_error;
		if (!_quad_edge_ring(r_mesh, ids, edge_loop, false, &loop_error)) {
			return _operation_error("MODEL_EDGE_LOOP_INVALID", loop_error);
		}
		ids = edge_loop;
	}
	if ((bool)p_args.get("connected", false)) {
		if (ids.is_empty()) {
			ids = _domain_selection(r_mesh, domain);
		}
		if (ids.is_empty()) {
			return _operation_error("MODEL_SELECTION_EMPTY", "Connected selection requires one or more seed elements.");
		}
		ids = _connected_elements(r_mesh, domain, ids);
	}
	if (p_args.has("spatial")) {
		const Variant spatial_value = p_args.get("spatial", Variant());
		if (spatial_value.get_type() != Variant::DICTIONARY) {
			return _operation_error("MODEL_ARGUMENT_INVALID", "Spatial selection must be an object.");
		}
		Vector<int64_t> matches;
		String spatial_error;
		if (!_spatial_filter(r_mesh, domain, spatial_value, ids.is_empty() ? _domain_ids(r_mesh, domain) : ids, matches, spatial_error)) {
			return _operation_error("MODEL_ARGUMENT_INVALID", spatial_error);
		}
		ids = matches;
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

static Dictionary _generate_uv(SolersEditableMesh &r_mesh, const Dictionary &p_args, bool p_pack_existing) {
	String error_message;
	const Error error = SolersModelUV::unwrap(r_mesh, p_args, p_pack_existing, &error_message);
	if (error != OK) {
		return _operation_error(p_pack_existing ? "MODEL_UV_PACK_FAILED" : "MODEL_UV_UNWRAP_FAILED", error_message);
	}
	Dictionary data;
	data["face_count"] = r_mesh.get_face_ids().size();
	data["packed_existing"] = p_pack_existing;
	return _operation_ok(data);
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

static int _cycle_distance(int p_from, int p_to, int p_size) {
	return (p_to - p_from + p_size) % p_size;
}

static Vector<int64_t> _cycle_path(const Vector<int64_t> &p_cycle, int p_from, int p_to) {
	Vector<int64_t> path;
	for (int index = p_from;; index = (index + 1) % p_cycle.size()) {
		path.push_back(p_cycle[index]);
		if (index == p_to) {
			break;
		}
	}
	return path;
}

static Dictionary _grid_fill(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> edge_ids = _edge_selection(r_mesh, p_args);
	Vector<int64_t> boundary;
	if (!_boundary_cycle(r_mesh, edge_ids, boundary)) {
		return _operation_error("MODEL_BOUNDARY_INVALID", "Grid Fill requires one closed loop of boundary edges.");
	}
	const Vector<int64_t> corners = _id_array(p_args.get("corners", Variant()));
	if (corners.size() != 4) {
		return _operation_error("MODEL_ARGUMENT_INVALID", "Grid Fill requires four boundary corner vertex IDs in cyclic order.");
	}
	Vector<int> corner_indices;
	for (int64_t corner : corners) {
		const int index = boundary.find(corner);
		if (index < 0 || corner_indices.has(index)) {
			return _operation_error("MODEL_ARGUMENT_INVALID", "Grid Fill corners must be four distinct vertices on the selected boundary.");
		}
		corner_indices.push_back(index);
	}
	bool ordered = true;
	for (int i = 0; i < 4; i++) {
		ordered = ordered && _cycle_distance(corner_indices[i], corner_indices[(i + 1) % 4], boundary.size()) > 0;
	}
	if (!ordered) {
		boundary.reverse();
		corner_indices.clear();
		for (int64_t corner : corners) {
			corner_indices.push_back(boundary.find(corner));
		}
		ordered = true;
		for (int i = 0; i < 4; i++) {
			ordered = ordered && _cycle_distance(corner_indices[i], corner_indices[(i + 1) % 4], boundary.size()) > 0;
		}
	}
	if (!ordered) {
		return _operation_error("MODEL_ARGUMENT_INVALID", "Grid Fill corners are not in cyclic boundary order.");
	}
	const Vector<int64_t> top = _cycle_path(boundary, corner_indices[0], corner_indices[1]);
	const Vector<int64_t> right = _cycle_path(boundary, corner_indices[1], corner_indices[2]);
	const Vector<int64_t> bottom = _cycle_path(boundary, corner_indices[2], corner_indices[3]);
	const Vector<int64_t> left = _cycle_path(boundary, corner_indices[3], corner_indices[0]);
	if (top.size() != bottom.size() || right.size() != left.size()) {
		return _operation_error("MODEL_GRID_FILL_INVALID", "Opposite Grid Fill boundary sides must have equal edge counts.");
	}
	const int columns = top.size() - 1;
	const int rows = right.size() - 1;
	Vector<Vector<int64_t>> grid;
	grid.resize(rows + 1);
	for (Vector<int64_t> &row : grid) {
		row.resize(columns + 1);
	}
	for (int column = 0; column <= columns; column++) {
		grid.write[0].write[column] = top[column];
		grid.write[rows].write[column] = bottom[columns - column];
	}
	for (int row = 0; row <= rows; row++) {
		grid.write[row].write[columns] = right[row];
		grid.write[row].write[0] = left[rows - row];
	}
	Vector<int64_t> created_vertices;
	for (int row = 1; row < rows; row++) {
		const double v = (double)row / rows;
		const Vector3 left_point = r_mesh.get_vertex(grid[row][0])->position;
		const Vector3 right_point = r_mesh.get_vertex(grid[row][columns])->position;
		for (int column = 1; column < columns; column++) {
			const double u = (double)column / columns;
			const Vector3 top_point = r_mesh.get_vertex(grid[0][column])->position;
			const Vector3 bottom_point = r_mesh.get_vertex(grid[rows][column])->position;
			const Vector3 bilinear = r_mesh.get_vertex(grid[0][0])->position * ((1.0 - u) * (1.0 - v)) +
					r_mesh.get_vertex(grid[0][columns])->position * (u * (1.0 - v)) +
					r_mesh.get_vertex(grid[rows][0])->position * ((1.0 - u) * v) +
					r_mesh.get_vertex(grid[rows][columns])->position * (u * v);
			const Vector3 point = top_point * (1.0 - v) + bottom_point * v + left_point * (1.0 - u) + right_point * u - bilinear;
			grid.write[row].write[column] = r_mesh.add_vertex(point);
			created_vertices.push_back(grid[row][column]);
		}
	}
	bool reverse_winding = false;
	for (int64_t edge_id : edge_ids) {
		const SolersEditableMesh::Edge *edge = r_mesh.get_edge(edge_id);
		if (!(SolersEditableMesh::EdgeKey(edge->vertex_a, edge->vertex_b) == SolersEditableMesh::EdgeKey(top[0], top[1]))) {
			continue;
		}
		if (!edge->loops.is_empty()) {
			const SolersEditableMesh::Loop *loop = r_mesh.get_loop(edge->loops[0]);
			reverse_winding = loop->vertex != top[0];
		}
		break;
	}
	Vector<int64_t> created_faces;
	for (int row = 0; row < rows; row++) {
		for (int column = 0; column < columns; column++) {
			Vector<int64_t> vertices;
			vertices.push_back(grid[row][column]);
			vertices.push_back(grid[row + 1][column]);
			vertices.push_back(grid[row + 1][column + 1]);
			vertices.push_back(grid[row][column + 1]);
			if (reverse_winding) {
				vertices.reverse();
			}
			String face_error;
			const int64_t face_id = r_mesh.add_face(vertices, p_args.get("material", 0), false, &face_error);
			if (face_id == 0) {
				return _operation_error("MODEL_GRID_FILL_FAILED", face_error);
			}
			created_faces.push_back(face_id);
			const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
			for (int i = 0; i < face->loops.size(); i++) {
				const int64_t vertex_id = r_mesh.get_loop(face->loops[i])->vertex;
				Vector2 uv;
				for (int grid_row = row; grid_row <= row + 1; grid_row++) {
					for (int grid_column = column; grid_column <= column + 1; grid_column++) {
						if (grid[grid_row][grid_column] == vertex_id) {
							uv = Vector2((double)grid_column / columns, (double)grid_row / rows);
						}
					}
				}
				r_mesh.set_loop_uv(face->loops[i], uv);
			}
		}
	}
	r_mesh.select_faces(created_faces);
	return _created_result(created_vertices, Vector<int64_t>(), created_faces);
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

struct _StoredFace {
	Vector<int64_t> vertices;
	int material = 0;
	bool smooth = false;
};

static _StoredFace _store_face(const SolersEditableMesh &p_mesh, int64_t p_face_id) {
	_StoredFace stored;
	stored.vertices = p_mesh.get_face_vertices(p_face_id);
	const SolersEditableMesh::Face *face = p_mesh.get_face(p_face_id);
	if (face) {
		stored.material = face->material;
		stored.smooth = face->smooth;
	}
	return stored;
}

static Dictionary _weld_vertices(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	Vector<int64_t> vertex_ids = _vertex_selection(r_mesh, p_args);
	if (vertex_ids.size() < 2) {
		return _operation_error("MODEL_SELECTION_INVALID", "Weld requires at least two vertices.");
	}
	HashSet<int64_t> selected;
	Vector3 target_position;
	for (int64_t vertex_id : vertex_ids) {
		const SolersEditableMesh::Vertex *vertex = r_mesh.get_vertex(vertex_id);
		if (!vertex) {
			return _operation_error("MODEL_ELEMENT_NOT_FOUND", vformat("Vertex %d does not exist.", vertex_id));
		}
		selected.insert(vertex_id);
		target_position += vertex->position;
	}
	const int64_t requested_target = p_args.get("target_vertex", 0);
	int64_t target_vertex = requested_target;
	if (requested_target != 0) {
		if (!selected.has(requested_target)) {
			return _operation_error("MODEL_ARGUMENT_INVALID", "Weld target_vertex must be part of the vertex selection.");
		}
		target_position = r_mesh.get_vertex(requested_target)->position;
	} else {
		target_position /= vertex_ids.size();
		target_vertex = r_mesh.add_vertex(target_position);
	}

	HashSet<int64_t> affected_face_set;
	for (int64_t vertex_id : vertex_ids) {
		for (int64_t face_id : r_mesh.get_vertex_faces(vertex_id)) {
			affected_face_set.insert(face_id);
		}
	}
	HashMap<int64_t, _StoredFace> stored_faces;
	for (int64_t face_id : affected_face_set) {
		stored_faces.insert(face_id, _store_face(r_mesh, face_id));
		r_mesh.remove_face(face_id, false);
	}
	Vector<int64_t> rebuilt_faces;
	for (const KeyValue<int64_t, _StoredFace> &entry : stored_faces) {
		Vector<int64_t> vertices;
		for (int64_t vertex_id : entry.value.vertices) {
			const int64_t mapped = selected.has(vertex_id) ? target_vertex : vertex_id;
			if (vertices.is_empty() || vertices[vertices.size() - 1] != mapped) {
				vertices.push_back(mapped);
			}
		}
		if (vertices.size() > 1 && vertices[0] == vertices[vertices.size() - 1]) {
			vertices.remove_at(vertices.size() - 1);
		}
		HashSet<int64_t> unique;
		for (int64_t vertex_id : vertices) {
			unique.insert(vertex_id);
		}
		if (unique.size() >= 3) {
			rebuilt_faces.push_back(r_mesh.add_face(vertices, entry.value.material, entry.value.smooth));
		}
	}
	r_mesh.remove_orphan_elements();
	Vector<int64_t> selected_target;
	selected_target.push_back(target_vertex);
	r_mesh.select_vertices(selected_target);
	Dictionary data;
	data["target_vertex"] = target_vertex;
	data["rebuilt_faces"] = _id_result(rebuilt_faces);
	return _operation_ok(data);
}

static bool _merged_face_boundary(const SolersEditableMesh &p_mesh, int64_t p_face_a, int64_t p_face_b, int64_t p_removed_edge, Vector<int64_t> &r_boundary) {
	HashMap<int64_t, Vector<int64_t>> adjacency;
	const int64_t face_ids[2] = { p_face_a, p_face_b };
	for (int64_t face_id : face_ids) {
		const SolersEditableMesh::Face *face = p_mesh.get_face(face_id);
		if (!face) {
			return false;
		}
		for (int64_t loop_id : face->loops) {
			const SolersEditableMesh::Loop *loop = p_mesh.get_loop(loop_id);
			if (loop->edge == p_removed_edge) {
				continue;
			}
			const int64_t a = loop->vertex;
			const int64_t b = p_mesh.get_loop(loop->next)->vertex;
			adjacency[a].push_back(b);
			adjacency[b].push_back(a);
		}
	}
	if (adjacency.is_empty()) {
		return false;
	}
	int64_t start = 0;
	for (const KeyValue<int64_t, Vector<int64_t>> &entry : adjacency) {
		if (entry.value.size() != 2) {
			return false;
		}
		if (start == 0) {
			start = entry.key;
		}
	}
	int64_t previous = 0;
	int64_t current = start;
	for (uint32_t i = 0; i < adjacency.size(); i++) {
		r_boundary.push_back(current);
		const Vector<int64_t> &neighbors = adjacency[current];
		const int64_t next = neighbors[0] == previous ? neighbors[1] : neighbors[0];
		previous = current;
		current = next;
	}
	return current == start;
}

static Dictionary _dissolve_edges(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> edge_ids = _edge_selection(r_mesh, p_args);
	if (edge_ids.is_empty()) {
		return _operation_error("MODEL_SELECTION_EMPTY", "Dissolve requires selected edges.");
	}
	Vector<int64_t> created_faces;
	for (int64_t edge_id : edge_ids) {
		const SolersEditableMesh::Edge *edge = r_mesh.get_edge(edge_id);
		if (!edge || edge->loops.size() != 2) {
			return _operation_error("MODEL_DISSOLVE_INVALID", vformat("Edge %d must separate exactly two faces.", edge_id));
		}
		const int64_t face_a = r_mesh.get_loop(edge->loops[0])->face;
		const int64_t face_b = r_mesh.get_loop(edge->loops[1])->face;
		Vector<int64_t> boundary;
		if (!_merged_face_boundary(r_mesh, face_a, face_b, edge_id, boundary)) {
			return _operation_error("MODEL_DISSOLVE_INVALID", vformat("Edge %d does not form a simple two-face boundary.", edge_id));
		}
		const _StoredFace stored = _store_face(r_mesh, face_a);
		r_mesh.remove_face(face_a, false);
		r_mesh.remove_face(face_b, false);
		String face_error;
		const int64_t merged = r_mesh.add_face(boundary, stored.material, stored.smooth, &face_error);
		if (merged == 0) {
			return _operation_error("MODEL_DISSOLVE_FAILED", face_error);
		}
		created_faces.push_back(merged);
	}
	r_mesh.remove_orphan_elements();
	r_mesh.select_faces(created_faces);
	Dictionary data;
	data["created_faces"] = _id_result(created_faces);
	return _operation_ok(data);
}

static Dictionary _split_faces(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector<int64_t> face_ids = _face_selection(r_mesh, p_args);
	if (face_ids.is_empty()) {
		return _operation_error("MODEL_SELECTION_EMPTY", "Split requires selected faces.");
	}
	HashMap<int64_t, int64_t> duplicates;
	HashMap<int64_t, _StoredFace> stored;
	Vector<int64_t> created_vertices;
	for (int64_t face_id : face_ids) {
		if (!r_mesh.get_face(face_id)) {
			return _operation_error("MODEL_ELEMENT_NOT_FOUND", vformat("Face %d does not exist.", face_id));
		}
		stored.insert(face_id, _store_face(r_mesh, face_id));
		for (int64_t vertex_id : stored[face_id].vertices) {
			if (!duplicates.has(vertex_id)) {
				const int64_t duplicate = r_mesh.add_vertex(r_mesh.get_vertex(vertex_id)->position);
				duplicates.insert(vertex_id, duplicate);
				created_vertices.push_back(duplicate);
			}
		}
	}
	for (int64_t face_id : face_ids) {
		r_mesh.remove_face(face_id, false);
	}
	Vector<int64_t> created_faces;
	for (int64_t face_id : face_ids) {
		Vector<int64_t> vertices;
		for (int64_t vertex_id : stored[face_id].vertices) {
			vertices.push_back(duplicates[vertex_id]);
		}
		created_faces.push_back(r_mesh.add_face(vertices, stored[face_id].material, stored[face_id].smooth));
	}
	r_mesh.remove_orphan_elements();
	r_mesh.select_faces(created_faces);
	return _created_result(created_vertices, Vector<int64_t>(), created_faces);
}

static Dictionary _loop_cut(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	Vector<int64_t> seed_edges = _id_array(p_args.get("edge_ids", Variant()));
	if (seed_edges.is_empty()) {
		seed_edges = r_mesh.get_selected_edges();
	}
	const double factor = p_args.get("factor", 0.5);
	if (!Math::is_finite(factor) || factor <= 0 || factor >= 1) {
		return _operation_error("MODEL_ARGUMENT_INVALID", "Loop Cut factor must be between zero and one.");
	}
	Vector<int64_t> ring;
	String ring_error;
	if (!_quad_edge_ring(r_mesh, seed_edges, ring, true, &ring_error)) {
		return _operation_error("MODEL_LOOP_CUT_INVALID", ring_error);
	}
	HashSet<int64_t> ring_edges;
	for (int64_t edge_id : ring) {
		ring_edges.insert(edge_id);
	}
	HashMap<int64_t, int64_t> cut_vertices;
	Vector<int64_t> created_vertices;
	for (int64_t edge_id : ring_edges) {
		const SolersEditableMesh::Edge *edge = r_mesh.get_edge(edge_id);
		const Vector3 position = r_mesh.get_vertex(edge->vertex_a)->position.lerp(r_mesh.get_vertex(edge->vertex_b)->position, factor);
		const int64_t vertex = r_mesh.add_vertex(position);
		cut_vertices.insert(edge_id, vertex);
		created_vertices.push_back(vertex);
	}
	HashSet<int64_t> affected_faces;
	for (int64_t edge_id : ring_edges) {
		for (int64_t loop_id : r_mesh.get_edge(edge_id)->loops) {
			affected_faces.insert(r_mesh.get_loop(loop_id)->face);
		}
	}
	struct CutFace {
		Vector<int64_t> vertices;
		Vector<int64_t> edges;
		int material = 0;
		bool smooth = false;
	};
	HashMap<int64_t, CutFace> cut_faces;
	for (int64_t face_id : affected_faces) {
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		CutFace cut;
		cut.material = face->material;
		cut.smooth = face->smooth;
		for (int64_t loop_id : face->loops) {
			cut.vertices.push_back(r_mesh.get_loop(loop_id)->vertex);
			cut.edges.push_back(r_mesh.get_loop(loop_id)->edge);
		}
		cut_faces.insert(face_id, cut);
	}
	for (int64_t face_id : affected_faces) {
		r_mesh.remove_face(face_id, false);
	}
	Vector<int64_t> created_faces;
	for (const KeyValue<int64_t, CutFace> &entry : cut_faces) {
		Vector<int> cuts;
		for (int i = 0; i < entry.value.edges.size(); i++) {
			if (ring_edges.has(entry.value.edges[i])) {
				cuts.push_back(i);
			}
		}
		if (cuts.size() != 2) {
			return _operation_error("MODEL_LOOP_CUT_INVALID", "Each ring face must contain two opposite cut edges.");
		}
		const int first = cuts[0];
		const int second = cuts[1];
		Vector<int64_t> side_a;
		side_a.push_back(cut_vertices[entry.value.edges[first]]);
		for (int index = (first + 1) % entry.value.vertices.size(); index != (second + 1) % entry.value.vertices.size(); index = (index + 1) % entry.value.vertices.size()) {
			side_a.push_back(entry.value.vertices[index]);
		}
		side_a.push_back(cut_vertices[entry.value.edges[second]]);
		Vector<int64_t> side_b;
		side_b.push_back(cut_vertices[entry.value.edges[second]]);
		for (int index = (second + 1) % entry.value.vertices.size(); index != (first + 1) % entry.value.vertices.size(); index = (index + 1) % entry.value.vertices.size()) {
			side_b.push_back(entry.value.vertices[index]);
		}
		side_b.push_back(cut_vertices[entry.value.edges[first]]);
		created_faces.push_back(r_mesh.add_face(side_a, entry.value.material, entry.value.smooth));
		created_faces.push_back(r_mesh.add_face(side_b, entry.value.material, entry.value.smooth));
	}
	r_mesh.remove_orphan_elements();
	r_mesh.select_vertices(created_vertices);
	return _created_result(created_vertices, Vector<int64_t>(), created_faces);
}

static Dictionary _slide_vertices(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Array moves = p_args.get("moves", Array());
	const double factor = p_args.get("factor", 0.5);
	if (moves.is_empty() || !Math::is_finite(factor)) {
		return _operation_error("MODEL_ARGUMENT_INVALID", "Slide requires moves and a finite factor.");
	}
	Vector<int64_t> modified;
	for (const Variant &move_value : moves) {
		if (move_value.get_type() != Variant::DICTIONARY) {
			return _operation_error("MODEL_ARGUMENT_INVALID", "Each slide move must be an object.");
		}
		const Dictionary move = move_value;
		const int64_t vertex_id = move.get("vertex", 0);
		const int64_t target_id = move.get("target", 0);
		SolersEditableMesh::Vertex *vertex = r_mesh.get_vertex(vertex_id);
		const SolersEditableMesh::Vertex *target = r_mesh.get_vertex(target_id);
		if (!vertex || !target) {
			return _operation_error("MODEL_ELEMENT_NOT_FOUND", "Slide vertex or target does not exist.");
		}
		const SolersEditableMesh::EdgeKey edge_key(vertex_id, target_id);
		bool connected = false;
		for (int64_t edge_id : r_mesh.get_vertex_edges(vertex_id)) {
			const SolersEditableMesh::Edge *edge = r_mesh.get_edge(edge_id);
			if (SolersEditableMesh::EdgeKey(edge->vertex_a, edge->vertex_b) == edge_key) {
				connected = true;
				break;
			}
		}
		if (!connected) {
			return _operation_error("MODEL_SLIDE_INVALID", "Slide targets must share an edge with their vertices.");
		}
		vertex->position = vertex->position.lerp(target->position, factor);
		modified.push_back(vertex_id);
	}
	Dictionary data;
	data["modified_vertices"] = _id_result(modified);
	return _operation_ok(data);
}

static int64_t _bisect_intersection_vertex(SolersEditableMesh &r_mesh, HashMap<SolersEditableMesh::EdgeKey, int64_t, SolersEditableMesh::EdgeKey> &r_intersections, int64_t p_a, int64_t p_b, double p_distance_a, double p_distance_b) {
	const SolersEditableMesh::EdgeKey key(p_a, p_b);
	if (const int64_t *existing = r_intersections.getptr(key)) {
		return *existing;
	}
	const double t = p_distance_a / (p_distance_a - p_distance_b);
	const Vector3 point = r_mesh.get_vertex(p_a)->position.lerp(r_mesh.get_vertex(p_b)->position, t);
	const int64_t vertex = r_mesh.add_vertex(point);
	r_intersections.insert(key, vertex);
	return vertex;
}

static Vector<int64_t> _clip_face_halfspace(SolersEditableMesh &r_mesh, const Vector<int64_t> &p_vertices, const Vector<double> &p_distances, bool p_positive, HashMap<SolersEditableMesh::EdgeKey, int64_t, SolersEditableMesh::EdgeKey> &r_intersections, Vector<int64_t> &r_cut_vertices) {
	Vector<int64_t> clipped;
	for (int i = 0; i < p_vertices.size(); i++) {
		const int next = (i + 1) % p_vertices.size();
		const bool inside = p_positive ? p_distances[i] >= -CMP_EPSILON : p_distances[i] <= CMP_EPSILON;
		const bool next_inside = p_positive ? p_distances[next] >= -CMP_EPSILON : p_distances[next] <= CMP_EPSILON;
		if (inside) {
			clipped.push_back(p_vertices[i]);
		}
		if (inside != next_inside) {
			const int64_t intersection = _bisect_intersection_vertex(r_mesh, r_intersections, p_vertices[i], p_vertices[next], p_distances[i], p_distances[next]);
			clipped.push_back(intersection);
			r_cut_vertices.push_back(intersection);
		}
	}
	return clipped;
}

static Dictionary _bisect(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const Vector3 origin = _vector3_arg(p_args.get("origin", Variant()));
	Vector3 normal = _vector3_arg(p_args.get("normal", Variant()), Vector3(0, 1, 0));
	if (!origin.is_finite() || !normal.is_finite() || normal.is_zero_approx()) {
		return _operation_error("MODEL_ARGUMENT_INVALID", "Bisect origin and normal must define a finite plane.");
	}
	normal.normalize();
	const bool clear_positive = p_args.get("clear_positive", false);
	const bool clear_negative = p_args.get("clear_negative", false);
	if (clear_positive && clear_negative) {
		return _operation_error("MODEL_ARGUMENT_INVALID", "Bisect cannot clear both sides of the plane.");
	}
	const Vector<int64_t> face_ids = r_mesh.get_face_ids();
	HashMap<SolersEditableMesh::EdgeKey, int64_t, SolersEditableMesh::EdgeKey> intersections;
	Vector<int64_t> created_faces;
	Vector<int64_t> created_vertices;
	for (int64_t face_id : face_ids) {
		const _StoredFace stored = _store_face(r_mesh, face_id);
		Vector<double> distances;
		bool has_positive = false;
		bool has_negative = false;
		for (int64_t vertex_id : stored.vertices) {
			const double distance = normal.dot(r_mesh.get_vertex(vertex_id)->position - origin);
			distances.push_back(distance);
			has_positive = has_positive || distance > CMP_EPSILON;
			has_negative = has_negative || distance < -CMP_EPSILON;
		}
		if (!has_positive || !has_negative) {
			if ((has_positive && clear_positive) || (has_negative && clear_negative)) {
				r_mesh.remove_face(face_id, false);
			}
			continue;
		}
		Vector<int64_t> cut_vertices;
		const Vector<int64_t> positive = _clip_face_halfspace(r_mesh, stored.vertices, distances, true, intersections, cut_vertices);
		const Vector<int64_t> negative = _clip_face_halfspace(r_mesh, stored.vertices, distances, false, intersections, cut_vertices);
		r_mesh.remove_face(face_id, false);
		if (!clear_positive && positive.size() >= 3) {
			created_faces.push_back(r_mesh.add_face(positive, stored.material, stored.smooth));
		}
		if (!clear_negative && negative.size() >= 3) {
			created_faces.push_back(r_mesh.add_face(negative, stored.material, stored.smooth));
		}
		for (int64_t vertex_id : cut_vertices) {
			if (!created_vertices.has(vertex_id)) {
				created_vertices.push_back(vertex_id);
			}
		}
	}
	r_mesh.remove_orphan_elements();
	r_mesh.select_vertices(created_vertices);
	Dictionary data;
	data["created_vertices"] = _id_result(created_vertices);
	data["created_faces"] = _id_result(created_faces);
	return _operation_ok(data);
}

static Dictionary _add_modifier(SolersEditableMesh &r_mesh, const Dictionary &p_args, const StringName &p_type) {
	const int64_t id = r_mesh.add_modifier(p_type, p_args);
	Dictionary data;
	data["modifier_id"] = id;
	data["type"] = p_type;
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

static Dictionary _set_modifier_enabled(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	const int64_t id = p_args.get("modifier_id", 0);
	for (SolersEditableMesh::Modifier &modifier : r_mesh.get_modifiers()) {
		if (modifier.id != id) {
			continue;
		}
		modifier.enabled = p_args.get("enabled", true);
		Dictionary data;
		data["modifier_id"] = id;
		data["enabled"] = modifier.enabled;
		Dictionary result;
		result["ok"] = true;
		result["data"] = data;
		return result;
	}
	Dictionary error;
	error["code"] = "MODEL_ELEMENT_NOT_FOUND";
	error["message"] = vformat("Modifier %d does not exist.", id);
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

static Dictionary _apply_modifiers(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	SolersEditableMesh evaluated;
	String error_message;
	if (SolersModelModifierEvaluator::evaluate(r_mesh, evaluated, &error_message) != OK) {
		Dictionary error;
		error["code"] = "MODEL_MODIFIER_FAILED";
		error["message"] = error_message;
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		return result;
	}
	evaluated.set_revision(r_mesh.get_revision());
	r_mesh = evaluated;
	Dictionary data;
	data["applied"] = true;
	data["vertex_count"] = r_mesh.get_vertex_ids().size();
	data["face_count"] = r_mesh.get_face_ids().size();
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _bevel_geometry(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	SolersEditableMesh base = r_mesh;
	const Vector<SolersEditableMesh::Modifier> modifiers = base.get_modifiers();
	base.get_modifiers().clear();
	base.add_modifier(SNAME("bevel"), p_args);
	SolersEditableMesh beveled;
	String error_message;
	if (SolersModelModifierEvaluator::evaluate(base, beveled, &error_message) != OK) {
		return _operation_error("MODEL_BEVEL_FAILED", error_message);
	}
	beveled.get_modifiers() = modifiers;
	beveled.set_revision(r_mesh.get_revision());
	r_mesh = beveled;
	Dictionary data;
	data["vertex_count"] = r_mesh.get_vertex_ids().size();
	data["face_count"] = r_mesh.get_face_ids().size();
	return _operation_ok(data);
}

static Dictionary _configure_build(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	Dictionary &settings = r_mesh.get_build_settings();
	if (p_args.has("weighted_normals")) {
		settings["weighted_normals"] = p_args.get("weighted_normals", true);
	}
	if (p_args.has("generate_tangents")) {
		settings["generate_tangents"] = p_args.get("generate_tangents", true);
	}
	if (p_args.has("generate_uv2")) {
		settings["generate_uv2"] = p_args.get("generate_uv2", false);
	}
	if (p_args.has("lightmap_texel_size")) {
		const double texel_size = p_args.get("lightmap_texel_size", 0.05);
		if (!Math::is_finite(texel_size) || texel_size <= 0) {
			Dictionary error;
			error["code"] = "MODEL_ARGUMENT_INVALID";
			error["message"] = "Lightmap texel size must be finite and positive.";
			Dictionary result;
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		settings["lightmap_texel_size"] = texel_size;
	}
	if (p_args.has("lod_levels")) {
		const Variant levels = p_args.get("lod_levels", Array());
		if (levels.get_type() != Variant::ARRAY) {
			Dictionary error;
			error["code"] = "MODEL_ARGUMENT_INVALID";
			error["message"] = "LOD levels must be an array.";
			Dictionary result;
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		settings["lod_levels"] = ((Array)levels).duplicate(true);
	}
	if (p_args.has("collision")) {
		const String collision = String(p_args.get("collision", "none")).to_lower();
		if (collision != "none" && collision != "trimesh" && collision != "convex") {
			Dictionary error;
			error["code"] = "MODEL_ARGUMENT_INVALID";
			error["message"] = "Collision mode must be none, trimesh, or convex.";
			Dictionary result;
			result["ok"] = false;
			result["error"] = error;
			return result;
		}
		settings["collision"] = collision;
	}
	Dictionary data;
	data["build_settings"] = settings.duplicate(true);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	return result;
}

static Dictionary _set_weighted_normals(SolersEditableMesh &r_mesh, const Dictionary &p_args) {
	r_mesh.get_build_settings()["weighted_normals"] = p_args.get("enabled", true);
	Dictionary data;
	data["enabled"] = r_mesh.get_build_settings()["weighted_normals"];
	return _operation_ok(data);
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
	_add(SNAME("add_box"), "Add a dimensionally exact box primitive to the current model.", _operation_schema(R"({"type":"object","properties":{"size":{"description":"Box dimensions in meters.","type":"object"},"center":{"description":"Primitive center.","type":"object"}},"additionalProperties":false})"), true, false, _add_box);
	_add(SNAME("add_plane"), "Add a planar quad primitive to the current model.", _operation_schema(R"({"type":"object","properties":{"size":{"description":"Plane width and depth.","type":"object"},"center":{"type":"object"}},"additionalProperties":false})"), true, false, _add_plane);
	_add(SNAME("add_cylinder"), "Add a capped cylinder with controlled radial topology to the current model.", _operation_schema(R"({"type":"object","properties":{"segments":{"type":"integer","minimum":3,"maximum":256},"radius":{"type":"number","exclusiveMinimum":0},"depth":{"type":"number","exclusiveMinimum":0},"center":{"type":"object"}},"additionalProperties":false})"), true, false, _add_cylinder);
	_add(SNAME("select"), "Select persistent topology IDs, connected regions, quad edge loops, boundaries, or spatial matches.", _operation_schema(R"({"type":"object","properties":{"domain":{"type":"string","enum":["vertex","edge","face"]},"ids":{"type":"array","items":{"type":"integer"}},"mode":{"type":"string","enum":["replace","add","subtract"]},"all":{"type":"boolean"},"boundary":{"type":"boolean"},"connected":{"type":"boolean"},"edge_loop":{"type":"boolean"},"spatial":{"type":"object","properties":{"center":{"type":"object"},"radius":{"type":"number","minimum":0},"min":{"type":"object"},"max":{"type":"object"}},"additionalProperties":false}},"required":["domain"],"additionalProperties":false})"), false, false, _select_elements);
	_add(SNAME("transform"), "Move, rotate, and scale selected topology with exact numeric values, axis constraints, and snapping.", _operation_schema(R"({"type":"object","properties":{"vertex_ids":{"type":"array","items":{"type":"integer"}},"translation":{"type":"object"},"rotation_degrees":{"type":"object"},"scale":{"type":"object"},"pivot":{"type":"object"},"axis":{"type":"string","enum":["all","x","y","z"]},"snap":{"type":"number","minimum":0}},"additionalProperties":false})"), false, false, _transform_selection);
	_add(SNAME("extrude_faces"), "Extrude a connected face region while preserving boundary adjacency.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"distance":{"type":"number"},"offset":{"type":"object"}},"additionalProperties":false})"), true, false, _extrude_faces);
	_add(SNAME("inset_faces"), "Inset selected polygon faces and create a clean surrounding ring.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"factor":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":1}},"additionalProperties":false})"), true, false, _inset_faces);
	_add(SNAME("delete"), "Delete selected topology with deterministic incident-face cleanup.", _operation_schema(R"({"type":"object","properties":{"domain":{"type":"string","enum":["vertex","edge","face"]},"ids":{"type":"array","items":{"type":"integer"}}},"required":["domain"],"additionalProperties":false})"), true, false, _delete_elements);
	_add(SNAME("flip_faces"), "Reverse selected face winding and normals.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}}},"additionalProperties":false})"), true, false, _flip_faces);
	_add(SNAME("set_smooth"), "Set smooth or flat shading on selected faces.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"enabled":{"type":"boolean"}},"additionalProperties":false})"), false, false, _set_face_smoothing);
	_add(SNAME("set_sharp"), "Mark selected edges as hard or smooth boundaries.", _operation_schema(R"({"type":"object","properties":{"edge_ids":{"type":"array","items":{"type":"integer"}},"enabled":{"type":"boolean"}},"additionalProperties":false})"), false, false, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _set_edge_flag(r_mesh, p_args, false); });
	_add(SNAME("set_uv_seam"), "Mark selected edges as UV seams.", _operation_schema(R"({"type":"object","properties":{"edge_ids":{"type":"array","items":{"type":"integer"}},"enabled":{"type":"boolean"}},"additionalProperties":false})"), false, false, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _set_edge_flag(r_mesh, p_args, true); });
	_add(SNAME("project_uv"), "Project UV coordinates on selected faces using a precise world axis or automatic face axis.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"axis":{"type":"string","enum":["auto","x","y","z"]},"scale":{"type":"object"},"offset":{"type":"object"}},"additionalProperties":false})"), false, false, _project_uv);
	const Dictionary uv_atlas_schema = _operation_schema(R"({"type":"object","properties":{"resolution":{"type":"integer","minimum":16,"maximum":16384},"padding":{"type":"integer","minimum":0,"maximum":256},"texels_per_unit":{"type":"number","minimum":0}},"additionalProperties":false})");
	_add(SNAME("unwrap_uv"), "Generate seam-aware UV1 charts and pack them with xatlas.", uv_atlas_schema, false, false, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _generate_uv(r_mesh, p_args, false); });
	_add(SNAME("pack_uv"), "Repack existing UV1 charts with xatlas while preserving their chart intent.", uv_atlas_schema, false, false, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _generate_uv(r_mesh, p_args, true); });
	_add(SNAME("set_material"), "Assign a material resource or existing material slot to selected faces.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"}},"material_index":{"type":"integer","minimum":0},"material_path":{"type":"string"}},"additionalProperties":false})"), false, false, _set_material);
	_add(SNAME("fill_hole"), "Fill one closed boundary edge loop with an n-gon.", _operation_schema(R"({"type":"object","properties":{"edge_ids":{"type":"array","items":{"type":"integer"}},"material":{"type":"integer","minimum":0}},"additionalProperties":false})"), true, false, _fill_hole);
	_add(SNAME("grid_fill"), "Fill a four-sided boundary with a regular quad grid using Coons interpolation.", _operation_schema(R"({"type":"object","properties":{"edge_ids":{"type":"array","items":{"type":"integer"},"minItems":4},"corners":{"type":"array","items":{"type":"integer"},"minItems":4,"maxItems":4},"material":{"type":"integer","minimum":0}},"required":["corners"],"additionalProperties":false})"), true, false, _grid_fill);
	_add(SNAME("bridge"), "Bridge two ordered vertex loops with quad topology.", _operation_schema(R"({"type":"object","properties":{"first":{"type":"array","items":{"type":"integer"},"minItems":2},"second":{"type":"array","items":{"type":"integer"},"minItems":2},"material":{"type":"integer","minimum":0}},"required":["first","second"],"additionalProperties":false})"), true, false, _bridge_loops);
	_add(SNAME("weld"), "Weld selected vertices to their center or to a persistent target vertex.", _operation_schema(R"({"type":"object","properties":{"vertex_ids":{"type":"array","items":{"type":"integer"},"minItems":2},"target_vertex":{"type":"integer"}},"additionalProperties":false})"), true, false, _weld_vertices);
	_add(SNAME("dissolve_edges"), "Dissolve selected two-face edges into validated n-gons.", _operation_schema(R"({"type":"object","properties":{"edge_ids":{"type":"array","items":{"type":"integer"},"minItems":1}},"additionalProperties":false})"), true, false, _dissolve_edges);
	_add(SNAME("split_faces"), "Detach a selected face region while preserving its internal topology.", _operation_schema(R"({"type":"object","properties":{"face_ids":{"type":"array","items":{"type":"integer"},"minItems":1}},"additionalProperties":false})"), true, false, _split_faces);
	_add(SNAME("loop_cut"), "Insert a continuous cut through an all-quad edge ring.", _operation_schema(R"({"type":"object","properties":{"edge_ids":{"type":"array","items":{"type":"integer"},"minItems":1,"maxItems":1},"factor":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":1}},"additionalProperties":false})"), true, false, _loop_cut);
	_add(SNAME("slide"), "Slide vertices along explicitly connected edges by a numeric factor.", _operation_schema(R"({"type":"object","properties":{"moves":{"type":"array","minItems":1,"items":{"type":"object","properties":{"vertex":{"type":"integer"},"target":{"type":"integer"}},"required":["vertex","target"],"additionalProperties":false}},"factor":{"type":"number"}},"required":["moves"],"additionalProperties":false})"), false, false, _slide_vertices);
	const Dictionary bisect_schema = _operation_schema(R"({"type":"object","properties":{"origin":{"type":"object"},"normal":{"type":"object"},"clear_positive":{"type":"boolean"},"clear_negative":{"type":"boolean"}},"required":["origin","normal"],"additionalProperties":false})");
	_add(SNAME("bisect"), "Cut all intersected faces with an exact plane and optionally remove one side.", bisect_schema, true, false, _bisect);
	_add(SNAME("knife_plane"), "Perform a deterministic knife cut defined by an exact plane.", bisect_schema, true, false, _bisect);
	_add(SNAME("bevel"), "Destructively bevel every edge of a closed manifold hard-surface mesh.", _operation_schema(R"({"type":"object","properties":{"width":{"type":"number","exclusiveMinimum":0},"segments":{"type":"integer","minimum":1,"maximum":12}},"additionalProperties":false})"), true, false, _bevel_geometry);
	_add(SNAME("add_mirror_modifier"), "Add a non-destructive mirror modifier.", _operation_schema(R"({"type":"object","properties":{"axis":{"type":"string","enum":["x","y","z"]},"origin":{"type":"object"},"merge":{"type":"boolean"},"merge_distance":{"type":"number","minimum":0}},"additionalProperties":false})"), false, true, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _add_modifier(r_mesh, p_args, SNAME("mirror")); });
	_add(SNAME("add_array_modifier"), "Add a non-destructive linear array modifier.", _operation_schema(R"({"type":"object","properties":{"count":{"type":"integer","minimum":1,"maximum":10000},"offset":{"type":"object"}},"additionalProperties":false})"), false, true, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _add_modifier(r_mesh, p_args, SNAME("array")); });
	_add(SNAME("add_solidify_modifier"), "Add a non-destructive solidify modifier to an open surface.", _operation_schema(R"({"type":"object","properties":{"thickness":{"type":"number"}},"additionalProperties":false})"), false, true, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _add_modifier(r_mesh, p_args, SNAME("solidify")); });
	_add(SNAME("add_bevel_modifier"), "Add a non-destructive hard-surface bevel modifier.", _operation_schema(R"({"type":"object","properties":{"width":{"type":"number","exclusiveMinimum":0},"segments":{"type":"integer","minimum":1,"maximum":12}},"additionalProperties":false})"), false, true, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _add_modifier(r_mesh, p_args, SNAME("bevel")); });
	_add(SNAME("add_boolean_modifier"), "Add a non-destructive Boolean modifier using another .smodel source.", _operation_schema(R"({"type":"object","properties":{"operand":{"type":"string"},"operation":{"type":"string","enum":["union","subtract","intersect"]},"translation":{"type":"object"},"rotation_degrees":{"type":"object"},"scale":{"type":"object"}},"required":["operand","operation"],"additionalProperties":false})"), false, true, [](SolersEditableMesh &r_mesh, const Dictionary &p_args) { return _add_modifier(r_mesh, p_args, SNAME("boolean")); });
	_add(SNAME("remove_modifier"), "Remove a modifier by persistent ID.", _operation_schema(R"({"type":"object","properties":{"modifier_id":{"type":"integer"}},"required":["modifier_id"],"additionalProperties":false})"), false, true, _remove_modifier);
	_add(SNAME("set_modifier_enabled"), "Enable or disable a modifier by persistent ID.", _operation_schema(R"({"type":"object","properties":{"modifier_id":{"type":"integer"},"enabled":{"type":"boolean"}},"required":["modifier_id","enabled"],"additionalProperties":false})"), false, true, _set_modifier_enabled);
	_add(SNAME("apply_modifiers"), "Bake the current modifier stack into editable base topology.", empty, true, true, _apply_modifiers);
	_add(SNAME("weighted_normals"), "Enable or disable area-weighted smooth normals while respecting hard edges.", _operation_schema(R"({"type":"object","properties":{"enabled":{"type":"boolean"}},"additionalProperties":false})"), false, false, _set_weighted_normals);
	_add(SNAME("configure_build"), "Configure tangents, UV2, LOD, static collision, and Lightmap preparation for the imported runtime mesh.", _operation_schema(R"({"type":"object","properties":{"weighted_normals":{"type":"boolean"},"generate_tangents":{"type":"boolean"},"generate_uv2":{"type":"boolean"},"lightmap_texel_size":{"type":"number","exclusiveMinimum":0},"lod_levels":{"type":"array","items":{"type":"object","properties":{"ratio":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":1},"distance":{"type":"number","exclusiveMinimum":0}},"required":["ratio","distance"],"additionalProperties":false}},"collision":{"type":"string","enum":["none","trimesh","convex"]}},"additionalProperties":false})"), false, false, _configure_build);
}

SolersModelOperationRegistry *SolersModelOperationRegistry::get_singleton() {
	static SolersModelOperationRegistry singleton;
	return &singleton;
}

const SolersModelOperationDefinition *SolersModelOperationRegistry::get_operation(const StringName &p_id) const {
	const int *index = operation_index.getptr(p_id);
	return index ? &operations[*index] : nullptr;
}

Dictionary SolersModelOperationRegistry::get_batch_item_schema() const {
	Dictionary binding_properties;
	Dictionary string;
	string["type"] = "string";
	binding_properties["result"] = string;
	binding_properties["field"] = string;
	Dictionary binding;
	binding["type"] = "object";
	binding["properties"] = binding_properties;
	Array binding_required;
	binding_required.push_back("result");
	binding_required.push_back("field");
	binding["required"] = binding_required;
	binding["additionalProperties"] = false;

	Array branches;
	for (const SolersModelOperationDefinition &operation : operations) {
		Dictionary parameters = operation.parameters_schema.duplicate(true);
		parameters.erase("required");
		Dictionary operation_bindings;
		operation_bindings["type"] = "object";
		Dictionary per_parameter;
		const Dictionary parameter_properties = operation.parameters_schema.get("properties", Dictionary());
		for (const Variant &key : parameter_properties.keys()) {
			per_parameter[key] = binding;
		}
		operation_bindings["properties"] = per_parameter;
		operation_bindings["additionalProperties"] = false;

		Dictionary operation_name;
		operation_name["const"] = String(operation.id);
		Dictionary properties;
		properties["id"] = string;
		properties["operation"] = operation_name;
		properties["parameters"] = parameters;
		properties["bindings"] = operation_bindings;
		Dictionary branch;
		branch["type"] = "object";
		branch["description"] = operation.description;
		branch["properties"] = properties;
		Array required;
		required.push_back("operation");
		branch["required"] = required;
		branch["additionalProperties"] = false;
		branches.push_back(branch);
	}
	Dictionary schema;
	schema["oneOf"] = branches;
	return schema;
}

Dictionary SolersModelOperationRegistry::validate_parameters(const StringName &p_id, const Dictionary &p_parameters, bool p_allow_missing_required) const {
	const SolersModelOperationDefinition *operation = get_operation(p_id);
	if (!operation) {
		return _error("MODEL_OPERATION_NOT_FOUND", vformat("Unknown modeling operation: %s", p_id), true);
	}
	Dictionary schema = operation->parameters_schema;
	if (p_allow_missing_required) {
		schema = schema.duplicate(true);
		schema.erase("required");
	}
	String argument_error;
	if (!_validate_schema_value(p_parameters, schema, "parameters", argument_error)) {
		return _error("MODEL_ARGUMENT_INVALID", argument_error, true);
	}
	return _ok();
}

Dictionary SolersModelOperationRegistry::execute(SolersEditableMesh &r_mesh, const StringName &p_id, const Dictionary &p_parameters) const {
	const Dictionary parameter_validation = validate_parameters(p_id, p_parameters);
	if (!(bool)parameter_validation.get("ok", false)) {
		return parameter_validation;
	}
	const SolersModelOperationDefinition *operation = get_operation(p_id);
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
