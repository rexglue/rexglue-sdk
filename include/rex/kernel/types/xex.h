/**
 * @file        kernel/types/xex.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's kernel/util/xex_module.cc (Ben Vanik, 2020).
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @remarks     Parts derived from Xenia (Ben Vanik, 2013).
 */

#pragma once

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex::kernel {

/** Memory protection class of a run of pages in the image. */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXSectionType {
  kXEXSectionCode = 1, /**< Pages are mapped read only unless the writable code cvar is set. */
  kXEXSectionData = 2, /**< Pages are mapped read and write. */
  kXEXSectionReadOnlyData = 3, /**< Pages are mapped read only, as for code. */
};

/** Image flag bits of the XEX security info. */
enum XEXImageFlags : u32 {
  /** @name Image flag bits of XEXSecurityInfoHeader::image_flags. */
  /** @{ */
  kXEXImageManufacturingUtility = 0x00000002,
  kXEXImageManufacturingSupportTools = 0x00000004,
  kXEXImageXGD2MediaOnly = 0x00000008,
  kXEXImageCardeaKey = 0x00000100,
  kXEXImageXeikaKey = 0x00000200,
  kXEXImageUserModeTitle = 0x00000400,
  kXEXImageUserModeSystem = 0x00000800,
  kXEXImageOrange0 = 0x00001000,
  kXEXImageOrange1 = 0x00002000,
  kXEXImageOrange2 = 0x00004000,
  kXEXImageIPTVSignupApplication = 0x00010000,
  kXEXImageIPTVTitleApplication = 0x00020000,
  kXEXImageKeyVaultPrivilegesRequired = 0x04000000,
  kXEXImageOnlineActivationRequired = 0x08000000,
  /** @} */
  kXEXImagePageSize4KB = 0x10000000, /**< Set for 4KB pages, otherwise 64KB. */
  /** @name Image flag bits of XEXSecurityInfoHeader::image_flags. */
  /** @{ */
  kXEXImageRegionFree = 0x20000000,
  kXEXImageRevocationCheckOptional = 0x40000000,
  kXEXImageRevocationCheckRequired = 0x80000000,
  /** @} */
};

enum XEXMediaFlags : u32 {
  /** @name Media bits of XEXSecurityInfoHeader::allowed_media_types. */
  /** @{ */
  kXEXMediaHardDisk = 0x00000001,
  kXEXMediaDVDX2 = 0x00000002,
  kXEXMediaDVDCD = 0x00000004,
  kXEXMediaDVD5 = 0x00000008,
  kXEXMediaDVD9 = 0x00000010,
  kXEXMediaSystemFlash = 0x00000020,
  kXEXMediaMemoryUnit = 0x00000080,
  kXEXMediaUSBMassStorageDevice = 0x00000100,
  kXEXMediaNetwork = 0x00000200,
  kXEXMediaDirectFromMemory = 0x00000400,
  kXEXMediaRAMDrive = 0x00000800,
  kXEXMediaSVOD = 0x00001000,
  kXEXMediaInsecurePackage = 0x01000000,
  kXEXMediaSaveGamePackage = 0x02000000,
  kXEXMediaLocallySignedPackage = 0x04000000,
  kXEXMediaLiveSignedPackage = 0x08000000,
  kXEXMediaXboxPackage = 0x10000000,
  /** @} */
};

enum XEXRegionFlags : u32 {
  /** North America region bits. */
  kXEXRegionNTSCU = 0x000000FF,
  /** Japan and Asia region bits. */
  kXEXRegionNTSCJ = 0x0000FF00,
  kXEXRegionNTSCJJapan = 0x00000100, /**< Japan subset of the NTSCJ bits. */
  kXEXRegionNTSCJChina = 0x00000200, /**< China subset of the NTSCJ bits. */
  kXEXRegionPAL = 0x00FF0000,        /**< Europe region bits. */
  /** Australia and New Zealand subset of the PAL bits. */
  kXEXRegionPALAUNZ = 0x00010000,
  kXEXRegionOther = 0xFF000000, /**< Rest of world region bits. */
  kXEXRegionAll = 0xFFFFFFFF,
};

// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXModuleFlags : u32 {
  kXEXModuleTitle = 0x00000001,
  kXEXModuleExportsToTitle = 0x00000002,
  kXEXModuleSystemDebugger = 0x00000004,
  kXEXModuleDLLModule = 0x00000008,
  kXEXModuleModulePatch = 0x00000010,
  kXEXModulePatchFull = 0x00000020,
  kXEXModulePatchDelta = 0x00000040,
  kXEXModuleUserMode = 0x00000080,
};

