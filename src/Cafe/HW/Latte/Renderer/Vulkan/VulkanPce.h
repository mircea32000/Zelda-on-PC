#pragma once

// BotW PC Edition - motion vectors of every object for frame generation (Vulkan).
//
// The scene's vertex shaders run their body a second time with the uniforms the same draw had in the previous frame
// (decompiler: _initPce / pceRunPrev). The draws of the scene's G-buffer pass are recorded every frame (key: shaders,
// index and vertex data, counts) together with their uniforms; in the next frame the uniforms of the matching draw are
// uploaded to the uniform ring buffer (which the shader reads as a storage buffer) and uf_pce tells the shader where they
// are. The pixel shaders write the screen motion to color attachment 6: a velocity image added to the framebuffers of the
// G-buffer pass, cleared before the first of them each frame, read when the motion vectors for FSR are built (pixels
// nothing wrote keep the camera's reprojection).

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include <functional>
#include <span>
#include <string>

class LatteDecompilerShader;
class VKRObjectTextureView;

namespace VulkanPce
{
	constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
	constexpr float kNoVelocity = 32768.0f; // cleared value: no motion written (the shaders write it too when they have none)

	// the draw about to be executed
	void SetDraw(uint32 baseVertex, uint32 instanceCount, uint32 count, MPTR indexData);
	// fills uf_pce of a vertex shader with pceMode (uniformBuf: its uniform var block); velocityPass: the draw goes to a
	// framebuffer with the velocity attachment. upload: data -> offset in the uniform ring buffer. Returns true if uf_pce
	// changed (the block has to be uploaded again)
	// ring: the uniform ring buffer (the remapped uniforms of the previous frame are read where they still are)
	bool FillUniforms(LatteDecompilerShader* vs, uint8* uniformBuf, bool velocityPass, const uint8* ring, const std::function<uint32(std::span<uint8>)>& upload);
	// where the vertex shader's remapped uniforms went in the uniform ring buffer (after FillUniforms)
	void OnVertexUniforms(uint32 remappedRingOffset);
	void OnFrameEnd();

	// the velocity image for framebuffers of this size (created / recreated as needed)
	VKRObjectTextureView* GetVelocityView(uint32 width, uint32 height);
	// before a render pass of a framebuffer with this velocity view begins (outside of a render pass): clears the image
	// once per frame
	void BeginVelocityPass(VkCommandBuffer cmd, VKRObjectTextureView* view);
	// the velocity image if something was drawn into it this frame (layout: attachmentLayout)
	struct VelocityImage
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
		uint32 width = 0, height = 0;
	};
	bool GetVelocityForRead(VelocityImage& out);

	std::string Stats();
	inline std::atomic<uint32> g_asyncPipelines{0}; // dev stat: pipelines compiled while playing (not in the cache)
	inline std::atomic<uint32> g_asyncVelocityPipelines{0}; // (of them: with the velocity attachment)
	inline int g_devMode = 0; // dev: 1 = draws recorded and matched but the shaders get none, 2 = nothing recorded
}
