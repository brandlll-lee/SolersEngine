/**************************************************************************/
/*  solers_trace.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_trace.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"

static Mutex solers_transcript_mutex;
static thread_local bool solers_transcript_io_active = false;

// Session artifacts (attachments, transcript, traces) live inside the project
// at res://.solers/ next to plugins.lock.json. user:// is resolved from
// application/config/name, so a project rename silently redirects it and
// strands every artifact written before the rename; the project directory
// itself is the only stable anchor. Dot-directories are already invisible to
// the editor filesystem scanner and importers.
String solers_session_dir() {
	static bool ensured = false;
	if (!ensured) {
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_RESOURCES);
		ensured = dir.is_valid() && dir->make_dir_recursive(".solers") == OK;
	}
	return "res://.solers";
}

void solers_trace_write(const String &p_where, const String &p_msg) {
	const String line = vformat("[SOLERS] %s | %s", p_where, p_msg);
	print_line(line);

	const String trace_path = solers_session_dir().path_join("trace.log");
	Ref<FileAccess> f = FileAccess::open(trace_path, FileAccess::READ_WRITE);
	if (f.is_null()) {
		f = FileAccess::open(trace_path, FileAccess::WRITE);
	}
	if (f.is_valid()) {
		f->seek_end();
		f->store_line(vformat("%d %s", (int64_t)OS::get_singleton()->get_ticks_msec(), line));
		f->flush();
	}
}

void solers_transcript_write(const Dictionary &p_event) {
	// The transcript is an append-only audit sink. Do not let an I/O diagnostic
	// recursively enter the same sink, and serialize worker/main-thread appends.
	if (solers_transcript_io_active) {
		return;
	}
	solers_transcript_io_active = true;
	{
		MutexLock lock(solers_transcript_mutex);
		Dictionary event = p_event.duplicate(true);
		event["ticks_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
		const String transcript_path = solers_session_dir().path_join("transcript.jsonl");
		Ref<FileAccess> file = FileAccess::open(transcript_path, FileAccess::READ_WRITE);
		if (file.is_null()) {
			file = FileAccess::open(transcript_path, FileAccess::WRITE);
		}
		if (file.is_valid()) {
			file->seek_end();
			file->store_line(JSON::stringify(event, "", false, true));
			file->flush();
		}
	}
	solers_transcript_io_active = false;
}

Vector<String> solers_transcript_read_snapshot() {
	Vector<String> lines;
	if (solers_transcript_io_active) {
		return lines;
	}
	solers_transcript_io_active = true;
	{
		MutexLock lock(solers_transcript_mutex);
		Ref<FileAccess> file = FileAccess::open(solers_session_dir().path_join("transcript.jsonl"), FileAccess::READ);
		if (file.is_valid()) {
			while (!file->eof_reached()) {
				lines.push_back(file->get_line());
			}
		}
	}
	solers_transcript_io_active = false;
	return lines;
}

bool solers_transcript_parse_record(const String &p_line, Dictionary &r_event) {
	r_event.clear();
	const String line = p_line.strip_edges();
	if (line.is_empty()) {
		return false;
	}
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(line) != OK || json->get_data().get_type() != Variant::DICTIONARY) {
		return false;
	}
	r_event = json->get_data();
	return true;
}

Vector<SolersSessionInfo> solers_list_sessions(const String &p_project_path) {
	Vector<SolersSessionInfo> sessions;
	const String project_path = p_project_path.strip_edges();
	if (project_path.is_empty()) {
		return sessions;
	}

	struct Scratch {
		SolersSessionInfo info;
		bool has_user = false;
		bool has_title = false;
	};
	Vector<Scratch> scratch;
	HashMap<String, int> by_id;

	const Vector<String> transcript_lines = solers_transcript_read_snapshot();
	for (const String &record : transcript_lines) {
		Dictionary event;
		if (!solers_transcript_parse_record(record, event)) {
			continue;
		}
		if (String(event.get("project_path", String())) != project_path) {
			continue;
		}
		const String session_id = String(event.get("session_id", String())).strip_edges();
		if (session_id.is_empty()) {
			continue;
		}

		if (!by_id.has(session_id)) {
			Scratch entry;
			entry.info.session_id = session_id;
			entry.info.title = "Current chat";
			entry.info.wall = (int64_t)event.get("wall", 0);
			scratch.push_back(entry);
			by_id[session_id] = scratch.size() - 1;
		}

		Scratch &entry = scratch.write[by_id[session_id]];
		if (event.has("wall")) {
			entry.info.wall = (int64_t)event.get("wall", 0);
		}
		const String role = event.get("role", String());
		if (role == "user" || role == "assistant") {
			entry.info.message_count++;
		}
		if (role == "user") {
			entry.has_user = true;
			if (!entry.has_title) {
				const String title = String(event.get("content", String())).strip_edges().replace("\r", " ").replace("\n", " ").strip_edges();
				if (!title.is_empty()) {
					entry.info.title = title;
					entry.has_title = true;
				}
			}
		}
	}

	for (const Scratch &entry : scratch) {
		if (entry.has_user) {
			sessions.push_back(entry.info);
		}
	}
	return sessions;
}
