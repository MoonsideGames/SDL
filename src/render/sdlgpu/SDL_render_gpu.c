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
#include "../../video/SDL_pixels_c.h"
#include "../SDL_d3dmath.h"
#include "../SDL_sysrender.h"
#include "SDL_gpu_util.h"
#include "SDL_pipeline_gpu.h"
#include "SDL_shaders_gpu.h"

typedef struct GPU_ShaderUniformData
{
    Float4X4 mvp;
    SDL_FColor color;
    float texture_size[2];
} GPU_ShaderUniformData;

typedef struct GPU_RenderData
{
    SDL_GpuDevice *device;
    GPU_Shaders shaders;
    GPU_PipelineCache pipeline_cache;
    SDL_GpuFence *present_fence;

    struct
    {
        SDL_GpuTexture *texture;
        SDL_GpuTextureFormat format;
        Uint32 width;
        Uint32 height;
    } backbuffer;

    struct
    {
        SDL_GpuSwapchainComposition composition;
        SDL_GpuPresentMode present_mode;
    } swapchain;

    struct
    {
        SDL_GpuTransferBuffer *transfer_buf;
        SDL_GpuBuffer *buffer;
        Uint32 buffer_size;
    } vertices;

    struct
    {
        SDL_GpuRenderPass *render_pass;
        SDL_Texture *render_target;
        SDL_GpuCommandBuffer *command_buffer;
        SDL_GpuColorAttachmentInfo color_attachment;
        SDL_GpuViewport viewport;
        SDL_Rect scissor;
        SDL_FColor draw_color;
        SDL_bool scissor_enabled;
        GPU_ShaderUniformData shader_data;
    } state;

    SDL_GpuSampler *samplers[3][2];
} GPU_RenderData;

typedef struct GPU_TextureData
{
    SDL_GpuTexture *texture;
    SDL_GpuTextureFormat format;
    GPU_FragmentShaderID shader;
    void *pixels;
    int pitch;
    SDL_Rect locked_rect;
} GPU_TextureData;

