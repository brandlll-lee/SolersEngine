/**************************************************************************/
/*  solers_protocol_openai_chat.cpp                                       */
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

#include "solers_protocol_openai_chat.h"

#include "core/io/json.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

String SolersOpenAIChatProtocol::_map_finish_reason(const String &p_native) {
	if (p_native == "tool_calls") {
		return SolersLLMStopReason::TOOL_USE;
	}
	if (p_native == "stop") {
		return SolersLLMStopReason::END_TURN;
	}
	if (p_native == "length") {
		return SolersLLMStopReason::MAX_TOKENS;
	}
	return SolersLLMStopReason::STOP;
}

Array SolersOpenAIChatProtocol::_lower_messages(const Dictionary &p_request) const {
	Array out;

	const String system = p_request.get("system", String());
	if (!system.is_empty()) {
		Dictionary s;
		s["role"] = "system";
		s["content"] = system;
		out.push_back(s);
	}

	const Array messages = p_request.get("messages", Array());
	for (int i = 0; i < messages.size(); i++) {
		const Dictionary m = messages[i];
		const String role = m.get("role", "user");

		if (role == SolersLLMRole::ASSISTANT && m.has("tool_calls")) {
			Dictionary a;
			a["role"] = "assistant";
			const String content = m.get("content", String());
			// OpenAI accepts null content alongside tool_calls.
			a["content"] = content.is_empty() ? Variant() : Variant(content);
			Array native_calls;
			const Array calls = m["tool_calls"];
			for (int c = 0; c < calls.size(); c++) {
				const Dictionary call = calls[c];
				Dictionary fn;
				fn["name"] = call.get("name", String());
				fn["arguments"] = call.get("arguments", String());
				Dictionary tc;
				tc["id"] = call.get("id", String());
				tc["type"] = "function";
				tc["function"] = fn;
				native_calls.push_back(tc);
			}
			a["tool_calls"] = native_calls;
			out.push_back(a);
		} else if (role == SolersLLMRole::TOOL) {
			Dictionary t;
			t["role"] = "tool";
			t["tool_call_id"] = m.get("tool_call_id", String());
			t["content"] = m.get("content", String());
			out.push_back(t);
		} else {
			Dictionary x;
			x["role"] = role;
			x["content"] = m.get("content", String());
			out.push_back(x);
		}
	}
	return out;
}

Array SolersOpenAIChatProtocol::_lower_tools(const Array &p_tools) const {
	Array out;
	for (int i = 0; i < p_tools.size(); i++) {
		const Dictionary tool = p_tools[i];
		Dictionary fn;
		fn["name"] = tool.get("name", String());
		fn["description"] = tool.get("description", String());
		fn["parameters"] = tool.get("parameters", Dictionary());
		Dictionary entry;
		entry["type"] = "function";
		entry["function"] = fn;
		out.push_back(entry);
	}
	return out;
}

Dictionary SolersOpenAIChatProtocol::build_request_body(const Dictionary &p_request) const {
	Dictionary body;
	body["model"] = p_request.get("model", String());
	body["messages"] = _lower_messages(p_request);

	const Array tools = p_request.get("tools", Array());
	if (!tools.is_empty()) {
		body["tools"] = _lower_tools(tools);
		if (p_request.has("tool_choice")) {
			body["tool_choice"] = p_request["tool_choice"];
		}
	}
	if (p_request.has("temperature")) {
		body["temperature"] = p_request["temperature"];
	}
	if (p_request.has("max_tokens")) {
		body["max_tokens"] = p_request["max_tokens"];
	}

	body["stream"] = true;
	Dictionary stream_options;
	stream_options["include_usage"] = true;
	body["stream_options"] = stream_options;
	return body;
}

Array SolersOpenAIChatProtocol::parse_event(Dictionary &r_state, const String &p_event_name, const String &p_data) const {
	Array events;
	const String data = p_data.strip_edges();
	if (data.is_empty() || data == "[DONE]") {
		return events;
	}

	const Variant parsed = JSON::parse_string(data);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return events;
	}
	const Dictionary obj = parsed;

	// Usage frames arrive with an empty `choices` array when stream_options
	// requested it; surface them regardless of position.
	if (obj.has("usage") && Dictionary(obj["usage"]).size() > 0) {
		const Dictionary u = obj["usage"];
		events.push_back(SolersLLMEvent::usage(u.get("prompt_tokens", 0), u.get("completion_tokens", 0)));
	}

	const Array choices = obj.get("choices", Array());
	if (choices.is_empty()) {
		return events;
	}
	const Dictionary choice = choices[0];
	const Dictionary delta = choice.get("delta", Dictionary());

	const Variant content_v = delta.get("content", Variant());
	if (content_v.get_type() == Variant::STRING) {
		const String content = content_v;
		if (!content.is_empty()) {
			events.push_back(SolersLLMEvent::text_delta(content));
		}
	}
	const Variant reasoning_v = delta.get("reasoning_content", Variant());
	if (reasoning_v.get_type() == Variant::STRING) {
		const String reasoning = reasoning_v;
		if (!reasoning.is_empty()) {
			events.push_back(SolersLLMEvent::reasoning_delta(reasoning));
		}
	}

	// Tool calls stream incrementally, keyed by `index`; accumulate id, name
	// and the argument JSON string across frames into the parser state.
	if (delta.has("tool_calls")) {
		Array calls = r_state.get("calls", Array());
		const Array deltas = delta["tool_calls"];
		for (int i = 0; i < deltas.size(); i++) {
			const Dictionary tc = deltas[i];
			const int idx = tc.get("index", 0);
			while (calls.size() <= idx) {
				Dictionary blank;
				blank["id"] = String();
				blank["name"] = String();
				blank["arguments"] = String();
				calls.push_back(blank);
			}
			Dictionary cur = calls[idx];
			if (tc.has("id")) {
				cur["id"] = tc["id"];
			}
			const Dictionary fn = tc.get("function", Dictionary());
			if (fn.has("name")) {
				cur["name"] = String(cur["name"]) + String(fn["name"]);
			}
			if (fn.has("arguments")) {
				cur["arguments"] = String(cur["arguments"]) + String(fn["arguments"]);
			}
			calls[idx] = cur;
		}
		r_state["calls"] = calls;
	}

	const Variant finish_v = choice.get("finish_reason", Variant());
	if (finish_v.get_type() == Variant::STRING && !String(finish_v).is_empty()) {
		const Array calls = r_state.get("calls", Array());
		for (int i = 0; i < calls.size(); i++) {
			const Dictionary c = calls[i];
			events.push_back(SolersLLMEvent::tool_call(c.get("id", String()), c.get("name", String()), c.get("arguments", String())));
		}
		events.push_back(SolersLLMEvent::finish(_map_finish_reason(finish_v)));
	}

	return events;
}
