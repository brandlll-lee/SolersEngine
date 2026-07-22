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

// Aggregated chat sessions from transcript.jsonl (authority for Session UI).
struct SolersSessionInfo {
	String session_id;
	String title;
	int64_t wall = 0;
	int message_count = 0;
};

Vector<SolersSessionInfo> solers_list_sessions(const String &p_project_path);

#define SOLERS_TRACE(m_where, m_msg) solers_trace_write(String(m_where), String(m_msg))
