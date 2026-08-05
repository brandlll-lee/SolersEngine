/**************************************************************************/
/*  solers_plugin_polyhaven.h                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "solers_plugin.h"

class SolersPluginPolyHaven : public SolersPlugin {
	GDCLASS(SolersPluginPolyHaven, SolersPlugin);

	static Dictionary _asset_metadata(const String &p_asset_id, const SafeFlag *p_cancel_requested);
	static bool _download_file(const Ref<SolersPluginJob> &p_job, const String &p_label, const Dictionary &p_spec, Array &r_files, Dictionary &r_checksums, String &r_error, const String &p_relative_path = String());
	static bool _download_variant(const Ref<SolersPluginJob> &p_job, const Dictionary &p_files, const String &p_kind, const String &p_variant, Array &r_files, Dictionary &r_maps, Dictionary &r_checksums, String &r_error);

public:
	static Vector<String> request_headers();
	static Array normalize_variants(const Dictionary &p_files, const String &p_kind);

	virtual Dictionary get_profile() const override;
	virtual Dictionary catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) override;
	virtual Dictionary catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) override;
	virtual void run_job(const Ref<SolersPluginJob> &p_job) override;
};
