/**************************************************************************/
/*  solers_model_uv.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_model_uv.h"

#include "core/math/geometry_2d.h"
#include "modules/solers_modeling/core/solers_editable_mesh.h"

#include <xatlas.h>

static void _set_uv_error(String *r_error, const String &p_message) {
	if (r_error) {
		*r_error = p_message;
	}
}

static Vector3 _uv_face_normal(const SolersEditableMesh &p_mesh, const SolersEditableMesh::Face &p_face) {
	Vector3 normal;
	for (int i = 0; i < p_face.loops.size(); i++) {
		const Vector3 current = p_mesh.get_vertex(p_mesh.get_loop(p_face.loops[i])->vertex)->position;
		const Vector3 next = p_mesh.get_vertex(p_mesh.get_loop(p_face.loops[(i + 1) % p_face.loops.size()])->vertex)->position;
		normal.x += (current.y - next.y) * (current.z + next.z);
		normal.y += (current.z - next.z) * (current.x + next.x);
		normal.z += (current.x - next.x) * (current.y + next.y);
	}
	return normal.normalized();
}

static HashMap<int64_t, uint32_t> _uv_chart_groups(const SolersEditableMesh &p_mesh) {
	HashMap<int64_t, uint32_t> groups;
	uint32_t next_group = 0;
	for (int64_t seed : p_mesh.get_face_ids()) {
		if (groups.has(seed)) {
			continue;
		}
		Vector<int64_t> pending;
		pending.push_back(seed);
		groups.insert(seed, next_group);
		while (!pending.is_empty()) {
			const int64_t face_id = pending[pending.size() - 1];
			pending.remove_at(pending.size() - 1);
			const SolersEditableMesh::Face *face = p_mesh.get_face(face_id);
			for (int64_t loop_id : face->loops) {
				const SolersEditableMesh::Edge *edge = p_mesh.get_edge(p_mesh.get_loop(loop_id)->edge);
				if (edge->seam) {
					continue;
				}
				for (int64_t radial_loop : edge->loops) {
					const int64_t neighbor = p_mesh.get_loop(radial_loop)->face;
					if (!groups.has(neighbor)) {
						groups.insert(neighbor, next_group);
						pending.push_back(neighbor);
					}
				}
			}
		}
		next_group++;
	}
	return groups;
}

Error SolersModelUV::unwrap(SolersEditableMesh &r_mesh, const Dictionary &p_options, bool p_use_input_uvs, String *r_error) {
	if (r_mesh.get_face_ids().is_empty()) {
		_set_uv_error(r_error, "UV generation requires at least one face.");
		return ERR_INVALID_DATA;
	}
	const int resolution = p_options.get("resolution", 1024);
	const int padding = p_options.get("padding", 4);
	const double texels_per_unit = p_options.get("texels_per_unit", 0.0);
	if (resolution < 16 || resolution > 16384 || padding < 0 || padding > 256 || !Math::is_finite(texels_per_unit) || texels_per_unit < 0) {
		_set_uv_error(r_error, "UV resolution, padding, or texel density is outside the supported range.");
		return ERR_INVALID_PARAMETER;
	}

	Vector<float> positions;
	Vector<float> normals;
	Vector<float> input_uvs;
	Vector<uint32_t> indices;
	Vector<uint8_t> face_vertex_counts;
	Vector<uint32_t> face_groups;
	Vector<int64_t> input_loops;
	const HashMap<int64_t, uint32_t> groups = _uv_chart_groups(r_mesh);
	for (int64_t face_id : r_mesh.get_face_ids()) {
		const SolersEditableMesh::Face *face = r_mesh.get_face(face_id);
		if (face->loops.size() > 255) {
			_set_uv_error(r_error, vformat("Face %d has more than 255 corners and cannot be unwrapped by xatlas.", face_id));
			return ERR_INVALID_DATA;
		}
		const Vector3 normal = _uv_face_normal(r_mesh, *face);
		if (normal.is_zero_approx()) {
			_set_uv_error(r_error, vformat("Face %d has zero area.", face_id));
			return ERR_INVALID_DATA;
		}
		face_vertex_counts.push_back(face->loops.size());
		face_groups.push_back(groups[face_id]);
		for (int64_t loop_id : face->loops) {
			const SolersEditableMesh::Loop *loop = r_mesh.get_loop(loop_id);
			const Vector3 position = r_mesh.get_vertex(loop->vertex)->position;
			positions.push_back(position.x);
			positions.push_back(position.y);
			positions.push_back(position.z);
			normals.push_back(normal.x);
			normals.push_back(normal.y);
			normals.push_back(normal.z);
			input_uvs.push_back(loop->uv.x);
			input_uvs.push_back(loop->uv.y);
			indices.push_back(indices.size());
			input_loops.push_back(loop_id);
		}
	}

	xatlas::MeshDecl input;
	input.vertexPositionData = positions.ptr();
	input.vertexPositionStride = sizeof(float) * 3;
	input.vertexNormalData = normals.ptr();
	input.vertexNormalStride = sizeof(float) * 3;
	input.vertexUvData = p_use_input_uvs ? input_uvs.ptr() : nullptr;
	input.vertexUvStride = p_use_input_uvs ? sizeof(float) * 2 : 0;
	input.vertexCount = input_loops.size();
	input.indexData = indices.ptr();
	input.indexCount = indices.size();
	input.indexFormat = xatlas::IndexFormat::UInt32;
	input.faceCount = face_vertex_counts.size();
	input.faceVertexCount = face_vertex_counts.ptr();
	input.faceMaterialData = face_groups.ptr();

	xatlas::Atlas *atlas = xatlas::Create();
	if (!atlas) {
		_set_uv_error(r_error, "Could not allocate an xatlas context.");
		return ERR_OUT_OF_MEMORY;
	}
	const xatlas::AddMeshError add_error = xatlas::AddMesh(atlas, input, 1);
	if (add_error != xatlas::AddMeshError::Success) {
		_set_uv_error(r_error, xatlas::StringForEnum(add_error));
		xatlas::Destroy(atlas);
		return ERR_INVALID_DATA;
	}
	xatlas::ChartOptions chart_options;
	chart_options.fixWinding = true;
	chart_options.useInputMeshUvs = p_use_input_uvs;
	xatlas::PackOptions pack_options;
	pack_options.resolution = resolution;
	pack_options.padding = padding;
	pack_options.texelsPerUnit = texels_per_unit;
	pack_options.rotateChartsToAxis = true;
	pack_options.rotateCharts = true;
	xatlas::Generate(atlas, chart_options, pack_options);
	if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
		_set_uv_error(r_error, "xatlas could not generate a non-empty UV atlas.");
		xatlas::Destroy(atlas);
		return ERR_CANT_CREATE;
	}

	const xatlas::Mesh &output = atlas->meshes[0];
	Vector<bool> assigned;
	assigned.resize(input_loops.size());
	for (uint32_t i = 0; i < output.vertexCount; i++) {
		const xatlas::Vertex &vertex = output.vertexArray[i];
		if (vertex.xref >= (uint32_t)input_loops.size() || assigned[vertex.xref]) {
			continue;
		}
		const Vector2 uv(vertex.uv[0] / atlas->width, vertex.uv[1] / atlas->height);
		r_mesh.set_loop_uv(input_loops[vertex.xref], uv);
		assigned.write[vertex.xref] = true;
	}
	xatlas::Destroy(atlas);
	for (bool was_assigned : assigned) {
		if (!was_assigned) {
			_set_uv_error(r_error, "xatlas did not return UV coordinates for every face corner.");
			return ERR_CANT_CREATE;
		}
	}
	return OK;
}

