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

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <rex/memory/utils.h>
#include <rex/runtime/xcontent.h>

using rex::memory::AllocationType;
using rex::memory::DeallocationType;
using rex::memory::PageAccess;
using rex::runtime::xam::XCONTENT_DATA;

TEST_CASE("a full file name stops at the field end before a guard page", "[xcontent]") {
  const size_t page = rex::memory::page_size();
  auto* base = static_cast<u8*>(rex::memory::AllocFixed(
      nullptr, page * 2, AllocationType::kReserveCommit, PageAccess::kReadWrite));
  REQUIRE(base != nullptr);
  REQUIRE(rex::memory::Protect(base + page, page, PageAccess::kNoAccess));

  auto* data =
      std::construct_at(reinterpret_cast<XCONTENT_DATA*>(base + page - sizeof(XCONTENT_DATA)));
  const std::string name(42, 'A');
  data->set_file_name(name);
  data->padding[0] = 'B';
  data->padding[1] = 'C';

  CHECK(data->file_name() == name);

  rex::memory::DeallocFixed(base, 0, DeallocationType::kRelease);
}

TEST_CASE("a language id outside the language range converts to no language", "[xcontent]") {
  using rex::runtime::ToXLanguage;
  using rex::runtime::XLanguage;
  STATIC_REQUIRE(ToXLanguage(1) == XLanguage::kEnglish);
  STATIC_REQUIRE(ToXLanguage(12) == XLanguage::kRussian);
  STATIC_REQUIRE(ToXLanguage(13) == XLanguage::kInvalid);
  STATIC_REQUIRE(ToXLanguage(0x101) == XLanguage::kInvalid);
}
