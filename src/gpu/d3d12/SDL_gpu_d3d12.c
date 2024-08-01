/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_internal.h"

#if SDL_GPU_D3D12

#define D3D12_NO_HELPERS
#define CINTERFACE
#define COBJMACROS

/* From the DirectX-Headers build system:
 * "MinGW has RPC headers which define old versions, and complain if D3D
 * headers are included before the RPC headers, since D3D headers were
 * generated with new MIDL and "require" new RPC headers."
 */
#define __REQUIRED_RPCNDR_H_VERSION__ 475
#ifndef WINAPI_PARTITION_GAMES
#define WINAPI_PARTITION_GAMES 0
#endif /* WINAPI_PARTITION_GAMES */
#include "../../video/directx/d3d12.h"

#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>

#include "../SDL_sysgpu.h"
#include "SDL_hashtable.h"

/* Macros */

#define D3DCOMPILER_API STDMETHODCALLTYPE

#define ERROR_CHECK(msg)                                     \
    if (FAILED(res)) {                                       \
        D3D12_INTERNAL_LogError(renderer->device, msg, res); \
    }

#define ERROR_CHECK_RETURN(msg, ret)                         \
    if (FAILED(res)) {                                       \
        D3D12_INTERNAL_LogError(renderer->device, msg, res); \
        return ret;                                          \
    }

#define EXPAND_ARRAY_IF_NEEDED(arr, elementType, newCount, capacity, newCapacity) \
    if (newCount >= capacity) {                                                   \
        capacity = newCapacity;                                                   \
        arr = (elementType *)SDL_realloc(                                         \
            arr,                                                                  \
            sizeof(elementType) * capacity);                                      \
    }

/* Defines */
#if defined(_WIN32)
#define D3D12_DLL     "d3d12.dll"
#define DXGI_DLL      "dxgi.dll"
#define DXGIDEBUG_DLL "dxgidebug.dll"

/* FIXME: Reuse the d3dcompiler loader in SDL_egl.c */
#ifdef D3DCOMPILER_DLL
#undef D3DCOMPILER_DLL
#endif
#define D3DCOMPILER_DLL "d3dcompiler_47.dll"
#elif defined(__APPLE__)
#define D3D12_DLL       "libdxvk_d3d12.dylib"
#define DXGI_DLL        "libdxvk_dxgi.dylib"
#define DXGIDEBUG_DLL   "libdxvk_dxgidebug.dylib"
#define D3DCOMPILER_DLL "libvkd3d-utils.1.dylib"
#else
#define D3D12_DLL       "libdxvk_d3d12.so"
#define DXGI_DLL        "libdxvk_dxgi.so"
#define DXGIDEBUG_DLL   "libdxvk_dxgidebug.so"
#define D3DCOMPILER_DLL "libvkd3d-utils.so.1"
#endif

#define D3D12_CREATE_DEVICE_FUNC            "D3D12CreateDevice"
#define D3D12_SERIALIZE_ROOT_SIGNATURE_FUNC "D3D12SerializeRootSignature"
#define CREATE_DXGI_FACTORY1_FUNC           "CreateDXGIFactory1"
#define D3DCOMPILE_FUNC                     "D3DCompile"
#define DXGI_GET_DEBUG_INTERFACE_FUNC       "DXGIGetDebugInterface"
#define D3D12_GET_DEBUG_INTERFACE_FUNC      "D3D12GetDebugInterface"
#define WINDOW_PROPERTY_DATA                "SDL_GpuD3D12WindowPropertyData"
#define D3D_FEATURE_LEVEL_CHOICE            D3D_FEATURE_LEVEL_11_1
#define D3D_FEATURE_LEVEL_CHOICE_STR        "11_1"
/* FIXME: just use sysgpu.h defines */
#define MAX_ROOT_SIGNATURE_PARAMETERS         64
#define VIEW_GPU_DESCRIPTOR_COUNT             65536
#define SAMPLER_GPU_DESCRIPTOR_COUNT          2048
#define VIEW_SAMPLER_STAGING_DESCRIPTOR_COUNT 1000000
#define TARGET_STAGING_DESCRIPTOR_COUNT       1000000
#define D3D12_FENCE_UNSIGNALED_VALUE          0
#define D3D12_FENCE_SIGNAL_VALUE              1

#define SDL_GPU_SHADERSTAGE_COMPUTE 2

#define EXPAND_ELEMENTS_IF_NEEDED(arr, initialValue, type) \
    if (arr->count == arr->capacity) {                     \
        if (arr->capacity == 0) {                          \
            arr->capacity = initialValue;                  \
        } else {                                           \
            arr->capacity *= 2;                            \
        }                                                  \
        arr->elements = (type *)SDL_realloc(               \
            arr->elements,                                 \
            arr->capacity * sizeof(type));                 \
    }

#ifdef _WIN32
#define HRESULT_FMT "(0x%08lX)"
#else
#define HRESULT_FMT "(0x%08X)"
#endif

/* Function Pointer Signatures */
typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY1)(const GUID *riid, void **ppFactory);
typedef HRESULT(WINAPI *PFN_DXGI_GET_DEBUG_INTERFACE)(const GUID *riid, void **ppDebug);
typedef HRESULT(WINAPI *PFN_D3D12_GET_DEBUG_INTERFACE)(const GUID *riid, void **ppDebug);
typedef HRESULT(D3DCOMPILER_API *PFN_D3DCOMPILE)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2, ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

/* IIDs (from https://www.magnumdb.com/) */
static const IID D3D_IID_IDXGIFactory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const IID D3D_IID_IDXGIFactory4 = { 0x1bc6ea02, 0xef36, 0x464f, { 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a } };
static const IID D3D_IID_IDXGIFactory5 = { 0x7632e1f5, 0xee65, 0x4dca, { 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d } };
static const IID D3D_IID_IDXGIFactory6 = { 0xc1b6694f, 0xff09, 0x44a9, { 0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17 } };
static const IID D3D_IID_IDXGIAdapter1 = { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };
static const IID D3D_IID_IDXGISwapChain3 = { 0x94d99bdb, 0xf1f8, 0x4ab0, { 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1 } };
static const IID D3D_IID_IDXGIDebug = { 0x119e7452, 0xde9e, 0x40fe, { 0x88, 0x06, 0x88, 0xf9, 0x0c, 0x12, 0xb4, 0x41 } };
static const IID D3D_IID_IDXGIInfoQueue = { 0xd67441c7, 0x672a, 0x476f, { 0x9e, 0x82, 0xcd, 0x55, 0xb4, 0x49, 0x49, 0xce } };
static const GUID D3D_IID_DXGI_DEBUG_ALL = { 0xe48ae283, 0xda80, 0x490b, { 0x87, 0xe6, 0x43, 0xe9, 0xa9, 0xcf, 0xda, 0x08 } };
static const GUID D3D_IID_D3DDebugObjectName = { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };

// static const IID D3D_IID_ID3DUserDefinedAnnotation = { 0xb2daad8b, 0x03d4, 0x4dbf, { 0x95, 0xeb, 0x32, 0xab, 0x4b, 0x63, 0xd0, 0xab } };

static const IID D3D_IID_ID3D12Device = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const IID D3D_IID_ID3D12CommandQueue = { 0x0ec870a6, 0x5d7e, 0x4c22, { 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed } };
static const IID D3D_IID_ID3D12DescriptorHeap = { 0x8efb471d, 0x616c, 0x4f49, { 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51 } };
static const IID D3D_IID_ID3D12Resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
static const IID D3D_IID_ID3D12CommandAllocator = { 0x6102dee4, 0xaf59, 0x4b09, { 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24 } };
static const IID D3D_IID_ID3D12CommandList = { 0x7116d91c, 0xe7e4, 0x47ce, { 0xb8, 0xc6, 0xec, 0x81, 0x68, 0xf4, 0x37, 0xe5 } };
static const IID D3D_IID_ID3D12GraphicsCommandList = { 0x5b160d0f, 0xac1b, 0x4185, { 0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55 } };
static const IID D3D_IID_ID3D12Fence = { 0x0a753dcf, 0xc4d8, 0x4b91, { 0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76 } };
static const IID D3D_IID_ID3D12RootSignature = { 0xc54a6b66, 0x72df, 0x4ee8, { 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14 } };
static const IID D3D_IID_ID3D12PipelineState = { 0x765a30f3, 0xf624, 0x4c6f, { 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 } };
static const IID D3D_IID_ID3D12Debug = { 0x344488b7, 0x6846, 0x474b, { 0xb9, 0x89, 0xf0, 0x27, 0x44, 0x82, 0x45, 0xe0 } };
static const IID D3D_IID_ID3D12InfoQueue = { 0x0742a90b, 0xc387, 0x483f, { 0xb9, 0x46, 0x30, 0xa7, 0xe4, 0xe6, 0x14, 0x58 } };

static const char *D3D12ShaderProfiles[3] = { "vs_5_1", "ps_5_1", "cs_5_1" };

/* Enums */

typedef enum D3D12BufferType
{
    D3D12_BUFFER_TYPE_GPU,
    D3D12_BUFFER_TYPE_UNIFORM,
    D3D12_BUFFER_TYPE_UPLOAD,
    D3D12_BUFFER_TYPE_DOWNLOAD
} D3D12BufferType;

/* Conversions */

static DXGI_FORMAT SwapchainCompositionToTextureFormat[] = {
    DXGI_FORMAT_B8G8R8A8_UNORM,                /* SDR */
    DXGI_FORMAT_B8G8R8A8_UNORM, /* SDR_SRGB */ /* NOTE: The RTV uses the sRGB format */
    DXGI_FORMAT_R16G16B16A16_FLOAT,            /* HDR */
    DXGI_FORMAT_R10G10B10A2_UNORM,             /* HDR_ADVANCED*/
};

static DXGI_COLOR_SPACE_TYPE SwapchainCompositionToColorSpace[] = {
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,   /* SDR */
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,   /* SDR_SRGB */
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,   /* HDR */
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 /* HDR_ADVANCED */
};

static D3D12_BLEND SDLToD3D12_BlendFactor[] = {
    D3D12_BLEND_ZERO,             /* ZERO */
    D3D12_BLEND_ONE,              /* ONE */
    D3D12_BLEND_SRC_COLOR,        /* SRC_COLOR */
    D3D12_BLEND_INV_SRC_COLOR,    /* ONE_MINUS_SRC_COLOR */
    D3D12_BLEND_DEST_COLOR,       /* DST_COLOR */
    D3D12_BLEND_INV_DEST_COLOR,   /* ONE_MINUS_DST_COLOR */
    D3D12_BLEND_SRC_ALPHA,        /* SRC_ALPHA */
    D3D12_BLEND_INV_SRC_ALPHA,    /* ONE_MINUS_SRC_ALPHA */
    D3D12_BLEND_DEST_ALPHA,       /* DST_ALPHA */
    D3D12_BLEND_INV_DEST_ALPHA,   /* ONE_MINUS_DST_ALPHA */
    D3D12_BLEND_BLEND_FACTOR,     /* CONSTANT_COLOR */
    D3D12_BLEND_INV_BLEND_FACTOR, /* ONE_MINUS_CONSTANT_COLOR */
    D3D12_BLEND_SRC_ALPHA_SAT,    /* SRC_ALPHA_SATURATE */
};

static D3D12_BLEND SDLToD3D12_BlendFactorAlpha[] = {
    D3D12_BLEND_ZERO,             /* ZERO */
    D3D12_BLEND_ONE,              /* ONE */
    D3D12_BLEND_SRC_ALPHA,        /* SRC_COLOR */
    D3D12_BLEND_INV_SRC_ALPHA,    /* ONE_MINUS_SRC_COLOR */
    D3D12_BLEND_DEST_ALPHA,       /* DST_COLOR */
    D3D12_BLEND_INV_DEST_ALPHA,   /* ONE_MINUS_DST_COLOR */
    D3D12_BLEND_SRC_ALPHA,        /* SRC_ALPHA */
    D3D12_BLEND_INV_SRC_ALPHA,    /* ONE_MINUS_SRC_ALPHA */
    D3D12_BLEND_DEST_ALPHA,       /* DST_ALPHA */
    D3D12_BLEND_INV_DEST_ALPHA,   /* ONE_MINUS_DST_ALPHA */
    D3D12_BLEND_BLEND_FACTOR,     /* CONSTANT_COLOR */
    D3D12_BLEND_INV_BLEND_FACTOR, /* ONE_MINUS_CONSTANT_COLOR */
    D3D12_BLEND_SRC_ALPHA_SAT,    /* SRC_ALPHA_SATURATE */
};

static D3D12_BLEND_OP SDLToD3D12_BlendOp[] = {
    D3D12_BLEND_OP_ADD,          /* ADD */
    D3D12_BLEND_OP_SUBTRACT,     /* SUBTRACT */
    D3D12_BLEND_OP_REV_SUBTRACT, /* REVERSE_SUBTRACT */
    D3D12_BLEND_OP_MIN,          /* MIN */
    D3D12_BLEND_OP_MAX           /* MAX */
};

static DXGI_FORMAT SDLToD3D12_TextureFormat[] = {
    DXGI_FORMAT_R8G8B8A8_UNORM,       /* R8G8B8A8 */
    DXGI_FORMAT_B8G8R8A8_UNORM,       /* B8G8R8A8 */
    DXGI_FORMAT_B5G6R5_UNORM,         /* B5G6R5 */
    DXGI_FORMAT_B5G5R5A1_UNORM,       /* B5G5R5A1 */
    DXGI_FORMAT_B4G4R4A4_UNORM,       /* B4G4R4A4 */
    DXGI_FORMAT_R10G10B10A2_UNORM,    /* R10G10B10A2 */
    DXGI_FORMAT_R16G16_UNORM,         /* R16G16 */
    DXGI_FORMAT_R16G16B16A16_UNORM,   /* R16G16B16A16 */
    DXGI_FORMAT_R8_UNORM,             /* R8 */
    DXGI_FORMAT_A8_UNORM,             /* A8 */
    DXGI_FORMAT_BC1_UNORM,            /* BC1 */
    DXGI_FORMAT_BC2_UNORM,            /* BC2 */
    DXGI_FORMAT_BC3_UNORM,            /* BC3 */
    DXGI_FORMAT_BC7_UNORM,            /* BC7 */
    DXGI_FORMAT_R8G8_SNORM,           /* R8G8_SNORM */
    DXGI_FORMAT_R8G8B8A8_SNORM,       /* R8G8B8A8_SNORM */
    DXGI_FORMAT_R16_FLOAT,            /* R16_SFLOAT */
    DXGI_FORMAT_R16G16_FLOAT,         /* R16G16_SFLOAT */
    DXGI_FORMAT_R16G16B16A16_FLOAT,   /* R16G16B16A16_SFLOAT */
    DXGI_FORMAT_R32_FLOAT,            /* R32_SFLOAT */
    DXGI_FORMAT_R32G32_FLOAT,         /* R32G32_SFLOAT */
    DXGI_FORMAT_R32G32B32A32_FLOAT,   /* R32G32B32A32_SFLOAT */
    DXGI_FORMAT_R8_UINT,              /* R8_UINT */
    DXGI_FORMAT_R8G8_UINT,            /* R8G8_UINT */
    DXGI_FORMAT_R8G8B8A8_UINT,        /* R8G8B8A8_UINT */
    DXGI_FORMAT_R16_UINT,             /* R16_UINT */
    DXGI_FORMAT_R16G16_UINT,          /* R16G16_UINT */
    DXGI_FORMAT_R16G16B16A16_UINT,    /* R16G16B16A16_UINT */
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  /* R8G8B8A8_SRGB */
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  /* B8G8R8A8_SRGB */
    DXGI_FORMAT_BC3_UNORM_SRGB,       /* BC3_SRGB */
    DXGI_FORMAT_BC7_UNORM_SRGB,       /* BC7_SRGB */
    DXGI_FORMAT_D16_UNORM,            /* D16_UNORM */
    DXGI_FORMAT_D24_UNORM_S8_UINT,    /* D24_UNORM */
    DXGI_FORMAT_D32_FLOAT,            /* D32_SFLOAT */
    DXGI_FORMAT_D24_UNORM_S8_UINT,    /* D24_UNORM_S8_UINT */
    DXGI_FORMAT_D32_FLOAT_S8X24_UINT, /* D32_SFLOAT_S8_UINT */
};

static D3D12_COMPARISON_FUNC SDLToD3D12_CompareOp[] = {
    D3D12_COMPARISON_FUNC_NEVER,         /* NEVER */
    D3D12_COMPARISON_FUNC_LESS,          /* LESS */
    D3D12_COMPARISON_FUNC_EQUAL,         /* EQUAL */
    D3D12_COMPARISON_FUNC_LESS_EQUAL,    /* LESS_OR_EQUAL */
    D3D12_COMPARISON_FUNC_GREATER,       /* GREATER */
    D3D12_COMPARISON_FUNC_NOT_EQUAL,     /* NOT_EQUAL */
    D3D12_COMPARISON_FUNC_GREATER_EQUAL, /* GREATER_OR_EQUAL */
    D3D12_COMPARISON_FUNC_ALWAYS         /* ALWAYS */
};

static D3D12_STENCIL_OP SDLToD3D12_StencilOp[] = {
    D3D12_STENCIL_OP_KEEP,     /* KEEP */
    D3D12_STENCIL_OP_ZERO,     /* ZERO */
    D3D12_STENCIL_OP_REPLACE,  /* REPLACE */
    D3D12_STENCIL_OP_INCR_SAT, /* INCREMENT_AND_CLAMP */
    D3D12_STENCIL_OP_DECR_SAT, /* DECREMENT_AND_CLAMP */
    D3D12_STENCIL_OP_INVERT,   /* INVERT */
    D3D12_STENCIL_OP_INCR,     /* INCREMENT_AND_WRAP */
    D3D12_STENCIL_OP_DECR      /* DECREMENT_AND_WRAP */
};

static D3D12_CULL_MODE SDLToD3D12_CullMode[] = {
    D3D12_CULL_MODE_NONE,  /* NONE */
    D3D12_CULL_MODE_FRONT, /* FRONT */
    D3D12_CULL_MODE_BACK   /* BACK */
};

static D3D12_FILL_MODE SDLToD3D12_FillMode[] = {
    D3D12_FILL_MODE_SOLID,    /* FILL */
    D3D12_FILL_MODE_WIREFRAME /* LINE */
};

static D3D12_INPUT_CLASSIFICATION SDLToD3D12_InputRate[] = {
    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,  /* VERTEX */
    D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA /* INSTANCE */
};

static DXGI_FORMAT SDLToD3D12_VertexFormat[] = {
    DXGI_FORMAT_R32_UINT,           /* UINT */
    DXGI_FORMAT_R32_FLOAT,          /* FLOAT */
    DXGI_FORMAT_R32G32_FLOAT,       /* VECTOR2 */
    DXGI_FORMAT_R32G32B32_FLOAT,    /* VECTOR3 */
    DXGI_FORMAT_R32G32B32A32_FLOAT, /* VECTOR4 */
    DXGI_FORMAT_R8G8B8A8_UNORM,     /* COLOR */
    DXGI_FORMAT_R8G8B8A8_UINT,      /* BYTE4 */
    DXGI_FORMAT_R16G16_SINT,        /* SHORT2 */
    DXGI_FORMAT_R16G16B16A16_SINT,  /* SHORT4 */
    DXGI_FORMAT_R16G16_SNORM,       /* NORMALIZEDSHORT2 */
    DXGI_FORMAT_R16G16B16A16_SNORM, /* NORMALIZEDSHORT4 */
    DXGI_FORMAT_R16G16_FLOAT,       /* HALFVECTOR2 */
    DXGI_FORMAT_R16G16B16A16_FLOAT  /* HALFVECTOR4 */
};

static int SDLToD3D12_SampleCount[] = {
    1, /* SDL_GPU_SAMPLECOUNT_1 */
    2, /* SDL_GPU_SAMPLECOUNT_2 */
    4, /* SDL_GPU_SAMPLECOUNT_4 */
    8, /* SDL_GPU_SAMPLECOUNT_8 */
};

static D3D12_PRIMITIVE_TOPOLOGY SDLToD3D12_PrimitiveType[] = {
    D3D_PRIMITIVE_TOPOLOGY_POINTLIST,    // POINTLIST
    D3D_PRIMITIVE_TOPOLOGY_LINELIST,     // LINELIST
    D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,    // LINESTRIP
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, // TRIANGLELIST
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP // TRIANGLESTRIP
};

static D3D12_TEXTURE_ADDRESS_MODE SDLToD3D12_SamplerAddressMode[] = {
    D3D12_TEXTURE_ADDRESS_MODE_WRAP,   /* REPEAT */
    D3D12_TEXTURE_ADDRESS_MODE_MIRROR, /* MIRRORED_REPEAT */
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP   /* CLAMP_TO_EDGE */
};

static D3D12_FILTER SDLToD3D12_Filter(
    SDL_GpuFilter minFilter,
    SDL_GpuFilter magFilter,
    SDL_GpuSamplerMipmapMode mipmapMode,
    SDL_bool comparisonEnabled,
    SDL_bool anisotropyEnabled)
{
    D3D12_FILTER result = D3D12_ENCODE_BASIC_FILTER(
        minFilter == SDL_GPU_FILTER_LINEAR,
        magFilter == SDL_GPU_FILTER_LINEAR,
        mipmapMode == SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
        comparisonEnabled);

    if (anisotropyEnabled) {
        result |= D3D12_ANISOTROPIC_FILTERING_BIT;
    }

    return result;
}

/* Structures */
typedef struct D3D12Renderer D3D12Renderer;
typedef struct D3D12CommandBufferPool D3D12CommandBufferPool;
typedef struct D3D12CommandBuffer D3D12CommandBuffer;
typedef struct D3D12Texture D3D12Texture;
typedef struct D3D12Shader D3D12Shader;
typedef struct D3D12GraphicsPipeline D3D12GraphicsPipeline;
typedef struct D3D12Buffer D3D12Buffer;
typedef struct D3D12BufferContainer D3D12BufferContainer;
typedef struct D3D12UniformBuffer D3D12UniformBuffer;
typedef struct D3D12DescriptorHeap D3D12DescriptorHeap;

typedef struct D3D12Fence
{
    ID3D12Fence *handle;
    HANDLE event; /* used for blocking */
    SDL_AtomicInt referenceCount;
} D3D12Fence;

struct D3D12DescriptorHeap
{
    ID3D12DescriptorHeap *handle;
    D3D12_DESCRIPTOR_HEAP_TYPE heapType;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptorHeapCPUStart;
    D3D12_GPU_DESCRIPTOR_HANDLE descriptorHeapGPUStart; /* only exists if staging is SDL_TRUE */
    Uint32 maxDescriptors;
    Uint32 descriptorSize;
    SDL_bool staging;

    Uint32 currentDescriptorIndex;

    Uint32 *inactiveDescriptorIndices; /* only exists if staging is SDL_TRUE */
    Uint32 inactiveDescriptorCount;
};

typedef struct D3D12DescriptorHeapPool
{
    Uint32 capacity;
    Uint32 count;
    D3D12DescriptorHeap **heaps;
    SDL_Mutex *lock;
} D3D12DescriptorHeapPool;

typedef struct D3D12CPUDescriptor
{
    D3D12DescriptorHeap *heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
    Uint32 cpuHandleIndex;
} D3D12CPUDescriptor;

