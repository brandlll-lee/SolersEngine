/**************************************************************************/
/*  solers_script_context.cpp                                             */
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

#include "solers_script_context.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/input/input.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

Vector<SolersNativeScriptJob> SolersScriptContext::native_jobs;

void SolersScriptContext::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_authority"), &SolersScriptContext::get_authority);
	ClassDB::bind_method(D_METHOD("get_subject"), &SolersScriptContext::get_subject);
	ClassDB::bind_method(D_METHOD("get_source_path"), &SolersScriptContext::get_source_path);
	ClassDB::bind_method(D_METHOD("get_import_options"), &SolersScriptContext::get_import_options);
	ClassDB::bind_method(D_METHOD("set_import_option", "name", "value"), &SolersScriptContext::set_import_option);
	ClassDB::bind_method(D_METHOD("mark_changed"), &SolersScriptContext::mark_changed);
	ClassDB::bind_method(D_METHOD("request_reimport"), &SolersScriptContext::request_reimport);
	ClassDB::bind_method(D_METHOD("log", "value"), &SolersScriptContext::log);
	ClassDB::bind_method(D_METHOD("fail", "code", "message"), &SolersScriptContext::fail);
	ClassDB::bind_method(D_METHOD("set_result", "result"), &SolersScriptContext::set_result);
	ClassDB::bind_method(D_METHOD("list_native_jobs", "target"), &SolersScriptContext::list_native_jobs);
	ClassDB::bind_method(D_METHOD("run_native_job", "id", "target", "args"), &SolersScriptContext::run_native_job);
	ClassDB::bind_method(D_METHOD("is_cancelled"), &SolersScriptContext::is_cancelled);
	ClassDB::bind_method(D_METHOD("input_event", "event"), &SolersScriptContext::input_event);
	ClassDB::bind_method(D_METHOD("wait_physics_frames", "frames"), &SolersScriptContext::wait_physics_frames);
	ClassDB::bind_method(D_METHOD("capture"), &SolersScriptContext::capture);
	ADD_SIGNAL(MethodInfo("physics_completed"));
	ADD_SIGNAL(MethodInfo("capture_completed", PropertyInfo(Variant::DICTIONARY, "result")));

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "authority"), "", "get_authority");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "subject"), "", "get_subject");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path"), "", "get_source_path");
}

void SolersScriptContext::register_native_job(const SolersNativeScriptJob &p_job) {
	ERR_FAIL_COND(p_job.id.is_empty() || p_job.target_class.is_empty() || !p_job.run);
	for (int i = 0; i < native_jobs.size(); i++) {
		ERR_FAIL_COND_MSG(native_jobs[i].id == p_job.id, vformat("Solers native script job already registered: %s", p_job.id));
	}
	native_jobs.push_back(p_job);
}

void SolersScriptContext::clear_native_jobs() {
	native_jobs.clear();
}

void SolersScriptContext::initialize(const StringName &p_authority, const Variant &p_subject, const String &p_source_path,
		const Dictionary &p_import_options, bool p_import_controls, const PackedStringArray &p_outputs, const String &p_progress_path,
		const String &p_cancel_path, uint64_t p_deadline_msec) {
	authority = p_authority;
	subject = p_subject;
	source_path = p_source_path;
	import_options = p_import_options.duplicate(true);
	import_controls = p_import_controls;
	outputs = p_outputs;
	progress_path = p_progress_path;
	cancel_path = p_cancel_path;
	deadline_msec = p_deadline_msec;
}

Object *SolersScriptContext::get_subject() const {
	return subject.get_type() == Variant::OBJECT ? subject.operator Object *() : nullptr;
}

void SolersScriptContext::set_import_option(const StringName &p_name, const Variant &p_value) {
	if (!import_controls) {
		fail("AUTHORITY_MISMATCH", "This script authority does not expose import options.");
		return;
	}
	import_options[p_name] = p_value;
	changed = true;
	reimport = true;
}

void SolersScriptContext::request_reimport() {
	if (!import_controls) {
		fail("AUTHORITY_MISMATCH", "This script authority does not expose reimport.");
		return;
	}
	reimport = true;
	changed = true;
}

void SolersScriptContext::log(const Variant &p_value) {
	if (logs.size() < 128) {
		logs.push_back(p_value);
	}
}

void SolersScriptContext::fail(const String &p_code, const String &p_message) {
	if (!failure.is_empty()) {
		return;
	}
	failure["code"] = p_code.is_empty() ? String("SCRIPT_FAILED") : p_code;
	failure["message"] = p_message;
	failure["recoverable"] = true;
}

