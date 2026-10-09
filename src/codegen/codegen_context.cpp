#include "decoded_binary.h"

#include <format>

#include <rex/codegen/codegen_context.h>
#include <rex/logging.h>

#include <rex/runtime.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/xex_module.h>

namespace rex::codegen {

Result<CodegenContext> CodegenContext::Create(const std::filesystem::path& configPath,
                                              Runtime& runtime) {
  CodegenContext ctx;

  if (!ctx.config_.Load(configPath.string())) {
    return Err<CodegenContext>(ErrorCategory::Config,
                               std::format("Failed to load config: {}", configPath.string()));
  }
  ctx.configDir_ = configPath.parent_path();

  std::filesystem::path xexPath = ctx.configDir_ / ctx.config_.filePath;

  if (std::filesystem::exists(xexPath)) {
    xexPath = std::filesystem::canonical(xexPath);
  } else {
    return Err<CodegenContext>(ErrorCategory::NotFound,
                               std::format("XEX file not found: {}", xexPath.string()));
  }

  auto xexFilename = xexPath.filename();
  auto vfsPath = "game:\\" + xexFilename.string();
  if (auto loaded = runtime.LoadXEXImage(vfsPath); !loaded) {
    return Err<CodegenContext>(
        ErrorCategory::Format,
        std::format("Failed to load XEX: {}, {}", xexPath.string(), loaded.error().message));
  }

  auto user_module = runtime.object_table().Get<rex::runtime::ModuleObject>(
      runtime.module_registry().executable().value_or(0));
  if (!user_module) {
    return Err<CodegenContext>(ErrorCategory::Format,
                               "Failed to get executable module after loading");
  }

  auto* module = rex::runtime::UserModuleXEX(*user_module);
  if (!module) {
    return Err<CodegenContext>(ErrorCategory::Format,
                               "Failed to get XexModule from the executable module");
  }

  ctx.binary_ = BinaryView::fromModule(*module);

  REXLOG_TRACE(codegen, "Loaded XEX: base=0x{:08X}, size=0x{:X}, entry=0x{:08X}",
               ctx.binary_.baseAddress(), ctx.binary_.imageSize(), ctx.binary_.entryPoint());

  ctx.analysisState_.format = "xex";
  ctx.analysisState_.loadAddress = ctx.binary_.baseAddress();
  ctx.analysisState_.entryPoint = ctx.binary_.entryPoint();
  ctx.analysisState_.imageSize = ctx.binary_.imageSize();

  return ctx;
}

CodegenContext CodegenContext::Create(BinaryView binary, RecompilerConfig config) {
  CodegenContext ctx;
  ctx.binary_ = std::move(binary);
  ctx.config_ = std::move(config);
  return ctx;
}

CodegenContext::~CodegenContext() = default;
CodegenContext::CodegenContext(CodegenContext&&) noexcept = default;
CodegenContext& CodegenContext::operator=(CodegenContext&&) noexcept = default;

DecodedBinary& CodegenContext::decoded() {
  assert(decoded_ && "Call initDecoded() before accessing decoded()");
  return *decoded_;
}

const DecodedBinary& CodegenContext::decoded() const {
  assert(decoded_ && "Call initDecoded() before accessing decoded()");
  return *decoded_;
}

void CodegenContext::initDecoded() {
  decoded_ = std::make_unique<DecodedBinary>(binary_);
  decoded_->decode();
}

}  // namespace rex::codegen
