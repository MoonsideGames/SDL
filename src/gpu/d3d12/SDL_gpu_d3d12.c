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

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>

#include "../SDL_sysgpu.h"

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

/* Defines */
#if defined(_WIN32)
#define D3D12_DLL     "d3d12.dll"
#define DXGI_DLL      "dxgi.dll"
#define DXGIDEBUG_DLL "dxgidebug.dll"
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
#define WINDOW_PROPERTY_DATA                "SDL_GpuD3D12WindowPropertyData"
#define D3D_FEATURE_LEVEL_CHOICE            D3D_FEATURE_LEVEL_11_1
#define D3D_FEATURE_LEVEL_CHOICE_STR        "11_1"
#define SWAPCHAIN_BUFFER_COUNT              2
#define MAX_ROOT_SIGNATURE_PARAMETERS       64
#define MAX_VERTEX_UNIFORM_BUFFERS          14
#define MAX_FRAGMENT_UNIFORM_BUFFERS        14
#define MAX_UNIFORM_BUFFER_POOL_SIZE        16
#define MAX_VERTEX_SAMPLERS                 16
#define MAX_FRAGMENT_SAMPLERS               16
#define MAX_VERTEX_RESOURCE_COUNT           (128 + 14 + 8)
#define MAX_FRAGMENT_RESOURCE_COUNT         (128 + 14 + 8)

#ifdef _WIN32
#define HRESULT_FMT "(0x%08lX)"
#else
#define HRESULT_FMT "(0x%08X)"
#endif

/* Function Pointer Signatures */
typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY1)(const GUID *riid, void **ppFactory);
typedef HRESULT(WINAPI *PFN_DXGI_GET_DEBUG_INTERFACE)(const GUID *riid, void **ppDebug);
typedef HRESULT(D3DCOMPILER_API *PFN_D3DCOMPILE)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2, ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

/* IIDs (from https://www.magnumdb.com/) */
static const IID D3D_IID_IDXGIFactory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const IID D3D_IID_IDXGIFactory4 = { 0x1bc6ea02, 0xef36, 0x464f, { 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a } };
static const IID D3D_IID_IDXGIFactory5 = { 0x7632e1f5, 0xee65, 0x4dca, { 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d } };
static const IID D3D_IID_IDXGIFactory6 = { 0xc1b6694f, 0xff09, 0x44a9, { 0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17 } };
static const IID D3D_IID_IDXGIAdapter1 = { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };
static const IID D3D_IID_IDXGISwapChain3 = { 0x94d99bdb, 0xf1f8, 0x4ab0, { 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1 } };
// static const IID D3D_IID_ID3DUserDefinedAnnotation = { 0xb2daad8b, 0x03d4, 0x4dbf, { 0x95, 0xeb, 0x32, 0xab, 0x4b, 0x63, 0xd0, 0xab } };
static const IID D3D_IID_IDXGIDebug = { 0x119e7452, 0xde9e, 0x40fe, { 0x88, 0x06, 0x88, 0xf9, 0x0c, 0x12, 0xb4, 0x41 } };

// static const IID D3D_IID_IDXGIInfoQueue = { 0xd67441c7, 0x672a, 0x476f, { 0x9e, 0x82, 0xcd, 0x55, 0xb4, 0x49, 0x49, 0xce } };

// static const GUID D3D_IID_D3DDebugObjectName = { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };
// static const GUID D3D_IID_DXGI_DEBUG_ALL = { 0xe48ae283, 0xda80, 0x490b, { 0x87, 0xe6, 0x43, 0xe9, 0xa9, 0xcf, 0xda, 0x08 } };

static const IID D3D_IID_ID3D12Device = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const IID D3D_IID_ID3D12CommandQueue = { 0x0ec870a6, 0x5d7e, 0x4c22, { 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed } };
static const IID D3D_IID_ID3D12DescriptorHeap = { 0x8efb471d, 0x616c, 0x4f49, { 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51 } };
static const IID D3D_IID_ID3D12Resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
static const IID D3D_IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const IID D3D_IID_ID3D12CommandAllocator = { 0x6102dee4, 0xaf59, 0x4b09, { 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24 } };
static const IID SDL_IID_ID3D12GraphicsCommandList2 = { 0x38C3E585, 0xFF17, 0x412C, { 0x91, 0x50, 0x4F, 0xC6, 0xF9, 0xD7, 0x2A, 0x28 } };
static const IID SDL_IID_ID3D12Fence = { 0x0a753dcf, 0xc4d8, 0x4b91, { 0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76 } };
static const IID SDL_IID_ID3D12RootSignature = { 0xc54a6b66, 0x72df, 0x4ee8, { 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14 } };
static const IID SDL_IID_ID3D12PipelineState = { 0x765a30f3, 0xf624, 0x4c6f, { 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 } };
static const IID SDL_IID_ID3D12DescriptorHeap = { 0x8efb471d, 0x616c, 0x4f49, { 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51 } };

static const char *D3D12ShaderProfiles[3] = { "vs_5_1", "ps_5_1", "cs_5_1" };

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

