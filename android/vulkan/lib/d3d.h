/* The D3D11/DXGI structures the engine's cold path hands to the native device (Win64 layout, which
 * equals AArch64 LP64 for these: 32-bit enums/UINT/BOOL, 64-bit pointers). */
#pragma once
#include <cstdint>

namespace rdvk::d3d {
typedef uint32_t UINT;
typedef int32_t BOOL;

enum : UINT { USAGE_DEFAULT = 0, USAGE_IMMUTABLE = 1, USAGE_DYNAMIC = 2, USAGE_STAGING = 3 };
enum : UINT {
    BIND_VERTEX_BUFFER = 0x1, BIND_INDEX_BUFFER = 0x2, BIND_CONSTANT_BUFFER = 0x4, BIND_SHADER_RESOURCE = 0x8,
    BIND_STREAM_OUTPUT = 0x10, BIND_RENDER_TARGET = 0x20, BIND_DEPTH_STENCIL = 0x40, BIND_UNORDERED_ACCESS = 0x80
};
enum : UINT { CPU_ACCESS_WRITE = 0x10000, CPU_ACCESS_READ = 0x20000 };
enum : UINT { MISC_GENERATE_MIPS = 0x1, MISC_TEXTURECUBE = 0x4, MISC_BUFFER_ALLOW_RAW_VIEWS = 0x20, MISC_BUFFER_STRUCTURED = 0x40 };
enum : UINT { MAP_READ = 1, MAP_WRITE = 2, MAP_READ_WRITE = 3, MAP_WRITE_DISCARD = 4, MAP_WRITE_NO_OVERWRITE = 5 };

struct BUFFER_DESC { UINT ByteWidth, Usage, BindFlags, CPUAccessFlags, MiscFlags, StructureByteStride; };
struct SUBRESOURCE_DATA { uint64_t pSysMem; UINT SysMemPitch, SysMemSlicePitch; };
struct SAMPLE_DESC { UINT Count, Quality; };
struct TEXTURE1D_DESC { UINT Width, MipLevels, ArraySize, Format, Usage, BindFlags, CPUAccessFlags, MiscFlags; };
struct TEXTURE2D_DESC { UINT Width, Height, MipLevels, ArraySize, Format; SAMPLE_DESC SampleDesc; UINT Usage, BindFlags, CPUAccessFlags, MiscFlags; };
struct TEXTURE3D_DESC { UINT Width, Height, Depth, MipLevels, Format, Usage, BindFlags, CPUAccessFlags, MiscFlags; };
static_assert(sizeof(BUFFER_DESC) == 24 && sizeof(SUBRESOURCE_DATA) == 16 && sizeof(TEXTURE2D_DESC) == 44, "D3D11 desc layout");

enum : UINT {
    SRV_UNKNOWN = 0, SRV_BUFFER = 1, SRV_TEXTURE1D = 2, SRV_TEXTURE1DARRAY = 3, SRV_TEXTURE2D = 4, SRV_TEXTURE2DARRAY = 5,
    SRV_TEXTURE2DMS = 6, SRV_TEXTURE2DMSARRAY = 7, SRV_TEXTURE3D = 8, SRV_TEXTURECUBE = 9, SRV_TEXTURECUBEARRAY = 10, SRV_BUFFEREX = 11
};
struct SRV_DESC { UINT Format, ViewDimension; UINT a, b, c, d; };   /* union members by dimension */
enum : UINT { RTV_BUFFER = 1, RTV_TEXTURE1D = 2, RTV_TEXTURE1DARRAY = 3, RTV_TEXTURE2D = 4, RTV_TEXTURE2DARRAY = 5,
              RTV_TEXTURE2DMS = 6, RTV_TEXTURE2DMSARRAY = 7, RTV_TEXTURE3D = 8 };
struct RTV_DESC { UINT Format, ViewDimension; UINT a, b, c; };
enum : UINT { DSV_TEXTURE1D = 1, DSV_TEXTURE1DARRAY = 2, DSV_TEXTURE2D = 3, DSV_TEXTURE2DARRAY = 4, DSV_TEXTURE2DMS = 5, DSV_TEXTURE2DMSARRAY = 6 };
enum : UINT { DSV_READ_ONLY_DEPTH = 1, DSV_READ_ONLY_STENCIL = 2 };
struct DSV_DESC { UINT Format, ViewDimension, Flags; UINT a, b, c; };
enum : UINT { UAV_BUFFER = 1, UAV_TEXTURE1D = 2, UAV_TEXTURE1DARRAY = 3, UAV_TEXTURE2D = 4, UAV_TEXTURE2DARRAY = 5, UAV_TEXTURE3D = 8 };
struct UAV_DESC { UINT Format, ViewDimension; UINT a, b, c; };
static_assert(sizeof(SRV_DESC) == 24 && sizeof(RTV_DESC) == 20 && sizeof(DSV_DESC) == 24 && sizeof(UAV_DESC) == 20, "view desc layout");

struct INPUT_ELEMENT_DESC { uint64_t SemanticName; UINT SemanticIndex, Format, InputSlot, AlignedByteOffset, InputSlotClass, InstanceDataStepRate; };
static_assert(sizeof(INPUT_ELEMENT_DESC) == 32, "input element layout");

struct RENDER_TARGET_BLEND_DESC { BOOL BlendEnable; UINT SrcBlend, DestBlend, BlendOp, SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha; uint8_t RenderTargetWriteMask; uint8_t pad[3]; };
struct BLEND_DESC { BOOL AlphaToCoverageEnable, IndependentBlendEnable; RENDER_TARGET_BLEND_DESC RenderTarget[8]; };
static_assert(sizeof(BLEND_DESC) == 264, "blend desc layout");
struct DEPTH_STENCILOP_DESC { UINT StencilFailOp, StencilDepthFailOp, StencilPassOp, StencilFunc; };
struct DEPTH_STENCIL_DESC { BOOL DepthEnable; UINT DepthWriteMask, DepthFunc; BOOL StencilEnable; uint8_t StencilReadMask, StencilWriteMask, pad[2];
                            DEPTH_STENCILOP_DESC FrontFace, BackFace; };
static_assert(sizeof(DEPTH_STENCIL_DESC) == 52, "depth desc layout");
struct RASTERIZER_DESC { UINT FillMode, CullMode; BOOL FrontCounterClockwise; int32_t DepthBias; float DepthBiasClamp, SlopeScaledDepthBias;
                         BOOL DepthClipEnable, ScissorEnable, MultisampleEnable, AntialiasedLineEnable; };
static_assert(sizeof(RASTERIZER_DESC) == 40, "raster desc layout");
struct SAMPLER_DESC { UINT Filter, AddressU, AddressV, AddressW; float MipLODBias; UINT MaxAnisotropy, ComparisonFunc; float BorderColor[4]; float MinLOD, MaxLOD; };
static_assert(sizeof(SAMPLER_DESC) == 52, "sampler desc layout");
struct QUERY_DESC { UINT Query, MiscFlags; };
enum : UINT { QUERY_EVENT = 0, QUERY_OCCLUSION = 1, QUERY_TIMESTAMP = 2, QUERY_TIMESTAMP_DISJOINT = 3, QUERY_PIPELINE_STATISTICS = 4,
              QUERY_OCCLUSION_PREDICATE = 5, QUERY_SO_STATISTICS = 6, QUERY_SO_OVERFLOW_PREDICATE = 7 };
struct MAPPED_SUBRESOURCE { uint64_t pData; UINT RowPitch, DepthPitch; };
struct BOX { UINT left, top, front, right, bottom, back; };
struct VIEWPORT { float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth; };
struct RECT { int32_t left, top, right, bottom; };

struct SWAP_CHAIN_DESC1 { UINT Width, Height, Format; BOOL Stereo; SAMPLE_DESC SampleDesc; UINT BufferUsage, BufferCount, Scaling, SwapEffect, AlphaMode, Flags; };
static_assert(sizeof(SWAP_CHAIN_DESC1) == 48, "swap chain desc layout");
struct ADAPTER_DESC { uint16_t Description[128]; UINT VendorId, DeviceId, SubSysId, Revision; uint64_t DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory; uint32_t LuidLow; int32_t LuidHigh; };
static_assert(sizeof(ADAPTER_DESC) == 304, "adapter desc layout");

/* HRESULTs */
constexpr uint32_t HR_S_OK = 0, HR_S_FALSE = 1, HR_E_NOINTERFACE = 0x80004002u, HR_E_INVALIDARG = 0x80070057u, HR_E_FAIL = 0x80004005u,
                   HR_E_OUTOFMEMORY = 0x8007000Eu, HR_DXGI_STILL_DRAWING = 0x887A000Au, HR_DXGI_INVALID_CALL = 0x887A0001u,
                   HR_DXGI_NOT_FOUND = 0x887A0002u;
}  // namespace rdvk::d3d
