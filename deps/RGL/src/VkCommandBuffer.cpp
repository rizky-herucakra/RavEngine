#if RGL_VK_AVAILABLE
#include "VkCommandBuffer.hpp"
#include "VkCommandQueue.hpp"
#include "VkDevice.hpp"
#include "VkRenderPipeline.hpp"
#include "VkTexture.hpp"
#include "VkBuffer.hpp"
#include "VkRenderPass.hpp"
#include "VkSampler.hpp"
#include "VkSwapchain.hpp"
#include "VkComputePipeline.hpp"
#include <cstring>

namespace RGL {
	VkAttachmentLoadOp RGL2LoadOp(LoadAccessOperation op) {
		switch (op) {
		case decltype(op)::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
		case decltype(op)::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
		case decltype(op)::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		case decltype(op)::NotAccessed: return VK_ATTACHMENT_LOAD_OP_NONE_EXT;
		}
	}

	VkAttachmentStoreOp RGL2StoreOp(StoreAccessOperation op) {
		switch (op) {
		case decltype(op)::Store: return VK_ATTACHMENT_STORE_OP_STORE;
		case decltype(op)::None: return VK_ATTACHMENT_STORE_OP_NONE;
		case decltype(op)::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
		}
	}

	void encodeResourceTransition(VkCommandBuffer commandBuffer, VkImage image, 
		decltype(VkImageMemoryBarrier::srcAccessMask) srcAccessMask,
		decltype(VkImageMemoryBarrier::dstAccessMask) dstAccessMask,
		decltype(VkImageMemoryBarrier::oldLayout) oldLayout,
		decltype(VkImageMemoryBarrier::newLayout) newLayout,
		decltype(decltype(VkImageMemoryBarrier::subresourceRange)::aspectMask) aspectMask,
		VkPipelineStageFlags srcStageMask,
		VkPipelineStageFlags dstStageMask
	) {
		const VkImageMemoryBarrier image_memory_barrier_begin{
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.srcAccessMask = srcAccessMask,
					.dstAccessMask = dstAccessMask,
					.oldLayout = oldLayout,
					.newLayout = newLayout,
					.image = image,
					.subresourceRange = {
					  .aspectMask = aspectMask,
					  .baseMipLevel = 0,
					  .levelCount = 1,
					  .baseArrayLayer = 0,
					  .layerCount = 1,
					}
		};

		vkCmdPipelineBarrier(
			commandBuffer,
			srcStageMask,  // srcStageMask
			dstStageMask, // dstStageMask
			0,
			0,
			nullptr,
			0,
			nullptr,
			1, // imageMemoryBarrierCount
			&image_memory_barrier_begin // pImageMemoryBarriers
		);
	}