static D3D12_FILL_MODE SDLToD3D12_InputRate[] = {
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

/* Structures */
typedef struct D3D12Renderer D3D12Renderer;
typedef struct D3D12CommandBuffer D3D12CommandBuffer;
typedef struct D3D12WindowData D3D12WindowData;
typedef struct D3D12Texture D3D12Texture;
typedef struct D3D12Shader D3D12Shader;
typedef struct D3D12GraphicsPipeline D3D12GraphicsPipeline;
typedef struct D3D12UniformBuffer D3D12UniformBuffer;

struct D3D12WindowData
{
    SDL_Window *window;
    IDXGISwapChain3 *swapchain;
    // D3D12TextureContainer textureContainer;
    SDL_GpuPresentMode presentMode;
    SDL_GpuSwapchainComposition swapchainComposition;
    DXGI_FORMAT swapchainFormat;
    DXGI_COLOR_SPACE_TYPE swapchainColorSpace;
    // D3D12Fence *inFlightFences[MAX_FRAMES_IN_FLIGHT];

    ID3D12DescriptorHeap *rtvHeap;
    ID3D12Resource *renderTargets[SWAPCHAIN_BUFFER_COUNT];
    D3D12Texture *renderTexture[SWAPCHAIN_BUFFER_COUNT];
    Uint32 frameCounter;

    // not owned, chain of active windows, see D3D12CommandBuffer::nextWindow
    D3D12WindowData *nextWindow;
    SDL_bool activeWindow;
};

struct D3D12Texture
{
    // ownership?!?! (currently D3D12WindowData->renderTargets)
    ID3D12Resource *resource;
    D3D12_RESOURCE_DESC desc;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    SDL_bool isRenderTarget;
};

struct D3D12Renderer
{
    void *dxgidebug_dll;
    IDXGIDebug *dxgiDebug;
    void *d3dcompiler_dll;
    PFN_D3DCOMPILE D3DCompile_func;
    void *dxgi_dll;
    IDXGIFactory4 *factory;
    BOOL supportsTearing;
    IDXGIAdapter1 *adapter;
    void *d3d12_dll;
    ID3D12Device *device;
    D3D12CommandBuffer *commandBuffer;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE D3D12SerializeRootSignature_func;

    Uint32 uniformBufferPoolCount;
    D3D12UniformBuffer *uniformBufferPool[MAX_UNIFORM_BUFFER_POOL_SIZE];
};

struct D3D12CommandBuffer
{
    // reserved for SDL_gpu
    CommandBufferCommonHeader common;

    // non owning parent reference
    D3D12Renderer *renderer;

    ID3D12CommandQueue *commandQueue;
    ID3D12CommandAllocator *commandAllocator;
    ID3D12GraphicsCommandList2 *graphicsCommandList;
    ID3D12Fence *fence;

    SDL_Mutex *fenceLock;
    UINT64 fenceValue;
    HANDLE fenceEvent;

    // not owned, head of chain of active windows
    D3D12WindowData *nextWindow;

    Uint32 colorAttachmentCount;
    D3D12Texture *colorAttachmentTexture[MAX_COLOR_TARGET_BINDINGS];
    D3D12GraphicsPipeline *currentGraphicsPipeline;

    ID3D12DescriptorHeap *descriptorHeap;
    D3D12_GPU_DESCRIPTOR_HANDLE descriptorHeapHandle;

    // cleanup
    D3D12UniformBuffer *vertexUniformBuffers[MAX_VERTEX_UNIFORM_BUFFERS];
    // cleanup
    D3D12UniformBuffer *fragmentUniformBuffers[MAX_FRAGMENT_UNIFORM_BUFFERS];

    SDL_bool needVertexUniformBufferBind;
    SDL_bool needFragmentUniformBufferBind;

    Uint32 usedUniformBufferCount;
    Uint32 usedUniformBufferCapacity;
    // not owned
    D3D12UniformBuffer **usedUniformBuffers;

    SDL_bool needVertexSamplerBind;
    SDL_bool needVertexResourceBind;
    SDL_bool needFragmentSamplerBind;
    SDL_bool needFragmentResourceBind;

    // cleanup
    ID3D12DescriptorHeap *vertexSamplerDescriptorHeap;
    D3D12_GPU_DESCRIPTOR_HANDLE vertexSamplerDescriptorHeapHandle;
    // cleanup
    ID3D12DescriptorHeap *fragmentSamplerDescriptorHeap;
    D3D12_GPU_DESCRIPTOR_HANDLE fragmentSamplerDescriptorHeapHandle;
    // cleanup
    ID3D12DescriptorHeap *vertexShaderResourceDescriptorHeap;
    D3D12_GPU_DESCRIPTOR_HANDLE vertexShaderResourceDescriptorHeapHandle;
    // cleanup
    ID3D12DescriptorHeap *fragmentShaderResourceDescriptorHeap;
    D3D12_GPU_DESCRIPTOR_HANDLE fragmentShaderResourceDescriptorHeapHandle;
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

struct D3D12UniformBuffer
{
    ID3D12Resource *buffer;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHandle;
    D3D12_GPU_VIRTUAL_ADDRESS bufferLocation;
    Uint32 writeOffset;
    Uint32 drawOffset;
    Uint32 currentBlockSize;
};

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
    dwChars = FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        res,
        0,
        wszMsgBuff,
        MAX_ERROR_LEN,
        NULL);

    /* No message? Screw it, just post the code. */
    if (dwChars == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s! Error Code: " HRESULT_FMT, msg, res);
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

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s! Error Code: %s " HRESULT_FMT, msg, wszMsgBuff, res);
}

static void D3D12_INTERNAL_DestroyCommandBuffer(D3D12CommandBuffer *commandBuffer)
{
    if (commandBuffer->descriptorHeap) {
        ID3D12DescriptorHeap_Release(commandBuffer->descriptorHeap);
        commandBuffer->descriptorHeap = NULL;
    }
    if (commandBuffer->fenceEvent) {
        CloseHandle(commandBuffer->fenceEvent);
        commandBuffer->fenceEvent = NULL;
    }
    if (commandBuffer->fenceLock) {
        SDL_DestroyMutex(commandBuffer->fenceLock);
        commandBuffer->fenceLock = NULL;
    }
    if (commandBuffer->graphicsCommandList) {
        ID3D12GraphicsCommandList2_Release(commandBuffer->graphicsCommandList);
        commandBuffer->graphicsCommandList = NULL;
    }
    if (commandBuffer->commandAllocator) {
        ID3D12CommandAllocator_Release(commandBuffer->commandAllocator);
        commandBuffer->commandAllocator = NULL;
    }
    if (commandBuffer->commandQueue) {
        ID3D12CommandQueue_Release(commandBuffer->commandQueue);
        commandBuffer->commandQueue = NULL;
    }
}

static void D3D12_INTERNAL_DestroyRenderer(D3D12Renderer *renderer)
{
    if (renderer->commandBuffer) {
        D3D12_INTERNAL_DestroyCommandBuffer(renderer->commandBuffer);
        SDL_free(renderer->commandBuffer);
        renderer->commandBuffer = NULL;
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

void D3D12_DestroyDevice(SDL_GpuDevice *device)
{
    D3D12Renderer *renderer = (D3D12Renderer *)device->driverData;
    if (renderer) {
        D3D12_INTERNAL_DestroyRenderer(renderer);
        SDL_free(renderer);
    }
    SDL_free(device);
}

/* State Creation */

ID3D12RootSignature *D3D12_INTERNAL_CreateRootSignature(D3D12Renderer *renderer, ID3D12Device *device, Uint32 samplerCount, Uint32 uniformBufferCount, Uint32 storageBufferCount, Uint32 storageTextureCount)
{
    D3D12_ROOT_PARAMETER rootParameters[MAX_ROOT_SIGNATURE_PARAMETERS];
    D3D12_DESCRIPTOR_RANGE descriptorRanges[MAX_ROOT_SIGNATURE_PARAMETERS];
    UINT parameterCount = 0;

    // Define descriptor ranges for uniform buffers
    if (uniformBufferCount > 0) {
        if (parameterCount >= MAX_ROOT_SIGNATURE_PARAMETERS) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Too many root signature arguments.");
            return NULL;
        }

        D3D12_DESCRIPTOR_RANGE descriptorRange = {};
        descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        descriptorRange.NumDescriptors = uniformBufferCount;
        descriptorRange.BaseShaderRegister = 0;
        descriptorRange.RegisterSpace = 0;
        descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        descriptorRanges[parameterCount] = descriptorRange;

        D3D12_ROOT_PARAMETER rootParameter = {};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameter.DescriptorTable.NumDescriptorRanges = 1;
        rootParameter.DescriptorTable.pDescriptorRanges = &descriptorRanges[parameterCount];
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[parameterCount] = rootParameter;
        parameterCount++;
    }

    // Define descriptor ranges for storage buffers
    if (storageBufferCount > 0) {
        if (parameterCount >= MAX_ROOT_SIGNATURE_PARAMETERS) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Too many root signature arguments.");
            return NULL;
        }
        D3D12_DESCRIPTOR_RANGE descriptorRange = {};
        descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRange.NumDescriptors = storageBufferCount;
        descriptorRange.BaseShaderRegister = 0;
        descriptorRange.RegisterSpace = 0;
        descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        descriptorRanges[parameterCount] = descriptorRange;

        D3D12_ROOT_PARAMETER rootParameter = {};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameter.DescriptorTable.NumDescriptorRanges = 1;
        rootParameter.DescriptorTable.pDescriptorRanges = &descriptorRanges[parameterCount];
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[parameterCount] = rootParameter;
        parameterCount++;
    }

    // Define descriptor ranges for storage textures
    if (storageTextureCount > 0) {
        if (parameterCount >= MAX_ROOT_SIGNATURE_PARAMETERS) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Too many root signature arguments.");
            return NULL;
        }
        D3D12_DESCRIPTOR_RANGE descriptorRange = {};
        descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRange.NumDescriptors = storageTextureCount;
        descriptorRange.BaseShaderRegister = 0;
        descriptorRange.RegisterSpace = 0;
        descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        descriptorRanges[parameterCount] = descriptorRange;

        D3D12_ROOT_PARAMETER rootParameter = {};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameter.DescriptorTable.NumDescriptorRanges = 1;
        rootParameter.DescriptorTable.pDescriptorRanges = &descriptorRanges[parameterCount];
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[parameterCount] = rootParameter;
        parameterCount++;
    }

    // Define descriptor ranges for samplers
    if (samplerCount > 0) {
        if (parameterCount >= MAX_ROOT_SIGNATURE_PARAMETERS) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Too many root signature arguments.");
            return NULL;
        }
        D3D12_DESCRIPTOR_RANGE descriptorRange = {};
        descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        descriptorRange.NumDescriptors = samplerCount;
        descriptorRange.BaseShaderRegister = 0;
        descriptorRange.RegisterSpace = 0;
        descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        descriptorRanges[parameterCount] = descriptorRange;

        D3D12_ROOT_PARAMETER rootParameter = {};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameter.DescriptorTable.NumDescriptorRanges = 1;
        rootParameter.DescriptorTable.pDescriptorRanges = &descriptorRanges[parameterCount];
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[parameterCount] = rootParameter;
        parameterCount++;
    }

    // Create the root signature description
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = parameterCount;
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

    res = ID3D12Device_CreateRootSignature(device,
                                           0,
                                           ID3D10Blob_GetBufferPointer(serializedRootSignature),
                                           ID3D10Blob_GetBufferSize(serializedRootSignature),
                                           &SDL_IID_ID3D12RootSignature,
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

static BOOL D3D12_INTERNAL_CreateShaderBytecode(
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
            return FALSE;
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
        return FALSE;
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

    return TRUE;
}

SDL_GpuComputePipeline *D3D12_CreateComputePipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuComputePipelineCreateInfo *pipelineCreateInfo)
{
    SDL_assert(SDL_FALSE);
    return NULL;
}