enum XEXSystemFlags : u32 {
  /** @name System flag bits. Bit N is privilege N of XexCheckExecutablePrivilege. */
  /** @{ */
  kXEXSystemNoForcedReboot = 0x00000001,
  kXEXSystemForegroundTasks = 0x00000002,
  kXEXSystemNoODDMapping = 0x00000004,
  kXEXSystemHandleMCEInput = 0x00000008,
  kXEXSystemRestrictedHUDFeatures = 0x00000010,
  kXEXSystemHandleGamepadDisconnect = 0x00000020,
  kXEXSystemInsecureSockets = 0x00000040,
  kXEXSystemXbox1Interoperability = 0x00000080,
  kXEXSystemDashContext = 0x00000100,
  kXEXSystemUsesGameVoiceChannel = 0x00000200,
  kXEXSystemPAL50Incompatible = 0x00000400,
  kXEXSystemInsecureUtilityDrive = 0x00000800,
  kXEXSystemXAMHooks = 0x00001000,
  kXEXSystemAccessPII = 0x00002000,
  kXEXSystemCrossPlatformSystemLink = 0x00004000,
  kXEXSystemMultiDiscSwap = 0x00008000,
  kXEXSystemMultiDiscInsecureMedia = 0x00010000,
  kXEXSystemAP25Media = 0x00020000,
  kXEXSystemNoConfirmExit = 0x00040000,
  kXEXSystemAllowBackgroundDownload = 0x00080000,
  kXEXSystemCreatePersistableRAMDrive = 0x00100000,
  kXEXSystemInheritPersistentRAMDrive = 0x00200000,
  kXEXSystemAllowHUDVibration = 0x00400000,
  kXEXSystemAccessUtilityPartitions = 0x00800000,
  kXEXSystemIPTVInputSupported = 0x01000000,
  kXEXSystemPreferBigButtonInput = 0x02000000,
  kXEXSystemAllowExtendedSystemReservation = 0x04000000,
  kXEXSystemMultiDiscCrossTitle = 0x08000000,
  kXEXSystemInstallIncompatible = 0x10000000,
  kXEXSystemAllowAvatarGetMetadataByXUID = 0x20000000,
  kXEXSystemAllowControllerSwapping = 0x40000000,
  kXEXSystemDashExtensibilityModule = 0x80000000,
  /** @} */
};

