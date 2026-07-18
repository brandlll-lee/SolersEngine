/**************************************************************************/
/*  solers_llm_message.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_llm_message.h"

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/templates/hash_map.h"

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
