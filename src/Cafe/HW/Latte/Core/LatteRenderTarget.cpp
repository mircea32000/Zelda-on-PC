#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include <fstream>

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "pcedition/PCEdition.h"
#include "pcedition/PCUI.h"
#include "pcedition/PCOverlay.h"
#include "pcedition/PCSettings.h"
#include "pcedition/PCInput.h"

// Zelda on PC: last presented TV image, re-presented (with the settings menu) while the game is paused
static LatteTextureView* s_pceLastTVView = nullptr;
static std::chrono::steady_clock::time_point s_pceLastPresent{};

void LatteRenderTarget_copyToBackbuffer(LatteTextureView* textureView, bool isPadView);
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "HW/Latte/Renderer/RendererCore.h"
#include "config/ActiveSettings.h"
#include "WindowSystem.h"
#include "Cafe/OS/libs/erreula/erreula.h"
#include "input/InputManager.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"

uint32 prevScissorX = 0;
uint32 prevScissorY = 0;
uint32 prevScissorWidth = 0;
uint32 prevScissorHeight = 0;

bool hasValidFramebufferAttached = false;

struct LatteMRTQuad
{
	sint32 width;
	sint32 height;
};

struct
{
	LatteMRTQuad currentRenderSize;
	LatteMRTQuad currentEffectiveSize;
	struct
	{
		sint32 width;
		sint32 height;
	}currentGuestViewport;
	bool renderTargetIsResized;
	// tracking
	sint32 rtUpdateListCount;
	LatteTextureView* rtUpdateList[64];
	sint32 rtUpdateListSlice[64];
	sint32 rtUpdateListMip[64];
}sLatteRenderTargetState;

struct
{
	struct
	{
		LatteTextureView* view{};
	}colorBuffer[8];
	struct
	{
		LatteTextureView* view{};
		bool hasStencil{false};
	}depthBuffer;
}sLatteCurrentRendertargets{};

LatteCachedFBO::LatteCachedFBO(uint64 key) : key(key)
{
	for (sint32 i = 0; i < 8; i++)
	{
		LatteTextureView* colorTexView = sLatteCurrentRendertargets.colorBuffer[i].view;
		colorBuffer[i].texture = colorTexView;
		if (colorTexView)
		{
			vectorAppendUnique(colorTexView->list_associatedFbo, this);
			m_referencedTextures.emplace_back(colorTexView->baseTexture);
		}
	}

	if (sLatteCurrentRendertargets.depthBuffer.view)
	{
		LatteTextureView* depthTexView = sLatteCurrentRendertargets.depthBuffer.view;
		depthBuffer.texture = depthTexView;
		depthBuffer.hasStencil = sLatteCurrentRendertargets.depthBuffer.hasStencil;
		if (depthTexView)
		{
			vectorAppendUnique(depthTexView->list_associatedFbo, this);
			m_referencedTextures.emplace_back(depthTexView->baseTexture);
		}
	}

	calculateEffectiveRenderAreaSize();
}

