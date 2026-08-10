/**************************************************************************/
/*  solers_permission_manager.cpp                                         */
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

#include "solers_permission_manager.h"

#include "core/object/class_db.h"

void SolersPermissionManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_permission_name", "permission"), &SolersPermissionManager::get_permission_name);
	ClassDB::bind_method(D_METHOD("is_auto_approved", "permission"), &SolersPermissionManager::is_auto_approved);
	ClassDB::bind_method(D_METHOD("is_auto_approve_all"), &SolersPermissionManager::is_auto_approve_all);
	ClassDB::bind_method(D_METHOD("set_auto_approve_all", "enabled"), &SolersPermissionManager::set_auto_approve_all);
	ClassDB::bind_method(D_METHOD("set_auto_approve_permission", "permission", "enabled"), &SolersPermissionManager::set_auto_approve_permission);
	ClassDB::bind_method(D_METHOD("get_auto_approve_permission", "permission"), &SolersPermissionManager::get_auto_approve_permission);
	ClassDB::bind_method(D_METHOD("request_user_approval", "tool_name", "args", "permission"), &SolersPermissionManager::request_user_approval);
	ClassDB::bind_method(D_METHOD("list_pending_requests"), &SolersPermissionManager::list_pending_requests);
	ClassDB::bind_method(D_METHOD("get_pending_request_count"), &SolersPermissionManager::get_pending_request_count);
	ClassDB::bind_method(D_METHOD("approve_request", "request_id"), &SolersPermissionManager::approve_request);
	ClassDB::bind_method(D_METHOD("reject_request", "request_id"), &SolersPermissionManager::reject_request);
	ClassDB::bind_method(D_METHOD("consume_approval", "request_id", "tool_name"), &SolersPermissionManager::consume_approval);
	ClassDB::bind_method(D_METHOD("get_request_decision", "request_id"), &SolersPermissionManager::get_request_decision);

	BIND_ENUM_CONSTANT(PERMISSION_OBSERVE);
	BIND_ENUM_CONSTANT(PERMISSION_EDIT_SCENE);
	BIND_ENUM_CONSTANT(PERMISSION_EDIT_FILES);
	BIND_ENUM_CONSTANT(PERMISSION_RUN_PROJECT);
	BIND_ENUM_CONSTANT(PERMISSION_EXPORT_BUILD);
	BIND_ENUM_CONSTANT(PERMISSION_NETWORK);
	BIND_ENUM_CONSTANT(PERMISSION_INSTALL_PLUGIN);
	BIND_ENUM_CONSTANT(PERMISSION_SHELL);

	BIND_ENUM_CONSTANT(DECISION_UNKNOWN);
	BIND_ENUM_CONSTANT(DECISION_PENDING);
	BIND_ENUM_CONSTANT(DECISION_APPROVED);
	BIND_ENUM_CONSTANT(DECISION_REJECTED);
}

int SolersPermissionManager::_find_pending_request_index(int p_request_id) const {
	for (int i = 0; i < pending_requests.size(); i++) {
		const Dictionary request = pending_requests[i];
		if ((int)request.get("id", 0) == p_request_id) {
			return i;
		}
	}
	return -1;
}

String SolersPermissionManager::get_permission_name(Permission p_permission) const {
	switch (p_permission) {
		case PERMISSION_OBSERVE:
			return "observe";
		case PERMISSION_EDIT_SCENE:
			return "edit_scene";
		case PERMISSION_EDIT_FILES:
			return "edit_files";
		case PERMISSION_RUN_PROJECT:
			return "run_project";
		case PERMISSION_EXPORT_BUILD:
			return "export_build";
		case PERMISSION_NETWORK:
			return "network";
		case PERMISSION_INSTALL_PLUGIN:
			return "install_plugin";
		case PERMISSION_SHELL:
			return "shell";
		case PERMISSION_MAX:
			break;
	}

	return "unknown";
}

