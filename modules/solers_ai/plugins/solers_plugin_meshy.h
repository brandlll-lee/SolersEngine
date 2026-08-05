/**************************************************************************/
/*  solers_plugin_meshy.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
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