typedef struct D3D12TextureContainer
{
    SDL_GpuTextureCreateInfo createInfo;

    D3D12Texture *activeTexture;

    D3D12Texture **textures;
    Uint32 textureCapacity;
    Uint32 textureCount;

    /* Swapchain images cannot be cycled */
    SDL_bool canBeCycled;

    char *debugName;
} D3D12TextureContainer;

/* TODO: how should we represent null views? */
typedef struct D3D12TextureSubresource
{
    D3D12Texture *parent;
    Uint32 layer;
    Uint32 level;
    Uint32 index;

    D3D12CPUDescriptor rtvHandle; /* NULL if not a color target */
    D3D12CPUDescriptor dsvHandle; /* NULL if not a depth stencil target */
    D3D12CPUDescriptor srvHandle; /* NULL if not a storage texture */
    D3D12CPUDescriptor uavHandle; /* NULL if not a compute storage write texture */

    SDL_AtomicInt referenceCount;
} D3D12TextureSubresource;

struct D3D12Texture
{
    D3D12TextureContainer *container;
    Uint32 containerIndex;

    D3D12TextureSubresource *subresources;
    Uint32 subresourceCount; /* layerCount * levelCount */

    ID3D12Resource *resource;
    D3D12CPUDescriptor srvHandle;
};

typedef struct D3D12Sampler
{
    SDL_GpuSamplerCreateInfo createInfo;
    D3D12CPUDescriptor handle;
} D3D12Sampler;

typedef struct D3D12WindowData
{
    SDL_Window *window;
    IDXGISwapChain3 *swapchain;
    SDL_GpuPresentMode presentMode;
    SDL_GpuSwapchainComposition swapchainComposition;
    DXGI_FORMAT swapchainFormat;
    DXGI_COLOR_SPACE_TYPE swapchainColorSpace;
    Uint32 frameCounter;

    D3D12TextureContainer textureContainers[MAX_FRAMES_IN_FLIGHT];
    D3D12Fence *inFlightFences[MAX_FRAMES_IN_FLIGHT];
} D3D12WindowData;

typedef struct D3D12PresentData
{
    D3D12WindowData *windowData;
    Uint32 swapchainImageIndex;
} D3D12PresentData;

struct D3D12Renderer
{
    void *dxgidebug_dll;
    IDXGIDebug *dxgiDebug;
    IDXGIInfoQueue *dxgiInfoQueue;
    ID3D12Debug *d3d12Debug;
    void *d3dcompiler_dll;
    PFN_D3DCOMPILE D3DCompile_func;
    void *dxgi_dll;
    IDXGIFactory4 *factory;
    SDL_bool supportsTearing;
    IDXGIAdapter1 *adapter;
    void *d3d12_dll;
    ID3D12Device *device;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE D3D12SerializeRootSignature_func;

    ID3D12CommandQueue *commandQueue;

    SDL_bool debugMode;
    SDL_bool UMA;

    /* Resources */

    D3D12CommandBuffer **availableCommandBuffers;
    Uint32 availableCommandBufferCount;
    Uint32 availableCommandBufferCapacity;

    D3D12CommandBuffer **submittedCommandBuffers;
    Uint32 submittedCommandBufferCount;
    Uint32 submittedCommandBufferCapacity;

    D3D12UniformBuffer **uniformBufferPool;
    Uint32 uniformBufferPoolCount;
    Uint32 uniformBufferPoolCapacity;

    D3D12WindowData **claimedWindows;
    Uint32 claimedWindowCount;
    Uint32 claimedWindowCapacity;

    D3D12Fence **availableFences;
    Uint32 availableFenceCount;
    Uint32 availableFenceCapacity;

    D3D12DescriptorHeap *stagingDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
    D3D12DescriptorHeapPool descriptorHeapPools[2];

    /* Locks */
    SDL_Mutex *stagingDescriptorHeapLock;
    SDL_Mutex *acquireCommandBufferLock;
    SDL_Mutex *acquireUniformBufferLock;
    SDL_Mutex *submitLock;
    SDL_Mutex *windowLock;
    SDL_Mutex *fenceLock;
};

struct D3D12CommandBuffer
{
    // reserved for SDL_gpu
    CommandBufferCommonHeader common;

    // non owning parent reference
    D3D12Renderer *renderer;

    ID3D12CommandAllocator *commandAllocator;
    ID3D12GraphicsCommandList *graphicsCommandList;
    D3D12Fence *inFlightFence;
    SDL_bool autoReleaseFence;

    /* Presentation data */
    D3D12PresentData *presentDatas;
    Uint32 presentDataCount;
    Uint32 presentDataCapacity;

    Uint32 colorAttachmentCount;
    D3D12TextureSubresource *colorAttachmentTextureSubresources[MAX_COLOR_TARGET_BINDINGS];
    D3D12TextureSubresource *depthStencilTextureSubresource;
    D3D12GraphicsPipeline *currentGraphicsPipeline;

    /* Set at acquire time */
    D3D12DescriptorHeap *gpuDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER + 1];

    D3D12UniformBuffer **usedUniformBuffers;
    Uint32 usedUniformBufferCount;
    Uint32 usedUniformBufferCapacity;

    /* Resource slot state */
    SDL_bool needVertexSamplerBind;
    SDL_bool needVertexStorageTextureBind;
    SDL_bool needVertexStorageBufferBind;
    SDL_bool needVertexUniformBufferBind[MAX_UNIFORM_BUFFERS_PER_STAGE];
    SDL_bool needFragmentSamplerBind;
    SDL_bool needFragmentStorageTextureBind;
    SDL_bool needFragmentStorageBufferBind;
    SDL_bool needFragmentUniformBufferBind[MAX_UNIFORM_BUFFERS_PER_STAGE];

    D3D12Texture *vertexSamplerTextures[MAX_TEXTURE_SAMPLERS_PER_STAGE];
    D3D12Sampler *vertexSamplers[MAX_TEXTURE_SAMPLERS_PER_STAGE];

    D3D12TextureSubresource *vertexStorageTextureSlices[MAX_STORAGE_TEXTURES_PER_STAGE];
    D3D12Buffer *vertexStorageBuffers[MAX_STORAGE_BUFFERS_PER_STAGE];

    D3D12Texture *fragmentSamplerTextures[MAX_TEXTURE_SAMPLERS_PER_STAGE];
    D3D12Sampler *fragmentSamplers[MAX_TEXTURE_SAMPLERS_PER_STAGE];

    D3D12TextureSubresource *fragmentStorageTextureSlices[MAX_STORAGE_TEXTURES_PER_STAGE];
    D3D12Buffer *fragmentStorageBuffers[MAX_STORAGE_BUFFERS_PER_STAGE];

    D3D12UniformBuffer *vertexUniformBuffers[MAX_UNIFORM_BUFFERS_PER_STAGE];
    D3D12UniformBuffer *fragmentUniformBuffers[MAX_UNIFORM_BUFFERS_PER_STAGE];

    /* Resource tracking */
    D3D12TextureSubresource **usedTextureSubresources;
    Uint32 usedTextureSubresourceCount;
    Uint32 usedTextureSubresourceCapacity;
};

struct D3D12Shader
{
    // todo cleanup
    void *bytecode;
    size_t bytecodeSize;

    Uint32 samplerCount;
    Uint32 uniformBufferCount;
    Uint32 storageBufferCount;
    Uint32 storageTextureCount;
};

struct D3D12GraphicsPipeline
{
    ID3D12PipelineState *pipelineState;
    ID3D12RootSignature *rootSignature;
    SDL_GpuPrimitiveType primitiveType;
    float blendConstants[4];
    Uint32 stencilRef;
    Uint32 vertexSamplerCount;
    Uint32 vertexUniformBufferCount;
    Uint32 vertexStorageBufferCount;
    Uint32 vertexStorageTextureCount;

    Uint32 fragmentSamplerCount;
    Uint32 fragmentUniformBufferCount;
    Uint32 fragmentStorageBufferCount;
    Uint32 fragmentStorageTextureCount;
};

struct D3D12Buffer
{
    D3D12BufferContainer *container;
    Uint32 containerIndex;

    ID3D12Resource *handle;
    D3D12CPUDescriptor uavDescriptor;
    D3D12CPUDescriptor srvDescriptor;
    D3D12CPUDescriptor cbvDescriptor;
    D3D12_GPU_VIRTUAL_ADDRESS virtualAddress;
    Uint8 *mapPointer; /* NULL except for upload buffers */
    SDL_AtomicInt referenceCount;
};

struct D3D12BufferContainer
{
    SDL_GpuBufferUsageFlags usageFlags;
    Uint32 size;
    D3D12BufferType type;

    D3D12Buffer *activeBuffer;

    D3D12Buffer **buffers;
    Uint32 bufferCapacity;
    Uint32 bufferCount;

    D3D12_RESOURCE_DESC bufferDesc;

    char *debugName;
};

struct D3D12UniformBuffer
{
    D3D12Buffer *buffer;
    Uint32 writeOffset;
    Uint32 drawOffset;
    Uint32 currentBlockSize;
};

/* Foward function declarations */

static void D3D12_UnclaimWindow(SDL_GpuRenderer *driverData, SDL_Window *window);
static void D3D12_Wait(SDL_GpuRenderer *driverData);
static void D3D12_WaitForFences(SDL_GpuRenderer *driverData, SDL_bool waitAll, SDL_GpuFence **pFences, Uint32 fenceCount);

/* Logging */

static void
D3D12_INTERNAL_LogError(
    ID3D12Device *device,
    const char *msg,
    HRESULT res)
{
#define MAX_ERROR_LEN 1024 /* FIXME: Arbitrary! */

    /* Buffer for text, ensure space for \0 terminator after buffer */
    char wszMsgBuff[MAX_ERROR_LEN + 1];
    DWORD dwChars; /* Number of chars returned. */

    if (res == DXGI_ERROR_DEVICE_REMOVED) {
        if (device) {
            res = ID3D12Device_GetDeviceRemovedReason(device);
        }
    }

    /* Try to get the message from the system errors. */
    dwChars = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        res,
        0,
        wszMsgBuff,
        MAX_ERROR_LEN,
        NULL);

    /* No message? Screw it, just post the code. */
    if (dwChars == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "%s! Error Code: " HRESULT_FMT, msg, res);
        return;
    }

    /* Ensure valid range */
    dwChars = SDL_min(dwChars, MAX_ERROR_LEN);

    /* Trim whitespace from tail of message */
    while (dwChars > 0) {
        if (wszMsgBuff[dwChars - 1] <= ' ') {
            dwChars--;
        } else {
            break;
        }
    }

    /* Ensure null-terminated string */
    wszMsgBuff[dwChars] = '\0';

    SDL_LogError(SDL_LOG_CATEGORY_GPU, "%s! Error Code: %s " HRESULT_FMT, msg, wszMsgBuff, res);
}

/* Debug Naming */

static void D3D12_INTERNAL_SetResourceName(
    D3D12Renderer *renderer,
    ID3D12Resource *resource,
    const char *text)
{
    if (renderer->debugMode) {
        ID3D12DeviceChild_SetPrivateData(
            resource,
            &D3D_IID_D3DDebugObjectName,
            (UINT)SDL_strlen(text),
            text);
    }
}

/* Release / Cleanup */

static void D3D12_INTERNAL_ReleaseFenceToPool(
    D3D12Renderer *renderer,
    D3D12Fence *fence)
{
    SDL_LockMutex(renderer->fenceLock);

    EXPAND_ARRAY_IF_NEEDED(
        renderer->availableFences,
        D3D12Fence *,
        renderer->availableFenceCount + 1,
        renderer->availableFenceCapacity,
        renderer->availableFenceCapacity * 2);

    renderer->availableFences[renderer->availableFenceCount] = fence;
    renderer->availableFenceCount += 1;

    SDL_UnlockMutex(renderer->fenceLock);
}

static void D3D12_ReleaseFence(
    SDL_GpuRenderer *driverData,
    SDL_GpuFence *fence)
{
    D3D12Fence *d3d12Fence = (D3D12Fence *)fence;

    if (SDL_AtomicDecRef(&d3d12Fence->referenceCount)) {
        D3D12_INTERNAL_ReleaseFenceToPool(
            (D3D12Renderer *)driverData,
            d3d12Fence);
    }
}

static SDL_bool D3D12_QueryFence(
    SDL_GpuRenderer *driverData,
    SDL_GpuFence *fence)
{
    SDL_assert(SDL_FALSE);
    return SDL_FALSE;
}

static void D3D12_INTERNAL_DestroyDescriptorHeap(D3D12DescriptorHeap *descriptorHeap)
{
    SDL_free(descriptorHeap->inactiveDescriptorIndices);
    ID3D12DescriptorHeap_Release(descriptorHeap->handle);
}

static void D3D12_INTERNAL_DestroyCommandBuffer(D3D12CommandBuffer *commandBuffer)
{
    ID3D12GraphicsCommandList_Release(commandBuffer->graphicsCommandList);
    ID3D12CommandAllocator_Release(commandBuffer->commandAllocator);
    SDL_free(commandBuffer->presentDatas);
    SDL_free(commandBuffer->usedUniformBuffers);
}

static void D3D12_INTERNAL_DestroyFence(D3D12Fence *fence)
{
    ID3D12Fence_Release(fence->handle);
    CloseHandle(fence->event);
}

/* FIXME: just move this into DestroyDevice */
static void D3D12_INTERNAL_DestroyRenderer(D3D12Renderer *renderer)
{
    /* Flush any remaining GPU work... */
    D3D12_Wait((SDL_GpuRenderer *)renderer);

    /* Release window data */
    for (Sint32 i = renderer->claimedWindowCount - 1; i >= 0; i -= 1) {
        D3D12_UnclaimWindow((SDL_GpuRenderer *)renderer, renderer->claimedWindows[i]->window);
    }
    SDL_free(renderer->claimedWindows);

    /* Clean up descriptor heaps */
    for (Uint32 i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; i += 1) {
        if (renderer->stagingDescriptorHeaps[i]) {
            D3D12_INTERNAL_DestroyDescriptorHeap(renderer->stagingDescriptorHeaps[i]);
            SDL_free(renderer->stagingDescriptorHeaps[i]);
            renderer->stagingDescriptorHeaps[i] = NULL;
        }
    }

    for (Uint32 i = 0; i < 2; i += 1) {
        if (renderer->descriptorHeapPools[i].heaps) {
            for (Uint32 j = 0; j < renderer->descriptorHeapPools[i].count; j += 1) {
                if (renderer->descriptorHeapPools[i].heaps[j]) {
                    D3D12_INTERNAL_DestroyDescriptorHeap(renderer->descriptorHeapPools[i].heaps[j]);
                    SDL_free(renderer->descriptorHeapPools[i].heaps[j]);
                    renderer->descriptorHeapPools[i].heaps[j] = NULL;
                }
            }
            SDL_free(renderer->descriptorHeapPools[i].heaps);
        }
        if (renderer->descriptorHeapPools[i].lock) {
            SDL_DestroyMutex(renderer->descriptorHeapPools[i].lock);
            renderer->descriptorHeapPools[i].lock = NULL;
        }
    }

    /* Release command buffers */
    for (Uint32 i = 0; i < renderer->availableCommandBufferCount; i += 1) {
        if (renderer->availableCommandBuffers[i]) {
            D3D12_INTERNAL_DestroyCommandBuffer(renderer->availableCommandBuffers[i]);
            SDL_free(renderer->availableCommandBuffers[i]);
            renderer->availableCommandBuffers[i] = NULL;
        }
    }

    /* Release fences */
    for (Uint32 i = 0; i < renderer->availableFenceCount; i += 1) {
        if (renderer->availableFences[i]) {
            D3D12_INTERNAL_DestroyFence(renderer->availableFences[i]);
            SDL_free(renderer->availableFences[i]);
            renderer->availableFences[i] = NULL;
        }
    }

    /* Clean up allocations */
    SDL_free(renderer->availableCommandBuffers);
    SDL_free(renderer->submittedCommandBuffers);
    SDL_free(renderer->availableFences);
    SDL_free(renderer->uniformBufferPool);

    /* Tear down D3D12 objects */
    if (renderer->commandQueue) {
        ID3D12CommandQueue_Release(renderer->commandQueue);
        renderer->commandQueue = NULL;
    }
    if (renderer->device) {
        ID3D12Device_Release(renderer->device);
        renderer->device = NULL;
    }
    if (renderer->adapter) {
        IDXGIAdapter1_Release(renderer->adapter);
        renderer->adapter = NULL;
    }
    if (renderer->factory) {
        IDXGIFactory4_Release(renderer->factory);
        renderer->factory = NULL;
    }
    if (renderer->dxgiDebug) {
        IDXGIDebug_ReportLiveObjects(
            renderer->dxgiDebug,
            D3D_IID_DXGI_DEBUG_ALL,
            DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_DETAIL);
        IDXGIDebug_Release(renderer->dxgiDebug);
        renderer->dxgiDebug = NULL;
    }
    if (renderer->d3d12_dll) {
        SDL_UnloadObject(renderer->d3d12_dll);
        renderer->d3d12_dll = NULL;
    }
    if (renderer->dxgi_dll) {
        SDL_UnloadObject(renderer->dxgi_dll);
        renderer->dxgi_dll = NULL;
    }
    if (renderer->d3dcompiler_dll) {
        SDL_UnloadObject(renderer->d3dcompiler_dll);
        renderer->d3dcompiler_dll = NULL;
    }
    if (renderer->dxgidebug_dll) {
        SDL_UnloadObject(renderer->dxgidebug_dll);
        renderer->dxgidebug_dll = NULL;
    }
    renderer->D3DCompile_func = NULL;
    renderer->D3D12SerializeRootSignature_func = NULL;

    SDL_DestroyMutex(renderer->stagingDescriptorHeapLock);
    SDL_DestroyMutex(renderer->acquireCommandBufferLock);
    SDL_DestroyMutex(renderer->acquireUniformBufferLock);
    SDL_DestroyMutex(renderer->submitLock);
    SDL_DestroyMutex(renderer->windowLock);
    SDL_DestroyMutex(renderer->fenceLock);
}

static void D3D12_INTERNAL_DestroyRendererAndFree(D3D12Renderer **rendererRef)
{
    D3D12Renderer *renderer;
    renderer = *rendererRef;
    if (!renderer)
        return;
    *rendererRef = NULL;
    D3D12_INTERNAL_DestroyRenderer(renderer);
    SDL_free(renderer);
}

static void D3D12_DestroyDevice(SDL_GpuDevice *device)
{
    D3D12Renderer *renderer = (D3D12Renderer *)device->driverData;
    if (renderer) {
        D3D12_INTERNAL_DestroyRenderer(renderer);
        SDL_free(renderer);
    }
    SDL_free(device);
}

/* Barriers */

static inline Uint32 D3D12_INTERNAL_CalcSubresource(
    Uint32 mipLevel,
    Uint32 layer,
    Uint32 numLevels)
{
    return mipLevel * (layer * numLevels);
}

