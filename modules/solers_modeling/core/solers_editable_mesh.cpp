/**************************************************************************/
/*  solers_editable_mesh.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_editable_mesh.h"

#include "core/io/resource_loader.h"
#include "core/math/geometry_2d.h"
#include "core/math/math_funcs.h"
#include "core/templates/hash_set.h"
#include "modules/solers_modeling/core/solers_model_modifier.h"
#include "scene/resources/mesh.h"
#include "scene/resources/material.h"
#include "scene/resources/surface_tool.h"
#include "scene/resources/3d/shape_3d.h"

static Array _ids_to_array(const Vector<int64_t> &p_ids) {
	Array out;
	for (int64_t id : p_ids) {
		out.push_back(id);
	}
	return out;
}

static Vector<int64_t> _sorted_ids(const HashMap<int64_t, SolersEditableMesh::Vertex> &p_elements) {
	Vector<int64_t> ids;
	for (const KeyValue<int64_t, SolersEditableMesh::Vertex> &element : p_elements) {
		ids.push_back(element.key);
	}
	ids.sort();
	return ids;
}

template <typename T>
static Vector<int64_t> _sorted_element_ids(const HashMap<int64_t, T> &p_elements) {
	Vector<int64_t> ids;
	for (const KeyValue<int64_t, T> &element : p_elements) {
		ids.push_back(element.key);
	}
	ids.sort();
	return ids;
}

int64_t SolersEditableMesh::_allocate_id() {
	ERR_FAIL_COND_V(next_id <= 0 || next_id == INT64_MAX, 0);
	return next_id++;
}

void SolersEditableMesh::_rebuild_indices() {
	edge_lookup.clear();
	for (const KeyValue<int64_t, Edge> &entry : edges) {
		edge_lookup.insert(EdgeKey(entry.value.vertex_a, entry.value.vertex_b), entry.key);
	}
}

int64_t SolersEditableMesh::_find_or_create_edge(int64_t p_vertex_a, int64_t p_vertex_b) {
	const EdgeKey key(p_vertex_a, p_vertex_b);
	if (const int64_t *existing = edge_lookup.getptr(key)) {
		return *existing;
	}
	Edge edge;
	edge.id = _allocate_id();
	edge.vertex_a = key.a;
	edge.vertex_b = key.b;
	edges.insert(edge.id, edge);
	edge_lookup.insert(key, edge.id);
	return edge.id;
}

void SolersEditableMesh::_remove_loop_from_edge(int64_t p_edge_id, int64_t p_loop_id) {
	Edge *edge = edges.getptr(p_edge_id);
	if (edge) {
		edge->loops.erase(p_loop_id);
	}
}

void SolersEditableMesh::_remove_orphan_elements() {
	Vector<int64_t> orphan_edges;
	for (const KeyValue<int64_t, Edge> &entry : edges) {
		if (entry.value.loops.is_empty()) {
			orphan_edges.push_back(entry.key);
		}
	}
	for (int64_t edge_id : orphan_edges) {
		const Edge *edge = edges.getptr(edge_id);
		if (edge) {
			edge_lookup.erase(EdgeKey(edge->vertex_a, edge->vertex_b));
		}
		edges.erase(edge_id);
	}

	HashSet<int64_t> used_vertices;
	for (const KeyValue<int64_t, Edge> &entry : edges) {
		used_vertices.insert(entry.value.vertex_a);
		used_vertices.insert(entry.value.vertex_b);
	}
	Vector<int64_t> orphan_vertices;
	for (const KeyValue<int64_t, Vertex> &entry : vertices) {
		if (!used_vertices.has(entry.key)) {
			orphan_vertices.push_back(entry.key);
		}
	}
	for (int64_t vertex_id : orphan_vertices) {
		vertices.erase(vertex_id);
	}
}

void SolersEditableMesh::clear() {
	revision = 0;
	next_id = 1;
	vertices.clear();
	edges.clear();
	loops.clear();
	faces.clear();
	edge_lookup.clear();
	material_paths.clear();
	modifiers.clear();
	build_settings.clear();
}

int64_t SolersEditableMesh::add_vertex(const Vector3 &p_position) {
	Vertex vertex;
	vertex.id = _allocate_id();
	vertex.position = p_position;
	vertices.insert(vertex.id, vertex);
	return vertex.id;
}

int64_t SolersEditableMesh::add_face(const Vector<int64_t> &p_vertices, int p_material, bool p_smooth, String *r_error) {
	if (p_vertices.size() < 3) {
		if (r_error) {
			*r_error = "A face requires at least three vertices.";
		}
		return 0;
	}
	HashSet<int64_t> unique;
	for (int i = 0; i < p_vertices.size(); i++) {
		const int64_t vertex_id = p_vertices[i];
		if (!vertices.has(vertex_id)) {
			if (r_error) {
				*r_error = vformat("Face references missing vertex %d.", vertex_id);
			}
			return 0;
		}
		if (vertex_id == p_vertices[(i + 1) % p_vertices.size()] || unique.has(vertex_id)) {
			if (r_error) {
				*r_error = "A face cannot repeat a vertex.";
			}
			return 0;
		}
		unique.insert(vertex_id);
	}

	Face face;
	face.id = _allocate_id();
	face.material = MAX(0, p_material);
	face.smooth = p_smooth;
	for (int i = 0; i < p_vertices.size(); i++) {
		Loop loop;
		loop.id = _allocate_id();
		loop.vertex = p_vertices[i];
		loop.edge = _find_or_create_edge(p_vertices[i], p_vertices[(i + 1) % p_vertices.size()]);
		loop.face = face.id;
		loops.insert(loop.id, loop);
		face.loops.push_back(loop.id);
		edges[loop.edge].loops.push_back(loop.id);
	}
	for (int i = 0; i < face.loops.size(); i++) {
		Loop &loop = loops[face.loops[i]];
		loop.previous = face.loops[(i + face.loops.size() - 1) % face.loops.size()];
		loop.next = face.loops[(i + 1) % face.loops.size()];
	}
	faces.insert(face.id, face);
	return face.id;
}

bool SolersEditableMesh::remove_face(int64_t p_face_id, bool p_remove_orphans) {
	Face *face = faces.getptr(p_face_id);
	if (!face) {
		return false;
	}
	const Vector<int64_t> face_loops = face->loops;
	for (int64_t loop_id : face_loops) {
		const Loop *loop = loops.getptr(loop_id);
		if (loop) {
			_remove_loop_from_edge(loop->edge, loop_id);
		}
		loops.erase(loop_id);
	}
	faces.erase(p_face_id);
	if (p_remove_orphans) {
		_remove_orphan_elements();
	}
	return true;
}

bool SolersEditableMesh::remove_edge(int64_t p_edge_id, bool p_remove_faces, String *r_error) {
	const Edge *edge = edges.getptr(p_edge_id);
	if (!edge) {
		if (r_error) {
			*r_error = vformat("Edge %d does not exist.", p_edge_id);
		}
		return false;
	}
	if (!edge->loops.is_empty() && !p_remove_faces) {
		if (r_error) {
			*r_error = "Deleting this edge also requires deleting its incident faces.";
		}
		return false;
	}
	HashSet<int64_t> incident_faces;
	for (int64_t loop_id : edge->loops) {
		const Loop *loop = loops.getptr(loop_id);
		if (loop) {
			incident_faces.insert(loop->face);
		}
	}
	for (int64_t face_id : incident_faces) {
		remove_face(face_id, false);
	}
	const Edge *remaining = edges.getptr(p_edge_id);
	if (remaining) {
		edge_lookup.erase(EdgeKey(remaining->vertex_a, remaining->vertex_b));
		edges.erase(p_edge_id);
	}
	_remove_orphan_elements();
	return true;
}

bool SolersEditableMesh::remove_vertex(int64_t p_vertex_id, bool p_remove_faces, String *r_error) {
	if (!vertices.has(p_vertex_id)) {
		if (r_error) {
			*r_error = vformat("Vertex %d does not exist.", p_vertex_id);
		}
		return false;
	}
	const Vector<int64_t> incident_edges = get_vertex_edges(p_vertex_id);
	if (!incident_edges.is_empty() && !p_remove_faces) {
		if (r_error) {
			*r_error = "Deleting this vertex also requires deleting its incident faces.";
		}
		return false;
	}
	for (int64_t edge_id : incident_edges) {
		if (edges.has(edge_id)) {
			remove_edge(edge_id, true);
		}
	}
	vertices.erase(p_vertex_id);
	return true;
}

bool SolersEditableMesh::reverse_face(int64_t p_face_id, String *r_error) {
	Face *face = faces.getptr(p_face_id);
	if (!face) {
		if (r_error) {
			*r_error = vformat("Face %d does not exist.", p_face_id);
		}
		return false;
	}
	Vector<int64_t> reversed_vertices;
	Vector<Vector2> reversed_uvs;
	for (int i = face->loops.size() - 1; i >= 0; i--) {
		const Loop &loop = loops[face->loops[i]];
		reversed_vertices.push_back(loop.vertex);
		reversed_uvs.push_back(loop.uv);
	}
	for (int64_t loop_id : face->loops) {
		_remove_loop_from_edge(loops[loop_id].edge, loop_id);
	}
	for (int i = 0; i < face->loops.size(); i++) {
		Loop &loop = loops[face->loops[i]];
		loop.vertex = reversed_vertices[i];
		loop.uv = reversed_uvs[i];
		loop.edge = _find_or_create_edge(reversed_vertices[i], reversed_vertices[(i + 1) % reversed_vertices.size()]);
		loop.previous = face->loops[(i + face->loops.size() - 1) % face->loops.size()];
		loop.next = face->loops[(i + 1) % face->loops.size()];
		edges[loop.edge].loops.push_back(loop.id);
	}
	_remove_orphan_elements();
	return true;
}

Vector<int64_t> SolersEditableMesh::get_vertex_ids() const {
	return _sorted_ids(vertices);
}

Vector<int64_t> SolersEditableMesh::get_edge_ids() const {
	return _sorted_element_ids(edges);
}

Vector<int64_t> SolersEditableMesh::get_loop_ids() const {
	return _sorted_element_ids(loops);
}

Vector<int64_t> SolersEditableMesh::get_face_ids() const {
	return _sorted_element_ids(faces);
}

Vector<int64_t> SolersEditableMesh::get_face_vertices(int64_t p_face_id) const {
	Vector<int64_t> result;
	const Face *face = faces.getptr(p_face_id);
	if (!face) {
		return result;
	}
	for (int64_t loop_id : face->loops) {
		const Loop *loop = loops.getptr(loop_id);
		if (loop) {
			result.push_back(loop->vertex);
		}
	}
	return result;
}

Vector<int64_t> SolersEditableMesh::get_vertex_edges(int64_t p_vertex_id) const {
	Vector<int64_t> result;
	for (const KeyValue<int64_t, Edge> &entry : edges) {
		if (entry.value.vertex_a == p_vertex_id || entry.value.vertex_b == p_vertex_id) {
			result.push_back(entry.key);
		}
	}
	result.sort();
	return result;
}

Vector<int64_t> SolersEditableMesh::get_vertex_faces(int64_t p_vertex_id) const {
	HashSet<int64_t> unique;
	for (const KeyValue<int64_t, Loop> &entry : loops) {
		if (entry.value.vertex == p_vertex_id) {
			unique.insert(entry.value.face);
		}
	}
	Vector<int64_t> result;
	for (int64_t face_id : unique) {
		result.push_back(face_id);
	}
	result.sort();
	return result;
}

Vector<int64_t> SolersEditableMesh::get_boundary_edges() const {
	Vector<int64_t> result;
	for (const KeyValue<int64_t, Edge> &entry : edges) {
		if (entry.value.loops.size() == 1) {
			result.push_back(entry.key);
		}
	}
	result.sort();
	return result;
}

void SolersEditableMesh::clear_selection() {
	for (KeyValue<int64_t, Vertex> &entry : vertices) {
		entry.value.selected = false;
	}
	for (KeyValue<int64_t, Edge> &entry : edges) {
		entry.value.selected = false;
	}
	for (KeyValue<int64_t, Face> &entry : faces) {
		entry.value.selected = false;
	}
}

void SolersEditableMesh::select_vertices(const Vector<int64_t> &p_ids, bool p_replace) {
	if (p_replace) {
		clear_selection();
	}
	for (int64_t id : p_ids) {
		if (Vertex *vertex = vertices.getptr(id)) {
			vertex->selected = true;
		}
	}
}

void SolersEditableMesh::select_edges(const Vector<int64_t> &p_ids, bool p_replace) {
	if (p_replace) {
		clear_selection();
	}
	for (int64_t id : p_ids) {
		if (Edge *edge = edges.getptr(id)) {
			edge->selected = true;
		}
	}
}

void SolersEditableMesh::select_faces(const Vector<int64_t> &p_ids, bool p_replace) {
	if (p_replace) {
		clear_selection();
	}
	for (int64_t id : p_ids) {
		if (Face *face = faces.getptr(id)) {
			face->selected = true;
		}
	}
}

Vector<int64_t> SolersEditableMesh::get_selected_vertices() const {
	Vector<int64_t> result;
	for (const KeyValue<int64_t, Vertex> &entry : vertices) {
		if (entry.value.selected) {
			result.push_back(entry.key);
		}
	}
	result.sort();
	return result;
}

Vector<int64_t> SolersEditableMesh::get_selected_edges() const {
	Vector<int64_t> result;
	for (const KeyValue<int64_t, Edge> &entry : edges) {
		if (entry.value.selected) {
			result.push_back(entry.key);
		}
	}
	result.sort();
	return result;
}

Vector<int64_t> SolersEditableMesh::get_selected_faces() const {
	Vector<int64_t> result;
	for (const KeyValue<int64_t, Face> &entry : faces) {
		if (entry.value.selected) {
			result.push_back(entry.key);
		}
	}
	result.sort();
	return result;
}

Vector<int64_t> SolersEditableMesh::expand_vertices_from_selection() const {
	HashSet<int64_t> selected;
	for (int64_t vertex_id : get_selected_vertices()) {
		selected.insert(vertex_id);
	}
	for (int64_t edge_id : get_selected_edges()) {
		const Edge *edge = edges.getptr(edge_id);
		if (edge) {
			selected.insert(edge->vertex_a);
			selected.insert(edge->vertex_b);
		}
	}
	for (int64_t face_id : get_selected_faces()) {
		for (int64_t vertex_id : get_face_vertices(face_id)) {
			selected.insert(vertex_id);
		}
	}
	Vector<int64_t> result;
	for (int64_t vertex_id : selected) {
		result.push_back(vertex_id);
	}
	result.sort();
	return result;
}

bool SolersEditableMesh::transform_vertices(const Vector<int64_t> &p_vertex_ids, const Transform3D &p_transform, String *r_error) {
	for (int64_t vertex_id : p_vertex_ids) {
		if (!vertices.has(vertex_id)) {
			if (r_error) {
				*r_error = vformat("Vertex %d does not exist.", vertex_id);
			}
			return false;
		}
	}
	for (int64_t vertex_id : p_vertex_ids) {
		Vertex &vertex = vertices[vertex_id];
		vertex.position = p_transform.xform(vertex.position);
	}
	return true;
}

void SolersEditableMesh::set_edge_seam(const Vector<int64_t> &p_edge_ids, bool p_enabled) {
	for (int64_t id : p_edge_ids) {
		if (Edge *edge = edges.getptr(id)) {
			edge->seam = p_enabled;
		}
	}
}

void SolersEditableMesh::set_edge_sharp(const Vector<int64_t> &p_edge_ids, bool p_enabled) {
	for (int64_t id : p_edge_ids) {
		if (Edge *edge = edges.getptr(id)) {
			edge->sharp = p_enabled;
		}
	}
}

void SolersEditableMesh::set_faces_smooth(const Vector<int64_t> &p_face_ids, bool p_enabled) {
	for (int64_t id : p_face_ids) {
		if (Face *face = faces.getptr(id)) {
			face->smooth = p_enabled;
		}
	}
}

void SolersEditableMesh::set_face_material(const Vector<int64_t> &p_face_ids, int p_material) {
	for (int64_t id : p_face_ids) {
		if (Face *face = faces.getptr(id)) {
			face->material = MAX(0, p_material);
		}
	}
}

void SolersEditableMesh::set_loop_uv(int64_t p_loop_id, const Vector2 &p_uv) {
	if (Loop *loop = loops.getptr(p_loop_id)) {
		loop->uv = p_uv;
	}
}

int SolersEditableMesh::add_material_path(const String &p_path) {
	const int existing = material_paths.find(p_path);
	if (existing >= 0) {
		return existing;
	}
	material_paths.push_back(p_path);
	return material_paths.size() - 1;
}

int64_t SolersEditableMesh::add_modifier(const StringName &p_type, const Dictionary &p_parameters) {
	Modifier modifier;
	modifier.id = _allocate_id();
	modifier.type = p_type;
	modifier.parameters = p_parameters.duplicate(true);
	modifiers.push_back(modifier);
	return modifier.id;
}

bool SolersEditableMesh::remove_modifier(int64_t p_id) {
	for (int i = 0; i < modifiers.size(); i++) {
		if (modifiers[i].id == p_id) {
			modifiers.remove_at(i);
			return true;
		}
	}
	return false;
}

Dictionary SolersEditableMesh::to_dictionary() const {
	Dictionary data;
	data["type"] = "solers_model";
	data["version"] = (int64_t)FORMAT_VERSION;
	data["revision"] = revision;
	data["next_id"] = next_id;

	Array vertex_array;
	for (int64_t id : get_vertex_ids()) {
		const Vertex &vertex = vertices[id];
		Dictionary item;
		item["id"] = vertex.id;
		item["position"] = vertex.position;
		item["selected"] = vertex.selected;
		vertex_array.push_back(item);
	}
	data["vertices"] = vertex_array;

	Array edge_array;
	for (int64_t id : get_edge_ids()) {
		const Edge &edge = edges[id];
		Dictionary item;
		item["id"] = edge.id;
		item["a"] = edge.vertex_a;
		item["b"] = edge.vertex_b;
		item["seam"] = edge.seam;
		item["sharp"] = edge.sharp;
		item["selected"] = edge.selected;
		edge_array.push_back(item);
	}
	data["edges"] = edge_array;

	Array loop_array;
	for (int64_t id : get_loop_ids()) {
		const Loop &loop = loops[id];
		Dictionary item;
		item["id"] = loop.id;
		item["vertex"] = loop.vertex;
		item["edge"] = loop.edge;
		item["face"] = loop.face;
		item["next"] = loop.next;
		item["previous"] = loop.previous;
		item["uv"] = loop.uv;
		loop_array.push_back(item);
	}
	data["loops"] = loop_array;

	Array face_array;
	for (int64_t id : get_face_ids()) {
		const Face &face = faces[id];
		Dictionary item;
		item["id"] = face.id;
		item["loops"] = _ids_to_array(face.loops);
		item["material"] = face.material;
		item["smooth"] = face.smooth;
		item["selected"] = face.selected;
		face_array.push_back(item);
	}
	data["faces"] = face_array;

	Array materials;
	for (const String &path : material_paths) {
		materials.push_back(path);
	}
	data["materials"] = materials;

	Array modifier_array;
	for (const Modifier &modifier : modifiers) {
		Dictionary item;
		item["id"] = modifier.id;
		item["type"] = String(modifier.type);
		item["parameters"] = modifier.parameters.duplicate(true);
		item["enabled"] = modifier.enabled;
		modifier_array.push_back(item);
	}
	data["modifiers"] = modifier_array;
	data["build_settings"] = build_settings.duplicate(true);
	return data;
}

static bool _require_array(const Dictionary &p_data, const StringName &p_key, Array &r_array, String *r_error) {
	const Variant value = p_data.get(p_key, Variant());
	if (value.get_type() != Variant::ARRAY) {
		if (r_error) {
			*r_error = vformat("Model field '%s' must be an array.", p_key);
		}
		return false;
	}
	r_array = value;
	return true;
}

Error SolersEditableMesh::from_dictionary(const Dictionary &p_data, String *r_error) {
	if (String(p_data.get("type", String())) != "solers_model" || (int64_t)p_data.get("version", 0) != FORMAT_VERSION) {
		if (r_error) {
			*r_error = "Unsupported or invalid Solers model source.";
		}
		return ERR_FILE_UNRECOGNIZED;
	}
	Array vertex_array;
	Array edge_array;
	Array loop_array;
	Array face_array;
	Array materials;
	Array modifier_array;
	if (!_require_array(p_data, "vertices", vertex_array, r_error) || !_require_array(p_data, "edges", edge_array, r_error) ||
			!_require_array(p_data, "loops", loop_array, r_error) || !_require_array(p_data, "faces", face_array, r_error) ||
			!_require_array(p_data, "materials", materials, r_error) || !_require_array(p_data, "modifiers", modifier_array, r_error)) {
		return ERR_INVALID_DATA;
	}

	clear();
	revision = p_data.get("revision", 0);
	next_id = p_data.get("next_id", 1);
	for (const Variant &value : vertex_array) {
		if (value.get_type() != Variant::DICTIONARY) {
			return ERR_INVALID_DATA;
		}
		const Dictionary item = value;
		Vertex vertex;
		vertex.id = item.get("id", 0);
		vertex.position = item.get("position", Vector3());
		vertex.selected = item.get("selected", false);
		vertices.insert(vertex.id, vertex);
	}
	for (const Variant &value : edge_array) {
		if (value.get_type() != Variant::DICTIONARY) {
			return ERR_INVALID_DATA;
		}
		const Dictionary item = value;
		Edge edge;
		edge.id = item.get("id", 0);
		edge.vertex_a = item.get("a", 0);
		edge.vertex_b = item.get("b", 0);
		edge.seam = item.get("seam", false);
		edge.sharp = item.get("sharp", false);
		edge.selected = item.get("selected", false);
		edges.insert(edge.id, edge);
	}
	for (const Variant &value : loop_array) {
		if (value.get_type() != Variant::DICTIONARY) {
			return ERR_INVALID_DATA;
		}
		const Dictionary item = value;
		Loop loop;
		loop.id = item.get("id", 0);
		loop.vertex = item.get("vertex", 0);
		loop.edge = item.get("edge", 0);
		loop.face = item.get("face", 0);
		loop.next = item.get("next", 0);
		loop.previous = item.get("previous", 0);
		loop.uv = item.get("uv", Vector2());
		loops.insert(loop.id, loop);
	}
	for (const Variant &value : face_array) {
		if (value.get_type() != Variant::DICTIONARY) {
			return ERR_INVALID_DATA;
		}
		const Dictionary item = value;
		Face face;
		face.id = item.get("id", 0);
		const Array item_loops = item.get("loops", Array());
		for (const Variant &loop_id : item_loops) {
			face.loops.push_back(loop_id);
		}
		face.material = item.get("material", 0);
		face.smooth = item.get("smooth", false);
		face.selected = item.get("selected", false);
		faces.insert(face.id, face);
	}
	for (const Variant &value : materials) {
		if (value.get_type() != Variant::STRING) {
			return ERR_INVALID_DATA;
		}
		material_paths.push_back(value);
	}
	for (const Variant &value : modifier_array) {
		if (value.get_type() != Variant::DICTIONARY) {
			return ERR_INVALID_DATA;
		}
		const Dictionary item = value;
		Modifier modifier;
		modifier.id = item.get("id", 0);
		modifier.type = StringName(item.get("type", String()));
		modifier.parameters = item.get("parameters", Dictionary());
		modifier.enabled = item.get("enabled", true);
		modifiers.push_back(modifier);
	}
	const Variant settings = p_data.get("build_settings", Dictionary());
	if (settings.get_type() != Variant::DICTIONARY) {
		if (r_error) {
			*r_error = "Model field 'build_settings' must be an object.";
		}
		return ERR_INVALID_DATA;
	}
	build_settings = ((Dictionary)settings).duplicate(true);
	_rebuild_indices();
	for (KeyValue<int64_t, Loop> &entry : loops) {
		Edge *edge = edges.getptr(entry.value.edge);
		if (edge) {
			edge->loops.push_back(entry.key);
		}
	}
	return validate(r_error);
}

Error SolersEditableMesh::validate(String *r_error) const {
	auto fail = [r_error](const String &p_message) -> Error {
		if (r_error) {
			*r_error = p_message;
		}
		return ERR_INVALID_DATA;
	};
	if (next_id <= 0) {
		return fail("The next persistent ID must be positive.");
	}
	HashSet<int64_t> all_ids;
	int64_t max_id = 0;
	for (const KeyValue<int64_t, Vertex> &entry : vertices) {
		if (entry.key <= 0 || entry.key != entry.value.id || all_ids.has(entry.key)) {
			return fail("Vertex IDs must be positive and unique.");
		}
		if (!entry.value.position.is_finite()) {
			return fail(vformat("Vertex %d has a non-finite position.", entry.key));
		}
		all_ids.insert(entry.key);
		max_id = MAX(max_id, entry.key);
	}
	HashSet<EdgeKey, EdgeKey> unique_edges;
	for (const KeyValue<int64_t, Edge> &entry : edges) {
		const Edge &edge = entry.value;
		if (entry.key <= 0 || entry.key != edge.id || all_ids.has(entry.key) || edge.vertex_a == edge.vertex_b || !vertices.has(edge.vertex_a) || !vertices.has(edge.vertex_b)) {
			return fail(vformat("Edge %d is invalid.", entry.key));
		}
		const EdgeKey key(edge.vertex_a, edge.vertex_b);
		if (unique_edges.has(key)) {
			return fail(vformat("Edge %d duplicates an existing edge.", entry.key));
		}
		unique_edges.insert(key);
		all_ids.insert(entry.key);
		max_id = MAX(max_id, entry.key);
	}
	for (const KeyValue<int64_t, Loop> &entry : loops) {
		const Loop &loop = entry.value;
		if (entry.key <= 0 || entry.key != loop.id || all_ids.has(entry.key) || !vertices.has(loop.vertex) || !edges.has(loop.edge) || !faces.has(loop.face) || !loops.has(loop.next) || !loops.has(loop.previous) || !loop.uv.is_finite()) {
			return fail(vformat("Loop %d is invalid.", entry.key));
		}
		const Edge &edge = edges[loop.edge];
		const Loop &next = loops[loop.next];
		if (loop.face != next.face || loops[loop.previous].next != loop.id || loops[loop.next].previous != loop.id ||
				!((edge.vertex_a == loop.vertex && edge.vertex_b == next.vertex) || (edge.vertex_b == loop.vertex && edge.vertex_a == next.vertex))) {
			return fail(vformat("Loop %d has inconsistent adjacency.", entry.key));
		}
		if (!edge.loops.has(loop.id)) {
			return fail(vformat("Edge %d is missing radial loop %d.", edge.id, loop.id));
		}
		all_ids.insert(entry.key);
		max_id = MAX(max_id, entry.key);
	}
	for (const KeyValue<int64_t, Face> &entry : faces) {
		const Face &face = entry.value;
		if (entry.key <= 0 || entry.key != face.id || all_ids.has(entry.key) || face.loops.size() < 3) {
			return fail(vformat("Face %d is invalid.", entry.key));
		}
		HashSet<int64_t> face_vertices;
		for (int i = 0; i < face.loops.size(); i++) {
			const Loop *loop = loops.getptr(face.loops[i]);
			if (!loop || loop->face != face.id || loop->next != face.loops[(i + 1) % face.loops.size()] || face_vertices.has(loop->vertex)) {
				return fail(vformat("Face %d has an invalid loop ring.", face.id));
			}
			face_vertices.insert(loop->vertex);
		}
		all_ids.insert(entry.key);
		max_id = MAX(max_id, entry.key);
	}
	for (const Modifier &modifier : modifiers) {
		if (modifier.id <= 0 || modifier.type.is_empty() || all_ids.has(modifier.id)) {
			return fail("Modifier IDs and types must be valid and unique.");
		}
		all_ids.insert(modifier.id);
		max_id = MAX(max_id, modifier.id);
	}
	if (next_id <= max_id) {
		return fail("The next persistent ID must exceed every stored element ID.");
	}
	return OK;
}

Dictionary SolersEditableMesh::inspect() const {
	Dictionary result;
	result["format_version"] = (int64_t)FORMAT_VERSION;
	result["revision"] = revision;
	result["vertex_count"] = vertices.size();
	result["edge_count"] = edges.size();
	result["loop_count"] = loops.size();
	result["face_count"] = faces.size();
	result["boundary_edge_count"] = get_boundary_edges().size();
	int non_manifold_edges = 0;
	for (const KeyValue<int64_t, Edge> &entry : edges) {
		if (entry.value.loops.size() > 2) {
			non_manifold_edges++;
		}
	}
	result["non_manifold_edge_count"] = non_manifold_edges;
	result["materials"] = material_paths.size();
	result["modifiers"] = modifiers.size();
	result["build_settings"] = build_settings.duplicate(true);
	result["selected_vertices"] = _ids_to_array(get_selected_vertices());
	result["selected_edges"] = _ids_to_array(get_selected_edges());
	result["selected_faces"] = _ids_to_array(get_selected_faces());
	return result;
}

static Vector3 _face_weighted_normal(const SolersEditableMesh &p_mesh, const SolersEditableMesh::Face &p_face) {
	Vector3 normal;
	for (int i = 0; i < p_face.loops.size(); i++) {
		const SolersEditableMesh::Loop *current_loop = p_mesh.get_loop(p_face.loops[i]);
		const SolersEditableMesh::Loop *next_loop = p_mesh.get_loop(p_face.loops[(i + 1) % p_face.loops.size()]);
		if (!current_loop || !next_loop) {
			continue;
		}
		const Vector3 current = p_mesh.get_vertex(current_loop->vertex)->position;
		const Vector3 next = p_mesh.get_vertex(next_loop->vertex)->position;
		normal.x += (current.y - next.y) * (current.z + next.z);
		normal.y += (current.z - next.z) * (current.x + next.x);
		normal.z += (current.x - next.x) * (current.y + next.y);
	}
	return normal;
}

struct _NormalCornerKey {
	int64_t face = 0;
	int64_t vertex = 0;

	static uint32_t hash(const _NormalCornerKey &p_key) {
		return hash_one_uint64((uint64_t)p_key.face) ^ hash_murmur3_one_64((uint64_t)p_key.vertex);
	}

	bool operator==(const _NormalCornerKey &p_other) const {
		return face == p_other.face && vertex == p_other.vertex;
	}
};

static Vector3 _corner_normal(const SolersEditableMesh &p_mesh, const SolersEditableMesh::Face &p_face, int64_t p_vertex, bool p_weighted) {
	const Vector3 flat = _face_weighted_normal(p_mesh, p_face).normalized();
	if (!p_face.smooth) {
		return flat;
	}
	HashSet<int64_t> visited;
	Vector<int64_t> pending;
	visited.insert(p_face.id);
	pending.push_back(p_face.id);
	Vector3 normal;
	while (!pending.is_empty()) {
		const int64_t face_id = pending[pending.size() - 1];
		pending.remove_at(pending.size() - 1);
		const SolersEditableMesh::Face *face = p_mesh.get_face(face_id);
		const Vector3 face_normal = _face_weighted_normal(p_mesh, *face);
		normal += p_weighted ? face_normal : face_normal.normalized();
		for (int64_t loop_id : face->loops) {
			const SolersEditableMesh::Loop *loop = p_mesh.get_loop(loop_id);
			const SolersEditableMesh::Loop *next = p_mesh.get_loop(loop->next);
			if (loop->vertex != p_vertex && next->vertex != p_vertex) {
				continue;
			}
			const SolersEditableMesh::Edge *edge = p_mesh.get_edge(loop->edge);
			if (edge->sharp) {
				continue;
			}
			for (int64_t radial_loop : edge->loops) {
				const int64_t neighbor_id = p_mesh.get_loop(radial_loop)->face;
				const SolersEditableMesh::Face *neighbor = p_mesh.get_face(neighbor_id);
				if (neighbor && neighbor->smooth && !visited.has(neighbor_id)) {
					visited.insert(neighbor_id);
					pending.push_back(neighbor_id);
				}
			}
		}
	}
	return normal.is_zero_approx() ? flat : normal.normalized();
}

static PackedVector2Array _project_face(const SolersEditableMesh &p_mesh, const SolersEditableMesh::Face &p_face, const Vector3 &p_normal) {
	PackedVector2Array polygon;
	const Vector3 axis = p_normal.abs();
	for (int64_t loop_id : p_face.loops) {
		const SolersEditableMesh::Loop *loop = p_mesh.get_loop(loop_id);
		const Vector3 point = p_mesh.get_vertex(loop->vertex)->position;
		if (axis.x >= axis.y && axis.x >= axis.z) {
			polygon.push_back(Vector2(point.y, point.z));
		} else if (axis.y >= axis.z) {
			polygon.push_back(Vector2(point.x, point.z));
		} else {
			polygon.push_back(Vector2(point.x, point.y));
		}
	}
	return polygon;
}

static PackedInt32Array _generate_lod(const Array &p_arrays, double p_ratio) {
	PackedInt32Array result;
	if (!SurfaceTool::simplify_with_attrib_func) {
		return result;
	}
	const PackedVector3Array vertices = p_arrays[Mesh::ARRAY_VERTEX];
	const PackedVector3Array normals = p_arrays[Mesh::ARRAY_NORMAL];
	const PackedVector2Array uvs = p_arrays[Mesh::ARRAY_TEX_UV];
	const PackedInt32Array indices = p_arrays[Mesh::ARRAY_INDEX];
	if (vertices.is_empty() || normals.size() != vertices.size() || uvs.size() != vertices.size() || indices.size() < 6) {
		return result;
	}
	Vector<float> attributes;
	Vector<float> positions;
	attributes.resize(vertices.size() * 5);
	positions.resize(vertices.size() * 3);
	for (int i = 0; i < vertices.size(); i++) {
		positions.write[i * 3] = vertices[i].x;
		positions.write[i * 3 + 1] = vertices[i].y;
		positions.write[i * 3 + 2] = vertices[i].z;
		attributes.write[i * 5] = normals[i].x;
		attributes.write[i * 5 + 1] = normals[i].y;
		attributes.write[i * 5 + 2] = normals[i].z;
		attributes.write[i * 5 + 3] = uvs[i].x;
		attributes.write[i * 5 + 4] = uvs[i].y;
	}
	Vector<unsigned int> simplified;
	simplified.resize(indices.size());
	const float weights[5] = { 1.0f, 1.0f, 1.0f, 0.5f, 0.5f };
	const int target = MAX(3, ((int)(indices.size() * CLAMP(p_ratio, 0.01, 0.99)) / 3) * 3);
	float simplification_error = 0.0f;
	const size_t count = SurfaceTool::simplify_with_attrib_func(
			simplified.ptrw(), reinterpret_cast<const unsigned int *>(indices.ptr()), indices.size(),
			positions.ptr(), vertices.size(), sizeof(float) * 3,
			attributes.ptr(), sizeof(float) * 5, weights, 5, nullptr, target, 1.0f,
			SurfaceTool::SIMPLIFY_LOCK_BORDER, &simplification_error);
	if (count < 3 || count >= (size_t)indices.size() || count % 3 != 0) {
		return result;
	}
	result.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		result.set(i, simplified[i]);
	}
	return result;
}

Ref<ArrayMesh> SolersEditableMesh::compile(String *r_error) const {
	String validation_error;
	if (validate(&validation_error) != OK) {
		if (r_error) {
			*r_error = validation_error;
		}
		return Ref<ArrayMesh>();
	}
	if (!modifiers.is_empty()) {
		SolersEditableMesh evaluated;
		if (SolersModelModifierEvaluator::evaluate(*this, evaluated, r_error) != OK) {
			return Ref<ArrayMesh>();
		}
		return evaluated.compile(r_error);
	}

	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	const bool weighted_normals = build_settings.get("weighted_normals", true);
	HashMap<_NormalCornerKey, Vector3, _NormalCornerKey> corner_normals;
	HashMap<int, Vector<int64_t>> faces_by_material;
	for (int64_t face_id : get_face_ids()) {
		faces_by_material[faces[face_id].material].push_back(face_id);
	}
	Vector<int> material_indices;
	for (const KeyValue<int, Vector<int64_t>> &entry : faces_by_material) {
		material_indices.push_back(entry.key);
	}
	material_indices.sort();

	for (int material_index : material_indices) {
		Ref<SurfaceTool> surface;
		surface.instantiate();
		surface->begin(Mesh::PRIMITIVE_TRIANGLES);
		for (int64_t face_id : faces_by_material[material_index]) {
			const Face &face = faces[face_id];
			const Vector3 normal = _face_weighted_normal(*this, face).normalized();
			if (normal.is_zero_approx()) {
				if (r_error) {
					*r_error = vformat("Face %d has zero area.", face.id);
				}
				return Ref<ArrayMesh>();
			}
			const PackedVector2Array polygon = _project_face(*this, face, normal);
			const PackedInt32Array triangles = Geometry2D::triangulate_polygon(polygon);
			if (triangles.size() < 3) {
				if (r_error) {
					*r_error = vformat("Face %d could not be triangulated.", face.id);
				}
				return Ref<ArrayMesh>();
			}
			for (int index : triangles) {
				const Loop &loop = loops[face.loops[index]];
				const _NormalCornerKey normal_key{ face.id, loop.vertex };
				if (!corner_normals.has(normal_key)) {
					corner_normals.insert(normal_key, _corner_normal(*this, face, loop.vertex, weighted_normals));
				}
				surface->set_uv(loop.uv);
				surface->set_normal(corner_normals[normal_key]);
				surface->add_vertex(vertices[loop.vertex].position);
			}
		}
		if ((bool)build_settings.get("generate_tangents", true)) {
			surface->generate_tangents();
		}
		surface->index();
		surface->optimize_indices_for_cache();
		const Array arrays = surface->commit_to_arrays();
		Dictionary lods;
		const Array lod_levels = build_settings.get("lod_levels", Array());
		for (const Variant &level_value : lod_levels) {
			if (level_value.get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary level = level_value;
			const double ratio = level.get("ratio", 0.5);
			const double distance = level.get("distance", 10.0);
			const PackedInt32Array generated = _generate_lod(arrays, ratio);
			if (!generated.is_empty()) {
				lods[distance] = generated;
			}
		}
		const int previous_surfaces = mesh->get_surface_count();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), lods);
		if (mesh->get_surface_count() != previous_surfaces + 1) {
			if (r_error) {
				*r_error = "ArrayMesh compilation failed.";
			}
			return Ref<ArrayMesh>();
		}
		if (material_index >= 0 && material_index < material_paths.size() && !material_paths[material_index].is_empty()) {
			Ref<Material> material = ResourceLoader::load(material_paths[material_index]);
			if (material.is_valid()) {
				mesh->surface_set_material(mesh->get_surface_count() - 1, material);
			}
		}
	}
	if ((bool)build_settings.get("generate_uv2", false) && mesh->get_surface_count() > 0) {
		const float texel_size = build_settings.get("lightmap_texel_size", 0.05);
		if (mesh->lightmap_unwrap(Transform3D(), texel_size) != OK) {
			if (r_error) {
				*r_error = "xatlas could not generate lightmap UV2 coordinates.";
			}
			return Ref<ArrayMesh>();
		}
	}
	const String collision_mode = build_settings.get("collision", "none");
	if (collision_mode == "trimesh" && mesh->get_surface_count() > 0) {
		mesh->set_meta("solers_collision_shape", mesh->create_trimesh_shape());
	} else if (collision_mode == "convex" && mesh->get_surface_count() > 0) {
		mesh->set_meta("solers_collision_shape", mesh->create_convex_shape());
	}
	mesh->set_meta("solers_build_settings", build_settings.duplicate(true));
	return mesh;
}
