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
  /** IMAGE_SCN_CNT_CODE. The section holds executable code. */
  kPESectionContainsCode = 0x00000020,
  /** IMAGE_SCN_CNT_INITIALIZED_DATA. The section holds initialized data. */
  kPESectionContainsDataInit = 0x00000040,
  /** IMAGE_SCN_CNT_UNINITIALIZED_DATA. The section holds zero filled data. */
  kPESectionContainsDataUninit = 0x00000080,
  /** IMAGE_SCN_MEM_EXECUTE. XEXModule reads it to mark the section executable. */
  kPESectionMemoryExecute = 0x20000000,
  /** IMAGE_SCN_MEM_READ. The section is readable. No code reads it. */
  kPESectionMemoryRead = 0x40000000,
  /** IMAGE_SCN_MEM_WRITE. XEXModule reads it to mark the section writable. */
  kPESectionMemoryWrite = 0x80000000,
};

/** 1 section of the title PE image. */
class PESection {
 public:
  char name[9];    /**< Section name from the 8 byte PE name plus a terminator. */
  u32 raw_address; /**< File offset of the section data (PointerToRawData). */
  u32 raw_size;    /**< Size of the section data in the file (SizeOfRawData). */
  u32 address;     /**< Image relative address of the section when loaded (VirtualAddress). */
  u32 size;        /**< Size of the section in memory (VirtualSize). */
  u32 flags;       /**< kPESection* flag bits. */
};

/** 1 named export of a PE image. */
struct PEExport {
  const char* name; /**< Name of the export. No code reads it. */
  u32 ordinal;      /**< Ordinal of the export. No code reads it. */

  u64 addr; /**< Address of the export. No code reads it. */
};

/** Memory protection class of a run of pages in the image. */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXSectionType {
  XEX_SECTION_CODE = 1, /**< Pages are mapped read only unless the writable code cvar is set. */
  XEX_SECTION_DATA = 2, /**< Pages are mapped read and write. */
  XEX_SECTION_READONLY_DATA = 3, /**< Pages are mapped read only, as for code. */
};

/** Image flag bits of the XEX security info. */
enum XEXImageFlags : u32 {
  /** @name Image flag bits of XEXSecurityInfoHeader::image_flags. No code reads them. */
  /** @{ */
  XEX_IMAGE_MANUFACTURING_UTILITY = 0x00000002,
  XEX_IMAGE_MANUFACTURING_SUPPORT_TOOLS = 0x00000004,
  XEX_IMAGE_XGD2_MEDIA_ONLY = 0x00000008,
  XEX_IMAGE_CARDEA_KEY = 0x00000100,
  XEX_IMAGE_XEIKA_KEY = 0x00000200,
  XEX_IMAGE_USERMODE_TITLE = 0x00000400,
  XEX_IMAGE_USERMODE_SYSTEM = 0x00000800,
  XEX_IMAGE_ORANGE0 = 0x00001000,
  XEX_IMAGE_ORANGE1 = 0x00002000,
  XEX_IMAGE_ORANGE2 = 0x00004000,
  XEX_IMAGE_IPTV_SIGNUP_APPLICATION = 0x00010000,
  XEX_IMAGE_IPTV_TITLE_APPLICATION = 0x00020000,
  XEX_IMAGE_KEYVAULT_PRIVILEGES_REQUIRED = 0x04000000,
  XEX_IMAGE_ONLINE_ACTIVATION_REQUIRED = 0x08000000,
  /** @} */
  XEX_IMAGE_PAGE_SIZE_4KB = 0x10000000, /**< Set for 4KB pages, otherwise 64KB. */
  /** @name Image flag bits of XEXSecurityInfoHeader::image_flags. No code reads them. */
  /** @{ */
  XEX_IMAGE_REGION_FREE = 0x20000000,
  XEX_IMAGE_REVOCATION_CHECK_OPTIONAL = 0x40000000,
  XEX_IMAGE_REVOCATION_CHECK_REQUIRED = 0x80000000,
  /** @} */
};

