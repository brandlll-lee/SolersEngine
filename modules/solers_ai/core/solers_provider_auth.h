/**************************************************************************/
/*  solers_provider_auth.h                                                */
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

#include "core/string/string_name.h"
#include "core/variant/dictionary.h"

// Provider methods own authentication and request-specific protocol facts.
class SolersProviderAuth {
public:
	virtual StringName get_method_id() const = 0;
	virtual Dictionary start(const Dictionary &p_inputs = Dictionary()) = 0;
	virtual void poll() = 0;
	virtual void cancel() = 0;
	virtual Dictionary get_status() const = 0;
	virtual Dictionary take_credential() = 0;
	virtual bool is_active() const = 0;

	// Worker-thread entry point. Returns { ok, credential, headers, profile }
	// or { ok:false, error }. Implementations may refresh and rotate credentials.
	virtual Dictionary prepare_request(const Dictionary &p_credential, const Dictionary &p_profile, const Dictionary &p_request, bool p_force_refresh) const = 0;

	virtual ~SolersProviderAuth() = default;
};
