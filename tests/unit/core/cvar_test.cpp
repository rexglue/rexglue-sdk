/**
 * @file        cvar_test.cpp
 * @brief       Unit tests for the cvar system
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <filesystem>
#include <fstream>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>

// Test cvars
REXCVAR_DEFINE_BOOL(test_bool_flag, false, "Test", "Test boolean flag");
REXCVAR_DEFINE_INT32(test_int32_flag, 42, "Test", "Test int32 flag");
REXCVAR_DEFINE_STRING(test_string_flag, "default", "Test", "Test string flag");
REXCVAR_DEFINE_DOUBLE(test_double_flag, 3.14, "Test", "Test double flag");
REXCVAR_DEFINE_INT32(test_ranged_flag, 5, "Test", "Test ranged flag").range(1, 10);
REXCVAR_DEFINE_STRING(test_enum_flag, "low", "Test", "Test enum-like flag")
    .allowed({"low", "medium", "high"});
REXCVAR_DEFINE_STRING(test_init_only_flag, "initial", "Test", "Init-only flag")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(test_restart_flag, false, "Test", "Requires restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

TEST_CASE("cvar registry stores flag metadata", "[cvar]") {
  auto flags = rex::cvar::ListFlags();

  SECTION("Flags are registered") {
    REQUIRE(flags.size() > 0);

    bool found_bool = false;
    bool found_int32 = false;
    bool found_string = false;

    for (const auto& name : flags) {
      if (name == "test_bool_flag")
        found_bool = true;
      if (name == "test_int32_flag")
        found_int32 = true;
      if (name == "test_string_flag")
        found_string = true;
    }

    CHECK(found_bool);
    CHECK(found_int32);
    CHECK(found_string);
  }
}

TEST_CASE("cvar REXCVAR_SET and REXCVAR_GET macros", "[cvar]") {
  SECTION("Set and get boolean flag") {
    REXCVAR_SET(test_bool_flag, true);
    CHECK(REXCVAR_GET(test_bool_flag) == true);

    REXCVAR_SET(test_bool_flag, false);
    CHECK(REXCVAR_GET(test_bool_flag) == false);
  }

  SECTION("Set and get int32 flag") {
    REXCVAR_SET(test_int32_flag, 123);
    CHECK(REXCVAR_GET(test_int32_flag) == 123);
  }

  SECTION("Set and get string flag") {
    REXCVAR_SET(test_string_flag, "hello world");
    CHECK(REXCVAR_GET(test_string_flag) == "hello world");
  }

  SECTION("Set and get double flag") {
    REXCVAR_SET(test_double_flag, 2.718);
    CHECK(REXCVAR_GET(test_double_flag) == Catch::Approx(2.718));
  }
}

TEST_CASE("cvar SetFlagByName and GetFlagByName string API", "[cvar]") {
  SECTION("Set and get by name with string conversion") {
    REQUIRE(rex::cvar::SetFlagByName("test_bool_flag", "true"));
    CHECK(rex::cvar::GetFlagByName("test_bool_flag") == "true");

    REQUIRE(rex::cvar::SetFlagByName("test_int32_flag", "456"));
    CHECK(rex::cvar::GetFlagByName("test_int32_flag") == "456");

    REQUIRE(rex::cvar::SetFlagByName("test_string_flag", "test value"));
    CHECK(rex::cvar::GetFlagByName("test_string_flag") == "test value");
  }

  SECTION("Unknown flag returns false") {
    CHECK_FALSE(rex::cvar::SetFlagByName("nonexistent_flag", "value"));
    CHECK(rex::cvar::GetFlagByName("nonexistent_flag").empty());
  }
}

TEST_CASE("cvar boolean parsing accepts multiple formats", "[cvar]") {
  // This test specifically tests the string-based SetFlagByName API
  // which parses string representations into native types
  SECTION("true values") {
    rex::cvar::SetFlagByName("test_bool_flag", "true");
    CHECK(REXCVAR_GET(test_bool_flag) == true);

    rex::cvar::SetFlagByName("test_bool_flag", "1");
    CHECK(REXCVAR_GET(test_bool_flag) == true);

    rex::cvar::SetFlagByName("test_bool_flag", "yes");
    CHECK(REXCVAR_GET(test_bool_flag) == true);
  }

  SECTION("false values") {
    rex::cvar::SetFlagByName("test_bool_flag", "false");
    CHECK(REXCVAR_GET(test_bool_flag) == false);

    rex::cvar::SetFlagByName("test_bool_flag", "0");
    CHECK(REXCVAR_GET(test_bool_flag) == false);

    rex::cvar::SetFlagByName("test_bool_flag", "no");
    CHECK(REXCVAR_GET(test_bool_flag) == false);
  }
}

TEST_CASE("cvar ListFlagsByCategory", "[cvar]") {
  auto test_flags = rex::cvar::ListFlagsByCategory("Test");

  CHECK(test_flags.size() >= 4);

  bool found = false;
  for (const auto& name : test_flags) {
    if (name == "test_bool_flag") {
      found = true;
      break;
    }
  }
  CHECK(found);
}

TEST_CASE("cvar GetFlagInfo returns metadata", "[cvar]") {
  auto* info = rex::cvar::GetFlagInfo("test_int32_flag");

  REQUIRE(info != nullptr);
  CHECK(info->name == "test_int32_flag");
  CHECK(info->type == rex::cvar::FlagType::Int32);
  CHECK(info->category == "Test");
  CHECK(info->default_value == "42");
  CHECK(info->lifecycle == rex::cvar::Lifecycle::kHotReload);

  SECTION("Unknown flag returns nullptr") {
    CHECK(rex::cvar::GetFlagInfo("nonexistent") == nullptr);
  }
}

TEST_CASE("cvar TOML config loading", "[cvar]") {
  auto temp_dir = std::filesystem::temp_directory_path();
  auto config_path = temp_dir / "test_config.toml";

  SECTION("Load flat config") {
    std::ofstream file(config_path);
    file << "test_bool_flag = true\n";
    file << "test_int32_flag = 999\n";
    file << "test_string_flag = \"from config\"\n";
    file.close();

    // Set initial values using REXCVAR_SET
    REXCVAR_SET(test_bool_flag, false);
    REXCVAR_SET(test_int32_flag, 0);
    REXCVAR_SET(test_string_flag, "");

    rex::cvar::LoadConfig(config_path);

    CHECK(REXCVAR_GET(test_bool_flag) == true);
    CHECK(REXCVAR_GET(test_int32_flag) == 999);
    CHECK(REXCVAR_GET(test_string_flag) == "from config");

    std::filesystem::remove(config_path);
  }

  SECTION("Nonexistent config is handled gracefully") {
    auto missing = temp_dir / "nonexistent.toml";
    rex::cvar::LoadConfig(missing);
  }
}

TEST_CASE("cvar range validation", "[cvar]") {
  SECTION("Value within range succeeds") {
    REQUIRE(rex::cvar::SetFlagByName("test_ranged_flag", "5"));
    CHECK(REXCVAR_GET(test_ranged_flag) == 5);

    REQUIRE(rex::cvar::SetFlagByName("test_ranged_flag", "1"));
    CHECK(REXCVAR_GET(test_ranged_flag) == 1);

    REQUIRE(rex::cvar::SetFlagByName("test_ranged_flag", "10"));
    CHECK(REXCVAR_GET(test_ranged_flag) == 10);
  }

  SECTION("Value outside range fails") {
    REXCVAR_SET(test_ranged_flag, 5);  // Reset to known value

    CHECK_FALSE(rex::cvar::SetFlagByName("test_ranged_flag", "0"));
    CHECK(REXCVAR_GET(test_ranged_flag) == 5);  // Unchanged

    CHECK_FALSE(rex::cvar::SetFlagByName("test_ranged_flag", "11"));
    CHECK(REXCVAR_GET(test_ranged_flag) == 5);  // Unchanged
  }
}

TEST_CASE("cvar allowed values validation", "[cvar]") {
  SECTION("Allowed value succeeds") {
    REQUIRE(rex::cvar::SetFlagByName("test_enum_flag", "low"));
    CHECK(REXCVAR_GET(test_enum_flag) == "low");

    REQUIRE(rex::cvar::SetFlagByName("test_enum_flag", "medium"));
    CHECK(REXCVAR_GET(test_enum_flag) == "medium");

    REQUIRE(rex::cvar::SetFlagByName("test_enum_flag", "high"));
    CHECK(REXCVAR_GET(test_enum_flag) == "high");
  }

  SECTION("Disallowed value fails") {
    REXCVAR_SET(test_enum_flag, "low");  // Reset

    CHECK_FALSE(rex::cvar::SetFlagByName("test_enum_flag", "ultra"));
    CHECK(REXCVAR_GET(test_enum_flag) == "low");  // Unchanged
  }
}

TEST_CASE("cvar lifecycle enforcement", "[cvar]") {
  // Note: This test must run before FinalizeInit is called elsewhere,
  // or use testing utilities to reset state

  SECTION("InitOnly flag can be set before finalization") {
    // Assuming not finalized yet in test context
    if (!rex::cvar::IsFinalized()) {
      REQUIRE(rex::cvar::SetFlagByName("test_init_only_flag", "modified"));
      CHECK(REXCVAR_GET(test_init_only_flag) == "modified");
    }
  }
}

TEST_CASE("cvar restart tracking", "[cvar]") {
  // Clear any existing pending flags
  rex::cvar::ClearPendingRestartFlags();

  SECTION("Changing RequiresRestart flag tracks it") {
    auto pending = rex::cvar::GetPendingRestartFlags();
    CHECK(pending.empty());

    REQUIRE(rex::cvar::SetFlagByName("test_restart_flag", "true"));

    pending = rex::cvar::GetPendingRestartFlags();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0] == "test_restart_flag");
  }

  SECTION("ClearPendingRestartFlags clears the list") {
    rex::cvar::SetFlagByName("test_restart_flag", "true");
    CHECK_FALSE(rex::cvar::GetPendingRestartFlags().empty());

    rex::cvar::ClearPendingRestartFlags();
    CHECK(rex::cvar::GetPendingRestartFlags().empty());
  }
}

TEST_CASE("cvar ListFlagsByLifecycle", "[cvar]") {
  auto init_only = rex::cvar::ListFlagsByLifecycle(rex::cvar::Lifecycle::kInitOnly);
  auto restart = rex::cvar::ListFlagsByLifecycle(rex::cvar::Lifecycle::kRequiresRestart);

  bool found_init =
      std::find(init_only.begin(), init_only.end(), "test_init_only_flag") != init_only.end();
  bool found_restart =
      std::find(restart.begin(), restart.end(), "test_restart_flag") != restart.end();

  CHECK(found_init);
  CHECK(found_restart);
}

TEST_CASE("cvar reset and diff utilities", "[cvar]") {
  SECTION("HasNonDefaultValue detects changes") {
    REXCVAR_SET(test_int32_flag, 42);  // Reset to default
    CHECK_FALSE(rex::cvar::HasNonDefaultValue("test_int32_flag"));

    REXCVAR_SET(test_int32_flag, 100);
    CHECK(rex::cvar::HasNonDefaultValue("test_int32_flag"));
  }

  SECTION("ResetToDefault restores default value") {
    REXCVAR_SET(test_int32_flag, 100);
    CHECK(REXCVAR_GET(test_int32_flag) == 100);

    rex::cvar::ResetToDefault("test_int32_flag");
    CHECK(REXCVAR_GET(test_int32_flag) == 42);
  }

  SECTION("ListModifiedFlags returns changed flags") {
    REXCVAR_SET(test_int32_flag, 42);    // Default
    REXCVAR_SET(test_bool_flag, false);  // Default

    auto modified = rex::cvar::ListModifiedFlags();
    bool found_int =
        std::find(modified.begin(), modified.end(), "test_int32_flag") != modified.end();
    CHECK_FALSE(found_int);

    REXCVAR_SET(test_int32_flag, 999);
    modified = rex::cvar::ListModifiedFlags();
    found_int = std::find(modified.begin(), modified.end(), "test_int32_flag") != modified.end();
    CHECK(found_int);
  }
}

TEST_CASE("cvar testing utilities", "[cvar]") {
  SECTION("ResetAllForTesting resets state") {
    REXCVAR_SET(test_int32_flag, 999);
    rex::cvar::testing::ResetAllForTesting();
    CHECK(REXCVAR_GET(test_int32_flag) == 42);  // Back to default
  }
}

TEST_CASE("cvar TOML serialization", "[cvar]") {
  rex::cvar::testing::ResetAllForTesting();

  REXCVAR_SET(test_int32_flag, 999);
  REXCVAR_SET(test_string_flag, "custom");

  auto toml = rex::cvar::SerializeToTOML();

  // Should contain modified flags
  CHECK(toml.find("test_int32_flag = 999") != std::string::npos);
  CHECK(toml.find("test_string_flag = \"custom\"") != std::string::npos);

  // Should not contain flags at default
  CHECK(toml.find("test_bool_flag") == std::string::npos);
}

TEST_CASE("cvar metadata integration test", "[cvar][integration]") {
  rex::cvar::testing::ResetAllForTesting();

  SECTION("Full metadata workflow") {
    // 1. Verify metadata is queryable
    auto* info = rex::cvar::GetFlagInfo("test_ranged_flag");
    REQUIRE(info != nullptr);
    CHECK(info->constraints.min.value_or(0) == 1);
    CHECK(info->constraints.max.value_or(0) == 10);

    // 2. Verify validation works (set to valid non-default value, then invalid)
    CHECK(rex::cvar::SetFlagByName("test_ranged_flag", "7"));
    CHECK_FALSE(rex::cvar::SetFlagByName("test_ranged_flag", "100"));

    // 3. Verify change tracking (value 7 is different from default 5)
    CHECK(rex::cvar::HasNonDefaultValue("test_ranged_flag"));

    // 4. Verify reset works
    rex::cvar::ResetToDefault("test_ranged_flag");
    CHECK_FALSE(rex::cvar::HasNonDefaultValue("test_ranged_flag"));
  }
}

// Additional test cvars for extended coverage
REXCVAR_DEFINE_INT64(test_int64_flag, 1000000000LL, "Test", "Test int64 flag");
REXCVAR_DEFINE_UINT32(test_uint32_flag, 42u, "Test", "Test uint32 flag");
REXCVAR_DEFINE_UINT64(test_uint64_flag, 999999999999ULL, "Test", "Test uint64 flag");
REXCVAR_DEFINE_STRING(test_validated_flag, "valid", "Test", "Custom validated flag")
    .validator([](std::string_view v) { return v.size() >= 3; });
REXCVAR_DEFINE_BOOL(test_debug_flag, false, "Test", "Debug-only flag").debug_only();
REXCVAR_DEFINE_STRING(test_category_flag, "value", "TestCategory", "For category filter test");

TEST_CASE("cvar INT64/UINT32/UINT64 types", "[cvar]") {
  SECTION("INT64 get/set") {
    REXCVAR_SET(test_int64_flag, 9876543210LL);
    CHECK(REXCVAR_GET(test_int64_flag) == 9876543210LL);

    REQUIRE(rex::cvar::SetFlagByName("test_int64_flag", "-123456789"));
    CHECK(REXCVAR_GET(test_int64_flag) == -123456789LL);
  }

  SECTION("UINT32 get/set") {
    REXCVAR_SET(test_uint32_flag, 4000000000u);
    CHECK(REXCVAR_GET(test_uint32_flag) == 4000000000u);

    REQUIRE(rex::cvar::SetFlagByName("test_uint32_flag", "123"));
    CHECK(REXCVAR_GET(test_uint32_flag) == 123u);
  }

  SECTION("UINT64 get/set") {
    REXCVAR_SET(test_uint64_flag, 18446744073709551000ULL);
    CHECK(REXCVAR_GET(test_uint64_flag) == 18446744073709551000ULL);

    REQUIRE(rex::cvar::SetFlagByName("test_uint64_flag", "999"));
    CHECK(REXCVAR_GET(test_uint64_flag) == 999ULL);
  }
}

TEST_CASE("cvar custom validator", "[cvar]") {
  SECTION("Valid value passes") {
    REQUIRE(rex::cvar::SetFlagByName("test_validated_flag", "abc"));
    CHECK(REXCVAR_GET(test_validated_flag) == "abc");

    REQUIRE(rex::cvar::SetFlagByName("test_validated_flag", "long_string"));
    CHECK(REXCVAR_GET(test_validated_flag) == "long_string");
  }

  SECTION("Invalid value fails") {
    REXCVAR_SET(test_validated_flag, "valid");

    // Too short (less than 3 chars)
    CHECK_FALSE(rex::cvar::SetFlagByName("test_validated_flag", "ab"));
    CHECK(REXCVAR_GET(test_validated_flag) == "valid");  // Unchanged

    CHECK_FALSE(rex::cvar::SetFlagByName("test_validated_flag", "x"));
    CHECK(REXCVAR_GET(test_validated_flag) == "valid");  // Unchanged
  }
}

TEST_CASE("cvar debug_only flag", "[cvar]") {
  auto* info = rex::cvar::GetFlagInfo("test_debug_flag");
  REQUIRE(info != nullptr);
  CHECK(info->is_debug_only == true);

  // Non-debug flag should have is_debug_only = false
  auto* non_debug = rex::cvar::GetFlagInfo("test_bool_flag");
  REQUIRE(non_debug != nullptr);
  CHECK(non_debug->is_debug_only == false);
}

TEST_CASE("cvar ScopedLifecycleOverride", "[cvar]") {
  rex::cvar::testing::ResetAllForTesting();

  // Finalize to lock init-only flags
  rex::cvar::FinalizeInit();
  CHECK(rex::cvar::IsFinalized());

  SECTION("Init-only blocked after finalization") {
    CHECK_FALSE(rex::cvar::SetFlagByName("test_init_only_flag", "blocked"));
  }

  SECTION("ScopedLifecycleOverride allows modification") {
    {
      rex::cvar::testing::ScopedLifecycleOverride override;
      REQUIRE(rex::cvar::SetFlagByName("test_init_only_flag", "overridden"));
      CHECK(REXCVAR_GET(test_init_only_flag) == "overridden");
    }
    // After scope, should be blocked again
    CHECK_FALSE(rex::cvar::SetFlagByName("test_init_only_flag", "blocked_again"));
  }

  rex::cvar::testing::ResetAllForTesting();
}

TEST_CASE("cvar ResetAllToDefaults", "[cvar]") {
  // Modify several flags
  REXCVAR_SET(test_bool_flag, true);
  REXCVAR_SET(test_int32_flag, 999);
  REXCVAR_SET(test_string_flag, "modified");

  CHECK(REXCVAR_GET(test_bool_flag) == true);
  CHECK(REXCVAR_GET(test_int32_flag) == 999);
  CHECK(REXCVAR_GET(test_string_flag) == "modified");

  rex::cvar::ResetAllToDefaults();

  CHECK(REXCVAR_GET(test_bool_flag) == false);
  CHECK(REXCVAR_GET(test_int32_flag) == 42);
  CHECK(REXCVAR_GET(test_string_flag) == "default");
}

TEST_CASE("cvar SerializeToTOML with category filter", "[cvar]") {
  rex::cvar::testing::ResetAllForTesting();

  // Modify flags in different categories
  REXCVAR_SET(test_int32_flag, 123);           // Category: Test
  REXCVAR_SET(test_category_flag, "changed");  // Category: TestCategory

  SECTION("Filter by category returns only that category") {
    auto test_toml = rex::cvar::SerializeToTOML("Test");
    auto category_toml = rex::cvar::SerializeToTOML("TestCategory");

    CHECK(test_toml.find("test_int32_flag") != std::string::npos);
    CHECK(test_toml.find("test_category_flag") == std::string::npos);

    CHECK(category_toml.find("test_category_flag") != std::string::npos);
    CHECK(category_toml.find("test_int32_flag") == std::string::npos);
  }
}

TEST_CASE("cvar SaveConfig", "[cvar]") {
  rex::cvar::testing::ResetAllForTesting();

  auto temp_dir = std::filesystem::temp_directory_path();
  auto save_path = temp_dir / "test_save_config.toml";

  // Clean up any existing file
  std::filesystem::remove(save_path);

  SECTION("SaveConfig writes modified flags to file") {
    REXCVAR_SET(test_int32_flag, 777);
    REXCVAR_SET(test_string_flag, "saved_value");

    rex::cvar::SaveConfig(save_path);

    // Verify file exists and contains expected content
    REQUIRE(std::filesystem::exists(save_path));

    std::string content;
    {
      std::ifstream file(save_path);
      content =
          std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    CHECK(content.find("test_int32_flag = 777") != std::string::npos);
    CHECK(content.find("test_string_flag = \"saved_value\"") != std::string::npos);

    std::filesystem::remove(save_path);
  }

  SECTION("SaveConfig with no modifications creates no file or empty") {
    rex::cvar::testing::ResetAllForTesting();
    rex::cvar::SaveConfig(save_path);
    // Either file doesn't exist or is minimal (just header comment)
  }
}

TEST_CASE("cvar ApplyEnvironment", "[cvar]") {
  rex::cvar::testing::ResetAllForTesting();

  // Note: This test modifies the environment, which may affect other tests
  // In practice, environment application happens once at startup

  SECTION("Environment variables are applied with REX_ prefix") {
// Set environment variable
#ifdef _WIN32
    _putenv_s("REX_TEST_INT32_FLAG", "12345");
#else
    setenv("REX_TEST_INT32_FLAG", "12345", 1);
#endif

    rex::cvar::ApplyEnvironment();

    CHECK(REXCVAR_GET(test_int32_flag) == 12345);

// Clean up
#ifdef _WIN32
    _putenv_s("REX_TEST_INT32_FLAG", "");
#else
    unsetenv("REX_TEST_INT32_FLAG");
#endif
  }
}