void LatteMRT::NotifyTextureDeletion(LatteTexture* texture)
{
	for (sint32 i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
	{
		if (sLatteCurrentRendertargets.colorBuffer[i].view && sLatteCurrentRendertargets.colorBuffer[i].view->baseTexture == texture)
		{
			sLatteCurrentRendertargets.colorBuffer[i].view = nullptr;
		}
	}
}

LatteCachedFBO* LatteMRT::CreateCachedFBO(uint64 key)
{
	return g_renderer->rendertarget_createCachedFBO(key);
}

void LatteMRT::DeleteCachedFBO(LatteCachedFBO* cfbo)
{
	// color textures
	for (sint32 i = 0; i < 8; i++)
	{
		if (cfbo->colorBuffer[i].texture == NULL)
			continue;
		cfbo->colorBuffer[i].texture->list_fboLookup.erase(std::remove(cfbo->colorBuffer[i].texture->list_fboLookup.begin(), cfbo->colorBuffer[i].texture->list_fboLookup.end(), cfbo), cfbo->colorBuffer[i].texture->list_fboLookup.end());
		cfbo->colorBuffer[i].texture->list_associatedFbo.erase(std::remove(cfbo->colorBuffer[i].texture->list_associatedFbo.begin(), cfbo->colorBuffer[i].texture->list_associatedFbo.end(), cfbo), cfbo->colorBuffer[i].texture->list_associatedFbo.end());
	}
	// depth texture
	if (cfbo->depthBuffer.texture)
	{
		cfbo->depthBuffer.texture->list_fboLookup.erase(std::remove(cfbo->depthBuffer.texture->list_fboLookup.begin(), cfbo->depthBuffer.texture->list_fboLookup.end(), cfbo), cfbo->depthBuffer.texture->list_fboLookup.end());
		cfbo->depthBuffer.texture->list_associatedFbo.erase(std::remove(cfbo->depthBuffer.texture->list_associatedFbo.begin(), cfbo->depthBuffer.texture->list_associatedFbo.end(), cfbo), cfbo->depthBuffer.texture->list_associatedFbo.end());
	}
	g_renderer->rendertarget_deleteCachedFBO(cfbo);
	delete cfbo;
}

LatteCachedFBO* g_emptyFBO = nullptr;

void LatteMRT::SetColorAttachment(uint32 index, LatteTextureView* view)
{
	sLatteCurrentRendertargets.colorBuffer[index].view = view;
}

void LatteMRT::SetDepthAndStencilAttachment(LatteTextureView* view, bool hasStencil)
{
	sLatteCurrentRendertargets.depthBuffer.view = view;
	sLatteCurrentRendertargets.depthBuffer.hasStencil = hasStencil;
}

LatteTextureView* LatteMRT::GetColorAttachment(uint32 index)
{
	cemu_assert_debug(index < 8);
	return sLatteCurrentRendertargets.colorBuffer[index].view;
}

LatteTextureView* LatteMRT::GetDepthAttachment()
{
	return sLatteCurrentRendertargets.depthBuffer.view;
}

void LatteMRT::ApplyCurrentState()
{
	uint64 key = 0;
	LatteTextureView* fboLookupView = NULL;
	for (sint32 i = 0; i < 8; i++)
	{
		LatteTextureView* colorView = sLatteCurrentRendertargets.colorBuffer[i].view;
		if (colorView)
		{
			key += ((uint64)colorView);
			key = std::rotl<uint64>(key, 5);
			fboLookupView = colorView;
		}
		key = std::rotl<uint64>(key, 7);
	}
	if (sLatteCurrentRendertargets.depthBuffer.view)
	{
		key += ((uint64)sLatteCurrentRendertargets.depthBuffer.view);
		key = std::rotl<uint64>(key, 5);
		key += (sLatteCurrentRendertargets.depthBuffer.hasStencil);
		if (fboLookupView == NULL)
		{
			fboLookupView = sLatteCurrentRendertargets.depthBuffer.view;
		}
	}
	// use fboLookupTexture to find cached FBO
	if (fboLookupView == nullptr)
	{
		if (!g_emptyFBO)
			g_emptyFBO = CreateCachedFBO(key);
		g_renderer->rendertarget_bindFramebufferObject(g_emptyFBO);
		return;
	}
	// look for FBO
	for (auto& fbo : fboLookupView->list_fboLookup)
	{
		if (fbo->key == key)
		{
			// found matching FBO
			g_renderer->rendertarget_bindFramebufferObject(fbo);
			return;
		}
	}
	// create new cached FBO
	LatteCachedFBO* cfbo = CreateCachedFBO(key);
	g_renderer->rendertarget_bindFramebufferObject(cfbo);
	fboLookupView->list_fboLookup.push_back(cfbo);
	// some extra checks to verify that looked up fbo matches active buffers
	cemu_assert_debug(cfbo->colorBuffer[0].texture == sLatteCurrentRendertargets.colorBuffer[0].view);
	cemu_assert_debug(cfbo->colorBuffer[1].texture == sLatteCurrentRendertargets.colorBuffer[1].view);
	cemu_assert_debug(cfbo->colorBuffer[2].texture == sLatteCurrentRendertargets.colorBuffer[2].view);
	cemu_assert_debug(cfbo->depthBuffer.texture == sLatteCurrentRendertargets.depthBuffer.view);
}

void LatteMRT::BindColorBufferOnly(LatteTextureView* view)
{
	cemu_assert_debug(!view->baseTexture->isDepth);
	SetColorAttachment(0, view);
	for (sint32 i = 1; i < 8; i++)
		SetColorAttachment(i, nullptr);
	SetDepthAndStencilAttachment(nullptr, false);
	ApplyCurrentState();
}

void LatteMRT::BindDepthBufferOnly(LatteTextureView* view)
{
	cemu_assert_debug(view->baseTexture->isDepth);
	for (sint32 i = 0; i < 8; i++)
		SetColorAttachment(i, nullptr);
	SetDepthAndStencilAttachment(view, view->baseTexture->hasStencil);
	ApplyCurrentState();
}

LatteTextureView* LatteMRT_CreateDepthBuffer(MPTR depthBufferPhysMem, uint32 width, uint32 height, uint32 pitch, Latte::E_HWTILEMODE tileMode, Latte::E_GX2SURFFMT format, uint32 swizzle, sint32 viewSlice)
{
	LatteTextureView* textureView = LatteTexture_CreateMapping(depthBufferPhysMem, MPTR_NULL, width, height, viewSlice+1, pitch, tileMode, swizzle, 0, 1, viewSlice, 1, format, viewSlice > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, true);
	LatteMRT::SetDepthAndStencilAttachment(textureView, textureView->baseTexture->hasStencil);
	return textureView;
}

sint32 _depthBufferSizeWarningCount = 0;

LatteTextureView* LatteMRT::GetColorAttachmentTexture(uint32 index, bool createNew, bool checkForTextureChanges)
{
	uint32* colorBufferRegBase = LatteGPUState.contextRegister+(mmCB_COLOR0_BASE + index);
	uint32 regColorBufferBase = colorBufferRegBase[mmCB_COLOR0_BASE - mmCB_COLOR0_BASE] & 0xFFFFFF00; // the low 8 bits are ignored? How to Survive seems to rely on this
	uint32 regColorSize = colorBufferRegBase[mmCB_COLOR0_SIZE - mmCB_COLOR0_BASE];
	uint32 regColorInfo = colorBufferRegBase[mmCB_COLOR0_INFO - mmCB_COLOR0_BASE];
	uint32 regColorView = colorBufferRegBase[mmCB_COLOR0_VIEW - mmCB_COLOR0_BASE];
	// decode color buffer reg info
	Latte::E_HWTILEMODE colorBufferTileMode = (Latte::E_HWTILEMODE)((regColorInfo >> 8) & 0xF);
	uint32 numberType = (regColorInfo >> 12) & 7;
	Latte::E_GX2SURFFMT colorBufferFormat = GetColorBufferFormat(index, LatteGPUState.contextNew);

	MPTR colorBufferPhysMem = regColorBufferBase;

	uint32 colorBufferSwizzle = 0;
	if ( Latte::TM_IsMacroTiled(colorBufferTileMode) )
	{
		colorBufferSwizzle = colorBufferPhysMem & 0x700;
		colorBufferPhysMem = colorBufferPhysMem & ~(7 << 8);
	}

	// get view slice and view slice num
	uint32 viewFirstSlice = (regColorView & 0x7FF);
	uint32 viewNumSlices = ((regColorView >> 13) & 0x7FF) - viewFirstSlice + 1;
	if (viewNumSlices != 1)
	{
		debug_printf("viewNumSlices is not 1! (%d)\n", viewNumSlices);
	}
	uint32 colorBufferPitch = (((regColorSize >> 0) & 0x3FF) + 1);
	colorBufferPitch <<= 3;
	uint32 pitchHeight = (((regColorSize >> 10) & 0xFFFFF) + 1);
	pitchHeight <<= 6;
	uint32 colorBufferHeight = pitchHeight / colorBufferPitch;
	uint32 colorBufferWidth = colorBufferPitch;

	// colorbuffer width/height has to be padded to 8/32 alignment but the actual resolution might be smaller
	// use the scissor box as a clue to figure out the original resolution if possible
	if(LatteGPUState.allowFramebufferSizeOptimization)
	{
		uint32 scissorBoxWidth = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_X();
		uint32 scissorBoxHeight = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();
		if (((scissorBoxWidth + 7) & ~7) == colorBufferWidth)
			colorBufferWidth = scissorBoxWidth;
		if (((colorBufferHeight + 31) & ~31) == colorBufferHeight)
			colorBufferHeight = scissorBoxHeight;
	}

	// log resolution changes if the above heuristic takes effect
	// this is useful to find resolutions which need to be updated in gfx pack texture rules
#if 0
	uint32 colorBufferHeight2 = pitchHeight / colorBufferPitch;
	static std::unordered_set<uint64> s_foundColorBufferResMappings;
	if (colorBufferPitch != colorBufferWidth || colorBufferHeight != colorBufferHeight2)
	{
		// only log unique, source and dest resolution. Encode into a key with 16 bits per component
		uint64 resHash = (uint64)colorBufferWidth | ((uint64)colorBufferHeight << 16) | ((uint64)colorBufferPitch << 32) | ((uint64)colorBufferHeight2 << 48);
		if( !s_foundColorBufferResMappings.contains(resHash) )
		{
			s_foundColorBufferResMappings.insert(resHash);
			cemuLog_log(LogType::Force, "[COLORBUFFER-DBG] Using res {}x{} instead of {}x{}", colorBufferWidth, colorBufferHeight, colorBufferPitch, colorBufferHeight2);
		}
	}
#endif

	bool colorBufferWasFound = false;
	sint32 viewFirstMip = 0; // todo

	cemu_assert_debug(viewNumSlices == 1);

	LatteTextureView* colorBufferView = LatteTextureViewLookupCache::lookupSliceEx(colorBufferPhysMem, colorBufferWidth, colorBufferHeight, colorBufferPitch, viewFirstMip, viewFirstSlice, colorBufferFormat, false);

	if (colorBufferView == nullptr)
	{
		// create color buffer view
		colorBufferView = LatteTexture_CreateMapping(colorBufferPhysMem, 0, colorBufferWidth, colorBufferHeight, (viewFirstSlice + viewNumSlices), colorBufferPitch, colorBufferTileMode, colorBufferSwizzle>>8, viewFirstMip, 1, viewFirstSlice, viewNumSlices, (Latte::E_GX2SURFFMT)colorBufferFormat, (viewFirstSlice + viewNumSlices)>1? Latte::E_DIM::DIM_2D_ARRAY: Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, false, true);
		LatteGPUState.repeatTextureInitialization = true;
		checkForTextureChanges = false;
	}

	if (colorBufferView->baseTexture->swizzle != colorBufferSwizzle)
	{
		colorBufferView->baseTexture->lastRenderTargetSwizzle = colorBufferSwizzle;
	}

	// check for texture changes
	if (checkForTextureChanges)
		LatteTexture_UpdateDataToLatest(colorBufferView->baseTexture);
	// mark as used
	LatteTC_MarkTextureStillInUse(colorBufferView->baseTexture);
	return colorBufferView;
}

// get mask of all used color buffers
uint8 LatteMRT::GetActiveColorBufferMask(const LatteDecompilerShader* pixelShader, const LatteContextRegister& lcr)
{
	if (!pixelShader) [[unlikely]]
		return 0;
	const uint32* regView = lcr.GetRawView();
	// check if color buffer output is active
	const Latte::LATTE_CB_COLOR_CONTROL& colorControlReg = lcr.CB_COLOR_CONTROL;
	uint32 colorBufferDisable = colorControlReg.get_SPECIAL_OP() == Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE;
	if (colorBufferDisable)
		return 0;
	cemu_assert_debug(colorControlReg.get_DEGAMMA_ENABLE() == false); // not supported
	// start with color buffer mask from pixel shader output
	uint8 colorBufferMask = pixelShader->pixelColorOutputMask;
	// combine color buffer mask with color channel mask from mmCB_TARGET_MASK (disable render buffer if all colors are blocked)
	uint32 channelTargetMask = lcr.CB_TARGET_MASK.get_MASK();
	for (uint32 i = 0; i < 8; i++)
	{
		if (((channelTargetMask >> (i * 4)) & 0xF) == 0)
			colorBufferMask &= ~(1 << i);
	}
	// render targets smaller than the scissor size are not allowed
	// this fixes a few render issues in Cemu but we dont know if this matches HW behavior
	// also check for color buffers without a valid pointer
	cemu_assert_debug(lcr.PA_SC_GENERIC_SCISSOR_TL.get_WINDOW_OFFSET_DISABLE() == true); // todo (not exposed by GX2 API)
	uint32 scissorAccessWidth = lcr.PA_SC_GENERIC_SCISSOR_BR.get_BR_X();
	uint32 scissorAccessHeight = lcr.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();
	for (uint32 i = 0; i < 8; i++)
	{
		if( (colorBufferMask&(1<<i)) == 0 )
			continue;
		if (regView[mmCB_COLOR0_BASE + i] == MPTR_NULL) [[unlikely]]
			colorBufferMask &= ~(1 << i);
		// get width/height
		uint32 regColorSize = regView[mmCB_COLOR0_SIZE + i];
		uint32 regColorInfo = regView[mmCB_COLOR0_INFO + i];
		// decode color buffer reg info
		uint32 colorBufferPitch = (((regColorSize >> 0) & 0x3FF) + 1);
		colorBufferPitch <<= 3;
		uint32 pitchHeight = (((regColorSize >> 10) & 0xFFFFF) + 1);
		pitchHeight <<= 6;
		uint32 colorBufferHeight = pitchHeight / colorBufferPitch;
		uint32 colorBufferWidth = colorBufferPitch;

		if ((colorBufferWidth < (sint32)scissorAccessWidth) || (colorBufferHeight < (sint32)scissorAccessHeight))
		{
            // log this?
			colorBufferMask &= ~(1<<i);
		}
	}
	return colorBufferMask;
}

// returns true if depth/stencil buffer is used
bool LatteMRT::GetActiveDepthBufferMask(const LatteContextRegister& lcr)
{
	bool depthBufferMask = true;
	// if depth test is not used then detach the depth buffer
	bool depthEnable = lcr.DB_DEPTH_CONTROL.get_Z_ENABLE();
	bool stencilTestEnable = lcr.DB_DEPTH_CONTROL.get_STENCIL_ENABLE();
	bool backStencilEnable = lcr.DB_DEPTH_CONTROL.get_BACK_STENCIL_ENABLE();

	if (!depthEnable && !stencilTestEnable && !backStencilEnable)
		depthBufferMask = false;

	return depthBufferMask;
}

const uint32 _colorBufferFormatBits[] =
{
	0, // 0
	0x200, // 1
	0, // 2
	0, // 3
	0x100, // 4
	0x300, // 5
	0x400, // 6
	0x800, // 7
};

Latte::E_GX2SURFFMT LatteMRT::GetColorBufferFormat(const uint32 index, const LatteContextRegister& lcr)
{
	cemu_assert_debug(index < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS);
	uint32 regColorInfo = lcr.GetRawView()[mmCB_COLOR0_INFO + index];
	uint32 colorBufferFormat = (regColorInfo >> 2) & 0x3F; // base HW format
	uint32 numberType = (regColorInfo >> 12) & 7;
	colorBufferFormat |= _colorBufferFormatBits[numberType];
	return (Latte::E_GX2SURFFMT)colorBufferFormat;
}

// return GX2 format of current depth buffer
Latte::E_GX2SURFFMT LatteMRT::GetDepthBufferFormat(const LatteContextRegister& lcr)
{
	uint32 regDepthBufferInfo = lcr.GetRawView()[mmDB_DEPTH_INFO];
	switch (regDepthBufferInfo & 7)
	{
	case 1:
		return Latte::E_GX2SURFFMT::D16_UNORM;
	case 3:
		return Latte::E_GX2SURFFMT::D24_S8_UNORM;
	case 5:
		return Latte::E_GX2SURFFMT::D24_S8_FLOAT;
	case 6:
		return Latte::E_GX2SURFFMT::D32_FLOAT;
	case 7:
		return Latte::E_GX2SURFFMT::D32_S8_FLOAT;
	default:
		debug_printf("Invalid DB_DEPTH_INFO depthbuffer format (%d)\n", (regDepthBufferInfo & 7));
		break;
	}
	return Latte::E_GX2SURFFMT::D16_UNORM;
}

bool LatteMRT::UpdateCurrentFBO()
{
	catchOpenGLError();
	sLatteRenderTargetState.rtUpdateListCount = 0;
	// combine color buffer mask with pixel output mask from pixel shader
	LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	uint8 colorBufferMask = GetActiveColorBufferMask(pixelShader, LatteGPUState.contextNew);
	bool depthBufferMask = GetActiveDepthBufferMask(LatteGPUState.contextNew);

	bool hasResizedTexture = false; // set to true if any of the color buffers or the depth buffer reference a resized texture (via graphic pack texture rules)
	sLatteRenderTargetState.renderTargetIsResized = false;
	// real size
	LatteMRTQuad* rtRealSize = &sLatteRenderTargetState.currentRenderSize;
	rtRealSize->width = 0;
	rtRealSize->height = 0;
	// effective size (differs from real size only if graphic pack rules overwrite texture sizes)
	LatteMRTQuad* rtEffectiveSize = &sLatteRenderTargetState.currentEffectiveSize;
	rtEffectiveSize->width = 0;
	rtEffectiveSize->height = 0;
	// get scissor box
	cemu_assert_debug(LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_WINDOW_OFFSET_DISABLE() == true); // todo (not exposed by GX2 API?)
	uint32 scissorX = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_TL_X();
	uint32 scissorY = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_TL_Y();
	uint32 scissorWidth = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_X() - scissorX;
	uint32 scissorHeight = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y() - scissorY;
	uint32 scissorAccessWidth = scissorX + scissorWidth;
	uint32 scissorAccessHeight = scissorY + scissorHeight;
	// color buffers
	for (uint32 i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
	{
		if (((colorBufferMask)&(1 << i)) == 0)
		{
			// unbind
			SetColorAttachment(i, nullptr);
			continue;
		}
		LatteTextureView* colorAttachmentView = GetColorAttachmentTexture(i, true, true);
		SetColorAttachment(i, colorAttachmentView);
		// after the drawcall mark the texture as updated
		sLatteRenderTargetState.rtUpdateList[sLatteRenderTargetState.rtUpdateListCount] = colorAttachmentView;
		sLatteRenderTargetState.rtUpdateListCount++;

		sint32 colorAttachmentWidth, colorAttachmentHeight;
		colorAttachmentView->baseTexture->GetSize(colorAttachmentWidth, colorAttachmentHeight, colorAttachmentView->firstMip);

		// set effective size
		sint32 effectiveWidth, effectiveHeight;
		colorAttachmentView->baseTexture->GetEffectiveSize(effectiveWidth, effectiveHeight, colorAttachmentView->firstMip);
		if (rtEffectiveSize->width == 0 && rtEffectiveSize->height == 0)
		{
			rtEffectiveSize->width = effectiveWidth;
			rtEffectiveSize->height = effectiveHeight;
		}
		else if (rtEffectiveSize->width != effectiveWidth && rtEffectiveSize->height != effectiveHeight)
		{
			cemuLog_logDebug(LogType::Force, "Color buffer size mismatch ({}x{}). Effective size: {}x{} Real size: {}x{} Mismatching texture: {:08x} {}x{} fmt {:04x}", rtEffectiveSize->width, rtEffectiveSize->height, effectiveWidth, effectiveHeight, colorAttachmentView->baseTexture->width, colorAttachmentView->baseTexture->height, colorAttachmentView->baseTexture->physAddress, colorAttachmentView->baseTexture->width, colorAttachmentView->baseTexture->height, (uint32)colorAttachmentView->baseTexture->format);
		}
		// currently the first color attachment defines the size of the current render target
		if (rtRealSize->width == 0 && rtRealSize->height == 0)
		{
			rtRealSize->width = colorAttachmentWidth;
			rtRealSize->height = colorAttachmentHeight;
		}

		if (colorAttachmentView)
			continue;
	}
	// depth buffer
	if (depthBufferMask)
	{
		uint32 regDepthBuffer = LatteGPUState.contextRegister[mmDB_HTILE_DATA_BASE];
		uint32 regDepthSize = LatteGPUState.contextRegister[mmDB_DEPTH_SIZE];
		uint32 regDepthBufferInfo = LatteGPUState.contextRegister[mmDB_DEPTH_INFO];

		// get format and tileMode from info reg
		Latte::E_GX2SURFFMT depthBufferFormat = GetDepthBufferFormat(LatteGPUState.contextNew);
		Latte::E_HWTILEMODE depthBufferTileMode = (Latte::E_HWTILEMODE)((regDepthBufferInfo >> 15) & 0xF);
		MPTR depthBufferPhysMem = regDepthBuffer << 8;

		uint32 depthBufferPitch = (((regDepthSize >> 0) & 0x3FF) + 1);
		uint32 depthBufferHeight = ((((regDepthSize >> 10) & 0xFFFFF) + 1) / depthBufferPitch);
		depthBufferPitch <<= 3;
		depthBufferHeight <<= 3;
		uint32 depthBufferWidth = depthBufferPitch;

		if (depthBufferWidth < 2)
		{
			debug_printf("depthBufferWidth has invalid value %d\n", depthBufferWidth);
			depthBufferWidth = 2;
		}
		if (depthBufferHeight < 2)
		{
			debug_printf("depthBufferHeight has invalid value %d\n", depthBufferHeight);
			depthBufferHeight = 2;
		}

		bool blockDepthBuffer = false;
		if (scissorAccessWidth > depthBufferPitch || scissorAccessHeight > depthBufferHeight)
		{
			SetDepthAndStencilAttachment(nullptr, false);
			blockDepthBuffer = true;
			// set effective size
			if( rtEffectiveSize->width == 0 && rtEffectiveSize->height == 0 )
			{
				rtEffectiveSize->width = rtRealSize->width;
				rtEffectiveSize->height = rtRealSize->height;
			}
		}

		if (blockDepthBuffer == false)
		{
			if (rtRealSize->width == 0)
			{
				rtRealSize->width = depthBufferWidth;
				rtRealSize->height = depthBufferHeight;
			}
			uint32 regDepthView = LatteGPUState.contextRegister[mmDB_DEPTH_VIEW];
			uint32 depthBufferViewFirstSlice = (regDepthView & 0x7FF);
			uint32 depthBufferViewNumSlices = ((regDepthView >> 13) & 0x7FF) - depthBufferViewFirstSlice + 1;
			cemu_assert_debug(depthBufferViewNumSlices == 1); // binding multiple layers makes no sense?

			uint32 depthBufferSwizzle = 0;
			if (Latte::TM_IsMacroTiled(depthBufferTileMode))
			{
				depthBufferSwizzle = (depthBufferPhysMem >> 8) & 7;
				depthBufferPhysMem = depthBufferPhysMem & ~(7 << 8);
			}

			if (depthBufferPhysMem != MPTR_NULL)
			{
				LatteTextureView* depthBufferView = LatteTextureViewLookupCache::lookupSliceEx(depthBufferPhysMem, depthBufferWidth, depthBufferHeight, depthBufferPitch, 0, depthBufferViewFirstSlice, depthBufferFormat, true);
				if (!depthBufferView)
				{
					// create new depth buffer view and if it doesn't exist then also create the texture
					depthBufferView = LatteTexture_CreateMapping(depthBufferPhysMem, 0, depthBufferWidth, depthBufferHeight, depthBufferViewFirstSlice+1, depthBufferPitch, depthBufferTileMode, depthBufferSwizzle, 0, 1, depthBufferViewFirstSlice, 1, depthBufferFormat, depthBufferViewFirstSlice > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, true, true);
					LatteGPUState.repeatTextureInitialization = true;
				}
				else
				{
					// check for texture changes
					LatteTexture_UpdateDataToLatest(depthBufferView->baseTexture);
				}
				// set effective size
				sint32 effectiveWidth, effectiveHeight;
				depthBufferView->baseTexture->GetEffectiveSize(effectiveWidth, effectiveHeight, depthBufferView->firstMip);
				if (rtEffectiveSize->width == 0 && rtEffectiveSize->height == 0)
				{
					rtEffectiveSize->width = effectiveWidth;
					rtEffectiveSize->height = effectiveHeight;
				}
				else if (rtEffectiveSize->width > effectiveWidth && rtEffectiveSize->height > effectiveHeight)
				{
					if (_depthBufferSizeWarningCount < 100)
					{
						cemuLog_logDebug(LogType::Force, "Depth buffer size too small. Effective size: {}x{} Real size: {}x{} Mismatching texture: {:08x} {}x{} fmt {:04x}", effectiveWidth, effectiveHeight, depthBufferView->baseTexture->width, depthBufferView->baseTexture->height, depthBufferView->baseTexture->physAddress, depthBufferView->baseTexture->width, depthBufferView->baseTexture->height, (uint32)depthBufferView->baseTexture->format);
						_depthBufferSizeWarningCount++;
					}
				}
				LatteTC_MarkTextureStillInUse(depthBufferView->baseTexture);
				// after the drawcall mark the texture as updated
				sLatteRenderTargetState.rtUpdateList[sLatteRenderTargetState.rtUpdateListCount] = depthBufferView;
				sLatteRenderTargetState.rtUpdateListCount++;
				SetDepthAndStencilAttachment(depthBufferView, depthBufferView->baseTexture->hasStencil);
			}
		}
		else
		{
			SetDepthAndStencilAttachment(nullptr, false);
		}
	}
	else
	{
		SetDepthAndStencilAttachment(nullptr, false);
	}
	catchOpenGLError();
	if (colorBufferMask || depthBufferMask)
	{
		hasValidFramebufferAttached = true;
	}
	else
	{
		hasValidFramebufferAttached = false;
		return true;
	}
	if( rtEffectiveSize->width != rtRealSize->width || rtEffectiveSize->height != rtRealSize->height )
	{
		//debug_printf("RenderTarget rescaled. Real: %dx%d Resized: %dx%d\n", rtRealSize->width, rtRealSize->height, rtEffectiveSize->width, rtEffectiveSize->height);
		sLatteRenderTargetState.renderTargetIsResized = true;
	}
	if (sLatteRenderTargetState.currentEffectiveSize.width == 0)
	{
		debug_printf("Render target effective size is 0. May indicate a bug in Cemu or invalid color/depth buffers\n");
		return false;
	}
	return true;
}

// return a vec4 with xy being the scale ratio for render target extend and zw being the virtual viewport dimensions
void LatteMRT::GetCurrentFragCoordScale(float* coordScale)
{
	if (sLatteRenderTargetState.renderTargetIsResized)
	{
		coordScale[0] = (float)sLatteRenderTargetState.currentRenderSize.width / (float)sLatteRenderTargetState.currentEffectiveSize.width;
		coordScale[1] = (float)sLatteRenderTargetState.currentRenderSize.height / (float)sLatteRenderTargetState.currentEffectiveSize.height;
		coordScale[2] = sLatteRenderTargetState.currentGuestViewport.width;
		coordScale[3] = sLatteRenderTargetState.currentGuestViewport.height;
	}
	else
	{
		coordScale[0] = 1.0f;
		coordScale[1] = 1.0f;
		coordScale[2] = sLatteRenderTargetState.currentGuestViewport.width;
		coordScale[3] = sLatteRenderTargetState.currentGuestViewport.height;
	}
}

void LatteMRT::GetVirtualViewportDimensions(sint32& width, sint32& height)
{
	width = sLatteRenderTargetState.currentGuestViewport.width;
	height = sLatteRenderTargetState.currentGuestViewport.height;
}

// flag all FBO textures as updated via GPU
// also handle texture readback
void LatteRenderTarget_trackUpdates()
{
	// after the drawcall, mark the render target textures as dynamically updated
	uint64 eventCounter = LatteTexture_getNextUpdateEventCounter();
	for (sint32 i = 0; i < sLatteRenderTargetState.rtUpdateListCount; i++)
	{
		LatteTextureView* texView = sLatteRenderTargetState.rtUpdateList[i];
		LatteTexture* baseTexture = texView->baseTexture;
		LatteTexture_TrackTextureGPUWrite(baseTexture, texView->firstSlice, texView->firstMip, eventCounter);
		// texture readback
		if (baseTexture->enableReadback)
		{
			LatteTextureReadback_Initate(texView);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// PC Edition: frame statistics for the FPS counter (real game frames and frames shown per second; with AMD FSR 3 frame
// generation the shown rate comes from its swapchain).
// ---------------------------------------------------------------------------------------------------------------------
static void LatteFSR_AfterBackbuffer(sint32 x, sint32 y, sint32 w, sint32 h);
static void LatteProbe_OnPresent();
bool LatteFSR_GetStats(float& shownFps);

namespace
{
	struct
	{
		uint64 statStart = 0;
		uint32 statReal = 0, statShown = 0;
		float baseFps = 0.0f, shownFps = 0.0f;
		uint64 presentTimes[256]{};
		uint32 presentCount = 0;
	} s_frameStats;

	void FrameStats_Count(bool real, bool shown)
	{
		const uint64 now = HighResolutionTimer::now().getTick();
		const uint64 freq = HighResolutionTimer::getFrequency();
		if (s_frameStats.statStart == 0)
			s_frameStats.statStart = now;
		s_frameStats.statReal += real ? 1 : 0;
		s_frameStats.statShown += shown ? 1 : 0;
		if (shown)
			s_frameStats.presentTimes[(s_frameStats.presentCount++) & 255] = now;
		if (now - s_frameStats.statStart >= freq)
		{
			const double secs = (double)(now - s_frameStats.statStart) / (double)freq;
			s_frameStats.baseFps = (float)(s_frameStats.statReal / secs);
			s_frameStats.shownFps = (float)(s_frameStats.statShown / secs);
			s_frameStats.statReal = s_frameStats.statShown = 0;
			s_frameStats.statStart = now;
		}
	}
}

void LatteFrameGen_GetStats(float& baseFps, float& shownFps, bool& generating)
{
	baseFps = s_frameStats.baseFps;
	shownFps = s_frameStats.shownFps;
	generating = false;
	float fsrShown;
	if (LatteFSR_GetStats(fsrShown))
	{
		shownFps = fsrShown;
		generating = fsrShown > baseFps * 1.2f;
	}
}

// present pacing over the last 255 presents: mean / standard deviation / 95th percentile of the intervals in ms
void LatteFrameGen_GetPacing(double& meanMs, double& stdMs, double& p95Ms)
{
	meanMs = stdMs = p95Ms = 0.0;
	const uint32 n = std::min<uint32>(s_frameStats.presentCount, 256);
	if (n < 8)
		return;
	const double freq = (double)HighResolutionTimer::getFrequency();
	std::vector<double> d;
	for (uint32 i = 1; i < n; i++)
	{
		const uint32 a = (s_frameStats.presentCount - i - 1) & 255, b = (s_frameStats.presentCount - i) & 255;
		d.push_back((double)(s_frameStats.presentTimes[b] - s_frameStats.presentTimes[a]) * 1000.0 / freq);
	}
	for (double v : d)
		meanMs += v;
	meanMs /= d.size();
	for (double v : d)
		stdMs += (v - meanMs) * (v - meanMs);
	stdMs = std::sqrt(stdMs / d.size());
	std::sort(d.begin(), d.end());
	p95Ms = d[(size_t)(d.size() * 0.95)];
}

// regular present of the real frame (GX2SwapScanBuffers)
static void LatteFrameGen_SwapRealFrame()
{
	g_renderer->SwapBuffers(true, true);
	LatteProbe_OnPresent();
	FrameStats_Count(false, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Zelda on PC dev tool: frame trace. Records one frame (between two TV presents): every change of render targets /
// shaders / vertex shader uniform blocks with its draw count, plus the contents of the uniform blocks.
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
	struct
	{
		int state = 0; // 0 idle, 1 armed (starts at the next present), 2 recording
		std::string path;
		std::vector<std::string> lines;
		std::string lastKey;
		uint32 groupDraws = 0;
		std::unordered_map<uint32, std::vector<uint8>> blocks;
	} s_trace;

	std::string TraceView(LatteTextureView* v)
	{
		if (!v)
			return "-";
		auto* t = v->baseTexture;
		return fmt::format("{:08x}:{}x{}:f{:x}:s{}m{}", t->physAddress, t->width, t->height, (uint32)t->format, v->firstSlice, v->firstMip);
	}

	void TraceFlushGroup()
	{
		if (!s_trace.lastKey.empty() && s_trace.groupDraws)
			s_trace.lines.push_back(fmt::format("draws={} {}", s_trace.groupDraws, s_trace.lastKey));
		s_trace.groupDraws = 0;
	}

	void TraceFrameBoundary()
	{
		if (s_trace.state == 1)
		{
			s_trace.state = 2;
			s_trace.lines.clear();
			s_trace.blocks.clear();
			s_trace.lastKey.clear();
			s_trace.groupDraws = 0;
			return;
		}
		if (s_trace.state != 2)
			return;
		TraceFlushGroup();
		std::ofstream f(_utf8ToPath(s_trace.path), std::ios::trunc);
		for (auto& l : s_trace.lines)
			f << l << "\n";
		f << "== uniform blocks\n";
		for (auto& [addr, data] : s_trace.blocks)
		{
			f << fmt::format("UB {:08x} {}:", addr, data.size());
			for (size_t i = 0; i + 4 <= data.size(); i += 4)
			{
				uint32 v = *(uint32*)(data.data() + i); // uniform blocks are stored little endian (GX2 endian swaps them)
				f << fmt::format(" {:.7g}", *(float*)&v);
			}
			f << "\n";
		}
		s_trace.state = 0;
	}
}

void LatteFrameTrace_Request(const std::string& path)
{
	s_trace.path = path;
	s_trace.state = 1;
}

void LatteFrameTrace_OnDraw()
{
	if (s_trace.state != 2) [[likely]]
		return;
	std::string key;
	for (uint32 i = 0; i < 8; i++)
		if (auto* v = LatteMRT::GetColorAttachment(i))
			key += fmt::format("c{}={} ", i, TraceView(v));
	key += fmt::format("d={} ", TraceView(LatteMRT::GetDepthAttachment()));
	auto* vs = LatteSHRC_GetActiveVertexShader();
	auto* ps = LatteSHRC_GetActivePixelShader();
	key += fmt::format("vs={:016x} ps={:016x} ub=", vs ? vs->baseHash : 0, ps ? ps->baseHash : 0);
	for (uint32 i = 0; i < 16; i++)
	{
		const MPTR addr = LatteGPUState.contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7 + 0];
		const uint32 size = LatteGPUState.contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7 + 1] + 1;
		if (!addr || size <= 1)
			continue;
		key += fmt::format("{}:{:08x}:{} ", i, addr, size);
		if (!s_trace.blocks.contains(addr) && s_trace.blocks.size() < 2048)
		{
			const uint32 n = std::min<uint32>(size, 1024);
			std::vector<uint8> data(n);
			memcpy(data.data(), memory_base + addr, n);
			s_trace.blocks[addr] = std::move(data);
		}
	}
	if (key != s_trace.lastKey)
	{
		TraceFlushGroup();
		s_trace.lastKey = key;
	}
	s_trace.groupDraws++;
}

// ---------------------------------------------------------------------------------------------------------------------
// Zelda on PC: inputs for AMD FSR 3 frame generation, captured while the frame is drawn:
//  - camera: the view/projection uniform block (vertex shader slot 1) of the scene G-buffer pass (several color targets +
//    a depth buffer of TV size). Layout (floats): view 3x4 @0, viewProj 4x4 @12, proj 4x4 @28, invView 3x4 @44,
//    near @56, far @57, aspect @62, render size @72 (row major).
//  - HUD-less color + scene depth: snapshot right before the first UI (nw::lyt, slot 1 block of 288 bytes) draw into the
//    TV color buffer, or at the end of the frame if there was no UI.
// ---------------------------------------------------------------------------------------------------------------------
bool g_latteFsrActive = false; // fast check for the draw hook

namespace
{
	struct
	{
		bool wanted = false;         // setting / dev request
		bool cameraValid = false;
		float cam[96]{};
		double prevVP[16]{};
		bool prevValid = false;
		bool snapshotDone = false;
		// scene depth buffer (looked up again by address when used, the game reuses its memory for UI targets)
		MPTR depthAddr = 0;
		sint32 depthW = 0, depthH = 0, depthPitch = 0;
		Latte::E_GX2SURFFMT depthFormat{};
		// TV color buffer (from the previous present)
		MPTR tvAddr = 0;
		Latte::E_GX2SURFFMT tvFormat{};
		sint32 tvW = 0, tvH = 0;
		uint32 mvFlags = 1; // 1 = flip y
		uint64 settingsRev = ~0ull;
		bool devForce = false;
		uint32 frames = 0, snapshots = 0, cameras = 0, prepared = 0;
		// frame handed to FSR after the backbuffer is drawn
		bool ready = false;
		bool generating = false; // FSR 3 generates frames for the last presented frame
		uint32 copies = 0;       // frames without the 3D scene handed to FSR as they are (no interpolation)
		float frameCam[96]{};
		uint64 lastFrameTick = 0;
		float frameTimeMs = 16.6f;
		bool reset = true;
		float lastPos[3]{};
		uint32 debugFlags = 0;
		// displayed frame rate (FSR swapchain present count)
		uint64 statStart = 0, statPresents = 0;
		float shownFps = 0.0f;
	} s_fsr;

	void Mat4FromRows(const float* src, double* dst)
	{
		for (int i = 0; i < 16; i++)
			dst[i] = src[i];
	}

	bool Mat4Inverse(const double* m, double* inv)
	{
		double a[4][8];
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 8; c++)
				a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
		for (int c = 0; c < 4; c++)
		{
			int p = c;
			for (int r = c + 1; r < 4; r++)
				if (std::fabs(a[r][c]) > std::fabs(a[p][c]))
					p = r;
			if (std::fabs(a[p][c]) < 1e-12)
				return false;
			if (p != c)
				for (int k = 0; k < 8; k++)
					std::swap(a[p][k], a[c][k]);
			const double d = a[c][c];
			for (int k = 0; k < 8; k++)
				a[c][k] /= d;
			for (int r = 0; r < 4; r++)
			{
				if (r == c)
					continue;
				const double f = a[r][c];
				for (int k = 0; k < 8; k++)
					a[r][k] -= f * a[c][k];
			}
		}
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				inv[r * 4 + c] = a[r][c + 4];
		return true;
	}

	void Mat4Mul(const double* a, const double* b, double* out)
	{
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
			{
				double s = 0;
				for (int k = 0; k < 4; k++)
					s += a[r * 4 + k] * b[k * 4 + c];
				out[r * 4 + c] = s;
			}
	}

	LatteTextureView* FSR_LookupDepth()
	{
		if (!s_fsr.depthAddr)
			return nullptr;
		sint32 searchIndex = 0;
		while (LatteTextureView* v = LatteTC_LookupTextureByData(s_fsr.depthAddr, s_fsr.depthW, s_fsr.depthH, s_fsr.depthPitch, 0, 1, 0, 1, &searchIndex))
		{
			if (v->baseTexture->isDepth && v->baseTexture->format == s_fsr.depthFormat)
				return v;
		}
		return nullptr;
	}

	void FSR_Snapshot(LatteTextureView* color)
	{
		LatteTextureView* depth = FSR_LookupDepth();
		if (!color || !depth)
			return;
		if (g_renderer->FSR_CaptureSnapshot(color, depth))
		{
			s_fsr.snapshotDone = true;
			s_fsr.snapshots++;
		}
	}
}

void LatteFSR_OnDraw(bool isFirstOfPass)
{
	// camera of the scene pass
	if (!s_fsr.cameraValid)
	{
		LatteTextureView* d = LatteMRT::GetDepthAttachment();
		if (d && LatteMRT::GetColorAttachment(1) && s_fsr.tvW && d->baseTexture->width == s_fsr.tvW && d->baseTexture->height == s_fsr.tvH)
		{
			const MPTR addr = LatteGPUState.contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + 1 * 7 + 0];
			const uint32 size = LatteGPUState.contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + 1 * 7 + 1] + 1;
			if (addr && size >= sizeof(s_fsr.cam))
			{
				memcpy(s_fsr.cam, memory_base + addr, sizeof(s_fsr.cam)); // uniform blocks are little endian
				s_fsr.cameraValid = true;
				s_fsr.cameras++;
				auto* t = d->baseTexture;
				s_fsr.depthAddr = t->physAddress;
				s_fsr.depthW = t->width;
				s_fsr.depthH = t->height;
				s_fsr.depthPitch = t->pitch;
				s_fsr.depthFormat = t->format;
			}
		}
		return;
	}
	// HUD-less snapshot right before the first UI draw into the TV buffer
	if (s_fsr.snapshotDone || !isFirstOfPass)
		return;
	LatteTextureView* c0 = LatteMRT::GetColorAttachment(0);
	if (!c0 || c0->baseTexture->physAddress != s_fsr.tvAddr || c0->baseTexture->format != s_fsr.tvFormat)
		return;
	const uint32 ubSize = LatteGPUState.contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + 1 * 7 + 1] + 1;
	if (ubSize == 288)
		FSR_Snapshot(c0);
}

