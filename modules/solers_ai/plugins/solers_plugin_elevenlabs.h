/**************************************************************************/
/*  solers_plugin_elevenlabs.h                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "solers_plugin.h"

class SolersPluginElevenLabs : public SolersPlugin {
	GDCLASS(SolersPluginElevenLabs, SolersPlugin);

public:
	virtual Dictionary get_profile() const override;
	virtual void run_job(const Ref<SolersPluginJob> &p_job) override;
};
