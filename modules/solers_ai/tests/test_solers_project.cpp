/**************************************************************************/
/*  test_solers_project.cpp                                               */
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

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/object/message_queue.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_tree.h"
#include "scene/gui/control.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/resource_format_text.h"
#include "scene/resources/shader.h"
#include "scene/resources/sky.h"
#ifdef DEBUG_ENABLED
#include "scene/debugger/scene_debugger_object.h"
#endif
#include "tests/test_macros.h"
#include "tests/test_tools.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/tests/support/solers_test_state.h"
#ifdef MODULE_CSG_ENABLED
#include "modules/csg/csg_shape.h"
#endif
#ifdef MODULE_GLTF_ENABLED
#include "modules/gltf/extensions/gltf_document_extension_convert_importer_mesh.h"
#include "modules/gltf/gltf_document.h"
#endif

TEST_FORCE_LINK(test_solers_project)

namespace TestSolersProject {

TEST_CASE("[SolersResourceService] missing resources terminate at the native existence fact") {
	SolersResourceService resources;
	Dictionary args;
	args["path"] = "res://missing-" + String::num_uint64(OS::get_singleton()->get_ticks_usec()) + ".tres";
	args["include_dependencies"] = true;
	ErrorDetector errors;
	const Dictionary result = resources.get_resource_info(args);
	REQUIRE(result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	CHECK_FALSE(data.get("exists", true));
	CHECK_FALSE(data.has("dependencies"));
	CHECK_FALSE(errors.has_error);
}

TEST_CASE("[SolersScriptService] project.godot cannot use the raw file path") {
	SolersScriptService scripts;
	Dictionary args;
	args["operation"] = "write_file";
	args["path"] = "res://project.godot";
	args["content"] = "[application]\n";
	const Dictionary result = scripts.edit_project(args);
	CHECK_FALSE(result.get("ok", true));
	CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "EDITOR_OWNED_FILE");
}

TEST_CASE("[SolersScriptService] invalid source never replaces the authoritative file") {
	const String path = "res://solers_script_validation_contract.gd";
	const String shader_path = "res://solers_shader_validation_contract.gdshader";
	SolersTestPaths cleanup;
	cleanup.add(path);
	cleanup.add(shader_path);

	SolersScriptService script_service;

	const String valid_source = "extends Node\nfunc value() -> int:\n\treturn 1\n";
	Dictionary write_args;
	write_args["path"] = path;
	write_args["content"] = valid_source;
	Dictionary written = script_service.write_file(write_args);
	REQUIRE((bool)written.get("ok", false));
	CHECK(Dictionary(written.get("data", Dictionary())).get("valid", false));
	const String valid_sha256 = FileAccess::get_sha256(path);

	Dictionary patch_args;
	patch_args["path"] = path;
	patch_args["old_text"] = "\treturn 1";
	patch_args["new_text"] = "\treturn +";
	patch_args["expected_sha256"] = valid_sha256;
	Dictionary patched = script_service.patch_file(patch_args);
	CHECK_FALSE((bool)patched.get("ok", true));
	CHECK(Dictionary(patched.get("error", Dictionary())).get("code", String()) == "SCRIPT_VALIDATION_FAILED");
	CHECK(FileAccess::get_sha256(path) == valid_sha256);
	CHECK(FileAccess::get_file_as_string(path) == valid_source);

	Dictionary shader_args;
	shader_args["operation"] = "create";
	shader_args["path"] = shader_path;
	shader_args["content"] = "shader_type spatial;\nvoid fragment() { ALBEDO = missing_identifier; }\n";
	const Dictionary shader_result = script_service.edit_script(shader_args);
	CHECK_FALSE((bool)shader_result.get("ok", true));
	CHECK_FALSE(FileAccess::exists(shader_path));
}

TEST_CASE("[SolersScriptService] replacement requires current identity and exact unique bytes") {
	const String path = "res://solers_exact_replace_contract.gd";
	SolersTestPaths cleanup;
	cleanup.add(path);
	SolersScriptService script_service;
	Dictionary write_args;
	write_args["path"] = path;
	write_args["content"] = "extends Node\n\nfunc alpha() -> int:\n\treturn 1\n\nfunc first_stub() -> void:\n\tpass\n\nfunc second_stub() -> void:\n\tpass\n";
	REQUIRE((bool)script_service.write_file(write_args).get("ok", false));
	const String initial_sha256 = FileAccess::get_sha256(path);
	const String initial_source = FileAccess::get_file_as_string(path);

	Dictionary unconditioned;
	unconditioned["path"] = path;
	unconditioned["old_text"] = "\treturn 1";
	unconditioned["new_text"] = "\treturn 10";
	const Dictionary unconditioned_result = script_service.patch_file(unconditioned);
	CHECK_FALSE(unconditioned_result.get("ok", true));
	CHECK(Dictionary(unconditioned_result.get("error", Dictionary())).get("code", String()) == "INVALID_ARGUMENT");
	CHECK(FileAccess::get_file_as_string(path) == initial_source);

	Dictionary exact;
	exact["path"] = path;
	exact["old_text"] = "\treturn 1";
	exact["new_text"] = "\treturn 10";
	exact["expected_sha256"] = initial_sha256;
	const Dictionary replaced = script_service.patch_file(exact);
	REQUIRE((bool)replaced.get("ok", false));
	const String replaced_sha256 = Dictionary(replaced.get("data", Dictionary())).get("sha256", String());
	CHECK(replaced_sha256 == FileAccess::get_sha256(path));
	CHECK(replaced_sha256 != initial_sha256);

	Dictionary stale = exact;
	stale["old_text"] = "\treturn 10";
	stale["new_text"] = "\treturn 11";
	const Dictionary stale_result = script_service.patch_file(stale);
	CHECK_FALSE(stale_result.get("ok", true));
	CHECK(Dictionary(stale_result.get("error", Dictionary())).get("code", String()) == "STATE_CONFLICT");
	CHECK(FileAccess::get_sha256(path) == replaced_sha256);

	Dictionary drift = stale;
	drift["old_text"] = "    return 10";
	drift["expected_sha256"] = replaced_sha256;
	const Dictionary drift_result = script_service.patch_file(drift);
	CHECK_FALSE(drift_result.get("ok", true));
	CHECK(Dictionary(drift_result.get("error", Dictionary())).get("code", String()) == "PATCH_TEXT_NOT_FOUND");

	Dictionary ambiguous;
	ambiguous["path"] = path;
	ambiguous["old_text"] = "\tpass";
	ambiguous["new_text"] = "\tbreakpoint";
	ambiguous["expected_sha256"] = replaced_sha256;
	const Dictionary ambiguous_result = script_service.patch_file(ambiguous);
	CHECK_FALSE(ambiguous_result.get("ok", true));
	CHECK(Dictionary(ambiguous_result.get("error", Dictionary())).get("code", String()) == "PATCH_TEXT_AMBIGUOUS");
	CHECK(FileAccess::get_sha256(path) == replaced_sha256);
	CHECK(FileAccess::get_file_as_string(path).contains("\treturn 10"));
}

TEST_CASE("[SolersScriptService] native serialized resources require native resource APIs") {
	REQUIRE(ResourceFormatLoaderText::singleton != nullptr);
	List<String> extensions;
	ResourceFormatLoaderText::singleton->get_recognized_extensions(&extensions);
	REQUIRE_FALSE(extensions.is_empty());

	SolersScriptService script_service;
	SolersTestPaths cleanup;
	for (const String &extension : extensions) {
		const String path = "res://solers_native_resource_contract." + extension;
		cleanup.add(path);
		Dictionary args;
		args["path"] = path;
		args["content"] = "native resource text must not bypass ResourceSaver";
		const Dictionary result = script_service.write_file(args);
		CHECK_FALSE((bool)result.get("ok", true));
		CHECK(Dictionary(result.get("error", Dictionary())).get("code", String()) == "NATIVE_RESOURCE_WRITE_BLOCKED");
		CHECK_FALSE(FileAccess::exists(path));
	}
}

TEST_CASE("[SolersScriptService] isolated compute commits only verified declared resources") {
	const String path = "res://.solers_compute_contract.tres";
	SolersTestPaths cleanup;
	cleanup.add(path);

	Dictionary output;
	output["from"] = "generated.tres";
	output["to"] = path;
	output["resource_type"] = "Resource";
	Array outputs;
	outputs.push_back(output);
	Dictionary args;
	args["source"] = "extends SceneTree\nfunc _init():\n\tvar value := Resource.new()\n\tvalue.resource_name = \"isolated\"\n\tif ResourceSaver.save(value, \"res://generated.tres\") != OK:\n\t\tquit(2)\n\t\treturn\n\tquit()\n";
	args["outputs"] = outputs;

	SolersScriptService service;
	REQUIRE(service.compute_script("compute-contract", args).get("ok", false));
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
	while (!service.compute_script_ready("compute-contract") && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(10000);
	}
	REQUIRE(service.compute_script_ready("compute-contract"));
	const Dictionary result = service.compute_script_finalize("compute-contract");
	REQUIRE(result.get("ok", false));
	const Array committed = Dictionary(result.get("data", Dictionary())).get("outputs", Array());
	REQUIRE(committed.size() == 1);
	CHECK_FALSE(String(Dictionary(committed[0]).get("sha256", String())).is_empty());
	const Ref<Resource> loaded = ResourceLoader::load(path, "Resource", ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP);
	REQUIRE(loaded.is_valid());
	CHECK(loaded->get_name() == "isolated");
}

TEST_CASE("[SolersResourceService] property coercion accepts named and nested Godot components") {
	Node3D *node = memnew(Node3D);
	Dictionary position;
	position["x"] = 1;
	position["y"] = 2.5;
	position["z"] = -3;
	Variant coerced;
	String error;
	REQUIRE(solers_coerce_property_value(node, SNAME("position"), position, coerced, error));
	CHECK(Vector3(coerced) == Vector3(1, 2.5, -3));

	Dictionary origin;
	origin["x"] = 4;
	origin["y"] = 5;
	origin["z"] = 6;
	Dictionary transform;
	transform["origin"] = origin;
	REQUIRE(solers_coerce_property_value(node, SNAME("transform"), transform, coerced, error));
	CHECK(Transform3D(coerced).origin == Vector3(4, 5, 6));
	CHECK_FALSE(solers_coerce_property_value(node, SNAME("position"), "Vector3(1, 2, 3)", coerced, error));
	memdelete(node);

	Ref<Shader> shader;
	shader.instantiate();
	Ref<ShaderMaterial> material;
	material.instantiate();
	const Dictionary handle = solers_summarize_display_value(shader);
	REQUIRE(solers_coerce_property_value(material.ptr(), SNAME("shader"), handle, coerced, error));
	CHECK(Ref<Shader>(coerced) == shader);

	MeshInstance3D *mesh_instance = memnew(MeshInstance3D);
	Dictionary material_properties;
	material_properties["albedo_color"] = Dictionary({ { "r", 0.25 }, { "g", 0.5 }, { "b", 0.75 }, { "a", 1.0 } });
	Dictionary material_spec;
	material_spec["class_name"] = "StandardMaterial3D";
	material_spec["properties"] = material_properties;
	Dictionary mesh_properties;
	mesh_properties["size"] = Dictionary({ { "x", 2.0 }, { "y", 3.0 }, { "z", 4.0 } });
	mesh_properties["material"] = material_spec;
	Dictionary mesh_spec;
	mesh_spec["class_name"] = "BoxMesh";
	mesh_spec["properties"] = mesh_properties;
	REQUIRE(solers_coerce_property_value(mesh_instance, SNAME("mesh"), mesh_spec, coerced, error));
	const Ref<BoxMesh> box = coerced;
	REQUIRE(box.is_valid());
	CHECK(box->get_size() == Vector3(2, 3, 4));
	CHECK(box->get_material().is_valid());
	memdelete(mesh_instance);
}

#ifdef MODULE_CSG_ENABLED

TEST_CASE("[SceneTree][SolersCSG] native boolean output exposes mesh and collision facts") {
	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGBox3D *volume = memnew(CSGBox3D);
	CSGBox3D *cut = memnew(CSGBox3D);
	root->add_child(volume);
	root->add_child(cut);
	SceneTree::get_singleton()->get_root()->add_child(root);
	volume->set_size(Vector3(4, 4, 4));
	cut->set_size(Vector3(2, 2, 5));
	cut->set_position(Vector3(1, 0, 0));
	cut->set_operation(CSGShape3D::OPERATION_SUBTRACTION);
	root->update_shape();

	const Ref<ArrayMesh> mesh = root->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	const Dictionary facts = solers_describe_geometry(root);
	CHECK((int)facts.get("surface_count", 0) > 0);
	CHECK((int)facts.get("triangle_count", 0) > 0);
#ifndef PHYSICS_3D_DISABLED
	const Ref<ConcavePolygonShape3D> collision = root->bake_collision_shape();
	REQUIRE(collision.is_valid());
	CHECK_FALSE(collision->get_faces().is_empty());
#endif
	root->queue_free();
	MessageQueue::get_singleton()->flush();
}
#endif

TEST_CASE("[SolersObservationService] observe_path digests any selection by engine authority") {
	// Contract: directory and ordinary file both get a digest.kind — not only PackedScene.
	const String dir_path = "res://solers_observe_path_contract";
	const String file_path = dir_path.path_join("note.txt");
	SolersTestPaths cleanup;
	cleanup.add(dir_path);
	{
		Ref<DirAccess> root = DirAccess::open("res://");
		REQUIRE(root.is_valid());
		CHECK(root->make_dir("solers_observe_path_contract") == OK);
		Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string("first\nsecond\nthird\n");
	}
	SolersObservationService observation_service;
	const Dictionary dir_observed = observation_service.observe_path(dir_path + "/");
	REQUIRE((bool)dir_observed.get("ok", false));
	const Dictionary dir_digest = dir_observed.get("digest", Dictionary());
	CHECK(dir_digest.get("kind", String()) == "directory");
	CHECK((int)dir_digest.get("file_count", 0) >= 1);
	// EFS-backed digests project import_valid/dependency_count; DirAccess fallback may omit them.
	const Array children = dir_digest.get("children", Array());
	REQUIRE(children.size() >= 1);

	const Dictionary file_observed = observation_service.observe_path(file_path);
	REQUIRE((bool)file_observed.get("ok", false));
	CHECK_FALSE(String(Dictionary(file_observed.get("digest", Dictionary())).get("kind", String())).is_empty());
	const Dictionary page = observation_service.read_project_file(file_path, 2, 1);
	CHECK(page.get("content", String()) == "second");
	CHECK((int)page.get("next_line", 0) == 3);
}

TEST_CASE("[SolersObservationService] runtime views require native debugger authority") {
	SolersObservationService observation_service;
	Dictionary observe_args;
	const Dictionary runtime = observation_service.observe_runtime(observe_args);
	CHECK(runtime.has("error_digest"));
	const Dictionary status = observation_service.get_runtime_status();
	CHECK_FALSE((bool)status.get("capture_ready", true));
	for (const char *target : { "scene", "stack" }) {
		Dictionary args;
		args["target"] = target;
		const Dictionary observed = observation_service.observe_runtime(args);
		CHECK_FALSE((bool)observed.get("available", false));
	}
	Variant value;
	const ObjectID object_id((uint64_t)1);
	PropertyInfo info;
	String observation_id;
	CHECK_FALSE(observation_service.get_runtime_property(0, NodePath("/root/Player"), object_id, SNAME("position"), value, info, observation_id));
}

#ifdef DEBUG_ENABLED
TEST_CASE("[SolersObservationService] remote tree projects one canonical runtime identity") {
	Node3D *root = memnew(Node3D);
	root->set_name("root");
	Node3D *player = memnew(Node3D);
	player->set_name("Player");
	root->add_child(player);
	Skeleton3D *skeleton = memnew(Skeleton3D);
	skeleton->set_name("Rig");
	skeleton->add_bone("Head");
	skeleton->set_bone_pose_position(0, Vector3(0.0, 1.7, 0.0));
	player->add_child(skeleton);
	Camera3D *camera = memnew(Camera3D);
	camera->set_name("Camera");
	camera->set_fov(73.0);
	skeleton->add_child(camera);
	AnimationTree *animation_tree = memnew(AnimationTree);
	animation_tree->set_name("Locomotion");
	animation_tree->set_active(false);
	player->add_child(animation_tree);
	Control *hud = memnew(Control);
	hud->set_name("HUD");
	root->add_child(hud);
	SceneDebuggerTree tree(root);

	const Array nodes = SolersObservationService::project_runtime_tree(tree, 17);
	REQUIRE(nodes.size() == 6);
	CHECK(Dictionary(nodes[0]).get("node_path", String()) == "/root");
	CHECK(Dictionary(nodes[2]).get("class_name", String()) == "Skeleton3D");
	CHECK(Dictionary(nodes[3]).get("node_path", String()) == "/root/Player/Rig/Camera");
	CHECK(Dictionary(nodes[3]).get("class_name", String()) == "Camera3D");
	CHECK((int64_t)Dictionary(nodes[3]).get("object_id", 0) == (int64_t)(uint64_t)camera->get_instance_id());
	CHECK(Dictionary(nodes[4]).get("node_path", String()) == "/root/Player/Locomotion");
	CHECK(Dictionary(nodes[4]).get("class_name", String()) == "AnimationTree");
	CHECK(Dictionary(nodes[5]).get("node_path", String()) == "/root/HUD");
	CHECK((int64_t)Dictionary(nodes[4]).get("runtime_epoch", 0) == 17);
	CHECK(camera->get_fov() == doctest::Approx(73.0));
	CHECK(skeleton->get_bone_count() == 1);
	CHECK(skeleton->get_bone_name(0) == "Head");
	CHECK(skeleton->get_bone_pose_position(0) == Vector3(0.0, 1.7, 0.0));
	CHECK_FALSE(animation_tree->is_active());
	memdelete(root);
}
#endif

TEST_CASE("[SolersScriptService] script.edit reports persisted file identity") {
	const String path = "res://solers_root_script_fact_contract.gd";
	SolersTestPaths cleanup;
	cleanup.add(path);
	SolersScriptService script_service;
	Dictionary args;
	args["path"] = path;
	args["content"] = "extends Node\nfunc _ready() -> void:\n\tpass\n";
	const Dictionary written = script_service.write_file(args);
	REQUIRE((bool)written.get("ok", false));
	const Dictionary data = written.get("data", Dictionary());
	CHECK(data.has("path"));
	CHECK(data.has("sha256"));
	CHECK_FALSE(data.has("affects_edited_scene_root_script"));
}

TEST_CASE("[SceneTree][SolersObservationService] RenderState follows native World3D authority") {
	SubViewport *viewport = memnew(SubViewport);
	viewport->set_use_own_world_3d(true);
	Node3D *root = memnew(Node3D);
	Camera3D *camera = memnew(Camera3D);
	DirectionalLight3D *sun = memnew(DirectionalLight3D);
	viewport->add_child(root);
	root->add_child(camera);
	root->add_child(sun);
	SceneTree::get_singleton()->get_root()->add_child(viewport);
	viewport->set_debug_draw(Viewport::DEBUG_DRAW_LIGHTING);
	camera->set_current(true);
	sun->set_param(Light3D::PARAM_INTENSITY, 125000.0);
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code("shader_type sky; uniform float strength = 2.0; void sky() { COLOR = vec3(strength); }");
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);
	Ref<Sky> sky;
	sky.instantiate();
	sky->set_material(material);
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_sky(sky);
	viewport->find_world_3d()->set_environment(environment);
	Ref<CameraAttributesPhysical> attributes;
	attributes.instantiate();
	viewport->find_world_3d()->set_camera_attributes(attributes);
	SolersObservationService observation;
	Dictionary state = observation.describe_render_state(viewport, camera, root);
	CHECK(state.get("environment_source", String()) == "world");
	CHECK(state.get("camera_attributes_source", String()) == "world");
	CHECK((int)state.get("viewport_debug_draw", -1) == (int)Viewport::DEBUG_DRAW_LIGHTING);
	CHECK(Array(state.get("lights", Array())).size() == 1);
	Dictionary parameters = Dictionary(Dictionary(Dictionary(state.get("environment", Dictionary())).get("sky_material", Dictionary())).get("parameters", Dictionary()));
	CHECK(Dictionary(parameters.get("strength", Dictionary())).get("source", String()) == "shader_default");
	const String before = SolersObservationService::render_state_fingerprint(state);
	material->set_shader_parameter("strength", 3.0);
	state = observation.describe_render_state(viewport, camera, root);
	parameters = Dictionary(Dictionary(Dictionary(state.get("environment", Dictionary())).get("sky_material", Dictionary())).get("parameters", Dictionary()));
	CHECK(Dictionary(parameters.get("strength", Dictionary())).get("source", String()) == "material_override");
	CHECK(SolersObservationService::render_state_fingerprint(state) != before);
	viewport->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SolersResourceService] create initializes properties and accepts listed Resource types") {
	const String texture_path = "res://.solers_texture_array_contract.tres";
	const String node_path = "res://.solers_visual_shader_node_contract.tres";
	const String invalid_path = "res://.solers_invalid_object_contract.tres";
	SolersTestPaths cleanup;
	cleanup.add(texture_path);
	cleanup.add(node_path);
	cleanup.add(invalid_path);
	SolersResourceService resource_service;

	Dictionary create_texture;
	create_texture["class_name"] = "CompressedTexture2DArray";
	create_texture["path"] = texture_path;
	REQUIRE(resource_service.create_resource(create_texture).get("ok", false));

	Dictionary node_properties;
	node_properties["texture_array"] = texture_path;
	Dictionary create_node;
	create_node["class_name"] = "VisualShaderNodeTexture2DArray";
	create_node["path"] = node_path;
	create_node["properties"] = node_properties;
	const Dictionary created_node = resource_service.create_resource(create_node);
	REQUIRE(created_node.get("ok", false));
	CHECK((int)Dictionary(created_node.get("data", Dictionary())).get("initialized_property_count", 0) == 1);

	Ref<Resource> node = ResourceLoader::load(node_path);
	REQUIRE(node.is_valid());
	Ref<Resource> texture = node->get("texture_array");
	REQUIRE(texture.is_valid());
	CHECK(texture->is_class("CompressedTexture2DArray"));

	Dictionary invalid_properties;
	invalid_properties["texture_array"] = Dictionary();
	create_node["path"] = invalid_path;
	create_node["properties"] = invalid_properties;
	const Dictionary invalid = resource_service.create_resource(create_node);
	CHECK_FALSE((bool)invalid.get("ok", true));
	CHECK(Dictionary(invalid.get("error", Dictionary())).get("code", String()) == "INVALID_PROPERTY_VALUE");
	CHECK_FALSE(FileAccess::exists(invalid_path));
}

