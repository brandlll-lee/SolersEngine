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
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/plugins/solers_plugin.h"
#endif

// Lucide glyph bodies (24x24 viewBox; ISC license, see solers_ai/UI_ICON_LICENSE.txt).
static const char *SOLERS_AI_LUCIDE_CLOUD = "<path d=\"M17.5 19H9a7 7 0 1 1 6.71-9h1.79a4.5 4.5 0 1 1 0 9Z\"/>";
static const char *SOLERS_AI_LUCIDE_EYE = "<path d=\"M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>";
static const char *SOLERS_AI_LUCIDE_BOX = "<path d=\"m21 8-9-5-9 5 9 5 9-5Z\"/><path d=\"M3 8v8l9 5 9-5V8\"/><path d=\"M12 13v8\"/>";

// Soft UE-toned status hues (muted, not toy-bright).
static const Color SOLERS_AI_COL_BLOCKER = Color(0.83f, 0.32f, 0.34f);
static const Color SOLERS_AI_COL_WARNING = Color(0.88f, 0.66f, 0.26f);
static const Color SOLERS_AI_COL_OK = Color(0.33f, 0.65f, 0.38f);

String SolersPMAIView::_asset_setting_path(const String &p_plugin_id, const String &p_key) const {
	return "solers/plugins/" + p_plugin_id + "/" + p_key;
}

String SolersPMAIView::_stored_asset_string(const String &p_plugin_id, const String &p_key, const String &p_default) const {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (settings && settings->has_setting(_asset_setting_path(p_plugin_id, p_key))) {
		return settings->get_setting(_asset_setting_path(p_plugin_id, p_key));
	}
	return p_default;
}

Dictionary SolersPMAIView::_asset_plugin_profile(const String &p_id) const {
#ifdef MODULE_SOLERS_AI_ENABLED
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(p_id);
	return plugin ? plugin->get_profile() : Dictionary();
#else
	return Dictionary();
#endif
}

bool SolersPMAIView::_is_asset_provider(const String &p_id) const {
	return selected_category != "llm" && !_asset_plugin_profile(p_id).is_empty();
}

bool SolersPMAIView::_uses_codex_auth(const String &p_id) const {
	return registry && String(registry->get_provider_profile(p_id).get("oauth_kind", String())) == "codex";
}

void SolersPMAIView::_build_nav() {
#ifdef MODULE_SOLERS_AI_ENABLED
	Array categories;
	for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
		const Array kinds = plugin->get_profile().get("kinds", Array());
		for (int i = 0; i < kinds.size(); i++) {
			const String kind = String(kinds[i]).to_lower();
			if (!categories.has(kind)) {
				categories.push_back(kind);
			}
		}
	}
	for (int i = 0; i < categories.size(); i++) {
		const String category = categories[i];
		SolersCategoryCard *card = memnew(SolersCategoryCard);
		card->configure(category.capitalize(), SolersPMTheme::lucide_icon(SOLERS_AI_LUCIDE_BOX, 16), Color());
		card->set_meta("category_id", category);
		card->set_pressed_callback(callable_mp(this, &SolersPMAIView::_select_category).bind(category));
		nav_list->add_child(card);
	}
	SolersCategoryCard *llm = memnew(SolersCategoryCard);
	llm->configure(TTR("LLM Provider"), SolersPMTheme::lucide_icon(SOLERS_AI_LUCIDE_CLOUD, 16), Color());
	llm->set_meta("category_id", "llm");
	llm->set_pressed_callback(callable_mp(this, &SolersPMAIView::_select_category).bind(String("llm")));
	nav_list->add_child(llm);
#endif
}

void SolersPMAIView::_select_category(const String &p_id) {
	selected_category = p_id;
	for (int i = 0; i < nav_list->get_child_count(); i++) {
		SolersCategoryCard *card = Object::cast_to<SolersCategoryCard>(nav_list->get_child(i));
		if (!card) {
			continue;
		}
		card->set_selected(String(card->get_meta("category_id", String())) == p_id);
	}
	_build_provider_list();
	_show_provider_list();
}

