/**************************************************************************/
/*  solers_rml_chat_surface.cpp                                           */
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

#include "solers_rml_chat_surface.h"

#ifdef SOLERS_RMLUI_ENABLED

#include "modules/solers_ai/core/solers_chat_ui_spec.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "editor/themes/editor_scale.h"
#include "modules/svg/image_loader_svg.h"
#include "scene/main/window.h"
#include "scene/resources/image_texture.h"
#include "servers/display/display_server.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/TextInputContext.h>
#include <RmlUi/Core/TextInputHandler.h>
#include <RmlUi/Core/Types.h>

#include <string.h>

namespace {

static String solers_to_godot_string(const Rml::String &p_value) {
	return String::utf8(p_value.c_str());
}

static Rml::String solers_to_rml_string(const String &p_value) {
	const CharString utf8 = p_value.utf8();
	return Rml::String(utf8.get_data(), utf8.length());
}

static String solers_source_root() {
	const String executable_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	if (executable_dir.get_file().to_lower() == "bin") {
		return executable_dir.get_base_dir();
	}
	return ProjectSettings::get_singleton()->get_resource_path().get_base_dir();
}

static String solers_find_existing_path(const Vector<String> &p_candidates) {
	for (int i = 0; i < p_candidates.size(); i++) {
		if (FileAccess::exists(p_candidates[i])) {
			return p_candidates[i];
		}
	}
	return String();
}

static String solers_logo_path() {
	const String source_root = solers_source_root();
	const String repo_root = source_root.get_base_dir();
	Vector<String> candidates;
	candidates.push_back(repo_root.path_join("branding/generated/solers02_icon_transparent_1024.png"));
	candidates.push_back(repo_root.path_join("branding/generated/solers_icon_transparent_1024.png"));
	candidates.push_back(source_root.path_join("branding/generated/solers02_icon_transparent_1024.png"));
	candidates.push_back(source_root.path_join("branding/generated/solers_icon_transparent_1024.png"));
	return solers_find_existing_path(candidates);
}

static String solers_font_path(const String &p_name) {
	const String source_root = solers_source_root();
	Vector<String> candidates;
	candidates.push_back(source_root.path_join("thirdparty/fonts").path_join(p_name));
	candidates.push_back(source_root.get_base_dir().path_join("thirdparty/fonts").path_join(p_name));
	return solers_find_existing_path(candidates);
}

static String solers_color_hex(const Color &p_color) {
	const Color c = p_color.clamp();
	return vformat("#%02X%02X%02X", int(Math::round(c.r * 255.0)), int(Math::round(c.g * 255.0)), int(Math::round(c.b * 255.0)));
}

// Built-in Solers icon set. The path data below is the official Lucide icon
// geometry (MIT licensed, https://lucide.dev), embedded as source so Solers keeps
// a single consistent icon language without pulling in an external runtime. Every
// icon shares Lucide's 24x24 viewBox and 2px rounded stroke. Keeping the geometry
// in one table gives us one place to fix or add icons (locality) while every UI
// call site benefits (leverage).
static String solers_icon_svg(const String &p_name, const Color &p_color) {
	const String stroke = solers_color_hex(p_color);
	String body;
	if (p_name.begins_with("panel")) {
		// lucide: panel-left
		body = "<rect width=\"18\" height=\"18\" x=\"3\" y=\"3\" rx=\"2\"/><path d=\"M9 3v18\"/>";
	} else if (p_name.begins_with("message-plus")) {
		// lucide: square-pen (new chat / compose)
		body = "<path d=\"M12 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7\"/><path d=\"M18.375 2.625a1 1 0 0 1 3 3l-9.013 9.014a2 2 0 0 1-.853.505l-2.873.84a.5.5 0 0 1-.62-.62l.84-2.873a2 2 0 0 1 .506-.852z\"/>";
	} else if (p_name.begins_with("more-vertical")) {
		// lucide: ellipsis-vertical
		body = "<circle cx=\"12\" cy=\"12\" r=\"1\"/><circle cx=\"12\" cy=\"5\" r=\"1\"/><circle cx=\"12\" cy=\"19\" r=\"1\"/>";
	} else if (p_name.begins_with("shield")) {
		// lucide: shield-check
		body = "<path d=\"M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z\"/><path d=\"m9 12 2 2 4-4\"/>";
	} else if (p_name.begins_with("chevron")) {
		// lucide: chevron-down
		body = "<path d=\"m6 9 6 6 6-6\"/>";
	} else if (p_name.begins_with("mic")) {
		// lucide: mic
		body = "<path d=\"M12 19v3\"/><path d=\"M19 10v2a7 7 0 0 1-14 0v-2\"/><rect x=\"9\" y=\"2\" width=\"6\" height=\"13\" rx=\"3\"/>";
	} else if (p_name.begins_with("audio-lines")) {
		// lucide: audio-lines
		body = "<path d=\"M2 10v3\"/><path d=\"M6 6v11\"/><path d=\"M10 3v18\"/><path d=\"M14 8v7\"/><path d=\"M18 5v13\"/><path d=\"M22 10v3\"/>";
	} else if (p_name.begins_with("send-circle")) {
		// Filled accent disc with a Lucide arrow-up glyph (send affordance).
		body = "<circle cx=\"12\" cy=\"12\" r=\"11\" fill=\"" + stroke + "\" stroke=\"none\"/><path d=\"m8 12 4-4 4 4\" stroke=\"#18191C\"/><path d=\"M12 16V8\" stroke=\"#18191C\"/>";
	} else if (p_name.begins_with("send")) {
		// lucide: send (paper plane)
		body = "<path d=\"M14.536 21.686a.5.5 0 0 0 .937-.024l6.5-19a.496.496 0 0 0-.635-.635l-19 6.5a.5.5 0 0 0-.024.937l7.93 3.18a2 2 0 0 1 1.112 1.11z\"/><path d=\"m21.854 2.147-10.94 10.939\"/>";
	} else {
		// lucide: plus (default, e.g. attach context)
		body = "<path d=\"M5 12h14\"/><path d=\"M12 5v14\"/>";
	}
	return vformat("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"%s\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%s</svg>", stroke, body);
}

static Color solers_icon_color(const String &p_source) {
	if (p_source.find("orange") >= 0) {
		return Color(1.0, 0.49, 0.20, 1.0);
	}
	if (p_source.find("send-circle") >= 0) {
		return Color(0.64, 0.65, 0.68, 1.0);
	}
	if (p_source.find("send") >= 0) {
		return Color(0.10, 0.11, 0.13, 1.0);
	}
	return Color(0.64, 0.69, 0.78, 1.0);
}

static String solers_icon_name_from_source(const String &p_source) {
	String name = p_source.trim_prefix("solers://icon/");
	const int query_pos = name.find("?");
	if (query_pos >= 0) {
		name = name.substr(0, query_pos);
	}
	return name;
}

static Color solers_unpremultiply(const Rml::ColourbPremultiplied &p_color) {
	const float alpha = float(p_color.alpha) / 255.0f;
	if (alpha <= 0.0f) {
		return Color(0, 0, 0, 0);
	}
	return Color(
			MIN(1.0f, (float(p_color.red) / 255.0f) / alpha),
			MIN(1.0f, (float(p_color.green) / 255.0f) / alpha),
			MIN(1.0f, (float(p_color.blue) / 255.0f) / alpha),
			alpha);
}

static Vector<uint8_t> solers_unpremultiply_pixels(Rml::Span<const Rml::byte> p_source) {
	Vector<uint8_t> data;
	data.resize((int)p_source.size());
	uint8_t *dst = data.ptrw();
	for (int i = 0; i + 3 < data.size(); i += 4) {
		const float alpha = float(p_source[i + 3]) / 255.0f;
		if (alpha <= 0.0f) {
			dst[i + 0] = 0;
			dst[i + 1] = 0;
			dst[i + 2] = 0;
			dst[i + 3] = 0;
			continue;
		}
		dst[i + 0] = uint8_t(CLAMP(Math::round((float(p_source[i + 0]) / alpha)), 0.0f, 255.0f));
		dst[i + 1] = uint8_t(CLAMP(Math::round((float(p_source[i + 1]) / alpha)), 0.0f, 255.0f));
		dst[i + 2] = uint8_t(CLAMP(Math::round((float(p_source[i + 2]) / alpha)), 0.0f, 255.0f));
		dst[i + 3] = uint8_t(p_source[i + 3]);
	}
	return data;
}

struct SolersRmlDrawVertex {
	Point2 point;
	Point2 uv;
	Color color;
};

enum class SolersRmlClipEdge {
	LEFT,
	RIGHT,
	TOP,
	BOTTOM,
};

static bool solers_clip_inside(const SolersRmlDrawVertex &p_vertex, const Rect2 &p_rect, SolersRmlClipEdge p_edge) {
	const Point2 end = p_rect.position + p_rect.size;
	switch (p_edge) {
		case SolersRmlClipEdge::LEFT:
			return p_vertex.point.x >= p_rect.position.x;
		case SolersRmlClipEdge::RIGHT:
			return p_vertex.point.x <= end.x;
		case SolersRmlClipEdge::TOP:
			return p_vertex.point.y >= p_rect.position.y;
		case SolersRmlClipEdge::BOTTOM:
			return p_vertex.point.y <= end.y;
	}
	return true;
}

static SolersRmlDrawVertex solers_clip_intersection(const SolersRmlDrawVertex &p_from, const SolersRmlDrawVertex &p_to, const Rect2 &p_rect, SolersRmlClipEdge p_edge) {
	const Point2 end = p_rect.position + p_rect.size;
	float edge_value = 0.0f;
	float from_value = 0.0f;
	float to_value = 0.0f;
	if (p_edge == SolersRmlClipEdge::LEFT || p_edge == SolersRmlClipEdge::RIGHT) {
		edge_value = p_edge == SolersRmlClipEdge::LEFT ? p_rect.position.x : end.x;
		from_value = p_from.point.x;
		to_value = p_to.point.x;
	} else {
		edge_value = p_edge == SolersRmlClipEdge::TOP ? p_rect.position.y : end.y;
		from_value = p_from.point.y;
		to_value = p_to.point.y;
	}
	const float denom = to_value - from_value;
	const float t = Math::is_zero_approx(denom) ? 0.0f : CLAMP((edge_value - from_value) / denom, 0.0f, 1.0f);
	SolersRmlDrawVertex result;
	result.point = p_from.point.lerp(p_to.point, t);
	result.uv = p_from.uv.lerp(p_to.uv, t);
	result.color = p_from.color.lerp(p_to.color, t);
	return result;
}

static Vector<SolersRmlDrawVertex> solers_clip_against_edge(const Vector<SolersRmlDrawVertex> &p_polygon, const Rect2 &p_rect, SolersRmlClipEdge p_edge) {
	Vector<SolersRmlDrawVertex> output;
	if (p_polygon.is_empty()) {
		return output;
	}
	SolersRmlDrawVertex previous = p_polygon[p_polygon.size() - 1];
	bool previous_inside = solers_clip_inside(previous, p_rect, p_edge);
	for (int i = 0; i < p_polygon.size(); i++) {
		const SolersRmlDrawVertex current = p_polygon[i];
		const bool current_inside = solers_clip_inside(current, p_rect, p_edge);
		if (current_inside != previous_inside) {
			output.push_back(solers_clip_intersection(previous, current, p_rect, p_edge));
		}
		if (current_inside) {
			output.push_back(current);
		}
		previous = current;
		previous_inside = current_inside;
	}
	return output;
}

static Vector<SolersRmlDrawVertex> solers_clip_polygon_to_rect(const Vector<SolersRmlDrawVertex> &p_polygon, const Rect2 &p_rect) {
	Vector<SolersRmlDrawVertex> clipped = p_polygon;
	clipped = solers_clip_against_edge(clipped, p_rect, SolersRmlClipEdge::LEFT);
	clipped = solers_clip_against_edge(clipped, p_rect, SolersRmlClipEdge::RIGHT);
	clipped = solers_clip_against_edge(clipped, p_rect, SolersRmlClipEdge::TOP);
	clipped = solers_clip_against_edge(clipped, p_rect, SolersRmlClipEdge::BOTTOM);
	return clipped;
}

static String solers_rml_escape(const String &p_text) {
	String escaped = p_text.xml_escape();
	escaped = escaped.replace("\r\n", "\n");
	escaped = escaped.replace("\n", "<br/>");
	return escaped;
}

struct SolersRmlMemoryFile {
	Vector<uint8_t> data;
	size_t cursor = 0;
};

class SolersRmlFileInterface : public Rml::FileInterface {
public:
	Rml::FileHandle Open(const Rml::String &p_path) override {
		const String path = solers_to_godot_string(p_path);
		Vector<uint8_t> data;
		if (path.ends_with("solers_chat.rcss")) {
			const CharString css = SolersChatUISpec::get_chat_pane_rcss().utf8();
			data.resize(css.length());
			if (data.size() > 0) {
				memcpy(data.ptrw(), css.get_data(), data.size());
			}
		} else {
			Error err = OK;
			data = FileAccess::get_file_as_bytes(path, &err);
			if (err != OK) {
				return 0;
			}
		}
		SolersRmlMemoryFile *file = memnew(SolersRmlMemoryFile);
		file->data = data;
		return reinterpret_cast<Rml::FileHandle>(file);
	}

	void Close(Rml::FileHandle p_file) override {
		SolersRmlMemoryFile *file = reinterpret_cast<SolersRmlMemoryFile *>(p_file);
		memdelete(file);
	}

	size_t Read(void *p_buffer, size_t p_size, Rml::FileHandle p_file) override {
		SolersRmlMemoryFile *file = reinterpret_cast<SolersRmlMemoryFile *>(p_file);
		if (!file || p_size == 0) {
			return 0;
		}
		const size_t remaining = file->cursor < size_t(file->data.size()) ? size_t(file->data.size()) - file->cursor : 0;
		const size_t count = MIN(p_size, remaining);
		if (count > 0) {
			memcpy(p_buffer, file->data.ptr() + file->cursor, count);
			file->cursor += count;
		}
		return count;
	}

	bool Seek(Rml::FileHandle p_file, long p_offset, int p_origin) override {
		SolersRmlMemoryFile *file = reinterpret_cast<SolersRmlMemoryFile *>(p_file);
		ERR_FAIL_NULL_V(file, false);
		long long base = 0;
		if (p_origin == SEEK_CUR) {
			base = (long long)file->cursor;
		} else if (p_origin == SEEK_END) {
			base = file->data.size();
		}
		const long long next = CLAMP(base + p_offset, 0LL, (long long)file->data.size());
		file->cursor = (size_t)next;
		return true;
	}

	size_t Tell(Rml::FileHandle p_file) override {
		SolersRmlMemoryFile *file = reinterpret_cast<SolersRmlMemoryFile *>(p_file);
		return file ? file->cursor : 0;
	}

	bool LoadFile(const Rml::String &p_path, Rml::String &r_data) override {
		const String path = solers_to_godot_string(p_path);
		if (path.ends_with("solers_chat.rcss")) {
			r_data = solers_to_rml_string(SolersChatUISpec::get_chat_pane_rcss());
			return true;
		}
		Error err = OK;
		const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(path, &err);
		if (err != OK) {
			return false;
		}
		r_data.assign(reinterpret_cast<const char *>(bytes.ptr()), bytes.size());
		return true;
	}
};

class SolersRmlSystemInterface : public Rml::SystemInterface {
public:
	double GetElapsedTime() override {
		return double(OS::get_singleton()->get_ticks_msec()) / 1000.0;
	}

	void JoinPath(Rml::String &r_translated_path, const Rml::String &p_document_path, const Rml::String &p_path) override {
		const String path = solers_to_godot_string(p_path);
		if (path.begins_with("solers://")) {
			r_translated_path = p_path;
			return;
		}
		Rml::SystemInterface::JoinPath(r_translated_path, p_document_path, p_path);
	}

	bool LogMessage(Rml::Log::Type p_type, const Rml::String &p_message) override {
		const String message = "[Solers RmlUi] " + solers_to_godot_string(p_message);
		if (p_type == Rml::Log::LT_ERROR || p_type == Rml::Log::LT_ASSERT) {
			ERR_PRINT(message);
		} else if (p_type == Rml::Log::LT_WARNING) {
			WARN_PRINT(message);
		} else {
			print_line(message);
		}
		return true;
	}

	void SetClipboardText(const Rml::String &p_text) override {
		if (DisplayServer::get_singleton()) {
			DisplayServer::get_singleton()->clipboard_set(solers_to_godot_string(p_text));
		}
	}

	void GetClipboardText(Rml::String &r_text) override {
		if (DisplayServer::get_singleton()) {
			r_text = solers_to_rml_string(DisplayServer::get_singleton()->clipboard_get());
		}
	}
};

struct SolersRmlTexture {
	Ref<Texture2D> texture;
};

struct SolersRmlGeometry {
	Vector<Rml::Vertex> vertices;
	Vector<int> indices;
};

class SolersRmlRenderInterface : public Rml::RenderInterface {
	SolersRmlChatSurface *target = nullptr;
	bool scissor_enabled = false;
	Rect2 scissor_rect;

	static Rml::TextureHandle _texture_handle_from_image(const Ref<Image> &p_image, Rml::Vector2i &r_dimensions) {
		if (p_image.is_null() || p_image->is_empty()) {
			return 0;
		}
		Ref<ImageTexture> texture = ImageTexture::create_from_image(p_image);
		if (texture.is_null()) {
			return 0;
		}
		r_dimensions = Rml::Vector2i(p_image->get_width(), p_image->get_height());
		SolersRmlTexture *record = memnew(SolersRmlTexture);
		record->texture = texture;
		return reinterpret_cast<Rml::TextureHandle>(record);
	}

public:
	void begin(SolersRmlChatSurface *p_target) {
		target = p_target;
		scissor_enabled = false;
		scissor_rect = Rect2();
	}

	void end() {
		target = nullptr;
	}

	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> p_vertices, Rml::Span<const int> p_indices) override {
		SolersRmlGeometry *geometry = memnew(SolersRmlGeometry);
		geometry->vertices.resize((int)p_vertices.size());
		for (int i = 0; i < geometry->vertices.size(); i++) {
			geometry->vertices.write[i] = p_vertices[i];
		}
		geometry->indices.resize((int)p_indices.size());
		for (int i = 0; i < geometry->indices.size(); i++) {
			geometry->indices.write[i] = p_indices[i];
		}
		return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
	}

	void RenderGeometry(Rml::CompiledGeometryHandle p_geometry, Rml::Vector2f p_translation, Rml::TextureHandle p_texture) override {
		if (!target || p_geometry == 0) {
			return;
		}
		SolersRmlGeometry *geometry = reinterpret_cast<SolersRmlGeometry *>(p_geometry);
		SolersRmlTexture *texture_record = reinterpret_cast<SolersRmlTexture *>(p_texture);
		Ref<Texture2D> texture = texture_record ? texture_record->texture : Ref<Texture2D>();

		for (int i = 0; i + 2 < geometry->indices.size(); i += 3) {
			Vector<SolersRmlDrawVertex> polygon;
			polygon.resize(3);
			Rect2 bounds;

			for (int j = 0; j < 3; j++) {
				const Rml::Vertex &vertex = geometry->vertices[geometry->indices[i + j]];
				const Point2 point(vertex.position.x + p_translation.x, vertex.position.y + p_translation.y);
				polygon.write[j].point = point;
				polygon.write[j].uv = Point2(vertex.tex_coord.x, vertex.tex_coord.y);
				polygon.write[j].color = solers_unpremultiply(vertex.colour);
				if (j == 0) {
					bounds = Rect2(point, Size2());
				} else {
					bounds = bounds.expand(point);
				}
			}

			if (scissor_enabled && !bounds.intersects(scissor_rect)) {
				continue;
			}
			if (scissor_enabled) {
				polygon = solers_clip_polygon_to_rect(polygon, scissor_rect);
			}
			if (polygon.size() < 3) {
				continue;
			}
			for (int j = 1; j + 1 < polygon.size(); j++) {
				Vector<Point2> points;
				Vector<Point2> uvs;
				Vector<Color> colors;
				points.resize(3);
				uvs.resize(3);
				colors.resize(3);
				const SolersRmlDrawVertex triangle[3] = { polygon[0], polygon[j], polygon[j + 1] };
				for (int k = 0; k < 3; k++) {
					points.write[k] = triangle[k].point;
					uvs.write[k] = triangle[k].uv;
					colors.write[k] = triangle[k].color;
				}
				target->draw_polygon(points, colors, uvs, texture);
			}
		}
	}

	void ReleaseGeometry(Rml::CompiledGeometryHandle p_geometry) override {
		if (p_geometry != 0) {
			memdelete(reinterpret_cast<SolersRmlGeometry *>(p_geometry));
		}
	}

	Rml::TextureHandle LoadTexture(Rml::Vector2i &r_texture_dimensions, const Rml::String &p_source) override {
		const String source = solers_to_godot_string(p_source);
		Ref<Image> image;
		if (source == "solers://logo") {
			const String logo = solers_logo_path();
			if (!logo.is_empty()) {
				image.instantiate();
				if (image->load(logo) != OK) {
					image.unref();
				}
			}
		} else if (source.begins_with("solers://icon/")) {
			image.instantiate();
			const String name = solers_icon_name_from_source(source);
			const String svg = solers_icon_svg(name, solers_icon_color(source));
			if (ImageLoaderSVG::create_image_from_string(image, svg, EDSCALE, false, HashMap<Color, Color>()) != OK) {
				image.unref();
			}
		} else {
			image.instantiate();
			if (image->load(source) != OK) {
				image.unref();
			}
		}

		return _texture_handle_from_image(image, r_texture_dimensions);
	}

	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> p_source, Rml::Vector2i p_source_dimensions) override {
		Vector<uint8_t> data = solers_unpremultiply_pixels(p_source);
		Ref<Image> image = Image::create_from_data(p_source_dimensions.x, p_source_dimensions.y, false, Image::FORMAT_RGBA8, data);
		Rml::Vector2i dimensions;
		return _texture_handle_from_image(image, dimensions);
	}

