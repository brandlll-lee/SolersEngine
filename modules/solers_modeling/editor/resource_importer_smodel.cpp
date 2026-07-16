/**************************************************************************/
/*  resource_importer_smodel.cpp                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "resource_importer_smodel.h"

#include "core/io/file_access.h"
#include "core/io/resource_saver.h"
#include "modules/solers_modeling/core/solers_model_source.h"
#include "scene/resources/mesh.h"

String ResourceImporterSolersModel::get_importer_name() const {
	return "solers_model";
}

String ResourceImporterSolersModel::get_visible_name() const {
	return "Solers Model";
}

void ResourceImporterSolersModel::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("smodel");
}

String ResourceImporterSolersModel::get_save_extension() const {
	return "res";
}

String ResourceImporterSolersModel::get_resource_type() const {
	return "ArrayMesh";
}

int ResourceImporterSolersModel::get_preset_count() const {
	return 0;
}

String ResourceImporterSolersModel::get_preset_name(int p_idx) const {
	return String();
}

void ResourceImporterSolersModel::get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset) const {
}

bool ResourceImporterSolersModel::get_option_visibility(const String &p_path, const String &p_option, const HashMap<StringName, Variant> &p_options) const {
	return true;
}

Error ResourceImporterSolersModel::import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants, List<String> *r_gen_files, Variant *r_metadata) {
	SolersEditableMesh editable_mesh;
	String error_message;
	Error error = SolersModelSource::load(p_source_file, editable_mesh, &error_message);
	if (error != OK) {
		ERR_PRINT(error_message);
		return error;
	}
	Ref<ArrayMesh> mesh = editable_mesh.compile(&error_message);
	if (mesh.is_null() && !editable_mesh.is_empty()) {
		ERR_PRINT(error_message);
		return ERR_INVALID_DATA;
	}
	if (mesh.is_null()) {
		mesh.instantiate();
	}
	mesh->set_meta("solers/source_hash", FileAccess::get_sha256(p_source_file));
	mesh->set_meta("solers/source_revision", editable_mesh.get_revision());
	return ResourceSaver::save(mesh, p_save_path + ".res");
}
