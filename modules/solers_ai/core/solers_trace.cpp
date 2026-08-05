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

static Mutex transcript_mutex;
static thread_local bool transcript_io_active = false;
static String journal_session_id;
static Ref<FileAccess> journal_file;
static Ref<FileAccess> trace_file;

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

static SolersSessionIndex session_index;
static constexpr int JOURNAL_SCHEMA = 5;

String solers_session_dir() {
	static bool ensured = false;
	if (!ensured) {
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_RESOURCES);
		ensured = dir.is_valid() && dir->make_dir_recursive(".solers/sessions") == OK;
	}
	return "res://.solers";
}

static String journal_path(const String &p_session_id) {
	return solers_session_dir().path_join("sessions").path_join(p_session_id.sha256_text() + ".jsonl");
}

static Ref<FileAccess> append_file(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ_WRITE);
	if (file.is_null()) {
		file = FileAccess::open(p_path, FileAccess::WRITE);
	}
	if (file.is_valid()) {
		file->seek_end();
	}
	return file;
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

bool solers_transcript_is_human_message(const Dictionary &p_event) {
	if (String(p_event.get("event_type", String())) != "message" || String(p_event.get("role", String())) != "user") {
		return false;
	}
	return p_event.has("author") ? String(p_event["author"]) == "human" : String(p_event.get("origin", String())).is_empty();
}

static void index_event(const Dictionary &p_event) {
	const String project_path = String(p_event.get("project_path", String())).strip_edges();
	const String session_id = String(p_event.get("session_id", String())).strip_edges();
	const bool authored = solers_transcript_is_human_message(p_event);
	const bool started = String(p_event.get("event_type", String())) == "turn_started";
	if (project_path.is_empty() || session_id.is_empty() || (!started && !authored)) {
		return;
	}
	if (session_index.valid && session_index.project_path != project_path) {
		session_index.valid = false;
		session_index.by_id.clear();
	}
	if (!session_index.valid) {
		return;
	}
	SolersSessionIndexEntry *entry = session_index.by_id.getptr(session_id);
	if (!entry) {
		SolersSessionIndexEntry created;
		created.info.session_id = session_id;
		created.info.title = "Current chat";
		session_index.by_id[session_id] = created;
		entry = session_index.by_id.getptr(session_id);
	}
	if (started || entry->info.wall == 0) {
		entry->info.wall = (int64_t)p_event.get("wall", entry->info.wall);
	}
	if (authored) {
		entry->has_user = true;
		if (!entry->has_title) {
			const String title = String(p_event.get("title", p_event.get("content", String()))).strip_edges().replace("\r", " ").replace("\n", " ");
			if (!title.is_empty()) {
				entry->info.title = title;
				entry->has_title = true;
			}
		}
	}
}

static Dictionary index_record(const Dictionary &p_event) {
	Dictionary record;
	record["project_path"] = p_event.get("project_path", String());
	record["session_id"] = p_event.get("session_id", String());
	record["event_type"] = p_event.get("event_type", String());
	record["wall"] = p_event.get("wall", 0);
	if (solers_transcript_is_human_message(p_event)) {
		record["role"] = "user";
		record["author"] = "human";
		record["title"] = String(p_event.get("content", String())).left(240);
	}
	return record;
}

void solers_trace_write(const String &p_where, const String &p_msg) {
	const String line = vformat("[SOLERS] %s | %s", p_where, p_msg);
	print_line(line);
	MutexLock lock(transcript_mutex);
	if (trace_file.is_null()) {
		trace_file = append_file(solers_session_dir().path_join("trace.log"));
	}
	if (trace_file.is_valid()) {
		trace_file->store_line(vformat("%d %s", (int64_t)OS::get_singleton()->get_ticks_msec(), line));
	}
}

void solers_transcript_write(const Dictionary &p_event) {
	if (transcript_io_active) {
		return;
	}
	transcript_io_active = true;
	Dictionary event = p_event.duplicate(true);
	event["schema"] = JOURNAL_SCHEMA;
	event["ticks_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	{
		MutexLock lock(transcript_mutex);
		const String id = event.get("session_id", String());
		if (!id.is_empty()) {
			if (journal_session_id != id) {
				journal_file = append_file(journal_path(id));
				journal_session_id = id;
			}
			if (journal_file.is_valid()) {
				journal_file->store_line(JSON::stringify(event, "", false, true));
			}
			if (solers_transcript_is_human_message(event) || String(event.get("event_type", String())) == "turn_started") {
				Ref<FileAccess> index = append_file(solers_session_dir().path_join("sessions.index.jsonl"));
				if (index.is_valid()) {
					index->store_line(JSON::stringify(index_record(event)));
				}
			}
		}
	}
	transcript_io_active = false;
	index_event(event);
}

