/**************************************************************************/
/*  solers_script_context.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "solers_script_context.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

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
	return (!cancel_path.is_empty() && FileAccess::exists(cancel_path)) || (deadline_msec > 0 && OS::get_singleton()->get_ticks_msec() >= deadline_msec);
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