static SDL_bool GPU_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    SDL_BlendFactor srcColorFactor = SDL_GetBlendModeSrcColorFactor(blendMode);
    SDL_BlendFactor srcAlphaFactor = SDL_GetBlendModeSrcAlphaFactor(blendMode);
    SDL_BlendOperation colorOperation = SDL_GetBlendModeColorOperation(blendMode);
    SDL_BlendFactor dstColorFactor = SDL_GetBlendModeDstColorFactor(blendMode);
    SDL_BlendFactor dstAlphaFactor = SDL_GetBlendModeDstAlphaFactor(blendMode);
    SDL_BlendOperation alphaOperation = SDL_GetBlendModeAlphaOperation(blendMode);

    if (GPU_ConvertBlendFactor(srcColorFactor) == SDL_GPU_BLENDFACTOR_INVALID ||
        GPU_ConvertBlendFactor(srcAlphaFactor) == SDL_GPU_BLENDFACTOR_INVALID ||
        GPU_ConvertBlendOperation(colorOperation) == SDL_GPU_BLENDOP_INVALID ||
        GPU_ConvertBlendFactor(dstColorFactor) == SDL_GPU_BLENDFACTOR_INVALID ||
        GPU_ConvertBlendFactor(dstAlphaFactor) == SDL_GPU_BLENDFACTOR_INVALID ||
        GPU_ConvertBlendOperation(alphaOperation) == SDL_GPU_BLENDOP_INVALID) {
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

static SDL_GpuTextureFormat PixFormatToTexFormat(SDL_PixelFormat pixel_format)
{
    switch (pixel_format) {
    case SDL_PIXELFORMAT_BGRA32:
    case SDL_PIXELFORMAT_BGRX32:
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8;
    case SDL_PIXELFORMAT_RGBA32:
    case SDL_PIXELFORMAT_RGBX32:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8;

    // YUV TODO
    case SDL_PIXELFORMAT_YV12:
    case SDL_PIXELFORMAT_IYUV:
    case SDL_PIXELFORMAT_NV12:
    case SDL_PIXELFORMAT_NV21:
    case SDL_PIXELFORMAT_UYVY:
    default:
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

static SDL_PixelFormat TexFormatToPixFormat(SDL_GpuTextureFormat tex_format)
{
    switch (tex_format) {
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8:
        return SDL_PIXELFORMAT_RGBA32;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8:
        return SDL_PIXELFORMAT_BGRA32;
    case SDL_GPU_TEXTUREFORMAT_B5G6R5:
        return SDL_PIXELFORMAT_BGR565;
    case SDL_GPU_TEXTUREFORMAT_B5G5R5A1:
        return SDL_PIXELFORMAT_BGRA5551;
    case SDL_GPU_TEXTUREFORMAT_B4G4R4A4:
        return SDL_PIXELFORMAT_BGRA4444;
    case SDL_GPU_TEXTUREFORMAT_R10G10B10A2:
        return SDL_PIXELFORMAT_ABGR2101010;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16:
        return SDL_PIXELFORMAT_RGBA64;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SNORM:
        return SDL_PIXELFORMAT_RGBA32;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_SFLOAT:
        return SDL_PIXELFORMAT_RGBA64_FLOAT;
    case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_SFLOAT:
        return SDL_PIXELFORMAT_RGBA128_FLOAT;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UINT:
        return SDL_PIXELFORMAT_RGBA32;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT:
        return SDL_PIXELFORMAT_RGBA64;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SRGB:
        return SDL_PIXELFORMAT_RGBA32;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_SRGB:
        return SDL_PIXELFORMAT_BGRA32;
    default:
        return SDL_PIXELFORMAT_UNKNOWN;
    }
}

static int GPU_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture, SDL_PropertiesID create_props)
{
    GPU_RenderData *renderdata = (GPU_RenderData *)renderer->internal;
    GPU_TextureData *data;
    SDL_GpuTextureFormat format;
    SDL_GpuTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER_BIT;

    format = PixFormatToTexFormat(texture->format);

    if (format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        return SDL_SetError("Texture format %s not supported by SDL_Gpu",
                            SDL_GetPixelFormatName(texture->format));
    }

    data = (GPU_TextureData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }

    if (texture->access == SDL_TEXTUREACCESS_STREAMING) {
        size_t size;
        data->pitch = texture->w * SDL_BYTESPERPIXEL(texture->format);
        size = (size_t)texture->h * data->pitch;
        if (texture->format == SDL_PIXELFORMAT_YV12 ||
            texture->format == SDL_PIXELFORMAT_IYUV) {
            /* Need to add size for the U and V planes */
            size += 2 * ((texture->h + 1) / 2) * ((data->pitch + 1) / 2);
        }
        if (texture->format == SDL_PIXELFORMAT_NV12 ||
            texture->format == SDL_PIXELFORMAT_NV21) {
            /* Need to add size for the U/V plane */
            size += 2 * ((texture->h + 1) / 2) * ((data->pitch + 1) / 2);
        }
        data->pixels = SDL_calloc(1, size);
        if (!data->pixels) {
            SDL_free(data);
            return -1;
        }

        // TODO allocate a persistent transfer buffer
    }

    if (texture->access == SDL_TEXTUREACCESS_TARGET) {
        usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET_BIT;
    }

    texture->internal = data;
    SDL_GpuTextureCreateInfo tci = { 0 };
    tci.format = format;
    tci.layerCountOrDepth = 1;
    tci.levelCount = 1;
    tci.usageFlags = usage;
    tci.width = texture->w;
    tci.height = texture->h;
    tci.sampleCount = SDL_GPU_SAMPLECOUNT_1;

    data->format = format;
    data->texture = SDL_GpuCreateTexture(renderdata->device, &tci);

    if (!data->texture) {
        return -1;
    }

    if (texture->format == SDL_PIXELFORMAT_RGBA32 || texture->format == SDL_PIXELFORMAT_BGRA32) {
        data->shader = FRAG_SHADER_TEXTURE_RGBA;
    } else {
        data->shader = FRAG_SHADER_TEXTURE_RGB;
    }

    return 0;
}

static int GPU_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                             const SDL_Rect *rect, const void *pixels, int pitch)
{
    GPU_RenderData *renderdata = (GPU_RenderData *)renderer->internal;
    GPU_TextureData *data = (GPU_TextureData *)texture->internal;
    const Uint32 texturebpp = SDL_BYTESPERPIXEL(texture->format);

    Uint32 row_size = texturebpp * rect->w;
    Uint32 data_size = row_size * rect->h;

    SDL_GpuTransferBufferCreateInfo tbci = { 0 };
    tbci.sizeInBytes = data_size;
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

    SDL_GpuTransferBuffer *tbuf = SDL_GpuCreateTransferBuffer(renderdata->device, &tbci);

    if (tbuf == NULL) {
        return -1;
    }

    Uint8 *output = SDL_GpuMapTransferBuffer(renderdata->device, tbuf, SDL_FALSE);

    if (pitch == row_size) {
        memcpy(output, pixels, data_size);
    } else {
        // FIXME is negative pitch supposed to work?
        // If not, maybe use SDL_GpuTextureTransferInfo::imagePitch instead of this
        const Uint8 *input = pixels;

        for (int i = 0; i < rect->h; ++i) {
            memcpy(output, input, row_size);
            output += row_size;
            input += pitch;
        }
    }

    SDL_GpuUnmapTransferBuffer(renderdata->device, tbuf);

    SDL_GpuCommandBuffer *cbuf = renderdata->state.command_buffer;
    SDL_GpuCopyPass *cpass = SDL_GpuBeginCopyPass(cbuf);

    SDL_GpuTextureTransferInfo tex_src = { 0 };
    tex_src.transferBuffer = tbuf;
    tex_src.imageHeight = rect->h;
    tex_src.imagePitch = rect->w;

    SDL_GpuTextureRegion tex_dst = { 0 };
    tex_dst.texture = data->texture;
    tex_dst.x = rect->x;
    tex_dst.y = rect->y;
    tex_dst.w = rect->w;
    tex_dst.h = rect->h;
    tex_dst.d = 1;

    SDL_GpuUploadToTexture(cpass, &tex_src, &tex_dst, SDL_TRUE);
    SDL_GpuEndCopyPass(cpass);
    SDL_GpuReleaseTransferBuffer(renderdata->device, tbuf);

    return 0;
}

static int GPU_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                           const SDL_Rect *rect, void **pixels, int *pitch)
{
    GPU_TextureData *data = (GPU_TextureData *)texture->internal;

    data->locked_rect = *rect;
    *pixels =
        (void *)((Uint8 *)data->pixels + rect->y * data->pitch +
                 rect->x * SDL_BYTESPERPIXEL(texture->format));
    *pitch = data->pitch;
    return 0;
}

