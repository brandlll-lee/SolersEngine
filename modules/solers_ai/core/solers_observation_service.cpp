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

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"
#include "core/version.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/run/game_view_plugin.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/debugger/scene_debugger.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/environment.h"
#include "servers/rendering/rendering_server.h"

static constexpr uint64_t SOLERS_CAPTURE_TIMEOUT_MSEC = 10000;
// Keep encoded captures small enough for model requests without losing layout
// readability.
static constexpr int SOLERS_CAPTURE_MAX_DIMENSION = 1280;

static Array _solers_vector3_array(const Vector3 &p_vector) {
	Array values;
	values.push_back(p_vector.x);
	values.push_back(p_vector.y);
	values.push_back(p_vector.z);
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
	static constexpr int frames[RenderingServer::ENV_SDFGI_CONVERGE_MAX] = { 5, 10, 15, 20, 25, 30 };
	return frames[CLAMP(p_convergence_setting, 0, (int)RenderingServer::ENV_SDFGI_CONVERGE_MAX - 1)];
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
	ClassDB::bind_method(D_METHOD("search_project_files", "query", "max_files"), &SolersObservationService::search_project_files, DEFVAL(128));
	ClassDB::bind_method(D_METHOD("read_project_file", "path", "max_bytes"), &SolersObservationService::read_project_file, DEFVAL(262144));
	ClassDB::bind_method(D_METHOD("get_open_scenes", "max_depth", "max_children_per_node"), &SolersObservationService::get_open_scenes, DEFVAL(1), DEFVAL(16));
	ClassDB::bind_method(D_METHOD("get_selection", "max_depth", "max_children_per_node"), &SolersObservationService::get_selection, DEFVAL(1), DEFVAL(16));
	ClassDB::bind_method(D_METHOD("get_scene_tree", "max_depth", "max_children_per_node"), &SolersObservationService::get_scene_tree, DEFVAL(8), DEFVAL(128));
	ClassDB::bind_method(D_METHOD("get_runtime_status"), &SolersObservationService::get_runtime_status);
	ClassDB::bind_method(D_METHOD("get_editor_logs", "max_messages"), &SolersObservationService::get_editor_logs, DEFVAL(200));
	ClassDB::bind_method(D_METHOD("get_editor_snapshot", "max_scene_depth", "max_children_per_node", "include_remote_scene"), &SolersObservationService::get_editor_snapshot, DEFVAL(4), DEFVAL(64), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("capture_viewport", "args"), &SolersObservationService::capture_viewport);
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

	// Captures are immutable evidence: every capture gets a unique file that is
	// never deleted while the session may still reference it in messages.
	const String relative_path = ProjectSettings::get_singleton()->get_project_data_path().path_join("solers/captures").path_join(p_capture_id + ".png");
	const String absolute_path = ProjectSettings::get_singleton()->globalize_path(relative_path);
	const Error directory_error = DirAccess::make_dir_recursive_absolute(absolute_path.get_base_dir());
	if (directory_error != OK) {
		return _capture_error("CAPTURE_DIRECTORY_FAILED", vformat("Could not create the capture directory: %s", absolute_path.get_base_dir()), false);
	}
	const Error save_error = image->save_png(absolute_path);
	if (save_error != OK) {
		return _capture_error("CAPTURE_SAVE_FAILED", vformat("Could not save the viewport capture: %s", absolute_path), false);
	}

	Dictionary attachment;
	attachment["id"] = p_capture_id;
	attachment["source"] = "tool_capture";
	attachment["target"] = p_target;
	attachment["type"] = "image";
	attachment["mime_type"] = "image/png";
	attachment["local_path"] = absolute_path;

	Dictionary data;
	data["status"] = "complete";
	data["target"] = p_target;
	data["capture_id"] = p_capture_id;
	data["width"] = image->get_width();
	data["height"] = image->get_height();
	data["visual_statistics"] = image_statistics(image);
	data["content_sha256"] = FileAccess::get_sha256(absolute_path);
	data["attachment"] = attachment;

	Array attachments;
	attachments.push_back(attachment);
	Dictionary result;
	result["ok"] = true;
	result["data"] = data;
	result["attachments"] = attachments;
	return result;
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
	Node *edited_root = EditorInterface::get_singleton()->get_edited_scene_root();
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
	if (!pending_captures.has(p_capture_id)) {
		DirAccess::remove_absolute(p_path);
		return;
	}
	Ref<Image> image = Image::load_from_file(p_path);
	DirAccess::remove_absolute(p_path);
	if (image.is_null()) {
		pending_captures[p_capture_id] = _capture_error("RUNTIME_CAPTURE_DECODE_FAILED", "The runtime returned an unreadable screenshot.", true);
		return;
	}
	Dictionary result = _capture_image(image, "runtime", p_capture_id);
	if ((bool)result.get("ok", false)) {
		Dictionary data = result.get("data", Dictionary());
		data["frame_valid"] = true;
		result["data"] = data;
	}
	pending_captures[p_capture_id] = result;
}