/** Media types an image may be loaded from. */
enum XEXMediaFlags : u32 {
  /** @name Media bits of XEXSecurityInfoHeader::allowed_media_types. No code reads them. */
  /** @{ */
  XEX_MEDIA_HARDDISK = 0x00000001,
  XEX_MEDIA_DVD_X2 = 0x00000002,
  XEX_MEDIA_DVD_CD = 0x00000004,
  XEX_MEDIA_DVD_5 = 0x00000008,
  XEX_MEDIA_DVD_9 = 0x00000010,
  XEX_MEDIA_SYSTEM_FLASH = 0x00000020,
  XEX_MEDIA_MEMORY_UNIT = 0x00000080,
  XEX_MEDIA_USB_MASS_STORAGE_DEVICE = 0x00000100,
  XEX_MEDIA_NETWORK = 0x00000200,
  XEX_MEDIA_DIRECT_FROM_MEMORY = 0x00000400,
  XEX_MEDIA_RAM_DRIVE = 0x00000800,
  XEX_MEDIA_SVOD = 0x00001000,
  XEX_MEDIA_INSECURE_PACKAGE = 0x01000000,
  XEX_MEDIA_SAVEGAME_PACKAGE = 0x02000000,
  XEX_MEDIA_LOCALLY_SIGNED_PACKAGE = 0x04000000,
  XEX_MEDIA_LIVE_SIGNED_PACKAGE = 0x08000000,
  XEX_MEDIA_XBOX_PACKAGE = 0x10000000,
  /** @} */
};

/** Regions an image may run in. */
enum XEXRegionFlags : u32 {
  /** North America region bits. The title language query tests them to pick English. */
  XEX_REGION_NTSCU = 0x000000FF,
  /** Japan and Asia region bits. The title language query tests them to pick Japanese. */
  XEX_REGION_NTSCJ = 0x0000FF00,
  XEX_REGION_NTSCJ_JAPAN = 0x00000100, /**< Japan subset of the NTSCJ bits. No code reads it. */
  XEX_REGION_NTSCJ_CHINA = 0x00000200, /**< China subset of the NTSCJ bits. No code reads it. */
  XEX_REGION_PAL = 0x00FF0000,         /**< Europe region bits. No code reads it. */
  /** Australia and New Zealand subset of the PAL bits. No code reads it. */
  XEX_REGION_PAL_AU_NZ = 0x00010000,
  XEX_REGION_OTHER = 0xFF000000, /**< Rest of world region bits. No code reads it. */
  XEX_REGION_ALL = 0xFFFFFFFF,   /**< Every region bit set. No code reads it. */
};

/** Module flag bits of the XEX header. */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXModuleFlags : u32 {
  XEX_MODULE_TITLE = 0x00000001, /**< The module is a title executable. No code reads it. */
  /** The module exports functions to the title. No code reads it. */
  XEX_MODULE_EXPORTS_TO_TITLE = 0x00000002,
  /** The module is a system debugger. No code reads it. */
  XEX_MODULE_SYSTEM_DEBUGGER = 0x00000004,
  /** The module is a DLL. UserModule reads it into is_dll_module. */
  XEX_MODULE_DLL_MODULE = 0x00000008,
  /** The image is a module patch. XEXImage::is_patch tests it. */
  XEX_MODULE_MODULE_PATCH = 0x00000010,
  /** The image is a full patch. XEXImage::is_patch tests it. */
  XEX_MODULE_PATCH_FULL = 0x00000020,
  /** The image is a delta patch. XEXImage::is_patch tests it. */
  XEX_MODULE_PATCH_DELTA = 0x00000040,
  XEX_MODULE_USER_MODE = 0x00000080, /**< The module runs in user mode. No code reads it. */
};

