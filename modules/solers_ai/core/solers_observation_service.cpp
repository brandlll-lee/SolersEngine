/**************************************************************************/
/*  solers_observation_service.cpp                                        */
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

#include "solers_observation_service.h"

#include "solers_trace.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/debugger/debugger_marshalls.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_log.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/run/editor_run_bar.h"
#include "editor/run/game_view_plugin.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/debugger/scene_debugger.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/environment.h"
#include "scene/resources/packed_scene.h"
#include "servers/rendering/rendering_server.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_resource_service.h"

static constexpr uint64_t SOLERS_CAPTURE_TIMEOUT_MSEC = 10000;
static constexpr int SOLERS_RUNTIME_EVENT_LIMIT = 512;
// Keep encoded captures small enough for model requests without losing layout
// readability.
static constexpr int SOLERS_CAPTURE_MAX_DIMENSION = 1280;

static bool _solers_capture_source_is_current(const Dictionary &p_source) {
	if (p_source.is_empty()) {
		return true;
	}
	EditorNode *editor = EditorNode::get_singleton();
	Node *root = editor && EditorNode::get_editor_data().get_edited_scene_count() > 0 ? editor->get_edited_scene() : nullptr;
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	const int history_id = root ? EditorNode::get_editor_data().get_current_edited_scene_history_id() : EditorUndoRedoManager::INVALID_HISTORY;
	if (!root || !manager || history_id == EditorUndoRedoManager::INVALID_HISTORY) {
		return false;
	}
	const uint64_t version = manager->get_or_create_history(history_id).undo_redo->get_version();
	return (int64_t)p_source.get("history_id", -1) == history_id &&
			(uint64_t)(int64_t)p_source.get("version", -1) == version &&
			(!p_source.has("root_object_id") || (uint64_t)(int64_t)p_source.get("root_object_id", 0) == (uint64_t)root->get_instance_id());
}

static Array _solers_vector3_array(const Vector3 &p_vector) {
	// Snapped to 1e-4: full double precision multiplies the serialized tree
	// size several times over without carrying placement information.
	Array values;
	values.push_back(Math::snapped(p_vector.x, (real_t)0.0001));
	values.push_back(Math::snapped(p_vector.y, (real_t)0.0001));
	values.push_back(Math::snapped(p_vector.z, (real_t)0.0001));
	return values;
}

static void _solers_collect_geometry_containing_point(Node *p_node, Node *p_root, const Vector3 &p_point, Array &r_paths) {
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		const AABB bounds = geometry->get_global_transform().xform(geometry->get_aabb());
		if (geometry->is_visible_in_tree() && bounds.has_volume() && bounds.has_point(p_point)) {
			r_paths.push_back(String(p_root->get_path_to(geometry)));
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_geometry_containing_point(p_node->get_child(i), p_root, p_point, r_paths);
	}
}

