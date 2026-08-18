/**************************************************************************/
/*  test_solers_editor.cpp                                                */
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

#include "core/object/message_queue.h"
#include "core/string/translation_server.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/split_container.h"
#include "scene/main/scene_tree.h"
#include "tests/test_macros.h"

#include "modules/modules_enabled.gen.h"
#include "modules/solers_ai/editor/solers_chat_cells.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/editor/solers_editor_plugin.h"
#include "modules/solers_ai/editor/solers_ui_theme.h"
#include "modules/solers_ai/generated/solers_svg_assets.gen.h"

TEST_FORCE_LINK(test_solers_editor)

namespace TestSolersEditor {

class ScopedEditorLanguage {
	String setting = "interface/editor/localization/editor_language";
	Variant previous;
	String previous_locale;

public:
	ScopedEditorLanguage() :
			previous(EditorSettings::get_singleton()->get_setting(setting)),
			previous_locale(TranslationServer::get_singleton()->get_locale()) {}

	~ScopedEditorLanguage() {
		EditorSettings::get_singleton()->set_setting(setting, previous);
		TranslationServer::get_singleton()->set_locale(previous_locale);
		solers_load_editor_translation();
	}
};

TEST_CASE("[SolersUITheme][SceneTree] typography and pane chrome stay inside the Solers subtree") {
	const Ref<Theme> theme = SolersUITheme::create();
	REQUIRE(theme.is_valid());
	const Ref<Font> body = theme->get_default_font();
	const Ref<Font> mono = theme->get_font(SceneStringName(font), SNAME("SolersMono"));
	const Ref<Font> title = theme->get_font(SceneStringName(font), SNAME("SolersSessionTitle"));
	REQUIRE(bool(body.is_valid() && mono.is_valid() && title.is_valid()));
	for (const char32_t character : { char32_t(0x4E16), char32_t(0x0636), char32_t(0x0939), char32_t(0x05E9), char32_t(0x0E17) }) {
		CHECK(body->has_char(character));
		CHECK(mono->has_char(character));
	}
	CHECK(theme->get_font_size(SceneStringName(font_size), SNAME("SolersSessionTitle")) > theme->get_font_size(SceneStringName(font_size), SNAME("SolersSessionMeta")));
	CHECK(theme->get_color(SceneStringName(font_color), SNAME("SolersSessionTitle")).a > theme->get_color(SceneStringName(font_color), SNAME("SolersSessionMeta")).a);
	CHECK(theme->get_constant(SNAME("separation"), SNAME("HSplitContainer")) > 0);
	CHECK(theme->get_stylebox(SNAME("split_bar_background"), SNAME("HSplitContainer")).is_valid());

	Control *host = memnew(Control);
	Label *ambient = memnew(Label("Godot"));
	VBoxContainer *solers_root = memnew(VBoxContainer);
	Label *body_label = memnew(Label(String::utf8("Solers \xE4\xB8\xAD\xE6\x96\x87")));
	solers_root->set_theme(theme);
	host->add_child(ambient);
	host->add_child(solers_root);
	solers_root->add_child(body_label);
	SceneTree::get_singleton()->get_root()->add_child(host);
	MessageQueue::get_singleton()->flush();
	CHECK(body_label->get_theme_default_font() == solers_root->get_theme()->get_default_font());
	CHECK(ambient->get_theme_default_font() != body_label->get_theme_default_font());
	host->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SolersUI][SceneTree][Editor] editor locale and technical tool chrome have separate authorities") {
	const String setting = "interface/editor/localization/editor_language";
	ScopedEditorLanguage restore;
	EditorSettings::get_singleton()->set_setting(setting, "zh_Hans");
	TranslationServer::get_singleton()->set_locale("zh_Hans");
	solers_load_editor_translation();
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Cancel", StringName()) != "Cancel");
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Send", StringName()) != "Send");
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("New chat", StringName()) == String::utf8("\xE6\x96\xB0\xE5\xBB\xBA\xE5\xAF\xB9\xE8\xAF\x9D"));
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Effort", StringName()) != "Effort");
	CHECK(solers_tool_icon_for_ui_kind("search") == SNAME("tool_search"));
	CHECK(solers_tool_icon_for_ui_kind("scene") == SNAME("tool_scene"));

	SolersToolCell *search = memnew(SolersToolCell);
	SolersToolCell *unknown = memnew(SolersToolCell);
	SceneTree::get_singleton()->get_root()->add_child(search);
	SceneTree::get_singleton()->get_root()->add_child(unknown);
	search->start("project.search", R"({"type":"path"})", "search");
	unknown->start("future.tool", "{}", "synthetic");
	CHECK(search->get_status_text() == "Running tool.project.search path");
	CHECK(unknown->get_status_text() == "Running tool.future.tool");
	search->update("runtime.control", R"({"action":"set_property"})", "run");
	search->finish(true, String(), 4);
	CHECK(search->get_status_text() == "Ran tool.runtime.control set_property");
	CHECK(search->get_tool_icon() == SNAME("tool_run"));
	CHECK(unknown->get_tool_icon() == SNAME("sparkle"));
