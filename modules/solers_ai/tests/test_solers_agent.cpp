/**************************************************************************/
/*  test_solers_agent.cpp                                                 */
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
#include "core/core_globals.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/tcp_server.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/scroll_container.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "tests/signal_watcher.h"
#include "tests/test_macros.h"
#include "tests/test_tools.h"

#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/editor/solers_chat_cells.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/editor/solers_dock.h"
#include "modules/solers_ai/editor/solers_ui_theme.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/tests/support/solers_test_state.h"

TEST_FORCE_LINK(test_solers_agent)

namespace TestSolersAgent {

Dictionary make_user_message(const String &p_text, int p_turn_id = 0) {
	Dictionary message;
	message["role"] = "user";
	message["content"] = p_text;
	if (p_turn_id > 0) {
		message["turn_id"] = p_turn_id;
	}
	return message;
}

Array make_tool_calls(const String &p_id, const String &p_name) {
	Dictionary call;
	call["id"] = p_id;
	call["name"] = p_name;
	call["arguments"] = "{}";
	return Array({ call });
}

class ScopedAgentSettings {
	EditorSettings *settings;
	Dictionary snapshot;

public:
	ScopedAgentSettings(EditorSettings *p_settings, const Array &p_paths) :
			settings(p_settings) {
		for (const Variant &path_value : p_paths) {
			const String path = path_value;
			snapshot[path] = settings->has_setting(path) ? settings->get_setting(path) : Variant();
			settings->erase(path);
		}
	}

	~ScopedAgentSettings() {
		for (const Variant &path_value : snapshot.keys()) {
			const String path = path_value;
			settings->erase(path);
			if (snapshot[path].get_type() != Variant::NIL) {
				settings->set_manually(path, snapshot[path]);
			}
		}
		EditorSettings::save();
	}
};

String make_event_stream_response(const String &p_body) {
	return vformat("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", p_body.utf8().length(), p_body);
}

String make_chat_tools_response(const Array &p_calls) {
	Array native_calls;
	for (int i = 0; i < p_calls.size(); i++) {
		const Dictionary call = p_calls[i];
		Dictionary function;
		function["name"] = call.get("name", String());
		function["arguments"] = call.get("arguments", "{}");
		Dictionary native_call;
		native_call["index"] = i;
		native_call["id"] = call.get("id", String());
		native_call["type"] = "function";
		native_call["function"] = function;
		native_calls.push_back(native_call);
	}
	Dictionary delta;
	delta["role"] = "assistant";
	delta["tool_calls"] = native_calls;
	const Dictionary streamed({ { "choices", Array({ Dictionary({ { "delta", delta }, { "finish_reason", Variant() } }) }) } });
	const Dictionary finished({ { "choices", Array({ Dictionary({ { "delta", Dictionary() }, { "finish_reason", "tool_calls" } }) }) } });
	return make_event_stream_response("data: " + JSON::stringify(streamed) + "\n\ndata: " + JSON::stringify(finished) + "\n\ndata: [DONE]\n\n");
}

String make_chat_tool_response(const String &p_call_id, const String &p_tool_name) {
	return make_chat_tools_response(make_tool_calls(p_call_id, p_tool_name));
}

String make_fragmented_chat_tool_response(const String &p_call_id, const String &p_tool_name) {
	const String id = JSON::stringify(p_call_id);
	const String name = JSON::stringify(p_tool_name);
	const String first = vformat("data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"tool_calls\":[{\"index\":0,\"id\":%s,\"type\":\"function\",\"function\":{\"name\":%s,\"arguments\":\"{\"}}]},\"finish_reason\":null}]}\n\n", id, name);
	const String second = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"}\"}}]},\"finish_reason\":null}]}\n\n";
	const String completed = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n";
	return make_event_stream_response(first + second + completed);
}

bool read_http_request(const Ref<StreamPeerTCP> &p_connection, PackedByteArray &r_buffer, String &r_body) {
	p_connection->poll();
	const int available = p_connection->get_available_bytes();
	if (available > 0) {
		const int offset = r_buffer.size();
		r_buffer.resize(offset + available);
		int received = 0;
		if (p_connection->get_partial_data(r_buffer.ptrw() + offset, available, received) != OK) {
			return false;
		}
		r_buffer.resize(offset + received);
	}
	if (r_buffer.is_empty()) {
		return false;
	}
	const String request = String::utf8((const char *)r_buffer.ptr(), r_buffer.size());
	const int separator = request.find("\r\n\r\n");
	if (separator < 0) {
		return false;
	}
	int content_length = -1;
	for (const String &line : request.substr(0, separator).split("\r\n")) {
		if (line.to_lower().begins_with("content-length:")) {
			content_length = line.get_slice(":", 1).strip_edges().to_int();
			break;
		}
	}
	const int body_offset = separator + 4;
	if (content_length < 0 || r_buffer.size() < body_offset + content_length) {
		return false;
	}
	r_body = String::utf8((const char *)r_buffer.ptr() + body_offset, content_length);
	return true;
}

int serve_agent_responses(SolersAgentSession &p_session, const Ref<TCPServer> &p_server, const Array &p_responses, Array *r_request_bodies = nullptr) {
	int served = 0;
	Ref<StreamPeerTCP> connection;
	PackedByteArray request_buffer;
	Vector<Ref<StreamPeerTCP>> completed_connections;
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 5000;
	while (p_session.is_running() && OS::get_singleton()->get_ticks_msec() < deadline) {
		p_session.poll();
		if (connection.is_null() && served < p_responses.size() && p_server->is_connection_available()) {
			connection = p_server->take_connection();
		}
		if (connection.is_valid()) {
			String body;
			if (read_http_request(connection, request_buffer, body)) {
				if (r_request_bodies) {
					r_request_bodies->push_back(body);
				}
				const CharString response = String(p_responses[served++]).utf8();
				connection->put_data((const uint8_t *)response.get_data(), response.length());
				completed_connections.push_back(connection);
				connection.unref();
				request_buffer.clear();
			}
		}
		OS::get_singleton()->delay_usec(1000);
	}
	return served;
}

Dictionary find_request_tool(const String &p_body, const String &p_name) {
	const Variant parsed = JSON::parse_string(p_body);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return Dictionary();
	}
	for (const Variant &value : Array(Dictionary(parsed).get("tools", Array()))) {
		const Dictionary function = Dictionary(value).get("function", Dictionary());
		if (function.get("name", String()) == p_name) {
			return function;
		}
	}
	return Dictionary();
}

