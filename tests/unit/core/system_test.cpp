/**
 * @file        core/system_test.cpp
 * @brief       Tests for the message box title storage in rex/system.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <rex/system.h>

TEST_CASE("SetMessageBoxTitle stores the title and empty restores the default", "[system]") {
  rex::SetMessageBoxTitle("");
  CHECK(rex::GetMessageBoxTitle().empty());

  rex::SetMessageBoxTitle("ReRevved");
  CHECK(rex::GetMessageBoxTitle() == "ReRevved");

  rex::SetMessageBoxTitle("");
  CHECK(rex::GetMessageBoxTitle().empty());
}