void SolersPMAIView::_add_provider_row(const String &p_id, const String &p_title, const Ref<Texture2D> &p_icon) {
	SolersCategoryCard *card = memnew(SolersCategoryCard);
	card->configure(p_title, p_icon, Color());
	card->set_meta("provider_id", p_id);
	card->set_pressed_callback(callable_mp(this, &SolersPMAIView::_open_provider).bind(p_id));
	provider_list->add_child(card);
}

void SolersPMAIView::_build_provider_list() {
#ifdef MODULE_SOLERS_AI_ENABLED
	for (int i = provider_list->get_child_count() - 1; i >= 0; i--) {
		Node *child = provider_list->get_child(i);
		provider_list->remove_child(child);
		child->queue_free();
	}
	const bool llm_category = selected_category == "llm";
	local_models_only_box->set_visible(llm_category);
	if (llm_category) {
		local_models_only_check->set_pressed_no_signal(settings_service->get_local_models_only());
	}

	if (!llm_category) {
		provider_list_title->set_text(selected_category.capitalize());
		provider_list_notes->set_text(TTR("Connect Solers plugins that provide this project asset kind."));
		for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
			const Dictionary profile = plugin->get_profile();
			if (!Array(profile.get("kinds", Array())).has(selected_category)) {
				continue;
			}
			const String id = profile.get("id", String());
			_add_provider_row(id, String(profile.get("label", id)), SolersPMTheme::lucide_icon(SOLERS_AI_LUCIDE_BOX, 16));
		}
		return;
	}

	provider_list_title->set_text(TTR("LLM Provider"));
	provider_list_notes->set_text(TTR("Connect the model provider Solers uses for chat and agent work."));
	const Array profiles = registry->list_provider_profiles();
	for (const Variant &profile_value : profiles) {
		const Dictionary profile = profile_value;
		const String id = profile.get("id", String());
		// The profile's models.dev catalog id doubles as the logo id; unknown
		// ids (custom gateways) fall back to the generic "synthetic" mark.
		const String catalog_id = profile.get("catalog_provider", id);
		_add_provider_row(id, TTRGET(String(profile.get("label", id))), SolersChatGlyphs::provider_logo(catalog_id, int(Math::round(16.0f * EDSCALE))));
	}
#endif
}

void SolersPMAIView::_open_provider(const String &p_id) {
	_select_provider(p_id, true);
}

void SolersPMAIView::_show_provider_list() {
	if (selected_category == "llm" && settings_service) {
		local_models_only_check->set_pressed_no_signal(settings_service->get_local_models_only());
	}
	provider_list_view->show();
	provider_detail_view->hide();
	if (saved_feedback) {
		saved_feedback->set_text(String());
	}
}

void SolersPMAIView::_select_provider(const String &p_id, bool p_load_stored) {
	selected_provider = p_id;
	for (int i = 0; i < provider_list->get_child_count(); i++) {
		SolersCategoryCard *card = Object::cast_to<SolersCategoryCard>(provider_list->get_child(i));
		if (!card) {
			continue;
		}
		card->set_selected(String(card->get_meta("provider_id", String())) == p_id);
	}
	provider_list_view->hide();
	provider_detail_view->show();
	_refresh_form(p_load_stored);
	_refresh_status();
	if (saved_feedback) {
		saved_feedback->set_text(String());
	}
}

