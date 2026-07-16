/**************************************************************************/
/*  test_solers_modeling.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/io/json.h"
#include "modules/solers_modeling/core/solers_model_operation.h"
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

} // namespace TestSolersModeling
