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

/* Internal Helper Utilities */

static inline int32_t Texture_GetBlockSize(
	SDL_GpuTextureFormat format
) {
	switch (format)
	{
		case SDL_GPU_TEXTUREFORMAT_BC1:
		case SDL_GPU_TEXTUREFORMAT_BC2:
		case SDL_GPU_TEXTUREFORMAT_BC3:
		case SDL_GPU_TEXTUREFORMAT_BC7:
			return 4;
		case SDL_GPU_TEXTUREFORMAT_R8:
		case SDL_GPU_TEXTUREFORMAT_R8_UINT:
		case SDL_GPU_TEXTUREFORMAT_R5G6B5:
		case SDL_GPU_TEXTUREFORMAT_B4G4R4A4:
		case SDL_GPU_TEXTUREFORMAT_A1R5G5B5:
		case SDL_GPU_TEXTUREFORMAT_R16_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R8G8_SNORM:
		case SDL_GPU_TEXTUREFORMAT_R8G8_UINT:
		case SDL_GPU_TEXTUREFORMAT_R16_UINT:
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8:
		case SDL_GPU_TEXTUREFORMAT_R32_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R16G16_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SNORM:
		case SDL_GPU_TEXTUREFORMAT_A2R10G10B10:
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UINT:
		case SDL_GPU_TEXTUREFORMAT_R16G16_UINT:
		case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R16G16B16A16:
		case SDL_GPU_TEXTUREFORMAT_R32G32_SFLOAT:
		case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT:
		case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_SFLOAT:
			return 1;
		default:
			SDL_GpuLogError(
				"Unrecognized TextureFormat!"
			);
			return 0;
	}
}

static inline uint32_t Texture_GetFormatSize(
	SDL_GpuTextureFormat format
) {
	switch (format)
	{
		case SDL_GPU_TEXTUREFORMAT_BC1:
			return 8;
		case SDL_GPU_TEXTUREFORMAT_BC2:
		case SDL_GPU_TEXTUREFORMAT_BC3:
		case SDL_GPU_TEXTUREFORMAT_BC7:
			return 16;
		case SDL_GPU_TEXTUREFORMAT_R8:
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
			SDL_GpuLogError(
				"Unrecognized TextureFormat!"
			);
			return 0;
	}
}

static inline uint32_t PrimitiveVerts(
	SDL_GpuPrimitiveType primitiveType,
	uint32_t primitiveCount
) {
	switch (primitiveType)
	{
		case SDL_GPU_PRIMITIVETYPE_TRIANGLELIST:
			return primitiveCount * 3;
		case SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP:
			return primitiveCount + 2;
		case SDL_GPU_PRIMITIVETYPE_LINELIST:
			return primitiveCount * 2;
		case SDL_GPU_PRIMITIVETYPE_LINESTRIP:
			return primitiveCount + 1;
		case SDL_GPU_PRIMITIVETYPE_POINTLIST:
			return primitiveCount;
		default:
			SDL_GpuLogError(
				"Unrecognized primitive type!"
			);
			return 0;
	}
}

static inline uint32_t IndexSize(SDL_GpuIndexElementSize size)
{
	return (size == SDL_GPU_INDEXELEMENTSIZE_16BIT) ? 2 : 4;
}

static inline uint32_t BytesPerRow(
	int32_t width,
	SDL_GpuTextureFormat format
) {
	uint32_t blocksPerRow = width;

	if (	format == SDL_GPU_TEXTUREFORMAT_BC1 ||
		format == SDL_GPU_TEXTUREFORMAT_BC2 ||
		format == SDL_GPU_TEXTUREFORMAT_BC3 ||
		format == SDL_GPU_TEXTUREFORMAT_BC7	)
	{
		blocksPerRow = (width + 3) / 4;
	}

	return blocksPerRow * Texture_GetFormatSize(format);
}