void SolersPMAIView::_refresh_form(bool p_load_stored) {
#ifdef MODULE_SOLERS_AI_ENABLED
	connection_grid->show();
	oauth_box->hide();
	save_btn->show();
	disconnect_btn->hide();
	model_label->show();
	model_edit->show();
	base_url_label->show();
	base_url_edit->show();
	api_key_label->show();
	api_key_edit->set_editable(true);
	api_key_reveal->show();

	if (_is_asset_provider(selected_provider)) {
		const Dictionary profile = _asset_plugin_profile(selected_provider);
		const String label = profile.get("label", selected_provider);
		const String default_base_url = profile.get("base_url", String());
		const String env_name = profile.get("api_key_env", String());
		const bool requires_api_key = profile.get("requires_api_key", false);
		provider_title->set_text(vformat(TTR("Connect %s"), label));
		provider_notes->set_text(String(profile.get("description", String())));
		model_label->hide();
		model_edit->hide();
		base_url_edit->set_placeholder(default_base_url);
		if (p_load_stored) {
			base_url_edit->set_text(_stored_asset_string(selected_provider, "base_url", default_base_url));
			api_key_edit->set_text(String());
		}
		api_key_edit->set_editable(requires_api_key);
		api_key_reveal->set_visible(requires_api_key);
		const bool key_stored = !_stored_asset_string(selected_provider, "api_key").is_empty();
		api_key_edit->set_placeholder(!requires_api_key ? TTR("No API key required") : (key_stored ? TTR("Configured - leave blank to keep the current key") : TTR("Paste your API key")));
		if (env_name.is_empty()) {
			env_hint->hide();
		} else {
			const bool env_set = OS::get_singleton()->has_environment(env_name) && !OS::get_singleton()->get_environment(env_name).is_empty();
			env_hint->set_text(vformat(env_set ? TTR("Environment fallback %s is set and will be used when no key is stored.") : TTR("Environment fallback: %s (not set)"), env_name));
			env_hint->show();
		}
		return;
	}

	const Dictionary profile = registry->get_provider_profile(selected_provider);
	const Dictionary config = settings_service->get_provider_config_for(selected_provider).get("data", Dictionary());
	provider_title->set_text(TTRGET(String(profile.get("label", selected_provider))));
	provider_notes->set_text(TTRGET(String(profile.get("notes", String()))));
	if (_uses_codex_auth(selected_provider)) {
		connection_grid->hide();
		env_hint->hide();
		oauth_box->show();
		save_btn->hide();
		const Dictionary auth_status = settings_service->get_codex_auth_status(selected_provider);
		const String auth_state = auth_status.get("state", "idle");
		const bool connected = auth_status.get("connected", false);
		const bool available = auth_status.get("available", false);
		if (connected && available) {
			oauth_status->set_text(TTR("Connected with ChatGPT. Codex models are available in the Chat panel."));
		} else if (connected) {
			oauth_status->set_text(TTR("Connected with ChatGPT. Disable Local Models Only to make Codex models available."));
		} else if (auth_state == "waiting_browser") {
			oauth_status->set_text(TTR("Waiting for authorization in your browser..."));
		} else if (auth_state == "exchanging") {
			oauth_status->set_text(TTR("Finishing secure authorization..."));
		} else if (auth_state == "failed") {
			oauth_status->set_text(auth_status.get("error", TTR("Authorization failed. Try again.")));
		} else {
			oauth_status->set_text(TTR("Use your ChatGPT Plus, Pro, Business, Edu, or Enterprise plan."));
		}
		const bool active = auth_status.get("active", false);
		oauth_connect_btn->set_visible(!connected && !active);
		oauth_cancel_btn->set_visible(active);
		oauth_disconnect_btn->set_visible(connected && !active);
		set_process_internal(active);
		return;
	}

	const String default_model = profile.get("default_model", String());
	const String default_base_url = profile.get("default_base_url", String());
	const String env_name = profile.get("api_key_env", String());
	const bool local = profile.get("local", false);
	const bool configured = config.get("configured", false);
	disconnect_btn->set_visible(configured || (bool)config.get("connected", false));
	model_edit->set_text(configured && p_load_stored ? String(config.get("model", String())) : String());
	model_edit->set_placeholder(default_model.is_empty() ? TTR("Model id (e.g. gpt-5)") : default_model);
	base_url_edit->set_text(configured && p_load_stored ? String(config.get("base_url", String())) : String());
	base_url_edit->set_placeholder(default_base_url.is_empty() ? TTR("https://your-gateway.example/v1") : default_base_url);
	api_key_edit->set_text(String());
	if (local) {
		api_key_edit->set_placeholder(TTR("Not required for local runtimes"));
	} else if (config.get("api_key_configured", false)) {
		api_key_edit->set_placeholder(TTR("Configured - leave blank to keep the current key"));
	} else {
		api_key_edit->set_placeholder(TTR("Paste your API key"));
	}
	if (env_name.is_empty()) {
		env_hint->hide();
	} else {
		const bool env_set = !env_name.is_empty() && OS::get_singleton()->has_environment(env_name) && !OS::get_singleton()->get_environment(env_name).is_empty();
		env_hint->set_text(vformat(env_set ? TTR("Environment fallback %s is set and will be used when no key is stored.") : TTR("Environment fallback: %s (not set)"), env_name));
		env_hint->show();
	}
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

	if (_is_asset_provider(selected_provider)) {
		const Dictionary profile = _asset_plugin_profile(selected_provider);
		const String env_name = profile.get("api_key_env", String());
		const bool requires_api_key = profile.get("requires_api_key", false);
		const bool key_pending = !api_key_edit->get_text().strip_edges().is_empty();
		const bool key_stored = !_stored_asset_string(selected_provider, "api_key").is_empty();
		const bool env_set = !env_name.is_empty() && OS::get_singleton()->has_environment(env_name) && !OS::get_singleton()->get_environment(env_name).is_empty();
		if (!requires_api_key || key_pending || key_stored || env_set) {
			_add_status_row(TTR("Configuration is valid and ready to use."), SOLERS_AI_COL_OK);
		} else {
			_add_status_row(TTR("API key is missing."), SOLERS_AI_COL_BLOCKER);
		}
		save_btn->set_disabled(false);
		return;
	}

	if (_uses_codex_auth(selected_provider)) {
		const Dictionary auth_status = settings_service->get_codex_auth_status(selected_provider);
		if (auth_status.get("available", false)) {
			_add_status_row(TTR("ChatGPT Codex is connected."), SOLERS_AI_COL_OK);
		} else if (auth_status.get("connected", false)) {
			_add_status_row(TTR("Local Models Only currently blocks this remote provider."), SOLERS_AI_COL_WARNING);
		} else if (auth_status.get("active", false)) {
			_add_status_row(TTR("Authorization is in progress."), SOLERS_AI_COL_WARNING);
		} else {
			_add_status_row(String(auth_status.get("error", TTR("Sign in with ChatGPT to connect Codex."))), auth_status.has("error") ? SOLERS_AI_COL_BLOCKER : SOLERS_AI_COL_WARNING);
		}
		return;
	}

	const Dictionary profile = registry->get_provider_profile(selected_provider);

	Dictionary config;
	config["provider"] = selected_provider;
	const String model = model_edit->get_text().strip_edges();
	const String base_url = base_url_edit->get_text().strip_edges();
	config["model"] = model.is_empty() ? String(profile.get("default_model", String())) : model;
	config["base_url"] = base_url.is_empty() ? String(profile.get("default_base_url", String())) : base_url;
	const Dictionary stored = settings_service->get_provider_config_for(selected_provider).get("data", Dictionary());
	const bool key_pending = !api_key_edit->get_text().strip_edges().is_empty();
	config["api_key_configured"] = key_pending || (bool)stored.get("api_key_configured", false);

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
	if (stored.get("connected", false) && !stored.get("available", false)) {
		_add_status_row(TTR("Local Models Only currently blocks this remote provider."), SOLERS_AI_COL_WARNING);
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

void SolersPMAIView::_on_local_models_only_toggled(bool p_pressed) {
	if (settings_service) {
		settings_service->set_local_models_only(p_pressed);
	}
}

void SolersPMAIView::_on_reveal_toggled(bool p_pressed) {
	api_key_edit->set_secret(!p_pressed);
}

void SolersPMAIView::_on_codex_connect() {
	if (!settings_service) {
		return;
	}
	const Dictionary result = settings_service->start_codex_login(selected_provider);
	if (!result.get("ok", false)) {
		oauth_status->set_text(Dictionary(result.get("error", Dictionary())).get("message", TTR("Could not start authorization.")));
	}
	_refresh_form(false);
	_refresh_status();
}

void SolersPMAIView::_on_codex_cancel() {
	if (settings_service) {
		settings_service->cancel_codex_login();
	}
	set_process_internal(false);
	_refresh_form(false);
	_refresh_status();
}

void SolersPMAIView::_on_codex_disconnect() {
	if (settings_service) {
		settings_service->disconnect_codex(selected_provider);
	}
	_refresh_form(false);
	_refresh_status();
}

void SolersPMAIView::_save() {
#ifdef MODULE_SOLERS_AI_ENABLED
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings) {
		return;
	}
	if (_is_asset_provider(selected_provider)) {
		const Dictionary profile = _asset_plugin_profile(selected_provider);
		const String default_base_url = profile.get("base_url", String());
		const String base_url = base_url_edit->get_text().strip_edges();
		settings->set_manually(_asset_setting_path(selected_provider, "base_url"), base_url.is_empty() ? default_base_url : base_url);
		const String new_key = api_key_edit->get_text().strip_edges();
		if (!new_key.is_empty()) {
			settings->set_manually(_asset_setting_path(selected_provider, "api_key"), SolersSecretStore::protect(new_key));
		}
		if ((bool)profile.get("supports_generation", false)) {
			const Array kinds = profile.get("kinds", Array());
			for (int i = 0; i < kinds.size(); i++) {
				const String path = "solers/plugins/default/" + String(kinds[i]).to_lower();
				settings->set_manually(path, selected_provider);
				settings->mark_setting_changed(path);
			}
		}
		settings->mark_setting_changed(_asset_setting_path(selected_provider, "base_url"));
		settings->mark_setting_changed(_asset_setting_path(selected_provider, "api_key"));
		settings->emit_signal(SNAME("settings_changed"));
		settings->notify_changes();
		EditorSettings::save();
		api_key_edit->set_text(String());
		_refresh_form(true);
		_refresh_status();
		if (saved_feedback) {
			saved_feedback->set_text(TTR("Saved"));
		}
		return;
	}
	if (_uses_codex_auth(selected_provider) || !settings_service) {
		return;
	}
	const Dictionary profile = registry->get_provider_profile(selected_provider);
	Dictionary config;
	config["provider"] = selected_provider;
	const String model = model_edit->get_text().strip_edges();
	config["model"] = model.is_empty() ? String(profile.get("default_model", String())) : model;
	config["base_url"] = base_url_edit->get_text().strip_edges();
	const String new_key = api_key_edit->get_text().strip_edges();
	if (!new_key.is_empty()) {
		config["api_key"] = new_key;
	}
	settings_service->set_provider_config(config);
	api_key_edit->set_text(String());
	_refresh_form(true);
	_refresh_status();
	if (saved_feedback) {
		saved_feedback->set_text(TTR("Saved"));
	}
#endif
}

void SolersPMAIView::_disconnect_selected_provider() {
	if (!settings_service || _is_asset_provider(selected_provider) || _uses_codex_auth(selected_provider)) {
		return;
	}
	settings_service->disconnect_provider(selected_provider);
	_refresh_form(false);
	_refresh_status();
	if (saved_feedback) {
		saved_feedback->set_text(TTR("Disconnected"));
	}
}

void SolersPMAIView::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		if (api_key_reveal) {
			api_key_reveal->set_button_icon(SolersPMTheme::lucide_icon(SOLERS_AI_LUCIDE_EYE, 15));
		}
	} else if (p_what == NOTIFICATION_INTERNAL_PROCESS && settings_service) {
		settings_service->poll_auth();
		set_process_internal(settings_service->is_auth_active());
		if (_uses_codex_auth(selected_provider)) {
			_refresh_form(false);
			_refresh_status();
		}
	}
}

