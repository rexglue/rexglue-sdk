/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/assert.h>
#include <rex/types.h>

namespace rex::filesystem {

/** Characteristics bits of a PE section header. */
enum PESectionFlags : u32 {
  kPESectionContainsCode = 0x00000020,       /**< IMAGE_SCN_CNT_CODE. The section holds code. */
  kPESectionContainsDataInit = 0x00000040,   /**< IMAGE_SCN_CNT_INITIALIZED_DATA. */
  kPESectionContainsDataUninit = 0x00000080, /**< IMAGE_SCN_CNT_UNINITIALIZED_DATA. */
  kPESectionMemoryExecute = 0x20000000,      /**< IMAGE_SCN_MEM_EXECUTE. */
  kPESectionMemoryRead = 0x40000000,         /**< IMAGE_SCN_MEM_READ. The section is readable. */
  kPESectionMemoryWrite = 0x80000000,        /**< IMAGE_SCN_MEM_WRITE. */
};

/** A section of the title PE image. */
class PESection {
 public:
  char name[9];    /**< Section name from the 8 byte PE name plus a terminator. */
  u32 raw_address; /**< File offset of the section data (PointerToRawData). */
  u32 raw_size;    /**< Size of the section data in the file (SizeOfRawData). */
  u32 address;     /**< Image relative address of the section when loaded (VirtualAddress). */
  u32 size;        /**< Size of the section in memory (VirtualSize). */
  u32 flags;       /**< kPESection* flag bits. */
};

/** A named export of a PE image. */
struct PEExport {
  const char* name; /**< Name of the export. */
  u32 ordinal;      /**< Ordinal of the export. */

  u64 addr; /**< Address of the export. */
};

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

/** Media types an image may be loaded from. */
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

/** Regions an image may run in. */
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
  kXEXRegionAll = 0xFFFFFFFF,   /**< Every region bit set. */
};

/** Module flag bits of the XEX header. */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXModuleFlags : u32 {
  kXEXModuleTitle = 0x00000001, /**< The module is a title executable. */
  /** The module exports functions to the title. */
  kXEXModuleExportsToTitle = 0x00000002,
  /** The module is a system debugger. */
  kXEXModuleSystemDebugger = 0x00000004,
  /** The module is a DLL. */
  kXEXModuleDLLModule = 0x00000008,
  /** The image is a module patch. */
  kXEXModuleModulePatch = 0x00000010,
  /** The image is a full patch. */
  kXEXModulePatchFull = 0x00000020,
  /** The image is a delta patch. */
  kXEXModulePatchDelta = 0x00000040,
  kXEXModuleUserMode = 0x00000080, /**< The module runs in user mode. */
};