static inline int32_t BytesPerImage(
	uint32_t width,
	uint32_t height,
	SDL_GpuTextureFormat format
) {
	uint32_t blocksPerRow = width;
	uint32_t blocksPerColumn = height;

	if (	format == SDL_GPU_TEXTUREFORMAT_BC1 ||
		format == SDL_GPU_TEXTUREFORMAT_BC2 ||
		format == SDL_GPU_TEXTUREFORMAT_BC3 ||
		format == SDL_GPU_TEXTUREFORMAT_BC7 )
	{
		blocksPerRow = (width + 3) / 4;
		blocksPerColumn = (height + 3) / 4;
	}

	return blocksPerRow * blocksPerColumn * Texture_GetFormatSize(format);
}

/* GraphicsDevice Limits */
/* TODO: can these be adjusted for modern low-end? */

#define MAX_TEXTURE_SAMPLERS		16
#define MAX_VERTEXTEXTURE_SAMPLERS	4
#define MAX_TOTAL_SAMPLERS		(MAX_TEXTURE_SAMPLERS + MAX_VERTEXTEXTURE_SAMPLERS)

#define MAX_BUFFER_BINDINGS			16

#define MAX_COLOR_TARGET_BINDINGS	4

/* Internal Shader Module Create Info */

typedef enum SDL_GpuDriver_ShaderType
{
	SDL_GPU_DRIVER_SHADERTYPE_VERTEX,
	SDL_GPU_DRIVER_SHADERTYPE_FRAGMENT,
	SDL_GPU_DRIVER_SHADERTYPE_COMPUTE
} SDL_GpuDriver_ShaderType;

typedef struct SDL_GpuDriver_ShaderModuleCreateInfo
{
	size_t codeSize;
	const uint32_t* byteCode;
	SDL_GpuDriver_ShaderType type;
} SDL_GpuDriver_ShaderModuleCreateInfo;

/* SDL_GpuDevice Definition */

typedef struct SDL_GpuRenderer SDL_GpuRenderer;

struct SDL_GpuDevice
{
	/* Quit */

	void (*DestroyDevice)(SDL_GpuDevice *device);

	/* State Creation */

	SDL_GpuComputePipeline* (*CreateComputePipeline)(
		SDL_GpuRenderer *driverData,
		SDL_GpuComputeShaderInfo *computeShaderInfo
	);

	SDL_GpuGraphicsPipeline* (*CreateGraphicsPipeline)(
		SDL_GpuRenderer *driverData,
		SDL_GpuGraphicsPipelineCreateInfo *pipelineCreateInfo
	);

	SDL_GpuSampler* (*CreateSampler)(
		SDL_GpuRenderer *driverData,
		SDL_GpuSamplerStateCreateInfo *samplerStateCreateInfo
	);

	SDL_GpuShaderModule* (*CreateShaderModule)(
		SDL_GpuRenderer *driverData,
		SDL_GpuDriver_ShaderModuleCreateInfo *shaderModuleCreateInfo
	);

	SDL_GpuTexture* (*CreateTexture)(
		SDL_GpuRenderer *driverData,
		SDL_GpuTextureCreateInfo *textureCreateInfo
	);

	SDL_GpuGpuBuffer* (*CreateGpuBuffer)(
		SDL_GpuRenderer *driverData,
		SDL_GpuBufferUsageFlags usageFlags,
		uint32_t sizeInBytes
	);

	SDL_GpuTransferBuffer* (*CreateTransferBuffer)(
		SDL_GpuRenderer *driverData,
		SDL_GpuTransferUsage usage,
		uint32_t sizeInBytes
	);

	/* Debug Naming */

	void (*SetGpuBufferName)(
		SDL_GpuRenderer *driverData,
		SDL_GpuGpuBuffer *buffer,
		const char *text
	);

	void (*SetTextureName)(
		SDL_GpuRenderer *driverData,
		SDL_GpuTexture *texture,
		const char *text
	);

	/* Disposal */

	void (*QueueDestroyTexture)(
		SDL_GpuRenderer *driverData,
		SDL_GpuTexture *texture
	);

	void (*QueueDestroySampler)(
		SDL_GpuRenderer *driverData,
		SDL_GpuSampler *sampler
	);

	void (*QueueDestroyGpuBuffer)(
		SDL_GpuRenderer *driverData,
		SDL_GpuGpuBuffer *gpuBuffer
	);

	void (*QueueDestroyTransferBuffer)(
		SDL_GpuRenderer *driverData,
		SDL_GpuTransferBuffer *transferBuffer
	);

