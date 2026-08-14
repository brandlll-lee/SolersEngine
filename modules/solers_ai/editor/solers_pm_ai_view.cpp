/**************************************************************************/
/*  solers_pm_ai_view.cpp                                                 */
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

#include "solers_pm_ai_view.h"

#include "solers_pm_cards.h"
#include "solers_ui_theme.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/string/translation_server.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_string_names.h"
#include "editor/inspector/editor_properties.h"
#include "editor/settings/editor_settings.h"
#include "editor/settings/editor_settings_dialog.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/style_box_flat.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_SOLERS_AI_ENABLED
#include "modules/solers_ai/core/solers_provider_registry.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/plugins/solers_plugin.h"
#endif

// Soft UE-toned status hues (muted, not toy-bright).
static const Color SOLERS_AI_COL_BLOCKER = Color(0.83f, 0.32f, 0.34f);
static const Color SOLERS_AI_COL_WARNING = Color(0.88f, 0.66f, 0.26f);
static const Color SOLERS_AI_COL_OK = Color(0.33f, 0.65f, 0.38f);

static String _solers_plugin_kinds_subtitle(const Dictionary &p_profile) {
	const Array kinds = p_profile.get("kinds", Array());
	PackedStringArray parts;
	for (int i = 0; i < kinds.size(); i++) {
		const String kind = String(kinds[i]).strip_edges();
		if (!kind.is_empty()) {
			parts.push_back(kind);
		}
	}
	return String(", ").join(parts);
}

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
	return !_asset_plugin_profile(p_id).is_empty();
}

bool SolersPMAIView::_uses_codex_auth(const String &p_id) const {
	return registry && String(registry->get_provider_profile(p_id).get("oauth_kind", String())) == "codex";
}

Dictionary SolersPMAIView::_provider_status(const String &p_id, bool p_live_form) const {
	Dictionary out;
	out["color"] = SOLERS_AI_COL_WARNING;
	out["text"] = TTR("Status unavailable.");
#ifdef MODULE_SOLERS_AI_ENABLED
	if (_is_asset_provider(p_id)) {
		const Dictionary profile = _asset_plugin_profile(p_id);
		const String env_name = profile.get("api_key_env", String());
		const bool requires_api_key = profile.get("requires_api_key", false);
		const bool key_pending = p_live_form && api_key_edit && !api_key_edit->get_text().strip_edges().is_empty();
		const bool key_stored = !_stored_asset_string(p_id, "api_key").is_empty();
		const bool env_set = !env_name.is_empty() && OS::get_singleton()->has_environment(env_name) && !OS::get_singleton()->get_environment(env_name).is_empty();
		const bool ok = !requires_api_key || key_pending || key_stored || env_set;
		out["color"] = ok ? SOLERS_AI_COL_OK : SOLERS_AI_COL_BLOCKER;
		out["text"] = ok ? TTR("Configuration is valid and ready to use.") : TTR("API key is missing.");
		return out;
	}
	if (!settings_service || !registry) {
		return out;
	}
	if (_uses_codex_auth(p_id)) {
		const Dictionary auth = settings_service->get_codex_auth_status(p_id);
		if (auth.get("available", false)) {
			out["color"] = SOLERS_AI_COL_OK;
			out["text"] = TTR("ChatGPT Codex is connected.");
		} else if (auth.get("connected", false)) {
			out["color"] = SOLERS_AI_COL_WARNING;
			out["text"] = TTR("Local Models Only currently blocks this remote provider.");
		} else if (auth.get("active", false)) {
			out["color"] = SOLERS_AI_COL_WARNING;
			out["text"] = TTR("Authorization is in progress.");
		} else {
			out["color"] = auth.has("error") ? SOLERS_AI_COL_BLOCKER : SOLERS_AI_COL_WARNING;
			out["text"] = String(auth.get("error", TTR("Sign in with ChatGPT to connect Codex.")));
		}
		return out;
	}

	const Dictionary profile = registry->get_provider_profile(p_id);
	const Dictionary stored = settings_service->get_provider_config_for(p_id).get("data", Dictionary());
	Dictionary config;
	config["provider"] = p_id;
	String model = String(stored.get("model", String())).strip_edges();
	String base_url = String(stored.get("base_url", String())).strip_edges();
	bool key_configured = stored.get("api_key_configured", false);
	if (p_live_form && model_edit && base_url_edit && api_key_edit) {
		const String live_model = model_edit->get_text().strip_edges();
		const String live_url = base_url_edit->get_text().strip_edges();
		if (!live_model.is_empty()) {
			model = live_model;
		}
		if (!live_url.is_empty()) {
			base_url = live_url;
		}
		key_configured = key_configured || !api_key_edit->get_text().strip_edges().is_empty();
	}
	config["model"] = model.is_empty() ? String(profile.get("default_model", String())) : model;
	config["base_url"] = base_url.is_empty() ? String(profile.get("default_base_url", String())) : base_url;
	config["api_key_configured"] = key_configured;

	const Dictionary validation = registry->validate_config(config).get("data", Dictionary());
	const Array blockers = validation.get("blockers", Array());
	const Array warnings = validation.get("warnings", Array());
	if (!blockers.is_empty()) {
		out["color"] = SOLERS_AI_COL_BLOCKER;
		out["text"] = String(blockers[0]);
		return out;
	}
	if (!warnings.is_empty()) {
		out["color"] = SOLERS_AI_COL_WARNING;
		out["text"] = String(warnings[0]);
		return out;
	}
	if (stored.get("connected", false) && !stored.get("available", false)) {
		out["color"] = SOLERS_AI_COL_WARNING;
		out["text"] = TTR("Local Models Only currently blocks this remote provider.");
		return out;
	}
	out["color"] = SOLERS_AI_COL_OK;
	out["text"] = TTR("Configuration is valid and ready to use.");
#endif
	return out;
}

