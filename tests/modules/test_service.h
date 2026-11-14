#pragma once

#include <rex/module.h>

namespace rex::test {

REX_DECLARE_INTERFACE(ITestService, 1)
  virtual int GetValue() const = 0;
};

}  // namespace rex::test