static void D3D12_INTERNAL_ImageMemoryBarrier(
    D3D12CommandBuffer *commandBuffer,
    D3D12_RESOURCE_STATES sourceUsageMode,
    D3D12_RESOURCE_STATES destinationUsageMode,
    D3D12TextureSubresource *textureSlice)
{
    D3D12_RESOURCE_BARRIER barrierDesc[2]; /* 0: transition barrier, 1: UAV barrier (optional) */
    Uint32 numBarriers = 1;

    barrierDesc[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrierDesc[0].Flags = 0;
    barrierDesc[0].Transition.pResource = textureSlice->parent->resource;
    barrierDesc[0].Transition.StateBefore = sourceUsageMode;
    barrierDesc[0].Transition.StateAfter = destinationUsageMode;
    barrierDesc[0].Transition.Subresource = D3D12_INTERNAL_CalcSubresource(
        textureSlice->level,
        textureSlice->layer,
        textureSlice->parent->container->createInfo.levelCount);

    if (textureSlice->parent->container->createInfo.usageFlags & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE_BIT) {
        barrierDesc[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrierDesc[1].Flags = 0;
        barrierDesc[1].UAV.pResource = textureSlice->parent->resource;

        numBarriers += 1;
    }

    ID3D12GraphicsCommandList_ResourceBarrier(
        commandBuffer->graphicsCommandList,
        numBarriers,
        barrierDesc);
}

static D3D12_RESOURCE_STATES D3D12_INTERNAL_DefaultTextureUsageMode(SDL_GpuTextureUsageFlags usageFlags)
{
    /* NOTE: order matters here! */

    if (usageFlags & SDL_GPU_TEXTUREUSAGE_SAMPLER_BIT) {
        return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    } else if (usageFlags & SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ_BIT) {
        return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    } else if (usageFlags & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET_BIT) {
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    } else if (usageFlags & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET_BIT) {
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else if (usageFlags & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ_BIT) {
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    } else if (usageFlags & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE_BIT) {
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Texture has no default usage mode!");
        return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
}

static void D3D12_INTERNAL_TextureSubresourceTransitionFromDefaultUsage(
    D3D12CommandBuffer *commandBuffer,
    D3D12_RESOURCE_STATES destinationUsageMode,
    D3D12TextureSubresource *textureSubresource)
{
    D3D12_INTERNAL_ImageMemoryBarrier(
        commandBuffer,
        D3D12_INTERNAL_DefaultTextureUsageMode(textureSubresource->parent->container->createInfo.usageFlags),
        destinationUsageMode,
        textureSubresource);
}

static void D3D12_INTERNAL_TextureSubresourceTransitionToDefaultUsage(
    D3D12CommandBuffer *commandBuffer,
    D3D12_RESOURCE_STATES sourceUsageMode,
    D3D12TextureSubresource *textureSubresource)
{
    D3D12_INTERNAL_ImageMemoryBarrier(
        commandBuffer,
        sourceUsageMode,
        D3D12_INTERNAL_DefaultTextureUsageMode(textureSubresource->parent->container->createInfo.usageFlags),
        textureSubresource);
}

/* Resource tracking */

#define TRACK_RESOURCE(resource, type, array, count, capacity) \
    Uint32 i;                                                  \
                                                               \
    for (i = 0; i < commandBuffer->count; i += 1) {            \
        if (commandBuffer->array[i] == resource) {             \
            return;                                            \
        }                                                      \
    }                                                          \
                                                               \
    if (commandBuffer->count == commandBuffer->capacity) {     \
        commandBuffer->capacity += 1;                          \
        commandBuffer->array = SDL_realloc(                    \
            commandBuffer->array,                              \
            commandBuffer->capacity * sizeof(type));           \
    }                                                          \
    commandBuffer->array[commandBuffer->count] = resource;     \
    commandBuffer->count += 1;                                 \
    SDL_AtomicIncRef(&resource->referenceCount);

static void D3D12_INTERNAL_TrackTextureSubresource(
    D3D12CommandBuffer *commandBuffer,
    D3D12TextureSubresource *textureSlice)
{
    TRACK_RESOURCE(
        textureSlice,
        D3D12TextureSubresource *,
        usedTextureSubresources,
        usedTextureSubresourceCount,
        usedTextureSubresourceCapacity)
}

#undef TRACK_RESOURCE

/* State Creation */

static D3D12DescriptorHeap *D3D12_INTERNAL_CreateDescriptorHeap(
    D3D12Renderer *renderer,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    Uint32 descriptorCount,
    SDL_bool staging)
{
    D3D12DescriptorHeap *heap;
    ID3D12DescriptorHeap *handle;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc;
    HRESULT res;

    heapDesc.NumDescriptors = descriptorCount;
    heapDesc.Type = type;
    heapDesc.Flags = staging ? D3D12_DESCRIPTOR_HEAP_FLAG_NONE : D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = 0;

    res = ID3D12Device_CreateDescriptorHeap(
        renderer->device,
        &heapDesc,
        &D3D_IID_ID3D12DescriptorHeap,
        (void **)&handle);
    ERROR_CHECK_RETURN("Failed to create descriptor heap!", NULL);

    heap = SDL_malloc(sizeof(D3D12DescriptorHeap));
    heap->handle = handle;
    heap->heapType = type;
    heap->maxDescriptors = descriptorCount;
    heap->staging = staging;
    heap->descriptorSize = ID3D12Device_GetDescriptorHandleIncrementSize(renderer->device, type);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(handle, &heap->descriptorHeapCPUStart);
    if (!staging) {
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(handle, &heap->descriptorHeapGPUStart);
    }

    heap->currentDescriptorIndex = 0;
    heap->inactiveDescriptorCount = 0;
    heap->inactiveDescriptorIndices = NULL;

    if (staging) {
        heap->inactiveDescriptorIndices = SDL_malloc(sizeof(Uint32) * descriptorCount);
    }

    return heap;
}

static D3D12DescriptorHeap *D3D12_INTERNAL_AcquireDescriptorHeapFromPool(
    D3D12CommandBuffer *commandBuffer,
    D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapType)
{
    D3D12DescriptorHeap *result;
    D3D12Renderer *renderer = commandBuffer->renderer;
    D3D12DescriptorHeapPool *pool = &renderer->descriptorHeapPools[descriptorHeapType];

    SDL_LockMutex(pool->lock);
    if (pool->count > 0) {
        result = pool->heaps[pool->count - 1];
        pool->count -= 1;
    } else {
        result = D3D12_INTERNAL_CreateDescriptorHeap(
            renderer,
            descriptorHeapType,
            descriptorHeapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? VIEW_GPU_DESCRIPTOR_COUNT : SAMPLER_GPU_DESCRIPTOR_COUNT,
            SDL_FALSE);
    }
    SDL_UnlockMutex(pool->lock);

    return result;
}

static void D3D12_INTERNAL_ReturnDescriptorHeapToPool(
    D3D12Renderer *renderer,
    D3D12DescriptorHeap *heap)
{
    D3D12DescriptorHeapPool *pool = &renderer->descriptorHeapPools[heap->heapType];

    heap->currentDescriptorIndex = 0;

    SDL_LockMutex(pool->lock);
    if (pool->count >= pool->capacity) {
        pool->capacity *= 2;
        pool->heaps = SDL_realloc(
            pool->heaps,
            pool->capacity * sizeof(D3D12DescriptorHeap *));
    }

    pool->heaps[pool->count] = heap;
    pool->count += 1;
    SDL_UnlockMutex(pool->lock);
}

/*
 * The root signature lets us define "root parameters" which are essentially bind points for resources.
 * These let us define the register ranges as well as the register "space".
 * The register space is akin to the descriptor set index in Vulkan, which allows us to group resources
 * by stage so that the registers from the vertex and fragment shaders don't clobber each other.
 *
 * Most of our root parameters are implemented as "descriptor tables" so we can
 * copy and then point to contiguous descriptor regions.
 * Uniform buffers are the exception - these have to be implemented as raw "root descriptors" so
 * that we can dynamically update the address that the constant buffer view points to.
 *
 * The root signature has a maximum size of 64 DWORDs.
 * A descriptor table uses 1 DWORD.
 * A root descriptor uses 2 DWORDS.
 *
 * The root parameter indices for graphics pipelines are as follows:
 *
 *     0: vertex samplers
 *     1: vertex sampled textures
 *     2: vertex storage textures
 *     3: vertex storage buffers
 *   4-7: vertex uniform buffers
 *     8: fragment samplers
 *     9: fragment sampled textures
 *    10: fragment storage textures
 *    11: fragment storage buffers
 * 12-15: fragment uniform buffers
 *
 * This means our root signature uses 24 DWORDs total, well under the limit.
 */
static ID3D12RootSignature *D3D12_INTERNAL_CreateGraphicsRootSignature(
    D3D12Renderer *renderer,
    D3D12Shader *vertexShader,
    D3D12Shader *fragmentShader)
{
    D3D12_ROOT_PARAMETER rootParameters[MAX_ROOT_SIGNATURE_PARAMETERS];
    D3D12_DESCRIPTOR_RANGE descriptorRanges[MAX_ROOT_SIGNATURE_PARAMETERS];
    Uint32 rangeCount = 0;
    D3D12_DESCRIPTOR_RANGE descriptorRange;

    for (int i = 0; i < MAX_ROOT_SIGNATURE_PARAMETERS; i += 1) {
        SDL_zero(rootParameters[i]);
        SDL_zero(descriptorRanges[i]);
    }

    SDL_zero(descriptorRange);

    /* Vertex Samplers */
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    descriptorRange.NumDescriptors = vertexShader->samplerCount;
    descriptorRange.BaseShaderRegister = 0;
    descriptorRange.RegisterSpace = 0;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptorRanges[rangeCount] = descriptorRange;

    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = vertexShader->samplerCount > 0;
    rootParameters[0].DescriptorTable.pDescriptorRanges = &descriptorRanges[rangeCount];
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rangeCount += 1;

    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = vertexShader->samplerCount;
    descriptorRange.BaseShaderRegister = 0;
    descriptorRange.RegisterSpace = 0;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptorRanges[rangeCount] = descriptorRange;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = vertexShader->samplerCount > 0;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &descriptorRanges[rangeCount];
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rangeCount += 1;

    /* Vertex storage textures */
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = vertexShader->storageTextureCount;
    descriptorRange.BaseShaderRegister = vertexShader->samplerCount;
    descriptorRange.RegisterSpace = 0;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptorRanges[rangeCount] = descriptorRange;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = vertexShader->storageTextureCount > 0;
    rootParameters[2].DescriptorTable.pDescriptorRanges = &descriptorRanges[rangeCount];
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rangeCount += 1;

    /* Vertex storage buffers */
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = vertexShader->storageBufferCount;
    descriptorRange.BaseShaderRegister = vertexShader->samplerCount + vertexShader->storageTextureCount;
    descriptorRange.RegisterSpace = 0;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptorRanges[rangeCount] = descriptorRange;

    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[3].DescriptorTable.NumDescriptorRanges = vertexShader->storageBufferCount > 0;
    rootParameters[3].DescriptorTable.pDescriptorRanges = &descriptorRanges[rangeCount];
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rangeCount += 1;

    /* Vertex Uniforms */
    for (Uint32 i = 0; i < MAX_UNIFORM_BUFFERS_PER_STAGE; i += 1) {
        rootParameters[4 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[4 + i].Descriptor.ShaderRegister = i;
        rootParameters[4 + i].Descriptor.RegisterSpace = 1;
        rootParameters[4 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }

    /* Fragment Samplers */
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    descriptorRange.NumDescriptors = fragmentShader->samplerCount;
    descriptorRange.BaseShaderRegister = 0;
    descriptorRange.RegisterSpace = 2;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptorRanges[rangeCount] = descriptorRange;

    rootParameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[8].DescriptorTable.NumDescriptorRanges = fragmentShader->samplerCount > 0;
    rootParameters[8].DescriptorTable.pDescriptorRanges = &descriptorRanges[rangeCount];
    rootParameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rangeCount += 1;

    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = vertexShader->samplerCount;
    descriptorRange.BaseShaderRegister = 0;
    descriptorRange.RegisterSpace = 2;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptorRanges[rangeCount] = descriptorRange;

    rootParameters[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[9].DescriptorTable.NumDescriptorRanges = fragmentShader->samplerCount > 0;
    rootParameters[9].DescriptorTable.pDescriptorRanges = &descriptorRanges[rangeCount];
    rootParameters[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rangeCount += 1;

    /* Fragment Storage Textures */
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = fragmentShader->storageTextureCount;
    descriptorRange.BaseShaderRegister = fragmentShader->samplerCount;
    descriptorRange.RegisterSpace = 2;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptorRanges[rangeCount] = descriptorRange;

    rootParameters[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[10].DescriptorTable.NumDescriptorRanges = fragmentShader->storageTextureCount > 0;
    rootParameters[10].DescriptorTable.pDescriptorRanges = &descriptorRanges[rangeCount];
    rootParameters[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rangeCount += 1;

    /* Fragment Storage Buffers */
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = fragmentShader->storageBufferCount;
    descriptorRange.BaseShaderRegister = fragmentShader->samplerCount + fragmentShader->storageTextureCount;
    descriptorRange.RegisterSpace = 2;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptorRanges[rangeCount] = descriptorRange;

    rootParameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[11].DescriptorTable.NumDescriptorRanges = fragmentShader->storageBufferCount > 0;
    rootParameters[11].DescriptorTable.pDescriptorRanges = &descriptorRanges[rangeCount];
    rootParameters[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rangeCount += 1;

    /* Fragment Uniforms */
    for (Uint32 i = 0; i < MAX_UNIFORM_BUFFERS_PER_STAGE; i += 1) {
        rootParameters[12 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[12 + i].Descriptor.ShaderRegister = i;
        rootParameters[12 + i].Descriptor.RegisterSpace = 3;
        rootParameters[12 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    SDL_assert(rangeCount <= MAX_ROOT_SIGNATURE_PARAMETERS);

    // Create the root signature description
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.NumParameters = 16;
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 0;
    rootSignatureDesc.pStaticSamplers = NULL;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // Serialize the root signature
    ID3DBlob *serializedRootSignature = NULL;
    ID3DBlob *errorBlob = NULL;
    HRESULT res = renderer->D3D12SerializeRootSignature_func(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSignature, &errorBlob);

    if (FAILED(res)) {
        if (errorBlob) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to serialize RootSignature: %s", (const char *)ID3D10Blob_GetBufferPointer(errorBlob));
            ID3D10Blob_Release(errorBlob);
        }
        return NULL;
    }

    // Create the root signature
    ID3D12RootSignature *rootSignature = NULL;

    res = ID3D12Device_CreateRootSignature(
        renderer->device,
        0,
        ID3D10Blob_GetBufferPointer(serializedRootSignature),
        ID3D10Blob_GetBufferSize(serializedRootSignature),
        &D3D_IID_ID3D12RootSignature,
        (void **)&rootSignature);

    if (FAILED(res)) {
        if (errorBlob) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to create RootSignature");
            ID3D10Blob_Release(errorBlob);
        }
        return NULL;
    }

    return rootSignature;
}

static SDL_bool D3D12_INTERNAL_CreateShaderBytecode(
    D3D12Renderer *renderer,
    Uint32 stage,
    SDL_GpuShaderFormat format,
    const Uint8 *code,
    size_t codeSize,
    const char *entryPointName,
    void **pBytecode,
    size_t *pBytecodeSize)
{
    ID3DBlob *blob = NULL;
    ID3DBlob *errorBlob = NULL;
    const Uint8 *bytecode;
    size_t bytecodeSize;
    HRESULT res;

    if (format == SDL_GPU_SHADERFORMAT_HLSL) {
        res = renderer->D3DCompile_func(
            code,
            codeSize,
            NULL,
            NULL,
            NULL,
            entryPointName,
            D3D12ShaderProfiles[stage],
            0,
            0,
            &blob,
            &errorBlob);
        if (FAILED(res)) {
            if (errorBlob) {
                SDL_LogError(SDL_LOG_CATEGORY_GPU, "%s", (const char *)ID3D10Blob_GetBufferPointer(errorBlob));
                ID3D10Blob_Release(errorBlob);
            }
            if (blob)
                ID3D10Blob_Release(blob);
            return SDL_FALSE;
        }
        if (errorBlob)
            ID3D10Blob_Release(errorBlob);
        bytecode = (const Uint8 *)ID3D10Blob_GetBufferPointer(blob);
        bytecodeSize = ID3D10Blob_GetBufferSize(blob);
    } else if (format == SDL_GPU_SHADERFORMAT_DXBC) {
        bytecode = code;
        bytecodeSize = codeSize;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Incompatible shader format for D3D12");
        return SDL_FALSE;
    }

    if (pBytecode != NULL) {
        *pBytecode = SDL_malloc(bytecodeSize);
        SDL_memcpy(*pBytecode, bytecode, bytecodeSize);
        *pBytecodeSize = bytecodeSize;
    }

    // Clean up
    if (blob) {
        ID3D10Blob_Release(blob);
    }

    return SDL_TRUE;
}

static SDL_GpuComputePipeline *D3D12_CreateComputePipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuComputePipelineCreateInfo *pipelineCreateInfo)
{
    SDL_assert(SDL_FALSE);
    return NULL;
}

static SDL_bool D3D12_INTERNAL_ConvertRasterizerState(SDL_GpuRasterizerState rasterizerState, D3D12_RASTERIZER_DESC *desc)
{
    if (!desc)
        return SDL_FALSE;

    desc->FillMode = SDLToD3D12_FillMode[rasterizerState.fillMode];
    desc->CullMode = SDLToD3D12_CullMode[rasterizerState.cullMode];

    switch (rasterizerState.frontFace) {
    case SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE:
        desc->FrontCounterClockwise = TRUE;
        break;
    case SDL_GPU_FRONTFACE_CLOCKWISE:
        desc->FrontCounterClockwise = FALSE;
        break;
    default:
        return SDL_FALSE;
    }

    if (rasterizerState.depthBiasEnable) {
        desc->DepthBias = SDL_lroundf(rasterizerState.depthBiasConstantFactor);
        desc->DepthBiasClamp = rasterizerState.depthBiasClamp;
        desc->SlopeScaledDepthBias = rasterizerState.depthBiasSlopeFactor;
    } else {
        desc->DepthBias = 0;
        desc->DepthBiasClamp = 0.0f;
        desc->SlopeScaledDepthBias = 0.0f;
    }

    desc->DepthClipEnable = TRUE;
    desc->MultisampleEnable = FALSE;
    desc->AntialiasedLineEnable = FALSE;
    desc->ForcedSampleCount = 0;
    desc->ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    return SDL_TRUE;
}

static SDL_bool D3D12_INTERNAL_ConvertBlendState(SDL_GpuGraphicsPipelineCreateInfo *pipelineInfo, D3D12_BLEND_DESC *blendDesc)
{
    if (!blendDesc)
        return SDL_FALSE;

    SDL_zerop(blendDesc);
    blendDesc->AlphaToCoverageEnable = FALSE;
    blendDesc->IndependentBlendEnable = FALSE;

    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        D3D12_RENDER_TARGET_BLEND_DESC rtBlendDesc = { 0 };
        rtBlendDesc.BlendEnable = FALSE;
        rtBlendDesc.LogicOpEnable = FALSE;
        rtBlendDesc.SrcBlend = D3D12_BLEND_ONE;
        rtBlendDesc.DestBlend = D3D12_BLEND_ZERO;
        rtBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
        rtBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // If attachmentInfo has more blend states, you can set IndependentBlendEnable to TRUE and assign different blend states to each render target slot
        if (i < pipelineInfo->attachmentInfo.colorAttachmentCount) {

            SDL_GpuColorAttachmentBlendState sdlBlendState = pipelineInfo->attachmentInfo.colorAttachmentDescriptions[i].blendState;

            rtBlendDesc.BlendEnable = sdlBlendState.blendEnable;
            rtBlendDesc.SrcBlend = SDLToD3D12_BlendFactor[sdlBlendState.srcColorBlendFactor];
            rtBlendDesc.DestBlend = SDLToD3D12_BlendFactor[sdlBlendState.dstColorBlendFactor];
            rtBlendDesc.BlendOp = SDLToD3D12_BlendOp[sdlBlendState.colorBlendOp];
            rtBlendDesc.SrcBlendAlpha = SDLToD3D12_BlendFactorAlpha[sdlBlendState.srcAlphaBlendFactor];
            rtBlendDesc.DestBlendAlpha = SDLToD3D12_BlendFactorAlpha[sdlBlendState.dstAlphaBlendFactor];
            rtBlendDesc.BlendOpAlpha = SDLToD3D12_BlendOp[sdlBlendState.alphaBlendOp];
            SDL_assert(sdlBlendState.colorWriteMask <= SDL_MAX_UINT8);
            rtBlendDesc.RenderTargetWriteMask = (UINT8)sdlBlendState.colorWriteMask;

            if (i > 0)
                blendDesc->IndependentBlendEnable = TRUE;
        }

        blendDesc->RenderTarget[i] = rtBlendDesc;
    }

    return SDL_TRUE;
}

static SDL_bool D3D12_INTERNAL_ConvertDepthStencilState(SDL_GpuDepthStencilState depthStencilState, D3D12_DEPTH_STENCIL_DESC *desc)
{
    if (desc == NULL) {
        return SDL_FALSE;
    }

    desc->DepthEnable = depthStencilState.depthTestEnable == SDL_TRUE ? TRUE : FALSE;
    desc->DepthWriteMask = depthStencilState.depthWriteEnable == SDL_TRUE ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc->DepthFunc = SDLToD3D12_CompareOp[depthStencilState.compareOp];
    desc->StencilEnable = depthStencilState.stencilTestEnable == SDL_TRUE ? TRUE : FALSE;
    desc->StencilReadMask = (UINT8)depthStencilState.compareMask;
    desc->StencilWriteMask = (UINT8)depthStencilState.writeMask;

    desc->FrontFace.StencilFailOp = SDLToD3D12_StencilOp[depthStencilState.frontStencilState.failOp];
    desc->FrontFace.StencilDepthFailOp = SDLToD3D12_StencilOp[depthStencilState.frontStencilState.depthFailOp];
    desc->FrontFace.StencilPassOp = SDLToD3D12_StencilOp[depthStencilState.frontStencilState.passOp];
    desc->FrontFace.StencilFunc = SDLToD3D12_CompareOp[depthStencilState.frontStencilState.compareOp];

    desc->BackFace.StencilFailOp = SDLToD3D12_StencilOp[depthStencilState.backStencilState.failOp];
    desc->BackFace.StencilDepthFailOp = SDLToD3D12_StencilOp[depthStencilState.backStencilState.depthFailOp];
    desc->BackFace.StencilPassOp = SDLToD3D12_StencilOp[depthStencilState.backStencilState.passOp];
    desc->BackFace.StencilFunc = SDLToD3D12_CompareOp[depthStencilState.backStencilState.compareOp];

    return SDL_TRUE;
}

static SDL_bool D3D12_INTERNAL_ConvertVertexInputState(SDL_GpuVertexInputState vertexInputState, D3D12_INPUT_ELEMENT_DESC *desc)
{
    if (desc == NULL || vertexInputState.vertexAttributeCount == 0) {
        return SDL_FALSE;
    }

    for (Uint32 i = 0; i < vertexInputState.vertexAttributeCount; ++i) {
        SDL_GpuVertexAttribute attribute = vertexInputState.vertexAttributes[i];

        desc[i].SemanticName = "TEXCOORD"; // Default to TEXCOORD, can be adjusted as needed
        desc[i].SemanticIndex = attribute.location;
        desc[i].Format = SDLToD3D12_VertexFormat[attribute.format];
        desc[i].InputSlot = attribute.binding;
        desc[i].AlignedByteOffset = attribute.offset;
        desc[i].InputSlotClass = SDLToD3D12_InputRate[vertexInputState.vertexBindings[attribute.binding].inputRate];
        desc[i].InstanceDataStepRate = vertexInputState.vertexBindings[attribute.binding].stepRate;
    }

    return SDL_TRUE;
}

static void D3D12_INTERNAL_AssignCpuDescriptorHandle(
    D3D12Renderer *renderer,
    D3D12_DESCRIPTOR_HEAP_TYPE heapType,
    D3D12CPUDescriptor *cpuDescriptor)
{
    D3D12DescriptorHeap *heap = renderer->stagingDescriptorHeaps[heapType];
    Uint32 descriptorIndex;

    cpuDescriptor->heap = heap;

    SDL_LockMutex(renderer->stagingDescriptorHeapLock);

    if (heap->inactiveDescriptorCount > 0) {
        descriptorIndex = heap->inactiveDescriptorIndices[heap->inactiveDescriptorCount - 1];
        heap->inactiveDescriptorCount -= 1;
    } else if (heap->currentDescriptorIndex < heap->maxDescriptors) {
        descriptorIndex = heap->currentDescriptorIndex;
        heap->currentDescriptorIndex += 1;
    } else {
        cpuDescriptor->cpuHandleIndex = SDL_MAX_UINT32;
        cpuDescriptor->cpuHandle.ptr = 0;
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Out of CPU descriptor handles, many bad things are going to happen!");
        SDL_UnlockMutex(renderer->stagingDescriptorHeapLock);
        return;
    }

    SDL_UnlockMutex(renderer->stagingDescriptorHeapLock);

    cpuDescriptor->cpuHandleIndex = descriptorIndex;
    cpuDescriptor->cpuHandle.ptr = heap->descriptorHeapCPUStart.ptr + (descriptorIndex * heap->descriptorSize);
}

/* TODO: call this when releasing resources */
static void D3D12_INTERNAL_ReleaseCpuDescriptorHandle(
    D3D12Renderer *renderer,
    D3D12CPUDescriptor *cpuDescriptor)
{
    D3D12DescriptorHeap *heap = cpuDescriptor->heap;

    SDL_LockMutex(renderer->stagingDescriptorHeapLock);
    heap->inactiveDescriptorIndices[heap->inactiveDescriptorCount] = cpuDescriptor->cpuHandleIndex;
    heap->inactiveDescriptorCount += 1;
    SDL_UnlockMutex(renderer->stagingDescriptorHeapLock);
}

static SDL_GpuGraphicsPipeline *D3D12_CreateGraphicsPipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuGraphicsPipelineCreateInfo *pipelineCreateInfo)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12Shader *vertShader = (D3D12Shader *)pipelineCreateInfo->vertexShader;
    D3D12Shader *fragShader = (D3D12Shader *)pipelineCreateInfo->fragmentShader;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = { 0 };
    SDL_zero(psoDesc);
    psoDesc.VS.pShaderBytecode = vertShader->bytecode;
    psoDesc.VS.BytecodeLength = vertShader->bytecodeSize;
    psoDesc.PS.pShaderBytecode = fragShader->bytecode;
    psoDesc.PS.BytecodeLength = fragShader->bytecodeSize;

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT];
    if (pipelineCreateInfo->vertexInputState.vertexAttributeCount > 0) {
        psoDesc.InputLayout.pInputElementDescs = inputElementDescs;
        psoDesc.InputLayout.NumElements = pipelineCreateInfo->vertexInputState.vertexAttributeCount;
        D3D12_INTERNAL_ConvertVertexInputState(pipelineCreateInfo->vertexInputState, inputElementDescs);
    }

    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    if (!D3D12_INTERNAL_ConvertRasterizerState(pipelineCreateInfo->rasterizerState, &psoDesc.RasterizerState))
        return NULL;
    if (!D3D12_INTERNAL_ConvertBlendState(pipelineCreateInfo, &psoDesc.BlendState))
        return NULL;
    if (!D3D12_INTERNAL_ConvertDepthStencilState(pipelineCreateInfo->depthStencilState, &psoDesc.DepthStencilState))
        return NULL;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = SDLToD3D12_SampleCount[pipelineCreateInfo->multisampleState.multisampleCount];
    psoDesc.SampleDesc.Quality = 0;

    psoDesc.DSVFormat = SDLToD3D12_TextureFormat[pipelineCreateInfo->attachmentInfo.depthStencilFormat];
    psoDesc.NumRenderTargets = pipelineCreateInfo->attachmentInfo.colorAttachmentCount;
    for (uint32_t i = 0; i < pipelineCreateInfo->attachmentInfo.colorAttachmentCount; ++i) {
        psoDesc.RTVFormats[i] = SDLToD3D12_TextureFormat[pipelineCreateInfo->attachmentInfo.colorAttachmentDescriptions[i].format];
    }

    // Assuming some default values or further initialization
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    psoDesc.CachedPSO.CachedBlobSizeInBytes = 0;
    psoDesc.CachedPSO.pCachedBlob = NULL;

    psoDesc.NodeMask = 0;

    ID3D12RootSignature *rootSignature = D3D12_INTERNAL_CreateGraphicsRootSignature(
        renderer,
        vertShader,
        fragShader);

    if (rootSignature == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Could not create root signature!");
        return NULL;
    }
    psoDesc.pRootSignature = rootSignature;
    ID3D12PipelineState *pipelineState = NULL;

    HRESULT res = ID3D12Device_CreateGraphicsPipelineState(renderer->device, &psoDesc, &D3D_IID_ID3D12PipelineState, (void **)&pipelineState);
    if (FAILED(res)) {
        D3D12_INTERNAL_LogError(renderer->device, "Could not create graphics pipeline state", res);
        ID3D12RootSignature_Release(rootSignature);
        return NULL;
    }

    D3D12GraphicsPipeline *pipeline = (D3D12GraphicsPipeline *)SDL_calloc(1, sizeof(D3D12GraphicsPipeline));
    SDL_zerop(pipeline);
    pipeline->pipelineState = pipelineState;
    pipeline->rootSignature = rootSignature;

    pipeline->primitiveType = pipelineCreateInfo->primitiveType;
    pipeline->blendConstants[0] = pipelineCreateInfo->blendConstants[0];
    pipeline->blendConstants[1] = pipelineCreateInfo->blendConstants[1];
    pipeline->blendConstants[2] = pipelineCreateInfo->blendConstants[2];
    pipeline->blendConstants[3] = pipelineCreateInfo->blendConstants[3];
    pipeline->stencilRef = pipelineCreateInfo->depthStencilState.reference;

    pipeline->vertexSamplerCount = vertShader->samplerCount;
    pipeline->vertexStorageTextureCount = vertShader->storageTextureCount;
    pipeline->vertexStorageBufferCount = vertShader->storageBufferCount;
    pipeline->vertexUniformBufferCount = vertShader->uniformBufferCount;

    pipeline->fragmentSamplerCount = fragShader->samplerCount;
    pipeline->fragmentStorageTextureCount = fragShader->storageTextureCount;
    pipeline->fragmentStorageBufferCount = fragShader->storageBufferCount;
    pipeline->fragmentUniformBufferCount = fragShader->uniformBufferCount;

    return (SDL_GpuGraphicsPipeline *)pipeline;
}

static SDL_GpuSampler *D3D12_CreateSampler(
    SDL_GpuRenderer *driverData,
    SDL_GpuSamplerCreateInfo *samplerCreateInfo)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12Sampler *sampler = (D3D12Sampler *)SDL_malloc(sizeof(D3D12Sampler));
    D3D12_SAMPLER_DESC samplerDesc;

    samplerDesc.Filter = SDLToD3D12_Filter(
        samplerCreateInfo->minFilter,
        samplerCreateInfo->magFilter,
        samplerCreateInfo->mipmapMode,
        samplerCreateInfo->compareEnable,
        samplerCreateInfo->anisotropyEnable);
    samplerDesc.AddressU = SDLToD3D12_SamplerAddressMode[samplerCreateInfo->addressModeU];
    samplerDesc.AddressV = SDLToD3D12_SamplerAddressMode[samplerCreateInfo->addressModeV];
    samplerDesc.AddressW = SDLToD3D12_SamplerAddressMode[samplerCreateInfo->addressModeW];
    samplerDesc.MaxAnisotropy = (Uint32)samplerCreateInfo->maxAnisotropy;
    samplerDesc.ComparisonFunc = SDLToD3D12_CompareOp[samplerCreateInfo->compareOp];
    samplerDesc.MinLOD = samplerCreateInfo->minLod;
    samplerDesc.MaxLOD = samplerCreateInfo->maxLod;
    samplerDesc.MipLODBias = samplerCreateInfo->mipLodBias;
    samplerDesc.BorderColor[0] = 0;
    samplerDesc.BorderColor[1] = 0;
    samplerDesc.BorderColor[2] = 0;
    samplerDesc.BorderColor[3] = 0;

    D3D12_INTERNAL_AssignCpuDescriptorHandle(
        renderer,
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
        &sampler->handle);

    ID3D12Device_CreateSampler(
        renderer->device,
        &samplerDesc,
        sampler->handle.cpuHandle);

    sampler->createInfo = *samplerCreateInfo;
    return (SDL_GpuSampler *)sampler;
}

static SDL_GpuShader *D3D12_CreateShader(
    SDL_GpuRenderer *driverData,
    SDL_GpuShaderCreateInfo *shaderCreateInfo)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    void *bytecode;
    size_t bytecodeSize;
    D3D12Shader *shader;

    if ((shaderCreateInfo->stage != SDL_GPU_SHADERSTAGE_VERTEX) && (shaderCreateInfo->stage != SDL_GPU_SHADERSTAGE_FRAGMENT)) {
        SDL_assert(SDL_FALSE);
    }

    if (!D3D12_INTERNAL_CreateShaderBytecode(
            renderer,
            shaderCreateInfo->stage,
            shaderCreateInfo->format,
            shaderCreateInfo->code,
            shaderCreateInfo->codeSize,
            shaderCreateInfo->entryPointName,
            &bytecode,
            &bytecodeSize)) {
        return NULL;
    }
    shader = (D3D12Shader *)SDL_calloc(1, sizeof(D3D12Shader));
    SDL_zerop(shader);
    shader->samplerCount = shaderCreateInfo->samplerCount;
    shader->storageBufferCount = shaderCreateInfo->storageBufferCount;
    shader->storageTextureCount = shaderCreateInfo->storageTextureCount;
    shader->uniformBufferCount = shaderCreateInfo->uniformBufferCount;

    shader->bytecode = bytecode;
    shader->bytecodeSize = bytecodeSize;

    return (SDL_GpuShader *)shader;
}

static D3D12Texture *D3D12_INTERNAL_CreateTexture(
    D3D12Renderer *renderer,
    SDL_GpuTextureCreateInfo *textureCreateInfo)
{
    D3D12Texture *texture;
    ID3D12Resource *handle;
    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_FLAGS resourceFlags = 0;
    D3D12_RESOURCE_STATES initialState;
    HRESULT res;

    if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE_BIT) {
        resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    heapProperties.MemoryPoolPreference = renderer->UMA ? D3D12_MEMORY_POOL_L0 : D3D12_MEMORY_POOL_L1;
    heapProperties.CreationNodeMask = 0; /* We don't do multi-adapter operation */
    heapProperties.VisibleNodeMask = 0;  /* We don't do multi-adapter operation */

    heapFlags = D3D12_HEAP_FLAG_NONE;

    if (textureCreateInfo->depth <= 1) {
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        desc.Width = textureCreateInfo->width;
        desc.Height = textureCreateInfo->height;
        desc.DepthOrArraySize = textureCreateInfo->isCube ? 6 : textureCreateInfo->layerCount;
        desc.MipLevels = textureCreateInfo->levelCount;
        desc.Format = SDLToD3D12_TextureFormat[textureCreateInfo->format];
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; /* Apparently this is the most efficient choice */
        desc.Flags = resourceFlags;
    } else {
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        desc.Width = textureCreateInfo->width;
        desc.Height = textureCreateInfo->height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = textureCreateInfo->levelCount;
        desc.Format = SDLToD3D12_TextureFormat[textureCreateInfo->format];
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = resourceFlags;
    }

    if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_SAMPLER_BIT) {
        initialState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    } else if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ_BIT) {
        initialState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    } else if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET_BIT) {
        initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    } else if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET_BIT) {
        initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ_BIT) {
        initialState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    } else if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE_BIT) {
        initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Creating texture with no usage flags is invalid!");
        return NULL;
    }

    res = ID3D12Device_CreateCommittedResource(
        renderer->device,
        &heapProperties,
        heapFlags,
        &desc,
        initialState,
        NULL,
        &D3D_IID_ID3D12Resource,
        (void **)&handle);
    ERROR_CHECK_RETURN("Failed to create texture!", NULL)

    texture = SDL_malloc(sizeof(D3D12Texture));
    texture->container = NULL;   /* this is replaced later */
    texture->containerIndex = 0; /* this is replaced later */

    texture->resource = handle;
    texture->srvHandle.heap = NULL;

    /* Create the SRV if applicable */
    if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_SAMPLER_BIT) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;

        D3D12_INTERNAL_AssignCpuDescriptorHandle(
            renderer,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            &texture->srvHandle);

        srvDesc.Format = SDLToD3D12_TextureFormat[textureCreateInfo->format];
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        if (textureCreateInfo->isCube) {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MipLevels = textureCreateInfo->levelCount;
            srvDesc.TextureCube.MostDetailedMip = 0;
            srvDesc.TextureCube.ResourceMinLODClamp = 0;
        } else if (textureCreateInfo->layerCount > 1) {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MipLevels = textureCreateInfo->levelCount;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = textureCreateInfo->layerCount;
            srvDesc.Texture2DArray.ResourceMinLODClamp = 0;
            srvDesc.Texture2DArray.PlaneSlice = 0;
        } else if (textureCreateInfo->depth > 1) {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srvDesc.Texture3D.MipLevels = textureCreateInfo->levelCount;
            srvDesc.Texture3D.MostDetailedMip = 0;
            srvDesc.Texture3D.ResourceMinLODClamp = 0; /* default behavior */
        } else {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = textureCreateInfo->levelCount;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.PlaneSlice = 0;
            srvDesc.Texture2D.ResourceMinLODClamp = 0; /* default behavior */
        }

        ID3D12Device_CreateShaderResourceView(
            renderer->device,
            handle,
            &srvDesc,
            texture->srvHandle.cpuHandle);
    }

    texture->subresourceCount = textureCreateInfo->levelCount * textureCreateInfo->layerCount;
    texture->subresources = SDL_malloc(
        texture->subresourceCount * sizeof(D3D12TextureSubresource));

    for (Uint32 layerIndex = 0; layerIndex < textureCreateInfo->layerCount; layerIndex += 1) {
        for (Uint32 levelIndex = 0; levelIndex < textureCreateInfo->levelCount; levelIndex += 1) {
            Uint32 subresourceIndex = D3D12_INTERNAL_CalcSubresource(
                levelIndex,
                layerIndex,
                textureCreateInfo->levelCount);

            texture->subresources[subresourceIndex].parent = texture;
            texture->subresources[subresourceIndex].layer = layerIndex;
            texture->subresources[subresourceIndex].level = levelIndex;
            texture->subresources[subresourceIndex].index = subresourceIndex;

            texture->subresources[subresourceIndex].rtvHandle.heap = NULL;
            texture->subresources[subresourceIndex].dsvHandle.heap = NULL;
            texture->subresources[subresourceIndex].srvHandle.heap = NULL;
            texture->subresources[subresourceIndex].uavHandle.heap = NULL;
            SDL_AtomicSet(&texture->subresources[subresourceIndex].referenceCount, 0);

            /* Create RTV if needed */
            if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET_BIT) {
                D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;

                D3D12_INTERNAL_AssignCpuDescriptorHandle(
                    renderer,
                    D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                    &texture->subresources[subresourceIndex].rtvHandle);

                rtvDesc.Format = SDLToD3D12_TextureFormat[textureCreateInfo->format];

                if (textureCreateInfo->layerCount > 1) {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtvDesc.Texture2DArray.MipSlice = levelIndex;
                    rtvDesc.Texture2DArray.FirstArraySlice = layerIndex;
                    rtvDesc.Texture2DArray.ArraySize = 1;
                } else if (textureCreateInfo->depth > 1) {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                    rtvDesc.Texture3D.MipSlice = levelIndex;
                    rtvDesc.Texture3D.FirstWSlice = 0;
                    rtvDesc.Texture3D.WSize = -1; /* all depths */
                } else {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    rtvDesc.Texture2D.MipSlice = levelIndex;
                    rtvDesc.Texture2D.PlaneSlice = 0;
                }

                ID3D12Device_CreateRenderTargetView(
                    renderer->device,
                    texture->resource,
                    &rtvDesc,
                    texture->subresources[subresourceIndex].rtvHandle.cpuHandle);
            }

            /* Create DSV if needed */
            if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET_BIT) {
                D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;

                D3D12_INTERNAL_AssignCpuDescriptorHandle(
                    renderer,
                    D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                    &texture->subresources[subresourceIndex].dsvHandle);

                dsvDesc.Format = SDLToD3D12_TextureFormat[textureCreateInfo->format];
                dsvDesc.Flags = 0;
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsvDesc.Texture2D.MipSlice = levelIndex;

                ID3D12Device_CreateDepthStencilView(
                    renderer->device,
                    texture->resource,
                    &dsvDesc,
                    texture->subresources[subresourceIndex].dsvHandle.cpuHandle);
            }

            /* Create subresource SRV if needed */
            if (
                (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ_BIT) ||
                (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ_BIT)) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;

                D3D12_INTERNAL_AssignCpuDescriptorHandle(
                    renderer,
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    &texture->subresources[subresourceIndex].srvHandle);

                srvDesc.Format = SDLToD3D12_TextureFormat[textureCreateInfo->format];
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

                if (textureCreateInfo->layerCount > 1) {
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    srvDesc.Texture2DArray.ArraySize = 1;
                    srvDesc.Texture2DArray.FirstArraySlice = layerIndex;
                    srvDesc.Texture2DArray.MipLevels = 1;
                    srvDesc.Texture2DArray.MostDetailedMip = levelIndex;
                    srvDesc.Texture2DArray.PlaneSlice = 0;
                    srvDesc.Texture2DArray.ResourceMinLODClamp = 0;
                } else if (textureCreateInfo->depth > 1) {
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    srvDesc.Texture3D.MipLevels = 1;
                    srvDesc.Texture3D.MostDetailedMip = levelIndex;
                    srvDesc.Texture3D.ResourceMinLODClamp = 0;
                } else {
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    srvDesc.Texture2D.MostDetailedMip = levelIndex;
                    srvDesc.Texture2D.PlaneSlice = 0;
                    srvDesc.Texture2D.ResourceMinLODClamp = 0;
                }

                ID3D12Device_CreateShaderResourceView(
                    renderer->device,
                    texture->resource,
                    &srvDesc,
                    texture->subresources[subresourceIndex].srvHandle.cpuHandle);
            }

            /* Create subresource UAV if necessary */
            if (textureCreateInfo->usageFlags & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE_BIT) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;

                D3D12_INTERNAL_AssignCpuDescriptorHandle(
                    renderer,
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    &texture->subresources[subresourceIndex].uavHandle);

                uavDesc.Format = SDLToD3D12_TextureFormat[textureCreateInfo->format];

                if (textureCreateInfo->layerCount > 1) {
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    uavDesc.Texture2DArray.MipSlice = levelIndex;
                    uavDesc.Texture2DArray.FirstArraySlice = layerIndex;
                    uavDesc.Texture2DArray.ArraySize = 1;
                } else if (textureCreateInfo->depth > 1) {
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                    uavDesc.Texture3D.MipSlice = levelIndex;
                    uavDesc.Texture3D.FirstWSlice = 0;
                    uavDesc.Texture3D.WSize = textureCreateInfo->layerCount;
                } else {
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    uavDesc.Texture2D.MipSlice = levelIndex;
                    uavDesc.Texture2D.PlaneSlice = 0;
                }

                ID3D12Device_CreateUnorderedAccessView(
                    renderer->device,
                    texture->resource,
                    NULL,
                    &uavDesc,
                    texture->subresources[subresourceIndex].uavHandle.cpuHandle);
            }
        }
    }

    return texture;
}

