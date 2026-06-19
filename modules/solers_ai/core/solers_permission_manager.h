/**************************************************************************/
/*  solers_permission_manager.h                                           */
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

#include "core/object/object.h"
#include "core/templates/hash_set.h"
#include "core/variant/binder_common.h"

class SolersPermissionManager : public Object {
	GDCLASS(SolersPermissionManager, Object);

public:
	enum Permission {
		PERMISSION_OBSERVE,
		PERMISSION_EDIT_SCENE,
		PERMISSION_EDIT_FILES,
		PERMISSION_RUN_PROJECT,
		PERMISSION_EXPORT_BUILD,
		PERMISSION_NETWORK,
		PERMISSION_SHELL,
	};

	enum RequestDecision {
		DECISION_UNKNOWN,
		DECISION_PENDING,
		DECISION_APPROVED,
		DECISION_REJECTED,
	};

private:
	HashSet<Permission> auto_approved_permissions;
	Dictionary approved_once_requests;
	HashSet<int> rejected_request_ids;
	Array pending_requests;
	int next_request_id = 1;
	bool auto_approve_all = false;

	int _find_pending_request_index(int p_request_id) const;

protected:
	static void _bind_methods();

public:
	String get_permission_name(Permission p_permission) const;
	bool is_auto_approved(Permission p_permission) const;
	bool is_auto_approve_all() const;
	void set_auto_approve_all(bool p_enabled);
	void set_auto_approve_permission(Permission p_permission, bool p_enabled);
	bool get_auto_approve_permission(Permission p_permission) const;
	Dictionary request_user_approval(const StringName &p_tool_name, const Dictionary &p_args, Permission p_permission);
	Array list_pending_requests() const;
	int get_pending_request_count() const;
	bool approve_request(int p_request_id);
	bool reject_request(int p_request_id);
	bool consume_approval(int p_request_id, const StringName &p_tool_name);
	RequestDecision get_request_decision(int p_request_id) const;

	SolersPermissionManager();
};

VARIANT_ENUM_CAST(SolersPermissionManager::Permission);
VARIANT_ENUM_CAST(SolersPermissionManager::RequestDecision);