SDL_bool D3D12_INTERNAL_ConvertRasterizerState(SDL_GpuRasterizerState rasterizerState, D3D12_RASTERIZER_DESC *desc)
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

SDL_bool D3D12_INTERNAL_ConvertBlendState(SDL_GpuGraphicsPipelineCreateInfo *pipelineInfo, D3D12_BLEND_DESC *blendDesc)
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
            SDL_assert(sdlBlendState.colorWriteMask <= UINT8_MAX);
            rtBlendDesc.RenderTargetWriteMask = (UINT8)sdlBlendState.colorWriteMask;

            if (i > 0)
                blendDesc->IndependentBlendEnable = TRUE;
        }

        blendDesc->RenderTarget[i] = rtBlendDesc;
    }

    return SDL_TRUE;
}

SDL_bool D3D12_INTERNAL_ConvertDepthStencilState(SDL_GpuDepthStencilState depthStencilState, D3D12_DEPTH_STENCIL_DESC *desc)
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

SDL_bool D3D12_INTERNAL_ConvertVertexInputState(SDL_GpuVertexInputState vertexInputState, D3D12_INPUT_ELEMENT_DESC *desc)
{
    if (desc == NULL || vertexInputState.vertexAttributeCount == 0) {
        return FALSE;
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

    return TRUE;
}

SDL_GpuGraphicsPipeline *D3D12_CreateGraphicsPipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuGraphicsPipelineCreateInfo *pipelineCreateInfo)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12Shader *vertShader = (D3D12Shader *)pipelineCreateInfo->vertexShader;
    D3D12Shader *fragShader = (D3D12Shader *)pipelineCreateInfo->fragmentShader;

    Uint32 samplerCount = max(vertShader->samplerCount, fragShader->samplerCount);
    Uint32 uniformBufferCount = max(vertShader->uniformBufferCount, fragShader->uniformBufferCount);
    Uint32 storageBufferCount = max(vertShader->storageBufferCount, fragShader->storageBufferCount);
    Uint32 storageTextureCount = max(vertShader->storageTextureCount, fragShader->storageTextureCount);

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

    psoDesc.DSVFormat = pipelineCreateInfo->attachmentInfo.depthStencilFormat;
    psoDesc.NumRenderTargets = pipelineCreateInfo->attachmentInfo.colorAttachmentCount;
    for (uint32_t i = 0; i < pipelineCreateInfo->attachmentInfo.colorAttachmentCount; ++i) {
        psoDesc.RTVFormats[i] = SDLToD3D12_TextureFormat[pipelineCreateInfo->attachmentInfo.colorAttachmentDescriptions[i].format];
    }

    // Assuming some default values or further initialization
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    psoDesc.CachedPSO.CachedBlobSizeInBytes = 0;
    psoDesc.CachedPSO.pCachedBlob = NULL;

    psoDesc.NodeMask = 0;

    ID3D12RootSignature *rootSignature = D3D12_INTERNAL_CreateRootSignature(renderer, renderer->device, samplerCount, uniformBufferCount, storageBufferCount, storageTextureCount);
    if (!rootSignature) {
        return NULL;
    }
    psoDesc.pRootSignature = rootSignature;
    ID3D12PipelineState *pipelineState = NULL;

    HRESULT res = ID3D12Device_CreateGraphicsPipelineState(renderer->device, &psoDesc, &SDL_IID_ID3D12PipelineState, (void **)&pipelineState);
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

SDL_GpuSampler *D3D12_CreateSampler(
    SDL_GpuRenderer *driverData,
    SDL_GpuSamplerCreateInfo *samplerCreateInfo)
{
    SDL_assert(SDL_FALSE);
    return NULL;
}

SDL_GpuShader *D3D12_CreateShader(
    SDL_GpuRenderer *driverData,
    SDL_GpuShaderCreateInfo *shaderCreateInfo)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    void *bytecode;
    size_t bytecodeSize;
    D3D12Shader *shader;

    if ((shaderCreateInfo->stage != SDL_GPU_SHADERSTAGE_VERTEX) && (shaderCreateInfo->stage != SDL_GPU_SHADERSTAGE_FRAGMENT)) {
        SDL_assert(FALSE);
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

SDL_GpuTexture *D3D12_CreateTexture(
    SDL_GpuRenderer *driverData,
    SDL_GpuTextureCreateInfo *textureCreateInfo)
{
    SDL_assert(SDL_FALSE);
    return NULL;
}

SDL_GpuBuffer *D3D12_CreateBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuBufferUsageFlags usageFlags,
    Uint32 sizeInBytes)
{
    SDL_assert(SDL_FALSE);
    return NULL;
}

SDL_GpuTransferBuffer *D3D12_CreateTransferBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBufferUsage usage,
    Uint32 sizeInBytes)
{
    SDL_assert(SDL_FALSE);
    return NULL;
}

/* Debug Naming */

void D3D12_SetBufferName(
    SDL_GpuRenderer *driverData,
    SDL_GpuBuffer *buffer,
    const char *text) { SDL_assert(SDL_FALSE); }

void D3D12_SetTextureName(
    SDL_GpuRenderer *driverData,
    SDL_GpuTexture *texture,
    const char *text) { SDL_assert(SDL_FALSE); }

void D3D12_InsertDebugLabel(
    SDL_GpuCommandBuffer *commandBuffer,
    const char *text) { SDL_assert(SDL_FALSE); }

void D3D12_PushDebugGroup(
    SDL_GpuCommandBuffer *commandBuffer,
    const char *name) { SDL_assert(SDL_FALSE); }

