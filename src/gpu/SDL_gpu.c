﻿/*
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

#define NULL_RETURN(name) if (name == NULL) { return; }
#define NULL_RETURN_NULL(name) if (name == NULL) { return NULL; }

#define CHECK_COMMAND_BUFFER \
    if (commandBuffer == NULL) { return; } \
    if (((CommandBufferCommonHeader*) commandBuffer)->submitted) \
    { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Command buffer already submitted!"); \
        return; \
    }

#define CHECK_COMMAND_BUFFER_RETURN_NULL \
    if (commandBuffer == NULL) { return NULL; } \
    if (((CommandBufferCommonHeader*) commandBuffer)->submitted) \
    { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Command buffer already submitted!"); \
        return NULL; \
    }

#define CHECK_ANY_PASS_IN_PROGRESS \
    if ( \
        ((CommandBufferCommonHeader*) commandBuffer)->renderPass.inProgress || \
        ((CommandBufferCommonHeader*) commandBuffer)->computePass.inProgress || \
        ((CommandBufferCommonHeader*) commandBuffer)->copyPass.inProgress ) \
    { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Pass already in progress!"); \
        return NULL; \
    }

#define CHECK_RENDERPASS \
    if (!((Pass*) renderPass)->inProgress) { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Render pass not in progress!"); \
        return; \
    }

#define CHECK_COMPUTEPASS \
    if (!((Pass*) computePass)->inProgress) { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Compute pass not in progress!"); \
        return; \
    }

#define CHECK_COPYPASS \
    if (!((Pass*) copyPass)->inProgress) { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Copy pass not in progress!"); \
        return; \
    } \

#define COMMAND_BUFFER_DEVICE \
    ((CommandBufferCommonHeader*) commandBuffer)->device

#define RENDERPASS_COMMAND_BUFFER \
    ((Pass*) renderPass)->commandBuffer

#define RENDERPASS_DEVICE \
    ((CommandBufferCommonHeader*) RENDERPASS_COMMAND_BUFFER)->device

#define COMPUTEPASS_COMMAND_BUFFER \
    ((Pass*) computePass)->commandBuffer

#define COMPUTEPASS_DEVICE \
    ((CommandBufferCommonHeader*) COMPUTEPASS_COMMAND_BUFFER)->device

#define COPYPASS_COMMAND_BUFFER \
    ((Pass*) copyPass)->commandBuffer

#define COPYPASS_DEVICE \
    ((CommandBufferCommonHeader*) COPYPASS_COMMAND_BUFFER)->device

/* Drivers */

#if SDL_GPU_VULKAN
	#define SDL_GPU_VULKAN_DRIVER &VulkanDriver
#else
	#define SDL_GPU_VULKAN_DRIVER NULL
#endif

#if SDL_GPU_D3D11
	#define SDL_GPU_D3D11_DRIVER &D3D11Driver
#else
	#define SDL_GPU_D3D11_DRIVER NULL
#endif

#if SDL_GPU_METAL
	#define SDL_GPU_METAL_DRIVER &MetalDriver
#else
	#define SDL_GPU_METAL_DRIVER NULL
#endif

static const SDL_GpuDriver *backends[] = {
	SDL_GPU_VULKAN_DRIVER,
	SDL_GPU_D3D11_DRIVER,
	SDL_GPU_METAL_DRIVER,
	NULL
};

/* Driver Functions */

static SDL_GpuBackend SDL_GpuSelectBackend(SDL_VideoDevice *_this, SDL_GpuBackend preferredBackends)
{
	Uint32 i;

	/* Environment override... */
	const char *gpudriver = SDL_GetHint(SDL_HINT_GPU_BACKEND);
	if (gpudriver != NULL)
	{
		for (i = 0; backends[i]; i += 1)
		{
			if (backends[i] != NULL && SDL_strcasecmp(gpudriver, backends[i]->Name) == 0 && backends[i]->PrepareDriver(_this))
			{
				return backends[i]->backendflag;
			}
		}

		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_HINT_GPU_BACKEND %s unsupported!", gpudriver);
		return SDL_GPU_BACKEND_INVALID;
	}

	/* Preferred backends... */
	if (preferredBackends != SDL_GPU_BACKEND_INVALID)
	{
		for (i = 0; backends[i]; i += 1)
		{
			if (backends[i] != NULL && (preferredBackends & backends[i]->backendflag) && backends[i]->PrepareDriver(_this))
			{
				return backends[i]->backendflag;
			}
		}
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No preferred SDL_Gpu backend found!");
	}

	/* ... Fallback backends */
	for (i = 0; backends[i]; i += 1)
	{
		if (backends[i] != NULL && backends[i]->PrepareDriver(_this))
		{
			return backends[i]->backendflag;
		}
	}

	SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No supported SDL_Gpu backend found!");
	return SDL_GPU_BACKEND_INVALID;
}

