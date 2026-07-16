/**************************************************************************/
/*  solers_model_source.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/variant/dictionary.h"
#include "modules/solers_modeling/core/solers_editable_mesh.h"

class SolersModelSource {
public:
	static constexpr uint32_t MAGIC = 0x4C444D53; // SMDL, little-endian.

	static Error encode(const SolersEditableMesh &p_mesh, PackedByteArray &r_bytes, String *r_error = nullptr);
	static Error decode(const PackedByteArray &p_bytes, SolersEditableMesh &r_mesh, String *r_error = nullptr);
	static Error load(const String &p_path, SolersEditableMesh &r_mesh, String *r_error = nullptr);
	static Error save(const String &p_path, const SolersEditableMesh &p_mesh, String *r_error = nullptr);
	static String hash(const PackedByteArray &p_bytes);
};

class SolersModelingService : public Object {
	GDCLASS(SolersModelingService, Object);

	static SolersModelingService *singleton;

	Dictionary _commit(const String &p_path, const SolersEditableMesh &p_mesh, const String &p_action, bool p_undoable);
	static Dictionary _ok(const Variant &p_data = Dictionary());
	static Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true);

protected:
	static void _bind_methods();

public:
	void _write_source_bytes(const String &p_path, const PackedByteArray &p_bytes);
	void _delete_source(const String &p_path);

	Dictionary create(const String &p_path, const StringName &p_primitive, const Dictionary &p_parameters = Dictionary(), bool p_undoable = true);
	Dictionary inspect(const String &p_path) const;
	Dictionary validate_source(const String &p_path) const;
	Dictionary apply(const String &p_path, const StringName &p_operation, const Dictionary &p_parameters = Dictionary(), int64_t p_expected_revision = -1, bool p_undoable = true);
	Dictionary batch(const String &p_path, const Array &p_operations, int64_t p_expected_revision = -1, bool p_undoable = true);
	Dictionary save_as(const String &p_source_path, const String &p_destination_path, bool p_undoable = true);

	static SolersModelingService *get_singleton() { return singleton; }

	SolersModelingService();
	~SolersModelingService();
};