bool SolersPermissionManager::is_auto_approved(Permission p_permission) const {
	ERR_FAIL_INDEX_V(p_permission, PERMISSION_MAX, false);
	const PermissionRule &rule = rules[p_permission];
	if (rule.locked) {
		return rule.allow;
	}
	return auto_approve_all || rule.allow;
}

bool SolersPermissionManager::is_auto_approve_all() const {
	return auto_approve_all;
}

void SolersPermissionManager::set_auto_approve_all(bool p_enabled) {
	auto_approve_all = p_enabled;
}

void SolersPermissionManager::set_auto_approve_permission(Permission p_permission, bool p_enabled) {
	ERR_FAIL_INDEX(p_permission, PERMISSION_MAX);
	if (rules[p_permission].locked) {
		return;
	}
	rules[p_permission].allow = p_enabled;
}

bool SolersPermissionManager::get_auto_approve_permission(Permission p_permission) const {
	ERR_FAIL_INDEX_V(p_permission, PERMISSION_MAX, false);
	return rules[p_permission].allow;
}

Dictionary SolersPermissionManager::request_user_approval(const StringName &p_tool_name, const Dictionary &p_args, Permission p_permission) {
	Dictionary request;
	request["id"] = next_request_id++;
	request["tool"] = p_tool_name;
	request["args"] = p_args;
	request["permission_id"] = (int)p_permission;
	request["permission"] = get_permission_name(p_permission);
	if (is_auto_approved(p_permission)) {
		approved_once_requests[request["id"]] = p_tool_name;
		return request;
	}
	pending_requests.push_back(request);
	return request;
}

Array SolersPermissionManager::list_pending_requests() const {
	return pending_requests.duplicate();
}

int SolersPermissionManager::get_pending_request_count() const {
	return pending_requests.size();
}

bool SolersPermissionManager::approve_request(int p_request_id) {
	const int index = _find_pending_request_index(p_request_id);
	if (index < 0) {
		return false;
	}
	const Dictionary request = pending_requests[index];
	approved_once_requests[p_request_id] = request.get("tool", StringName());
	rejected_request_ids.erase(p_request_id);
	pending_requests.remove_at(index);
	return true;
}

bool SolersPermissionManager::reject_request(int p_request_id) {
	const int index = _find_pending_request_index(p_request_id);
	if (index < 0) {
		return false;
	}
	rejected_request_ids.insert(p_request_id);
	approved_once_requests.erase(p_request_id);
	pending_requests.remove_at(index);
	return true;
}

bool SolersPermissionManager::consume_approval(int p_request_id, const StringName &p_tool_name) {
	// Approvals are consumed strictly by request id: the id travels with the
	// blocked call and back through its retry, so a grant can never leak onto
	// a different call that merely shares the tool name.
	if (p_request_id <= 0) {
		return false;
	}
	if (rejected_request_ids.has(p_request_id) || !approved_once_requests.has(p_request_id)) {
		return false;
	}
	const StringName approved_tool = StringName(approved_once_requests[p_request_id]);
	if (approved_tool != p_tool_name) {
		return false;
	}
	approved_once_requests.erase(p_request_id);
	return true;
}

SolersPermissionManager::RequestDecision SolersPermissionManager::get_request_decision(int p_request_id) const {
	if (p_request_id <= 0) {
		return DECISION_UNKNOWN;
	}
	if (approved_once_requests.has(p_request_id)) {
		return DECISION_APPROVED;
	}
	if (rejected_request_ids.has(p_request_id)) {
		return DECISION_REJECTED;
	}
	if (_find_pending_request_index(p_request_id) >= 0) {
		return DECISION_PENDING;
	}
	return DECISION_UNKNOWN;
}

SolersPermissionManager::SolersPermissionManager() {
	// Observation is always allowed; third-party plugin installation always
	// asks. Both are locked trust boundaries, everything else defaults to ask
	// and can be relaxed per session ("Allow always" / auto-approve mode).
	rules[PERMISSION_OBSERVE] = PermissionRule{ /*allow*/ true, /*locked*/ true };
	rules[PERMISSION_INSTALL_PLUGIN] = PermissionRule{ /*allow*/ false, /*locked*/ true };
}
