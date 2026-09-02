/**************************************************************************/
/*  solers_script_jobs.cpp                                                */
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

#include "core/io/json.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/main/node.h"

#include "modules/solers_ai/core/solers_script_context.h"

static bool _lightmap_bake_progress(float p_completion, const String &p_message, void *p_userdata, bool) {
	SolersScriptContext *context = static_cast<SolersScriptContext *>(p_userdata);
	context->report_progress(p_completion, p_message);
	return context->is_cancelled();
}

static Dictionary _run_lightmap_bake(SolersScriptContext *p_context, Object *p_target, const Dictionary &p_args) {
	LightmapGI *lightmap = Object::cast_to<LightmapGI>(p_target);
	Node *scene_root = Object::cast_to<Node>(p_context->get_subject());
	const String output_path = String(p_args.get("output_path", String())).simplify_path();
	if (!lightmap || !scene_root || !output_path.begins_with("res://")) {
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "INVALID_ARGUMENT" }, { "message", "lightmap.bake requires a scene LightmapGI target and res:// output_path." }, { "recoverable", true } }) } });
	}
	if (!p_context->is_output_declared(output_path)) {
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "OUTPUT_NOT_DECLARED" }, { "message", "Declare output_path in scene.script outputs before starting the native job." }, { "recoverable", true } }) } });
	}
	Node *from = scene_root;
	const NodePath from_path = p_args.get("from_path", NodePath());
	if (!from_path.is_empty()) {
		from = scene_root->get_node_or_null(from_path);
		if (!from) {
			return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "NODE_NOT_FOUND" }, { "message", vformat("Lightmap source node was not found: %s", String(from_path)) }, { "recoverable", true } }) } });
		}
	}
	const LightmapGI::BakeError error = lightmap->bake(from, output_path, _lightmap_bake_progress, p_context);
	if (error != LightmapGI::BAKE_ERROR_OK) {
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", error == LightmapGI::BAKE_ERROR_USER_ABORTED ? "NATIVE_JOB_CANCELLED" : "NATIVE_JOB_FAILED" }, { "message", vformat("LightmapGI bake failed with native error %d.", error) }, { "recoverable", true }, { "native_error", error } }) } });
	}
	p_context->mark_changed();
	p_context->report_progress(1.0, "Complete");
	return Dictionary({ { "ok", true }, { "data", Dictionary({ { "output_path", output_path }, { "native_error", error } }) } });
}

void solers_script_jobs_initialize() {
	SolersNativeScriptJob lightmap;
	lightmap.id = SNAME("lightmap.bake");
	lightmap.target_class = SNAME("LightmapGI");
	lightmap.description = "Bake native LightmapGI data with progress and cancellation.";
	lightmap.authorities.push_back("scene");
	lightmap.input_schema = JSON::parse_string(R"({"type":"object","properties":{"output_path":{"type":"string","pattern":"^res://"},"from_path":{"type":"string"}},"required":["output_path"],"additionalProperties":false})");
	lightmap.run = _run_lightmap_bake;
	SolersScriptContext::register_native_job(lightmap);
}

void solers_script_jobs_uninitialize() {
	SolersScriptContext::clear_native_jobs();
}
