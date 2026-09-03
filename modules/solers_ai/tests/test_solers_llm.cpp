/**************************************************************************/
/*  test_solers_llm.cpp                                                   */
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

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/tcp_server.h"
#include "core/os/os.h"
#include "tests/test_macros.h"

#include "modules/solers_ai/llm/solers_llm_client.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/llm/solers_llm_protocol.h"
#include "modules/solers_ai/llm/solers_llm_retry.h"
#include "modules/solers_ai/llm/solers_protocol_anthropic_messages.h"
#include "modules/solers_ai/llm/solers_protocol_openai_chat.h"
#include "modules/solers_ai/llm/solers_protocol_openai_responses.h"
#include "modules/solers_ai/tests/support/solers_test_state.h"

TEST_FORCE_LINK(test_solers_llm)

namespace TestSolersLlm {

Dictionary find_event_kind(const Array &p_events, const String &p_kind) {
	for (int i = 0; i < p_events.size(); i++) {
		const Dictionary event = p_events[i];
		if (event.get("kind", String()) == p_kind) {
			return event;
		}
	}
	return Dictionary();
}

Dictionary run_openai_failure_response(const String &p_response, Array &r_events, const String &p_protocol = "openai-chat") {
	Ref<TCPServer> server;
	server.instantiate();
	if (server->listen(0, IPAddress("127.0.0.1")) != OK) {
		return Dictionary();
	}
	SolersLLMProtocolRegistry protocols;
	protocols.register_builtin_protocols();
	SolersLLMClient client;
	client.set_protocol_registry(&protocols);
	Dictionary request;
	request["model"] = "synthetic-model";
	Array messages;
	messages.push_back(SolersLLMMessage::user("test"));
	request["messages"] = messages;
	Dictionary profile;
	profile["protocol"] = p_protocol;
	profile["base_url"] = vformat("http://127.0.0.1:%d/v1", server->get_local_port());
	Dictionary auth;
	auth["type"] = "none";
	if (client.begin(request, profile, auth) != OK) {
		return Dictionary();
	}
	bool responded = false;
	Ref<StreamPeerTCP> connection;
	const uint64_t started = OS::get_singleton()->get_ticks_msec();
	while (!client.is_failed() && OS::get_singleton()->get_ticks_msec() - started < 3000) {
		if (!responded && server->is_connection_available()) {
			connection = server->take_connection();
			if (connection.is_valid()) {
				if (p_response.is_empty()) {
					connection->disconnect_from_host();
				} else {
					connection->poll();
					uint8_t discard[4096];
					int received = 0;
					connection->get_partial_data(discard, 4096, received);
					const CharString bytes = p_response.utf8();
					connection->put_data((const uint8_t *)bytes.get_data(), bytes.length());
				}
			}
			responded = true;
			server->stop();
		}
		r_events.append_array(client.poll());
		OS::get_singleton()->delay_usec(1000);
	}
	r_events.append_array(client.poll());
	return client.is_failed() ? client.get_error() : Dictionary();
}

TEST_CASE("[SolersLLMEvent] represents streaming reasoning as canonical events") {
	Dictionary event = SolersLLMEvent::reasoning_delta("Inspecting the scene tree.");

	CHECK(event.get("kind", String()) == String(SolersLLMEventKind::REASONING_DELTA));
	CHECK(event.get("text", String()) == "Inspecting the scene tree.");
}

TEST_CASE("[SolersLLMRetry] retry classification uses structured transport facts") {
	Dictionary explicit_retry;
	explicit_retry["retryable"] = true;
	explicit_retry["message"] = "ordinary failure";
	CHECK(SolersLLMRetry::is_retryable(explicit_retry));

	Dictionary explicit_terminal;
	explicit_terminal["retryable"] = false;
	explicit_terminal["http_status"] = 503;
	CHECK_FALSE(SolersLLMRetry::is_retryable(explicit_terminal));

	Dictionary rate_limited;
	rate_limited["http_status"] = 429;
	CHECK(SolersLLMRetry::is_retryable(rate_limited));

	Dictionary text_only;
	text_only["message"] = "rate limit";
	CHECK_FALSE(SolersLLMRetry::is_retryable(text_only));
}

TEST_CASE("[SolersLLMClient] transport failures use the failed state without a protocol error event") {
	Array events;
	const Dictionary error = run_openai_failure_response(String(), events);
	REQUIRE_FALSE(error.is_empty());
	CHECK((bool)error.get("retryable", false));
	CHECK(find_event_kind(events, SolersLLMEventKind::ERROR).is_empty());
}

TEST_CASE("[SolersLLMClient] an error after streamed content is an interrupted retryable response") {
	const String body = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":null}]}\n\n"
						"data: {\"error\":{\"message\":\"Upstream HTTP/2 stream failed\",\"type\":\"upstream_error\",\"code\":\"upstream_error\"}}\n\n";
	Array events;
	const Dictionary error = run_openai_failure_response(vformat("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", body.utf8().length(), body), events);
	REQUIRE_FALSE(error.is_empty());
	CHECK_FALSE(find_event_kind(events, SolersLLMEventKind::TEXT_DELTA).is_empty());
	CHECK(String(error.get("code", String())) == "upstream_error");
	CHECK((bool)error.get("retryable", false));
	CHECK(SolersLLMRetry::is_retryable(error));
}

TEST_CASE("[SolersLLMClient] empty HTTP 200 stream without finish is retryable") {
	Array events;
	const Dictionary error = run_openai_failure_response("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n0\r\n\r\n", events);
	REQUIRE_FALSE(error.is_empty());
	CHECK(String(error.get("code", String())) == "STREAM_ENDED_WITHOUT_FINISH");
	CHECK((bool)error.get("retryable", false));
	CHECK((int)error.get("response_bytes", -1) == 0);
	CHECK(SolersLLMRetry::is_retryable(error));
}

TEST_CASE("[SolersLLMClient] JSON error body on HTTP 200 is terminal") {
	const String json_body = "{\"error\":{\"message\":\"quota exhausted\"}}";
	Array events;
	const Dictionary error = run_openai_failure_response(vformat("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", json_body.utf8().length(), json_body), events);
	REQUIRE_FALSE(error.is_empty());
	CHECK(String(error.get("code", String())) == "PROVIDER_ERROR");
	CHECK(String(error.get("message", String())).contains("quota exhausted"));
	CHECK_FALSE((bool)error.get("retryable", true));
}

TEST_CASE("[SolersLLMClient] successful HTML response is a terminal protocol failure") {
	const String body = "<html>gateway page</html>";
	Array events;
	const Dictionary error = run_openai_failure_response(vformat("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", body.utf8().length(), body), events);
	CHECK(String(error.get("code", String())) == "UNEXPECTED_RESPONSE_MEDIA_TYPE");
	CHECK_FALSE(SolersLLMRetry::is_retryable(error));
}

TEST_CASE("[SolersLLMClient] decoded stream without a terminal event is retryable") {
	const String body = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":null}]}\n\n";
	Array events;
	const Dictionary error = run_openai_failure_response(vformat("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", body.utf8().length(), body), events);
	REQUIRE_FALSE(error.is_empty());
	CHECK(String(error.get("code", String())) == "STREAM_INTERRUPTED");
	CHECK(SolersLLMRetry::is_retryable(error));
}

TEST_CASE("[SolersOpenAIResponsesProtocol] classifies failed responses from protocol facts") {
	SolersOpenAIResponsesProtocol protocol;
	Dictionary state;
	Array events = protocol.parse_event(state, "response.failed", R"json({"type":"response.failed","response":{"error":{"code":"rate_limit_exceeded","type":"server_error","message":"Rate limited."}}})json");
	Dictionary error = find_event_kind(events, SolersLLMEventKind::ERROR);
	REQUIRE_FALSE(error.is_empty());
	CHECK((bool)error.get("retryable", false));
	CHECK(String(Dictionary(error.get("details", Dictionary())).get("error_type", String())) == "server_error");

	events = protocol.parse_event(state, "response.failed", R"json({"type":"response.failed","response":{"error":{"code":"context_length_exceeded","type":"invalid_request_error","message":"Too much context."}}})json");
	error = find_event_kind(events, SolersLLMEventKind::ERROR);
	REQUIRE_FALSE(error.is_empty());
	CHECK_FALSE((bool)error.get("retryable", true));
	CHECK(String(error.get("failure_kind", String())) == "context_overflow");

	events = protocol.parse_event(state, "response.failed", R"json({"type":"response.failed","response":{"error":{"code":"insufficient_quota","type":"insufficient_quota","message":"Quota exhausted."}}})json");
	error = find_event_kind(events, SolersLLMEventKind::ERROR);
	REQUIRE_FALSE(error.is_empty());
	CHECK_FALSE((bool)error.get("retryable", true));

	events = protocol.parse_event(state, "response.failed", R"json({"type":"response.failed","response":{"error":null}})json");
	error = find_event_kind(events, SolersLLMEventKind::ERROR);
	REQUIRE_FALSE(error.is_empty());
	CHECK((bool)error.get("retryable", false));
}

TEST_CASE("[SolersLLMClient] preserves retryable Responses failure details") {
	const String body = "event: response.failed\ndata: {\"type\":\"response.failed\",\"response\":{\"id\":\"resp_synthetic\",\"status\":\"failed\",\"error\":null}}\n\n";
	Array events;
	const Dictionary error = run_openai_failure_response(vformat("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", body.utf8().length(), body), events, "openai-responses");
	REQUIRE_FALSE(error.is_empty());
	CHECK((bool)error.get("retryable", false));
	const Dictionary details = error.get("details", Dictionary());
	CHECK(String(details.get("response_id", String())) == "resp_synthetic");
	CHECK(String(details.get("status", String())) == "failed");
}

TEST_CASE("[SolersOpenAIResponsesProtocol] lowers encrypted reasoning and tool continuation") {
	Array calls;
	Dictionary call;
	call["id"] = "call_weather";
	call["name"] = "get_weather";
	call["arguments"] = "{\"city\":\"Paris\"}";
	calls.push_back(call);

	Dictionary reasoning;
	reasoning["id"] = "rs_1";
	reasoning["summary"] = Array();
	reasoning["encrypted_content"] = "encrypted";
	Array reasoning_items;
	reasoning_items.push_back(reasoning);
	Dictionary openai;
	openai["reasoning_items"] = reasoning_items;
	Dictionary metadata;
	metadata["openai_responses"] = openai;

	Array messages;
	messages.push_back(SolersLLMMessage::user("Weather?"));
	messages.push_back(SolersLLMMessage::assistant(String(), calls, metadata));
	messages.push_back(SolersLLMMessage::tool_result("call_weather", "get_weather", "sunny"));
	Dictionary request;
	request["model"] = "gpt-5.5";
	request["system"] = "Use tools.";
	request["messages"] = messages;
	request["tools"] = Array();
	request["max_tokens"] = 4096;
	request["session_id"] = "synthetic-session";
	request["reasoning_effort"] = "high";

	SolersOpenAIResponsesProtocol protocol;
	Dictionary headers;
	protocol.augment_headers(headers, request);
	CHECK(headers.get("session-id", String()) == "synthetic-session");
	const Dictionary body = protocol.build_request_body(request);
	const Array input = body.get("input", Array());
	REQUIRE(input.size() == 4);
	CHECK(Dictionary(input[0]).get("role", String()) == "user");
	CHECK(Dictionary(input[1]).get("type", String()) == "reasoning");
	CHECK(Dictionary(input[2]).get("type", String()) == "function_call");
	CHECK(Dictionary(input[3]).get("type", String()) == "function_call_output");
	CHECK_FALSE(body.get("store", true));
	CHECK(body.get("stream", false));
	CHECK((int)body.get("max_output_tokens", 0) == 4096);
	CHECK(Dictionary(body.get("reasoning", Dictionary())).get("effort", String()) == "high");
}

TEST_CASE("[SolersOpenAIResponsesProtocol] lifts streamed tool calls usage and continuation metadata") {
	const Dictionary canonical = SolersLLMEvent::usage(300, 100, 25, 25, 50);
	CHECK((int)canonical.get("input_tokens", 0) == 300);
	CHECK((int)canonical.get("output_tokens", 0) == 100);
	CHECK((int)canonical.get("reasoning_tokens", 0) == 50);
	CHECK((int)canonical.get("cache_read_tokens", 0) == 25);
	CHECK((int)canonical.get("cache_write_tokens", 0) == 25);

	SolersOpenAIResponsesProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());
	protocol.parse_event(state, "response.output_item.done", R"json({"type":"response.output_item.done","item":{"id":"rs_1","type":"reasoning","summary":[],"encrypted_content":"cipher"}})json");

