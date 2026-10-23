#include "pcedition/PCGraphics.h"
#include "pcedition/PCPrompts.h"
#include "pcedition/PCEdition.h"
#include "pcedition/PCSettings.h"

#if BOOST_OS_WINDOWS
#include <windows.h>
#include <shlobj.h>
#endif
#include <fstream>
#include <boost/algorithm/string.hpp>

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/CafeSystem.h"

namespace pce::Graphics
{
	namespace
	{
		bool sRestartPending = false;
		Settings sAppliedAtBoot;

		// The community graphic packs this edition builds on ("Graphics": resolution / shadows / anti-aliasing, "FPS++":
		// frame rate with corrected game speed). Sources in order: the copy shipped with the edition
		// (tools/get_graphic_packs.ps1), the packs downloaded by an existing Cemu installation (read only).
		std::vector<fs::path> CommunityPackSources()
		{
			std::vector<fs::path> r;
			r.emplace_back(GetExeDir() / "resources" / "pcedition" / "graphicPacks" / "BreathOfTheWild");
#if BOOST_OS_WINDOWS
			PWSTR path = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)))
				r.emplace_back(fs::path(path) / "Cemu" / "graphicPacks" / "downloadedGraphicPacks" / "BreathOfTheWild");
			CoTaskMemFree(path);
#else
			const char* home = getenv("HOME");
			if (home)
				r.emplace_back(fs::path(home) / ".local" / "share" / "Cemu" / "graphicPacks" / "downloadedGraphicPacks" / "BreathOfTheWild");
