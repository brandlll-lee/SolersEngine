/**************************************************************************/
/*  solers_editable_mesh.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers' editable polygon topology. Runtime meshes remain ordinary      */
/* ArrayMesh resources; this data exists only in editor source files.     */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/hashfuncs.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

class ArrayMesh;

class SolersEditableMesh {
public:
	static constexpr uint32_t FORMAT_VERSION = 1;

	struct EdgeKey {
		int64_t a = 0;
		int64_t b = 0;

		static uint32_t hash(const EdgeKey &p_key) {
			return hash_one_uint64((uint64_t)p_key.a) ^ hash_murmur3_one_64((uint64_t)p_key.b);
		}

		bool operator==(const EdgeKey &p_other) const {
			return a == p_other.a && b == p_other.b;
		}

		EdgeKey() = default;
		EdgeKey(int64_t p_a, int64_t p_b) {
			a = MIN(p_a, p_b);
			b = MAX(p_a, p_b);
		}
	};

	struct Vertex {
		int64_t id = 0;
		Vector3 position;
		bool selected = false;
	};

	struct Edge {
		int64_t id = 0;
		int64_t vertex_a = 0;
		int64_t vertex_b = 0;
		Vector<int64_t> loops;
		bool seam = false;
		bool sharp = false;
		bool selected = false;
	};

	struct Loop {
		int64_t id = 0;
		int64_t vertex = 0;
		int64_t edge = 0;
		int64_t face = 0;
		int64_t next = 0;
		int64_t previous = 0;
		Vector2 uv;
	};

	struct Face {
		int64_t id = 0;
		Vector<int64_t> loops;
		int material = 0;
		bool smooth = false;
		bool selected = false;
	};

	struct Modifier {
		int64_t id = 0;
		StringName type;
		Dictionary parameters;
		bool enabled = true;
	};

private:
	int64_t revision = 0;
	int64_t next_id = 1;
	HashMap<int64_t, Vertex> vertices;
	HashMap<int64_t, Edge> edges;
	HashMap<int64_t, Loop> loops;
	HashMap<int64_t, Face> faces;
	HashMap<EdgeKey, int64_t, EdgeKey> edge_lookup;
	Vector<String> material_paths;
	Vector<Modifier> modifiers;

	int64_t _allocate_id();
	void _rebuild_indices();
	int64_t _find_or_create_edge(int64_t p_vertex_a, int64_t p_vertex_b);
	void _remove_loop_from_edge(int64_t p_edge_id, int64_t p_loop_id);
	void _remove_orphan_elements();

public:
	void clear();
	bool is_empty() const { return vertices.is_empty() && faces.is_empty(); }

	int64_t get_revision() const { return revision; }
	void set_revision(int64_t p_revision) { revision = MAX((int64_t)0, p_revision); }
	void increment_revision() { revision++; }

	int64_t add_vertex(const Vector3 &p_position);
	int64_t add_face(const Vector<int64_t> &p_vertices, int p_material = 0, bool p_smooth = false, String *r_error = nullptr);
	bool remove_face(int64_t p_face_id, bool p_remove_orphans = true);
	bool remove_edge(int64_t p_edge_id, bool p_remove_faces, String *r_error = nullptr);
	bool remove_vertex(int64_t p_vertex_id, bool p_remove_faces, String *r_error = nullptr);
	void remove_orphan_elements() { _remove_orphan_elements(); }
	bool reverse_face(int64_t p_face_id, String *r_error = nullptr);

	Vertex *get_vertex(int64_t p_id) { return vertices.getptr(p_id); }
	const Vertex *get_vertex(int64_t p_id) const { return vertices.getptr(p_id); }
	Edge *get_edge(int64_t p_id) { return edges.getptr(p_id); }
	const Edge *get_edge(int64_t p_id) const { return edges.getptr(p_id); }
	Loop *get_loop(int64_t p_id) { return loops.getptr(p_id); }
	const Loop *get_loop(int64_t p_id) const { return loops.getptr(p_id); }
	Face *get_face(int64_t p_id) { return faces.getptr(p_id); }
	const Face *get_face(int64_t p_id) const { return faces.getptr(p_id); }

	Vector<int64_t> get_vertex_ids() const;
	Vector<int64_t> get_edge_ids() const;
	Vector<int64_t> get_loop_ids() const;
	Vector<int64_t> get_face_ids() const;
	Vector<int64_t> get_face_vertices(int64_t p_face_id) const;
	Vector<int64_t> get_vertex_edges(int64_t p_vertex_id) const;
	Vector<int64_t> get_vertex_faces(int64_t p_vertex_id) const;
	Vector<int64_t> get_boundary_edges() const;

	void clear_selection();
	void select_vertices(const Vector<int64_t> &p_ids, bool p_replace = true);
	void select_edges(const Vector<int64_t> &p_ids, bool p_replace = true);
	void select_faces(const Vector<int64_t> &p_ids, bool p_replace = true);
	Vector<int64_t> get_selected_vertices() const;
	Vector<int64_t> get_selected_edges() const;
	Vector<int64_t> get_selected_faces() const;
	Vector<int64_t> expand_vertices_from_selection() const;

	bool transform_vertices(const Vector<int64_t> &p_vertex_ids, const Transform3D &p_transform, String *r_error = nullptr);
	void set_edge_seam(const Vector<int64_t> &p_edge_ids, bool p_enabled);
	void set_edge_sharp(const Vector<int64_t> &p_edge_ids, bool p_enabled);
	void set_faces_smooth(const Vector<int64_t> &p_face_ids, bool p_enabled);
	void set_face_material(const Vector<int64_t> &p_face_ids, int p_material);
	void set_loop_uv(int64_t p_loop_id, const Vector2 &p_uv);

	int add_material_path(const String &p_path);
	const Vector<String> &get_material_paths() const { return material_paths; }
	Vector<Modifier> &get_modifiers() { return modifiers; }
	const Vector<Modifier> &get_modifiers() const { return modifiers; }
	int64_t add_modifier(const StringName &p_type, const Dictionary &p_parameters);
	bool remove_modifier(int64_t p_id);

	Dictionary to_dictionary() const;
	Error from_dictionary(const Dictionary &p_data, String *r_error = nullptr);
	Error validate(String *r_error = nullptr) const;
	Dictionary inspect() const;

	Ref<ArrayMesh> compile(String *r_error = nullptr) const;
};
