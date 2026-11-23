// The memory layout of textures in title memory, shared by the texture
// layout library (xg_xs) and the graphics library (d3d12_xs).
//
// The console layout library tells titles where each subresource of a
// texture lies in memory and copies texel data there for them; titles follow
// whatever it reports. The layout chosen here is the desktop's copyable
// footprint layout (documented for ID3D12Device::GetCopyableFootprints):
// subresources in order (mip levels within array slices, then planes), each
// starting at a multiple of 512 bytes, rows of blocks at a pitch that is a
// multiple of 256 bytes. Texture memory in that layout can be copied into a
// host texture with a plain buffer-to-texture copy.
#pragma once

#include <dxgiformat.h>

#include <cstdint>

namespace holy::lib {

struct FormatInfo {
  uint32_t block_bytes = 0;  // bytes per texel, or per 4x4 block for block-compressed formats
  uint32_t block_size = 1;   // 1, or 4 for block-compressed formats
};

// Documented DXGI format sizes. Unknown formats report zero bytes.
inline FormatInfo format_info(uint32_t format) {
  switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
      return {16, 1};
    case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
      return {12, 1};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
      return {8, 1};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return {4, 1};
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
      return {2, 1};
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
      return {1, 1};
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
      return {8, 4};
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
      return {16, 4};
    default:
      return {};
  }
}

// The resource description fields the layout depends on (the desktop
// D3D12_RESOURCE_DESC layout: dimension at 0, width at 16, height at 24,
// depth or array size at 28, mip levels at 30, format at 32).
struct TextureShape {
  uint32_t dimension = 0;  // 2 1D, 3 2D, 4 3D
  uint64_t width = 0;
  uint32_t height = 1;
  uint32_t depth_or_array = 1;
  uint32_t mips = 1;
  uint32_t format = 0;
};

struct SubresourceLayout {
  uint64_t offset = 0;      // from the texture's start
  uint32_t row_pitch = 0;   // bytes between rows of blocks
  uint32_t rows = 0;        // rows of blocks per slice
  uint32_t row_bytes = 0;   // bytes of texels in a row
  uint32_t depth = 1;       // slices (3D textures)
  uint64_t size = 0;        // bytes the subresource spans
};

inline uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

inline uint32_t subresource_count(const TextureShape& shape) {
  const uint32_t mips = shape.mips ? shape.mips : 1;
  return mips * (shape.dimension == 4 ? 1 : (shape.depth_or_array ? shape.depth_or_array : 1));
}

// Layout of subresource `index` (mip level fastest, then array slice).
// Returns false for formats whose size is not known.
inline bool subresource_layout(const TextureShape& shape, uint32_t index, SubresourceLayout* out) {
  const FormatInfo f = format_info(shape.format);
  if (!f.block_bytes) return false;
  const uint32_t mips = shape.mips ? shape.mips : 1;
  uint64_t offset = 0;
  for (uint32_t i = 0; i <= index; ++i) {
    const uint32_t mip = i % mips;
    const uint64_t width = (shape.width >> mip) ? (shape.width >> mip) : 1;
    const uint32_t height = (shape.height >> mip) ? (shape.height >> mip) : 1;
    const uint32_t depth =
        shape.dimension == 4 ? ((shape.depth_or_array >> mip) ? (shape.depth_or_array >> mip) : 1) : 1;
    SubresourceLayout layout;
    layout.offset = align_up(offset, 512);
    layout.row_bytes = static_cast<uint32_t>((width + f.block_size - 1) / f.block_size * f.block_bytes);
    layout.row_pitch = static_cast<uint32_t>(align_up(layout.row_bytes, 256));
    layout.rows = (height + f.block_size - 1) / f.block_size;
    layout.depth = depth;
    layout.size = static_cast<uint64_t>(layout.row_pitch) * layout.rows * depth;
    if (i == index) {
      *out = layout;
      return true;
    }
    offset = layout.offset + layout.size;
  }
  return false;
}

// Total bytes of the texture in this layout.
inline uint64_t texture_size(const TextureShape& shape) {
  const uint32_t count = subresource_count(shape);
  SubresourceLayout last;
  if (!count || !subresource_layout(shape, count - 1, &last)) return 0;
  return last.offset + last.size;
}

}  // namespace holy::lib