TEST_CASE("[SolersTrace] transcript parser skips incomplete audit records silently") {
	Dictionary record;
	CHECK_FALSE(solers_transcript_parse_record("", record));
	CHECK_FALSE(solers_transcript_parse_record("{incomplete", record));
	CHECK_FALSE(solers_transcript_parse_record("[]", record));
	CHECK(solers_transcript_parse_record("{\"kind\":\"tool_result\",\"call_id\":\"call_1\"}", record));
	CHECK(record.get("call_id", String()) == "call_1");
}

TEST_CASE("[SolersContextManager] compaction replaces the active prefix and preserves its tool suffix") {
	SolersContextManager context;
	Array history;
	CHECK_FALSE(context.should_compact(899, 1000, 100));
	CHECK(context.should_compact(900, 1000, 100));
	CHECK_FALSE(context.should_compact(900, 0, 100));
	Dictionary old_summary = make_user_message("superseded continuation");
	old_summary["role"] = SolersContextManager::MODEL_CONTEXT_ROLE;
	old_summary["origin"] = "compaction_summary";
	history.push_back(old_summary);
	history.push_back(make_user_message(String("obsolete ").repeat(8000), 1));
	history.push_back(SolersLLMMessage::tool_result("call_old", "scene.node.update", R"({"ok":true,"data":{"mutation":{"receipt":{"scene_after":{"root_object_id":42,"version":7}}}}})"));
	history.push_back(make_user_message("Continue the current build exactly.", 2));
	history.push_back(SolersLLMMessage::assistant("Capturing.", make_tool_calls("call_live", "render.capture")));
	history.push_back(SolersLLMMessage::tool_result("call_live", "render.capture", String("exact payload ").repeat(1000)));
	const Dictionary result = context.apply_compaction(history, "Keep building from engine evidence.", 8000);
	CHECK(result.get("accepted", false));
	const Array compacted = result.get("messages", Array());
	REQUIRE(compacted.size() == 5);
	CHECK(Dictionary(compacted[0]).get("origin", String()) == "compaction_summary");
	CHECK(Dictionary(compacted[1]).get("origin", String()) == "turn_checkpoint");
	CHECK(String(Dictionary(compacted[1]).get("content", String())).contains("root_object_id"));
	const String wire = JSON::stringify(compacted);
	CHECK(wire.contains("exact payload"));
	CHECK(wire.contains("Continue the current build exactly."));
	CHECK_FALSE(wire.contains("obsolete"));
	CHECK_FALSE(wire.contains("superseded continuation"));
	CHECK((int)result.get("tokens_after", 0) < (int)result.get("tokens_before", 0));
}

TEST_CASE("[SolersContextManager] completed-turn projection preserves the exact conversation spine") {
	Array history;
	history.push_back(make_user_message("Old task B: tune the sky.", 7));
	history.push_back(SolersLLMMessage::assistant("Task B is complete.", Array()));
	history.push_back(make_user_message("Task A: repair the fence collision.", 8));
	history.push_back(SolersLLMMessage::assistant("The missing collision is at z=-7.244.", make_tool_calls("query_a", "scene.inspect")));
	history.push_back(SolersLLMMessage::tool_result("query_a", "scene.inspect", String("large scene payload ").repeat(2000)));

	Array projected = SolersContextManager::project_completed_turns(history);
	const String wire = JSON::stringify(projected);
	CHECK(wire.contains("Task A: repair the fence collision."));
	CHECK(wire.contains("The missing collision is at z=-7.244."));
	CHECK(wire.contains("Old task B: tune the sky."));
	CHECK_FALSE(wire.contains("large scene payload"));
	CHECK_FALSE(wire.contains("query_a"));

	projected.push_back(make_user_message("Continue exactly.", 9));
	const String continued = JSON::stringify(SolersContextManager::project_completed_turns(projected));
	CHECK(continued.contains("Task A: repair the fence collision."));
	CHECK(continued.contains("Continue exactly."));
}

TEST_CASE("[SolersContextManager] completed tool evidence is outside tool-call syntax") {
	Array history;
	history.push_back(make_user_message("Inspect the scene.", 1));
	Array consumed_calls = make_tool_calls("consumed", "scene.node.update");
	Dictionary consumed_call = consumed_calls[0];
	const String consumed_arguments = JSON::stringify(Dictionary({ { "capability", "scene.node.update" }, { "arguments", Dictionary({ { "properties", Dictionary({ { "large", String("argument ").repeat(2000) } }) } }) } }));
	consumed_call["arguments"] = consumed_arguments;
	consumed_calls[0] = consumed_call;
	history.push_back(SolersLLMMessage::assistant("Applying the observed state.", consumed_calls));
	history.push_back(SolersLLMMessage::tool_result("consumed", "scene.node.update", R"({"ok":true,"data":{"mutation":{"receipt":{"scene_after":{"version":9}}}}})"));
	history.push_back(SolersLLMMessage::assistant("The scene receipt is now version 9.", Array()));
	Array live_calls = make_tool_calls("live", "render.capture");
	Dictionary live_call = live_calls[0];
	const String live_arguments = JSON::stringify(Dictionary({ { "focus_paths", Array({ String("/root/Player").repeat(200) }) } }));
	live_call["arguments"] = live_arguments;
	live_calls[0] = live_call;
	history.push_back(SolersLLMMessage::assistant("Verifying pixels.", live_calls));
	history.push_back(SolersLLMMessage::tool_result("live", "render.capture", "exact image receipt"));

	const String canonical = JSON::stringify(history);
	const Array projected = SolersContextManager::project_tool_evidence(history);
	const String wire = JSON::stringify(projected);
	CHECK(JSON::stringify(history) == canonical);
	CHECK(projected.size() == history.size());
	CHECK_FALSE(wire.contains(consumed_arguments));
	CHECK_FALSE(wire.contains("consumed"));
	CHECK_FALSE(wire.contains("original_sha256"));
	CHECK(Dictionary(projected[2]).get("role", String()) == SolersContextManager::MODEL_CONTEXT_ROLE);
	CHECK(Dictionary(projected[2]).get("origin", String()) == "tool_evidence");
	CHECK_FALSE(Dictionary(projected[2]).has("tool_call_id"));
	CHECK(String(Dictionary(projected[2]).get("content", String())).contains("scene_after"));
	CHECK(Dictionary(Array(Dictionary(projected[4]).get("tool_calls", Array()))[0]).get("arguments", String()) == live_arguments);
	CHECK(Dictionary(projected[5]).get("role", String()) == SolersLLMRole::TOOL);
	CHECK(String(Dictionary(projected[5]).get("content", String())) == "exact image receipt");
}