	void ReleaseTexture(Rml::TextureHandle p_texture) override {
		if (p_texture != 0) {
			memdelete(reinterpret_cast<SolersRmlTexture *>(p_texture));
		}
	}

	void EnableScissorRegion(bool p_enable) override {
		scissor_enabled = p_enable;
	}

	void SetScissorRegion(Rml::Rectanglei p_region) override {
		scissor_rect = Rect2(Point2(p_region.Left(), p_region.Top()), Size2(p_region.Width(), p_region.Height()));
	}
};

class SolersRmlButtonListener : public Rml::EventListener {
	SolersRmlChatSurface *surface = nullptr;

public:
	explicit SolersRmlButtonListener(SolersRmlChatSurface *p_surface) :
			surface(p_surface) {}

	void ProcessEvent(Rml::Event &p_event) override;
};

static SolersRmlSystemInterface *g_system_interface = nullptr;
static SolersRmlFileInterface *g_file_interface = nullptr;
static int g_runtime_refcount = 0;
static bool g_runtime_ready = false;

static bool solers_rml_runtime_acquire() {
	if (g_runtime_refcount == 0) {
		g_system_interface = memnew(SolersRmlSystemInterface);
		g_file_interface = memnew(SolersRmlFileInterface);
		Rml::SetSystemInterface(g_system_interface);
		Rml::SetFileInterface(g_file_interface);
		g_runtime_ready = Rml::Initialise();
		if (g_runtime_ready) {
			const String regular = solers_font_path("Inter_Regular.woff2");
			const String bold = solers_font_path("Inter_Bold.woff2");
			if (!regular.is_empty()) {
				Rml::LoadFontFace(solers_to_rml_string(regular), "Inter", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, true);
			}
			if (!bold.is_empty()) {
				Rml::LoadFontFace(solers_to_rml_string(bold), "Inter", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold, false);
			}
			// Register broad-coverage fallback faces so non-Latin scripts (CJK in
			// particular) render real glyphs instead of missing-glyph boxes. Inter
			// carries no CJK coverage, so without these any Chinese/Japanese/Korean
			// text shows up as tofu rectangles. Fallback faces are consulted, in
			// load order, for any codepoint absent from the primary font. We use the
			// two-argument LoadFontFace overload so each fallback keeps its own
			// family name and does not overwrite the "Inter" face used for Latin.
			static const char *const fallback_font_files[] = {
				"DroidSansFallback.woff2", // Simplified/Traditional Chinese, Japanese, Korean
				"DroidSansJapanese.woff2", // Additional Japanese coverage
			};
			for (const char *const fallback_file : fallback_font_files) {
				const String fallback_path = solers_font_path(fallback_file);
				if (!fallback_path.is_empty()) {
					Rml::LoadFontFace(solers_to_rml_string(fallback_path), true);
				}
			}
		}
	}
	g_runtime_refcount++;
	return g_runtime_ready;
}

