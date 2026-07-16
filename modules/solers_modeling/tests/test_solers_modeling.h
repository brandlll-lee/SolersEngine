/**************************************************************************/
/*  test_solers_modeling.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/io/json.h"
#include "core/math/random_pcg.h"
#include "modules/solers_modeling/core/solers_model_operation.h"
#include "modules/solers_modeling/core/solers_model_modifier.h"
#include "modules/solers_modeling/core/solers_model_source.h"
#include "tests/test_macros.h"

namespace TestSolersModeling {

TEST_CASE("[SolersModeling] editable topology round-trips with stable IDs") {
	SolersEditableMesh source;
	Dictionary box;
	box["size"] = Vector3(4, 3, 2);
	Dictionary created = SolersModelOperationRegistry::get_singleton()->execute(source, SNAME("create_box"), box);
	INFO(JSON::stringify(created));
	REQUIRE((bool)created.get("ok", false));
	CHECK(source.validate() == OK);

	const Vector<int64_t> vertex_ids = source.get_vertex_ids();
	const Vector<int64_t> edge_ids = source.get_edge_ids();
	const Vector<int64_t> face_ids = source.get_face_ids();
	PackedByteArray bytes;
	REQUIRE(SolersModelSource::encode(source, bytes) == OK);

	SolersEditableMesh restored;
	REQUIRE(SolersModelSource::decode(bytes, restored) == OK);
	CHECK(restored.get_vertex_ids() == vertex_ids);
	CHECK(restored.get_edge_ids() == edge_ids);
	CHECK(restored.get_face_ids() == face_ids);
	CHECK(restored.to_dictionary() == source.to_dictionary());
}

TEST_CASE("[SolersModeling] failed topology operations leave the source copy uncommitted") {
	SolersEditableMesh mesh;
	const Dictionary plane = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("create_plane"), Dictionary());
	INFO(JSON::stringify(plane));
	REQUIRE((bool)plane.get("ok", false));
	const Dictionary before = mesh.to_dictionary();
	Dictionary invalid_bridge;
	Array first;
	first.push_back(1);
	first.push_back(2);
	first.push_back(3);
	Array second;
	second.push_back(4);
	second.push_back(5);
	invalid_bridge["first"] = first;
	invalid_bridge["second"] = second;
	const Dictionary result = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("bridge"), invalid_bridge);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(mesh.to_dictionary() == before);
}

TEST_CASE("[SolersModeling] extrude creates a valid editable region and runtime mesh") {
	SolersEditableMesh mesh;
	const Dictionary plane = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("create_plane"), Dictionary());
	INFO(JSON::stringify(plane));
	REQUIRE((bool)plane.get("ok", false));
	Dictionary extrude;
	extrude["distance"] = 2.0;
	const Dictionary extruded = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("extrude_faces"), extrude);
	INFO(JSON::stringify(extruded));
	REQUIRE((bool)extruded.get("ok", false));
	CHECK(mesh.validate() == OK);
	CHECK(mesh.get_face_ids().size() == 5);
}

TEST_CASE("[SolersModeling] operation definitions own their schemas") {
	const Vector<SolersModelOperationDefinition> &operations = SolersModelOperationRegistry::get_singleton()->get_operations();
	REQUIRE(operations.size() >= 15);
	HashSet<StringName> names;
	for (const SolersModelOperationDefinition &operation : operations) {
		CHECK_FALSE(operation.id.is_empty());
		CHECK_FALSE(operation.description.is_empty());
		CHECK(operation.parameters_schema.get("type", String()) == "object");
		CHECK_FALSE(names.has(operation.id));
		names.insert(operation.id);
	}
}

TEST_CASE("[SolersModeling] core modifier stack evaluates real topology") {
	SolersEditableMesh source;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(source, SNAME("create_box"), Dictionary()).get("ok", false));

	Dictionary mirror;
	mirror["axis"] = "x";
	mirror["origin"] = Vector3(2, 0, 0);
	source.add_modifier(SNAME("mirror"), mirror);
	Dictionary array;
	array["count"] = 2;
	array["offset"] = Vector3(0, 0, 3);
	source.add_modifier(SNAME("array"), array);

	SolersEditableMesh evaluated;
	String error;
	REQUIRE(SolersModelModifierEvaluator::evaluate(source, evaluated, &error) == OK);
	INFO(error);
	CHECK(evaluated.validate() == OK);
	CHECK(evaluated.get_face_ids().size() == 24);
	CHECK(evaluated.get_modifiers().is_empty());
}

TEST_CASE("[SolersModeling] solidify closes an open plane") {
	SolersEditableMesh source;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(source, SNAME("create_plane"), Dictionary()).get("ok", false));
	Dictionary parameters;
	parameters["thickness"] = 0.2;
	source.add_modifier(SNAME("solidify"), parameters);

	SolersEditableMesh evaluated;
	String error;
	REQUIRE(SolersModelModifierEvaluator::evaluate(source, evaluated, &error) == OK);
	INFO(error);
	CHECK(evaluated.validate() == OK);
	CHECK(evaluated.get_face_ids().size() == 6);
	CHECK(evaluated.get_boundary_edges().is_empty());
}

TEST_CASE("[SolersModeling] segmented bevel produces valid closed topology") {
	SolersEditableMesh source;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(source, SNAME("create_box"), Dictionary()).get("ok", false));
	Dictionary parameters;
	parameters["width"] = 0.08;
	parameters["segments"] = 2;
	source.add_modifier(SNAME("bevel"), parameters);

	SolersEditableMesh evaluated;
	String error;
	REQUIRE(SolersModelModifierEvaluator::evaluate(source, evaluated, &error) == OK);
	INFO(error);
	CHECK(evaluated.validate() == OK);
	CHECK(evaluated.get_face_ids().size() > source.get_face_ids().size());
	CHECK(evaluated.get_boundary_edges().is_empty());
}

TEST_CASE("[SolersModeling] loop cut and dissolve preserve topology invariants") {
	SolersEditableMesh mesh;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("create_box"), Dictionary()).get("ok", false));
	Dictionary loop_cut;
	Array seed;
	seed.push_back(mesh.get_edge_ids()[0]);
	loop_cut["edge_ids"] = seed;
	loop_cut["factor"] = 0.4;
	const Dictionary cut = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("loop_cut"), loop_cut);
	INFO(JSON::stringify(cut));
	REQUIRE((bool)cut.get("ok", false));
	CHECK(mesh.validate() == OK);
	CHECK(mesh.get_face_ids().size() == 10);

	Dictionary dissolve;
	Array edge;
	for (int64_t edge_id : mesh.get_edge_ids()) {
		if (mesh.get_edge(edge_id)->loops.size() == 2) {
			edge.push_back(edge_id);
			break;
		}
	}
	dissolve["edge_ids"] = edge;
	const Dictionary dissolved = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("dissolve_edges"), dissolve);
	INFO(JSON::stringify(dissolved));
	REQUIRE((bool)dissolved.get("ok", false));
	CHECK(mesh.validate() == OK);
	CHECK(mesh.get_face_ids().size() == 9);
}

TEST_CASE("[SolersModeling] grid fill closes a four-sided room opening") {
	SolersEditableMesh mesh;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("create_box"), Dictionary()).get("ok", false));
	const int64_t removed_face = mesh.get_face_ids()[0];
	const Vector<int64_t> corners = mesh.get_face_vertices(removed_face);
	Dictionary remove;
	remove["domain"] = "face";
	Array removed;
	removed.push_back(removed_face);
	remove["ids"] = removed;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("delete"), remove).get("ok", false));
	CHECK(mesh.get_boundary_edges().size() == 4);

	Dictionary fill;
	Array edges;
	for (int64_t edge_id : mesh.get_boundary_edges()) {
		edges.push_back(edge_id);
	}
	Array corner_array;
	for (int64_t corner : corners) {
		corner_array.push_back(corner);
	}
	fill["edge_ids"] = edges;
	fill["corners"] = corner_array;
	const Dictionary filled = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("grid_fill"), fill);
	INFO(JSON::stringify(filled));
	REQUIRE((bool)filled.get("ok", false));
	CHECK(mesh.validate() == OK);
	CHECK(mesh.get_face_ids().size() == 6);
	CHECK(mesh.get_boundary_edges().is_empty());
}

TEST_CASE("[SolersModeling] xatlas generates finite packed UV1 coordinates") {
	SolersEditableMesh mesh;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("create_box"), Dictionary()).get("ok", false));
	Dictionary options;
	options["resolution"] = 256;
	options["padding"] = 2;
	const Dictionary unwrapped = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("unwrap_uv"), options);
	INFO(JSON::stringify(unwrapped));
	REQUIRE((bool)unwrapped.get("ok", false));
	for (int64_t loop_id : mesh.get_loop_ids()) {
		const Vector2 uv = mesh.get_loop(loop_id)->uv;
		CHECK(uv.is_finite());
		CHECK(uv.x >= 0.0);
		CHECK(uv.y >= 0.0);
		CHECK(uv.x <= 1.0);
		CHECK(uv.y <= 1.0);
	}
	const Dictionary packed = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("pack_uv"), options);
	INFO(JSON::stringify(packed));
	CHECK((bool)packed.get("ok", false));
}

TEST_CASE("[SolersModeling] build settings survive modifiers and source round-trip") {
	SolersEditableMesh mesh;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("create_box"), Dictionary()).get("ok", false));
	Dictionary build;
	build["weighted_normals"] = true;
	build["generate_uv2"] = true;
	build["collision"] = "trimesh";
	Array lods;
	Dictionary lod;
	lod["ratio"] = 0.5;
	lod["distance"] = 12.0;
	lods.push_back(lod);
	build["lod_levels"] = lods;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("configure_build"), build).get("ok", false));
	mesh.add_modifier(SNAME("array"), Dictionary());
	SolersEditableMesh evaluated;
	String error;
	REQUIRE(SolersModelModifierEvaluator::evaluate(mesh, evaluated, &error) == OK);
	INFO(error);
	CHECK(evaluated.get_build_settings() == mesh.get_build_settings());

	PackedByteArray bytes;
	REQUIRE(SolersModelSource::encode(mesh, bytes, &error) == OK);
	SolersEditableMesh restored;
	REQUIRE(SolersModelSource::decode(bytes, restored, &error) == OK);
	CHECK(restored.get_build_settings() == mesh.get_build_settings());
}

TEST_CASE("[SolersModeling] deterministic mixed edits preserve topology invariants") {
	RandomPCG random(0x534F4C455253ULL);
	SolersEditableMesh mesh;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("create_box"), Dictionary()).get("ok", false));
	for (int step = 0; step < 128; step++) {
		if (mesh.get_face_ids().is_empty()) {
			Dictionary primitive;
			primitive["center"] = Vector3(random.random(-2.0f, 2.0f), 0, random.random(-2.0f, 2.0f));
			REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("create_box"), primitive).get("ok", false));
		}
		const Vector<int64_t> faces = mesh.get_face_ids();
		const Vector<int64_t> edges = mesh.get_edge_ids();
		const Vector<int64_t> vertices = mesh.get_vertex_ids();
		Dictionary parameters;
		StringName operation;
		switch (random.rand(8)) {
			case 0: {
				operation = SNAME("transform");
				Array ids;
				ids.push_back(vertices[random.rand(vertices.size())]);
				parameters["vertex_ids"] = ids;
				parameters["translation"] = Vector3(random.random(-0.1f, 0.1f), random.random(-0.1f, 0.1f), random.random(-0.1f, 0.1f));
			} break;
			case 1: {
				operation = SNAME("extrude_faces");
				Array ids;
				ids.push_back(faces[random.rand(faces.size())]);
				parameters["face_ids"] = ids;
				parameters["distance"] = random.random(-0.15f, 0.15f);
			} break;
			case 2: {
				operation = SNAME("inset_faces");
				Array ids;
				ids.push_back(faces[random.rand(faces.size())]);
				parameters["face_ids"] = ids;
				parameters["factor"] = random.random(0.1f, 0.4f);
			} break;
			case 3: {
				operation = SNAME("flip_faces");
				Array ids;
				ids.push_back(faces[random.rand(faces.size())]);
				parameters["face_ids"] = ids;
			} break;
			case 4: {
				operation = SNAME("dissolve_edges");
				Array ids;
				ids.push_back(edges[random.rand(edges.size())]);
				parameters["edge_ids"] = ids;
			} break;
			case 5: {
				operation = SNAME("split_faces");
				Array ids;
				ids.push_back(faces[random.rand(faces.size())]);
				parameters["face_ids"] = ids;
			} break;
			case 6: {
				operation = SNAME("loop_cut");
				Array ids;
				ids.push_back(edges[random.rand(edges.size())]);
				parameters["edge_ids"] = ids;
				parameters["factor"] = random.random(0.2f, 0.8f);
			} break;
			default: {
				operation = SNAME("delete");
				Array ids;
				ids.push_back(faces[random.rand(faces.size())]);
				parameters["domain"] = "face";
				parameters["ids"] = ids;
			} break;
		}
		SolersEditableMesh working = mesh;
		const Dictionary result = SolersModelOperationRegistry::get_singleton()->execute(working, operation, parameters);
		if ((bool)result.get("ok", false)) {
			mesh = working;
		}
		INFO(vformat("step=%d operation=%s result=%s", step, operation, JSON::stringify(result)));
		CHECK(mesh.validate() == OK);
	}
}

} // namespace TestSolersModeling