TEST_CASE("[SolersContextManager] consumed tool evidence shares one newest-first budget") {
	Array history;
	history.push_back(SolersLLMMessage::assistant("Inspecting old state.", make_tool_calls("old", "scene.inspect")));
	history.push_back(SolersLLMMessage::tool_result("old", "scene.inspect", String("old scene evidence ").repeat(3000)));
	history.push_back(SolersLLMMessage::assistant("Inspecting current state.", make_tool_calls("current", "scene.inspect")));
	history.push_back(SolersLLMMessage::tool_result("current", "scene.inspect", String("current scene evidence ").repeat(3000)));
	history.push_back(SolersLLMMessage::assistant("Continue.", Array()));
	const Array projected = SolersContextManager::project_tool_evidence(history);
	const String wire = JSON::stringify(projected);
	CHECK(wire.contains("current scene evidence"));
	CHECK_FALSE(wire.contains("old scene evidence"));
	CHECK(SolersContextManager::estimate_messages_tokens(projected) < SolersContextManager::estimate_messages_tokens(history));
}

TEST_CASE("[SolersContextManager] envelope clamp keeps head and tail under the token budget") {
	const String body = String("token-text-").repeat(2000);
	const String clamped = SolersContextManager::clamp_to_tokens(body, 64);
	CHECK(SolersContextManager::estimate_tokens(clamped) <= 64);
	CHECK(clamped.begins_with("token-text-"));
	CHECK(clamped.ends_with("token-text-"));
	CHECK(clamped.contains("[...truncated...]"));
	CHECK(SolersContextManager::clamp_to_tokens("short", 64) == "short");
	CHECK(SolersContextManager::clamp_to_tokens(body, 0).is_empty());
}

TEST_CASE("[SolersContextManager] compaction never returns more than its budget") {
	Array history;
	history.push_back(make_user_message(String("obsolete ").repeat(4000), 1));
	history.push_back(SolersLLMMessage::assistant("query", make_tool_calls("c1", "scene.inspect")));
	history.push_back(SolersLLMMessage::tool_result("c1", "scene.inspect", String("scene dump ").repeat(4000)));
	history.push_back(make_user_message("place the unit", 2));
	SolersContextManager context;
	const Dictionary result = context.apply_compaction(history, "Continue from the live scene.", 2000);
	CHECK(result.get("accepted", false));
	const Array compacted = result.get("messages", Array());
	CHECK((int)result.get("tokens_after", 0) <= 2000);
	CHECK((int)result.get("tokens_after", 0) < (int)result.get("tokens_before", 0));
	const String wire = JSON::stringify(compacted);
	CHECK(wire.contains("place the unit"));
	CHECK_FALSE(wire.contains(String("obsolete ").repeat(8)));
	CHECK_FALSE(wire.contains(String("scene dump ").repeat(8)));
	CHECK(context.get_token_count_with_pending(compacted, "sys", 4) == SolersContextManager::estimate_tokens("sys") + 4 + SolersContextManager::estimate_messages_tokens(compacted));
}

TEST_CASE("[SolersContextManager] rejects a projection that cannot make progress") {
	SolersContextManager context;
	const Array history = Array({ make_user_message("current instruction", 7) });
	const Dictionary result = context.apply_compaction(history, String("larger summary ").repeat(200), 32);
	CHECK_FALSE(result.get("accepted", true));
	CHECK_FALSE(result.has("messages"));
	CHECK(context.get_compaction_count() == 0);
}

TEST_CASE("[SolersContextManager] one observation can never fill the window") {
	const String dump = String("{\"node\":\"Node3D\"},").repeat(60000);
	const String clamped = SolersContextManager::clamp_to_tokens(dump, SolersContextManager::TOOL_RESULT_MAX_TOKENS);
	CHECK(SolersContextManager::estimate_tokens(clamped) <= SolersContextManager::TOOL_RESULT_MAX_TOKENS);
	// Position-independent truncation is what keeps the prompt prefix cacheable.
	CHECK(clamped == SolersContextManager::clamp_to_tokens(dump, SolersContextManager::TOOL_RESULT_MAX_TOKENS));
}

TEST_CASE("[SolersContextManager] transient request state replaces the prior request snapshot") {
	SolersContextManager context;
	Array persistent;
	persistent.push_back(SolersLLMMessage::user("Persistent turn"));
	context.record_usage(1000, persistent.size(), 300);
	CHECK(context.get_token_count_with_pending(persistent, String(), 0, 50) == 750);
	persistent.push_back(SolersLLMMessage::assistant("New durable content", Array()));
	Array pending;
	pending.push_back(persistent[1]);
	CHECK(context.get_token_count_with_pending(persistent, String(), 0, 50) == 750 + SolersContextManager::estimate_messages_tokens(pending));
}

