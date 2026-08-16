/**************************************************************************/
/*  solers_llm_message.cpp                                                */
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

#include "solers_llm_message.h"

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

String SolersLLMMessage::attachment_identity(const Dictionary &p_attachment) {
	const String sha = String(p_attachment.get("content_sha256", String())).strip_edges();
	if (!sha.is_empty()) {
		return "sha256:" + sha;
	}
	const String id = String(p_attachment.get("id", String())).strip_edges();
	if (!id.is_empty()) {
		return "id:" + id;
	}
	const String path = String(p_attachment.get("local_path", String())).strip_edges();
	if (!path.is_empty()) {
		return "path:" + path;
	}
	return String();
}

static String _solers_attachment_reference(const Dictionary &p_attachment) {
	const String id = String(p_attachment.get("id", String())).strip_edges();
	const String sha = String(p_attachment.get("content_sha256", String())).strip_edges();
	String identity;
	if (!id.is_empty()) {
		identity += " id=" + id;
	}
	if (!sha.is_empty()) {
		identity += " sha256=" + sha;
	}
	return vformat("[Image evidence%s is represented by its native receipt; request a fresh capture if pixels are needed.]", identity);
}

static String _solers_capture_view_key(const Dictionary &p_message, const Dictionary &p_attachment) {
	if (String(p_attachment.get("source", String())) != "tool_capture") {
		return String();
	}
	const Variant parsed = JSON::parse_string(p_message.get("content", String()));
	if (parsed.get_type() != Variant::DICTIONARY) {
		return String();
	}
	const Dictionary data = Dictionary(parsed).get("data", Dictionary());
	return String(Dictionary(data.get("render_receipt", Dictionary())).get("view_key", String()));
}

Array SolersLLMMessage::project_attachments(const Array &p_messages) {
	HashSet<String> current_views;
	HashSet<String> superseded;
	for (int i = p_messages.size() - 1; i >= 0; i--) {
		const Dictionary message = p_messages[i];
		const Array attachments = message.get("attachments", Array());
		for (int a = 0; a < attachments.size(); a++) {
			const Dictionary attachment = attachments[a];
			const String view_key = _solers_capture_view_key(message, attachment);
			if (view_key.is_empty()) {
				continue;
			}
			const String identity = attachment_identity(attachment);
			if (current_views.has(view_key)) {
				if (!identity.is_empty()) {
					superseded.insert(identity);
				}
			} else {
				current_views.insert(view_key);
			}
		}
	}
	HashSet<String> seen;
	Array projected;
	for (int i = 0; i < p_messages.size(); i++) {
		Dictionary message = Dictionary(p_messages[i]).duplicate(true);
		const Array attachments = message.get("attachments", Array());
		if (attachments.is_empty()) {
			projected.push_back(message);
			continue;
		}
		Array wire_attachments;
		String content = message.get("content", String());
		for (int a = 0; a < attachments.size(); a++) {
			const Dictionary attachment = attachments[a];
			const String key = attachment_identity(attachment);
			const bool reference_only = !key.is_empty() && (seen.has(key) || superseded.has(key));
			if (reference_only) {
				if (!content.is_empty()) {
					content += "\n";
				}
				content += _solers_attachment_reference(attachment);
				continue;
			}
			wire_attachments.push_back(attachment);
			if (!key.is_empty()) {
				seen.insert(key);
			}
		}
		message["content"] = content;
		if (wire_attachments.is_empty()) {
			message.erase("attachments");
		} else {
			message["attachments"] = wire_attachments;
		}
		projected.push_back(message);
	}
	return projected;
}

Dictionary SolersLLMMessage::encode_image_attachment(const Dictionary &p_attachment) {
	const String path = String(p_attachment.get("local_path", String())).strip_edges();
	const String mime_type = String(p_attachment.get("mime_type", String())).strip_edges();
	if (path.is_empty() || !mime_type.begins_with("image/")) {
		return Dictionary();
	}

	// Attachments are immutable evidence, so each file is encoded once per
	// process. A failed load is cached too: the protocols already hand the
	// model an explicit missing-image marker, so re-reading the same dead
	// path on every request would only repeat the log noise.
	static HashMap<String, Dictionary> encoded_cache;
	if (const Dictionary *cached = encoded_cache.getptr(path)) {
		return *cached;
	}

	Dictionary encoded;
	Error error = OK;
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(path, &error);
	if (error != OK || bytes.is_empty()) {
		// Evidence must never vanish silently: captures are immutable, so a
		// missing file indicates a real bug worth surfacing.
		ERR_PRINT(vformat("Solers: image attachment '%s' is missing on disk: %s", String(p_attachment.get("id", String())), path));
	} else {
		const String data = CryptoCore::b64_encode_str(bytes.ptr(), bytes.size());
		encoded["mime_type"] = mime_type;
		encoded["base64"] = data;
		encoded["data_uri"] = vformat("data:%s;base64,%s", mime_type, data);
	}
	encoded_cache.insert(path, encoded);
	return encoded;
}

const char *SolersLLMRole::SYSTEM = "system";
const char *SolersLLMRole::USER = "user";
const char *SolersLLMRole::ASSISTANT = "assistant";
const char *SolersLLMRole::TOOL = "tool";

const char *SolersLLMEventKind::TEXT_DELTA = "text_delta";
const char *SolersLLMEventKind::REASONING_DELTA = "reasoning_delta";
const char *SolersLLMEventKind::TOOL_INPUT_START = "tool_input_start";
const char *SolersLLMEventKind::TOOL_INPUT_DELTA = "tool_input_delta";
const char *SolersLLMEventKind::TOOL_CALL = "tool_call";
const char *SolersLLMEventKind::USAGE = "usage";
const char *SolersLLMEventKind::FINISH = "finish";
const char *SolersLLMEventKind::ERROR = "error";

const char *SolersLLMStopReason::END_TURN = "end_turn";
const char *SolersLLMStopReason::TOOL_USE = "tool_use";
const char *SolersLLMStopReason::MAX_TOKENS = "max_tokens";
const char *SolersLLMStopReason::STOP = "stop";
