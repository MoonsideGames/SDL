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
#include "SDL_gpu_driver.h"
#include "SDL_gpu_spirv_c.h"
#include "spirv_cross_c.h"

#if defined(_WIN32)
#define SPIRV_CROSS_DLL "spirv-cross-c-shared.dll"
#elif defined(__APPLE__)
#define SPIRV_CROSS_DLL "libspirv-cross-c-shared.0.dylib"
#else
#define SPIRV_CROSS_DLL "libspirv-cross-c-shared.so.0"
#endif

#define SPVC_ERROR(func) \
    SDL_SetError(#func " failed: %s", SDL_spvc_context_get_last_error_string(context))

static void *spirvcross_dll = NULL;

typedef spvc_result (*pfn_spvc_context_create)(spvc_context *context);
typedef void (*pfn_spvc_context_destroy)(spvc_context);
typedef spvc_result (*pfn_spvc_context_parse_spirv)(spvc_context, const SpvId *, size_t, spvc_parsed_ir *);
typedef spvc_result (*pfn_spvc_context_create_compiler)(spvc_context, spvc_backend, spvc_parsed_ir, spvc_capture_mode, spvc_compiler *);
typedef spvc_result (*pfn_spvc_compiler_create_compiler_options)(spvc_compiler, spvc_compiler_options *);
typedef spvc_result (*pfn_spvc_compiler_options_set_uint)(spvc_compiler_options, spvc_compiler_option, unsigned);
typedef spvc_result (*pfn_spvc_compiler_install_compiler_options)(spvc_compiler, spvc_compiler_options);
typedef spvc_result (*pfn_spvc_compiler_compile)(spvc_compiler, const char **);
typedef const char *(*pfn_spvc_context_get_last_error_string)(spvc_context);
typedef SpvExecutionModel (*pfn_spvc_compiler_get_execution_model)(spvc_compiler compiler);
typedef const char *(*pfn_spvc_compiler_get_cleansed_entry_point_name)(spvc_compiler compiler, const char *name, SpvExecutionModel model);

static pfn_spvc_context_create SDL_spvc_context_create = NULL;
static pfn_spvc_context_destroy SDL_spvc_context_destroy = NULL;
static pfn_spvc_context_parse_spirv SDL_spvc_context_parse_spirv = NULL;
static pfn_spvc_context_create_compiler SDL_spvc_context_create_compiler = NULL;
static pfn_spvc_compiler_create_compiler_options SDL_spvc_compiler_create_compiler_options = NULL;
static pfn_spvc_compiler_options_set_uint SDL_spvc_compiler_options_set_uint = NULL;
static pfn_spvc_compiler_install_compiler_options SDL_spvc_compiler_install_compiler_options = NULL;
static pfn_spvc_compiler_compile SDL_spvc_compiler_compile = NULL;
static pfn_spvc_context_get_last_error_string SDL_spvc_context_get_last_error_string = NULL;
static pfn_spvc_compiler_get_execution_model SDL_spvc_compiler_get_execution_model = NULL;
static pfn_spvc_compiler_get_cleansed_entry_point_name SDL_spvc_compiler_get_cleansed_entry_point_name = NULL;

static int SDL_TranslateShaderFromSPIRV(
    SDL_GpuDevice* device,
    const Uint8* code,
    size_t codesize,
    const char *original_entrypoint,
    SDL_GpuShaderFormat *out_shader_format,
    const char **out_cleansed_entrypoint,
    const char **out_translated_source,
    spvc_context *out_context)
{
    SDL_GpuShader *shader;
    spvc_result result;
    spvc_backend backend;
    spvc_context context = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_compiler_options options = NULL;

    switch (SDL_GpuGetBackend(device)) {
    case SDL_GPU_BACKEND_D3D11:
        backend = SPVC_BACKEND_HLSL;
        *out_shader_format = SDL_GPU_SHADERFORMAT_HLSL;
        break;
    case SDL_GPU_BACKEND_METAL:
        backend = SPVC_BACKEND_MSL;
        *out_shader_format = SDL_GPU_SHADERFORMAT_MSL;
        break;
    default:
        SDL_SetError("SDL_CreateShaderFromSPIRV: Unexpected SDL_GpuBackend");
        return -1;
    }

    /* FIXME: spirv-cross could probably be loaded in a better spot */
    if (spirvcross_dll == NULL) {
        spirvcross_dll = SDL_LoadObject(SPIRV_CROSS_DLL);
        if (spirvcross_dll == NULL) {
            return -1;
        }
    }

#define CHECK_FUNC(func)                                                  \
    if (SDL_##func == NULL) {                                             \
        SDL_##func = (pfn_##func)SDL_LoadFunction(spirvcross_dll, #func); \
        if (SDL_##func == NULL) {                                         \
            return -1;                                                    \
        }                                                                 \
    }
    CHECK_FUNC(spvc_context_create)
    CHECK_FUNC(spvc_context_destroy)
    CHECK_FUNC(spvc_context_parse_spirv)
    CHECK_FUNC(spvc_context_create_compiler)
    CHECK_FUNC(spvc_compiler_create_compiler_options)
    CHECK_FUNC(spvc_compiler_options_set_uint)
    CHECK_FUNC(spvc_compiler_install_compiler_options)
    CHECK_FUNC(spvc_compiler_compile)
    CHECK_FUNC(spvc_context_get_last_error_string)
    CHECK_FUNC(spvc_compiler_get_execution_model)
    CHECK_FUNC(spvc_compiler_get_cleansed_entry_point_name)
#undef CHECK_FUNC

    /* Create the SPIRV-Cross context */
    result = SDL_spvc_context_create(&context);
    if (result < 0) {
        SDL_SetError("spvc_context_create failed: %X", result);
        return -1;
    }

    /* Parse the SPIR-V into IR */
    result = SDL_spvc_context_parse_spirv(context, (const SpvId *)code, codesize / sizeof(SpvId), &ir);
    if (result < 0) {
        SPVC_ERROR(spvc_context_parse_spirv);
        SDL_spvc_context_destroy(context);
        return -1;
    }

    /* Create the cross-compiler */
    result = SDL_spvc_context_create_compiler(context, backend, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler);
    if (result < 0) {
        SPVC_ERROR(spvc_context_create_compiler);
        SDL_spvc_context_destroy(context);
        return -1;
    }

    /* Set up the cross-compiler options */
    result = SDL_spvc_compiler_create_compiler_options(compiler, &options);
    if (result < 0) {
        SPVC_ERROR(spvc_compiler_create_compiler_options);
        SDL_spvc_context_destroy(context);
        return -1;
    }

    if (backend == SPVC_BACKEND_HLSL) {
        SDL_spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, 50);
        SDL_spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_HLSL_NONWRITABLE_UAV_TEXTURE_AS_SRV, 1);
    }

    result = SDL_spvc_compiler_install_compiler_options(compiler, options);
    if (result < 0) {
        SPVC_ERROR(spvc_compiler_install_compiler_options);
        SDL_spvc_context_destroy(context);
        return -1;
    }

    /* Compile to the target shader language */
    result = SDL_spvc_compiler_compile(compiler, out_translated_source);
    if (result < 0) {
        SPVC_ERROR(spvc_compiler_compile);
        SDL_spvc_context_destroy(context);
        return -1;
    }

    /* Determine the "cleansed" entrypoint name (e.g. main -> main0 on MSL) */
    *out_cleansed_entrypoint = SDL_spvc_compiler_get_cleansed_entry_point_name(
        compiler,
        original_entrypoint,
        SDL_spvc_compiler_get_execution_model(compiler));

    *out_context = context;

    return 0;
}