/** System flag bits of the XEX system flags optional header. */
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
  kXEXRatingESRBEC = 0x00,      /**< Early Childhood. */
  kXEXRatingESRBE = 0x02,       /**< Everyone. */
  kXEXRatingESRBE10 = 0x04,     /**< Everyone 10 and older. */
  kXEXRatingESRBT = 0x06,       /**< Teen. */
  kXEXRatingESRBM = 0x08,       /**< Mature. */
  kXEXRatingESRBAO = 0x0E,      /**< Adults Only. */
  kXEXRatingESRBUnrated = 0xFF, /**< No rating assigned. */
};
/** PEGI (Pan European Game Information) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingPEGIValue : u32 {
  kXEXRatingPEGI3Plus = 0,      /**< Age 3 and up. */
  kXEXRatingPEGI7Plus = 4,      /**< Age 7 and up. */
  kXEXRatingPEGI12Plus = 9,     /**< Age 12 and up. */
  kXEXRatingPEGI16Plus = 13,    /**< Age 16 and up. */
  kXEXRatingPEGI18Plus = 14,    /**< Age 18 and up. */
  kXEXRatingPEGIUnrated = 0xFF, /**< No rating assigned. */
};
/** PEGI (Pan European Game Information) for Finland */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingPEGIFIValue : u32 {
  kXEXRatingPEGIFI3Plus = 0,      /**< Age 3 and up. */
  kXEXRatingPEGIFI7Plus = 4,      /**< Age 7 and up. */
  kXEXRatingPEGIFI11Plus = 8,     /**< Age 11 and up. */
  kXEXRatingPEGIFI15Plus = 12,    /**< Age 15 and up. */
  kXEXRatingPEGIFI18Plus = 14,    /**< Age 18 and up. */
  kXEXRatingPEGIFIUnrated = 0xFF, /**< No rating assigned. */
};
/** PEGI (Pan European Game Information) for Portugal */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingPEGIPTValue : u32 {
  kXEXRatingPEGIPT4Plus = 1,      /**< Age 4 and up. */
  kXEXRatingPEGIPT6Plus = 3,      /**< Age 6 and up. */
  kXEXRatingPEGIPT12Plus = 9,     /**< Age 12 and up. */
  kXEXRatingPEGIPT16Plus = 13,    /**< Age 16 and up. */
  kXEXRatingPEGIPT18Plus = 14,    /**< Age 18 and up. */
  kXEXRatingPEGIPTUnrated = 0xFF, /**< No rating assigned. */
};
/** BBFC (British Board of Film Classification) for UK and Ireland */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingBBFCValue : u32 {
  kXEXRatingBBFCUniversal = 1,  /**< Suitable for all ages. */
  kXEXRatingBBFCPG = 5,         /**< Parental guidance. */
  kXEXRatingBBFC3Plus = 0,      /**< Age 3 and up. */
  kXEXRatingBBFC7Plus = 4,      /**< Age 7 and up. */
  kXEXRatingBBFC12Plus = 9,     /**< Age 12 and up. */
  kXEXRatingBBFC15Plus = 12,    /**< Age 15 and up. */
  kXEXRatingBBFC16Plus = 13,    /**< Age 16 and up. */
  kXEXRatingBBFC18Plus = 14,    /**< Age 18 and up. */
  kXEXRatingBBFCUnrated = 0xFF, /**< No rating assigned. */
};
/** CERO (Computer Entertainment Rating Organization) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingCEROValue : u32 {
  kXEXRatingCEROA = 0,          /**< All ages. */
  kXEXRatingCEROB = 2,          /**< Age 12 and up. */
  kXEXRatingCEROC = 4,          /**< Age 15 and up. */
  kXEXRatingCEROD = 6,          /**< Age 17 and up. */
  kXEXRatingCEROZ = 8,          /**< Restricted to age 18 and up. */
  kXEXRatingCEROUnrated = 0xFF, /**< No rating assigned. */
};
/** USK (Unterhaltungssoftware SelbstKontrolle) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingUSKValue : u32 {
  kXEXRatingUSKAll = 0,        /**< All ages. */
  kXEXRatingUSK6Plus = 2,      /**< Age 6 and up. */
  kXEXRatingUSK12Plus = 4,     /**< Age 12 and up. */
  kXEXRatingUSK16Plus = 6,     /**< Age 16 and up. */
  kXEXRatingUSK18Plus = 8,     /**< Age 18 and up. */
  kXEXRatingUSKUnrated = 0xFF, /**< No rating assigned. */
};
/** OFLC (Office of Film and Literature Classification) for Australia */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingOFLCAUValue : u32 {
  kXEXRatingOFLCAUG = 0,          /**< General. */
  kXEXRatingOFLCAUPG = 2,         /**< Parental guidance. */
  kXEXRatingOFLCAUM = 4,          /**< Mature. */
  kXEXRatingOFLCAUMA15Plus = 6,   /**< Mature accompanied, age 15 and up. */
  kXEXRatingOFLCAUUnrated = 0xFF, /**< No rating assigned. */
};
/** OFLC (Office of Film and Literature Classification) for New Zealand */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingOFLCNZValue : u32 {
  kXEXRatingOFLCNZG = 0,          /**< General. */
  kXEXRatingOFLCNZPG = 2,         /**< Parental guidance. */
  kXEXRatingOFLCNZM = 4,          /**< Mature. */
  kXEXRatingOFLCNZMA15Plus = 6,   /**< Mature accompanied, age 15 and up. */
  kXEXRatingOFLCNZUnrated = 0xFF, /**< No rating assigned. */
};
/** KMRB (Korea Media Rating Board) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingKMRBValue : u32 {
  kXEXRatingKMRBAll = 0,        /**< All ages. */
  kXEXRatingKMRB12Plus = 2,     /**< Age 12 and up. */
  kXEXRatingKMRB15Plus = 4,     /**< Age 15 and up. */
  kXEXRatingKMRB18Plus = 6,     /**< Age 18 and up. */
  kXEXRatingKMRBUnrated = 0xFF, /**< No rating assigned. */
};
/** Brazil ratings */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingBrazilValue : u32 {
  kXEXRatingBrazilAll = 0,        /**< All ages. */
  kXEXRatingBrazil12Plus = 2,     /**< Age 12 and up. */
  kXEXRatingBrazil14Plus = 4,     /**< Age 14 and up. */
  kXEXRatingBrazil16Plus = 5,     /**< Age 16 and up. */
  kXEXRatingBrazil18Plus = 8,     /**< Age 18 and up. */
  kXEXRatingBrazilUnrated = 0xFF, /**< No rating assigned. */
};
/** FPB (Film and Publication Board) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingFPBValue : u32 {
  kXEXRatingFPBAll = 0,        /**< All ages. */
  kXEXRatingFPBPG = 6,         /**< Parental guidance. */
  kXEXRatingFPB10Plus = 7,     /**< Age 10 and up. */
  kXEXRatingFPB13Plus = 10,    /**< Age 13 and up. */
  kXEXRatingFPB16Plus = 13,    /**< Age 16 and up. */
  kXEXRatingFPB18Plus = 14,    /**< Age 18 and up. */
  kXEXRatingFPBUnrated = 0xFF, /**< No rating assigned. */
};