SDL_GpuDevice* SDL_GpuCreateDevice(
	SDL_GpuBackend preferredBackends,
	SDL_bool debugMode
) {
	int i;
	SDL_GpuDevice *result = NULL;
	SDL_GpuBackend selectedBackend;
	SDL_VideoDevice *_this = SDL_GetVideoDevice();

	if (_this == NULL)
	{
		SDL_SetError("Video subsystem not initialized");
		return NULL;
	}

	selectedBackend = SDL_GpuSelectBackend(_this, preferredBackends);
	if (selectedBackend != SDL_GPU_BACKEND_INVALID)
	{
		for (i = 0; backends[i]; i += 1)
		{
			if (backends[i]->backendflag == selectedBackend)
			{
				result = backends[i]->CreateDevice(debugMode);
				if (result != NULL) {
					result->backend = backends[i]->backendflag;
					break;
				}
			}
		}
	}
	return result;
}

void SDL_GpuDestroyDevice(SDL_GpuDevice *device)
{
	NULL_RETURN(device);
	device->DestroyDevice(device);
}

SDL_GpuBackend SDL_GpuGetBackend(SDL_GpuDevice *device)
{
    if (device == NULL) {
        return SDL_GPU_BACKEND_INVALID;
    }
    return device->backend;
}

Uint32 SDL_GpuTextureFormatTexelBlockSize(
    SDL_GpuTextureFormat textureFormat
) {
    switch (textureFormat)
	{
		case SDL_GPU_TEXTUREFORMAT_BC1:
			return 8;
		case SDL_GPU_TEXTUREFORMAT_BC2:
		case SDL_GPU_TEXTUREFORMAT_BC3:
		case SDL_GPU_TEXTUREFORMAT_BC7:
		case SDL_GPU_TEXTUREFORMAT_BC3_SRGB:
		case SDL_GPU_TEXTUREFORMAT_BC7_SRGB:
			return 16;
		case SDL_GPU_TEXTUREFORMAT_R8:
        case SDL_GPU_TEXTUREFORMAT_A8:
		case SDL_GPU_TEXTUREFORMAT_R8_UINT:
			return 1;
		case SDL_GPU_TEXTUREFORMAT_R5G6B5:
		case SDL_GPU_TEXTUREFORMAT_B4G4R4A4:
		case SDL_GPU_TEXTUREFORMAT_A1R5G5B5:
		case SDL_GPU_TEXTUREFORMAT_R16_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R8G8_SNORM:
		case SDL_GPU_TEXTUREFORMAT_R8G8_UINT:
		case SDL_GPU_TEXTUREFORMAT_R16_UINT:
			return 2;
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8:
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8:
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SRGB:
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_SRGB:
		case SDL_GPU_TEXTUREFORMAT_R32_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R16G16_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SNORM:
		case SDL_GPU_TEXTUREFORMAT_A2R10G10B10:
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UINT:
		case SDL_GPU_TEXTUREFORMAT_R16G16_UINT:
			return 4;
		case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R16G16B16A16:
		case SDL_GPU_TEXTUREFORMAT_R32G32_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT:
			return 8;
		case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_SFLOAT:
			return 16;
		default:
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
				"Unrecognized TextureFormat!"
			);
			return 0;
	}
}

SDL_bool SDL_GpuIsTextureFormatSupported(
    SDL_GpuDevice *device,
    SDL_GpuTextureFormat format,
    SDL_GpuTextureType type,
    SDL_GpuTextureUsageFlags usage
) {
    if (device == NULL) { return SDL_FALSE; }
    return device->IsTextureFormatSupported(
        device->driverData,
        format,
        type,
        usage
    );
}

SDL_GpuSampleCount SDL_GpuGetBestSampleCount(
    SDL_GpuDevice* device,
    SDL_GpuTextureFormat format,
    SDL_GpuSampleCount desiredSampleCount
) {
    if (device == NULL) { return 0; }
    return device->GetBestSampleCount(
        device->driverData,
        format,
        desiredSampleCount
    );
}

/* State Creation */

SDL_GpuComputePipeline* SDL_GpuCreateComputePipeline(
	SDL_GpuDevice *device,
	SDL_GpuComputePipelineCreateInfo *computePipelineCreateInfo
) {
	NULL_RETURN_NULL(device)
	return device->CreateComputePipeline(
		device->driverData,
		computePipelineCreateInfo
	);
}