	Array events = protocol.parse_event(state, "response.output_item.added", R"json({"type":"response.output_item.added","item":{"id":"fc_1","type":"function_call","call_id":"call_1","name":"project_get_info","arguments":""}})json");
	CHECK(find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START).get("id", String()) == "call_1");
	events = protocol.parse_event(state, "response.function_call_arguments.delta", R"json({"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"{}"})json");
	CHECK(find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_DELTA).get("arguments", String()) == "{}");
	events = protocol.parse_event(state, "response.output_item.done", R"json({"type":"response.output_item.done","item":{"id":"fc_1","type":"function_call","call_id":"call_1","name":"project_get_info","arguments":"{}"}})json");
	CHECK(find_event_kind(events, SolersLLMEventKind::TOOL_CALL).get("id", String()) == "call_1");

	events = protocol.parse_event(state, "response.completed", R"json({"type":"response.completed","response":{"id":"resp_1","usage":{"input_tokens":325,"input_tokens_details":{"cached_tokens":25},"output_tokens":150,"output_tokens_details":{"reasoning_tokens":50}}}})json");
	const Dictionary usage = find_event_kind(events, SolersLLMEventKind::USAGE);
	CHECK((int)usage.get("input_tokens", 0) == 300);
	CHECK((int)usage.get("cache_read_tokens", 0) == 25);
	CHECK((int)usage.get("output_tokens", 0) == 100);
	CHECK((int)usage.get("reasoning_tokens", 0) == 50);
	const Dictionary finish = find_event_kind(events, SolersLLMEventKind::FINISH);
	CHECK(finish.get("stop_reason", String()) == SolersLLMStopReason::TOOL_USE);
	const Dictionary finish_metadata = finish.get("provider_metadata", Dictionary());
	const Dictionary finish_openai = finish_metadata.get("openai_responses", Dictionary());
	CHECK(Array(finish_openai.get("reasoning_items", Array())).size() == 1);
}

