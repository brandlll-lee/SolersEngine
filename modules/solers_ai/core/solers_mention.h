/**************************************************************************/
/*  solers_mention.h                                                      */
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
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersSceneObservation;

// Context mentions pinned via @ in the chat composer.
// Solers connectors: SolersPluginRegistry. Godot project addons: plugin.cfg scan
// (EditorPluginSettings::list_plugin_configs). Files/scenes/selection: observation.
// Never name whitelists in the dock.
namespace SolersMention {

static constexpr int COLLECT_LIMIT = 32; // Cross-source quick find.

String query_at(const String &p_text, int p_caret, int &r_mention_start);
Array parse(const String &p_text);
// Per-line completed mention spans for TextEdit inline objects:
// { column, length, mention }. Does not dedupe — every occurrence is drawn.
Array scan_line_spans(const String &p_line);
String format_token(const Dictionary &p_mention);
String prompt_block(const Array &p_mentions);
// Peel owned model-only appendix for UI/title. Marker through end of string.
String strip_prompt_block(const String &p_text);
String dedupe_key(const Dictionary &p_mention);
// Resolves an indexed res:// path beginning at p_offset and reports the exact
// span backed by EditorFileSystem. Unknown text is left unresolved.
Dictionary resolve_project_path_at(const String &p_text, int p_offset, int &r_length);

// Stable root sections: { id, label }.
Array collect_root_sections();
// Items for one section (or all sources when p_section_id is empty): mention dicts.
// p_max_items <= 0 keeps the complete indexed section; quick find uses COLLECT_LIMIT.
Array collect_section_items(const String &p_section_id, SolersSceneObservation *p_observation, const String &p_query = String(), int p_max_items = 0);

} // namespace SolersMention