SDL_GpuGraphicsPipeline* SDL_GpuCreateGraphicsPipeline(
	SDL_GpuDevice *device,
	SDL_GpuGraphicsPipelineCreateInfo *graphicsPipelineCreateInfo
) {
	NULL_RETURN_NULL(device)
	return device->CreateGraphicsPipeline(
		device->driverData,
		graphicsPipelineCreateInfo
	);
}

SDL_GpuSampler* SDL_GpuCreateSampler(
	SDL_GpuDevice *device,
	SDL_GpuSamplerStateCreateInfo *samplerStateCreateInfo
) {
	NULL_RETURN_NULL(device)
	return device->CreateSampler(
		device->driverData,
		samplerStateCreateInfo
	);
}

SDL_GpuShader* SDL_GpuCreateShader(
    SDL_GpuDevice *device,
    SDL_GpuShaderCreateInfo *shaderCreateInfo
) {
    return device->CreateShader(
        device->driverData,
        shaderCreateInfo
    );
}

SDL_GpuTexture* SDL_GpuCreateTexture(
	SDL_GpuDevice *device,
	SDL_GpuTextureCreateInfo *textureCreateInfo
) {
	SDL_GpuTextureFormat newFormat;

	NULL_RETURN_NULL(device)

	/* Automatically swap out the depth format if it's unsupported.
	 * All backends have universal support for D16.
	 * Vulkan always supports at least one of { D24, D32 } and one of { D24_S8, D32_S8 }.
	 * D3D11 always supports all depth formats.
	 * Metal always supports D32 and D32_S8.
	 * So if D32/_S8 is not supported, we can safely fall back to D24/_S8, and vice versa.
	 */
	if (IsDepthFormat(textureCreateInfo->format))
	{
		if (!device->IsTextureFormatSupported(
			device->driverData,
			textureCreateInfo->format,
			SDL_GPU_TEXTURETYPE_2D, /* assuming that driver support for 2D implies support for Cube */
			textureCreateInfo->usageFlags)
		) {
			switch (textureCreateInfo->format)
			{
				case SDL_GPU_TEXTUREFORMAT_D24_UNORM:
					newFormat = SDL_GPU_TEXTUREFORMAT_D32_SFLOAT;
					break;
				case SDL_GPU_TEXTUREFORMAT_D32_SFLOAT:
					newFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
					break;
				case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
					newFormat = SDL_GPU_TEXTUREFORMAT_D32_SFLOAT_S8_UINT;
					break;
				case SDL_GPU_TEXTUREFORMAT_D32_SFLOAT_S8_UINT:
					newFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
					break;
				default:
					/* This should never happen, but just in case... */
					newFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
					break;
			}

			SDL_LogWarn(
				SDL_LOG_CATEGORY_APPLICATION,
				"Requested unsupported depth format %d, falling back to format %d!",
				textureCreateInfo->format,
				newFormat
			);
			textureCreateInfo->format = newFormat;
		}
	}

	return device->CreateTexture(
		device->driverData,
		textureCreateInfo
	);
}

SDL_GpuBuffer* SDL_GpuCreateGpuBuffer(
	SDL_GpuDevice *device,
	SDL_GpuBufferUsageFlags usageFlags,
	Uint32 sizeInBytes
) {
	NULL_RETURN_NULL(device)
	return device->CreateGpuBuffer(
		device->driverData,
		usageFlags,
		sizeInBytes
	);
}

SDL_GpuUniformBuffer* SDL_GpuCreateUniformBuffer(
	SDL_GpuDevice *device,
	Uint32 sizeInBytes
) {
	NULL_RETURN_NULL(device)
	return device->CreateUniformBuffer(
		device->driverData,
		sizeInBytes
	);
}

SDL_GpuTransferBuffer* SDL_GpuCreateTransferBuffer(
	SDL_GpuDevice *device,
	SDL_GpuTransferUsage usage,
	Uint32 sizeInBytes
) {
	NULL_RETURN_NULL(device)
	return device->CreateTransferBuffer(
		device->driverData,
		usage,
		sizeInBytes
	);
}

SDL_GpuOcclusionQuery* SDL_GpuCreateOcclusionQuery(
    SDL_GpuDevice *device
) {
    NULL_RETURN_NULL(device)
    return device->CreateOcclusionQuery(
        device->driverData
    );
}

/* Debug Naming */

void SDL_GpuSetGpuBufferName(
	SDL_GpuDevice *device,
	SDL_GpuBuffer *buffer,
	const char *text
) {
	NULL_RETURN(device)
	NULL_RETURN(buffer)

	device->SetGpuBufferName(
		device->driverData,
		buffer,
		text
	);
}

void SDL_GpuSetTextureName(
	SDL_GpuDevice *device,
	SDL_GpuTexture *texture,
	const char *text
) {
	NULL_RETURN(device)
	NULL_RETURN(texture)

	device->SetTextureName(
		device->driverData,
		texture,
		text
	);
}