TEST_CASE("[SolersSession][SceneTree][Editor] journal rows preserve terminal semantics and virtualized ownership") {
	const String session_id = "timeline-authority-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
	const String project = "test://" + session_id;
	auto write = [&](const String &p_type, int p_id, int p_turn, Dictionary p_event) {
		p_event["event_type"] = p_type;
		p_event["event_id"] = p_id;
		p_event["turn_id"] = p_turn;
		p_event["project_path"] = project;
		p_event["session_id"] = session_id;
		solers_transcript_write(p_event);
	};
	Dictionary first = make_user_message("first turn");
	first["author"] = "human";
	write("message", 10, 1, first);
	Dictionary compacting;
	compacting["compaction_id"] = 41;
	compacting["phase"] = "started";
	write("context_compaction", 11, 1, compacting);
	compacting["phase"] = "cancelled";
	write("context_compaction", 12, 1, compacting);
	write("plan_updated", 13, 1, Dictionary({ { "plan", Array({ Dictionary({ { "step", "Inspect" }, { "status", "in_progress" } }) }) } }));
	write("turn_outcome", 14, 1, Dictionary({ { "outcome", "aborted" }, { "usage", Dictionary({ { "input_tokens", 300 }, { "output_tokens", 100 }, { "reasoning_tokens", 50 }, { "cache_read_tokens", 25 }, { "cache_write_tokens", 25 }, { "context_window", 1000 }, { "message_count", 6 }, { "media_reference_count", 2 }, { "future_provider_field", 7 } }) } }));
	for (int i = 0; i < 30; i++) {
		Dictionary message = make_user_message("history " + itos(i));
		message["author"] = "human";
		write("message", 15 + i, 2 + i, message);
	}
	solers_transcript_flush(session_id);
	SolersAgentSession restored;
	restored.set_session(project, session_id);
	const Dictionary last_request_usage = restored.get_status().get("last_request_usage", Dictionary());
	CHECK((int64_t)last_request_usage.get("used_tokens", 0) == 500);
	CHECK((int64_t)last_request_usage.get("context_window", 0) == 1000);
	CHECK((int)last_request_usage.get("message_count", 0) == 6);
	CHECK((int)last_request_usage.get("media_reference_count", 0) == 2);
	CHECK(restored.get_plan().is_empty());
	const Array timeline = restored.get_timeline_entries();
	REQUIRE(timeline.size() == 33);
	CHECK(Dictionary(timeline[1]).get("phase", String()) == "cancelled");
	SolersDock *dock = memnew(SolersDock);
	dock->set_theme(SolersUITheme::create());
	dock->set_size(Size2(640, 720));
	SceneTree::get_singleton()->get_root()->add_child(dock);
	dock->set_agent_session(&restored);
	dock->load_chat_history(timeline);
	SolersPermissionManager permissions;
	const Dictionary denied_request = permissions.request_user_approval("scene.node.update", Dictionary(), SolersPermissionManager::PERMISSION_EDIT_SCENE);
	dock->set_services(nullptr, nullptr, nullptr, &permissions, nullptr);
	MessageQueue::get_singleton()->flush();
	Node *approval_mode = dock->find_child("ApprovalModeOption", true, false);
	HBoxContainer *composer_toolbar = Object::cast_to<HBoxContainer>(dock->find_child("ComposerToolbar", true, false));
	REQUIRE((approval_mode != nullptr && composer_toolbar != nullptr));
	REQUIRE(composer_toolbar->get_child_count() >= 2);
	CHECK(composer_toolbar->get_child(1)->get_name() == "ComposerModelChip");
	SolersContextRing *context_ring = Object::cast_to<SolersContextRing>(composer_toolbar->get_node_or_null(NodePath("ContextUsageRing")));
	REQUIRE(context_ring != nullptr);
	CHECK(composer_toolbar->get_child(composer_toolbar->get_child_count() - 2) == context_ring);
	CHECK(Math::is_equal_approx(context_ring->get_usage_ratio(), 0.5f));
	CHECK(context_ring->get_tooltip_text().contains("latest model request"));
	context_ring->set_usage(212000, 256000, 7, 2, 1);
	CHECK(context_ring->get_tooltip_text().contains("212K"));
	CHECK(context_ring->get_tooltip_text().contains("256K"));
	CHECK(context_ring->get_tooltip_text().contains("7 projected messages, 2 media references"));
	CHECK(context_ring->get_tooltip_text().contains("1 context compactions"));
	CHECK(context_ring->get_usage_ratio() > 0.82f);
	CHECK(context_ring->get_custom_minimum_size().x == 28 * EDSCALE);
	context_ring->set_usage(1500, 1000);
	CHECK(Math::is_equal_approx(context_ring->get_usage_ratio(), 1.0f));
	context_ring->set_usage(500, 0);
	CHECK(context_ring->get_usage_ratio() < 0.0f);
	CHECK_FALSE(context_ring->get_tooltip_text().is_empty());
	VBoxContainer *rows = Object::cast_to<VBoxContainer>(dock->find_child("ChatTimelineMessages", true, false));
	ScrollContainer *scroll = Object::cast_to<ScrollContainer>(dock->find_child("ChatTimelineScroll", true, false));
	REQUIRE((rows != nullptr && scroll != nullptr));
	REQUIRE(rows->get_child_count() == timeline.size());
	for (int i = 0; i < timeline.size(); i++) {
		Node *row = rows->get_child(i);
		CHECK((int64_t)row->get_meta("timeline_event_id", -1) == (int64_t)Dictionary(timeline[i]).get("event_id", -1));
		CHECK((bool)row->get_meta("timeline_row", false));
		for (int child_index = 0; child_index < row->get_child_count(); child_index++) {
			CHECK_FALSE((bool)row->get_child(child_index)->get_meta("timeline_row", false));
		}
	}
	Control *history_editor = Object::cast_to<Control>(dock->find_child("HistoryMessageEditorSurface", true, false));
	REQUIRE(history_editor != nullptr);
	CHECK_FALSE(history_editor->is_visible());
	SolersSurface *permission_prompt = Object::cast_to<SolersSurface>(dock->find_child("PermissionPrompt", true, false));
	Button *allow_once = Object::cast_to<Button>(dock->find_child("PermissionAllowOnce", true, false));
	Button *allow_always = Object::cast_to<Button>(dock->find_child("PermissionAllowAlways", true, false));
	Button *deny = Object::cast_to<Button>(dock->find_child("PermissionDeny", true, false));
	Label *permission_tool = Object::cast_to<Label>(dock->find_child("PermissionTool", true, false));
	Label *permission_details = Object::cast_to<Label>(dock->find_child("PermissionDetails", true, false));
	REQUIRE(bool(permission_prompt && allow_once && allow_always && deny && permission_tool && permission_details));
	CHECK(permission_prompt->is_visible_in_tree());
	CHECK(permission_tool->get_text() == "scene.node.update");
	CHECK(permission_details->get_text().contains("scene"));
	CHECK(dock->find_child("QuestionPanel", true, false) == nullptr);
	deny->emit_signal(SceneStringName(pressed));
	CHECK(permissions.get_request_decision(denied_request.get("id", 0)) == SolersPermissionManager::DECISION_REJECTED);
	const Dictionary once_request = permissions.request_user_approval("resource.edit", Dictionary(), SolersPermissionManager::PERMISSION_EDIT_FILES);
	dock->set_services(nullptr, nullptr, nullptr, &permissions, nullptr);
	allow_once->emit_signal(SceneStringName(pressed));
	CHECK(permissions.get_request_decision(once_request.get("id", 0)) == SolersPermissionManager::DECISION_APPROVED);
	permissions.request_user_approval("network.fetch", Dictionary(), SolersPermissionManager::PERMISSION_NETWORK);
	dock->set_services(nullptr, nullptr, nullptr, &permissions, nullptr);
	allow_always->emit_signal(SceneStringName(pressed));
	CHECK(permissions.get_auto_approve_permission(SolersPermissionManager::PERMISSION_NETWORK));

	scroll->set_v_scroll((int)scroll->get_v_scroll_bar()->get_max());
	dock->set_size(Size2(480, 720));
	MessageQueue::get_singleton()->flush();
	CHECK(rows->get_child_count() == timeline.size());
	scroll->set_v_scroll(0);
	MessageQueue::get_singleton()->flush();
	CHECK_FALSE(history_editor->is_visible());
	dock->queue_free();
	MessageQueue::get_singleton()->flush();
	restored.reset_conversation();
	CHECK(Dictionary(restored.get_status().get("last_request_usage", Dictionary())).is_empty());
	const String empty_id = restored.get_status().get("session_id", String());
	CHECK_FALSE(FileAccess::exists(solers_session_dir().path_join("sessions").path_join(empty_id.sha256_text() + ".jsonl")));
	restored.shutdown();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SolersJournal] append repairs only an incomplete crash tail") {
	const String session_id = "crash-tail-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
	const String path = solers_session_dir().path_join("sessions").path_join(session_id.sha256_text() + ".jsonl");
	SolersTestPaths cleanup;
	cleanup.add(path);
	solers_transcript_flush(session_id);
	Ref<FileAccess> corrupt = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(corrupt.is_valid());
	Dictionary first;
	first["session_id"] = session_id;
	first["event_type"] = "model_retry";
	first["event_id"] = 1;
	corrupt->store_line(JSON::stringify(first));
	corrupt->store_string("{\"event_type\":\"message\"");
	corrupt.unref();
	Dictionary second;
	second["session_id"] = session_id;
	second["event_type"] = "model_retry";
	second["event_id"] = 2;
	solers_transcript_write(second);
	solers_transcript_flush(session_id);

	Ref<FileAccess> repaired = FileAccess::open(path, FileAccess::READ);
	int records = 0;
	while (repaired.is_valid() && !repaired->eof_reached()) {
		const String line = repaired->get_line();
		if (line.strip_edges().is_empty()) {
			continue;
		}
		Dictionary event;
		CHECK(solers_transcript_parse_record(line, event));
		records++;
	}
	CHECK(records == 2);
	repaired.unref();
}