static void solers_rml_runtime_release() {
	ERR_FAIL_COND(g_runtime_refcount <= 0);
	g_runtime_refcount--;
	if (g_runtime_refcount == 0) {
		if (g_runtime_ready) {
			Rml::Shutdown();
		}
		g_runtime_ready = false;
		memdelete(g_file_interface);
		memdelete(g_system_interface);
		g_file_interface = nullptr;
		g_system_interface = nullptr;
	}
}

static Rml::Input::KeyIdentifier solers_rml_key(Key p_key) {
	switch (p_key) {
		case Key::SPACE:
			return Rml::Input::KI_SPACE;
		case Key::BACKSPACE:
			return Rml::Input::KI_BACK;
		case Key::TAB:
			return Rml::Input::KI_TAB;
		case Key::ENTER:
			return Rml::Input::KI_RETURN;
		case Key::KP_ENTER:
			return Rml::Input::KI_NUMPADENTER;
		case Key::ESCAPE:
			return Rml::Input::KI_ESCAPE;
		case Key::PAGEUP:
			return Rml::Input::KI_PRIOR;
		case Key::PAGEDOWN:
			return Rml::Input::KI_NEXT;
		case Key::END:
			return Rml::Input::KI_END;
		case Key::HOME:
			return Rml::Input::KI_HOME;
		case Key::LEFT:
			return Rml::Input::KI_LEFT;
		case Key::UP:
			return Rml::Input::KI_UP;
		case Key::RIGHT:
			return Rml::Input::KI_RIGHT;
		case Key::DOWN:
			return Rml::Input::KI_DOWN;
		case Key::INSERT:
			return Rml::Input::KI_INSERT;
		case Key::KEY_DELETE:
			return Rml::Input::KI_DELETE;
		default:
			break;
	}
	if (p_key >= Key::A && p_key <= Key::Z) {
		return Rml::Input::KeyIdentifier(int(Rml::Input::KI_A) + int(p_key) - int(Key::A));
	}
	if (p_key >= Key::KEY_0 && p_key <= Key::KEY_9) {
		return Rml::Input::KeyIdentifier(int(Rml::Input::KI_0) + int(p_key) - int(Key::KEY_0));
	}
	return Rml::Input::KI_UNKNOWN;
}

