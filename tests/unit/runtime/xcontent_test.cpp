/**
 * @file        tests/unit/runtime/xcontent_test.cpp
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

#include <rex/memory/virtual_memory.h>
#include <rex/runtime/xcontent_data.h>

using rex::memory::AllocationType;
using rex::memory::DeallocationType;
using rex::memory::PageAccess;
using rex::runtime::XCONTENT_DATA;

REX_TEST_CASE("a full file name stops at the field end before a guard page",
              "[runtime][xcontent]") {
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
              "[runtime][xcontent][case]") {
  using rex::runtime::XCONTENT_AGGREGATE_DATA;
  using rex::runtime::XContentType;
  XCONTENT_DATA left{};
  XCONTENT_DATA right{};
  left.device_id = 1;
  right.device_id = 1;
  left.content_type = XContentType::kSavedGame;
  right.content_type = XContentType::kSavedGame;
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
  aggregate_right.content_type = XContentType::kMarketplaceContent;
  CHECK_FALSE(aggregate_left == aggregate_right);
}

REX_TEST_CASE("a language id outside the language range converts to no language",
              "[runtime][xcontent]") {
  using rex::runtime::ToXLanguage;
  using rex::runtime::XLanguage;
  STATIC_REQUIRE(ToXLanguage(1) == XLanguage::kEnglish);
  STATIC_REQUIRE(ToXLanguage(12) == XLanguage::kRussian);
  STATIC_REQUIRE(ToXLanguage(13) == XLanguage::kInvalid);
  STATIC_REQUIRE(ToXLanguage(0x101) == XLanguage::kInvalid);
}
