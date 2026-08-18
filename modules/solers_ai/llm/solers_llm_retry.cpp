/**************************************************************************/
/*  solers_llm_retry.cpp                                                  */
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

#include "solers_llm_retry.h"

#include "core/string/ustring.h"
#include "core/typedefs.h"
#include "core/variant/array.h"
#include "core/variant/variant.h"

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
	const Variant retryable = p_error.get("retryable", Variant());
	if (retryable.get_type() == Variant::BOOL) {
		return (bool)retryable;
	}
	const int http_status = (int)p_error.get("http_status", 0);
	return http_status == 429 || http_status >= 500;
}

uint64_t SolersLLMRetry::delay_msec(int p_attempt, const Dictionary &p_error) {
	const int attempt = MAX(1, p_attempt);

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