// Nearest visible-geometry AABB hit along a ray. Works on whitebox scenes with
// no physics bodies, so the agent learns "the camera stares into a wall 0.5m
// away" directly from the scene snapshot.
static void _solers_collect_ray_hit(Node *p_node, Node *p_root, const Vector3 &p_from, const Vector3 &p_dir, real_t &r_best_distance, String &r_best_path) {
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		const AABB bounds = geometry->get_global_transform().xform(geometry->get_aabb());
		if (geometry->is_visible_in_tree() && bounds.has_volume()) {
			bool inside = false;
			Vector3 point;
			if (bounds.find_intersects_ray(p_from, p_dir, inside, &point) && !inside) {
				const real_t distance = p_from.distance_to(point);
				if (distance > (real_t)0.01 && (r_best_distance < 0 || distance < r_best_distance)) {
					r_best_distance = distance;
					r_best_path = String(p_root->get_path_to(geometry));
				}
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_ray_hit(p_node->get_child(i), p_root, p_from, p_dir, r_best_distance, r_best_path);
	}
}

static void _solers_accumulate_world_aabb(Node *p_node, AABB &r_bounds, bool &r_found) {
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		const AABB bounds = geometry->get_global_transform().xform(geometry->get_aabb());
		if (geometry->is_visible_in_tree() && bounds.has_volume()) {
			r_bounds = r_found ? r_bounds.merge(bounds) : bounds;
			r_found = true;
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_accumulate_world_aabb(p_node->get_child(i), r_bounds, r_found);
	}
}

static Camera3D *_solers_find_first_camera(Node *p_node) {
	if (Camera3D *camera = Object::cast_to<Camera3D>(p_node)) {
		return camera;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (Camera3D *camera = _solers_find_first_camera(p_node->get_child(i))) {
			return camera;
		}
	}
	return nullptr;
}

// A failed node lookup should navigate, not dead-end: report where paths are
// anchored and what actually exists so one error is enough to self-correct.
static String _solers_scene_path_hint(Node *p_root) {
	PackedStringArray children;
	for (int i = 0; i < p_root->get_child_count() && i < 16; i++) {
		children.push_back(String(p_root->get_child(i)->get_name()));
	}
	return vformat("Paths are relative to the edited scene root '%s' (top-level children: %s).", String(p_root->get_name()), children.is_empty() ? String("none") : String(", ").join(children));
}

static void _solers_collect_camera_paths(Node *p_node, Node *p_root, PackedStringArray &r_paths) {
	if (Object::cast_to<Camera3D>(p_node) && r_paths.size() < 16) {
		r_paths.push_back(String(p_root->get_path_to(p_node)));
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_camera_paths(p_node->get_child(i), p_root, r_paths);
	}
}

// The editor draws lazily; queue one full redraw so a frame-gated capture is
// guaranteed to see a freshly rendered frame instead of timing out while the
// editor idles.
static void _solers_request_editor_redraw() {
	if (EditorNode::get_singleton() && EditorNode::get_singleton()->get_gui_base()) {
		EditorNode::get_singleton()->get_gui_base()->queue_redraw();
	}
}

int SolersObservationService::get_capture_settle_frame_count(bool p_sdfgi_enabled, int p_convergence_setting) {
	if (!p_sdfgi_enabled) {
		return 1;
	}
	static constexpr int frames[RSE::ENV_SDFGI_CONVERGE_MAX] = { 5, 10, 15, 20, 25, 30 };
	return frames[CLAMP(p_convergence_setting, 0, (int)RSE::ENV_SDFGI_CONVERGE_MAX - 1)];
}

static Ref<Environment> _solers_capture_environment(const Ref<World3D> &p_world, const Ref<Environment> &p_override = Ref<Environment>()) {
	if (p_override.is_valid()) {
		return p_override;
	}
	if (p_world.is_null()) {
		return Ref<Environment>();
	}
	const Ref<Environment> environment = p_world->get_environment();
	return environment.is_valid() ? environment : p_world->get_fallback_environment();
}

static int _solers_capture_settle_frames(const Ref<World3D> &p_world, const Ref<Environment> &p_override = Ref<Environment>()) {
	const Ref<Environment> environment = _solers_capture_environment(p_world, p_override);
	const bool sdfgi_enabled = environment.is_valid() && environment->is_sdfgi_enabled();
	return SolersObservationService::get_capture_settle_frame_count(sdfgi_enabled, (int)GLOBAL_GET("rendering/global_illumination/sdfgi/frames_to_converge"));
}

void SolersObservationService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_project_info"), &SolersObservationService::get_project_info);
	ClassDB::bind_method(D_METHOD("get_project_settings_summary"), &SolersObservationService::get_project_settings_summary);
	ClassDB::bind_method(D_METHOD("list_project_files", "max_files"), &SolersObservationService::list_project_files, DEFVAL(512));
	ClassDB::bind_method(D_METHOD("search_project", "args"), &SolersObservationService::search_project);
	ClassDB::bind_method(D_METHOD("observe_path", "path"), &SolersObservationService::observe_path);
	ClassDB::bind_method(D_METHOD("digest_packed_scene", "path", "max_nodes"), &SolersObservationService::digest_packed_scene, DEFVAL(96));
	ClassDB::bind_method(D_METHOD("read_project_file", "path", "max_bytes", "raw"), &SolersObservationService::read_project_file, DEFVAL(262144), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_open_scenes", "max_depth", "max_children_per_node"), &SolersObservationService::get_open_scenes, DEFVAL(1), DEFVAL(16));
	ClassDB::bind_method(D_METHOD("get_selection", "max_depth", "max_children_per_node"), &SolersObservationService::get_selection, DEFVAL(1), DEFVAL(16));
	ClassDB::bind_method(D_METHOD("get_scene_tree", "max_depth", "max_children_per_node", "token_budget"), &SolersObservationService::get_scene_tree);
	ClassDB::bind_method(D_METHOD("get_runtime_status"), &SolersObservationService::get_runtime_status);
	ClassDB::bind_method(D_METHOD("observe_runtime", "args"), &SolersObservationService::observe_runtime);
	ClassDB::bind_method(D_METHOD("get_editor_logs", "max_messages"), &SolersObservationService::get_editor_logs, DEFVAL(200));
	ClassDB::bind_method(D_METHOD("capture_viewport", "args"), &SolersObservationService::capture_viewport);
}

void SolersObservationService::_render_frame_post_draw() {
	render_post_draw_sequence++;
}

Dictionary SolersObservationService::_capture_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;
	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

Dictionary SolersObservationService::_runtime_capture_unavailable() const {
	Dictionary failure = _capture_error("RUNTIME_CAPTURE_UNAVAILABLE", "Runtime capture transport cannot request a root viewport screenshot.", true);
	Dictionary data;
	data["runtime"] = get_runtime_status();
	failure["data"] = data;
	return failure;
}

Dictionary SolersObservationService::image_statistics(const Ref<Image> &p_image) {
	if (p_image.is_null() || p_image->get_width() <= 0 || p_image->get_height() <= 0) {
		return Dictionary();
	}
	Ref<Image> sample = p_image->duplicate();
	const int max_dimension = MAX(sample->get_width(), sample->get_height());
	if (max_dimension > 256) {
		const double scale = 256.0 / (double)max_dimension;
		sample->resize(MAX(1, (int)(sample->get_width() * scale)), MAX(1, (int)(sample->get_height() * scale)), Image::INTERPOLATE_LANCZOS);
	}
	const int64_t pixel_count = (int64_t)sample->get_width() * sample->get_height();
	double sum = 0.0;
	double sum_squares = 0.0;
	double saturation_sum = 0.0;
	double channel_sums[3] = {};
	double region_sums[9] = {};
	int64_t region_counts[9] = {};
	int64_t near_black = 0;
	int64_t near_white = 0;
	int histogram[256] = {};
	for (int y = 0; y < sample->get_height(); y++) {
		for (int x = 0; x < sample->get_width(); x++) {
			const Color color = sample->get_pixel(x, y);
			const double luminance = 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b;
			channel_sums[0] += color.r;
			channel_sums[1] += color.g;
			channel_sums[2] += color.b;
			const int region_x = MIN(2, x * 3 / sample->get_width());
			const int region_y = MIN(2, y * 3 / sample->get_height());
			const int region = region_y * 3 + region_x;
			region_sums[region] += luminance;
			region_counts[region]++;
			sum += luminance;
			sum_squares += luminance * luminance;
			saturation_sum += color.get_s();
			histogram[CLAMP((int)Math::round(luminance * 255.0), 0, 255)]++;
			near_black += luminance <= 0.01;
			near_white += luminance >= 0.99;
		}
	}
	const double mean = pixel_count > 0 ? sum / pixel_count : 0.0;
	const double variance = pixel_count > 0 ? MAX(0.0, sum_squares / pixel_count - mean * mean) : 0.0;
	Dictionary statistics;
	statistics["sample_width"] = sample->get_width();
	statistics["sample_height"] = sample->get_height();
	statistics["mean_luminance"] = mean;
	statistics["luminance_stddev"] = Math::sqrt(variance);
	statistics["mean_saturation"] = pixel_count > 0 ? saturation_sum / pixel_count : 0.0;
	statistics["near_black_fraction"] = pixel_count > 0 ? (double)near_black / pixel_count : 0.0;
	statistics["near_white_fraction"] = pixel_count > 0 ? (double)near_white / pixel_count : 0.0;
	const double channel_total = channel_sums[0] + channel_sums[1] + channel_sums[2];
	Array chromaticity;
	for (int channel = 0; channel < 3; channel++) {
		chromaticity.push_back(channel_total > 0.0 ? channel_sums[channel] / channel_total : 0.0);
	}
	statistics["mean_rgb_chromaticity"] = chromaticity;
	Array regions;
	for (int region = 0; region < 9; region++) {
		regions.push_back(region_counts[region] > 0 ? region_sums[region] / region_counts[region] : 0.0);
	}
	statistics["region_luminance_3x3"] = regions;
	const int percentile_ranks[3] = { 5, 50, 95 };
	const char *percentile_names[3] = { "luminance_p05", "luminance_p50", "luminance_p95" };
	for (int percentile = 0; percentile < 3; percentile++) {
		const int target = pixel_count > 0 ? (int)(((int64_t)percentile_ranks[percentile] * (pixel_count - 1)) / 100) : 0;
		int cumulative = 0;
		int value = 0;
		for (; value < 256; value++) {
			cumulative += histogram[value];
			if (cumulative > target) {
				break;
			}
		}
		statistics[percentile_names[percentile]] = (double)MIN(value, 255) / 255.0;
	}
	// Compress the 256-bin histogram into 8 equal exposure bands so the model
	// gets a structured ladder without inventing bedroom-specific thresholds.
	Array exposure_bands;
	exposure_bands.resize(8);
	if (pixel_count > 0) {
		for (int band = 0; band < 8; band++) {
			int64_t band_count = 0;
			const int start = band * 32;
			const int end = start + 32;
			for (int value = start; value < end; value++) {
				band_count += histogram[value];
			}
			exposure_bands[band] = (double)band_count / (double)pixel_count;
		}
	} else {
		for (int band = 0; band < 8; band++) {
			exposure_bands[band] = 0.0;
		}
	}
	statistics["exposure_bands_8"] = exposure_bands;
	return statistics;
}

Dictionary SolersObservationService::_capture_image(const Ref<Image> &p_image, const String &p_target, const String &p_capture_id) {
	if (p_image.is_null() || p_image->get_width() <= 1 || p_image->get_height() <= 1) {
		return _capture_error("VIEWPORT_IMAGE_UNAVAILABLE", "The viewport has not produced a readable frame.", true);
	}

	Ref<Image> image = p_image;
	const int max_dimension = MAX(image->get_width(), image->get_height());
	if (max_dimension > SOLERS_CAPTURE_MAX_DIMENSION) {
		image = p_image->duplicate();
		const double scale = (double)SOLERS_CAPTURE_MAX_DIMENSION / (double)max_dimension;
		image->resize(MAX(1, (int)(image->get_width() * scale)), MAX(1, (int)(image->get_height() * scale)), Image::INTERPOLATE_LANCZOS);
	}

	const String capture_dir = ProjectSettings::get_singleton()->globalize_path(ProjectSettings::get_singleton()->get_project_data_path().path_join("solers/captures"));
	const Error directory_error = DirAccess::make_dir_recursive_absolute(capture_dir);
	if (directory_error != OK) {
		return _capture_error("CAPTURE_DIRECTORY_FAILED", vformat("Could not create the capture directory: %s", capture_dir), false);
	}
	const String temporary_path = capture_dir.path_join(p_capture_id + ".tmp.png");
	const Error save_error = image->save_png(temporary_path);
	if (save_error != OK) {
		return _capture_error("CAPTURE_SAVE_FAILED", vformat("Could not save the viewport capture: %s", temporary_path), false);
	}
	const String content_sha256 = FileAccess::get_sha256(temporary_path);
	const String absolute_path = capture_dir.path_join(content_sha256 + ".png");
	if (FileAccess::exists(absolute_path)) {
		DirAccess::remove_absolute(temporary_path);
	} else if (DirAccess::rename_absolute(temporary_path, absolute_path) != OK) {
		return _capture_error("CAPTURE_SAVE_FAILED", "Could not commit content-addressed viewport evidence.", false);
	}

	Dictionary attachment;
	attachment["id"] = p_capture_id;
	attachment["source"] = "tool_capture";
	attachment["target"] = p_target;
	attachment["type"] = "image";
	attachment["mime_type"] = "image/png";
	attachment["local_path"] = absolute_path;
	attachment["width"] = image->get_width();
	attachment["height"] = image->get_height();

	Dictionary data;
	data["status"] = "complete";
	data["target"] = p_target;
	data["capture_id"] = p_capture_id;
	data["width"] = image->get_width();
	data["height"] = image->get_height();
	data["visual_statistics"] = image_statistics(image);
	data["content_sha256"] = content_sha256;
	attachment["content_sha256"] = content_sha256;
	data["attachment"] = attachment;

	Array attachments;
	attachments.push_back(attachment);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	result["attachments"] = attachments;
	return result;
}

Dictionary SolersObservationService::_attach_render_receipt(Dictionary p_result, const Dictionary &p_pending) {
	if (!(bool)p_result.get("ok", false)) {
		return p_result;
	}
	Dictionary data = p_result.get("data", Dictionary());
	Dictionary receipt;
	const String target = data.get("target", p_pending.get("target", String()));
	const String capture_id = data.get("capture_id", p_pending.get("capture_id", String()));
	receipt["capture_id"] = capture_id;
	receipt["target"] = target;
	receipt["render_sequence"] = p_pending.get("render_sequence", 0);
	receipt["runtime_epoch"] = p_pending.get("runtime_epoch", 0);
	receipt["requested_frame"] = p_pending.get("start_frame", 0);
	receipt["captured_frame"] = (int64_t)Engine::get_singleton()->get_frames_drawn();
	receipt["requested_post_draw_sequence"] = p_pending.get("start_post_draw", 0);
	receipt["captured_post_draw_sequence"] = (int64_t)render_post_draw_sequence;
	receipt["viewport_object_id"] = p_pending.get("viewport_id", 0);
	receipt["viewport_rid"] = p_pending.get("viewport_rid", 0);
	receipt["source_state"] = p_pending.get("source_state", Dictionary());
	receipt["image_sha256"] = data.get("content_sha256", String());

	const Dictionary source_state = receipt.get("source_state", Dictionary());
	String view_key = target + ":" + String(source_state.get("scene_path", String()));
	for (const char *field : { "camera_path", "view_spec_hash" }) {
		if (p_pending.has(field)) {
			view_key += ":" + String(p_pending[field]);
		}
	}
	if (target == "editor") {
		view_key += ":" + String::num_int64((int64_t)p_pending.get("viewport_rid", 0));
	} else if (target == "runtime") {
		view_key += ":" + String::num_int64((int64_t)p_pending.get("runtime_epoch", 0));
	}
	receipt["view_key"] = view_key.sha256_text();
	const Dictionary *previous = last_render_by_view.getptr(view_key);
	const bool same_pixels = previous && String(previous->get("image_sha256", String())) == String(receipt.get("image_sha256", String()));
	receipt["same_pixels"] = same_pixels;
	if (same_pixels) {
		receipt["same_as"] = previous->get("capture_id", String());
		receipt["same_as_source_state"] = previous->get("source_state", Dictionary());
	}
	Dictionary current;
	current["capture_id"] = capture_id;
	current["image_sha256"] = receipt.get("image_sha256", String());
	current["source_state"] = receipt.get("source_state", Dictionary());
	last_render_by_view[view_key] = current;
	data["render_receipt"] = receipt;
	p_result["data"] = data;
	return p_result;
}

Dictionary SolersObservationService::_editor_3d_viewport_state() const {
	Dictionary result;
	Node3DEditor *editor_3d = Node3DEditor::get_singleton();
	Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
	Viewport *viewport = editor_viewport ? editor_viewport->get_viewport_node() : nullptr;
	result["available"] = viewport != nullptr;
	if (!viewport) {
		return result;
	}
	const Viewport::DebugDraw debug_draw = viewport->get_debug_draw();
	result["state"] = editor_viewport->get_state();
	result["debug_draw"] = (int)debug_draw;
	result["material_preview"] = debug_draw == Viewport::DEBUG_DRAW_DISABLED;
	Array containing_geometry;
	Camera3D *camera = editor_viewport->get_camera_3d();
	EditorNode *editor = EditorNode::get_singleton();
	Node *edited_root = editor && EditorNode::get_editor_data().get_edited_scene_count() > 0 ? editor->get_edited_scene() : nullptr;
	if (camera && edited_root) {
		result["camera_position"] = _solers_vector3_array(camera->get_global_position());
		_solers_collect_geometry_containing_point(edited_root, edited_root, camera->get_global_position(), containing_geometry);
	}
	result["camera_inside_geometry"] = containing_geometry;
	result["camera_inside_geometry_is_aabb_test"] = true;
	result["frame_valid"] = debug_draw == Viewport::DEBUG_DRAW_DISABLED;
	if (debug_draw != Viewport::DEBUG_DRAW_DISABLED) {
		result["verification_warning"] = "The editor viewport is using a debug display mode, so this image is not valid evidence of final material appearance. Switch View > Display Normal and capture again.";
	} else if (!containing_geometry.is_empty()) {
		result["verification_warning"] = "One or more geometry AABBs contain the editor camera. This is a conservative overlap warning, not proof that the camera is inside a solid surface; inspect the capture before accepting it.";
	}
	return result;
}

void SolersObservationService::_runtime_screenshot_ready(int64_t p_width, int64_t p_height, const String &p_path, const Rect2i &p_rect, const String &p_capture_id) {
	const Dictionary *entry = pending_captures.getptr(p_capture_id);
	// The capture may have been swept at its deadline, or belong to a runtime
	// that stopped and restarted since the request went out. Either way this
	// frame is evidence for nothing anyone is still waiting on, and writing it
	// back would resurrect a dead entry.
	const Dictionary pending_data = entry ? Dictionary(entry->get("data", Dictionary())) : Dictionary();
	const uint64_t requested_epoch = (uint64_t)(int64_t)pending_data.get("runtime_epoch", 0);
	if (!entry || requested_epoch != runtime_epoch) {
		if (!p_path.is_empty()) {
			DirAccess::remove_absolute(p_path);
		}
		return;
	}
	Ref<Image> image = p_path.is_empty() ? Ref<Image>() : Image::load_from_file(p_path);
	if (!p_path.is_empty()) {
		DirAccess::remove_absolute(p_path);
	}
	// A truncated or compressed payload cannot be sampled: get_pixel and resize
	// both fail hard on those, so it is rejected as evidence right here.
	if (image.is_null() || image->is_empty() || image->is_compressed()) {
		pending_captures[p_capture_id] = _capture_error("RUNTIME_CAPTURE_DECODE_FAILED", "The runtime returned an unreadable screenshot.", true);
		return;
	}
	Dictionary result = _capture_image(image, "runtime", p_capture_id);
	if ((bool)result.get("ok", false)) {
		Dictionary data = result.get("data", Dictionary());
		data["frame_valid"] = true;
		result["data"] = data;
	}
	pending_captures[p_capture_id] = _attach_render_receipt(result, pending_data);
}

static void _solers_free_capture_viewport(const Dictionary &p_data) {
	if (!(bool)p_data.get("owns_viewport", false)) {
		return;
	}
	const int64_t viewport_id = p_data.get("viewport_id", 0);
	if (viewport_id == 0) {
		return;
	}
	if (SubViewport *viewport = Object::cast_to<SubViewport>(ObjectDB::get_instance(ObjectID((uint64_t)viewport_id)))) {
		viewport->queue_free();
	}
}

// All captures resolve asynchronously through the pending/poll contract: the
// image is grabbed only after the renderer has drawn a frame that postdates
// the request, so the model never receives a stale pre-edit frame.
Dictionary SolersObservationService::_register_pending_capture(const String &p_target, const Dictionary &p_extra) {
	// Sweep abandoned entries (an aborted turn may never poll its capture) so
	// the map and any transient viewports they hold cannot accumulate.
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	LocalVector<String> expired;
	for (const KeyValue<String, Dictionary> &entry : pending_captures) {
		const Dictionary data = entry.value.get("data", Dictionary());
		if (String(data.get("status", String())) == "pending" && now >= (uint64_t)(int64_t)data.get("deadline_msec", 0)) {
			_solers_free_capture_viewport(data);
			expired.push_back(entry.key);
		}
	}
	for (const String &key : expired) {
		pending_captures.erase(key);
	}

	const String capture_id = vformat("%s_%d", p_target, ++capture_sequence);
	Dictionary data;
	data["status"] = "pending";
	data["render_sequence"] = (int64_t)capture_sequence;
	data["target"] = p_target;
	data["capture_id"] = capture_id;
	data["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + SOLERS_CAPTURE_TIMEOUT_MSEC);
	// Which runtime this capture belongs to: a screenshot that arrives after a
	// restart answers a question about a process that no longer exists.
	data["runtime_epoch"] = (int64_t)runtime_epoch;
	for (const Variant *key = p_extra.next(nullptr); key; key = p_extra.next(key)) {
		data[*key] = p_extra[*key];
	}
	if (p_target == "runtime") {
		Dictionary source_state;
		source_state["runtime_epoch"] = (int64_t)runtime_epoch;
		data["source_state"] = source_state;
	} else if (p_target == "editor") {
		Node3DEditor *editor_3d = Node3DEditor::get_singleton();
		Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
		Viewport *viewport = editor_viewport ? editor_viewport->get_viewport_node() : nullptr;
		if (viewport) {
			data["viewport_id"] = (int64_t)(uint64_t)viewport->get_instance_id();
			data["viewport_rid"] = (int64_t)viewport->get_viewport_rid().get_id();
		}
	}
	if (p_target != "runtime") {
		if (p_target == "editor") {
			Node3DEditor *editor_3d = Node3DEditor::get_singleton();
			Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
			Viewport *viewport = editor_viewport ? editor_viewport->get_viewport_node() : nullptr;
			const Ref<World3D> world = viewport ? viewport->find_world_3d() : Ref<World3D>();
			const Ref<Environment> environment = _solers_capture_environment(world);
			data["sdfgi_enabled"] = environment.is_valid() && environment->is_sdfgi_enabled();
			data["render_frames_required"] = _solers_capture_settle_frames(world);
		}
		const uint64_t start_frame = Engine::get_singleton()->get_frames_drawn();
		const int render_frames = MAX(1, (int)data.get("render_frames_required", 1));
		data["start_frame"] = (int64_t)start_frame;
		data["ready_frame"] = (int64_t)(start_frame + render_frames);
		data["start_post_draw"] = (int64_t)render_post_draw_sequence;
		data["ready_post_draw"] = (int64_t)(render_post_draw_sequence + render_frames);
		_solers_request_editor_redraw();
	}
	Dictionary poll_args;
	poll_args["target"] = p_target;
	poll_args["capture_id"] = capture_id;
	data["poll_args"] = poll_args;
	Dictionary pending;
	pending["ok"] = true;
	pending["data"] = data;
	pending_captures[capture_id] = pending;
	return pending;
}

Dictionary SolersObservationService::_finish_frame_gated_capture(const String &p_capture_id, const Dictionary &p_data) {
	const String target = p_data.get("target", String());
	if (!_solers_capture_source_is_current(p_data.get("source_state", Dictionary()))) {
		_solers_free_capture_viewport(p_data);
		return _capture_error("CAPTURE_SOURCE_CHANGED", "The edited scene changed while the requested viewport frame was rendering.", true);
	}
	if (target == "editor") {
		Node3DEditor *editor_3d = Node3DEditor::get_singleton();
		Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
		if (!editor_viewport || !editor_viewport->get_viewport_node()) {
			return _capture_error("EDITOR_VIEWPORT_UNAVAILABLE", "The active 3D editor viewport is unavailable.", true);
		}
		Dictionary result = _capture_image(editor_viewport->get_viewport_node()->get_texture()->get_image(), "editor", p_capture_id);
		if ((bool)result.get("ok", false)) {
			Dictionary data = result.get("data", Dictionary());
			const Dictionary viewport_state = _editor_3d_viewport_state();
			const int64_t frames_waited = (int64_t)Engine::get_singleton()->get_frames_drawn() - (int64_t)p_data.get("start_frame", 0);
			const int64_t post_draws_waited = (int64_t)render_post_draw_sequence - (int64_t)p_data.get("start_post_draw", 0);
			const bool settled = post_draws_waited >= (int64_t)p_data.get("render_frames_required", 1);
			data["editor_viewport"] = viewport_state;
			data["material_preview"] = viewport_state.get("material_preview", false);
			data["frame_valid"] = settled && (bool)viewport_state.get("frame_valid", false);
			data["render_frames_required"] = p_data.get("render_frames_required", 1);
			data["render_frames_waited"] = frames_waited;
			data["render_post_draws_waited"] = post_draws_waited;
			data["sdfgi_enabled"] = p_data.get("sdfgi_enabled", false);
			result["data"] = data;
		}
		return _attach_render_receipt(result, p_data);
	}

	// camera / top_down render into a transient SubViewport that shares the
	// edited scene's World3D.
	const int64_t viewport_id = p_data.get("viewport_id", 0);
	SubViewport *viewport = Object::cast_to<SubViewport>(ObjectDB::get_instance(ObjectID((uint64_t)viewport_id)));
	if (!viewport) {
		return _capture_error("CAPTURE_VIEWPORT_LOST", "The transient capture viewport was freed before the frame rendered.", true);
	}
	viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
	Dictionary result = _capture_image(viewport->get_texture()->get_image(), target, p_capture_id);
	viewport->queue_free();
	if ((bool)result.get("ok", false)) {
		Dictionary data = result.get("data", Dictionary());
		const int64_t frames_waited = (int64_t)Engine::get_singleton()->get_frames_drawn() - (int64_t)p_data.get("start_frame", 0);
		const int64_t post_draws_waited = (int64_t)render_post_draw_sequence - (int64_t)p_data.get("start_post_draw", 0);
		const bool settled = post_draws_waited >= (int64_t)p_data.get("render_frames_required", 1);
		data["material_preview"] = true;
		data["frame_valid"] = settled;
		data["render_frames_required"] = p_data.get("render_frames_required", 1);
		data["render_frames_waited"] = frames_waited;
		data["render_post_draws_waited"] = post_draws_waited;
		data["sdfgi_enabled"] = p_data.get("sdfgi_enabled", false);
		for (const char *key : { "camera_path", "camera_forward_hit_node", "camera_forward_hit_distance", "orientation", "axis", "direction", "section_position", "focus_paths", "view_spec_hash" }) {
			if (p_data.has(key)) {
				data[key] = p_data[key];
			}
		}
		if (data.has("camera_forward_hit_node") || data.has("camera_forward_hit_distance")) {
			Dictionary framing;
			framing["camera_path"] = data.get("camera_path", String());
			framing["hit_node"] = data.get("camera_forward_hit_node", String());
			framing["hit_distance"] = data.get("camera_forward_hit_distance", 0.0);
			data["framing"] = framing;
		}
		result["data"] = data;
	}
	return _attach_render_receipt(result, p_data);
}

Dictionary SolersObservationService::_poll_pending_capture(const String &p_capture_id) {
	Dictionary *entry = pending_captures.getptr(p_capture_id);
	if (!entry) {
		return _capture_error("CAPTURE_NOT_FOUND", vformat("Unknown capture_id: %s", p_capture_id), true);
	}
	Dictionary result = *entry;
	Dictionary data = result.get("data", Dictionary());
	if (!(bool)result.get("ok", false) || String(data.get("status", String())) == "complete") {
		pending_captures.erase(p_capture_id);
		return result;
	}

	const String target = data.get("target", String());
	const bool deadline_reached = OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)data.get("deadline_msec", 0);

	if (target == "runtime" && (bool)data.get("awaiting_runtime_ready", false)) {
		if (_is_runtime_visual_ready()) {
			data["awaiting_runtime_ready"] = false;
			// The epoch is stamped when the request actually goes out, not when
			// the capture was parked: the runtime only started just now.
			data["runtime_epoch"] = (int64_t)runtime_epoch;
			Dictionary source_state;
			source_state["runtime_epoch"] = (int64_t)runtime_epoch;
			data["source_state"] = source_state;
			result["data"] = data;
			pending_captures[p_capture_id] = result;
			if (!_request_runtime_screenshot(p_capture_id)) {
				pending_captures.erase(p_capture_id);
				return _runtime_capture_unavailable();
			}
			entry = pending_captures.getptr(p_capture_id);
			return entry ? *entry : result;
		}
		if (deadline_reached) {
			pending_captures.erase(p_capture_id);
			return _capture_error("CAPTURE_TIMEOUT", "The runtime viewport was not visually ready before the capture deadline.", true);
		}
		return result;
	}

	if (target != "runtime" && render_post_draw_sequence >= (uint64_t)(int64_t)data.get("ready_post_draw", 0)) {
		const Dictionary final_result = _finish_frame_gated_capture(p_capture_id, data);
		pending_captures.erase(p_capture_id);
		return final_result;
	}
	if (deadline_reached) {
		_solers_free_capture_viewport(data);
		pending_captures.erase(p_capture_id);
		return _capture_error("CAPTURE_TIMEOUT", vformat("The %s viewport did not produce a frame before the capture deadline.", target), true);
	}
	if (target != "runtime") {
		_solers_request_editor_redraw();
	}
	return result;
}

Dictionary SolersObservationService::_begin_scene_view_capture(const String &p_target, const Dictionary &p_args) {
	Node3DEditor *editor_3d = Node3DEditor::get_singleton();
	Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
	Viewport *world_viewport = editor_viewport ? editor_viewport->get_viewport_node() : nullptr;
	if (!world_viewport) {
		return _capture_error("EDITOR_VIEWPORT_UNAVAILABLE", "The 3D editor viewport (and its World3D) is unavailable.", true);
	}
	Node *edited_root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!edited_root) {
		return _capture_error("NO_EDITED_SCENE", "No scene is being edited.", true);
	}

	Dictionary extra;
	Vector3 orthographic_target;
	Ref<Environment> camera_environment;
	SubViewport *viewport = memnew(SubViewport);
	viewport->set_name("SolersCaptureViewport");
	const Ref<World3D> world = world_viewport->find_world_3d();
	viewport->set_world_3d(world);
	viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
	Camera3D *camera = memnew(Camera3D);
	viewport->add_child(camera);

	if (p_target == "camera") {
		Camera3D *scene_camera = nullptr;
		const String node_path = String(p_args.get("node_path", String())).strip_edges();
		if (!node_path.is_empty()) {
			scene_camera = Object::cast_to<Camera3D>(edited_root->get_node_or_null(NodePath(node_path)));
			if (!scene_camera) {
				memdelete(viewport);
				PackedStringArray camera_paths;
				_solers_collect_camera_paths(edited_root, edited_root, camera_paths);
				return _capture_error("CAMERA_NOT_FOUND", vformat("No Camera3D at node_path: %s. %s Camera3D nodes present: %s.", node_path, _solers_scene_path_hint(edited_root), camera_paths.is_empty() ? String("none") : String(", ").join(camera_paths)), true);
			}
		} else {
			scene_camera = _solers_find_first_camera(edited_root);
			if (!scene_camera) {
				memdelete(viewport);
				return _capture_error("NO_CAMERA_IN_SCENE", "The edited scene has no Camera3D. Add one or pass node_path.", true);
			}
		}
		int width = GLOBAL_GET("display/window/size/viewport_width");
		int height = GLOBAL_GET("display/window/size/viewport_height");
		if (width <= 0 || height <= 0) {
			width = 1152;
			height = 648;
		}
		const int longest = MAX(width, height);
		if (longest > SOLERS_CAPTURE_MAX_DIMENSION) {
			width = MAX(1, width * SOLERS_CAPTURE_MAX_DIMENSION / longest);
			height = MAX(1, height * SOLERS_CAPTURE_MAX_DIMENSION / longest);
		}
		viewport->set_size(Size2i(width, height));
		camera->set_projection(scene_camera->get_projection());
		camera->set_fov(scene_camera->get_fov());
		camera->set_size(scene_camera->get_size());
		camera->set_near(scene_camera->get_near());
		camera->set_far(scene_camera->get_far());
		camera->set_keep_aspect_mode(scene_camera->get_keep_aspect_mode());
		camera->set_cull_mask(scene_camera->get_cull_mask());
		camera->set_environment(scene_camera->get_environment());
		camera_environment = scene_camera->get_environment();
		camera->set_attributes(scene_camera->get_attributes());
		camera->set_transform(scene_camera->get_global_transform());
		extra["camera_path"] = String(edited_root->get_path_to(scene_camera));

		const Vector3 origin = scene_camera->get_global_position();
		const Vector3 forward = -scene_camera->get_global_transform().basis.get_column(2).normalized();
		real_t hit_distance = -1;
		String hit_path;
		_solers_collect_ray_hit(edited_root, edited_root, origin, forward, hit_distance, hit_path);
		if (hit_distance > 0) {
			extra["camera_forward_hit_node"] = hit_path;
			extra["camera_forward_hit_distance"] = hit_distance;
		}
	} else { // top_down / orthographic
		AABB bounds;
		bool found = false;
		const Array focus_paths = p_args.get("focus_paths", Array());
		if (focus_paths.is_empty()) {
			_solers_accumulate_world_aabb(edited_root, bounds, found);
		} else {
			for (int i = 0; i < focus_paths.size(); i++) {
				const String focus_path = String(focus_paths[i]).strip_edges();
				Node *focus = edited_root->get_node_or_null(NodePath(focus_path));
				if (!focus) {
					memdelete(viewport);
					return _capture_error("FOCUS_NODE_NOT_FOUND", vformat("No edited-scene node at focus_paths[%d]: %s. %s", i, focus_path, _solers_scene_path_hint(edited_root)), true);
				}
				_solers_accumulate_world_aabb(focus, bounds, found);
			}
		}
		if (!found) {
			// Nodes rendering through RenderingServer directly (for example
			// GDExtension terrain) are invisible to the node AABB walk, so
			// frame a default volume and let the caller judge the image.
			bounds = AABB(Vector3(-64, -16, -64), Vector3(128, 96, 128));
		}
		extra["bounds_source"] = found ? "visible_geometry" : "default_volume";
		String axis_name = p_target == "top_down" ? String("y") : String(p_args.get("axis", String())).to_lower();
		String direction_name = p_target == "top_down" ? String("positive") : String(p_args.get("direction", String())).to_lower();
		const int axis = axis_name == "x" ? Vector3::AXIS_X : (axis_name == "y" ? Vector3::AXIS_Y : (axis_name == "z" ? Vector3::AXIS_Z : -1));
		if (axis < 0 || (direction_name != "positive" && direction_name != "negative")) {
			memdelete(viewport);
			return _capture_error("INVALID_ORTHOGRAPHIC_VIEW", "orthographic capture requires axis=x|y|z and direction=positive|negative.", true);
		}
		viewport->set_size(Size2i(1024, 1024));
		orthographic_target = bounds.get_center();
		const int perpendicular_a = (axis + 1) % 3;
		const int perpendicular_b = (axis + 2) % 3;
		const real_t span = MAX(bounds.size[perpendicular_a], bounds.size[perpendicular_b]);
		const real_t distance = bounds.size[axis] + MAX(span, (real_t)1.0);
		Vector3 view_axis;
		view_axis[axis] = direction_name == "positive" ? 1.0 : -1.0;
		Vector3 camera_position = orthographic_target + view_axis * distance;
		if (p_args.has("section_position")) {
			const Variant section = p_args["section_position"];
			if (section.get_type() != Variant::FLOAT && section.get_type() != Variant::INT) {
				memdelete(viewport);
				return _capture_error("INVALID_SECTION_POSITION", "section_position must be a world-space number on the view axis.", true);
			}
			camera_position[axis] = (real_t)section + view_axis[axis] * (real_t)0.05;
			extra["section_position"] = section;
		}
		camera->set_orthogonal(MAX(span * (real_t)1.15, (real_t)1.0), 0.05, distance * 2.0 + 10.0);
		camera->set_transform(Transform3D(Basis(), camera_position));
		extra["axis"] = axis_name;
		extra["direction"] = direction_name;
		extra["focus_paths"] = focus_paths;
		extra["view_spec_hash"] = String::num_uint64(p_args.hash());
		extra["orientation"] = axis == Vector3::AXIS_Y ? String("Orthographic top view. Image up = -Z, image right = +X.") : vformat("Orthographic %s-axis view. Image up = +Y.", axis_name.to_upper());
	}
	camera->set_current(true);
	const Ref<Environment> environment = _solers_capture_environment(world, camera_environment);
	extra["sdfgi_enabled"] = environment.is_valid() && environment->is_sdfgi_enabled();
	extra["render_frames_required"] = _solers_capture_settle_frames(world, camera_environment);

	EditorNode *editor_node = EditorNode::get_singleton();
	if (!editor_node) {
		memdelete(viewport);
		return _capture_error("EDITOR_UNAVAILABLE", "EditorNode is unavailable.", false);
	}
	editor_node->add_child(viewport);
	if (p_target == "top_down" || p_target == "orthographic") {
		// look_at needs a valid global basis, so orient after entering the tree.
		const Vector3 up = String(extra.get("axis", String())) == "y" ? Vector3(0, 0, -1) : Vector3(0, 1, 0);
		camera->look_at_from_position(camera->get_global_position(), orthographic_target, up);
	}
	extra["viewport_id"] = (int64_t)(uint64_t)viewport->get_instance_id();
	extra["owns_viewport"] = true;
	extra["viewport_rid"] = (int64_t)viewport->get_viewport_rid().get_id();
	if (p_args.has("_source_state")) {
		extra["source_state"] = p_args["_source_state"];
	}
	return _register_pending_capture(p_target, extra);
}

