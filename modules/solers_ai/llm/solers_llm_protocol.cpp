/**************************************************************************/
/*  solers_llm_protocol.cpp                                               */
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

#include "solers_llm_protocol.h"

#include "solers_protocol_anthropic_messages.h"
#include "solers_protocol_openai_chat.h"

void SolersLLMProtocolRegistry::register_protocol(SolersLLMProtocol *p_protocol) {
	ERR_FAIL_NULL(p_protocol);
	const StringName id = p_protocol->get_id();
	if (protocols.has(id)) {
		memdelete(protocols[id]);
	}
	protocols[id] = p_protocol;
}

SolersLLMProtocol *SolersLLMProtocolRegistry::get(const StringName &p_id) const {
	HashMap<StringName, SolersLLMProtocol *>::ConstIterator it = protocols.find(p_id);
	return it ? it->value : nullptr;
}

bool SolersLLMProtocolRegistry::has(const StringName &p_id) const {
	return protocols.has(p_id);
}

LocalVector<StringName> SolersLLMProtocolRegistry::list_ids() const {
	LocalVector<StringName> ids;
	for (const KeyValue<StringName, SolersLLMProtocol *> &kv : protocols) {
		ids.push_back(kv.key);
	}
	return ids;
}

void SolersLLMProtocolRegistry::register_builtin_protocols() {
	register_protocol(memnew(SolersOpenAIChatProtocol));
	register_protocol(memnew(SolersAnthropicMessagesProtocol));
}

SolersLLMProtocolRegistry::~SolersLLMProtocolRegistry() {
	for (const KeyValue<StringName, SolersLLMProtocol *> &kv : protocols) {
		memdelete(kv.value);
	}
	protocols.clear();
}