/** Keys of the XEX optional headers. */
enum XEXHeaderKey : u32 {
  kXEXHeaderResourceInfo = 0x000002FF,         /**< Selects XEXOptResourceInfo. */
  kXEXHeaderFileFormatInfo = 0x000003FF,       /**< Selects XEXOptFileFormatInfo. */
  kXEXHeaderDeltaPatchDescriptor = 0x000005FF, /**< Selects XEXOptDeltaPatchDescriptor. */
  /** Header of a base image reference. */
  kXEXHeaderBaseReference = 0x00000405,
  kXEXHeaderBoundPath = 0x000080FF, /**< Selects XEXOptBoundPath. */
  kXEXHeaderDeviceId = 0x00008105,  /**< Header holding a device identifier. */
  /** Header holding the base address the image was linked at. */
  kXEXHeaderOriginalBaseAddress = 0x00010001,
  /** Inline value holding the guest address of the entry point. */
  kXEXHeaderEntryPoint = 0x00010100,
  /** Inline value holding the guest address the image loads at. */
  kXEXHeaderImageBaseAddress = 0x00010201,
  kXEXHeaderImportLibraries = 0x000103FF, /**< Selects XEXOptImportLibraries. */
  /** Header holding the image checksum and timestamp. */
  kXEXHeaderChecksumTimestamp = 0x00018002,
  /** Header marking the image for call profiling. */
  kXEXHeaderEnabledForCallCap = 0x00018102,
  /** Header marking the image for fast profiling. */
  kXEXHeaderEnabledForFastCap = 0x00018200,
  /** Selects XEXOptOriginalPEName. */
  kXEXHeaderOriginalPEName = 0x000183FF,
  /** Selects XEXOptStaticLibraries. */
  kXEXHeaderStaticLibraries = 0x000200FF,
  kXEXHeaderTLSInfo = 0x00020104, /**< Selects XEXOptTLSInfo. */
  /** Inline value holding the default thread stack size in bytes. */
  kXEXHeaderDefaultStackSize = 0x00020200,
  /** Inline value holding the default filesystem cache size. */
  kXEXHeaderDefaultFilesystemCacheSize = 0x00020301,
  /** Inline value holding the default heap size. */
  kXEXHeaderDefaultHeapSize = 0x00020401,
  /** Header holding the page heap size and flags. */
  kXEXHeaderPageHeapSizeAndFlags = 0x00028002,
  kXEXHeaderSystemFlags = 0x00030000,   /**< Inline value holding the XEXSystemFlags bits. */
  kXEXHeaderExecutionInfo = 0x00040006, /**< Selects XEXOptExecutionInfo. */
  /** Inline value holding the title workspace size. */
  kXEXHeaderTitleWorkspaceSize = 0x00040201,
  kXEXHeaderGameRatings = 0x00040310, /**< Selects XEXGameRatings. */
  kXEXHeaderLANKey = 0x00040404,      /**< Selects XEXOptLANKey. */
  kXEXHeaderXbox360Logo = 0x000405FF, /**< Header holding the logo bitmap. */
  /** Header listing the media ids of a multi disc title. */
  kXEXHeaderMultiDiscMediaIds = 0x000406FF,
  /** Selects XEXOptU32Words holding alternate title ids. */
  kXEXHeaderAlternateTitleIds = 0x000407FF,
  /** Header holding the additional title memory request. */
  kXEXHeaderAdditionalTitleMemory = 0x00040801,
  /** Selects XEXOptDataDirectory locating the named export table. */
  kXEXHeaderExportsByName = 0x00E10402,
};

