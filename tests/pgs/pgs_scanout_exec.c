/* paraLLEl-GS scanout circuit, run as shipped.
 *
 * The scanout shader in the bank (sample_circuit.frag, the VRAM variant)
 * is drawn on a Vulkan device over a VRAM of known contents, and every
 * output pixel is checked against the sample-layer model in
 * pgs_scanout_model.h: for each grid and scanout shape, the layers of
 * the native pixel under the output pixel, averaged where the shape
 * averages them; the base layer where the pixel's super samples are
 * marked invalid; and, with the tent reconstruction on, the [1 2 1]/4
 * tent over the nine neighbouring grid positions, the neighbouring
 * native pixel's samples where a tap crosses an edge. The tent output
 * must also differ from the point-sampled output, and from a tent kept
 * inside the native pixel (negative controls).
 *
 * The VRAM's address swizzle is not modelled: the shader is first run
 * as a native scanout, whose base layer holds each word's own address,
 * which gives the address of every native pixel.
 *
 * Sample values are multiples of 32 so that every average and tent sum
 * the shader forms (an average of two rows under the tent at the least
 * exact) is an exact 8-bit value.
 *
 *   pgs_scanout_exec sample_circuit.spv fullscreen.spv
 *
 * Needs a Vulkan device (llvmpipe does; no subgroup requirement, this is
 * a fragment shader). The code is C89; the Vulkan headers are not, hence
 * the C99 build. From tests/pgs:
 *   cc -O2 -std=c99 -pedantic -Wall -I../../3rdparty/vulkan-headers/include \
 *      pgs_scanout_exec.c -o pgs_scanout_exec -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "pgs_scanout_model.h"

#define VRAM_MASK 0xffffu
#define VRAM_WORDS ((VRAM_MASK + 1) / 4)
#define LAYERS 34            /* base, validity, 32 samples */
#define BASE_LAYER 2
#define FBW 2                /* 128 pixels across */
#define NATIVE_W (FBW * 64)
#define NATIVE_H 64
#define MAX_OUT (NATIVE_W * 4 * NATIVE_H * 8)

struct push
{
   unsigned fbp, fbw, dbx, dby, phase, phase_stride;
};

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static unsigned queue_family;
static VkPhysicalDeviceMemoryProperties mem_props;
static VkCommandPool pool;
static VkDescriptorSetLayout ds_layout;
static VkPipelineLayout pipe_layout;
static VkDescriptorPool ds_pool;
static VkDescriptorSet ds;
static VkShaderModule frag_module, vert_module;
static VkBuffer vram_buf, read_buf;
static VkDeviceMemory vram_mem, read_mem;
static unsigned *vram;           /* mapped */
static unsigned char *readback;  /* mapped */

static unsigned addr_of[NATIVE_H][NATIVE_W];
static unsigned char sample_byte[LAYERS][VRAM_WORDS][4];
static int valid_addr[VRAM_WORDS];

static int check(VkResult r, const char *what)
{
   if (r != VK_SUCCESS)
   {
      fprintf(stderr, "%s: VkResult %d\n", what, (int)r);
      exit(1);
   }
   return 0;
}

static unsigned find_memory(unsigned type_bits, VkMemoryPropertyFlags flags)
{
   unsigned i;
   for (i = 0; i < mem_props.memoryTypeCount; i++)
      if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   fprintf(stderr, "no memory type\n");
   exit(1);
}

static void *read_file(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   void *buf;
   long n;
   if (!f)
   {
      fprintf(stderr, "cannot read %s\n", path);
      exit(1);
   }
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = malloc((size_t)n);
   if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
      exit(1);
   fclose(f);
   *len = (size_t)n;
   return buf;
}

static VkShaderModule load_module(const char *path)
{
   VkShaderModuleCreateInfo info;
   VkShaderModule m;
   size_t len;
   void *code = read_file(path, &len);
   memset(&info, 0, sizeof(info));
   info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   info.codeSize = len;
   info.pCode = (const unsigned *)code;
   check(vkCreateShaderModule(dev, &info, NULL, &m), "vkCreateShaderModule");
   free(code);
   return m;
}