void SolersPMAIView::_build_nav() {
#ifdef MODULE_SOLERS_AI_ENABLED
	SolersCategoryCard *plugins = memnew(SolersCategoryCard);
	plugins->configure(TTR("Plugins"), SolersIcons::get(SNAME("plugin"), int(Math::round(16.0f * EDSCALE))));
	plugins->set_meta("category_id", "plugins");
	plugins->set_pressed_callback(callable_mp(this, &SolersPMAIView::_select_category).bind(String("plugins")));
	nav_list->add_child(plugins);

	SolersCategoryCard *llm = memnew(SolersCategoryCard);
	llm->configure(TTR("LLM Provider"), SolersIcons::get(SNAME("cloud"), int(Math::round(16.0f * EDSCALE))));
	llm->set_meta("category_id", "llm");
	llm->set_pressed_callback(callable_mp(this, &SolersPMAIView::_select_category).bind(String("llm")));
	nav_list->add_child(llm);

	SolersCategoryCard *quick = memnew(SolersCategoryCard);
	quick->configure(TTR("Quick Settings"), SolersIcons::get(SNAME("adjustments"), int(Math::round(16.0f * EDSCALE))));
	quick->set_meta("category_id", "quick");
	quick->set_pressed_callback(callable_mp(this, &SolersPMAIView::_select_category).bind(String("quick")));
	nav_list->add_child(quick);
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
	const bool quick = p_id == "quick";
	if (quick_settings_view) {
		quick_settings_view->set_visible(quick);
	}
	if (quick) {
		provider_list_view->hide();
		provider_detail_view->hide();
		_update_quick_settings_values();
		return;
	}
	_build_provider_list();
	_show_provider_list();
}

void SolersPMAIView::_add_provider_row(const String &p_id, const String &p_title, const Ref<Texture2D> &p_icon, bool p_preserve_icon_color, const String &p_subtitle) {
	SolersCategoryCard *card = memnew(SolersCategoryCard);
	// Plugins keep readiness dots; LLM rows use subtitle source tags (OpenCode/Kilo).
	const Color dot = selected_category == "llm" ? Color(0, 0, 0, 0) : Color(_provider_status(p_id).get("color", Color()));
	card->configure(p_title, p_icon, p_subtitle, dot);
	card->set_preserve_icon_color(p_preserve_icon_color);
	card->set_meta("provider_id", p_id);
	card->set_pressed_callback(callable_mp(this, &SolersPMAIView::_select_provider).bind(p_id, true));
	provider_list->add_child(card);
}

String SolersPMAIView::_source_label(const String &p_source) const {
	if (p_source == "env") {
		return TTR("Environment");
	}
	if (p_source == "oauth") {
		return TTR("OAuth");
	}
	if (p_source == "custom") {
		return TTR("Custom");
	}
	if (p_source == "config") {
		return TTR("Config");
	}
	if (p_source == "api") {
		return TTR("API key");
	}
	return String();
}

void SolersPMAIView::_add_section_label(const String &p_text) {
	Label *label = memnew(Label);
	label->set_text(p_text);
	label->set_theme_type_variation(SNAME("SolersSessionMeta"));
	label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	provider_list->add_child(label);
}