TEST_CASE("[SolersOpenAIChatProtocol] starts chat completions with store disabled") {
	Array messages;
	messages.push_back(SolersLLMMessage::user("What is in this project?"));

	Dictionary request;
	request["model"] = "gpt-5.5";
	request["messages"] = messages;
	request["reasoning_effort"] = "high";

	SolersOpenAIChatProtocol protocol;
	Dictionary body = protocol.build_request_body(request);
	Array lowered = body.get("messages", Array());

	REQUIRE(lowered.size() == 1);
	Dictionary user = lowered[0];

	CHECK_FALSE(body.get("store", true));
	CHECK(body.get("stream", false));
	Dictionary stream_options = body.get("stream_options", Dictionary());
	CHECK(stream_options.get("include_usage", false));
	CHECK(body.get("reasoning_effort", String()) == "high");
	CHECK(user.get("role", String()) == "user");

	Dictionary state = protocol.begin_stream(Dictionary());
	const Array events = protocol.parse_event(state, String(), R"json({"usage":{"prompt_tokens":325,"prompt_tokens_details":{"cached_tokens":25},"completion_tokens":150,"completion_tokens_details":{"reasoning_tokens":50}},"choices":[]})json");
	const Dictionary usage = find_event_kind(events, SolersLLMEventKind::USAGE);
	CHECK((int)usage.get("input_tokens", 0) == 300);
	CHECK((int)usage.get("cache_read_tokens", 0) == 25);
	CHECK((int)usage.get("output_tokens", 0) == 100);
	CHECK((int)usage.get("reasoning_tokens", 0) == 50);
}