static void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem, void **map)
{
   VkBufferCreateInfo info;
   VkMemoryRequirements req;
   VkMemoryAllocateInfo alloc;
   memset(&info, 0, sizeof(info));
   info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   info.size = size;
   info.usage = usage;
   info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   check(vkCreateBuffer(dev, &info, NULL, buf), "vkCreateBuffer");
   vkGetBufferMemoryRequirements(dev, *buf, &req);
   memset(&alloc, 0, sizeof(alloc));
   alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   alloc.allocationSize = req.size;
   alloc.memoryTypeIndex = find_memory(req.memoryTypeBits,
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   check(vkAllocateMemory(dev, &alloc, NULL, mem), "vkAllocateMemory");
   check(vkBindBufferMemory(dev, *buf, *mem, 0), "vkBindBufferMemory");
   check(vkMapMemory(dev, *mem, 0, size, 0, map), "vkMapMemory");
}

/* Returns 0 when no usable device is there. */
static int init_vulkan(void)
{
   VkApplicationInfo app;
   VkInstanceCreateInfo icreate;
   unsigned count = 0, i;
   VkPhysicalDevice devices[8];
   VkPhysicalDeviceFeatures2 feats2;
   VkPhysicalDevice16BitStorageFeatures storage16;
   VkDeviceQueueCreateInfo qinfo;
   VkDeviceCreateInfo dinfo;
   float prio = 1.0f;
   VkCommandPoolCreateInfo pinfo;
   VkDescriptorSetLayoutBinding binding;
   VkDescriptorSetLayoutCreateInfo lcreate;
   VkPushConstantRange range;
   VkPipelineLayoutCreateInfo plcreate;
   VkDescriptorPoolSize psize;
   VkDescriptorPoolCreateInfo dpcreate;
   VkDescriptorSetAllocateInfo dsalloc;
   VkDescriptorBufferInfo binfo;
   VkWriteDescriptorSet write;

   memset(&app, 0, sizeof(app));
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.pApplicationName = "pgs_scanout_exec";
   app.apiVersion = VK_API_VERSION_1_1;
   memset(&icreate, 0, sizeof(icreate));
   icreate.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   icreate.pApplicationInfo = &app;
   if (vkCreateInstance(&icreate, NULL, &instance) != VK_SUCCESS)
      return 0;
   if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || !count)
      return 0;
   if (count > 8)
      count = 8;
   vkEnumeratePhysicalDevices(instance, &count, devices);

   for (i = 0; i < count; i++)
   {
      VkQueueFamilyProperties families[16];
      unsigned nf = 16, f;
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(devices[i], &props);
      if (props.apiVersion < VK_API_VERSION_1_1)
         continue;
      vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &nf, families);
      for (f = 0; f < nf; f++)
         if (families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
      if (f == nf)
         continue;
      memset(&storage16, 0, sizeof(storage16));
      storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
      memset(&feats2, 0, sizeof(feats2));
      feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
      feats2.pNext = &storage16;
      vkGetPhysicalDeviceFeatures2(devices[i], &feats2);
      if (!feats2.features.shaderInt16 || !storage16.storageBuffer16BitAccess)
         continue;
      phys = devices[i];
      queue_family = f;
      printf("device: %s\n", props.deviceName);
      break;
   }
   if (!phys)
      return 0;

   vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);

   memset(&qinfo, 0, sizeof(qinfo));
   qinfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
   qinfo.queueFamilyIndex = queue_family;
   qinfo.queueCount = 1;
   qinfo.pQueuePriorities = &prio;
   memset(&storage16, 0, sizeof(storage16));
   storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
   storage16.storageBuffer16BitAccess = VK_TRUE;
   memset(&feats2, 0, sizeof(feats2));
   feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
   feats2.pNext = &storage16;
   feats2.features.shaderInt16 = VK_TRUE;
   memset(&dinfo, 0, sizeof(dinfo));
   dinfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   dinfo.pNext = &feats2;
   dinfo.queueCreateInfoCount = 1;
   dinfo.pQueueCreateInfos = &qinfo;
   check(vkCreateDevice(phys, &dinfo, NULL, &dev), "vkCreateDevice");
   vkGetDeviceQueue(dev, queue_family, 0, &queue);

   memset(&pinfo, 0, sizeof(pinfo));
   pinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   pinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   pinfo.queueFamilyIndex = queue_family;
   check(vkCreateCommandPool(dev, &pinfo, NULL, &pool), "vkCreateCommandPool");

   memset(&binding, 0, sizeof(binding));
   binding.binding = 0;
   binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   binding.descriptorCount = 1;
   binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
   memset(&lcreate, 0, sizeof(lcreate));
   lcreate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   lcreate.bindingCount = 1;
   lcreate.pBindings = &binding;
   check(vkCreateDescriptorSetLayout(dev, &lcreate, NULL, &ds_layout), "vkCreateDescriptorSetLayout");

   memset(&range, 0, sizeof(range));
   range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
   range.size = sizeof(struct push);
   memset(&plcreate, 0, sizeof(plcreate));
   plcreate.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
   plcreate.setLayoutCount = 1;
   plcreate.pSetLayouts = &ds_layout;
   plcreate.pushConstantRangeCount = 1;
   plcreate.pPushConstantRanges = &range;
   check(vkCreatePipelineLayout(dev, &plcreate, NULL, &pipe_layout), "vkCreatePipelineLayout");

   create_buffer((VkDeviceSize)LAYERS * (VRAM_MASK + 1), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
         &vram_buf, &vram_mem, (void **)&vram);
   create_buffer((VkDeviceSize)MAX_OUT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         &read_buf, &read_mem, (void **)&readback);

   memset(&psize, 0, sizeof(psize));
   psize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   psize.descriptorCount = 1;
   memset(&dpcreate, 0, sizeof(dpcreate));
   dpcreate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpcreate.maxSets = 1;
   dpcreate.poolSizeCount = 1;
   dpcreate.pPoolSizes = &psize;
   check(vkCreateDescriptorPool(dev, &dpcreate, NULL, &ds_pool), "vkCreateDescriptorPool");
   memset(&dsalloc, 0, sizeof(dsalloc));
   dsalloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   dsalloc.descriptorPool = ds_pool;
   dsalloc.descriptorSetCount = 1;
   dsalloc.pSetLayouts = &ds_layout;
   check(vkAllocateDescriptorSets(dev, &dsalloc, &ds), "vkAllocateDescriptorSets");
   memset(&binfo, 0, sizeof(binfo));
   binfo.buffer = vram_buf;
   binfo.range = VK_WHOLE_SIZE;
   memset(&write, 0, sizeof(write));
   write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   write.dstSet = ds;
   write.descriptorCount = 1;
   write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   write.pBufferInfo = &binfo;
   vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);
   return 1;
}

