/**
 * @file        tests/modules/null_gpu/null_gpu_module.cpp
 * @brief       GPU backend test module with a recording overlay drawer and no device
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <map>
#include <memory>
#include <span>

#include <rex/api.h>
#include <rex/gpu.h>
#include <rex/module.h>

#include "../test_module_common.h"

REXMOD_TEST_SINK()

namespace {

constexpr const char* kName = "rexmod-test-null-gpu";
int g_fail_textures = 0;

class NullTexture final : public rex::gpu::OverlayTexture {
 public:
  NullTexture(rex::u32 width, rex::u32 height) : OverlayTexture(width, height) {}
  ~NullTexture() override { Record(kName, "texture_freed"); }
};

class NullContext final : public rex::gpu::OverlayDrawContext {
 public:
  explicit NullContext(rex::gpu::IPresenter* presenter) : OverlayDrawContext(presenter, 640, 360) {}
};

class NullDrawer final : public rex::gpu::IOverlayDrawer {
 public:
  rex::Result<std::unique_ptr<rex::gpu::OverlayTexture>> CreateTexture(
      rex::u32 width, rex::u32 height, rex::gpu::OverlayTextureFilter, bool,
      std::span<const rex::u8>) override {
    if (g_fail_textures > 0) {
      --g_fail_textures;
      Record(kName, "create_texture_failed");
      return rex::Err<std::unique_ptr<rex::gpu::OverlayTexture>>(rex::ErrorCategory::Runtime,
                                                                 "null device not ready");
    }
    Record(kName, "create_texture");
    return std::unique_ptr<rex::gpu::OverlayTexture>(std::make_unique<NullTexture>(width, height));
  }
  void Begin(rex::gpu::OverlayDrawContext&, rex::f32, rex::f32) override { Record(kName, "begin"); }
  void BeginBatch(const rex::gpu::OverlayBatch&) override { Record(kName, "begin_batch"); }
  void Draw(const rex::gpu::OverlayDraw&) override { Record(kName, "draw"); }
  void EndBatch() override { Record(kName, "end_batch"); }
  void End() override { Record(kName, "end"); }
};

class NullPresenter final : public rex::gpu::IPresenter {
 public:
  rex::gpu::Surface::TypeFlags supported_surface_types() const override { return ~0u; }
  bool Attach(rex::gpu::IPresentationHost* host, rex::gpu::Surface*) override {
    if (host_ && host_ != host) {
      rex::gpu::IPresentationHost* old_host = host_;
      host_ = nullptr;
      old_host->OnPresenterDetached();
    }
    host_ = host;
    Record(kName, host ? "attach" : "detach");
    return false;
  }
  void OnSurfaceMonitorUpdate(bool) override {}
  void OnSurfaceResize() override {}
  void Paint(bool) override {
    NullContext context(this);
    const std::multimap<size_t, rex::gpu::IOverlayLayer*> layers = layers_;
    for (const auto& [z_order, layer] : layers) {
      layer->Draw(context);
    }
  }
  void RequestOverlayPaint() override {}
  void AddOverlayLayer(rex::gpu::IOverlayLayer* layer, size_t z_order) override {
    RemoveOverlayLayer(layer);
    layers_.emplace(z_order, layer);
  }
  void RemoveOverlayLayer(rex::gpu::IOverlayLayer* layer) override {
    std::erase_if(layers_, [layer](const auto& entry) { return entry.second == layer; });
  }

 private:
  rex::gpu::IPresentationHost* host_ = nullptr;
  std::multimap<size_t, rex::gpu::IOverlayLayer*> layers_;
};

class NullGPUModule final : public rex::IModule, public rex::gpu::IBackend {
 public:
  static void* GetModule() { return static_cast<rex::IModule*>(&Instance()); }
  static void* GetBackend() { return static_cast<rex::gpu::IBackend*>(&Instance()); }
  static constexpr std::array<rex::InterfaceEntry, 2> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
      {rex::gpu::IBackend::kInterfaceName, rex::gpu::IBackend::kInterfaceVersion, &GetBackend},
  }};

  const char* Name() const override { return kName; }
  std::span<const char* const> RequiredInterfaces() const override { return {}; }
  rex::Result<void> Connect(rex::IInterfaceFactory*, const rex::Config&) override {
    return rex::Ok();
  }
  void Disconnect() override {}
  rex::Result<void> Init() override { return rex::Ok(); }
  void Shutdown() override { presenter_.reset(); }

  rex::Result<void> SetupPresentation(rex::gpu::IUIThread& ui_thread) override {
    ui_thread.RunSynchronous([this] { presenter_ = std::make_unique<NullPresenter>(); });
    return rex::Ok();
  }
  rex::Result<void> SetupGuestGPU(rex::runtime::FunctionDispatcher&, rex::Runtime&) override {
    return rex::Ok();
  }
  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::gpu::IPresenter* presenter() const override { return presenter_.get(); }
  rex::Result<std::unique_ptr<rex::gpu::IPresenter>> CreatePresenter() override {
    Record(kName, "create_presenter");
    return std::unique_ptr<rex::gpu::IPresenter>(std::make_unique<NullPresenter>());
  }
  rex::Result<std::unique_ptr<rex::gpu::IOverlayDrawer>> CreateOverlayDrawer(
      rex::gpu::IPresenter&) override {
    Record(kName, "create_drawer");
    return std::unique_ptr<rex::gpu::IOverlayDrawer>(std::make_unique<NullDrawer>());
  }
  void SetInterruptCallback(rex::u32, rex::u32) override {}
  void InitializeRingBuffer(rex::u32, rex::u32) override {}
  void EnableReadPointerWriteBack(rex::u32, rex::u32) override {}
  rex::Result<void> InitializeShaderStorage(const std::filesystem::path&, rex::u32, bool) override {
    return rex::Ok();
  }

 private:
  static NullGPUModule& Instance() {
    static NullGPUModule instance;
    return instance;
  }
  std::unique_ptr<NullPresenter> presenter_;
};

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" REX_DETAIL_EXPORT void rexmod_test_null_gpu_fail_textures(int count) {
  g_fail_textures = count;
}

REX_DECLARE_MODULE(NullGPUModule)