void SDL_GpuSetStringMarker(
    SDL_GpuCommandBuffer *commandBuffer,
    const char *text
) {
    CHECK_COMMAND_BUFFER
    COMMAND_BUFFER_DEVICE->SetStringMarker(
        commandBuffer,
        text
    );
}

/* Disposal */

void SDL_GpuQueueDestroyTexture(
	SDL_GpuDevice *device,
	SDL_GpuTexture *texture
) {
	NULL_RETURN(device);
	device->QueueDestroyTexture(
		device->driverData,
		texture
	);
}

void SDL_GpuQueueDestroySampler(
	SDL_GpuDevice *device,
	SDL_GpuSampler *sampler
) {
	NULL_RETURN(device);
	device->QueueDestroySampler(
		device->driverData,
		sampler
	);
}

void SDL_GpuQueueDestroyGpuBuffer(
	SDL_GpuDevice *device,
	SDL_GpuBuffer *gpuBuffer
) {
	NULL_RETURN(device);
	device->QueueDestroyGpuBuffer(
		device->driverData,
		gpuBuffer
	);
}

void SDL_GpuQueueDestroyUniformBuffer(
	SDL_GpuDevice *device,
	SDL_GpuUniformBuffer *uniformBuffer
) {
	NULL_RETURN(device);
	device->QueueDestroyUniformBuffer(
		device->driverData,
		uniformBuffer
	);
}

void SDL_GpuQueueDestroyTransferBuffer(
	SDL_GpuDevice *device,
	SDL_GpuTransferBuffer *transferBuffer
) {
	NULL_RETURN(device);
	device->QueueDestroyTransferBuffer(
		device->driverData,
		transferBuffer
	);
}

void SDL_GpuQueueDestroyShader(
	SDL_GpuDevice *device,
	SDL_GpuShader *shader
) {
	NULL_RETURN(device);
	device->QueueDestroyShader(
		device->driverData,
		shader
	);
}

void SDL_GpuQueueDestroyComputePipeline(
	SDL_GpuDevice *device,
	SDL_GpuComputePipeline *computePipeline
) {
	NULL_RETURN(device);
	device->QueueDestroyComputePipeline(
		device->driverData,
		computePipeline
	);
}

void SDL_GpuQueueDestroyGraphicsPipeline(
	SDL_GpuDevice *device,
	SDL_GpuGraphicsPipeline *graphicsPipeline
) {
	NULL_RETURN(device);
	device->QueueDestroyGraphicsPipeline(
		device->driverData,
		graphicsPipeline
	);
}

void SDL_GpuQueueDestroyOcclusionQuery(
    SDL_GpuDevice *device,
    SDL_GpuOcclusionQuery *query
) {
    NULL_RETURN(device);
    device->QueueDestroyOcclusionQuery(
        device->driverData,
        query
    );
}

/* Render Pass */

SDL_GpuRenderPass* SDL_GpuBeginRenderPass(
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuColorAttachmentInfo *colorAttachmentInfos,
	Uint32 colorAttachmentCount,
	SDL_GpuDepthStencilAttachmentInfo *depthStencilAttachmentInfo
) {
    CommandBufferCommonHeader *commandBufferHeader;

    CHECK_COMMAND_BUFFER_RETURN_NULL
    CHECK_ANY_PASS_IN_PROGRESS
	COMMAND_BUFFER_DEVICE->BeginRenderPass(
		commandBuffer,
		colorAttachmentInfos,
		colorAttachmentCount,
		depthStencilAttachmentInfo
	);

    commandBufferHeader = (CommandBufferCommonHeader*) commandBuffer;
    commandBufferHeader->renderPass.inProgress = SDL_TRUE;
    return (SDL_GpuRenderPass*) &(commandBufferHeader->renderPass);
}

void SDL_GpuBindGraphicsPipeline(
	SDL_GpuRenderPass *renderPass,
	SDL_GpuGraphicsPipeline *graphicsPipeline
) {
    NULL_RETURN(renderPass)
	RENDERPASS_DEVICE->BindGraphicsPipeline(
		RENDERPASS_COMMAND_BUFFER,
		graphicsPipeline
	);
}

void SDL_GpuSetViewport(
	SDL_GpuRenderPass *renderPass,
	SDL_GpuViewport *viewport
) {
	NULL_RETURN(renderPass)
    CHECK_RENDERPASS
	RENDERPASS_DEVICE->SetViewport(
		RENDERPASS_COMMAND_BUFFER,
		viewport
	);
}

void SDL_GpuSetScissor(
	SDL_GpuRenderPass *renderPass,
	SDL_GpuRect *scissor
) {
	NULL_RETURN(renderPass)
    CHECK_RENDERPASS
	RENDERPASS_DEVICE->SetScissor(
		RENDERPASS_COMMAND_BUFFER,
		scissor
	);
}

