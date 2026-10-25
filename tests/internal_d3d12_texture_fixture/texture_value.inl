// Included only inside D3D12TextureCache members, where TextureKey is accessible.
const auto fixture_value = [&](const Texture& actual,
                               bool target_only = true) -> fixture::TextureValue {
  fixture::TextureValue value;
  if (!fixture::Enabled())
    return value;
  TextureKey expected;
  xenos::xe_gpu_texture_fetch_t fetch;
  static_assert(sizeof(fetch) == sizeof(fixture::Config::fetch));
  std::memcpy(&fetch, fixture::GetConfig().fetch, sizeof(fetch));
  uint8_t signs = 0;
  BindingInfoFromFetchConstant(fetch, expected, &signs);
  if (target_only && expected != actual.key())
    return value;
  if (sizeof(TextureKey) > value.key.size() || !expected.is_valid || expected.scaled_resolve ||
      expected.signed_separate || expected.mip_max_level || expected.GetWidth() != 256 ||
      expected.GetHeight() != 32 || expected.dimension != xenos::DataDimension::k2DOrStacked ||
      expected.format != xenos::TextureFormat::k_8_8_8_8 || expected.pitch != 8 ||
      expected.mip_page || expected.GetDepthOrArraySize() != 1 || !expected.tiled ||
      expected.endianness != xenos::Endian::kNone || !texture_util::IsAnySignNotSigned(signs)) {
    fixture::Error(20);
    return value;
  }
  if (!fixture::Owner(&command_processor_, this))
    return value;
  if (target_only) {
    if (const auto* seed = fixture::Seed()) {
      TextureKey sealed;
      if (seed->key_size != sizeof(TextureKey)) {
        fixture::Error(23);
        return value;
      }
      std::memcpy(&sealed, seed->key.data(), sizeof(TextureKey));
      if (actual.key() != sealed) {
        fixture::Error(24);
        return value;
      }
    }
  }
  value.instance = actual.instance_id();
  value.generation = actual.content_generation();
  value.dirty = actual.outdated_mask();
  if (value.generation != actual.content_generation())
    fixture::Error(21);
  value.resource = reinterpret_cast<uint64_t>(static_cast<const D3D12Texture&>(actual).resource());
  value.key_size = sizeof(TextureKey);
  std::memcpy(value.key.data(), &actual.key(), sizeof(TextureKey));
  value.base_address = actual.key().base_page << 12;
  value.base_size = actual.GetGuestBaseSize();
  value.mip_address = actual.key().mip_page << 12;
  value.mip_size = actual.GetGuestMipsSize();
  const auto desc = static_cast<const D3D12Texture&>(actual).resource()->GetDesc();
  value.host_width = uint32_t(desc.Width);
  value.host_height = desc.Height;
  value.host_depth = desc.DepthOrArraySize;
  value.host_mips = desc.MipLevels;
  value.host_format = uint32_t(desc.Format);
  return value;
};