void solers_transcript_flush(const String &p_session_id) {
	MutexLock lock(transcript_mutex);
	if (p_session_id.is_empty() || p_session_id == journal_session_id) {
		journal_file.unref();
		journal_session_id = String();
	}
	if (trace_file.is_valid()) {
		trace_file->flush();
	}
}

void solers_transcript_close() {
	MutexLock lock(transcript_mutex);
	journal_file.unref();
	journal_session_id = String();
	if (trace_file.is_valid()) {
		trace_file->flush();
		trace_file.unref();
	}
	session_index.by_id.clear();
	session_index.project_path.clear();
	session_index.valid = false;
}

void solers_transcript_foreach_session(const String &p_session_id, void *p_userdata, bool (*p_callback)(void *, const String &)) {
	ERR_FAIL_NULL(p_callback);
	if (transcript_io_active || p_session_id.is_empty()) {
		return;
	}
	transcript_io_active = true;
	{
		MutexLock lock(transcript_mutex);
		if (journal_session_id == p_session_id) {
			journal_file.unref();
			journal_session_id = String();
		}
		Ref<FileAccess> file = FileAccess::open(journal_path(p_session_id), FileAccess::READ);
		while (file.is_valid() && !file->eof_reached() && p_callback(p_userdata, file->get_line())) {
		}
	}
	transcript_io_active = false;
}

static void migrate_session_index() {
	const String index_path = solers_session_dir().path_join("sessions.index.jsonl");
	Ref<FileAccess> index = FileAccess::open(index_path, FileAccess::READ);
	while (index.is_valid() && !index->eof_reached()) {
		Dictionary record;
		if (solers_transcript_parse_record(index->get_line(), record) && (int)record.get("schema", 0) >= JOURNAL_SCHEMA) {
			return;
		}
	}
	if (journal_file.is_valid()) {
		journal_file->flush();
	}
	index = FileAccess::open(index_path, FileAccess::WRITE);
	Ref<DirAccess> sessions = DirAccess::open(solers_session_dir().path_join("sessions"));
	if (index.is_valid() && sessions.is_valid()) {
		sessions->list_dir_begin();
		for (String name = sessions->get_next(); !name.is_empty(); name = sessions->get_next()) {
			if (sessions->current_is_dir() || name.get_extension() != "jsonl") {
				continue;
			}
			Ref<FileAccess> journal = FileAccess::open(sessions->get_current_dir().path_join(name), FileAccess::READ);
			while (journal.is_valid() && !journal->eof_reached()) {
				Dictionary event;
				solers_transcript_parse_record(journal->get_line(), event);
				if (solers_transcript_is_human_message(event) || String(event.get("event_type", String())) == "turn_started") {
					index->store_line(JSON::stringify(index_record(event)));
				}
			}
		}
		Dictionary marker;
		marker["schema"] = JOURNAL_SCHEMA;
		index->store_line(JSON::stringify(marker));
	}
}

static void rebuild_index(const String &p_project_path) {
	session_index.by_id.clear();
	session_index.project_path = p_project_path;
	session_index.valid = true;
	migrate_session_index();
	Ref<FileAccess> file = FileAccess::open(solers_session_dir().path_join("sessions.index.jsonl"), FileAccess::READ);
	while (file.is_valid() && !file->eof_reached()) {
		Dictionary event;
		if (solers_transcript_parse_record(file->get_line(), event) && String(event.get("project_path", String())) == p_project_path) {
			index_event(event);
		}
	}
}

Vector<SolersSessionInfo> solers_list_sessions(const String &p_project_path) {
	Vector<SolersSessionInfo> sessions;
	const String project_path = p_project_path.strip_edges();
	if (project_path.is_empty()) {
		return sessions;
	}
	{
		MutexLock lock(transcript_mutex);
		if (!session_index.valid || session_index.project_path != project_path) {
			rebuild_index(project_path);
		}
	}
	for (const KeyValue<String, SolersSessionIndexEntry> &entry : session_index.by_id) {
		if (entry.value.has_user) {
			sessions.push_back(entry.value.info);
		}
	}
	return sessions;
}
