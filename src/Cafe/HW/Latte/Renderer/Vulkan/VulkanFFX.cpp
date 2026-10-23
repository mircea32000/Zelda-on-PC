#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanFFX.h"

#if BOOST_OS_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifndef _WIN32
#define __declspec(x) __attribute__((visibility("default")))
#endif
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/vk/ffx_api_vk.h>
#ifndef _WIN32
#undef __declspec
#endif

namespace
{
	PfnFfxCreateContext Create(void* p) { return (PfnFfxCreateContext)p; }
	PfnFfxDestroyContext Destroy(void* p) { return (PfnFfxDestroyContext)p; }
	PfnFfxConfigure Configure(void* p) { return (PfnFfxConfigure)p; }
	PfnFfxQuery Query(void* p) { return (PfnFfxQuery)p; }
	PfnFfxDispatch Dispatch(void* p) { return (PfnFfxDispatch)p; }

	VkQueueInfoFFXAPI ToFFX(const VulkanFFX::QueueInfo& q)
	{
		VkQueueInfoFFXAPI r{};
		r.queue = q.queue;
		r.familyIndex = q.family;
		r.submitFunc = nullptr;
		return r;
	}

	FfxApiResource ImageResource(VkImage image, VkFormat format, uint32 w, uint32 h, uint32 state, bool uav)
	{
		FfxApiResource r{};
		r.resource = (void*)image;
		r.state = state;
		r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
		r.description.format = ffxApiGetSurfaceFormatVK(format);
		r.description.width = w;
		r.description.height = h;
		r.description.depth = 1;
		r.description.mipCount = 1;
		r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
		r.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY | (uav ? FFX_API_RESOURCE_USAGE_UAV : 0);
		return r;
	}

	ffxReturnCode_t FrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* user);

	struct CallbackContext
	{
		PfnFfxDispatch dispatch;
		ffxContext* context;
		bool* resetPending;
	} s_callback{};

	ffxReturnCode_t FrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* user)
	{
		auto* c = (CallbackContext*)user;
		if (*c->resetPending)
		{
			params->reset = true;
			*c->resetPending = false;
		}
		return c->dispatch(c->context, &params->header);
	}
}

VulkanFFX& VulkanFFX::Get()
{
	static VulkanFFX s;
	return s;
}

bool VulkanFFX::Load()
{
	if (m_loadTried)
		return m_loaded;
	m_loadTried = true;
#if BOOST_OS_WINDOWS
	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring dir(path);
	dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
	HMODULE mod = LoadLibraryW((dir + L"amd_fidelityfx_vk.dll").c_str());
	if (!mod)
	{
		cemuLog_log(LogType::Force, "FSR: amd_fidelityfx_vk.dll not found, frame generation unavailable");
		return false;
	}
	m_module = mod;
	m_fnCreate = (void*)GetProcAddress(mod, "ffxCreateContext");
	m_fnDestroy = (void*)GetProcAddress(mod, "ffxDestroyContext");
	m_fnConfigure = (void*)GetProcAddress(mod, "ffxConfigure");
	m_fnQuery = (void*)GetProcAddress(mod, "ffxQuery");
	m_fnDispatch = (void*)GetProcAddress(mod, "ffxDispatch");
#else
	void* mod = dlopen("libamd_fidelityfx_vk.so", RTLD_NOW | RTLD_LOCAL);
	if (!mod)
	{
		cemuLog_log(LogType::Force, "FSR: libamd_fidelityfx_vk.so not found, frame generation unavailable");
		return false;
	}
	m_module = mod;
	m_fnCreate = dlsym(mod, "ffxCreateContext");
	m_fnDestroy = dlsym(mod, "ffxDestroyContext");
	m_fnConfigure = dlsym(mod, "ffxConfigure");
	m_fnQuery = dlsym(mod, "ffxQuery");
	m_fnDispatch = dlsym(mod, "ffxDispatch");
#endif
	m_loaded = m_fnCreate && m_fnDestroy && m_fnConfigure && m_fnQuery && m_fnDispatch;
	cemuLog_log(LogType::Force, "FSR: amd_fidelityfx_vk.dll {}", m_loaded ? "loaded" : "is missing exports");
	return m_loaded;
}

void VulkanFFX::SetQueues(const QueueInfo& game, const QueueInfo& present, const QueueInfo& acquire, const QueueInfo& asyncCompute)
{
	m_game = game;
	m_present = present;
	m_acquire = acquire;
	m_compute = asyncCompute;
}