/** System flag bits of the XEX system flags optional header. */
enum XEXSystemFlags : u32 {
  /** @name System flag bits. Bit N is privilege N of XexCheckExecutablePrivilege. */
  /** @{ */
  XEX_SYSTEM_NO_FORCED_REBOOT = 0x00000001,
  XEX_SYSTEM_FOREGROUND_TASKS = 0x00000002,
  XEX_SYSTEM_NO_ODD_MAPPING = 0x00000004,
  XEX_SYSTEM_HANDLE_MCE_INPUT = 0x00000008,
  XEX_SYSTEM_RESTRICTED_HUD_FEATURES = 0x00000010,
  XEX_SYSTEM_HANDLE_GAMEPAD_DISCONNECT = 0x00000020,
  XEX_SYSTEM_INSECURE_SOCKETS = 0x00000040,
  XEX_SYSTEM_XBOX1_INTEROPERABILITY = 0x00000080,
  XEX_SYSTEM_DASH_CONTEXT = 0x00000100,
  XEX_SYSTEM_USES_GAME_VOICE_CHANNEL = 0x00000200,
  XEX_SYSTEM_PAL50_INCOMPATIBLE = 0x00000400,
  XEX_SYSTEM_INSECURE_UTILITY_DRIVE = 0x00000800,
  XEX_SYSTEM_XAM_HOOKS = 0x00001000,
  XEX_SYSTEM_ACCESS_PII = 0x00002000,
  XEX_SYSTEM_CROSS_PLATFORM_SYSTEM_LINK = 0x00004000,
  XEX_SYSTEM_MULTIDISC_SWAP = 0x00008000,
  XEX_SYSTEM_MULTIDISC_INSECURE_MEDIA = 0x00010000,
  XEX_SYSTEM_AP25_MEDIA = 0x00020000,
  XEX_SYSTEM_NO_CONFIRM_EXIT = 0x00040000,
  XEX_SYSTEM_ALLOW_BACKGROUND_DOWNLOAD = 0x00080000,
  XEX_SYSTEM_CREATE_PERSISTABLE_RAMDRIVE = 0x00100000,
  XEX_SYSTEM_INHERIT_PERSISTENT_RAMDRIVE = 0x00200000,
  XEX_SYSTEM_ALLOW_HUD_VIBRATION = 0x00400000,
  XEX_SYSTEM_ACCESS_UTILITY_PARTITIONS = 0x00800000,
  XEX_SYSTEM_IPTV_INPUT_SUPPORTED = 0x01000000,
  XEX_SYSTEM_PREFER_BIG_BUTTON_INPUT = 0x02000000,
  XEX_SYSTEM_ALLOW_EXTENDED_SYSTEM_RESERVATION = 0x04000000,
  XEX_SYSTEM_MULTIDISC_CROSS_TITLE = 0x08000000,
  XEX_SYSTEM_INSTALL_INCOMPATIBLE = 0x10000000,
  XEX_SYSTEM_ALLOW_AVATAR_GET_METADATA_BY_XUID = 0x20000000,
  XEX_SYSTEM_ALLOW_CONTROLLER_SWAPPING = 0x40000000,
  XEX_SYSTEM_DASH_EXTENSIBILITY_MODULE = 0x80000000,
  /** @} */
};

