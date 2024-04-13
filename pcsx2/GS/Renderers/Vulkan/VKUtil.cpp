/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "VKUtil.h"
#include "VKContext.h"
#include "VKShaderCache.h"
#include "common/Console.h"
#include "common/StringUtil.h"

#include <cmath>

bool IsDepthFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return true;
		default:
			break;
	}
	return false;
}

bool IsDepthStencilFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return true;
		default:
			break;
	}
	return false;
}

VkFormat GetLinearFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8_SRGB:
			return VK_FORMAT_R8_UNORM;
		case VK_FORMAT_R8G8_SRGB:
			return VK_FORMAT_R8G8_UNORM;
		case VK_FORMAT_R8G8B8_SRGB:
			return VK_FORMAT_R8G8B8_UNORM;
		case VK_FORMAT_R8G8B8A8_SRGB:
			return VK_FORMAT_R8G8B8A8_UNORM;
		case VK_FORMAT_B8G8R8_SRGB:
			return VK_FORMAT_B8G8R8_UNORM;
		case VK_FORMAT_B8G8R8A8_SRGB:
			return VK_FORMAT_B8G8R8A8_UNORM;
		default:
			break;
	}
	return format;
}

void SetViewport(VkCommandBuffer command_buffer, int x, int y, int width, int height,
		float min_depth /*= 0.0f*/, float max_depth /*= 1.0f*/)
{
	const VkViewport vp{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
		static_cast<float>(height), min_depth, max_depth};
	vkCmdSetViewport(command_buffer, 0, 1, &vp);
}

void SetScissor(VkCommandBuffer command_buffer, int x, int y, int width, int height)
{
	const VkRect2D scissor{{x, y}, {static_cast<u32>(width), static_cast<u32>(height)}};
	vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}

void SafeDestroyFramebuffer(VkFramebuffer& fb)
{
	if (fb != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(g_vulkan_context->GetDevice(), fb, nullptr);
		fb = VK_NULL_HANDLE;
	}
}

void SafeDestroyShaderModule(VkShaderModule& sm)
{
	if (sm != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(g_vulkan_context->GetDevice(), sm, nullptr);
		sm = VK_NULL_HANDLE;
	}
}

void SafeDestroyPipeline(VkPipeline& p)
{
	if (p != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(g_vulkan_context->GetDevice(), p, nullptr);
		p = VK_NULL_HANDLE;
	}
}

void SafeDestroyPipelineLayout(VkPipelineLayout& pl)
{
	if (pl != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(g_vulkan_context->GetDevice(), pl, nullptr);
		pl = VK_NULL_HANDLE;
	}
}

void SafeDestroyDescriptorSetLayout(VkDescriptorSetLayout& dsl)
{
	if (dsl != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(g_vulkan_context->GetDevice(), dsl, nullptr);
		dsl = VK_NULL_HANDLE;
	}
}

void SafeDestroyBufferView(VkBufferView& bv)
{
	if (bv != VK_NULL_HANDLE)
	{
		vkDestroyBufferView(g_vulkan_context->GetDevice(), bv, nullptr);
		bv = VK_NULL_HANDLE;
	}
}

void SafeDestroyImageView(VkImageView& iv)
{
	if (iv != VK_NULL_HANDLE)
	{
		vkDestroyImageView(g_vulkan_context->GetDevice(), iv, nullptr);
		iv = VK_NULL_HANDLE;
	}
}

void SafeDestroySampler(VkSampler& samp)
{
	if (samp != VK_NULL_HANDLE)
	{
		vkDestroySampler(g_vulkan_context->GetDevice(), samp, nullptr);
		samp = VK_NULL_HANDLE;
	}
}

void SafeDestroySemaphore(VkSemaphore& sem)
{
	if (sem != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(g_vulkan_context->GetDevice(), sem, nullptr);
		sem = VK_NULL_HANDLE;
	}
}

void SafeFreeGlobalDescriptorSet(VkDescriptorSet& ds)
{
	if (ds != VK_NULL_HANDLE)
	{
		g_vulkan_context->FreeGlobalDescriptorSet(ds);
		ds = VK_NULL_HANDLE;
	}
}

void BufferMemoryBarrier(VkCommandBuffer command_buffer, VkBuffer buffer, VkAccessFlags src_access_mask,
		VkAccessFlags dst_access_mask, VkDeviceSize offset, VkDeviceSize size, VkPipelineStageFlags src_stage_mask,
		VkPipelineStageFlags dst_stage_mask)
{
	VkBufferMemoryBarrier buffer_info = {
		VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, // VkStructureType    sType
		nullptr, // const void*        pNext
		src_access_mask, // VkAccessFlags      srcAccessMask
		dst_access_mask, // VkAccessFlags      dstAccessMask
		VK_QUEUE_FAMILY_IGNORED, // uint32_t           srcQueueFamilyIndex
		VK_QUEUE_FAMILY_IGNORED, // uint32_t           dstQueueFamilyIndex
		buffer, // VkBuffer           buffer
		offset, // VkDeviceSize       offset
		size // VkDeviceSize       size
	};

	vkCmdPipelineBarrier(
			command_buffer, src_stage_mask, dst_stage_mask, 0, 0, nullptr, 1, &buffer_info, 0, nullptr);
}

void AddPointerToChain(void* head, const void* ptr)
{
	VkBaseInStructure* last_st = static_cast<VkBaseInStructure*>(head);
	while (last_st->pNext)
	{
		if (last_st->pNext == ptr)
			return;

		last_st = const_cast<VkBaseInStructure*>(last_st->pNext);
	}

	last_st->pNext = static_cast<const VkBaseInStructure*>(ptr);
}