static SDL_GpuTexture *D3D12_CreateTexture(
    SDL_GpuRenderer *driverData,
    SDL_GpuTextureCreateInfo *textureCreateInfo)
{
    D3D12Texture *texture = D3D12_INTERNAL_CreateTexture(
        (D3D12Renderer *)driverData,
        textureCreateInfo);
    D3D12TextureContainer *container = SDL_malloc(sizeof(D3D12TextureContainer));

    container->createInfo = *textureCreateInfo;
    container->textureCapacity = 1;
    container->textureCount = 1;
    container->textures = SDL_malloc(
        container->textureCapacity * sizeof(D3D12Texture *));
    container->textures[0] = texture;
    container->activeTexture = texture;
    container->debugName = NULL;
    container->canBeCycled = SDL_TRUE;

    texture->container = container;
    texture->containerIndex = 0;

    return (SDL_GpuTexture *)container;
}

static D3D12Buffer *D3D12_INTERNAL_CreateBuffer(
    D3D12Renderer *renderer,
    SDL_GpuBufferUsageFlags usageFlags,
    Uint32 sizeInBytes,
    D3D12BufferType type)
{
    D3D12Buffer *buffer;
    ID3D12Resource *handle;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_RESOURCE_DESC desc;
    D3D12_HEAP_FLAGS heapFlags = 0;
    D3D12_RESOURCE_FLAGS resourceFlags = 0;
    D3D12_RESOURCE_STATES initialState;
    HRESULT res;

    if (usageFlags & SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE_BIT) {
        resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    heapProperties.CreationNodeMask = 0; /* We don't do multi-adapter operation */
    heapProperties.VisibleNodeMask = 0;  /* We don't do multi-adapter operation */

    if (type == D3D12_BUFFER_TYPE_GPU) {
        heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM; /* FIXME: should we just use HEAP_TYPE_UPLOAD? */
        heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        heapProperties.MemoryPoolPreference = renderer->UMA ? D3D12_MEMORY_POOL_L0 : D3D12_MEMORY_POOL_L1;
        heapFlags = D3D12_HEAP_FLAG_NONE;

        if (usageFlags & SDL_GPU_BUFFERUSAGE_VERTEX_BIT) {
            initialState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        } else if (usageFlags & SDL_GPU_BUFFERUSAGE_INDEX_BIT) {
            initialState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        } else if (usageFlags & SDL_GPU_BUFFERUSAGE_INDIRECT_BIT) {
            initialState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        } else if (usageFlags & SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ_BIT) {
            initialState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        } else if (usageFlags & SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ_BIT) {
            initialState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        } else if (usageFlags & SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE_BIT) {
            initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Creating GPU buffer with no usage flags is invalid!");
            return NULL;
        }
    } else if (type == D3D12_BUFFER_TYPE_UPLOAD) {
        heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM;                           /* FIXME: should we just use HEAP_TYPE_UPLOAD? */
        heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE; /* FIXME: is this right? */
        heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        heapFlags = D3D12_HEAP_FLAG_NONE;
        initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if (type == D3D12_BUFFER_TYPE_DOWNLOAD) {
        heapProperties.Type = D3D12_HEAP_TYPE_READBACK; /* FIXME: is this right? */
        heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapFlags = D3D12_HEAP_FLAG_NONE;
        initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    } else if (type == D3D12_BUFFER_TYPE_UNIFORM) {
        heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM; /* FIXME: should we just use HEAP_TYPE_UPLOAD? */
        heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        heapProperties.MemoryPoolPreference = renderer->UMA ? D3D12_MEMORY_POOL_L0 : D3D12_MEMORY_POOL_L1;
        heapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        initialState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Unrecognized buffer type!");
        return NULL;
    }

    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Width = sizeInBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = resourceFlags;

    res = ID3D12Device_CreateCommittedResource(
        renderer->device,
        &heapProperties,
        heapFlags,
        &desc,
        initialState,
        NULL,
        &D3D_IID_ID3D12Resource,
        (void **)&handle);
    ERROR_CHECK_RETURN("Could not create buffer!", NULL);

    buffer = SDL_malloc(sizeof(D3D12Buffer));
    buffer->handle = handle;
    SDL_AtomicSet(&buffer->referenceCount, 0);

    if (usageFlags & SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE_BIT) {
        D3D12_INTERNAL_AssignCpuDescriptorHandle(
            renderer,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            &buffer->uavDescriptor);

        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = sizeInBytes / sizeof(Uint32);
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        uavDesc.Buffer.CounterOffsetInBytes = 0; /* TODO: support counters? */
        uavDesc.Buffer.StructureByteStride = 0;

        /* Create UAV */
        ID3D12Device_CreateUnorderedAccessView(
            renderer->device,
            handle,
            NULL, /* TODO: support counters? */
            &uavDesc,
            buffer->uavDescriptor.cpuHandle);
    }

    if (usageFlags & SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ_BIT) {
        D3D12_INTERNAL_AssignCpuDescriptorHandle(
            renderer,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            &buffer->srvDescriptor);

        srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = sizeInBytes / sizeof(Uint32);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        srvDesc.Buffer.StructureByteStride = 0;

        /* Create SRV */
        ID3D12Device_CreateShaderResourceView(
            renderer->device,
            handle,
            &srvDesc,
            buffer->srvDescriptor.cpuHandle);
    }

    if (type == D3D12_BUFFER_TYPE_UNIFORM) {
        D3D12_INTERNAL_AssignCpuDescriptorHandle(
            renderer,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            &buffer->cbvDescriptor);

        cbvDesc.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(handle);
        cbvDesc.SizeInBytes = sizeInBytes;

        /* Create CBV */
        ID3D12Device_CreateConstantBufferView(
            renderer->device,
            &cbvDesc,
            buffer->cbvDescriptor.cpuHandle);
    }

    buffer->virtualAddress = 0;
    if (type == D3D12_BUFFER_TYPE_UNIFORM) {
        buffer->virtualAddress = ID3D12Resource_GetGPUVirtualAddress(buffer->handle);
    }

    buffer->mapPointer = NULL;
    /* Persistently map upload buffers */
    if (type == D3D12_BUFFER_TYPE_UPLOAD) {
        ID3D12Resource_Map(
            buffer->handle,
            0,
            NULL,
            (void **)&buffer->mapPointer);
    }

    SDL_AtomicSet(&buffer->referenceCount, 0);
    return buffer;
}

/* TODO */
static D3D12BufferContainer *D3D12_INTERNAL_CreateBufferContainer(
    D3D12Renderer *renderer,
    SDL_GpuBufferUsageFlags usageFlags,
    Uint32 sizeInBytes,
    D3D12BufferType type)
{
    D3D12BufferContainer *container;
    D3D12Buffer *buffer;

    buffer = D3D12_INTERNAL_CreateBuffer(
        renderer,
        usageFlags,
        sizeInBytes,
        type);

    if (buffer == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to create buffer!");
        return NULL;
    }

    container = SDL_malloc(sizeof(D3D12BufferContainer));

    container->usageFlags = usageFlags;
    container->size = sizeInBytes;
    container->type = type;

    container->activeBuffer = buffer;
    container->bufferCapacity = 1;
    container->bufferCount = 1;
    container->buffers = SDL_malloc(
        container->bufferCapacity * sizeof(D3D12BufferContainer *));
    container->buffers[0] = buffer;
    container->debugName = NULL;

    return container;
}

static SDL_GpuBuffer *D3D12_CreateBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuBufferUsageFlags usageFlags,
    Uint32 sizeInBytes)
{
    return (SDL_GpuBuffer *)D3D12_INTERNAL_CreateBufferContainer(
        (D3D12Renderer *)driverData,
        usageFlags,
        sizeInBytes,
        D3D12_BUFFER_TYPE_GPU);
}

static SDL_GpuTransferBuffer *D3D12_CreateTransferBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBufferUsage usage,
    Uint32 sizeInBytes)
{
    return (SDL_GpuTransferBuffer *)D3D12_INTERNAL_CreateBufferContainer(
        (D3D12Renderer *)driverData,
        0,
        sizeInBytes,
        usage == SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD ? D3D12_BUFFER_TYPE_UPLOAD : D3D12_BUFFER_TYPE_DOWNLOAD);
}

