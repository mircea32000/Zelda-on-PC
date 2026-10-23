#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanFSR.h"

#include <fstream>

std::vector<uint32> RendererShaderVk_CompileCompute(const std::string& source, std::string& errorOut); // RendererShaderVk.cpp

namespace
{
	// motion vectors by reprojection: uv of the same world point in the previous frame minus the current uv
	const char* kMotionVectorShader = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform PC { mat4 reproj; ivec4 size; } pc;
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1, rg16f) uniform writeonly image2D mvOut;
layout(set = 0, binding = 2, r32f) uniform writeonly image2D depthOut;
void main()
{
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= pc.size.x || p.y >= pc.size.y)
		return;
	float d = texelFetch(depthTex, p, 0).r;
	vec2 uv = (vec2(p) + 0.5) / vec2(pc.size.xy);
	bool flipY = (pc.size.z & 1) != 0;
	bool dxClip = (pc.size.z & 2) != 0;
	vec4 ndc = vec4(uv.x * 2.0 - 1.0, flipY ? 1.0 - uv.y * 2.0 : uv.y * 2.0 - 1.0, dxClip ? d : d * 2.0 - 1.0, 1.0);
	vec4 prev = pc.reproj * ndc;
	vec2 prevNdc = prev.xy / max(abs(prev.w), 1e-6) * sign(prev.w);
	vec2 prevUv = vec2(prevNdc.x * 0.5 + 0.5, flipY ? 0.5 - prevNdc.y * 0.5 : prevNdc.y * 0.5 + 0.5);
	imageStore(mvOut, p, vec4(prevUv - uv, 0.0, 0.0));
	imageStore(depthOut, p, vec4(d, 0.0, 0.0, 0.0));
}
)";

	uint32 FindMemory(VkPhysicalDevice pd, uint32 bits, VkMemoryPropertyFlags want)
	{
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		for (uint32 i = 0; i < mp.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
				return i;
		return UINT32_MAX;
	}

	bool IsDepthFormat(VkFormat f)
	{
		return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
			   f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM_S8_UINT;
	}

	void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
		VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
	{
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = srcAccess;
		b.dstAccessMask = dstAccess;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = {aspect, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
	}
}

VulkanFSRInputs::VulkanFSRInputs(VkDevice device, VkPhysicalDevice physicalDevice) : m_device(device), m_physicalDevice(physicalDevice) {}

VulkanFSRInputs::~VulkanFSRInputs()
{
	vkDeviceWaitIdle(m_device);
	DestroyImage(m_color);
	DestroyImage(m_depthCopy);
	DestroyImage(m_depthF);
	DestroyImage(m_mv);
	DestroyImage(m_hudDisplay[0]);
	DestroyImage(m_hudDisplay[1]);
	if (m_pipeline)
		vkDestroyPipeline(m_device, m_pipeline, nullptr);
	if (m_pipelineLayout)
		vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
	if (m_setLayout)
		vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
	if (m_pool)
		vkDestroyDescriptorPool(m_device, m_pool, nullptr);
	if (m_sampler)
		vkDestroySampler(m_device, m_sampler, nullptr);
	if (m_dumpBuffer)
		vkDestroyBuffer(m_device, m_dumpBuffer, nullptr);
	if (m_dumpMemory)
		vkFreeMemory(m_device, m_dumpMemory, nullptr);
}

bool VulkanFSRInputs::CreateImage(Image& img, uint32 w, uint32 h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags viewAspect)
{
	VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = format;
	ii.extent = {w, h, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = usage;
	ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(m_device, &ii, nullptr, &img.image) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(m_device, img.image, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = FindMemory(m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &ai, nullptr, &img.memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(m_device, img.image, img.memory, 0);
	VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = img.image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange = {viewAspect, 0, 1, 0, 1};
	if (vkCreateImageView(m_device, &vi, nullptr, &img.view) != VK_SUCCESS)
		return false;
	img.format = format;
	img.width = w;
	img.height = h;
	return true;
}

void VulkanFSRInputs::DestroyImage(Image& img)
{
	if (img.view)
		vkDestroyImageView(m_device, img.view, nullptr);
	if (img.image)
		vkDestroyImage(m_device, img.image, nullptr);
	if (img.memory)
		vkFreeMemory(m_device, img.memory, nullptr);
	img = Image{};
}

bool VulkanFSRInputs::EnsurePipeline()
{
	if (m_pipeline)
		return true;
	if (m_pipelineFailed)
		return false;
	m_pipelineFailed = true;
	VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(m_device, &si, nullptr, &m_sampler) != VK_SUCCESS)
		return false;
	VkDescriptorSetLayoutBinding b[3]{};
	b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
	b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
	b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = 3;
	li.pBindings = b;
	if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout) != VK_SUCCESS)
		return false;
	VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &m_setLayout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pr;
	if (vkCreatePipelineLayout(m_device, &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS)
		return false;
	std::string err;
	auto spirv = RendererShaderVk_CompileCompute(kMotionVectorShader, err);
	if (spirv.empty())
	{
		cemuLog_log(LogType::Force, "FSR: motion vector shader failed: {}", err);
		return false;
	}
	VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	mi.codeSize = spirv.size() * 4;
	mi.pCode = spirv.data();
	VkShaderModule module;
	if (vkCreateShaderModule(m_device, &mi, nullptr, &module) != VK_SUCCESS)
		return false;
	VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	ci.stage.module = module;
	ci.stage.pName = "main";
	ci.layout = m_pipelineLayout;
	const VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline);
	vkDestroyShaderModule(m_device, module, nullptr);
	if (r != VK_SUCCESS)
		return false;
	VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
	VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pi.maxSets = 2;
	pi.poolSizeCount = 2;
	pi.pPoolSizes = ps;
	if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_pool) != VK_SUCCESS)
		return false;
	m_pipelineFailed = false;
	return true;
}

