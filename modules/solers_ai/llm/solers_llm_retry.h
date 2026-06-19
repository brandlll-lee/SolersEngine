/**************************************************************************/
/*  solers_llm_retry.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Transient-failure classification + exponential backoff for LLM         */
/* requests. Ported faithfully from opencode                              */
/* (_research/opencode/packages/opencode/src/session/retry.ts: retryable, */
/* delay; and provider/error.ts: context-overflow / quota / 5xx rules).   */
/*                                                                        */
/* Pure functions over the canonical error Dictionary produced by         */
/* SolersLLMClient ({ code, message, http_status?, headers? }). No engine  */
/* singletons, no hidden state: the same error always classifies the same  */
/* way, and a brand-new error code/status is judged by structured facts    */
/* (HTTP class, transport code, provider error text), never a name list.   */
/**************************************************************************/

#pragma once

#include "core/variant/dictionary.h"

class SolersLLMRetry {
public:
	// True when the failure is transient and the identical request is worth
	// re-sending (5xx, 429, transport stalls, rate-limit/overload text). False
	// for caller errors that re-sending cannot fix (auth, bad request, context
	// overflow, quota). Mirrors opencode retry.ts `retryable()`.
	static bool is_retryable(const Dictionary &p_error);

	// Backoff in milliseconds for a 1-based attempt. Honors Retry-After /
	// retry-after-ms response headers when present, else exponential
	// (2000 * 2^(attempt-1)) capped at 30s. Mirrors opencode retry.ts `delay()`.
	static uint64_t delay_msec(int p_attempt, const Dictionary &p_error);
};