/* Draw the circuit for `samples` at factors (sx, sy) into a target of
 * (NATIVE_W << sx) x (NATIVE_H << sy), and read it back as RGBA8. */
static void run_circuit(unsigned samples, unsigned sx, unsigned sy, int tent)
{
   const unsigned w = NATIVE_W << sx, h = NATIVE_H << sy;
   VkImageCreateInfo icreate;
   VkImage image;
   VkMemoryRequirements req;
   VkMemoryAllocateInfo alloc;
   VkDeviceMemory imem;
   VkImageViewCreateInfo vcreate;
   VkImageView view;
   VkAttachmentDescription att;
   VkAttachmentReference ref;
   VkSubpassDescription sub;
   VkRenderPassCreateInfo rpcreate;
   VkRenderPass rp;
   VkFramebufferCreateInfo fbcreate;
   VkFramebuffer fb;
   unsigned spec_data[3];
   VkSpecializationMapEntry entries[3];
   VkSpecializationInfo spec;
   VkPipelineShaderStageCreateInfo stages[2];
   VkPipelineVertexInputStateCreateInfo vi;
   VkPipelineInputAssemblyStateCreateInfo ia;
   VkViewport viewport;
   VkRect2D scissor;
   VkPipelineViewportStateCreateInfo vp;
   VkPipelineRasterizationStateCreateInfo rs;
   VkPipelineMultisampleStateCreateInfo ms;
   VkPipelineColorBlendAttachmentState blend_att;
   VkPipelineColorBlendStateCreateInfo blend;
   VkGraphicsPipelineCreateInfo pcreate;
   VkPipeline pipeline;
   VkCommandBufferAllocateInfo cballoc;
   VkCommandBuffer cmd;
   VkCommandBufferBeginInfo begin;
   VkClearValue clear;
   VkRenderPassBeginInfo rpbegin;
   struct push push;
   VkBufferImageCopy region;
   VkSubmitInfo submit;
   unsigned i;

   memset(&icreate, 0, sizeof(icreate));
   icreate.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   icreate.imageType = VK_IMAGE_TYPE_2D;
   icreate.format = VK_FORMAT_R8G8B8A8_UNORM;
   icreate.extent.width = w;
   icreate.extent.height = h;
   icreate.extent.depth = 1;
   icreate.mipLevels = 1;
   icreate.arrayLayers = 1;
   icreate.samples = VK_SAMPLE_COUNT_1_BIT;
   icreate.tiling = VK_IMAGE_TILING_OPTIMAL;
   icreate.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   icreate.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   icreate.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   check(vkCreateImage(dev, &icreate, NULL, &image), "vkCreateImage");
   vkGetImageMemoryRequirements(dev, image, &req);
   memset(&alloc, 0, sizeof(alloc));
   alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   alloc.allocationSize = req.size;
   alloc.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   check(vkAllocateMemory(dev, &alloc, NULL, &imem), "vkAllocateMemory(image)");
   check(vkBindImageMemory(dev, image, imem, 0), "vkBindImageMemory");

   memset(&vcreate, 0, sizeof(vcreate));
   vcreate.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   vcreate.image = image;
   vcreate.viewType = VK_IMAGE_VIEW_TYPE_2D;
   vcreate.format = VK_FORMAT_R8G8B8A8_UNORM;
   vcreate.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   vcreate.subresourceRange.levelCount = 1;
   vcreate.subresourceRange.layerCount = 1;
   check(vkCreateImageView(dev, &vcreate, NULL, &view), "vkCreateImageView");

   memset(&att, 0, sizeof(att));
   att.format = VK_FORMAT_R8G8B8A8_UNORM;
   att.samples = VK_SAMPLE_COUNT_1_BIT;
   att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   memset(&ref, 0, sizeof(ref));
   ref.attachment = 0;
   ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   memset(&sub, 0, sizeof(sub));
   sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   sub.colorAttachmentCount = 1;
   sub.pColorAttachments = &ref;
   memset(&rpcreate, 0, sizeof(rpcreate));
   rpcreate.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
   rpcreate.attachmentCount = 1;
   rpcreate.pAttachments = &att;
   rpcreate.subpassCount = 1;
   rpcreate.pSubpasses = &sub;
   check(vkCreateRenderPass(dev, &rpcreate, NULL, &rp), "vkCreateRenderPass");

   memset(&fbcreate, 0, sizeof(fbcreate));
   fbcreate.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
   fbcreate.renderPass = rp;
   fbcreate.attachmentCount = 1;
   fbcreate.pAttachments = &view;
   fbcreate.width = w;
   fbcreate.height = h;
   fbcreate.layers = 1;
   check(vkCreateFramebuffer(dev, &fbcreate, NULL, &fb), "vkCreateFramebuffer");

   /* Specialization as sample_crtc_circuit sets it: PSM, the VRAM mask,
    * the sample count. PSMCT32 is 0. */
   spec_data[0] = 0;
   spec_data[1] = VRAM_MASK;
   spec_data[2] = samples;
   for (i = 0; i < 3; i++)
   {
      entries[i].constantID = i;
      entries[i].offset = i * 4;
      entries[i].size = 4;
   }
   memset(&spec, 0, sizeof(spec));
   spec.mapEntryCount = 3;
   spec.pMapEntries = entries;
   spec.dataSize = sizeof(spec_data);
   spec.pData = spec_data;

   memset(stages, 0, sizeof(stages));
   stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
   stages[0].module = vert_module;
   stages[0].pName = "main";
   stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
   stages[1].module = frag_module;
   stages[1].pName = "main";
   stages[1].pSpecializationInfo = &spec;

   memset(&vi, 0, sizeof(vi));
   vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
   memset(&ia, 0, sizeof(ia));
   ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
   ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   memset(&viewport, 0, sizeof(viewport));
   viewport.width = (float)w;
   viewport.height = (float)h;
   viewport.maxDepth = 1.0f;
   memset(&scissor, 0, sizeof(scissor));
   scissor.extent.width = w;
   scissor.extent.height = h;
   memset(&vp, 0, sizeof(vp));
   vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
   vp.viewportCount = 1;
   vp.pViewports = &viewport;
   vp.scissorCount = 1;
   vp.pScissors = &scissor;
   memset(&rs, 0, sizeof(rs));
   rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
   rs.polygonMode = VK_POLYGON_MODE_FILL;
   rs.cullMode = VK_CULL_MODE_NONE;
   rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
   rs.lineWidth = 1.0f;
   memset(&ms, 0, sizeof(ms));
   ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
   ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   memset(&blend_att, 0, sizeof(blend_att));
   blend_att.colorWriteMask = 0xf;
   memset(&blend, 0, sizeof(blend));
   blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
   blend.attachmentCount = 1;
   blend.pAttachments = &blend_att;
   memset(&pcreate, 0, sizeof(pcreate));
   pcreate.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
   pcreate.stageCount = 2;
   pcreate.pStages = stages;
   pcreate.pVertexInputState = &vi;
   pcreate.pInputAssemblyState = &ia;
   pcreate.pViewportState = &vp;
   pcreate.pRasterizationState = &rs;
   pcreate.pMultisampleState = &ms;
   pcreate.pColorBlendState = &blend;
   pcreate.layout = pipe_layout;
   pcreate.renderPass = rp;
   check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pcreate, NULL, &pipeline), "vkCreateGraphicsPipelines");

   memset(&cballoc, 0, sizeof(cballoc));
   cballoc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cballoc.commandPool = pool;
   cballoc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cballoc.commandBufferCount = 1;
   check(vkAllocateCommandBuffers(dev, &cballoc, &cmd), "vkAllocateCommandBuffers");
   memset(&begin, 0, sizeof(begin));
   begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

   memset(&clear, 0, sizeof(clear));
   memset(&rpbegin, 0, sizeof(rpbegin));
   rpbegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   rpbegin.renderPass = rp;
   rpbegin.framebuffer = fb;
   rpbegin.renderArea.extent.width = w;
   rpbegin.renderArea.extent.height = h;
   rpbegin.clearValueCount = 1;
   rpbegin.pClearValues = &clear;
   vkCmdBeginRenderPass(cmd, &rpbegin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &ds, 0, NULL);
   /* As the renderer packs it: the scale log2s in bits 16 and 20, the
    * tent flag in bit 24, a progressive frame's stride of 1 below. */
   push.fbp = 0;
   push.fbw = FBW;
   push.dbx = 0;
   push.dby = 0;
   push.phase = 0;
   push.phase_stride = 1u | (sx << 16) | (sy << 20) | ((unsigned)(tent != 0) << 24);
   vkCmdPushConstants(cmd, pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);

   memset(&region, 0, sizeof(region));
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   region.imageSubresource.layerCount = 1;
   region.imageExtent.width = w;
   region.imageExtent.height = h;
   region.imageExtent.depth = 1;
   vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_buf, 1, &region);
   check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

   memset(&submit, 0, sizeof(submit));
   submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   submit.commandBufferCount = 1;
   submit.pCommandBuffers = &cmd;
   check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
   check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

   vkFreeCommandBuffers(dev, pool, 1, &cmd);
   vkDestroyPipeline(dev, pipeline, NULL);
   vkDestroyFramebuffer(dev, fb, NULL);
   vkDestroyRenderPass(dev, rp, NULL);
   vkDestroyImageView(dev, view, NULL);
   vkDestroyImage(dev, image, NULL);
   vkFreeMemory(dev, imem, NULL);
}

