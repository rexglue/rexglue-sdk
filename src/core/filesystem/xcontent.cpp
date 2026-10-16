#include <rex/filesystem/xcontent.h>

#include <rex/math.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

std::u16string ReadBigEndian(const be_u16* units, size_t count) {
  std::u16string result;
  for (size_t i = 0; i < count && units[i] != 0; ++i) {
    result.push_back(static_cast<char16_t>(static_cast<u16>(units[i])));
  }
  return result;
}

u32 LanguageSlot(XLanguage language) {
  const u32 slot = static_cast<u32>(language) - 1;
  if (slot >= XContentMetadata::kNumLanguagesV2) {
    return std::to_underlying(XLanguage::kEnglish) - 1;
  }
  return slot;
}

}  // namespace

std::u16string XContentMetadata::GetDisplayName(XLanguage language) const {
  const u32 slot = LanguageSlot(language);
  if (slot < kNumLanguagesV1) {
    return ReadBigEndian(display_name_raw.uint[slot], 128);
  }
  if (metadata_version >= 2) {
    return ReadBigEndian(display_name_ex_raw.uint[slot - kNumLanguagesV1], 128);
  }
  return {};
}

std::u16string XContentMetadata::GetDescription(XLanguage language) const {
  const u32 slot = LanguageSlot(language);
  if (slot < kNumLanguagesV1) {
    return ReadBigEndian(description_raw.uint[slot], 128);
  }
  if (metadata_version >= 2) {
    return ReadBigEndian(description_ex_raw.uint[slot - kNumLanguagesV1], 128);
  }
  return {};
}

std::u16string XContentMetadata::GetPublisher() const {
  return ReadBigEndian(publisher_raw.uint, 64);
}

std::u16string XContentMetadata::GetTitleName() const {
  return ReadBigEndian(title_name_raw.uint, 64);
}

bool XContentMetadata::SetDisplayName(XLanguage language, std::u16string_view value) {
  const u32 slot = LanguageSlot(language);
  char16_t* destination = nullptr;
  if (slot < kNumLanguagesV1) {
    destination = display_name_raw.chars[slot];
  } else if (metadata_version >= 2) {
    destination = display_name_ex_raw.chars[slot - kNumLanguagesV1];
  }
  if (!destination) {
    return false;
  }
  copy_and_swap_truncating(destination, value, CountOf(display_name_raw.chars[0]));
  return true;
}

bool XContentMetadata::SetDescription(XLanguage language, std::u16string_view value) {
  const u32 slot = LanguageSlot(language);
  char16_t* destination = nullptr;
  if (slot < kNumLanguagesV1) {
    destination = description_raw.chars[slot];
  } else if (metadata_version >= 2) {
    destination = description_ex_raw.chars[slot - kNumLanguagesV1];
  }
  if (!destination) {
    return false;
  }
  copy_and_swap_truncating(destination, value, CountOf(description_raw.chars[0]));
  return true;
}

void XContentMetadata::SetPublisher(std::u16string_view value) {
  copy_and_swap_truncating(publisher_raw.chars, value, CountOf(publisher_raw.chars));
}

void XContentMetadata::SetTitleName(std::u16string_view value) {
  copy_and_swap_truncating(title_name_raw.chars, value, CountOf(title_name_raw.chars));
}

}  // namespace rex::filesystem