void SDL_GpuBindVertexBuffers(
    SDL_GpuRenderPass *renderPass,
	Uint32 firstBinding,
	Uint32 bindingCount,
	SDL_GpuBufferBinding *pBindings
) {
	NULL_RETURN(renderPass)
    CHECK_RENDERPASS
	RENDERPASS_DEVICE->BindVertexBuffers(
		RENDERPASS_COMMAND_BUFFER,
		firstBinding,
		bindingCount,
		pBindings
	);
}

void SDL_GpuBindIndexBuffer(
    SDL_GpuRenderPass *renderPass,
	SDL_GpuBufferBinding *pBinding,
	SDL_GpuIndexElementSize indexElementSize
) {
	NULL_RETURN(renderPass)
    CHECK_RENDERPASS
	RENDERPASS_DEVICE->BindIndexBuffer(
		RENDERPASS_COMMAND_BUFFER,
		pBinding,
		indexElementSize
	);
}

void SDL_GpuBindGraphicsResourceSet(
	SDL_GpuRenderPass *renderPass,
	Uint32 setIndex,
	SDL_GpuShaderResourceBinding *resourceBindings,
	Uint32 resourceBindingCount
) {
	NULL_RETURN(renderPass)
	CHECK_RENDERPASS
	RENDERPASS_DEVICE->BindGraphicsResourceSet(
		RENDERPASS_COMMAND_BUFFER,
		setIndex,
		resourceBindings,
		resourceBindingCount
	);
}

void SDL_GpuPushGraphicsUniformData(
	SDL_GpuRenderPass *renderPass,
	SDL_GpuUniformBuffer *uniformBuffer,
	void *data,
	Uint32 dataLengthInBytes
) {
	NULL_RETURN(uniformBuffer)
	CHECK_RENDERPASS
	RENDERPASS_DEVICE->PushGraphicsUniformData(
		RENDERPASS_COMMAND_BUFFER,
		uniformBuffer,
		data,
		dataLengthInBytes
	);
}

void SDL_GpuDrawInstancedPrimitives(
    SDL_GpuRenderPass *renderPass,
	Uint32 baseVertex,
	Uint32 startIndex,
	Uint32 primitiveCount,
	Uint32 instanceCount
) {
	NULL_RETURN(renderPass)
    CHECK_RENDERPASS
	RENDERPASS_DEVICE->DrawInstancedPrimitives(
		RENDERPASS_COMMAND_BUFFER,
		baseVertex,
		startIndex,
		primitiveCount,
		instanceCount
	);
}

void SDL_GpuDrawPrimitives(
    SDL_GpuRenderPass *renderPass,
	Uint32 vertexStart,
	Uint32 primitiveCount
) {
	NULL_RETURN(renderPass)
    CHECK_RENDERPASS
	RENDERPASS_DEVICE->DrawPrimitives(
		RENDERPASS_COMMAND_BUFFER,
		vertexStart,
		primitiveCount
	);
}

void SDL_GpuDrawPrimitivesIndirect(
    SDL_GpuRenderPass *renderPass,
	SDL_GpuBuffer *gpuBuffer,
	Uint32 offsetInBytes,
	Uint32 drawCount,
	Uint32 stride
) {
	NULL_RETURN(renderPass)
    CHECK_RENDERPASS
	RENDERPASS_DEVICE->DrawPrimitivesIndirect(
		RENDERPASS_COMMAND_BUFFER,
		gpuBuffer,
		offsetInBytes,
		drawCount,
		stride
	);
}

void SDL_GpuEndRenderPass(
    SDL_GpuRenderPass *renderPass
) {
	NULL_RETURN(renderPass)
    CHECK_RENDERPASS
	RENDERPASS_DEVICE->EndRenderPass(
		RENDERPASS_COMMAND_BUFFER
	);

    ((CommandBufferCommonHeader*) RENDERPASS_COMMAND_BUFFER)->renderPass.inProgress = SDL_FALSE;
}

/* Compute Pass */

SDL_GpuComputePass* SDL_GpuBeginComputePass(
	SDL_GpuCommandBuffer *commandBuffer
) {
    CommandBufferCommonHeader* commandBufferHeader;

    CHECK_COMMAND_BUFFER_RETURN_NULL
    CHECK_ANY_PASS_IN_PROGRESS
	COMMAND_BUFFER_DEVICE->BeginComputePass(
		commandBuffer
	);

    commandBufferHeader = (CommandBufferCommonHeader*) commandBuffer;
    commandBufferHeader->computePass.inProgress = SDL_TRUE;
    return (SDL_GpuComputePass*) &(commandBufferHeader->computePass);
}

