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

#define NULL_RETURN(name) if (name == NULL) { return; }
#define NULL_RETURN_NULL(name) if (name == NULL) { return NULL; }

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

static SDL_GpuBackend selectedBackend = SDL_GPU_BACKEND_INVALID;

SDL_GpuBackend SDL_GpuSelectBackend(
	SDL_GpuBackend *preferredBackends,
	Uint32 preferredBackendCount,
	Uint32 *flags
) {
	Uint32 i;
	SDL_GpuBackend currentPreferredBackend;

	/* Iterate the array and return if a backend successfully prepares. */

	for (i = 0; i < preferredBackendCount; i += 1)
	{
		currentPreferredBackend = preferredBackends[i];
		if (backends[currentPreferredBackend] != NULL && backends[currentPreferredBackend]->PrepareDriver(flags))
		{
			selectedBackend = currentPreferredBackend;
			return currentPreferredBackend;
		}
	}

	SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No supported SDL_Gpu backend found!");
	return SDL_GPU_BACKEND_INVALID;
}

SDL_GpuDevice* SDL_GpuCreateDevice(
	Uint8 debugMode
) {
	SDL_GpuDevice *result;

	if (selectedBackend == SDL_GPU_BACKEND_INVALID)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid backend selection. Did you call SDL_GpuSelectBackend?");
		return NULL;
	}

	result = backends[selectedBackend]->CreateDevice(
		debugMode
	);
	if (result != NULL) {
		result->backend = selectedBackend;
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
	SDL_GpuComputeShaderInfo *computeShaderInfo
) {
	NULL_RETURN_NULL(device);
	return device->CreateComputePipeline(
		device->driverData,
		computeShaderInfo
	);
}

SDL_GpuGraphicsPipeline* SDL_GpuCreateGraphicsPipeline(
	SDL_GpuDevice *device,
	SDL_GpuGraphicsPipelineCreateInfo *pipelineCreateInfo
) {
	NULL_RETURN_NULL(device);
	return device->CreateGraphicsPipeline(
		device->driverData,
		pipelineCreateInfo
	);
}

SDL_GpuSampler* SDL_GpuCreateSampler(
	SDL_GpuDevice *device,
	SDL_GpuSamplerStateCreateInfo *samplerStateCreateInfo
) {
	NULL_RETURN_NULL(device);
	return device->CreateSampler(
		device->driverData,
		samplerStateCreateInfo
	);
}

SDL_GpuShaderModule* SDL_GpuCreateShaderModule(
    SDL_GpuDevice *device,
    SDL_GpuShaderModuleCreateInfo *shaderModuleCreateInfo
) {
    return device->CreateShaderModule(
        device->driverData,
        shaderModuleCreateInfo
    );
}

SDL_GpuTexture* SDL_GpuCreateTexture(
	SDL_GpuDevice *device,
	SDL_GpuTextureCreateInfo *textureCreateInfo
) {
	SDL_GpuTextureFormat newFormat;

	NULL_RETURN_NULL(device);

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
	NULL_RETURN_NULL(device);
	return device->CreateGpuBuffer(
		device->driverData,
		usageFlags,
		sizeInBytes
	);
}

SDL_GpuTransferBuffer* SDL_GpuCreateTransferBuffer(
	SDL_GpuDevice *device,
	SDL_GpuTransferUsage usage,
	Uint32 sizeInBytes
) {
	NULL_RETURN_NULL(device);
	return device->CreateTransferBuffer(
		device->driverData,
		usage,
		sizeInBytes
	);
}

SDL_GpuOcclusionQuery* SDL_GpuCreateOcclusionQuery(
    SDL_GpuDevice *device
) {
    NULL_RETURN_NULL(device);
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
	NULL_RETURN(device);
	NULL_RETURN(buffer);

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
	NULL_RETURN(device);
	NULL_RETURN(texture);

	device->SetTextureName(
		device->driverData,
		texture,
		text
	);
}

