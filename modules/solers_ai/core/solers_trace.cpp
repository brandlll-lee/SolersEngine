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
#include "core/os/os.h"
#include "core/string/print_string.h"

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
	Dictionary event = p_event.duplicate(true);
	event["ticks_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	Ref<FileAccess> f = FileAccess::open("user://solers_ai_transcript.jsonl", FileAccess::READ_WRITE);
	if (f.is_null()) {
		f = FileAccess::open("user://solers_ai_transcript.jsonl", FileAccess::WRITE);
	}
	if (f.is_valid()) {
		f->seek_end();
		f->store_line(JSON::stringify(event, "", false, true));
		f->flush();
	}
}