TEST_CASE("[SolersAnthropicMessagesProtocol] lowers model effort into output config") {
	Dictionary request;
	request["model"] = "claude-sonnet-4.6";
	request["messages"] = Array();
	request["reasoning_effort"] = "high";

	SolersAnthropicMessagesProtocol protocol;
	const Dictionary body = protocol.build_request_body(request);
	CHECK(Dictionary(body.get("output_config", Dictionary())).get("effort", String()) == "high");
}

TEST_CASE("[SolersOpenAIChatProtocol] normalizes Responses item ids into stable Chat operation ids") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());
	const String normalized_id = "call_solers_" + String("fc_item_1").md5_text().substr(0, 24);

	Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"fc_item_1","type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == normalized_id);
	CHECK(input_start.get("name", String()) == "project_get_info");
	CHECK(input_start.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	bool saw_tool_call = false;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		if (event.get("kind", String()) == String(SolersLLMEventKind::TOOL_CALL)) {
			saw_tool_call = true;
			CHECK(event.get("id", String()) == normalized_id);
			CHECK(event.get("name", String()) == "project_get_info");
			CHECK(event.get("arguments", String()) == "{}");
		}
	}
	CHECK(saw_tool_call);
}

TEST_CASE("[SolersOpenAIChatProtocol] normalizes a Responses item id exposed as call_id") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());
	const String normalized_id = "call_solers_" + String("fc_item_1").md5_text().substr(0, 24);

	Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"fc_item_1","call_id":"fc_item_1","type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == normalized_id);
	CHECK(input_start.get("name", String()) == "project_get_info");
	CHECK(input_start.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	bool saw_tool_call = false;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		if (event.get("kind", String()) == String(SolersLLMEventKind::TOOL_CALL)) {
			saw_tool_call = true;
			CHECK(event.get("id", String()) == normalized_id);
			CHECK(event.get("name", String()) == "project_get_info");
			CHECK(event.get("arguments", String()) == "{}");
		}
	}
	CHECK(saw_tool_call);
}