void SDL_GpuSetStringMarker(
    SDL_GpuDevice *device,
    SDL_GpuCommandBuffer *commandBuffer,
    const char *text
) {
    NULL_RETURN(device);
    NULL_RETURN(commandBuffer);

    device->SetStringMarker(
        device->driverData,
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

void SDL_GpuQueueDestroyShaderModule(
	SDL_GpuDevice *device,
	SDL_GpuShaderModule *shaderModule
) {
	NULL_RETURN(device);
	device->QueueDestroyShaderModule(
		device->driverData,
		shaderModule
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

void SDL_GpuBeginRenderPass(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuColorAttachmentInfo *colorAttachmentInfos,
	Uint32 colorAttachmentCount,
	SDL_GpuDepthStencilAttachmentInfo *depthStencilAttachmentInfo
) {
	NULL_RETURN(device);
	device->BeginRenderPass(
		device->driverData,
		commandBuffer,
		colorAttachmentInfos,
		colorAttachmentCount,
		depthStencilAttachmentInfo
	);
}

void SDL_GpuBindGraphicsPipeline(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuGraphicsPipeline *graphicsPipeline
) {
	NULL_RETURN(device);
	device->BindGraphicsPipeline(
		device->driverData,
		commandBuffer,
		graphicsPipeline
	);
}

void SDL_GpuSetViewport(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuViewport *viewport
) {
	NULL_RETURN(device)
	device->SetViewport(
		device->driverData,
		commandBuffer,
		viewport
	);
}

void SDL_GpuSetScissor(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuRect *scissor
) {
	NULL_RETURN(device)
	device->SetScissor(
		device->driverData,
		commandBuffer,
		scissor
	);
}

void SDL_GpuBindVertexBuffers(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	Uint32 firstBinding,
	Uint32 bindingCount,
	SDL_GpuBufferBinding *pBindings
) {
	NULL_RETURN(device);
	device->BindVertexBuffers(
		device->driverData,
		commandBuffer,
		firstBinding,
		bindingCount,
		pBindings
	);
}

void SDL_GpuBindIndexBuffer(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuBufferBinding *pBinding,
	SDL_GpuIndexElementSize indexElementSize
) {
	NULL_RETURN(device);
	device->BindIndexBuffer(
		device->driverData,
		commandBuffer,
		pBinding,
		indexElementSize
	);
}

void SDL_GpuBindVertexSamplers(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuTextureSamplerBinding *pBindings
) {
	NULL_RETURN(device);
	device->BindVertexSamplers(
		device->driverData,
		commandBuffer,
		pBindings
	);
}

void SDL_GpuBindFragmentSamplers(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuTextureSamplerBinding *pBindings
) {
	NULL_RETURN(device);
	device->BindFragmentSamplers(
		device->driverData,
		commandBuffer,
		pBindings
	);
}

void SDL_GpuPushVertexShaderUniforms(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	void *data,
	Uint32 dataLengthInBytes
) {
	NULL_RETURN(device);
	device->PushVertexShaderUniforms(
		device->driverData,
		commandBuffer,
		data,
		dataLengthInBytes
	);
}

void SDL_GpuPushFragmentShaderUniforms(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	void *data,
	Uint32 dataLengthInBytes
) {
	NULL_RETURN(device);
	device->PushFragmentShaderUniforms(
		device->driverData,
		commandBuffer,
		data,
		dataLengthInBytes
	);
}

void SDL_GpuDrawInstancedPrimitives(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	Uint32 baseVertex,
	Uint32 startIndex,
	Uint32 primitiveCount,
	Uint32 instanceCount
) {
	NULL_RETURN(device);
	device->DrawInstancedPrimitives(
		device->driverData,
		commandBuffer,
		baseVertex,
		startIndex,
		primitiveCount,
		instanceCount
	);
}

void SDL_GpuDrawPrimitives(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	Uint32 vertexStart,
	Uint32 primitiveCount
) {
	NULL_RETURN(device);
	device->DrawPrimitives(
		device->driverData,
		commandBuffer,
		vertexStart,
		primitiveCount
	);
}

void SDL_GpuDrawPrimitivesIndirect(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuBuffer *gpuBuffer,
	Uint32 offsetInBytes,
	Uint32 drawCount,
	Uint32 stride
) {
	NULL_RETURN(device);
	device->DrawPrimitivesIndirect(
		device->driverData,
		commandBuffer,
		gpuBuffer,
		offsetInBytes,
		drawCount,
		stride
	);
}

void SDL_GpuEndRenderPass(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer
) {
	NULL_RETURN(device);
	device->EndRenderPass(
		device->driverData,
		commandBuffer
	);
}

/* Compute Pass */

void SDL_GpuBeginComputePass(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer
) {
	NULL_RETURN(device);
	device->BeginComputePass(
		device->driverData,
		commandBuffer
	);
}

void SDL_GpuBindComputePipeline(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuComputePipeline *computePipeline
) {
	NULL_RETURN(device);
	device->BindComputePipeline(
		device->driverData,
		commandBuffer,
		computePipeline
	);
}

void SDL_GpuBindComputeBuffers(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuComputeBufferBinding *pBindings
) {
	NULL_RETURN(device);
	device->BindComputeBuffers(
		device->driverData,
		commandBuffer,
		pBindings
	);
}

void SDL_GpuBindComputeTextures(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuComputeTextureBinding *pBindings
) {
	NULL_RETURN(device);
	device->BindComputeTextures(
		device->driverData,
		commandBuffer,
		pBindings
	);
}

void SDL_GpuPushComputeShaderUniforms(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	void *data,
	Uint32 dataLengthInBytes
) {
	NULL_RETURN(device);
	device->PushComputeShaderUniforms(
		device->driverData,
		commandBuffer,
		data,
		dataLengthInBytes
	);
}

void SDL_GpuDispatchCompute(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	Uint32 groupCountX,
	Uint32 groupCountY,
	Uint32 groupCountZ
) {
	NULL_RETURN(device);
	device->DispatchCompute(
		device->driverData,
		commandBuffer,
		groupCountX,
		groupCountY,
		groupCountZ
	);
}

void SDL_GpuEndComputePass(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer
) {
	NULL_RETURN(device);
	device->EndComputePass(
		device->driverData,
		commandBuffer
	);
}

/* TransferBuffer Set/Get */

void SDL_GpuSetTransferData(
	SDL_GpuDevice *device,
	void* data,
	SDL_GpuTransferBuffer *transferBuffer,
	SDL_GpuBufferCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(device);
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
	NULL_RETURN(device);
	device->GetTransferData(
		device->driverData,
		transferBuffer,
		data,
		copyParams
	);
}

/* Copy Pass */

void SDL_GpuBeginCopyPass(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer
) {
	NULL_RETURN(device);
	device->BeginCopyPass(
		device->driverData,
		commandBuffer
	);
}

void SDL_GpuUploadToTexture(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuTransferBuffer *transferBuffer,
	SDL_GpuTextureRegion *textureRegion,
	SDL_GpuBufferImageCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(device);
	device->UploadToTexture(
		device->driverData,
		commandBuffer,
		transferBuffer,
		textureRegion,
		copyParams,
		cycle
	);
}

void SDL_GpuUploadToBuffer(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuTransferBuffer *transferBuffer,
	SDL_GpuBuffer *gpuBuffer,
	SDL_GpuBufferCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(device);
	device->UploadToBuffer(
		device->driverData,
		commandBuffer,
		transferBuffer,
		gpuBuffer,
		copyParams,
		cycle
	);
}

void SDL_GpuCopyTextureToTexture(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuTextureRegion *source,
	SDL_GpuTextureRegion *destination,
	SDL_bool cycle
) {
	NULL_RETURN(device);
	device->CopyTextureToTexture(
		device->driverData,
		commandBuffer,
		source,
		destination,
		cycle
	);
}

void SDL_GpuCopyBufferToBuffer(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuBuffer *source,
	SDL_GpuBuffer *destination,
	SDL_GpuBufferCopy *copyParams,
	SDL_bool cycle
) {
	NULL_RETURN(device);
	device->CopyBufferToBuffer(
		device->driverData,
		commandBuffer,
		source,
		destination,
		copyParams,
		cycle
	);
}

void SDL_GpuGenerateMipmaps(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_GpuTexture *texture
) {
	NULL_RETURN(device);
	device->GenerateMipmaps(
		device->driverData,
		commandBuffer,
		texture
	);
}

void SDL_GpuEndCopyPass(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer
) {
	NULL_RETURN(device);
	device->EndCopyPass(
		device->driverData,
		commandBuffer
	);
}

void SDL_GpuBlit(
    SDL_GpuDevice *device,
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureRegion *source,
    SDL_GpuTextureRegion *destination,
    SDL_GpuFilter filterMode,
	SDL_bool cycle
) {
    NULL_RETURN(device);
    device->Blit(
        device->driverData,
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

SDL_GpuCommandBuffer* SDL_GpuAcquireCommandBuffer(
	SDL_GpuDevice *device
) {
	NULL_RETURN_NULL(device);
	return device->AcquireCommandBuffer(
		device->driverData
	);
}

SDL_GpuTexture* SDL_GpuAcquireSwapchainTexture(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer,
	SDL_Window *windowHandle,
	Uint32 *pWidth,
	Uint32 *pHeight
) {
	NULL_RETURN_NULL(device);
	return device->AcquireSwapchainTexture(
		device->driverData,
		commandBuffer,
		windowHandle,
		pWidth,
		pHeight
	);
}

void SDL_GpuSubmit(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer
) {
	NULL_RETURN(device);
	device->Submit(
		device->driverData,
		commandBuffer
	);
}

SDL_GpuFence* SDL_GpuSubmitAndAcquireFence(
	SDL_GpuDevice *device,
	SDL_GpuCommandBuffer *commandBuffer
) {
	NULL_RETURN_NULL(device);
	return device->SubmitAndAcquireFence(
		device->driverData,
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
    SDL_GpuDevice *device,
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuOcclusionQuery *query
) {
    NULL_RETURN(device);
    device->OcclusionQueryBegin(
        device->driverData,
        commandBuffer,
        query
    );
}

void SDL_GpuOcclusionQueryEnd(
    SDL_GpuDevice *device,
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuOcclusionQuery *query
) {
    NULL_RETURN(device);
    device->OcclusionQueryEnd(
        device->driverData,
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