#ifdef MODULE_SVG_ENABLED
	Ref<Texture2D> tool_icon = SolersIcons::get(search->get_tool_icon(), 16);
	CHECK(tool_icon.is_valid());
	if (tool_icon.is_valid()) {
		CHECK(tool_icon->get_rid() == SolersIcons::get(SNAME("tool_run"), 16)->get_rid());
	}
	tool_icon.unref();
#endif
	search->queue_free();
	unknown->queue_free();
	MessageQueue::get_singleton()->flush();
	SolersIcons::clear_cache();
}

TEST_CASE("[SolersUI][SceneTree] user messages preserve the bubble, hover footer, and full-width editor") {
	SolersUserMessageCell *cell = memnew(SolersUserMessageCell);
	cell->set_theme(SolersUITheme::create());
	cell->configure(42, "Edit this earlier request", Array(), "2026.8.17 12:30", Callable(), Callable());
	SceneTree::get_singleton()->get_root()->add_child(cell);
	MessageQueue::get_singleton()->flush();
	Control *bubble = Object::cast_to<Control>(cell->find_child("UserMessageBubble", true, false));
	Control *footer = Object::cast_to<Control>(cell->find_child("UserMessageFooter", true, false));
	Control *editor = Object::cast_to<Control>(cell->find_child("HistoryMessageEditorSurface", true, false));
	REQUIRE(bool(bubble && footer && editor));
	CHECK(editor->get_theme_constant("margin_top") == 14 * EDSCALE);
	CHECK(editor->get_theme_constant("margin_bottom") == 7 * EDSCALE);
	TypedArray<Node> action_buttons = editor->find_children("*", "Button", true, false);
	REQUIRE(action_buttons.size() == 2);
	for (int i = 0; i < action_buttons.size(); i++) {
		Button *button = Object::cast_to<Button>(action_buttons[i].operator Object *());
		REQUIRE(button != nullptr);
		CHECK(button->get_translation_domain() == SNAME("godot.editor"));
		CHECK(button->get_text() == (i == 0 ? "Cancel" : "Send"));
	}
	CHECK(cell->get_mouse_filter() == Control::MOUSE_FILTER_PASS);
	CHECK(footer->get_mouse_filter() == Control::MOUSE_FILTER_PASS);
	for (int i = 0; i < footer->get_child_count(); i++) {
		Control *footer_child = Object::cast_to<Control>(footer->get_child(i));
		REQUIRE(bool(footer_child));
		CHECK(footer_child->get_mouse_filter() != Control::MOUSE_FILTER_STOP);
	}
	CHECK(bubble->is_visible());
	CHECK(footer->is_visible());
	CHECK_FALSE(editor->is_visible());
	CHECK(footer->get_modulate().a == 0.0f);
	cell->notification(Control::NOTIFICATION_MOUSE_ENTER);
	CHECK(footer->get_modulate().a == 1.0f);
	cell->notification(Control::NOTIFICATION_MOUSE_EXIT);
	CHECK(footer->get_modulate().a == 0.0f);
	CHECK(footer->get_mouse_filter() == Control::MOUSE_FILTER_PASS);
#ifdef MODULE_SVG_ENABLED
	CHECK(SolersIcons::get(SNAME("copy"), 16).is_valid());
#endif
	cell->queue_free();
	MessageQueue::get_singleton()->flush();
	SolersIcons::clear_cache();
}
} // namespace TestSolersEditor