Dictionary SolersObservationService::capture_viewport(const Dictionary &p_args) {
	const String requested_id = String(p_args.get("capture_id", String())).strip_edges();
	if (!requested_id.is_empty()) {
		return _poll_pending_capture(requested_id);
	}

	const String target = String(p_args.get("target", String())).strip_edges().to_lower();
	if (target == "editor") {
		EditorNode *editor_node = EditorNode::get_singleton();
		if (!editor_node || editor_node->get_editor_main_screen()->get_selected_index() != EditorMainScreen::EDITOR_3D) {
			return _capture_error("EDITOR_3D_NOT_ACTIVE", "The 3D editor must be the active main screen before capturing it.", true);
		}
		Node3DEditor *editor_3d = Node3DEditor::get_singleton();
		Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
		if (!editor_viewport || !editor_viewport->get_viewport_node()) {
			return _capture_error("EDITOR_VIEWPORT_UNAVAILABLE", "The active 3D editor viewport is unavailable.", true);
		}
		Dictionary extra;
		if (p_args.has("_source_state")) {
			extra["source_state"] = p_args["_source_state"];
		}
		return _register_pending_capture("editor", extra);
	}
	if (target == "camera" || target == "top_down" || target == "orthographic") {
		return _begin_scene_view_capture(target, p_args);
	}
	if (target == "runtime") {
		if (!_is_runtime_visual_ready()) {
			Dictionary extra;
			extra["awaiting_runtime_ready"] = true;
			extra["runtime"] = get_runtime_status();
			return _register_pending_capture("runtime", extra);
		}
		const Dictionary pending = _register_pending_capture("runtime", Dictionary());
		const String capture_id = Dictionary(pending.get("data", Dictionary())).get("capture_id", String());
		if (!_request_runtime_screenshot(capture_id)) {
			pending_captures.erase(capture_id);
			return _runtime_capture_unavailable();
		}
		return pending;
	}
	return _capture_error("INVALID_CAPTURE_TARGET", "target must be one of 'editor', 'runtime', 'camera', 'top_down', or 'orthographic'.", true);
}