void SDL_GpuBindComputePipeline(
	SDL_GpuComputePass *computePass,
	SDL_GpuComputePipeline *computePipeline
) {
	NULL_RETURN(computePass)
    CHECK_COMPUTEPASS
	COMPUTEPASS_DEVICE->BindComputePipeline(
		COMPUTEPASS_COMMAND_BUFFER,
		computePipeline
	);
}

void SDL_GpuBindComputeResourceSet(
	SDL_GpuComputePass *computePass,
	Uint32 setIndex,
	SDL_GpuShaderResourceBinding *resourceBindings,
	Uint32 resourceBindingCount
) {
	NULL_RETURN(computePass)
	CHECK_COMPUTEPASS
	COMPUTEPASS_DEVICE->BindComputeResourceSet(
		COMPUTEPASS_COMMAND_BUFFER,
		setIndex,
		resourceBindings,
		resourceBindingCount
	);
}

void SDL_GpuPushComputeUniformData(
	SDL_GpuComputePass *computePass,
	SDL_GpuUniformBuffer *uniformBuffer,
	void *data,
	Uint32 dataLengthInBytes
) {
	NULL_RETURN(computePass)
	CHECK_COMPUTEPASS
	COMPUTEPASS_DEVICE->PushComputeUniformData(
		COMPUTEPASS_COMMAND_BUFFER,
		uniformBuffer,
		data,
		dataLengthInBytes
	);
}

void SDL_GpuDispatchCompute(
	SDL_GpuComputePass *computePass,
	Uint32 groupCountX,
	Uint32 groupCountY,
	Uint32 groupCountZ
) {
	NULL_RETURN(computePass)
    CHECK_COMPUTEPASS
	COMPUTEPASS_DEVICE->DispatchCompute(
		COMPUTEPASS_COMMAND_BUFFER,
		groupCountX,
		groupCountY,
		groupCountZ
	);
}

void SDL_GpuEndComputePass(
	SDL_GpuComputePass *computePass
) {
	NULL_RETURN(computePass)
    CHECK_COMPUTEPASS
	COMPUTEPASS_DEVICE->EndComputePass(
        COMPUTEPASS_COMMAND_BUFFER
	);

    ((CommandBufferCommonHeader*) COMPUTEPASS_COMMAND_BUFFER)->computePass.inProgress = SDL_FALSE;
}

/* TransferBuffer Set/Get */

void SDL_GpuSetTransferData(
	SDL_GpuDevice *device,
	void* data,
	SDL_GpuTransferBuffer *transferBuffer,
	SDL_GpuBufferCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(device)
	device->SetTransferData(
		device->driverData,
		data,
		transferBuffer,
		copyParams,
		cycle
	);
}

void SDL_GpuGetTransferData(
	SDL_GpuDevice *device,
	SDL_GpuTransferBuffer *transferBuffer,
	void* data,
	SDL_GpuBufferCopy *copyParams
) {
	NULL_RETURN(device)
	device->GetTransferData(
		device->driverData,
		transferBuffer,
		data,
		copyParams
	);
}

/* Copy Pass */

SDL_GpuCopyPass* SDL_GpuBeginCopyPass(
	SDL_GpuCommandBuffer *commandBuffer
) {
    CommandBufferCommonHeader *commandBufferHeader;

    CHECK_COMMAND_BUFFER_RETURN_NULL
    CHECK_ANY_PASS_IN_PROGRESS
	COMMAND_BUFFER_DEVICE->BeginCopyPass(
		commandBuffer
	);

    commandBufferHeader = (CommandBufferCommonHeader*) commandBuffer;
    commandBufferHeader->copyPass.inProgress = SDL_TRUE;
    return (SDL_GpuCopyPass*) &(commandBufferHeader->copyPass);
}

void SDL_GpuUploadToTexture(
    SDL_GpuCopyPass *copyPass,
	SDL_GpuTransferBuffer *transferBuffer,
	SDL_GpuTextureRegion *textureRegion,
	SDL_GpuBufferImageCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(copyPass)
    CHECK_COPYPASS
	COPYPASS_DEVICE->UploadToTexture(
		COPYPASS_COMMAND_BUFFER,
		transferBuffer,
		textureRegion,
		copyParams,
		cycle
	);
}

void SDL_GpuUploadToBuffer(
    SDL_GpuCopyPass *copyPass,
	SDL_GpuTransferBuffer *transferBuffer,
	SDL_GpuBuffer *gpuBuffer,
	SDL_GpuBufferCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(copyPass)
	COPYPASS_DEVICE->UploadToBuffer(
		COPYPASS_COMMAND_BUFFER,
		transferBuffer,
		gpuBuffer,
		copyParams,
		cycle
	);
}

