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

Dictionary SolersLLMMessage::encode_image_attachment(const Dictionary &p_attachment) {
	const String path = String(p_attachment.get("local_path", String())).strip_edges();
	const String mime_type = String(p_attachment.get("mime_type", String())).strip_edges();
	if (path.is_empty() || !mime_type.begins_with("image/")) {
		return Dictionary();
	}

	Error error = OK;
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(path, &error);
	if (error != OK || bytes.is_empty()) {
		// Evidence must never vanish silently: captures are immutable, so a
		// missing file indicates a real bug worth surfacing.
		ERR_PRINT(vformat("Solers: image attachment '%s' is missing on disk: %s", String(p_attachment.get("id", String())), path));
		return Dictionary();
	}

	const String data = CryptoCore::b64_encode_str(bytes.ptr(), bytes.size());
	Dictionary encoded;
	encoded["mime_type"] = mime_type;
	encoded["base64"] = data;
	encoded["data_uri"] = vformat("data:%s;base64,%s", mime_type, data);
	return encoded;
}
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