/** ESRB (Entertainment Software Rating Board) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingESRBValue : u32 {
  kXEXRatingESRBEC = 0x00,
  kXEXRatingESRBE = 0x02,
  kXEXRatingESRBE10 = 0x04,
  kXEXRatingESRBT = 0x06,
  kXEXRatingESRBM = 0x08,
  kXEXRatingESRBAO = 0x0E,
  kXEXRatingESRBUnrated = 0xFF,
};
/** PEGI (Pan European Game Information) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingPEGIValue : u32 {
  kXEXRatingPEGI3Plus = 0,
  kXEXRatingPEGI7Plus = 4,
  kXEXRatingPEGI12Plus = 9,
  kXEXRatingPEGI16Plus = 13,
  kXEXRatingPEGI18Plus = 14,
  kXEXRatingPEGIUnrated = 0xFF,
};
/** PEGI (Pan European Game Information) for Finland */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingPEGIFIValue : u32 {
  kXEXRatingPEGIFI3Plus = 0,
  kXEXRatingPEGIFI7Plus = 4,
  kXEXRatingPEGIFI11Plus = 8,
  kXEXRatingPEGIFI15Plus = 12,
  kXEXRatingPEGIFI18Plus = 14,
  kXEXRatingPEGIFIUnrated = 0xFF,
};
/** PEGI (Pan European Game Information) for Portugal */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingPEGIPTValue : u32 {
  kXEXRatingPEGIPT4Plus = 1,
  kXEXRatingPEGIPT6Plus = 3,
  kXEXRatingPEGIPT12Plus = 9,
  kXEXRatingPEGIPT16Plus = 13,
  kXEXRatingPEGIPT18Plus = 14,
  kXEXRatingPEGIPTUnrated = 0xFF,
};
/** BBFC (British Board of Film Classification) for UK and Ireland */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingBBFCValue : u32 {
  kXEXRatingBBFCUniversal = 1,
  kXEXRatingBBFCPG = 5,
  kXEXRatingBBFC3Plus = 0,
  kXEXRatingBBFC7Plus = 4,
  kXEXRatingBBFC12Plus = 9,
  kXEXRatingBBFC15Plus = 12,
  kXEXRatingBBFC16Plus = 13,
  kXEXRatingBBFC18Plus = 14,
  kXEXRatingBBFCUnrated = 0xFF,
};
/** CERO (Computer Entertainment Rating Organization) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingCEROValue : u32 {
  kXEXRatingCEROA = 0,
  kXEXRatingCEROB = 2,
  kXEXRatingCEROC = 4,
  kXEXRatingCEROD = 6,
  kXEXRatingCEROZ = 8,
  kXEXRatingCEROUnrated = 0xFF,
};
/** USK (Unterhaltungssoftware SelbstKontrolle) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingUSKValue : u32 {
  kXEXRatingUSKAll = 0,
  kXEXRatingUSK6Plus = 2,
  kXEXRatingUSK12Plus = 4,
  kXEXRatingUSK16Plus = 6,
  kXEXRatingUSK18Plus = 8,
  kXEXRatingUSKUnrated = 0xFF,
};
/** OFLC (Office of Film and Literature Classification) for Australia */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingOFLCAUValue : u32 {
  kXEXRatingOFLCAUG = 0,
  kXEXRatingOFLCAUPG = 2,
  kXEXRatingOFLCAUM = 4,
  kXEXRatingOFLCAUMA15Plus = 6,
  kXEXRatingOFLCAUUnrated = 0xFF,
};
/** OFLC (Office of Film and Literature Classification) for New Zealand */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingOFLCNZValue : u32 {
  kXEXRatingOFLCNZG = 0,
  kXEXRatingOFLCNZPG = 2,
  kXEXRatingOFLCNZM = 4,
  kXEXRatingOFLCNZMA15Plus = 6,
  kXEXRatingOFLCNZUnrated = 0xFF,
};
/** KMRB (Korea Media Rating Board) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingKMRBValue : u32 {
  kXEXRatingKMRBAll = 0,
  kXEXRatingKMRB12Plus = 2,
  kXEXRatingKMRB15Plus = 4,
  kXEXRatingKMRB18Plus = 6,
  kXEXRatingKMRBUnrated = 0xFF,
};
/** Brazil ratings */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingBrazilValue : u32 {
  kXEXRatingBrazilAll = 0,
  kXEXRatingBrazil12Plus = 2,
  kXEXRatingBrazil14Plus = 4,
  kXEXRatingBrazil16Plus = 5,
  kXEXRatingBrazil18Plus = 8,
  kXEXRatingBrazilUnrated = 0xFF,
};
/** FPB (Film and Publication Board) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingFPBValue : u32 {
  kXEXRatingFPBAll = 0,
  kXEXRatingFPBPG = 6,
  kXEXRatingFPB10Plus = 7,
  kXEXRatingFPB13Plus = 10,
  kXEXRatingFPB16Plus = 13,
  kXEXRatingFPB18Plus = 14,
  kXEXRatingFPBUnrated = 0xFF,
};

enum XEXHeaderKey : u32 {
  kXEXHeaderResourceInfo = 0x000002FF,
  kXEXHeaderFileFormatInfo = 0x000003FF,
  kXEXHeaderDeltaPatchDescriptor = 0x000005FF,
  kXEXHeaderBaseReference = 0x00000405,
  kXEXHeaderBoundPath = 0x000080FF,
  kXEXHeaderDeviceId = 0x00008105,
  kXEXHeaderOriginalBaseAddress = 0x00010001,
  /** Inline value holding the guest address of the entry point. */
  kXEXHeaderEntryPoint = 0x00010100,
  /** Inline value holding the guest address the image loads at. */
  kXEXHeaderImageBaseAddress = 0x00010201,
  kXEXHeaderImportLibraries = 0x000103FF,
  kXEXHeaderChecksumTimestamp = 0x00018002,
  kXEXHeaderEnabledForCallCap = 0x00018102,
  kXEXHeaderEnabledForFastCap = 0x00018200,
  kXEXHeaderOriginalPEName = 0x000183FF,
  kXEXHeaderStaticLibraries = 0x000200FF,
  kXEXHeaderTLSInfo = 0x00020104,
  /** Inline value holding the default thread stack size in bytes. */
  kXEXHeaderDefaultStackSize = 0x00020200,
  /** Inline value holding the default filesystem cache size. */
  kXEXHeaderDefaultFilesystemCacheSize = 0x00020301,
  /** Inline value holding the default heap size. */
  kXEXHeaderDefaultHeapSize = 0x00020401,
  kXEXHeaderPageHeapSizeAndFlags = 0x00028002,
  kXEXHeaderSystemFlags = 0x00030000,
  kXEXHeaderExecutionInfo = 0x00040006,
  /** Inline value holding the title workspace size. */
  kXEXHeaderTitleWorkspaceSize = 0x00040201,
  kXEXHeaderGameRatings = 0x00040310,
  kXEXHeaderLANKey = 0x00040404,
  kXEXHeaderXbox360Logo = 0x000405FF,
  kXEXHeaderMultiDiscMediaIDs = 0x000406FF,
  /** Selects XEXOptU32Words holding alternate title IDs. */
  kXEXHeaderAlternateTitleIDs = 0x000407FF,
  kXEXHeaderAdditionalTitleMemory = 0x00040801,
  /** Selects XEXOptDataDirectory locating the named export table. */
  kXEXHeaderExportsByName = 0x00E10402,
};

// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXEncryptionType : u16 {
  kXEXEncryptionNone = 0,
  kXEXEncryptionNormal = 1,
};

// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXCompressionType : u16 {
  kXEXCompressionNone = 0,
  /** Image data is stored as the runs of XEXFileBasicCompressionInfo. */
  kXEXCompressionBasic = 1,
  /** Image data is LZX compressed per XEXFileNormalCompressionInfo. */
  kXEXCompressionNormal = 2,
  kXEXCompressionDelta = 3, /**< The image is a delta patch applied to a base image. */
};

enum XEXApprovalType : u8 {
  /** @name Values of XEXOptStaticLibrary::approval_type. */
  /** @{ */
  kXEXApprovalUnapproved = 0,
  kXEXApprovalPossible = 1,
  kXEXApprovalApproved = 2,
  kXEXApprovalExpired = 3,
  /** @} */
};

/** Age rating of the title for each rating board. */
struct XEXGameRatings {
  BE<XEXRatingESRBValue> esrb;
  BE<XEXRatingPEGIValue> pegi;
  BE<XEXRatingPEGIFIValue> pegifi;
  BE<XEXRatingPEGIPTValue> pegipt;
  BE<XEXRatingBBFCValue> bbfc;
  BE<XEXRatingCEROValue> cero;
  BE<XEXRatingUSKValue> usk;
  BE<XEXRatingOFLCAUValue> oflcau;
  BE<XEXRatingOFLCNZValue> oflcnz;
  BE<XEXRatingKMRBValue> kmrb;
  BE<XEXRatingBrazilValue> brazil;
  BE<XEXRatingFPBValue> fpb;
};

/** A run of basic compressed data followed by a run of zeros. */
struct XEXFileBasicCompressionBlock {
  be_u32 data_size; /**< Number of image data bytes in the run. */
  be_u32 zero_size; /**< Number of zero bytes that follow the run in the loaded image. */
};
REX_STATIC_ASSERT_SIZE(XEXFileBasicCompressionBlock, 0x8);

struct XEXFileBasicCompressionInfo {
  /** Data and zero runs, counted as (info_size - 8) / 8. */
  XEXFileBasicCompressionBlock blocks[1];
};
REX_STATIC_ASSERT_SIZE(XEXFileBasicCompressionInfo, 0x8);

struct XEXCompressedBlockInfo {
  be_u32 block_size; /**< Size in bytes of the block this info belongs to, this info included. */
  u8 block_hash[20];
};
REX_STATIC_ASSERT_SIZE(XEXCompressedBlockInfo, 0x18);

struct XEXFileNormalCompressionInfo {
  be_u32 window_size; /**< Size of the LZX window in bytes. */
  XEXCompressedBlockInfo first_block;
};
REX_STATIC_ASSERT_SIZE(XEXFileNormalCompressionInfo, 0x1C);