	void (*QueueDestroyShaderModule)(
		SDL_GpuRenderer *driverData,
		SDL_GpuShaderModule *shaderModule
	);

	void (*QueueDestroyComputePipeline)(
		SDL_GpuRenderer *driverData,
		SDL_GpuComputePipeline *computePipeline
	);

	void (*QueueDestroyGraphicsPipeline)(
		SDL_GpuRenderer *driverData,
		SDL_GpuGraphicsPipeline *graphicsPipeline
	);

	/* Render Pass */

	void (*BeginRenderPass)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuColorAttachmentInfo *colorAttachmentInfos,
		uint32_t colorAttachmentCount,
		SDL_GpuDepthStencilAttachmentInfo *depthStencilAttachmentInfo
	);

	void (*BindGraphicsPipeline)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuGraphicsPipeline *graphicsPipeline
	);

	void (*SetViewport)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuViewport *viewport
	);

	void (*SetScissor)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuRect *scissor
	);

	void (*BindVertexBuffers)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		uint32_t firstBinding,
		uint32_t bindingCount,
		SDL_GpuBufferBinding *pBindings
	);

	void (*BindIndexBuffer)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuBufferBinding *pBinding,
		SDL_GpuIndexElementSize indexElementSize
	);

	void (*BindVertexSamplers)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuTextureSamplerBinding *pBindings
	);

	void (*BindFragmentSamplers)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuTextureSamplerBinding *pBindings
	);

	void (*PushVertexShaderUniforms)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		void *data,
		uint32_t dataLengthInBytes
	);

	void (*PushFragmentShaderUniforms)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		void *data,
		uint32_t dataLengthInBytes
	);

	void (*DrawInstancedPrimitives)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		uint32_t baseVertex,
		uint32_t startIndex,
		uint32_t primitiveCount,
		uint32_t instanceCount
	);

	void (*DrawPrimitives)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		uint32_t vertexStart,
		uint32_t primitiveCount
	);

	void (*DrawPrimitivesIndirect)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuGpuBuffer *gpuBuffer,
		uint32_t offsetInBytes,
		uint32_t drawCount,
		uint32_t stride
	);

	void (*EndRenderPass)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer
	);

	/* Compute Pass */

	void (*BeginComputePass)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer
	);

	void (*BindComputePipeline)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuComputePipeline *computePipeline
	);

	void (*BindComputeBuffers)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuComputeBufferBinding *pBindings
	);

	void (*BindComputeTextures)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuComputeTextureBinding *pBindings
	);

	void (*PushComputeShaderUniforms)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		void *data,
		uint32_t dataLengthInBytes
	);

	void (*DispatchCompute)(
		SDL_GpuRenderer *device,
		SDL_GpuCommandBuffer *commandBuffer,
		uint32_t groupCountX,
		uint32_t groupCountY,
		uint32_t groupCountZ
	);

	void (*EndComputePass)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer
	);

	/* TransferBuffer Set/Get */

	void (*SetTransferData)(
		SDL_GpuRenderer *driverData,
		void* data,
		SDL_GpuTransferBuffer *transferBuffer,
		SDL_GpuBufferCopy *copyParams,
		SDL_GpuTransferOptions transferOption
	);

	void (*GetTransferData)(
		SDL_GpuRenderer *driverData,
		SDL_GpuTransferBuffer *transferBuffer,
		void* data,
		SDL_GpuBufferCopy *copyParams
	);

	/* Copy Pass */

	void (*BeginCopyPass)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer
	);

	void (*UploadToTexture)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuTransferBuffer *transferBuffer,
		SDL_GpuTextureRegion *textureSlice,
		SDL_GpuBufferImageCopy *copyParams,
		SDL_GpuTextureWriteOptions writeOption
	);

	void (*UploadToBuffer)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuTransferBuffer *transferBuffer,
		SDL_GpuGpuBuffer *gpuBuffer,
		SDL_GpuBufferCopy *copyParams,
		SDL_GpuBufferWriteOptions writeOption
	);

	void (*CopyTextureToTexture)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuTextureRegion *source,
		SDL_GpuTextureRegion *destination,
		SDL_GpuTextureWriteOptions writeOption
	);

	void (*CopyBufferToBuffer)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuGpuBuffer *source,
		SDL_GpuGpuBuffer *destination,
		SDL_GpuBufferCopy *copyParams,
		SDL_GpuBufferWriteOptions writeOption
	);

	void (*GenerateMipmaps)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		SDL_GpuTexture *texture
	);

	void (*EndCopyPass)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer
	);

	/* Submission/Presentation */

	uint8_t (*ClaimWindow)(
		SDL_GpuRenderer *driverData,
		void *windowHandle,
		SDL_GpuPresentMode presentMode
	);

	void (*UnclaimWindow)(
		SDL_GpuRenderer *driverData,
		void *windowHandle
	);

	void (*SetSwapchainPresentMode)(
		SDL_GpuRenderer *driverData,
		void *windowHandle,
		SDL_GpuPresentMode presentMode
	);

	SDL_GpuTextureFormat (*GetSwapchainFormat)(
		SDL_GpuRenderer *driverData,
		void *windowHandle
	);

	SDL_GpuCommandBuffer* (*AcquireCommandBuffer)(
		SDL_GpuRenderer *driverData
	);

	SDL_GpuTexture* (*AcquireSwapchainTexture)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer,
		void *windowHandle,
		uint32_t *pWidth,
		uint32_t *pHeight
	);

	void (*Submit)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer
	);

	SDL_GpuFence* (*SubmitAndAcquireFence)(
		SDL_GpuRenderer *driverData,
		SDL_GpuCommandBuffer *commandBuffer
	);

	void (*Wait)(
		SDL_GpuRenderer *driverData
	);

	void (*WaitForFences)(
		SDL_GpuRenderer *driverData,
		uint8_t waitAll,
		uint32_t fenceCount,
		SDL_GpuFence **pFences
	);

	int (*QueryFence)(
		SDL_GpuRenderer *driverData,
		SDL_GpuFence *fence
	);

	void (*ReleaseFence)(
		SDL_GpuRenderer *driverData,
		SDL_GpuFence *fence
	);

	void (*DownloadFromTexture)(
		SDL_GpuRenderer *driverData,
		SDL_GpuTextureRegion *textureSlice,
		SDL_GpuTransferBuffer *transferBuffer,
		SDL_GpuBufferImageCopy *copyParams,
		SDL_GpuTransferOptions transferOption
	);

	void (*DownloadFromBuffer)(
		SDL_GpuRenderer *driverData,
		SDL_GpuGpuBuffer *gpuBuffer,
		SDL_GpuTransferBuffer *transferBuffer,
		SDL_GpuBufferCopy *copyParams,
		SDL_GpuTransferOptions transferOption
	);

	/* Opaque pointer for the Driver */
	SDL_GpuRenderer *driverData;
};