/* Debug Naming */

static void D3D12_SetBufferName(
    SDL_GpuRenderer *driverData,
    SDL_GpuBuffer *buffer,
    const char *text) { SDL_assert(SDL_FALSE); }

static void D3D12_SetTextureName(
    SDL_GpuRenderer *driverData,
    SDL_GpuTexture *texture,
    const char *text) { SDL_assert(SDL_FALSE); }

static void D3D12_InsertDebugLabel(
    SDL_GpuCommandBuffer *commandBuffer,
    const char *text) { SDL_assert(SDL_FALSE); }

static void D3D12_PushDebugGroup(
    SDL_GpuCommandBuffer *commandBuffer,
    const char *name) { SDL_assert(SDL_FALSE); }

static void D3D12_PopDebugGroup(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

/* Disposal */

static void D3D12_ReleaseTexture(
    SDL_GpuRenderer *driverData,
    SDL_GpuTexture *texture) { SDL_assert(SDL_FALSE); }

static void D3D12_ReleaseSampler(
    SDL_GpuRenderer *driverData,
    SDL_GpuSampler *sampler) { SDL_assert(SDL_FALSE); }

static void D3D12_ReleaseBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuBuffer *buffer) { SDL_assert(SDL_FALSE); }

static void D3D12_ReleaseTransferBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBuffer *transferBuffer) { SDL_assert(SDL_FALSE); }

static void D3D12_ReleaseShader(
    SDL_GpuRenderer *driverData,
    SDL_GpuShader *shader)
{
    /* D3D12Renderer *renderer = (D3D12Renderer *)driverData; */
    D3D12Shader *d3d12shader = (D3D12Shader *)shader;

    if (d3d12shader->bytecode) {
        SDL_free(d3d12shader->bytecode);
        d3d12shader->bytecode = NULL;
    }
    SDL_free(d3d12shader);
}

static void D3D12_ReleaseComputePipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuComputePipeline *computePipeline) { SDL_assert(SDL_FALSE); }

static void D3D12_ReleaseGraphicsPipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuGraphicsPipeline *graphicsPipeline)
{
    D3D12GraphicsPipeline *pipeline = (D3D12GraphicsPipeline *)graphicsPipeline;
    if (pipeline->pipelineState) {
        ID3D12PipelineState_Release(pipeline->pipelineState);
        pipeline->pipelineState = NULL;
    }
    if (pipeline->rootSignature) {
        ID3D12RootSignature_Release(pipeline->rootSignature);
        pipeline->rootSignature = NULL;
    }
    SDL_free(pipeline);
}

/* Render Pass */

static void D3D12_SetViewport(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuViewport *viewport)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12_VIEWPORT d3d12Viewport;
    d3d12Viewport.TopLeftX = viewport->x;
    d3d12Viewport.TopLeftY = viewport->y;
    d3d12Viewport.Width = viewport->w;
    d3d12Viewport.Height = viewport->h;
    d3d12Viewport.MinDepth = viewport->minDepth;
    d3d12Viewport.MaxDepth = viewport->maxDepth;
    ID3D12GraphicsCommandList_RSSetViewports(d3d12CommandBuffer->graphicsCommandList, 1, &d3d12Viewport);
}

static void D3D12_SetScissor(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuRect *scissor)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12_RECT scissorRect;
    scissorRect.left = scissor->x;
    scissorRect.top = scissor->y;
    scissorRect.right = scissor->x + scissor->w;
    scissorRect.bottom = scissor->y + scissor->h;
    ID3D12GraphicsCommandList_RSSetScissorRects(d3d12CommandBuffer->graphicsCommandList, 1, &scissorRect);
}

static D3D12TextureSubresource *D3D12_INTERNAL_FetchTextureSubresource(
    D3D12TextureContainer *container,
    Uint32 layer,
    Uint32 level)
{
    Uint32 index = D3D12_INTERNAL_CalcSubresource(
        level,
        layer,
        container->createInfo.levelCount);
    return &container->activeTexture->subresources[index];
}

static void D3D12_INTERNAL_CycleActiveTexture(
    D3D12Renderer *renderer,
    D3D12TextureContainer *container)
{
    D3D12Texture *texture;
    Sint32 refCountTotal;

    /* If a previously-cycled texture is available, we can use that. */
    for (Uint32 i = 0; i < container->textureCount; i += 1) {
        texture = container->textures[i];

        refCountTotal = 0;
        for (Uint32 j = 0; j < texture->subresourceCount; j += 1) {
            refCountTotal += SDL_AtomicGet(&texture->subresources[j].referenceCount);
        }

        if (refCountTotal == 0) {
            container->activeTexture = texture;
            return;
        }
    }

    /* No texture is available, generate a new one. */
    texture = D3D12_INTERNAL_CreateTexture(
        renderer,
        &container->createInfo);

    EXPAND_ARRAY_IF_NEEDED(
        container->textures,
        D3D12Texture *,
        container->textureCount + 1,
        container->textureCapacity,
        container->textureCapacity * 2);

    container->textures[container->textureCount] = texture;
    texture->container = container;
    texture->containerIndex = container->textureCount;
    container->textureCount += 1;

    container->activeTexture = texture;

    if (renderer->debugMode && container->debugName != NULL) {
        D3D12_INTERNAL_SetResourceName(
            renderer,
            container->activeTexture->resource,
            container->debugName);
    }
}

static D3D12TextureSubresource *D3D12_INTERNAL_PrepareTextureSubresourceForWrite(
    D3D12CommandBuffer *commandBuffer,
    D3D12TextureContainer *container,
    Uint32 layer,
    Uint32 level,
    SDL_bool cycle,
    D3D12_RESOURCE_STATES destinationUsageMode)
{
    D3D12TextureSubresource *subresource = D3D12_INTERNAL_FetchTextureSubresource(
        container,
        layer,
        level);

    if (
        container->canBeCycled &&
        cycle &&
        SDL_AtomicGet(&subresource->referenceCount) > 0) {
        D3D12_INTERNAL_CycleActiveTexture(
            commandBuffer->renderer,
            container);

        subresource = D3D12_INTERNAL_FetchTextureSubresource(
            container,
            layer,
            level);
    }

    D3D12_INTERNAL_TextureSubresourceTransitionFromDefaultUsage(
        commandBuffer,
        destinationUsageMode,
        subresource);

    return subresource;
}

static void D3D12_INTERNAL_CycleActiveBuffer(
    D3D12Renderer *renderer,
    D3D12BufferContainer *container)
{
    /* If a previously-cycled buffer is available, we can use that. */
    for (Uint32 i = 0; i < container->bufferCount; i += 1) {
        D3D12Buffer *buffer = container->buffers[i];
        if (SDL_AtomicGet(&buffer->referenceCount) == 0) {
            container->activeBuffer = buffer;
            return;
        }
    }

    /* No buffer handle is available, create a new one. */
    D3D12Buffer *buffer = D3D12_INTERNAL_CreateBuffer(
        renderer,
        container->usageFlags,
        container->size,
        container->type);

    EXPAND_ARRAY_IF_NEEDED(
        container->buffers,
        D3D12Buffer *,
        container->bufferCount + 1,
        container->bufferCapacity,
        container->bufferCapacity * 2);

    container->buffers[container->bufferCount] = buffer;
    buffer->container = container;
    buffer->containerIndex = container->bufferCount;
    container->bufferCount += 1;

    container->activeBuffer = buffer;

    if (renderer->debugMode && container->debugName != NULL) {
        D3D12_INTERNAL_SetResourceName(
            renderer,
            container->activeBuffer->handle,
            container->debugName);
    }
}

static void D3D12_BeginRenderPass(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuColorAttachmentInfo *colorAttachmentInfos,
    Uint32 colorAttachmentCount,
    SDL_GpuDepthStencilAttachmentInfo *depthStencilAttachmentInfo)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    /* D3D12Renderer *renderer = d3d12CommandBuffer->renderer; */

    Uint32 framebufferWidth = SDL_MAX_UINT32;
    Uint32 framebufferHeight = SDL_MAX_UINT32;

    for (Uint32 i = 0; i < colorAttachmentCount; ++i) {
        D3D12TextureContainer *container = (D3D12TextureContainer *)colorAttachmentInfos[i].textureSlice.texture;
        Uint32 h = container->createInfo.height >> colorAttachmentInfos[i].textureSlice.mipLevel;
        Uint32 w = container->createInfo.width >> colorAttachmentInfos[i].textureSlice.mipLevel;

        /* The framebuffer cannot be larger than the smallest attachment. */

        if (w < framebufferWidth) {
            framebufferWidth = w;
        }

        if (h < framebufferHeight) {
            framebufferHeight = h;
        }

        if (!(container->createInfo.usageFlags & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET_BIT)) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Color attachment texture was not designated as a color target!");
            return;
        }
    }

    if (depthStencilAttachmentInfo != NULL) {
        D3D12TextureContainer *container = (D3D12TextureContainer *)depthStencilAttachmentInfo->textureSlice.texture;

        Uint32 h = container->createInfo.height >> depthStencilAttachmentInfo->textureSlice.mipLevel;
        Uint32 w = container->createInfo.width >> depthStencilAttachmentInfo->textureSlice.mipLevel;

        /* The framebuffer cannot be larger than the smallest attachment. */

        if (w < framebufferWidth) {
            framebufferWidth = w;
        }

        if (h < framebufferHeight) {
            framebufferHeight = h;
        }

        /* Fixme: */
        if (!(container->createInfo.usageFlags & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET_BIT)) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Depth stencil attachment texture was not designated as a depth target!");
            return;
        }
    }

    /* Layout transitions */

    d3d12CommandBuffer->colorAttachmentCount = colorAttachmentCount;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[MAX_COLOR_TARGET_BINDINGS];

    for (Uint32 i = 0; i < colorAttachmentCount; i += 1) {
        SDL_bool cycle;
        if (colorAttachmentInfos[i].loadOp == SDL_GPU_LOADOP_LOAD) {
            cycle = SDL_FALSE;
        } else {
            cycle = colorAttachmentInfos[i].cycle;
        }

        D3D12TextureContainer *container = (D3D12TextureContainer *)colorAttachmentInfos[i].textureSlice.texture;
        D3D12TextureSubresource *subresource =
            D3D12_INTERNAL_PrepareTextureSubresourceForWrite(
                d3d12CommandBuffer,
                container,
                colorAttachmentInfos[i].textureSlice.layer,
                colorAttachmentInfos[i].textureSlice.mipLevel,
                cycle,
                D3D12_RESOURCE_STATE_RENDER_TARGET);

        if (colorAttachmentInfos[i].loadOp == SDL_GPU_LOADOP_CLEAR) {
            float clearColor[4];
            clearColor[0] = colorAttachmentInfos[i].clearColor.r;
            clearColor[1] = colorAttachmentInfos[i].clearColor.g;
            clearColor[2] = colorAttachmentInfos[i].clearColor.b;
            clearColor[3] = colorAttachmentInfos[i].clearColor.a;

            ID3D12GraphicsCommandList_ClearRenderTargetView(
                d3d12CommandBuffer->graphicsCommandList,
                subresource->rtvHandle.cpuHandle,
                clearColor,
                0,
                NULL);
        }

        rtvs[i] = subresource->rtvHandle.cpuHandle;
        d3d12CommandBuffer->colorAttachmentTextureSubresources[i] = subresource;
        D3D12_INTERNAL_TrackTextureSubresource(d3d12CommandBuffer, subresource);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    if (depthStencilAttachmentInfo != NULL) {
        SDL_bool cycle;

        if (
            depthStencilAttachmentInfo->loadOp == SDL_GPU_LOADOP_LOAD ||
            depthStencilAttachmentInfo->stencilLoadOp == SDL_GPU_LOADOP_LOAD) {
            cycle = SDL_FALSE;
        } else {
            cycle = depthStencilAttachmentInfo->cycle;
        }

        D3D12TextureContainer *container = (D3D12TextureContainer *)depthStencilAttachmentInfo->textureSlice.texture;
        D3D12TextureSubresource *subresource = D3D12_INTERNAL_PrepareTextureSubresourceForWrite(
            d3d12CommandBuffer,
            container,
            depthStencilAttachmentInfo->textureSlice.layer,
            depthStencilAttachmentInfo->textureSlice.mipLevel,
            cycle,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);

        if (
            depthStencilAttachmentInfo->loadOp == SDL_GPU_LOADOP_LOAD ||
            depthStencilAttachmentInfo->stencilLoadOp == SDL_GPU_LOADOP_LOAD) {
            D3D12_CLEAR_FLAGS clearFlags = 0;
            if (depthStencilAttachmentInfo->loadOp == SDL_GPU_LOADOP_LOAD) {
                clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
            }
            if (depthStencilAttachmentInfo->stencilLoadOp == SDL_GPU_LOADOP_LOAD) {
                clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
            }

            ID3D12GraphicsCommandList_ClearDepthStencilView(
                d3d12CommandBuffer->graphicsCommandList,
                subresource->dsvHandle.cpuHandle,
                clearFlags,
                depthStencilAttachmentInfo->depthStencilClearValue.depth,
                depthStencilAttachmentInfo->depthStencilClearValue.stencil,
                0,
                NULL);
        }

        dsv = subresource->dsvHandle.cpuHandle;
        d3d12CommandBuffer->depthStencilTextureSubresource = subresource;
        D3D12_INTERNAL_TrackTextureSubresource(d3d12CommandBuffer, subresource);
    }

    ID3D12GraphicsCommandList_OMSetRenderTargets(
        d3d12CommandBuffer->graphicsCommandList,
        colorAttachmentCount,
        rtvs,
        SDL_FALSE,
        (depthStencilAttachmentInfo == NULL) ? NULL : &dsv);

    /* Set sensible default viewport state */
    SDL_GpuViewport defaultViewport;
    defaultViewport.x = 0;
    defaultViewport.y = 0;
    defaultViewport.w = framebufferWidth;
    defaultViewport.h = framebufferHeight;
    defaultViewport.minDepth = 0;
    defaultViewport.maxDepth = 1;

    D3D12_SetViewport(
        commandBuffer,
        &defaultViewport);

    SDL_GpuRect defaultScissor;
    defaultScissor.x = 0;
    defaultScissor.y = 0;
    defaultScissor.w = framebufferWidth;
    defaultScissor.h = framebufferHeight;

    D3D12_SetScissor(
        commandBuffer,
        &defaultScissor);
}

static void D3D12_INTERNAL_TrackUniformBuffer(
    D3D12CommandBuffer *commandBuffer,
    D3D12UniformBuffer *uniformBuffer)
{
    Uint32 i;
    for (i = 0; i < commandBuffer->usedUniformBufferCount; i += 1) {
        if (commandBuffer->usedUniformBuffers[i] == uniformBuffer) {
            return;
        }
    }

    if (commandBuffer->usedUniformBufferCount == commandBuffer->usedUniformBufferCapacity) {
        commandBuffer->usedUniformBufferCapacity += 1;
        commandBuffer->usedUniformBuffers = SDL_realloc(
            commandBuffer->usedUniformBuffers,
            commandBuffer->usedUniformBufferCapacity * sizeof(D3D12UniformBuffer *));
        for (i = commandBuffer->usedUniformBufferCount; i < commandBuffer->usedUniformBufferCapacity; ++i)
            SDL_zerop(commandBuffer->usedUniformBuffers[i]);
    }

    commandBuffer->usedUniformBuffers[commandBuffer->usedUniformBufferCount] = uniformBuffer;
    commandBuffer->usedUniformBufferCount += 1;
}

static D3D12UniformBuffer *D3D12_INTERNAL_AcquireUniformBufferFromPool(
    D3D12CommandBuffer *commandBuffer)
{
    D3D12Renderer *renderer = commandBuffer->renderer;
    D3D12UniformBuffer *uniformBuffer;

    SDL_LockMutex(renderer->acquireUniformBufferLock);

    if (renderer->uniformBufferPoolCount > 0) {
        uniformBuffer = renderer->uniformBufferPool[renderer->uniformBufferPoolCount - 1];
        renderer->uniformBufferPoolCount -= 1;
    } else {
        uniformBuffer = SDL_malloc(sizeof(D3D12UniformBuffer));
        uniformBuffer->buffer = D3D12_INTERNAL_CreateBuffer(
            renderer,
            0,
            UNIFORM_BUFFER_SIZE,
            D3D12_BUFFER_TYPE_UNIFORM);
    }

    SDL_UnlockMutex(renderer->acquireUniformBufferLock);

    uniformBuffer->currentBlockSize = 0;
    uniformBuffer->drawOffset = 0;
    uniformBuffer->writeOffset = 0;

    D3D12_INTERNAL_TrackUniformBuffer(commandBuffer, uniformBuffer);

    return uniformBuffer;
}

static void D3D12_INTERNAL_ReturnUniformBufferToPool(
    D3D12Renderer *renderer,
    D3D12UniformBuffer *uniformBuffer)
{
    if (renderer->uniformBufferPoolCount >= renderer->uniformBufferPoolCapacity) {
        renderer->uniformBufferPoolCapacity *= 2;
        renderer->uniformBufferPool = SDL_realloc(
            renderer->uniformBufferPool,
            renderer->uniformBufferPoolCapacity * sizeof(D3D12UniformBuffer *));
    }

    renderer->uniformBufferPool[renderer->uniformBufferPoolCount] = uniformBuffer;
    renderer->uniformBufferPoolCount += 1;
}

static void D3D12_BindGraphicsPipeline(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuGraphicsPipeline *graphicsPipeline)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12GraphicsPipeline *pipeline = (D3D12GraphicsPipeline *)graphicsPipeline;
    Uint32 i;

    d3d12CommandBuffer->currentGraphicsPipeline = pipeline;

    // Set the pipeline state
    ID3D12GraphicsCommandList_SetPipelineState(d3d12CommandBuffer->graphicsCommandList, pipeline->pipelineState);

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(d3d12CommandBuffer->graphicsCommandList, pipeline->rootSignature);

    ID3D12GraphicsCommandList_IASetPrimitiveTopology(d3d12CommandBuffer->graphicsCommandList, SDLToD3D12_PrimitiveType[pipeline->primitiveType]);

    float blendFactor[4] = {
        pipeline->blendConstants[0],
        pipeline->blendConstants[1],
        pipeline->blendConstants[2],
        pipeline->blendConstants[3]
    };
    ID3D12GraphicsCommandList_OMSetBlendFactor(d3d12CommandBuffer->graphicsCommandList, blendFactor);

    ID3D12GraphicsCommandList_OMSetStencilRef(d3d12CommandBuffer->graphicsCommandList, pipeline->stencilRef);

    // Mark that bindings are needed
    d3d12CommandBuffer->needVertexSamplerBind = SDL_TRUE;
    d3d12CommandBuffer->needVertexStorageTextureBind = SDL_TRUE;
    d3d12CommandBuffer->needVertexStorageBufferBind = SDL_TRUE;
    d3d12CommandBuffer->needFragmentSamplerBind = SDL_TRUE;
    d3d12CommandBuffer->needFragmentStorageTextureBind = SDL_TRUE;
    d3d12CommandBuffer->needFragmentStorageBufferBind = SDL_TRUE;

    for (i = 0; i < MAX_UNIFORM_BUFFERS_PER_STAGE; i += 1) {
        d3d12CommandBuffer->needVertexUniformBufferBind[i] = SDL_TRUE;
        d3d12CommandBuffer->needFragmentUniformBufferBind[i] = SDL_TRUE;
    }

    for (i = 0; i < pipeline->vertexUniformBufferCount; i += 1) {
        if (d3d12CommandBuffer->vertexUniformBuffers[i] == NULL) {
            d3d12CommandBuffer->vertexUniformBuffers[i] = D3D12_INTERNAL_AcquireUniformBufferFromPool(
                d3d12CommandBuffer);
        }
    }

    for (i = 0; i < pipeline->fragmentUniformBufferCount; i += 1) {
        if (d3d12CommandBuffer->fragmentUniformBuffers[i] == NULL) {
            d3d12CommandBuffer->fragmentUniformBuffers[i] = D3D12_INTERNAL_AcquireUniformBufferFromPool(
                d3d12CommandBuffer);
        }
    }
}

static void D3D12_BindVertexBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstBinding,
    SDL_GpuBufferBinding *pBindings,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_BindIndexBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBufferBinding *pBinding,
    SDL_GpuIndexElementSize indexElementSize) { SDL_assert(SDL_FALSE); }