/** Encryption applied to the image data. */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXEncryptionType : u16 {
  kXEXEncryptionNone = 0,   /**< Image data is stored unencrypted. */
  kXEXEncryptionNormal = 1, /**< Image data is AES encrypted with the image key. */
};

/** Compression applied to the image data. */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXCompressionType : u16 {
  kXEXCompressionNone = 0, /**< Image data is stored uncompressed. */
  /** Image data is stored as the runs of XEXFileBasicCompressionInfo. */
  kXEXCompressionBasic = 1,
  /** Image data is LZX compressed per XEXFileNormalCompressionInfo. */
  kXEXCompressionNormal = 2,
  kXEXCompressionDelta = 3, /**< The image is a delta patch applied to a base image. */
};

/** Approval state of a static library. */
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
  BE<XEXRatingESRBValue> esrb;     /**< ESRB rating. */
  BE<XEXRatingPEGIValue> pegi;     /**< PEGI rating. */
  BE<XEXRatingPEGIFIValue> pegifi; /**< PEGI rating for Finland. */
  BE<XEXRatingPEGIPTValue> pegipt; /**< PEGI rating for Portugal. */
  BE<XEXRatingBBFCValue> bbfc;     /**< BBFC rating. */
  BE<XEXRatingCEROValue> cero;     /**< CERO rating. */
  BE<XEXRatingUSKValue> usk;       /**< USK rating. */
  BE<XEXRatingOFLCAUValue> oflcau; /**< OFLC rating for Australia. */
  BE<XEXRatingOFLCNZValue> oflcnz; /**< OFLC rating for New Zealand. */
  BE<XEXRatingKMRBValue> kmrb;     /**< KMRB rating. */
  BE<XEXRatingBrazilValue> brazil; /**< Brazilian rating. */
  BE<XEXRatingFPBValue> fpb;       /**< FPB rating. */
};

/** A run of basic compressed data followed by a run of zeros. */
struct XEXFileBasicCompressionBlock {
  be_u32 data_size;
  be_u32 zero_size;
};
REX_STATIC_ASSERT_SIZE(XEXFileBasicCompressionBlock, 0x8);

