/**************************************************************************/
/*  solers_protocol_openai_responses.cpp                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_protocol_openai_responses.h"

#include "core/io/json.h"
#include "modules/solers_ai/llm/solers_llm_message.h"

static Dictionary _responses_image_part(const Dictionary &p_attachment) {
	const Dictionary encoded = SolersLLMMessage::encode_image_attachment(p_attachment);
	if (encoded.is_empty()) {
		Dictionary missing;
		missing["type"] = "input_text";
		missing["text"] = vformat("[Solers: image attachment '%s' could not be loaded and was omitted.]", String(p_attachment.get("id", String())));
		return missing;
	}
	Dictionary image;
	image["type"] = "input_image";
	image["image_url"] = encoded["data_uri"];
	return image;
}

static Array _responses_content(const String &p_text, const Array &p_attachments, const String &p_text_type) {
	Array content;
	if (!p_text.is_empty() || p_attachments.is_empty()) {
		Dictionary text;
		text["type"] = p_text_type;
		text["text"] = p_text;
		content.push_back(text);
	}
	for (const Variant &attachment : p_attachments) {
		content.push_back(_responses_image_part(attachment));
	}
	return content;
}

Array SolersOpenAIResponsesProtocol::_lower_input(const Dictionary &p_request) const {
	Array input;
	const Array messages = p_request.get("messages", Array());
	for (const Variant &message_value : messages) {
		const Dictionary message = message_value;
		const String role = message.get("role", SolersLLMRole::USER);
		const String text = message.get("content", String());
		const Array attachments = message.get("attachments", Array());

		if (role == SolersLLMRole::USER) {
			Dictionary item;
			item["role"] = "user";
			item["content"] = _responses_content(text, attachments, "input_text");
			input.push_back(item);
			continue;
		}
		if (role == SolersLLMRole::SYSTEM) {
			Dictionary item;
			item["role"] = "system";
			item["content"] = text;
			input.push_back(item);
			continue;
		}
		if (role == SolersLLMRole::ASSISTANT) {
			const Dictionary provider_metadata = message.get("provider_metadata", Dictionary());
			const Dictionary openai = provider_metadata.get("openai_responses", Dictionary());
			const Array reasoning_items = openai.get("reasoning_items", Array());
			for (const Variant &reasoning_value : reasoning_items) {
				Dictionary reasoning = Dictionary(reasoning_value).duplicate(true);
				if (String(reasoning.get("encrypted_content", String())).is_empty()) {
					continue;
				}
				reasoning["type"] = "reasoning";
				if (!reasoning.has("summary")) {
					reasoning["summary"] = Array();
				}
				input.push_back(reasoning);
			}
			if (!text.is_empty()) {
				Dictionary item;
				item["role"] = "assistant";
				item["content"] = _responses_content(text, Array(), "output_text");
				input.push_back(item);
			}
			const Array calls = message.get("tool_calls", Array());
			for (const Variant &call_value : calls) {
				const Dictionary call = call_value;
				Dictionary native_call;
				native_call["type"] = "function_call";
				native_call["call_id"] = call.get("id", String());
				native_call["name"] = call.get("name", String());
				native_call["arguments"] = call.get("arguments", "{}");
				input.push_back(native_call);
			}
			continue;
		}
		if (role == SolersLLMRole::TOOL) {
			Dictionary output;
			output["type"] = "function_call_output";
			output["call_id"] = message.get("tool_call_id", String());
			output["output"] = attachments.is_empty() ? Variant(text) : Variant(_responses_content(text, attachments, "input_text"));
			input.push_back(output);
		}
	}
	return input;
}

Array SolersOpenAIResponsesProtocol::_lower_tools(const Array &p_tools) const {
	Array out;
	for (const Variant &tool_value : p_tools) {
		const Dictionary tool = tool_value;
		Dictionary native_tool;
		native_tool["type"] = "function";
		native_tool["name"] = tool.get("name", String());
		native_tool["description"] = tool.get("description", String());
		native_tool["parameters"] = tool.get("parameters", Dictionary());
		native_tool["strict"] = false;
		out.push_back(native_tool);
	}
	return out;
}

Dictionary SolersOpenAIResponsesProtocol::build_request_body(const Dictionary &p_request) const {
	Dictionary body;
	body["model"] = p_request.get("model", String());
	body["input"] = _lower_input(p_request);
	const int max_tokens = p_request.get("max_tokens", 0);
	if (max_tokens > 0) {
		body["max_output_tokens"] = max_tokens;
	}
	const String system = p_request.get("system", String());
	if (!system.is_empty()) {
		body["instructions"] = system;
	}
	const Array tools = p_request.get("tools", Array());
	if (!tools.is_empty()) {
		body["tools"] = _lower_tools(tools);
		body["tool_choice"] = p_request.get("tool_choice", "auto");
	}
	Dictionary reasoning;
	if (p_request.has("reasoning_effort")) {
		reasoning["effort"] = p_request["reasoning_effort"];
	}
	reasoning["summary"] = "auto";
	body["reasoning"] = reasoning;
	Array include;
	include.push_back("reasoning.encrypted_content");
	body["include"] = include;
	body["store"] = false;
	body["stream"] = true;
	return body;
}

void SolersOpenAIResponsesProtocol::augment_headers(Dictionary &r_headers, const Dictionary &p_request) const {
	const String session_id = p_request.get("session_id", String());
	if (!session_id.is_empty()) {
		r_headers["session-id"] = session_id;
	}
}

Array SolersOpenAIResponsesProtocol::parse_event(Dictionary &r_state, const String &p_event_name, const String &p_data) const {
	Array events;
	const Variant parsed = JSON::parse_string(p_data.strip_edges());
	if (parsed.get_type() != Variant::DICTIONARY) {
		return events;
	}
	const Dictionary obj = parsed;
	const String type = obj.get("type", p_event_name);

	if (type == "response.output_text.delta") {
		const String delta = obj.get("delta", String());
		if (!delta.is_empty()) {
			events.push_back(SolersLLMEvent::text_delta(delta));
		}
		return events;
	}
	if (type == "response.reasoning_text.delta" || type == "response.reasoning_summary.delta" || type == "response.reasoning_summary_text.delta") {
		const String delta = obj.get("delta", String());
		if (!delta.is_empty()) {
			events.push_back(SolersLLMEvent::reasoning_delta(delta));
		}
		return events;
	}

	Dictionary calls = r_state.get("calls", Dictionary());
	if (type == "response.output_item.added") {
		const Dictionary item = obj.get("item", Dictionary());
		if (String(item.get("type", String())) == "function_call") {
			const String item_id = item.get("id", String());
			if (!item_id.is_empty()) {
				Dictionary call;
				call["id"] = item.get("call_id", item_id);
				call["name"] = item.get("name", String());
				call["arguments"] = item.get("arguments", String());
				call["started"] = true;
				call["emitted"] = false;
				calls[item_id] = call;
				r_state["calls"] = calls;
				events.push_back(SolersLLMEvent::tool_input_start(call.get("id", String()), call.get("name", String()), call.get("arguments", String())));
			}
		}
		return events;
	}
	if (type == "response.function_call_arguments.delta") {
		const String item_id = obj.get("item_id", String());
		const String delta = obj.get("delta", String());
		if (calls.has(item_id) && !delta.is_empty()) {
			Dictionary call = calls[item_id];
			call["arguments"] = String(call.get("arguments", String())) + delta;
			calls[item_id] = call;
			r_state["calls"] = calls;
			events.push_back(SolersLLMEvent::tool_input_delta(call.get("id", String()), call.get("name", String()), delta, call.get("arguments", String())));
		}
		return events;
	}
	if (type == "response.output_item.done") {
		const Dictionary item = obj.get("item", Dictionary());
		const String item_type = item.get("type", String());
		if (item_type == "reasoning") {
			const String item_id = item.get("id", String());
			if (!item_id.is_empty()) {
				Dictionary reasoning;
				reasoning["id"] = item_id;
				reasoning["summary"] = item.get("summary", Array());
				if (item.has("encrypted_content")) {
					reasoning["encrypted_content"] = item["encrypted_content"];
				}
				Array reasoning_items = r_state.get("reasoning_items", Array());
				reasoning_items.push_back(reasoning);
				r_state["reasoning_items"] = reasoning_items;
			}
			return events;
		}
		if (item_type == "function_call") {
			const String item_id = item.get("id", String());
			Dictionary call = calls.get(item_id, Dictionary());
			if (call.is_empty()) {
				call["id"] = item.get("call_id", item_id);
				call["name"] = item.get("name", String());
				call["started"] = false;
				call["emitted"] = false;
			}
			if (item.has("arguments")) {
				call["arguments"] = item["arguments"];
			}
			if (!(bool)call.get("started", false)) {
				events.push_back(SolersLLMEvent::tool_input_start(call.get("id", String()), call.get("name", String()), call.get("arguments", String())));
			}
			if (!(bool)call.get("emitted", false)) {
				Dictionary metadata;
				Dictionary openai;
				openai["item_id"] = item_id;
				metadata["openai_responses"] = openai;
				events.push_back(SolersLLMEvent::tool_call(call.get("id", String()), call.get("name", String()), call.get("arguments", "{}"), metadata));
				call["emitted"] = true;
				r_state["has_tool_call"] = true;
			}
			calls[item_id] = call;
			r_state["calls"] = calls;
		}
		return events;
	}

	if (type == "response.completed" || type == "response.incomplete") {
		if (r_state.get("finished", false)) {
			return events;
		}
		const Dictionary response = obj.get("response", Dictionary());
		const Dictionary usage = response.get("usage", Dictionary());
		if (!usage.is_empty()) {
			// Canonical usage separates cached from fresh input tokens.
			const int input_tokens = usage.get("input_tokens", 0);
			const int cached_tokens = Dictionary(usage.get("input_tokens_details", Dictionary())).get("cached_tokens", 0);
			events.push_back(SolersLLMEvent::usage(MAX(0, input_tokens - cached_tokens), usage.get("output_tokens", 0), cached_tokens > 0 ? cached_tokens : -1));
		}
		String stop_reason = r_state.get("has_tool_call", false) ? SolersLLMStopReason::TOOL_USE : SolersLLMStopReason::END_TURN;
		if (type == "response.incomplete") {
			const Dictionary details = response.get("incomplete_details", Dictionary());
			if (String(details.get("reason", String())) == "max_output_tokens") {
				stop_reason = SolersLLMStopReason::MAX_TOKENS;
			}
		}
		Dictionary metadata;
		Dictionary openai;
		openai["reasoning_items"] = r_state.get("reasoning_items", Array());
		openai["response_id"] = response.get("id", String());
		metadata["openai_responses"] = openai;
		events.push_back(SolersLLMEvent::finish(stop_reason, metadata));
		r_state["finished"] = true;
		return events;
	}

	if (type == "response.failed" || type == "error") {
		const Dictionary response = obj.get("response", Dictionary());
		const Dictionary nested = response.get("error", Dictionary());
		const String code = obj.get("code", nested.get("code", "OPENAI_RESPONSES_ERROR"));
		const String message = obj.get("message", nested.get("message", "OpenAI Responses reported an error."));
		events.push_back(SolersLLMEvent::error(code, message, code == "context_length_exceeded" ? "context_overflow" : String()));
	}
	return events;
}
