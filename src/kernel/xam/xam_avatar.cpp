/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {

ppc_u32_result_t XamAvatarInitialize_entry(ppc_u32_t unk1,              // 1, 4, etc
                                           ppc_u32_t unk2,              // 0 or 1
                                           ppc_u32_t processor_number,  // for thread creation?
                                           ppc_pu32_t function_ptrs,    // 20b, 5 pointers
                                           ppc_pvoid_t unk5,            // ptr in data segment
                                           ppc_u32_t unk6  // flags - 0x00300000, 0x30, etc
) {
  // Negative to fail. Game should immediately call XamAvatarShutdown.
  return ~0u;
}

void XamAvatarShutdown_entry() {
  // No-op.
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__XamAvatarInitialize, rex::kernel::xam::XamAvatarInitialize_entry)
XAM_EXPORT(__imp__XamAvatarShutdown, rex::kernel::xam::XamAvatarShutdown_entry)

XAM_EXPORT_STUB(__imp__XamAvatarBeginEnumAssets);
XAM_EXPORT_STUB(__imp__XamAvatarEndEnumAssets);
XAM_EXPORT_STUB(__imp__XamAvatarEnumAssets);
XAM_EXPORT_STUB(__imp__XamAvatarGenerateMipMaps);
XAM_EXPORT_STUB(__imp__XamAvatarGetAssetBinary);
XAM_EXPORT_STUB(__imp__XamAvatarGetAssetIcon);
XAM_EXPORT_STUB(__imp__XamAvatarGetAssets);
XAM_EXPORT_STUB(__imp__XamAvatarGetAssetsResultSize);
XAM_EXPORT_STUB(__imp__XamAvatarGetInstalledAssetPackageDescription);
XAM_EXPORT_STUB(__imp__XamAvatarGetInstrumentation);
XAM_EXPORT_STUB(__imp__XamAvatarGetManifestLocalUser);
XAM_EXPORT_STUB(__imp__XamAvatarGetManifestsByXuid);
XAM_EXPORT_STUB(__imp__XamAvatarGetMetadataRandom);
XAM_EXPORT_STUB(__imp__XamAvatarGetMetadataSignedOutProfile);
XAM_EXPORT_STUB(__imp__XamAvatarGetMetadataSignedOutProfileCount);
XAM_EXPORT_STUB(__imp__XamAvatarLoadAnimation);
XAM_EXPORT_STUB(__imp__XamAvatarManifestGetBodyType);
XAM_EXPORT_STUB(__imp__XamAvatarReinstallAwardedAsset);
XAM_EXPORT_STUB(__imp__XamAvatarSetCustomAsset);
XAM_EXPORT_STUB(__imp__XamAvatarSetManifest);
XAM_EXPORT_STUB(__imp__XamAvatarSetMocks);
XAM_EXPORT_STUB(__imp__XamAvatarWearNow);