Array SolersScriptContext::list_native_jobs(Object *p_target) const {
	Array jobs;
	if (!p_target) {
		return jobs;
	}
	for (const SolersNativeScriptJob &job : native_jobs) {
		if ((job.authorities.is_empty() || job.authorities.has(authority)) && ClassDB::is_parent_class(p_target->get_class_name(), job.target_class)) {
			jobs.push_back(Dictionary({ { "id", job.id }, { "target_class", job.target_class }, { "description", job.description }, { "input_schema", job.input_schema } }));
		}
	}
	return jobs;
}

Dictionary SolersScriptContext::run_native_job(const StringName &p_id, Object *p_target, const Dictionary &p_args) {
	if (!p_target) {
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "INVALID_TARGET" }, { "message", "A live Godot target is required." }, { "recoverable", true } }) } });
	}
	for (const SolersNativeScriptJob &job : native_jobs) {
		if (job.id != p_id) {
			continue;
		}
		if (!ClassDB::is_parent_class(p_target->get_class_name(), job.target_class)) {
			return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "TARGET_TYPE_MISMATCH" }, { "message", vformat("Job %s requires %s.", p_id, job.target_class) }, { "recoverable", true } }) } });
		}
		if (!job.authorities.is_empty() && !job.authorities.has(authority)) {
			return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "AUTHORITY_MISMATCH" }, { "message", vformat("Job %s is unavailable in %s.script.", p_id, authority) }, { "recoverable", true } }) } });
		}
		return job.run(this, p_target, p_args);
	}
	return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "NATIVE_JOB_NOT_FOUND" }, { "message", vformat("Unknown native job: %s", p_id) }, { "recoverable", true } }) } });
}

bool SolersScriptContext::is_cancelled() const {
	return finished || (!cancel_path.is_empty() && FileAccess::exists(cancel_path)) || (deadline_msec > 0 && OS::get_singleton()->get_ticks_msec() >= deadline_msec);
}

bool SolersScriptContext::input_event(const Ref<InputEvent> &p_event) {
	if (authority != SNAME("runtime") || is_cancelled() || p_event.is_null()) {
		fail("INPUT_UNAVAILABLE", "Input requires an active runtime script and a native InputEvent.");
		return false;
	}
	for (int i = held_inputs.size() - 1; i >= 0; i--) {
		if (held_inputs[i]->get_device() == p_event->get_device() && held_inputs[i]->is_match(p_event, false)) {
			held_inputs.remove_at(i);
		}
	}
	Ref<InputEvent> release = p_event->duplicate();
	Ref<InputEventKey> key = release;
	if (key.is_valid()) {
		key->set_echo(false);
	}
	Ref<InputEventJoypadMotion> axis = release;
	if (axis.is_valid()) {
		axis->set_axis_value(0);
		held_inputs.push_back(release);
	} else if (p_event->is_pressed()) {
		bool valid = false;
		release->set("pressed", false, &valid);
		if (valid) {
			held_inputs.push_back(release);
		}
	}
	Input::get_singleton()->parse_input_event(p_event);
	Input::get_singleton()->flush_buffered_events();
	return true;
}

Signal SolersScriptContext::wait_physics_frames(int p_frames) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || p_frames < 1 || physics_frames > 0 || is_cancelled()) {
		fail("PHYSICS_WAIT_UNAVAILABLE", "Wait for a positive number of physics frames, one wait at a time.");
		return Signal();
	}
	physics_frames = p_frames;
	// Deferred delivery observes node physics processing from the completed frame.
	tree->connect(SNAME("physics_frame"), callable_mp(this, &SolersScriptContext::_physics_frame), CONNECT_DEFERRED);
	return Signal(this, SNAME("physics_completed"));
}

void SolersScriptContext::_physics_frame() {
	if (physics_frames > 0 && --physics_frames == 0) {
		SceneTree::get_singleton()->disconnect(SNAME("physics_frame"), callable_mp(this, &SolersScriptContext::_physics_frame));
		emit_signal(SNAME("physics_completed"));
	}
}

Signal SolersScriptContext::capture() {
	RenderingServer::get_singleton()->request_frame_drawn_callback(callable_mp(this, &SolersScriptContext::_capture_frame));
	return Signal(this, SNAME("capture_completed"));
}

void SolersScriptContext::_capture_frame() {
	if (is_cancelled()) {
		return;
	}
	const String id = vformat("runtime_%d_%d_%d", OS::get_singleton()->get_process_id(), (int64_t)get_instance_id(), captures.size());
	const Dictionary receipt = capture_runtime_image(id);
	if ((bool)receipt.get("ok", false)) {
		captures.append_array(receipt.get("attachments", Array()));
	}
	emit_signal(SNAME("capture_completed"), receipt);
}

