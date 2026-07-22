/**************************************************************************/
/*  solers_mention.h                                                      */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class SolersObservationService;

// Context mentions pinned via @ in the chat composer. Membership comes from
// SolersPluginRegistry capability flags and SolersObservationService facts —
// never from name whitelists in the dock.
namespace SolersMention {

static constexpr int COLLECT_LIMIT = 32;

String query_at(const String &p_text, int p_caret, int &r_mention_start);
Array parse(const String &p_text);
// Per-line completed mention spans for TextEdit inline objects:
// { column, length, mention }. Does not dedupe — every occurrence is drawn.
Array scan_line_spans(const String &p_line);
String format_token(const Dictionary &p_mention);
String prompt_block(const Array &p_mentions);
String dedupe_key(const Dictionary &p_mention);

// Non-empty root sections: { id, label, count }.
Array collect_root_sections(SolersObservationService *p_observation, const String &p_query = String());
// Items for one section (or all sources when p_section_id is empty): mention dicts.
Array collect_section_items(const String &p_section_id, SolersObservationService *p_observation, const String &p_query = String());

} // namespace SolersMention