// end of frame (TV present), before the backbuffer is drawn
// latency probe: input (first VPAD read turning the camera) -> first frame with the camera moved -> its present
extern std::atomic<bool> g_pceProbeArmed;
extern std::atomic<uint64> g_pceProbeInputTick;
namespace
{
	struct
	{
		float lastFwd[3]{};
		uint64 frameTick = 0, presentTick = 0;
		uint32 framesSinceInput = 0;
		bool waitPresent = false;
	} s_probe;
}

static void LatteProbe_OnCamera(const float* cam)
{
	const float fwd[3] = {cam[8], cam[9], cam[10]};
	const float dot = fwd[0] * s_probe.lastFwd[0] + fwd[1] * s_probe.lastFwd[1] + fwd[2] * s_probe.lastFwd[2];
	memcpy(s_probe.lastFwd, fwd, sizeof(fwd));
	if (!g_pceProbeArmed.load() || g_pceProbeInputTick.load() == 0 || s_probe.frameTick)
		return;
	s_probe.framesSinceInput++;
	if (dot < 0.999999f)
	{
		s_probe.frameTick = HighResolutionTimer::now().getTick();
		s_probe.waitPresent = true;
	}
}

static void LatteProbe_OnPresent()
{
	if (s_probe.waitPresent)
	{
		s_probe.presentTick = HighResolutionTimer::now().getTick();
		s_probe.waitPresent = false;
	}
}