bool VulkanFFX::CreateSwapchain(VkPhysicalDevice physicalDevice, VkDevice device, VkSwapchainKHR* swapchain, const VkSwapchainCreateInfoKHR& createInfo)
{
	if (!m_loaded || !HasQueues())
		return false;
	DestroySwapchain();
	ffxCreateContextDescFrameGenerationSwapChainVK desc{};
	desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
	desc.physicalDevice = physicalDevice;
	desc.device = device;
	desc.swapchain = swapchain;
	desc.allocator = nullptr;
	desc.createInfo = createInfo;
	desc.createInfo.oldSwapchain = *swapchain;
	desc.gameQueue = ToFFX(m_game);
	desc.asyncComputeQueue = ToFFX(m_compute);
	desc.presentQueue = ToFFX(m_present);
	desc.imageAcquireQueue = ToFFX(m_acquire);
	ffxContext ctx = nullptr;
	const ffxReturnCode_t r = Create(m_fnCreate)(&ctx, &desc.header, nullptr);
	if (r != FFX_API_RETURN_OK)
	{
		cemuLog_log(LogType::Force, "FSR: frame interpolation swapchain creation failed ({})", r);
		return false;
	}
	ffxQueryDescSwapchainReplacementFunctionsVK q{};
	q.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
	Query(m_fnQuery)(&ctx, &q.header);
	m_swapchainContext = ctx;
	m_swapchain = *swapchain;
	m_fnGetImages = (void*)q.pOutGetSwapchainImagesKHR;
	m_fnAcquire = (void*)q.pOutAcquireNextImageKHR;
	m_fnPresent = (void*)q.pOutQueuePresentKHR;
	m_fnPresentCount = (void*)q.pOutGetLastPresentCountFFXAPI;
	m_resetPending = true;
	cemuLog_log(LogType::Force, "FSR: frame interpolation swapchain created ({}x{})", createInfo.imageExtent.width, createInfo.imageExtent.height);
	return m_fnGetImages && m_fnAcquire && m_fnPresent;
}

void VulkanFFX::DestroySwapchain()
{
	DestroyFrameGenContext();
	if (m_swapchainContext)
	{
		ffxContext ctx = m_swapchainContext;
		Destroy(m_fnDestroy)(&ctx, nullptr);
		m_swapchainContext = nullptr;
	}
	m_swapchain = VK_NULL_HANDLE;
}

VkResult VulkanFFX::GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32* count, VkImage* images)
{
	return ((PFN_vkGetSwapchainImagesKHR)m_fnGetImages)(device, swapchain, count, images);
}

VkResult VulkanFFX::AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64 timeout, VkSemaphore semaphore, VkFence fence, uint32* index)
{
	// the replacement returns VK_NOT_READY while all its buffers wait for presentation
	VkResult r;
	const auto start = std::chrono::steady_clock::now();
	while ((r = ((PFN_vkAcquireNextImageKHR)m_fnAcquire)(device, swapchain, timeout, semaphore, fence, index)) == VK_NOT_READY)
	{
		if (std::chrono::steady_clock::now() - start > std::chrono::nanoseconds(timeout))
			return VK_TIMEOUT;
		std::this_thread::yield();
	}
	return r;
}

VkResult VulkanFFX::QueuePresent(VkQueue queue, const VkPresentInfoKHR* info)
{
	return ((PFN_vkQueuePresentKHR)m_fnPresent)(queue, info);
}

uint64 VulkanFFX::PresentCount() const
{
	if (!m_fnPresentCount || !m_swapchain)
		return 0;
	return ((PFN_getLastPresentCountFFXAPI)m_fnPresentCount)(m_swapchain);
}

bool VulkanFFX::EnsureFrameGenContext(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D displaySize, VkExtent2D renderSize, VkFormat backbufferFormat,
	VkFormat hudlessFormat)
{
	if (m_fgContext && m_fgDisplay.width == displaySize.width && m_fgDisplay.height == displaySize.height && m_fgRender.width >= renderSize.width &&
		m_fgRender.height >= renderSize.height && m_fgBackbuffer == backbufferFormat && m_fgHudless == hudlessFormat)
		return true;
	DestroyFrameGenContext();
	ffxCreateBackendVKDesc backend{};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
	backend.vkDevice = device;
	backend.vkPhysicalDevice = physicalDevice;
	backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
	ffxCreateContextDescFrameGenerationHudless hudless{};
	hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
	hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(hudlessFormat);
	ffxCreateContextDescFrameGeneration desc{};
	desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	desc.header.pNext = &backend.header;
	backend.header.pNext = &hudless.header;
	desc.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT; // interpolation on the async compute queue, off the game's queue
	desc.displaySize = {displaySize.width, displaySize.height};
	desc.maxRenderSize = {std::max(renderSize.width, displaySize.width), std::max(renderSize.height, displaySize.height)};
	desc.backBufferFormat = ffxApiGetSurfaceFormatVK(backbufferFormat);
	ffxContext ctx = nullptr;
	const ffxReturnCode_t r = Create(m_fnCreate)(&ctx, &desc.header, nullptr);
	if (r != FFX_API_RETURN_OK)
	{
		cemuLog_log(LogType::Force, "FSR: frame generation context creation failed ({})", r);
		return false;
	}
	m_fgContext = ctx;
	m_fgDisplay = displaySize;
	m_fgRender = {desc.maxRenderSize.width, desc.maxRenderSize.height};
	m_fgBackbuffer = backbufferFormat;
	m_fgHudless = hudlessFormat;
	m_frameID = 0;
	m_resetPending = true;
	cemuLog_log(LogType::Force, "FSR: frame generation context created (display {}x{}, render {}x{})", displaySize.width, displaySize.height, renderSize.width, renderSize.height);
	return true;
}