static void D3D12_BindVertexSamplers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSamplerBinding *textureSamplerBindings,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_BindVertexStorageTextures(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSlice *storageTextureSlices,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_BindVertexStorageBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuBuffer **storageBuffers,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_BindFragmentSamplers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSamplerBinding *textureSamplerBindings,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_BindFragmentStorageTextures(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSlice *storageTextureSlices,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_BindFragmentStorageBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuBuffer **storageBuffers,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_PushVertexUniformData(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 slotIndex,
    const void *data,
    Uint32 dataLengthInBytes) { SDL_assert(SDL_FALSE); }

static void D3D12_PushFragmentUniformData(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 slotIndex,
    const void *data,
    Uint32 dataLengthInBytes) { SDL_assert(SDL_FALSE); }

static void D3D12_DrawIndexedPrimitives(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 baseVertex,
    Uint32 startIndex,
    Uint32 primitiveCount,
    Uint32 instanceCount) { SDL_assert(SDL_FALSE); }

static void D3D12_INTERNAL_WriteGPUDescriptors(
    D3D12CommandBuffer *commandBuffer,
    D3D12_DESCRIPTOR_HEAP_TYPE heapType,
    D3D12_CPU_DESCRIPTOR_HANDLE *resourceDescriptorHandles,
    Uint32 resourceHandleCount,
    D3D12_GPU_DESCRIPTOR_HANDLE *gpuBaseDescriptor)
{
    D3D12DescriptorHeap *heap = commandBuffer->gpuDescriptorHeaps[heapType];
    D3D12_CPU_DESCRIPTOR_HANDLE gpuHeapCpuHandle;

    /* FIXME: need to error on overflow */
    gpuHeapCpuHandle.ptr = heap->descriptorHeapCPUStart.ptr + (heap->currentDescriptorIndex + heap->descriptorSize);
    gpuBaseDescriptor->ptr = heap->descriptorHeapGPUStart.ptr + (heap->currentDescriptorIndex + heap->descriptorSize);

    for (Uint32 i = 0; i < resourceHandleCount; i += 1) {
        ID3D12Device_CopyDescriptorsSimple(
            commandBuffer->renderer->device,
            1,
            gpuHeapCpuHandle,
            resourceDescriptorHandles[i],
            heapType);

        heap->currentDescriptorIndex += 1;
        gpuHeapCpuHandle.ptr += heap->descriptorSize;
    }
}

static void D3D12_INTERNAL_BindGraphicsResources(
    D3D12CommandBuffer *commandBuffer)
{
    D3D12GraphicsPipeline *graphicsPipeline = commandBuffer->currentGraphicsPipeline;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandles[MAX_TEXTURE_SAMPLERS_PER_STAGE];
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHandle;

    if (commandBuffer->needVertexSamplerBind) {
        if (graphicsPipeline->vertexSamplerCount > 0) {
            for (Uint32 i = 0; i < graphicsPipeline->vertexSamplerCount; i += 1) {
                cpuHandles[i] = commandBuffer->vertexSamplers[i]->handle.cpuHandle;
            }

            D3D12_INTERNAL_WriteGPUDescriptors(
                commandBuffer,
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                cpuHandles,
                graphicsPipeline->vertexSamplerCount,
                &gpuDescriptorHandle);

            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                0,
                gpuDescriptorHandle);

            for (Uint32 i = 0; i < graphicsPipeline->vertexSamplerCount; i += 1) {
                cpuHandles[i] = commandBuffer->vertexSamplerTextures[i]->srvHandle.cpuHandle;
            }

            D3D12_INTERNAL_WriteGPUDescriptors(
                commandBuffer,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                cpuHandles,
                graphicsPipeline->vertexSamplerCount,
                &gpuDescriptorHandle);

            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                1,
                gpuDescriptorHandle);
        }
        commandBuffer->needVertexSamplerBind = SDL_FALSE;
    }

    if (commandBuffer->needVertexStorageTextureBind) {
        if (graphicsPipeline->vertexStorageTextureCount > 0) {
            for (Uint32 i = 0; i < graphicsPipeline->vertexStorageTextureCount; i += 1) {
                cpuHandles[i] = commandBuffer->vertexStorageTextureSlices[i]->srvHandle.cpuHandle;
            }

            D3D12_INTERNAL_WriteGPUDescriptors(
                commandBuffer,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                cpuHandles,
                graphicsPipeline->vertexStorageTextureCount,
                &gpuDescriptorHandle);

            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                2,
                gpuDescriptorHandle);
        }
        commandBuffer->needVertexStorageTextureBind = SDL_FALSE;
    }

    if (commandBuffer->needVertexStorageBufferBind) {
        if (graphicsPipeline->vertexStorageBufferCount > 0) {
            for (Uint32 i = 0; i < graphicsPipeline->vertexStorageBufferCount; i += 1) {
                cpuHandles[i] = commandBuffer->vertexStorageBuffers[i]->srvDescriptor.cpuHandle;
            }

            D3D12_INTERNAL_WriteGPUDescriptors(
                commandBuffer,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                cpuHandles,
                graphicsPipeline->vertexStorageBufferCount,
                &gpuDescriptorHandle);

            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                3,
                gpuDescriptorHandle);
        }
        commandBuffer->needVertexStorageBufferBind = SDL_FALSE;
    }

    for (Uint32 i = 0; i < MAX_UNIFORM_BUFFERS_PER_STAGE; i += 1) {
        if (commandBuffer->needVertexUniformBufferBind[i]) {
            if (graphicsPipeline->vertexUniformBufferCount > i) {
                ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
                    commandBuffer->graphicsCommandList,
                    4 + i,
                    commandBuffer->vertexUniformBuffers[i]->buffer->virtualAddress + commandBuffer->vertexUniformBuffers[i]->drawOffset);
            }
            commandBuffer->needVertexUniformBufferBind[i] = SDL_FALSE;
        }
    }

    if (commandBuffer->needFragmentSamplerBind) {
        if (graphicsPipeline->fragmentSamplerCount > 0) {
            for (Uint32 i = 0; i < graphicsPipeline->fragmentSamplerCount; i += 1) {
                cpuHandles[i] = commandBuffer->fragmentSamplers[i]->handle.cpuHandle;
            }

            D3D12_INTERNAL_WriteGPUDescriptors(
                commandBuffer,
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                cpuHandles,
                graphicsPipeline->fragmentSamplerCount,
                &gpuDescriptorHandle);

            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                5,
                gpuDescriptorHandle);

            for (Uint32 i = 0; i < graphicsPipeline->fragmentSamplerCount; i += 1) {
                cpuHandles[i] = commandBuffer->fragmentSamplerTextures[i]->srvHandle.cpuHandle;
            }

            D3D12_INTERNAL_WriteGPUDescriptors(
                commandBuffer,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                cpuHandles,
                graphicsPipeline->fragmentSamplerCount,
                &gpuDescriptorHandle);

            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                6,
                gpuDescriptorHandle);
        }
        commandBuffer->needFragmentSamplerBind = SDL_FALSE;
    }

    if (commandBuffer->needFragmentStorageTextureBind) {
        if (graphicsPipeline->fragmentStorageTextureCount > 0) {
            for (Uint32 i = 0; i < graphicsPipeline->fragmentStorageTextureCount; i += 1) {
                cpuHandles[i] = commandBuffer->fragmentStorageTextureSlices[i]->srvHandle.cpuHandle;
            }

            D3D12_INTERNAL_WriteGPUDescriptors(
                commandBuffer,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                cpuHandles,
                graphicsPipeline->fragmentStorageTextureCount,
                &gpuDescriptorHandle);

            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                7,
                gpuDescriptorHandle);
        }
        commandBuffer->needFragmentStorageTextureBind = SDL_FALSE;
    }

    if (commandBuffer->needFragmentStorageBufferBind) {
        if (graphicsPipeline->fragmentStorageBufferCount > 0) {
            for (Uint32 i = 0; i < graphicsPipeline->fragmentStorageBufferCount; i += 1) {
                cpuHandles[i] = commandBuffer->fragmentStorageBuffers[i]->srvDescriptor.cpuHandle;
            }

            D3D12_INTERNAL_WriteGPUDescriptors(
                commandBuffer,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                cpuHandles,
                graphicsPipeline->fragmentStorageBufferCount,
                &gpuDescriptorHandle);

            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                8,
                gpuDescriptorHandle);
        }
        commandBuffer->needFragmentStorageBufferBind = SDL_FALSE;
    }

    for (Uint32 i = 0; i < MAX_UNIFORM_BUFFERS_PER_STAGE; i += 1) {
        if (commandBuffer->needFragmentUniformBufferBind[i]) {
            if (graphicsPipeline->fragmentUniformBufferCount > i) {
                ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
                    commandBuffer->graphicsCommandList,
                    12 + i,
                    commandBuffer->fragmentUniformBuffers[i]->buffer->virtualAddress + commandBuffer->fragmentUniformBuffers[i]->drawOffset);
            }
            commandBuffer->needFragmentUniformBufferBind[i] = SDL_FALSE;
        }
    }
}

static void D3D12_DrawPrimitives(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 vertexStart,
    Uint32 primitiveCount)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12_INTERNAL_BindGraphicsResources(d3d12CommandBuffer);

    // Record the draw call
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(
        d3d12CommandBuffer->graphicsCommandList,
        SDLToD3D12_PrimitiveType[d3d12CommandBuffer->currentGraphicsPipeline->primitiveType]);

    ID3D12GraphicsCommandList_DrawInstanced(
        d3d12CommandBuffer->graphicsCommandList,
        PrimitiveVerts(d3d12CommandBuffer->currentGraphicsPipeline->primitiveType, primitiveCount),
        1, // Instance count
        vertexStart,
        0 // Start instance location
    );
}

static void D3D12_DrawPrimitivesIndirect(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBuffer *buffer,
    Uint32 offsetInBytes,
    Uint32 drawCount,
    Uint32 stride) { SDL_assert(SDL_FALSE); }

static void D3D12_DrawIndexedPrimitivesIndirect(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBuffer *buffer,
    Uint32 offsetInBytes,
    Uint32 drawCount,
    Uint32 stride) { SDL_assert(SDL_FALSE); }

static void D3D12_EndRenderPass(
    SDL_GpuCommandBuffer *commandBuffer)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    Uint32 i;

    for (i = 0; i < d3d12CommandBuffer->colorAttachmentCount; i += 1) {
        D3D12_INTERNAL_TextureSubresourceTransitionToDefaultUsage(
            d3d12CommandBuffer,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            d3d12CommandBuffer->colorAttachmentTextureSubresources[i]);

        d3d12CommandBuffer->colorAttachmentTextureSubresources[i] = NULL;
    }
    d3d12CommandBuffer->colorAttachmentCount = 0;

    if (d3d12CommandBuffer->depthStencilTextureSubresource != NULL) {
        D3D12_INTERNAL_TextureSubresourceTransitionToDefaultUsage(
            d3d12CommandBuffer,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            d3d12CommandBuffer->depthStencilTextureSubresource);

        d3d12CommandBuffer->depthStencilTextureSubresource = NULL;
    }

    d3d12CommandBuffer->currentGraphicsPipeline = NULL;

    ID3D12GraphicsCommandList_OMSetRenderTargets(
        d3d12CommandBuffer->graphicsCommandList,
        0,
        NULL,
        SDL_FALSE,
        NULL);
}

/* Compute Pass */

static void D3D12_BeginComputePass(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuStorageTextureReadWriteBinding *storageTextureBindings,
    Uint32 storageTextureBindingCount,
    SDL_GpuStorageBufferReadWriteBinding *storageBufferBindings,
    Uint32 storageBufferBindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_BindComputePipeline(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuComputePipeline *computePipeline) { SDL_assert(SDL_FALSE); }

static void D3D12_BindComputeStorageTextures(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSlice *storageTextureSlices,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_BindComputeStorageBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuBuffer **storageBuffers,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

static void D3D12_PushComputeUniformData(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 slotIndex,
    const void *data,
    Uint32 dataLengthInBytes) { SDL_assert(SDL_FALSE); }

static void D3D12_DispatchCompute(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 groupCountX,
    Uint32 groupCountY,
    Uint32 groupCountZ) { SDL_assert(SDL_FALSE); }

static void D3D12_DispatchComputeIndirect(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBuffer *buffer,
    Uint32 offsetInBytes) { SDL_assert(SDL_FALSE); }

static void D3D12_EndComputePass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

/* TransferBuffer Data */

static void D3D12_MapTransferBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBuffer *transferBuffer,
    SDL_bool cycle,
    void **ppData)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12BufferContainer *container = (D3D12BufferContainer *)transferBuffer;

    if (
        cycle &&
        SDL_AtomicGet(&container->activeBuffer->referenceCount) > 0) {
        D3D12_INTERNAL_CycleActiveBuffer(
            renderer,
            container);
    }

    /* Upload buffers are persistently mapped, download buffers are not */
    if (container->type == D3D12_BUFFER_TYPE_UPLOAD) {
        *ppData = container->activeBuffer->mapPointer;
    } else {
        ID3D12Resource_Map(
            container->activeBuffer->handle,
            0,
            NULL,
            ppData);
    }
}

static void D3D12_UnmapTransferBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBuffer *transferBuffer)
{
    (void)driverData;
    D3D12BufferContainer *container = (D3D12BufferContainer *)transferBuffer;

    /* Upload buffers are persistently mapped, download buffers are not */
    if (container->type == D3D12_BUFFER_TYPE_DOWNLOAD) {
        ID3D12Resource_Unmap(
            container->activeBuffer->handle,
            0,
            NULL);
    }
}

static void D3D12_SetTransferData(
    SDL_GpuRenderer *driverData,
    const void *source,
    SDL_GpuTransferBufferRegion *destination,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

static void D3D12_GetTransferData(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBufferRegion *source,
    void *destination) { SDL_assert(SDL_FALSE); }

/* Copy Pass */

static void D3D12_BeginCopyPass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

static void D3D12_UploadToTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureTransferInfo *source,
    SDL_GpuTextureRegion *destination,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

static void D3D12_UploadToBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTransferBufferLocation *source,
    SDL_GpuBufferRegion *destination,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

static void D3D12_CopyTextureToTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureLocation *source,
    SDL_GpuTextureLocation *destination,
    Uint32 w,
    Uint32 h,
    Uint32 d,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

static void D3D12_CopyBufferToBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBufferLocation *source,
    SDL_GpuBufferLocation *destination,
    Uint32 size,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

static void D3D12_GenerateMipmaps(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTexture *texture) { SDL_assert(SDL_FALSE); }

static void D3D12_DownloadFromTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureRegion *source,
    SDL_GpuTextureTransferInfo *destination) { SDL_assert(SDL_FALSE); }

static void D3D12_DownloadFromBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBufferRegion *source,
    SDL_GpuTransferBufferLocation *destination) { SDL_assert(SDL_FALSE); }

static void D3D12_EndCopyPass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

static void D3D12_Blit(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureRegion *source,
    SDL_GpuTextureRegion *destination,
    SDL_GpuFilter filterMode,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

/* Submission/Presentation */

static SDL_bool D3D12_SupportsSwapchainComposition(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuSwapchainComposition swapchainComposition)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    DXGI_FORMAT format;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { 0 };
    HRESULT res;

    format = SwapchainCompositionToTextureFormat[swapchainComposition];

    formatSupport.Format = format;
    res = ID3D12Device_CheckFeatureSupport(
        renderer->device,
        D3D12_FEATURE_FORMAT_SUPPORT,
        &formatSupport,
        sizeof(formatSupport));
    if (FAILED(res)) {
        /* Format is apparently unknown */
        return SDL_FALSE;
    }

    return (formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_DISPLAY) != 0;
}

static SDL_bool D3D12_SupportsPresentMode(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuPresentMode presentMode)
{
    (void)driverData;
    (void)window;

    SDL_bool result = SDL_FALSE;
    switch (presentMode) {
    case SDL_GPU_PRESENTMODE_IMMEDIATE:
    case SDL_GPU_PRESENTMODE_VSYNC:
    case SDL_GPU_PRESENTMODE_MAILBOX:
        result = SDL_TRUE;
        break;
    default:
        SDL_assert(!"Unrecognized present mode");
        result = SDL_FALSE;
        break;
    }
    return result;
}

static D3D12WindowData *D3D12_INTERNAL_FetchWindowData(
    SDL_Window *window)
{
    SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    return (D3D12WindowData *)SDL_GetPointerProperty(properties, WINDOW_PROPERTY_DATA, NULL);
}

static SDL_bool D3D12_INTERNAL_InitializeSwapchainTexture(
    D3D12Renderer *renderer,
    IDXGISwapChain3 *swapchain,
    DXGI_FORMAT swapchainFormat,
    DXGI_FORMAT rtvFormat,
    Uint32 index,
    D3D12TextureContainer *pTextureContainer)
{
    D3D12Texture *pTexture;
    ID3D12Resource *swapchainTexture;
    D3D12_RESOURCE_DESC textureDesc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
    HRESULT res;

    res = IDXGISwapChain_GetBuffer(
        swapchain,
        index,
        &D3D_IID_ID3D12Resource,
        (void **)&swapchainTexture);
    ERROR_CHECK_RETURN("Could not get buffer from swapchain!", 0);

    pTexture = SDL_malloc(sizeof(D3D12Texture));
    pTexture->resource = NULL; /* This will be set in AcquireSwapchainTexture */
    pTexture->subresourceCount = 1;
    pTexture->subresources = SDL_malloc(sizeof(D3D12TextureSubresource));
    pTexture->subresources[0].srvHandle.heap = NULL;
    pTexture->subresources[0].dsvHandle.heap = NULL;
    pTexture->subresources[0].rtvHandle.heap = NULL;
    pTexture->subresources[0].uavHandle.heap = NULL;
    pTexture->subresources[0].parent = pTexture;
    pTexture->subresources[0].index = 0;
    pTexture->subresources[0].layer = 0;
    pTexture->subresources[0].level = 0;
    SDL_AtomicSet(&pTexture->subresources[0].referenceCount, 0);

    ID3D12Resource_GetDesc(swapchainTexture, &textureDesc);
    pTextureContainer->createInfo.width = textureDesc.Width;
    pTextureContainer->createInfo.height = textureDesc.Height;
    pTextureContainer->createInfo.depth = 1;
    pTextureContainer->createInfo.layerCount = 1;
    pTextureContainer->createInfo.levelCount = 1;
    pTextureContainer->createInfo.isCube = 0;
    pTextureContainer->createInfo.usageFlags =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET_BIT |
        SDL_GPU_TEXTUREUSAGE_SAMPLER_BIT;
    pTextureContainer->createInfo.sampleCount = SDL_GPU_SAMPLECOUNT_1;
    pTextureContainer->createInfo.format = 0; /* this is never used */

    pTextureContainer->debugName = NULL;
    pTextureContainer->textures = SDL_malloc(sizeof(D3D12Texture *));
    pTextureContainer->textureCapacity = 1;
    pTextureContainer->textureCount = 1;
    pTextureContainer->textures[0] = pTexture;
    pTextureContainer->activeTexture = pTexture;
    pTextureContainer->canBeCycled = SDL_FALSE;

    pTexture->container = pTextureContainer;
    pTexture->containerIndex = 0;

    /* Create the SRV for the swapchain */
    D3D12_INTERNAL_AssignCpuDescriptorHandle(
        renderer,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        &pTexture->srvHandle);

    srvDesc.Format = swapchainFormat;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0;
    srvDesc.Texture2D.PlaneSlice = 0;

    ID3D12Device_CreateShaderResourceView(
        renderer->device,
        swapchainTexture,
        &srvDesc,
        pTexture->srvHandle.cpuHandle);

    /* Create the RTV for the swapchain */
    D3D12_INTERNAL_AssignCpuDescriptorHandle(
        renderer,
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        &pTexture->subresources[0].rtvHandle);

    rtvDesc.Format = rtvFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    rtvDesc.Texture2D.PlaneSlice = 0;

    ID3D12Device_CreateRenderTargetView(
        renderer->device,
        swapchainTexture,
        &rtvDesc,
        pTexture->subresources[0].rtvHandle.cpuHandle);

    ID3D12Resource_Release(swapchainTexture);

    return SDL_TRUE;
}

static SDL_bool D3D12_INTERNAL_ResizeSwapchain(
    D3D12Renderer *renderer,
    D3D12WindowData *windowData,
    Sint32 width,
    Sint32 height)
{
    /* Wait so we don't release in-flight views */
    D3D12_Wait((SDL_GpuRenderer *)renderer);

    /* Release views and clean up */
    for (Uint32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i += 1) {
        D3D12_INTERNAL_ReleaseCpuDescriptorHandle(
            renderer,
            &windowData->textureContainers[i].activeTexture->srvHandle);
        D3D12_INTERNAL_ReleaseCpuDescriptorHandle(
            renderer,
            &windowData->textureContainers[i].activeTexture->subresources[0].rtvHandle);

        SDL_free(windowData->textureContainers[i].activeTexture->subresources);
        SDL_free(windowData->textureContainers[i].activeTexture);
        SDL_free(windowData->textureContainers[i].textures);
    }

    /* Resize the swapchain */
    HRESULT res = IDXGISwapChain_ResizeBuffers(
        windowData->swapchain,
        0, /* Keep buffer count the same */
        width,
        height,
        DXGI_FORMAT_UNKNOWN, /* Keep the old format */
        renderer->supportsTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ERROR_CHECK_RETURN("Could not resize swapchain buffers", 0)

    /* Create texture object for the swapchain */
    for (Uint32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i += 1) {
        if (!D3D12_INTERNAL_InitializeSwapchainTexture(
                renderer,
                windowData->swapchain,
                windowData->swapchainFormat,
                (windowData->swapchainComposition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : windowData->swapchainFormat,
                i,
                &windowData->textureContainers[i])) {
            return SDL_FALSE;
        }
    }

    return SDL_TRUE;
}

static void D3D12_INTERNAL_DestroySwapchain(
    D3D12Renderer *renderer,
    D3D12WindowData *windowData)
{
    /* Release views and clean up */
    for (Uint32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i += 1) {
        D3D12_INTERNAL_ReleaseCpuDescriptorHandle(
            renderer,
            &windowData->textureContainers[i].activeTexture->srvHandle);
        D3D12_INTERNAL_ReleaseCpuDescriptorHandle(
            renderer,
            &windowData->textureContainers[i].activeTexture->subresources[0].rtvHandle);

        SDL_free(windowData->textureContainers[i].activeTexture->subresources);
        SDL_free(windowData->textureContainers[i].activeTexture);
        SDL_free(windowData->textureContainers[i].textures);
    }

    IDXGISwapChain_Release(windowData->swapchain);
    windowData->swapchain = NULL;
}

static SDL_bool D3D12_INTERNAL_CreateSwapchain(
    D3D12Renderer *renderer,
    D3D12WindowData *windowData,
    SDL_GpuSwapchainComposition swapchainComposition,
    SDL_GpuPresentMode presentMode)
{
    HWND dxgiHandle;
    int width, height;
    // Uint32 i;
    DXGI_SWAP_CHAIN_DESC1 swapchainDesc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc;
    DXGI_FORMAT swapchainFormat;
    IDXGIFactory1 *pParent;
    IDXGISwapChain1 *swapchain;
    IDXGISwapChain3 *swapchain3;
    Uint32 colorSpaceSupport;
    HRESULT res;

    /* Get the DXGI handle */
#ifdef _WIN32
    dxgiHandle = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(windowData->window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#else
    dxgiHandle = (HWND)windowData->window;
#endif

    /* Get the window size */
    SDL_GetWindowSize(windowData->window, &width, &height);

    swapchainFormat = SwapchainCompositionToTextureFormat[swapchainComposition];

    SDL_zero(swapchainDesc);
    SDL_zero(fullscreenDesc);

    // Initialize the swapchain buffer descriptor
    swapchainDesc.Width = 0;
    swapchainDesc.Height = 0;
    swapchainDesc.Format = swapchainFormat;
    swapchainDesc.SampleDesc.Count = 1;
    swapchainDesc.SampleDesc.Quality = 0;
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // | DXGI_USAGE_UNORDERED_ACCESS | DXGI_USAGE_SHADER_INPUT;
    swapchainDesc.BufferCount = MAX_FRAMES_IN_FLIGHT;
    swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapchainDesc.Flags = 0;

    // Initialize the fullscreen descriptor (if needed)
    fullscreenDesc.RefreshRate.Numerator = 0;
    fullscreenDesc.RefreshRate.Denominator = 0;
    fullscreenDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    fullscreenDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    fullscreenDesc.Windowed = SDL_TRUE;

    if (renderer->supportsTearing) {
        swapchainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    } else {
        swapchainDesc.Flags = 0;
    }

#ifndef SDL_PLATFORM_WINRT
    if (!IsWindow(dxgiHandle)) {
        return SDL_FALSE;
    }
#endif

    /* Create the swapchain! */
    res = IDXGIFactory4_CreateSwapChainForHwnd(
        renderer->factory,
        (IUnknown *)renderer->commandQueue,
        dxgiHandle,
        &swapchainDesc,
        &fullscreenDesc,
        NULL,
        &swapchain);
    ERROR_CHECK_RETURN("Could not create swapchain", 0);

    res = IDXGISwapChain1_QueryInterface(
        swapchain,
        &D3D_IID_IDXGISwapChain3,
        (void **)&swapchain3);
    IDXGISwapChain1_Release(swapchain);
    ERROR_CHECK_RETURN("Could not create IDXGISwapChain3", 0);

    IDXGISwapChain3_CheckColorSpaceSupport(
        swapchain3,
        windowData->swapchainColorSpace,
        &colorSpaceSupport);

    if (!(colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Requested colorspace is unsupported!");
        return SDL_FALSE;
    }

    IDXGISwapChain3_SetColorSpace1(
        swapchain3,
        windowData->swapchainColorSpace);

    /*
     * The swapchain's parent is a separate factory from the factory that
     * we used to create the swapchain, and only that parent can be used to
     * set the window association. Trying to set an association on our factory
     * will silently fail and doesn't even verify arguments or return errors.
     * See https://gamedev.net/forums/topic/634235-dxgidisabling-altenter/4999955/
     */
    res = IDXGISwapChain3_GetParent(
        swapchain3,
        &D3D_IID_IDXGIFactory1,
        (void **)&pParent);
    if (FAILED(res)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Could not get swapchain parent! Error Code: " HRESULT_FMT,
            res);
    } else {
        /* Disable DXGI window crap */
        res = IDXGIFactory1_MakeWindowAssociation(
            pParent,
            dxgiHandle,
            DXGI_MWA_NO_WINDOW_CHANGES);
        if (FAILED(res)) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "MakeWindowAssociation failed! Error Code: " HRESULT_FMT,
                res);
        }

        /* We're done with the parent now */
        IDXGIFactory1_Release(pParent);
    }
    /* Initialize the swapchain data */
    windowData->swapchain = swapchain3;
    windowData->presentMode = presentMode;
    windowData->swapchainComposition = swapchainComposition;
    windowData->swapchainFormat = swapchainFormat;
    windowData->swapchainColorSpace = SwapchainCompositionToColorSpace[swapchainComposition];
    windowData->frameCounter = 0;

    /* If a you are using a FLIP model format you can't create the swapchain as DXGI_FORMAT_B8G8R8A8_UNORM_SRGB.
     * You have to create the swapchain as DXGI_FORMAT_B8G8R8A8_UNORM and then set the render target view's format to DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
     */
    for (Uint32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i += 1) {
        if (!D3D12_INTERNAL_InitializeSwapchainTexture(
                renderer,
                swapchain3,
                swapchainFormat,
                (swapchainComposition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : windowData->swapchainFormat,
                i,
                &windowData->textureContainers[i])) {
            IDXGISwapChain3_Release(swapchain3);
            return SDL_FALSE;
        }
    }

    return SDL_TRUE;
}

static SDL_bool D3D12_ClaimWindow(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuSwapchainComposition swapchainComposition,
    SDL_GpuPresentMode presentMode)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12WindowData *windowData = D3D12_INTERNAL_FetchWindowData(window);

    if (windowData == NULL) {
        windowData = (D3D12WindowData *)SDL_malloc(sizeof(D3D12WindowData));
        SDL_zerop(windowData);
        windowData->window = window;

        if (D3D12_INTERNAL_CreateSwapchain(renderer, windowData, swapchainComposition, presentMode)) {
            SDL_SetPointerProperty(SDL_GetWindowProperties(window), WINDOW_PROPERTY_DATA, windowData);

            SDL_LockMutex(renderer->windowLock);

            if (renderer->claimedWindowCount >= renderer->claimedWindowCapacity) {
                renderer->claimedWindowCapacity *= 2;
                renderer->claimedWindows = SDL_realloc(
                    renderer->claimedWindows,
                    renderer->claimedWindowCapacity * sizeof(D3D12WindowData *));
            }
            renderer->claimedWindows[renderer->claimedWindowCount] = windowData;
            renderer->claimedWindowCount += 1;

            SDL_UnlockMutex(renderer->windowLock);

            return SDL_TRUE;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not create swapchain, failed to claim window!");
            SDL_free(windowData);
            return SDL_FALSE;
        }
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Window already claimed!");
        return SDL_FALSE;
    }
}

static void D3D12_UnclaimWindow(
    SDL_GpuRenderer *driverData,
    SDL_Window *window)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12WindowData *windowData = D3D12_INTERNAL_FetchWindowData(window);

    if (windowData == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Window already unclaimed!");
        return;
    }

    D3D12_Wait(driverData);

    for (Uint32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i += 1) {
        if (windowData->inFlightFences[i] != NULL) {
            D3D12_ReleaseFence(
                driverData,
                (SDL_GpuFence *)windowData->inFlightFences[i]);
        }
    }

    D3D12_INTERNAL_DestroySwapchain(renderer, windowData);

    SDL_LockMutex(renderer->windowLock);
    for (Uint32 i = 0; i < renderer->claimedWindowCount; i += 1) {
        if (renderer->claimedWindows[i]->window == window) {
            renderer->claimedWindows[i] = renderer->claimedWindows[renderer->claimedWindowCount - 1];
            renderer->claimedWindowCount -= 1;
            break;
        }
    }
    SDL_UnlockMutex(renderer->windowLock);

    SDL_free(windowData);
    SDL_ClearProperty(SDL_GetWindowProperties(window), WINDOW_PROPERTY_DATA);
}