Dictionary SolersObservationService::poll_viewport_capture(const Dictionary &p_args) {
	const String capture_id = String(p_args.get("capture_id", String())).strip_edges();
	if (capture_id.is_empty()) {
		return _capture_error("INVALID_CAPTURE_CONTINUATION", "capture_id is required to continue a viewport capture.", false);
	}
	return _poll_pending_capture(capture_id);
}

bool SolersObservationService::is_viewport_capture_ready(const Dictionary &p_args) {
	const String capture_id = String(p_args.get("capture_id", String())).strip_edges();
	const Dictionary *entry = pending_captures.getptr(capture_id);
	if (!entry) {
		return true;
	}
	const Dictionary data = entry->get("data", Dictionary());
	if (!(bool)entry->get("ok", false) || String(data.get("status", String())) != "pending") {
		return true;
	}
	if (OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)data.get("deadline_msec", 0)) {
		return true;
	}
	const String target = data.get("target", String());
	if (target == "runtime") {
		// Wake the poll loop when readiness becomes true so the screenshot
		// request can start; after that, wait for the callback or deadline.
		return (bool)data.get("awaiting_runtime_ready", false) && _is_runtime_visual_ready();
	}
	const bool ready = render_post_draw_sequence >= (uint64_t)(int64_t)data.get("ready_post_draw", 0);
	if (!ready) {
		_solers_request_editor_redraw();
	}
	return ready;
}