static int solers_rml_modifiers(const Ref<InputEventWithModifiers> &p_event) {
	int modifiers = 0;
	if (p_event.is_valid()) {
		if (p_event->is_ctrl_pressed()) {
			modifiers |= Rml::Input::KM_CTRL;
		}
		if (p_event->is_shift_pressed()) {
			modifiers |= Rml::Input::KM_SHIFT;
		}
		if (p_event->is_alt_pressed()) {
			modifiers |= Rml::Input::KM_ALT;
		}
		if (p_event->is_meta_pressed()) {
			modifiers |= Rml::Input::KM_META;
		}
	}
	return modifiers;
}

} // namespace

class SolersRmlComposerChangeListener : public Rml::EventListener {
	SolersRmlChatSurface *surface = nullptr;

public:
	explicit SolersRmlComposerChangeListener(SolersRmlChatSurface *p_surface) :
			surface(p_surface) {}

	void ProcessEvent(Rml::Event &p_event) override;
};

class SolersRmlTextInputHandler : public Rml::TextInputHandler {
	SolersRmlChatSurface *surface = nullptr;

public:
	Rml::TextInputContext *active_context = nullptr;

	explicit SolersRmlTextInputHandler(SolersRmlChatSurface *p_surface) :
			surface(p_surface) {}

