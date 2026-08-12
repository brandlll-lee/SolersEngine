/**************************************************************************/
/*  solers_protocol_anthropic_messages.cpp                                */
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

#include "solers_protocol_anthropic_messages.h"

#include "core/io/json.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

static Dictionary _anthropic_image_block(const Dictionary &p_attachment) {
	const Dictionary encoded = SolersLLMMessage::encode_image_attachment(p_attachment);
	if (encoded.is_empty()) {
		return Dictionary();
	}
	Dictionary source;
	source["type"] = "base64";
	source["media_type"] = encoded["mime_type"];
	source["data"] = encoded["base64"];
	Dictionary image_block;
	image_block["type"] = "image";
	image_block["source"] = source;
	return image_block;
}

// The model must know when referenced evidence could not be loaded, instead of
// silently believing it has seen an image that was never sent.
static Dictionary _anthropic_missing_image_block(const Dictionary &p_attachment) {
	Dictionary text_block;
	text_block["type"] = "text";
	text_block["text"] = vformat("[Solers: image attachment '%s' could not be loaded from disk and was omitted. Capture fresh evidence before relying on it.]", String(p_attachment.get("id", String())));
	return text_block;
}

String SolersAnthropicMessagesProtocol::_map_stop_reason(const String &p_native) {
	if (p_native == "tool_use") {
		return SolersLLMStopReason::TOOL_USE;
	}
	if (p_native == "end_turn") {
		return SolersLLMStopReason::END_TURN;
	}
	if (p_native == "max_tokens") {
		return SolersLLMStopReason::MAX_TOKENS;
	}
	return SolersLLMStopReason::STOP;
}

Array SolersAnthropicMessagesProtocol::_lower_messages(const Dictionary &p_request) const {
	Array out;
	const Array messages = p_request.get("messages", Array());
	auto append_attachment_blocks = [&](Array &r_content, const Dictionary &p_attachment) {
		const Dictionary image_block = _anthropic_image_block(p_attachment);
		r_content.push_back(image_block.is_empty() ? _anthropic_missing_image_block(p_attachment) : image_block);
	};

	// Tool results must be collapsed into a single user message of tool_result
	// blocks. Buffer consecutive tool messages and flush them as one user turn.
	Array pending_tool_results;
	auto flush_tool_results = [&]() {
		if (pending_tool_results.is_empty()) {
			return;
		}
		Dictionary user_msg;
		user_msg["role"] = "user";
		user_msg["content"] = pending_tool_results.duplicate();
		out.push_back(user_msg);
		pending_tool_results.clear();
	};

	for (int i = 0; i < messages.size(); i++) {
		const Dictionary m = messages[i];
		const String role = m.get("role", "user");

		if (role == SolersLLMRole::TOOL) {
			Dictionary block;
			block["type"] = "tool_result";
			block["tool_use_id"] = m.get("tool_call_id", String());
			const Array attachments = m.get("attachments", Array());
			if (attachments.is_empty()) {
				block["content"] = m.get("content", String());
			} else {
				Array content;
				Dictionary text_block;
				text_block["type"] = "text";
				text_block["text"] = m.get("content", String());
				content.push_back(text_block);
				for (int a = 0; a < attachments.size(); a++) {
					append_attachment_blocks(content, attachments[a]);
				}
				block["content"] = content;
			}
			pending_tool_results.push_back(block);
			continue;
		}

		flush_tool_results();

		if (role == SolersLLMRole::ASSISTANT && m.has("tool_calls")) {
			Array content;
			const String text = m.get("content", String());
			if (!text.is_empty()) {
				Dictionary text_block;
				text_block["type"] = "text";
				text_block["text"] = text;
				content.push_back(text_block);
			}
			const Array calls = m["tool_calls"];
			for (int c = 0; c < calls.size(); c++) {
				const Dictionary call = calls[c];
				const Variant parsed_input = JSON::parse_string(call.get("arguments", "{}"));
				Dictionary block;
				block["type"] = "tool_use";
				block["id"] = call.get("id", String());
				block["name"] = call.get("name", String());
				block["input"] = parsed_input.get_type() == Variant::DICTIONARY ? parsed_input : Variant(Dictionary());
				content.push_back(block);
			}
			Dictionary assistant_msg;
			assistant_msg["role"] = "assistant";
			assistant_msg["content"] = content;
			out.push_back(assistant_msg);
		} else {
			Dictionary x;
			x["role"] = role;
			const Array attachments = m.get("attachments", Array());
			if (role == SolersLLMRole::USER && !attachments.is_empty()) {
				Array content;
				Dictionary text_block;
				text_block["type"] = "text";
				text_block["text"] = m.get("content", String());
				content.push_back(text_block);
				for (int a = 0; a < attachments.size(); a++) {
					append_attachment_blocks(content, attachments[a]);
				}
				x["content"] = content;
			} else {
				x["content"] = m.get("content", String());
			}
			out.push_back(x);
		}
	}
	flush_tool_results();
	return out;
}