#endif
			return r;
		}

		bool SameFile(const fs::path& a, const fs::path& b)
		{
			std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
			if (!fa || !fb)
				return false;
			const std::string sa((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
			const std::string sb((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
			return sa == sb;
		}

		fs::path PacksDir()
		{
			return ActiveSettings::GetUserDataPath("graphicPacks");
		}

		std::shared_ptr<GraphicPack2> FindPack(std::string_view relDirSuffix)
		{
			for (auto& gp : GraphicPack2::GetGraphicPacks())
			{
				std::string p = gp->GetNormalizedPathString();
				boost::replace_all(p, "\\", "/");
				if (boost::icontains(p, std::string(relDirSuffix) + "/rules.txt"))
					return gp;
			}
			return nullptr;
		}

		// select the first preset of a category whose name starts with the given prefix
		bool SelectPreset(const std::shared_ptr<GraphicPack2>& gp, const std::string& category, const std::string& namePrefix, bool* changed = nullptr)
		{
			for (auto& p : gp->GetPresets())
			{
				if (p->category == category && boost::istarts_with(p->name, namePrefix))
				{
					if (gp->GetActivePreset(category) != p->name)
					{
						gp->SetActivePreset(category, p->name, false);
						if (changed)
							*changed = true;
					}
					return true;
				}
			}
			cemuLog_log(LogType::Force, "PCE: preset '{}' / '{}' not found in {}", category, namePrefix, gp->GetName());
			return false;
		}

		// Mouse camera for the main (chase) camera controller of U-King.rpx v1.5.0 (module checksum 0x6267BFD0).
		// The camera update (0x02B9B8A0) computes per-frame yaw/pitch deltas in degrees from the right stick;
		// the hooks add the exact mouse rotation through HLE calls into the PC Edition input module.
		void WriteCameraPatch(const fs::path& file)
		{
			std::ofstream f(file, std::ios::trunc);
			f << R"([BotWPCE_MouseCamera]
moduleMatches = 0x6267BFD0

.origin = codecave

pceNormDelta:
.float 0.0

; camera update entry: report that the main camera is active this frame (r3 = camera controller)
_pceCamEntry:
mflr r0
bl import.coreinit.PCE_CamTick
mtlr r0
lis r12, pceNormDelta@ha
li r11, 0
stw r11, pceNormDelta@l(r12)
stwu r1, -0x298(r1)
b 0x02B9B8A4

; climbing camera (walls, ladders): its update (0x02BEE0F8, r3 = camera) keeps yaw / pitch in degrees at +0x98 / +0x8C;
; the mouse rotation is added to them before the update runs
_pceClimbCamEntry:
mflr r0
bl import.coreinit.PCE_ClimbCamTick
mtlr r0
stwu r1, -0x1f8(r1)
b 0x02BEE0FC

; horse camera (riding): its update (0x02BD2A44, r3 = camera) takes its angles from the pose every frame; the eye is
; turned around the look-at point by the mouse rotation before the update runs
_pceHorseCamEntry:
mflr r0
bl import.coreinit.PCE_HorseCamTick
mtlr r0
stwu r1, -0x108(r1)
b 0x02BD2A48

; master cycle zero camera (riding): its update (0x02BDFB20, r3 = camera) takes its angles from the pose every frame;
; the eye is turned around the look-at point by the mouse rotation before the update runs
_pceCycleCamEntry:
mflr r0
bl import.coreinit.PCE_CycleCamTick
mtlr r0
stwu r1, -0x170(r1)
b 0x02BDFB24

; conversation camera: its update (0x02BCB3A0, r3 = camera) keeps yaw / pitch in degrees at +0x48 / +0x40
_pceTalkCamEntry:
mflr r0
bl import.coreinit.PCE_TalkCamTick
mtlr r0
stwu r1, -0x190(r1)
b 0x02BCB3A4

; Magnesis with an object held: the rune's action (update 0x020D72D0, r3 = action) keeps the direction of the object
; around Link; the mouse rotation is added to it before the update runs
_pceMagnetEntry:
mflr r0
bl import.coreinit.PCE_MagnetTick
mtlr r0
stwu r1, -0x108(r1)
b 0x020D72D4

; ... and its camera (update 0x02BF0234) is turned by the same angle: r3 = pose of the camera (0 = none), r29 = camera
; (replaces "or. r27, r3, r3"; the function has saved its return address and r27 by then)
_pceMagnetCam:
mflr r27
mr r4, r29
bl import.coreinit.PCE_MagnetCamTick
mtlr r27
or. r27, r3, r3
blr

; ... and the position of the object it frames is turned by what the object has not done yet: end of the camera's
; "where is the object" (0x02BD8A30), r31 = camera (replaces the reload of r0 from 0x14(r1); r3 is kept)
_pceMagnetTarget:
mflr r12
mr r4, r31
bl import.coreinit.PCE_MagnetTarget
mtlr r12
lwz r0, 0x14(r1)
blr

; ... and the action's own check whether the object is held back (in 0x020D6A50) is corrected for the turn: the call
; of 0x03C6FD44 (f1 = distance between the points at r3 and r4) is made from here, then f1 is looked at with Link's
; position (0x38(r1), the frame is still the caller's). r31 is free at that point: the function reloads it before use.
_pceMagnetDistance:
mflr r31
lis r12, 0x03C7
addi r12, r12, -700
mtctr r12
bctrl
addi r3, r1, 0x38
bl import.coreinit.PCE_MagnetDistance
mtlr r31
blr

; manual yaw: f13 = rotation for this frame (scaled later by the frame time factor at this+0x120)
_pceYaw:
fmuls f13, f10, f6
mflr r11
bl import.coreinit.PCE_CamYaw
mtlr r11
lfs f0, 0x124(r31)
fmadds f13, f1, f0, f13
blr

; pitch: convert the mouse pitch (degrees) into the controller's normalized pitch parameter
; using the game's own mapping (0x02B96758), so the camera distance follows the original orbit curve
_pcePitchPrep:
fmr f23, f1
mflr r0
stwu r1, -0x20(r1)
stw r0, 0x24(r1)
bl import.coreinit.PCE_CamPitch
lfs f2, 0xcc(r31)
fadds f1, f2, f1
mr r3, r31
lis r12, 0x02B9
ori r12, r12, 0x6758
mtctr r12
bctrl
fsubs f1, f1, f23
lis r12, pceNormDelta@ha
stfs f1, pceNormDelta@l(r12)
lwz r0, 0x24(r1)
mtlr r0
addi r1, r1, 0x20
blr

; add the normalized mouse pitch delta to the stick delta (stick path and auto path)
_pcePitch:
lis r12, pceNormDelta@ha
lfs f0, pceNormDelta@l(r12)
fadds f1, f1, f0
fadds f23, f23, f1
blr

; lock-on camera (a target held with the lock-on button; the class of the Magnesis camera): its update (0x02BF0234,
; r3 = camera) keeps yaw / pitch in degrees at +0xAC / +0xA8, the mouse rotation is added before the update runs
_pceLockCamEntry:
mflr r0
bl import.coreinit.PCE_LockCamTick
mtlr r0
stwu r1, -0x220(r1)
b 0x02BF0238

; camera rune (the Sheikah Slate's camera): its update (0x02BCED3C, r3 = camera) keeps the pitch in degrees at +0x28, the
; mouse pitch is added before the update runs
_pceRuneCamEntry:
mflr r0
bl import.coreinit.PCE_RuneCamTick
mtlr r0
stwu r1, -0x1f8(r1)
b 0x02BCED40

; ... and its yaw: f1 = stick x (f31) * rate (f27) + current yaw (f12), degrees; r11 and f2 are free (a call follows)
_pceRuneCamYaw:
fmuls f1, f31, f27
fmr f2, f12
mflr r11
bl import.coreinit.PCE_AimYawAbs
mtlr r11
fadds f1, f12, f1
blr

; shield surf camera: its update (0x02BE8B30, r3 = camera)
_pceSurfCamEntry:
mflr r0
bl import.coreinit.PCE_SurfCamTick
mtlr r0
stwu r1, -0x280(r1)
b 0x02BE8B34

; ... its pitch (degrees, +0xA4) as the stick turns it: f1 = pitch, r29 = camera; r11 is free (a call follows)
_pceSurfCamPitch:
lfs f1, 0xa4(r29)
mflr r11
mr r3, r29
li r4, 0
bl import.coreinit.PCE_SurfCamPitch
mtlr r11
blr

; sand seal camera (towed on the shield): the same kind of camera as the shield surf one, update 0x02BF5ACC (r3 =
; camera), pitch +0xA0 read at 0x02BF5E80 (camera in r30), yaw at 0x02BF65E8: f1 = current yaw (f27) + stick turn (f8)
_pceSealCamEntry:
mflr r0
bl import.coreinit.PCE_SealCamTick
mtlr r0
stwu r1, -0x280(r1)
b 0x02BF5AD0

_pceSealCamPitch:
lfs f1, 0xa0(r30)
mflr r11
mr r3, r30
li r4, 1
bl import.coreinit.PCE_SurfCamPitch
mtlr r11
blr

_pceSealCamYaw:
fadds f1, f27, f8
mflr r11
bl import.coreinit.PCE_SurfCamYaw
mtlr r11
blr

; ... and its yaw while the stick is held: f1 = current yaw (f30) + stick turn (f0), degrees; r11 is free (a call follows)
_pceSurfCamYaw:
fadds f1, f30, f0
mflr r11
bl import.coreinit.PCE_SurfCamYaw
mtlr r11
blr

; waterfall camera (swimming up a waterfall with the Zora armour): its update (0x02BF8F88, r3 = camera) turns only the yaw,
; by the stick while mode +0x28 is 2: f1 = current yaw (f31) + stick turn (f30), degrees, at 0x02BF914C
_pceFallCamEntry:
mflr r0
bl import.coreinit.PCE_FallCamTick
mtlr r0
stwu r1, -0x128(r1)
b 0x02BF8F8C

_pceFallCamYaw:
fadds f1, f31, f30
mflr r11
bl import.coreinit.PCE_SurfCamYaw
mtlr r11
blr

; ---- the two aim cameras: 0x02B9250C is the zoomed one (scope, bow drawn: "Scope" below), 0x02B94E30 the one of the
; runes that are aimed (Magnesis, Stasis, Cryonis: "Bow" below, named before a bow could be tested) ----
_pceScopeEntry:
mflr r0
bl import.coreinit.PCE_ScopeTick
mtlr r0
stwu r1, -0x1a8(r1)
b 0x02B92510

_pceBowEntry:
mflr r0
bl import.coreinit.PCE_AimTick
mtlr r0
stwu r1, -0x1e0(r1)
b 0x02B94E34

; scope pitch: f1 = new pitch (degrees) = old + stick delta
_pceScopePitch:
fmadds f1, f26, f10, f11
mflr r11
fmr f13, f1
bl import.coreinit.PCE_AimPitch
fadds f1, f13, f1
mtlr r11
blr

; scope yaw: f1 = yaw delta (degrees, after the frame time factor), base yaw at 0x10(r1)
_pceScopeYaw:
fmuls f1, f27, f8
mflr r11
lfs f2, 0x10(r1)
bl import.coreinit.PCE_AimYawAbs
mtlr r11
blr

; bow yaw: same layout as the scope
_pceBowYaw:
fmuls f1, f26, f9
mflr r11
lfs f2, 0x10(r1)
bl import.coreinit.PCE_AimYawAbs
mtlr r11
blr

; bow pitch: pitch angle (degrees) before clamping / normalization
_pceBowPitch:
mflr r11
bl import.coreinit.PCE_AimPitch
mtlr r11
lfs f2, 0x134(r31)
fadds f1, f2, f1
blr

0x02BCED3C = b _pceRuneCamEntry
0x02BF0234 = b _pceLockCamEntry
0x02BCF698 = bla _pceRuneCamYaw
0x02BE8B30 = b _pceSurfCamEntry
0x02BE8EF8 = bla _pceSurfCamPitch
0x02BE9698 = bla _pceSurfCamYaw
0x02BF5ACC = b _pceSealCamEntry
0x02BF5E80 = bla _pceSealCamPitch
0x02BF65E8 = bla _pceSealCamYaw
0x02BF8F88 = b _pceFallCamEntry
0x02BF914C = bla _pceFallCamYaw
0x02B9250C = b _pceScopeEntry
0x02B94E30 = b _pceBowEntry
0x02B92848 = bla _pceScopePitch
0x02B92A48 = bla _pceScopeYaw
0x02B956B0 = bla _pceBowYaw
0x02B95278 = bla _pceBowPitch

0x02B9B8A0 = b _pceCamEntry
0x02BEE0F8 = b _pceClimbCamEntry
0x02BCB3A0 = b _pceTalkCamEntry
0x02BD2A44 = b _pceHorseCamEntry
0x02BDFB20 = b _pceCycleCamEntry
0x020D72D0 = b _pceMagnetEntry
0x02BF02A8 = bla _pceMagnetCam
0x02BD8AB8 = bla _pceMagnetTarget
0x020D6DAC = bla _pceMagnetDistance
0x02B9C950 = bla _pceYaw
0x02B9C120 = bla _pcePitchPrep
0x02B9C188 = bla _pcePitch
0x02B9C1AC = bla _pcePitch

[BotWPCE_UI]
moduleMatches = 0x6267BFD0

.origin = codecave

; nw::lyt::TextBox::SetString epilogue: r29 = text box (full string in its buffer)
_pceUIText:
mflr r12
mr r3, r29
bl import.coreinit.PCE_UIText
mtlr r12
lwz r29, 0xc(r1)
blr

; ui screen layout load: r30 = screen, r24 = layout name
_pceUIScreen:
mflr r0
mr r3, r30
mr r4, r24
bl import.coreinit.PCE_UIScreen
mtlr r0
lwz r12, 0xe4(r11)
blr

; paraglider: the per-frame update of the player's glide state (0x02CB6FE8) reports that Link is gliding this frame
; (replaces "mr r30, r3", after the function saved its return address)
_pceGlideTick:
mflr r12
bl import.coreinit.PCE_GlideTick
mtlr r12
mr r30, r3
blr

; world map screen update: r27 = map screen (its map controller holds the scroll position)
_pceMapObj:
mflr r12
mr r3, r27
bl import.coreinit.PCE_MapObj
mtlr r12
lwz r6, 0xa8(r27)
blr

; menu box cursor: per-frame update of its manager (r31), reached only while the cursor takes input. PCE_CursorTick
; may put an entry into the manager's request slot (+0x28); the code that follows checks and focuses it.
_pceCursorTick:
stw r29, 0x20(r1)
mflr r12
mr r3, r31
bl import.coreinit.PCE_CursorTick
mtlr r12
blr

; list widget of the shop style screens (r30), right after it asked the cursor for its current entry (r31):
; PCE_ListTick may set the index the widget is to select next (+0x29C)
_pceListTick:
stw r31, 0x2a8(r30)
mflr r12
mr r3, r30
mr r4, r31
bl import.coreinit.PCE_ListTick
mtlr r12
blr

; temperature unit. The game shows Fahrenheit when its region language (0x030ADCF0) is USen (1). Two places ask:
; the conversion for the map (0x0307BE74: x * 1.8 + 32), and the code that sets the frame of a layout's
; "RegionLanguageType" animation (0x03A2F870, r30 = layout: unit symbol of the map, temperature gauge of the HUD).
; Both get the answer of PCE_RegionLanguage(r3 = region language, r4 = layout or 0).
_pceRegionLanguageLayout:
stwu r1, -0x10(r1)
mflr r0
stw r0, 0x14(r1)
bl 0x030ADCF0
mr r4, r30
bl import.coreinit.PCE_RegionLanguage
lwz r0, 0x14(r1)
mtlr r0
addi r1, r1, 0x10
blr
_pceRegionLanguageTemp:
stwu r1, -0x10(r1)
mflr r0
stw r0, 0x14(r1)
bl 0x030ADCF0
li r4, 0
bl import.coreinit.PCE_RegionLanguage
lwz r0, 0x14(r1)
mtlr r0
addi r1, r1, 0x10
blr

; inventory page carousel: in place of easing the scroll offset (screen+0x1B98, replaces bl ease(r3, r4 = &offset,
; f1 = target, ...)) PCE_SlideSnap sets it (at once: a page change is applied in the frame of its flick, or a short slide)
; and returns r11 = 1; r11 = 0 keeps the game's easing (the Slow page turn setting)
_pceSlideSnap:
mflr r0
bl import.coreinit.PCE_SlideSnap
mtlr r0
cmpwi r11, 0
bne _pceSlideSnapDone
b 0x03082AB0
_pceSlideSnapDone:
li r3, 1
blr

; inventory, before the page carousel update (0x02FF644C, r3 = inventory screen): PCE_InvJump r11 = 1 after it wrote the
; first page of a clicked category to +0x1B90; the game's own page jump (0x02FF3AC0) then shows it, without sliding
_pceInvUpdate:
mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
stw r31, 0xc(r1)
mr r31, r3
bl import.coreinit.PCE_InvJump
cmpwi r11, 0
beq _pceInvUpdateCarousel
mr r3, r31
bl 0x02FF3AC0
_pceInvUpdateCarousel:
mr r3, r31
bl 0x02FF644C
lwz r31, 0xc(r1)
lwz r0, 0x14(r1)
mtlr r0
addi r1, r1, 0x10
blr

0x03C4BA98 = bla _pceUIText
0x03A3EDC8 = bla _pceUIScreen
0x02F033E0 = bla _pceMapObj
0x03A2E8E4 = bla _pceCursorTick
0x02E816E8 = bla _pceListTick
0x03A2FB5C = bla _pceRegionLanguageLayout
0x0307BE94 = bla _pceRegionLanguageTemp
0x02CB7004 = bla _pceGlideTick
0x02FF64CC = bla _pceSlideSnap
0x02FF6768 = bla _pceInvUpdate
0x02FF67EC = bla _pceInvUpdate
)";
		}

		void SetPacksFromSettings(const Settings& s, bool live, bool* fpsChanged = nullptr, bool* gfxChanged = nullptr)
		{
			// resolution, shadows, anti-aliasing
			if (auto gfx = FindPack("BreathOfTheWild/Graphics"))
			{
				gfx->SetEnabled(true);
				const int aspect = EffectiveAspect(s.aspectRatio);
				const auto& res = GetResolutionOptions(aspect);
				const int ri = std::clamp(s.resolutionPreset, 0, (int)res.size() - 1);
				if (!live)
				{
					static const char* kAspect[] = {"16:9", "16:9", "16:10", "21:9", "32:9"};
					SelectPreset(gfx, "Aspect Ratio", kAspect[aspect], gfxChanged);
					SelectPreset(gfx, "Resolution", fmt::format("{}x{}", res[ri].width, res[ri].height), gfxChanged);
					if (aspect >= 3)
						SelectPreset(gfx, "Ultrawide HUD Mode", s.ultrawideHud == 1 ? "Centered HUD" : "Edge HUD", gfxChanged);
					cemuLog_log(LogType::Force, "PCE: aspect ratio {} (setting {}), resolution {}x{}", kAspect[aspect], s.aspectRatio, res[ri].width, res[ri].height);
					static const char* kShadow[] = {"Low", "Medium", "High", "Ultra"};
					SelectPreset(gfx, "Shadows", kShadow[std::clamp(s.shadowPreset, 0, 3)], gfxChanged);
					static const char* kShadowDist[] = {"Medium", "High", "Very High", "Ultra"};
					SelectPreset(gfx, "Shadow Draw Distance", kShadowDist[std::clamp(s.shadowPreset, 0, 3)], gfxChanged);
				}
				static const char* kAA[] = {"None", "Normal FXAA", "NVIDIA FXAA"};
				SelectPreset(gfx, "Anti-Aliasing", kAA[std::clamp(s.antiAliasing, 0, 2)], gfxChanged);
			}
			// FPS++: full dynamic game speed (physics corrected), user chosen limit
			if (auto fps = FindPack("BreathOfTheWild/Mods/FPS++"))
			{
				fps->SetEnabled(true);
				SelectPreset(fps, "Mode", "Normal Settings", fpsChanged);
				SelectPreset(fps, "Fence Type", "Performance Fence", fpsChanged);
				SelectPreset(fps, "Cutscene FPS Limit", "Automatically Limit", fpsChanged);
				SelectPreset(fps, "Frame Average", "8 Frames", fpsChanged);
				SelectPreset(fps, "Static Mode", "Disabled", fpsChanged);
				SelectPreset(fps, "Debug Options", "Disabled", fpsChanged);
				// with frame generation the limit counts the frames shown: the game runs at half of it (steady pacing too)
				static const int kLimits[] = {20, 30, 45, 55, 60, 72, 75, 90, 120, 144, 165, 240}; // FPS++ presets
				const int target = s.frameGeneration == 1 ? s.fpsLimit / 2 : s.fpsLimit;
				int limit = kLimits[0];
				for (int v : kLimits)
					if (target >= v)
						limit = v;
				SelectPreset(fps, "FPS Limit", fmt::format("{}FPS Limit", limit), fpsChanged);
				SelectPreset(fps, "Framerate Limit", fmt::format("{}FPS (", limit), fpsChanged);
			}
			if (auto pce = FindPack("BotWPCEdition"))
				pce->SetEnabled(true);
		}
	}

	int EffectiveAspect(int setting)
	{
		if (setting >= 1 && setting <= 4)
			return setting;
		// automatic: the shape of the main display
		int w = 1920, h = 1080;
#if BOOST_OS_WINDOWS
		w = GetSystemMetrics(SM_CXSCREEN);
		h = GetSystemMetrics(SM_CYSCREEN);
#endif
		const double ratio = h > 0 ? (double)w / (double)h : 16.0 / 9.0;
		return ratio > 3.2 ? 4 : (ratio > 2.2 ? 3 : 1); // 32:9 = 3.56, 21:9 = 2.33 - 2.39, 16:9 = 1.78
	}

	const std::vector<std::string>& GetAspectOptions()
	{
		static const std::vector<std::string> r = {"Automatic", "16:9", "16:10", "21:9 (ultrawide)", "32:9 (super ultrawide)"};
		return r;
	}

	const std::vector<ResolutionOption>& GetResolutionOptions(int aspect)
	{
		// (resolutions the community graphics pack has for each aspect ratio)
		static const std::vector<ResolutionOption> r16_9 = {
			{1280, 720, "1280 x 720"}, {1920, 1080, "1920 x 1080"}, {2560, 1440, "2560 x 1440"}, {3200, 1800, "3200 x 1800"}, {3840, 2160, "3840 x 2160"},
		};
		static const std::vector<ResolutionOption> r16_10 = {
			{1280, 800, "1280 x 800"}, {1920, 1200, "1920 x 1200"}, {2560, 1600, "2560 x 1600"}, {2880, 1800, "2880 x 1800"}, {3840, 2400, "3840 x 2400"},
		};
		static const std::vector<ResolutionOption> r21_9 = {
			{1720, 720, "1720 x 720"}, {2560, 1080, "2560 x 1080"}, {3440, 1440, "3440 x 1440"}, {4300, 1800, "4300 x 1800"}, {5120, 2160, "5120 x 2160"},
		};
		static const std::vector<ResolutionOption> r32_9 = {
			{3840, 1080, "3840 x 1080 (smallest)"}, {3840, 1080, "3840 x 1080"}, {5120, 1440, "5120 x 1440"}, {5120, 1440, "5120 x 1440 (largest below 2160)"}, {7680, 2160, "7680 x 2160"},
		};
		switch (aspect)
		{
		case 2: return r16_10;
		case 3: return r21_9;
		case 4: return r32_9;
		default: return r16_9;
		}
	}

	bool GetPictureArea(int& x, int& y, int& width, int& height, int& windowWidth, int& windowHeight)
	{
		sint32 ix = 0, iy = 0, iw = 0, ih = 0, fw = 0, fh = 0;
		LatteRenderTarget_getScreenImageArea(&ix, &iy, &iw, &ih, &fw, &fh, false);
		x = ix; y = iy; width = iw; height = ih; windowWidth = fw; windowHeight = fh;
		return iw > 0 && ih > 0 && fw > 0 && fh > 0;
	}

	const std::vector<std::string>& GetShadowOptions()
	{
		static const std::vector<std::string> r = {"Low", "Medium", "High", "Ultra"};
		return r;
	}

	const std::vector<std::string>& GetAAOptions()
	{
		static const std::vector<std::string> r = {"Off", "FXAA", "FXAA (High Quality)"};
		return r;
	}

	const std::vector<int>& GetFpsOptions()
	{
		static const std::vector<int> r = {30, 60};
		return r;
	}

	fs::path GetEditionPackDir()
	{
		return PacksDir() / "BotWPCEdition";
	}

	namespace
	{
		std::atomic<int> sDisplayModeApplied{1};
	}

	void SetDisplayModeApplied(int mode)
	{
		sDisplayModeApplied = mode;
	}

	int GetDisplayModeApplied()
	{
		return sDisplayModeApplied.load();
	}

	void PrepareGraphicPacks()
	{
		std::error_code ec;
		const fs::path dst = PacksDir() / "BreathOfTheWild";
		// copy the community packs we use into the data folder: once, and again when the shipped copy changed
		const auto sources = CommunityPackSources();
		for (const char* sub : {"Graphics", "Mods/FPS++"})
		{
			const fs::path d = dst / sub;
			bool have = fs::exists(d / "rules.txt", ec);
			for (size_t i = 0; i < sources.size(); i++)
			{
				const fs::path s = sources[i] / sub;
				if (!fs::exists(s / "rules.txt", ec))
					continue;
				const bool shipped = i == 0;
				if (have && (!shipped || SameFile(s / "rules.txt", d / "rules.txt")))
					break;
				fs::remove_all(d, ec);
				fs::create_directories(d, ec);
				fs::copy(s, d, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
				cemuLog_log(LogType::Force, "PCE: copied graphic pack {} from {} ({})", sub, _pathToUtf8(s), ec ? ec.message() : "ok");
				have = !ec;
				break;
			}
			if (!have)
				cemuLog_log(LogType::Force, "PCE: graphic pack {} is missing (run tools/get_graphic_packs.ps1): the game runs at 30 FPS / 720p without it", sub);
		}
		// PC Edition pack: rules + patches are generated by the edition at startup
		const fs::path pce = GetEditionPackDir();
		fs::create_directories(pce, ec);
		std::ofstream rules(pce / "rules.txt", std::ios::trunc);
		rules << "[Definition]\n"
				 "titleIds = 00050000101C9300,00050000101C9400,00050000101C9500\n"
				 "name = PC Edition\n"
				 "path = \"The Legend of Zelda: Breath of the Wild/PC Edition\"\n"
				 "description = Native PC controls and presentation.\n"
				 "version = 7\n";
		rules.close();
		// remove leftovers from older builds
		fs::remove(pce / "patch_dev.asm", ec);
		WriteCameraPatch(pce / "patch_PCCamera.asm");
		// keyboard & mouse button prompts (font archive + resource size table in the pack's content folder)
		Prompts::Prepare(pce);
		Prompts::PrepareTips(pce);
		sAppliedAtBoot = GetSettings();
	}

	void ApplySettingsToPacks()
	{
		SetPacksFromSettings(GetSettings(), false);
	}

	bool ApplyLive()
	{
		Settings s = GetSettings();
		GetConfig().vsync = s.vsync ? 1 : 0;
		bool fpsChanged = false, gfxChanged = false;
		SetPacksFromSettings(s, true, &fpsChanged, &gfxChanged);
		if (CafeSystem::IsTitleRunning())
		{
			if (fpsChanged)
			{
				auto gp = FindPack("BreathOfTheWild/Mods/FPS++");
				if (gp && gp->IsActivated() && !gp->RequiresRestart(false, true))
					gp->Reload();
			}
			if (gfxChanged)
			{
				auto gp = FindPack("BreathOfTheWild/Graphics");
				if (gp && gp->IsActivated() && !gp->RequiresRestart(false, true))
					gp->Reload();
			}
		}
		sRestartPending = s.resolutionPreset != sAppliedAtBoot.resolutionPreset || s.shadowPreset != sAppliedAtBoot.shadowPreset ||
						  s.aspectRatio != sAppliedAtBoot.aspectRatio || s.ultrawideHud != sAppliedAtBoot.ultrawideHud;
		return sRestartPending;
	}

	bool RestartPending()
	{
		return sRestartPending;
	}
}
