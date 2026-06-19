/**************************************************************************/
/*  solers_markdown_view.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_markdown_view.h"

#include "core/io/resource_loader.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "editor/editor_interface.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/rich_text_label.h"
#include "scene/resources/style_box.h"
#include "scene/resources/style_box_flat.h"
#include "scene/theme/theme_db.h"
#include "servers/display/display_server.h"

#include "thirdparty/md4c/md4c.h"

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

static const Color SOLERS_MD_TEXT_BODY = Color(0.918, 0.929, 0.945);
static const Color SOLERS_MD_TEXT_DIM = Color(0.667, 0.690, 0.733);
static const Color SOLERS_MD_LINK = Color::hex(0x6fb1ffff);
// Inline code: no box (Cursor-style), just a soft neutral tint + mono shaping.
static const Color SOLERS_MD_CODE_SPAN_TEXT = Color(0.847, 0.871, 0.914);
static const Color SOLERS_MD_QUOTE_TEXT = Color(0.78, 0.80, 0.84);
static const Color SOLERS_MD_RULE = Color(1, 1, 1, 0.12f);
static const Color SOLERS_MD_TABLE_BORDER = Color(1, 1, 1, 0.08f);
static const Color SOLERS_MD_TABLE_HEADER_BG = Color(1, 1, 1, 0.06f);
static const Color SOLERS_MD_TABLE_CELL_BG = Color(1, 1, 1, 0.025f);
static const Color SOLERS_MD_CODE_PANEL_BG = Color(0.071, 0.082, 0.102);
static const Color SOLERS_MD_CODE_PANEL_BORDER = Color(1, 1, 1, 0.055f);

// Syntax highlighting (Godot editor inspired).
static const Color SOLERS_SYN_KEYWORD = Color::hex(0xff7085ff);
static const Color SOLERS_SYN_STRING = Color::hex(0xffeda1ff);
static const Color SOLERS_SYN_NUMBER = Color::hex(0xa1ffe0ff);
static const Color SOLERS_SYN_COMMENT = Color(0.80, 0.84, 0.96, 0.36f);
static const Color SOLERS_SYN_FUNCTION = Color::hex(0x57b3ffff);
static const Color SOLERS_SYN_DEFAULT = Color(0.863, 0.890, 0.933);

static const char32_t SOLERS_MD_CARET = 0x258D; // "▍" stream write head.

/* ------------------------------------------------------------------ */
/* Lightweight syntax highlighting for code blocks                     */
/* ------------------------------------------------------------------ */

struct SolersLangProfile {
	HashSet<String> keywords;
	bool hash_comments = true;
	bool slash_comments = true;
};

static void solers_profile_add(SolersLangProfile &p_profile, const char *p_words) {
	for (const String &w : String(p_words).split(" ", false)) {
		p_profile.keywords.insert(w);
	}
}

static const SolersLangProfile &solers_lang_profile(const String &p_lang) {
	static SolersLangProfile gdscript, python, jslike, clike, json, generic;
	static bool initialized = false;
	if (!initialized) {
		initialized = true;

		solers_profile_add(gdscript,
				"func var const class class_name extends is in as self signal await static enum "
				"if elif else for while match when break continue pass return breakpoint tool super "
				"and or not true false null void set get export onready assert preload "
				"int float bool String StringName Array Dictionary Callable Signal Variant "
				"Vector2 Vector2i Vector3 Vector3i Vector4 Rect2 Transform2D Transform3D Color NodePath "
				"PI TAU INF NAN");
		gdscript.slash_comments = false;

		solers_profile_add(python,
				"def class import from as if elif else for while try except finally with return yield "
				"lambda pass break continue global nonlocal assert raise in is not and or del "
				"True False None async await match case");
		python.slash_comments = false;

		solers_profile_add(jslike,
				"function const let var class extends return if else for while do switch case break "
				"continue new delete typeof instanceof this super import export from default try catch "
				"finally throw async await yield of in true false null undefined interface type enum "
				"implements private public protected readonly static void any number string boolean");
		jslike.hash_comments = false;

		solers_profile_add(clike,
				"int float double char bool void const static struct class enum union unsigned signed "
				"long short if else for while do switch case break continue return new delete this "
				"template typename namespace using public private protected virtual override final "
				"nullptr true false auto sizeof operator friend inline constexpr extern try catch throw "
				"string fn let mut impl trait pub match crate mod use go defer chan map range func "
				"package var interface select");
		clike.hash_comments = false;

		solers_profile_add(json, "true false null");
		json.hash_comments = false;

		solers_profile_add(generic,
				"if else for while return function func def class var const let true false null none");
	}

	const String l = p_lang.to_lower();
	if (l == "gd" || l == "gdscript") {
		return gdscript;
	}
	if (l == "py" || l == "python") {
		return python;
	}
	if (l == "js" || l == "javascript" || l == "ts" || l == "typescript" || l == "jsx" || l == "tsx") {
		return jslike;
	}
	if (l == "c" || l == "cpp" || l == "c++" || l == "h" || l == "hpp" || l == "cs" || l == "csharp" ||
			l == "java" || l == "rust" || l == "rs" || l == "go" || l == "glsl" || l == "shader" || l == "gdshader") {
		return clike;
	}
	if (l == "json" || l == "jsonc") {
		return json;
	}
	return generic;
}

