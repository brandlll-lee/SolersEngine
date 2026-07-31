/**************************************************************************/
/*  solers_trace.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

String solers_session_dir();
void solers_trace_write(const String &p_where, const String &p_msg);
void solers_transcript_write(const Dictionary &p_event);
Vector<String> solers_transcript_read_snapshot();
bool solers_transcript_parse_record(const String &p_line, Dictionary &r_event);
// Stream transcript lines without retaining the whole file in a Vector. The
// callback returns false to stop early. Used by session restore so unrelated
// sessions' huge tool rows are never kept alive.
void solers_transcript_foreach_line(void *p_userdata, bool (*p_callback)(void *p_userdata, const String &p_line));
// Drop a top-level JSON string field (e.g. result_replay) before parse so a
// 200KB encrypted blob never becomes a Variant.
String solers_transcript_strip_json_string_field(const String &p_line, const String &p_key);
bool solers_transcript_line_has_session(const String &p_line, const String &p_session_id);

// Aggregated chat sessions from transcript.jsonl (authority for Session UI).
struct SolersSessionInfo {
	String session_id;
	String title;
	int64_t wall = 0;
	int message_count = 0;
};

Vector<SolersSessionInfo> solers_list_sessions(const String &p_project_path);

#define SOLERS_TRACE(m_where, m_msg) solers_trace_write(String(m_where), String(m_msg))
