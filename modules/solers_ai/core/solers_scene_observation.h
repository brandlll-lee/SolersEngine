/**************************************************************************/
/*  solers_scene_observation.h                                            */
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

#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "scene/main/node.h"

class Camera3D;
class Image;
class SolersRuntimeObservation;
class Viewport;

class SolersSceneObservation : public Object {
	GDCLASS(SolersSceneObservation, Object);

	SolersRuntimeObservation *runtime_observation = nullptr;
	uint64_t capture_sequence = 0;
	uint64_t render_post_draw_sequence = 0;
	HashMap<String, Dictionary> pending_captures;
	HashMap<String, Dictionary> last_render_by_view;

	uint64_t _runtime_epoch() const;
	void _render_frame_post_draw();
	Dictionary _capture_error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	Dictionary _runtime_capture_unavailable() const;
	Dictionary _capture_image(const Ref<Image> &p_image, const String &p_target, const String &p_capture_id = String());
	Dictionary _render_state_for_pending(const Dictionary &p_pending) const;
	Dictionary _attach_render_receipt(Dictionary p_result, const Dictionary &p_pending);
	Dictionary _register_pending_capture(const String &p_target, const Dictionary &p_extra);
	Dictionary _poll_pending_capture(const String &p_capture_id);
	Dictionary _finish_frame_gated_capture(const String &p_capture_id, const Dictionary &p_data);
	Dictionary _begin_scene_view_capture(const String &p_target, const Dictionary &p_args);
	Dictionary _editor_3d_viewport_state() const;
	void _runtime_screenshot_ready(int64_t p_width, int64_t p_height, const String &p_path, const Rect2i &p_rect, const String &p_capture_id);
	bool _is_runtime_visual_ready() const;
	bool _request_runtime_screenshot(const String &p_capture_id);

protected:
	static void _bind_methods();

public:
	static int get_capture_settle_frame_count(bool p_sdfgi_enabled, int p_convergence_setting);
	static String render_state_fingerprint(const Dictionary &p_state);
	Dictionary describe_render_state(Viewport *p_viewport, Camera3D *p_camera = nullptr, Node *p_scene_root = nullptr) const;
	Dictionary get_editor_state() const;
	Dictionary get_open_scenes() const;
	Dictionary get_selection() const;
	Dictionary query_scene_nodes(const Dictionary &p_args, int p_token_budget) const;
	Dictionary capture_viewport(const Dictionary &p_args);
	Dictionary poll_viewport_capture(const Dictionary &p_args);
	bool is_viewport_capture_ready(const Dictionary &p_args);
	void set_runtime_observation(SolersRuntimeObservation *p_runtime_observation) { runtime_observation = p_runtime_observation; }

	SolersSceneObservation();
};