static void GPU_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    GPU_TextureData *data = (GPU_TextureData *)texture->internal;
    const SDL_Rect *rect;
    void *pixels;

    rect = &data->locked_rect;
    pixels =
        (void *)((Uint8 *)data->pixels + rect->y * data->pitch +
                 rect->x * SDL_BYTESPERPIXEL(texture->format));
    GPU_UpdateTexture(renderer, texture, rect, pixels, data->pitch);
}

static void GPU_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scale_mode)
{
    /* nothing to do in this backend. */
}

static int GPU_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    data->state.render_target = texture;

    return 0;
}

static int GPU_QueueNoOp(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    return 0; /* nothing to do in this backend. */
}

static SDL_FColor GetDrawCmdColor(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    SDL_FColor color = cmd->data.color.color;

    if (SDL_RenderingLinearSpace(renderer)) {
        SDL_ConvertToLinear(&color);
    }

    color.r *= cmd->data.color.color_scale;
    color.g *= cmd->data.color.color_scale;
    color.b *= cmd->data.color.color_scale;

    return color;
}

static int GPU_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    float *verts = (float *)SDL_AllocateRenderVertices(renderer, count * 2 * sizeof(float), 0, &cmd->data.draw.first);

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    for (int i = 0; i < count; i++) {
        *(verts++) = 0.5f + points[i].x;
        *(verts++) = 0.5f + points[i].y;
    }

    return 0;
}

static int GPU_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                             const float *xy, int xy_stride, const SDL_FColor *color, int color_stride, const float *uv, int uv_stride,
                             int num_vertices, const void *indices, int num_indices, int size_indices,
                             float scale_x, float scale_y)
{
    int i;
    int count = indices ? num_indices : num_vertices;
    float *verts;
    size_t sz = 2 * sizeof(float) + 4 * sizeof(float) + (texture ? 2 : 0) * sizeof(float);
    const float color_scale = cmd->data.draw.color_scale;
    SDL_bool convert_color = SDL_RenderingLinearSpace(renderer);

    verts = (float *)SDL_AllocateRenderVertices(renderer, count * sz, 0, &cmd->data.draw.first);
    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    for (i = 0; i < count; i++) {
        int j;
        float *xy_;
        SDL_FColor col_;
        if (size_indices == 4) {
            j = ((const Uint32 *)indices)[i];
        } else if (size_indices == 2) {
            j = ((const Uint16 *)indices)[i];
        } else if (size_indices == 1) {
            j = ((const Uint8 *)indices)[i];
        } else {
            j = i;
        }

        xy_ = (float *)((char *)xy + j * xy_stride);

        *(verts++) = xy_[0] * scale_x;
        *(verts++) = xy_[1] * scale_y;

        col_ = *(SDL_FColor *)((char *)color + j * color_stride);
        if (convert_color) {
            SDL_ConvertToLinear(&col_);
        }

        // FIXME: The Vulkan backend doesn't multiply by color_scale. GL does. I'm not sure which one is wrong.
        *(verts++) = col_.r * color_scale;
        *(verts++) = col_.g * color_scale;
        *(verts++) = col_.b * color_scale;
        *(verts++) = col_.a;

        if (texture) {
            float *uv_ = (float *)((char *)uv + j * uv_stride);
            *(verts++) = uv_[0] * texture->w;
            *(verts++) = uv_[1] * texture->h;
        }
    }
    return 0;
}

static void GPU_InvalidateCachedState(SDL_Renderer *renderer)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    data->state.render_target = NULL;
    data->state.scissor_enabled = SDL_FALSE;
}

static SDL_GpuRenderPass *RestartRenderPass(GPU_RenderData *data)
{
    if (data->state.render_pass) {
        SDL_GpuEndRenderPass(data->state.render_pass);
    }

    data->state.render_pass = SDL_GpuBeginRenderPass(
        data->state.command_buffer, &data->state.color_attachment, 1, NULL);

    if (data->state.viewport.w > 0 && data->state.viewport.h > 0) {
        SDL_GpuSetViewport(data->state.render_pass, &data->state.viewport);
    }

    if (data->state.scissor_enabled) {
        SDL_GpuSetScissor(data->state.render_pass, &data->state.scissor);
    }

    data->state.color_attachment.loadOp = SDL_GPU_LOADOP_LOAD;

    return data->state.render_pass;
}

static void PushUniforms(GPU_RenderData *data, SDL_RenderCommand *cmd)
{
    GPU_ShaderUniformData uniforms = { 0 };
    uniforms.mvp.m[0][0] = 2.0f / data->state.viewport.w;
    uniforms.mvp.m[1][1] = -2.0f / data->state.viewport.h;
    uniforms.mvp.m[2][2] = 1.0f;
    uniforms.mvp.m[3][0] = -1.0f;
    uniforms.mvp.m[3][1] = 1.0f;
    uniforms.mvp.m[3][3] = 1.0f;

    uniforms.color = data->state.draw_color;

    if (cmd->data.draw.texture) {
        uniforms.texture_size[0] = cmd->data.draw.texture->w;
        uniforms.texture_size[1] = cmd->data.draw.texture->h;
    }

    SDL_GpuPushVertexUniformData(data->state.command_buffer, 0, &uniforms, sizeof(uniforms));
}

