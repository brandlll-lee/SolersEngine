/**************************************************************************/
/*  test_solers_assets.cpp                                                */
/**************************************************************************/

#include "tests/test_macros.h"

#include "modules/solers_ai/core/solers_asset_service.h"

TEST_FORCE_LINK(test_solers_assets)

namespace TestSolersAssets {

TEST_CASE("[SolersAssetService] import terminal states are explicit") {
	CHECK(SolersAssetService::is_project_import_terminal_status("imported"));
	CHECK(SolersAssetService::is_project_import_terminal_status("failed"));
	CHECK(SolersAssetService::is_project_import_terminal_status("cancelled"));
	CHECK_FALSE(SolersAssetService::is_project_import_terminal_status("queued"));
	CHECK_FALSE(SolersAssetService::is_project_import_terminal_status("running"));
}

TEST_CASE("[SolersAssetService] standalone service exposes no agent registry state") {
	SolersAssetService assets;
	CHECK_FALSE(assets.has_active_tasks("test-session"));
	CHECK(SolersAssetService::asset_root().is_absolute_path());
}

} // namespace TestSolersAssets
