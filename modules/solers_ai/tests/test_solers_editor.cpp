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

#include "core/config/project_settings.h"
#include "core/input/input_event.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/message_queue.h"
#include "core/string/translation_server.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/file_dialog.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/split_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/skin.h"
#include "scene/resources/animation.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/environment.h"
#include "scene/resources/style_box_flat.h"
#include "tests/test_macros.h"
#include "tests/test_tools.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_GLTF_ENABLED
#include "modules/gltf/gltf_document.h"
#include "modules/gltf/gltf_state.h"
#endif
#include "modules/solers_ai/editor/solers_asset_grid.h"
#include "modules/solers_ai/editor/solers_chat_cells.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/editor/solers_editor_plugin.h"
#include "modules/solers_ai/editor/solers_model_preview.h"
#include "modules/solers_ai/editor/solers_popup_list.h"
#include "modules/solers_ai/editor/solers_schema_form.h"
#include "modules/solers_ai/editor/solers_ui_theme.h"
#include "modules/solers_ai/generated/solers_svg_assets.gen.h"

TEST_FORCE_LINK(test_solers_editor)

namespace TestSolersEditor {

static void _stage_schema_image(const Variant &, const Callable &p_callback) {
	Dictionary data;
	data["local_path"] = "user://staged-schema-image.png";
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	Ref<Image> image = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
	p_callback.call(result, image);
}

static String selected_asset_callback;
static String menu_asset_callback;

static void _record_selected_asset(const String &p_asset_id) {
	selected_asset_callback = p_asset_id;
}

static void _record_menu_asset(const String &p_asset_id, Control *) {
	menu_asset_callback = p_asset_id;
}

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
	CHECK(theme->has_stylebox(SNAME("panel"), SNAME("PanelContainer")));
	CHECK(theme->has_stylebox(SNAME("normal"), SNAME("Button")));
	CHECK(theme->has_stylebox(SNAME("normal"), SNAME("OptionButton")));
	CHECK(theme->has_stylebox(SNAME("normal"), SNAME("LineEdit")));
	CHECK(theme->has_stylebox(SNAME("normal"), SNAME("TextEdit")));
	CHECK(theme->has_stylebox(SNAME("panel"), SNAME("ItemList")));
	CHECK(theme->has_stylebox(SNAME("panel"), SNAME("TabContainer")));
	CHECK(theme->has_stylebox(SNAME("background"), SNAME("ProgressBar")));
	CHECK(theme->has_stylebox(SNAME("normal"), SNAME("CheckBox")));
	CHECK(theme->has_stylebox(SNAME("normal"), SNAME("CheckButton")));
	CHECK(theme->get_type_variation_base(SNAME("SolersPrimaryButton")) == SNAME("Button"));
	CHECK(theme->has_stylebox(SNAME("normal"), SNAME("SolersPrimaryButton")));
	CHECK(theme->get_color(SNAME("font_disabled_color"), SNAME("SolersPrimaryButton")) == SolersUITheme::make_tokens().on_primary);
	CHECK(theme->get_type_variation_base(SNAME("SolersStudioPrompt")) == SNAME("TextEdit"));
	CHECK(theme->get_type_variation_base(SNAME("SolersStudioSegment")) == SNAME("Button"));
	CHECK(theme->has_stylebox(SNAME("pressed"), SNAME("SolersStudioSegment")));
	CHECK(theme->get_type_variation_base(SNAME("SolersStudioActionButton")) == SNAME("Button"));
	CHECK(theme->get_color(SNAME("icon_hover_color"), SNAME("SolersStudioActionButton")) != theme->get_color(SNAME("icon_normal_color"), SNAME("SolersStudioActionButton")));
	CHECK(theme->get_type_variation_base(SNAME("SolersStudioRail")) == SNAME("ItemList"));
	CHECK(theme->has_stylebox(SNAME("hovered"), SNAME("SolersStudioRail")));
	Ref<StyleBoxFlat> rail_selected = theme->get_stylebox(SNAME("selected"), SNAME("SolersStudioRail"));
	REQUIRE(rail_selected.is_valid());
	CHECK(rail_selected->get_border_width(SIDE_LEFT) == 0);
	CHECK(theme->get_constant(SNAME("v_separation"), SNAME("SolersStudioRail")) > 0);
	CHECK(theme->get_type_variation_base(SNAME("SolersStudioScroll")) == SNAME("VScrollBar"));
	CHECK(theme->has_stylebox(SNAME("grabber_highlight"), SNAME("SolersStudioScroll")));
	CHECK(theme->get_type_variation_base(SNAME("SolersAssetCard")) == SNAME("Button"));
	CHECK(theme->get_color(SNAME("failure_color"), SNAME("SolersAssetCard")) != theme->get_color(SNAME("placeholder_color"), SNAME("SolersAssetCard")));
	CHECK(theme->get_type_variation_base(SNAME("SolersPopupPanel")) == SNAME("PanelContainer"));
	CHECK(theme->get_type_variation_base(SNAME("SolersPopupItem")) == SNAME("Button"));
	CHECK(theme->get_type_variation_base(SNAME("SolersPopupDangerItem")) == SNAME("Button"));
	CHECK(theme->get_font_size(SceneStringName(font_size), SNAME("SolersHeroTitle")) > theme->get_font_size(SceneStringName(font_size), SNAME("SolersSessionTitle")));
	CHECK(bool(theme->has_icon(SNAME("arrow"), SNAME("OptionButton")) && theme->has_icon(SNAME("radio_checked"), SNAME("PopupMenu")) && theme->has_icon(SNAME("radio_unchecked"), SNAME("PopupMenu"))));
	PanelContainer *stage = memnew(PanelContainer);
	stage->set_position(Vector2(20, 20));
	stage->set_size(Size2(400, 300));
	SolersActivityIndicator *activity = memnew(SolersActivityIndicator);
	activity->set_custom_minimum_size(Size2(64, 64));
	stage->add_child(activity);
	CHECK(activity->is_processing_internal());
	CHECK(activity->get_combined_minimum_size() == Size2(64, 64));
	TextureRect *fallback = memnew(TextureRect);
	fallback->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	stage->add_child(fallback);
	SolersModelPreview *model_preview = memnew(SolersModelPreview);
	stage->add_child(model_preview);
	SceneTree::get_singleton()->get_root()->add_child(stage);
	MessageQueue::get_singleton()->flush();
	CHECK(model_preview->is_mouse_target_enabled());
	const String source_model_path = TestUtils::get_data_path("models/suzanne.glb");
	const String model_path = "user://solers-preview-suzanne.glb";
	Ref<FileAccess> model_file = FileAccess::open(model_path, FileAccess::WRITE);
	REQUIRE(model_file.is_valid());
	model_file->store_buffer(FileAccess::get_file_as_bytes(source_model_path));
	model_file.unref();
	{
		ErrorDetector errors;
		REQUIRE(model_preview->load_model(model_path) == OK);
		CHECK_FALSE(errors.has_error);
	}
	CHECK(model_preview->has_model());
	CHECK(Object::cast_to<MeshInstance3D>(model_preview->get_child(0)->find_child("Suzanne", true, false)) != nullptr);
	Camera3D *preview_camera = Object::cast_to<Camera3D>(model_preview->get_child(0)->get_child(0));
	REQUIRE(preview_camera);
	const Ref<Environment> preview_environment = preview_camera->get_environment();
	REQUIRE(preview_environment.is_valid());
	CHECK(preview_environment->get_sky().is_valid());
	Ref<InputEventMouseButton> middle;
	middle.instantiate();
	middle->set_position(Vector2(200, 150));
	middle->set_button_index(MouseButton::MIDDLE);
	middle->set_pressed(true);
	SceneTree::get_singleton()->get_root()->push_input(middle, true);
	CHECK(SceneTree::get_singleton()->get_root()->gui_get_hovered_control() == model_preview);
	const Vector3 camera_before_pan = preview_camera->get_global_position();
	Ref<InputEventMouseMotion> pan;
	pan.instantiate();
	pan->set_position(Vector2(220, 160));
	pan->set_relative(Vector2(20, 10));
	pan->set_button_mask(MouseButtonMask::MIDDLE);
	SceneTree::get_singleton()->get_root()->push_input(pan, true);
	CHECK(preview_camera->get_global_position() != camera_before_pan);
	middle->set_pressed(false);
	SceneTree::get_singleton()->get_root()->push_input(middle, true);
	const Vector3 camera_before_orbit = preview_camera->get_global_position();
	pan->set_relative(Vector2(10, 0));
	pan->set_button_mask(MouseButtonMask::LEFT);
	SceneTree::get_singleton()->get_root()->push_input(pan, true);
	CHECK(preview_camera->get_global_position() != camera_before_orbit);
	const Vector3 camera_before_zoom = preview_camera->get_global_position();
	Ref<InputEventMouseButton> wheel;
	wheel.instantiate();
	wheel->set_position(Vector2(200, 150));
	wheel->set_button_index(MouseButton::WHEEL_UP);
	wheel->set_pressed(true);
	SceneTree::get_singleton()->get_root()->push_input(wheel, true);
	CHECK(preview_camera->get_global_position() != camera_before_zoom);
	CHECK(model_preview->load_model("res://missing-solers-preview.glb") != OK);
	CHECK_FALSE(model_preview->has_model());
#ifdef MODULE_GLTF_ENABLED
	const String rigged_path = "user://solers-preview-rigged.glb";
	Node3D *rigged_source = memnew(Node3D);
	rigged_source->set_name("RiggedPreview");
	Skeleton3D *skeleton = memnew(Skeleton3D);
	skeleton->set_name("Skeleton3D");
	skeleton->add_bone("Root");
	rigged_source->add_child(skeleton);
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	PackedVector3Array vertices;
	vertices.push_back(Vector3(-0.5, 0, 0));
	vertices.push_back(Vector3(0.5, 0, 0));
	vertices.push_back(Vector3(0, 1, 0));
	PackedInt32Array bones;
	PackedFloat32Array weights;
	for (int i = 0; i < vertices.size(); i++) {
		for (int influence = 0; influence < 4; influence++) {
			bones.push_back(0);
			weights.push_back(influence == 0 ? 1.0 : 0.0);
		}
	}
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_BONES] = bones;
	arrays[Mesh::ARRAY_WEIGHTS] = weights;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	MeshInstance3D *skinned_mesh = memnew(MeshInstance3D);
	skinned_mesh->set_name("SkinnedMesh");
	skinned_mesh->set_mesh(mesh);
	rigged_source->add_child(skinned_mesh);
	skinned_mesh->set_skeleton_path(skinned_mesh->get_path_to(skeleton));
	skinned_mesh->set_skin(skeleton->create_skin_from_rest_transforms());
	AnimationPlayer *player = memnew(AnimationPlayer);
	player->set_name("AnimationPlayer");
	rigged_source->add_child(player);
	Ref<Animation> idle;
	idle.instantiate();
	idle->set_length(1.0);
	const int track = idle->add_track(Animation::TYPE_POSITION_3D);
	idle->track_set_path(track, NodePath("Skeleton3D:Root"));
	idle->track_insert_key(track, 0.0, Vector3());
	idle->track_insert_key(track, 1.0, Vector3(0, 0.1, 0));
	Ref<AnimationLibrary> library;
	library.instantiate();
	REQUIRE(library->add_animation("Idle", idle) == OK);
	REQUIRE(player->add_animation_library(StringName(), library) == OK);
	Ref<GLTFDocument> document;
	document.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	REQUIRE(document->append_from_scene(rigged_source, state) == OK);
	REQUIRE(document->write_to_filesystem(state, rigged_path) == OK);
	memdelete(rigged_source);
	REQUIRE(model_preview->load_model(rigged_path) == OK);
	CHECK(model_preview->has_skeleton());
	CHECK(model_preview->get_animation_names().has("Idle"));
	CHECK(model_preview->play_animation("Idle"));
	CHECK(model_preview->is_animation_playing());
	model_preview->pause_animation();
	model_preview->stop_animation();
	model_preview->set_skeleton_visible(true);
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(rigged_path));
#endif
	stage->queue_free();
	MessageQueue::get_singleton()->flush();
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(model_path));

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