/** ESRB (Entertainment Software Rating Board) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingESRBValue : u32 {
  XEX_RATING_ESRB_eC = 0x00,      /**< Early Childhood. */
  XEX_RATING_ESRB_E = 0x02,       /**< Everyone. */
  XEX_RATING_ESRB_E10 = 0x04,     /**< Everyone 10 and older. */
  XEX_RATING_ESRB_T = 0x06,       /**< Teen. */
  XEX_RATING_ESRB_M = 0x08,       /**< Mature. */
  XEX_RATING_ESRB_AO = 0x0E,      /**< Adults Only. */
  XEX_RATING_ESRB_UNRATED = 0xFF, /**< No rating assigned. */
};
/** PEGI (Pan European Game Information) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingPEGIValue : u32 {
  XEX_RATING_PEGI_3_PLUS = 0,     /**< Age 3 and up. */
  XEX_RATING_PEGI_7_PLUS = 4,     /**< Age 7 and up. */
  XEX_RATING_PEGI_12_PLUS = 9,    /**< Age 12 and up. */
  XEX_RATING_PEGI_16_PLUS = 13,   /**< Age 16 and up. */
  XEX_RATING_PEGI_18_PLUS = 14,   /**< Age 18 and up. */
  XEX_RATING_PEGI_UNRATED = 0xFF, /**< No rating assigned. */
};
/** PEGI (Pan European Game Information) for Finland */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingPEGIFIValue : u32 {
  XEX_RATING_PEGI_FI_3_PLUS = 0,     /**< Age 3 and up. */
  XEX_RATING_PEGI_FI_7_PLUS = 4,     /**< Age 7 and up. */
  XEX_RATING_PEGI_FI_11_PLUS = 8,    /**< Age 11 and up. */
  XEX_RATING_PEGI_FI_15_PLUS = 12,   /**< Age 15 and up. */
  XEX_RATING_PEGI_FI_18_PLUS = 14,   /**< Age 18 and up. */
  XEX_RATING_PEGI_FI_UNRATED = 0xFF, /**< No rating assigned. */
};
/** PEGI (Pan European Game Information) for Portugal */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingPEGIPTValue : u32 {
  XEX_RATING_PEGI_PT_4_PLUS = 1,     /**< Age 4 and up. */
  XEX_RATING_PEGI_PT_6_PLUS = 3,     /**< Age 6 and up. */
  XEX_RATING_PEGI_PT_12_PLUS = 9,    /**< Age 12 and up. */
  XEX_RATING_PEGI_PT_16_PLUS = 13,   /**< Age 16 and up. */
  XEX_RATING_PEGI_PT_18_PLUS = 14,   /**< Age 18 and up. */
  XEX_RATING_PEGI_PT_UNRATED = 0xFF, /**< No rating assigned. */
};
/** BBFC (British Board of Film Classification) for UK and Ireland */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingBBFCValue : u32 {
  XEX_RATING_BBFC_UNIVERSAL = 1,  /**< Suitable for all ages. */
  XEX_RATING_BBFC_PG = 5,         /**< Parental guidance. */
  XEX_RATING_BBFC_3_PLUS = 0,     /**< Age 3 and up. */
  XEX_RATING_BBFC_7_PLUS = 4,     /**< Age 7 and up. */
  XEX_RATING_BBFC_12_PLUS = 9,    /**< Age 12 and up. */
  XEX_RATING_BBFC_15_PLUS = 12,   /**< Age 15 and up. */
  XEX_RATING_BBFC_16_PLUS = 13,   /**< Age 16 and up. */
  XEX_RATING_BBFC_18_PLUS = 14,   /**< Age 18 and up. */
  XEX_RATING_BBFC_UNRATED = 0xFF, /**< No rating assigned. */
};
/** CERO (Computer Entertainment Rating Organization) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingCEROValue : u32 {
  XEX_RATING_CERO_A = 0,          /**< All ages. */
  XEX_RATING_CERO_B = 2,          /**< Age 12 and up. */
  XEX_RATING_CERO_C = 4,          /**< Age 15 and up. */
  XEX_RATING_CERO_D = 6,          /**< Age 17 and up. */
  XEX_RATING_CERO_Z = 8,          /**< Restricted to age 18 and up. */
  XEX_RATING_CERO_UNRATED = 0xFF, /**< No rating assigned. */
};
/** USK (Unterhaltungssoftware SelbstKontrolle) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingUSKValue : u32 {
  XEX_RATING_USK_ALL = 0,        /**< All ages. */
  XEX_RATING_USK_6_PLUS = 2,     /**< Age 6 and up. */
  XEX_RATING_USK_12_PLUS = 4,    /**< Age 12 and up. */
  XEX_RATING_USK_16_PLUS = 6,    /**< Age 16 and up. */
  XEX_RATING_USK_18_PLUS = 8,    /**< Age 18 and up. */
  XEX_RATING_USK_UNRATED = 0xFF, /**< No rating assigned. */
};
/** OFLC (Office of Film and Literature Classification) for Australia */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingOFLCAUValue : u32 {
  XEX_RATING_OFLC_AU_G = 0,          /**< General. */
  XEX_RATING_OFLC_AU_PG = 2,         /**< Parental guidance. */
  XEX_RATING_OFLC_AU_M = 4,          /**< Mature. */
  XEX_RATING_OFLC_AU_MA15_PLUS = 6,  /**< Mature accompanied, age 15 and up. */
  XEX_RATING_OFLC_AU_UNRATED = 0xFF, /**< No rating assigned. */
};
/** OFLC (Office of Film and Literature Classification) for New Zealand */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingOFLCNZValue : u32 {
  XEX_RATING_OFLC_NZ_G = 0,          /**< General. */
  XEX_RATING_OFLC_NZ_PG = 2,         /**< Parental guidance. */
  XEX_RATING_OFLC_NZ_M = 4,          /**< Mature. */
  XEX_RATING_OFLC_NZ_MA15_PLUS = 6,  /**< Mature accompanied, age 15 and up. */
  XEX_RATING_OFLC_NZ_UNRATED = 0xFF, /**< No rating assigned. */
};
/** KMRB (Korea Media Rating Board) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingKMRBValue : u32 {
  XEX_RATING_KMRB_ALL = 0,        /**< All ages. */
  XEX_RATING_KMRB_12_PLUS = 2,    /**< Age 12 and up. */
  XEX_RATING_KMRB_15_PLUS = 4,    /**< Age 15 and up. */
  XEX_RATING_KMRB_18_PLUS = 6,    /**< Age 18 and up. */
  XEX_RATING_KMRB_UNRATED = 0xFF, /**< No rating assigned. */
};
/** Brazil ratings */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingBrazilValue : u32 {
  XEX_RATING_BRAZIL_ALL = 0,        /**< All ages. */
  XEX_RATING_BRAZIL_12_PLUS = 2,    /**< Age 12 and up. */
  XEX_RATING_BRAZIL_14_PLUS = 4,    /**< Age 14 and up. */
  XEX_RATING_BRAZIL_16_PLUS = 5,    /**< Age 16 and up. */
  XEX_RATING_BRAZIL_18_PLUS = 8,    /**< Age 18 and up. */
  XEX_RATING_BRAZIL_UNRATED = 0xFF, /**< No rating assigned. */
};
/** FPB (Film and Publication Board) */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXRatingFPBValue : u32 {
  XEX_RATING_FPB_ALL = 0,        /**< All ages. */
  XEX_RATING_FPB_PG = 6,         /**< Parental guidance. */
  XEX_RATING_FPB_10_PLUS = 7,    /**< Age 10 and up. */
  XEX_RATING_FPB_13_PLUS = 10,   /**< Age 13 and up. */
  XEX_RATING_FPB_16_PLUS = 13,   /**< Age 16 and up. */
  XEX_RATING_FPB_18_PLUS = 14,   /**< Age 18 and up. */
  XEX_RATING_FPB_UNRATED = 0xFF, /**< No rating assigned. */
};