std::string LatteProbe_Command(const std::vector<std::string>& a)
{
	if (a.size() > 2 && a[2] == "arm")
	{
		s_probe.frameTick = s_probe.presentTick = 0;
		s_probe.framesSinceInput = 0;
		s_probe.waitPresent = false;
		g_pceProbeInputTick = 0;
		g_pceProbeArmed = true;
		return "armed";
	}
	const double f = (double)HighResolutionTimer::getFrequency() / 1000.0;
	const uint64 t0 = g_pceProbeInputTick.load();
	if (!t0 || !s_probe.frameTick)
		return fmt::format("pending (input {})", t0 != 0);
	g_pceProbeArmed = false;
	return fmt::format("input->camera frame end {:.1f} ms ({} frames) | input->present {:.1f} ms", (double)(s_probe.frameTick - t0) / f, s_probe.framesSinceInput,
		s_probe.presentTick ? (double)(s_probe.presentTick - t0) / f : -1.0);
}

static void LatteFSR_OnFrameEnd(LatteTextureView* tvView)
{
	if (pce::GetSettingsRevision() != s_fsr.settingsRev)
	{
		s_fsr.settingsRev = pce::GetSettingsRevision();
		s_fsr.wanted = pce::IsActive() && pce::GetSettings().frameGeneration == 1;
		g_renderer->FSR_SetWanted(s_fsr.wanted);
	}
	const bool active = (s_fsr.wanted || s_fsr.devForce) && g_renderer->GetType() == RendererAPI::Vulkan && !(pce::IsActive() && pce::Overlay::IsGamePaused());
	if (active && s_fsr.cameraValid)
	{
		if (!s_fsr.snapshotDone)
			FSR_Snapshot(tvView); // no UI this frame
		double cur[16];
		Mat4FromRows(s_fsr.cam + 12, cur);
		if (s_fsr.snapshotDone)
		{
			double reproj[16];
			double inv[16];
			if (s_fsr.prevValid && Mat4Inverse(cur, inv))
				Mat4Mul(s_fsr.prevVP, inv, reproj);
			else
			{
				for (int i = 0; i < 16; i++)
					reproj[i] = (i % 5) == 0 ? 1.0 : 0.0;
			}
			float rf[16];
			for (int i = 0; i < 16; i++)
				rf[i] = (float)reproj[i];
			g_renderer->FSR_BuildMotionVectors(rf, s_fsr.mvFlags);
		}
		LatteProbe_OnCamera(s_fsr.cam);
		// camera for FSR (used once the backbuffer is drawn)
		const uint64 now = HighResolutionTimer::now().getTick();
		if (s_fsr.lastFrameTick)
			s_fsr.frameTimeMs = (float)((double)(now - s_fsr.lastFrameTick) * 1000.0 / (double)HighResolutionTimer::getFrequency());
		s_fsr.lastFrameTick = now;
		memcpy(s_fsr.frameCam, s_fsr.cam, sizeof(s_fsr.cam));
		const float pos[3] = {s_fsr.cam[47], s_fsr.cam[51], s_fsr.cam[55]};
		const float dx = pos[0] - s_fsr.lastPos[0], dy = pos[1] - s_fsr.lastPos[1], dz = pos[2] - s_fsr.lastPos[2];
		if (!s_fsr.prevValid || dx * dx + dy * dy + dz * dz > 15.0f * 15.0f || s_fsr.frameTimeMs > 200.0f)
			s_fsr.reset = true; // camera cut / teleport / hitch: no interpolation across it
		memcpy(s_fsr.lastPos, pos, sizeof(pos));
		s_fsr.ready = s_fsr.snapshotDone;
		memcpy(s_fsr.prevVP, cur, sizeof(cur));
		s_fsr.prevValid = true;
	}
	else if (!active)
	{
		s_fsr.prevValid = false;
		s_fsr.ready = false;
		s_fsr.lastFrameTick = 0;
	}
	if (active && !s_fsr.ready && g_renderer->FSR_IsRunning() && g_renderer->FSR_CaptureColor(tvView))
	{
		// no 3D scene this frame (map, inventory, fades, loading): frame generation keeps running on the frame as it is, with
		// reset (FSR shows a copy of it in between, no interpolation). The screen keeps the steady rate it has in play; a
		// variable refresh rate screen (FreeSync / G-Sync) flickers whenever generation stops and the rate drops.
		static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		g_renderer->FSR_BuildMotionVectors(kIdentity, s_fsr.mvFlags);
		const uint64 now = HighResolutionTimer::now().getTick();
		if (s_fsr.lastFrameTick)
			s_fsr.frameTimeMs = (float)((double)(now - s_fsr.lastFrameTick) * 1000.0 / (double)HighResolutionTimer::getFrequency());
		s_fsr.lastFrameTick = now;
		s_fsr.reset = true;
		s_fsr.prevValid = false; // the next scene frame starts without history
		s_fsr.ready = true;
		s_fsr.copies++;
	}
	s_fsr.frames++;
	// next frame
	s_fsr.cameraValid = false;
	s_fsr.snapshotDone = false;
	auto* t = tvView->baseTexture;
	s_fsr.tvAddr = t->physAddress;
	s_fsr.tvFormat = t->format;
	s_fsr.tvW = t->width;
	s_fsr.tvH = t->height;
	g_latteFsrActive = active;
}