void SolersPMAIView::_add_llm_row(const String &p_id, const String &p_title, const String &p_subtitle, bool p_connect_action) {
#ifdef MODULE_SOLERS_AI_ENABLED
	const Dictionary profile = registry ? registry->get_provider_profile(p_id) : Dictionary();
	const String catalog_id = profile.get("catalog_provider", p_id);
	String subtitle = p_subtitle;
	if (p_connect_action && subtitle.is_empty()) {
		subtitle = TTR("Connect");
	}
	_add_provider_row(p_id, p_title, SolersIcons::provider_logo(catalog_id, int(Math::round(16.0f * EDSCALE))), false, subtitle);
#else
	(void)p_id;
	(void)p_title;
	(void)p_subtitle;
	(void)p_connect_action;
#endif
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
		local_models_only_check->set_pressed_no_signal(settings_service && settings_service->get_local_models_only());
	}

	if (!llm_category) {
		provider_list_title->set_text(TTR("Plugins"));
		provider_list_notes->set_text(TTR("Connect Solers plugins for project assets — generation, libraries, and tooling."));
		for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
			const Dictionary profile = plugin->get_profile();
			const String id = profile.get("id", String());
			if (id.is_empty()) {
				continue;
			}
			const int logo_px = int(Math::round(16.0f * EDSCALE));
			const Ref<Texture2D> color_logo = SolersIcons::provider_logo_color(id, logo_px);
			_add_provider_row(id, String(profile.get("label", id)),
					color_logo.is_valid() ? color_logo : SolersIcons::provider_logo(id, logo_px),
					color_logo.is_valid(),
					_solers_plugin_kinds_subtitle(profile));
		}
		return;
	}

	provider_list_title->set_text(TTR("LLM Provider"));
	provider_list_notes->set_text(TTR("Connect the model provider Solers uses for chat and agent work."));
	if (!settings_service || !registry) {
		return;
	}

	const Dictionary view = settings_service->list_provider_view().get("data", Dictionary());
	const Array connected = view.get("connected", Array());
	const Array popular = view.get("popular", Array());
	const Dictionary custom = view.get("custom", Dictionary());

	if (!connected.is_empty()) {
		_add_section_label(TTR("Connected"));
		for (const Variant &row_v : connected) {
			const Dictionary row = row_v;
			const String id = row.get("provider", String());
			const Dictionary profile = row.get("profile", registry->get_provider_profile(id));
			String subtitle = _source_label(row.get("source", String()));
			if (row.get("connected", false) && !row.get("available", true)) {
				subtitle = subtitle.is_empty() ? TTR("Local Models Only") : subtitle + " · " + TTR("Local Models Only");
			}
			_add_llm_row(id, TTRGET(String(profile.get("label", id))), subtitle, false);
		}
	}

	_add_section_label(TTR("Popular"));
	for (const Variant &row_v : popular) {
		const Dictionary row = row_v;
		const String id = row.get("provider", String());
		if (id == "custom_openai_compatible") {
			continue;
		}
		const Dictionary profile = row.get("profile", registry->get_provider_profile(id));
		_add_llm_row(id, TTRGET(String(profile.get("label", id))), String(), true);
	}

	const String custom_id = custom.get("provider", "custom_openai_compatible");
	if (!custom.get("connected", false)) {
		_add_llm_row(custom_id, TTR("Custom provider"), TTR("Connect"), true);
	}

	SolersCategoryCard *view_all = memnew(SolersCategoryCard);
	view_all->configure(TTR("View all providers"), SolersIcons::provider_logo("synthetic", int(Math::round(16.0f * EDSCALE))), TTR("Browse catalog"));
	view_all->set_meta("provider_id", String("__view_all__"));
	view_all->set_pressed_callback(callable_mp(this, &SolersPMAIView::_open_view_all));
	provider_list->add_child(view_all);
#endif
}

void SolersPMAIView::_show_provider_list() {
	if (quick_settings_view) {
		quick_settings_view->hide();
	}
	if (selected_category == "llm" && settings_service) {
		local_models_only_check->set_pressed_no_signal(settings_service->get_local_models_only());
	}
	_build_provider_list();
	provider_list_view->show();
	provider_detail_view->hide();
	if (saved_feedback) {
		saved_feedback->set_text(String());
	}
}

