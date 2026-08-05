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
void solers_transcript_flush(const String &p_session_id = String());
void solers_transcript_close();
bool solers_transcript_parse_record(const String &p_line, Dictionary &r_event);
bool solers_transcript_is_human_message(const Dictionary &p_event);
void solers_transcript_foreach_session(const String &p_session_id, void *p_userdata, bool (*p_callback)(void *p_userdata, const String &p_line));

// Aggregated chat sessions from the compact session index.
struct SolersSessionInfo {
	String session_id;
	String title;
	int64_t wall = 0;
};

Vector<SolersSessionInfo> solers_list_sessions(const String &p_project_path);

#define SOLERS_TRACE(m_where, m_msg) solers_trace_write(String(m_where), String(m_msg))