// the backbuffer shows the real frame (no overlay yet): hand the frame to FSR 3 frame generation
static void LatteFSR_AfterBackbuffer(sint32 x, sint32 y, sint32 w, sint32 h)
{
	if (!g_renderer->FSR_IsRunning())
		return;
	// displayed frame rate
	const uint64 now = HighResolutionTimer::now().getTick();
	const uint64 freq = HighResolutionTimer::getFrequency();
	if (s_fsr.statStart == 0 || now - s_fsr.statStart >= freq)
	{
		const uint64 presents = g_renderer->FSR_PresentCount();
		if (s_fsr.statStart)
			s_fsr.shownFps = (float)((double)(presents - s_fsr.statPresents) * (double)freq / (double)(now - s_fsr.statStart));
		s_fsr.statStart = now;
		s_fsr.statPresents = presents;
	}
	if (!s_fsr.ready)
	{
		g_renderer->FSR_DisableGeneration();
		s_fsr.reset = true;
		s_fsr.generating = false;
		return;
	}
	s_fsr.ready = false;
	const float* c = s_fsr.frameCam;
	Renderer::FSRFrameParams p{};
	p.cameraNear = c[56];
	p.cameraFar = c[57];
	p.fovY = 2.0f * std::atan(1.0f / std::max(c[33], 1e-4f)); // projection row 1, column 1
	p.position[0] = c[47];
	p.position[1] = c[51];
	p.position[2] = c[55];
	// view matrix rows = camera axes in world space (looking down -z)
	for (int i = 0; i < 3; i++)
	{
		p.right[i] = c[0 + i];
		p.up[i] = c[4 + i];
		p.forward[i] = -c[8 + i];
	}
	p.frameTimeMs = s_fsr.frameTimeMs;
	p.reset = s_fsr.reset;
	p.debugFlags = s_fsr.debugFlags;
	p.imageX = x;
	p.imageY = y;
	p.imageWidth = w;
	p.imageHeight = h;
	s_fsr.generating = g_renderer->FSR_PrepareFrame(p);
	if (s_fsr.generating)
	{
		s_fsr.prepared++;
		s_fsr.reset = false;
	}
}