static unsigned hash32(unsigned x)
{
   x ^= x >> 16;
   x *= 0x7feb352du;
   x ^= x >> 15;
   x *= 0x846ca68bu;
   x ^= x >> 16;
   return x;
}

/* The base layer carries each word's address, a nibble to a byte so the
 * scanout's RGBA8 gives it back; the validity layer is all ones; every
 * sample layer holds a multiple of 32 in each byte. */
static void fill_vram(void)
{
   unsigned a, l, k;
   for (a = 0; a < VRAM_WORDS; a++)
   {
      for (k = 0; k < 4; k++)
         sample_byte[0][a][k] = (unsigned char)(((a >> (4 * k)) & 15u) << 4);
      for (k = 0; k < 4; k++)
         sample_byte[1][a][k] = 0xff;
      for (l = BASE_LAYER; l < LAYERS; l++)
         for (k = 0; k < 4; k++)
            sample_byte[l][a][k] = (unsigned char)((hash32(a * 977u + l * 131u + k) & 7u) << 5);
      valid_addr[a] = 1;
   }
   for (l = 0; l < LAYERS; l++)
      for (a = 0; a < VRAM_WORDS; a++)
         vram[l * VRAM_WORDS + a] = (unsigned)sample_byte[l][a][0] | ((unsigned)sample_byte[l][a][1] << 8) |
                                    ((unsigned)sample_byte[l][a][2] << 16) | ((unsigned)sample_byte[l][a][3] << 24);
}