TEST_CASE("[SolersReflectionService] introspection reports native Object argument classes") {
	SolersReflectionService reflection_service;
	Dictionary args;
	args["class_name"] = "Mesh";
	args["include_inherited"] = false;
	args["member_query"] = "surface_set_material";
	const Dictionary result = reflection_service.introspect_class(args);
	REQUIRE(result.get("ok", false));

	const Array methods = Dictionary(result.get("data", Dictionary())).get("methods", Array());
	bool found = false;
	for (int i = 0; i < methods.size(); i++) {
		const Dictionary method = methods[i];
		if (String(method.get("name", String())) != "surface_set_material") {
			continue;
		}
		const Array arguments = method.get("arguments", Array());
		REQUIRE(arguments.size() == 2);
		CHECK(Dictionary(arguments[1]).get("class_name", String()) == "Material");
		found = true;
		break;
	}
	CHECK(found);
}

TEST_CASE("[SolersReflectionService] lean introspect returns member_names without typed members") {
	SolersReflectionService reflection_service;
	Dictionary lean_args;
	lean_args["class_name"] = "Node3D";
	lean_args["include_inherited"] = false;
	lean_args["max_members"] = 1;
	const Dictionary lean = reflection_service.introspect_class(lean_args);
	REQUIRE(lean.get("ok", false));
	const Dictionary lean_data = lean.get("data", Dictionary());
	REQUIRE(PackedStringArray(lean_data.get("member_names", PackedStringArray())).size() == 1);
	CHECK((int)lean_data.get("member_count", 0) > 0);
	CHECK(lean_data.get("truncated", false));
	REQUIRE(lean_data.has("next_cursor"));
	Dictionary next_args = lean_args.duplicate();
	next_args["cursor"] = lean_data["next_cursor"];
	const Dictionary next = reflection_service.introspect_class(next_args);
	REQUIRE(next.get("ok", false));
	CHECK(PackedStringArray(Dictionary(next.get("data", Dictionary())).get("member_names", PackedStringArray())).size() == 1);
	CHECK_FALSE(lean_data.has("methods"));
	CHECK_FALSE(lean_data.has("properties"));

	Dictionary expand_args = lean_args;
	expand_args["member_query"] = "position";
	expand_args["max_members"] = 64;
	const Dictionary expanded = reflection_service.introspect_class(expand_args);
	REQUIRE(expanded.get("ok", false));
	const Dictionary expand_data = expanded.get("data", Dictionary());
	CHECK(expand_data.has("properties"));
	CHECK(Array(expand_data.get("properties", Array())).size() >= 1);

	Dictionary color_args;
	color_args["class_name"] = "DirectionalLight3D";
	color_args["member_query"] = "light_color";
	const Dictionary color_result = reflection_service.introspect_class(color_args);
	REQUIRE((bool)color_result.get("ok", false));
	const Array color_properties = Dictionary(color_result.get("data", Dictionary())).get("properties", Array());
	REQUIRE(color_properties.size() == 1);
	const Dictionary wire_shape = Dictionary(color_properties[0]).get("wire_shape", Dictionary());
	CHECK(wire_shape.get("encoding", String()) == "named_members");
	CHECK(Dictionary(wire_shape.get("members", Dictionary())).get("r", String()) == "float");
}