bool LatteFSR_GetStats(float& shownFps)
{
	if (!g_renderer || !g_renderer->FSR_IsRunning())
		return false;
	shownFps = s_fsr.shownFps;
	return true;
}

std::string LatteFSR_Command(const std::vector<std::string>& a)
{
	if (a.size() > 1 && a[1] == "probe")
		return LatteProbe_Command(a);
	if (a.size() > 2 && a[1] == "dump")
	{
		g_renderer->FSR_RequestDump(a[2], a.size() > 3 ? std::stoi(a[3]) : 1);
		return "ok";
	}
	if (a.size() > 2 && a[1] == "flags")
	{
		s_fsr.mvFlags = (uint32)std::stoul(a[2], nullptr, 0);
		return "ok";
	}
	if (a.size() > 2 && a[1] == "force")
	{
		s_fsr.devForce = a[2] != "0";
		return "ok";
	}
	if (a.size() > 2 && a[1] == "debug")
	{
		s_fsr.debugFlags = (uint32)std::stoul(a[2], nullptr, 0);
		return "ok";
	}
	return fmt::format("frames {} cameras {} snapshots {} prepared {} active {} running {} shown {:.1f} flags {} | near {} far {} aspect {} size {}x{} | generating {} copies {}", s_fsr.frames,
		s_fsr.cameras, s_fsr.snapshots, s_fsr.prepared, g_latteFsrActive, g_renderer->FSR_IsRunning(), s_fsr.shownFps, s_fsr.mvFlags, s_fsr.cam[56], s_fsr.cam[57],
		s_fsr.cam[62], s_fsr.cam[72], s_fsr.cam[73], s_fsr.generating, s_fsr.copies);
}

void LatteRenderTarget_PCEIdlePresent()
{
	if (!pce::IsActive() || !pce::Overlay::IsGamePaused() || !s_pceLastTVView)
		return;
	if (std::chrono::steady_clock::now() - s_pceLastPresent < std::chrono::milliseconds(15))
		return;
	LatteRenderTarget_copyToBackbuffer(s_pceLastTVView, false);
	g_renderer->SwapBuffers(true, false);
}

void LatteRenderTarget_itHLESwapScanBuffer()
{
	performanceMonitor.cycle[performanceMonitor.cycleIndex].frameCounter++;
	if(LatteGPUState.frameCounter > 5)
		performanceMonitor.gpuTime_frameTime.endMeasuring();
	LattePerformanceMonitor_frameEnd();
	FrameBenchmarkHelper::FrameEnd();
	LatteGPUState.frameCounter++;
	LatteFrameGen_SwapRealFrame();

	catchOpenGLError();
	performanceMonitor.gpuTime_frameTime.beginMeasuring();

	LatteTC_CleanupUnusedTextures();
	LatteSHRC_CleanupShaderStateCache();
#ifdef ENABLE_OPENGL
	LatteDraw_cleanupAfterFrame();
#endif
	LatteQuery_CancelActiveGPU7Queries();
	LatteBufferCache_notifySwapTVScanBuffer();
	LattePerformanceMonitor_frameBegin();
}

void LatteRenderTarget_applyTextureColorClear(LatteTexture* texture, uint32 sliceIndex, uint32 mipIndex, float r, float g, float b, float a, uint64 eventCounter)
{
	if (texture->isDepth)
	{
		cemuLog_logDebug(LogType::Force, "Unsupported clear depth as color");
	}
	else
	{
		g_renderer->texture_clearColorSlice(texture, sliceIndex, mipIndex, r, g, b, a);
		LatteTexture_MarkDynamicTextureAsChanged(texture->baseView, sliceIndex, mipIndex, eventCounter);
	}
}