void D3D12_PopDebugGroup(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

/* Disposal */

void D3D12_ReleaseTexture(
    SDL_GpuRenderer *driverData,
    SDL_GpuTexture *texture) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseSampler(
    SDL_GpuRenderer *driverData,
    SDL_GpuSampler *sampler) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuBuffer *buffer) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseTransferBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBuffer *transferBuffer) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseShader(
    SDL_GpuRenderer *driverData,
    SDL_GpuShader *shader)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12Shader *d3d12shader = (D3D12Shader *)shader;

    if (d3d12shader->bytecode) {
        SDL_free(d3d12shader->bytecode);
        d3d12shader->bytecode = NULL;
    }
    SDL_free(d3d12shader);
}

void D3D12_ReleaseComputePipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuComputePipeline *computePipeline) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseGraphicsPipeline(
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

void D3D12_SetViewport(
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
    ID3D12GraphicsCommandList2_RSSetViewports(d3d12CommandBuffer->graphicsCommandList, 1, &d3d12Viewport);
}

void D3D12_SetScissor(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuRect *scissor)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12_RECT scissorRect;
    scissorRect.left = scissor->x;
    scissorRect.top = scissor->y;
    scissorRect.right = scissor->x + scissor->w;
    scissorRect.bottom = scissor->y + scissor->h;
    ID3D12GraphicsCommandList2_RSSetScissorRects(d3d12CommandBuffer->graphicsCommandList, 1, &scissorRect);
}

void D3D12_BeginRenderPass(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuColorAttachmentInfo *colorAttachmentInfos,
    Uint32 colorAttachmentCount,
    SDL_GpuDepthStencilAttachmentInfo *depthStencilAttachmentInfo)
{

    SDL_assert(commandBuffer);
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12Renderer *renderer = d3d12CommandBuffer->renderer;

    Uint32 framebufferWidth = UINT32_MAX;
    Uint32 framebufferHeight = UINT32_MAX;

    for (Uint32 i = 0; i < colorAttachmentCount; ++i) {
        D3D12Texture *texture = (D3D12Texture *)colorAttachmentInfos[i].textureSlice.texture;
        auto h = texture->desc.Height >> colorAttachmentInfos[i].textureSlice.mipLevel;
        auto w = (int)texture->desc.Width >> colorAttachmentInfos[i].textureSlice.mipLevel;

        /* The framebuffer cannot be larger than the smallest attachment. */

        if (w < framebufferWidth) {
            framebufferWidth = w;
        }

        if (h < framebufferHeight) {
            framebufferHeight = h;
        }

        if (!texture->isRenderTarget) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Color attachment texture was not designated as a target!");
            return;
        }
    }

    if (depthStencilAttachmentInfo != NULL) {
        D3D12Texture *texture = (D3D12Texture *)depthStencilAttachmentInfo->textureSlice.texture;

        auto h = texture->desc.Height >> depthStencilAttachmentInfo->textureSlice.mipLevel;
        auto w = (int)texture->desc.Width >> depthStencilAttachmentInfo->textureSlice.mipLevel;

        /* The framebuffer cannot be larger than the smallest attachment. */

        if (w < framebufferWidth) {
            framebufferWidth = w;
        }

        if (h < framebufferHeight) {
            framebufferHeight = h;
        }

        if (!texture->isRenderTarget) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Depth stencil attachment texture was not designated as a target!");
            return;
        }
    }

    /* Layout transitions */

    d3d12CommandBuffer->colorAttachmentCount = colorAttachmentCount;

    for (Uint32 i = 0; i < colorAttachmentCount; i += 1) {

        D3D12Texture *texture = (D3D12Texture *)colorAttachmentInfos[i].textureSlice.texture;
        d3d12CommandBuffer->colorAttachmentTexture[i] = texture;

        D3D12_RESOURCE_BARRIER barrierDesc;
        SDL_zero(barrierDesc);

        barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrierDesc.Transition.pResource = texture->resource;
        barrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT,
        barrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;

        ID3D12GraphicsCommandList2_ResourceBarrier(d3d12CommandBuffer->graphicsCommandList, 1, &barrierDesc);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor = texture->rtvHandle;

        ID3D12GraphicsCommandList2_OMSetRenderTargets(d3d12CommandBuffer->graphicsCommandList, 1, &rtvDescriptor, SDL_FALSE, NULL);

        float clearColor[4];
        clearColor[0] = colorAttachmentInfos[i].clearColor.r;
        clearColor[1] = colorAttachmentInfos[i].clearColor.g;
        clearColor[2] = colorAttachmentInfos[i].clearColor.b;
        clearColor[3] = colorAttachmentInfos[i].clearColor.a;

        ID3D12GraphicsCommandList2_ClearRenderTargetView(d3d12CommandBuffer->graphicsCommandList, rtvDescriptor, clearColor, 0, NULL);
    }

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
        for (int i = commandBuffer->usedUniformBufferCount; i < commandBuffer->usedUniformBufferCapacity; ++i)
            SDL_zerop(commandBuffer->usedUniformBuffers[i]);
    }

    commandBuffer->usedUniformBuffers[commandBuffer->usedUniformBufferCount] = uniformBuffer;
    commandBuffer->usedUniformBufferCount += 1;
}

static D3D12UniformBuffer *D3D12_INTERNAL_CreateUniformBuffer(
    D3D12Renderer *renderer,
    Uint32 sizeInBytes)
{
    D3D12UniformBuffer *uniformBuffer;
    ID3D12Resource *buffer;
    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_RESOURCE_DESC resourceDesc;
    D3D12_RESOURCE_STATES initialState;
    HRESULT res;

    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = sizeInBytes;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    initialState = D3D12_RESOURCE_STATE_GENERIC_READ;

    res = ID3D12Device_CreateCommittedResource(
        renderer->device,
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        initialState,
        NULL,
        &D3D_IID_ID3D12Resource,
        (void **)&buffer);
    if (FAILED(res)) {
        ERROR_CHECK_RETURN("Could not create uniform buffer", NULL)
    }

    uniformBuffer = (D3D12UniformBuffer *)SDL_malloc(sizeof(D3D12UniformBuffer));
    SDL_zerop(uniformBuffer);
    uniformBuffer->buffer = buffer;

    return uniformBuffer;
}

static D3D12UniformBuffer *D3D12_INTERNAL_AcquireUniformBufferFromPool(
    D3D12CommandBuffer *commandBuffer)
{
    D3D12Renderer *renderer = commandBuffer->renderer;
    D3D12UniformBuffer *uniformBuffer;

    if (renderer->uniformBufferPoolCount > 0) {
        uniformBuffer = renderer->uniformBufferPool[renderer->uniformBufferPoolCount - 1];
        renderer->uniformBufferPoolCount -= 1;
    } else {
        uniformBuffer = D3D12_INTERNAL_CreateUniformBuffer(
            renderer,
            UNIFORM_BUFFER_SIZE);
    }

    D3D12_INTERNAL_TrackUniformBuffer(commandBuffer, uniformBuffer);

    return uniformBuffer;
}

