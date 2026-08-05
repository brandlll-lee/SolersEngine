/**************************************************************************/
/*  solers_protocol_openai_responses.h                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
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
	virtual void augment_headers(Dictionary &r_headers, const Dictionary &p_request) const override;
	virtual Array parse_event(Dictionary &r_state, const String &p_event_name, const String &p_data) const override;
};