TEST_CASE("[SolersOpenAIChatProtocol] rejects a tool call with no provider identity") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());

	protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	const Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	CHECK_FALSE(find_event_kind(events, SolersLLMEventKind::ERROR).is_empty());
	CHECK(find_event_kind(events, SolersLLMEventKind::TOOL_CALL).is_empty());
}

TEST_CASE("[SolersOpenAIChatProtocol] prefers explicit call_id over leaked Responses item id") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());

	Array events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"fc_item_1","call_id":"call_real_1","type":"function","function":{"name":"project_get_info","arguments":"{}"}}]},"finish_reason":null}]})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == "call_real_1");
	CHECK(input_start.get("name", String()) == "project_get_info");
	CHECK(input_start.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "", R"json({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})json");
	bool saw_tool_call = false;
	for (int i = 0; i < events.size(); i++) {
		const Dictionary event = events[i];
		if (event.get("kind", String()) == String(SolersLLMEventKind::TOOL_CALL)) {
			saw_tool_call = true;
			CHECK(event.get("id", String()) == "call_real_1");
			CHECK(event.get("name", String()) == "project_get_info");
			CHECK(event.get("arguments", String()) == "{}");
		}
	}
	CHECK(saw_tool_call);
}

TEST_CASE("[SolersLLMProtocol] streams tool input before the executable tool call") {
	SolersAnthropicMessagesProtocol protocol;
	Dictionary state = protocol.begin_stream(Dictionary());

	Array events = protocol.parse_event(state, "content_block_start", R"json({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"call_1","name":"project_get_info","input":{}}})json");
	Dictionary input_start = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_START);
	REQUIRE_FALSE(input_start.is_empty());
	CHECK(input_start.get("id", String()) == "call_1");
	CHECK(input_start.get("name", String()) == "project_get_info");

	events = protocol.parse_event(state, "content_block_delta", R"json({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{}"}})json");
	Dictionary input_delta = find_event_kind(events, SolersLLMEventKind::TOOL_INPUT_DELTA);
	REQUIRE_FALSE(input_delta.is_empty());
	CHECK(input_delta.get("arguments_delta", String()) == "{}");
	CHECK(input_delta.get("arguments", String()) == "{}");

	events = protocol.parse_event(state, "content_block_stop", R"json({"type":"content_block_stop","index":0})json");
	Dictionary tool_call = find_event_kind(events, SolersLLMEventKind::TOOL_CALL);
	REQUIRE_FALSE(tool_call.is_empty());
	CHECK(tool_call.get("id", String()) == "call_1");
	CHECK(tool_call.get("arguments", String()) == "{}");
}