void D3D12_BindGraphicsPipeline(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuGraphicsPipeline *graphicsPipeline)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12GraphicsPipeline *pipeline = (D3D12GraphicsPipeline *)graphicsPipeline;

    d3d12CommandBuffer->currentGraphicsPipeline = pipeline;

    // Set the pipeline state
    ID3D12GraphicsCommandList2_SetPipelineState(d3d12CommandBuffer->graphicsCommandList, pipeline->pipelineState);

    ID3D12GraphicsCommandList2_SetGraphicsRootSignature(d3d12CommandBuffer->graphicsCommandList, pipeline->rootSignature);

    ID3D12GraphicsCommandList2_IASetPrimitiveTopology(d3d12CommandBuffer->graphicsCommandList, SDLToD3D12_PrimitiveType[pipeline->primitiveType]);

    float blendFactor[4] = {
        pipeline->blendConstants[0],
        pipeline->blendConstants[1],
        pipeline->blendConstants[2],
        pipeline->blendConstants[3]
    };
    ID3D12GraphicsCommandList2_OMSetBlendFactor(d3d12CommandBuffer->graphicsCommandList, pipeline->blendConstants);

    ID3D12GraphicsCommandList2_OMSetStencilRef(d3d12CommandBuffer->graphicsCommandList, pipeline->stencilRef);

    ID3D12DescriptorHeap *descriptorHeaps[] = { d3d12CommandBuffer->descriptorHeap };
    ID3D12GraphicsCommandList2_SetDescriptorHeaps(d3d12CommandBuffer->graphicsCommandList, _countof(descriptorHeaps), descriptorHeaps);

    // Bind the uniform buffers (descriptor tables)
    for (Uint32 i = 0; i < pipeline->vertexUniformBufferCount; i++) {
        if (d3d12CommandBuffer->vertexUniformBuffers[i] == NULL) {
            d3d12CommandBuffer->vertexUniformBuffers[i] = D3D12_INTERNAL_AcquireUniformBufferFromPool(
                d3d12CommandBuffer);
        }
        ID3D12GraphicsCommandList2_SetGraphicsRootDescriptorTable(
            d3d12CommandBuffer->graphicsCommandList,
            i,
            d3d12CommandBuffer->vertexUniformBuffers[i]->gpuDescriptorHandle);
    }

    for (Uint32 i = 0; i < pipeline->fragmentUniformBufferCount; i++) {
        if (d3d12CommandBuffer->fragmentUniformBuffers[i] == NULL) {
            d3d12CommandBuffer->fragmentUniformBuffers[i] = D3D12_INTERNAL_AcquireUniformBufferFromPool(
                d3d12CommandBuffer);
        }
        ID3D12GraphicsCommandList2_SetGraphicsRootDescriptorTable(
            d3d12CommandBuffer->graphicsCommandList,
            pipeline->vertexUniformBufferCount + i,
            d3d12CommandBuffer->fragmentUniformBuffers[i]->gpuDescriptorHandle);
    }

    // Mark that uniform bindings are needed
    d3d12CommandBuffer->needVertexUniformBufferBind = SDL_TRUE;
    d3d12CommandBuffer->needFragmentUniformBufferBind = SDL_TRUE;
}

void D3D12_BindVertexBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstBinding,
    SDL_GpuBufferBinding *pBindings,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindIndexBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBufferBinding *pBinding,
    SDL_GpuIndexElementSize indexElementSize) { SDL_assert(SDL_FALSE); }

void D3D12_BindVertexSamplers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSamplerBinding *textureSamplerBindings,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindVertexStorageTextures(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSlice *storageTextureSlices,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindVertexStorageBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuBuffer **storageBuffers,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindFragmentSamplers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSamplerBinding *textureSamplerBindings,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindFragmentStorageTextures(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSlice *storageTextureSlices,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindFragmentStorageBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuBuffer **storageBuffers,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_PushVertexUniformData(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 slotIndex,
    const void *data,
    Uint32 dataLengthInBytes) { SDL_assert(SDL_FALSE); }

void D3D12_PushFragmentUniformData(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 slotIndex,
    const void *data,
    Uint32 dataLengthInBytes) { SDL_assert(SDL_FALSE); }

void D3D12_DrawIndexedPrimitives(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 baseVertex,
    Uint32 startIndex,
    Uint32 primitiveCount,
    Uint32 instanceCount) { SDL_assert(SDL_FALSE); }

static void D3D12_INTERNAL_BindGraphicsResources(
    D3D12CommandBuffer *commandBuffer)
{
    D3D12GraphicsPipeline *graphicsPipeline = commandBuffer->currentGraphicsPipeline;

    Uint32 vertexResourceCount =
        graphicsPipeline->vertexSamplerCount +
        graphicsPipeline->vertexStorageTextureCount +
        graphicsPipeline->vertexStorageBufferCount;

    Uint32 fragmentResourceCount =
        graphicsPipeline->fragmentSamplerCount +
        graphicsPipeline->fragmentStorageTextureCount +
        graphicsPipeline->fragmentStorageBufferCount;

    if (commandBuffer->needVertexSamplerBind) {
        if (graphicsPipeline->vertexSamplerCount > 0) {
            ID3D12GraphicsCommandList2_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                0,
                commandBuffer->vertexSamplerDescriptorHeapHandle);
        }
        commandBuffer->needVertexSamplerBind = SDL_FALSE;
    }

    if (commandBuffer->needVertexResourceBind) {
        if (vertexResourceCount > 0) {
            ID3D12GraphicsCommandList2_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                1,
                commandBuffer->vertexShaderResourceDescriptorHeapHandle);
        }
        commandBuffer->needVertexResourceBind = SDL_FALSE;
    }

    if (commandBuffer->needVertexUniformBufferBind) {
        for (Uint32 i = 0; i < graphicsPipeline->vertexUniformBufferCount; ++i) {
            ID3D12GraphicsCommandList2_SetGraphicsRootConstantBufferView(
                commandBuffer->graphicsCommandList,
                i + 2,
                commandBuffer->vertexUniformBuffers[i]->bufferLocation);
        }
        commandBuffer->needVertexUniformBufferBind = SDL_FALSE;
    }

    if (commandBuffer->needFragmentSamplerBind) {
        if (graphicsPipeline->fragmentSamplerCount > 0) {
            ID3D12GraphicsCommandList2_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                graphicsPipeline->vertexSamplerCount + 2,
                commandBuffer->fragmentSamplerDescriptorHeapHandle);
        }
        commandBuffer->needFragmentSamplerBind = SDL_FALSE;
    }

    if (commandBuffer->needFragmentResourceBind) {
        if (fragmentResourceCount > 0) {
            ID3D12GraphicsCommandList2_SetGraphicsRootDescriptorTable(
                commandBuffer->graphicsCommandList,
                graphicsPipeline->vertexSamplerCount + 3,
                commandBuffer->fragmentShaderResourceDescriptorHeapHandle);
        }
        commandBuffer->needFragmentResourceBind = SDL_FALSE;
    }

    if (commandBuffer->needFragmentUniformBufferBind) {
        for (Uint32 i = 0; i < graphicsPipeline->fragmentUniformBufferCount; ++i) {
            ID3D12GraphicsCommandList2_SetGraphicsRootConstantBufferView(
                commandBuffer->graphicsCommandList,
                graphicsPipeline->vertexUniformBufferCount + 2 + i,
                commandBuffer->fragmentUniformBuffers[i]->bufferLocation);
        }
        commandBuffer->needFragmentUniformBufferBind = SDL_FALSE;
    }
}

void D3D12_DrawPrimitives(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 vertexStart,
    Uint32 primitiveCount)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12_INTERNAL_BindGraphicsResources(d3d12CommandBuffer);

    // Record the draw call
    ID3D12GraphicsCommandList2_IASetPrimitiveTopology(
        d3d12CommandBuffer->graphicsCommandList,
        SDLToD3D12_PrimitiveType[d3d12CommandBuffer->currentGraphicsPipeline->primitiveType]);

    ID3D12GraphicsCommandList2_DrawInstanced(
        d3d12CommandBuffer->graphicsCommandList,
        PrimitiveVerts(d3d12CommandBuffer->currentGraphicsPipeline->primitiveType, primitiveCount),
        1, // Instance count
        vertexStart,
        0 // Start instance location
    );
}

