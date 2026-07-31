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

// Session-list index derived from the append-only transcript. The UI must not
// re-parse every tool_result line on every open/click; writes keep this warm.
struct SolersSessionIndexEntry {
	SolersSessionInfo info;
	bool has_user = false;
	bool has_title = false;
};

struct SolersSessionIndex {
	String project_path;
	bool valid = false;
	HashMap<String, SolersSessionIndexEntry> by_id;
};

static SolersSessionIndex solers_session_index;

static void solers_session_index_clear() {
	solers_session_index.valid = false;
	solers_session_index.project_path.clear();
	solers_session_index.by_id.clear();
}

static void solers_session_index_apply_chat_event(const Dictionary &p_event, bool p_require_project_match) {
	const String project_path = String(p_event.get("project_path", String())).strip_edges();
	const String session_id = String(p_event.get("session_id", String())).strip_edges();
	if (project_path.is_empty() || session_id.is_empty()) {
		return;
	}
	if (p_require_project_match && project_path != solers_session_index.project_path) {
		return;
	}

	const String role = p_event.get("role", String());
	if (role != "user" && role != "assistant") {
		return;
	}

	SolersSessionIndexEntry *entry = solers_session_index.by_id.getptr(session_id);
	if (!entry) {
		SolersSessionIndexEntry created;
		created.info.session_id = session_id;
		created.info.title = "Current chat";
		created.info.wall = (int64_t)p_event.get("wall", 0);
		solers_session_index.by_id[session_id] = created;
		entry = solers_session_index.by_id.getptr(session_id);
	}
	if (!entry) {
		return;
	}
	if (p_event.has("wall")) {
		entry->info.wall = (int64_t)p_event.get("wall", 0);
	}
	entry->info.message_count++;
	if (role == "user") {
		entry->has_user = true;
		if (!entry->has_title) {
			const String title = String(p_event.get("content", String())).strip_edges().replace("\r", " ").replace("\n", " ").strip_edges();
			if (!title.is_empty()) {
				entry->info.title = title;
				entry->has_title = true;
			}
		}
	}
}

static void solers_session_index_note_event(const Dictionary &p_event) {
	const String project_path = String(p_event.get("project_path", String())).strip_edges();
	const String session_id = String(p_event.get("session_id", String())).strip_edges();
	if (project_path.is_empty() || session_id.is_empty()) {
		return;
	}
	if (!solers_session_index.valid || solers_session_index.project_path != project_path) {
		// Cold or cross-project: leave rebuild to the next list_sessions call.
		if (solers_session_index.valid && solers_session_index.project_path != project_path) {
			solers_session_index_clear();
		}
		return;
	}
	solers_session_index_apply_chat_event(p_event, false);
}

static bool solers_transcript_line_may_index(const String &p_line) {
	// Writers use compact JSON (JSON::stringify with empty indent). Index only
	// needs chat roles — positive filter, not a tool-name/event blacklist.
	return p_line.find("\"role\":\"user\"") >= 0 || p_line.find("\"role\":\"assistant\"") >= 0;
}

static void solers_session_index_rebuild(const String &p_project_path) {
	solers_session_index_clear();
	solers_session_index.project_path = p_project_path;

	solers_transcript_foreach_line(nullptr, [](void *, const String &p_record) -> bool {
		if (!solers_transcript_line_may_index(p_record)) {
			return true;
		}
		Dictionary event;
		if (!solers_transcript_parse_record(p_record, event)) {
			return true;
		}
		solers_session_index_apply_chat_event(event, true);
		return true;
	});
	solers_session_index.valid = true;
}

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
	Dictionary event = p_event.duplicate(true);
	event["ticks_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	{
		MutexLock lock(solers_transcript_mutex);
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
	solers_session_index_note_event(event);
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

void solers_transcript_foreach_line(void *p_userdata, bool (*p_callback)(void *p_userdata, const String &p_line)) {
	ERR_FAIL_NULL(p_callback);
	if (solers_transcript_io_active) {
		return;
	}
	solers_transcript_io_active = true;
	{
		MutexLock lock(solers_transcript_mutex);
		Ref<FileAccess> file = FileAccess::open(solers_session_dir().path_join("transcript.jsonl"), FileAccess::READ);
		if (file.is_valid()) {
			while (!file->eof_reached()) {
				if (!p_callback(p_userdata, file->get_line())) {
					break;
				}
			}
		}
	}
	solers_transcript_io_active = false;
}

bool solers_transcript_line_has_session(const String &p_line, const String &p_session_id) {
	if (p_session_id.is_empty() || p_line.is_empty()) {
		return false;
	}
	// Cheap authority filter before JSON::parse. Session ids are opaque tokens
	// written by us; writers use compact JSON so one form is enough.
	return p_line.find("\"session_id\":\"" + p_session_id + "\"") >= 0;
}

String solers_transcript_strip_json_string_field(const String &p_line, const String &p_key) {
	if (p_line.is_empty() || p_key.is_empty()) {
		return p_line;
	}
	const String key_token = "\"" + p_key + "\"";
	const int key_pos = p_line.find(key_token);
	if (key_pos < 0) {
		return p_line;
	}
	auto is_ws = [](char32_t c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r';
	};
	int i = key_pos + key_token.length();
	while (i < p_line.length() && is_ws(p_line[i])) {
		i++;
	}
	if (i >= p_line.length() || p_line[i] != ':') {
		return p_line;
	}
	i++;
	while (i < p_line.length() && is_ws(p_line[i])) {
		i++;
	}
	if (i >= p_line.length() || p_line[i] != '"') {
		return p_line;
	}
	i++; // opening quote of the value
	while (i < p_line.length()) {
		const char32_t ch = p_line[i];
		if (ch == '\\') {
			i += i + 1 < p_line.length() ? 2 : 1;
			continue;
		}
		if (ch == '"') {
			i++;
			break;
		}
		i++;
	}
	int remove_end = i;
	while (remove_end < p_line.length() && is_ws(p_line[remove_end])) {
		remove_end++;
	}
	if (remove_end < p_line.length() && p_line[remove_end] == ',') {
		remove_end++;
	}
	int remove_start = key_pos;
	int before = key_pos - 1;
	while (before >= 0 && is_ws(p_line[before])) {
		before--;
	}
	if (before >= 0 && p_line[before] == ',') {
		remove_start = before;
	}
	return p_line.substr(0, remove_start) + p_line.substr(remove_end);
}

Vector<SolersSessionInfo> solers_list_sessions(const String &p_project_path) {
	Vector<SolersSessionInfo> sessions;
	const String project_path = p_project_path.strip_edges();
	if (project_path.is_empty()) {
		return sessions;
	}

	if (!solers_session_index.valid || solers_session_index.project_path != project_path) {
		solers_session_index_rebuild(project_path);
	}

	for (const KeyValue<String, SolersSessionIndexEntry> &kv : solers_session_index.by_id) {
		if (kv.value.has_user) {
			sessions.push_back(kv.value.info);
		}
	}
	return sessions;
}
