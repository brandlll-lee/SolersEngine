/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "register_types.h"

#ifdef TOOLS_ENABLED
#include "core/io/resource_importer.h"
#include "editor/editor_node.h"
#include "modules/solers_modeling/core/solers_model_source.h"
#include "modules/solers_modeling/editor/resource_importer_smodel.h"

static SolersModelingService *modeling_service = nullptr;

static void _register_solers_model_importer() {
	Ref<ResourceImporterSolersModel> importer;
	importer.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(importer);
}
#endif

void initialize_solers_modeling_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(SolersModelingService);
		GDREGISTER_CLASS(ResourceImporterSolersModel);
		modeling_service = memnew(SolersModelingService);
		EditorNode::add_init_callback(_register_solers_model_importer);
	}
#endif
}

void uninitialize_solers_modeling_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR && modeling_service) {
		memdelete(modeling_service);
		modeling_service = nullptr;
	}
#endif
}