/* What the circuit reads for grid position (gx, gy) of the output at
 * factors (sx, sy), with the native pixel taken from `nx, ny` (the
 * position's own unless the caller keeps it elsewhere). */
static void model_point(unsigned samples, unsigned sx, unsigned sy, unsigned nx, unsigned ny,
                        unsigned gx, unsigned gy, double *out)
{
   const unsigned a = addr_of[ny][nx];
   unsigned layers[8], n, i, k;
   for (k = 0; k < 4; k++)
      out[k] = 0.0;
   if (!valid_addr[a])
   {
      for (k = 0; k < 4; k++)
         out[k] = sample_byte[0][a][k];
      return;
   }
   n = scanout_layers(samples, sx, sy, gx, gy, layers);
   for (i = 0; i < n; i++)
      for (k = 0; k < 4; k++)
         out[k] += sample_byte[BASE_LAYER + layers[i]][a][k];
   for (k = 0; k < 4; k++)
      out[k] /= n;
}

/* The tent over the nine grid positions around (X, Y); with `wrap` the
 * taps stay in the centre's native pixel (negative control). */
static void model_tent(unsigned samples, unsigned sx, unsigned sy, unsigned X, unsigned Y, int wrap, double *out)
{
   static const double w[3] = { 0.25, 0.5, 0.25 };
   int dx, dy;
   unsigned k;
   for (k = 0; k < 4; k++)
      out[k] = 0.0;
   for (dy = -1; dy <= 1; dy++)
      for (dx = -1; dx <= 1; dx++)
      {
         int Gx = (int)X + dx, Gy = (int)Y + dy;
         double v[4];
         unsigned nx, ny;
         if (Gx < 0)
            Gx = 0;
         if (Gy < 0)
            Gy = 0;
         nx = wrap ? X >> sx : (unsigned)Gx >> sx;
         ny = wrap ? Y >> sy : (unsigned)Gy >> sy;
         model_point(samples, sx, sy, nx, ny, (unsigned)Gx & ((1u << sx) - 1u), (unsigned)Gy & ((1u << sy) - 1u), v);
         for (k = 0; k < 4; k++)
            out[k] += w[dx + 1] * w[dy + 1] * v[k];
      }
}

