/**************************************************************************/
/*  solers_protocol_openai_responses.h                                    */
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

#include "modules/solers_ai/llm/solers_llm_protocol.h"

// OpenAI Responses wire protocol used by ChatGPT Codex. Provider identity,
// OAuth, and endpoints remain profile data; this class only lowers canonical
// messages and lifts SSE frames.
class SolersOpenAIResponsesProtocol : public SolersLLMProtocol {
	Array _lower_input(const Dictionary &p_request) const;
	Array _lower_tools(const Array &p_tools) const;

public:
	virtual StringName get_id() const override { return StringName("openai-responses"); }
	virtual String get_default_path() const override { return "/responses"; }
	virtual Dictionary build_request_body(const Dictionary &p_request) const override;
	virtual Array parse_event(Dictionary &r_state, const String &p_event_name, const String &p_data) const override;
};
