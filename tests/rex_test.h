/**
 * @file        tests/rex_test.h
 * @brief       Test case registration through noexcept initializers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <catch2/catch_test_macros.hpp>

/**
 * Defines a test function and registers it through a noexcept function. REX_TEST_CASE expands
 * to it.
 *
 * @param Name Unique name of the test function.
 * @param Register Unique name of the registration function.
 * @param ... Test name and optional tags.
 *
 * @code
 * REX_TEST_CASE_IMPL(rex_test_1, rex_test_register_1, "Adds two numbers", "[math]") {}
 * @endcode
 */
#define REX_TEST_CASE_IMPL(Name, Register, ...)                                        \
  static void Name();                                                                  \
  CATCH_INTERNAL_START_WARNINGS_SUPPRESSION                                            \
  CATCH_INTERNAL_SUPPRESS_GLOBALS_WARNINGS                                             \
  CATCH_INTERNAL_SUPPRESS_UNUSED_VARIABLE_WARNINGS                                     \
  namespace {                                                                          \
  ::Catch::AutoReg Register() noexcept {                                               \
    return ::Catch::AutoReg(::Catch::makeTestInvoker(&Name), CATCH_INTERNAL_LINEINFO,  \
                            ::Catch::StringRef(), ::Catch::NameAndTags{__VA_ARGS__});  \
  }                                                                                    \
  const ::Catch::AutoReg INTERNAL_CATCH_UNIQUE_NAME(rex_test_registrar_) = Register(); \
  }                                                                                    \
  CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION                                             \
  static void Name()

/**
 * Defines a test method on a class derived from the fixture and registers it through a noexcept
 * function. REX_TEST_CASE_METHOD expands to it.
 *
 * @param Name Unique name of the derived class.
 * @param Register Unique name of the registration function.
 * @param Fixture Class the test derives from.
 * @param ... Test name and optional tags.
 *
 * @code
 * REX_TEST_CASE_METHOD_IMPL(rex_test_1, rex_test_register_1, Fixture, "Starts", "[app]") {}
 * @endcode
 */
#define REX_TEST_CASE_METHOD_IMPL(Name, Register, Fixture, ...)                             \
  CATCH_INTERNAL_START_WARNINGS_SUPPRESSION                                                 \
  CATCH_INTERNAL_SUPPRESS_GLOBALS_WARNINGS                                                  \
  CATCH_INTERNAL_SUPPRESS_UNUSED_VARIABLE_WARNINGS                                          \
  namespace {                                                                               \
  struct Name : Fixture {                                                                   \
    void test();                                                                            \
  };                                                                                        \
  ::Catch::AutoReg Register() noexcept {                                                    \
    return ::Catch::AutoReg(::Catch::makeTestInvoker(&Name::test), CATCH_INTERNAL_LINEINFO, \
                            #Fixture##_catch_sr, ::Catch::NameAndTags{__VA_ARGS__});        \
  }                                                                                         \
  const ::Catch::AutoReg INTERNAL_CATCH_UNIQUE_NAME(rex_test_registrar_) = Register();      \
  }                                                                                         \
  CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION                                                  \
  void Name::test()

/**
 * Registers a test case.
 *
 * @param ... Test name and optional tags, as TEST_CASE takes them.
 *
 * @code
 * REX_TEST_CASE("Adds two numbers", "[math]") {
 *   CHECK(1 + 1 == 2);
 * }
 * @endcode
 */
#define REX_TEST_CASE(...)                                  \
  REX_TEST_CASE_IMPL(INTERNAL_CATCH_UNIQUE_NAME(rex_test_), \
                     INTERNAL_CATCH_UNIQUE_NAME(rex_test_register_), __VA_ARGS__)

/**
 * Registers a test case that runs as a member of a fresh fixture object.
 *
 * @param Fixture Class the test derives from.
 * @param ... Test name and optional tags, as TEST_CASE_METHOD takes them.
 *
 * @code
 * REX_TEST_CASE_METHOD(Fixture, "Starts with an empty queue", "[app]") {
 *   CHECK(queue.empty());
 * }
 * @endcode
 */
#define REX_TEST_CASE_METHOD(Fixture, ...)                         \
  REX_TEST_CASE_METHOD_IMPL(INTERNAL_CATCH_UNIQUE_NAME(rex_test_), \
                            INTERNAL_CATCH_UNIQUE_NAME(rex_test_register_), Fixture, __VA_ARGS__)