/* Mismatching pixels between the readback and a model; the last column
 * and row are left out, where a tent tap reads past the region. */
static unsigned count_mismatches(unsigned samples, unsigned sx, unsigned sy, int model, int report)
{
   const unsigned w = NATIVE_W << sx, h = NATIVE_H << sy;
   unsigned X, Y, k, bad = 0;
   for (Y = 0; Y + 1 < h; Y++)
      for (X = 0; X + 1 < w; X++)
      {
         double e[4];
         const unsigned char *got = readback + (Y * w + X) * 4;
         int diff = 0;
         if (model == 0)
            model_point(samples, sx, sy, X >> sx, Y >> sy, X & ((1u << sx) - 1u), Y & ((1u << sy) - 1u), e);
         else
            model_tent(samples, sx, sy, X, Y, model == 2, e);
         for (k = 0; k < 4; k++)
            if ((int)(e[k] + 0.5) != got[k])
               diff = 1;
         if (diff)
         {
            if (report && bad < 4)
               printf("    %u samples at %u,%u output %u,%u: got %02x%02x%02x%02x, model %.1f %.1f %.1f %.1f\n",
                     samples, sx, sy, X, Y, got[0], got[1], got[2], got[3], e[0], e[1], e[2], e[3]);
            bad++;
         }
      }
   return bad;
}

