#include "decoded_binary.h"

#include <format>

#include "codegen_context.h"
#include <rex/logging/log.h>
#include "codegen_categories.h"

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/module.h>
#include <rex/kernel/object/object_table.h>

namespace rex::codegen {

Result<CodegenContext> CodegenContext::Create(const std::filesystem::path& config_path,
                                              rex::kernel::TitleRuntime& runtime) {
  CodegenContext ctx;

  if (!ctx.config_.Load(config_path.string())) {
    return Err<CodegenContext>(ErrorCategory::kConfig,
                               std::format("Failed to load config: {}", config_path.string()));
  }
  ctx.config_dir_ = config_path.parent_path();

  std::filesystem::path xex_path = ctx.config_dir_ / ctx.config_.file_path;

  if (std::filesystem::exists(xex_path)) {
    xex_path = std::filesystem::canonical(xex_path);
  } else {
    return Err<CodegenContext>(ErrorCategory::kNotFound,
                               std::format("XEX file not found: {}", xex_path.string()));
  }

  auto user_module = runtime.GetObjectTable().Get<rex::kernel::ModuleObject>(
      runtime.GetModuleRegistry().GetExecutable().value_or(0));
  if (!user_module) {
    return Err<CodegenContext>(ErrorCategory::kFormat,
                               "Failed to get executable module after loading");
  }

  auto* module = user_module->GetXEX();
  if (!module) {
    return Err<CodegenContext>(ErrorCategory::kFormat,
                               "Failed to get XEXModule from the executable module");
  }

  ctx.binary_ = BinaryView::FromModule(*module);

  REXLOG_TRACE(codegen, "Loaded XEX: base=0x{:08X}, size=0x{:X}, entry=0x{:08X}",
               ctx.binary_.GetBaseAddress(), ctx.binary_.GetImageSize(),
               ctx.binary_.GetEntryPoint());

  ctx.analysis_state_.format = "xex";
  ctx.analysis_state_.load_address = ctx.binary_.GetBaseAddress();
  ctx.analysis_state_.entry_point = ctx.binary_.GetEntryPoint();
  ctx.analysis_state_.image_size = ctx.binary_.GetImageSize();

  return ctx;
}

CodegenContext CodegenContext::Create(BinaryView binary, RecompilerConfig config) {
  CodegenContext ctx;
  ctx.binary_ = std::move(binary);
  ctx.config_ = std::move(config);
  return ctx;
}

CodegenContext::~CodegenContext() = default;
CodegenContext::CodegenContext(CodegenContext&&) noexcept(false) = default;
CodegenContext& CodegenContext::operator=(CodegenContext&&) noexcept(false) = default;

DecodedBinary& CodegenContext::GetDecoded() {
  assert(decoded_ && "decoded_ is null until InitDecoded runs");
  return *decoded_;
}

const DecodedBinary& CodegenContext::GetDecoded() const {
  assert(decoded_ && "decoded_ is null until InitDecoded runs");
  return *decoded_;
}

void CodegenContext::InitDecoded() {
  decoded_ = std::make_unique<DecodedBinary>(binary_);
  decoded_->Decode();
}

}  // namespace rex::codegen
