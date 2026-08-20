/**************************************************************************/
/*  solers_plugin_ambientcg.h                                             */
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

class SolersPluginAmbientCG : public SolersPlugin {
	GDCLASS(SolersPluginAmbientCG, SolersPlugin);

	static Dictionary _asset_metadata(const String &p_asset_id, const SafeFlag *p_cancel_requested);
	static bool _extract_archive(const Ref<SolersPluginJob> &p_job, const PackedByteArray &p_archive, Array &r_files, String &r_error);

public:
	virtual Dictionary get_profile() const override;
	virtual Dictionary catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) override;
	virtual Dictionary catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) override;
	virtual void run_job(const Ref<SolersPluginJob> &p_job) override;
};