bool SolersObservationService::_normalize_project_path(const String &p_path, String &r_res_path, String &r_error) const {
	String path = p_path.strip_edges().replace_char('\\', '/');
	if (path.is_empty()) {
		r_error = "Path is empty.";
		return false;
	}

	if (path.is_absolute_path() && !path.begins_with("res://")) {
		r_error = "Only res:// or project-relative paths are allowed.";
		return false;
	}

	if (!path.begins_with("res://")) {
		path = String("res://").path_join(path);
	}

	path = path.simplify_path();
	if (!path.begins_with("res://") || path.contains("..")) {
		r_error = "Path escapes the project root.";
		return false;
	}

	r_res_path = path;
	return true;
}

bool SolersObservationService::_collect_project_folders_indexed(const String &p_query, int p_max_folders, Array &r_folders, int &r_scanned_count, bool &r_truncated) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return false;
	}
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (!efs || !efs->get_filesystem() || efs->is_scanning()) {
		return false;
	}
	EditorFileSystemDirectory *root = efs->get_filesystem();
	LocalVector<EditorFileSystemDirectory *> stack;
	stack.push_back(root);
	while (!stack.is_empty()) {
		EditorFileSystemDirectory *dir = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		for (int i = 0; i < dir->get_subdir_count(); i++) {
			EditorFileSystemDirectory *sub = dir->get_subdir(i);
			if (!sub) {
				continue;
			}
			r_scanned_count++;
			const String path = sub->get_path();
			if (p_query.is_empty() || path.findn(p_query) != -1) {
				if (r_folders.size() >= p_max_folders) {
					r_truncated = true;
					return true;
				}
				r_folders.push_back(path);
			}
			stack.push_back(sub);
		}
	}
	return true;
}

void SolersObservationService::_collect_project_files(const String &p_dir, const String &p_query, int p_max_files, Array &r_files, int &r_scanned_count, bool &r_truncated, uint64_t p_deadline_msec) const {
	if (r_files.size() >= p_max_files) {
		r_truncated = true;
		return;
	}
	if (OS::get_singleton()->get_ticks_msec() >= p_deadline_msec) {
		r_truncated = true;
		return;
	}

	Error err = OK;
	Ref<DirAccess> dir = DirAccess::open(p_dir, &err);
	if (dir.is_null() || err != OK) {
		return;
	}

	dir->set_include_hidden(false);
	dir->list_dir_begin();
	LocalVector<String> subdirectories;
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (OS::get_singleton()->get_ticks_msec() >= p_deadline_msec) {
			r_truncated = true;
			break;
		}
		const String path = p_dir.path_join(entry).replace_char('\\', '/');
		if (dir->current_is_dir()) {
			if (entry != ".godot" && entry != ".git" && entry != "__pycache__") {
				subdirectories.push_back(path);
			}
		} else {
			r_scanned_count++;
			if (p_query.is_empty() || path.findn(p_query) != -1) {
				r_files.push_back(path);
				if (r_files.size() >= p_max_files) {
					r_truncated = true;
					break;
				}
			}
		}
		if (OS::get_singleton()->get_ticks_msec() >= p_deadline_msec) {
			r_truncated = true;
			break;
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();
	for (const String &path : subdirectories) {
		_collect_project_files(path, p_query, p_max_files, r_files, r_scanned_count, r_truncated, p_deadline_msec);
		if (r_truncated) {
			break;
		}
	}
}

// Nodes a subtree contributes, so an elided branch reports how much of the
// scene the model is not seeing rather than only its immediate child count.
static int _solers_subtree_node_count(Node *p_node) {
	if (!p_node) {
		return 0;
	}
	int total = 1;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		total += _solers_subtree_node_count(p_node->get_child(i));
	}
	return total;
}

Dictionary SolersObservationService::_serialize_node(Node *p_node, Node *p_edited_root, int p_depth, int p_max_depth, int p_max_children_per_node, int &r_token_budget, int &r_elided_nodes) const {
	Dictionary node_data;
	if (!p_node) {
		node_data["valid"] = false;
		return node_data;
	}

	node_data["valid"] = true;
	node_data["name"] = p_node->get_name();
	node_data["type"] = p_node->get_class();
	node_data["path"] = p_node->is_inside_tree() ? String(p_node->get_path()) : String(p_node->get_name());
	node_data["scene_file_path"] = p_node->get_scene_file_path();
	node_data["child_count"] = p_node->get_child_count();
	const Ref<Script> script = p_node->get_script();
	if (script.is_valid()) {
		node_data["script_path"] = script->get_path();
	}
	if (Node3D *node_3d = Object::cast_to<Node3D>(p_node)) {
		Dictionary transform;
		transform["position"] = _solers_vector3_array(node_3d->get_position());
		transform["global_position"] = _solers_vector3_array(node_3d->get_global_position());
		transform["rotation_degrees"] = _solers_vector3_array(node_3d->get_rotation_degrees());
		transform["global_rotation_degrees"] = _solers_vector3_array(node_3d->get_global_rotation_degrees());
		transform["scale"] = _solers_vector3_array(node_3d->get_scale());
		transform["visible"] = node_3d->is_visible();
		node_data["transform"] = transform;
	}
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		const AABB bounds = geometry->get_global_transform().xform(geometry->get_aabb());
		Dictionary global_aabb;
		global_aabb["position"] = _solers_vector3_array(bounds.position);
		global_aabb["size"] = _solers_vector3_array(bounds.size);
		node_data["global_aabb"] = global_aabb;
	}
	if (Camera3D *camera = Object::cast_to<Camera3D>(p_node)) {
		Array containing_geometry;
		const Vector3 origin = camera->get_global_position();
		const Vector3 forward = -camera->get_global_transform().basis.get_column(2).normalized();
		node_data["camera_forward"] = _solers_vector3_array(forward);
		if (p_edited_root) {
			_solers_collect_geometry_containing_point(p_edited_root, p_edited_root, origin, containing_geometry);
			real_t hit_distance = -1;
			String hit_path;
			_solers_collect_ray_hit(p_edited_root, p_edited_root, origin, forward, hit_distance, hit_path);
			if (hit_distance > 0) {
				node_data["camera_forward_hit_node"] = hit_path;
				node_data["camera_forward_hit_distance"] = hit_distance;
			}
		}
		node_data["camera_inside_geometry"] = containing_geometry;
	}
	Dictionary resource_properties;
	List<PropertyInfo> properties;
	p_node->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.type != Variant::OBJECT || !(property.usage & PROPERTY_USAGE_STORAGE) || property.name == SNAME("script")) {
			continue;
		}
		Resource *resource = Object::cast_to<Resource>(p_node->get(property.name));
		if (!resource) {
			continue;
		}
		Dictionary reference;
		reference["class_name"] = resource->get_class();
		reference["path"] = resource->get_path();
		resource_properties[property.name] = reference;
	}
	if (!resource_properties.is_empty()) {
		node_data["resource_properties"] = resource_properties;
	}

	if (p_edited_root) {
		if (p_node == p_edited_root) {
			node_data["relative_path"] = ".";
		} else if (p_edited_root->is_ancestor_of(p_node)) {
			node_data["relative_path"] = String(p_edited_root->get_path_to(p_node));
		}
	}

	Node *owner = p_node->get_owner();
	if (owner) {
		node_data["owner_path"] = owner->is_inside_tree() ? String(owner->get_path()) : String(owner->get_name());
	}

	// This node's own payload is charged before descending, so the budget is
	// spent on nodes that are actually reported in full.
	r_token_budget -= SolersContextManager::estimate_tokens(JSON::stringify(node_data, "", false, true));

	if (p_depth >= p_max_depth) {
		node_data["children_truncated_by_depth"] = p_node->get_child_count() > 0;
		return node_data;
	}

	Array children;
	const int child_count = p_node->get_child_count();
	const int max_children = MAX(0, p_max_children_per_node);
	const int child_limit = MIN(child_count, max_children);
	int serialized_children = 0;
	for (int i = 0; i < child_limit; i++) {
		if (r_token_budget <= 0) {
			break;
		}
		children.push_back(_serialize_node(p_node->get_child(i), p_edited_root, p_depth + 1, p_max_depth, p_max_children_per_node, r_token_budget, r_elided_nodes));
		serialized_children++;
	}
	node_data["children"] = children;
	if (child_count > serialized_children) {
		node_data["children_truncated_count"] = child_count - serialized_children;
		if (serialized_children < child_limit) {
			node_data["children_truncated_by_budget"] = true;
			for (int i = serialized_children; i < child_limit; i++) {
				r_elided_nodes += _solers_subtree_node_count(p_node->get_child(i));
			}
		}
	}

	return node_data;
}

Array SolersObservationService::_serialize_node_array(const TypedArray<Node> &p_nodes, Node *p_edited_root, int p_max_depth, int p_max_children_per_node) const {
	Array serialized;
	int token_budget = INT32_MAX;
	int elided_nodes = 0;
	for (int i = 0; i < p_nodes.size(); i++) {
		Node *node = Object::cast_to<Node>(p_nodes[i]);
		if (node) {
			serialized.push_back(_serialize_node(node, p_edited_root, 0, p_max_depth, p_max_children_per_node, token_budget, elided_nodes));
		}
	}
	return serialized;
}

Dictionary SolersObservationService::get_project_info() const {
	Dictionary info;
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(project_settings, info);

	const String project_name = GLOBAL_GET("application/config/name");
	const String main_scene = GLOBAL_GET("application/run/main_scene");

	info["name"] = project_name;
	info["resource_path"] = project_settings->get_resource_path();
	info["main_scene"] = main_scene;
	info["godot_version"] = VERSION_FULL_NAME;
	info["engine_distribution"] = "Solers Engine";
	info["project_settings_path"] = project_settings->get_resource_path().path_join("project.godot");
	return info;
}

Dictionary SolersObservationService::get_project_settings_summary() const {
	Dictionary summary;
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(project_settings, summary);

	summary["application/config/name"] = GLOBAL_GET("application/config/name");
	summary["application/run/main_scene"] = GLOBAL_GET("application/run/main_scene");
	summary["rendering/renderer/rendering_method"] = GLOBAL_GET("rendering/renderer/rendering_method");
	summary["rendering/lights_and_shadows/use_physical_light_units"] = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");
	summary["display/window/size/viewport_width"] = GLOBAL_GET("display/window/size/viewport_width");
	summary["display/window/size/viewport_height"] = GLOBAL_GET("display/window/size/viewport_height");
	summary["project_settings_path"] = project_settings->get_resource_path().path_join("project.godot");
	return summary;
}

// Disk-walk fallback budget: the editor index covers every normal case, so
// the fallback only runs during very early startup; it must never be able to
// freeze the editor regardless of project size.
static constexpr uint64_t SOLERS_FILE_WALK_BUDGET_MSEC = 250;
// Path lists are fed back into the model context; keep them bounded.
static constexpr int SOLERS_FILE_LIST_HARD_CAP = 2000;

Dictionary SolersObservationService::list_project_files(int p_max_files) const {
	Dictionary result;
	Array files;
	int scanned_count = 0;
	bool truncated = false;
	const int max_files = CLAMP(p_max_files, 1, SOLERS_FILE_LIST_HARD_CAP);
	_collect_project_files("res://", String(), max_files, files, scanned_count, truncated, OS::get_singleton()->get_ticks_msec() + SOLERS_FILE_WALK_BUDGET_MSEC);
	result["files"] = files;
	result["count"] = files.size();
	result["scanned_count"] = scanned_count;
	result["truncated"] = truncated;
	result["source"] = "disk_walk";
	return result;
}