/** Optional header with the encryption and compression types and the compression info. */
struct XEXOptFileFormatInfo {
  be_u32 info_size; /**< Size of this header in bytes. */
  BE<XEXEncryptionType> encryption_type;
  BE<XEXCompressionType> compression_type;
  union {
    XEXFileBasicCompressionInfo basic;   /**< Valid when compression_type is basic. */
    XEXFileNormalCompressionInfo normal; /**< Valid when compression_type is normal. */
  } compression_info;                    /**< Compression details selected by compression_type. */
};
REX_STATIC_ASSERT_SIZE(XEXOptFileFormatInfo, 0x24);

/** XEX version word, major in bits 28 to 31, minor 24 to 27, build 8 to 23 and QFE 0 to 7. */
union XEXVersion {
  u32 value;
  struct {
    u32 qfe : 8;
    u32 build : 16;
    u32 minor : 4;
    u32 major : 4;
  };
};

struct XEXOptLANKey {
  u8 key[0x10];
};

struct XEXOptBoundPath {
  be_u32 size;  /**< Size of this header in bytes, the size field included. */
  char path[1]; /**< First character of the bound path. */
};
static_assert(offsetof(XEXOptBoundPath, path) == 0x4);
REX_STATIC_ASSERT_SIZE(XEXOptBoundPath, 0x8);

struct XEXOptStaticLibrary {
  char name[8];
  be_u16 version_major;
  be_u16 version_minor;
  be_u16 version_build;
  u8 approval_type;
  u8 version_qfe;
};
REX_STATIC_ASSERT_SIZE(XEXOptStaticLibrary, 0x10);

struct XEXOptStaticLibraries {
  be_u32 size;                      /**< Size of this header in bytes, the size field included. */
  XEXOptStaticLibrary libraries[1]; /**< First of the static library entries. */
};

struct XEXOptOriginalPEName {
  be_u32 size;  /**< Size of this header in bytes, the size field included. */
  char name[1]; /**< First character of the original file name. */
};
static_assert(offsetof(XEXOptOriginalPEName, name) == 0x4);
REX_STATIC_ASSERT_SIZE(XEXOptOriginalPEName, 0x8);

/** Optional header locating a PE data directory in the image. */
struct XEXOptDataDirectory {
  be_u32 offset; /**< Offset of the directory in the loaded image. */
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XEXOptDataDirectory, 0x8);

struct XEXOptTLSInfo {
  be_u32 slot_count;
  be_u32 raw_data_address;
  be_u32 data_size;     /**< Size of the TLS data of each thread in bytes. */
  be_u32 raw_data_size; /**< Size of the initial TLS data in bytes. */
};
REX_STATIC_ASSERT_SIZE(XEXOptTLSInfo, 0x10);

struct XEXResource {
  char name[8];
  be_u32 address;
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XEXResource, 0x10);

struct XEXOptResourceInfo {
  be_u32 size;              /**< Resource count is (size - 4) / 16. */
  XEXResource resources[1]; /**< First of the resource entries. */
};
static_assert(offsetof(XEXOptResourceInfo, resources) == 0x4);
REX_STATIC_ASSERT_SIZE(XEXOptResourceInfo, 0x14);

struct XEXDeltaPatch {
  be_u32 old_addr;
  be_u32 new_addr;
  be_u16 uncompressed_len; /**< Size of the patch data once decompressed, in bytes. */
  be_u16 compressed_len;
  char patch_data[1]; /**< First byte of the compressed delta data. */
};
static_assert(offsetof(XEXDeltaPatch, patch_data) == 0xC);
REX_STATIC_ASSERT_SIZE(XEXDeltaPatch, 0x10);

struct XEXOptDeltaPatchDescriptor {
  be_u32 size;
  be_u32 target_version_value;   /**< Version word of the patched image, as XEXVersion::value. */
  be_u32 source_version_value;   /**< Version word of the base image, as XEXVersion::value. */
  u8 digest_source[0x14];        /**< SHA1 digest of the RSA signature of the base XEX. */
  u8 image_key_source[0x10];     /**< Encrypted image key of the base XEX. */
  be_u32 size_of_target_headers; /**< Size in bytes of the headers of the patched image. */
  /** Offset in the base headers of the header bytes kept by the patch. */
  be_u32 delta_headers_source_offset;
  be_u32 delta_headers_source_size; /**< Size in bytes of the header bytes kept by the patch. */
  /** Offset in the patched headers where the kept header bytes are placed. */
  be_u32 delta_headers_target_offset;
  /** Offset in the base image of the image bytes kept by the patch. */
  be_u32 delta_image_source_offset;
  be_u32 delta_image_source_size; /**< Size in bytes of the image bytes kept by the patch. */
  /** Offset in the patched image where the kept image bytes are placed. */
  be_u32 delta_image_target_offset;
  XEXDeltaPatch info;