void SolersPMAIView::_select_provider(const String &p_id, bool p_load_stored) {
	if (p_id == "__view_all__") {
		_open_view_all();
		return;
	}
	if (view_all_dialog && view_all_dialog->is_visible()) {
		view_all_dialog->hide();
	}
	if (quick_settings_view) {
		quick_settings_view->hide();
	}
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

void SolersPMAIView::_open_view_all() {
#ifdef MODULE_SOLERS_AI_ENABLED
	if (!view_all_dialog) {
		view_all_dialog = memnew(AcceptDialog);
		view_all_dialog->set_title(TTR("All providers"));
		view_all_dialog->set_min_size(Size2(420, 480) * EDSCALE);
		add_child(view_all_dialog);

		VBoxContainer *box = memnew(VBoxContainer);
		box->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		box->add_theme_constant_override("separation", 8 * EDSCALE);
		view_all_dialog->add_child(box);

		view_all_search = memnew(LineEdit);
		view_all_search->set_placeholder(TTR("Search providers..."));
		view_all_search->set_clear_button_enabled(true);
#ifdef MODULE_SOLERS_AI_ENABLED
		solers_style_bare_search_line_edit(view_all_search);
#endif
		view_all_search->connect(SceneStringName(text_changed), callable_mp(this, &SolersPMAIView::_on_view_all_search));
		box->add_child(view_all_search);

		ScrollContainer *scroll = memnew(ScrollContainer);
		scroll->set_v_size_flags(SIZE_EXPAND_FILL);
		scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
		box->add_child(scroll);

		view_all_list = memnew(VBoxContainer);
		view_all_list->set_h_size_flags(SIZE_EXPAND_FILL);
		view_all_list->add_theme_constant_override("separation", 2 * EDSCALE);
		scroll->add_child(view_all_list);
	}
	if (view_all_search) {
		view_all_search->set_text(String());
	}
	_rebuild_view_all_list();
	view_all_dialog->popup_centered(Size2(420, 480) * EDSCALE);
#endif
}

void SolersPMAIView::_on_view_all_search(const String &p_text) {
	_rebuild_view_all_list(p_text);
}

void SolersPMAIView::_rebuild_view_all_list(const String &p_filter) {
#ifdef MODULE_SOLERS_AI_ENABLED
	if (!view_all_list || !registry) {
		return;
	}
	for (int i = view_all_list->get_child_count() - 1; i >= 0; i--) {
		Node *child = view_all_list->get_child(i);
		view_all_list->remove_child(child);
		child->queue_free();
	}
	const String filter = p_filter.strip_edges().to_lower();
	const Array profiles = registry->list_provider_profiles();
	for (const Variant &profile_value : profiles) {
		const Dictionary profile = profile_value;
		const String id = profile.get("id", String());
		const String label = String(profile.get("label", id));
		if (id == "custom_openai_compatible") {
			continue;
		}
		if (!filter.is_empty() && !id.to_lower().contains(filter) && !label.to_lower().contains(filter)) {
			continue;
		}
		SolersCategoryCard *card = memnew(SolersCategoryCard);
		const String catalog_id = profile.get("catalog_provider", id);
		card->configure(TTRGET(label), SolersIcons::provider_logo(catalog_id, int(Math::round(16.0f * EDSCALE))), TTR("Connect"));
		card->set_pressed_callback(callable_mp(this, &SolersPMAIView::_select_provider).bind(id, true));
		view_all_list->add_child(card);
	}
#else
	(void)p_filter;
#endif
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
	context_window_edit->set_value(configured && p_load_stored ? (int)config.get("context_window", 0) : 0);
	max_tokens_edit->set_value(configured && p_load_stored ? (int)config.get("max_tokens", 0) : 0);
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
	if (selected_provider.is_empty()) {
		return;
	}
	const Dictionary st = _provider_status(selected_provider, true);
	_add_status_row(st.get("text", String()), Color(st.get("color", SOLERS_AI_COL_WARNING)));
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
	if (selected_category == "llm") {
		_build_provider_list();
	}
	_refresh_status();
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
	config["context_window"] = (int)context_window_edit->get_value();
	config["max_tokens"] = (int)max_tokens_edit->get_value();
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

void SolersPMAIView::_fetch_quick_setting_values() {
#ifndef ANDROID_ENABLED
	editor_languages.clear();
#endif
	editor_styles.clear();
	editor_themes.clear();
	editor_scales.clear();
	editor_network_modes.clear();
	editor_check_for_updates.clear();
	editor_directory_naming_conventions.clear();

	List<PropertyInfo> editor_settings_properties;
	EditorSettings::get_singleton()->get_property_list(&editor_settings_properties);
	for (const PropertyInfo &pi : editor_settings_properties) {
		if (pi.name == "interface/editor/editor_language") {
#ifndef ANDROID_ENABLED
			editor_languages = pi.hint_string.split(";", false);
#endif
		} else if (pi.name == "interface/theme/style") {
			editor_styles = pi.hint_string.split(",");
		} else if (pi.name == "interface/theme/color_preset") {
			editor_themes = pi.hint_string.split(",");
		} else if (pi.name == "interface/editor/display_scale") {
			editor_scales = pi.hint_string.split(",");
		} else if (pi.name == "network/connection/network_mode") {
			editor_network_modes = pi.hint_string.split(",");
		} else if (pi.name == "network/connection/check_for_updates") {
			editor_check_for_updates = pi.hint_string.split(",");
		} else if (pi.name == "project_manager/directory_naming_convention") {
			editor_directory_naming_conventions = pi.hint_string.split(",");
		}
	}
}

void SolersPMAIView::_update_quick_settings_values() {
	if (!quick_settings_list) {
		return;
	}
#ifndef ANDROID_ENABLED
	{
		const String current_lang = EDITOR_GET("interface/editor/editor_language");
		for (int i = 0; i < editor_languages.size(); i++) {
			if (current_lang == editor_languages[i].get_slicec('/', 0)) {
				language_option_button->set_text(editor_languages[i].get_slicec('/', 1));
				language_option_button->select(i);
				break;
			}
		}
	}
#endif
	{
		const String current_style = EDITOR_GET("interface/theme/style");
		for (int i = 0; i < editor_styles.size(); i++) {
			if (current_style == editor_styles[i]) {
				style_option_button->select(i);
			}
		}
	}
	{
		const String current_theme = EDITOR_GET("interface/theme/color_preset");
		for (int i = 0; i < editor_themes.size(); i++) {
			if (current_theme == editor_themes[i]) {
				theme_option_button->set_text(current_theme);
				theme_option_button->select(i);
				theme_option_button->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
				custom_theme_label->set_visible(current_theme == "Custom");
			}
		}
	}
	{
		const int current_scale = EDITOR_GET("interface/editor/display_scale");
		for (int i = 0; i < editor_scales.size(); i++) {
			if (current_scale == i) {
				scale_option_button->set_text(editor_scales[i]);
				scale_option_button->select(i);
			}
		}
	}
	{
		const int current_network_mode = EDITOR_GET("network/connection/network_mode");
		for (int i = 0; i < editor_network_modes.size(); i++) {
			if (current_network_mode == i) {
				network_mode_option_button->set_text(editor_network_modes[i]);
				network_mode_option_button->select(i);
			}
		}
	}
	{
		const int current_update_mode = EDITOR_GET("network/connection/check_for_updates");
		for (int i = 0; i < editor_check_for_updates.size(); i++) {
			if (current_update_mode == i) {
				check_for_update_button->set_text(editor_check_for_updates[i]);
				check_for_update_button->select(i);
				check_for_update_button->set_disabled(!EDITOR_GET("network/connection/network_mode"));
			}
		}
	}
	{
		const int current_directory_naming = EDITOR_GET("project_manager/directory_naming_convention");
		for (int i = 0; i < editor_directory_naming_conventions.size(); i++) {
			if (current_directory_naming == i) {
				directory_naming_convention_button->set_text(editor_directory_naming_conventions[i]);
				directory_naming_convention_button->select(i);
			}
		}
	}
}

void SolersPMAIView::_add_quick_setting_control(const String &p_text, Control *p_control) {
	HBoxContainer *container = memnew(HBoxContainer);
	container->add_theme_constant_override("separation", 12 * EDSCALE);
	quick_settings_list->add_child(container);

	Label *label = memnew(Label(p_text));
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	label->add_theme_color_override(SceneStringName(font_color), Color(0.886f, 0.890f, 0.902f, 0.78f));
	container->add_child(label);

	p_control->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	container->add_child(p_control);
}

#ifndef ANDROID_ENABLED
void SolersPMAIView::_language_selected(int p_id) {
	_set_quick_setting_value("interface/editor/localization/editor_language", language_option_button->get_item_metadata(p_id));
}
#endif

void SolersPMAIView::_style_selected(int p_id) {
	_set_quick_setting_value("interface/theme/style", style_option_button->get_item_text(p_id));
}

void SolersPMAIView::_theme_selected(int p_id) {
	const String selected_theme = theme_option_button->get_item_text(p_id);
	_set_quick_setting_value("interface/theme/color_preset", selected_theme);
	custom_theme_label->set_visible(selected_theme == "Custom");
}

void SolersPMAIView::_scale_selected(int p_id) {
	_set_quick_setting_value("interface/editor/display_scale", p_id, true);
}

void SolersPMAIView::_network_mode_selected(int p_id) {
	_set_quick_setting_value("network/connection/network_mode", p_id);
	check_for_update_button->set_disabled(!p_id);
}

void SolersPMAIView::_check_for_update_selected(int p_id) {
	_set_quick_setting_value("network/connection/check_for_updates", p_id);
}

void SolersPMAIView::_directory_naming_convention_selected(int p_id) {
	_set_quick_setting_value("project_manager/directory_naming_convention", p_id);
}

void SolersPMAIView::_set_quick_setting_value(const String &p_setting, const Variant &p_value, bool p_restart_required) {
	EditorSettings::get_singleton()->set(p_setting, p_value);
	EditorSettings::get_singleton()->notify_changes();
	EditorSettings::get_singleton()->save();
	if (p_restart_required && restart_required_label && restart_required_button) {
		restart_required_label->show();
		restart_required_button->show();
	}
}

void SolersPMAIView::_show_full_settings() {
	// EditorSettingsDialog is a native Window. It must NOT be parented under the
	// Settings AcceptDialog we are about to hide — invisible parent Window ⇒
	// blank client area (Godot #114812). Anchor as sibling of the host dialog.
	Node *anchor = get_parent() ? get_parent()->get_parent() : nullptr;
	if (!anchor) {
		anchor = this;
	}
	if (!editor_settings_dialog) {
		EditorHelp::generate_doc();
		Ref<EditorInspectorDefaultPlugin> eidp;
		eidp.instantiate();
		EditorInspector::add_inspector_plugin(eidp);
		EditorPropertyNameProcessor *epnp = memnew(EditorPropertyNameProcessor);
		anchor->add_child(epnp);
		editor_settings_dialog = memnew(EditorSettingsDialog);
		anchor->add_child(editor_settings_dialog);
		editor_settings_dialog->connect("restart_requested", callable_mp(this, &SolersPMAIView::_request_restart));
	} else if (editor_settings_dialog->get_parent() != anchor) {
		editor_settings_dialog->reparent(anchor);
	}
	if (AcceptDialog *host = Object::cast_to<AcceptDialog>(get_parent())) {
		host->hide();
	}
	editor_settings_dialog->popup_edit_settings();
}

void SolersPMAIView::_request_restart() {
	emit_signal(SNAME("restart_required"));
}

void SolersPMAIView::_notification(int p_what) {
	if (p_what == NOTIFICATION_POSTINITIALIZE || p_what == NOTIFICATION_THEME_CHANGED) {
		if (api_key_reveal) {
			api_key_reveal->set_button_icon(SolersIcons::get(SNAME("tool_observe"), int(Math::round(15.0f * EDSCALE))));
		}
		if (restart_required_label) {
			restart_required_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
		}
		if (custom_theme_label) {
			custom_theme_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("font_placeholder_color"), EditorStringName(Editor)));
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

void SolersPMAIView::_bind_methods() {
	ADD_SIGNAL(MethodInfo("restart_required"));
}

void SolersPMAIView::select_category(const String &p_id) {
	_select_category(p_id);
}

void SolersPMAIView::update_quick_popup_size_limits(const Size2 &p_max_popup_size) {
#ifndef ANDROID_ENABLED
	if (language_option_button) {
		language_option_button->get_popup()->set_max_size(p_max_popup_size);
	}
#else
	(void)p_max_popup_size;
#endif
}

void SolersPMAIView::bind_services(SolersSettingsService *p_settings_service) {
#ifdef MODULE_SOLERS_AI_ENABLED
	if (owns_services) {
		if (settings_service) {
			memdelete(settings_service);
			settings_service = nullptr;
		}
		if (registry) {
			memdelete(registry);
			registry = nullptr;
		}
		owns_services = false;
	}
	settings_service = p_settings_service;
	registry = p_settings_service ? p_settings_service->get_provider_registry() : nullptr;
	refresh();
#else
	(void)p_settings_service;
#endif
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
	add_theme_constant_override("separation", 0);

#ifdef MODULE_SOLERS_AI_ENABLED
	registry = memnew(SolersProviderRegistry);
	settings_service = memnew(SolersSettingsService);
	settings_service->set_provider_registry(registry);
	owns_services = true;

	const SolersUITheme::Tokens tokens = SolersUITheme::make_tokens();

	// Left rail — layout only (dialog host paints the single flat fill).
	VBoxContainer *rail = memnew(VBoxContainer);
	rail->set_custom_minimum_size(Size2(200, 0) * EDSCALE);
	rail->set_v_size_flags(SIZE_EXPAND_FILL);
	rail->add_theme_constant_override("separation", 4 * EDSCALE);
	add_child(rail);

	MarginContainer *rail_margin = memnew(MarginContainer);
	rail_margin->set_v_size_flags(SIZE_EXPAND_FILL);
	rail_margin->add_theme_constant_override("margin_left", 8 * EDSCALE);
	rail_margin->add_theme_constant_override("margin_right", 8 * EDSCALE);
	rail_margin->add_theme_constant_override("margin_top", 14 * EDSCALE);
	rail_margin->add_theme_constant_override("margin_bottom", 14 * EDSCALE);
	rail->add_child(rail_margin);

	VBoxContainer *rail_inner = memnew(VBoxContainer);
	rail_inner->set_v_size_flags(SIZE_EXPAND_FILL);
	rail_inner->add_theme_constant_override("separation", 2 * EDSCALE);
	rail_margin->add_child(rail_inner);

	ScrollContainer *rail_scroll = memnew(ScrollContainer);
	rail_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	rail_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	rail_inner->add_child(rail_scroll);

	nav_list = memnew(VBoxContainer);
	nav_list->set_h_size_flags(SIZE_EXPAND_FILL);
	nav_list->add_theme_constant_override("separation", 2 * EDSCALE);
	rail_scroll->add_child(nav_list);

	_build_nav();

	// Pane edge is inherited from the native split theme.
	VSeparator *sep = memnew(VSeparator);
	sep->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(sep);

	ScrollContainer *scroll = memnew(ScrollContainer);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	scroll->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(scroll);

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
	provider_list_title->add_theme_color_override(SceneStringName(font_color), tokens.text);
	provider_list_view->add_child(provider_list_title);

	provider_list_notes = memnew(Label);
	provider_list_notes->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	provider_list_notes->add_theme_color_override(SceneStringName(font_color), tokens.text_dim);
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
	local_models_only_note->add_theme_color_override(SceneStringName(font_color), tokens.text_dim);
	local_models_only_note->add_theme_font_size_override(SceneStringName(font_size), MAX(9, (int)(11 * EDSCALE)));
	local_models_only_box->add_child(local_models_only_note);

	provider_list = memnew(VBoxContainer);
	provider_list->set_h_size_flags(SIZE_EXPAND_FILL);
	provider_list->add_theme_constant_override("separation", 2 * EDSCALE);
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
	provider_title->add_theme_color_override(SceneStringName(font_color), tokens.text);
	detail_top->add_child(provider_title);

	provider_notes = memnew(Label);
	provider_notes->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	provider_notes->add_theme_color_override(SceneStringName(font_color), tokens.text_dim);
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
		label->add_theme_color_override(SceneStringName(font_color), Color(tokens.text.r, tokens.text.g, tokens.text.b, 0.72f));
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

	auto add_token_limit = [&](const String &p_label, const String &p_tooltip) -> SpinBox * {
		add_form_label(p_label);
		SpinBox *input = memnew(SpinBox);
		input->set_min(0);
		input->set_max(INT32_MAX);
		input->set_allow_greater(true);
		input->set_step(1024);
		input->set_h_size_flags(SIZE_EXPAND_FILL);
		input->set_tooltip_text(p_tooltip);
		input->get_line_edit()->connect(SceneStringName(text_changed), callable_mp(this, &SolersPMAIView::_on_field_changed));
		connection_grid->add_child(input);
		return input;
	};
	context_window_edit = add_token_limit(TTR("Context window"), TTR("Use 0 for the provider catalog, or unknown on custom endpoints."));
	max_tokens_edit = add_token_limit(TTR("Maximum output"), TTR("Use 0 for the provider or Solers request default."));

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
	oauth_status->add_theme_color_override(SceneStringName(font_color), Color(tokens.text.r, tokens.text.g, tokens.text.b, 0.72f));
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
	env_hint->add_theme_color_override(SceneStringName(font_color), tokens.text_dim);
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

	// Quick Settings pane (EditorSettings) — sibling of provider list/detail.
	quick_settings_view = memnew(VBoxContainer);
	quick_settings_view->set_h_size_flags(SIZE_EXPAND_FILL);
	quick_settings_view->set_v_size_flags(SIZE_EXPAND_FILL);
	quick_settings_view->add_theme_constant_override("separation", 10 * EDSCALE);
	quick_settings_view->hide();
	content->add_child(quick_settings_view);

	Label *quick_title = memnew(Label(TTR("Quick Settings")));
	quick_title->add_theme_font_size_override(SceneStringName(font_size), MAX(12, (int)(17 * EDSCALE)));
	quick_title->add_theme_color_override(SceneStringName(font_color), tokens.text);
	quick_settings_view->add_child(quick_title);

	Label *quick_notes = memnew(Label(TTR("Common editor preferences. Open all settings for the full inspector.")));
	quick_notes->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	quick_notes->add_theme_color_override(SceneStringName(font_color), tokens.text_dim);
	quick_settings_view->add_child(quick_notes);

	_fetch_quick_setting_values();
	quick_settings_list = memnew(VBoxContainer);
	quick_settings_list->add_theme_constant_override("separation", 8 * EDSCALE);
	quick_settings_view->add_child(quick_settings_list);

#ifndef ANDROID_ENABLED
	{
		language_option_button = memnew(OptionButton);
		language_option_button->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
		language_option_button->set_fit_to_longest_item(false);
		language_option_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersPMAIView::_language_selected));
		for (int i = 0; i < editor_languages.size(); i++) {
			language_option_button->add_item(editor_languages[i].get_slicec('/', 1), i);
			language_option_button->set_item_metadata(i, editor_languages[i].get_slicec('/', 0));
		}
		_add_quick_setting_control(TTRC("Language"), language_option_button);
	}
#endif
	{
		style_option_button = memnew(OptionButton);
		style_option_button->set_fit_to_longest_item(false);
		style_option_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersPMAIView::_style_selected));
		for (int i = 0; i < editor_styles.size(); i++) {
			style_option_button->add_item(editor_styles[i], i);
		}
		_add_quick_setting_control(TTRC("Style"), style_option_button);
	}
	{
		theme_option_button = memnew(OptionButton);
		theme_option_button->set_fit_to_longest_item(false);
		theme_option_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersPMAIView::_theme_selected));
		for (int i = 0; i < editor_themes.size(); i++) {
			theme_option_button->add_item(editor_themes[i], i);
		}
		_add_quick_setting_control(TTRC("Color Preset"), theme_option_button);
		custom_theme_label = memnew(Label(TTRC("Custom preset can be further configured in the editor.")));
		custom_theme_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD);
		custom_theme_label->add_theme_color_override(SceneStringName(font_color), tokens.text_dim);
		custom_theme_label->hide();
		quick_settings_list->add_child(custom_theme_label);
	}
	{
		scale_option_button = memnew(OptionButton);
		scale_option_button->set_fit_to_longest_item(false);
		scale_option_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersPMAIView::_scale_selected));
		for (int i = 0; i < editor_scales.size(); i++) {
			scale_option_button->add_item(editor_scales[i], i);
		}
		_add_quick_setting_control(TTRC("Display Scale"), scale_option_button);
	}
	{
		network_mode_option_button = memnew(OptionButton);
		network_mode_option_button->set_fit_to_longest_item(false);
		network_mode_option_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersPMAIView::_network_mode_selected));
		for (int i = 0; i < editor_network_modes.size(); i++) {
			network_mode_option_button->add_item(editor_network_modes[i], i);
		}
		_add_quick_setting_control(TTRC("Network Mode"), network_mode_option_button);
	}
	{
		check_for_update_button = memnew(OptionButton);
		check_for_update_button->set_fit_to_longest_item(false);
		check_for_update_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersPMAIView::_check_for_update_selected));
		for (int i = 0; i < editor_check_for_updates.size(); i++) {
			check_for_update_button->add_item(editor_check_for_updates[i], i);
		}
		_add_quick_setting_control(TTRC("Check for Updates"), check_for_update_button);
	}
	{
		directory_naming_convention_button = memnew(OptionButton);
		directory_naming_convention_button->set_fit_to_longest_item(false);
		directory_naming_convention_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersPMAIView::_directory_naming_convention_selected));
		for (int i = 0; i < editor_directory_naming_conventions.size(); i++) {
			directory_naming_convention_button->add_item(editor_directory_naming_conventions[i], i);
		}
		_add_quick_setting_control(TTRC("Directory Naming Convention"), directory_naming_convention_button);
	}

	Button *open_full_settings = memnew(Button(TTRC("Edit All Settings")));
	open_full_settings->set_h_size_flags(SIZE_SHRINK_END);
	open_full_settings->connect(SceneStringName(pressed), callable_mp(this, &SolersPMAIView::_show_full_settings));
	quick_settings_list->add_child(open_full_settings);

	restart_required_label = memnew(Label(TTRC("Settings changed! The project manager must be restarted for changes to take effect.")));
	restart_required_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD);
	restart_required_label->hide();
	quick_settings_view->add_child(restart_required_label);

	restart_required_button = memnew(Button(TTRC("Restart Now")));
	restart_required_button->set_h_size_flags(SIZE_SHRINK_BEGIN);
	restart_required_button->hide();
	restart_required_button->connect(SceneStringName(pressed), callable_mp(this, &SolersPMAIView::_request_restart));
	quick_settings_view->add_child(restart_required_button);

	_update_quick_settings_values();
	_select_category("plugins");
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
	if (owns_services) {
		if (settings_service) {
			memdelete(settings_service);
			settings_service = nullptr;
		}
		if (registry) {
			memdelete(registry);
			registry = nullptr;
		}
	}
#endif
}