void LatteRenderTarget_applyTextureDepthClear(LatteTexture* texture, uint32 sliceIndex, uint32 mipIndex, bool hasDepthClear, bool hasStencilClear, float depthValue, uint8 stencilValue, uint64 eventCounter)
{
	if(texture->isDepth)
	{
		g_renderer->texture_clearDepthSlice(texture, sliceIndex, mipIndex, hasDepthClear, hasStencilClear, depthValue, stencilValue);
	}
	else
	{
		// clearing a color texture using depth clear
		if (hasStencilClear)
			return; // operation likely not intended as a color clear
		//cemu_assert_debug(!hasStencilClear);
		if (hasDepthClear)
		{
			g_renderer->texture_clearColorSlice(texture, sliceIndex, mipIndex, depthValue, depthValue, depthValue, depthValue);
		}

	}
	LatteTexture_MarkDynamicTextureAsChanged(texture->baseView, sliceIndex, mipIndex, eventCounter);
}

void LatteRenderTarget_itHLEClearColorDepthStencil(uint32 clearMask,
												   MPTR colorBufferMPTR, Latte::E_GX2SURFFMT colorBufferFormat, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferPitch, uint32 colorBufferViewFirstSlice, uint32 colorBufferViewNumSlice,
												   MPTR depthBufferMPTR, Latte::E_GX2SURFFMT depthBufferFormat, Latte::E_HWTILEMODE depthBufferTileMode, sint32 depthBufferWidth, sint32 depthBufferHeight, sint32 depthBufferPitch, sint32 depthBufferViewFirstSlice, sint32 depthBufferViewNumSlice,
												   float r, float g, float b, float a, float clearDepth, uint32 clearStencil)
{
	uint32 depthBufferMipIndex = 0; // todo
	uint32 colorBufferMipIndex = 0; // todo

	bool hasColorClear = (clearMask & 1);
	bool hasDepthClear = (clearMask & 2);
	bool hasStencilClear = (clearMask & 4);

	// extract swizzle offset from pointer
	uint32 colorBufferSwizzle = 0;
	uint32 depthBufferSwizzle = 0;
	if (Latte::TM_IsMacroTiled(colorBufferTilemode))
	{
		colorBufferSwizzle = (colorBufferMPTR >> 8) & 7;
		colorBufferMPTR = colorBufferMPTR & ~(7 << 8);
	}
	if (Latte::TM_IsMacroTiled(depthBufferTileMode))
	{
		depthBufferSwizzle = (depthBufferMPTR >> 8) & 7;
		depthBufferMPTR = depthBufferMPTR & ~(7 << 8);
	}

	cemu_assert_debug(colorBufferViewNumSlice <= 1);
	cemu_assert_debug(depthBufferViewNumSlice <= 1);

	// clear color buffer (if flag set)
	uint64 eventCounter = LatteTexture_getNextUpdateEventCounter();
	if ((clearMask & 1) != 0 && colorBufferMPTR != MPTR_NULL && colorBufferWidth > 0 && colorBufferHeight > 0)
	{
		// clear color
		sint32 searchIndex = 0;
		bool targetFound = false;
		while (true)
		{
			LatteTextureView* colorView = LatteTC_LookupTextureByData(colorBufferMPTR, colorBufferWidth, colorBufferHeight, colorBufferPitch, 0, 1, colorBufferViewFirstSlice, 1, &searchIndex);
			if (!colorView)
				break;
			if (Latte::GetFormatBits(colorBufferFormat) != Latte::GetFormatBits(colorView->baseTexture->format))
				continue;

			if (colorView->baseTexture->pitch == colorBufferPitch && colorView->baseTexture->height == colorBufferHeight)
				targetFound = true;

			LatteRenderTarget_applyTextureColorClear(colorView->baseTexture, colorBufferViewFirstSlice, colorBufferMipIndex, r, g, b, a, eventCounter);

		}
		if (targetFound == false)
		{
			// create new texture with matching format
			cemu_assert_debug(colorBufferViewNumSlice <= 1);
			LatteTextureView* newColorView = LatteTexture_CreateMapping(colorBufferMPTR, MPTR_NULL, colorBufferWidth, colorBufferHeight, colorBufferViewFirstSlice+1, colorBufferPitch, colorBufferTilemode, colorBufferSwizzle, 0, 1, colorBufferViewFirstSlice, 1, colorBufferFormat, colorBufferViewFirstSlice > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, false);
			LatteRenderTarget_applyTextureColorClear(newColorView->baseTexture, colorBufferViewFirstSlice, colorBufferMipIndex, r, g, b, a, eventCounter);
		}
	}
	// clear depth or stencil buffer (if flag set)
	if ((hasDepthClear || hasStencilClear) && depthBufferMPTR != MPTR_NULL)
	{
		std::vector<LatteTexture*> list_depthClearTextures;
		LatteTC_LookupTexturesByPhysAddr(depthBufferMPTR, list_depthClearTextures);
		bool foundMatchingDepthBuffer = false;
		// todo - support for clearing depth mips?
		cemu_assert_debug(depthBufferViewNumSlice == 1);

		for (auto& texItr : list_depthClearTextures)
		{
			// only clear depth buffers if they are smaller
			if (texItr->pitch > depthBufferPitch)
				continue;
			if (depthBufferViewFirstSlice >= texItr->depth)
				continue; // slice out of range

			if (texItr->pitch == depthBufferPitch && texItr->height == depthBufferHeight)
				foundMatchingDepthBuffer = true;

			// todo - calculate actual sliceIndex and mipIndex since the textures in list_depthClearTextures dont necessarily share the same base
			LatteRenderTarget_applyTextureDepthClear(texItr, depthBufferViewFirstSlice, depthBufferMipIndex, hasDepthClear, hasStencilClear, clearDepth, clearStencil, eventCounter);
		}

		if (foundMatchingDepthBuffer == false)
		{
			LatteTextureView* newDepthBufferView = LatteMRT_CreateDepthBuffer(depthBufferMPTR, depthBufferWidth, depthBufferHeight, depthBufferPitch, depthBufferTileMode, (Latte::E_GX2SURFFMT)depthBufferFormat, depthBufferSwizzle, depthBufferViewFirstSlice);
			LatteRenderTarget_applyTextureDepthClear(newDepthBufferView->baseTexture, depthBufferViewFirstSlice, depthBufferMipIndex, hasDepthClear, hasStencilClear, clearDepth, clearStencil, eventCounter);
		}
	}
}

sint32 _currentOutputImageWidth = 0;
sint32 _currentOutputImageHeight = 0;

void LatteRenderTarget_getScreenImageArea(sint32* x, sint32* y, sint32* width, sint32* height, sint32* fullWidth, sint32* fullHeight, bool padView)
{
	int w, h;
	if(padView && WindowSystem::IsPadWindowOpen())
		WindowSystem::GetPadWindowPhysSize(w, h);
	else
		WindowSystem::GetWindowPhysSize(w, h);

	sint32 scaledOutputX;
	sint32 scaledOutputY;
	if (GetConfig().fullscreen_scaling == kKeepAspectRatio)
	{
		// calculate maximum possible resolution with intact aspect ratio
		scaledOutputX = w;
		scaledOutputY = _currentOutputImageHeight * w / std::max(_currentOutputImageWidth, 1);
		if (scaledOutputY > h)
		{
			scaledOutputX = _currentOutputImageWidth * h / std::max(_currentOutputImageHeight, 1);
			scaledOutputY = h;
		}
	}
	else
	{
		scaledOutputX = w;
		scaledOutputY = h;
	}

	*x = (w - scaledOutputX) / 2;
	*y = (h - scaledOutputY) / 2;
	*width = scaledOutputX;
	*height = scaledOutputY;
	if (fullWidth)
		*fullWidth = w;
	if (fullHeight)
		*fullHeight = h;
}