  XEXVersion GetTargetVersion() const { return XEXVersion{target_version_value}; }

  void SetTargetVersion(XEXVersion version) { target_version_value = version.value; }

  XEXVersion GetSourceVersion() const { return XEXVersion{source_version_value}; }

  void SetSourceVersion(XEXVersion version) { source_version_value = version.value; }
};
static_assert(offsetof(XEXOptDeltaPatchDescriptor, info) == 0x4C);
REX_STATIC_ASSERT_SIZE(XEXOptDeltaPatchDescriptor, 0x5C);

struct XEXOptExecutionInfo {
  be_u32 media_id;
  be_u32 version_value;      /**< Version word of the title, as XEXVersion::value. */
  be_u32 base_version_value; /**< Version word of the base title, as XEXVersion::value. */
  be_u32 title_id;
  u8 platform;
  u8 executable_table;
  u8 disc_number; /**< Number of this disc in a multi disc title. */
  u8 disc_count;
  be_u32 savegame_id;

  XEXVersion GetVersion() const { return XEXVersion{version_value}; }

  void SetVersion(XEXVersion version) { version_value = version.value; }

  XEXVersion GetBaseVersion() const { return XEXVersion{base_version_value}; }

  void SetBaseVersion(XEXVersion version) { base_version_value = version.value; }
};
REX_STATIC_ASSERT_SIZE(XEXOptExecutionInfo, 0x18);

struct XEXOptImportLibraries {
  be_u32 size; /**< Size of this header in bytes, the size field included. */
  struct {
    be_u32 size;
    be_u32 count;
    char data[1]; /**< The name strings, string_table.size bytes. */
  } string_table;
};
static_assert(offsetof(XEXOptImportLibraries, string_table.data) == 0xC);
REX_STATIC_ASSERT_SIZE(XEXOptImportLibraries, 0x10);

/** An imported library with its version range and import table. */
struct XEXImportLibraryHeader {
  be_u32 size; /**< Size of this header in bytes, the import table included. */
  char next_import_digest[0x14];
  be_u32 id;
  be_u32 version_value; /**< Version word of the library, as XEXVersion::value. */
  /** Lowest version word of the library the image accepts, as XEXVersion::value. */
  be_u32 version_min_value;
  be_u16 name_index; /**< Index of the library name in the string table. */
  be_u16 count;
  be_u32 import_table[1]; /**< First of the import records. */

  XEXVersion GetVersion() const { return XEXVersion{version_value}; }

  void SetVersion(XEXVersion version) { version_value = version.value; }

  XEXVersion GetVersionMin() const { return XEXVersion{version_min_value}; }

  void SetVersionMin(XEXVersion version) { version_min_value = version.value; }
};
static_assert(offsetof(XEXImportLibraryHeader, import_table) == 0x28);
REX_STATIC_ASSERT_SIZE(XEXImportLibraryHeader, 0x2C);

/** Optional header that is a byte size followed by 32 bit words. */
struct XEXOptU32Words {
  be_u32 size;      /**< Size of this header in bytes, the size field included. */
  be_u32 values[1]; /**< First of the `GetCount()` values. */

  u32 GetCount() const { return (size - 4) / 4; }
};

/**
 * An entry of the optional header table.
 *
 * The low byte of key selects how the second field is used. 0x00 and 0x01 hold the data inline in
 * value, 0xFF holds the offset of a block that starts with its byte size, and any other low byte N
 * holds the offset of a block of N 32 bit words.
 */
struct XEXOptHeader {
  be_u32 key; /**< Header ID, its low byte selects the use of the second field. */

  union {
    be_u32 value;  /**< Data held inline. */
    be_u32 offset; /**< Offset of the data block from the start of the file. */
  };
};

/** Fixed header at the start of an XEX2 file. */
struct XEXHeader {
  be_u32 magic; /**< Equals 'XEX2'. */
  BE<XEXModuleFlags> module_flags;
  be_u32 header_size; /**< Size of the headers in bytes and the offset of the image data. */
  be_u32 reserved;
  be_u32 security_offset; /**< Offset of the security info from the start of the file. */
  be_u32 header_count;

