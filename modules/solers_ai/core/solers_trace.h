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
#include "core/variant/dictionary.h"

void solers_trace_write(const String &p_where, const String &p_msg);
void solers_transcript_write(const Dictionary &p_event);

#define SOLERS_TRACE(m_where, m_msg) solers_trace_write(String(m_where), String(m_msg))