/** Keys of the XEX optional headers. */
enum XEXHeaderKey : u32 {
  XEX_HEADER_RESOURCE_INFO = 0x000002FF,          /**< Selects XEXOptResourceInfo. */
  XEX_HEADER_FILE_FORMAT_INFO = 0x000003FF,       /**< Selects XEXOptFileFormatInfo. */
  XEX_HEADER_DELTA_PATCH_DESCRIPTOR = 0x000005FF, /**< Selects XEXOptDeltaPatchDescriptor. */
  /** Header of a base image reference. No code reads it. */
  XEX_HEADER_BASE_REFERENCE = 0x00000405,
  XEX_HEADER_BOUNDING_PATH = 0x000080FF, /**< Selects XEXOptBoundPath. No code reads it. */
  XEX_HEADER_DEVICE_ID = 0x00008105, /**< Header holding a device identifier. No code reads it. */
  /** Header holding the base address the image was linked at. No code reads it. */
  XEX_HEADER_ORIGINAL_BASE_ADDRESS = 0x00010001,
  /** Inline value holding the guest address of the entry point. */
  XEX_HEADER_ENTRY_POINT = 0x00010100,
  /** Inline value holding the guest address the image loads at. */
  XEX_HEADER_IMAGE_BASE_ADDRESS = 0x00010201,
  XEX_HEADER_IMPORT_LIBRARIES = 0x000103FF, /**< Selects XEXOptImportLibraries. */
  /** Header holding the image checksum and timestamp. No code reads it. */
  XEX_HEADER_CHECKSUM_TIMESTAMP = 0x00018002,
  /** Header marking the image for call profiling. No code reads it. */
  XEX_HEADER_ENABLED_FOR_CALLCAP = 0x00018102,
  /** Header marking the image for fast profiling. No code reads it. */
  XEX_HEADER_ENABLED_FOR_FASTCAP = 0x00018200,
  /** Selects XEXOptOriginalPEName. No code reads it. */
  XEX_HEADER_ORIGINAL_PE_NAME = 0x000183FF,
  /** Selects XEXOptStaticLibraries. No code reads it. */
  XEX_HEADER_STATIC_LIBRARIES = 0x000200FF,
  XEX_HEADER_TLS_INFO = 0x00020104, /**< Selects XEXOptTLSInfo. */
  /** Inline value holding the default thread stack size in bytes. */
  XEX_HEADER_DEFAULT_STACK_SIZE = 0x00020200,
  /** Inline value holding the default filesystem cache size. No code reads it. */
  XEX_HEADER_DEFAULT_FILESYSTEM_CACHE_SIZE = 0x00020301,
  /** Inline value holding the default heap size. No code reads it. */
  XEX_HEADER_DEFAULT_HEAP_SIZE = 0x00020401,
  /** Header holding the page heap size and flags. No code reads it. */
  XEX_HEADER_PAGE_HEAP_SIZE_AND_FLAGS = 0x00028002,
  XEX_HEADER_SYSTEM_FLAGS = 0x00030000,   /**< Inline value holding the XEXSystemFlags bits. */
  XEX_HEADER_EXECUTION_INFO = 0x00040006, /**< Selects XEXOptExecutionInfo. */
  /** Inline value holding the title workspace size. */
  XEX_HEADER_TITLE_WORKSPACE_SIZE = 0x00040201,
  XEX_HEADER_GAME_RATINGS = 0x00040310, /**< Selects XEXGameRatings. No code reads it. */
  XEX_HEADER_LAN_KEY = 0x00040404,      /**< Selects XEXOptLANKey. No code reads it. */
  XEX_HEADER_XBOX360_LOGO = 0x000405FF, /**< Header holding the logo bitmap. No code reads it. */
  /** Header listing the media ids of a multi disc title. No code reads it. */
  XEX_HEADER_MULTIDISC_MEDIA_IDS = 0x000406FF,
  /** Selects XEXOptGenericU32 holding alternate title ids. */
  XEX_HEADER_ALTERNATE_TITLE_IDS = 0x000407FF,
  /** Header holding the additional title memory request. No code reads it. */
  XEX_HEADER_ADDITIONAL_TITLE_MEMORY = 0x00040801,
  /** Selects XEXOptDataDirectory locating the named export table. */
  XEX_HEADER_EXPORTS_BY_NAME = 0x00E10402,
};