void SDL_GpuCopyTextureToTexture(
    SDL_GpuCopyPass *copyPass,
	SDL_GpuTextureRegion *source,
	SDL_GpuTextureRegion *destination,
	SDL_bool cycle
) {
	NULL_RETURN(copyPass)
	COPYPASS_DEVICE->CopyTextureToTexture(
		COPYPASS_COMMAND_BUFFER,
		source,
		destination,
		cycle
	);
}

void SDL_GpuCopyBufferToBuffer(
    SDL_GpuCopyPass *copyPass,
	SDL_GpuBuffer *source,
	SDL_GpuBuffer *destination,
	SDL_GpuBufferCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(copyPass)
	COPYPASS_DEVICE->CopyBufferToBuffer(
		COPYPASS_COMMAND_BUFFER,
		source,
		destination,
		copyParams,
		cycle
	);
}

void SDL_GpuGenerateMipmaps(
    SDL_GpuCopyPass *copyPass,
	SDL_GpuTexture *texture
) {
	NULL_RETURN(copyPass)
	COPYPASS_DEVICE->GenerateMipmaps(
		COPYPASS_COMMAND_BUFFER,
		texture
	);
}

void SDL_GpuEndCopyPass(
	SDL_GpuCopyPass *copyPass
) {
	NULL_RETURN(copyPass)
    CHECK_COPYPASS
	COPYPASS_DEVICE->EndCopyPass(
		COPYPASS_COMMAND_BUFFER
	);

    ((CommandBufferCommonHeader*) COPYPASS_COMMAND_BUFFER)->copyPass.inProgress = SDL_FALSE;
}

void SDL_GpuBlit(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureRegion *source,
    SDL_GpuTextureRegion *destination,
    SDL_GpuFilter filterMode,
	SDL_bool cycle
) {
    CHECK_COMMAND_BUFFER
    COMMAND_BUFFER_DEVICE->Blit(
        commandBuffer,
        source,
        destination,
        filterMode,
        cycle
    );
}

/* Submission/Presentation */

SDL_bool SDL_GpuClaimWindow(
	SDL_GpuDevice *device,
	SDL_Window *windowHandle,
	SDL_GpuPresentMode presentMode,
    SDL_GpuTextureFormat swapchainFormat,
    SDL_GpuColorSpace colorSpace
) {
	if (device == NULL) { return 0; }
	return device->ClaimWindow(
		device->driverData,
		windowHandle,
		presentMode,
        swapchainFormat,
        colorSpace
	);
}

void SDL_GpuUnclaimWindow(
	SDL_GpuDevice *device,
	SDL_Window *windowHandle
) {
	NULL_RETURN(device);
	device->UnclaimWindow(
		device->driverData,
		windowHandle
	);
}

void SDL_GpuSetSwapchainParameters(
	SDL_GpuDevice *device,
	SDL_Window *windowHandle,
	SDL_GpuPresentMode presentMode,
    SDL_GpuTextureFormat swapchainFormat,
    SDL_GpuColorSpace colorSpace
) {
	NULL_RETURN(device);
	device->SetSwapchainParameters(
		device->driverData,
		windowHandle,
		presentMode,
        swapchainFormat,
        colorSpace
	);
}

SDL_GpuTextureFormat SDL_GpuGetSwapchainFormat(
	SDL_GpuDevice *device,
	SDL_Window *windowHandle
) {
	if (device == NULL) { return 0; }
	return device->GetSwapchainFormat(
		device->driverData,
		windowHandle
	);
}

SDL_bool SDL_GpuSupportsPresentMode(
	SDL_GpuDevice *device,
	SDL_GpuPresentMode presentMode
) {
	if (device == NULL) { return SDL_FALSE; }
	return device->SupportsPresentMode(
		device->driverData,
		presentMode
	);
}

SDL_GpuCommandBuffer* SDL_GpuAcquireCommandBuffer(
	SDL_GpuDevice *device
) {
	SDL_GpuCommandBuffer* commandBuffer;
    CommandBufferCommonHeader *commandBufferHeader;
    NULL_RETURN_NULL(device);

    commandBuffer = device->AcquireCommandBuffer(
		device->driverData
	);

    if (commandBuffer == NULL)
    {
        return NULL;
    }

    commandBufferHeader = (CommandBufferCommonHeader*) commandBuffer;
    commandBufferHeader->device = device;
    commandBufferHeader->renderPass.commandBuffer = commandBuffer;
    commandBufferHeader->renderPass.inProgress = SDL_FALSE;
    commandBufferHeader->computePass.commandBuffer = commandBuffer;
    commandBufferHeader->computePass.inProgress = SDL_FALSE;
    commandBufferHeader->copyPass.commandBuffer = commandBuffer;
    commandBufferHeader->copyPass.inProgress = SDL_FALSE;
    commandBufferHeader->submitted = SDL_FALSE;

    return commandBuffer;
}

