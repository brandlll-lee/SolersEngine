/**************************************************************************/
/*  solers_llm_retry.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_llm_retry.h"

#include "core/string/ustring.h"
#include "core/typedefs.h"
#include "core/variant/array.h"
#include "core/variant/variant.h"

// Ported verbatim from opencode retry.ts:
//   RETRY_INITIAL_DELAY = 2000, RETRY_BACKOFF_FACTOR = 2,
//   RETRY_MAX_DELAY_NO_HEADERS = 30_000.
static constexpr uint64_t SOLERS_RETRY_INITIAL_DELAY_MSEC = 2000;
static constexpr uint64_t SOLERS_RETRY_BACKOFF_FACTOR = 2;
static constexpr uint64_t SOLERS_RETRY_MAX_DELAY_NO_HEADERS_MSEC = 30000;
// When the provider hands us an explicit Retry-After, honor it but cap the wait
// so a pathological "retry-after: 3600" cannot park the turn for an hour.
static constexpr uint64_t SOLERS_RETRY_MAX_DELAY_WITH_HEADERS_MSEC = 60000;

// Case-insensitive header lookup (servers vary the casing of Retry-After).
static String solers_find_header(const Dictionary &p_headers, const String &p_lower_name) {
	const Array keys = p_headers.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String key = keys[i];
		if (key.to_lower() == p_lower_name) {
			return String(p_headers[keys[i]]);
		}
	}
	return String();
}

bool SolersLLMRetry::is_retryable(const Dictionary &p_error) {
	const String code = String(p_error.get("code", String()));
	const int http_status = (int)p_error.get("http_status", 0);
	const String message = String(p_error.get("message", String())).to_lower();

	// Never retry: the input itself is rejected, so re-sending it cannot help.
	// Mirrors opencode retry.ts (ContextOverflowError not retried) and error.ts
	// (insufficient_quota / invalid_prompt are isRetryable: false).
	if (message.find("context_length_exceeded") >= 0 || message.find("context window") >= 0 ||
			message.find("insufficient_quota") >= 0 || message.find("invalid_api_key") >= 0 ||
			message.find("invalid_prompt") >= 0) {
		return false;
	}
	// 4xx caller errors (auth, bad request, payload too large, unprocessable)
	// are not transient. 429 is handled as retryable below.
	if (http_status == 400 || http_status == 401 || http_status == 403 ||
			http_status == 404 || http_status == 413 || http_status == 422) {
		return false;
	}

	// 5xx are transient server failures — always retry (opencode retry.ts:
	// "5xx errors ... should always be retried").
	if (http_status >= 500) {
		return true;
	}
	if (http_status == 429) {
		return true;
	}

	// Transport-level failures from SolersLLMClient: the connection, not the
	// request, is at fault, so the identical request is worth re-sending.
	if (code == "HEADER_TIMEOUT" || code == "STREAM_STALL" || code == "PROVIDER_TIMEOUT" ||
			code == "CONNECT_FAILED" || code == "CONNECTION_ERROR" || code == "CANT_CONNECT" ||
			code == "TLS_ERROR") {
		return true;
	}

	// Plain-text rate-limit / overload patterns (opencode retry.ts text match).
	if (message.find("rate limit") >= 0 || message.find("rate_limit") >= 0 ||
			message.find("too many requests") >= 0 || message.find("overloaded") >= 0 ||
			message.find("exhausted") >= 0 || message.find("unavailable") >= 0 ||
			message.find("rate increased too quickly") >= 0) {
		return true;
	}

	// Config errors (CANT_RESOLVE, NO_*), 4xx not listed above, and anything
	// unrecognized are treated as terminal.
	return false;
}

uint64_t SolersLLMRetry::delay_msec(int p_attempt, const Dictionary &p_error) {
	const int attempt = MAX(1, p_attempt);

	// Header-driven wait wins (opencode delay(): retry-after-ms then retry-after).
	const Dictionary headers = p_error.get("headers", Dictionary());
	if (!headers.is_empty()) {
		const String retry_after_ms = solers_find_header(headers, "retry-after-ms");
		if (retry_after_ms.is_valid_float()) {
			const double ms = retry_after_ms.to_float();
			if (ms >= 0.0) {
				return MIN((uint64_t)ms, SOLERS_RETRY_MAX_DELAY_WITH_HEADERS_MSEC);
			}
		}
		const String retry_after = solers_find_header(headers, "retry-after");
		if (retry_after.is_valid_float()) {
			const double seconds = retry_after.to_float();
			if (seconds >= 0.0) {
				return MIN((uint64_t)(seconds * 1000.0), SOLERS_RETRY_MAX_DELAY_WITH_HEADERS_MSEC);
			}
		}
	}

	// Exponential backoff: 2000 * 2^(attempt-1), capped at 30s.
	uint64_t d = SOLERS_RETRY_INITIAL_DELAY_MSEC;
	for (int i = 1; i < attempt; i++) {
		d *= SOLERS_RETRY_BACKOFF_FACTOR;
		if (d >= SOLERS_RETRY_MAX_DELAY_NO_HEADERS_MSEC) {
			break;
		}
	}
	return MIN(d, SOLERS_RETRY_MAX_DELAY_NO_HEADERS_MSEC);
}