Array SolersAnthropicMessagesProtocol::_lower_tools(const Array &p_tools) const {
	Array out;
	for (int i = 0; i < p_tools.size(); i++) {
		const Dictionary tool = p_tools[i];
		Dictionary entry;
		entry["name"] = tool.get("name", String());
		entry["description"] = tool.get("description", String());
		entry["input_schema"] = tool.get("parameters", Dictionary());
		out.push_back(entry);
	}
	return out;
}

Dictionary SolersAnthropicMessagesProtocol::build_request_body(const Dictionary &p_request) const {
	Dictionary body;
	body["model"] = p_request.get("model", String());
	body["max_tokens"] = p_request.has("max_tokens") ? p_request["max_tokens"] : Variant(4096);
	body["stream"] = true;

	const String system = p_request.get("system", String());
	if (!system.is_empty()) {
		// Cache breakpoint after the byte-stable request prefix (tools +
		// system prompt), so repeated session requests hit the prompt cache.
		Dictionary cache_control;
		cache_control["type"] = "ephemeral";
		Dictionary system_block;
		system_block["type"] = "text";
		system_block["text"] = system;
		system_block["cache_control"] = cache_control;
		Array system_blocks;
		system_blocks.push_back(system_block);
		body["system"] = system_blocks;
	}
	Array lowered_messages = _lower_messages(p_request);
	// Second breakpoint on the newest message: the next request extends this
	// exact history, so everything up to here is a cache hit.
	if (!lowered_messages.is_empty()) {
		Dictionary last = lowered_messages[lowered_messages.size() - 1];
		const Variant content = last.get("content", Variant());
		Array blocks;
		if (content.get_type() == Variant::STRING && !String(content).is_empty()) {
			Dictionary text_block;
			text_block["type"] = "text";
			text_block["text"] = content;
			blocks.push_back(text_block);
		} else if (content.get_type() == Variant::ARRAY) {
			blocks = content;
		}
		if (!blocks.is_empty()) {
			Dictionary cache_control;
			cache_control["type"] = "ephemeral";
			Dictionary last_block = Dictionary(blocks[blocks.size() - 1]).duplicate();
			last_block["cache_control"] = cache_control;
			blocks[blocks.size() - 1] = last_block;
			last["content"] = blocks;
			lowered_messages[lowered_messages.size() - 1] = last;
		}
	}
	body["messages"] = lowered_messages;

	const Array tools = p_request.get("tools", Array());
	if (!tools.is_empty()) {
		body["tools"] = _lower_tools(tools);
	}
	if (p_request.has("temperature")) {
		body["temperature"] = p_request["temperature"];
	}
	if (p_request.has("reasoning_effort")) {
		Dictionary output_config;
		output_config["effort"] = p_request["reasoning_effort"];
		body["output_config"] = output_config;
	}
	return body;
}

void SolersAnthropicMessagesProtocol::augment_protocol_headers(Dictionary &r_headers, const Dictionary &p_request) const {
	r_headers["anthropic-version"] = "2023-06-01";
}