TEST_CASE("[SolersSession] rewind marker projects one active append-only branch") {
	const String session_id = "rewind-projection-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
	const String project = "test://" + session_id;
	SolersTestPaths cleanup;
	cleanup.add(solers_session_dir().path_join("sessions").path_join(session_id.sha256_text() + ".jsonl"));
	auto write = [&](int p_id, const String &p_type, Dictionary p_event) {
		p_event["event_id"] = p_id;
		p_event["event_type"] = p_type;
		p_event["project_path"] = project;
		p_event["session_id"] = session_id;
		solers_transcript_write(p_event);
	};
	Dictionary first = make_user_message("first");
	first["wall"] = 100;
	first["session_revision"] = 1;
	write(1, "message", first);
	write(2, "message", SolersLLMMessage::assistant("answer", Array()));
	Dictionary replaced = make_user_message("replace me");
	write(3, "message", replaced);
	write(4, "rewind_prepared", Dictionary({ { "transaction", Dictionary({ { "transaction_id", "tx" } }) } }));
	write(5, "session_rewind", Dictionary({ { "target_event_id", 3 } }));
	Dictionary edited = make_user_message("edited");
	write(6, "message", edited);
	solers_transcript_flush(session_id);
	SolersAgentSession restored;
	restored.set_session(project, session_id);
	const Array timeline = restored.get_timeline_entries();
	REQUIRE(timeline.size() == 3);
	CHECK((int64_t)Dictionary(timeline[0]).get("wall", 0) == 100);
	CHECK((int64_t)Dictionary(timeline[0]).get("session_revision", 0) == 1);
	CHECK(String(Dictionary(timeline[2]).get("content", String())) == "edited");
	restored.shutdown();
}

TEST_CASE("[SolersSession][SceneTree][Editor] diagnostics never rewrite a handler result") {
	const String session_id = "idle-diagnostic-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
	const String project = "test://" + session_id;
	SolersAgentSession session;
	session.set_session(project, session_id);
	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	SolersToolCapability capability;
	capability.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	registry.register_tool(memnew(SolersFunctionTool("synthetic.native_success", "Return the handler result after a native diagnostic.", Dictionary({ { "type", "object" }, { "properties", Dictionary() } }), SolersToolExposure::MODEL, capability,
			[](const SolersToolContext &, const Dictionary &) {
				ERR_PRINT("synthetic native diagnostic");
				Dictionary result;
				result["ok"] = true;
				result["data"] = Dictionary({ { "native_postcondition", true } });
				return result;
			})));
	const bool print_ready = CoreGlobals::print_ready;
	CoreGlobals::print_ready = true;
	const Dictionary result = registry.call_tool(SNAME("synthetic.native_success"), Dictionary());
	CoreGlobals::print_ready = print_ready;
	REQUIRE((bool)result.get("ok", false));
	CHECK((bool)Dictionary(result.get("data", Dictionary())).get("native_postcondition", false));
	CHECK((int)session.get_status().get("godot_log_errors", -1) == 1);
	session.shutdown();
}