SDL_GpuTexture* SDL_GpuAcquireSwapchainTexture(
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_Window *windowHandle,
	Uint32 *pWidth,
	Uint32 *pHeight
) {
    CHECK_COMMAND_BUFFER_RETURN_NULL
	return COMMAND_BUFFER_DEVICE->AcquireSwapchainTexture(
		commandBuffer,
		windowHandle,
		pWidth,
		pHeight
	);
}

void SDL_GpuSubmit(
	SDL_GpuCommandBuffer *commandBuffer
) {
    CHECK_COMMAND_BUFFER
    CommandBufferCommonHeader *commandBufferHeader = (CommandBufferCommonHeader*) commandBuffer;

    if (
        commandBufferHeader->renderPass.inProgress ||
        commandBufferHeader->computePass.inProgress ||
        commandBufferHeader->copyPass.inProgress
    ) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot submit command buffer while a pass is in progress!");
        return;
    }

    commandBufferHeader->submitted = SDL_TRUE;

	COMMAND_BUFFER_DEVICE->Submit(
		commandBuffer
	);
}

SDL_GpuFence* SDL_GpuSubmitAndAcquireFence(
	SDL_GpuCommandBuffer *commandBuffer
) {
    CHECK_COMMAND_BUFFER_RETURN_NULL
    CommandBufferCommonHeader *commandBufferHeader = (CommandBufferCommonHeader*) commandBuffer;

    if (
        commandBufferHeader->renderPass.inProgress ||
        commandBufferHeader->computePass.inProgress ||
        commandBufferHeader->copyPass.inProgress
    ) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot submit command buffer while a pass is in progress!");
        return NULL;
    }

    commandBufferHeader->submitted = SDL_TRUE;

	return COMMAND_BUFFER_DEVICE->SubmitAndAcquireFence(
		commandBuffer
	);
}

void SDL_GpuWait(
	SDL_GpuDevice *device
) {
	NULL_RETURN(device);
	device->Wait(
		device->driverData
	);
}

void SDL_GpuWaitForFences(
	SDL_GpuDevice *device,
	Uint8 waitAll,
	Uint32 fenceCount,
	SDL_GpuFence **pFences
) {
	NULL_RETURN(device);
	device->WaitForFences(
		device->driverData,
		waitAll,
		fenceCount,
		pFences
	);
}

SDL_bool SDL_GpuQueryFence(
	SDL_GpuDevice *device,
	SDL_GpuFence *fence
) {
	if (device == NULL) { return 0; }

	return device->QueryFence(
		device->driverData,
		fence
	);
}

void SDL_GpuReleaseFence(
	SDL_GpuDevice *device,
	SDL_GpuFence *fence
) {
	NULL_RETURN(device);
	device->ReleaseFence(
		device->driverData,
		fence
	);
}

void SDL_GpuDownloadFromTexture(
	SDL_GpuDevice *device,
	SDL_GpuTextureRegion *textureRegion,
	SDL_GpuTransferBuffer *transferBuffer,
	SDL_GpuBufferImageCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(device);
	device->DownloadFromTexture(
		device->driverData,
		textureRegion,
		transferBuffer,
		copyParams,
		cycle
	);
}

void SDL_GpuDownloadFromBuffer(
	SDL_GpuDevice *device,
	SDL_GpuBuffer *gpuBuffer,
	SDL_GpuTransferBuffer *transferBuffer,
	SDL_GpuBufferCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(device);
	device->DownloadFromBuffer(
		device->driverData,
		gpuBuffer,
		transferBuffer,
		copyParams,
		cycle
	);
}

void SDL_GpuOcclusionQueryBegin(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuOcclusionQuery *query
) {
    NULL_RETURN(commandBuffer);
    COMMAND_BUFFER_DEVICE->OcclusionQueryBegin(
        commandBuffer,
        query
    );
}

void SDL_GpuOcclusionQueryEnd(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuOcclusionQuery *query
) {
    NULL_RETURN(commandBuffer);
    COMMAND_BUFFER_DEVICE->OcclusionQueryEnd(
        commandBuffer,
        query
    );
}

SDL_bool SDL_GpuOcclusionQueryPixelCount(
    SDL_GpuDevice *device,
    SDL_GpuOcclusionQuery *query,
    Uint32 *pixelCount
) {
    if (device == NULL)
        return SDL_FALSE;

    return device->OcclusionQueryPixelCount(
        device->driverData,
        query,
        pixelCount
    );
}
