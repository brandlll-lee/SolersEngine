/**************************************************************************/
/*  solers_trace.h                                                        */
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

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

String solers_session_dir();
void solers_trace_write(const String &p_where, const String &p_msg);
Error solers_transcript_write(const Dictionary &p_event);
Error solers_transcript_flush(const String &p_session_id = String());
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
