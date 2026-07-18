/**************************************************************************/
/*  solers_model_operation.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "modules/solers_modeling/core/solers_editable_mesh.h"

struct SolersModelOperationDefinition {
	using Handler = Dictionary (*)(SolersEditableMesh &, const Dictionary &);

	StringName id;
	String description;
	Dictionary parameters_schema;
	bool changes_topology = false;
	bool modifier_operation = false;
	Handler handler = nullptr;
};

class SolersModelOperationRegistry {
	Vector<SolersModelOperationDefinition> operations;
	HashMap<StringName, int> operation_index;

	void _add(const StringName &p_id, const String &p_description, const Dictionary &p_schema, bool p_changes_topology, bool p_modifier_operation, SolersModelOperationDefinition::Handler p_handler);
	static Dictionary _ok(const Dictionary &p_data = Dictionary());
	static Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true);

	SolersModelOperationRegistry();

public:
	static SolersModelOperationRegistry *get_singleton();

	const Vector<SolersModelOperationDefinition> &get_operations() const { return operations; }
	const SolersModelOperationDefinition *get_operation(const StringName &p_id) const;
	Dictionary get_batch_item_schema() const;
	Dictionary validate_parameters(const StringName &p_id, const Dictionary &p_parameters, bool p_allow_missing_required = false) const;
	Dictionary execute(SolersEditableMesh &r_mesh, const StringName &p_id, const Dictionary &p_parameters) const;
};
