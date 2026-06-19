/**************************************************************************/
/*  solers_reflection_service.h                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/variant/dictionary.h"

class Node;
class SolersActionTimeline;
struct MethodInfo;

class SolersReflectionService : public Object {
	GDCLASS(SolersReflectionService, Object);

	SolersActionTimeline *action_timeline = nullptr;

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;

	Node *_resolve_node(const String &p_node_path, String &r_error) const;
	Dictionary _call_method_on_object(Object *p_object, const String &p_owner, const String &p_method, const MethodInfo &p_info, const Array &p_args) const;

	bool _coerce_value(Node *p_node, const StringName &p_property, const Variant &p_value, Variant &r_out, String &r_error) const;

	static bool _safe_node_path(Node *p_node, String &r_out);

	Dictionary _create_node(const Dictionary &p_args);
	Dictionary _reparent_node(const Dictionary &p_args);
	Dictionary _connect_signal(const Dictionary &p_args);
	Dictionary _attach_script(const Dictionary &p_args);
	Dictionary _remove_node(const Dictionary &p_args);
	Dictionary _list_properties(const Dictionary &p_args);
	Dictionary _list_signal_connections(const Dictionary &p_args);

protected:
	static void _bind_methods();

public:
	void set_action_timeline(SolersActionTimeline *p_action_timeline) { action_timeline = p_action_timeline; }

	Dictionary introspect_class(const Dictionary &p_args);

	Dictionary get_property(const Dictionary &p_args);

	Dictionary set_property(const Dictionary &p_args);

	Dictionary call_method(const Dictionary &p_args);

	Dictionary invoke_editor(const Dictionary &p_args);

	Dictionary batch(const Dictionary &p_args);

	SolersReflectionService();
};
