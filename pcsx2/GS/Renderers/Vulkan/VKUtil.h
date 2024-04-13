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

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/StringUtil.h"
#include "VKLoader.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <string_view>

bool IsDepthFormat(VkFormat format);
bool IsDepthStencilFormat(VkFormat format);
VkFormat GetLinearFormat(VkFormat format);

// Safe destroy helpers
void SafeDestroyFramebuffer(VkFramebuffer& fb);
void SafeDestroyShaderModule(VkShaderModule& sm);
void SafeDestroyPipeline(VkPipeline& p);
void SafeDestroyPipelineLayout(VkPipelineLayout& pl);
void SafeDestroyDescriptorSetLayout(VkDescriptorSetLayout& dsl);
void SafeDestroyBufferView(VkBufferView& bv);
void SafeDestroyImageView(VkImageView& iv);
void SafeDestroySampler(VkSampler& samp);
void SafeDestroySemaphore(VkSemaphore& sem);
void SafeFreeGlobalDescriptorSet(VkDescriptorSet& ds);

// Wrapper for creating an barrier on a buffer
void BufferMemoryBarrier(VkCommandBuffer command_buffer, VkBuffer buffer, VkAccessFlags src_access_mask,
		VkAccessFlags dst_access_mask, VkDeviceSize offset, VkDeviceSize size, VkPipelineStageFlags src_stage_mask,
		VkPipelineStageFlags dst_stage_mask);

// Adds a structure to a chain.
void AddPointerToChain(void* head, const void* ptr);