/** Compression info of a basic compressed image. */
struct XEXFileBasicCompressionInfo {
  XEXFileBasicCompressionBlock blocks[1];
};
REX_STATIC_ASSERT_SIZE(XEXFileBasicCompressionInfo, 0x8);

/** Size and hash of the next compressed block. */
struct XEXCompressedBlockInfo {
  be_u32 block_size;
  u8 block_hash[20];
};
REX_STATIC_ASSERT_SIZE(XEXCompressedBlockInfo, 0x18);

/** Compression info of a normal compressed image. */
struct XEXFileNormalCompressionInfo {
  be_u32 window_size;
  XEXCompressedBlockInfo first_block;
};
REX_STATIC_ASSERT_SIZE(XEXFileNormalCompressionInfo, 0x1C);

/** Optional header describing the encryption and compression of the image. */
struct XEXOptFileFormatInfo {
  be_u32 info_size;
  BE<XEXEncryptionType> encryption_type;
  BE<XEXCompressionType> compression_type;
  union {
    XEXFileBasicCompressionInfo basic;   /**< Valid when compression_type is basic. */
    XEXFileNormalCompressionInfo normal; /**< Valid when compression_type is normal. */
  } compression_info;
};
REX_STATIC_ASSERT_SIZE(XEXOptFileFormatInfo, 0x24);

/**
 * XEX version word, packed MSB first on disk. After the be<> byte swap to native LE the bits of
 * value are 28-31 major, 24-27 minor, 8-23 build and 0-7 qfe. x86 LE compilers allocate bitfields
 * LSB first, so the LSB fields are listed first to match the packing.
 */
union XEXVersion {
  u32 value; /**< Whole version word in native byte order. */
  struct {
    u32 qfe : 8;    /**< Bits 0 to 7 of value. Quick fix engineering number. */
    u32 build : 16; /**< Bits 8 to 23 of value. Build number. */
    u32 minor : 4;  /**< Bits 24 to 27 of value. Minor version. */
    u32 major : 4;  /**< Bits 28 to 31 of value. Major version. */
  };
};

/** Optional header holding the LAN key. */
struct XEXOptLANKey {
  u8 key[0x10]; /**< LAN key, 16 bytes. */
};

/** Optional header holding the path the image is bound to. */
struct XEXOptBoundPath {
  be_u32 size;
  char path[1];
};
static_assert(offsetof(XEXOptBoundPath, path) == 0x4);
REX_STATIC_ASSERT_SIZE(XEXOptBoundPath, 0x8);

/** A static library linked into the image. */
struct XEXOptStaticLibrary {
  char name[8];
  be_u16 version_major;
  be_u16 version_minor;
  be_u16 version_build;
  u8 approval_type;
  u8 version_qfe;
};
REX_STATIC_ASSERT_SIZE(XEXOptStaticLibrary, 0x10);

/** Optional header listing the static libraries linked into the image. */
struct XEXOptStaticLibraries {
  be_u32 size;                      /**< Size of this header in bytes, the size field included. */
  XEXOptStaticLibrary libraries[1]; /**< First of the static library entries. */
};

/** Optional header holding the original PE file name. */
struct XEXOptOriginalPEName {
  be_u32 size;
  char name[1];
};
static_assert(offsetof(XEXOptOriginalPEName, name) == 0x4);
REX_STATIC_ASSERT_SIZE(XEXOptOriginalPEName, 0x8);

/** Optional header locating a PE data directory in the image. */
struct XEXOptDataDirectory {
  be_u32 offset;
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XEXOptDataDirectory, 0x8);

/** Optional header describing the thread local storage of the image. */
struct XEXOptTLSInfo {
  be_u32 slot_count;
  be_u32 raw_data_address;
  be_u32 data_size;
  be_u32 raw_data_size;
};
REX_STATIC_ASSERT_SIZE(XEXOptTLSInfo, 0x10);

/** A resource embedded in the image. */
struct XEXResource {
  char name[8];
  be_u32 address;
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XEXResource, 0x10);