Array SolersAnthropicMessagesProtocol::parse_event(Dictionary &r_state, const String &p_event_name, const String &p_data) const {
	Array events;
	const String data = p_data.strip_edges();
	if (data.is_empty()) {
		return events;
	}
	const Variant parsed = JSON::parse_string(data);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return events;
	}
	const Dictionary obj = parsed;
	const String type = p_event_name.is_empty() ? String(obj.get("type", String())) : p_event_name;

	if (!r_state.has("blocks")) {
		r_state["blocks"] = Dictionary();
	}

	if (type == "content_block_start") {
		const int index = obj.get("index", 0);
		const Dictionary block = obj.get("content_block", Dictionary());
		Dictionary blocks = r_state["blocks"];
		Dictionary tracked;
		tracked["type"] = block.get("type", String());
		tracked["id"] = block.get("id", String());
		tracked["name"] = block.get("name", String());
		tracked["json"] = String();
		blocks[itos(index)] = tracked;
		r_state["blocks"] = blocks;
		if (String(tracked.get("type", String())) == "tool_use") {
			events.push_back(SolersLLMEvent::tool_input_start(tracked.get("id", String()), tracked.get("name", String()), String()));
		}
	} else if (type == "content_block_delta") {
		const int index = obj.get("index", 0);
		const Dictionary delta = obj.get("delta", Dictionary());
		const String delta_type = delta.get("type", String());
		if (delta_type == "text_delta") {
			events.push_back(SolersLLMEvent::text_delta(delta.get("text", String())));
		} else if (delta_type == "thinking_delta") {
			events.push_back(SolersLLMEvent::reasoning_delta(delta.get("thinking", String())));
		} else if (delta_type == "input_json_delta") {
			Dictionary blocks = r_state["blocks"];
			Dictionary tracked = blocks.get(itos(index), Dictionary());
			const String partial = delta.get("partial_json", String());
			tracked["json"] = String(tracked.get("json", String())) + partial;
			blocks[itos(index)] = tracked;
			r_state["blocks"] = blocks;
			events.push_back(SolersLLMEvent::tool_input_delta(tracked.get("id", String()), tracked.get("name", String()), partial, tracked.get("json", String())));
		}
	} else if (type == "content_block_stop") {
		const int index = obj.get("index", 0);
		const Dictionary blocks = r_state["blocks"];
		const Dictionary tracked = blocks.get(itos(index), Dictionary());
		if (String(tracked.get("type", String())) == "tool_use") {
			String args = tracked.get("json", String());
			if (args.strip_edges().is_empty()) {
				args = "{}";
			}
			events.push_back(SolersLLMEvent::tool_call(tracked.get("id", String()), tracked.get("name", String()), args));
		}
	} else if (type == "message_start") {
		// Prompt/cache accounting arrives once here; message_delta only
		// refreshes output tokens, so stash these for the final usage event.
		const Dictionary usage = Dictionary(obj.get("message", Dictionary())).get("usage", Dictionary());
		if (!usage.is_empty()) {
			r_state["input_tokens"] = usage.get("input_tokens", 0);
			r_state["cache_read_tokens"] = usage.get("cache_read_input_tokens", -1);
			r_state["cache_write_tokens"] = usage.get("cache_creation_input_tokens", -1);
		}
	} else if (type == "message_delta") {
		const Dictionary delta = obj.get("delta", Dictionary());
		if (obj.has("usage")) {
			const Dictionary usage = obj["usage"];
			events.push_back(SolersLLMEvent::usage(
					usage.get("input_tokens", r_state.get("input_tokens", 0)),
					usage.get("output_tokens", 0),
					usage.get("cache_read_input_tokens", r_state.get("cache_read_tokens", -1)),
					usage.get("cache_creation_input_tokens", r_state.get("cache_write_tokens", -1))));
		}
		const Variant stop_v = delta.get("stop_reason", Variant());
		if (stop_v.get_type() == Variant::STRING && !String(stop_v).is_empty()) {
			events.push_back(SolersLLMEvent::finish(_map_stop_reason(stop_v)));
		}
	} else if (type == "error") {
		const Dictionary err = obj.get("error", Dictionary());
		events.push_back(SolersLLMEvent::error(err.get("type", "provider_error"), err.get("message", "Anthropic stream error")));
	}

	return events;
}
