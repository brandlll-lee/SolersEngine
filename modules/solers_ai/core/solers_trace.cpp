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

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

static Mutex solers_transcript_mutex;
static thread_local bool solers_transcript_io_active = false;

void solers_trace_write(const String &p_where, const String &p_msg) {
	const String line = vformat("[SOLERS] %s | %s", p_where, p_msg);
	print_line(line);

	Ref<FileAccess> f = FileAccess::open("user://solers_ai_trace.log", FileAccess::READ_WRITE);
	if (f.is_null()) {
		f = FileAccess::open("user://solers_ai_trace.log", FileAccess::WRITE);
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
		Ref<FileAccess> file = FileAccess::open("user://solers_ai_transcript.jsonl", FileAccess::READ_WRITE);
		if (file.is_null()) {
			file = FileAccess::open("user://solers_ai_transcript.jsonl", FileAccess::WRITE);
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
		Ref<FileAccess> file = FileAccess::open("user://solers_ai_transcript.jsonl", FileAccess::READ);
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