TEST_CASE("[SolersStudio][SceneTree] schema form projects unknown connector fields without provider branches") {
	const Dictionary properties = JSON::parse_string(R"({"future_mode":{"type":"string","enum_source":"future_choices","enum_value":"wire_value","enum_label":"display_name","default":"beta"},"future_count":{"type":"integer","minimum":1,"maximum":9,"default":4},"future_enabled":{"type":"boolean","default":true},"future_formats":{"type":"array","maxItems":1,"items":{"type":"string","enum":["glb","obj"]},"default":["glb"]},"future_prompt":{"type":"string","default":"Detailed"},"future_image":{"type":"string"}})");
	const Dictionary extras = JSON::parse_string(R"({"future_choices":[{"wire_value":"alpha","display_name":"Alpha"},{"wire_value":"beta","display_name":"Beta"}]})");
	const Dictionary presentation = JSON::parse_string(R"({"controls":{"future_mode":{"control":"segmented"},"future_count":{"control":"slider"},"future_formats":{"control":"multi_select"},"future_prompt":{"control":"multiline"},"future_image":{"control":"image"}}})");
	SolersSchemaForm *form = memnew(SolersSchemaForm);
	SceneTree::get_singleton()->get_root()->add_child(form);
	form->set_image_stager(callable_mp_static(_stage_schema_image));
	form->set_schema(properties, extras, presentation);
	CHECK(form->find_children("*", "CheckButton", true, false).size() == 1);
	CHECK(form->get_values().get("future_mode", String()) == "beta");
	CHECK((int)form->get_values().get("future_count", 0) == 4);
	CHECK(form->get_values().get("future_enabled", false));
	CHECK(Array(form->get_values().get("future_formats", Array())).has("glb"));
	CHECK(form->get_values().get("future_prompt", String()) == "Detailed");
	CHECK(form->find_children("*", "SolersSurface", true, false).size() == 1);
	const TypedArray<Node> menus = form->find_children("*", "MenuButton", true, false);
	MenuButton *format_selector = nullptr;
	for (const Variant &node : menus) {
		MenuButton *candidate = Object::cast_to<MenuButton>(node.operator Object *());
		format_selector = candidate && candidate->get_meta("field_kind", String()) == "multi_select" ? candidate : format_selector;
	}
	REQUIRE(format_selector);
	PopupMenu *formats = format_selector->get_popup();
	CHECK(formats->is_search_bar_enabled());
	CHECK_FALSE(formats->is_hide_on_checkable_item_selection());
	CHECK(formats->is_item_checked(0));
	formats->emit_signal(SNAME("id_pressed"), 1);
	CHECK_FALSE(formats->is_item_checked(1));
	const String source_path = "user://solers-schema-image.png";
	Ref<Image> image = Image::create_empty(640, 360, false, Image::FORMAT_RGBA8);
	REQUIRE(image->save_png(source_path) == OK);
	const TypedArray<Node> dialogs = form->find_children("*", "FileDialog", true, false);
	REQUIRE(dialogs.size() == 1);
	FileDialog *dialog = Object::cast_to<FileDialog>(dialogs[0]);
	REQUIRE(dialog);
	const TypedArray<Node> image_buttons = dialog->get_parent()->find_children("*", "Button", false, false);
	REQUIRE(image_buttons.size() == 1);
	Button *image_button = Object::cast_to<Button>(image_buttons[0]);
	REQUIRE(image_button);
	CHECK(image_button->get_mouse_filter() == Control::MOUSE_FILTER_PASS);
	image_button->emit_signal(SceneStringName(mouse_entered));
	CHECK(image_button->has_focus());
	PackedStringArray files;
	files.push_back(source_path);
	dialog->emit_signal(SNAME("files_selected"), files);
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(source_path));
	CHECK(form->get_values().get("future_image", String()) == "user://staged-schema-image.png");
	CHECK(image_button->get_combined_minimum_size().x < image->get_width());
	CHECK(image_button->get_combined_minimum_size().y < image->get_height());
	Dictionary changed;
	changed["future_mode"] = "alpha";
	changed["future_count"] = 7;
	changed["future_enabled"] = false;
	changed["future_formats"] = JSON::parse_string(R"(["obj"])");
	form->set_values(changed);
	CHECK(form->get_values().get("future_mode", String()) == "alpha");
	CHECK((int)form->get_values().get("future_count", 0) == 7);
	CHECK_FALSE((bool)form->get_values().get("future_enabled", true));
	CHECK(Array(form->get_values().get("future_formats", Array())).has("obj"));
	form->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SolersStudio][SceneTree] selectors keep native state with the shared rich popup") {
	Control *host = memnew(Control);
	SolersPopupList *popup = memnew(SolersPopupList);
	Button *anchor = memnew(Button);
	host->set_size(Size2(420, 240) * EDSCALE);
	anchor->set_position(Size2(24, 196) * EDSCALE);
	anchor->set_size(Size2(160, 32) * EDSCALE);
	host->add_child(anchor);
	host->add_child(popup);
	SceneTree::get_singleton()->get_root()->add_child(host);
	MessageQueue::get_singleton()->flush();

	const Array popup_items = JSON::parse_string(R"([{"id":"fast","label":"Fast","description":"Quick preview"},{"id":"detail","label":"Detailed","description":"Highest fidelity"}])");
	popup->popup(anchor, popup_items, "detail", Callable());
	Button *selected_row = Object::cast_to<Button>(host->get_viewport()->gui_get_focus_owner());
	REQUIRE(selected_row);
	Ref<InputEventKey> escape;
	escape.instantiate();
	escape->set_keycode(Key::ESCAPE);
	escape->set_pressed(true);
	popup->unhandled_key_input(escape);
	CHECK_FALSE(popup->is_visible());
	Array overflow_items;
	const int overflow_count = int(host->get_size().y / (34 * EDSCALE)) + 2;
	for (int i = 0; i < overflow_count; i++) {
		Dictionary item;
		item["id"] = itos(i);
		item["label"] = "Synthetic item " + itos(i);
		overflow_items.push_back(item);
	}
	popup->popup(anchor, overflow_items, itos(overflow_count - 1), Callable());
	MessageQueue::get_singleton()->flush();
	PanelContainer *popup_panel = Object::cast_to<PanelContainer>(popup->get_child(1));
	REQUIRE(popup_panel);
	ScrollContainer *popup_scroll = Object::cast_to<ScrollContainer>(popup_panel->get_child(0));
	REQUIRE(popup_scroll);
	CHECK(popup_panel->get_position().y < anchor->get_position().y);
	CHECK(popup_panel->get_rect().get_end().y <= host->get_size().y);
	CHECK(popup_scroll->is_following_focus());
	CHECK(popup_scroll->get_size().y < Object::cast_to<Control>(popup_scroll->get_child(0))->get_combined_minimum_size().y);
	popup->close();
	SolersSchemaForm *form = memnew(SolersSchemaForm);
	form->set_popup_list(popup);
	host->add_child(form);
	const Dictionary properties = JSON::parse_string(R"({"quality":{"type":"string","enum":[{"id":"fast","label":"Fast","description":"Quick preview"},{"id":"detail","label":"Detailed","description":"Highest fidelity"}],"enum_value":"id","enum_label":"label","default":"detail"}})");
	form->set_schema(properties, Dictionary(), Dictionary());
	const TypedArray<Node> option_nodes = form->find_children("*", "OptionButton", true, false);
	REQUIRE(option_nodes.size() == 1);
	CHECK(Object::cast_to<SolersStudioSelect>(option_nodes[0]) != nullptr);
	SolersAssetGrid *grid = memnew(SolersAssetGrid);
	grid->set_theme(SolersUITheme::create());
	selected_asset_callback.clear();
	menu_asset_callback.clear();
	grid->set_callbacks(callable_mp_static(_record_selected_asset), callable_mp_static(_record_menu_asset));
	host->add_child(grid);
	grid->add_asset(JSON::parse_string(R"({"id":"busy","status":"running"})"), Ref<Texture2D>());
	grid->add_asset(JSON::parse_string(R"({"id":"ready","status":"ready"})"), Ref<Texture2D>());
	grid->set_selected_asset("busy");
	MessageQueue::get_singleton()->flush();
	const TypedArray<Node> card_buttons = grid->find_children("*", "Button", true, false);
	Button *card = card_buttons.size() > 0 ? Object::cast_to<Button>(card_buttons[0]) : nullptr;
	Button *menu = card_buttons.size() > 1 ? Object::cast_to<Button>(card_buttons[1]) : nullptr;
	Button *ready_card = card_buttons.size() > 2 ? Object::cast_to<Button>(card_buttons[2]) : nullptr;
	Button *ready_menu = card_buttons.size() > 3 ? Object::cast_to<Button>(card_buttons[3]) : nullptr;
	const TypedArray<Node> activity_nodes = grid->find_children("*", "SolersActivityIndicator", true, false);
	Control *card_activity = activity_nodes.is_empty() ? nullptr : Object::cast_to<Control>(activity_nodes[0]);
	REQUIRE(bool(card && menu && ready_card && ready_menu && card_activity));
	CHECK(card_activity->get_combined_minimum_size() == Size2(32, 32) * EDSCALE);
	CHECK(Rect2(Vector2(), card->get_size()).encloses(menu->get_rect()));
	CHECK(menu->get_mouse_filter() == Control::MOUSE_FILTER_STOP);
	ready_menu->emit_signal(SceneStringName(pressed));
	CHECK(menu_asset_callback == "ready");
	CHECK(selected_asset_callback.is_empty());
	CHECK(card->is_pressed());
	CHECK_FALSE(ready_card->is_pressed());
	card->grab_focus();
	CHECK(menu->is_visible());
	ready_menu->grab_focus();
	CHECK(ready_menu->is_visible());
	grid->clear_assets();
	grid->add_asset(JSON::parse_string(R"({"id":"failed","status":"failed"})"), Ref<Texture2D>());
	MessageQueue::get_singleton()->flush();
	const TypedArray<Node> failed_icons = grid->find_children("*", "TextureRect", true, false);
	TextureRect *failed_icon = nullptr;
	const Color failure_color = grid->get_theme()->get_color(SNAME("failure_color"), SNAME("SolersAssetCard"));
	for (const Variant &node : failed_icons) {
		TextureRect *candidate = Object::cast_to<TextureRect>(node.operator Object *());
		failed_icon = candidate && candidate->get_self_modulate() == failure_color ? candidate : failed_icon;
	}
	REQUIRE(failed_icon);
	CHECK(failed_icon->get_combined_minimum_size() == Size2(32, 32) * EDSCALE);
	CHECK(failed_icon->get_self_modulate() == failure_color);
	const Ref<Texture2D> replacement = SolersIcons::get(SNAME("photo_ai"), 32);
	grid->set_asset_preview("failed", replacement);
	CHECK(failed_icon->get_texture()->get_rid() == replacement->get_rid());
	CHECK(failed_icon->get_stretch_mode() == TextureRect::STRETCH_KEEP_ASPECT_COVERED);
	CHECK(failed_icon->get_self_modulate() == Color(1, 1, 1));

	memdelete(host);
	SolersIcons::clear_cache();
}