TEST_CASE("[SolersAgentSession][Editor] repeated read observations remain executable model input") {
	Ref<TCPServer> server;
	server.instantiate();
	REQUIRE(server->listen(0, IPAddress("127.0.0.1")) == OK);
	EditorSettings *editor_settings = EditorSettings::get_singleton();
	REQUIRE(editor_settings != nullptr);
	SolersProviderRegistry providers;
	SolersSettingsService settings;
	settings.set_provider_registry(&providers);
	const String prefix = "solers/ai/";
	const String provider_prefix = prefix + "providers/custom_openai_compatible/";
	ScopedAgentSettings restore(editor_settings, Array({ prefix + "provider", prefix + "local_models_only", provider_prefix + "configured", provider_prefix + "model", provider_prefix + "reasoning_effort", provider_prefix + "base_url", provider_prefix + "context_window", provider_prefix + "max_tokens", provider_prefix + "api_key" }));
	Dictionary config({ { "provider", "custom_openai_compatible" }, { "model", "synthetic-model" }, { "base_url", vformat("http://127.0.0.1:%d/v1", server->get_local_port()) }, { "api_key", "synthetic-key" } });
	REQUIRE((bool)settings.set_provider_config(config).get("ok", false));

	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	int executions = 0;
	SolersToolCapability capability;
	capability.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	registry.register_tool(memnew(SolersFunctionTool("synthetic.stable_observation", "Return one stable synthetic observation.", Dictionary({ { "type", "object" }, { "properties", Dictionary() }, { "additionalProperties", false } }), SolersToolExposure::MODEL, capability,
			[&executions](const SolersToolContext &, const Dictionary &) {
				executions++;
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "fact", "stable" } }) } });
			})));

	const String session_id = "repeated-observation-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
	SolersAgentSession session;
	session.set_models_dev(nullptr);
	session.set_tool_registry(&registry);
	session.set_settings_service(&settings);
	session.set_session("test://" + session_id, session_id);
	REQUIRE((bool)session.start_turn(Dictionary({ { "prompt", "Observe the same authority twice, then finish." } })).get("ok", false));

	SIGNAL_WATCH(&session, "tool_call_started");
	Array responses({ make_fragmented_chat_tool_response("call_first", registry.get_model_tool_name("synthetic.stable_observation")), make_chat_tool_response("call_second", registry.get_model_tool_name("synthetic.stable_observation")), make_event_stream_response("data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Complete.\"},\"finish_reason\":null}]}\n\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n") });
	const int served = serve_agent_responses(session, server, responses);
	session.abort();
	server->stop();
	const Dictionary status = session.get_status();
	CHECK(served == 3);
	CHECK(executions == 2);
	CHECK((int)status.get("model_requests", 0) == 3);
	CHECK(status.get("last_outcome", String()) == "completed");
	CHECK((int)status.get("godot_log_errors", -1) == 0);
	SIGNAL_CHECK("tool_call_started", Array({ Array({ "call_first", "synthetic.stable_observation", "{}" }), Array({ "call_second", "synthetic.stable_observation", "{}" }) }));
	SIGNAL_UNWATCH(&session, "tool_call_started");
	CHECK_MESSAGE(JSON::stringify(session.get_messages()).count("fact") == 2, JSON::stringify(session.get_messages()));
	session.reset_conversation();
	session.shutdown();
}