	void RGL::CommandBufferVk::Reset()
	{
		VK_CHECK(vkResetCommandBuffer(commandBuffer, 0));
	}
	void CommandBufferVk::Begin()
	{
		VkCommandBufferBeginInfo beginInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = 0,
		.pInheritanceInfo = nullptr,
		};
		VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo))
	}
	void CommandBufferVk::End()
	{		
		VK_CHECK(vkEndCommandBuffer(commandBuffer));
	}
	void CommandBufferVk::BindRenderPipeline(RGLRenderPipelinePtr generic_pipeline)
	{
		auto pipeline = std::static_pointer_cast<RenderPipelineVk>(generic_pipeline);
		

		// drawing commands
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->graphicsPipeline);

		/*
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipelineLayout->layout, 0, 1, &pipeline->pipelineLayout->descriptorSet, 0, nullptr);
		*/
		currentRenderPipeline = pipeline;

	}
	void CommandBufferVk::BeginRendering(RGLRenderPassPtr renderPassPtr)
	{
		auto renderPass = std::static_pointer_cast<RenderPassVk>(renderPassPtr);
		currentRenderPass = renderPass;

		stackarray(attachmentInfos, VkRenderingAttachmentInfoKHR, renderPass->config.attachments.size());

		auto makeAttachmentInfo = [](const RenderPassConfig::AttachmentDesc& attachment, VkImageView imageView) -> VkRenderingAttachmentInfoKHR {
			VkClearValue clearColor = { {{attachment.clearColor[0], attachment.clearColor[1], attachment.clearColor[2], attachment.clearColor[3]}} };

			return {
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
				.imageView = imageView,
				.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR,
				.loadOp = RGL2LoadOp(attachment.loadOp),
				.storeOp = RGL2StoreOp(attachment.storeOp),
				.clearValue = clearColor,
			};
		};


		uint32_t i = 0;
		for (const auto& attachment : renderPass->config.attachments) {
			
			attachmentInfos[i] = makeAttachmentInfo(attachment, renderPass->textures[i]->vkImageView);

			// the swapchain image may be in the wrong state (present state vs write state) so it needs to be transitioned
			auto castedImage = static_cast<TextureVk*>(renderPass->textures[i]);

			if (castedImage->owningSwapchain) {
				swapchainsToSignal.insert(castedImage->owningSwapchain);
			}

			i++;
		}

		// repeat for depth stencil attachment

		auto texSize = renderPass->textures[0]->GetSize();
		VkRenderingAttachmentInfoKHR* depthAttachmentinfo = nullptr;
		VkRenderingAttachmentInfoKHR depthAttachmentInfoData{};

		if (renderPass->config.depthAttachment.has_value()) {
			auto& da = renderPass->config.depthAttachment.value();
			depthAttachmentInfoData = makeAttachmentInfo(da, renderPass->depthTexture->vkImageView);
			depthAttachmentinfo = &depthAttachmentInfoData;
		}

		VkRenderingAttachmentInfoKHR* stencilAttachmentInfo = nullptr;
		VkRenderingAttachmentInfoKHR stencilAttachmentInfoData{};

		if (renderPass->config.stencilAttachment.has_value()) {
			auto& sa = renderPass->config.stencilAttachment.value();
			stencilAttachmentInfoData = makeAttachmentInfo(sa, renderPass->depthTexture->vkImageView);
			stencilAttachmentInfo = &stencilAttachmentInfoData;
		}


		const VkRenderingInfoKHR render_info{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
			.renderArea = {
				.offset = {0,0},
				.extent = VkExtent2D{.width = texSize.width, .height = texSize.height},
			},
			.layerCount = 1,
			.colorAttachmentCount = static_cast<uint32_t>(renderPass->config.attachments.size()),
			.pColorAttachments = attachmentInfos,
			.pDepthAttachment = depthAttachmentinfo,
			.pStencilAttachment = stencilAttachmentInfo
		};


		vkCmdBeginRendering(commandBuffer, &render_info);
	}
	void CommandBufferVk::EndRendering()
	{
		vkCmdEndRendering(commandBuffer);
		currentRenderPipeline = nullptr;	// reset this to avoid having stale state
	}
	void CommandBufferVk::BeginCompute(RGLComputePipelinePtr inPipeline)
	{
		currentComputePipeline = std::static_pointer_cast<ComputePipelineVk>(inPipeline);
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, currentComputePipeline->computePipeline);
	}
	void CommandBufferVk::EndCompute()
	{
		currentComputePipeline = nullptr;
	}
	void CommandBufferVk::DispatchCompute(uint32_t threadsX, uint32_t threadsY, uint32_t threadsZ)
	{
		vkCmdDispatch(commandBuffer, threadsX, threadsY, threadsZ);
	}
	void CommandBufferVk::BindBuffer(RGLBufferPtr buffer, uint32_t bindingOffset, uint32_t offsetIntoBuffer)
	{
		GenericBindBuffer(buffer, offsetIntoBuffer, bindingOffset, VK_PIPELINE_BIND_POINT_GRAPHICS);
		//vkUpdateDescriptorSets(currentRenderPipeline->owningDevice->device, 1, &writeinfo, 0, nullptr);
	}

	void CommandBufferVk::GenericBindBuffer(RGLBufferPtr& buffer, const uint32_t& offsetIntoBuffer, const uint32_t& bindingOffset, VkPipelineBindPoint bindPoint)
	{
		auto vkbuffer = std::static_pointer_cast<BufferVk>(buffer);
		VkDescriptorBufferInfo bufferInfo{
			.buffer = vkbuffer->buffer,
			.offset = offsetIntoBuffer * vkbuffer->stride,
			.range = VK_WHOLE_SIZE,
		};
		VkWriteDescriptorSet writeinfo{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = VK_NULL_HANDLE,	// we push descriptors instead
			.dstBinding = bindingOffset,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pImageInfo = nullptr,
			.pBufferInfo = &bufferInfo,
			.pTexelBufferView = nullptr
		};
		owningQueue->owningDevice->vkCmdPushDescriptorSetKHR(
			commandBuffer,
			bindPoint,
			(bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE? currentComputePipeline->pipelineLayout->layout : currentRenderPipeline->pipelineLayout->layout),
			0,
			1,
			&writeinfo
		);
	}

	void CommandBufferVk::BindComputeBuffer(RGLBufferPtr buffer, uint32_t binding, uint32_t offsetIntoBuffer)
	{
		GenericBindBuffer(buffer, offsetIntoBuffer, binding, VK_PIPELINE_BIND_POINT_COMPUTE);
	}

	void CommandBufferVk::SetVertexBuffer(RGLBufferPtr buffer, const VertexBufferBinding& bindingInfo)
	{
		auto vkbuffer = std::static_pointer_cast<BufferVk>(buffer);
		VkBuffer vertexBuffers[] = { vkbuffer->buffer };
		VkDeviceSize offsets[] = { bindingInfo.offsetIntoBuffer * vkbuffer->stride };
		vkCmdBindVertexBuffers(commandBuffer, bindingInfo.bindingPosition, std::size(vertexBuffers), vertexBuffers, offsets);
	}

	void CommandBufferVk::setPushConstantData(const RGL::untyped_span& data, const uint32_t& offset, VkShaderStageFlags stages)
	{
		// size must be a multiple of 4
		// need to get a little extra space for safety
		auto size = data.size() + (data.size() % 4 != 0 ? 4 : 0);
		stackarray(localdata, std::byte, size);
		std::memset(localdata, 0, size);
		std::memcpy(localdata, data.data(), data.size());

		auto layout = (stages == VK_SHADER_STAGE_COMPUTE_BIT ? currentComputePipeline->pipelineLayout : currentRenderPipeline->pipelineLayout)->layout;

		vkCmdPushConstants(commandBuffer, layout, stages, offset, size, localdata);
	}

	void CommandBufferVk::SetVertexBytes(const untyped_span data, uint32_t offset)
	{
		auto stages = currentRenderPipeline->pipelineLayout->pushConstantBindingStageFlags.at(offset);
		setPushConstantData(data, offset, stages);
	}
	void CommandBufferVk::SetFragmentBytes(const untyped_span data, uint32_t offset)
	{
		auto stages = currentRenderPipeline->pipelineLayout->pushConstantBindingStageFlags.at(offset);
		setPushConstantData(data, offset, stages);
	}
	void CommandBufferVk::SetComputeBytes(const untyped_span data, uint32_t offset)
	{
		setPushConstantData(data, offset, VK_SHADER_STAGE_COMPUTE_BIT);
	}
	void CommandBufferVk::SetIndexBuffer(RGLBufferPtr buffer)
	{
		const auto casted = std::static_pointer_cast<BufferVk>(buffer);
		const auto size_type = casted->stride == sizeof(uint16_t) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
		vkCmdBindIndexBuffer(commandBuffer, casted->buffer, 0, size_type);
	}
	void CommandBufferVk::SetVertexSampler(RGLSamplerPtr sampler, uint32_t index)
	{
		
	}
	void CommandBufferVk::SetFragmentSampler(RGLSamplerPtr sampler, uint32_t index)
	{
	}
	void CommandBufferVk::SetVertexTexture(const ITexture* texture, uint32_t index)
	{
	}
	void CommandBufferVk::SetFragmentTexture(const ITexture* texture, uint32_t index)
	{
	}
	void CommandBufferVk::SetCombinedTextureSampler(RGLSamplerPtr sampler, const ITexture* texture, uint32_t index)
	{
		auto castedImage = static_cast<const TextureVk*>(texture);
		VkDescriptorImageInfo imginfo{
					.sampler = std::static_pointer_cast<SamplerVk>(sampler)->sampler,
					.imageView = castedImage->vkImageView,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet writeinfo{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = VK_NULL_HANDLE,
				.dstBinding = index,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &imginfo,
				.pBufferInfo = nullptr,
				.pTexelBufferView = nullptr
		};
		owningQueue->owningDevice->vkCmdPushDescriptorSetKHR(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			currentRenderPipeline->pipelineLayout->layout,
			0,
			1,
			&writeinfo
		);

		//vkUpdateDescriptorSets(currentRenderPipeline->owningDevice->device, 1, &writeinfo, 0, nullptr);
		if (castedImage->owningSwapchain) {
			swapchainsToSignal.insert(castedImage->owningSwapchain);
		}

	}
	void CommandBufferVk::Draw(uint32_t nVertices, const DrawInstancedConfig& config)
	{
		vkCmdDraw(commandBuffer, nVertices, config.nInstances, config.startVertex, config.firstInstance);
	}
	void CommandBufferVk::DrawIndexed(uint32_t nIndices, const DrawIndexedInstancedConfig& config)
	{
		vkCmdDrawIndexed(commandBuffer, nIndices, config.nInstances, config.firstIndex, config.startVertex, config.firstInstance);
	}
	void CommandBufferVk::TransitionResource(const ITexture* texture, RGL::ResourceLayout current, RGL::ResourceLayout target, TransitionPosition position)
	{
		auto img = static_cast<const TextureVk*>(texture);

		auto decideFlagsForBegin = [img]() {
			VkAccessFlags writeFlags = 0;
			if (img->createdConfig.aspect.HasColor) {
				writeFlags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			}
			if (img->createdConfig.aspect.HasDepth || img->createdConfig.aspect.HasDepth) {
				writeFlags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			}
			return writeFlags;
		};

		auto decideFlagsForEnd = [img]() {
			VkPipelineStageFlags writeFlags = 0;
			if (img->createdConfig.aspect.HasColor) {
				writeFlags |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			}
			if (img->createdConfig.aspect.HasDepth || img->createdConfig.aspect.HasDepth) {
				writeFlags |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			}
			return writeFlags;
		};

		auto beginFlags = decideFlagsForBegin();
		auto endFlags = decideFlagsForEnd();	

		encodeResourceTransition(commandBuffer,
			img->vkImage,
			position == TransitionPosition::Top ? VK_ACCESS_NONE : beginFlags,
			position == TransitionPosition::Top ? beginFlags : VK_ACCESS_NONE,
			rgl2vkImageLayout(current),
			rgl2vkImageLayout(target),
			img->createdAspectVk,
			position == TransitionPosition::Top ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : endFlags,
			position == TransitionPosition::Top? endFlags : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
		);
	}
	void CommandBufferVk::CopyTextureToBuffer(RGL::ITexture* sourceTexture, const Rect& sourceRect, size_t offset, RGLBufferPtr destBuffer)
	{
		auto casted = static_cast<TextureVk*>(sourceTexture);
		auto castedDest = std::static_pointer_cast<BufferVk>(destBuffer);
		VkBufferImageCopy region{
			.bufferOffset = 0,
			.bufferRowLength = 0,
			.bufferImageHeight = 0,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
			.imageOffset = {
				.x = sourceRect.offset[0],
				.y = sourceRect.offset[1],
				.z = 0
			},
			.imageExtent = {
				.width = sourceRect.extent[0],
				.height = sourceRect.extent[1],
				.depth = 1
			}	
		};

		vkCmdCopyImageToBuffer(commandBuffer, casted->vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, castedDest->buffer, 1, &region);
	}
	void CommandBufferVk::SetViewport(const Viewport& viewport)
	{
		VkViewport vp{
			.x = viewport.x,
			.y = viewport.height - viewport.y,
			.width = viewport.width,
			.height = -viewport.height, // make Vulkan a Y-up system
			.minDepth = viewport.minDepth,
			.maxDepth = viewport.maxDepth
		};
		vkCmdSetViewport(commandBuffer, 0, 1, &vp);
	}
	void CommandBufferVk::SetScissor(const Rect& scissorin)
	{
		VkRect2D scissor{
		.offset = {scissorin.offset[0], scissorin.offset[1]},
		.extent = {scissorin.extent[0], scissorin.extent[1]},
		};
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
	}
	void CommandBufferVk::Commit(const CommitConfig& config)
	{
		owningQueue->Submit(this, config);
		swapchainsToSignal.clear();
	}
	void CommandBufferVk::SetResourceBarrier(const ResourceBarrierConfig& config)
	{
		stackarray(bufferBarriers, VkBufferMemoryBarrier2, config.buffers.size());

		uint32_t i = 0;
		for (const auto& bufferBase : config.buffers) {
			auto buffer = std::static_pointer_cast<BufferVk>(bufferBase);
			auto owningDeviceFamily = buffer->owningDevice->indices.graphicsFamily.value();
			bufferBarriers[i] = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				.pNext = nullptr,
				.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT ,
				.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
				.srcQueueFamilyIndex = owningDeviceFamily,
				.dstQueueFamilyIndex = owningDeviceFamily,
				.buffer = buffer->buffer,
				.offset = 0,
				.size = buffer->getBufferSize()
			};
			i++;
		}


		stackarray(textureBarriers, VkImageMemoryBarrier2, config.textures.size());
		i = 0;
		for (const auto& textureBase : config.textures) {
			auto image = std::static_pointer_cast<TextureVk>(textureBase);
			textureBarriers[i] = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.pNext = nullptr,
				.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,	// if these are set to the same value, no transition is executed
				.newLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.srcQueueFamilyIndex = 0,
				.dstQueueFamilyIndex = 0,
				.image = image->vkImage,
				.subresourceRange = {
					.aspectMask = image->createdAspectVk,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				}
			};
			i++;
		}

		VkDependencyInfo depInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.pNext = nullptr,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
			.memoryBarrierCount = 0,
			.pMemoryBarriers = nullptr,
			.bufferMemoryBarrierCount = static_cast<uint32_t>(config.buffers.size()),
			.pBufferMemoryBarriers = bufferBarriers,
			.imageMemoryBarrierCount = static_cast<uint32_t>(config.textures.size()),
			.pImageMemoryBarriers = textureBarriers
		};
		vkCmdPipelineBarrier2(
			commandBuffer,
			&depInfo
		);
	}
	void CommandBufferVk::SetRenderPipelineBarrier(const PipelineBarrierConfig& config)
	{
		VkPipelineStageFlagBits2 stageFlags = 0;
		if (config.Vertex) {
			stageFlags |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
		}
		if (config.Fragment) {
			stageFlags |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		}
		if (config.Compute) {
			stageFlags |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		}

		VkMemoryBarrier2 memBarrier{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.pNext = nullptr,
			.srcStageMask = 0,				// sync before
			.srcAccessMask = 0,				// access before
			.dstStageMask = stageFlags,		// sync after
			.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,				// access after
		};

		VkDependencyInfo depInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.pNext = nullptr,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &memBarrier,
			.bufferMemoryBarrierCount = 0,
			.imageMemoryBarrierCount = 0,
		};

		vkCmdPipelineBarrier2(
			commandBuffer,
			&depInfo
		);
	}
	void CommandBufferVk::ExecuteIndirect(const IndirectConfig& config)
	{
		const auto buffer = std::static_pointer_cast<BufferVk>(config.indirectBuffer);
		vkCmdDrawIndirect(commandBuffer,
			buffer->buffer,
			config.offsetIntoBuffer,
			config.nDraws,
			sizeof(IndirectCommand)
		);
	}
	void CommandBufferVk::ExecuteIndirectIndexed(const IndirectConfig& config)
	{
		const auto buffer = std::static_pointer_cast<BufferVk>(config.indirectBuffer);
		vkCmdDrawIndexedIndirect(commandBuffer,
			buffer->buffer,
			config.offsetIntoBuffer,
			config.nDraws,
			sizeof(IndirectIndexedCommand)
		);
	}
	CommandBufferVk::CommandBufferVk(decltype(owningQueue) owningQueue) : owningQueue(owningQueue)
	{
		auto device = owningQueue->owningDevice->device;
		VkCommandBufferAllocateInfo allocInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = owningQueue->owningDevice->commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
		};
		VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer));
	}
	CommandBufferVk::~CommandBufferVk()
	{
		
	}
}

#endif