void SolersPMAIView::refresh() {
#ifdef MODULE_SOLERS_AI_ENABLED
	_build_provider_list();
	if (provider_detail_view->is_visible()) {
		_refresh_form(true);
		_refresh_status();
	}
#endif
}

SolersPMAIView::SolersPMAIView() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("separation", 8 * EDSCALE);

#ifdef MODULE_SOLERS_AI_ENABLED
	registry = memnew(SolersProviderRegistry);
	settings_service = memnew(SolersSettingsService);
	settings_service->set_provider_registry(registry);

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

	VBoxContainer *content = memnew(VBoxContainer);
	content->set_h_size_flags(SIZE_EXPAND_FILL);
	content->set_custom_minimum_size(Size2(420, 0) * EDSCALE);
	content->add_theme_constant_override("separation", 10 * EDSCALE);
	margin->add_child(content);

	provider_list_view = memnew(VBoxContainer);
	provider_list_view->set_h_size_flags(SIZE_EXPAND_FILL);
	provider_list_view->set_v_size_flags(SIZE_EXPAND_FILL);
	provider_list_view->add_theme_constant_override("separation", 10 * EDSCALE);
	content->add_child(provider_list_view);

	provider_list_title = memnew(Label);
	provider_list_title->add_theme_font_size_override(SceneStringName(font_size), MAX(12, (int)(17 * EDSCALE)));
	provider_list_title->add_theme_color_override(SceneStringName(font_color), Color(1, 1, 1));
	provider_list_view->add_child(provider_list_title);

	provider_list_notes = memnew(Label);
	provider_list_notes->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	provider_list_notes->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.5f));
	provider_list_view->add_child(provider_list_notes);

	local_models_only_box = memnew(VBoxContainer);
	local_models_only_box->add_theme_constant_override("separation", 4 * EDSCALE);
	provider_list_view->add_child(local_models_only_box);

	Label *local_models_only_header = memnew(Label(TTR("MODEL ACCESS")));
	local_models_only_header->set_theme_type_variation("PMNavHeader");
	local_models_only_box->add_child(local_models_only_header);

	local_models_only_check = memnew(CheckBox(TTR("Local Models Only")));
	local_models_only_check->set_tooltip_text(TTR("Pause remote model providers without disconnecting accounts or deleting credentials."));
	local_models_only_check->connect(SceneStringName(toggled), callable_mp(this, &SolersPMAIView::_on_local_models_only_toggled));
	local_models_only_box->add_child(local_models_only_check);

	Label *local_models_only_note = memnew(Label(TTR("When enabled, Solers can use connected local model providers only. Remote connections remain configured.")));
	local_models_only_note->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	local_models_only_note->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.45f));
	local_models_only_note->add_theme_font_size_override(SceneStringName(font_size), MAX(9, (int)(11 * EDSCALE)));
	local_models_only_box->add_child(local_models_only_note);

	provider_list = memnew(VBoxContainer);
	provider_list->set_h_size_flags(SIZE_EXPAND_FILL);
	provider_list->add_theme_constant_override("separation", 3 * EDSCALE);
	provider_list_view->add_child(provider_list);

	provider_detail_view = memnew(VBoxContainer);
	provider_detail_view->set_h_size_flags(SIZE_EXPAND_FILL);
	provider_detail_view->add_theme_constant_override("separation", 8 * EDSCALE);
	content->add_child(provider_detail_view);

	HBoxContainer *detail_top = memnew(HBoxContainer);
	detail_top->add_theme_constant_override("separation", 10 * EDSCALE);
	provider_detail_view->add_child(detail_top);

	detail_back_btn = memnew(Button);
	detail_back_btn->set_flat(true);
	detail_back_btn->set_text("<");
	detail_back_btn->set_accessibility_name(TTR("Back to providers"));
	detail_back_btn->connect(SceneStringName(pressed), callable_mp(this, &SolersPMAIView::_show_provider_list));
	detail_top->add_child(detail_back_btn);

	VBoxContainer *form = provider_detail_view;

	provider_title = memnew(Label);
	provider_title->add_theme_font_size_override(SceneStringName(font_size), MAX(12, (int)(17 * EDSCALE)));
	provider_title->add_theme_color_override(SceneStringName(font_color), Color(1, 1, 1));
	detail_top->add_child(provider_title);

	provider_notes = memnew(Label);
	provider_notes->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	provider_notes->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.5f));
	form->add_child(provider_notes);

	{
		Label *section = memnew(Label(TTR("CONNECTION")));
		section->set_theme_type_variation("PMNavHeader");
		form->add_child(section);
	}

	connection_grid = memnew(GridContainer);
	connection_grid->set_columns(2);
	connection_grid->add_theme_constant_override("h_separation", 16 * EDSCALE);
	connection_grid->add_theme_constant_override("v_separation", 10 * EDSCALE);
	connection_grid->set_h_size_flags(SIZE_EXPAND_FILL);
	form->add_child(connection_grid);

	auto add_form_label = [&](const String &p_text) -> Label * {
		Label *label = memnew(Label(p_text));
		label->set_v_size_flags(SIZE_SHRINK_CENTER);
		label->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.72f));
		connection_grid->add_child(label);
		return label;
	};

	model_label = add_form_label(TTR("Model"));
	model_edit = memnew(LineEdit);
	model_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	model_edit->set_accessibility_name(TTR("Model"));
	model_edit->connect(SceneStringName(text_changed), callable_mp(this, &SolersPMAIView::_on_field_changed));
	connection_grid->add_child(model_edit);

	base_url_label = add_form_label(TTR("Base URL"));
	base_url_edit = memnew(LineEdit);
	base_url_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	base_url_edit->set_accessibility_name(TTR("Base URL"));
	base_url_edit->connect(SceneStringName(text_changed), callable_mp(this, &SolersPMAIView::_on_field_changed));
	connection_grid->add_child(base_url_edit);

	api_key_label = add_form_label(TTR("API Key"));
	{
		HBoxContainer *key_row = memnew(HBoxContainer);
		key_row->set_h_size_flags(SIZE_EXPAND_FILL);
		key_row->add_theme_constant_override("separation", 6 * EDSCALE);
		connection_grid->add_child(key_row);

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

	oauth_box = memnew(VBoxContainer);
	oauth_box->add_theme_constant_override("separation", 10 * EDSCALE);
	oauth_box->hide();
	form->add_child(oauth_box);
	oauth_status = memnew(Label);
	oauth_status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	oauth_status->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.72f));
	oauth_box->add_child(oauth_status);
	HBoxContainer *oauth_actions = memnew(HBoxContainer);
	oauth_actions->add_theme_constant_override("separation", 8 * EDSCALE);
	oauth_box->add_child(oauth_actions);
	oauth_connect_btn = memnew(Button(TTR("Sign in with ChatGPT")));
	oauth_connect_btn->set_theme_type_variation("PMPrimaryButton");
	oauth_connect_btn->connect(SceneStringName(pressed), callable_mp(this, &SolersPMAIView::_on_codex_connect));
	oauth_actions->add_child(oauth_connect_btn);
	oauth_cancel_btn = memnew(Button(TTR("Cancel")));
	oauth_cancel_btn->connect(SceneStringName(pressed), callable_mp(this, &SolersPMAIView::_on_codex_cancel));
	oauth_actions->add_child(oauth_cancel_btn);
	oauth_disconnect_btn = memnew(Button(TTR("Disconnect")));
	oauth_disconnect_btn->connect(SceneStringName(pressed), callable_mp(this, &SolersPMAIView::_on_codex_disconnect));
	oauth_actions->add_child(oauth_disconnect_btn);

	env_hint = memnew(Label);
	env_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	env_hint->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.45f));
	env_hint->add_theme_font_size_override(SceneStringName(font_size), MAX(9, (int)(11 * EDSCALE)));
	form->add_child(env_hint);

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

	disconnect_btn = memnew(Button(TTR("Disconnect")));
	disconnect_btn->connect(SceneStringName(pressed), callable_mp(this, &SolersPMAIView::_disconnect_selected_provider));
	actions->add_child(disconnect_btn);

	saved_feedback = memnew(Label);
	saved_feedback->set_v_size_flags(SIZE_SHRINK_CENTER);
	saved_feedback->add_theme_color_override(SceneStringName(font_color), Color(SOLERS_AI_COL_OK.r, SOLERS_AI_COL_OK.g, SOLERS_AI_COL_OK.b, 0.9f));
	actions->add_child(saved_feedback);

	_select_category("llm");
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
	if (settings_service) {
		memdelete(settings_service);
		settings_service = nullptr;
	}
	if (registry) {
		memdelete(registry);
		registry = nullptr;
	}
#endif
}