/** Encryption applied to the image data. */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXEncryptionType : u16 {
  XEX_ENCRYPTION_NONE = 0,   /**< Image data is stored unencrypted. */
  XEX_ENCRYPTION_NORMAL = 1, /**< Image data is AES encrypted with the image key. */
};

/** Compression applied to the image data. */
// NOLINTNEXTLINE(performance-enum-size): XEX file format field width
enum XEXCompressionType : u16 {
  XEX_COMPRESSION_NONE = 0, /**< Image data is stored uncompressed. */
  /** Image data is stored as the runs of XEXFileBasicCompressionInfo. */
  XEX_COMPRESSION_BASIC = 1,
  /** Image data is LZX compressed per XEXFileNormalCompressionInfo. */
  XEX_COMPRESSION_NORMAL = 2,
  XEX_COMPRESSION_DELTA = 3, /**< The image is a delta patch applied to a base image. */
};

/** Approval state of a static library. */
enum XEXApprovalType : u8 {
  /** @name Values of XEXOptStaticLibrary::approval_type. No code reads them. */
  /** @{ */
  XEX_APPROVAL_UNAPPROVED = 0,
  XEX_APPROVAL_POSSIBLE = 1,
  XEX_APPROVAL_APPROVED = 2,
  XEX_APPROVAL_EXPIRED = 3,
  /** @} */
};