static SDL_GpuSampler **SamplerPointer(
    GPU_RenderData *data, SDL_TextureAddressMode address_mode, SDL_ScaleMode scale_mode)
{
    return &data->samplers[scale_mode][address_mode - 1];
}

static void Draw(
    GPU_RenderData *data, SDL_RenderCommand *cmd,
    Uint32 num_verts,
    Uint32 offset,
    SDL_GpuPrimitiveType prim)
{
    if (!data->state.render_pass) {
        RestartRenderPass(data);
    }

    GPU_VertexShaderID v_shader;
    GPU_FragmentShaderID f_shader;
    SDL_GpuRenderPass *pass = data->state.render_pass;
    GPU_TextureData *tdata = NULL;

    if (cmd->data.draw.texture) {
        tdata = (GPU_TextureData *)cmd->data.draw.texture->internal;
    }

    if (prim == SDL_GPU_PRIMITIVETYPE_TRIANGLELIST) {
        if (cmd->data.draw.texture) {
            v_shader = VERT_SHADER_TRI_TEXTURE;
            f_shader = tdata->shader;
        } else {
            v_shader = VERT_SHADER_TRI_COLOR;
            f_shader = FRAG_SHADER_COLOR;
        }
    } else {
        v_shader = VERT_SHADER_LINEPOINT;
        f_shader = FRAG_SHADER_COLOR;
    }

    GPU_PipelineParameters pipe_params = { 0 };
    pipe_params.blend_mode = cmd->data.draw.blend;
    pipe_params.vert_shader = v_shader;
    pipe_params.frag_shader = f_shader;
    pipe_params.primitive_type = prim;

    if (data->state.render_target) {
        pipe_params.attachment_format = ((GPU_TextureData *)data->state.render_target->internal)->format;
    } else {
        pipe_params.attachment_format = data->backbuffer.format;
    }

    SDL_GpuGraphicsPipeline *pipe = GPU_GetPipeline(&data->pipeline_cache, &data->shaders, data->device, &pipe_params);

    if (!pipe) {
        return;
    }

    SDL_GpuBindGraphicsPipeline(data->state.render_pass, pipe);

    if (tdata) {
        SDL_GpuTextureSamplerBinding sampler_bind = { 0 };
        sampler_bind.sampler = *SamplerPointer(data, cmd->data.draw.texture_address_mode, cmd->data.draw.texture->scaleMode);
        sampler_bind.texture = tdata->texture;
        SDL_GpuBindFragmentSamplers(pass, 0, &sampler_bind, 1);
    }

    SDL_GpuBufferBinding buffer_bind = { 0 };
    buffer_bind.buffer = data->vertices.buffer;
    buffer_bind.offset = offset;

    SDL_GpuBindVertexBuffers(pass, 0, &buffer_bind, 1);
    PushUniforms(data, cmd);
    SDL_GpuDrawPrimitives(data->state.render_pass, 0, num_verts);
}

static void ReleaseVertexBuffer(GPU_RenderData *data)
{
    if (data->vertices.buffer) {
        SDL_GpuReleaseBuffer(data->device, data->vertices.buffer);
    }

    if (data->vertices.transfer_buf) {
        SDL_GpuReleaseTransferBuffer(data->device, data->vertices.transfer_buf);
    }

    data->vertices.buffer_size = 0;
}

static int InitVertexBuffer(GPU_RenderData *data, Uint32 size)
{
    SDL_GpuBufferCreateInfo bci = { 0 };
    bci.sizeInBytes = size;
    bci.usageFlags = SDL_GPU_BUFFERUSAGE_VERTEX_BIT;

    data->vertices.buffer = SDL_GpuCreateBuffer(data->device, &bci);

    if (!data->vertices.buffer) {
        return -1;
    }

    SDL_GpuTransferBufferCreateInfo tbci = { 0 };
    tbci.sizeInBytes = size;
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

    data->vertices.transfer_buf = SDL_GpuCreateTransferBuffer(data->device, &tbci);

    if (!data->vertices.transfer_buf) {
        return -1;
    }

    return 0;
}

static int UploadVertices(GPU_RenderData *data, void *vertices, size_t vertsize)
{
    if (vertsize == 0) {
        return 0;
    }

    if (vertsize > data->vertices.buffer_size) {
        ReleaseVertexBuffer(data);
        if (InitVertexBuffer(data, vertsize) != 0) {
            return -1;
        }
    }

    void *staging_buf = SDL_GpuMapTransferBuffer(data->device, data->vertices.transfer_buf, SDL_TRUE);
    memcpy(staging_buf, vertices, vertsize);
    SDL_GpuUnmapTransferBuffer(data->device, data->vertices.transfer_buf);

    SDL_GpuCopyPass *pass = SDL_GpuBeginCopyPass(data->state.command_buffer);

    if (!pass) {
        return -1;
    }

    SDL_GpuTransferBufferLocation src = { 0 };
    src.transferBuffer = data->vertices.transfer_buf;

    SDL_GpuBufferRegion dst = { 0 };
    dst.buffer = data->vertices.buffer;
    dst.size = (Uint32)vertsize;

    SDL_GpuUploadToBuffer(pass, &src, &dst, SDL_TRUE);
    SDL_GpuEndCopyPass(pass);

    return 0;
}

