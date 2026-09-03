/**************************************************************************/
/*  solers_authoring_tools.cpp                                            */
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

#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

void SolersToolRegistry::_register_script_tools() {
	if (!script_service) {
		return;
	}
	SolersScriptService *service = script_service;

	_add("project.settings", "Apply ProjectSettings changes through the live singleton and one native UndoRedo action.", R"({"type":"object","properties":{"values":{"type":"object","writeOnly":true},"erase":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"expected_sha256":{"type":"string","minLength":64,"maxLength":64}},"additionalProperties":false})", [service](const SolersToolContext &ctx, const Dictionary &a) {
		Dictionary args = a.duplicate(true);
		args["operation"] = "settings";
		return service->edit_project_with_context(args, ctx);
	});

	_add("edit", "Atomically create, replace, write, or remove one project path. Registered script languages are parser-validated before commit; expected_sha256 rejects stale file content.", R"({"type":"object","properties":{"operation":{"type":"string","enum":["create","replace","write","create_directory","remove"]},"path":{"type":"string","minLength":6},"content":{"type":"string","writeOnly":true},"old_text":{"type":"string","minLength":1,"writeOnly":true},"new_text":{"type":"string","writeOnly":true},"expected_sha256":{"type":"string","minLength":64,"maxLength":64}},"required":["operation","path"],"additionalProperties":false})", [service](const SolersToolContext &ctx, const Dictionary &a) {
		return service->edit_path_with_context(a, ctx);
	});

	_add("script.validate", "Validate source through the ScriptLanguage registered by Godot for the supplied project path.", R"({"type":"object","properties":{"path":{"type":"string","minLength":6},"source":{"type":"string","writeOnly":true}},"required":["path"],"additionalProperties":false})", [service](const SolersToolContext &, const Dictionary &a) {
		return service->validate_script(a);
	});

	_add("editor.script", "Run a bounded GDScript transaction against an authority registered by the editor host. target_path and outputs are project paths; the authority resolves the native Godot subject.", R"({"type":"object","properties":{"authority":{"type":"string","minLength":1},"target_path":{"type":"string","minLength":6},"source":{"type":"string","minLength":1,"writeOnly":true},"script_path":{"type":"string","minLength":6},"outputs":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"string","minLength":6}},"expected_sha256":{"type":"string","minLength":64,"maxLength":64},"timeout_msec":{"type":"integer","minimum":1000,"maximum":600000}},"required":["authority","target_path"],"additionalProperties":false})", [service](const SolersToolContext &ctx, const Dictionary &a) {
		return service->start_authority_script(StringName(a.get("authority", String())), a, ctx.call_id, &ctx);
	}, [service](const SolersToolContext &, const Dictionary &a) {
		return service->poll_authority_script(a);
	}, [service](const SolersToolContext &, const Dictionary &a) {
		return service->is_authority_script_ready(a);
	}, [service](const SolersToolContext &ctx, const Dictionary &, const Dictionary &) {
		service->complete_authority_script(Dictionary({ { "call_id", ctx.call_id } }));
	});
}