/** Age rating of the title for each rating board. */
struct XEXGameRatings {
  be<XEXRatingESRBValue> esrb;     /**< ESRB rating. */
  be<XEXRatingPEGIValue> pegi;     /**< PEGI rating. */
  be<XEXRatingPEGIFIValue> pegifi; /**< PEGI rating for Finland. */
  be<XEXRatingPEGIPTValue> pegipt; /**< PEGI rating for Portugal. */
  be<XEXRatingBBFCValue> bbfc;     /**< BBFC rating. */
  be<XEXRatingCEROValue> cero;     /**< CERO rating. */
  be<XEXRatingUSKValue> usk;       /**< USK rating. */
  be<XEXRatingOFLCAUValue> oflcau; /**< OFLC rating for Australia. */
  be<XEXRatingOFLCNZValue> oflcnz; /**< OFLC rating for New Zealand. */
  be<XEXRatingKMRBValue> kmrb;     /**< KMRB rating. */
  be<XEXRatingBrazilValue> brazil; /**< Brazilian rating. */
  be<XEXRatingFPBValue> fpb;       /**< FPB rating. */
};

/** 1 run of basic compressed data followed by a run of zeros. */
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
  be<XEXEncryptionType> encryption_type;
  be<XEXCompressionType> compression_type;
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

/** 1 static library linked into the image. */
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

/** 1 resource embedded in the image. */
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

/** 1 delta patch entry. */
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

  XEXVersion target_version() const { return XEXVersion{target_version_value}; }

  void set_target_version(XEXVersion version) { target_version_value = version.value; }

  XEXVersion source_version() const { return XEXVersion{source_version_value}; }

  void set_source_version(XEXVersion version) { source_version_value = version.value; }
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

  XEXVersion version() const { return XEXVersion{version_value}; }

  void set_version(XEXVersion version) { version_value = version.value; }

  XEXVersion base_version() const { return XEXVersion{base_version_value}; }

  void set_base_version(XEXVersion version) { base_version_value = version.value; }
};
REX_STATIC_ASSERT_SIZE(XEXOptExecutionInfo, 0x18);

/** Optional header listing the libraries the image imports from. */
struct XEXOptImportLibraries {
  be_u32 size;
  struct {
    be_u32 size;
    be_u32 count;
    char data[1]; /**< string_table_size bytes. */
  } string_table;
};
static_assert(offsetof(XEXOptImportLibraries, string_table.data) == 0xC);
REX_STATIC_ASSERT_SIZE(XEXOptImportLibraries, 0x10);

/** 1 imported library with its version range and import table. */
struct XEXImportLibraryHeader {
  be_u32 size;
  char next_import_digest[0x14];
  be_u32 id;
  be_u32 version_value;
  be_u32 version_min_value;
  be_u16 name_index;
  be_u16 count;
  be_u32 import_table[1];

  XEXVersion version() const { return XEXVersion{version_value}; }

  void set_version(XEXVersion version) { version_value = version.value; }

  XEXVersion version_min() const { return XEXVersion{version_min_value}; }

  void set_version_min(XEXVersion version) { version_min_value = version.value; }
};
static_assert(offsetof(XEXImportLibraryHeader, import_table) == 0x28);
REX_STATIC_ASSERT_SIZE(XEXImportLibraryHeader, 0x2C);

/** SDK shape of an optional header that is a byte size followed by 32 bit words. */
struct XEXOptGenericU32 {
  be_u32 size;      /**< Size of this header in bytes, the size field included. */
  be_u32 values[1]; /**< First of the `count()` values. */

  u32 count() const { return (size - 4) / 4; }
};

/**
 * 1 entry of the optional header table.
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
  be<XEXModuleFlags> module_flags;
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
      XEXSectionType info : 4; /**< Bits 0 to 3 of value. Section type of the pages. */
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
  be_u32 unk_108;
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
  be_u32 modulenumber[2];
  be_u32 version[3];
  be_u32 imagebaseaddr; /**< Shifted left 16 for the true address. */
  be_u32 count;
  be_u32 base;
  /** ordOffset[0] + (imagebaseaddr << 16) is the function address. */
  be_u32 ordOffset[1];
};
static_assert(offsetof(XEXExportTable, ordOffset) == 0x2C);
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