SDL_GpuShader *SDL_CreateShaderFromSPIRV(SDL_GpuDevice *device, SDL_GpuShaderCreateInfo *createInfo)
{
    SDL_GpuShaderFormat shader_format;
    const char *cleansed_entrypoint;
    const char *translated_source;
    spvc_context context;
    SDL_GpuShaderCreateInfo newCreateInfo;
    SDL_GpuShader *shader;

    int result = SDL_TranslateShaderFromSPIRV(
        device,
        createInfo->code,
        createInfo->codeSize,
        createInfo->entryPointName,
        &shader_format,
        &cleansed_entrypoint,
        &translated_source,
        &context);
    if (result < 0) {
        return NULL;
    }

    /* Copy the original create info, but with the new source code */
    newCreateInfo = *createInfo;
    newCreateInfo.format = shader_format;
    newCreateInfo.code = translated_source;
    newCreateInfo.codeSize = SDL_strlen(translated_source) + 1;
    newCreateInfo.entryPointName = cleansed_entrypoint;

    /* Create the shader! */
    shader = SDL_GpuCreateShader(device, &newCreateInfo);

    /* Clean up */
    SDL_spvc_context_destroy(context);

    return shader;
}

SDL_GpuComputePipeline* SDL_CreateComputePipelineFromSPIRV(
    SDL_GpuDevice* device,
    SDL_GpuComputePipelineCreateInfo* createInfo)
{
    SDL_GpuShaderFormat shader_format;
    const char *cleansed_entrypoint;
    const char *translated_source;
    spvc_context context;
    SDL_GpuComputePipelineCreateInfo newCreateInfo;
    SDL_GpuComputePipeline *pipeline;

    int result = SDL_TranslateShaderFromSPIRV(
        device,
        createInfo->code,
        createInfo->codeSize,
        createInfo->entryPointName,
        &shader_format,
        &cleansed_entrypoint,
        &translated_source,
        &context);
    if (result < 0) {
        return NULL;
    }

    /* Copy the original create info, but with the new source code */
    newCreateInfo = *createInfo;
    newCreateInfo.format = shader_format;
    newCreateInfo.code = translated_source;
    newCreateInfo.codeSize = SDL_strlen(translated_source) + 1;
    newCreateInfo.entryPointName = cleansed_entrypoint;

    /* Create the pipeline! */
    pipeline = SDL_GpuCreateComputePipeline(device, &newCreateInfo);

    /* Clean up */
    SDL_spvc_context_destroy(context);

    return pipeline;
}