TEST_CASE("[SolersUI][SceneTree][Editor] editor locale and technical tool presentation have separate authorities") {
	const String setting = "interface/editor/localization/editor_language";
	ScopedEditorLanguage restore;
	EditorSettings::get_singleton()->set_setting(setting, "zh_Hans");
	TranslationServer::get_singleton()->set_locale("zh_Hans");
	solers_load_editor_translation();
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Cancel", StringName()) != "Cancel");
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Send", StringName()) != "Send");
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("New chat", StringName()) == String::utf8("\xE6\x96\xB0\xE5\xBB\xBA\xE5\xAF\xB9\xE8\xAF\x9D"));
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Effort", StringName()) != "Effort");
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Generating", StringName()) != "Generating");
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Text prompt", StringName()) != "Text prompt");
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Highest detail and fidelity", StringName()) != "Highest detail and fidelity");
	CHECK(TranslationServer::get_singleton()->get_editor_domain()->translate("Smart low-poly topology", StringName()) != "Smart low-poly topology");

	SolersToolCell *search = memnew(SolersToolCell);
	SolersToolCell *unknown = memnew(SolersToolCell);
	SceneTree::get_singleton()->get_root()->add_child(search);
	SceneTree::get_singleton()->get_root()->add_child(unknown);
	search->set_size(Size2(480, 20) * EDSCALE);
	const Dictionary search_presentation({ { "running", "Searching" }, { "completed", "Searched" }, { "failed", "Failed to search" } });
	search->start("synthetic.lookup", R"({"query":"unseen-subject","limit":7})", search_presentation, "unseen-subject");
	unknown->start("future.tool", "{}", Dictionary(), String());
	CHECK(search->get_status_text() == "Searching unseen-subject");
	CHECK(unknown->get_status_text() == "Running future.tool");
	CHECK_FALSE(search->is_expanded());

	Ref<InputEventMouseButton> click;
	click.instantiate();
	click->set_button_index(MouseButton::LEFT);
	click->set_pressed(true);
	search->gui_input(click);
	CHECK(search->is_expanded());
	CHECK(search->get_detail_text().contains("synthetic.lookup"));
	CHECK(search->get_detail_text().contains("unseen-subject"));
	const float expanded_height = search->get_minimum_size().y;
	CHECK(expanded_height > 20.0f * EDSCALE);

	search->finish(Dictionary({ { "ok", true }, { "data", Dictionary({ { "value", 7 } }) } }), 4);
	CHECK(search->get_status_text() == "Searched unseen-subject");
	unknown->finish(Dictionary({ { "ok", false }, { "error", Dictionary({ { "code", "SYNTHETIC_FAILURE" }, { "message", "failed" } }) } }), 5);
	CHECK(unknown->get_status_text() == "Failed future.tool");

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