TEST_CASE("[SolersReflectionService] member_query reports unmatched tokens from ClassDB") {
	// OR matching must not silently drop a token that exists on no member of the class.
	SolersReflectionService reflection_service;
	Dictionary args;
	args["class_name"] = "ProceduralSkyMaterial";
	args["include_inherited"] = false;
	args["member_query"] = "sun_angle_max sun_disk_scale";
	const Dictionary result = reflection_service.introspect_class(args);
	REQUIRE(result.get("ok", false));
	const Dictionary data = result.get("data", Dictionary());
	const Array unmatched = data.get("unmatched_member_query_tokens", Array());
	REQUIRE(unmatched.size() >= 1);
	bool saw_sun_disk = false;
	for (int i = 0; i < unmatched.size(); i++) {
		const Dictionary entry = unmatched[i];
		if (String(entry.get("token", String())) != "sun_disk_scale") {
			continue;
		}
		saw_sun_disk = true;
		CHECK(Array(entry.get("nearest_members", Array())).size() >= 0);
		const PackedStringArray siblings = entry.get("classes_with_member", PackedStringArray());
		CHECK(siblings.has("PhysicalSkyMaterial"));
	}
	CHECK(saw_sun_disk);
}

TEST_CASE("[SolersTransaction][SceneTree][UndoRedo] update properties remain one native action") {
	Node3D *target = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(target);
	UndoRedo undo_redo;
	undo_redo.create_action("Solers: Update");
	undo_redo.add_do_property(target, SNAME("position"), Vector3(1, 2, 3));
	undo_redo.add_undo_property(target, SNAME("position"), target->get_position());
	undo_redo.add_do_property(target, SNAME("visible"), false);
	undo_redo.add_undo_property(target, SNAME("visible"), target->is_visible());
	undo_redo.commit_action();
	CHECK((target->get_position() == Vector3(1, 2, 3) && !target->is_visible()));
	REQUIRE(undo_redo.undo());
	CHECK((target->get_position() == Vector3() && target->is_visible()));
	REQUIRE(undo_redo.redo());
	CHECK((target->get_position() == Vector3(1, 2, 3) && !target->is_visible()));
	target->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SolersResourceService] nearest names rank containment above similarity") {
	PackedStringArray candidates;
	candidates.push_back("get_size()");
	candidates.push_back("size_flags_horizontal");
	candidates.push_back("visible");
	candidates.push_back("scale");
	const PackedStringArray nearest = solers_nearest_names("size", candidates, 3);
	REQUIRE(nearest.size() >= 2);
	CHECK(nearest.has("get_size()"));
	CHECK(nearest.has("size_flags_horizontal"));
	CHECK_FALSE(nearest.has("visible"));
	CHECK(solers_nearest_names("", candidates, 3).is_empty());
}

} // namespace TestSolersProject