Dictionary SolersObservationService::list_project_folders(int p_max_folders, const String &p_query) const {
	Dictionary result;
	Array folders;
	int scanned_count = 0;
	bool truncated = false;
	const int max_folders = CLAMP(p_max_folders, 1, SOLERS_FILE_LIST_HARD_CAP);
	const bool indexed = _collect_project_folders_indexed(p_query.strip_edges(), max_folders, folders, scanned_count, truncated);
	result["folders"] = folders;
	result["count"] = folders.size();
	result["scanned_count"] = scanned_count;
	result["truncated"] = truncated;
	result["source"] = indexed ? "editor_index" : "unavailable";
	return result;
}

Dictionary SolersObservationService::_search_project_paths(const String &p_query, int p_max_files) const {
	Dictionary result;
	const String query = p_query.strip_edges();
	if (query.is_empty()) {
		result = list_project_files(p_max_files);
		result["ok"] = true;
		result["query"] = String();
		result["mode"] = "list_all";
		return result;
	}

	Array files;
	int scanned_count = 0;
	bool truncated = false;
	const int max_files = CLAMP(p_max_files, 1, SOLERS_FILE_LIST_HARD_CAP);
	_collect_project_files("res://", query, max_files, files, scanned_count, truncated, OS::get_singleton()->get_ticks_msec() + SOLERS_FILE_WALK_BUDGET_MSEC);
	result["ok"] = true;
	result["query"] = query;
	result["files"] = files;
	result["count"] = files.size();
	result["scanned_count"] = scanned_count;
	result["truncated"] = truncated;
	result["source"] = "disk_walk";
	return result;
}

static bool _solers_text_search_extension(const String &p_extension) {
	return p_extension == "gd" || p_extension == "cs" || p_extension == "shader" || p_extension == "gdshader" ||
			p_extension == "tscn" || p_extension == "tres" || p_extension == "cfg" || p_extension == "json" ||
			p_extension == "xml" || p_extension == "md" || p_extension == "txt" || p_extension == "cpp" ||
			p_extension == "h" || p_extension == "py";
}

static bool _solers_identifier_character(char32_t p_character) {
	return p_character == '_' || (p_character >= '0' && p_character <= '9') || (p_character >= 'A' && p_character <= 'Z') ||
			(p_character >= 'a' && p_character <= 'z') || p_character >= 128;
}

static int _solers_find_text(const String &p_line, const String &p_query, bool p_symbol) {
	const String line = p_line.to_lower();
	const String query = p_query.to_lower();
	int from = 0;
	while (from <= line.length() - query.length()) {
		const int position = line.find(query, from);
		if (position < 0 || !p_symbol) {
			return position;
		}
		const bool left_boundary = position == 0 || !_solers_identifier_character(line[position - 1]);
		const int end = position + query.length();
		const bool right_boundary = end == line.length() || !_solers_identifier_character(line[end]);
		if (left_boundary && right_boundary) {
			return position;
		}
		from = position + 1;
	}
	return -1;
}

static Dictionary _solers_project_result(const String &p_path) {
	Dictionary result;
	result["path"] = p_path;
	return result;
}

static bool _solers_is_packed_scene_type(const String &p_resource_type) {
	return p_resource_type == "PackedScene";
}

Dictionary SolersObservationService::observe_path(const String &p_path) const {
	Dictionary result;
	String res_path;
	String path_error;
	if (!_normalize_project_path(p_path, res_path, path_error)) {
		result["ok"] = false;
		result["error"] = path_error;
		return result;
	}
	if (res_path != "res://" && res_path.ends_with("/")) {
		res_path = res_path.substr(0, res_path.length() - 1);
	}

	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	EditorFileSystemDirectory *efs_dir = (efs && !efs->is_scanning()) ? efs->get_filesystem_path(res_path) : nullptr;
	const bool is_directory = efs_dir != nullptr || DirAccess::open(res_path).is_valid();
	if (is_directory) {
		const int max_children = 48;
		Array children;
		Dictionary type_counts;
		int file_count = 0;
		int subdir_count = 0;
		bool truncated = false;

		if (efs_dir) {
			subdir_count = efs_dir->get_subdir_count();
			file_count = efs_dir->get_file_count();
			for (int i = 0; i < efs_dir->get_subdir_count(); i++) {
				EditorFileSystemDirectory *sub = efs_dir->get_subdir(i);
				if (!sub) {
					continue;
				}
				type_counts["directory"] = (int)type_counts.get("directory", 0) + 1;
				if (children.size() >= max_children) {
					truncated = true;
					continue;
				}
				Dictionary child;
				child["path"] = sub->get_path();
				child["kind"] = "directory";
				children.push_back(child);
			}
			for (int i = 0; i < efs_dir->get_file_count(); i++) {
				const String child_type = String(efs_dir->get_file_type(i));
				const String count_key = child_type.is_empty() ? String("file") : child_type;
				type_counts[count_key] = (int)type_counts.get(count_key, 0) + 1;
				if (children.size() >= max_children) {
					truncated = true;
					continue;
				}
				Dictionary child;
				const String child_path = efs_dir->get_file_path(i);
				child["path"] = child_path;
				child["kind"] = count_key;
				child["import_valid"] = efs_dir->get_file_import_is_valid(i);
				const ResourceUID::ID child_uid = ResourceLoader::get_resource_uid(child_path);
				if (child_uid != ResourceUID::INVALID_ID) {
					child["uid"] = ResourceUID::get_singleton()->id_to_text(child_uid);
				}
				child["dependency_count"] = efs_dir->get_file_deps(i).size();
				children.push_back(child);
			}
		} else {
			Ref<DirAccess> dir = DirAccess::open(res_path);
			if (dir.is_valid()) {
				dir->list_dir_begin();
				for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
					if (name == "." || name == "..") {
						continue;
					}
					const String child_path = res_path.path_join(name);
					if (dir->current_is_dir()) {
						subdir_count++;
						if (children.size() < max_children) {
							Dictionary child;
							child["path"] = child_path;
							child["kind"] = "directory";
							children.push_back(child);
						} else {
							truncated = true;
						}
						type_counts["directory"] = (int)type_counts.get("directory", 0) + 1;
					} else {
						file_count++;
						const String child_type = ResourceLoader::get_resource_type(child_path);
						const String kind = child_type.is_empty() ? String("file") : child_type;
						if (children.size() < max_children) {
							Dictionary child;
							child["path"] = child_path;
							child["kind"] = kind;
							children.push_back(child);
						} else {
							truncated = true;
						}
						type_counts[kind] = (int)type_counts.get(kind, 0) + 1;
					}
				}
			}
		}

		Dictionary digest;
		digest["kind"] = "directory";
		digest["path"] = res_path;
		digest["file_count"] = file_count;
		digest["subdir_count"] = subdir_count;
		digest["children"] = children;
		digest["type_counts"] = type_counts;
		digest["truncated"] = truncated || (file_count + subdir_count) > children.size();
		digest["summary"] = vformat("Directory with %d files and %d subdirectories.", file_count, subdir_count);
		result["ok"] = true;
		result["path"] = res_path;
		result["digest"] = digest;
		return result;
	}

	if (!FileAccess::exists(res_path)) {
		result["ok"] = false;
		result["error"] = vformat("Path does not exist.%s", solers_file_suggestions(res_path));
		result["path"] = res_path;
		return result;
	}

	const String resource_type = ResourceLoader::get_resource_type(res_path);
	if (_solers_is_packed_scene_type(resource_type)) {
		return digest_packed_scene(res_path, 96);
	}

	Dictionary digest;
	digest["kind"] = resource_type.is_empty() ? String("file") : resource_type;
	digest["path"] = res_path;
	if (!resource_type.is_empty()) {
		digest["resource_type"] = resource_type;
	}
	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(res_path);
	if (uid != ResourceUID::INVALID_ID) {
		digest["uid"] = ResourceUID::get_singleton()->id_to_text(uid);
	}
	digest["size_bytes"] = FileAccess::get_size(res_path);

	Array dependencies;
	List<String> dep_list;
	ResourceLoader::get_dependencies(res_path, &dep_list, true);
	const int max_deps = 32;
	int dep_i = 0;
	for (const String &raw_dependency : dep_list) {
		if (dep_i >= max_deps) {
			break;
		}
		dependencies.push_back(ResourceUID::ensure_path(raw_dependency.get_slice("::", 0)));
		dep_i++;
	}
	digest["dependencies"] = dependencies;
	digest["dependencies_truncated"] = dep_list.size() > dependencies.size();
	digest["summary"] = resource_type.is_empty()
			? vformat("File (%d bytes).", (int64_t)digest.get("size_bytes", 0))
			: vformat("%s resource (%d bytes, %d deps).", resource_type, (int64_t)digest.get("size_bytes", 0), dependencies.size());

	result["ok"] = true;
	result["path"] = res_path;
	result["digest"] = digest;
	return result;
}

Dictionary SolersObservationService::digest_packed_scene(const String &p_path, int p_max_nodes) const {
	Dictionary result;
	String res_path;
	String path_error;
	if (!_normalize_project_path(p_path, res_path, path_error)) {
		result["ok"] = false;
		result["error"] = path_error;
		return result;
	}
	const String resource_type = ResourceLoader::get_resource_type(res_path);
	if (!_solers_is_packed_scene_type(resource_type)) {
		result["ok"] = false;
		result["error"] = vformat("Not a PackedScene (resource_type=%s).", resource_type.is_empty() ? String("unknown") : resource_type);
		result["path"] = res_path;
		return result;
	}

	const Ref<PackedScene> scene = ResourceLoader::load(res_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_REUSE);
	if (scene.is_null()) {
		result["ok"] = false;
		result["error"] = "Unable to load PackedScene.";
		result["path"] = res_path;
		return result;
	}
	const Ref<SceneState> state = scene->get_state();
	if (state.is_null()) {
		result["ok"] = false;
		result["error"] = "PackedScene has no SceneState.";
		result["path"] = res_path;
		return result;
	}

	const int max_nodes = CLAMP(p_max_nodes, 1, 256);
	const int node_count = state->get_node_count();
	Array nodes;
	Array scripts;
	Array external_scenes;
	HashSet<String> seen_scripts;
	HashSet<String> seen_scenes;

	for (int i = 0; i < node_count; i++) {
		const String node_path = String(state->get_node_path(i));
		const String name = String(state->get_node_name(i));
		const String node_type = String(state->get_node_type(i));
		if (nodes.size() < max_nodes) {
			Dictionary node;
			node["path"] = node_path;
			node["name"] = name;
			node["type"] = node_type;
			if (state->is_node_instance_placeholder(i)) {
				const String placeholder = state->get_node_instance_placeholder(i);
				node["instance"] = placeholder;
				if (!placeholder.is_empty() && !seen_scenes.has(placeholder)) {
					seen_scenes.insert(placeholder);
					external_scenes.push_back(placeholder);
				}
			} else {
				const Ref<PackedScene> inst = state->get_node_instance(i);
				if (inst.is_valid()) {
					const String inst_path = inst->get_path();
					if (!inst_path.is_empty()) {
						node["instance"] = inst_path;
						if (!seen_scenes.has(inst_path)) {
							seen_scenes.insert(inst_path);
							external_scenes.push_back(inst_path);
						}
					}
				}
			}
			nodes.push_back(node);
		}
		for (int p = 0; p < state->get_node_property_count(i); p++) {
			if (state->get_node_property_name(i, p) != StringName("script")) {
				continue;
			}
			const Variant value = state->get_node_property_value(i, p);
			String script_path;
			if (value.get_type() == Variant::OBJECT) {
				const Ref<Resource> res = value;
				if (res.is_valid()) {
					script_path = res->get_path();
				}
			} else if (value.get_type() == Variant::STRING) {
				script_path = String(value);
			}
			if (!script_path.is_empty() && !seen_scripts.has(script_path)) {
				seen_scripts.insert(script_path);
				scripts.push_back(script_path);
			}
		}
	}

	Array dependencies;
	List<String> dep_list;
	ResourceLoader::get_dependencies(res_path, &dep_list, true);
	const int max_deps = 32;
	int dep_i = 0;
	for (const String &raw_dependency : dep_list) {
		if (dep_i >= max_deps) {
			break;
		}
		dependencies.push_back(ResourceUID::ensure_path(raw_dependency.get_slice("::", 0)));
		dep_i++;
	}

	Dictionary digest;
	digest["kind"] = "PackedScene";
	digest["path"] = res_path;
	digest["resource_type"] = resource_type;
	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(res_path);
	if (uid != ResourceUID::INVALID_ID) {
		digest["uid"] = ResourceUID::get_singleton()->id_to_text(uid);
	}
	if (node_count > 0) {
		digest["root_name"] = String(state->get_node_name(0));
		digest["root_type"] = String(state->get_node_type(0));
	}
	digest["node_count"] = node_count;
	digest["nodes_reported"] = nodes.size();
	digest["nodes_truncated"] = node_count > nodes.size();
	digest["nodes"] = nodes;
	digest["scripts"] = scripts;
	digest["external_scenes"] = external_scenes;
	digest["dependencies"] = dependencies;
	digest["dependencies_truncated"] = dep_list.size() > dependencies.size();
	digest["summary"] = vformat("PackedScene root=%s (%s), %d nodes, %d scripts, %d external scenes.",
			String(digest.get("root_name", String())), String(digest.get("root_type", String())), node_count, scripts.size(), external_scenes.size());

	result["ok"] = true;
	result["path"] = res_path;
	result["digest"] = digest;
	return result;
}

