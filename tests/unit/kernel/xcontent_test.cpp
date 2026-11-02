/**
 * @file        tests/unit/kernel/xcontent_test.cpp
 * @brief       Content data file name bounds
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <string>

#include <rex/kernel/types/content.h>
#include <rex/memory/virtual_memory.h>

using rex::kernel::XCONTENT_DATA;
using rex::memory::AllocationType;
using rex::memory::DeallocationType;
using rex::memory::PageAccess;

REX_TEST_CASE("a full file name stops at the field end before a guard page", "[kernel][xcontent]") {
  const size_t page = rex::memory::GetPageSize();
  auto* base = static_cast<u8*>(rex::memory::AllocFixed(
      nullptr, page * 2, AllocationType::kReserveCommit, PageAccess::kReadWrite));
  REQUIRE(base != nullptr);
  REQUIRE(rex::memory::Protect(base + page, page, PageAccess::kNoAccess));

  auto* data =
      std::construct_at(reinterpret_cast<XCONTENT_DATA*>(base + page - sizeof(XCONTENT_DATA)));
  const std::string name(42, 'A');
  data->SetFileName(name);
  data->padding[0] = 'B';
  data->padding[1] = 'C';

  CHECK(data->GetFileName() == name);

  rex::memory::DeallocFixed(base, 0, DeallocationType::kRelease);
}

REX_TEST_CASE("content data locating the same package compares equal without regard to case",
              "[kernel][xcontent][case]") {
  using rex::kernel::XCONTENT_AGGREGATE_DATA;
  using rex::kernel::XContentType;
  XCONTENT_DATA left{};
  XCONTENT_DATA right{};
  left.device_id = 1;
  right.device_id = 1;
  left.content_type = rex::kernel::XContentType::kSavedGame;
  right.content_type = rex::kernel::XContentType::kSavedGame;
  left.SetFileName("Save1");
  right.SetFileName("SAVE1");
  CHECK(left == right);
  right.SetFileName("Save2");
  CHECK_FALSE(left == right);

  XCONTENT_AGGREGATE_DATA aggregate_left{};
  XCONTENT_AGGREGATE_DATA aggregate_right{};
  aggregate_left.SetFileName("Save1");
  aggregate_right.SetFileName("sAVE1");
  CHECK(aggregate_left == aggregate_right);
  aggregate_right.content_type = rex::kernel::XContentType::kMarketplaceContent;
  CHECK_FALSE(aggregate_left == aggregate_right);
}

REX_TEST_CASE("a language id outside the language range converts to no language",
              "[kernel][xcontent]") {
  using rex::kernel::ToXLanguage;
  using rex::kernel::XLanguage;
  STATIC_REQUIRE(rex::kernel::ToXLanguage(1) == rex::kernel::XLanguage::kEnglish);
  STATIC_REQUIRE(rex::kernel::ToXLanguage(12) == rex::kernel::XLanguage::kRussian);
  STATIC_REQUIRE(rex::kernel::ToXLanguage(13) == rex::kernel::XLanguage::kSwedish);
  STATIC_REQUIRE(rex::kernel::ToXLanguage(16) == rex::kernel::XLanguage::kDutch);
  STATIC_REQUIRE(rex::kernel::ToXLanguage(17) == rex::kernel::XLanguage::kSChinese);
  STATIC_REQUIRE(rex::kernel::ToXLanguage(18) == rex::kernel::XLanguage::kInvalid);
  STATIC_REQUIRE(rex::kernel::ToXLanguage(0x101) == rex::kernel::XLanguage::kInvalid);
}