TEST_CASE("[SolersOpenAIChatProtocol] classifies structured context overflow without message matching") {
	SolersOpenAIChatProtocol protocol;
	Dictionary state;
	const Array events = protocol.parse_event(state, "error", R"json({"error":{"code":"context_length_exceeded","type":"invalid_request_error","message":"localized provider text"}})json");
	const Dictionary error = find_event_kind(events, SolersLLMEventKind::ERROR);
	REQUIRE_FALSE(error.is_empty());
	CHECK(error.get("failure_kind", String()) == "context_overflow");
}

TEST_CASE("[SolersOpenAIChatProtocol] replays one tool call as assistant tool_calls then tool result") {
	Array tool_calls;
	Dictionary call;
	call["id"] = "call_1";
	call["name"] = "project_get_info";
	call["arguments"] = "{}";
	tool_calls.push_back(call);

	Array messages;
	messages.push_back(SolersLLMMessage::user("What is in this project?"));
	messages.push_back(SolersLLMMessage::assistant("", tool_calls));
	messages.push_back(SolersLLMMessage::tool_result("call_1", "project_get_info", "{\"ok\":true}"));

	Dictionary request;
	request["model"] = "gpt-5.5";
	request["messages"] = messages;

	SolersOpenAIChatProtocol protocol;
	Dictionary body = protocol.build_request_body(request);
	Array lowered = body.get("messages", Array());

	REQUIRE(lowered.size() == 3);
	Dictionary user = lowered[0];
	Dictionary assistant = lowered[1];
	Dictionary output = lowered[2];

	CHECK_FALSE(body.get("store", true));
	CHECK(user.get("role", String()) == "user");
	CHECK(assistant.get("role", String()) == "assistant");
	Array native_calls = assistant.get("tool_calls", Array());
	REQUIRE(native_calls.size() == 1);
	Dictionary native_call = native_calls[0];
	Dictionary fn = native_call.get("function", Dictionary());
	CHECK(native_call.get("id", String()) == "call_1");
	CHECK(native_call.get("type", String()) == "function");
	CHECK(fn.get("name", String()) == "project_get_info");
	CHECK(fn.get("arguments", String()) == "{}");
	CHECK(output.get("role", String()) == "tool");
	CHECK(output.get("tool_call_id", String()) == "call_1");
}