	void OnActivate(Rml::TextInputContext *p_input_context) override {
		active_context = p_input_context;
		if (surface) {
			surface->_open_ime_window();
		}
	}

	void OnDeactivate(Rml::TextInputContext *p_input_context) override {
		if (active_context == p_input_context) {
			active_context = nullptr;
		}
		if (surface) {
			surface->_close_ime_window();
		}
	}

	void OnDestroy(Rml::TextInputContext *p_input_context) override {
		if (active_context == p_input_context) {
			active_context = nullptr;
		}
	}
};

struct SolersRmlChatSurface::Impl {
	Rml::Context *context = nullptr;
	Rml::ElementDocument *document = nullptr;
	SolersRmlRenderInterface render_interface;
	SolersRmlButtonListener send_listener;
	SolersRmlComposerChangeListener composer_change_listener;
	SolersRmlTextInputHandler text_input_handler;
	Rml::String context_name;
	String messages_rml;
	bool runtime_acquired = false;
	bool document_loaded = false;
	bool animation_suspended = false;
	bool composer_layout_dirty = true;
	String last_composer_value;
	String ime_text;
	uint64_t next_update_msec = 0;
	int composer_rows = 1;

	explicit Impl(SolersRmlChatSurface *p_owner) :
			send_listener(p_owner),
			composer_change_listener(p_owner),
			text_input_handler(p_owner) {}
};

void SolersRmlButtonListener::ProcessEvent(Rml::Event &p_event) {
	if (p_event.GetId() == Rml::EventId::Click && surface) {
		surface->submit_current_prompt();
	}
}

void SolersRmlComposerChangeListener::ProcessEvent(Rml::Event &p_event) {
	if (!surface) {
		return;
	}
	const Rml::EventId id = p_event.GetId();
	if (id == Rml::EventId::Change) {
		surface->_mark_composer_layout_dirty();
	} else if (id == Rml::EventId::Focus) {
		surface->_set_composer_focused(true);
	} else if (id == Rml::EventId::Blur) {
		surface->_set_composer_focused(false);
	}
}

void SolersRmlChatSurface::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_animation_suspended", "suspended"), &SolersRmlChatSurface::set_animation_suspended);
	ADD_SIGNAL(MethodInfo("prompt_submitted", PropertyInfo(Variant::STRING, "prompt")));
}

void SolersRmlChatSurface::_ensure_runtime() {
	if (!impl || impl->runtime_acquired) {
		return;
	}
	impl->runtime_acquired = solers_rml_runtime_acquire();
	if (!impl->runtime_acquired) {
		WARN_PRINT("Solers RmlUi runtime did not initialise.");
		return;
	}

	impl->context_name = solers_to_rml_string("solers_chat_" + itos(get_instance_id()));
	impl->context = Rml::CreateContext(impl->context_name, Rml::Vector2i(MAX(1, int(get_size().x)), MAX(1, int(get_size().y))), &impl->render_interface, &impl->text_input_handler);
	if (!impl->context) {
		WARN_PRINT("Solers could not create an RmlUi context.");
		return;
	}
	_reload_document();
}

