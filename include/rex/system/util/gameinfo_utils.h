#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <string>
#include <vector>

#include <rex/memory.h>

namespace rex {
namespace system {
namespace util {

class GameInfoWrapper {
 public:
  GameInfoWrapper(const uint8_t* data, size_t data_size);

  bool is_valid() const { return data_ != nullptr; }

 protected:
  struct GameInfoBlockHeader {
    rex::be<uint32_t> magic;
    rex::be<uint32_t> block_size;
  };
  static_assert_size(GameInfoBlockHeader, 8);

  struct GameInfoBlockExec {
    const char* virtual_titleid;
    const char* module_name;
    const char* build_description;

    const uint32_t VirtualTitleIdLength = 32;
    const uint32_t ModuleNameLength = 42;
    const uint32_t BuildDescriptionLength = 64;
  };

  struct GameInfoBlockComm {
    rex::be<uint32_t> title_id;
  };
  static_assert_size(GameInfoBlockComm, 4);

  struct GameInfoBlockTitl {
    rex::be<char16_t> title[128];
    rex::be<char16_t> description[256];
    rex::be<char16_t> publisher[256];  // assumed field name from wxPirs
  };

 private:
  const uint8_t* data_ = nullptr;
  size_t data_size_ = 0;

 protected:
  GameInfoBlockExec exec_;
  const GameInfoBlockComm* comm_ = nullptr;
  const GameInfoBlockTitl* titl_ = nullptr;
};

class GameInfo : public GameInfoWrapper {
 public:
  GameInfo(const std::vector<uint8_t>& data)
      : GameInfoWrapper(reinterpret_cast<const uint8_t*>(data.data()), data.size()) {}

  uint32_t title_id() const;
  std::string virtual_title_id() const;
  std::string module_name() const;
};

}  // namespace util
}  // namespace system
}  // namespace rex