int main(int argc, char **argv)
{
   static const struct
   {
      unsigned samples, sx, sy;
      int tent;
   } shapes[] = {
      { 4, 1, 1, 0 }, { 8, 1, 1, 0 }, { 16, 1, 1, 0 }, { 32, 1, 1, 0 },
      { 8, 1, 2, 0 }, { 16, 1, 2, 0 }, { 32, 1, 2, 0 },
      { 16, 2, 2, 0 }, { 16, 2, 2, 1 },
      { 32, 2, 2, 0 }, { 32, 2, 2, 1 },
      { 32, 2, 3, 0 }, { 32, 2, 3, 1 }
   };
   int fail = 0;
   unsigned x, y, i;

   (void)sample_point; /* the model's, for pgs_field_scanout */
   if (argc != 3)
   {
      fprintf(stderr, "usage: %s sample_circuit.spv fullscreen.spv\n", argv[0]);
      return 2;
   }
   if (!init_vulkan())
   {
      printf("scanout circuit: NOT RUN (no Vulkan 1.1 device with 16-bit storage)\n");
      return 0;
   }
   frag_module = load_module(argv[1]);
   vert_module = load_module(argv[2]);
   fill_vram();

   /* The native scanout hands back each pixel's address. */
   run_circuit(1, 0, 0, 0);
   for (y = 0; y < NATIVE_H; y++)
      for (x = 0; x < NATIVE_W; x++)
      {
         const unsigned char *p = readback + (y * NATIVE_W + x) * 4;
         unsigned a = 0, k;
         for (k = 0; k < 4; k++)
            a |= (unsigned)(p[k] >> 4) << (4 * k);
         if (a >= VRAM_WORDS)
         {
            printf("  native pixel %u,%u reads word %u, past the VRAM\n", x, y, a);
            fail++;
            a = 0;
         }
         addr_of[y][x] = a;
      }
   /* the swizzle is a permutation: no two pixels at one word */
   {
      static int seen[VRAM_WORDS];
      for (y = 0; y < NATIVE_H; y++)
         for (x = 0; x < NATIVE_W; x++)
         {
            if (seen[addr_of[y][x]]++)
            {
               printf("  native pixel %u,%u shares word %u\n", x, y, addr_of[y][x]);
               fail++;
            }
         }
   }
   if (fail)
      return 1;

   /* One native pixel with its super samples marked invalid: the base
    * layer must come back for it, and for the taps that reach it. */
   valid_addr[addr_of[7][5]] = 0;
   vram[VRAM_WORDS + addr_of[7][5]] = 0;

   for (i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++)
   {
      const unsigned samples = shapes[i].samples, sx = shapes[i].sx, sy = shapes[i].sy;
      unsigned bad;
      run_circuit(samples, sx, sy, shapes[i].tent);
      if (!shapes[i].tent)
      {
         bad = count_mismatches(samples, sx, sy, 0, 1);
         printf("  %2u samples at %u,%u: %s\n", samples, sx, sy, bad ? "MISMATCH" : "ok");
         fail += bad != 0;
      }
      else
      {
         bad = count_mismatches(samples, sx, sy, 1, 1);
         printf("  %2u samples at %u,%u, tent: %s\n", samples, sx, sy, bad ? "MISMATCH" : "ok");
         fail += bad != 0;
         if (!count_mismatches(samples, sx, sy, 0, 0))
         {
            printf("  %2u samples at %u,%u: negative control: the tent output is the point-sampled output\n", samples, sx, sy);
            fail++;
         }
         if (!count_mismatches(samples, sx, sy, 2, 0))
         {
            printf("  %2u samples at %u,%u: negative control: a tent kept inside the native pixel matches\n", samples, sx, sy);
            fail++;
         }
      }
   }

   vkDeviceWaitIdle(dev);
   vkDestroyShaderModule(dev, frag_module, NULL);
   vkDestroyShaderModule(dev, vert_module, NULL);
   vkDestroyDescriptorPool(dev, ds_pool, NULL);
   vkDestroyPipelineLayout(dev, pipe_layout, NULL);
   vkDestroyDescriptorSetLayout(dev, ds_layout, NULL);
   vkDestroyBuffer(dev, vram_buf, NULL);
   vkFreeMemory(dev, vram_mem, NULL);
   vkDestroyBuffer(dev, read_buf, NULL);
   vkFreeMemory(dev, read_mem, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(instance, NULL);

   printf("scanout circuit: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