void LatteRenderTarget_copyToBackbuffer(LatteTextureView* textureView, bool isPadView)
{
	if (!isPadView)
	{
		TraceFrameBoundary();
		LatteFSR_OnFrameEnd(textureView);
	}
	if (!isPadView)
	{
		s_pceLastTVView = textureView;
		s_pceLastPresent = std::chrono::steady_clock::now();
	}
	// make sure texture is updated to latest data in cache
	LatteTexture_UpdateDataToLatest(textureView->baseTexture);
	// mark source texture as still in use
	LatteTC_MarkTextureStillInUse(textureView->baseTexture);

	sint32 effectiveWidth, effectiveHeight;
	textureView->baseTexture->GetEffectiveSize(effectiveWidth, effectiveHeight, 0);
	_currentOutputImageWidth = effectiveWidth;
	_currentOutputImageHeight = effectiveHeight;

	sint32 imageX, imageY;
	sint32 imageWidth, imageHeight;
	sint32 fullscreenWidth, fullscreenHeight;

	LatteRenderTarget_getScreenImageArea(&imageX, &imageY, &imageWidth, &imageHeight, &fullscreenWidth, &fullscreenHeight, isPadView);

	bool clearBackground = false;
	if (imageWidth != fullscreenWidth || imageHeight != fullscreenHeight)
		clearBackground = true;

	const bool renderUpsideDown = ActiveSettings::RenderUpsideDownEnabled();
	// force disable bicubic scaling if output resolution is equal/smaller than input resolution
	const bool downscaling = (imageWidth <= effectiveWidth || imageHeight <= effectiveHeight);
	// check for graphic pack shaders
	RendererOutputShader* shader = nullptr;
	LatteTextureView::MagFilter filter = LatteTextureView::MagFilter::kLinear;
	for(const auto& gp : GraphicPack2::GetActiveGraphicPacks())
	{
		if(downscaling)
		{
			shader = gp->GetDownscalingShader(renderUpsideDown);
			if (shader)
			{
				filter = gp->GetDownscalingMagFilter();
				break;
			}
		}
		else
		{
			shader = gp->GetUpscalingShader(renderUpsideDown);
			if (shader)
			{
				filter = gp->GetUpscalingMagFilter();
				break;
			}
		}

		shader = gp->GetOuputShader(renderUpsideDown);
		if (shader)
		{
			filter = downscaling ? gp->GetDownscalingMagFilter() : gp->GetUpscalingMagFilter();
			break;
		}
	}

	if (shader == nullptr)
	{
		sint32 scaling_filter = downscaling ? GetConfig().downscale_filter : GetConfig().upscale_filter;

		if (scaling_filter == kLinearFilter)
		{
			if(renderUpsideDown)
				shader = RendererOutputShader::s_copy_shader_ud;
			else
				shader = RendererOutputShader::s_copy_shader;

			filter = LatteTextureView::MagFilter::kLinear;
		}
		else if (scaling_filter == kBicubicFilter)
		{
			if (renderUpsideDown)
				shader = RendererOutputShader::s_bicubic_shader_ud;
			else
				shader = RendererOutputShader::s_bicubic_shader;

			filter = LatteTextureView::MagFilter::kLinear;
		}
		else if (scaling_filter == kBicubicHermiteFilter)
		{
			if (renderUpsideDown)
				shader = RendererOutputShader::s_hermit_shader_ud;
			else
				shader = RendererOutputShader::s_hermit_shader;

			filter = LatteTextureView::MagFilter::kLinear;
		}
		else if (scaling_filter == kNearestNeighborFilter)
		{
			if (renderUpsideDown)
				shader = RendererOutputShader::s_copy_shader_ud;
			else
				shader = RendererOutputShader::s_copy_shader;

			filter = LatteTextureView::MagFilter::kNearestNeighbor;
		}
	}
	cemu_assert(shader);
	g_renderer->DrawBackbufferQuad(textureView, shader, filter==LatteTextureView::MagFilter::kLinear, imageX, imageY, imageWidth, imageHeight, isPadView, clearBackground);
	if (!pce::IsActive())
		g_renderer->HandleScreenshotRequest(textureView, isPadView);
	if (!isPadView)
		LatteFSR_AfterBackbuffer(imageX, imageY, imageWidth, imageHeight);
	if (!isPadView && !(pce::IsActive() && pce::Overlay::IsGamePaused()))
		FrameStats_Count(true, false); // a new game frame (a paused frame is only shown again)
	if (!g_renderer->ImguiBegin(!isPadView))
		return;
	swkbd_render(!isPadView);
	nn::erreula::render(!isPadView);
	LatteOverlay_render(isPadView);
	g_renderer->ImguiEnd();
}

void LatteRenderTarget_itHLECopyColorBufferToScanBuffer(MPTR colorBufferPtr, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferSliceIndex, uint32 colorBufferFormat, uint32 colorBufferPitch, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferSwizzle, uint32 renderTarget)
{
	cemu_assert_debug(colorBufferSliceIndex == 0); // todo - support for non-zero slice
	// Zelda on PC: the GPU thread reached the TV image of a frame (see pce::UI::OnGameFrame)
	if ((renderTarget & RENDER_TARGET_TV) && pce::IsActive())
		pce::UI::OnGpuFrame();
	LatteTextureView* texView = LatteTC_GetTextureSliceViewOrTryCreate(colorBufferPtr, MPTR_NULL, (Latte::E_GX2SURFFMT)colorBufferFormat, colorBufferTilemode, colorBufferWidth, colorBufferHeight, 1, colorBufferPitch, colorBufferSwizzle, 0, 0, true);
	if (!texView)
	{
		return;
	}

	auto getVPADScreenActive = [](size_t n) -> std::pair<bool, bool> {
		auto controller = InputManager::instance().get_vpad_controller(n);
		if (!controller)
			return {false,false};
		auto pressed = controller->is_screen_active();
		auto toggle = controller->is_screen_active_toggle();
		return {pressed && !toggle, pressed && toggle};
	};

	const bool tabPressed = !pce::IsActive() && WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::TAB);
	const bool ctrlPressed = !pce::IsActive() && WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::LCONTROL);
	const auto [vpad0Active, vpad0Toggle] = getVPADScreenActive(0);
	const auto [vpad1Active, vpad1Toggle] = getVPADScreenActive(1);

	const bool altScreenRequested = (!ctrlPressed && tabPressed) || vpad0Active || vpad1Active;
	const bool togglePressed = (ctrlPressed && tabPressed) || vpad0Toggle || vpad1Toggle;
	static bool togglePressedLast = false;

	bool& isDRCPrimary = LatteGPUState.isDRCPrimary;

	if(togglePressed && !togglePressedLast)
		isDRCPrimary = !isDRCPrimary;
	togglePressedLast = togglePressed;

	bool showDRC = swkbd_hasKeyboardInputHook() == false && (isDRCPrimary ^ altScreenRequested);

	if ((renderTarget & RENDER_TARGET_DRC) && g_renderer->IsPadWindowActive())
		LatteRenderTarget_copyToBackbuffer(texView, true);
	if (((renderTarget & RENDER_TARGET_TV) && !showDRC) || ((renderTarget & RENDER_TARGET_DRC) && showDRC))
		LatteRenderTarget_copyToBackbuffer(texView, false);
}



// returns the current size of the virtual viewport (not the same as effective size, which can be influenced by texture rules)
void LatteRenderTarget_GetCurrentVirtualViewportSize(sint32* viewportWidth, sint32* viewportHeight)
{
	*viewportWidth = sLatteRenderTargetState.currentGuestViewport.width;
	*viewportHeight = sLatteRenderTargetState.currentGuestViewport.height;
}

void LatteRenderTarget_updateViewport()
{
	float vpWidth = LatteGPUState.contextNew.PA_CL_VPORT_XSCALE.get_SCALE() / 0.5f;
	float vpX = LatteGPUState.contextNew.PA_CL_VPORT_XOFFSET.get_OFFSET() - LatteGPUState.contextNew.PA_CL_VPORT_XSCALE.get_SCALE();
	float vpHeight = LatteGPUState.contextNew.PA_CL_VPORT_YSCALE.get_SCALE() / -0.5f;
	float vpY = LatteGPUState.contextNew.PA_CL_VPORT_YOFFSET.get_OFFSET() + LatteGPUState.contextNew.PA_CL_VPORT_YSCALE.get_SCALE();

	bool halfZ = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF();

	// calculate near/far
	float farZ;
	float nearZ;
	float s = LatteGPUState.contextNew.PA_CL_VPORT_ZSCALE.get_SCALE();
	float b = LatteGPUState.contextNew.PA_CL_VPORT_ZOFFSET.get_OFFSET();
	if (halfZ == false)
	{
		farZ = s + b;
		nearZ = b - s;
	}
	else
	{
		farZ = s + b;
		nearZ = b;
	}

	sLatteRenderTargetState.currentGuestViewport.width = vpWidth;
	sLatteRenderTargetState.currentGuestViewport.height = vpHeight;
	if (sLatteRenderTargetState.renderTargetIsResized)
	{
		vpX *= ((float)sLatteRenderTargetState.currentEffectiveSize.width / (float)sLatteRenderTargetState.currentRenderSize.width);
		vpY *= ((float)sLatteRenderTargetState.currentEffectiveSize.height / (float)sLatteRenderTargetState.currentRenderSize.height);
		vpWidth *= ((float)sLatteRenderTargetState.currentEffectiveSize.width / (float)sLatteRenderTargetState.currentRenderSize.width);
		vpHeight *= ((float)sLatteRenderTargetState.currentEffectiveSize.height / (float)sLatteRenderTargetState.currentRenderSize.height);
	}
	g_renderer->renderTarget_setViewport(vpX, vpY, vpWidth, vpHeight, nearZ, farZ, halfZ);
}

void LatteRenderTarget_updateScissorBox()
{
	// update scissor box
	uint32 scissorX = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_TL_X();
	uint32 scissorY = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_TL_Y();
	uint32 scissorWidth = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_X() - scissorX;
	uint32 scissorHeight = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y() - scissorY;
	if( sLatteRenderTargetState.renderTargetIsResized )
	{
		scissorX = (sint32)((float)scissorX * ((float)sLatteRenderTargetState.currentEffectiveSize.width / (float)sLatteRenderTargetState.currentRenderSize.width));
		scissorY = (sint32)((float)scissorY * ((float)sLatteRenderTargetState.currentEffectiveSize.height / (float)sLatteRenderTargetState.currentRenderSize.height));
		scissorWidth = (sint32)((float)scissorWidth * ((float)sLatteRenderTargetState.currentEffectiveSize.width / (float)sLatteRenderTargetState.currentRenderSize.width));
		scissorHeight = (sint32)((float)scissorHeight * ((float)sLatteRenderTargetState.currentEffectiveSize.height / (float)sLatteRenderTargetState.currentRenderSize.height));
	}

	if( scissorX != prevScissorX || scissorY != prevScissorY || scissorWidth != prevScissorWidth || scissorHeight != prevScissorHeight )
	{
		g_renderer->renderTarget_setScissor(scissorX, scissorY, scissorWidth, scissorHeight);
		prevScissorX = scissorX;
		prevScissorY = scissorY;
		prevScissorWidth = scissorWidth;
		prevScissorHeight = scissorHeight;
	}
}

void LatteRenderTarget_unloadAll()
{
	if (g_emptyFBO)
	{
		LatteMRT::DeleteCachedFBO(g_emptyFBO);
		g_emptyFBO = nullptr;
	}
}
