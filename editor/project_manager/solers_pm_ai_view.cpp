/**************************************************************************/
/*  solers_pm_ai_view.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_pm_ai_view.h"

#include "core/os/os.h"
#include "editor/project_manager/solers_pm_cards.h"
#include "editor/project_manager/solers_pm_theme.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/scroll_container.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_SOLERS_AI_ENABLED
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#endif

// Curated rail order — remote vendors first, local runtimes, then custom.
static const char *SOLERS_AI_PROVIDER_ORDER[] = {
	"openai",
	"anthropic",
	"gemini",
	"deepseek",
	"qwen",
	"ollama",
	"lm_studio",
	"custom_openai_compatible",
};

// Lucide glyph bodies (24x24 viewBox; ISC license, see solers_ai/UI_ICON_LICENSE.txt).
static const char *SOLERS_AI_LUCIDE_CLOUD = "<path d=\"M17.5 19H9a7 7 0 1 1 6.71-9h1.79a4.5 4.5 0 1 1 0 9Z\"/>";
static const char *SOLERS_AI_LUCIDE_MONITOR = "<rect width=\"20\" height=\"14\" x=\"2\" y=\"3\" rx=\"2\"/><line x1=\"8\" x2=\"16\" y1=\"21\" y2=\"21\"/><line x1=\"12\" x2=\"12\" y1=\"17\" y2=\"21\"/>";
static const char *SOLERS_AI_LUCIDE_SLIDERS = "<line x1=\"21\" x2=\"14\" y1=\"4\" y2=\"4\"/><line x1=\"10\" x2=\"3\" y1=\"4\" y2=\"4\"/><line x1=\"21\" x2=\"12\" y1=\"12\" y2=\"12\"/><line x1=\"8\" x2=\"3\" y1=\"12\" y2=\"12\"/><line x1=\"21\" x2=\"16\" y1=\"20\" y2=\"20\"/><line x1=\"12\" x2=\"3\" y1=\"20\" y2=\"20\"/><line x1=\"14\" x2=\"14\" y1=\"2\" y2=\"6\"/><line x1=\"8\" x2=\"8\" y1=\"10\" y2=\"14\"/><line x1=\"16\" x2=\"16\" y1=\"18\" y2=\"22\"/>";
static const char *SOLERS_AI_LUCIDE_EYE = "<path d=\"M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>";

// Soft UE-toned status hues (muted, not toy-bright).
static const Color SOLERS_AI_COL_BLOCKER = Color(0.83f, 0.32f, 0.34f);
static const Color SOLERS_AI_COL_WARNING = Color(0.88f, 0.66f, 0.26f);
static const Color SOLERS_AI_COL_OK = Color(0.33f, 0.65f, 0.38f);

String SolersPMAIView::_setting_path(const String &p_key) const {
	return "solers/ai/" + p_key;
}

String SolersPMAIView::_stored_string(const String &p_key, const String &p_default) const {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (settings && settings->has_setting(_setting_path(p_key))) {
		return settings->get_setting(_setting_path(p_key));
	}
	return p_default;
}

bool SolersPMAIView::_stored_bool(const String &p_key, bool p_default) const {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (settings && settings->has_setting(_setting_path(p_key))) {
		return (bool)settings->get_setting(_setting_path(p_key));
	}
	return p_default;
}

void SolersPMAIView::_build_nav() {
#ifdef MODULE_SOLERS_AI_ENABLED
	for (const char *id_cstr : SOLERS_AI_PROVIDER_ORDER) {
		const String id = id_cstr;
		const Dictionary profile = registry->get_provider_profile(id);
		if (profile.is_empty()) {
			continue;
		}
		const bool local = profile.get("local", false);
		const char *glyph = local ? SOLERS_AI_LUCIDE_MONITOR : SOLERS_AI_LUCIDE_CLOUD;
		if (id == "custom_openai_compatible") {
			glyph = SOLERS_AI_LUCIDE_SLIDERS;
		}

		SolersCategoryCard *card = memnew(SolersCategoryCard);
		card->configure(TTRGET(String(profile.get("label", id))), SolersPMTheme::lucide_icon(glyph, 16), Color());
		card->set_meta("provider_id", id);
		card->set_pressed_callback(callable_mp(this, &SolersPMAIView::_select_provider).bind(id, true));
		nav_list->add_child(card);
	}
#endif
}

void SolersPMAIView::_select_provider(const String &p_id, bool p_load_stored) {
	selected_provider = p_id;
	for (int i = 0; i < nav_list->get_child_count(); i++) {
		SolersCategoryCard *card = Object::cast_to<SolersCategoryCard>(nav_list->get_child(i));
		if (!card) {
			continue;
		}
		card->set_selected(String(card->get_meta("provider_id", String())) == p_id);
	}
	_refresh_form(p_load_stored);
	_refresh_status();
	if (saved_feedback) {
		saved_feedback->set_text(String());
	}
}

void SolersPMAIView::_refresh_form(bool p_load_stored) {
#ifdef MODULE_SOLERS_AI_ENABLED
	const Dictionary profile = registry->get_provider_profile(selected_provider);
	const String default_model = profile.get("default_model", String());
	const String default_base_url = profile.get("default_base_url", String());
	const String env_name = profile.get("api_key_env", String());
	const bool local = profile.get("local", false);

	// Labels/notes are catalog *data*; TTRGET runs them through the runtime
	// translation table so localized entries render in the user's language
	// while brand names without a translation pass through untouched.
	provider_title->set_text(TTRGET(String(profile.get("label", selected_provider))));
	provider_notes->set_text(TTRGET(String(profile.get("notes", String()))));

	// The stored model/base_url belong to the *stored* provider; for any other
	// rail selection we surface the profile defaults instead.
	const bool is_stored_provider = p_load_stored && _stored_string("provider") == selected_provider;
	model_edit->set_text(is_stored_provider ? _stored_string("model") : String());
	model_edit->set_placeholder(default_model.is_empty() ? TTR("Model id (e.g. gpt-5)") : default_model);
	base_url_edit->set_text(is_stored_provider ? _stored_string("base_url") : String());
	base_url_edit->set_placeholder(default_base_url.is_empty() ? TTR("https://your-gateway.example/v1") : default_base_url);

	// Secrets are write-only in this UI: a configured key shows as a placeholder
	// promise, never as text.
	api_key_edit->set_text(String());
	const bool key_stored = !_stored_string("api_key").is_empty() && is_stored_provider;
	if (local) {
		api_key_edit->set_placeholder(TTR("Not required for local runtimes"));
		api_key_edit->set_editable(true);
	} else if (key_stored) {
		api_key_edit->set_placeholder(TTR("Configured - leave blank to keep the current key"));
	} else {
		api_key_edit->set_placeholder(TTR("Paste your API key"));
	}

	if (env_name.is_empty()) {
		env_hint->set_text(String());
		env_hint->hide();
	} else {
		const bool env_set = OS::get_singleton()->has_environment(env_name) && !OS::get_singleton()->get_environment(env_name).is_empty();
		env_hint->set_text(vformat(env_set ? TTR("Environment fallback %s is set and will be used when no key is stored.") : TTR("Environment fallback: %s (not set)"), env_name));
		env_hint->show();
	}

	privacy_check->set_pressed_no_signal(_stored_bool("privacy_mode", true));
#endif
}

void SolersPMAIView::_add_status_row(const String &p_text, const Color &p_dot_color) {
	HBoxContainer *row = memnew(HBoxContainer);
	row->add_theme_constant_override("separation", 8 * EDSCALE);
	status_list->add_child(row);

	Label *dot = memnew(Label);
	dot->set_text(String::utf8("\u25CF")); // ●
	dot->add_theme_color_override(SceneStringName(font_color), p_dot_color);
	dot->add_theme_font_size_override(SceneStringName(font_size), MAX(8, (int)(9 * EDSCALE)));
	dot->set_v_size_flags(SIZE_SHRINK_CENTER);
	row->add_child(dot);

	Label *text = memnew(Label);
	text->set_text(p_text);
	text->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	text->set_h_size_flags(SIZE_EXPAND_FILL);
	text->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.78f));
	row->add_child(text);
}

void SolersPMAIView::_refresh_status() {
#ifdef MODULE_SOLERS_AI_ENABLED
	for (int i = status_list->get_child_count() - 1; i >= 0; i--) {
		status_list->get_child(i)->queue_free();
	}

	const Dictionary profile = registry->get_provider_profile(selected_provider);

	Dictionary config;
	config["provider"] = selected_provider;
	config["privacy_mode"] = privacy_check->is_pressed();
	const String model = model_edit->get_text().strip_edges();
	const String base_url = base_url_edit->get_text().strip_edges();
	config["model"] = model.is_empty() ? String(profile.get("default_model", String())) : model;
	config["base_url"] = base_url.is_empty() ? String(profile.get("default_base_url", String())) : base_url;
	const bool is_stored_provider = _stored_string("provider") == selected_provider;
	const bool key_pending = !api_key_edit->get_text().strip_edges().is_empty();
	const bool key_stored = is_stored_provider && !_stored_string("api_key").is_empty();
	config["api_key_configured"] = key_pending || key_stored;

	const Dictionary result = registry->validate_config(config);
	const Dictionary validation = result.get("data", Dictionary());
	const Array blockers = validation.get("blockers", Array());
	const Array warnings = validation.get("warnings", Array());

	for (int i = 0; i < blockers.size(); i++) {
		_add_status_row(String(blockers[i]), SOLERS_AI_COL_BLOCKER);
	}
	for (int i = 0; i < warnings.size(); i++) {
		_add_status_row(String(warnings[i]), SOLERS_AI_COL_WARNING);
	}
	if (blockers.is_empty() && warnings.is_empty()) {
		_add_status_row(TTR("Configuration is valid and ready to use."), SOLERS_AI_COL_OK);
	}

	save_btn->set_disabled(false);
#endif
}

void SolersPMAIView::_on_field_changed(const String &p_ignored) {
	_refresh_status();
	if (saved_feedback) {
		saved_feedback->set_text(String());
	}
}

void SolersPMAIView::_on_privacy_toggled(bool p_pressed) {
	_on_field_changed();
}

void SolersPMAIView::_on_reveal_toggled(bool p_pressed) {
	api_key_edit->set_secret(!p_pressed);
}

void SolersPMAIView::_save() {
#ifdef MODULE_SOLERS_AI_ENABLED
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings) {
		return;
	}
	const Dictionary profile = registry->get_provider_profile(selected_provider);

	settings->set_manually(_setting_path("provider"), selected_provider);
	const String model = model_edit->get_text().strip_edges();
	settings->set_manually(_setting_path("model"), model.is_empty() ? String(profile.get("default_model", String())) : model);
	settings->set_manually(_setting_path("base_url"), base_url_edit->get_text().strip_edges());
	settings->set_manually(_setting_path("privacy_mode"), privacy_check->is_pressed());

	const String new_key = api_key_edit->get_text().strip_edges();
	if (!new_key.is_empty()) {
		settings->set_manually(_setting_path("api_key"), SolersSecretStore::protect(new_key));
	} else if (_stored_string("provider") != selected_provider) {
		// Switching providers invalidates the previous provider's key.
		settings->set_manually(_setting_path("api_key"), String());
	}
	settings->mark_setting_changed("solers/ai/provider");
	settings->mark_setting_changed("solers/ai/model");
	settings->mark_setting_changed("solers/ai/base_url");
	settings->mark_setting_changed("solers/ai/privacy_mode");
	settings->emit_signal(SNAME("settings_changed"));
	settings->notify_changes();
	EditorSettings::save();

	api_key_edit->set_text(String());
	_refresh_form(true);
	_refresh_status();
	if (saved_feedback) {
		saved_feedback->set_text(TTR("Saved"));
	}
#endif
}

void SolersPMAIView::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		if (api_key_reveal) {
			api_key_reveal->set_button_icon(SolersPMTheme::lucide_icon(SOLERS_AI_LUCIDE_EYE, 15));
		}
	}
}

SolersPMAIView::SolersPMAIView() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("separation", 8 * EDSCALE);

#ifdef MODULE_SOLERS_AI_ENABLED
	registry = memnew(SolersProviderRegistry);

	// Left rail — provider catalog.
	VBoxContainer *rail = memnew(VBoxContainer);
	rail->set_custom_minimum_size(Size2(208, 0) * EDSCALE);
	rail->add_theme_constant_override("separation", 3 * EDSCALE);
	add_child(rail);

	Label *rail_header = memnew(Label(TTR("PROVIDERS")));
	rail_header->set_theme_type_variation("PMNavHeader");
	rail->add_child(rail_header);

	ScrollContainer *rail_scroll = memnew(ScrollContainer);
	rail_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	rail_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	rail->add_child(rail_scroll);

	nav_list = memnew(VBoxContainer);
	nav_list->set_h_size_flags(SIZE_EXPAND_FILL);
	nav_list->add_theme_constant_override("separation", 2 * EDSCALE);
	rail_scroll->add_child(nav_list);

	_build_nav();

	// Right pane — the configuration form on the recessed content panel.
	PanelContainer *pane = memnew(PanelContainer);
	pane->set_h_size_flags(SIZE_EXPAND_FILL);
	pane->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(pane);

	ScrollContainer *scroll = memnew(ScrollContainer);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	pane->add_child(scroll);

	MarginContainer *margin = memnew(MarginContainer);
	margin->set_h_size_flags(SIZE_EXPAND_FILL);
	margin->add_theme_constant_override("margin_left", 28 * EDSCALE);
	margin->add_theme_constant_override("margin_right", 28 * EDSCALE);
	margin->add_theme_constant_override("margin_top", 22 * EDSCALE);
	margin->add_theme_constant_override("margin_bottom", 22 * EDSCALE);
	scroll->add_child(margin);

	VBoxContainer *form = memnew(VBoxContainer);
	form->set_h_size_flags(SIZE_EXPAND_FILL);
	form->set_custom_minimum_size(Size2(420, 0) * EDSCALE);
	form->add_theme_constant_override("separation", 8 * EDSCALE);
	margin->add_child(form);

	provider_title = memnew(Label);
	provider_title->add_theme_font_size_override(SceneStringName(font_size), MAX(12, (int)(17 * EDSCALE)));
	provider_title->add_theme_color_override(SceneStringName(font_color), Color(1, 1, 1));
	form->add_child(provider_title);

	provider_notes = memnew(Label);
	provider_notes->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	provider_notes->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.5f));
	form->add_child(provider_notes);

	{
		Label *section = memnew(Label(TTR("CONNECTION")));
		section->set_theme_type_variation("PMNavHeader");
		form->add_child(section);
	}

	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(2);
	grid->add_theme_constant_override("h_separation", 16 * EDSCALE);
	grid->add_theme_constant_override("v_separation", 10 * EDSCALE);
	grid->set_h_size_flags(SIZE_EXPAND_FILL);
	form->add_child(grid);

	auto add_form_label = [&](const String &p_text) {
		Label *l = memnew(Label(p_text));
		l->set_v_size_flags(SIZE_SHRINK_CENTER);
		l->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.72f));
		grid->add_child(l);
	};

	add_form_label(TTR("Model"));
	model_edit = memnew(LineEdit);
	model_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	model_edit->set_accessibility_name(TTR("Model"));
	model_edit->connect(SceneStringName(text_changed), callable_mp(this, &SolersPMAIView::_on_field_changed));
	grid->add_child(model_edit);

	add_form_label(TTR("Base URL"));
	base_url_edit = memnew(LineEdit);
	base_url_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	base_url_edit->set_accessibility_name(TTR("Base URL"));
	base_url_edit->connect(SceneStringName(text_changed), callable_mp(this, &SolersPMAIView::_on_field_changed));
	grid->add_child(base_url_edit);

	add_form_label(TTR("API Key"));
	{
		HBoxContainer *key_row = memnew(HBoxContainer);
		key_row->set_h_size_flags(SIZE_EXPAND_FILL);
		key_row->add_theme_constant_override("separation", 6 * EDSCALE);
		grid->add_child(key_row);

		api_key_edit = memnew(LineEdit);
		api_key_edit->set_secret(true);
		api_key_edit->set_h_size_flags(SIZE_EXPAND_FILL);
		api_key_edit->set_accessibility_name(TTR("API Key"));
		api_key_edit->connect(SceneStringName(text_changed), callable_mp(this, &SolersPMAIView::_on_field_changed));
		key_row->add_child(api_key_edit);

		api_key_reveal = memnew(Button);
		api_key_reveal->set_toggle_mode(true);
		api_key_reveal->set_tooltip_text(TTR("Show the key while typing"));
		api_key_reveal->set_accessibility_name(TTR("Show API Key"));
		api_key_reveal->connect(SceneStringName(toggled), callable_mp(this, &SolersPMAIView::_on_reveal_toggled));
		key_row->add_child(api_key_reveal);
	}

	env_hint = memnew(Label);
	env_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	env_hint->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.45f));
	env_hint->add_theme_font_size_override(SceneStringName(font_size), MAX(9, (int)(11 * EDSCALE)));
	form->add_child(env_hint);

	{
		Label *section = memnew(Label(TTR("PRIVACY")));
		section->set_theme_type_variation("PMNavHeader");
		form->add_child(section);
	}

	privacy_check = memnew(CheckBox);
	privacy_check->set_text(TTR("Privacy mode (local providers only)"));
	privacy_check->connect(SceneStringName(toggled), callable_mp(this, &SolersPMAIView::_on_privacy_toggled));
	form->add_child(privacy_check);

	{
		Label *privacy_note = memnew(Label);
		privacy_note->set_text(TTR("While enabled, Solers blocks every request to remote providers at dispatch time. Only Ollama and LM Studio are allowed."));
		privacy_note->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		privacy_note->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.45f));
		privacy_note->add_theme_font_size_override(SceneStringName(font_size), MAX(9, (int)(11 * EDSCALE)));
		form->add_child(privacy_note);
	}

	{
		Label *section = memnew(Label(TTR("STATUS")));
		section->set_theme_type_variation("PMNavHeader");
		form->add_child(section);
	}

	status_list = memnew(VBoxContainer);
	status_list->add_theme_constant_override("separation", 4 * EDSCALE);
	form->add_child(status_list);

	{
		Control *gap = memnew(Control);
		gap->set_custom_minimum_size(Size2(0, 6) * EDSCALE);
		form->add_child(gap);
	}

	HBoxContainer *actions = memnew(HBoxContainer);
	actions->add_theme_constant_override("separation", 10 * EDSCALE);
	form->add_child(actions);

	save_btn = memnew(Button);
	save_btn->set_text(TTR("Save Configuration"));
	save_btn->set_theme_type_variation("PMPrimaryButton");
	save_btn->connect(SceneStringName(pressed), callable_mp(this, &SolersPMAIView::_save));
	actions->add_child(save_btn);

	saved_feedback = memnew(Label);
	saved_feedback->set_v_size_flags(SIZE_SHRINK_CENTER);
	saved_feedback->add_theme_color_override(SceneStringName(font_color), Color(SOLERS_AI_COL_OK.r, SOLERS_AI_COL_OK.g, SOLERS_AI_COL_OK.b, 0.9f));
	actions->add_child(saved_feedback);

	// Initial selection: the stored provider, falling back to local-first.
	String initial = _stored_string("provider", "ollama");
	if (registry->get_provider_profile(initial).is_empty()) {
		initial = "ollama";
	}
	_select_provider(initial, true);
#else
	Label *unavailable = memnew(Label);
	unavailable->set_text(TTR("The Solers AI module is not compiled into this build."));
	unavailable->set_h_size_flags(SIZE_EXPAND_FILL);
	unavailable->set_v_size_flags(SIZE_EXPAND_FILL);
	unavailable->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	unavailable->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	add_child(unavailable);
#endif
}

SolersPMAIView::~SolersPMAIView() {
#ifdef MODULE_SOLERS_AI_ENABLED
	if (registry) {
		memdelete(registry);
		registry = nullptr;
	}
#endif
}
