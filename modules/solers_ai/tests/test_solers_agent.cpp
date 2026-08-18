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
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "scene/gui/box_container.h"
#include "scene/gui/scroll_container.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "tests/test_macros.h"
#include "tests/test_tools.h"

#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/editor/solers_chat_cells.h"
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
	history.push_back(SolersLLMMessage::tool_result("call_old", "object.transaction", R"({"ok":true,"data":{"mutation":{"receipt":{"scene_after":{"root_object_id":42,"version":7}}}}})"));
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
	history.push_back(SolersLLMMessage::assistant("The missing collision is at z=-7.244.", make_tool_calls("query_a", "object.query")));
	history.push_back(SolersLLMMessage::tool_result("query_a", "object.query", String("large scene payload ").repeat(2000)));

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

TEST_CASE("[SolersContextManager] consumed calls project arguments without rewriting tool evidence") {
	Array history;
	history.push_back(make_user_message("Inspect the scene.", 1));
	Array consumed_calls = make_tool_calls("consumed", "object.transaction");
	Dictionary consumed_call = consumed_calls[0];
	const String consumed_arguments = JSON::stringify(Dictionary({ { "operations", Array({ Dictionary({ { "op", "update" }, { "properties", Dictionary({ { "large", String("argument ").repeat(2000) } }) } }) }) } }));
	consumed_call["arguments"] = consumed_arguments;
	consumed_calls[0] = consumed_call;
	history.push_back(SolersLLMMessage::assistant("Applying the observed state.", consumed_calls));
	history.push_back(SolersLLMMessage::tool_result("consumed", "object.transaction", R"({"ok":true,"data":{"mutation":{"receipt":{"scene_after":{"version":9}}}}})"));
	history.push_back(SolersLLMMessage::assistant("The scene receipt is now version 9.", Array()));
	Array live_calls = make_tool_calls("live", "render.capture");
	Dictionary live_call = live_calls[0];
	const String live_arguments = JSON::stringify(Dictionary({ { "focus_paths", Array({ String("/root/Player").repeat(200) }) } }));
	live_call["arguments"] = live_arguments;
	live_calls[0] = live_call;
	history.push_back(SolersLLMMessage::assistant("Verifying pixels.", live_calls));
	history.push_back(SolersLLMMessage::tool_result("live", "render.capture", "exact image receipt"));

	SolersContextManager manager;
	CHECK(manager.project_consumed_tool_arguments(history) > 0);
	const String wire = JSON::stringify(history);
	const String projected_arguments = Dictionary(Array(Dictionary(history[1]).get("tool_calls", Array()))[0]).get("arguments", String());
	const Dictionary projection = JSON::parse_string(projected_arguments);
	CHECK(projection.get("consumed", false));
	CHECK(projection.get("original_sha256", String()) == consumed_arguments.sha256_text());
	CHECK(Dictionary(Array(Dictionary(history[4]).get("tool_calls", Array()))[0]).get("arguments", String()) == live_arguments);
	CHECK(wire.contains("scene_after"));
	CHECK(wire.contains("exact image receipt"));
	CHECK(manager.project_consumed_tool_arguments(history) == 0);
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
	history.push_back(SolersLLMMessage::assistant("query", make_tool_calls("c1", "object.query")));
	history.push_back(SolersLLMMessage::tool_result("c1", "object.query", String("scene dump ").repeat(4000)));
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
	write("turn_outcome", 13, 1, Dictionary({ { "outcome", "aborted" } }));
	for (int i = 0; i < 30; i++) {
		Dictionary message = make_user_message("history " + itos(i));
		message["author"] = "human";
		write("message", 14 + i, 2 + i, message);
	}
	solers_transcript_flush(session_id);
	SolersAgentSession restored;
	restored.set_session(project, session_id);
	const Array timeline = restored.get_timeline_entries();
	REQUIRE(timeline.size() == 33);
	CHECK(Dictionary(timeline[1]).get("phase", String()) == "cancelled");
	SolersDock *dock = memnew(SolersDock);
	dock->set_theme(SolersUITheme::create());
	dock->set_size(Size2(640, 720));
	SceneTree::get_singleton()->get_root()->add_child(dock);
	dock->load_chat_history(timeline);
	MessageQueue::get_singleton()->flush();
	Node *approval_mode = dock->find_child("ApprovalModeOption", true, false);
	HBoxContainer *composer_toolbar = Object::cast_to<HBoxContainer>(dock->find_child("ComposerToolbar", true, false));
	REQUIRE((approval_mode != nullptr && composer_toolbar != nullptr));
	REQUIRE(composer_toolbar->get_child_count() >= 2);
	CHECK(composer_toolbar->get_child(1)->get_name() == "ComposerModelChip");
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
	registry.register_tool(memnew(SolersFunctionTool("synthetic.native_success", "Return the handler result after a native diagnostic.", Dictionary({ { "type", "object" }, { "properties", Dictionary() } }), SolersToolExposure::DIRECT, capability,
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