/** Optional header listing the resources embedded in the image. */
struct XEXOptResourceInfo {
  be_u32 size; /**< Resource count is (size - 4) / 16. */
  XEXResource resources[1];
};
static_assert(offsetof(XEXOptResourceInfo, resources) == 0x4);
REX_STATIC_ASSERT_SIZE(XEXOptResourceInfo, 0x14);

/** A delta patch entry. */
struct XEXDeltaPatch {
  be_u32 old_addr;
  be_u32 new_addr;
  be_u16 uncompressed_len;
  be_u16 compressed_len;
  char patch_data[1];
};
static_assert(offsetof(XEXDeltaPatch, patch_data) == 0xC);
REX_STATIC_ASSERT_SIZE(XEXDeltaPatch, 0x10);

/** Optional header describing a delta patch against a base image. */
struct XEXOptDeltaPatchDescriptor {
  be_u32 size;
  be_u32 target_version_value;
  be_u32 source_version_value;
  u8 digest_source[0x14];
  u8 image_key_source[0x10];
  be_u32 size_of_target_headers;
  be_u32 delta_headers_source_offset;
  be_u32 delta_headers_source_size;
  be_u32 delta_headers_target_offset;
  be_u32 delta_image_source_offset;
  be_u32 delta_image_source_size;
  be_u32 delta_image_target_offset;
  XEXDeltaPatch info;

  XEXVersion GetTargetVersion() const { return XEXVersion{target_version_value}; }

  void SetTargetVersion(XEXVersion version) { target_version_value = version.value; }

  XEXVersion GetSourceVersion() const { return XEXVersion{source_version_value}; }

  void SetSourceVersion(XEXVersion version) { source_version_value = version.value; }
};
static_assert(offsetof(XEXOptDeltaPatchDescriptor, info) == 0x4C);
REX_STATIC_ASSERT_SIZE(XEXOptDeltaPatchDescriptor, 0x5C);

/** Optional header holding the identity and version of the title. */
struct XEXOptExecutionInfo {
  be_u32 media_id;
  be_u32 version_value;
  be_u32 base_version_value;
  be_u32 title_id;
  u8 platform;
  u8 executable_table;
  u8 disc_number;
  u8 disc_count;
  be_u32 savegame_id;

  XEXVersion GetVersion() const { return XEXVersion{version_value}; }

  void SetVersion(XEXVersion version) { version_value = version.value; }

  XEXVersion GetBaseVersion() const { return XEXVersion{base_version_value}; }

  void SetBaseVersion(XEXVersion version) { base_version_value = version.value; }
};
REX_STATIC_ASSERT_SIZE(XEXOptExecutionInfo, 0x18);

/** Optional header listing the libraries the image imports from. */
struct XEXOptImportLibraries {
  be_u32 size;
  struct {
    be_u32 size;
    be_u32 count;
    char data[1]; /**< string_table.size bytes. */
  } string_table;
};
static_assert(offsetof(XEXOptImportLibraries, string_table.data) == 0xC);
REX_STATIC_ASSERT_SIZE(XEXOptImportLibraries, 0x10);

/** An imported library with its version range and import table. */
struct XEXImportLibraryHeader {
  be_u32 size;
  char next_import_digest[0x14];
  be_u32 id;
  be_u32 version_value;
  be_u32 version_min_value;
  be_u16 name_index;
  be_u16 count;
  be_u32 import_table[1];

  XEXVersion GetVersion() const { return XEXVersion{version_value}; }

  void SetVersion(XEXVersion version) { version_value = version.value; }

  XEXVersion GetVersionMin() const { return XEXVersion{version_min_value}; }

  void SetVersionMin(XEXVersion version) { version_min_value = version.value; }
};
static_assert(offsetof(XEXImportLibraryHeader, import_table) == 0x28);
REX_STATIC_ASSERT_SIZE(XEXImportLibraryHeader, 0x2C);

