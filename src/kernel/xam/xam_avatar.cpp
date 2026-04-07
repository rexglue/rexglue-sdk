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
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {

u32 XamAvatarInitialize_entry(u32 unk1,                  // 1, 4, etc
                              u32 unk2,                  // 0 or 1
                              u32 processor_number,      // for thread creation?
                              mapped_u32 function_ptrs,  // 20b, 5 pointers
                              mapped_void unk5,          // ptr in data segment
                              u32 unk6                   // flags - 0x00300000, 0x30, etc
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

REX_EXPORT(__imp__XamAvatarInitialize, rex::kernel::xam::XamAvatarInitialize_entry)
REX_EXPORT(__imp__XamAvatarShutdown, rex::kernel::xam::XamAvatarShutdown_entry)

REX_EXPORT_STUB(__imp__XamAvatarBeginEnumAssets);
REX_EXPORT_STUB(__imp__XamAvatarEndEnumAssets);
REX_EXPORT_STUB(__imp__XamAvatarEnumAssets);
REX_EXPORT_STUB(__imp__XamAvatarGenerateMipMaps);
REX_EXPORT_STUB(__imp__XamAvatarGetAssetBinary);
REX_EXPORT_STUB(__imp__XamAvatarGetAssetIcon);
REX_EXPORT_STUB(__imp__XamAvatarGetAssets);
REX_EXPORT_STUB(__imp__XamAvatarGetAssetsResultSize);
REX_EXPORT_STUB(__imp__XamAvatarGetInstalledAssetPackageDescription);
REX_EXPORT_STUB(__imp__XamAvatarGetInstrumentation);
REX_EXPORT_STUB(__imp__XamAvatarGetManifestLocalUser);
REX_EXPORT_STUB(__imp__XamAvatarGetManifestsByXuid);
REX_EXPORT_STUB(__imp__XamAvatarGetMetadataRandom);
REX_EXPORT_STUB(__imp__XamAvatarGetMetadataSignedOutProfile);
REX_EXPORT_STUB(__imp__XamAvatarGetMetadataSignedOutProfileCount);
REX_EXPORT_STUB(__imp__XamAvatarLoadAnimation);
REX_EXPORT_STUB(__imp__XamAvatarManifestGetBodyType);
REX_EXPORT_STUB(__imp__XamAvatarReinstallAwardedAsset);
REX_EXPORT_STUB(__imp__XamAvatarSetCustomAsset);
REX_EXPORT_STUB(__imp__XamAvatarSetManifest);
REX_EXPORT_STUB(__imp__XamAvatarSetMocks);
REX_EXPORT_STUB(__imp__XamAvatarWearNow);
