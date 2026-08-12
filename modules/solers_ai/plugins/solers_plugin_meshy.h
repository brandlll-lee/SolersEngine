/**************************************************************************/
/*  solers_plugin_meshy.h                                                 */
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

#include "solers_plugin.h"

class SolersPluginMeshy : public SolersPlugin {
	GDCLASS(SolersPluginMeshy, SolersPlugin);

	static Dictionary _poll_task(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers);
	static Dictionary _submit_and_poll(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const String &p_url, const Vector<String> &p_headers, const Dictionary &p_body, const String &p_stage);
	static bool _download_model(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const Dictionary &p_detail);
	static void _download_basic_animations(const Ref<SolersPluginJob> &p_job, Dictionary &r_state, const Dictionary &p_detail);

public:
	static Array animation_actions();
	static bool animation_action_exists(int64_t p_action_id);

	virtual Dictionary get_profile() const override;
	virtual Dictionary get_generation_options_schema(const String &p_kind) const override;
	virtual Array get_operation_defs() const override;
	virtual Dictionary prepare_generate(const String &p_kind, const Dictionary &p_args, Dictionary &r_manifest) const override;
	virtual Dictionary prepare_operation(const Dictionary &p_operation, const Dictionary &p_source_manifest, Dictionary &r_provider_options) const override;
	virtual Dictionary capability_extras(const Dictionary &p_manifest) const override;
	virtual void run_job(const Ref<SolersPluginJob> &p_job) override;
};