TEST_CASE("[SolersAgentSession][Editor] observations activate exact deferred schemas on the next request") {
	Ref<TCPServer> server;
	server.instantiate();
	REQUIRE(server->listen(0, IPAddress("127.0.0.1")) == OK);
	EditorSettings *editor_settings = EditorSettings::get_singleton();
	REQUIRE(editor_settings != nullptr);
	SolersProviderRegistry providers;
	SolersSettingsService settings;
	settings.set_provider_registry(&providers);
	const String prefix = "solers/ai/";
	const String provider_prefix = prefix + "providers/custom_openai_compatible/";
	ScopedAgentSettings restore(editor_settings, Array({ prefix + "provider", prefix + "local_models_only", provider_prefix + "configured", provider_prefix + "model", provider_prefix + "reasoning_effort", provider_prefix + "base_url", provider_prefix + "context_window", provider_prefix + "max_tokens", provider_prefix + "api_key" }));
	Dictionary config({ { "provider", "custom_openai_compatible" }, { "model", "synthetic-model" }, { "base_url", vformat("http://127.0.0.1:%d/v1", server->get_local_port()) }, { "api_key", "synthetic-key" } });
	REQUIRE((bool)settings.set_provider_config(config).get("ok", false));

	SolersPermissionManager permissions;
	permissions.set_auto_approve_permission(SolersPermissionManager::PERMISSION_OBSERVE, true);
	SolersToolRegistry registry;
	registry.set_permission_manager(&permissions);
	registry.register_default_tools();
	int observation_executions = 0;
	int deferred_executions = 0;
	SolersToolCapability observation_capability;
	observation_capability.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	registry.register_tool(memnew(SolersFunctionTool("synthetic.bootstrap_observation", "Return one observation and advertise its native follow-ups.", Dictionary({ { "type", "object" }, { "properties", Dictionary() }, { "additionalProperties", false } }), SolersToolExposure::MODEL, observation_capability,
			[&observation_executions](const SolersToolContext &, const Dictionary &) {
				observation_executions++;
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "fact", "bootstrap" } }) }, { "added_tools", Array({ "synthetic.deferred_operation", "synthetic.retired_operation" }) } });
			})));
	registry.register_tool(memnew(SolersFunctionTool("synthetic.failed_observation", "Return a failed observation that cannot activate tools.", Dictionary({ { "type", "object" }, { "properties", Dictionary() }, { "additionalProperties", false } }), SolersToolExposure::MODEL, observation_capability,
			[](const SolersToolContext &, const Dictionary &) {
				return Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "SYNTHETIC_OBSERVATION_FAILED" }, { "message", "Synthetic observation failed." }, { "recoverable", true } }) }, { "added_tools", Array({ "synthetic.failure_operation" }) } });
			})));
	SolersToolCapability deferred_capability;
	deferred_capability.permission = SolersPermissionManager::PERMISSION_OBSERVE;
	deferred_capability.operation_domain = SolersOperationDomain::EDITOR;
	deferred_capability.operation_mode = SolersOperationMode::QUERY;
	const Dictionary deferred_schema({ { "type", "object" }, { "properties", Dictionary({ { "exact_value", Dictionary({ { "type", "string" } }) } }) }, { "additionalProperties", false } });
	registry.register_tool(memnew(SolersFunctionTool("synthetic.deferred_operation", "Return the observation's exact deferred follow-up.", deferred_schema, SolersToolExposure::DEFERRED, deferred_capability,
			[&deferred_executions](const SolersToolContext &, const Dictionary &) {
				deferred_executions++;
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "fact", "deferred" } }) } });
			})));
	registry.register_tool(memnew(SolersFunctionTool("synthetic.retired_operation", "Synthetic capability used to verify catalog invalidation.", deferred_schema, SolersToolExposure::DEFERRED, deferred_capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary({ { "ok", true }, { "data", Dictionary() } }); })));
	registry.register_tool(memnew(SolersFunctionTool("synthetic.rewound_operation", "Synthetic capability used to verify rewind restoration.", deferred_schema, SolersToolExposure::DEFERRED, deferred_capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary({ { "ok", true }, { "data", Dictionary() } }); })));
	registry.register_tool(memnew(SolersFunctionTool("synthetic.failure_operation", "Capability that must not be activated by a failed observation.", deferred_schema, SolersToolExposure::DEFERRED, deferred_capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary({ { "ok", true }, { "data", Dictionary() } }); })));

	const String session_id = "deferred-activation-" + String::num_uint64(OS::get_singleton()->get_ticks_usec());
	const String project = "test://" + session_id;
	SolersTestPaths cleanup;
	cleanup.add(solers_session_dir().path_join("sessions").path_join(session_id.sha256_text() + ".jsonl"));
	SolersAgentSession session;
	session.set_models_dev(nullptr);
	session.set_tool_registry(&registry);
	session.set_settings_service(&settings);
	session.set_session(project, session_id);
	REQUIRE((bool)session.start_turn(Dictionary({ { "prompt", "Observe, then use the returned follow-up capability." } })).get("ok", false));

	Array first_calls = make_tool_calls("call_bootstrap", registry.get_model_tool_name("synthetic.bootstrap_observation"));
	first_calls.append_array(make_tool_calls("call_inactive", registry.get_model_tool_name("synthetic.deferred_operation")));
	first_calls.append_array(make_tool_calls("call_failed", registry.get_model_tool_name("synthetic.failed_observation")));
	const String completed_response = make_event_stream_response("data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Complete.\"},\"finish_reason\":null}]}\n\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n");
	Array request_bodies;
	const Array responses({ make_chat_tools_response(first_calls), make_chat_tool_response("call_deferred", registry.get_model_tool_name("synthetic.deferred_operation")), completed_response });
	const int served = serve_agent_responses(session, server, responses, &request_bodies);
	const Dictionary status = session.get_status();
	CHECK(served == 3);
	CHECK(observation_executions == 1);
	CHECK(deferred_executions == 1);
	CHECK((int)status.get("model_requests", 0) == 3);
	CHECK(status.get("last_outcome", String()) == "completed");
	const String conversation = JSON::stringify(session.get_messages());
	CHECK(conversation.contains("TOOL_NOT_ACTIVE"));
	CHECK(conversation.contains("deferred"));
	CHECK_FALSE(conversation.contains("\"added_tools\""));
	REQUIRE(request_bodies.size() == 3);
	const String deferred_model_name = registry.get_model_tool_name("synthetic.deferred_operation");
	const String retired_model_name = registry.get_model_tool_name("synthetic.retired_operation");
	const String rewound_model_name = registry.get_model_tool_name("synthetic.rewound_operation");
	const String failure_model_name = registry.get_model_tool_name("synthetic.failure_operation");
	CHECK(find_request_tool(request_bodies[0], deferred_model_name).is_empty());
	CHECK(find_request_tool(request_bodies[0], retired_model_name).is_empty());
	CHECK(find_request_tool(request_bodies[0], failure_model_name).is_empty());
	const Dictionary activated_tool = find_request_tool(request_bodies[1], deferred_model_name);
	REQUIRE_FALSE(activated_tool.is_empty());
	CHECK(Dictionary(activated_tool.get("parameters", Dictionary())).recursive_equal(deferred_schema, 0));
	CHECK(find_request_tool(request_bodies[1], failure_model_name).is_empty());
	CHECK_FALSE(String(request_bodies[1]).contains("added_tool_names"));
	REQUIRE(solers_transcript_flush(session_id) == OK);
	session.shutdown();
	const int64_t rewind_target_event_id = 100000;
	auto write_branch_event = [&](int64_t p_event_id, const String &p_type, Dictionary p_event) {
		p_event["event_id"] = p_event_id;
		p_event["event_type"] = p_type;
		p_event["project_path"] = project;
		p_event["session_id"] = session_id;
		solers_transcript_write(p_event);
	};
	write_branch_event(rewind_target_event_id, "message", Dictionary({ { "role", SolersLLMRole::USER }, { "author", "human" }, { "content", "Discarded branch." } }));
	write_branch_event(rewind_target_event_id + 1, "tool_result", Dictionary({ { "role", SolersLLMRole::TOOL }, { "call_id", "rewound_call" }, { "tool", "synthetic.rewound_operation" }, { "content", R"({"ok":true})" }, { "added_tool_names", Array({ rewound_model_name }) } }));
	write_branch_event(rewind_target_event_id + 2, "session_rewind", Dictionary({ { "target_event_id", rewind_target_event_id } }));
	REQUIRE(solers_transcript_flush(session_id) == OK);

	SolersToolRegistry restored_registry;
	restored_registry.set_permission_manager(&permissions);
	restored_registry.register_default_tools();
	restored_registry.register_tool(memnew(SolersFunctionTool("synthetic.deferred_operation", "Return the observation's exact deferred follow-up.", deferred_schema, SolersToolExposure::DEFERRED, deferred_capability,
			[&deferred_executions](const SolersToolContext &, const Dictionary &) {
				deferred_executions++;
				return Dictionary({ { "ok", true }, { "data", Dictionary({ { "fact", "deferred" } }) } });
			})));
	restored_registry.register_tool(memnew(SolersFunctionTool("synthetic.retired_operation", "No longer model-visible.", deferred_schema, SolersToolExposure::HIDDEN, deferred_capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary({ { "ok", true }, { "data", Dictionary() } }); })));
	restored_registry.register_tool(memnew(SolersFunctionTool("synthetic.rewound_operation", "Synthetic capability used to verify rewind restoration.", deferred_schema, SolersToolExposure::DEFERRED, deferred_capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary({ { "ok", true }, { "data", Dictionary() } }); })));
	SolersAgentSession restored_session;
	restored_session.set_models_dev(nullptr);
	restored_session.set_tool_registry(&restored_registry);
	restored_session.set_settings_service(&settings);
	restored_session.set_session(project, session_id);
	restored_registry.register_tool(memnew(SolersFunctionTool("synthetic.revision_observation", "New default observation after a catalog revision.", Dictionary({ { "type", "object" }, { "properties", Dictionary() } }), SolersToolExposure::MODEL, observation_capability,
			[](const SolersToolContext &, const Dictionary &) { return Dictionary({ { "ok", true }, { "data", Dictionary() } }); })));
	REQUIRE((bool)restored_session.start_turn(Dictionary({ { "prompt", "Continue from the restored capability state." } })).get("ok", false));
	CHECK(serve_agent_responses(restored_session, server, Array({ completed_response }), &request_bodies) == 1);
	REQUIRE(request_bodies.size() == 4);
	const Dictionary restored_tool = find_request_tool(request_bodies[3], deferred_model_name);
	REQUIRE_FALSE(restored_tool.is_empty());
	CHECK(Dictionary(restored_tool.get("parameters", Dictionary())).recursive_equal(deferred_schema, 0));
	CHECK(find_request_tool(request_bodies[3], retired_model_name).is_empty());
	CHECK(find_request_tool(request_bodies[3], rewound_model_name).is_empty());
	CHECK_FALSE(find_request_tool(request_bodies[3], restored_registry.get_model_tool_name("synthetic.revision_observation")).is_empty());

	restored_session.reset_conversation();
	const String reset_session_id = restored_session.get_status().get("session_id", String());
	cleanup.add(solers_session_dir().path_join("sessions").path_join(reset_session_id.sha256_text() + ".jsonl"));
	REQUIRE((bool)restored_session.start_turn(Dictionary({ { "prompt", "Attempt the deferred capability without a new observation." } })).get("ok", false));
	CHECK(serve_agent_responses(restored_session, server, Array({ make_chat_tool_response("call_after_reset", deferred_model_name), completed_response }), &request_bodies) == 2);
	REQUIRE(request_bodies.size() == 6);
	CHECK(find_request_tool(request_bodies[4], deferred_model_name).is_empty());
	CHECK(deferred_executions == 1);
	CHECK(JSON::stringify(restored_session.get_messages()).contains("TOOL_NOT_ACTIVE"));
	restored_session.shutdown();
	server->stop();
}