static int GPU_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    if (UploadVertices(data, vertices, vertsize) != 0) {
        return -1;
    }

    data->state.color_attachment.loadOp = SDL_GPU_LOADOP_LOAD;

    if (renderer->target) {
        GPU_TextureData *tdata = renderer->target->internal;
        data->state.color_attachment.texture = tdata->texture;
    } else {
        data->state.color_attachment.texture = data->backbuffer.texture;
    }

    if (!data->state.color_attachment.texture) {
        // FIXME is this an error? Happens if the swapchain texture couldn't be acquired
        return SDL_SetError("Render target texture is NULL");
    }

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETDRAWCOLOR:
        {
            data->state.draw_color = GetDrawCmdColor(renderer, cmd);
            break;
        }

        case SDL_RENDERCMD_SETVIEWPORT:
        {
            SDL_Rect *viewport = &cmd->data.viewport.rect;
            data->state.viewport.x = viewport->x;
            data->state.viewport.y = viewport->y;
            data->state.viewport.w = viewport->w;
            data->state.viewport.h = viewport->h;
            data->state.viewport.minDepth = 0;
            data->state.viewport.maxDepth = 1;

            if (data->state.render_pass && viewport->w > 0 && viewport->h > 0) {
                SDL_GpuSetViewport(data->state.render_pass, &data->state.viewport);
            }

            break;
        }

        case SDL_RENDERCMD_SETCLIPRECT:
        {
            const SDL_Rect *rect = &cmd->data.cliprect.rect;
            data->state.scissor.x = (int)data->state.viewport.x + rect->x;
            data->state.scissor.y = (int)data->state.viewport.y + rect->y;
            data->state.scissor.w = rect->w;
            data->state.scissor.h = rect->h;
            data->state.scissor_enabled = cmd->data.cliprect.enabled;

            if (data->state.render_pass && cmd->data.cliprect.enabled) {
                SDL_GpuSetScissor(data->state.render_pass, &data->state.scissor);
            }

            break;
        }

        case SDL_RENDERCMD_CLEAR:
        {
            data->state.color_attachment.clearColor = GetDrawCmdColor(renderer, cmd);
            data->state.color_attachment.loadOp = SDL_GPU_LOADOP_CLEAR;

            if (data->state.render_pass) {
                RestartRenderPass(data);
            }

            break;
        }

        case SDL_RENDERCMD_FILL_RECTS: /* unused */
            break;

        case SDL_RENDERCMD_COPY: /* unused */
            break;

        case SDL_RENDERCMD_COPY_EX: /* unused */
            break;

        case SDL_RENDERCMD_DRAW_LINES:
        {
            Uint32 count = (Uint32)cmd->data.draw.count;
            Uint32 offset = (Uint32)cmd->data.draw.first;

            if (count > 2) {
                /* joined lines cannot be grouped */
                Draw(data, cmd, count, offset, SDL_GPU_PRIMITIVETYPE_LINESTRIP);
            } else {
                /* let's group non joined lines */
                SDL_RenderCommand *finalcmd = cmd;
                SDL_RenderCommand *nextcmd = cmd->next;
                SDL_BlendMode thisblend = cmd->data.draw.blend;

                while (nextcmd) {
                    const SDL_RenderCommandType nextcmdtype = nextcmd->command;
                    if (nextcmdtype != SDL_RENDERCMD_DRAW_LINES) {
                        break; /* can't go any further on this draw call, different render command up next. */
                    } else if (nextcmd->data.draw.count != 2) {
                        break; /* can't go any further on this draw call, those are joined lines */
                    } else if (nextcmd->data.draw.blend != thisblend) {
                        break; /* can't go any further on this draw call, different blendmode copy up next. */
                    } else {
                        finalcmd = nextcmd; /* we can combine copy operations here. Mark this one as the furthest okay command. */
                        count += (Uint32)nextcmd->data.draw.count;
                    }
                    nextcmd = nextcmd->next;
                }

                Draw(data, cmd, count, offset, SDL_GPU_PRIMITIVETYPE_LINELIST);
                cmd = finalcmd; /* skip any copy commands we just combined in here. */
            }
            break;
        }

        case SDL_RENDERCMD_DRAW_POINTS:
        case SDL_RENDERCMD_GEOMETRY:
        {
            /* as long as we have the same copy command in a row, with the
               same texture, we can combine them all into a single draw call. */
            SDL_Texture *thistexture = cmd->data.draw.texture;
            SDL_BlendMode thisblend = cmd->data.draw.blend;
            const SDL_RenderCommandType thiscmdtype = cmd->command;
            SDL_RenderCommand *finalcmd = cmd;
            SDL_RenderCommand *nextcmd = cmd->next;
            Uint32 count = (Uint32)cmd->data.draw.count;
            Uint32 offset = (Uint32)cmd->data.draw.first;

            while (nextcmd) {
                const SDL_RenderCommandType nextcmdtype = nextcmd->command;
                if (nextcmdtype != thiscmdtype) {
                    break; /* can't go any further on this draw call, different render command up next. */
                } else if (nextcmd->data.draw.texture != thistexture || nextcmd->data.draw.blend != thisblend) {
                    // FIXME should we check address mode too?
                    break; /* can't go any further on this draw call, different texture/blendmode copy up next. */
                } else {
                    finalcmd = nextcmd; /* we can combine copy operations here. Mark this one as the furthest okay command. */
                    count += (Uint32)nextcmd->data.draw.count;
                }
                nextcmd = nextcmd->next;
            }

            SDL_GpuPrimitiveType prim = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; /* SDL_RENDERCMD_GEOMETRY */
            if (thiscmdtype == SDL_RENDERCMD_DRAW_POINTS) {
                prim = SDL_GPU_PRIMITIVETYPE_POINTLIST;
            }

            Draw(data, cmd, count, offset, prim);

            cmd = finalcmd; /* skip any copy commands we just combined in here. */
            break;
        }

        case SDL_RENDERCMD_NO_OP:
            break;
        }

        cmd = cmd->next;
    }

    if (data->state.color_attachment.loadOp && !data->state.render_pass) {
        RestartRenderPass(data);
    }

    if (data->state.render_pass) {
        SDL_GpuEndRenderPass(data->state.render_pass);
        data->state.render_pass = NULL;
    }

    return 0;
}