TEST_CASE("[SolersOpenAIChatProtocol] preserves one assistant tool_call and tool result per parallel call") {
	Array tool_calls;
	Dictionary first;
	first["id"] = "call_a";
	first["name"] = "project_get_info";
	first["arguments"] = "{}";
	tool_calls.push_back(first);
	Dictionary second;
	second["id"] = "call_b";
	second["name"] = "scene_get_open_scenes";
	second["arguments"] = "{\"max_depth\":1}";
	tool_calls.push_back(second);

	Array messages;
	messages.push_back(SolersLLMMessage::user("Inspect the project."));
	messages.push_back(SolersLLMMessage::assistant("", tool_calls));
	messages.push_back(SolersLLMMessage::tool_result("call_a", "project_get_info", "{\"ok\":true}"));
	messages.push_back(SolersLLMMessage::tool_result("call_b", "scene_get_open_scenes", "{\"scenes\":[]}"));

	Dictionary request;
	request["model"] = "gpt-5.5";
	request["messages"] = messages;

	SolersOpenAIChatProtocol protocol;
	Dictionary body = protocol.build_request_body(request);
	Array lowered = body.get("messages", Array());

	REQUIRE(lowered.size() == 4);
	Dictionary assistant = lowered[1];
	Array native_calls = assistant.get("tool_calls", Array());
	REQUIRE(native_calls.size() == 2);
	Dictionary first_call = native_calls[0];
	Dictionary second_call = native_calls[1];
	Dictionary first_output = lowered[2];
	Dictionary second_output = lowered[3];

	CHECK(first_call.get("id", String()) == "call_a");
	CHECK(second_call.get("id", String()) == "call_b");
	CHECK(first_output.get("role", String()) == "tool");
	CHECK(first_output.get("tool_call_id", String()) == "call_a");
	CHECK(second_output.get("role", String()) == "tool");
	CHECK(second_output.get("tool_call_id", String()) == "call_b");
}