void SolersRmlChatSurface::_release_runtime() {
	if (!impl) {
		return;
	}
	if (impl->document) {
		if (Rml::Element *send = impl->document->GetElementById("send-button")) {
			send->RemoveEventListener(Rml::EventId::Click, &impl->send_listener);
		}
		if (Rml::Element *input = impl->document->GetElementById("composer-input")) {
			input->RemoveEventListener(Rml::EventId::Change, &impl->composer_change_listener);
			input->RemoveEventListener(Rml::EventId::Focus, &impl->composer_change_listener);
			input->RemoveEventListener(Rml::EventId::Blur, &impl->composer_change_listener);
		}
		impl->document = nullptr;
	}
	if (impl->context) {
		Rml::RemoveContext(impl->context_name);
		impl->context = nullptr;
	}
	if (impl->runtime_acquired) {
		solers_rml_runtime_release();
		impl->runtime_acquired = false;
	}
}

void SolersRmlChatSurface::_reload_document() {
	if (!impl || !impl->context) {
		return;
	}
	if (impl->document) {
		if (Rml::Element *send = impl->document->GetElementById("send-button")) {
			send->RemoveEventListener(Rml::EventId::Click, &impl->send_listener);
		}
		if (Rml::Element *input = impl->document->GetElementById("composer-input")) {
			input->RemoveEventListener(Rml::EventId::Change, &impl->composer_change_listener);
			input->RemoveEventListener(Rml::EventId::Focus, &impl->composer_change_listener);
			input->RemoveEventListener(Rml::EventId::Blur, &impl->composer_change_listener);
		}
		impl->document->Close();
		impl->document = nullptr;
	}
	impl->document = impl->context->LoadDocumentFromMemory(solers_to_rml_string(SolersChatUISpec::get_chat_pane_rml()), "solers_chat_pane.rml");
	if (!impl->document) {
		WARN_PRINT("Solers RmlUi chat document failed to load.");
		return;
	}
	if (Rml::Element *send = impl->document->GetElementById("send-button")) {
		send->AddEventListener(Rml::EventId::Click, &impl->send_listener);
	}
	if (Rml::Element *input = impl->document->GetElementById("composer-input")) {
		input->AddEventListener(Rml::EventId::Change, &impl->composer_change_listener);
		input->AddEventListener(Rml::EventId::Focus, &impl->composer_change_listener);
		input->AddEventListener(Rml::EventId::Blur, &impl->composer_change_listener);
	}
	impl->document->Show();
	impl->document_loaded = true;
	_update_context_size();
	_set_messages_rml(impl->messages_rml);
	impl->composer_layout_dirty = true;
	_sync_composer_layout(true);
	_request_rml_update();
}

void SolersRmlChatSurface::_update_context_size() {
	if (!impl || !impl->context) {
		return;
	}
	const int width = MAX(1, int(get_size().x));
	const int height = MAX(1, int(get_size().y));
	impl->context->SetDimensions(Rml::Vector2i(width, height));
	if (impl->document) {
		impl->document->SetProperty("width", vformat("%dpx", width).utf8().get_data());
		impl->document->SetProperty("height", vformat("%dpx", height).utf8().get_data());
	}
}

void SolersRmlChatSurface::_request_rml_update() {
	if (!impl || impl->animation_suspended) {
		return;
	}
	impl->next_update_msec = 0;
	if (impl->context) {
		set_process(true);
	}
	queue_redraw();
}

void SolersRmlChatSurface::_schedule_next_rml_update() {
	if (!impl || !impl->context || impl->animation_suspended) {
		set_process(false);
		return;
	}

	const double delay = impl->context->GetNextUpdateDelay();
	if (Math::is_inf(delay)) {
		set_process(false);
		return;
	}

	const uint64_t now_msec = OS::get_singleton()->get_ticks_msec();
	impl->next_update_msec = now_msec + uint64_t(MAX(0.0, delay) * 1000.0);
	set_process(true);
}

void SolersRmlChatSurface::_set_messages_rml(const String &p_rml) {
	if (!impl || !impl->document) {
		return;
	}
	Rml::Element *messages = impl->document->GetElementById("messages");
	if (!messages) {
		return;
	}
	if (p_rml.is_empty()) {
		return;
	}
	messages->SetInnerRML(solers_to_rml_string(p_rml));
}

void SolersRmlChatSurface::_mark_composer_layout_dirty() {
	if (!impl) {
		return;
	}
	impl->composer_layout_dirty = true;
	_request_rml_update();
}

void SolersRmlChatSurface::_set_composer_focused(bool p_focused) {
	if (!impl || !impl->document) {
		return;
	}
	if (Rml::Element *composer = impl->document->GetElementById("composer-root")) {
		composer->SetClass("focused", p_focused);
	}
	_request_rml_update();
}

void SolersRmlChatSurface::_sync_composer_layout(bool p_force_context_update) {
	if (!impl || !impl->document) {
		return;
	}
	if (!impl->composer_layout_dirty && !p_force_context_update) {
		return;
	}
	Rml::Element *input_element = impl->document->GetElementById("composer-input");
	Rml::ElementFormControlTextArea *textarea = rmlui_dynamic_cast<Rml::ElementFormControlTextArea *>(input_element);
	if (!textarea) {
		return;
	}

	const String value = solers_to_godot_string(textarea->GetValue());
	impl->last_composer_value = value;

	// The text area's intrinsic height is rows * line-height; RmlUi has no
	// content-based auto-sizing for text areas (see ElementFormControlTextArea::
	// GetIntrinsicDimensions). So we measure the real wrapped content height and
	// pick the smallest row count that avoids vertical overflow, capped at
	// MAX_COMPOSER_ROWS. Beyond the cap the internal overflow-y:auto scrollbar
	// takes over. Critically, layout/scroll metrics are computed lazily during
	// Context::Update(), so we must refresh after every SetNumRows() before
	// reading GetScrollHeight()/GetClientHeight(); the previous implementation
	// read stale metrics and therefore never grew for soft-wrapped lines.
	const int MAX_COMPOSER_ROWS = 8;

	// Reset to a single row first so the measurement can shrink as well as grow.
	if (impl->composer_rows != 1) {
		textarea->SetNumRows(1);
	}
	impl->context->Update();

	int rows = 1;
	while (rows < MAX_COMPOSER_ROWS && textarea->GetScrollHeight() > textarea->GetClientHeight() + 1.0f) {
		rows++;
		textarea->SetNumRows(rows);
		impl->context->Update();
	}
	impl->composer_rows = rows;

	// Once we are scrolling inside the capped area, keep the caret line in view.
	if (textarea->GetScrollHeight() > textarea->GetClientHeight() + 1.0f) {
		textarea->SetScrollTop(textarea->GetScrollHeight());
	}
	impl->composer_layout_dirty = false;
}

