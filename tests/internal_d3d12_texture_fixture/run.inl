int wmain() {
  struct LoggingScope {
    LoggingScope() {
      rex::LogConfig config;
      config.log_to_console = true;
      rex::InitLogging(config);
    }
    ~LoggingScope() { rex::ShutdownLogging(); }
  } logging;
  if (!capture_io::Initialize())
    return 2;
  float producer_alpha = 1.0F;

  // Match the production ReXApp order by loading the plugin before runtime setup.
  auto graphics = rex::system::LoadGpuPlugin("xenos", "d3d12");
  if (!Require(graphics != nullptr, "xenos d3d12 plugin was not loaded")) {
    return 1;
  }

  if (!rex::cvar::SetFlagByName("render_target_path_d3d12", "rov"))
    return 2;
  if (!test_fixture::Connect())
    return 2;
  rex::Runtime runtime(std::filesystem::path{});
  if (!Require(runtime.Setup(rex::RuntimeConfig{
                   .kernel_init = rex::kernel::InitializeKernel,
                   .tool_mode = true,
               }) == static_cast<rex::X_STATUS>(0),
               "runtime setup failed")) {
    return 1;
  }

  // Select full readback after the plugin registers its backend flags.
  // This process-local test setting leaves SDK defaults unchanged.
  if (!Require(rex::cvar::SetFlagByName("readback_resolve", "full"),
               "readback_resolve=full was rejected")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  if (!Require(rex::cvar::SetFlagByName("async_shader_compilation", "false"),
               "synchronous shader compilation was rejected")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  if (!Require(graphics->SetupPresentation(nullptr) == static_cast<rex::X_STATUS>(0),
               "offscreen D3D12 presenter setup failed")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  if (!Require(graphics->SetupGuestGpu(runtime.function_dispatcher(), runtime.kernel_state()) ==
                   static_cast<rex::X_STATUS>(0),
               "guest GPU setup failed")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  auto mailbox = rex::graphics::diagnostic::GetDrawCaptureMailbox();
  auto observation = std::make_shared<OriginalObservation>();
  const auto observer_token = mailbox->RegisterFrameInputObserver(observation);
  if (!Require(static_cast<bool>(observer_token), "mailbox observer registration failed")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  rex::memory::Memory& memory = *runtime.memory();
  const uint32_t producer_guest_extent =
      GuestTextureExtent(kSurfaceWidth, kSurfaceHeight, kSurfaceWidth);
  const uint32_t consumer_guest_extent =
      GuestTextureExtent(kConsumerWidth, kConsumerHeight, kConsumerWidth);
  const uint32_t producer_guest_allocation_bytes = AlignToPage(producer_guest_extent);
  const uint32_t consumer_guest_allocation_bytes = AlignToPage(consumer_guest_extent);
  if (!Require(producer_guest_extent == kSurfaceBytes && producer_guest_allocation_bytes != 0U,
               "producer tiled texture layout was not the qualified extent") ||
      !Require(consumer_guest_extent != 0U && consumer_guest_allocation_bytes != 0U,
               "consumer tiled texture layout was empty")) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  GuestAllocation ring(memory, kRingWordCount * sizeof(uint32_t), 0x1000U);
  GuestAllocation writeback(memory, 0x1000U, 0x1000U);
  GuestAllocation destination(memory, producer_guest_allocation_bytes, 0x1000U);
  GuestAllocation consumer_destination(memory, consumer_guest_allocation_bytes, 0x1000U);
  GuestAllocation clear_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation b_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation c_texture(memory, producer_guest_allocation_bytes, 0x1000U);
  GuestAllocation resolve_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation full_resolve_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation consumer_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation index_data(memory, 0x1000U, 0x1000U);
  GuestAllocation clear_vertex(memory, static_cast<uint32_t>(kClearVertexShader.size() * 4U),
                               0x1000U);
  GuestAllocation clear_pixel(memory, static_cast<uint32_t>(kClearPixelShader.size() * 4U),
                              0x1000U);
  GuestAllocation consumer_vertex(memory, static_cast<uint32_t>(kConsumerVertexShader.size() * 4U),
                                  0x1000U);
  GuestAllocation consumer_pixel(memory, static_cast<uint32_t>(kConsumerPixelShader.size() * 4U),
                                 0x1000U);
  const bool allocations_ok = b_geometry && c_texture && ring && writeback && destination &&
                              consumer_destination && clear_geometry && resolve_geometry &&
                              full_resolve_geometry && consumer_geometry && index_data &&
                              clear_vertex && clear_pixel && consumer_vertex && consumer_pixel;
  if (!Require(allocations_ok, "guest physical test allocations failed")) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    consumer_pixel.Reset();
    consumer_vertex.Reset();
    clear_pixel.Reset();
    clear_vertex.Reset();
    index_data.Reset();
    consumer_geometry.Reset();
    full_resolve_geometry.Reset();
    resolve_geometry.Reset();
    clear_geometry.Reset();
    consumer_destination.Reset();
    destination.Reset();
    writeback.Reset();
    ring.Reset();
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  const auto b_geometry_bytes = ClearGeometry(0.0F);
  std::memcpy(b_geometry.physical_bytes(), b_geometry_bytes.data(), b_geometry_bytes.size());
  std::memset(c_texture.physical_bytes(), 0x55, producer_guest_allocation_bytes);
  const auto clear_geometry_bytes = ClearGeometry(producer_alpha);
  const auto resolve_geometry_bytes = ResolveGeometry();
  const auto full_resolve_geometry_bytes = FullResolveGeometry();
  const auto consumer_geometry_bytes = ConsumerGeometry();
  std::memcpy(clear_geometry.physical_bytes(), clear_geometry_bytes.data(),
              clear_geometry_bytes.size());
  std::memcpy(resolve_geometry.physical_bytes(), resolve_geometry_bytes.data(),
              resolve_geometry_bytes.size());
  std::memcpy(full_resolve_geometry.physical_bytes(), full_resolve_geometry_bytes.data(),
              full_resolve_geometry_bytes.size());
  std::memcpy(consumer_geometry.physical_bytes(), consumer_geometry_bytes.data(),
              consumer_geometry_bytes.size());
  const std::array<uint8_t, 6> indices = {0, 0, 0, 1, 0, 2};
  std::memcpy(index_data.physical_bytes(), indices.data(), indices.size());
  std::memset(destination.physical_bytes(), 0, kSurfaceBytes);
  std::memset(consumer_destination.physical_bytes(), 0, consumer_guest_allocation_bytes);
  // IM_LOAD reads guest-endian ucode dwords. Keep the arrays in host order
  // for the packet builder and write their guest representation explicitly.
  WriteShaderWords(clear_vertex, kClearVertexShader);
  WriteShaderWords(clear_pixel, kClearPixelShader);
  WriteShaderWords(consumer_vertex, kConsumerVertexShader);
  WriteShaderWords(consumer_pixel, kConsumerPixelShader);

  graphics->InitializeRingBuffer(ring.physical_address(), kRingSizeLog2);
  graphics->EnableReadPointerWriteBack(writeback.physical_address(), kWritebackBlockSizeLog2);

  const auto destination_texture =
      TextureFetch(destination.physical_address(), kSurfaceWidth, kSurfaceHeight, kSurfaceWidth);
  const auto consumer_destination_texture = TextureFetch(
      consumer_destination.physical_address(), kConsumerWidth, kConsumerHeight, kConsumerWidth);
  auto clear_fetch = VertexFetch(clear_geometry.physical_address(),
                                 static_cast<uint32_t>(clear_geometry_bytes.size()));
  clear_fetch[1] |= 0x10000000U;
  const auto resolve_fetch = VertexFetch(resolve_geometry.physical_address(),
                                         static_cast<uint32_t>(resolve_geometry_bytes.size()));
  const auto full_resolve_fetch =
      VertexFetch(full_resolve_geometry.physical_address(),
                  static_cast<uint32_t>(full_resolve_geometry_bytes.size()));
  const auto consumer_fetch = VertexFetch(consumer_geometry.physical_address(),
                                          static_cast<uint32_t>(consumer_geometry_bytes.size()));
  const auto index_fetch_address = index_data.physical_address();

  auto b_fetch = VertexFetch(b_geometry.physical_address(), uint32_t(b_geometry_bytes.size()));
  b_fetch[1] |= 0x10000000U;
  const auto c_fetch =
      TextureFetch(c_texture.physical_address(), kSurfaceWidth, kSurfaceHeight, kSurfaceWidth);
  test_fixture::target = destination.physical_bytes();
  test_fixture::target_address = destination.physical_address();
  test_fixture::terminal_address = consumer_destination.physical_address();
  test_fixture::fetch_words = destination_texture;
  if (!test_fixture::Configure(destination_texture))
    return 2;
  PacketStream stream;
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, destination_texture);
  stream.Swap(destination.physical_address(), kSurfaceWidth, kSurfaceHeight);
  const auto append_producer = [&](const auto& producer_fetch) {
    // Replay only state written by the consumer/resolve blocks. These are
    // the original producer's values, including the initial zero values.
    stream.Type0(0x2080U, {0U, 0U});
    stream.Type0(0x210FU, {0U, 0U, 0U, 0U});
    stream.Type0(0x2180U, {0U, 0U, 0U});
    stream.Type0(0x2202U, {0U});
    stream.Type0(0x2302U, {0U});
    stream.Type0(0x2307U, {0U, 0U});
    // Synthetic earliest producer: clear a 256x32 RGBA8 color-0 surface.
    stream.LoadShader(clear_vertex.physical_address(), rex::graphics::xenos::ShaderType::kVertex,
                      static_cast<uint32_t>(kClearVertexShader.size()));
    stream.LoadShader(clear_pixel.physical_address(), rex::graphics::xenos::ShaderType::kPixel,
                      static_cast<uint32_t>(kClearPixelShader.size()));
    stream.Type0(0x2000U, {0x04000140U, 0U, 0x530U});
    stream.Type0(0x200FU, {0x20002000U});
    stream.Type0(0x2082U, {0x20002000U});
    stream.Type0(0x2104U, {0xFU});
    stream.Type0(0x2100U, {2U});
    stream.Type0(0x210DU, {0x00FF0000U});
    stream.Type0(0x2200U, {0x8777U});
    stream.Type0(0x2201U, {0x00010001U});
    stream.Type0(0x2204U, {0x10000U});
    stream.Type0(0x2205U, {0x10000U});
    stream.Type0(0x2206U, {0x300U});
    stream.Type0(0x2208U, {4U});
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, producer_fetch);
    stream.Type0(0x21FCU,
                 {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) |
                  (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kAutoIndex) << 6) |
                  (3U << 16)});
    stream.DrawAuto(0x00030088U);
  };
  const auto append_resolve = [&](uint32_t output_address) {
    // Resolve the current EDRAM color-0 surface with an actual copy draw through
    // the D3D12 render-target cache and readback owner, not a mailbox fixture.
    stream.Type0(0x2000U, {0x04000140U, 0U, 0x530U});
    stream.Type0(0x200FU, {0x20002000U});
    stream.Type0(0x2080U, {0U, 0U, kSurfaceWidth | (kSurfaceHeight << 16)});
    stream.Type0(0x2205U, {0x10000U});
    stream.Type0(0x2208U, {6U});
    stream.Type0(0x2318U, {0x00100000U});
    stream.Type0(0x2319U, {output_address});
    stream.Type0(0x231AU, {kSurfaceWidth | (kSurfaceHeight << 16)});
    stream.Type0(0x231BU, {0x300U});
    stream.Type0(0x231CU, {0xFFFU});
    stream.Type0(0x231DU, {0xFFFFFF00U});
    stream.Type0(0x231EU, {0U, 0U});
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, resolve_fetch);
    stream.Type0(0x21FCU,
                 {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) |
                  (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kAutoIndex) << 6) |
                  (3U << 16)});
    stream.DrawAuto(0x00030088U);
  };

  const auto append_full_resolve = [&](uint32_t output_address) {
    // Resolve the full 1280x720 consumer color-0 surface. The source surface
    // and copy pitch stay at the consumer dimensions for the terminal owner.
    stream.Type0(0x2000U, {0x14000500U, 0U, 0x530U});
    stream.Type0(0x200FU, {0x20002000U});
    stream.Type0(0x2080U, {0U, 0U, kConsumerWidth | (kConsumerHeight << 16)});
    stream.Type0(0x2205U, {0x10000U});
    stream.Type0(0x2208U, {6U});
    stream.Type0(0x2318U, {0x00100000U});
    stream.Type0(0x2319U, {output_address});
    stream.Type0(0x231AU, {0x02D00500U});
    stream.Type0(0x231BU, {0x300U});
    stream.Type0(0x231CU, {0xFFFU});
    stream.Type0(0x231DU, {0xFFFFFF00U});
    stream.Type0(0x231EU, {0U, 0U});
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, full_resolve_fetch);
    stream.Type0(0x21FCU,
                 {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) |
                  (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kAutoIndex) << 6) |
                  (3U << 16)});
    stream.DrawAuto(0x00030088U);
  };

  const auto append_consumer = [&](const auto& source_fetch) {
    // Required consumer draw in the next frame, sampling the exact resolve destination.
    stream.LoadShader(consumer_vertex.physical_address(), rex::graphics::xenos::ShaderType::kVertex,
                      static_cast<uint32_t>(kConsumerVertexShader.size()));
    stream.LoadShader(consumer_pixel.physical_address(), rex::graphics::xenos::ShaderType::kPixel,
                      static_cast<uint32_t>(kConsumerPixelShader.size()));
    // Cataloged 0048 color-0 state: 1X, 1280-pixel pitch, program 0x10010002,
    // context 4, one interpolator, alpha test off. The resolve reads the full
    // consumer surface into a distinct terminal texture.
    stream.Type0(0x2000U, {0x14000500U, 0U, 0x530U});
    stream.Type0(0x200FU, {0x20002000U});
    stream.Type0(0x2080U, {0U, 0U});
    stream.Type0(0x2082U, {1280U | (720U << 16)});
    stream.Type0(0x2100U, {2U});
    stream.Type0(0x2104U, {0xFU});
    stream.Type0(0x210FU, {std::bit_cast<uint32_t>(640.0F), std::bit_cast<uint32_t>(640.0F),
                           std::bit_cast<uint32_t>(-360.0F), std::bit_cast<uint32_t>(360.0F)});
    stream.Type0(0x2180U, {0x10010002U});
    stream.Type0(0x2181U, {4U});
    stream.Type0(0x2182U, {1U});
    stream.Type0(0x2201U, {0x00010006U});
    stream.Type0(0x2202U, {0x87000000U});
    stream.Type0(0x2204U, {0x80000U});
    stream.Type0(0x2205U, {0x18000U});
    stream.Type0(0x2206U, {0x43FU});
    stream.Type0(0x2208U, {4U});
    stream.Type0(0x2302U, {4U});
    stream.Type0(0x2307U, {0xFF000U});
    stream.Type0(0x2308U, {0xFF100U});
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X, kConsumerVertexConstants);
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X, kConsumerPixelConstant0);
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U,
                 kConsumerPixelConstant255);
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, source_fetch);
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U, consumer_fetch);
    stream.Type0(
        0x21FCU,
        {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
         (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6) | (3U << 16)});
    stream.DrawDma(0x00030004U, index_fetch_address, 3U, rex::graphics::xenos::Endian::k8in16);
  };

  append_producer(clear_fetch);
  append_resolve(destination.physical_address());
  // The A callback arms capture before this swap opens the owned frame.
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, destination_texture);
  stream.Swap(destination.physical_address(), kSurfaceWidth, kSurfaceHeight);
  append_consumer(destination_texture);  // Seal SeedA after actual issued use.
  append_producer(b_fetch);
  append_resolve(destination.physical_address());  // Real watched B overwrite.
  append_consumer(destination_texture);            // One-shot failed reload when enabled.
  append_consumer(destination_texture);            // Identical repeat must not retry.
  append_consumer(c_fetch);                        // Disjoint key breaks binding reuse.
  append_consumer(destination_texture);            // Exactly one successful retry.
  append_full_resolve(consumer_destination.physical_address());
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, consumer_destination_texture);
  stream.Swap(consumer_destination.physical_address(), kConsumerWidth, kConsumerHeight);
  if (!Require(stream.words().size() < kRingWordCount, "PM4 capacity"))
    return 2;
  if (!capture_io::Bytes(
          "pm4-host-words.bin",
          {reinterpret_cast<const uint8_t*>(stream.words().data()), stream.words().size() * 4}) ||
      !capture_io::Bytes("target-fetch-words.bin",
                         {reinterpret_cast<const uint8_t*>(destination_texture.data()),
                          sizeof(destination_texture)}) ||
      !capture_io::Bytes("c-fetch-words.bin",
                         {reinterpret_cast<const uint8_t*>(c_fetch.data()), sizeof(c_fetch)}) ||
      !capture_io::Bytes("terminal-fetch-words.bin",
                         {reinterpret_cast<const uint8_t*>(consumer_destination_texture.data()),
                          sizeof(consumer_destination_texture)}))
    return 2;
  WritePacketWords(memory, ring.physical_address(), stream.words());
  PublishWritePointer(memory, uint32_t(stream.words().size()));
  const bool progressed =
      WaitForWriteback(memory, writeback.physical_address(), uint32_t(stream.words().size()));
  const bool completed =
      progressed && observation->WaitForCompletion(consumer_destination.physical_address());
  capture_io::Take();
  const bool saved = capture_io::Save();
  const bool validated = test_fixture::Validate(observation->TakeSnapshot());
  const bool retired = test_fixture::retired;
  graphics->Shutdown();
  mailbox->UnregisterFrameInputObserver(observer_token);
  // All allocations must retire while their memory owner still exists.
  consumer_pixel.Reset();
  consumer_vertex.Reset();
  clear_pixel.Reset();
  clear_vertex.Reset();
  index_data.Reset();
  consumer_geometry.Reset();
  full_resolve_geometry.Reset();
  resolve_geometry.Reset();
  b_geometry.Reset();
  c_texture.Reset();
  clear_geometry.Reset();
  consumer_destination.Reset();
  destination.Reset();
  writeback.Reset();
  ring.Reset();
  runtime.Shutdown();
  std::cout << "progressed=" << progressed << " completed=" << completed << " saved=" << saved
            << " validated=" << validated << " retired=" << retired << '\n';
  return completed && saved && validated && retired ? 0 : 1;
}