static SDL_bool D3D12_SetSwapchainParameters(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuSwapchainComposition swapchainComposition,
    SDL_GpuPresentMode presentMode)
{
    SDL_assert(SDL_FALSE);
    return SDL_FALSE;
}

static SDL_GpuTextureFormat D3D12_GetSwapchainTextureFormat(
    SDL_GpuRenderer *driverData,
    SDL_Window *window)
{
    D3D12WindowData *windowData = D3D12_INTERNAL_FetchWindowData(window);

    if (windowData == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Cannot get swapchain format, window has not been claimed!");
        return 0;
    }

    switch (windowData->swapchainFormat) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8;

    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_SRGB;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_SFLOAT;

    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return SDL_GPU_TEXTUREFORMAT_R10G10B10A2;

    default:
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Unrecognized swapchain format!");
        return 0;
    }
}

static D3D12Fence *D3D12_INTERNAL_AcquireFence(
    D3D12Renderer *renderer)
{
    D3D12Fence *fence;
    ID3D12Fence *handle;
    HRESULT res;

    SDL_LockMutex(renderer->fenceLock);

    if (renderer->availableFenceCount == 0) {
        res = ID3D12Device_CreateFence(
            renderer->device,
            D3D12_FENCE_UNSIGNALED_VALUE,
            D3D12_FENCE_FLAG_NONE,
            &D3D_IID_ID3D12Fence,
            (void **)&handle);
        ERROR_CHECK_RETURN("Failed to create fence!", NULL)

        fence = SDL_malloc(sizeof(D3D12Fence));
        fence->handle = handle;
        fence->event = CreateEventEx(NULL, 0, 0, EVENT_ALL_ACCESS);
        SDL_AtomicSet(&fence->referenceCount, 0);
    } else {
        fence = renderer->availableFences[renderer->availableFenceCount - 1];
        renderer->availableFenceCount -= 1;
        ID3D12Fence_Signal(fence->handle, D3D12_FENCE_UNSIGNALED_VALUE);
    }

    SDL_UnlockMutex(renderer->fenceLock);
    return fence;
}

static void D3D12_INTERNAL_AllocateCommandBuffer(
    D3D12Renderer *renderer)
{
    D3D12CommandBuffer *commandBuffer;
    HRESULT res;
    ID3D12CommandAllocator *commandAllocator;
    ID3D12GraphicsCommandList *commandList;

    res = ID3D12Device_CreateCommandAllocator(
        renderer->device,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        &D3D_IID_ID3D12CommandAllocator,
        (void **)&commandAllocator);
    if (FAILED(res)) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to create ID3D12CommandAllocator");
        return;
    }

    res = ID3D12Device_CreateCommandList(
        renderer->device,
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocator,
        NULL,
        &D3D_IID_ID3D12GraphicsCommandList,
        (void **)&commandList);

    if (FAILED(res)) {
        ID3D12CommandAllocator_Release(commandAllocator);
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to create ID3D12CommandList");
        return;
    }

    renderer->availableCommandBufferCapacity += 1;

    renderer->availableCommandBuffers = SDL_realloc(
        renderer->availableCommandBuffers,
        sizeof(D3D12CommandBuffer *) * renderer->availableCommandBufferCapacity);

    commandBuffer = SDL_malloc(sizeof(D3D12CommandBuffer));
    commandBuffer->renderer = renderer;
    commandBuffer->commandAllocator = commandAllocator;
    commandBuffer->graphicsCommandList = commandList;
    commandBuffer->inFlightFence = NULL;

    /* Window handling */
    commandBuffer->presentDataCapacity = 1;
    commandBuffer->presentDataCount = 0;
    commandBuffer->presentDatas = SDL_malloc(
        commandBuffer->presentDataCapacity * sizeof(D3D12PresentData));

    /* Resource tracking */
    commandBuffer->usedTextureSubresourceCapacity = 4;
    commandBuffer->usedTextureSubresourceCount = 0;
    commandBuffer->usedTextureSubresources = SDL_malloc(
        commandBuffer->usedTextureSubresourceCapacity * sizeof(D3D12TextureSubresource *));

    commandBuffer->usedUniformBufferCapacity = 4;
    commandBuffer->usedUniformBufferCount = 0;
    commandBuffer->usedUniformBuffers = SDL_malloc(
        commandBuffer->usedUniformBufferCapacity * sizeof(D3D12UniformBuffer *));

    /* Add to inactive command buffer array */
    renderer->availableCommandBuffers[renderer->availableCommandBufferCount] = commandBuffer;
    renderer->availableCommandBufferCount += 1;
}

static D3D12CommandBuffer *D3D12_INTERNAL_AcquireCommandBufferFromPool(
    D3D12Renderer *renderer)
{
    D3D12CommandBuffer *commandBuffer;

    if (renderer->availableCommandBufferCount == 0) {
        D3D12_INTERNAL_AllocateCommandBuffer(renderer);
    }

    commandBuffer = renderer->availableCommandBuffers[renderer->availableCommandBufferCount - 1];
    renderer->availableCommandBufferCount -= 1;

    return commandBuffer;
}

static SDL_GpuCommandBuffer *D3D12_AcquireCommandBuffer(
    SDL_GpuRenderer *driverData)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12CommandBuffer *commandBuffer;
    ID3D12DescriptorHeap *heaps[2];

    SDL_LockMutex(renderer->acquireCommandBufferLock);
    commandBuffer = D3D12_INTERNAL_AcquireCommandBufferFromPool(renderer);
    SDL_UnlockMutex(renderer->acquireCommandBufferLock);

    if (commandBuffer == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to acquire command buffer!");
        return NULL;
    }

    /* Set the descriptor heaps! */
    commandBuffer->gpuDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] =
        D3D12_INTERNAL_AcquireDescriptorHeapFromPool(commandBuffer, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    commandBuffer->gpuDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] =
        D3D12_INTERNAL_AcquireDescriptorHeapFromPool(commandBuffer, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    heaps[0] = commandBuffer->gpuDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]->handle;
    heaps[1] = commandBuffer->gpuDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER]->handle;

    ID3D12GraphicsCommandList_SetDescriptorHeaps(
        commandBuffer->graphicsCommandList,
        2,
        heaps);

    /* Set the bind state */
    commandBuffer->currentGraphicsPipeline = NULL;

    SDL_zeroa(commandBuffer->colorAttachmentTextureSubresources);
    commandBuffer->colorAttachmentCount = 0;
    commandBuffer->depthStencilTextureSubresource = NULL;

    SDL_zeroa(commandBuffer->vertexSamplerTextures);
    SDL_zeroa(commandBuffer->vertexSamplers);
    SDL_zeroa(commandBuffer->vertexStorageTextureSlices);
    SDL_zeroa(commandBuffer->vertexStorageBuffers);
    SDL_zeroa(commandBuffer->vertexUniformBuffers);

    SDL_zeroa(commandBuffer->fragmentSamplerTextures);
    SDL_zeroa(commandBuffer->fragmentSamplers);
    SDL_zeroa(commandBuffer->fragmentStorageTextureSlices);
    SDL_zeroa(commandBuffer->fragmentStorageBuffers);
    SDL_zeroa(commandBuffer->fragmentUniformBuffers);

    commandBuffer->autoReleaseFence = SDL_TRUE;

    return (SDL_GpuCommandBuffer *)commandBuffer;
}

static SDL_GpuTexture *D3D12_AcquireSwapchainTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_Window *window,
    Uint32 *pWidth,
    Uint32 *pHeight)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12Renderer *renderer = d3d12CommandBuffer->renderer;
    D3D12WindowData *windowData;
    DXGI_SWAP_CHAIN_DESC swapchainDesc;
    int w, h;
    HRESULT res;

    windowData = D3D12_INTERNAL_FetchWindowData(window);
    if (windowData == NULL) {
        return NULL;
    }

    /* Check for window size changes and resize the swapchain if needed. */
    IDXGISwapChain_GetDesc(windowData->swapchain, &swapchainDesc);
    SDL_GetWindowSize(window, &w, &h);

    if (w != swapchainDesc.BufferDesc.Width || h != swapchainDesc.BufferDesc.Height) {
        res = D3D12_INTERNAL_ResizeSwapchain(
            renderer,
            windowData,
            w,
            h);
        ERROR_CHECK_RETURN("Could not resize swapchain", NULL);
    }

    if (windowData->inFlightFences[windowData->frameCounter] != NULL) {
        if (windowData->presentMode == SDL_GPU_PRESENTMODE_VSYNC) {
            /* In VSYNC mode, block until the least recent presented frame is done */
            D3D12_WaitForFences(
                (SDL_GpuRenderer *)renderer,
                SDL_TRUE,
                (SDL_GpuFence **)&windowData->inFlightFences[windowData->frameCounter],
                1);
        } else {
            if (!D3D12_QueryFence(
                    (SDL_GpuRenderer *)renderer,
                    (SDL_GpuFence *)windowData->inFlightFences[windowData->frameCounter])) {
                /*
                 * In MAILBOX or IMMEDIATE mode, if the least recent fence is not signaled,
                 * return NULL to indicate that rendering should be skipped
                 */
                return NULL;
            }
        }

        D3D12_ReleaseFence(
            (SDL_GpuRenderer *)renderer,
            (SDL_GpuFence *)windowData->inFlightFences[windowData->frameCounter]);

        windowData->inFlightFences[windowData->frameCounter] = NULL;
    }

    Uint32 swapchainIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(windowData->swapchain);

    /* Set the handle on the windowData texture data. */
    res = IDXGISwapChain_GetBuffer(
        windowData->swapchain,
        swapchainIndex,
        &D3D_IID_ID3D12Resource,
        (void **)&windowData->textureContainers[swapchainIndex].activeTexture->resource);
    ERROR_CHECK_RETURN("Could not acquire swapchain!", NULL);

    /* Send the dimensions to the out parameters. */
    *pWidth = windowData->textureContainers[swapchainIndex].createInfo.width;
    *pHeight = windowData->textureContainers[swapchainIndex].createInfo.height;

    /* TODO: Set up the texture container */

    /* Set up presentation */
    if (d3d12CommandBuffer->presentDataCount == d3d12CommandBuffer->presentDataCapacity) {
        d3d12CommandBuffer->presentDataCapacity += 1;
        d3d12CommandBuffer->presentDatas = SDL_realloc(
            d3d12CommandBuffer->presentDatas,
            d3d12CommandBuffer->presentDataCapacity * sizeof(D3D12PresentData));
    }
    d3d12CommandBuffer->presentDatas[d3d12CommandBuffer->presentDataCount].windowData = windowData;
    d3d12CommandBuffer->presentDatas[d3d12CommandBuffer->presentDataCount].swapchainImageIndex = swapchainIndex;
    d3d12CommandBuffer->presentDataCount += 1;

    return (SDL_GpuTexture *)&windowData->textureContainers[swapchainIndex];
}

static void D3D12_INTERNAL_PerformPendingDestroys(D3D12Renderer *renderer)
{
    /* TODO */
}

static void D3D12_INTERNAL_CleanCommandBuffer(
    D3D12Renderer *renderer,
    D3D12CommandBuffer *commandBuffer)
{
    Uint32 i;
    HRESULT res;

    res = ID3D12CommandAllocator_Reset(commandBuffer->commandAllocator);
    ERROR_CHECK("Could not reset command allocator")

    res = ID3D12GraphicsCommandList_Reset(
        commandBuffer->graphicsCommandList,
        commandBuffer->commandAllocator,
        NULL);
    ERROR_CHECK("Could not reset graphicsCommandList")

    /* Return descriptor heaps to pool */
    D3D12_INTERNAL_ReturnDescriptorHeapToPool(
        renderer,
        commandBuffer->gpuDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]);
    D3D12_INTERNAL_ReturnDescriptorHeapToPool(
        renderer,
        commandBuffer->gpuDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER]);

    /* Uniform buffers are now available */
    SDL_LockMutex(renderer->acquireUniformBufferLock);

    for (i = 0; i < commandBuffer->usedUniformBufferCount; i += 1) {
        D3D12_INTERNAL_ReturnUniformBufferToPool(
            renderer,
            commandBuffer->usedUniformBuffers[i]);
    }
    commandBuffer->usedUniformBufferCount = 0;

    SDL_UnlockMutex(renderer->acquireUniformBufferLock);

    /* TODO: More reference counting */

    for (i = 0; i < commandBuffer->usedTextureSubresourceCount; i += 1) {
        (void)SDL_AtomicDecRef(&commandBuffer->usedTextureSubresources[i]->referenceCount);
    }
    commandBuffer->usedTextureSubresourceCount = 0;

    /* Reset presentation */
    commandBuffer->presentDataCount = 0;

    /* The fence is now available (unless SubmitAndAcquireFence was called) */
    if (commandBuffer->autoReleaseFence) {
        D3D12_ReleaseFence(
            (SDL_GpuRenderer *)renderer,
            (SDL_GpuFence *)commandBuffer->inFlightFence);

        commandBuffer->inFlightFence = NULL;
    }

    /* Return command buffer to pool */
    SDL_LockMutex(renderer->acquireCommandBufferLock);

    if (renderer->availableCommandBufferCount == renderer->availableCommandBufferCapacity) {
        renderer->availableCommandBufferCapacity += 1;
        renderer->availableCommandBuffers = SDL_realloc(
            renderer->availableCommandBuffers,
            renderer->availableCommandBufferCapacity * sizeof(D3D12CommandBuffer *));
    }

    renderer->availableCommandBuffers[renderer->availableCommandBufferCount] = commandBuffer;
    renderer->availableCommandBufferCount += 1;

    SDL_UnlockMutex(renderer->acquireCommandBufferLock);

    /* Remove this command buffer from the submitted list */
    for (i = 0; i < renderer->submittedCommandBufferCount; i += 1) {
        if (renderer->submittedCommandBuffers[i] == commandBuffer) {
            renderer->submittedCommandBuffers[i] = renderer->submittedCommandBuffers[renderer->submittedCommandBufferCount - 1];
            renderer->submittedCommandBufferCount -= 1;
        }
    }
}

static void D3D12_Submit(
    SDL_GpuCommandBuffer *commandBuffer)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12Renderer *renderer = d3d12CommandBuffer->renderer;
    ID3D12CommandList *commandLists[1];
    HRESULT res;

    SDL_LockMutex(renderer->submitLock);

    /* Transition present textures to present mode */
    for (Uint32 i = 0; i < d3d12CommandBuffer->presentDataCount; i += 1) {
        Uint32 swapchainIndex = d3d12CommandBuffer->presentDatas[i].swapchainImageIndex;
        D3D12TextureContainer *container = &d3d12CommandBuffer->presentDatas[i].windowData->textureContainers[swapchainIndex];
        D3D12TextureSubresource *subresource = D3D12_INTERNAL_FetchTextureSubresource(container, 0, 0);
        D3D12_INTERNAL_TextureSubresourceTransitionFromDefaultUsage(
            d3d12CommandBuffer,
            D3D12_RESOURCE_STATE_PRESENT,
            subresource);
    }

    /* Notify the command buffer that we have completed recording */
    res = ID3D12GraphicsCommandList_Close(d3d12CommandBuffer->graphicsCommandList);
    ERROR_CHECK("Failed to close command list!");

    ID3D12GraphicsCommandList_QueryInterface(
        d3d12CommandBuffer->graphicsCommandList,
        &D3D_IID_ID3D12CommandList,
        (void **)&commandLists[0]);

    /* Submit the command list to the queue */
    ID3D12CommandQueue_ExecuteCommandLists(
        renderer->commandQueue,
        1,
        commandLists);

    ID3D12CommandList_Release(commandLists[0]);

    /* Acquire a fence and set it to the in-flight fence */
    d3d12CommandBuffer->inFlightFence = D3D12_INTERNAL_AcquireFence(renderer);
    /* Command buffer has a reference to the in-flight fence */
    (void)SDL_AtomicIncRef(&d3d12CommandBuffer->inFlightFence->referenceCount);

    /* Mark that a fence should be signaled after command list execution */
    res = ID3D12CommandQueue_Signal(
        renderer->commandQueue,
        d3d12CommandBuffer->inFlightFence->handle,
        D3D12_FENCE_SIGNAL_VALUE);
    ERROR_CHECK("Failed to enqueue fence signal!");

    /* Mark the command buffer as submitted */
    if (renderer->submittedCommandBufferCount + 1 >= renderer->submittedCommandBufferCapacity) {
        renderer->submittedCommandBufferCapacity = renderer->submittedCommandBufferCount + 1;

        renderer->submittedCommandBuffers = SDL_realloc(
            renderer->submittedCommandBuffers,
            sizeof(D3D12CommandBuffer *) * renderer->submittedCommandBufferCapacity);
    }

    renderer->submittedCommandBuffers[renderer->submittedCommandBufferCount] = d3d12CommandBuffer;
    renderer->submittedCommandBufferCount += 1;

    /* Present, if applicable */
    for (Uint32 i = 0; i < d3d12CommandBuffer->presentDataCount; i += 1) {
        D3D12PresentData *presentData = &d3d12CommandBuffer->presentDatas[i];
        D3D12WindowData *windowData = presentData->windowData;

        /* NOTE: flip discard always supported since DXGI 1.4 is required */
        Uint32 syncInterval = 1;
        if (windowData->presentMode == SDL_GPU_PRESENTMODE_IMMEDIATE ||
            windowData->presentMode == SDL_GPU_PRESENTMODE_MAILBOX) {
            syncInterval = 0;
        }

        Uint32 presentFlags = 0;
        if (renderer->supportsTearing &&
            windowData->presentMode == SDL_GPU_PRESENTMODE_IMMEDIATE) {
            presentFlags = DXGI_PRESENT_ALLOW_TEARING;
        }

        IDXGISwapChain_Present(
            windowData->swapchain,
            syncInterval,
            presentFlags);

        ID3D12Resource_Release(windowData->textureContainers[presentData->swapchainImageIndex].activeTexture->resource);

        windowData->inFlightFences[windowData->frameCounter] = d3d12CommandBuffer->inFlightFence;
        (void)SDL_AtomicIncRef(&d3d12CommandBuffer->inFlightFence->referenceCount);
        windowData->frameCounter = (windowData->frameCounter + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    /* Check for cleanups */
    for (Sint32 i = renderer->submittedCommandBufferCount - 1; i >= 0; i -= 1) {
        Uint64 fenceValue = ID3D12Fence_GetCompletedValue(
            renderer->submittedCommandBuffers[i]->inFlightFence->handle);

        if (fenceValue == D3D12_FENCE_SIGNAL_VALUE) {
            D3D12_INTERNAL_CleanCommandBuffer(
                renderer,
                renderer->submittedCommandBuffers[i]);
        }
    }

    D3D12_INTERNAL_PerformPendingDestroys(renderer);

    SDL_UnlockMutex(renderer->submitLock);
}

static SDL_GpuFence *D3D12_SubmitAndAcquireFence(
    SDL_GpuCommandBuffer *commandBuffer)
{
    SDL_assert(SDL_FALSE);
    return NULL;
}

static void D3D12_Wait(
    SDL_GpuRenderer *driverData)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12Fence *fence = D3D12_INTERNAL_AcquireFence(renderer);
    HRESULT res;

    SDL_LockMutex(renderer->submitLock);

    if (renderer->commandQueue) {
        /* Insert a signal into the end of the command queue... */
        ID3D12CommandQueue_Signal(
            renderer->commandQueue,
            fence->handle,
            D3D12_FENCE_SIGNAL_VALUE);

        /* ...and then block on it. */
        if (ID3D12Fence_GetCompletedValue(fence->handle) != D3D12_FENCE_SIGNAL_VALUE) {
            res = ID3D12Fence_SetEventOnCompletion(
                fence->handle,
                D3D12_FENCE_SIGNAL_VALUE,
                fence->event);
            ERROR_CHECK_RETURN("Setting fence event failed", )

            WaitForSingleObject(fence->event, INFINITE);
        }
    }

    D3D12_ReleaseFence(
        (SDL_GpuRenderer *)renderer,
        (SDL_GpuFence *)fence);

    /* Clean up */
    for (Sint32 i = renderer->submittedCommandBufferCount - 1; i >= 0; i -= 1) {
        D3D12_INTERNAL_CleanCommandBuffer(renderer, renderer->submittedCommandBuffers[i]);
    }

    D3D12_INTERNAL_PerformPendingDestroys(renderer);

    SDL_UnlockMutex(renderer->submitLock);
}