void SolersRmlChatSurface::_open_ime_window() {
	if (!impl || !DisplayServer::get_singleton() || !DisplayServer::get_singleton()->has_feature(DisplayServer::FEATURE_IME)) {
		return;
	}
	DisplayServer::WindowID wid = get_window() ? get_window()->get_window_id() : DisplayServer::INVALID_WINDOW_ID;
	if (wid == DisplayServer::INVALID_WINDOW_ID) {
		return;
	}
	DisplayServer::get_singleton()->window_set_ime_active(true, wid);
	_update_ime_window_position();
}

void SolersRmlChatSurface::_close_ime_window() {
	if (!impl || !DisplayServer::get_singleton() || !DisplayServer::get_singleton()->has_feature(DisplayServer::FEATURE_IME)) {
		return;
	}
	DisplayServer::WindowID wid = get_window() ? get_window()->get_window_id() : DisplayServer::INVALID_WINDOW_ID;
	if (wid == DisplayServer::INVALID_WINDOW_ID) {
		return;
	}
	impl->ime_text = String();
	DisplayServer::get_singleton()->window_set_ime_position(Point2(), wid);
	DisplayServer::get_singleton()->window_set_ime_active(false, wid);
}

void SolersRmlChatSurface::_update_ime_window_position() {
	if (!impl || !impl->document || !DisplayServer::get_singleton() || !DisplayServer::get_singleton()->has_feature(DisplayServer::FEATURE_IME)) {
		return;
	}
	DisplayServer::WindowID wid = get_window() ? get_window()->get_window_id() : DisplayServer::INVALID_WINDOW_ID;
	if (wid == DisplayServer::INVALID_WINDOW_ID) {
		return;
	}

	Point2 pos = get_global_position();
	if (Rml::Element *input = impl->document->GetElementById("composer-input")) {
		const Rml::Vector2f offset = input->GetAbsoluteOffset();
		const Rml::Vector2f size = input->GetBox().GetSize();
		pos += Point2(offset.x + 8.0f, offset.y + size.y);
	} else {
		pos += Point2(24.0f, get_size().y - 44.0f);
	}
	if (get_window()->get_embedder()) {
		pos += get_viewport()->get_popup_base_transform().get_origin();
	}
	pos = get_window()->get_screen_transform().xform(pos);
	DisplayServer::get_singleton()->window_set_ime_position(pos, wid);
}

void SolersRmlChatSurface::submit_current_prompt() {
	if (!impl || !impl->document) {
		return;
	}
	Rml::Element *input_element = impl->document->GetElementById("composer-input");
	Rml::ElementFormControlTextArea *textarea = rmlui_dynamic_cast<Rml::ElementFormControlTextArea *>(input_element);
	if (!textarea) {
		return;
	}
	const String prompt = solers_to_godot_string(textarea->GetValue()).strip_edges();
	if (prompt.is_empty()) {
		return;
	}
	textarea->SetValue("");
	impl->composer_rows = 1;
	impl->last_composer_value = String();
	impl->composer_layout_dirty = true;
	textarea->SetNumRows(1);
	emit_signal(SNAME("prompt_submitted"), prompt);
	_request_rml_update();
}

void SolersRmlChatSurface::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_clip_contents(true);
			set_mouse_filter(MOUSE_FILTER_STOP);
			_ensure_runtime();
			_request_rml_update();
		} break;
		case NOTIFICATION_RESIZED: {
			if (!impl || !impl->animation_suspended) {
				_update_context_size();
				_request_rml_update();
			}
		} break;
		case NOTIFICATION_DRAW: {
			if (!impl || !impl->context || impl->animation_suspended) {
				draw_rect(Rect2(Point2(), get_size()), Color(0.105, 0.106, 0.112, 1.0), true);
				return;
			}
			_sync_composer_layout(false);
			impl->context->Update();
			impl->render_interface.begin(this);
			impl->context->Render();
			impl->render_interface.end();
			_schedule_next_rml_update();
		} break;
		case NOTIFICATION_PROCESS: {
			if (!impl || impl->animation_suspended) {
				set_process(false);
				return;
			}
			if (impl->next_update_msec == 0 || OS::get_singleton()->get_ticks_msec() >= impl->next_update_msec) {
				queue_redraw();
			}
		} break;
		case NOTIFICATION_MOUSE_EXIT: {
			if (impl && impl->context) {
				// Clear hover/active state when the pointer leaves the panel so a
				// button does not stay visually hovered, and repaint once.
				impl->context->ProcessMouseLeave();
				_request_rml_update();
			}
		} break;
		case NOTIFICATION_FOCUS_EXIT: {
			_close_ime_window();
		} break;
		case NOTIFICATION_FOCUS_ENTER: {
			if (impl && impl->text_input_handler.active_context) {
				_open_ime_window();
			}
		} break;
		case MainLoop::NOTIFICATION_OS_IME_UPDATE: {
			if (!impl || !impl->context || !impl->text_input_handler.active_context || !DisplayServer::get_singleton()) {
				break;
			}
			const String new_ime_text = DisplayServer::get_singleton()->ime_get_text();
			if (new_ime_text == impl->ime_text) {
				break;
			}
			impl->ime_text = new_ime_text;
			_update_ime_window_position();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_close_ime_window();
			_release_runtime();
		} break;
	}
}

