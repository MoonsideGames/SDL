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

#if SDL_VIDEO_RENDER_GPU

#include "SDL_shaders_gpu.h"

/* SDL_Gpu shader implementation */

typedef struct GPU_ShaderModuleSource
{
    const unsigned char *code;
    unsigned int code_len;
    SDL_GpuShaderFormat format;
} GPU_ShaderModuleSource;

// FIXME Please fix this in the build system!
#ifndef SDL_GPU_VULKAN
#warning SDL_GPU_VULKAN was not defined, please fix
#define SDL_GPU_VULKAN 0
#endif
#ifndef SDL_GPU_D3D11
#warning SDL_GPU_D3D11 was not defined, please fix
#define SDL_GPU_D3D11 0
#endif
#ifndef SDL_GPU_D3D12
#warning SDL_GPU_D3D12 was not defined, please fix
#define SDL_GPU_D3D12 0
#endif
#ifndef SDL_GPU_METAL
#warning SDL_GPU_METAL was not defined, please fix
#define SDL_GPU_METAL 0
#endif

#if SDL_GPU_VULKAN
#define IF_VULKAN(...) __VA_ARGS__
#include "shaders/spir-v.h"
#else
#define IF_VULKAN(...)
#endif

#if SDL_GPU_D3D11
#define IF_D3D11(...) __VA_ARGS__
#include "shaders/dxbc50.h"
#else
#define IF_D3D11(...)
#endif

#if SDL_GPU_D3D12
#define IF_D3D12(...) __VA_ARGS__
#include "shaders/dxil60.h"
#else
#define IF_D3D12(...)
#endif

#if SDL_GPU_METAL
#define IF_METAL(...) __VA_ARGS__
#include "shaders/metal.h"
#else
#define IF_METAL(...)
#endif

typedef struct GPU_ShaderSources
{
    IF_VULKAN(GPU_ShaderModuleSource spirv;)
    IF_D3D11(GPU_ShaderModuleSource dxbc50;)
    IF_D3D12(GPU_ShaderModuleSource dxil60;)
    IF_METAL(GPU_ShaderModuleSource msl;)
    unsigned int num_samplers;
    unsigned int num_uniform_buffers;
} GPU_ShaderSources;

#define SHADER_SPIRV(code) \
    IF_VULKAN(.spirv = { code, sizeof(code), SDL_GPU_SHADERFORMAT_SPIRV }, )

#define SHADER_DXBC50(code) \
    IF_D3D11(.dxbc50 = { code, sizeof(code), SDL_GPU_SHADERFORMAT_DXBC }, )

#define SHADER_DXIL60(code) \
    IF_D3D12(.dxil60 = { code, sizeof(code), SDL_GPU_SHADERFORMAT_DXIL }, )

#define SHADER_METAL(code) \
    IF_METAL(.msl = { code, sizeof(code), SDL_GPU_SHADERFORMAT_MSL }, )

/* clang-format off */
static const GPU_ShaderSources vert_shader_sources[NUM_VERT_SHADERS] = {
    [VERT_SHADER_LINEPOINT] = {
        .num_samplers = 0,
        .num_uniform_buffers = 1,
        SHADER_SPIRV(linepoint_vert_spv)
        SHADER_DXBC50(linepoint_vert_sm50_dxbc)
        SHADER_DXIL60(linepoint_vert_sm60_dxil)
        SHADER_METAL(linepoint_vert_metal)
    },
    [VERT_SHADER_TRI_COLOR] = {
        .num_samplers = 0,
        .num_uniform_buffers = 1,
        SHADER_SPIRV(tri_color_vert_spv)
        SHADER_DXBC50(tri_color_vert_sm50_dxbc)
        SHADER_DXIL60(tri_color_vert_sm60_dxil)
        SHADER_METAL(tri_color_vert_metal)
    },
    [VERT_SHADER_TRI_TEXTURE] = {
        .num_samplers = 0,
        .num_uniform_buffers = 1,
        SHADER_SPIRV(tri_texture_vert_spv)
        SHADER_DXBC50(tri_texture_vert_sm50_dxbc)
        SHADER_DXIL60(tri_texture_vert_sm60_dxil)
        SHADER_METAL(tri_texture_vert_metal)
    },
};