  XEXOptHeader headers[1]; /**< First of the optional header table entries. */
};
static_assert(offsetof(XEXHeader, headers) == 0x18);
REX_STATIC_ASSERT_SIZE(XEXHeader, 0x20);

/** Section type, page count and digest of a run of image pages. */
struct XEXPageDescriptor {
  union {
    be_u32 value; /**< Packed section type and page count. */
    struct {
      XEXSectionType type : 4; /**< Bits 0 to 3 of value. Section type of the pages. */
      u32 page_count : 28;     /**< Bits 4 to 31 of value. Number of pages in the run. */
    };
  };
  char data_digest[0x14];
};
REX_STATIC_ASSERT_SIZE(XEXPageDescriptor, 0x18);

/** Security info of an XEX2 image. */
struct XEXSecurityInfoHeader {
  be_u32 header_size;
  be_u32 image_size;
  char rsa_signature[0x100];
  be_u32 info_size;
  be_u32 image_flags; /**< XEXImageFlags bits. */
  be_u32 load_address;
  char section_digest[0x14]; /**< Digest of the image sections. */
  be_u32 import_table_count;
  char import_table_digest[0x14];
  char xgd2_media_id[0x10];
  char aes_key[0x10]; /**< Encrypted AES key of the image. */
  be_u32 export_table;
  char header_digest[0x14];
  be_u32 region;              /**< XEXRegionFlags bits the image may run in. */
  be_u32 allowed_media_types; /**< XEXMediaFlags bits the image may load from. */
  be_u32 page_descriptor_count;
  XEXPageDescriptor page_descriptors[1]; /**< First of the page descriptors. */
};
static_assert(offsetof(XEXSecurityInfoHeader, page_descriptors) == 0x184);
REX_STATIC_ASSERT_SIZE(XEXSecurityInfoHeader, 0x19C);

/** Security info of an XEX1 image. */
struct XEX1SecurityInfo {
  be_u32 header_size;
  be_u32 image_size;
  char rsa_signature[0x100];
  char image_digest[0x14];
  char import_table_digest[0x14];
  be_u32 load_address;
  char aes_key[0x10]; /**< Encrypted AES key of the image. */
  char xgd2_media_id[0x10];
  be_u32 region;      /**< XEXRegionFlags bits the image may run in. */
  be_u32 image_flags; /**< XEXImageFlags bits. */
  be_u32 export_table;
  be_u32 allowed_media_types; /**< XEXMediaFlags bits the image may load from. */
  be_u32 page_descriptor_count;
  XEXPageDescriptor page_descriptors[1]; /**< First of the page descriptors. */
};
static_assert(offsetof(XEX1SecurityInfo, page_descriptors) == 0x168);
REX_STATIC_ASSERT_SIZE(XEX1SecurityInfo, 0x180);

/** Export table of an image, indexed by ordinal. */
struct XEXExportTable {
  be_u32 magic[3];
  be_u32 module_number[2]; /**< Module number words of the export table. */
  be_u32 version[3];
  be_u32 image_base_address; /**< Shifted left 16 to give the image base address. */
  be_u32 count;              /**< Number of entries in ordinal_offset. */
  be_u32 base;               /**< Ordinal of the first entry. */
  /** ordinal_offset[0] + (image_base_address << 16) is the function address. */
  be_u32 ordinal_offset[1];
};
static_assert(offsetof(XEXExportTable, ordinal_offset) == 0x2C);
REX_STATIC_ASSERT_SIZE(XEXExportTable, 0x30);

/** Little endian PE export directory from winnt.h. */
struct X_IMAGE_EXPORT_DIRECTORY {
  u32 Characteristics; /**< Reserved export flags from winnt.h. */
  u32 TimeDateStamp;   /**< Time the export table was created, in seconds since 1970. */
  u16 MajorVersion;
  u16 MinorVersion;
  u32 Name;                  /**< RVA of the module name string. */
  u32 Base;                  /**< Ordinal of the first entry of AddressOfFunctions. */
  u32 NumberOfFunctions;     /**< Number of entries in AddressOfFunctions. */
  u32 NumberOfNames;         /**< Number of entries in AddressOfNames. */
  u32 AddressOfFunctions;    /**< RVA from base of image. */
  u32 AddressOfNames;        /**< RVA from base of image. */
  u32 AddressOfNameOrdinals; /**< RVA from base of image. */
};

}  // namespace rex::kernel