void D3D12_DrawPrimitivesIndirect(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBuffer *buffer,
    Uint32 offsetInBytes,
    Uint32 drawCount,
    Uint32 stride) { SDL_assert(SDL_FALSE); }

void D3D12_DrawIndexedPrimitivesIndirect(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBuffer *buffer,
    Uint32 offsetInBytes,
    Uint32 drawCount,
    Uint32 stride) { SDL_assert(SDL_FALSE); }

void D3D12_EndRenderPass(
    SDL_GpuCommandBuffer *commandBuffer)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12Renderer *renderer = (D3D12Renderer *)d3d12CommandBuffer->renderer;
    Uint32 i;

    for (Uint32 i = 0; i < d3d12CommandBuffer->colorAttachmentCount; i += 1) {

        D3D12Texture *texture = d3d12CommandBuffer->colorAttachmentTexture[i];
        d3d12CommandBuffer->colorAttachmentTexture[i] = NULL;
        D3D12_RESOURCE_BARRIER barrierDesc;
        SDL_zero(barrierDesc);

        barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrierDesc.Transition.pResource = texture->resource;
        barrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
        barrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;

        ID3D12GraphicsCommandList2_ResourceBarrier(d3d12CommandBuffer->graphicsCommandList, 1, &barrierDesc);
    }

    d3d12CommandBuffer->colorAttachmentCount = 0;
}

/* Compute Pass */

