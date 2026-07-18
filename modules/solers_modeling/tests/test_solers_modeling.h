/**************************************************************************/
/*  test_solers_modeling.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/math/random_pcg.h"
#include "core/config/project_settings.h"
#include "modules/solers_modeling/core/solers_model_operation.h"
#include "modules/solers_modeling/core/solers_model_modifier.h"
#include "modules/solers_modeling/core/solers_model_source.h"
#include "tests/test_macros.h"

namespace TestSolersModeling {

struct TemporaryModelSource {
	String path;

	~TemporaryModelSource() {
		if (FileAccess::exists(path)) {
			DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(path));
		}
	}
};

static Error write_model_source(const String &p_path, const SolersEditableMesh &p_mesh) {
	PackedByteArray bytes;
	Error error = SolersModelSource::encode(p_mesh, bytes);
	if (error != OK) {
		return error;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return FileAccess::get_open_error();
	}
	file->store_buffer(bytes);
	return file->get_error();
}

TEST_CASE("[SolersModeling] editable topology round-trips with stable IDs") {
	SolersEditableMesh source;
	Dictionary box;
	box["size"] = Vector3(4, 3, 2);
	Dictionary created = SolersModelOperationRegistry::get_singleton()->execute(source, SNAME("add_box"), box);
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
	const Dictionary plane = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_plane"), Dictionary());
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
	const Dictionary plane = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_plane"), Dictionary());
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

TEST_CASE("[SolersModeling] operation schemas reject unknown fields and enum values") {
	SolersEditableMesh mesh;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), Dictionary()).get("ok", false));
	const Dictionary before = mesh.to_dictionary();

	Dictionary misspelled;
	misspelled["cout"] = 3;
	Dictionary result = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_array_modifier"), misspelled);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "MODEL_ARGUMENT_INVALID");
	CHECK(mesh.to_dictionary() == before);

	Dictionary invalid_boolean;
	invalid_boolean["operand"] = "res://operand.smodel";
	invalid_boolean["operation"] = "difference";
	result = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_boolean_modifier"), invalid_boolean);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "MODEL_ARGUMENT_INVALID");
	CHECK(mesh.to_dictionary() == before);
	Dictionary invalid_size;
	Dictionary size;
	size["x"] = 1.0;
	size["why"] = 2.0;
	invalid_size["size"] = size;
	result = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), invalid_size);
	CHECK_FALSE((bool)result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "MODEL_ARGUMENT_INVALID");
	CHECK(mesh.to_dictionary() == before);
	CHECK(SolersModelOperationRegistry::get_singleton()->get_operation(SNAME("add_modifier")) == nullptr);
}

TEST_CASE("[SolersModeling] triangulation preserves every source face winding") {
	SolersEditableMesh mesh;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), Dictionary()).get("ok", false));
	for (int64_t face_id : mesh.get_face_ids()) {
		const Vector<int64_t> vertices = mesh.get_face_vertices(face_id);
		Vector3 normal;
		for (int i = 0; i < vertices.size(); i++) {
			const Vector3 a = mesh.get_vertex(vertices[i])->position;
			const Vector3 b = mesh.get_vertex(vertices[(i + 1) % vertices.size()])->position;
			normal += a.cross(b);
		}
		const PackedInt32Array triangles = mesh.triangulate_face(face_id);
		REQUIRE(triangles.size() == 6);
		for (int i = 0; i < triangles.size(); i += 3) {
			const Vector3 a = mesh.get_vertex(vertices[triangles[i]])->position;
			const Vector3 b = mesh.get_vertex(vertices[triangles[i + 1]])->position;
			const Vector3 c = mesh.get_vertex(vertices[triangles[i + 2]])->position;
			CHECK((b - a).cross(c - a).dot(normal) > 0.0);
		}
	}
}

TEST_CASE("[SolersModeling][SceneTree] Boolean union subtract and intersect produce valid runtime geometry") {
	TemporaryModelSource operand_file{ "res://.godot/solers_modeling_boolean_operand.smodel" };
	SolersEditableMesh operand;
	Dictionary operand_box;
	operand_box["size"] = Vector3(1, 1, 1);
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(operand, SNAME("add_box"), operand_box).get("ok", false));
	REQUIRE(write_model_source(operand_file.path, operand) == OK);

	for (const String &operation : { String("union"), String("subtract"), String("intersect") }) {
		SolersEditableMesh mesh;
		Dictionary base_box;
		base_box["size"] = Vector3(2, 2, 2);
		REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), base_box).get("ok", false));
		Dictionary boolean;
		boolean["operand"] = operand_file.path;
		boolean["operation"] = operation;
		boolean["translation"] = Vector3(0.75, 0, 0);
		Dictionary added = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_boolean_modifier"), boolean);
		INFO(vformat("operation=%s add=%s", operation, JSON::stringify(added)));
		REQUIRE((bool)added.get("ok", false));
		Dictionary applied = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("apply_modifiers"), Dictionary());
		INFO(vformat("operation=%s apply=%s", operation, JSON::stringify(applied)));
		REQUIRE((bool)applied.get("ok", false));
		CHECK(mesh.validate() == OK);
		String compile_error;
		Ref<ArrayMesh> runtime_mesh = mesh.compile(&compile_error);
		INFO(vformat("operation=%s compile=%s", operation, compile_error));
		REQUIRE(runtime_mesh.is_valid());
		CHECK(runtime_mesh->get_aabb().has_volume());
	}
}

TEST_CASE("[SolersModeling][SceneTree] source creation is truthful in a new directory") {
	const String directory = "res://.godot/solers_modeling_source_test";
	const String path = directory.path_join("new/model.smodel");
	const String absolute_directory = ProjectSettings::get_singleton()->globalize_path(directory);
	if (DirAccess::dir_exists_absolute(absolute_directory)) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(path));
	}
	SolersModelingService *service = SolersModelingService::get_singleton();
	REQUIRE(service != nullptr);
	const Dictionary created = service->create(path, false);
	INFO(JSON::stringify(created));
	REQUIRE((bool)created.get("ok", false));
	CHECK(FileAccess::exists(path));
	CHECK(Dictionary(created.get("data", Dictionary())).get("import_status", String()) == "scan_requested");

	const Dictionary duplicate = service->create(path, false);
	CHECK_FALSE((bool)duplicate.get("ok", true));
	CHECK(Dictionary(duplicate.get("error", Dictionary())).get("code", String()) == "MODEL_ALREADY_EXISTS");
	CHECK((bool)service->validate_source(path).get("ok", false));

	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(path));
}

TEST_CASE("[SolersModeling] core modifier stack evaluates real topology") {
	SolersEditableMesh source;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(source, SNAME("add_box"), Dictionary()).get("ok", false));

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
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(source, SNAME("add_plane"), Dictionary()).get("ok", false));
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
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(source, SNAME("add_box"), Dictionary()).get("ok", false));
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

TEST_CASE("[SolersModeling] disconnected box batch supports every bevel segment count") {
	for (int segments = 1; segments <= 12; segments++) {
		SolersEditableMesh source;
		for (int i = 0; i < 24; i++) {
			Dictionary box;
			box["size"] = Vector3(0.8, 0.6, 0.7);
			box["center"] = Vector3((i % 6) * 1.1, (i / 6) * 0.9, 0);
			const Dictionary added = SolersModelOperationRegistry::get_singleton()->execute(source, SNAME("add_box"), box);
			INFO(vformat("segments=%d box=%d result=%s", segments, i, JSON::stringify(added)));
			REQUIRE((bool)added.get("ok", false));
		}
		Dictionary parameters;
		parameters["width"] = 0.08;
		parameters["segments"] = segments;
		source.add_modifier(SNAME("bevel"), parameters);

		SolersEditableMesh evaluated;
		String error;
		INFO(vformat("segments=%d error=%s", segments, error));
		REQUIRE(SolersModelModifierEvaluator::evaluate(source, evaluated, &error) == OK);
		CHECK(evaluated.validate() == OK);
		CHECK(evaluated.get_boundary_edges().is_empty());
	}
}

TEST_CASE("[SolersModeling] primitives report only geometry created by that operation") {
	SolersEditableMesh mesh;
	const Dictionary first = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), Dictionary());
	REQUIRE((bool)first.get("ok", false));
	Dictionary second_box;
	second_box["center"] = Vector3(2, 0, 0);
	const Dictionary second = SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), second_box);
	REQUIRE((bool)second.get("ok", false));
	const Dictionary first_data = first.get("data", Dictionary());
	const Dictionary second_data = second.get("data", Dictionary());
	CHECK(Array(first_data.get("created_edges", Array())).size() == 12);
	CHECK(Array(second_data.get("created_edges", Array())).size() == 12);
	for (const Variant &edge_id : Array(first_data.get("created_edges", Array()))) {
		CHECK_FALSE(Array(second_data.get("created_edges", Array())).has(edge_id));
	}
}

TEST_CASE("[SolersModeling] loop cut and dissolve preserve topology invariants") {
	SolersEditableMesh mesh;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), Dictionary()).get("ok", false));
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
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), Dictionary()).get("ok", false));
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

TEST_CASE("[SolersModeling] xatlas unwraps mixed polygon sizes into finite UV1 coordinates") {
	SolersEditableMesh mesh;
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), Dictionary()).get("ok", false));
	Dictionary cylinder;
	cylinder["segments"] = 12;
	cylinder["center"] = Vector3(2, 0, 0);
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_cylinder"), cylinder).get("ok", false));
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
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), Dictionary()).get("ok", false));
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
	REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), Dictionary()).get("ok", false));
	for (int step = 0; step < 128; step++) {
		if (mesh.get_face_ids().is_empty()) {
			Dictionary primitive;
			primitive["center"] = Vector3(random.random(-2.0f, 2.0f), 0, random.random(-2.0f, 2.0f));
			REQUIRE((bool)SolersModelOperationRegistry::get_singleton()->execute(mesh, SNAME("add_box"), primitive).get("ok", false));
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