void SolersRmlChatSurface::gui_input(const Ref<InputEvent> &p_event) {
	if (!impl || !impl->context) {
		return;
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		const Vector2 position = motion->get_position();
		impl->context->ProcessMouseMove((int)position.x, (int)position.y, solers_rml_modifiers(motion));
		accept_event();
		// A mouse move can change the hover/active chain (button :hover
		// backgrounds, cursor shape, etc.). RmlUi only reflects that on the next
		// Update()/Render(), so we must always request a redraw here. The old
		// gate on GetNextUpdateDelay()==0 is only true while an animation is
		// running, so hover states stayed stale until some unrelated event forced
		// a repaint, which felt like a multi-second lag.
		_request_rml_update();
		return;
	}

	Ref<InputEventMouseButton> mouse_button = p_event;
	if (mouse_button.is_valid()) {
		const MouseButton button = mouse_button->get_button_index();
		if (button == MouseButton::WHEEL_UP || button == MouseButton::WHEEL_DOWN || button == MouseButton::WHEEL_LEFT || button == MouseButton::WHEEL_RIGHT) {
			Rml::Vector2f delta(0.0f, 0.0f);
			if (button == MouseButton::WHEEL_UP) {
				delta.y = -mouse_button->get_factor();
			} else if (button == MouseButton::WHEEL_DOWN) {
				delta.y = mouse_button->get_factor();
			} else if (button == MouseButton::WHEEL_LEFT) {
				delta.x = -mouse_button->get_factor();
			} else {
				delta.x = mouse_button->get_factor();
			}
			impl->context->ProcessMouseWheel(delta, solers_rml_modifiers(mouse_button));
			accept_event();
			_request_rml_update();
			return;
		}

		int rml_button = -1;
		if (button == MouseButton::LEFT) {
			rml_button = 0;
		} else if (button == MouseButton::RIGHT) {
			rml_button = 1;
		} else if (button == MouseButton::MIDDLE) {
			rml_button = 2;
		}
		if (rml_button >= 0) {
			const Vector2 position = mouse_button->get_position();
			impl->context->ProcessMouseMove((int)position.x, (int)position.y, solers_rml_modifiers(mouse_button));
			if (mouse_button->is_pressed()) {
				grab_focus();
				impl->context->ProcessMouseButtonDown(rml_button, solers_rml_modifiers(mouse_button));
				_update_ime_window_position();
			} else {
				impl->context->ProcessMouseButtonUp(rml_button, solers_rml_modifiers(mouse_button));
			}
			accept_event();
			_request_rml_update();
			return;
		}
	}

	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed()) {
		const Key keycode = key->get_keycode();
		if (!key->is_echo() && (keycode == Key::ENTER || keycode == Key::KP_ENTER) && !key->is_shift_pressed()) {
			submit_current_prompt();
			accept_event();
			return;
		}
		bool text_changed = false;
		const Rml::Input::KeyIdentifier rml_key = solers_rml_key(keycode);
		if (rml_key != Rml::Input::KI_UNKNOWN) {
			impl->context->ProcessKeyDown(rml_key, solers_rml_modifiers(key));
			if (keycode == Key::BACKSPACE || keycode == Key::KEY_DELETE || keycode == Key::ENTER || keycode == Key::KP_ENTER) {
				text_changed = true;
			}
		}
		if (keycode == Key::ENTER && key->is_shift_pressed()) {
			impl->context->ProcessTextInput('\n');
			text_changed = true;
		} else if (!key->is_ctrl_pressed() && !key->is_meta_pressed() && key->get_unicode() >= 32) {
			const char32_t codepoint = key->get_unicode();
			char32_t chars[2] = { codepoint, 0 };
			impl->context->ProcessTextInput(String(chars).utf8().get_data());
			text_changed = true;
		}
		if (text_changed) {
			impl->composer_layout_dirty = true;
			_update_ime_window_position();
		}
		accept_event();
		_request_rml_update();
	}
}

void SolersRmlChatSurface::set_animation_suspended(bool p_suspended) {
	if (!impl) {
		return;
	}
	if (impl->animation_suspended == p_suspended) {
		if (p_suspended) {
			queue_redraw();
		}
		return;
	}
	impl->animation_suspended = p_suspended;
	if (p_suspended) {
		set_process(false);
		queue_redraw();
		return;
	}
	_update_context_size();
	_request_rml_update();
}

void SolersRmlChatSurface::append_message(const String &p_speaker, const String &p_message) {
	if (!impl) {
		return;
	}
	const bool user = p_speaker.to_lower() == "you" || p_speaker.to_lower() == "user";
	if (user) {
		impl->messages_rml += "<div class=\"user-bubble\">" + solers_rml_escape(p_message) + "</div>";
	} else {
		impl->messages_rml += "<div class=\"agent-step-card strong\">" + solers_rml_escape(p_message) + "</div>";
	}
	_set_messages_rml(impl->messages_rml);
	_request_rml_update();
}

bool SolersRmlChatSurface::is_runtime_ready() const {
	return impl && impl->runtime_acquired && impl->context && impl->document_loaded;
}

SolersRmlChatSurface::SolersRmlChatSurface() {
	impl = memnew(Impl(this));
	set_focus_mode(FOCUS_ALL);
	set_h_size_flags(Control::SIZE_EXPAND_FILL);
	set_v_size_flags(Control::SIZE_EXPAND_FILL);
}

SolersRmlChatSurface::~SolersRmlChatSurface() {
	_release_runtime();
	memdelete(impl);
	impl = nullptr;
}

#else

void SolersRmlChatSurface::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_animation_suspended", "suspended"), &SolersRmlChatSurface::set_animation_suspended);
	ADD_SIGNAL(MethodInfo("prompt_submitted", PropertyInfo(Variant::STRING, "prompt")));
}

void SolersRmlChatSurface::_notification(int p_what) {
	if (p_what == NOTIFICATION_DRAW) {
		draw_rect(Rect2(Point2(), get_size()), Color(0.105, 0.106, 0.112, 1.0), true);
	}
}

void SolersRmlChatSurface::gui_input(const Ref<InputEvent> &p_event) {
}

void SolersRmlChatSurface::append_message(const String &p_speaker, const String &p_message) {
}

void SolersRmlChatSurface::set_animation_suspended(bool p_suspended) {
}

bool SolersRmlChatSurface::is_runtime_ready() const {
	return false;
}

SolersRmlChatSurface::SolersRmlChatSurface() {
}

SolersRmlChatSurface::~SolersRmlChatSurface() {
}

#endif