/** SDK shape of an optional header that is a byte size followed by 32 bit words. */
struct XEXOptU32Words {
  be_u32 size;      /**< Size of this header in bytes, the size field included. */
  be_u32 values[1]; /**< First of the `GetCount()` values. */

  u32 GetCount() const { return (size - 4) / 4; }
};

/**
 * An entry of the optional header table.
 *
 * The low byte of key selects how the second field is used. 0x00 and 0x01 hold the data inline in
 * value, 0xFF holds the offset of a block that starts with its byte size, and any
 * other low byte N holds the offset of a block of N 32 bit words.
 */
struct XEXOptHeader {
  be_u32 key; /**< Header id, its low byte selects the use of the second field. */

  union {
    be_u32 value;  /**< Data held inline. */
    be_u32 offset; /**< Offset of the data block from the start of the file. */
  };
};

/** Fixed header at the start of an XEX2 file. */
struct XEXHeader {
  be_u32 magic; /**< Value 'XEX2'. */
  BE<XEXModuleFlags> module_flags;
  be_u32 header_size;
  be_u32 reserved;
  be_u32 security_offset;
  be_u32 header_count;

  XEXOptHeader headers[1];
};
static_assert(offsetof(XEXHeader, headers) == 0x18);
REX_STATIC_ASSERT_SIZE(XEXHeader, 0x20);

/** Section type, page count and digest of a run of image pages. */
struct XEXPageDescriptor {
  union {
    be_u32 value;
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
  be_u32 image_flags;
  be_u32 load_address;
  char section_digest[0x14];
  be_u32 import_table_count;
  char import_table_digest[0x14];
  char xgd2_media_id[0x10];
  char aes_key[0x10];
  be_u32 export_table;
  char header_digest[0x14];
  be_u32 region;
  be_u32 allowed_media_types;
  be_u32 page_descriptor_count;
  XEXPageDescriptor page_descriptors[1];
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
  char aes_key[0x10];
  char xgd2_media_id[0x10];
  be_u32 region;
  be_u32 image_flags;
  be_u32 export_table;
  be_u32 allowed_media_types;
  be_u32 page_descriptor_count;
  XEXPageDescriptor page_descriptors[1];
};
static_assert(offsetof(XEX1SecurityInfo, page_descriptors) == 0x168);
REX_STATIC_ASSERT_SIZE(XEX1SecurityInfo, 0x180);

/** Export table of an image, indexed by ordinal. */
struct XEXExportTable {
  be_u32 magic[3];
  be_u32 module_number[2];
  be_u32 version[3];
  be_u32 image_base_address; /**< Shifted left 16 for the true address. */
  be_u32 count;
  be_u32 base;
  /** ordinal_offset[0] + (imagebaseaddr << 16) is the function address. */
  be_u32 ordinal_offset[1];
};
static_assert(offsetof(XEXExportTable, ordinal_offset) == 0x2C);
REX_STATIC_ASSERT_SIZE(XEXExportTable, 0x30);

/** Little endian PE export directory from winnt.h. */
struct X_IMAGE_EXPORT_DIRECTORY {
  u32 Characteristics;       /**< Reserved export flags from winnt.h. */
  u32 TimeDateStamp;         /**< Time the export table was created, in seconds since 1970. */
  u16 MajorVersion;          /**< Major version of the export table. */
  u16 MinorVersion;          /**< Minor version of the export table. */
  u32 Name;                  /**< RVA of the module name string. */
  u32 Base;                  /**< Ordinal of the first entry of AddressOfFunctions. */
  u32 NumberOfFunctions;     /**< Number of entries in AddressOfFunctions. */
  u32 NumberOfNames;         /**< Number of entries in AddressOfNames. */
  u32 AddressOfFunctions;    /**< RVA from base of image. */
  u32 AddressOfNames;        /**< RVA from base of image. */
  u32 AddressOfNameOrdinals; /**< RVA from base of image. */
};

}  // namespace rex::filesystem