void SolersScriptContext::finish() {
	finished = true;
	if (physics_frames > 0 && SceneTree::get_singleton()) {
		SceneTree::get_singleton()->disconnect(SNAME("physics_frame"), callable_mp(this, &SolersScriptContext::_physics_frame));
		physics_frames = 0;
	}
	if (Input::get_singleton() && !held_inputs.is_empty()) {
		for (const Ref<InputEvent> &event : held_inputs) {
			Input::get_singleton()->parse_input_event(event);
		}
		Input::get_singleton()->flush_buffered_events();
	}
	held_inputs.clear();
}

SolersScriptContext::~SolersScriptContext() {
	finish();
}

Dictionary SolersScriptContext::store_image(const Ref<Image> &p_image, const String &p_target, const String &p_capture_id) {
	auto error = [](const String &p_code, const String &p_message) {
		return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", p_code }, { "message", p_message }, { "recoverable", true } }) } });
	};
	if (p_image.is_null() || p_image->is_compressed() || p_image->get_width() <= 1 || p_image->get_height() <= 1) {
		return error("VIEWPORT_IMAGE_UNAVAILABLE", "The viewport has not produced a readable frame.");
	}
	Ref<Image> image = p_image;
	const int dimension = MAX(image->get_width(), image->get_height());
	if (dimension > 1280) {
		image = p_image->duplicate();
		image->resize(MAX(1, image->get_width() * 1280 / dimension), MAX(1, image->get_height() * 1280 / dimension), Image::INTERPOLATE_LANCZOS);
	}
	const String directory = ProjectSettings::get_singleton()->globalize_path(ProjectSettings::get_singleton()->get_project_data_path().path_join("solers/captures"));
	if (DirAccess::make_dir_recursive_absolute(directory) != OK) {
		return error("CAPTURE_DIRECTORY_FAILED", "Could not create the capture directory.");
	}
	const String temporary = directory.path_join(p_capture_id + ".tmp.png");
	if (image->save_png(temporary) != OK) {
		return error("CAPTURE_SAVE_FAILED", "Could not save viewport evidence.");
	}
	const String sha = FileAccess::get_sha256(temporary);
	const String path = directory.path_join(sha + ".png");
	if (FileAccess::exists(path)) {
		DirAccess::remove_absolute(temporary);
	} else if (DirAccess::rename_absolute(temporary, path) != OK) {
		return error("CAPTURE_SAVE_FAILED", "Could not publish viewport evidence.");
	}
	Dictionary attachment({ { "id", p_capture_id }, { "source", "tool_capture" }, { "target", p_target }, { "type", "image" }, { "mime_type", "image/png" }, { "local_path", path }, { "width", image->get_width() }, { "height", image->get_height() }, { "content_sha256", sha } });
	Dictionary data({ { "status", "complete" }, { "target", p_target }, { "capture_id", p_capture_id }, { "width", image->get_width() }, { "height", image->get_height() }, { "content_sha256", sha }, { "attachment", attachment } });
	return Dictionary({ { "ok", true }, { "data", data }, { "attachments", Array({ attachment }) } });
}

Dictionary SolersScriptContext::capture_runtime_image(const String &p_capture_id) {
	Window *root = SceneTree::get_singleton() ? SceneTree::get_singleton()->get_root() : nullptr;
	Dictionary receipt = store_image(root ? root->get_texture()->get_image() : Ref<Image>(), "runtime", p_capture_id);
	Dictionary data = receipt.get("data", Dictionary());
	data["runtime_frame"] = (int64_t)Engine::get_singleton()->get_frames_drawn();
	data["physics_frame"] = (int64_t)Engine::get_singleton()->get_physics_frames();
	if ((bool)receipt.get("ok", false)) {
		Dictionary attachment = data["attachment"];
		attachment["runtime_frame"] = data["runtime_frame"];
		attachment["physics_frame"] = data["physics_frame"];
		receipt["attachments"] = Array({ attachment });
	}
	receipt["data"] = data;
	return receipt;
}

bool SolersScriptContext::is_output_declared(const String &p_path) const {
	return outputs.has(p_path.simplify_path());
}

void SolersScriptContext::_write_progress(float p_completion, const String &p_message) const {
	if (progress_path.is_empty()) {
		return;
	}
	Ref<FileAccess> file = FileAccess::open(progress_path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(JSON::stringify(Dictionary({ { "completion", CLAMP(p_completion, 0.0f, 1.0f) }, { "message", p_message } })));
	}
}

void SolersScriptContext::report_progress(float p_completion, const String &p_message) const {
	_write_progress(p_completion, p_message);
}