Dictionary SolersObservationService::search_project(const Dictionary &p_args) const {
	const String type = String(p_args.get("type", "path")).to_lower();
	const String query = String(p_args.get("query", String())).strip_edges();
	const int max_results = CLAMP((int)p_args.get("max_results", 64), 1, 256);
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 300;
	Array results;
	int scanned = 0;
	bool truncated = false;

	if (type == "path") {
		const Dictionary paths = _search_project_paths(query, max_results);
		const Array files = paths.get("files", Array());
		for (int i = 0; i < files.size(); i++) {
			results.push_back(_solers_project_result(files[i]));
		}
		scanned = paths.get("scanned_count", 0);
		truncated = paths.get("truncated", false);
	} else {
		const Dictionary index = list_project_files(SOLERS_FILE_LIST_HARD_CAP);
		const Array files = index.get("files", Array());
		if (type == "text" || type == "symbol") {
			for (int i = 0; i < files.size(); i++) {
				if (OS::get_singleton()->get_ticks_msec() > deadline) {
					truncated = true;
					break;
				}
				const String path = files[i];
				if (!_solers_text_search_extension(path.get_extension().to_lower())) {
					continue;
				}
				scanned++;
				// Internal index scan may read PackedScene source; tool-facing
				// project.read_file still requires raw=true for scenes.
				const Dictionary file = read_project_file(path, 1048576, true);
				if (!(bool)file.get("ok", false)) {
					continue;
				}
				const PackedStringArray lines = String(file.get("content", String())).split("\n", true);
				for (int line_index = 0; line_index < lines.size(); line_index++) {
					const int column = _solers_find_text(lines[line_index], query, type == "symbol");
					if (column < 0) {
						continue;
					}
					Dictionary match = _solers_project_result(path);
					match["line"] = line_index + 1;
					match["column"] = column + 1;
					match["preview"] = lines[line_index].strip_edges().left(240);
					results.push_back(match);
					if (results.size() >= max_results) {
						truncated = true;
						break;
					}
				}
				if (results.size() >= max_results) {
					break;
				}
			}
		}
	}

	Dictionary result;
	result["type"] = type;
	result["query"] = query;
	result["results"] = results;
	result["count"] = results.size();
	result["scanned_count"] = scanned;
	result["truncated"] = truncated;
	return result;
}

Dictionary SolersObservationService::read_project_file(const String &p_path, int p_max_bytes, bool p_raw) const {
	Dictionary result;
	String res_path;
	String path_error;
	if (!_normalize_project_path(p_path, res_path, path_error)) {
		result["ok"] = false;
		result["error"] = path_error;
		return result;
	}

	if (!FileAccess::exists(res_path)) {
		result["ok"] = false;
		result["error"] = vformat("File does not exist.%s", solers_file_suggestions(res_path));
		result["path"] = res_path;
		return result;
	}

	const String resource_type = p_raw ? String() : ResourceLoader::get_resource_type(res_path);
	if (_solers_is_packed_scene_type(resource_type)) {
		const Dictionary observed = observe_path(res_path);
		result["ok"] = false;
		result["code"] = "SCENE_TEXT_DENIED";
		result["error"] = "PackedScene source text is not the scene fact model. The digest field is the complete answer for structure/identity — do not retry with raw=true. Use object.query target=scene for live details. Pass raw=true only when editing .tscn/.scn source text.";
		result["path"] = res_path;
		result["resource_type"] = resource_type;
		result["recovery"] = "Use the included digest or object.query target=scene. Do not call project.read_file with raw=true for observation.";
		if ((bool)observed.get("ok", false) && observed.has("digest")) {
			result["digest"] = observed["digest"];
		}
		return result;
	}

	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(res_path, FileAccess::READ, &err);
	if (file.is_null() || err != OK) {
		result["ok"] = false;
		result["error"] = "Unable to open file for reading.";
		result["path"] = res_path;
		return result;
	}

	const int64_t length = file->get_length();
	const int max_bytes = CLAMP(p_max_bytes, 0, 1048576);
	result["ok"] = true;
	result["path"] = res_path;
	result["size_bytes"] = length;
	result["sha256"] = FileAccess::get_sha256(res_path);
	result["truncated"] = length > max_bytes;
	Vector<uint8_t> bytes = file->get_buffer(MIN((int64_t)max_bytes, length));
	result["content"] = bytes.is_empty() ? String() : String::utf8((const char *)bytes.ptr(), bytes.size());
	return result;
}

Dictionary SolersObservationService::get_open_scenes(int p_max_depth, int p_max_children_per_node) const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	PackedStringArray paths = editor_interface->get_open_scenes();
	TypedArray<Node> roots = editor_interface->get_open_scene_roots();
	Node *edited_root = editor_interface->get_edited_scene_root();

	Array path_array;
	for (int i = 0; i < paths.size(); i++) {
		path_array.push_back(paths[i]);
	}

	result["count"] = roots.size();
	result["paths"] = path_array;
	result["roots"] = _serialize_node_array(roots, edited_root, p_max_depth, p_max_children_per_node);
	result["current_scene_path"] = edited_root ? edited_root->get_scene_file_path() : String();
	return result;
}

Dictionary SolersObservationService::get_selection(int p_max_depth, int p_max_children_per_node) const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	EditorSelection *selection = editor_interface->get_selection();
	if (!selection) {
		result["count"] = 0;
		result["nodes"] = Array();
		return result;
	}

	TypedArray<Node> selected_nodes = selection->get_selected_nodes();
	Node *edited_root = editor_interface->get_edited_scene_root();
	result["count"] = selected_nodes.size();
	result["nodes"] = _serialize_node_array(selected_nodes, edited_root, p_max_depth, p_max_children_per_node);
	return result;
}

Dictionary SolersObservationService::get_scene_tree(int p_max_depth, int p_max_children_per_node, int p_token_budget) const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	Node *edited_root = editor_interface->get_edited_scene_root();
	result["has_edited_scene"] = edited_root != nullptr;
	if (!edited_root) {
		result["root"] = Dictionary();
		return result;
	}

	const int token_budget_start = MAX(1, p_token_budget);
	int token_budget = token_budget_start;
	int elided_nodes = 0;
	result["root"] = _serialize_node(edited_root, edited_root, 0, p_max_depth, p_max_children_per_node, token_budget, elided_nodes);
	result["max_depth"] = p_max_depth;
	if (elided_nodes > 0) {
		// The model is told exactly how much of the scene it is not seeing and
		// how to ask a narrower question that fits.
		result["elided_nodes"] = elided_nodes;
		result["token_budget"] = token_budget_start;
		result["recovery"] = "This tree filled the per-result token budget. Pass node_paths for the nodes that matter, or lower max_depth/max_children, instead of re-reading the whole tree.";
	}
	return result;
}

Dictionary SolersObservationService::get_runtime_status() const {
	Dictionary result;
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	const bool playing = run_bar && run_bar->is_playing();
	result["is_playing"] = playing;
	result["playing_scene"] = playing ? run_bar->get_playing_scene() : String();
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	result["debugger_connected"] = debugger && debugger->is_session_active();
	result["is_breaked"] = debugger && debugger->is_breaked();
	result["remote_pid"] = debugger ? debugger->get_remote_pid() : 0;
	result["error_count"] = debugger ? debugger->get_error_count() : 0;
	result["warning_count"] = debugger ? debugger->get_warning_count() : 0;
	result["runtime_epoch"] = (int64_t)runtime_epoch;
	result["capture_ready"] = _is_runtime_visual_ready();
	return result;
}