static void D3D12_WaitForFences(
    SDL_GpuRenderer *driverData,
    SDL_bool waitAll,
    SDL_GpuFence **pFences,
    Uint32 fenceCount)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12Fence *fence;
    HANDLE *events = SDL_stack_alloc(HANDLE, fenceCount);
    HRESULT res;

    for (Uint32 i = 0; i < fenceCount; i += 1) {
        fence = (D3D12Fence *)pFences[i];

        res = ID3D12Fence_SetEventOnCompletion(
            fence->handle,
            D3D12_FENCE_SIGNAL_VALUE,
            fence->event);
        ERROR_CHECK_RETURN("Setting fence event failed", )

        events[i] = fence->event;
    }

    WaitForMultipleObjects(
        fenceCount,
        events,
        waitAll,
        INFINITE);

    SDL_stack_free(events);
}

/* Feature Queries */

static SDL_bool D3D12_IsTextureFormatSupported(
    SDL_GpuRenderer *driverData,
    SDL_GpuTextureFormat format,
    SDL_GpuTextureType type,
    SDL_GpuTextureUsageFlags usage)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    DXGI_FORMAT dxgiFormat = SDLToD3D12_TextureFormat[format];
    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { dxgiFormat, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
    HRESULT res;

    res = ID3D12Device_CheckFeatureSupport(
        renderer->device,
        D3D12_FEATURE_FORMAT_SUPPORT,
        &formatSupport,
        sizeof(formatSupport));
    if (FAILED(res)) {
        /* Format is apparently unknown */
        return SDL_FALSE;
    }

    /* Is the texture type supported? */
    if (type == SDL_GPU_TEXTURETYPE_2D && !(formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D)) {
        return SDL_FALSE;
    }
    if (type == SDL_GPU_TEXTURETYPE_3D && !(formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE3D)) {
        return SDL_FALSE;
    }
    if (type == SDL_GPU_TEXTURETYPE_CUBE && !(formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURECUBE)) {
        return SDL_FALSE;
    }

    /* Are the usage flags supported? */
    if ((usage & SDL_GPU_TEXTUREUSAGE_SAMPLER_BIT) && !(formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) {
        return SDL_FALSE;
    }
    if ((usage & (SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ_BIT | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ_BIT | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE_BIT)) && !(formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) {
        return SDL_FALSE;
    }
    if ((usage & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET_BIT) && !(formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) {
        return SDL_FALSE;
    }
    if ((usage & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET_BIT) && !(formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) {
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

static SDL_GpuSampleCount D3D12_GetBestSampleCount(
    SDL_GpuRenderer *driverData,
    SDL_GpuTextureFormat format,
    SDL_GpuSampleCount desiredSampleCount)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    SDL_GpuSampleCount maxSupported = SDL_GPU_SAMPLECOUNT_8;
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS featureData = { 0 };
    HRESULT res;

    while (maxSupported >= SDL_GPU_SAMPLECOUNT_1) {
        featureData.Format = SDLToD3D12_TextureFormat[format];
        featureData.SampleCount = (UINT)maxSupported;
        res = ID3D12Device_CheckFeatureSupport(
            renderer->device,
            D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
            &featureData,
            sizeof(featureData));

        if (SUCCEEDED(res) && featureData.NumQualityLevels > 0) {
            break;
        }
        maxSupported = (SDL_GpuSampleCount)((int)maxSupported - 1);
    }

    return (SDL_GpuSampleCount)SDL_min(maxSupported, desiredSampleCount);
}

static SDL_bool D3D12_PrepareDriver(SDL_VideoDevice *_this)
{
    void *d3d12_dll, *dxgi_dll, *d3dcompiler_dll;
    PFN_D3D12_CREATE_DEVICE D3D12CreateDeviceFunc;
    PFN_CREATE_DXGI_FACTORY1 CreateDXGIFactoryFunc;
    PFN_D3DCOMPILE D3DCompileFunc;
    HRESULT res;
    ID3D12Device *device;

    IDXGIFactory1 *factory;
    IDXGIFactory4 *factory4;
    IDXGIFactory6 *factory6;
    IDXGIAdapter1 *adapter;

    /* Can we load D3D12? */

    d3d12_dll = SDL_LoadObject(D3D12_DLL);
    if (d3d12_dll == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find " D3D12_DLL);
        return SDL_FALSE;
    }

    D3D12CreateDeviceFunc = (PFN_D3D12_CREATE_DEVICE)SDL_LoadFunction(
        d3d12_dll,
        D3D12_CREATE_DEVICE_FUNC);
    if (D3D12CreateDeviceFunc == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find function " D3D12_CREATE_DEVICE_FUNC " in " D3D12_DLL);
        SDL_UnloadObject(d3d12_dll);
        return SDL_FALSE;
    }

    /* Can we load DXGI? */

    dxgi_dll = SDL_LoadObject(DXGI_DLL);
    if (dxgi_dll == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find " DXGI_DLL);
        return SDL_FALSE;
    }

    CreateDXGIFactoryFunc = (PFN_CREATE_DXGI_FACTORY1)SDL_LoadFunction(
        dxgi_dll,
        CREATE_DXGI_FACTORY1_FUNC);
    if (CreateDXGIFactoryFunc == NULL) {
        SDL_UnloadObject(dxgi_dll);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find function " CREATE_DXGI_FACTORY1_FUNC " in " DXGI_DLL);
        return SDL_FALSE;
    }

    /* Can we create a device? */

    /* Create the DXGI factory */
    res = CreateDXGIFactoryFunc(
        &D3D_IID_IDXGIFactory1,
        (void **)&factory);
    if (FAILED(res)) {
        SDL_UnloadObject(d3d12_dll);
        SDL_UnloadObject(dxgi_dll);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not create DXGIFactory");
        return SDL_FALSE;
    }

    /* Check for DXGI 1.4 support */
    res = IDXGIFactory1_QueryInterface(
        factory,
        &D3D_IID_IDXGIFactory4,
        (void **)&factory4);
    if (FAILED(res)) {
        IDXGIFactory1_Release(factory);
        SDL_UnloadObject(d3d12_dll);
        SDL_UnloadObject(dxgi_dll);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Failed to find DXGI1.4 support, required for DX12");
        return SDL_FALSE;
    }
    IDXGIFactory4_Release(factory4);

    res = IDXGIFactory1_QueryInterface(
        factory,
        &D3D_IID_IDXGIFactory6,
        (void **)&factory6);
    if (SUCCEEDED(res)) {
        res = IDXGIFactory6_EnumAdapterByGpuPreference(
            factory6,
            0,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            &D3D_IID_IDXGIAdapter1,
            (void **)&adapter);
        IDXGIFactory6_Release(factory6);
    } else {
        res = IDXGIFactory1_EnumAdapters1(
            factory,
            0,
            &adapter);
    }
    if (FAILED(res)) {
        IDXGIFactory1_Release(factory);
        SDL_UnloadObject(d3d12_dll);
        SDL_UnloadObject(dxgi_dll);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Failed to find adapter for D3D12Device");
        return SDL_FALSE;
    }

    res = D3D12CreateDeviceFunc(
        (IUnknown *)adapter,
        D3D_FEATURE_LEVEL_CHOICE,
        &D3D_IID_ID3D12Device,
        (void **)&device);

    if (SUCCEEDED(res)) {
        ID3D12Device_Release(device);
    }
    IDXGIAdapter1_Release(adapter);
    IDXGIFactory1_Release(factory);

    SDL_UnloadObject(d3d12_dll);
    SDL_UnloadObject(dxgi_dll);

    if (FAILED(res)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not create D3D12Device with feature level " D3D_FEATURE_LEVEL_CHOICE_STR);
        return SDL_FALSE;
    }

    /* Can we load D3DCompiler? */

    d3dcompiler_dll = SDL_LoadObject(D3DCOMPILER_DLL);
    if (d3dcompiler_dll == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find " D3DCOMPILER_DLL);
        return SDL_FALSE;
    }

    D3DCompileFunc = (PFN_D3DCOMPILE)SDL_LoadFunction(
        d3dcompiler_dll,
        D3DCOMPILE_FUNC);
    SDL_UnloadObject(d3dcompiler_dll); /* We're not going to call this function, so we can just unload now. */
    if (D3DCompileFunc == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find function D3DCompile in " D3DCOMPILER_DLL);
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

static void D3D12_INTERNAL_TryInitializeDXGIDebug(D3D12Renderer *renderer)
{
    PFN_DXGI_GET_DEBUG_INTERFACE DXGIGetDebugInterfaceFunc;
    HRESULT res;

    renderer->dxgidebug_dll = SDL_LoadObject(DXGIDEBUG_DLL);
    if (renderer->dxgidebug_dll == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not find " DXGIDEBUG_DLL);
        return;
    }

    DXGIGetDebugInterfaceFunc = (PFN_DXGI_GET_DEBUG_INTERFACE)SDL_LoadFunction(
        renderer->dxgidebug_dll,
        DXGI_GET_DEBUG_INTERFACE_FUNC);
    if (DXGIGetDebugInterfaceFunc == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not load function: " DXGI_GET_DEBUG_INTERFACE_FUNC);
        return;
    }

    res = DXGIGetDebugInterfaceFunc(&D3D_IID_IDXGIDebug, (void **)&renderer->dxgiDebug);
    if (FAILED(res)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not get IDXGIDebug interface");
    }

    res = DXGIGetDebugInterfaceFunc(&D3D_IID_IDXGIInfoQueue, (void **)&renderer->dxgiInfoQueue);
    if (FAILED(res)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not get IDXGIInfoQueue interface");
    }
}

static void D3D12_INTERNAL_TryInitializeD3D12Debug(D3D12Renderer *renderer)
{
    PFN_D3D12_GET_DEBUG_INTERFACE D3D12GetDebugInterfaceFunc;
    HRESULT res;

    D3D12GetDebugInterfaceFunc = (PFN_D3D12_GET_DEBUG_INTERFACE)SDL_LoadFunction(
        renderer->d3d12_dll,
        D3D12_GET_DEBUG_INTERFACE_FUNC);
    if (D3D12GetDebugInterfaceFunc == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "Could not load function: " D3D12_GET_DEBUG_INTERFACE_FUNC);
        return;
    }

    res = D3D12GetDebugInterfaceFunc(&D3D_IID_ID3D12Debug, (void **)&renderer->d3d12Debug);
    if (FAILED(res)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not get ID3D12Debug interface");
        return;
    }

    ID3D12Debug_EnableDebugLayer(renderer->d3d12Debug);
}

static void D3D12_INTERNAL_TryInitializeD3D12DebugInfoQueue(D3D12Renderer *renderer)
{
    ID3D12InfoQueue *infoQueue = NULL;
    D3D12_MESSAGE_SEVERITY severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
    D3D12_INFO_QUEUE_FILTER filter;
    HRESULT res;

    res = ID3D12Device_QueryInterface(
        renderer->device,
        &D3D_IID_ID3D12InfoQueue,
        (void **)infoQueue);
    if (FAILED(res)) {
        ERROR_CHECK_RETURN("Failed to convert ID3D12Device to ID3D12InfoQueue", );
    }

    SDL_zero(filter);
    filter.DenyList.NumSeverities = 1;
    filter.DenyList.pSeverityList = severities;
    ID3D12InfoQueue_PushStorageFilter(
        infoQueue,
        &filter);

    ID3D12InfoQueue_SetBreakOnSeverity(
        infoQueue,
        D3D12_MESSAGE_SEVERITY_ERROR,
        SDL_TRUE);

    ID3D12InfoQueue_SetBreakOnSeverity(
        infoQueue,
        D3D12_MESSAGE_SEVERITY_CORRUPTION,
        SDL_TRUE);

    ID3D12InfoQueue_Release(infoQueue);
}

static SDL_GpuDevice *D3D12_CreateDevice(SDL_bool debugMode, SDL_bool preferLowPower)
{
    SDL_GpuDevice *result;
    D3D12Renderer *renderer;
    PFN_CREATE_DXGI_FACTORY1 CreateDXGIFactoryFunc;
    HRESULT res;
    IDXGIFactory1 *factory1;
    IDXGIFactory5 *factory5;
    IDXGIFactory6 *factory6;
    DXGI_ADAPTER_DESC1 adapterDesc;
    D3D12_FEATURE_DATA_ARCHITECTURE architecture;
    PFN_D3D12_CREATE_DEVICE D3D12CreateDeviceFunc;
    D3D12_COMMAND_QUEUE_DESC queueDesc;

    renderer = (D3D12Renderer *)SDL_malloc(sizeof(D3D12Renderer));
    SDL_zerop(renderer);

    /* Load the D3DCompiler library */
    renderer->d3dcompiler_dll = SDL_LoadObject(D3DCOMPILER_DLL);
    if (renderer->d3dcompiler_dll == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not find " D3DCOMPILER_DLL);
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        return NULL;
    }

    renderer->D3DCompile_func = (PFN_D3DCOMPILE)SDL_LoadFunction(renderer->d3dcompiler_dll, D3DCOMPILE_FUNC);
    if (renderer->D3DCompile_func == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load function: " D3DCOMPILE_FUNC);
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        return NULL;
    }

    /* Load the DXGI library */
    renderer->dxgi_dll = SDL_LoadObject(DXGI_DLL);
    if (renderer->dxgi_dll == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not find " DXGI_DLL);
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        return NULL;
    }

    /* Initialize the DXGI debug layer, if applicable */
    if (debugMode) {
        D3D12_INTERNAL_TryInitializeDXGIDebug(renderer);
    }

    /* Load the CreateDXGIFactory1 function */
    CreateDXGIFactoryFunc = (PFN_CREATE_DXGI_FACTORY1)SDL_LoadFunction(
        renderer->dxgi_dll,
        CREATE_DXGI_FACTORY1_FUNC);
    if (CreateDXGIFactoryFunc == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load function: " CREATE_DXGI_FACTORY1_FUNC);
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        return NULL;
    }

    /* Create the DXGI factory */
    res = CreateDXGIFactoryFunc(
        &D3D_IID_IDXGIFactory1,
        (void **)&factory1);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not create DXGIFactory", NULL);
    }

    /* Check for DXGI 1.4 support */
    res = IDXGIFactory1_QueryInterface(
        factory1,
        &D3D_IID_IDXGIFactory4,
        (void **)&renderer->factory);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("DXGI1.4 support not found, required for DX12", NULL);
    }
    IDXGIFactory1_Release(factory1);

    /* Check for explicit tearing support */
    res = IDXGIFactory4_QueryInterface(
        renderer->factory,
        &D3D_IID_IDXGIFactory5,
        (void **)&factory5);
    if (SUCCEEDED(res)) {
        res = IDXGIFactory5_CheckFeatureSupport(
            factory5,
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &renderer->supportsTearing,
            sizeof(renderer->supportsTearing));
        if (FAILED(res)) {
            renderer->supportsTearing = SDL_FALSE;
        }
        IDXGIFactory5_Release(factory5);
    }

    /* Select the appropriate device for rendering */
    res = IDXGIFactory4_QueryInterface(
        renderer->factory,
        &D3D_IID_IDXGIFactory6,
        (void **)&factory6);
    if (SUCCEEDED(res)) {
        res = IDXGIFactory6_EnumAdapterByGpuPreference(
            factory6,
            0,
            preferLowPower ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            &D3D_IID_IDXGIAdapter1,
            (void **)&renderer->adapter);
        IDXGIFactory6_Release(factory6);
    } else {
        res = IDXGIFactory4_EnumAdapters1(
            renderer->factory,
            0,
            &renderer->adapter);
    }

    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not find adapter for D3D12Device", NULL);
    }

    /* Get information about the selected adapter. Used for logging info. */
    res = IDXGIAdapter1_GetDesc1(renderer->adapter, &adapterDesc);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not get adapter description", NULL);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "SDL_Gpu Driver: D3D12");
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "D3D12 Adapter: %S", adapterDesc.Description);

    /* Load the D3D library */
    renderer->d3d12_dll = SDL_LoadObject(D3D12_DLL);
    if (renderer->d3d12_dll == NULL) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not find " D3D12_DLL);
        return NULL;
    }

    /* Load the CreateDevice function */
    D3D12CreateDeviceFunc = (PFN_D3D12_CREATE_DEVICE)SDL_LoadFunction(
        renderer->d3d12_dll,
        D3D12_CREATE_DEVICE_FUNC);
    if (D3D12CreateDeviceFunc == NULL) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load function: " D3D12_CREATE_DEVICE_FUNC);
        return NULL;
    }

    renderer->D3D12SerializeRootSignature_func = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)SDL_LoadFunction(
        renderer->d3d12_dll,
        D3D12_SERIALIZE_ROOT_SIGNATURE_FUNC);
    if (renderer->D3D12SerializeRootSignature_func == NULL) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load function: " D3D12_SERIALIZE_ROOT_SIGNATURE_FUNC);
        return NULL;
    }

    /* Initialize the D3D12 debug layer, if applicable */
    if (debugMode) {
        D3D12_INTERNAL_TryInitializeD3D12Debug(renderer);
    }

    /* Create the D3D12Device */
    res = D3D12CreateDeviceFunc(
        (IUnknown *)renderer->adapter,
        D3D_FEATURE_LEVEL_CHOICE,
        &D3D_IID_ID3D12Device,
        (void **)&renderer->device);

    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not create D3D12Device", NULL);
    }

    /* Initialize the D3D12 debug info queue, if applicable */
    if (debugMode) {
        D3D12_INTERNAL_TryInitializeD3D12DebugInfoQueue(renderer);
    }

    /* Check UMA */
    /* Call seems to currently go to the wrong vtbl entry
    res = ID3D12Device_CheckFeatureSupport(
        renderer->device,
        D3D12_FEATURE_ARCHITECTURE,
        &architecture,
        sizeof(D3D12_FEATURE_DATA_ARCHITECTURE));
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not get device architecture", NULL);
    }

    renderer->UMA = (SDL_bool)architecture.UMA;
    */
    renderer->UMA = SDL_FALSE;

    SDL_zero(queueDesc);
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    res = ID3D12Device_CreateCommandQueue(
        renderer->device,
        &queueDesc,
        &D3D_IID_ID3D12CommandQueue,
        (void **)&renderer->commandQueue);

    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not create D3D12CommandQueue", NULL);
    }

    /* Initialize pools */

    renderer->submittedCommandBufferCapacity = 4;
    renderer->submittedCommandBufferCount = 0;
    renderer->submittedCommandBuffers = SDL_malloc(
        renderer->submittedCommandBufferCapacity * sizeof(D3D12CommandBuffer *));

    renderer->uniformBufferPoolCapacity = 4;
    renderer->uniformBufferPoolCount = 0;
    renderer->uniformBufferPool = SDL_malloc(
        renderer->uniformBufferPoolCapacity * sizeof(D3D12UniformBuffer *));

    renderer->claimedWindowCapacity = 4;
    renderer->claimedWindowCount = 0;
    renderer->claimedWindows = SDL_malloc(
        renderer->claimedWindowCapacity * sizeof(D3D12WindowData *));

    renderer->availableFenceCapacity = 4;
    renderer->availableFenceCount = 0;
    renderer->availableFences = SDL_malloc(
        renderer->availableFenceCapacity * sizeof(D3D12Fence *));

    /* Initialize CPU descriptor heaps */
    for (Uint32 i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; i += 1) {
        renderer->stagingDescriptorHeaps[i] = D3D12_INTERNAL_CreateDescriptorHeap(
            renderer,
            i,
            (i <= D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) ? VIEW_SAMPLER_STAGING_DESCRIPTOR_COUNT : TARGET_STAGING_DESCRIPTOR_COUNT,
            SDL_TRUE);
    }

    /* Initialize GPU descriptor heaps */
    for (Uint32 i = 0; i < 2; i += 1) {
        renderer->descriptorHeapPools[i].lock = SDL_CreateMutex();
        renderer->descriptorHeapPools[i].capacity = 4;
        renderer->descriptorHeapPools[i].count = 4;
        renderer->descriptorHeapPools[i].heaps = SDL_malloc(
            renderer->descriptorHeapPools[i].capacity * sizeof(D3D12DescriptorHeap *));

        for (Uint32 j = 0; j < renderer->descriptorHeapPools[i].capacity; j += 1) {
            renderer->descriptorHeapPools[i].heaps[j] = D3D12_INTERNAL_CreateDescriptorHeap(
                renderer,
                i,
                i == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? VIEW_GPU_DESCRIPTOR_COUNT : SAMPLER_GPU_DESCRIPTOR_COUNT,
                SDL_FALSE);
        }
    }

    /* Locks */
    renderer->stagingDescriptorHeapLock = SDL_CreateMutex();
    renderer->acquireCommandBufferLock = SDL_CreateMutex();
    renderer->acquireUniformBufferLock = SDL_CreateMutex();
    renderer->submitLock = SDL_CreateMutex();
    renderer->windowLock = SDL_CreateMutex();
    renderer->fenceLock = SDL_CreateMutex();

    renderer->debugMode = debugMode;

    /* Create the SDL_Gpu Device */
    result = (SDL_GpuDevice *)SDL_malloc(sizeof(SDL_GpuDevice));
    SDL_zerop(result);
    ASSIGN_DRIVER(D3D12)
    result->driverData = (SDL_GpuRenderer *)renderer;
    result->debugMode = debugMode;

    return result;
}

SDL_GpuDriver D3D12Driver = {
    "D3D12",
    SDL_GPU_BACKEND_D3D12,
    D3D12_PrepareDriver,
    D3D12_CreateDevice
};

#endif /* SDL_GPU_D12 */
