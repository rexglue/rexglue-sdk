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

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/reset_scope.h>
#include <rex/string.h>
#include <rex/system/compiler/compiler_passes.h>
#include <rex/system/processor.h>
#include <rex/system/test_module.h>

namespace rex::runtime {

using rex::runtime::backend::Backend;
using rex::runtime::compiler::Compiler;
using rex::runtime::hir::HIRBuilder;
namespace passes = rex::runtime::compiler::passes;

TestModule::TestModule(Processor* processor, const std::string_view name,
                       std::function<bool(uint32_t)> contains_address,
                       std::function<bool(hir::HIRBuilder&)> generate)
    : Module(processor), name_(name), contains_address_(contains_address), generate_(generate) {
  builder_.reset(new HIRBuilder());
  compiler_.reset(new Compiler(processor));
  assembler_ = processor->backend()->CreateAssembler();
  assembler_->Initialize();

  // Merge blocks early. This will let us use more context in other passes.
  // The CFG is required for simplification and dirtied by it.
  compiler_->AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler_->AddPass(std::make_unique<passes::ControlFlowSimplificationPass>());
  compiler_->AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());

  // Passes are executed in the order they are added. Multiple of the same
  // pass type may be used.
  compiler_->AddPass(std::make_unique<passes::ContextPromotionPass>());
  compiler_->AddPass(std::make_unique<passes::SimplificationPass>());
  compiler_->AddPass(std::make_unique<passes::ConstantPropagationPass>());
  compiler_->AddPass(std::make_unique<passes::SimplificationPass>());
  // compiler_->AddPass(std::make_unique<passes::DeadStoreEliminationPass>());
  compiler_->AddPass(std::make_unique<passes::DeadCodeEliminationPass>());

  //// Removes all unneeded variables. Try not to add new ones after this.
  // compiler_->AddPass(new passes::ValueReductionPass());

  // Register allocation for the target backend.
  // Will modify the HIR to add loads/stores.
  // This should be the last pass before finalization, as after this all
  // registers are assigned and ready to be emitted.
  compiler_->AddPass(
      std::make_unique<passes::RegisterAllocationPass>(processor->backend()->machine_info()));

  // Must come last. The HIR is not really HIR after this.
  compiler_->AddPass(std::make_unique<passes::FinalizationPass>());
}

TestModule::~TestModule() = default;

bool TestModule::ContainsAddress(uint32_t address) {
  return contains_address_(address);
}

std::unique_ptr<Function> TestModule::CreateFunction(uint32_t address) {
  return std::unique_ptr<Function>(processor_->backend()->CreateGuestFunction(this, address));
}

Symbol::Status TestModule::DeclareFunction(uint32_t address, Function** out_function) {
  Symbol::Status status = Module::DeclareFunction(address, out_function);
  if (status == Symbol::Status::kNew) {
    auto function = static_cast<GuestFunction*>(*out_function);

    // Reset() all caching when we leave.
    rex::make_reset_scope(compiler_);
    rex::make_reset_scope(assembler_);

    if (!generate_(*builder_.get())) {
      function->set_status(Symbol::Status::kFailed);
      return Symbol::Status::kFailed;
    }

    // Run optimization passes.
    compiler_->Compile(builder_.get());

    // Assemble the function.
    assembler_->Assemble(function, builder_.get(), 0, nullptr);

    status = Symbol::Status::kDefined;
    function->set_status(status);
  }
  return status;
}

}  // namespace rex::runtime
// (removed orphan xe namespace)