static void _solers_free_capture_viewport(const Dictionary &p_data) {
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
	data["target"] = p_target;
	data["capture_id"] = capture_id;
	data["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + SOLERS_CAPTURE_TIMEOUT_MSEC);
	for (const Variant *key = p_extra.next(nullptr); key; key = p_extra.next(key)) {
		data[*key] = p_extra[*key];
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
			const bool settled = frames_waited >= (int64_t)p_data.get("render_frames_required", 1);
			data["editor_viewport"] = viewport_state;
			data["material_preview"] = viewport_state.get("material_preview", false);
			data["frame_valid"] = settled && (bool)viewport_state.get("frame_valid", false);
			data["render_frames_required"] = p_data.get("render_frames_required", 1);
			data["render_frames_waited"] = frames_waited;
			data["sdfgi_enabled"] = p_data.get("sdfgi_enabled", false);
			result["data"] = data;
		}
		return result;
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
		const bool settled = frames_waited >= (int64_t)p_data.get("render_frames_required", 1);
		data["material_preview"] = true;
		data["frame_valid"] = settled;
		data["render_frames_required"] = p_data.get("render_frames_required", 1);
		data["render_frames_waited"] = frames_waited;
		data["sdfgi_enabled"] = p_data.get("sdfgi_enabled", false);
		for (const char *key : { "camera_path", "camera_forward_hit_node", "camera_forward_hit_distance", "orientation", "axis", "direction", "section_position", "focus_paths", "view_spec_hash" }) {
			if (p_data.has(key)) {
				data[key] = p_data[key];
			}
		}
		result["data"] = data;
	}
	return result;
}

Dictionary SolersObservationService::_poll_pending_capture(const String &p_capture_id) {
	Dictionary *entry = pending_captures.getptr(p_capture_id);
	if (!entry) {
		return _capture_error("CAPTURE_NOT_FOUND", vformat("Unknown capture_id: %s", p_capture_id), true);
	}
	const Dictionary result = *entry;
	const Dictionary data = result.get("data", Dictionary());
	if (!(bool)result.get("ok", false) || String(data.get("status", String())) == "complete") {
		pending_captures.erase(p_capture_id);
		return result;
	}

	const String target = data.get("target", String());
	if (target != "runtime" && Engine::get_singleton()->get_frames_drawn() >= (uint64_t)(int64_t)data.get("ready_frame", 0)) {
		const Dictionary final_result = _finish_frame_gated_capture(p_capture_id, data);
		pending_captures.erase(p_capture_id);
		return final_result;
	}
	if (OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)data.get("deadline_msec", 0)) {
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
				return _capture_error("CAMERA_NOT_FOUND", vformat("No Camera3D at node_path: %s", node_path), true);
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
					return _capture_error("FOCUS_NODE_NOT_FOUND", vformat("No edited-scene node at focus_paths[%d]: %s", i, focus_path), true);
				}
				_solers_accumulate_world_aabb(focus, bounds, found);
			}
		}
		if (!found) {
			memdelete(viewport);
			return _capture_error("SCENE_HAS_NO_GEOMETRY", "The requested orthographic focus contains no visible 3D geometry.", true);
		}
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
	return _register_pending_capture(p_target, extra);
}

