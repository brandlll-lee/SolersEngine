/**************************************************************************/
/*  solers_reflection_service.h                                           */
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

#pragma once

#include "core/math/aabb.h"
#include "core/object/object.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

class Node;
class Node3D;
struct MethodInfo;
struct SolersToolContext;

class SolersReflectionService : public Object {
	GDCLASS(SolersReflectionService, Object);

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;

	Node *_resolve_node(const String &p_node_path, String &r_error) const;

	bool _prepare_property_change(Node *p_node, const String &p_property, const Variant &p_value, Variant &r_old, Variant &r_value, NodePath &r_indexed_path, String &r_error) const;
	bool _apply_initial_properties(Node *p_node, const Dictionary &p_properties, Dictionary &r_applied, String &r_error) const;

	static bool _safe_node_path(Node *p_node, String &r_out);

	Dictionary _spatial_facts(Node3D *p_node) const;
	// Facts a subsystem computes and no property dump contains: bone poses,
	// playback state, live particle bounds, the resolved material, the
	// environment actually in force. Dispatch is on the node's engine type,
	// which is the authority on which of these exist for it.
	Dictionary _subsystem_facts(Node *p_node) const;
	String _instance_scene_path(Node *p_node) const;

	Dictionary _list_signal_connections(const Dictionary &p_args);
	Dictionary create_node(const Dictionary &p_args);
	Dictionary instantiate_scene(const Dictionary &p_args);
	Dictionary update_node(const Dictionary &p_args);
	Dictionary reparent_node(const Dictionary &p_args);
	Dictionary connect_signal(const Dictionary &p_args);
	Dictionary attach_script(const Dictionary &p_args);
	Dictionary remove_node(const Dictionary &p_args);

protected:
	static void _bind_methods();

public:
	Dictionary search_classes(const Dictionary &p_args);

	Dictionary introspect_class(const Dictionary &p_args);

	Dictionary get_scene_state() const;
	Dictionary inspect_nodes(const Dictionary &p_args);
	Dictionary edit_scene(const Dictionary &p_args, const SolersToolContext *p_context = nullptr);
	Dictionary measure_spatial_relations(const Dictionary &p_args) const;
	Dictionary open_scene(const Dictionary &p_args);
	Dictionary open_scene_with_context(const Dictionary &p_args, const SolersToolContext *p_context);
};