static SDL_Surface *GPU_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;
    SDL_GpuTexture *gpu_tex;
    SDL_PixelFormat pixfmt;

    if (data->state.render_target) {
        SDL_Texture *texture = data->state.render_target;
        GPU_TextureData *texdata = texture->internal;
        gpu_tex = texdata->texture;
        pixfmt = texture->format;
    } else {
        gpu_tex = data->backbuffer.texture;
        pixfmt = TexFormatToPixFormat(data->backbuffer.format);

        if (pixfmt == SDL_PIXELFORMAT_UNKNOWN) {
            SDL_SetError("Unsupported backbuffer format");
            return NULL;
        }
    }

    Uint32 bpp = SDL_BYTESPERPIXEL(pixfmt);
    Uint32 row_size = rect->w * bpp;
    Uint32 image_size = row_size * rect->h;

    SDL_Surface *surface = SDL_CreateSurface(rect->w, rect->h, pixfmt);

    if (!surface) {
        return NULL;
    }

    SDL_GpuTransferBufferCreateInfo tbci = { 0 };
    tbci.sizeInBytes = image_size;
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;

    SDL_GpuTransferBuffer *tbuf = SDL_GpuCreateTransferBuffer(data->device, &tbci);

    if (!tbuf) {
        return NULL;
    }

    SDL_GpuCopyPass *pass = SDL_GpuBeginCopyPass(data->state.command_buffer);

    SDL_GpuTextureRegion src = { 0 };
    src.texture = gpu_tex;
    src.x = rect->x;
    src.y = rect->y;
    src.w = rect->w;
    src.h = rect->h;
    src.d = 1;

    SDL_GpuTextureTransferInfo dst = { 0 };
    dst.transferBuffer = tbuf;
    dst.imageHeight = rect->h;
    dst.imagePitch = rect->w;

    SDL_GpuDownloadFromTexture(pass, &src, &dst);
    SDL_GpuEndCopyPass(pass);

    SDL_GpuFence *fence = SDL_GpuSubmitAndAcquireFence(data->state.command_buffer);
    SDL_GpuWaitForFences(data->device, SDL_TRUE, &fence, 1);
    SDL_GpuReleaseFence(data->device, fence);
    data->state.command_buffer = SDL_GpuAcquireCommandBuffer(data->device);

    void *mapped_tbuf = SDL_GpuMapTransferBuffer(data->device, tbuf, SDL_FALSE);

    if (surface->pitch == row_size) {
        memcpy(surface->pixels, mapped_tbuf, image_size);
    } else {
        Uint8 *input = mapped_tbuf;
        Uint8 *output = surface->pixels;

        for (Uint32 row = 0; row < rect->h; ++row) {
            memcpy(output, input, row_size);
            output += surface->pitch;
            input += row_size;
        }
    }

    SDL_GpuUnmapTransferBuffer(data->device, tbuf);
    SDL_GpuReleaseTransferBuffer(data->device, tbuf);

    return surface;
}

static int CreateBackbuffer(GPU_RenderData *data, Uint32 w, Uint32 h, SDL_GpuTextureFormat fmt)
{
    SDL_GpuTextureCreateInfo tci = { 0 };
    tci.width = w;
    tci.height = h;
    tci.format = fmt;
    tci.layerCountOrDepth = 1;
    tci.levelCount = 1;
    tci.sampleCount = SDL_GPU_SAMPLECOUNT_1;
    tci.usageFlags = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET_BIT | SDL_GPU_TEXTUREUSAGE_SAMPLER_BIT;

    data->backbuffer.texture = SDL_GpuCreateTexture(data->device, &tci);
    data->backbuffer.width = w;
    data->backbuffer.height = h;
    data->backbuffer.format = fmt;

    return data->backbuffer.texture ? 0 : -1;
}