TEST_CASE("[SolersAgentSession] validates the Codex update_plan contract") {
	Array valid_steps;
	Dictionary first;
	first["step"] = "Whitebox";
	first["status"] = "completed";
	valid_steps.push_back(first);
	Dictionary second;
	second["step"] = "Lighting";
	second["status"] = "in_progress";
	valid_steps.push_back(second);
	Dictionary args;
	args["plan"] = valid_steps;
	CHECK(SolersAgentSession::validate_plan(args).get("ok", false));

	Dictionary duplicate = second.duplicate();
	duplicate["step"] = "Materials";
	valid_steps.push_back(duplicate);
	args["plan"] = valid_steps;
	Dictionary invalid = SolersAgentSession::validate_plan(args);
	CHECK_FALSE(invalid.get("ok", true));
	CHECK(Dictionary(invalid.get("error", Dictionary())).get("code", String()) == "INVALID_PLAN");

	valid_steps.resize(1);
	first["status"] = "blocked";
	valid_steps[0] = first;
	args["plan"] = valid_steps;
	CHECK_FALSE(SolersAgentSession::validate_plan(args).get("ok", true));

	first["status"] = "completed";
	valid_steps[0] = first;
	args["plan"] = valid_steps;
	CHECK(SolersAgentSession::validate_plan(args).get("ok", false));
}

TEST_CASE("[solers_format_plan_text] replaces its plan snapshot in place") {
	Array first_plan;
	Dictionary first_step;
	first_step["step"] = "Whitebox";
	first_step["status"] = "in_progress";
	first_plan.push_back(first_step);
	const String first_text = solers_format_plan_text("Starting geometry", first_plan);

	Array second_plan;
	Dictionary second_step;
	second_step["step"] = "Whitebox";
	second_step["status"] = "completed";
	second_plan.push_back(second_step);
	const String second_text = solers_format_plan_text("Geometry verified", second_plan);

	CHECK(first_text.contains("Starting geometry"));
	CHECK(second_text.contains("Geometry verified"));
	CHECK(second_text.contains(String::utf8("✓ Whitebox")));
	CHECK_FALSE(second_text.contains("Starting geometry"));
}

} // namespace TestSolersAgent