void D3D12_BeginComputePass(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuStorageTextureReadWriteBinding *storageTextureBindings,
    Uint32 storageTextureBindingCount,
    SDL_GpuStorageBufferReadWriteBinding *storageBufferBindings,
    Uint32 storageBufferBindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindComputePipeline(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuComputePipeline *computePipeline) { SDL_assert(SDL_FALSE); }

void D3D12_BindComputeStorageTextures(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSlice *storageTextureSlices,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindComputeStorageBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuBuffer **storageBuffers,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_PushComputeUniformData(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 slotIndex,
    const void *data,
    Uint32 dataLengthInBytes) { SDL_assert(SDL_FALSE); }

void D3D12_DispatchCompute(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 groupCountX,
    Uint32 groupCountY,
    Uint32 groupCountZ) { SDL_assert(SDL_FALSE); }

void D3D12_EndComputePass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

/* TransferBuffer Data */

void D3D12_MapTransferBuffer(
    SDL_GpuRenderer *device,
    SDL_GpuTransferBuffer *transferBuffer,
    SDL_bool cycle,
    void **ppData) { SDL_assert(SDL_FALSE); }

void D3D12_UnmapTransferBuffer(
    SDL_GpuRenderer *device,
    SDL_GpuTransferBuffer *transferBuffer) { SDL_assert(SDL_FALSE); }

void D3D12_SetTransferData(
    SDL_GpuRenderer *driverData,
    const void *source,
    SDL_GpuTransferBufferRegion *destination,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_GetTransferData(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBufferRegion *source,
    void *destination) { SDL_assert(SDL_FALSE); }

/* Copy Pass */

void D3D12_BeginCopyPass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

void D3D12_UploadToTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureTransferInfo *source,
    SDL_GpuTextureRegion *destination,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_UploadToBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTransferBufferLocation *source,
    SDL_GpuBufferRegion *destination,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_CopyTextureToTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureLocation *source,
    SDL_GpuTextureLocation *destination,
    Uint32 w,
    Uint32 h,
    Uint32 d,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_CopyBufferToBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBufferLocation *source,
    SDL_GpuBufferLocation *destination,
    Uint32 size,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_GenerateMipmaps(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTexture *texture) { SDL_assert(SDL_FALSE); }

void D3D12_DownloadFromTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureRegion *source,
    SDL_GpuTextureTransferInfo *destination) { SDL_assert(SDL_FALSE); }

void D3D12_DownloadFromBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBufferRegion *source,
    SDL_GpuTransferBufferLocation *destination) { SDL_assert(SDL_FALSE); }

void D3D12_EndCopyPass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

void D3D12_Blit(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureRegion *source,
    SDL_GpuTextureRegion *destination,
    SDL_GpuFilter filterMode,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

/* Submission/Presentation */

SDL_bool D3D12_SupportsSwapchainComposition(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuSwapchainComposition swapchainComposition)
{
    SDL_assert(SDL_FALSE);
    return SDL_FALSE;
}

SDL_bool D3D12_SupportsPresentMode(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuPresentMode presentMode)
{
    SDL_assert(SDL_FALSE);
    return SDL_FALSE;
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
    D3D12WindowData *windowData)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
    HRESULT res;

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { 0 };
    rtvHeapDesc.NumDescriptors = SWAPCHAIN_BUFFER_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    res = ID3D12Device_CreateDescriptorHeap(renderer->device, &rtvHeapDesc,
                                            &D3D_IID_ID3D12DescriptorHeap,
                                            (void **)&windowData->rtvHeap);
    ERROR_CHECK_RETURN("Could not create descriptor heap!", 0);

    UINT rtvDescriptorSize = ID3D12Device_GetDescriptorHandleIncrementSize(renderer->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor;
    SDL_zero(rtvDescriptor);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(windowData->rtvHeap, &rtvDescriptor);

    SDL_zero(windowData->renderTargets);

    for (UINT i = 0; i < SWAPCHAIN_BUFFER_COUNT; ++i) {
        // Get a pointer to the back buffer
        res = IDXGISwapChain3_GetBuffer(swapchain, i,
                                        &D3D_IID_ID3D12Resource,
                                        (void **)&windowData->renderTargets[i]);
        ERROR_CHECK_RETURN("Could not get swapchain buffer descriptor heap!", 0);

        // Create an RTV for each buffer

        SDL_zero(rtvDesc);
        rtvDesc.Format = rtvFormat;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        ID3D12Device_CreateRenderTargetView(renderer->device, windowData->renderTargets[i], &rtvDesc, rtvDescriptor);

        windowData->renderTexture[i] = SDL_calloc(1, sizeof(D3D12Texture));
        SDL_assert(windowData->renderTexture[i]);
        SDL_zerop(windowData->renderTexture[i]);
        ID3D12Resource_GetDesc(windowData->renderTargets[i], &windowData->renderTexture[i]->desc);
        windowData->renderTexture[i]->rtvHandle = rtvDescriptor;
        windowData->renderTexture[i]->resource = windowData->renderTargets[i];
        windowData->renderTexture[i]->isRenderTarget = SDL_TRUE;

        rtvDescriptor.ptr += 1 * rtvDescriptorSize;
    }
    return SDL_TRUE;
}

static void D3D12_INTERNAL_DestroyWindowData(
    D3D12Renderer *renderer,
    D3D12WindowData *windowData)
{
    for (int i = SWAPCHAIN_BUFFER_COUNT - 1; i >= 0; --i) {
        D3D12Texture *texture = windowData->renderTexture[i];
        windowData->renderTargets[i] = NULL;
        windowData->renderTexture[i] = NULL;
        if (texture) {
            ID3D12Resource *resource = texture->resource;
            SDL_free(texture);
            if (resource) {
                ID3D12Resource_Release(resource);
            }
        }
    }
    if (windowData->rtvHeap) {
        ID3D12DescriptorHeap_Release(windowData->rtvHeap);
        windowData->rtvHeap = NULL;
    }
    if (windowData->swapchain) {
        IDXGISwapChain3_Release(windowData->swapchain);
        windowData->swapchain = NULL;
    }
}

static SDL_bool D3D12_INTERNAL_CreateSwapchain(
    D3D12Renderer *renderer,
    D3D12WindowData *windowData,
    SDL_GpuSwapchainComposition swapchainComposition,
    SDL_GpuPresentMode presentMode)
{
    HWND dxgiHandle;
    int width, height;
    Uint32 i;
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
    swapchainDesc.Stereo = SDL_FALSE;
    swapchainDesc.SampleDesc.Count = 1;
    swapchainDesc.SampleDesc.Quality = 0;
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // | DXGI_USAGE_UNORDERED_ACCESS | DXGI_USAGE_SHADER_INPUT;
    swapchainDesc.BufferCount = SWAPCHAIN_BUFFER_COUNT;
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

    if (!IsWindow(dxgiHandle)) {
        return SDL_FALSE;
    }

    /* Create the swapchain! */
    res = IDXGIFactory4_CreateSwapChainForHwnd(
        renderer->factory,
        (IUnknown *)renderer->commandBuffer->commandQueue,
        dxgiHandle,
        &swapchainDesc,
        &fullscreenDesc,
        NULL,
        &swapchain);
    ERROR_CHECK_RETURN("Could not create swapchain", 0);

    res = IDXGISwapChain3_QueryInterface(
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
    res = IDXGISwapChain_GetParent(
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

    //    for (i = 0; i < MAX_FRAMES_IN_FLIGHT; i += 1) {
    //        windowData->inFlightFences[i] = NULL;
    //    }

    /* If a you are using a FLIP model format you can't create the swapchain as DXGI_FORMAT_B8G8R8A8_UNORM_SRGB.
     * You have to create the swapchain as DXGI_FORMAT_B8G8R8A8_UNORM and then set the render target view's format to DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
     */
    if (!D3D12_INTERNAL_InitializeSwapchainTexture(
            renderer,
            swapchain3,
            swapchainFormat,
            (swapchainComposition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : windowData->swapchainFormat,
            windowData)) {
        IDXGISwapChain_Release(swapchain3);
        return SDL_FALSE;
    }

    /* Initialize dummy container */
    // SDL_zerop(&windowData->textureContainer);
    // windowData->textureContainer.textures = SDL_calloc(1, sizeof(D3D12Texture *));

    return SDL_TRUE;
}

SDL_bool D3D12_ClaimWindow(
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

void D3D12_UnclaimWindow(
    SDL_GpuRenderer *driverData,
    SDL_Window *window)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12WindowData *windowData = D3D12_INTERNAL_FetchWindowData(window);

    if (windowData == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Window already unclaimed!");
        return;
    }

    SDL_assert(!windowData->activeWindow);

    D3D12_INTERNAL_DestroyWindowData(renderer, windowData);
    SDL_ClearProperty(SDL_GetWindowProperties(window), WINDOW_PROPERTY_DATA);
    SDL_free(windowData);
}

SDL_bool D3D12_SetSwapchainParameters(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuSwapchainComposition swapchainComposition,
    SDL_GpuPresentMode presentMode)
{
    SDL_assert(SDL_FALSE);
    return SDL_FALSE;
}

SDL_GpuTextureFormat D3D12_GetSwapchainTextureFormat(
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

SDL_GpuCommandBuffer *D3D12_AcquireCommandBuffer(
    SDL_GpuRenderer *driverData)
{
    SDL_assert(driverData);
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    SDL_assert(renderer->commandBuffer);
    return (SDL_GpuCommandBuffer *)renderer->commandBuffer;
}

SDL_GpuTexture *D3D12_AcquireSwapchainTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_Window *window,
    Uint32 *pWidth,
    Uint32 *pHeight)
{
    HRESULT res;
    D3D12Texture *texture;
    SDL_assert(commandBuffer);
    SDL_assert(window);
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12Renderer *renderer = d3d12CommandBuffer->renderer;
    D3D12WindowData *windowData = D3D12_INTERNAL_FetchWindowData(window);
    SDL_assert(windowData);

    if (!windowData->activeWindow) {
        D3D12WindowData **nextWindowPtr = &d3d12CommandBuffer->nextWindow;
        while (*nextWindowPtr)
            nextWindowPtr = &(*nextWindowPtr)->nextWindow;
        *nextWindowPtr = windowData;
        windowData->nextWindow = NULL;
        windowData->activeWindow = TRUE;
    }

    texture = windowData->renderTexture[windowData->frameCounter];
    if (texture) {
        SDL_assert(texture->desc.Width <= UINT32_MAX);
        *pWidth = (UINT32)(texture->desc.Width);
        *pHeight = texture->desc.Height;
    } else {
        *pWidth = 0;
        *pHeight = 0;
    }
    return (SDL_GpuTexture *)texture;
}

void D3D12_Submit(
    SDL_GpuCommandBuffer *commandBuffer)
{
    HRESULT res;

    SDL_assert(commandBuffer);
    D3D12CommandBuffer *d3d12commandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12Renderer *renderer = d3d12commandBuffer->renderer;
    ID3D12CommandList *commandLists[1];
    res = ID3D12GraphicsCommandList2_Close(d3d12commandBuffer->graphicsCommandList);
    ERROR_CHECK("Could not close graphicsCommandList");
    commandLists[0] = (ID3D12CommandList *)d3d12commandBuffer->graphicsCommandList;
    ID3D12CommandQueue_ExecuteCommandLists(d3d12commandBuffer->commandQueue, _countof(commandLists), commandLists);

    D3D12WindowData *window = d3d12commandBuffer->nextWindow;
    d3d12commandBuffer->nextWindow = NULL;

    while (window) {
        SDL_assert(window->activeWindow);
        D3D12WindowData *nextWindow = window->nextWindow;
        window->nextWindow = NULL;
        window->activeWindow = FALSE;
        IDXGISwapChain3_Present(window->swapchain, 1, 0);
        window->frameCounter = IDXGISwapChain3_GetCurrentBackBufferIndex(window->swapchain);
        window = nextWindow;
    }

    const UINT64 fenceToWaitFor = d3d12commandBuffer->fenceValue;
    res = ID3D12CommandQueue_Signal(d3d12commandBuffer->commandQueue, d3d12commandBuffer->fence, d3d12commandBuffer->fenceValue);
    ERROR_CHECK("Could not signale commandQueue");
    d3d12commandBuffer->fenceValue++;

    if (ID3D12Fence_GetCompletedValue(d3d12commandBuffer->fence) < fenceToWaitFor) {
        ID3D12Fence_SetEventOnCompletion(d3d12commandBuffer->fence, fenceToWaitFor, d3d12commandBuffer->fenceEvent);
        WaitForSingleObject(d3d12commandBuffer->fenceEvent, INFINITE);
    }

    res = ID3D12CommandAllocator_Reset(d3d12commandBuffer->commandAllocator);
    ERROR_CHECK("Could not reset commandAllocator");
    res = ID3D12GraphicsCommandList2_Reset(d3d12commandBuffer->graphicsCommandList, d3d12commandBuffer->commandAllocator, NULL);
    ERROR_CHECK("Could not reset graphicsCommandList");
}

SDL_GpuFence *D3D12_SubmitAndAcquireFence(
    SDL_GpuCommandBuffer *commandBuffer)
{
    SDL_assert(SDL_FALSE);
    return NULL;
}

void D3D12_Wait(
    SDL_GpuRenderer *driverData) { SDL_assert(SDL_FALSE); }

void D3D12_WaitForFences(
    SDL_GpuRenderer *driverData,
    SDL_bool waitAll,
    SDL_GpuFence **pFences,
    Uint32 fenceCount) { SDL_assert(SDL_FALSE); }

SDL_bool D3D12_QueryFence(
    SDL_GpuRenderer *driverData,
    SDL_GpuFence *fence)
{
    SDL_assert(SDL_FALSE);
    return SDL_FALSE;
}

void D3D12_ReleaseFence(
    SDL_GpuRenderer *driverData,
    SDL_GpuFence *fence) { SDL_assert(SDL_FALSE); }

/* Feature Queries */

SDL_bool D3D12_IsTextureFormatSupported(
    SDL_GpuRenderer *driverData,
    SDL_GpuTextureFormat format,
    SDL_GpuTextureType type,
    SDL_GpuTextureUsageFlags usage)
{
    SDL_assert(SDL_FALSE);
    return SDL_FALSE;
}

SDL_GpuSampleCount D3D12_GetBestSampleCount(
    SDL_GpuRenderer *driverData,
    SDL_GpuTextureFormat format,
    SDL_GpuSampleCount desiredSampleCount)
{
    SDL_assert(SDL_FALSE);
    return SDL_GPU_SAMPLECOUNT_1;
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

    // D3D12 support is incomplete at this time
    return SDL_FALSE;

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

    res = IDXGIAdapter1_QueryInterface(
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

    /*
    res = DXGIGetDebugInterfaceFunc(&D3D_IID_IDXGIInfoQueue, (void **)&renderer->dxgiInfoQueue);
    if (FAILED(res)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not get IDXGIInfoQueue interface");
    }
    */
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
            renderer->supportsTearing = FALSE;
        }
        IDXGIFactory5_Release(factory5);
    }

    /* Select the appropriate device for rendering */
    res = IDXGIAdapter1_QueryInterface(
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

    renderer->commandBuffer = (D3D12CommandBuffer *)SDL_malloc(sizeof(D3D12CommandBuffer));
    SDL_zerop(renderer->commandBuffer);
    renderer->commandBuffer->renderer = renderer;

    renderer->commandBuffer->fenceLock = SDL_CreateMutex();

    SDL_zero(queueDesc);
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    res = ID3D12Device_CreateCommandQueue(
        renderer->device,
        &queueDesc,
        &D3D_IID_ID3D12CommandQueue,
        (void **)&renderer->commandBuffer->commandQueue);

    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not create D3D12CommandQueue", NULL);
    }

    // Create the command allocator
    res = ID3D12Device_CreateCommandAllocator(
        renderer->device,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        &D3D_IID_ID3D12CommandAllocator,
        (void **)&renderer->commandBuffer->commandAllocator);

    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not create ID3D12CommandAllocator", NULL);
    }

    // Create the command list
    res = ID3D12Device_CreateCommandList(
        renderer->device,
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        renderer->commandBuffer->commandAllocator,
        NULL,
        &SDL_IID_ID3D12GraphicsCommandList2,
        (void **)&renderer->commandBuffer->graphicsCommandList);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not create ID3D12CommandList", NULL);
    }

    res = ID3D12GraphicsCommandList2_Close(renderer->commandBuffer->graphicsCommandList);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not close ID3D12CommandList", NULL);
    }

    res = ID3D12CommandAllocator_Reset(renderer->commandBuffer->commandAllocator);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not reset commandAllocator", NULL);
    }
    res = ID3D12GraphicsCommandList2_Reset(renderer->commandBuffer->graphicsCommandList, renderer->commandBuffer->commandAllocator, NULL);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not reset graphicsCommandList", NULL);
    }

    // Create fence
    ID3D12Device_CreateFence(
        renderer->device,
        0,
        D3D12_FENCE_FLAG_NONE,
        &SDL_IID_ID3D12Fence,
        (void **)&renderer->commandBuffer->fence);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
        ERROR_CHECK_RETURN("Could not create ID3D12Fence", NULL);
    }
    renderer->commandBuffer->fenceValue = 1;
    renderer->commandBuffer->fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = { 0 };
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        res = ID3D12Device_CreateDescriptorHeap(
            renderer->device,
            &heapDesc,
            &SDL_IID_ID3D12DescriptorHeap,
            (void **)&renderer->commandBuffer->descriptorHeap);
        if (FAILED(res)) {
            D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
            ERROR_CHECK_RETURN("Could not create ID3D12DescriptorHeap", NULL);
        }
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(renderer->commandBuffer->descriptorHeap, &renderer->commandBuffer->descriptorHeapHandle);
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = { 0 };
        samplerHeapDesc.NumDescriptors = MAX_VERTEX_SAMPLERS;
        samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        res = ID3D12Device_CreateDescriptorHeap(
            renderer->device,
            &samplerHeapDesc,
            &SDL_IID_ID3D12DescriptorHeap,
            (void **)&renderer->commandBuffer->vertexSamplerDescriptorHeap);
        if (FAILED(res)) {
            D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
            ERROR_CHECK_RETURN("Could not create ID3D12DescriptorHeap for vertex samplers", NULL);
        }
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(renderer->commandBuffer->vertexSamplerDescriptorHeap, &renderer->commandBuffer->vertexSamplerDescriptorHeapHandle);
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = { 0 };
        samplerHeapDesc.NumDescriptors = MAX_FRAGMENT_SAMPLERS;
        samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        res = ID3D12Device_CreateDescriptorHeap(
            renderer->device,
            &samplerHeapDesc,
            &SDL_IID_ID3D12DescriptorHeap,
            (void **)&renderer->commandBuffer->fragmentSamplerDescriptorHeap);
        if (FAILED(res)) {
            D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
            ERROR_CHECK_RETURN("Could not create ID3D12DescriptorHeap for fragment samplers", NULL);
        }
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(renderer->commandBuffer->fragmentSamplerDescriptorHeap, &renderer->commandBuffer->fragmentSamplerDescriptorHeapHandle);
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = { 0 };
        srvHeapDesc.NumDescriptors = MAX_VERTEX_RESOURCE_COUNT;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        res = ID3D12Device_CreateDescriptorHeap(
            renderer->device,
            &srvHeapDesc,
            &SDL_IID_ID3D12DescriptorHeap,
            (void **)&renderer->commandBuffer->vertexShaderResourceDescriptorHeap);
        if (FAILED(res)) {
            D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
            ERROR_CHECK_RETURN("Could not create ID3D12DescriptorHeap for vertex shader resources", NULL);
        }
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(renderer->commandBuffer->vertexShaderResourceDescriptorHeap, &renderer->commandBuffer->vertexShaderResourceDescriptorHeapHandle);
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = { 0 };
        srvHeapDesc.NumDescriptors = MAX_FRAGMENT_RESOURCE_COUNT;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        res = ID3D12Device_CreateDescriptorHeap(
            renderer->device,
            &srvHeapDesc,
            &SDL_IID_ID3D12DescriptorHeap,
            (void **)&renderer->commandBuffer->fragmentShaderResourceDescriptorHeap);
        if (FAILED(res)) {
            D3D12_INTERNAL_DestroyRendererAndFree(&renderer);
            ERROR_CHECK_RETURN("Could not create ID3D12DescriptorHeap for fragment shader resources", NULL);
        }
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(renderer->commandBuffer->fragmentShaderResourceDescriptorHeap, &renderer->commandBuffer->fragmentShaderResourceDescriptorHeapHandle);
    }

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
