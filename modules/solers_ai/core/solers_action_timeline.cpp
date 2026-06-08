/**************************************************************************/
/*  solers_action_timeline.cpp                                            */
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

#include "solers_action_timeline.h"

#include "core/object/class_db.h"
#include "core/os/time.h"

void SolersActionTimeline::_bind_methods() {
	ClassDB::bind_method(D_METHOD("record_event", "type", "payload"), &SolersActionTimeline::record_event);
	ClassDB::bind_method(D_METHOD("list_actions", "limit"), &SolersActionTimeline::list_actions, DEFVAL(100));
	ClassDB::bind_method(D_METHOD("clear"), &SolersActionTimeline::clear);
	ClassDB::bind_method(D_METHOD("get_action_count"), &SolersActionTimeline::get_action_count);
}

int64_t SolersActionTimeline::record_event(const String &p_type, const Dictionary &p_payload) {
	const int64_t event_id = next_event_id++;
	Dictionary event;
	event["id"] = event_id;
	event["type"] = p_type;
	event["timestamp_unix"] = Time::get_singleton()->get_unix_time_from_system();
	event["timestamp_msec"] = Time::get_singleton()->get_ticks_msec();
	event["payload"] = p_payload;
	events.push_back(event);
	return event_id;
}

Array SolersActionTimeline::list_actions(int p_limit) const {
	Array result;
	const int limit = CLAMP(p_limit, 0, events.size());
	const int start = MAX(0, events.size() - limit);
	for (int i = start; i < events.size(); i++) {
		result.push_back(events[i]);
	}
	return result;
}

void SolersActionTimeline::clear() {
	events.clear();
	next_event_id = 1;
}

int SolersActionTimeline::get_action_count() const {
	return events.size();
}