static int GPU_RenderPresent(SDL_Renderer *renderer)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    Uint32 swapchain_w, swapchain_h;

    SDL_GpuTexture *swapchain = SDL_GpuAcquireSwapchainTexture(data->state.command_buffer, renderer->window, &swapchain_w, &swapchain_h);

    if (swapchain == NULL) {
        goto submit;
    }

    SDL_GpuTextureFormat swapchain_fmt = SDL_GpuGetSwapchainTextureFormat(data->device, renderer->window);

    if (swapchain_w != data->backbuffer.width || swapchain_h != data->backbuffer.height || swapchain_fmt != data->backbuffer.format) {
        SDL_GpuTextureRegion src = { 0 };
        src.texture = data->backbuffer.texture;
        src.w = data->backbuffer.width;
        src.h = data->backbuffer.height;
        src.d = 1;

        SDL_GpuTextureRegion dst = { 0 };
        dst.texture = swapchain;
        dst.w = swapchain_w;
        dst.h = swapchain_h;
        dst.d = 1;

        SDL_GpuBlit(data->state.command_buffer, &src, &dst, SDL_GPU_FILTER_LINEAR, SDL_TRUE);
        SDL_GpuReleaseTexture(data->device, data->backbuffer.texture);
        CreateBackbuffer(data, swapchain_w, swapchain_h, swapchain_fmt);
    } else {
        SDL_GpuTextureLocation src = { 0 };
        src.texture = data->backbuffer.texture;

        SDL_GpuTextureLocation dst = { 0 };
        dst.texture = swapchain;

        SDL_GpuCopyPass *pass = SDL_GpuBeginCopyPass(data->state.command_buffer);
        SDL_GpuCopyTextureToTexture(pass, &src, &dst, swapchain_w, swapchain_h, 1, SDL_TRUE);
        SDL_GpuEndCopyPass(pass);
    }

submit:
#if 1
    if (data->present_fence) {
        SDL_GpuWaitForFences(data->device, SDL_TRUE, &data->present_fence, 1);
        SDL_GpuReleaseFence(data->device, data->present_fence);
    }

    data->present_fence = SDL_GpuSubmitAndAcquireFence(data->state.command_buffer);
#else
    SDL_GpuSubmit(data->state.command_buffer);
#endif

    data->state.command_buffer = SDL_GpuAcquireCommandBuffer(data->device);

    return 0;
}

static void GPU_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    GPU_RenderData *renderdata = (GPU_RenderData *)renderer->internal;
    GPU_TextureData *data = (GPU_TextureData *)texture->internal;

    if (renderdata->state.render_target == texture) {
        renderdata->state.render_target = NULL;
    }

    if (!data) {
        return;
    }

    SDL_GpuReleaseTexture(renderdata->device, data->texture);
    SDL_free(data->pixels);
    SDL_free(data);
    texture->internal = NULL;
}

static void GPU_DestroyRenderer(SDL_Renderer *renderer)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    if (!data) {
        return;
    }

    if (data->present_fence) {
        SDL_GpuWaitForFences(data->device, SDL_TRUE, &data->present_fence, 1);
        SDL_GpuReleaseFence(data->device, data->present_fence);
    }

    if (data->state.command_buffer) {
        SDL_GpuSubmit(data->state.command_buffer);
        data->state.command_buffer = NULL;
    }

    for (Uint32 i = 0; i < sizeof(data->samplers) / sizeof(SDL_GpuSampler *); ++i) {
        SDL_GpuReleaseSampler(data->device, ((SDL_GpuSampler **)data->samplers)[i]);
    }

    if (data->backbuffer.texture) {
        SDL_GpuReleaseTexture(data->device, data->backbuffer.texture);
    }

    if (renderer->window) {
        SDL_GpuUnclaimWindow(data->device, renderer->window);
    }

    ReleaseVertexBuffer(data);
    GPU_DestroyPipelineCache(&data->pipeline_cache);
    GPU_ReleaseShaders(&data->shaders, data->device);
    SDL_GpuDestroyDevice(data->device);

    SDL_free(data);
}

static int ChoosePresentMode(SDL_GpuDevice *device, SDL_Window *window, const int vsync, SDL_GpuPresentMode *out_mode)
{
    SDL_GpuPresentMode mode;

    switch (vsync) {
    case 0:
        mode = SDL_GPU_PRESENTMODE_MAILBOX;

        if (!SDL_GpuSupportsPresentMode(device, window, mode)) {
            mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
        }

        if (!SDL_GpuSupportsPresentMode(device, window, mode)) {
            mode = SDL_GPU_PRESENTMODE_VSYNC;
        }

        // FIXME should we return an error if both mailbox and immediate fail?
        break;

    case 1:
        mode = SDL_GPU_PRESENTMODE_VSYNC;
        break;

    default:
        return SDL_Unsupported();
    }

    *out_mode = mode;
    return 0;
}

static int GPU_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;
    SDL_GpuPresentMode mode;

    if (ChoosePresentMode(data->device, renderer->window, vsync, &mode) != 0) {
        return -1;
    }

    if (mode != data->swapchain.present_mode) {
        // XXX returns bool instead of SDL-style error code
        if (SDL_GpuSetSwapchainParameters(data->device, renderer->window, data->swapchain.composition, mode)) {
            data->swapchain.present_mode = mode;
            return 0;
        } else {
            return -1;
        }
    }

    return 0;
}