TEST_CASE("[SolersLLMMessage] attachment projection sends current pixels until a model response consumes them") {
	const String image_path = "user://.solers_tool_image_contract.bin";
	SolersTestPaths cleanup;
	cleanup.add(image_path);
	Ref<FileAccess> image_file = FileAccess::open(image_path, FileAccess::WRITE);
	REQUIRE(image_file.is_valid());
	PackedByteArray bytes;
	bytes.push_back(1);
	bytes.push_back(2);
	bytes.push_back(3);
	image_file->store_buffer(bytes);
	image_file.unref();

	Dictionary attachment;
	attachment["id"] = "capture_contract";
	attachment["content_sha256"] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	attachment["type"] = "image";
	attachment["mime_type"] = "image/png";
	attachment["local_path"] = image_path;
	Array attachments;
	attachments.push_back(attachment);
	Dictionary capture_attachment = attachment.duplicate(true);
	capture_attachment["id"] = "runtime_capture";
	capture_attachment["content_sha256"] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	capture_attachment["source"] = "tool_capture";
	capture_attachment["view_key"] = "runtime_view";
	Array capture_attachments;
	capture_attachments.push_back(capture_attachment);

	Array calls;
	Dictionary call;
	call["id"] = "call_capture";
	call["name"] = "render_capture";
	call["arguments"] = "{\"target\":\"runtime\"}";
	calls.push_back(call);
	Dictionary second_call;
	second_call["id"] = "call_query";
	second_call["name"] = "object_query";
	second_call["arguments"] = "{\"target\":\"scene\"}";
	calls.push_back(second_call);

	Array messages;
	Dictionary user_message = SolersLLMMessage::user("Inspect the reference.");
	user_message["attachments"] = attachments;
	messages.push_back(user_message);
	messages.push_back(SolersLLMMessage::assistant("", calls));
	messages.push_back(SolersLLMMessage::tool_result("call_capture", "render_capture", "{\"ok\":true}", capture_attachments));
	messages.push_back(SolersLLMMessage::tool_result("call_query", "object_query", "{\"ok\":true}"));

	const Array first_projection = SolersLLMMessage::project_attachments(messages);
	CHECK_FALSE(Dictionary(first_projection[0]).has("attachments"));
	CHECK(String(Dictionary(first_projection[0]).get("content", String())).contains("sha256=aaaaaaaa"));
	CHECK(Array(Dictionary(first_projection[2]).get("attachments", Array())).size() == 1);

	Dictionary request;
	request["model"] = "custom-gateway-model";
	request["messages"] = first_projection;
	SolersOpenAIChatProtocol openai;
	const Dictionary first_body = openai.build_request_body(request);
	CHECK(JSON::stringify(first_body).count("data:image/png;base64,") == 1);
	SolersLLMProtocolRegistry protocols;
	protocols.register_builtin_protocols();
	SolersLLMClient client;
	client.set_protocol_registry(&protocols);
	Dictionary profile;
	profile["protocol"] = "openai-chat";
	profile["base_url"] = "http://127.0.0.1:9/v1";
	Dictionary auth;
	auth["type"] = "none";
	REQUIRE(client.begin(request, profile, auth) == OK);
	const uint64_t materialize_deadline = OS::get_singleton()->get_ticks_msec() + 2000;
	while (client.get_request_body_bytes() == 0 && OS::get_singleton()->get_ticks_msec() < materialize_deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(client.get_request_body_bytes() == JSON::stringify(first_body, "", false, true).utf8().length());
	client.abort();

	SolersAnthropicMessagesProtocol anthropic;
	const Dictionary anthropic_body = anthropic.build_request_body(request);
	CHECK(JSON::stringify(anthropic_body).count("\"type\":\"image\"") == 1);

	messages.push_back(SolersLLMMessage::assistant("Observed the current pixels.", Array()));
	const Array second_projection = SolersLLMMessage::project_attachments(messages);
	CHECK_FALSE(Dictionary(second_projection[0]).has("attachments"));
	CHECK_FALSE(Dictionary(second_projection[2]).has("attachments"));
	CHECK(String(Dictionary(second_projection[2]).get("content", String())).contains("sha256=bbbbbbbb"));
	request["messages"] = second_projection;
	CHECK(JSON::stringify(openai.build_request_body(request)).count("data:image/png;base64,") == 0);
	CHECK(JSON::stringify(anthropic.build_request_body(request)).count("\"type\":\"image\"") == 0);
}

TEST_CASE("[SolersLLMMessage] attachment projection keeps one latest image per semantic view across epochs") {
	Dictionary old_capture;
	old_capture["id"] = "capture_old";
	old_capture["content_sha256"] = "old_pixels";
	old_capture["source"] = "tool_capture";
	old_capture["view_key"] = "runtime_view";
	Dictionary new_capture;
	new_capture["id"] = "capture_new";
	new_capture["content_sha256"] = "new_pixels";
	new_capture["source"] = "tool_capture";
	new_capture["view_key"] = "runtime_view";
	Dictionary editor_capture;
	editor_capture["id"] = "capture_editor";
	editor_capture["content_sha256"] = "editor_pixels";
	editor_capture["source"] = "tool_capture";
	editor_capture["view_key"] = "editor_view";
	Array old_attachments;
	old_attachments.push_back(old_capture);
	Array new_attachments;
	new_attachments.push_back(new_capture);
	Array editor_attachments;
	editor_attachments.push_back(editor_capture);

	Array messages;
	messages.push_back(SolersLLMMessage::tool_result("old", "render.capture", "Tool 'render.capture' evidence: old.", old_attachments));
	messages.push_back(SolersLLMMessage::tool_result("new", "render.capture", "Tool 'render.capture' evidence: new.", new_attachments));
	messages.push_back(SolersLLMMessage::tool_result("editor", "render.capture", "Tool 'render.capture' evidence: editor.", editor_attachments));

	const Array projected = SolersLLMMessage::project_attachments(messages);
	CHECK_FALSE(Dictionary(projected[0]).has("attachments"));
	CHECK(String(Dictionary(projected[0]).get("content", String())).contains("sha256=old_pixels"));
	CHECK(Array(Dictionary(projected[1]).get("attachments", Array())).size() == 1);
	CHECK(Array(Dictionary(projected[2]).get("attachments", Array())).size() == 1);
	int retained_pixels = 0;
	for (int i = 0; i < projected.size(); i++) {
		retained_pixels += Array(Dictionary(projected[i]).get("attachments", Array())).size();
	}
	CHECK(retained_pixels == 2);
}

} // namespace TestSolersLlm