void VulkanFFX::DestroyFrameGenContext()
{
	if (!m_fgContext)
		return;
	if (m_swapchainContext)
	{
		// detach from the swapchain before destroying the context
		ffxConfigureDescFrameGeneration cfg{};
		cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		cfg.swapChain = m_swapchain;
		cfg.frameGenerationEnabled = false;
		ffxContext ctx = m_fgContext;
		Configure(m_fnConfigure)(&ctx, &cfg.header);
	}
	ffxContext ctx = m_fgContext;
	Destroy(m_fnDestroy)(&ctx, nullptr);
	m_fgContext = nullptr;
}

bool VulkanFFX::PrepareFrame(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D displaySize, VkFormat backbufferFormat, const FrameInfo& info)
{
	if (!m_swapchainContext)
		return false;
	if (!EnsureFrameGenContext(physicalDevice, device, displaySize, {info.renderWidth, info.renderHeight}, backbufferFormat, info.hudlessFormat))
		return false;
	if (info.reset)
		m_resetPending = true;
	s_callback.dispatch = Dispatch(m_fnDispatch);
	s_callback.context = (ffxContext*)&m_fgContext;
	s_callback.resetPending = &m_resetPending;

	ffxConfigureDescFrameGeneration cfg{};
	cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	cfg.swapChain = m_swapchain;
	cfg.presentCallback = nullptr;
	cfg.presentCallbackUserContext = nullptr;
	cfg.frameGenerationCallback = FrameGenerationCallback;
	cfg.frameGenerationCallbackUserContext = &s_callback;
	cfg.frameGenerationEnabled = true;
	cfg.allowAsyncWorkloads = true;
	cfg.HUDLessColor = ImageResource(info.hudless, info.hudlessFormat, displaySize.width, displaySize.height, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
	cfg.flags = info.debugFlags & 0xFF;
	cfg.onlyPresentGenerated = (info.debugFlags & 0x80000000u) != 0; // dev: show only generated frames
	cfg.generationRect = {0, 0, (int32_t)displaySize.width, (int32_t)displaySize.height};
	cfg.frameID = m_frameID;
	ffxContext ctx = m_fgContext;
	ffxReturnCode_t r = Configure(m_fnConfigure)(&ctx, &cfg.header);
	if (r != FFX_API_RETURN_OK)
	{
		cemuLog_logOnce(LogType::Force, "FSR: configure failed ({})", r);
		return false;
	}

	ffxDispatchDescFrameGenerationPrepareCameraInfo cam{};
	cam.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
	memcpy(cam.cameraPosition, info.position, sizeof(cam.cameraPosition));
	memcpy(cam.cameraUp, info.up, sizeof(cam.cameraUp));
	memcpy(cam.cameraRight, info.right, sizeof(cam.cameraRight));
	memcpy(cam.cameraForward, info.forward, sizeof(cam.cameraForward));
	ffxDispatchDescFrameGenerationPrepare prep{};
	prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
	prep.header.pNext = &cam.header;
	prep.frameID = m_frameID;
	prep.flags = info.debugFlags & 0xFF;
	prep.commandList = (void*)info.cmd;
	prep.renderSize = {info.renderWidth, info.renderHeight};
	prep.jitterOffset = {0.0f, 0.0f};
	prep.motionVectorScale = {(float)info.renderWidth, (float)info.renderHeight};
	prep.frameTimeDelta = info.frameTimeMs;
	prep.cameraNear = info.cameraNear;
	prep.cameraFar = info.cameraFar;
	prep.cameraFovAngleVertical = info.fovY;
	prep.viewSpaceToMetersFactor = 1.0f;
	prep.depth = ImageResource(info.depth, VK_FORMAT_R32_SFLOAT, info.renderWidth, info.renderHeight, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true);
	prep.motionVectors = ImageResource(info.motionVectors, VK_FORMAT_R16G16_SFLOAT, info.renderWidth, info.renderHeight, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true);
	r = Dispatch(m_fnDispatch)(&ctx, &prep.header);
	if (r != FFX_API_RETURN_OK)
	{
		cemuLog_logOnce(LogType::Force, "FSR: prepare dispatch failed ({})", r);
		return false;
	}
	m_frameID++;
	return true;
}

void VulkanFFX::DisableGeneration()
{
	if (!m_fgContext || !m_swapchainContext)
		return;
	ffxConfigureDescFrameGeneration cfg{};
	cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	cfg.swapChain = m_swapchain;
	cfg.frameGenerationEnabled = false;
	cfg.frameID = m_frameID;
	ffxContext ctx = m_fgContext;
	Configure(m_fnConfigure)(&ctx, &cfg.header);
	m_resetPending = true;
}
