#pragma once

// Zelda on PC - inputs for AMD FSR 3.1 frame generation (Vulkan).
//
// Per frame the game data FSR needs is captured on the GPU timeline:
//  - the HUD-less scene: the TV color buffer right before the game starts drawing its UI (nw::lyt),
//  - the scene depth buffer at the same moment (the game reuses its memory for UI render targets afterwards),
//  - motion vectors: reprojection of every depth sample with the camera of the previous frame (camera motion is exact,
//    moving objects are left to FSR's optical flow).

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include <array>
#include <string>

class VulkanFSRInputs
{
public:
	VulkanFSRInputs(VkDevice device, VkPhysicalDevice physicalDevice);
	~VulkanFSRInputs();

	struct Image
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;      // color, or depth aspect for depth formats
		VkFormat format = VK_FORMAT_UNDEFINED;
		uint32 width = 0, height = 0;
	};

	// copies the TV color (any layout -> caller restores) and the depth image into the snapshot images. Images must be
	// in TRANSFER_SRC_OPTIMAL layout. Recreates the snapshot images on size / format changes.
	bool CaptureSnapshot(VkCommandBuffer cmd, VkImage color, VkFormat colorFormat, uint32 cw, uint32 ch, VkImage depth, VkFormat depthFormat, uint32 dw, uint32 dh);
	// a frame without the 3D scene: copies only the color into the snapshot (same size / format as the last snapshot,
	// else false); the depth copy of the last scene stays. Color must be in TRANSFER_SRC_OPTIMAL layout.
	bool CaptureColor(VkCommandBuffer cmd, VkImage color, VkFormat colorFormat, uint32 cw, uint32 ch);
	// reprojection matrix: clip position in the previous frame = reproj * (ndc x, ndc y, ndc z, 1) of the current one
	// (row major). flags: 1 = flip y, 2 = depth already in [-1, 1] range mapping (DX clip space: ndc z = depth)
	void BuildMotionVectors(VkCommandBuffer cmd, const float reproj[16], uint32 flags);
	bool HasSnapshot() const { return m_hasSnapshot; }
	// scales the HUD-less snapshot into the area of the backbuffer that shows the game (same as the real frame), into one of
	// two images (FSR reads it when presenting). Returns the image (SHADER_READ_ONLY_OPTIMAL)
	VkImage BuildHudlessDisplay(VkCommandBuffer cmd, VkExtent2D extent, VkFormat format, sint32 x, sint32 y, sint32 w, sint32 h);

	const Image& HudlessColor() const { return m_color; }
	const Image& Depth() const { return m_depthF; }        // R32_SFLOAT copy of the depth
	const Image& MotionVectors() const { return m_mv; }    // RG16F, previous uv - current uv

	// dumps the inputs of `count` consecutive frames to pathPrefix_N.bin (as long as the GPU keeps up)
	void RequestDump(const std::string& pathPrefix, int count = 1) { m_dumpBase = pathPrefix; m_dumpIndex = 0; m_dumpRemaining = count; m_dumpRequest = pathPrefix + "_0.bin"; }
	bool DumpRecorded() const { return m_dumpRecorded; }
	void FinishDump();

private:
	bool CreateImage(Image& img, uint32 w, uint32 h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags viewAspect);
	void DestroyImage(Image& img);
	bool EnsurePipeline();
	void RecordDump(VkCommandBuffer cmd);

	VkDevice m_device;
	VkPhysicalDevice m_physicalDevice;
	Image m_color;      // HUD-less TV color (same format as the TV buffer)
	Image m_depthCopy;  // depth (same format as the game's depth buffer)
	Image m_depthF;     // depth as R32F (FSR input)
	Image m_mv;         // motion vectors (FSR input)
	Image m_hudDisplay[2];
	uint32 m_hudIndex = 0;
	bool m_hasSnapshot = false;
	bool m_layoutsInitialized = false;

	VkSampler m_sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_pipeline = VK_NULL_HANDLE;
	VkDescriptorPool m_pool = VK_NULL_HANDLE;
	VkDescriptorSet m_set = VK_NULL_HANDLE;
	bool m_pipelineFailed = false;

	std::string m_dumpRequest;
	std::string m_dumpPath;
	std::string m_dumpBase;
	int m_dumpIndex = 0, m_dumpRemaining = 0;
	uint64 m_frameCounter = 0, m_dumpFrame = 0;
	bool m_dumpRecorded = false;
	VkBuffer m_dumpBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_dumpMemory = VK_NULL_HANDLE;
};