enum SolersTokenKind {
	SOLERS_TOK_DEFAULT,
	SOLERS_TOK_KEYWORD,
	SOLERS_TOK_STRING,
	SOLERS_TOK_NUMBER,
	SOLERS_TOK_COMMENT,
	SOLERS_TOK_FUNCTION,
};

static bool solers_is_ident_char(char32_t c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Tokenizes `p_code` and emits colored runs into `p_label`. This is a
// presentation-grade highlighter (keywords/strings/numbers/comments/calls),
// not a language server; unknown languages degrade to plain text gracefully.
static void solers_highlight_code(RichTextLabel *p_label, const String &p_lang, const String &p_code) {
	const SolersLangProfile &profile = solers_lang_profile(p_lang);
	const int n = p_code.length();

	int run_start = 0;
	SolersTokenKind run_kind = SOLERS_TOK_DEFAULT;

	auto flush = [&](int p_end, SolersTokenKind p_next_kind) {
		if (p_end > run_start) {
			const String chunk = p_code.substr(run_start, p_end - run_start);
			switch (run_kind) {
				case SOLERS_TOK_DEFAULT:
					p_label->add_text(chunk);
					break;
				case SOLERS_TOK_KEYWORD:
					p_label->push_color(SOLERS_SYN_KEYWORD);
					p_label->add_text(chunk);
					p_label->pop();
					break;
				case SOLERS_TOK_STRING:
					p_label->push_color(SOLERS_SYN_STRING);
					p_label->add_text(chunk);
					p_label->pop();
					break;
				case SOLERS_TOK_NUMBER:
					p_label->push_color(SOLERS_SYN_NUMBER);
					p_label->add_text(chunk);
					p_label->pop();
					break;
				case SOLERS_TOK_COMMENT:
					p_label->push_color(SOLERS_SYN_COMMENT);
					p_label->add_text(chunk);
					p_label->pop();
					break;
				case SOLERS_TOK_FUNCTION:
					p_label->push_color(SOLERS_SYN_FUNCTION);
					p_label->add_text(chunk);
					p_label->pop();
					break;
			}
		}
		run_start = p_end;
		run_kind = p_next_kind;
	};

	int i = 0;
	while (i < n) {
		const char32_t c = p_code[i];

		// Comments.
		if ((c == '#' && profile.hash_comments) ||
				(c == '/' && profile.slash_comments && i + 1 < n && p_code[i + 1] == '/')) {
			flush(i, SOLERS_TOK_COMMENT);
			while (i < n && p_code[i] != '\n') {
				i++;
			}
			flush(i, SOLERS_TOK_DEFAULT);
			continue;
		}
		if (c == '/' && profile.slash_comments && i + 1 < n && p_code[i + 1] == '*') {
			flush(i, SOLERS_TOK_COMMENT);
			const int end = p_code.find("*/", i + 2);
			i = (end < 0) ? n : end + 2;
			flush(i, SOLERS_TOK_DEFAULT);
			continue;
		}

		// Strings (single line; escapes honored).
		if (c == '"' || c == '\'') {
			flush(i, SOLERS_TOK_STRING);
			int j = i + 1;
			while (j < n && p_code[j] != '\n') {
				if (p_code[j] == '\\') {
					j += 2;
					continue;
				}
				if (p_code[j] == c) {
					j++;
					break;
				}
				j++;
			}
			i = MIN(j, n);
			flush(i, SOLERS_TOK_DEFAULT);
			continue;
		}

		// Numbers.
		if (is_digit(c)) {
			flush(i, SOLERS_TOK_NUMBER);
			int j = i + 1;
			while (j < n && (solers_is_ident_char(p_code[j]) || p_code[j] == '.')) {
				j++;
			}
			i = j;
			flush(i, SOLERS_TOK_DEFAULT);
			continue;
		}

		// Identifiers: keyword or call.
		if (solers_is_ident_char(c) && !is_digit(c)) {
			int j = i + 1;
			while (j < n && solers_is_ident_char(p_code[j])) {
				j++;
			}
			const String ident = p_code.substr(i, j - i);
			SolersTokenKind kind = SOLERS_TOK_DEFAULT;
			if (profile.keywords.has(ident)) {
				kind = SOLERS_TOK_KEYWORD;
			} else {
				int k = j;
				while (k < n && (p_code[k] == ' ' || p_code[k] == '\t')) {
					k++;
				}
				if (k < n && p_code[k] == '(') {
					kind = SOLERS_TOK_FUNCTION;
				}
			}
			flush(i, kind);
			i = j;
			flush(i, SOLERS_TOK_DEFAULT);
			continue;
		}

		i++;
	}
	flush(n, SOLERS_TOK_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* md4c -> RichTextLabel renderer                                      */
/* ------------------------------------------------------------------ */

struct SolersMdFrame {
	int blocks = 0;
	bool text_seen = false;
};

struct SolersMdState {
	RichTextLabel *rtl = nullptr;
	int base_font_size = 14;
	int indent_level = 0; // shared by lists/quotes, mirrors RTL bbcode frames
	LocalVector<SolersMdFrame> frames;

	bool in_code_block = false;
	String code_buf;

	int img_depth = 0;
	String img_src;
	String img_alt;

	bool in_table_header = false;
	LocalVector<bool> cell_aligned;
};

static String solers_md_str(const MD_CHAR *p_text, MD_SIZE p_size) {
	return String::utf8(p_text, int(p_size));
}

static String solers_md_attr(const MD_ATTRIBUTE &p_attr) {
	return solers_md_str(p_attr.text, p_attr.size);
}

static String solers_md_entity(const String &p_entity) {
	if (p_entity.begins_with("&#")) {
		const bool hex = p_entity.length() > 2 && (p_entity[2] == 'x' || p_entity[2] == 'X');
		const String digits = p_entity.substr(hex ? 3 : 2).trim_suffix(";");
		const int64_t code = hex ? digits.hex_to_int() : digits.to_int();
		if (code > 0 && code <= 0x10FFFF) {
			return String::chr(char32_t(code));
		}
		return String::chr(0xFFFD);
	}
	if (p_entity == "&amp;") {
		return "&";
	}
	if (p_entity == "&lt;") {
		return "<";
	}
	if (p_entity == "&gt;") {
		return ">";
	}
	if (p_entity == "&quot;") {
		return "\"";
	}
	if (p_entity == "&apos;" || p_entity == "&#39;") {
		return "'";
	}
	if (p_entity == "&nbsp;") {
		return String::chr(0x00A0);
	}
	return p_entity;
}

// Emits the blank-line separation between sibling blocks of the current
// frame (never before the first child, so containers stay flush).
static void solers_md_block_gap(SolersMdState &p_state) {
	if (p_state.frames.is_empty()) {
		return;
	}
	SolersMdFrame &top = p_state.frames[p_state.frames.size() - 1];
	if (top.blocks > 0 || top.text_seen) {
		p_state.rtl->add_newline();
	}
	top.blocks++;
}

static int solers_md_enter_block(MD_BLOCKTYPE p_type, void *p_detail, void *p_userdata) {
	SolersMdState &s = *static_cast<SolersMdState *>(p_userdata);
	RichTextLabel *rtl = s.rtl;
	const float ed = EDSCALE;

	switch (p_type) {
		case MD_BLOCK_DOC: {
			s.frames.push_back(SolersMdFrame());
		} break;
		case MD_BLOCK_QUOTE: {
			solers_md_block_gap(s);
			s.indent_level++;
			rtl->push_indent(s.indent_level);
			rtl->push_color(SOLERS_MD_QUOTE_TEXT);
			s.frames.push_back(SolersMdFrame());
		} break;
		case MD_BLOCK_UL: {
			solers_md_block_gap(s);
			s.indent_level++;
			rtl->push_list(s.indent_level, RichTextLabel::LIST_DOTS, false);
			s.frames.push_back(SolersMdFrame());
		} break;
		case MD_BLOCK_OL: {
			solers_md_block_gap(s);
			s.indent_level++;
			rtl->push_list(s.indent_level, RichTextLabel::LIST_NUMBERS, false);
			s.frames.push_back(SolersMdFrame());
		} break;
		case MD_BLOCK_LI: {
			SolersMdFrame &top = s.frames[s.frames.size() - 1];
			if (top.blocks > 0) {
				rtl->add_newline();
			}
			top.blocks++;
			s.frames.push_back(SolersMdFrame());
			const MD_BLOCK_LI_DETAIL *li = static_cast<const MD_BLOCK_LI_DETAIL *>(p_detail);
			if (li && li->is_task) {
				rtl->add_text(li->task_mark == ' ' ? String::utf8("\u2610 ") : String::utf8("\u2611 "));
			}
		} break;
		case MD_BLOCK_HR: {
			solers_md_block_gap(s);
			rtl->add_hr(35, MAX(1, int(ed)), SOLERS_MD_RULE, HORIZONTAL_ALIGNMENT_LEFT, true, false);
		} break;
		case MD_BLOCK_H: {
			solers_md_block_gap(s);
			static const float factors[6] = { 1.45f, 1.3f, 1.15f, 1.05f, 1.0f, 0.95f };
			const MD_BLOCK_H_DETAIL *h = static_cast<const MD_BLOCK_H_DETAIL *>(p_detail);
			const int level = h ? CLAMP(int(h->level), 1, 6) : 1;
			rtl->push_font_size(int(Math::round(s.base_font_size * factors[level - 1])));
			rtl->push_bold();
		} break;
		case MD_BLOCK_CODE: {
			solers_md_block_gap(s);
			s.in_code_block = true;
			s.code_buf = String();
		} break;
		case MD_BLOCK_P: {
			solers_md_block_gap(s);
		} break;
		case MD_BLOCK_TABLE: {
			solers_md_block_gap(s);
			const MD_BLOCK_TABLE_DETAIL *table = static_cast<const MD_BLOCK_TABLE_DETAIL *>(p_detail);
			rtl->push_table(table ? MAX(1, int(table->col_count)) : 1, INLINE_ALIGNMENT_TOP);
		} break;
		case MD_BLOCK_THEAD: {
			s.in_table_header = true;
		} break;
		case MD_BLOCK_TBODY: {
			s.in_table_header = false;
		} break;
		case MD_BLOCK_TH:
		case MD_BLOCK_TD: {
			rtl->push_cell();
			const Color bg = (p_type == MD_BLOCK_TH) ? SOLERS_MD_TABLE_HEADER_BG : SOLERS_MD_TABLE_CELL_BG;
			rtl->set_cell_row_background_color(bg, bg);
			rtl->set_cell_border_color(SOLERS_MD_TABLE_BORDER);
			rtl->set_cell_padding(Rect2(8 * ed, 3 * ed, 8 * ed, 3 * ed));
			const MD_BLOCK_TD_DETAIL *td = static_cast<const MD_BLOCK_TD_DETAIL *>(p_detail);
			bool aligned = false;
			if (td && td->align == MD_ALIGN_CENTER) {
				rtl->push_paragraph(HORIZONTAL_ALIGNMENT_CENTER);
				aligned = true;
			} else if (td && td->align == MD_ALIGN_RIGHT) {
				rtl->push_paragraph(HORIZONTAL_ALIGNMENT_RIGHT);
				aligned = true;
			}
			s.cell_aligned.push_back(aligned);
			if (p_type == MD_BLOCK_TH) {
				rtl->push_bold();
			}
			s.frames.push_back(SolersMdFrame());
		} break;
		default:
			break;
	}
	return 0;
}

static int solers_md_leave_block(MD_BLOCKTYPE p_type, void *p_detail, void *p_userdata) {
	SolersMdState &s = *static_cast<SolersMdState *>(p_userdata);
	RichTextLabel *rtl = s.rtl;
	const float ed = EDSCALE;

	switch (p_type) {
		case MD_BLOCK_DOC: {
			s.frames.resize(s.frames.size() - 1);
		} break;
		case MD_BLOCK_QUOTE: {
			rtl->pop(); // color
			rtl->pop(); // indent
			s.indent_level--;
			s.frames.resize(s.frames.size() - 1);
		} break;
		case MD_BLOCK_UL:
		case MD_BLOCK_OL: {
			rtl->pop(); // list
			s.indent_level--;
			s.frames.resize(s.frames.size() - 1);
		} break;
		case MD_BLOCK_LI: {
			s.frames.resize(s.frames.size() - 1);
		} break;
		case MD_BLOCK_H: {
			rtl->pop(); // bold
			rtl->pop(); // font size
		} break;
		case MD_BLOCK_CODE: {
			// Indented or list-nested code: rendered as a single-cell table so
			// it still reads as a panel inside flowing prose. Top-level fenced
			// code never reaches here (the view extracts it into a
			// SolersCodeBlock before parsing).
			rtl->push_table(1, INLINE_ALIGNMENT_TOP);
			rtl->push_cell();
			rtl->set_cell_row_background_color(SOLERS_MD_CODE_PANEL_BG, SOLERS_MD_CODE_PANEL_BG);
			rtl->set_cell_border_color(SOLERS_MD_CODE_PANEL_BORDER);
			rtl->set_cell_padding(Rect2(9 * ed, 6 * ed, 9 * ed, 6 * ed));
			rtl->push_mono();
			rtl->push_font_size(int(s.base_font_size * 0.92f));
			rtl->push_color(SOLERS_SYN_DEFAULT);
			String code = s.code_buf;
			while (code.ends_with("\n")) {
				code = code.substr(0, code.length() - 1);
			}
			rtl->add_text(code);
			rtl->pop(); // color
			rtl->pop(); // font size
			rtl->pop(); // mono
			rtl->pop(); // cell
			rtl->pop(); // table
			s.in_code_block = false;
			s.code_buf = String();
		} break;
		case MD_BLOCK_TABLE: {
			rtl->pop(); // table
		} break;
		case MD_BLOCK_TH:
		case MD_BLOCK_TD: {
			if (p_type == MD_BLOCK_TH) {
				rtl->pop(); // bold
			}
			if (!s.cell_aligned.is_empty()) {
				if (s.cell_aligned[s.cell_aligned.size() - 1]) {
					rtl->pop(); // paragraph alignment
				}
				s.cell_aligned.resize(s.cell_aligned.size() - 1);
			}
			rtl->pop(); // cell
			s.frames.resize(s.frames.size() - 1);
		} break;
		default:
			break;
	}
	return 0;
}

static int solers_md_enter_span(MD_SPANTYPE p_type, void *p_detail, void *p_userdata) {
	SolersMdState &s = *static_cast<SolersMdState *>(p_userdata);
	RichTextLabel *rtl = s.rtl;

	if (s.img_depth > 0 && p_type != MD_SPAN_IMG) {
		return 0; // Inside image alt text: spans collapse to plain text.
	}

	switch (p_type) {
		case MD_SPAN_EM: {
			rtl->push_italics();
		} break;
		case MD_SPAN_STRONG: {
			rtl->push_bold();
		} break;
		case MD_SPAN_DEL: {
			rtl->push_strikethrough();
		} break;
		case MD_SPAN_CODE: {
			rtl->push_mono();
			rtl->push_color(SOLERS_MD_CODE_SPAN_TEXT);
		} break;
		case MD_SPAN_A: {
			const MD_SPAN_A_DETAIL *a = static_cast<const MD_SPAN_A_DETAIL *>(p_detail);
			const String href = a ? solers_md_attr(a->href) : String();
			rtl->push_meta(href, RichTextLabel::META_UNDERLINE_ALWAYS, href);
			rtl->push_color(SOLERS_MD_LINK);
		} break;
		case MD_SPAN_IMG: {
			if (s.img_depth == 0) {
				const MD_SPAN_IMG_DETAIL *img = static_cast<const MD_SPAN_IMG_DETAIL *>(p_detail);
				s.img_src = img ? solers_md_attr(img->src) : String();
				s.img_alt = String();
			}
			s.img_depth++;
		} break;
		default:
			break;
	}
	return 0;
}

static int solers_md_leave_span(MD_SPANTYPE p_type, void *p_detail, void *p_userdata) {
	SolersMdState &s = *static_cast<SolersMdState *>(p_userdata);
	RichTextLabel *rtl = s.rtl;

	if (s.img_depth > 0 && p_type != MD_SPAN_IMG) {
		return 0;
	}

	switch (p_type) {
		case MD_SPAN_EM:
		case MD_SPAN_STRONG:
		case MD_SPAN_DEL: {
			rtl->pop();
		} break;
		case MD_SPAN_CODE: {
			rtl->pop(); // color
			rtl->pop(); // mono
		} break;
		case MD_SPAN_A: {
			rtl->pop(); // color
			rtl->pop(); // meta
		} break;
		case MD_SPAN_IMG: {
			s.img_depth--;
			if (s.img_depth > 0) {
				break;
			}
			bool placed = false;
			if (s.img_src.begins_with("res://") && ResourceLoader::exists(s.img_src)) {
				Ref<Texture2D> texture = ResourceLoader::load(s.img_src);
				if (texture.is_valid()) {
					const int max_width = int(360 * EDSCALE);
					rtl->add_image(texture, MIN(texture->get_width(), max_width), 0);
					placed = true;
				}
			}
			if (!placed) {
				// Unresolvable image: degrade to a link so the target stays reachable.
				rtl->push_meta(s.img_src, RichTextLabel::META_UNDERLINE_ALWAYS, s.img_src);
				rtl->push_color(SOLERS_MD_LINK);
				rtl->add_text(s.img_alt.is_empty() ? s.img_src : s.img_alt);
				rtl->pop();
				rtl->pop();
			}
		} break;
		default:
			break;
	}
	return 0;
}

static int solers_md_text(MD_TEXTTYPE p_type, const MD_CHAR *p_text, MD_SIZE p_size, void *p_userdata) {
	SolersMdState &s = *static_cast<SolersMdState *>(p_userdata);

	if (s.in_code_block) {
		if (p_type == MD_TEXT_CODE) {
			s.code_buf += solers_md_str(p_text, p_size);
		}
		return 0;
	}
	if (s.img_depth > 0) {
		if (p_type == MD_TEXT_BR || p_type == MD_TEXT_SOFTBR) {
			s.img_alt += " ";
		} else {
			s.img_alt += solers_md_str(p_text, p_size);
		}
		return 0;
	}

	if (!s.frames.is_empty()) {
		s.frames[s.frames.size() - 1].text_seen = true;
	}

	switch (p_type) {
		case MD_TEXT_BR: {
			s.rtl->add_newline();
		} break;
		case MD_TEXT_SOFTBR: {
			s.rtl->add_text(" ");
		} break;
		case MD_TEXT_ENTITY: {
			s.rtl->add_text(solers_md_entity(solers_md_str(p_text, p_size)));
		} break;
		case MD_TEXT_NULLCHAR: {
			s.rtl->add_text(String::chr(0xFFFD));
		} break;
		default: {
			s.rtl->add_text(solers_md_str(p_text, p_size));
		} break;
	}
	return 0;
}

static bool solers_md_render(RichTextLabel *p_label, const String &p_source, int p_base_font_size) {
	SolersMdState state;
	state.rtl = p_label;
	state.base_font_size = p_base_font_size;

	MD_PARSER parser = {};
	parser.abi_version = 0;
	parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
			MD_FLAG_PERMISSIVEURLAUTOLINKS | MD_FLAG_PERMISSIVEWWWAUTOLINKS |
			MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS;
	parser.enter_block = solers_md_enter_block;
	parser.leave_block = solers_md_leave_block;
	parser.enter_span = solers_md_enter_span;
	parser.leave_span = solers_md_leave_span;
	parser.text = solers_md_text;

	const CharString utf8 = p_source.utf8();
	return md_parse(utf8.get_data(), MD_SIZE(utf8.length()), &parser, &state) == 0;
}

/* ------------------------------------------------------------------ */
/* SolersCodeBlock                                                     */
/* ------------------------------------------------------------------ */

static constexpr float SOLERS_CODE_HEADER_H = 26.0f;
static constexpr float SOLERS_CODE_PAD = 10.0f;
static constexpr int SOLERS_CODE_COPIED_FEEDBACK_MSEC = 1400;

SolersCodeBlock::SolersCodeBlock() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_mouse_filter(MOUSE_FILTER_PASS);

	body = memnew(RichTextLabel);
	body->set_use_bbcode(false);
	body->set_fit_content(true);
	body->set_scroll_active(false);
	body->set_selection_enabled(true);
	body->set_context_menu_enabled(true);
	body->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
	body->set_mouse_filter(MOUSE_FILTER_PASS);
	body->add_theme_style_override("normal", memnew(StyleBoxEmpty));
	body->add_theme_style_override("focus", memnew(StyleBoxEmpty));
	body->add_theme_color_override("default_color", SOLERS_SYN_DEFAULT);
	body->add_theme_constant_override("line_separation", int(3 * EDSCALE));
	add_child(body);

	copy_button = memnew(Button);
	copy_button->set_text(TTR("Copy"));
	copy_button->set_flat(true);
	copy_button->set_focus_mode(FOCUS_NONE);
	copy_button->add_theme_font_size_override("font_size", int(11 * EDSCALE));
	copy_button->add_theme_color_override("font_color", SOLERS_MD_TEXT_DIM);
	copy_button->connect(SceneStringName(pressed), callable_mp(this, &SolersCodeBlock::_copy_pressed));
	add_child(copy_button);
}

void SolersCodeBlock::set_code(const String &p_language, const String &p_code, bool p_caret) {
	language = p_language;
	String normalized = p_code;
	while (normalized.ends_with("\n")) {
		normalized = normalized.substr(0, normalized.length() - 1);
	}
	code = normalized;
	caret = p_caret;
	layout_width = -1.0f; // Force re-measure on next layout pass.
	queue_redraw();
}

void SolersCodeBlock::_render_body() {
	if (rendered_code == code && rendered_caret == caret) {
		return;
	}
	rendered_code = code;
	rendered_caret = caret;

	const float ed = EDSCALE;
	Ref<Font> mono = get_theme_font(SNAME("source"), SNAME("EditorFonts"));
	if (mono.is_valid()) {
		body->add_theme_font_override("normal_font", mono);
	}
	body->add_theme_font_size_override("normal_font_size", int(12 * ed));

	body->clear();
	solers_highlight_code(body, language, code);
	if (caret) {
		body->add_text(String::chr(SOLERS_MD_CARET));
	}
}

float SolersCodeBlock::measure(float p_width) {
	const float ed = EDSCALE;
	_render_body();

	const float pad = SOLERS_CODE_PAD * ed;
	const float header_h = SOLERS_CODE_HEADER_H * ed;
	const float body_width = MAX(p_width - pad * 2.0f, 40.0f * ed);

	body->set_position(Point2(pad, header_h + 4.0f * ed));
	body->set_size(Size2(body_width, 0));
	const float body_h = float(body->get_content_height());
	body->set_size(Size2(body_width, body_h));

	const Size2 button_size = copy_button->get_combined_minimum_size();
	copy_button->set_size(button_size);
	copy_button->set_position(Point2(p_width - button_size.x - 6.0f * ed, (header_h - button_size.y) * 0.5f));

	block_height = header_h + 4.0f * ed + body_h + pad;
	layout_width = p_width;
	update_minimum_size();
	return block_height;
}

Size2 SolersCodeBlock::get_minimum_size() const {
	return Size2(0, block_height);
}

void SolersCodeBlock::_copy_pressed() {
	DisplayServer::get_singleton()->clipboard_set(code);
	copy_button->set_text(TTR("Copied"));
	copied_until_msec = OS::get_singleton()->get_ticks_msec() + SOLERS_CODE_COPIED_FEEDBACK_MSEC;
	set_process_internal(true);
}

void SolersCodeBlock::_restore_copy_label() {
	copy_button->set_text(TTR("Copy"));
	set_process_internal(false);
}

void SolersCodeBlock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (OS::get_singleton()->get_ticks_msec() >= copied_until_msec) {
				_restore_copy_label();
			}
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			rendered_code = String(); // Fonts may have changed; re-render.
			layout_width = -1.0f;
			queue_redraw();
		} break;
		case NOTIFICATION_DRAW: {
			const float ed = EDSCALE;
			static Ref<StyleBoxFlat> panel;
			if (panel.is_null()) {
				panel.instantiate();
				panel->set_anti_aliased(true);
			}
			panel->set_bg_color(SOLERS_MD_CODE_PANEL_BG);
			panel->set_corner_radius_all(int(9 * ed));
			panel->set_border_width_all(MAX(1, int(ed)));
			panel->set_border_color(SOLERS_MD_CODE_PANEL_BORDER);
			draw_style_box(panel, Rect2(Point2(), get_size()));

			const float header_h = SOLERS_CODE_HEADER_H * ed;
			draw_rect(Rect2(1, header_h, get_size().x - 2, MAX(1.0f, ed)), Color(1, 1, 1, 0.045f));

			const Ref<Font> mono = get_theme_font(SNAME("source"), SNAME("EditorFonts"));
			if (mono.is_valid()) {
				const int font_size = int(11 * ed);
				const float baseline = (header_h - mono->get_height(font_size)) * 0.5f + mono->get_ascent(font_size);
				const String tag = language.is_empty() ? String("text") : language.to_lower();
				draw_string(mono, Point2(SOLERS_CODE_PAD * ed, baseline).floor(), tag, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, SOLERS_MD_TEXT_DIM);
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* SolersMarkdownView                                                  */
/* ------------------------------------------------------------------ */

SolersMarkdownView::SolersMarkdownView() {
	set_mouse_filter(MOUSE_FILTER_PASS);
}

Vector<SolersMarkdownView::Segment> SolersMarkdownView::_split_segments(const String &p_markdown) {
	Vector<Segment> out;
	String current;
	bool in_code = false;
	String code_lang;
	char32_t fence_char = 0;
	int fence_len = 0;

	auto flush_prose = [&]() {
		if (!current.strip_edges().is_empty()) {
			Segment seg;
			seg.text = current;
			out.push_back(seg);
		}
		current = String();
	};

	const String normalized = p_markdown.replace("\r\n", "\n").replace("\r", "\n");
	const PackedStringArray lines = normalized.split("\n", true);

	for (int li = 0; li < lines.size(); li++) {
		const String &line = lines[li];

		if (!in_code) {
			int lead = 0;
			while (lead < line.length() && line[lead] == ' ') {
				lead++;
			}
			if (lead <= 3 && lead < line.length() && (line[lead] == '`' || line[lead] == '~')) {
				const char32_t fc = line[lead];
				int n = 0;
				while (lead + n < line.length() && line[lead + n] == fc) {
					n++;
				}
				if (n >= 3) {
					flush_prose();
					in_code = true;
					fence_char = fc;
					fence_len = n;
					code_lang = line.substr(lead + n).strip_edges().get_slicec(' ', 0);
					continue;
				}
			}
			if (line.strip_edges().is_empty()) {
				flush_prose();
			} else {
				current += line;
				current += "\n";
			}
		} else {
			const String trimmed = line.strip_edges();
			bool closing = false;
			if (!trimmed.is_empty() && trimmed[0] == fence_char) {
				int n = 0;
				while (n < trimmed.length() && trimmed[n] == fence_char) {
					n++;
				}
				closing = n >= fence_len && trimmed.substr(n).strip_edges().is_empty();
			}
			if (closing) {
				Segment seg;
				seg.is_code = true;
				seg.lang = code_lang;
				seg.text = current;
				out.push_back(seg);
				current = String();
				in_code = false;
				code_lang = String();
			} else {
				current += line;
				current += "\n";
			}
		}
	}

	if (in_code) {
		// Unterminated fence: the stream is still inside the code block.
		Segment seg;
		seg.is_code = true;
		seg.lang = code_lang;
		seg.text = current;
		out.push_back(seg);
	} else {
		flush_prose();
	}
	return out;
}

// Closes unbalanced inline markers in the trailing open segment so partially
// streamed `code` / **bold** never flashes as raw asterisks and backticks.
static String solers_heal_open_markdown(const String &p_text) {
	String healed = p_text;

	int bold_marks = 0;
	int search = 0;
	while ((search = healed.find("**", search)) != -1) {
		bold_marks++;
		search += 2;
	}

	int ticks = 0;
	for (int i = 0; i < healed.length(); i++) {
		if (healed[i] == '`') {
			ticks++;
		}
	}

	if (ticks % 2 == 1) {
		healed += "`";
	}
	if (bold_marks % 2 == 1) {
		healed += "**";
	}
	return healed;
}

RichTextLabel *SolersMarkdownView::_make_paragraph_label() {
	const float ed = EDSCALE;
	RichTextLabel *rtl = memnew(RichTextLabel);
	rtl->set_use_bbcode(false);
	rtl->set_fit_content(true);
	rtl->set_scroll_active(false);
	rtl->set_selection_enabled(true);
	rtl->set_context_menu_enabled(true);
	rtl->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	rtl->set_mouse_filter(MOUSE_FILTER_PASS);
	// Flush on the chat canvas: drop the editor RichTextLabel's default panel
	// background so assistant prose reads like Cursor's, not a boxed card.
	rtl->add_theme_style_override("normal", memnew(StyleBoxEmpty));
	rtl->add_theme_style_override("focus", memnew(StyleBoxEmpty));
	rtl->add_theme_color_override("default_color", SOLERS_MD_TEXT_BODY);
	rtl->add_theme_constant_override("line_separation", int(4 * ed));
	rtl->add_theme_constant_override("paragraph_separation", int(3 * ed));
	rtl->add_theme_font_size_override("normal_font_size", int(14 * ed));
	rtl->add_theme_font_size_override("bold_font_size", int(14 * ed));
	rtl->add_theme_font_size_override("italics_font_size", int(14 * ed));
	rtl->add_theme_font_size_override("bold_italics_font_size", int(14 * ed));
	rtl->add_theme_font_size_override("mono_font_size", int(13 * ed));
	const Ref<Font> mono = get_theme_font(SNAME("source"), SNAME("EditorFonts"));
	if (mono.is_valid()) {
		rtl->add_theme_font_override("mono_font", mono);
	}
	rtl->connect(SNAME("meta_clicked"), callable_mp(this, &SolersMarkdownView::_on_meta_clicked));
	add_child(rtl);
	return rtl;
}

void SolersMarkdownView::_render_segment(int p_index, const Segment &p_segment, bool p_open) {
	while (int(blocks.size()) <= p_index) {
		blocks.push_back(Block());
	}

	Block &block = blocks[p_index];
	const bool unchanged = block.is_code == p_segment.is_code && block.source == p_segment.text &&
			block.lang == p_segment.lang && block.rendered_caret == p_open;
	if (unchanged) {
		return;
	}
	if (block.is_code != p_segment.is_code && block.control) {
		memdelete(block.control);
		block.control = nullptr;
	}

	block.is_code = p_segment.is_code;
	block.lang = p_segment.lang;
	block.source = p_segment.text;
	block.rendered_caret = p_open;

	if (p_segment.is_code) {
		SolersCodeBlock *code_block = Object::cast_to<SolersCodeBlock>(block.control);
		if (!code_block) {
			code_block = memnew(SolersCodeBlock);
			add_child(code_block);
			block.control = code_block;
		}
		code_block->set_code(p_segment.lang, p_segment.text, p_open);
	} else {
		RichTextLabel *rtl = Object::cast_to<RichTextLabel>(block.control);
		if (!rtl) {
			rtl = _make_paragraph_label();
			block.control = rtl;
		}
		_render_paragraph(rtl, p_segment.text, p_open);
	}
}

void SolersMarkdownView::_render_paragraph(RichTextLabel *p_label, const String &p_source, bool p_caret) {
	String source = p_source;
	if (p_caret) {
		source = solers_heal_open_markdown(source);
		source += String::chr(SOLERS_MD_CARET);
	}
	p_label->clear();
	if (!solers_md_render(p_label, source, int(14 * EDSCALE))) {
		p_label->add_text(source); // Parser failure: degrade to plain text.
	}
}

void SolersMarkdownView::set_markdown(const String &p_markdown, bool p_streaming) {
	// S2 early-out: identical text + streaming flag + already laid out at the
	// current width means there is nothing new to split or render.
	if (rendered_md_valid && p_streaming == rendered_streaming && p_markdown == rendered_md &&
			Math::is_equal_approx(get_size().x, layout_width)) {
		return;
	}
	rendered_md = p_markdown;
	rendered_streaming = p_streaming;
	rendered_md_valid = true;

	const Vector<Segment> segments = _split_segments(p_markdown);

	for (int i = 0; i < segments.size(); i++) {
		_render_segment(i, segments[i], p_streaming && i == segments.size() - 1);
	}

	while (int(blocks.size()) > segments.size()) {
		Block &block = blocks[blocks.size() - 1];
		if (block.control) {
			memdelete(block.control);
		}
		blocks.resize(blocks.size() - 1);
	}

	_relayout();
}

void SolersMarkdownView::append_markdown_delta(const String &p_delta, bool p_streaming) {
	if (p_delta.is_empty()) {
		if (!rendered_md_valid || rendered_streaming == p_streaming) {
			return;
		}
		rendered_streaming = p_streaming;
		if (!blocks.is_empty()) {
			const int last_index = int(blocks.size()) - 1;
			const Block &last = blocks[last_index];
			Segment segment;
			segment.is_code = last.is_code;
			segment.lang = last.lang;
			segment.text = last.source;
			_render_segment(last_index, segment, p_streaming);
			_relayout();
		}
		return;
	}
	const String markdown = rendered_md + p_delta;
	const bool starts_new_block = (rendered_md.ends_with("\n") && p_delta.begins_with("\n")) || p_delta.find("\n\n") >= 0;
	const bool touches_fence = p_delta.find("```") >= 0 || p_delta.find("~~~") >= 0;
	if (!rendered_md_valid || !rendered_streaming || !p_streaming || blocks.is_empty() ||
			!Math::is_equal_approx(get_size().x, layout_width) || starts_new_block || touches_fence || blocks[blocks.size() - 1].is_code) {
		set_markdown(markdown, p_streaming);
		return;
	}

	Segment segment;
	Block &last = blocks[blocks.size() - 1];
	segment.is_code = last.is_code;
	segment.lang = last.lang;
	segment.text = last.source + p_delta;
	rendered_md = markdown;
	rendered_streaming = p_streaming;
	rendered_md_valid = true;
	_render_segment(int(blocks.size()) - 1, segment, p_streaming);
	_relayout();
}

void SolersMarkdownView::_relayout() {
	const float ed = EDSCALE;
	const float width = MAX(get_size().x, 60.0f * ed);
	const float gap = 8.0f * ed;

	float y = 0.0f;
	for (uint32_t i = 0; i < blocks.size(); i++) {
		Control *control = blocks[i].control;
		if (!control) {
			continue;
		}
		if (y > 0.0f) {
			y += gap;
		}
		control->set_position(Point2(0, y));

		float height = 0.0f;
		if (blocks[i].is_code) {
			height = static_cast<SolersCodeBlock *>(control)->measure(width);
		} else {
			RichTextLabel *rtl = static_cast<RichTextLabel *>(control);
			rtl->set_size(Size2(width, 0));
			height = MAX(float(rtl->get_content_height()), 10.0f * ed);
			rtl->set_size(Size2(width, height));
		}
		control->set_size(Size2(width, height));
		y += height;
	}

	layout_width = width;
	if (!Math::is_equal_approx(content_height, y)) {
		content_height = y;
		update_minimum_size();
	}
}

Size2 SolersMarkdownView::get_minimum_size() const {
	return Size2(0, content_height);
}

void SolersMarkdownView::_on_meta_clicked(const Variant &p_meta) {
	const String target = p_meta;

	if (target.begins_with("http://") || target.begins_with("https://")) {
		OS::get_singleton()->shell_open(target);
		return;
	}
	if (!target.begins_with("res://")) {
		return;
	}

	// Accept res://path/script.gd:42 to jump straight to a line.
	String path = target;
	int line = -1;
	const int colon = path.rfind(":");
	if (colon > 5) {
		const String tail = path.substr(colon + 1);
		if (tail.is_valid_int()) {
			line = tail.to_int();
			path = path.substr(0, colon);
		}
	}

	if (!ResourceLoader::exists(path)) {
		return;
	}
	if (path.ends_with(".tscn") || path.ends_with(".scn")) {
		EditorInterface::get_singleton()->open_scene_from_path(path);
		return;
	}
	Ref<Resource> resource = ResourceLoader::load(path);
	if (resource.is_null()) {
		return;
	}
	Ref<Script> script = resource;
	if (script.is_valid()) {
		EditorInterface::get_singleton()->edit_script(script, line);
		return;
	}
	EditorInterface::get_singleton()->edit_resource(resource);
}

void SolersMarkdownView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_RESIZED: {
			if (!Math::is_equal_approx(get_size().x, layout_width)) {
				_relayout();
			}
		} break;
	}
}