static const GPU_ShaderSources frag_shader_sources[NUM_FRAG_SHADERS] = {
    [FRAG_SHADER_COLOR] = {
        .num_samplers = 0,
        .num_uniform_buffers = 0,
        SHADER_SPIRV(color_frag_spv)
        SHADER_DXBC50(color_frag_sm50_dxbc)
        SHADER_DXIL60(color_frag_sm60_dxil)
        SHADER_METAL(color_frag_metal)
    },
    [FRAG_SHADER_TEXTURE_RGB] = {
        .num_samplers = 1,
        .num_uniform_buffers = 0,
        SHADER_SPIRV(texture_rgb_frag_spv)
        SHADER_DXBC50(texture_rgb_frag_sm50_dxbc)
        SHADER_DXIL60(texture_rgb_frag_sm60_dxil)
        SHADER_METAL(texture_rgb_frag_metal)
    },
    [FRAG_SHADER_TEXTURE_RGBA] = {
        .num_samplers = 1,
        .num_uniform_buffers = 0,
        SHADER_SPIRV(texture_rgba_frag_spv)
        SHADER_DXBC50(texture_rgba_frag_sm50_dxbc)
        SHADER_DXIL60(texture_rgba_frag_sm60_dxil)
        SHADER_METAL(texture_rgb_frag_metal)
    },
};
/* clang-format on */

static SDL_GpuShader *CompileShader(const GPU_ShaderSources *sources, SDL_GpuDevice *device, SDL_GpuShaderStage stage)
{
    const GPU_ShaderModuleSource *sms = NULL;
    SDL_GpuDriver driver = SDL_GpuGetDriver(device);

    switch (driver) {
        /* clang-format off */
        IF_VULKAN(  case SDL_GPU_DRIVER_VULKAN: sms = &sources->spirv;  break;)
        IF_D3D11(   case SDL_GPU_DRIVER_D3D11:  sms = &sources->dxbc50; break;)
        IF_D3D12(   case SDL_GPU_DRIVER_D3D12:  sms = &sources->dxil60; break;)
        IF_METAL(   case SDL_GPU_DRIVER_METAL:  sms = &sources->msl;    break;)
        /* clang-format on */

    default:
        SDL_SetError("Unsupported GPU backend");
        return NULL;
    }

    SDL_GpuShaderCreateInfo sci = { 0 };
    sci.code = sms->code;
    sci.codeSize = sms->code_len;
    sci.format = sms->format;
    /* FIXME not sure if this is correct */
    sci.entryPointName = driver == SDL_GPU_DRIVER_METAL ? "main0" : "main";
    sci.samplerCount = sources->num_samplers;
    sci.uniformBufferCount = sources->num_uniform_buffers;
    sci.stage = stage;

    return SDL_GpuCreateShader(device, &sci);
}

int GPU_InitShaders(GPU_Shaders *shaders, SDL_GpuDevice *device)
{
    for (int i = 0; i < SDL_arraysize(vert_shader_sources); ++i) {
        shaders->vert_shaders[i] = CompileShader(
            &vert_shader_sources[i], device, SDL_GPU_SHADERSTAGE_VERTEX);
        if (shaders->vert_shaders[i] == NULL) {
            GPU_ReleaseShaders(shaders, device);
            return -1;
        }
    }

    for (int i = 0; i < SDL_arraysize(frag_shader_sources); ++i) {
        shaders->frag_shaders[i] = CompileShader(
            &frag_shader_sources[i], device, SDL_GPU_SHADERSTAGE_FRAGMENT);
        if (shaders->frag_shaders[i] == NULL) {
            GPU_ReleaseShaders(shaders, device);
            return -1;
        }
    }

    return 0;
}

void GPU_ReleaseShaders(GPU_Shaders *shaders, SDL_GpuDevice *device)
{
    for (int i = 0; i < SDL_arraysize(shaders->vert_shaders); ++i) {
        SDL_GpuReleaseShader(device, shaders->vert_shaders[i]);
        shaders->vert_shaders[i] = NULL;
    }

    for (int i = 0; i < SDL_arraysize(shaders->frag_shaders); ++i) {
        SDL_GpuReleaseShader(device, shaders->frag_shaders[i]);
        shaders->frag_shaders[i] = NULL;
    }
}

SDL_GpuShader *GPU_GetVertexShader(GPU_Shaders *shaders, GPU_VertexShaderID id)
{
    SDL_assert((unsigned int)id < SDL_arraysize(shaders->vert_shaders));
    SDL_GpuShader *shader = shaders->vert_shaders[id];
    SDL_assert(shader != NULL);
    return shader;
}

SDL_GpuShader *GPU_GetFragmentShader(GPU_Shaders *shaders, GPU_FragmentShaderID id)
{
    SDL_assert((unsigned int)id < SDL_arraysize(shaders->frag_shaders));
    SDL_GpuShader *shader = shaders->frag_shaders[id];
    SDL_assert(shader != NULL);
    return shader;
}

void GPU_FillSupportedShaderFormats(SDL_PropertiesID props)
{
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_CREATEDEVICE_SHADERS_SPIRV_BOOL, SDL_GPU_VULKAN);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_CREATEDEVICE_SHADERS_DXBC_BOOL, SDL_GPU_D3D11);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_CREATEDEVICE_SHADERS_DXIL_BOOL, SDL_GPU_D3D12);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_CREATEDEVICE_SHADERS_MSL_BOOL, SDL_GPU_METAL);
}

#endif /* SDL_VIDEO_RENDER_OGL */