/**************************************************************************/
/*  solers_plugin_ambientcg.h                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "solers_plugin.h"

class SolersPluginAmbientCG : public SolersPlugin {
	GDCLASS(SolersPluginAmbientCG, SolersPlugin);

	void _catalog_fetch_page(uint32_t p_index, void *p_batch);
	static Dictionary _asset_metadata(const String &p_asset_id, const SafeFlag *p_cancel_requested);
	static bool _extract_archive(const Ref<SolersPluginJob> &p_job, const PackedByteArray &p_archive, Array &r_files, String &r_error);

public:
	virtual Dictionary get_profile() const override;
	virtual Dictionary catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) override;
	virtual Dictionary catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) override;
	virtual void run_job(const Ref<SolersPluginJob> &p_job) override;
};
