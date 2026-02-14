/**
 * Unit tests for SHA256 hashing utilities
 *
 * Tests SHA256 computation for strings and files.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/crypto/sha256.h>

TEST_CASE("sha256 empty string", "[sha256]") {
  auto hash = rex::crypto::sha256("");
  CHECK(hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("sha256 hello world", "[sha256]") {
  auto hash = rex::crypto::sha256("hello world");
  CHECK(hash == "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_CASE("sha256 file content", "[sha256]") {
  std::string content = "project_name = \"test\"\nxex_path = \"test.xex\"\n";
  auto hash = rex::crypto::sha256(content);
  CHECK(hash.size() == 64);
}