Dictionary SolersObservationService::capture_viewport(const Dictionary &p_args) {
	const String requested_id = String(p_args.get("capture_id", String())).strip_edges();
	if (!requested_id.is_empty()) {
		return _poll_pending_capture(requested_id);
	}

	const String target = String(p_args.get("target", String())).strip_edges().to_lower();
	if (target == "editor") {
		Node3DEditor *editor_3d = Node3DEditor::get_singleton();
		if (!editor_3d || !editor_3d->is_visible_in_tree()) {
			return _capture_error("EDITOR_3D_NOT_ACTIVE", "The 3D or Modeling workspace must be active before capturing its shared viewport.", true);
		}
		Node3DEditorViewport *editor_viewport = editor_3d ? editor_3d->get_last_used_viewport() : nullptr;
		if (!editor_viewport || !editor_viewport->get_viewport_node()) {
			return _capture_error("EDITOR_VIEWPORT_UNAVAILABLE", "The active 3D editor viewport is unavailable.", true);
		}
		return _register_pending_capture("editor", Dictionary());
	}
	if (target == "camera" || target == "top_down" || target == "orthographic") {
		return _begin_scene_view_capture(target, p_args);
	}
	if (target == "runtime") {
		const Dictionary pending = _register_pending_capture("runtime", Dictionary());
		const String capture_id = Dictionary(pending.get("data", Dictionary())).get("capture_id", String());
		if (!GameViewDebugger::request_root_viewport_screenshot(callable_mp(this, &SolersObservationService::_runtime_screenshot_ready).bind(capture_id))) {
			pending_captures.erase(capture_id);
			return _capture_error("RUNTIME_NOT_RUNNING", "No active runtime debugger session can capture the game root viewport.", true);
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
	const bool ready = String(data.get("target", String())) != "runtime" && Engine::get_singleton()->get_frames_drawn() >= (uint64_t)(int64_t)data.get("ready_frame", 0);
	if (!ready && String(data.get("target", String())) != "runtime") {
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

// Walk the EditorFileSystem's in-memory index instead of re-scanning the
// disk. The editor kernel already maintains this tree incrementally (initial
// scan, file watcher, import pipeline), so the agent reads the exact same
// project view the FileSystem dock shows — in microseconds, with zero I/O,
// and without freezing the main thread on large projects.
bool SolersObservationService::_collect_project_files_indexed(const String &p_query, int p_max_files, Array &r_files, int &r_scanned_count, bool &r_truncated) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return false;
	}
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (!efs || efs->is_scanning()) {
		return false;
	}
	EditorFileSystemDirectory *root = efs->get_filesystem();
	if (!root) {
		return false;
	}

	// Iterative DFS; explicit stack so a pathological tree can never blow the
	// C++ call stack.
	LocalVector<EditorFileSystemDirectory *> stack;
	stack.push_back(root);
	while (!stack.is_empty()) {
		EditorFileSystemDirectory *dir = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);

		const int file_count = dir->get_file_count();
		for (int i = 0; i < file_count; i++) {
			r_scanned_count++;
			const String path = dir->get_file_path(i);
			if (p_query.is_empty() || path.findn(p_query) != -1) {
				if (r_files.size() >= p_max_files) {
					r_truncated = true;
					return true;
				}
				r_files.push_back(path);
			}
		}
		for (int i = dir->get_subdir_count() - 1; i >= 0; i--) {
			stack.push_back(dir->get_subdir(i));
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
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		const String path = p_dir.path_join(entry).replace_char('\\', '/');
		if (dir->current_is_dir()) {
			if (entry != ".godot" && entry != ".git" && entry != "__pycache__") {
				_collect_project_files(path, p_query, p_max_files, r_files, r_scanned_count, r_truncated, p_deadline_msec);
				if (r_truncated) {
					break;
				}
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
}

Dictionary SolersObservationService::_serialize_node(Node *p_node, Node *p_edited_root, int p_depth, int p_max_depth, int p_max_children_per_node) const {
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

	if (p_depth >= p_max_depth) {
		node_data["children_truncated_by_depth"] = p_node->get_child_count() > 0;
		return node_data;
	}

	Array children;
	const int child_count = p_node->get_child_count();
	const int max_children = MAX(0, p_max_children_per_node);
	const int child_limit = MIN(child_count, max_children);
	for (int i = 0; i < child_limit; i++) {
		children.push_back(_serialize_node(p_node->get_child(i), p_edited_root, p_depth + 1, p_max_depth, p_max_children_per_node));
	}
	node_data["children"] = children;
	if (child_count > child_limit) {
		node_data["children_truncated_count"] = child_count - child_limit;
	}

	return node_data;
}

static void _skip_remote_node(const LocalVector<SceneDebuggerTree::RemoteNode> &p_nodes, int &r_index) {
	if (r_index >= (int)p_nodes.size()) {
		return;
	}
	const int child_count = p_nodes[r_index++].child_count;
	for (int i = 0; i < child_count; i++) {
		_skip_remote_node(p_nodes, r_index);
	}
}

static Dictionary _serialize_remote_node(const LocalVector<SceneDebuggerTree::RemoteNode> &p_nodes, int &r_index, int p_depth, int p_max_depth, int p_max_children_per_node) {
	Dictionary node_data;
	if (r_index >= (int)p_nodes.size()) {
		node_data["valid"] = false;
		return node_data;
	}
	const SceneDebuggerTree::RemoteNode &node = p_nodes[r_index++];
	node_data["valid"] = true;
	node_data["name"] = node.name;
	node_data["type"] = node.type_name;
	node_data["id"] = String::num_uint64((uint64_t)node.id);
	node_data["scene_file_path"] = node.scene_file_path;
	node_data["child_count"] = node.child_count;
	if (node.view_flags & SceneDebuggerTree::RemoteNode::VIEW_HAS_VISIBLE_METHOD) {
		node_data["visible"] = (node.view_flags & SceneDebuggerTree::RemoteNode::VIEW_VISIBLE) != 0;
		node_data["visible_in_tree"] = (node.view_flags & SceneDebuggerTree::RemoteNode::VIEW_VISIBLE_IN_TREE) != 0;
	}
	if (p_depth >= p_max_depth) {
		node_data["children_truncated_by_depth"] = node.child_count > 0;
		for (int i = 0; i < node.child_count; i++) {
			_skip_remote_node(p_nodes, r_index);
		}
		return node_data;
	}
	Array children;
	const int child_limit = MIN(node.child_count, MAX(0, p_max_children_per_node));
	for (int i = 0; i < node.child_count; i++) {
		if (i < child_limit) {
			children.push_back(_serialize_remote_node(p_nodes, r_index, p_depth + 1, p_max_depth, p_max_children_per_node));
		} else {
			_skip_remote_node(p_nodes, r_index);
		}
	}
	node_data["children"] = children;
	if (node.child_count > child_limit) {
		node_data["children_truncated_count"] = node.child_count - child_limit;
	}
	return node_data;
}

Array SolersObservationService::_serialize_node_array(const TypedArray<Node> &p_nodes, Node *p_edited_root, int p_max_depth, int p_max_children_per_node) const {
	Array serialized;
	for (int i = 0; i < p_nodes.size(); i++) {
		Node *node = Object::cast_to<Node>(p_nodes[i]);
		if (node) {
			serialized.push_back(_serialize_node(node, p_edited_root, 0, p_max_depth, p_max_children_per_node));
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
	const bool indexed = _collect_project_files_indexed(String(), max_files, files, scanned_count, truncated);
	if (!indexed) {
		_collect_project_files("res://", String(), max_files, files, scanned_count, truncated, OS::get_singleton()->get_ticks_msec() + SOLERS_FILE_WALK_BUDGET_MSEC);
	}
	result["files"] = files;
	result["count"] = files.size();
	result["scanned_count"] = scanned_count;
	result["truncated"] = truncated;
	result["source"] = indexed ? "editor_index" : "disk_walk";
	return result;
}

Dictionary SolersObservationService::search_project_files(const String &p_query, int p_max_files) const {
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
	const bool indexed = _collect_project_files_indexed(query, max_files, files, scanned_count, truncated);
	if (!indexed) {
		_collect_project_files("res://", query, max_files, files, scanned_count, truncated, OS::get_singleton()->get_ticks_msec() + SOLERS_FILE_WALK_BUDGET_MSEC);
	}
	result["ok"] = true;
	result["query"] = query;
	result["files"] = files;
	result["count"] = files.size();
	result["scanned_count"] = scanned_count;
	result["truncated"] = truncated;
	result["source"] = indexed ? "editor_index" : "disk_walk";
	return result;
}

Dictionary SolersObservationService::read_project_file(const String &p_path, int p_max_bytes) const {
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
		result["error"] = "File does not exist.";
		result["path"] = res_path;
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

Dictionary SolersObservationService::get_scene_tree(int p_max_depth, int p_max_children_per_node) const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	Node *edited_root = editor_interface->get_edited_scene_root();
	result["has_edited_scene"] = edited_root != nullptr;
	if (!edited_root) {
		result["root"] = Dictionary();
		return result;
	}

	result["root"] = _serialize_node(edited_root, edited_root, 0, p_max_depth, p_max_children_per_node);
	return result;
}

Dictionary SolersObservationService::get_runtime_status() const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	const bool playing = editor_interface->is_playing_scene();
	result["is_playing"] = playing;
	result["playing_scene"] = playing ? editor_interface->get_playing_scene() : String();
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

Dictionary SolersObservationService::get_editor_snapshot(int p_max_scene_depth, int p_max_children_per_node, bool p_include_remote_scene) const {
	Dictionary snapshot;
	const Dictionary project = get_project_info();
	snapshot["project"] = project;
	snapshot["main_scene"] = project.get("main_scene", String());
	snapshot["project_settings"] = get_project_settings_summary();
	snapshot["open_scenes"] = get_open_scenes(1, p_max_children_per_node);
	snapshot["scene_tree"] = get_scene_tree(p_max_scene_depth, p_max_children_per_node);
	snapshot["selection"] = get_selection(1, p_max_children_per_node);
	snapshot["runtime"] = get_runtime_status();
	snapshot["editor_log"] = get_editor_logs(40);
	snapshot["editor_3d_viewport"] = _editor_3d_viewport_state();

	Dictionary file_index = list_project_files(96);
	const Array paths = file_index.get("files", Array());
	Dictionary extensions;
	for (int i = 0; i < paths.size(); i++) {
		const String path = paths[i];
		String ext = path.get_extension().to_lower();
		if (ext.is_empty()) {
			ext = "<none>";
		}
		extensions[ext] = (int)extensions.get(ext, 0) + 1;
	}
	file_index["extension_counts"] = extensions;
	snapshot["file_index"] = file_index;
	if (p_include_remote_scene) {
		EditorInterface *editor_interface = EditorInterface::get_singleton();
		if (!editor_interface || !editor_interface->is_playing_scene()) {
			snapshot["remote_scene"] = Variant();
			snapshot["remote_scene_reason"] = "not_playing";
		} else {
			EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
			ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
			const SceneDebuggerTree *remote_tree = debugger ? debugger->get_remote_tree() : nullptr;
			if (!remote_tree) {
				if (debugger) {
					debugger->request_remote_tree();
				}
				snapshot["remote_scene"] = Variant();
				snapshot["remote_scene_reason"] = "unavailable";
			} else {
				LocalVector<SceneDebuggerTree::RemoteNode> nodes;
				for (const SceneDebuggerTree::RemoteNode &node : remote_tree->nodes) {
					nodes.push_back(node);
				}
				int index = 0;
				Dictionary remote;
				remote["node_count"] = nodes.size();
				remote["root"] = nodes.is_empty() ? Dictionary() : _serialize_remote_node(nodes, index, 0, p_max_scene_depth, p_max_children_per_node);
				snapshot["remote_scene"] = remote;
			}
		}
	}
	return snapshot;
}