static int InitSamplers(GPU_RenderData *data)
{
    struct
    {
        struct
        {
            SDL_TextureAddressMode address_mode;
            SDL_ScaleMode scale_mode;
        } sdl;
        struct
        {
            SDL_GpuSamplerAddressMode address_mode;
            SDL_GpuFilter filter;
            Uint32 anisotropy;
        } gpu;
    } configs[] = {
        {
            { SDL_TEXTURE_ADDRESS_CLAMP, SDL_SCALEMODE_NEAREST },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_NEAREST, 0 },
        },
        {
            { SDL_TEXTURE_ADDRESS_CLAMP, SDL_SCALEMODE_LINEAR },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_LINEAR, 0 },
        },
        {
            { SDL_TEXTURE_ADDRESS_CLAMP, SDL_SCALEMODE_BEST },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_LINEAR, 16 },
        },
        {
            { SDL_TEXTURE_ADDRESS_WRAP, SDL_SCALEMODE_NEAREST },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_NEAREST, 0 },
        },
        {
            { SDL_TEXTURE_ADDRESS_WRAP, SDL_SCALEMODE_LINEAR },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_LINEAR, 0 },
        },
        {
            { SDL_TEXTURE_ADDRESS_WRAP, SDL_SCALEMODE_BEST },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_LINEAR, 16 },
        },
    };

    for (Uint32 i = 0; i < SDL_arraysize(configs); ++i) {
        SDL_GpuSamplerCreateInfo sci = { 0 };
        sci.maxAnisotropy = configs[i].gpu.anisotropy;
        sci.anisotropyEnable = configs[i].gpu.anisotropy > 0;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = configs[i].gpu.address_mode;
        sci.minFilter = sci.magFilter = configs[i].gpu.filter;

        SDL_GpuSampler *sampler = SDL_GpuCreateSampler(data->device, &sci);

        if (sampler == NULL) {
            return -1;
        }

        *SamplerPointer(data, configs[i].sdl.address_mode, configs[i].sdl.scale_mode) = sampler;
    }

    return 0;
}

static int GPU_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, SDL_PropertiesID create_props)
{
    GPU_RenderData *data = NULL;

    data = (GPU_RenderData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }

    renderer->internal = data;

    SDL_SetupRendererColorspace(renderer, create_props);

    if (renderer->output_colorspace != SDL_COLORSPACE_SRGB) {
        // TODO
        SDL_SetError("Unsupported output colorspace");
        goto error;
    }

    SDL_SetBooleanProperty(create_props, SDL_PROP_GPU_CREATEDEVICE_DEBUGMODE_BOOL, SDL_TRUE);
    GPU_FillSupportedShaderFormats(create_props);
    data->device = SDL_GpuCreateDeviceWithProperties(create_props);

    if (!data->device) {
        goto error;
    }

    if (GPU_InitShaders(&data->shaders, data->device) != 0) {
        goto error;
    }

    if (GPU_InitPipelineCache(&data->pipeline_cache, data->device) != 0) {
        goto error;
    }

    // XXX what's a good initial size?
    if (InitVertexBuffer(data, 1 << 16) != 0) {
        goto error;
    }

    if (InitSamplers(data) != 0) {
        goto error;
    }

    renderer->SupportsBlendMode = GPU_SupportsBlendMode;
    renderer->CreateTexture = GPU_CreateTexture;
    renderer->UpdateTexture = GPU_UpdateTexture;
    renderer->LockTexture = GPU_LockTexture;
    renderer->UnlockTexture = GPU_UnlockTexture;
    renderer->SetTextureScaleMode = GPU_SetTextureScaleMode;
    renderer->SetRenderTarget = GPU_SetRenderTarget;
    renderer->QueueSetViewport = GPU_QueueNoOp;
    renderer->QueueSetDrawColor = GPU_QueueNoOp;
    renderer->QueueDrawPoints = GPU_QueueDrawPoints;
    renderer->QueueDrawLines = GPU_QueueDrawPoints; /* lines and points queue vertices the same way. */
    renderer->QueueGeometry = GPU_QueueGeometry;
    renderer->InvalidateCachedState = GPU_InvalidateCachedState;
    renderer->RunCommandQueue = GPU_RunCommandQueue;
    renderer->RenderReadPixels = GPU_RenderReadPixels;
    renderer->RenderPresent = GPU_RenderPresent;
    renderer->DestroyTexture = GPU_DestroyTexture;
    renderer->DestroyRenderer = GPU_DestroyRenderer;
    renderer->SetVSync = GPU_SetVSync;
    GPU_InvalidateCachedState(renderer);
    renderer->window = window;
    renderer->name = GPU_RenderDriver.name;

    data->swapchain.composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    data->swapchain.present_mode = SDL_GPU_PRESENTMODE_VSYNC;

    int vsync = (int)SDL_GetNumberProperty(create_props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 0);
    data->swapchain.present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    ChoosePresentMode(data->device, window, vsync, &data->swapchain.present_mode);

    if (!SDL_GpuClaimWindow(data->device, window, data->swapchain.composition, data->swapchain.present_mode)) {
        goto error;
    }

    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBA32);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGRA32);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBX32);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGRX32);

    renderer->rect_index_order[0] = 0;
    renderer->rect_index_order[1] = 1;
    renderer->rect_index_order[2] = 3;
    renderer->rect_index_order[3] = 1;
    renderer->rect_index_order[4] = 3;
    renderer->rect_index_order[5] = 2;

    data->state.draw_color.r = 1.0f;
    data->state.draw_color.g = 1.0f;
    data->state.draw_color.b = 1.0f;
    data->state.draw_color.a = 1.0f;

    data->state.command_buffer = SDL_GpuAcquireCommandBuffer(data->device);

    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);

    if (CreateBackbuffer(data, w, h, SDL_GpuGetSwapchainTextureFormat(data->device, window)) != 0) {
        goto error;
    }

    return 0;

error:
    GPU_DestroyRenderer(renderer);
    return -1;
}

SDL_RenderDriver GPU_RenderDriver = {
    GPU_CreateRenderer, "gpu"
};

#endif /* SDL_VIDEO_RENDER_GPU */