void SolersObservationService::_append_runtime_event(const StringName &p_type, const Dictionary &p_data, bool p_persist) {
	Dictionary event = p_data.duplicate(true);
	event["type"] = p_type;
	event["cursor"] = (int64_t)++runtime_cursor;
	event["runtime_epoch"] = (int64_t)runtime_epoch;
	event["ticks_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	runtime_events.push_back(event);
	if (runtime_events.size() > SOLERS_RUNTIME_EVENT_LIMIT) {
		runtime_events.remove_at(0);
	}
	if (p_persist) {
		Dictionary audit;
		audit["event_type"] = "runtime_observation";
		ProjectSettings *project_settings = ProjectSettings::get_singleton();
		audit["project_path"] = project_settings ? project_settings->get_resource_path() : String();
		audit["observation"] = event;
		solers_transcript_write(audit);
	}
}

bool SolersObservationService::_is_runtime_visual_ready() const {
	if (!GameViewDebugger::has_active_capture_session()) {
		return false;
	}
	for (int i = runtime_events.size() - 1; i >= 0; i--) {
		const Dictionary event = runtime_events[i];
		if ((uint64_t)(int64_t)event.get("runtime_epoch", 0) != runtime_epoch) {
			continue;
		}
		if (StringName(event.get("type", String())) == SNAME("started")) {
			return true;
		}
	}
	return false;
}

bool SolersObservationService::_request_runtime_screenshot(const String &p_capture_id) {
	return GameViewDebugger::request_root_viewport_screenshot(callable_mp(this, &SolersObservationService::_runtime_screenshot_ready).bind(p_capture_id));
}

void SolersObservationService::_runtime_started() {
	runtime_epoch++;
	_append_runtime_event(SNAME("started"), Dictionary(), true);
}

void SolersObservationService::_runtime_stopped() {
	performance_capture_active = false;
	performance_monitor_names.clear();
	performance_monitor_types.clear();
	_append_runtime_event(SNAME("stopped"), Dictionary(), true);
}

void SolersObservationService::_runtime_output(const String &p_message, int p_level) {
	Dictionary data;
	data["message"] = p_message.left(4096);
	data["level"] = p_level;
	data["truncated"] = p_message.length() > 4096;
	// Ring keeps the evidence; transcript does not need per-line floods.
	_append_runtime_event(SNAME("output"), data, false);
}

void SolersObservationService::_runtime_breaked(bool p_breaked, bool p_can_debug, const String &p_reason, bool p_has_stackdump) {
	Dictionary data;
	data["breaked"] = p_breaked;
	data["can_debug"] = p_can_debug;
	data["reason"] = p_reason;
	data["has_stackdump"] = p_has_stackdump;
	_append_runtime_event(SNAME("break"), data, false);
}

static Variant _solers_bounded_runtime_value(const Variant &p_value) {
	const String encoded = JSON::stringify(p_value, "", false, true);
	if (encoded.utf8().length() <= 4096) {
		return p_value;
	}
	Dictionary summary;
	summary["type"] = Variant::get_type_name(p_value.get_type());
	summary["preview"] = encoded.left(4096);
	summary["truncated"] = true;
	return summary;
}

void SolersObservationService::_runtime_debug_data(const String &p_message, const Array &p_data) {
	// Game-side errors arrive on the same debugger channel as profiler data;
	// decode them into a first-class event so runtime.observe callers see
	// real failures without digging through the raw debug_data firehose.
	if (p_message == "error") {
		DebuggerMarshalls::OutputError output_error;
		if (output_error.deserialize(p_data)) {
			Dictionary event;
			event["error"] = output_error.error;
			event["details"] = output_error.error_descr;
			event["source_file"] = output_error.source_file;
			event["source_function"] = output_error.source_func;
			event["source_line"] = output_error.source_line;
			event["warning"] = output_error.warning;
			_append_runtime_event(SNAME("error"), event, false);
			return;
		}
	}
	if (p_message == "performance:profile_names" && p_data.size() == 2) {
		performance_monitor_names = p_data[0];
		performance_monitor_types = p_data[1];
	}
	Array data;
	const int count = MIN(p_data.size(), 32);
	for (int i = 0; i < count; i++) {
		data.push_back(_solers_bounded_runtime_value(p_data[i]));
	}
	Dictionary event;
	event["message"] = p_message;
	event["data"] = data;
	event["truncated"] = count < p_data.size();
	const bool performance = p_message == "performance:profile_frame";
	if (performance) {
		Array samples;
		const int sample_count = MIN(p_data.size(), 128);
		for (int i = 0; i < sample_count; i++) {
			Dictionary sample;
			sample["name"] = i < performance_monitor_names.size() ? performance_monitor_names[i] : Variant(i);
			sample["value"] = p_data[i];
			if (i < performance_monitor_types.size()) {
				sample["type"] = performance_monitor_types[i];
			}
			samples.push_back(sample);
		}
		event["samples"] = samples;
		event["truncated"] = sample_count < p_data.size();
	}
	_append_runtime_event(performance ? SNAME("performance") : SNAME("debug_data"), event);
	if (performance && performance_capture_active) {
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		if (debugger && debugger->is_session_active()) {
			debugger->toggle_profiler("performance", false, Array());
		}
		performance_capture_active = false;
	}
}

void SolersObservationService::_runtime_tree_updated() {
	_append_runtime_event(SNAME("remote_scene"));
}

void SolersObservationService::_bind_runtime_debugger() {
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const ObjectID debugger_id = debugger ? debugger->get_instance_id() : ObjectID();
	if (debugger_id == observed_debugger_id) {
		return;
	}
	if (ScriptEditorDebugger *previous = Object::cast_to<ScriptEditorDebugger>(ObjectDB::get_instance(observed_debugger_id))) {
		previous->disconnect(SNAME("started"), callable_mp(this, &SolersObservationService::_runtime_started));
		previous->disconnect(SNAME("stopped"), callable_mp(this, &SolersObservationService::_runtime_stopped));
		previous->disconnect(SNAME("output"), callable_mp(this, &SolersObservationService::_runtime_output));
		previous->disconnect(SNAME("breaked"), callable_mp(this, &SolersObservationService::_runtime_breaked));
		previous->disconnect(SNAME("debug_data"), callable_mp(this, &SolersObservationService::_runtime_debug_data));
		previous->disconnect(SNAME("remote_tree_updated"), callable_mp(this, &SolersObservationService::_runtime_tree_updated));
	}
	observed_debugger_id = debugger_id;
	if (!debugger) {
		return;
	}
	debugger->connect(SNAME("started"), callable_mp(this, &SolersObservationService::_runtime_started));
	debugger->connect(SNAME("stopped"), callable_mp(this, &SolersObservationService::_runtime_stopped));
	debugger->connect(SNAME("output"), callable_mp(this, &SolersObservationService::_runtime_output));
	debugger->connect(SNAME("breaked"), callable_mp(this, &SolersObservationService::_runtime_breaked));
	debugger->connect(SNAME("debug_data"), callable_mp(this, &SolersObservationService::_runtime_debug_data));
	debugger->connect(SNAME("remote_tree_updated"), callable_mp(this, &SolersObservationService::_runtime_tree_updated));
	if (debugger->is_session_active()) {
		runtime_epoch++;
		_append_runtime_event(SNAME("started"), Dictionary(), true);
	}
}

void SolersObservationService::poll() {
	_bind_runtime_debugger();
}

bool SolersObservationService::_has_runtime_event_after(const StringName &p_type, uint64_t p_cursor) const {
	for (int i = runtime_events.size() - 1; i >= 0; i--) {
		const Dictionary event = runtime_events[i];
		if ((uint64_t)(int64_t)event.get("cursor", 0) <= p_cursor) {
			return false;
		}
		if (StringName(event.get("type", String())) == p_type) {
			return true;
		}
	}
	return false;
}

bool SolersObservationService::is_runtime_observation_ready(const Dictionary &p_args) const {
	const Array requested_types = p_args.get("types", Array());
	if (!requested_types.has("performance")) {
		return true;
	}
	const uint64_t since_cursor = (int64_t)p_args.get("since_cursor", 0);
	if (_has_runtime_event_after(SNAME("performance"), since_cursor)) {
		return true;
	}
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	return !debugger || !debugger->is_session_active() || OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)p_args.get("deadline_msec", 0);
}

Dictionary SolersObservationService::observe_runtime(const Dictionary &p_args) {
	const uint64_t since_cursor = (int64_t)p_args.get("since_cursor", 0);
	const bool include_prior_epochs = (bool)p_args.get("include_prior_epochs", false);
	const uint64_t min_epoch = p_args.has("since_epoch")
			? (uint64_t)(int64_t)p_args.get("since_epoch", 0)
			: (include_prior_epochs ? 0 : runtime_epoch);
	const bool include_events = (bool)p_args.get("include_events", false);
	const int max_events = CLAMP((int)p_args.get("max_events", include_events ? 32 : 0), 0, 256);
	const Array requested_types = p_args.get("types", Array());
	HashSet<StringName> types;
	for (int i = 0; i < requested_types.size(); i++) {
		types.insert(StringName(requested_types[i]));
	}

	Array events;
	Dictionary error_digest;
	uint64_t consumed_cursor = since_cursor;
	bool truncated = false;
	int64_t epoch_error_count = 0;

	for (int i = 0; i < runtime_events.size(); i++) {
		const Dictionary event = runtime_events[i];
		const uint64_t event_cursor = (int64_t)event.get("cursor", 0);
		const uint64_t event_epoch = (int64_t)event.get("runtime_epoch", 0);
		if (event_cursor <= since_cursor) {
			continue;
		}
		if (event_epoch < min_epoch) {
			consumed_cursor = event_cursor;
			continue;
		}
		if (!types.is_empty() && !types.has(StringName(event.get("type", String())))) {
			consumed_cursor = event_cursor;
			continue;
		}
		consumed_cursor = event_cursor;

		const StringName event_type = StringName(event.get("type", String()));
		if (event_type == SNAME("error") && !(bool)event.get("warning", false)) {
			epoch_error_count++;
			const String message = String(event.get("error", String())) + "\n" + String(event.get("details", String()));
			const String fingerprint = String::num_uint64((String(event_type) + String::chr(0x1f) + message.strip_edges()).hash64());
			Dictionary entry = error_digest.has(fingerprint) ? Dictionary(error_digest[fingerprint]) : Dictionary();
			const int64_t count = (int64_t)entry.get("count", 0) + 1;
			entry["fingerprint"] = fingerprint;
			entry["count"] = count;
			entry["last_cursor"] = (int64_t)event_cursor;
			if (!entry.has("first_cursor")) {
				entry["first_cursor"] = (int64_t)event_cursor;
				Dictionary sample;
				sample["error"] = event.get("error", String());
				sample["details"] = event.get("details", String());
				sample["source_file"] = event.get("source_file", String());
				sample["source_line"] = event.get("source_line", 0);
				sample["warning"] = event.get("warning", false);
				entry["sample"] = sample;
			}
			error_digest[fingerprint] = entry;
		}

		if (include_events && max_events > 0) {
			if (events.size() >= max_events) {
				truncated = true;
			} else {
				events.push_back(event);
			}
		}
	}

	Dictionary result;
	result["runtime"] = get_runtime_status();
	result["cursor"] = (int64_t)consumed_cursor;
	result["runtime_epoch"] = (int64_t)runtime_epoch;
	result["min_epoch"] = (int64_t)min_epoch;
	result["error_digest"] = error_digest;
	result["epoch_error_count"] = epoch_error_count;
	result["events"] = events;
	result["truncated"] = truncated;
	result["earliest_cursor"] = runtime_events.is_empty() ? Variant((int64_t)runtime_cursor) : runtime_events[0].get("cursor", 0);
	result["events_dropped"] = !runtime_events.is_empty() && since_cursor + 1 < (uint64_t)(int64_t)runtime_events[0].get("cursor", 0);
	if (requested_types.has("performance") && !_has_runtime_event_after(SNAME("performance"), since_cursor)) {
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		if (!debugger || !debugger->is_session_active()) {
			result["performance_unavailable"] = "runtime_not_connected";
		} else {
			Dictionary poll_args = p_args.duplicate(true);
			const uint64_t deadline = (int64_t)p_args.get("deadline_msec", 0);
			if (deadline == 0) {
				poll_args["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + 3000);
			}
			if (OS::get_singleton()->get_ticks_msec() < (uint64_t)(int64_t)poll_args.get("deadline_msec", 0)) {
				if (!performance_capture_active) {
					debugger->toggle_profiler("performance", true, Array());
					performance_capture_active = true;
				}
				result["status"] = "pending";
				result["poll_args"] = poll_args;
			} else {
				if (performance_capture_active) {
					debugger->toggle_profiler("performance", false, Array());
					performance_capture_active = false;
				}
				result["performance_unavailable"] = "sample_timeout";
			}
		}
	}
	return result;
}

Dictionary SolersObservationService::get_editor_logs(int p_max_messages) const {
	Dictionary result;
	EditorLog *log = EditorNode::get_log();
	if (!log) {
		result["available"] = false;
		result["messages"] = Array();
		result["counts"] = Dictionary();
		return result;
	}

	result["available"] = true;
	result["messages"] = log->get_messages(p_max_messages);
	result["counts"] = log->get_message_counts();
	return result;
}

SolersObservationService::SolersObservationService() {
	if (RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->connect(SNAME("frame_post_draw"), callable_mp(this, &SolersObservationService::_render_frame_post_draw));
	}
}