#define ASSIGN_DRIVER_FUNC(func, name) \
	result->func = name##_##func;
#define ASSIGN_DRIVER(name) \
	ASSIGN_DRIVER_FUNC(DestroyDevice, name) \
	ASSIGN_DRIVER_FUNC(CreateComputePipeline, name) \
	ASSIGN_DRIVER_FUNC(CreateGraphicsPipeline, name) \
	ASSIGN_DRIVER_FUNC(CreateSampler, name) \
	ASSIGN_DRIVER_FUNC(CreateShaderModule, name) \
	ASSIGN_DRIVER_FUNC(CreateTexture, name) \
	ASSIGN_DRIVER_FUNC(CreateGpuBuffer, name) \
	ASSIGN_DRIVER_FUNC(CreateTransferBuffer, name) \
	ASSIGN_DRIVER_FUNC(SetGpuBufferName, name) \
	ASSIGN_DRIVER_FUNC(SetTextureName, name) \
	ASSIGN_DRIVER_FUNC(QueueDestroyTexture, name) \
	ASSIGN_DRIVER_FUNC(QueueDestroySampler, name) \
	ASSIGN_DRIVER_FUNC(QueueDestroyGpuBuffer, name) \
	ASSIGN_DRIVER_FUNC(QueueDestroyTransferBuffer, name) \
	ASSIGN_DRIVER_FUNC(QueueDestroyShaderModule, name) \
	ASSIGN_DRIVER_FUNC(QueueDestroyComputePipeline, name) \
	ASSIGN_DRIVER_FUNC(QueueDestroyGraphicsPipeline, name) \
	ASSIGN_DRIVER_FUNC(BeginRenderPass, name) \
	ASSIGN_DRIVER_FUNC(BindGraphicsPipeline, name) \
	ASSIGN_DRIVER_FUNC(SetViewport, name) \
	ASSIGN_DRIVER_FUNC(SetScissor, name) \
	ASSIGN_DRIVER_FUNC(BindVertexBuffers, name) \
	ASSIGN_DRIVER_FUNC(BindIndexBuffer, name) \
	ASSIGN_DRIVER_FUNC(BindVertexSamplers, name) \
	ASSIGN_DRIVER_FUNC(BindFragmentSamplers, name) \
	ASSIGN_DRIVER_FUNC(PushVertexShaderUniforms, name) \
	ASSIGN_DRIVER_FUNC(PushFragmentShaderUniforms, name) \
	ASSIGN_DRIVER_FUNC(DrawInstancedPrimitives, name) \
	ASSIGN_DRIVER_FUNC(DrawPrimitives, name) \
	ASSIGN_DRIVER_FUNC(DrawPrimitivesIndirect, name) \
	ASSIGN_DRIVER_FUNC(EndRenderPass, name) \
	ASSIGN_DRIVER_FUNC(BeginComputePass, name) \
	ASSIGN_DRIVER_FUNC(BindComputePipeline, name) \
	ASSIGN_DRIVER_FUNC(BindComputeBuffers, name) \
	ASSIGN_DRIVER_FUNC(BindComputeTextures, name) \
	ASSIGN_DRIVER_FUNC(PushComputeShaderUniforms, name) \
	ASSIGN_DRIVER_FUNC(DispatchCompute, name) \
	ASSIGN_DRIVER_FUNC(EndComputePass, name) \
	ASSIGN_DRIVER_FUNC(SetTransferData, name) \
	ASSIGN_DRIVER_FUNC(GetTransferData, name) \
	ASSIGN_DRIVER_FUNC(BeginCopyPass, name) \
	ASSIGN_DRIVER_FUNC(UploadToTexture, name) \
	ASSIGN_DRIVER_FUNC(UploadToBuffer, name) \
	ASSIGN_DRIVER_FUNC(DownloadFromTexture, name) \
	ASSIGN_DRIVER_FUNC(DownloadFromBuffer, name) \
	ASSIGN_DRIVER_FUNC(CopyTextureToTexture, name) \
	ASSIGN_DRIVER_FUNC(CopyBufferToBuffer, name) \
	ASSIGN_DRIVER_FUNC(GenerateMipmaps, name) \
	ASSIGN_DRIVER_FUNC(EndCopyPass, name) \
	ASSIGN_DRIVER_FUNC(ClaimWindow, name) \
	ASSIGN_DRIVER_FUNC(UnclaimWindow, name) \
	ASSIGN_DRIVER_FUNC(SetSwapchainPresentMode, name) \
	ASSIGN_DRIVER_FUNC(GetSwapchainFormat, name) \
	ASSIGN_DRIVER_FUNC(AcquireCommandBuffer, name) \
	ASSIGN_DRIVER_FUNC(AcquireSwapchainTexture, name) \
	ASSIGN_DRIVER_FUNC(Submit, name) \
	ASSIGN_DRIVER_FUNC(SubmitAndAcquireFence, name) \
	ASSIGN_DRIVER_FUNC(Wait, name) \
	ASSIGN_DRIVER_FUNC(WaitForFences, name) \
	ASSIGN_DRIVER_FUNC(QueryFence, name) \
	ASSIGN_DRIVER_FUNC(ReleaseFence, name)

typedef struct SDL_GpuDriver
{
	const char *Name;
	uint8_t (*PrepareDriver)(uint32_t *flags);
	SDL_GpuDevice* (*CreateDevice)(
		uint8_t debugMode
	);
} SDL_GpuDriver;

extern SDL_GpuDriver VulkanDriver;
extern SDL_GpuDriver D3D11Driver;
extern SDL_GpuDriver PS5Driver;