bool VulkanFSRInputs::CaptureSnapshot(VkCommandBuffer cmd, VkImage color, VkFormat colorFormat, uint32 cw, uint32 ch, VkImage depth, VkFormat depthFormat, uint32 dw,
	uint32 dh)
{
	if (!EnsurePipeline())
		return false;
	const bool depthHasStencil = depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || depthFormat == VK_FORMAT_D16_UNORM_S8_UINT;
	if (!IsDepthFormat(depthFormat))
		return false;
	// (re)create on size / format changes
	if (m_color.format != colorFormat || m_color.width != cw || m_color.height != ch || m_depthCopy.format != depthFormat || m_depthCopy.width != dw ||
		m_depthCopy.height != dh)
	{
		vkDeviceWaitIdle(m_device);
		DestroyImage(m_color);
		DestroyImage(m_depthCopy);
		DestroyImage(m_depthF);
		DestroyImage(m_mv);
		if (m_set)
		{
			vkFreeDescriptorSets(m_device, m_pool, 1, &m_set);
			m_set = VK_NULL_HANDLE;
		}
		bool ok = CreateImage(m_color, cw, ch, colorFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
		ok = ok && CreateImage(m_depthCopy, dw, dh, depthFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
		ok = ok && CreateImage(m_depthF, dw, dh, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
		ok = ok && CreateImage(m_mv, dw, dh, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
		if (!ok)
		{
			cemuLog_log(LogType::Force, "FSR: failed to create input images");
			DestroyImage(m_color);
			DestroyImage(m_depthCopy);
			DestroyImage(m_depthF);
			DestroyImage(m_mv);
			return false;
		}
		VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = m_pool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &m_setLayout;
		vkAllocateDescriptorSets(m_device, &ai, &m_set);
		VkDescriptorImageInfo ii[3]{};
		ii[0] = {m_sampler, m_depthCopy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		ii[1] = {VK_NULL_HANDLE, m_mv.view, VK_IMAGE_LAYOUT_GENERAL};
		ii[2] = {VK_NULL_HANDLE, m_depthF.view, VK_IMAGE_LAYOUT_GENERAL};
		VkWriteDescriptorSet w[3]{};
		for (uint32 i = 0; i < 3; i++)
		{
			w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w[i].dstSet = m_set;
			w[i].dstBinding = i;
			w[i].descriptorCount = 1;
			w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			w[i].pImageInfo = &ii[i];
		}
		vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);
		m_layoutsInitialized = false;
		m_hasSnapshot = false;
	}
	const VkImageLayout colorPrev = m_layoutsInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT | (depthHasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	Barrier(cmd, m_color.image, VK_IMAGE_ASPECT_COLOR_BIT, colorPrev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	Barrier(cmd, m_depthCopy.image, depthAspect, colorPrev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkImageCopy c{};
	c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.extent = {cw, ch, 1};
	vkCmdCopyImage(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	c.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	c.extent = {dw, dh, 1};
	vkCmdCopyImage(cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_depthCopy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	Barrier(cmd, m_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
	Barrier(cmd, m_depthCopy.image, depthAspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	if (!m_layoutsInitialized)
	{
		Barrier(cmd, m_mv.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		Barrier(cmd, m_depthF.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		m_layoutsInitialized = true;
	}
	m_hasSnapshot = true;
	return true;
}

bool VulkanFSRInputs::CaptureColor(VkCommandBuffer cmd, VkImage color, VkFormat colorFormat, uint32 cw, uint32 ch)
{
	if (!m_hasSnapshot || !m_layoutsInitialized || m_color.format != colorFormat || m_color.width != cw || m_color.height != ch)
		return false;
	Barrier(cmd, m_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkImageCopy c{};
	c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.extent = {cw, ch, 1};
	vkCmdCopyImage(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	Barrier(cmd, m_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
	return true;
}

void VulkanFSRInputs::BuildMotionVectors(VkCommandBuffer cmd, const float reproj[16], uint32 flags)
{
	if (!m_hasSnapshot || !m_pipeline)
		return;
	struct
	{
		float m[16]; // column major for GLSL
		sint32 size[4];
	} pc{};
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			pc.m[c * 4 + r] = reproj[r * 4 + c];
	pc.size[0] = (sint32)m_mv.width;
	pc.size[1] = (sint32)m_mv.height;
	pc.size[2] = (sint32)flags;
	// previous readers of the outputs (FSR, dumps) must be done
	VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &m_set, 0, nullptr);
	vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (m_mv.width + 7) / 8, (m_mv.height + 7) / 8, 1);
	mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	m_frameCounter++;
	if (!m_dumpRequest.empty() && !m_dumpRecorded)
		RecordDump(cmd);
}

VkImage VulkanFSRInputs::BuildHudlessDisplay(VkCommandBuffer cmd, VkExtent2D extent, VkFormat format, sint32 x, sint32 y, sint32 w, sint32 h)
{
	if (!m_hasSnapshot)
		return VK_NULL_HANDLE;
	if (m_hudDisplay[0].width != extent.width || m_hudDisplay[0].height != extent.height || m_hudDisplay[0].format != format)
	{
		vkDeviceWaitIdle(m_device);
		for (auto& i : m_hudDisplay)
		{
			DestroyImage(i);
			if (!CreateImage(i, extent.width, extent.height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
				return VK_NULL_HANDLE;
		}
	}
	Image& dst = m_hudDisplay[(m_hudIndex++) & 1];
	Barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	if (x > 0 || y > 0 || x + w < (sint32)extent.width || y + h < (sint32)extent.height)
	{
		VkClearColorValue black{};
		VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		Barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	Barrier(cmd, m_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkImageBlit b{};
	b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	b.srcOffsets[1] = {(sint32)m_color.width, (sint32)m_color.height, 1};
	b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	b.dstOffsets[0] = {std::max(x, 0), std::max(y, 0), 0};
	b.dstOffsets[1] = {std::min(x + w, (sint32)extent.width), std::min(y + h, (sint32)extent.height), 1};
	vkCmdBlitImage(cmd, m_color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
	Barrier(cmd, m_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
		VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	Barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	return dst.image;
}

void VulkanFSRInputs::RecordDump(VkCommandBuffer cmd)
{
	const VkDeviceSize colorBytes = (VkDeviceSize)m_color.width * m_color.height * 4;
	const VkDeviceSize mvBytes = (VkDeviceSize)m_mv.width * m_mv.height * 4;
	const VkDeviceSize depthBytes = (VkDeviceSize)m_depthF.width * m_depthF.height * 4;
	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = colorBytes + mvBytes + depthBytes;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (vkCreateBuffer(m_device, &bi, nullptr, &m_dumpBuffer) != VK_SUCCESS)
		return;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(m_device, m_dumpBuffer, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = FindMemory(m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &ai, nullptr, &m_dumpMemory) != VK_SUCCESS)
	{
		vkDestroyBuffer(m_device, m_dumpBuffer, nullptr);
		m_dumpBuffer = VK_NULL_HANDLE;
		return;
	}
	vkBindBufferMemory(m_device, m_dumpBuffer, m_dumpMemory, 0);
	Barrier(cmd, m_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy c{};
	c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.imageExtent = {m_color.width, m_color.height, 1};
	vkCmdCopyImageToBuffer(cmd, m_color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_dumpBuffer, 1, &c);
	c.bufferOffset = colorBytes;
	c.imageExtent = {m_mv.width, m_mv.height, 1};
	vkCmdCopyImageToBuffer(cmd, m_mv.image, VK_IMAGE_LAYOUT_GENERAL, m_dumpBuffer, 1, &c);
	c.bufferOffset = colorBytes + mvBytes;
	c.imageExtent = {m_depthF.width, m_depthF.height, 1};
	vkCmdCopyImageToBuffer(cmd, m_depthF.image, VK_IMAGE_LAYOUT_GENERAL, m_dumpBuffer, 1, &c);
	Barrier(cmd, m_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
		VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	m_dumpPath = m_dumpRequest;
	m_dumpFrame = m_frameCounter;
	m_dumpRequest.clear();
	m_dumpRecorded = true;
}

void VulkanFSRInputs::FinishDump()
{
	if (!m_dumpRecorded)
		return;
	void* data = nullptr;
	if (vkMapMemory(m_device, m_dumpMemory, 0, VK_WHOLE_SIZE, 0, &data) == VK_SUCCESS)
	{
		// raw dump: header (magic, color w/h/vkformat, mv w/h) + color + mv + depth
		std::ofstream f(_utf8ToPath(m_dumpPath), std::ios::binary | std::ios::trunc);
		const uint32 header[8] = {0x46535232, m_color.width, m_color.height, (uint32)m_color.format, m_mv.width, m_mv.height, (uint32)m_dumpFrame, 0};
		f.write((const char*)header, sizeof(header));
		const size_t total = (size_t)m_color.width * m_color.height * 4 + (size_t)m_mv.width * m_mv.height * 8;
		f.write((const char*)data, total);
		vkUnmapMemory(m_device, m_dumpMemory);
	}
	vkDestroyBuffer(m_device, m_dumpBuffer, nullptr);
	vkFreeMemory(m_device, m_dumpMemory, nullptr);
	m_dumpBuffer = VK_NULL_HANDLE;
	m_dumpMemory = VK_NULL_HANDLE;
	m_dumpRecorded = false;
	if (--m_dumpRemaining > 0)
		m_dumpRequest = fmt::format("{}_{}.bin", m_dumpBase, ++m_dumpIndex);
}
