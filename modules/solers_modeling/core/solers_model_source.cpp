/**************************************************************************/
/*  solers_model_source.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_model_source.h"

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/io/resource_loader.h"
#include "core/string/print_string.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "modules/solers_modeling/core/solers_model_operation.h"

SolersModelingService *SolersModelingService::singleton = nullptr;

static void _set_source_error(String *r_error, const String &p_message) {
	if (r_error) {
		*r_error = p_message;
	}
}

Error SolersModelSource::encode(const SolersEditableMesh &p_mesh, PackedByteArray &r_bytes, String *r_error) {
	String validation_error;
	const Error validation = p_mesh.validate(&validation_error);
	if (validation != OK) {
		_set_source_error(r_error, validation_error);
		return validation;
	}
	const Dictionary data = p_mesh.to_dictionary();
	int payload_size = 0;
	Error error = encode_variant(data, nullptr, payload_size, false);
	if (error != OK) {
		_set_source_error(r_error, "Could not encode Solers model data.");
		return error;
	}
	r_bytes.resize(12 + payload_size);
	uint8_t *write = r_bytes.ptrw();
	encode_uint32(MAGIC, write);
	encode_uint32(SolersEditableMesh::FORMAT_VERSION, write + 4);
	encode_uint32(payload_size, write + 8);
	error = encode_variant(data, write + 12, payload_size, false);
	if (error != OK) {
		r_bytes.clear();
		_set_source_error(r_error, "Could not encode Solers model payload.");
	}
	return error;
}

Error SolersModelSource::decode(const PackedByteArray &p_bytes, SolersEditableMesh &r_mesh, String *r_error) {
	if (p_bytes.size() < 12) {
		_set_source_error(r_error, "Solers model source is truncated.");
		return ERR_FILE_CORRUPT;
	}
	const uint8_t *read = p_bytes.ptr();
	if (decode_uint32(read) != MAGIC) {
		_set_source_error(r_error, "Solers model source has an invalid signature.");
		return ERR_FILE_UNRECOGNIZED;
	}
	if (decode_uint32(read + 4) != SolersEditableMesh::FORMAT_VERSION) {
		_set_source_error(r_error, "Solers model source version is not supported.");
		return ERR_FILE_UNRECOGNIZED;
	}
	const uint32_t payload_size = decode_uint32(read + 8);
	if (payload_size != (uint32_t)(p_bytes.size() - 12)) {
		_set_source_error(r_error, "Solers model source payload length is invalid.");
		return ERR_FILE_CORRUPT;
	}
	Variant decoded;
	int consumed = 0;
	const Error error = decode_variant(decoded, read + 12, payload_size, &consumed, false);
	if (error != OK || consumed != (int)payload_size || decoded.get_type() != Variant::DICTIONARY) {
		_set_source_error(r_error, "Solers model source contains invalid Variant data.");
		return ERR_FILE_CORRUPT;
	}
	return r_mesh.from_dictionary(decoded, r_error);
}

Error SolersModelSource::load(const String &p_path, SolersEditableMesh &r_mesh, String *r_error) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		_set_source_error(r_error, vformat("Could not open Solers model source: %s", p_path));
		return ERR_CANT_OPEN;
	}
	return decode(file->get_buffer(file->get_length()), r_mesh, r_error);
}

Error SolersModelSource::save(const String &p_path, const SolersEditableMesh &p_mesh, String *r_error) {
	PackedByteArray bytes;
	Error error = encode(p_mesh, bytes, r_error);
	if (error != OK) {
		return error;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
	if (file.is_null()) {
		_set_source_error(r_error, vformat("Could not write Solers model source: %s", p_path));
		return error == OK ? ERR_CANT_CREATE : error;
	}
	file->store_buffer(bytes);
	return file->get_error();
}

String SolersModelSource::hash(const PackedByteArray &p_bytes) {
	uint8_t digest[32];
	if (CryptoCore::sha256(p_bytes.ptr(), p_bytes.size(), digest) != OK) {
		return String();
	}
	return String::hex_encode_buffer(digest, 32);
}

void SolersModelingService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_write_source_bytes", "path", "bytes"), &SolersModelingService::_write_source_bytes);
	ClassDB::bind_method(D_METHOD("_delete_source", "path"), &SolersModelingService::_delete_source);
	ClassDB::bind_method(D_METHOD("create", "path", "primitive", "parameters", "undoable"), &SolersModelingService::create, DEFVAL(Dictionary()), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("inspect", "path"), &SolersModelingService::inspect);
	ClassDB::bind_method(D_METHOD("validate_source", "path"), &SolersModelingService::validate_source);
	ClassDB::bind_method(D_METHOD("apply", "path", "operation", "parameters", "expected_revision", "undoable"), &SolersModelingService::apply, DEFVAL(Dictionary()), DEFVAL(-1), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("batch", "path", "operations", "expected_revision", "undoable"), &SolersModelingService::batch, DEFVAL(-1), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("save_as", "source_path", "destination_path", "undoable"), &SolersModelingService::save_as, DEFVAL(true));
}

Dictionary SolersModelingService::_ok(const Variant &p_data) {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersModelingService::_error(const String &p_code, const String &p_message, bool p_recoverable) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

static bool _valid_source_path(const String &p_path) {
	return p_path.begins_with("res://") && p_path.get_extension().to_lower() == "smodel" && !p_path.contains("..") && p_path.simplify_path() == p_path;
}

static PackedByteArray _read_source_bytes(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	return file.is_valid() ? file->get_buffer(file->get_length()) : PackedByteArray();
}

void SolersModelingService::_write_source_bytes(const String &p_path, const PackedByteArray &p_bytes) {
	ERR_FAIL_COND_MSG(!_valid_source_path(p_path), "Invalid Solers model source path.");
	const String base_dir = p_path.get_base_dir();
	if (!DirAccess::dir_exists_absolute(base_dir)) {
		const Error dir_error = DirAccess::make_dir_recursive_absolute(base_dir);
		ERR_FAIL_COND_MSG(dir_error != OK, vformat("Could not create Solers model directory: %s", base_dir));
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	ERR_FAIL_COND_MSG(file.is_null(), vformat("Could not write Solers model source: %s", p_path));
	file->store_buffer(p_bytes);
	file.unref();
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(p_path);
	}
}

void SolersModelingService::_delete_source(const String &p_path) {
	ERR_FAIL_COND_MSG(!_valid_source_path(p_path), "Invalid Solers model source path.");
	if (FileAccess::exists(p_path)) {
		const Error error = DirAccess::remove_absolute(p_path);
		ERR_FAIL_COND_MSG(error != OK, vformat("Could not remove Solers model source: %s", p_path));
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(p_path);
	}
}

Dictionary SolersModelingService::_commit(const String &p_path, const SolersEditableMesh &p_mesh, const String &p_action, bool p_undoable) {
	if (!_valid_source_path(p_path)) {
		return _error("MODEL_PATH_INVALID", "Model paths must be normalized res:// paths ending in .smodel.", false);
	}
	String compile_error;
	if (p_mesh.compile(&compile_error).is_null() && !p_mesh.is_empty()) {
		return _error("MODEL_COMPILE_FAILED", compile_error, true);
	}
	PackedByteArray new_bytes;
	String encode_error;
	if (SolersModelSource::encode(p_mesh, new_bytes, &encode_error) != OK) {
		return _error("MODEL_ENCODE_FAILED", encode_error, false);
	}
	const bool existed = FileAccess::exists(p_path);
	const PackedByteArray old_bytes = existed ? _read_source_bytes(p_path) : PackedByteArray();
	EditorUndoRedoManager *undo_redo = p_undoable ? EditorUndoRedoManager::get_singleton() : nullptr;
	if (undo_redo) {
		undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, this);
		undo_redo->add_do_method(this, SNAME("_write_source_bytes"), p_path, new_bytes);
		if (existed) {
			undo_redo->add_undo_method(this, SNAME("_write_source_bytes"), p_path, old_bytes);
		} else {
			undo_redo->add_undo_method(this, SNAME("_delete_source"), p_path);
		}
		undo_redo->commit_action();
	} else {
		_write_source_bytes(p_path, new_bytes);
	}
	Dictionary data = p_mesh.inspect();
	data["path"] = p_path;
	data["source_hash"] = SolersModelSource::hash(new_bytes);
	return _ok(data);
}

Dictionary SolersModelingService::create(const String &p_path, const StringName &p_primitive, const Dictionary &p_parameters, bool p_undoable) {
	if (FileAccess::exists(p_path)) {
		return _error("MODEL_ALREADY_EXISTS", vformat("A model source already exists at %s.", p_path), true);
	}
	SolersEditableMesh mesh;
	const StringName operation = StringName("create_" + String(p_primitive).to_lower());
	Dictionary result = SolersModelOperationRegistry::get_singleton()->execute(mesh, operation, p_parameters);
	if (!(bool)result.get("ok", false)) {
		return result;
	}
	mesh.increment_revision();
	return _commit(p_path, mesh, vformat("Solers Modeling: Create %s", String(p_primitive).capitalize()), p_undoable);
}

Dictionary SolersModelingService::inspect(const String &p_path) const {
	SolersEditableMesh mesh;
	String error;
	if (SolersModelSource::load(p_path, mesh, &error) != OK) {
		return _error("MODEL_LOAD_FAILED", error, true);
	}
	Dictionary data = mesh.inspect();
	data["path"] = p_path;
	data["source_hash"] = FileAccess::get_sha256(p_path);
	data["document"] = mesh.to_dictionary();
	return _ok(data);
}

Dictionary SolersModelingService::validate_source(const String &p_path) const {
	SolersEditableMesh mesh;
	String error;
	if (SolersModelSource::load(p_path, mesh, &error) != OK) {
		return _error("MODEL_LOAD_FAILED", error, true);
	}
	if (mesh.validate(&error) != OK) {
		return _error("MODEL_TOPOLOGY_INVALID", error, true);
	}
	if (mesh.compile(&error).is_null() && !mesh.is_empty()) {
		return _error("MODEL_COMPILE_FAILED", error, true);
	}
	Dictionary data = mesh.inspect();
	data["valid"] = true;
	return _ok(data);
}

Dictionary SolersModelingService::apply(const String &p_path, const StringName &p_operation, const Dictionary &p_parameters, int64_t p_expected_revision, bool p_undoable) {
	SolersEditableMesh mesh;
	String error;
	if (SolersModelSource::load(p_path, mesh, &error) != OK) {
		return _error("MODEL_LOAD_FAILED", error, true);
	}
	if (p_expected_revision >= 0 && mesh.get_revision() != p_expected_revision) {
		return _error("MODEL_REVISION_CONFLICT", vformat("Expected model revision %d, found %d.", p_expected_revision, mesh.get_revision()), true);
	}
	Dictionary result = SolersModelOperationRegistry::get_singleton()->execute(mesh, p_operation, p_parameters);
	if (!(bool)result.get("ok", false)) {
		return result;
	}
	mesh.increment_revision();
	Dictionary commit = _commit(p_path, mesh, vformat("Solers Modeling: %s", String(p_operation).capitalize()), p_undoable);
	if ((bool)commit.get("ok", false)) {
		Dictionary data = commit["data"];
		data["operation"] = p_operation;
		data["result"] = result.get("data", Dictionary());
		commit["data"] = data;
	}
	return commit;
}

Dictionary SolersModelingService::batch(const String &p_path, const Array &p_operations, int64_t p_expected_revision, bool p_undoable) {
	SolersEditableMesh mesh;
	String error;
	if (SolersModelSource::load(p_path, mesh, &error) != OK) {
		return _error("MODEL_LOAD_FAILED", error, true);
	}
	if (p_expected_revision >= 0 && mesh.get_revision() != p_expected_revision) {
		return _error("MODEL_REVISION_CONFLICT", vformat("Expected model revision %d, found %d.", p_expected_revision, mesh.get_revision()), true);
	}
	Array results;
	for (int i = 0; i < p_operations.size(); i++) {
		if (p_operations[i].get_type() != Variant::DICTIONARY) {
			return _error("MODEL_BATCH_INVALID", vformat("Batch item %d must be an object.", i), false);
		}
		const Dictionary item = p_operations[i];
		const StringName operation = StringName(item.get("operation", String()));
		const Dictionary parameters = item.get("parameters", Dictionary());
		Dictionary result = SolersModelOperationRegistry::get_singleton()->execute(mesh, operation, parameters);
		if (!(bool)result.get("ok", false)) {
			Dictionary failed = result;
			Dictionary failure = failed.get("error", Dictionary());
			failure["batch_index"] = i;
			failure["operation"] = operation;
			failed["error"] = failure;
			return failed;
		}
		results.push_back(result.get("data", Dictionary()));
	}
	mesh.increment_revision();
	Dictionary commit = _commit(p_path, mesh, "Solers Modeling: Batch", p_undoable);
	if ((bool)commit.get("ok", false)) {
		Dictionary data = commit["data"];
		data["results"] = results;
		commit["data"] = data;
	}
	return commit;
}

Dictionary SolersModelingService::save_as(const String &p_source_path, const String &p_destination_path, bool p_undoable) {
	if (FileAccess::exists(p_destination_path)) {
		return _error("MODEL_ALREADY_EXISTS", vformat("A model source already exists at %s.", p_destination_path), true);
	}
	SolersEditableMesh mesh;
	String error;
	if (SolersModelSource::load(p_source_path, mesh, &error) != OK) {
		return _error("MODEL_LOAD_FAILED", error, true);
	}
	return _commit(p_destination_path, mesh, "Solers Modeling: Save As", p_undoable);
}

SolersModelingService::SolersModelingService() {
	ERR_FAIL_COND_MSG(singleton != nullptr, "SolersModelingService singleton already exists.");
	singleton = this;
}

SolersModelingService::~SolersModelingService() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

