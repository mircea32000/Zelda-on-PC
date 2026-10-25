#include "pcedition/PCUI.h"
#include "pcedition/PCInput.h"
#include "pcedition/PCPrompts.h"
#include "pcedition/PCSettings.h"
#include "pcedition/PCGraphics.h"

#include <mutex>
#include <chrono>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include <deque>
#include <array>
#include <sstream>
#include <functional>

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/MMU/MMU.h"

namespace pce::UI
{
	namespace
	{
		// --- nw::lyt::Pane layout (U-King v1.5.0) ---
		constexpr uint32 kNext = 0x00;      // sibling list node
		constexpr uint32 kParent = 0x0C;
		constexpr uint32 kChildHead = 0x14; // child list head node (next / prev)
		constexpr uint32 kTranslate = 0x1C; // vec3
		constexpr uint32 kScale = 0x34;     // vec2
		constexpr uint32 kSize = 0x3C;      // vec2
		constexpr uint32 kFlag = 0x44;      // u8 flags (bit0 visible), u8 alpha, u8 global alpha, u8 base position
		constexpr uint32 kName = 0x80;      // char[24]
		constexpr uint32 kTextBuf = 0xA4;   // TextBox: char16* buffer
		constexpr uint32 kTextLen = 0xCE;   // TextBox: u16 length
		// layout space of the TV layouts
		constexpr float kLayoutW = 1280.0f, kLayoutH = 720.0f;

		struct TextInfo
		{
			std::string name;
			std::string text; // UTF-8 (control codes stripped)
			uint16 first = 0; // first UTF-16 unit (button glyphs are private use chars)
		};

		std::mutex sMutex;
		std::unordered_map<uint32, TextInfo> sTexts;
		std::unordered_map<std::string, uint32> sScreens;
		// screen object -> class signature (vtable) at registration: closed screens are freed and their memory reused
		std::unordered_map<uint32, uint32> sScreenSig;
		// screens the player escaped from with the Esc safety net: ignored until the game hides them once
		std::unordered_map<uint32, bool> sSuppressed;
		State sState;
		uint32 sStateFrame = 0xFFFFFFFF;

		bool ValidPtr(uint32 a, uint32 len = 0x100)
		{
			// (the game's heaps reach past 0x50000000 with the Extended Memory mod: its main heap is 0x50000000 larger)
			return a >= 0x10000000 && a < 0xF0000000 && memory_isAddressRangeAccessible(a, len);
		}

		std::string ReadName(uint32 pane)
		{
			char buf[25]{};
			const uint8* p = memory_getPointerFromVirtualOffset(pane + kName);
			for (int i = 0; i < 24; i++)
			{
				char c = (char)p[i];
				if (c == 0)
					break;
				if (c < 0x20 || c > 0x7E)
					return {};
				buf[i] = c;
			}
			return buf;
		}

		float RF(uint32 a)
		{
			uint32 v = memory_readU32(a);
			float f;
			memcpy(&f, &v, 4);
			return f;
		}

		std::string Utf16ToUtf8(const std::u16string& s)
		{
			std::string o;
			for (char16_t c : s)
			{
				if (c < 0x80)
					o.push_back((char)c);
				else if (c < 0x800)
				{
					o.push_back((char)(0xC0 | (c >> 6)));
					o.push_back((char)(0x80 | (c & 0x3F)));
				}
				else
				{
					o.push_back((char)(0xE0 | (c >> 12)));
					o.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
					o.push_back((char)(0x80 | (c & 0x3F)));
				}
			}
			return o;
		}

		void OnTextSetPrompts(uint32 pane, const std::string& name, uint32 buf, uint32 len);

		void HLE_UIText(PPCInterpreter_t* hCPU)
		{
			const uint32 pane = hCPU->gpr[3];
			if (ValidPtr(pane, 0xD0))
			{
				TextInfo ti;
				ti.name = ReadName(pane);
				const uint32 buf = memory_readU32(pane + kTextBuf);
				const uint32 len = std::min<uint32>(memory_readU16(pane + kTextLen), 256);
				if (!ti.name.empty() && ValidPtr(buf, len * 2 + 2))
				{
					std::u16string s;
					for (uint32 i = 0; i < len; i++)
					{
						char16_t c = (char16_t)memory_readU16(buf + i * 2);
						if (c == 0)
							break;
						if (c == 0x0E) // inline tag: skip header + params
						{
							if (i + 3 < len)
							{
								uint32 paramBytes = memory_readU16(buf + (i + 3) * 2);
								i += 3 + (paramBytes + 1) / 2;
							}
							continue;
						}
						s.push_back(c);
					}
					ti.first = s.empty() ? 0 : (uint16)s[0];
					ti.text = Prompts::GuideToEnglish(Utf16ToUtf8(s)); // guide texts are matched in English
					std::lock_guard lock(sMutex);
					if (sTexts.size() > 30000)
						sTexts.clear();
					const std::string name = ti.name;
					sTexts[pane] = std::move(ti);
					if (Prompts::Active())
						OnTextSetPrompts(pane, name, buf, len);
				}
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_UIScreen(PPCInterpreter_t* hCPU)
		{
			const uint32 obj = hCPU->gpr[3];
			const uint32 namePtr = hCPU->gpr[4];
			if (ValidPtr(namePtr, 1))
			{
				std::string name;
				for (uint32 i = 0; i < 48; i++)
				{
					char c = (char)memory_readU8(namePtr + i);
					if (c == 0)
						break;
					name.push_back(c);
				}
				std::lock_guard lock(sMutex);
				// an object address can be reused by another screen later
				for (auto it = sScreens.begin(); it != sScreens.end();)
					it = it->second == obj ? sScreens.erase(it) : std::next(it);
				sScreens[name] = obj;
				sScreenSig[obj] = memory_readU32(obj + 0x0C);
				sSuppressed.erase(obj);
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// world map screen (r27 of the map update), refreshed every frame while the map is shown
		std::atomic<uint32> sMapObj{0};
		std::atomic<uint32> sMapObjFrame{0};
		void HLE_MapObj(PPCInterpreter_t* hCPU)
		{
			sMapObj = hCPU->gpr[3];
			sMapObjFrame = Input::GetDebug().frameCounter.load();
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		bool PaneOk(uint32 pane)
		{
			return ValidPtr(pane, 0x9C);
		}

		// ---- menu box cursor (U-King v1.5.0) ----
		// One manager (it belongs to the BoxCursorTV screen) owns the selection box of all menus. Every screen keeps a
		// list of the entries the box can rest on. The manager's update moves the box with the d-pad, and it also takes
		// a requested entry: it runs the game's own "can be selected" test on it, focuses it (callbacks, description,
		// item preview: the same path as a cursor move) and clears the request. The mouse uses that: the entry under the
		// pointer is requested and the box is on it in the next frame, instead of walking there with d-pad presses.
		//   manager  +0x0C control ([+0x4C] = BoxCursorTV screen), +0x24 focused entry, +0x28 requested entry
		//   screen   +0x14 screen list ([+0x14] count, [+0x18] screen pointers, [+0x20] one byte per slot, bit 7 = free)
		//            +0x1C control state ([+0x20] bit 1 = takes input), +0x50 entry list (first node +0x54, node
		//            offset +0x5C), +0x95 / +0x96 state (shown: +0x96 == 2 and +0x95 >= 0)
		//   entry    +0x04 next node, +0x08 owner control, +0x0C screen
		//   owner    +0x1E flags (0x10 = can be selected), +0x38 pane (global matrix: translation at +0x54 / +0x64).
		//            The pane is the hit pane ("B_Hit_00") of the part the pointer knows as a target.
		// Screens use the same request slot themselves (0x03A2EEE4), some of them in every frame for the entry they
		// consider current: the slot is then already taken when our hook runs, and a request of ours would be overruled
		// one frame later (the cursor flickers between two entries and plays its sound each time). Those screens keep
		// their own current entry:
		//   - shop style screens (the armour list of a Great Fairy, shops) have a list widget that is told the wanted
		//     entry directly (see ListIndexOf)
		//   - the others (the quests of the adventure log) are driven with the d-pad, which moves their own row
		struct FocusRequest
		{
			uint32 part = 0;
			Rect area{};
			uint32 frame = 0xF0000000;
			int result = 0; // 0 not looked at yet, 1 requested, 2 no selectable entry, 3 already focused, 4 the screen has its own row
		};
		std::mutex sFocusMutex;
		FocusRequest sFocusReq;
		std::atomic<uint32> sCursorMgr{0}, sCursorFrame{0xF0000000}, sCursorRequested{0}, sCursorOwnedFrame{0xF0000000};
		std::atomic<uint32> sListFrame{0xF0000000}; // a list widget took the request

		// levels between a pane and a part pane above it (0 = the pane itself), -1 if the part is not above it
		int LevelsBelow(uint32 pane, uint32 part)
		{
			for (int up = 0; up < 4 && PaneOk(pane); up++)
			{
				if (pane == part)
					return up;
				pane = memory_readU32(pane + kParent);
			}
			return -1;
		}

		// pane of the entry the game's cursor is on; 0 while the cursor is not running
		uint32 CursorFocusPane()
		{
			const uint32 mgr = sCursorMgr.load();
			if (Input::GetDebug().frameCounter.load() - sCursorFrame.load() > 3 || !ValidPtr(mgr, 0x30))
				return 0;
			const uint32 focus = memory_readU32(mgr + 0x24);
			if (!ValidPtr(focus, 0x30))
				return 0;
			const uint32 owner = memory_readU32(focus + 0x08);
			if (!ValidPtr(owner, 0x3C))
				return 0;
			const uint32 pane = memory_readU32(owner + 0x38);
			return PaneOk(pane) ? pane : 0;
		}

		// selectable entry of the shown menus that belongs to the part (its hit pane lies below the part pane); for a
		// target without such an entry, the one that lies inside the area (nearest to its centre)
		uint32 FindCursorEntry(uint32 mgr, uint32 part, const Rect& area)
		{
			const uint32 control = memory_readU32(mgr + 0x0C);
			if (!ValidPtr(control, 0x50))
				return 0;
			const uint32 boxScreen = memory_readU32(control + 0x4C);
			if (!ValidPtr(boxScreen, 0x18))
				return 0;
			const uint32 list = memory_readU32(boxScreen + 0x14);
			if (!ValidPtr(list, 0x24))
				return 0;
			const uint32 count = std::min<uint32>(memory_readU32(list + 0x14), 256);
			const uint32 screens = memory_readU32(list + 0x18), freeFlags = memory_readU32(list + 0x20);
			if (!count || !ValidPtr(screens, count * 4) || !ValidPtr(freeFlags, count))
				return 0;
			uint32 best = 0, bestOwn = 0;
			float bestDist = 1e9f;
			int bestLevels = 99;
			for (uint32 i = 0; i < count; i++)
			{
				if (memory_readU8(freeFlags + i) & 0x80)
					continue;
				const uint32 screen = memory_readU32(screens + i * 4);
				if (!ValidPtr(screen, 0xA0) || (sint8)memory_readU8(screen + 0x96) != 2 || (sint8)memory_readU8(screen + 0x95) < 0)
					continue;
				const uint32 state = memory_readU32(screen + 0x1C);
				if (!ValidPtr(state, 0x24) || !(memory_readU32(state + 0x20) & 2))
					continue;
				const uint32 offset = memory_readU32(screen + 0x5C), end = screen + 0x50;
				uint32 node = memory_readU32(screen + 0x54);
				for (int n = 0; n < 512 && node != end && ValidPtr(node, 8); n++, node = memory_readU32(node + 4))
				{
					const uint32 entry = node - offset;
					if (!ValidPtr(entry, 0x30))
						break;
					const uint32 owner = memory_readU32(entry + 0x08);
					if (!ValidPtr(owner, 0x3C) || !(memory_readU16(owner + 0x1E) & 0x10))
						continue;
					const uint32 pane = memory_readU32(owner + 0x38);
					if (!PaneOk(pane))
						continue;
					const int levels = part ? LevelsBelow(pane, part) : -1;
					if (levels >= 0 && levels < bestLevels)
					{
						bestLevels = levels;
						bestOwn = entry;
					}
					const float x = 0.5f + RF(pane + 0x54) / kLayoutW, y = 0.5f - RF(pane + 0x64) / kLayoutH;
					if (!area.Contains(x, y))
						continue;
					const float dx = x - area.cx(), dy = y - area.cy();
					if (dx * dx + dy * dy < bestDist)
					{
						bestDist = dx * dx + dy * dy;
						best = entry;
					}
				}
			}
			return bestOwn ? bestOwn : best;
		}

		// ---- list widget of the shop style screens (U-King v1.5.0, update 0x02E810CC) ----
		// It has a current index (+0x298) and a wanted index (+0x29C). Its update adopts the wanted one (the d-pad
		// changes it) and asks the cursor for the entry of the current one in every frame. Writing the wanted index
		// selects an entry in the next frame, with everything the game does on a cursor move.
		//   widget    +0x21C / +0x224 categories (count / pointers), +0x298 current index, +0x29C wanted index
		//   category  +0x04 pages {+0x00 count, +0x08 pointers}, +0x230 number of entries, +0x234 * +0x238 entries per page
		//   page      +0x04 category, +0x0C / +0x14 slots (count / pointers), +0x1C state (+0x0C page number, < 0 unused)
		//   slot      +0x08 page, +0x10 cursor entry, +0x14 flags (2 = in use), +0x20 position on its page
		// The pages are shared: a page shows one category at a time (its +0x04).
		// index = entries of the categories before it + page number * entries per page + position (0x02E7C340 /
		// 0x02E73E54 are the game's own conversions)
		sint32 ListIndexOf(uint32 widget, uint32 entry)
		{
			const uint32 catCount = std::min<uint32>(memory_readU32(widget + 0x21C), 32), cats = memory_readU32(widget + 0x224);
			if (!catCount || !ValidPtr(cats, catCount * 4))
				return -1;
			// the slot that shows the entry (the pages are shared by the categories: any category leads to them)
			uint32 found = 0;
			for (uint32 c = 0; c < catCount && !found; c++)
			{
				const uint32 cat = memory_readU32(cats + c * 4);
				const uint32 pages = ValidPtr(cat, 0x280) ? memory_readU32(cat + 0x04) : 0;
				const uint32 pageCount = ValidPtr(pages, 0x0C) ? std::min<uint32>(memory_readU32(pages), 16) : 0;
				const uint32 pageArr = pageCount ? memory_readU32(pages + 0x08) : 0;
				for (uint32 p = 0; p < pageCount && ValidPtr(pageArr, pageCount * 4) && !found; p++)
				{
					const uint32 page = memory_readU32(pageArr + p * 4);
					if (!ValidPtr(page, 0x20))
						continue;
					const uint32 slotCount = std::min<uint32>(memory_readU32(page + 0x0C), 64), slots = memory_readU32(page + 0x14);
					for (uint32 k = 0; k < slotCount && ValidPtr(slots, slotCount * 4); k++)
					{
						const uint32 slot = memory_readU32(slots + k * 4);
						if (ValidPtr(slot, 0x30) && (memory_readU32(slot + 0x14) & 2) && memory_readU32(slot + 0x10) == entry)
						{
							found = slot;
							break;
						}
					}
				}
			}
			if (!found)
				return -1;
			// its page tells the category it shows at the moment and the page number
			const uint32 page = memory_readU32(found + 0x08);
			if (!ValidPtr(page, 0x20))
				return -1;
			const uint32 cat = memory_readU32(page + 0x04), state = memory_readU32(page + 0x1C);
			if (!ValidPtr(cat, 0x280) || !ValidPtr(state, 0x10) || (sint32)memory_readU32(state + 0x0C) < 0)
				return -1;
			sint32 index = (sint32)(memory_readU32(state + 0x0C) * memory_readU32(cat + 0x234) * memory_readU32(cat + 0x238) + memory_readU32(found + 0x20));
			for (uint32 c = 0; c < catCount; c++)
			{
				const uint32 other = memory_readU32(cats + c * 4);
				if (other == cat)
					return index;
				if (ValidPtr(other, 0x280))
					index += (sint32)memory_readU32(other + 0x230);
			}
			return -1;
		}

		void HLE_ListTick(PPCInterpreter_t* hCPU)
		{
			const uint32 widget = hCPU->gpr[3], current = hCPU->gpr[4];
			const uint32 frame = Input::GetDebug().frameCounter.load();
			const uint32 mgr = sCursorMgr.load();
			if (ValidPtr(widget, 0x2C0) && ValidPtr(mgr, 0x30) && frame - sCursorFrame.load() <= 3)
			{
				std::lock_guard lock(sFocusMutex);
				if (frame - sFocusReq.frame <= 2)
				{
					const uint32 entry = FindCursorEntry(mgr, sFocusReq.part, sFocusReq.area);
					const sint32 index = entry && entry != current ? ListIndexOf(widget, entry) : -1;
					if (index >= 0)
					{
						memory_writeU32(widget + 0x29C, (uint32)index);
						sListFrame = frame;
					}
				}
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_CursorTick(PPCInterpreter_t* hCPU)
		{
			const uint32 mgr = hCPU->gpr[3];
			const uint32 frame = Input::GetDebug().frameCounter.load();
			sCursorMgr = mgr;
			sCursorFrame = frame;
			if (ValidPtr(mgr, 0x30))
			{
				// Our own request was consumed by the update that followed the last hook, so a taken slot means that the
				// shown screen asked for an entry itself.
				const uint32 pending = memory_readU32(mgr + 0x28);
				if (pending != 0)
					sCursorOwnedFrame = frame;
				const bool owned = frame - sCursorOwnedFrame.load() <= 8;
				std::lock_guard lock(sFocusMutex);
				if (frame - sFocusReq.frame <= 2)
				{
					const uint32 entry = FindCursorEntry(mgr, sFocusReq.part, sFocusReq.area);
					if (!entry)
						sFocusReq.result = 2;
					else if (entry == memory_readU32(mgr + 0x24) && (pending == 0 || pending == entry))
						sFocusReq.result = 3;
					else if (owned)
						sFocusReq.result = frame - sListFrame.load() <= 4 ? 1 : 4; // its list widget has it / d-pad
					else
					{
						memory_writeU32(mgr + 0x28, entry);
						sCursorRequested = entry;
						sFocusReq.result = 1;
					}
				}
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// ---- temperature unit (U-King v1.5.0) ----
		// The game shows degrees Fahrenheit when its region language is USen (1; the ids go JPja, USen, USes, USfr, EUde,
		// EUen = 5, ...). Two places ask for the region language because of that (see the patch): the conversion of the
		// temperature for the map, and the code that sets a layout's "RegionLanguageType" animation to the region
		// language. Ten layouts have such an animation; for two of them frame 1 is the Fahrenheit variant: the
		// temperature of the map (PaAppMapTemp2_00: unit symbol) and the temperature gauge of the HUD (PaTempMeter_00).
		// Their part panes are named "Pa_Temp*" (layout + 0x0C = part pane). For a forced unit these two get USen, or
		// EUen in place of USen (same arrangement, Celsius); every other layout keeps the real region language (clock
		// format, text arrangement).
		void HLE_RegionLanguage(PPCInterpreter_t* hCPU)
		{
			uint32 language = hCPU->gpr[3];
			const uint32 layout = hCPU->gpr[4];
			bool temperature = layout == 0;
			if (layout && ValidPtr(layout, 0x10))
			{
				const uint32 part = memory_readU32(layout + 0x0C);
				temperature = PaneOk(part) && ReadName(part).rfind("Pa_Temp", 0) == 0;
			}
			if (temperature)
			{
				const int unit = GetSettings().temperatureUnit;
				if (unit == 2)
					language = 1;
				else if (unit == 1 && language == 1)
					language = 5;
			}
			hCPU->gpr[3] = language;
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// root pane -> is its screen currently shown on the TV (rebuilt every computation)
		std::unordered_map<uint32, bool> sRootShown;
		std::unordered_map<uint32, std::string> sRootScreen;

		bool ScreenShownObj(const std::string& name, uint32 obj)
		{
			if (!ValidPtr(obj, 0x200))
				return false;
			// GamePad-only screens are never displayed by the PC Edition
			if (name.find("DRC") != std::string::npos || name == "GamePadBG_00" || name == "SeekPadMenuBG_00")
				return false;
			// a closed screen's memory may already belong to another object
			if (auto sg = sScreenSig.find(obj); sg != sScreenSig.end() && memory_readU32(obj + 0x0C) != sg->second)
				return false;
			const bool shown = (memory_readU32(obj + 0x94) & 0x200) != 0;
			if (auto sp = sSuppressed.find(obj); sp != sSuppressed.end())
			{
				if (!shown)
					sSuppressed.erase(sp);
				return false;
			}
			return shown;
		}

		// The state of a screen (+0x94): 0x100 opening (its "in" animation plays), 0x200 open, 0x300 closing, 0 closed.
		// ScreenShownObj is "open or closing" (a menu is usable); the game already draws the screen while it opens, which
		// is what matters for everything the overlay draws on top of it (HUD prompt keys, fades).
		bool ScreenDrawnObj(const std::string& name, uint32 obj)
		{
			if (!ValidPtr(obj, 0x200))
				return false;
			if (auto sg = sScreenSig.find(obj); sg != sScreenSig.end() && memory_readU32(obj + 0x0C) != sg->second)
				return false;
			return (memory_readU32(obj + 0x94) & 0x300) != 0;
		}

		bool ScreenVisible(const std::string& name)
		{
			auto it = sScreens.find(name);
			if (it == sScreens.end() || !ValidPtr(it->second, 0x200))
				return false;
			return (memory_readU32(it->second + 0x94) & 0x200) != 0;
		}

		void RebuildRoots()
		{
			sRootShown.clear();
			sRootScreen.clear();
			bool mapOnTv = false;
			if (auto it = sScreens.find("AppHome_00"); it != sScreens.end())
				mapOnTv = ScreenShownObj(it->first, it->second);
			for (auto& [name, obj] : sScreens)
			{
				if (!ValidPtr(obj, 0x40))
					continue;
				const uint32 lyt = memory_readU32(obj + 0x18);
				if (!ValidPtr(lyt, 0x10))
					continue;
				const uint32 root = memory_readU32(lyt + 0x0C);
				bool shown = ScreenShownObj(name, obj);
				if (name == "AppMap_00")
					shown = mapOnTv; // otherwise it is the GamePad map
				sRootShown[root] = shown;
				sRootScreen[root] = name;
			}
		}

		// visible = this pane and all ancestors visible with non-zero alpha, and its screen shown
		bool Visible(uint32 pane, std::string* screenOut = nullptr)
		{
			uint32 last = pane;
			for (int depth = 0; depth < 32 && pane; depth++)
			{
				if (!PaneOk(pane))
					return false;
				const uint8 flags = memory_readU8(pane + kFlag);
				const uint8 alpha = memory_readU8(pane + kFlag + 1);
				if (!(flags & 1) || alpha == 0)
					return false;
				last = pane;
				pane = memory_readU32(pane + kParent);
			}
			auto it = sRootShown.find(last);
			if (it != sRootShown.end())
			{
				if (screenOut)
					*screenOut = sRootScreen[last];
				return it->second;
			}
			return false;
		}

		// screen rectangle (normalized) from accumulated translate/scale (menus do not rotate)
		bool PaneRectImpl(uint32 pane, Rect& r)
		{
			uint32 chain[32];
			int n = 0;
			for (uint32 p = pane; p && n < 32; p = memory_readU32(p + kParent))
			{
				if (!PaneOk(p))
					return false;
				chain[n++] = p;
			}
			float px = 0, py = 0, sx = 1, sy = 1;
			for (int i = n - 1; i >= 0; i--)
			{
				const uint32 p = chain[i];
				px += sx * RF(p + kTranslate);
				py += sy * RF(p + kTranslate + 4);
				sx *= RF(p + kScale);
				sy *= RF(p + kScale + 4);
			}
			const float w = RF(pane + kSize) * fabsf(sx);
			const float h = RF(pane + kSize + 4) * fabsf(sy);
			const uint8 bp = memory_readU8(pane + kFlag + 3);
			const int hpos = bp % 3, vpos = bp / 3; // 0 center, 1 left/top, 2 right/bottom
			float x0 = hpos == 0 ? px - w * 0.5f : (hpos == 1 ? px : px - w);
			float y1 = vpos == 0 ? py + h * 0.5f : (vpos == 1 ? py : py + h); // y up
			r.x0 = 0.5f + x0 / kLayoutW;
			r.x1 = 0.5f + (x0 + w) / kLayoutW;
			r.y0 = 0.5f - y1 / kLayoutH;
			r.y1 = 0.5f - (y1 - h) / kLayoutH;
			return w > 0 && h > 0;
		}

		uint32 FindAncestorPart(uint32 pane)
		{
			for (uint32 p = memory_readU32(pane + kParent); p && PaneOk(p); p = memory_readU32(p + kParent))
			{
				std::string n = ReadName(p);
				if (n.rfind("Pa_", 0) == 0)
					return p;
			}
			return 0;
		}

		// sibling text pane with the given name under the same parent
		uint32 FindSibling(uint32 pane, const std::string& name)
		{
			const uint32 parent = memory_readU32(pane + kParent);
			if (!PaneOk(parent))
				return 0;
			const uint32 head = parent + kChildHead;
			uint32 node = memory_readU32(head);
			for (int i = 0; i < 256 && node && node != head; i++)
			{
				if (!PaneOk(node))
					return 0;
				if (ReadName(node) == name)
					return node;
				node = memory_readU32(node + kNext);
			}
			return 0;
		}

		// ---------------------------------------------------------------- keyboard & mouse prompts
		// text boxes whose glyphs were re-targeted: the original button codes (index in the text buffer, U+E040..) and what
		// the text box was. The game frees text boxes (layouts are unloaded) and their memory is taken by other objects:
		// writing into that corrupted the game (reported crashes: heap pointers 0x44xxxxxx turned into 0x64xxxxxx by the
		// redraw flag). So a text box is only written while it is still the same one: name, buffer, and in the buffer the
		// original code or the glyph we put there.
		struct PromptBox
		{
			std::string name;
			uint32 buf = 0;
			std::vector<std::pair<uint32, uint16>> codes;
			std::vector<uint16> written; // the glyph we last wrote at each code (0: none yet)
		};
		std::unordered_map<uint32, PromptBox> sPromptGlyphs;

		bool StillPromptBox(uint32 pane, const PromptBox& box)
		{
			if (!PaneOk(pane) || memory_readU32(pane + kTextBuf) != box.buf || ReadName(pane) != box.name)
				return false;
			const uint32 len = memory_readU16(pane + kTextLen);
			for (size_t i = 0; i < box.codes.size(); i++)
			{
				const uint32 idx = box.codes[i].first;
				if (idx >= len || !ValidPtr(box.buf + idx * 2, 2))
					return false;
				const uint16 cur = memory_readU16(box.buf + idx * 2);
				if (cur != box.codes[i].second && cur != box.written[i])
					return false;
			}
			return true;
		}

		bool IsMenuScreen(const std::string& s)
		{
			return s.rfind("Pause", 0) == 0 || s.rfind("App", 0) == 0 || s.rfind("SystemWindow", 0) == 0 ||
				   s.rfind("Title", 0) == 0 || s.rfind("GameOver", 0) == 0 || s.rfind("Shop", 0) == 0 ||
				   s.rfind("Option", 0) == 0 || s.rfind("End_", 0) == 0;
		}

		std::string ScreenOf(uint32 pane)
		{
			uint32 last = pane;
			for (int depth = 0; depth < 40 && pane; depth++)
			{
				if (!PaneOk(pane))
					break;
				last = pane;
				pane = memory_readU32(pane + kParent);
			}
			auto it = sRootScreen.find(last);
			if (it == sRootScreen.end())
			{
				RebuildRoots();
				it = sRootScreen.find(last);
			}
			return it != sRootScreen.end() ? it->second : std::string();
		}

		bool HasAncestor(uint32 pane, std::initializer_list<const char*> prefixes)
		{
			for (int depth = 0; depth < 40 && PaneOk(pane); depth++)
			{
				const std::string n = ReadName(pane);
				for (const char* p : prefixes)
					if (n.rfind(p, 0) == 0)
						return true;
				pane = memory_readU32(pane + kParent);
			}
			return false;
		}

		bool sHeldItems = false; // State::HeldItems of the last state
		bool sActionOffered = false; // the last state had an action on the interact button (State::actionA)
		bool sThrowShown = false; // the last state had "Throw" (a bomb or an object in the hands)

		// which PC glyph a prompt should show: menus are driven by the pointer, gameplay prompts follow the
		// context controls of PCInput (scope, runes, bombs, climbing, swimming, cutscenes, magnesis)
		uint16 PromptGlyph(uint32 pane, uint16 code, std::string screen, const std::string& label, const Settings& s)
		{
			if (screen.rfind("PauseMenu", 0) == 0 && screen != "PauseMenuInfo_00")
			{
				// inventory: the controls help lists gameplay controls; the category hint is the mouse wheel
				if (HasAncestor(pane, {"N_PauseTips", "Pa_AllTips"}))
					return Prompts::DefaultGlyphFor(s, code);
				if (HasAncestor(pane, {"Pa_Rstick", "N_RGuide"}))
					return Prompts::kGlyphWheelUpDown;
			}
			using namespace Prompts;
			const uint16 lmb = KeyGlyph(Key::MouseLeft), rmb = KeyGlyph(Key::MouseRight);
			auto act = [&](pce::Action a) { return ActionGlyph(s, a); };
			auto has = [&](const char* w) { return label.find(w) != std::string::npos; };
			if (IsMenuScreen(screen))
			{
				const bool pause = screen.rfind("Pause", 0) == 0;
				uint16 g = 0;
				switch (code)
				{
				case 0xE040: g = lmb; break;                                   // select = click
				case 0xE041: g = KeyGlyph(Key::Escape); break;                 // back = Esc
				case 0xE043: g = act(pce::Action::Crouch); break;                   // Y (sort ...) = crouch key in menus
				case 0xE044: g = pause ? act(s.inventoryMoveKeys ? pce::Action::MenuPrevPage : pce::Action::MoveLeft) : 0; break; // inventory pages: A / D
				case 0xE045: g = pause ? act(s.inventoryMoveKeys ? pce::Action::MenuNextPage : pce::Action::MoveRight) : 0; break;
				case 0xE05F: g = (screen.rfind("AppHome", 0) == 0 || screen.rfind("AppMap", 0) == 0) ? kGlyphWheelUpDown : 0; break;
				default: break;
				}
				return g ? g : DefaultGlyphFor(s, code);
			}
			uint16 g = 0;
			if (sHeldItems && label == "Drop")
				g = lmb;
			else if (sHeldItems && label == "Put away")
				g = rmb;
			else if (has("Place pin") || has("Throw"))
				g = lmb;
			else if (has("Quit") || has("Cancel") || (has("Detonate") && !s.bombKeepsBow))
				g = rmb;
			else if (has("Let go") && sActionOffered)
				g = 0; // climbing with a fruit in reach: the interact key takes it, letting go is on its own button
			else if (has("Let go") || has("Skip"))
				g = act(pce::Action::Interact);
			else if (has("Dash"))
				g = act(pce::Action::Sprint);
			else if ((has("Push") || has("Pull") || has("Far") || has("Near")) && !sThrowShown)
				g = kGlyphWheelUpDown; // (not with something to throw: a text of another language may stand for several)
			return g ? g : DefaultGlyphFor(s, code);
		}

		// false: the text box is not the one we know any more (forget it)
		bool ApplyPrompts(uint32 pane, bool markDirty, const Settings& s)
		{
			auto it = sPromptGlyphs.find(pane);
			if (it == sPromptGlyphs.end())
				return true;
			PromptBox& box = it->second;
			if (!StillPromptBox(pane, box))
				return false;
			const uint32 buf = box.buf;
			auto ti = sTexts.find(pane);
			if (ti == sTexts.end())
				return true;
			// label: the guide text next to an icon, else the text box itself
			std::string label = ti->second.text;
			const std::string& name = ti->second.name;
			if (name.rfind("T_Icon_", 0) == 0)
			{
				if (uint32 g = FindSibling(pane, "T_Guide_" + name.substr(7)))
				{
					auto gt = sTexts.find(g);
					if (gt != sTexts.end())
						label = gt->second.text;
				}
			}
			const std::string screen = ScreenOf(pane);
			bool changed = false;
			for (size_t i = 0; i < box.codes.size(); i++)
			{
				const auto [idx, code] = box.codes[i];
				const uint16 g = PromptGlyph(pane, code, screen, label, s);
				if (memory_readU16(buf + idx * 2) != g)
				{
					memory_writeU16(buf + idx * 2, g);
					box.written[i] = g;
					changed = true;
				}
			}
			if (changed && markDirty)
				memory_writeU8(pane + 0xD0, memory_readU8(pane + 0xD0) | 0x20); // text box: rebuild glyphs
			return true;
		}

		void OnTextSetPrompts(uint32 pane, const std::string& name, uint32 buf, uint32 len)
		{
			std::vector<std::pair<uint32, uint16>> codes;
			for (uint32 i = 0; i < len; i++)
			{
				const uint16 c = memory_readU16(buf + i * 2);
				if (c == 0)
					break;
				if (c == 0x0E)
				{
					if (i + 3 < len)
						i += 3 + (memory_readU16(buf + (i + 3) * 2) + 1) / 2;
					continue;
				}
				if (c >= 0xE040 && c <= 0xE08F)
					codes.emplace_back(i, c);
			}
			if (codes.empty())
				sPromptGlyphs.erase(pane);
			else
			{
				if (sPromptGlyphs.size() > 4000)
					sPromptGlyphs.clear();
				PromptBox& box = sPromptGlyphs[pane];
				box.name = name;
				box.buf = buf;
				box.written.assign(codes.size(), 0);
				box.codes = std::move(codes);
				// first guess (the text box may not be attached to its screen yet; RefreshPrompts corrects it)
				if (!ApplyPrompts(pane, false, GetSettings()))
					sPromptGlyphs.erase(pane);
			}
		}

		// every frame: re-target the prompts that are shown (their screen and label are known by now; bindings
		// may have changed)
		void RefreshPrompts()
		{
			if (sPromptGlyphs.empty())
				return;
			const Settings s = GetSettings();
			for (auto it = sPromptGlyphs.begin(); it != sPromptGlyphs.end();)
			{
				if (!PaneOk(it->first) || (Visible(it->first) && !ApplyPrompts(it->first, true, s)))
				{
					it = sPromptGlyphs.erase(it);
					continue;
				}
				++it;
			}
		}

		Btn GlyphToBtn(uint16 g)
		{
			switch (g)
			{
			case 0xE040: return Btn::A;
			case 0xE041: return Btn::B;
			case 0xE042: return Btn::X;
			case 0xE043: return Btn::Y;
			case 0xE044: return Btn::L;
			case 0xE045: return Btn::R;
			case 0xE046: return Btn::ZL;
			case 0xE047: return Btn::ZR;
			default: return Btn::None;
			}
		}



		// HUD action diamond (DoCommand_00): the button circles are textures, so the controller artwork is hidden and
		// the overlay draws the PC key at the same place (high resolution, follows bindings and context live).
		// The keys must appear in the very frame the game draws their label in: the game hands a finished frame to the GPU
		// thread, which shows it later, when the UI state in memory has moved on. So a snapshot is taken per game frame
		// (OnGameFrame, CPU thread) and picked up when the GPU thread reaches the frame it belongs to (OnGpuFrame).
		std::mutex sHudMutex;
		std::deque<std::pair<uint64, std::vector<HudPrompt>>> sHudQueue; // frame sequence number, its prompts
		std::vector<HudPrompt> sHudPrompts;                              // prompts of the frame the GPU thread is showing
		std::vector<HudPrompt> sHudLast;                                 // snapshot of the previous game frame
		uint64 sHudSeqCpu = 0, sHudSeqGpu = 0;
		// the minimap's rotation, place and size and Link's position, the same way: the icons drawn on the minimap belong to
		// the picture of it they are drawn over (read when the overlay draws, they were a frame or two off while turning)
		std::deque<std::pair<uint64, MinimapFrame>> sMiniQueue;
		MinimapFrame sMiniShown, sMiniLast;
		bool sMiniShownValid = false;
		std::atomic<float> sFadeNow{0.0f};
		// developer: per frame record of the prompt panes
		struct HudTraceEntry
		{
			uint64 seq;
			std::string name, label;
			float chain, global;
			bool visible;
			Rect rect;
		};
		std::vector<HudTraceEntry> sHudTrace;
		std::atomic<bool> sHudTraceOn{false};

		float AlphaChain(uint32 pane, bool skipOwnVisibility)
		{
			float a = 1.0f;
			for (int depth = 0; depth < 40 && PaneOk(pane); depth++)
			{
				const uint8 flags = memory_readU8(pane + kFlag);
				if (!(flags & 1) && !(skipOwnVisibility && depth == 0))
					return 0.0f;
				a *= memory_readU8(pane + kFlag + 1) / 255.0f;
				pane = memory_readU32(pane + kParent);
			}
			return a;
		}

		uint32 FindChildByPrefix(uint32 parent, const std::string& prefix)
		{
			if (!PaneOk(parent))
				return 0;
			const uint32 head = parent + kChildHead;
			uint32 node = memory_readU32(head);
			for (int i = 0; i < 64 && node && node != head; i++)
			{
				if (!PaneOk(node))
					break;
				if (ReadName(node).rfind(prefix, 0) == 0)
					return node;
				node = memory_readU32(node + kNext);
			}
			return 0;
		}

		// how far the screen is faded to black (0..1): the fade screens (loading, warps, events) are drawn over the HUD,
		// the overlay's keys have to fade with it
		float FadeAmount()
		{
			float fade = 0.0f;
			for (auto& [name, obj] : sScreens)
			{
				if (name.rfind("Fade", 0) != 0 || !ScreenDrawnObj(name, obj))
					continue;
				const uint32 lyt = memory_readU32(obj + 0x18);
				if (!ValidPtr(lyt, 0x10))
					continue;
				std::vector<uint32> stack{memory_readU32(lyt + 0x0C)};
				for (int guard = 0; !stack.empty() && guard < 400; guard++)
				{
					const uint32 pane = stack.back();
					stack.pop_back();
					if (!PaneOk(pane))
						continue;
					if (ReadName(pane).rfind("P_Black", 0) == 0)
					{
						fade = std::max(fade, AlphaChain(pane, false));
						continue;
					}
					const uint32 head = pane + kChildHead;
					uint32 node = memory_readU32(head);
					for (int i = 0; i < 64 && node && node != head; i++)
					{
						if (!PaneOk(node))
							break;
						stack.push_back(node);
						node = memory_readU32(node + kNext);
					}
				}
			}
			return std::clamp(fade, 0.0f, 1.0f);
		}

		void RefreshDoCommand(const State& st, std::vector<HudPrompt>& out)
		{
			const float fade = FadeAmount();
			sFadeNow = fade;
			// (drawn, not "shown": the label is on screen from the first frame of the opening animation, 0.45 s before
			// the screen counts as open)
			auto it = sScreens.find("DoCommand_00");
			if (it == sScreens.end() || !ScreenDrawnObj(it->first, it->second))
				return;
			const uint32 lyt = memory_readU32(it->second + 0x18);
			if (!ValidPtr(lyt, 0x10))
				return;
			const uint32 root = memory_readU32(lyt + 0x0C);
			if (!PaneOk(root))
				return;
			const Settings s = GetSettings();
			const bool runeMode = st.Has("Cancel") && !st.anyMenu;
			std::vector<uint32> stack{root};
			for (int guard = 0; !stack.empty() && guard < 4000; guard++)
			{
				const uint32 pane = stack.back();
				stack.pop_back();
				if (!PaneOk(pane))
					continue;
				const std::string name = ReadName(pane);
				const bool letter = name.size() == 6 && name[0] == 'T' && name[1] == '_' && name[3] == '_' &&
									std::string("ABXY").find(name[2]) != std::string::npos;
				const bool keyTex = name.rfind("Nt_KeyTex", 0) == 0 && name.size() >= 10;
				if (keyTex || letter)
				{
					const uint8 f = memory_readU8(pane + kFlag);
					if (f & 1)
						memory_writeU8(pane + kFlag, f & ~1); // controller artwork off
				}
				if (keyTex)
				{
					// the action label next to it ("T_GuideA_00" beside "Nt_KeyTexA_01"); the key appears with the label
					// (the GamePad circle pops in a little later in the game's animation)
					const uint32 lp = FindChildByPrefix(memory_readU32(pane + kParent), std::string("T_Guide") + name[9]);
					const float alpha = (lp ? AlphaChain(lp, false) : AlphaChain(pane, true)) * (1.0f - fade);
					if (sHudTraceOn.load() && sHudTrace.size() < 20000 && lp)
					{
						HudTraceEntry e{sHudSeqCpu + 1, name, {}, AlphaChain(lp, false), memory_readU8(lp + kFlag + 2) / 255.0f, true, {}};
						for (uint32 q = lp; PaneOk(q); q = memory_readU32(q + kParent))
							e.visible &= (memory_readU8(q + kFlag) & 1) != 0;
						if (auto ti = sTexts.find(lp); ti != sTexts.end())
							e.label = ti->second.text;
						PaneRectImpl(pane, e.rect);
						sHudTrace.push_back(e);
					}
					static const std::string kButtons = "ABXYL";
					const size_t bi = kButtons.find(name[9]);
					Rect r{};
					if (alpha > 0.01f && bi != std::string::npos && PaneRectImpl(pane, r))
					{
						std::string label;
						if (lp)
						{
							auto ti = sTexts.find(lp);
							if (ti != sTexts.end())
								label = ti->second.text;
						}
						uint16 glyph = PromptGlyph(pane, (uint16)(0xE040 + bi), "DoCommand_00", label, s);
						if (name[9] == 'A' && runeMode)
							glyph = Prompts::KeyGlyph(Key::MouseLeft); // rune aiming: left click activates
						if (glyph)
							out.push_back({r, glyph, alpha});
					}
				}
				const uint32 head = pane + kChildHead;
				uint32 node = memory_readU32(head);
				for (int i = 0; i < 512 && node && node != head; i++)
				{
					if (!PaneOk(node))
						break;
					stack.push_back(node);
					node = memory_readU32(node + kNext);
				}
			}
		}

		// The System page of the pause menu shows a picture of the GamePad with the controls of the moment (part pane
		// Pa_AllTips_00, titled "Basic Controls", "Riding Controls", ...). It is hidden: the PC settings are drawn in its
		// place (PCOverlay).
		uint32 sAllTips = 0;
		void SystemPanel(State& st)
		{
			if (!PaneOk(sAllTips) || ReadName(sAllTips) != "Pa_AllTips_00")
			{
				sAllTips = 0;
				for (auto& [pane, ti] : sTexts)
				{
					if (ti.name.rfind("T_ControlTitle", 0) != 0 || !PaneOk(pane) || ReadName(pane) != ti.name)
						continue;
					for (uint32 p = memory_readU32(pane + kParent); p && PaneOk(p) && !sAllTips; p = memory_readU32(p + kParent))
						if (ReadName(p) == "Pa_AllTips_00")
							sAllTips = p;
					if (sAllTips)
						break;
				}
				if (!sAllTips)
					return;
			}
			const uint32 parent = memory_readU32(sAllTips + kParent);
			Rect r{};
			if (!PaneOk(parent) || !Visible(parent) || !PaneRectImpl(sAllTips, r))
				return;
			const uint8 f = memory_readU8(sAllTips + kFlag);
			if (f & 1)
				memory_writeU8(sAllTips + kFlag, f & ~1);
			if (r.x0 < -0.05f || r.x1 > 1.05f)
				return; // another page is shown (the System page has slid off the screen)
			st.systemPanel = true;
			st.systemRect = r;
		}

		// "Ability Controls": the GamePad pictures of its pages (layout PaMessageTipsDrcImage_00: P_DrcImage_00, one node
		// per button lit when the page uses it). The pictures are hidden every frame (the nodes are left as they are: the
		// game decides which one is on), the buttons are kept for PCOverlay.
		std::mutex sTipMutex;
		std::vector<TipKeys> sTipKeys;
		bool sAmiiboFrameShown = false;
		Rect sAmiiboFrame{};

		void HidePane(uint32 p)
		{
			if (!PaneOk(p))
				return;
			const uint8 f = memory_readU8(p + kFlag);
			if (f & 1)
				memory_writeU8(p + kFlag, f & ~1);
		}

		uint32 FindChildNamed(uint32 parent, const std::string& name)
		{
			if (!PaneOk(parent))
				return 0;
			const uint32 head = parent + kChildHead;
			for (uint32 node = memory_readU32(head), i = 0; node && node != head && i < 256; node = memory_readU32(node + kNext), i++)
			{
				if (!PaneOk(node))
					break;
				if (ReadName(node) == name)
					return node;
			}
			return 0;
		}
		void TipPictures()
		{
			static const std::pair<const char*, uint16> kButtons[] = {
				{"N_A_00", 0xE040}, {"N_B_00", 0xE041}, {"N_X_00", 0xE042}, {"N_Y_00", 0xE043}, {"N_L_00", 0xE044}, {"N_R_00", 0xE045},
				{"N_ZL_00", 0xE046}, {"N_ZR_00", 0xE047}, {"N_Plus_00", 0xE04B}, {"N_Minus_00", 0xE04C},
				{"N_Ue_00", 0xE060}, {"N_Sita_00", 0xE061}, {"N_Left_00", 0xE062}, {"N_Right_00", 0xE063}, {"N_Joge_00", 0xE064},
				{"N_Juji_00", 0xE065}, {"N_Sayu_00", 0xE065},
				{"N_Lstick_Up_00", 0xE052}, {"N_Lstick_Down_00", 0xE050}, {"N_Lstick_Left_00", 0xE05A}, {"N_Lstick_Right_00", 0xE058},
				{"N_Lstick_UD_00", Prompts::kGlyphWS}, {"N_Lstick_LR_00", 0xE054}, {"N_Lstick_Push_00", 0xE056}, {"N_Lstick_00", 0xE05E},
				{"N_Rstick_Up_00", 0xE053}, {"N_Rstick_Down_00", 0xE051}, {"N_Rstick_Left_00", 0xE05B}, {"N_Rstick_Right_00", 0xE059},
				{"N_Rstick_UD_00", 0xE05D}, {"N_Rstick_LR_00", 0xE055}, {"N_Rstick_Push_00", 0xE057}, {"N_Rstick_00", 0xE05F},
			};
			std::vector<TipKeys> found;
			bool amiiboShown = false;
			Rect amiiboFrame{};
			// Ability Controls (pause menu) and the tips of the game (tutorials during play, "Use amiibo")
			for (const char* screen : {"ControllerWindow_00", "MessageTips_00"})
			{
			auto it = sScreens.find(screen);
			if (it != sScreens.end() && ScreenShownObj(it->first, it->second) && ValidPtr(it->second, 0x40))
			{
				const uint32 lyt = memory_readU32(it->second + 0x18);
				const uint32 root = ValidPtr(lyt, 0x10) ? memory_readU32(lyt + 0x0C) : 0;
				std::vector<uint32> stack{root};
				for (int guard = 0; !stack.empty() && guard < 20000; guard++)
				{
					const uint32 pane = stack.back();
					stack.pop_back();
					if (!PaneOk(pane))
						continue;
					if (ReadName(pane) == "P_amiiboDRC_00")
					{
						// "Use amiibo": the GamePad, the arrow to its NFC touchpoint and the text asking to touch an amiibo
						// to it go; the frame stays (the choice of amiibo files is drawn in it)
						const uint32 part = memory_readU32(pane + kParent);
						HidePane(pane);
						if (PaneOk(part))
						{
							HidePane(FindChildNamed(part, "N_ArrowPos_00"));
							HidePane(FindChildNamed(memory_readU32(part + kParent), "T_Msg_00"));
							Rect fr{};
							const uint32 frame = FindChildNamed(part, "W_Base_02");
							if (Visible(part) && ((frame && PaneRectImpl(frame, fr)) || PaneRectImpl(pane, fr)))
							{
								amiiboShown = true;
								amiiboFrame = fr;
							}
						}
						continue;
					}
					if (ReadName(pane) == "P_DrcImage_00")
					{
						const uint32 part = memory_readU32(pane + kParent);
						Rect r{};
						if (!PaneOk(part) || !PaneRectImpl(pane, r))
							continue;
						TipKeys tk{r, {}};
						const uint32 head = part + kChildHead;
						for (uint32 node = memory_readU32(head), i = 0; node && node != head && i < 128; node = memory_readU32(node + kNext), i++)
						{
							if (!PaneOk(node))
								break;
							const std::string n = ReadName(node);
							for (const auto& [bn, code] : kButtons)
							{
								if (n != bn)
									continue;
								if ((memory_readU8(node + kFlag) & 1) && memory_readU8(node + kFlag + 1) > 0)
									tk.codes.push_back(code);
								// its pictures off (the lit button, arrows); the node stays the game's
								std::vector<uint32> sub{node};
								for (int g = 0; !sub.empty() && g < 64; g++)
								{
									const uint32 q = sub.back();
									sub.pop_back();
									const uint32 h = q + kChildHead;
									for (uint32 c = memory_readU32(h), k = 0; c && c != h && k < 32; c = memory_readU32(c + kNext), k++)
									{
										if (!PaneOk(c))
											break;
										const std::string cn = ReadName(c);
										if (cn.rfind("P_", 0) == 0 || cn.rfind("W_", 0) == 0)
										{
											const uint8 f = memory_readU8(c + kFlag);
											if (f & 1)
												memory_writeU8(c + kFlag, f & ~1);
										}
										else
											sub.push_back(c);
									}
								}
							}
						}
						const uint8 f = memory_readU8(pane + kFlag);
						if (f & 1)
							memory_writeU8(pane + kFlag, f & ~1); // the picture of the GamePad
						// the keys go in the picture's frame (the picture itself is a cut-out of the GamePad)
						if (const uint32 frame = FindChildByPrefix(part, "W_Base_00"))
						{
							Rect fr{};
							if (PaneRectImpl(frame, fr))
								tk.rect = fr;
						}
						if (Visible(part) && r.x1 > 0.0f && r.x0 < 1.0f && r.y1 > 0.0f && r.y0 < 1.0f)
							found.push_back(std::move(tk));
						continue;
					}
					const uint32 head = pane + kChildHead;
					for (uint32 node = memory_readU32(head), i = 0; node && node != head && i < 256; node = memory_readU32(node + kNext), i++)
					{
						if (!PaneOk(node))
							break;
						stack.push_back(node);
					}
				}
			}
			}
			std::lock_guard lock(sTipMutex);
			sTipKeys = std::move(found);
			sAmiiboFrameShown = amiiboShown;
			sAmiiboFrame = amiiboFrame;
		}

		void Compute(State& st)
		{
			RebuildRoots();
			if (Prompts::Active())
				RefreshPrompts();
			st = State{};
			st.valid = true;
			st.inventory = ScreenVisible("PauseMenu_00");
			st.map = ScreenVisible("AppHome_00");
			st.title = ScreenVisible("Title_00");
			st.hud = ScreenVisible("MainScreen_00");
			st.cameraRune = ScreenVisible("AppCamera_00");
			if (auto it = sScreens.find("MainScreen_00"); it != sScreens.end() && ValidPtr(it->second, 0x200))
				st.anyMenu = (memory_readU32(it->second + 0x180) & 0x1A) != 0x1A;
			if (st.inventory)
				SystemPanel(st);
			TipPictures();
			for (auto& [name, obj] : sScreens)
			{
				if (name.rfind("SystemWindow", 0) == 0 || name.rfind("GameOver", 0) == 0 || name.rfind("ShopBtnList", 0) == 0 ||
					name == "AppSystemWindow_00" || name == "OptionWindow_00" || name == "End_00" || name == "ControllerWindow_00" ||
					name == "DLCWindow_00")
				{
					if (ScreenShownObj(name, obj))
						st.dialog = true;
				}
			}
			std::unordered_map<uint32, bool> partSeen;
			for (auto& [pane, ti] : sTexts)
			{
				if (ti.name.size() < 3 || ti.name[0] != 'T' || ti.name[1] != '_')
					continue;
				// only the text boxes the state needs (cheap name test before the memory walks)
				const std::string& nm = ti.name;
				const bool relevant = nm.rfind("T_Guide", 0) == 0 || nm.rfind("T_A_", 0) == 0 || nm.rfind("T_B_", 0) == 0 ||
									  nm.rfind("T_BX_", 0) == 0 || nm.rfind("T_Btn_", 0) == 0 || ti.text == "Place pin" || nm == "T_Name_00";
				if (!relevant)
					continue;
				if (ti.text == "Place pin" && PaneOk(pane) && ReadName(pane) == ti.name && Visible(pane))
					st.scope = true;
				if (!PaneOk(pane) || ReadName(pane) != ti.name || !Visible(pane))
					continue;
				const std::string& n = ti.name;
				if (n == "T_Name_00")
				{
					std::string scr;
					if (Visible(pane, &scr) && scr == "MainShortCut_00")
					{
						st.shortcut = true;
						st.shortcutName = ti.text;
					}
					continue;
				}
				const bool placeholder = ti.text.size() >= 5 && ti.text.compare(ti.text.size() - 5, 5, "Guide") == 0; // "YGuide"
				if (n.rfind("T_GuideA_", 0) == 0)
				{
					if (!placeholder)
					{
						st.actionA = ti.text;
						st.actions.push_back({ti.text, Btn::A});
					}
				}
				else if (n.size() == 11 && n.rfind("T_Guide", 0) == 0 && n[8] == '_' && std::string("BXYL").find(n[7]) != std::string::npos)
				{
					// HUD action diamond: actions on the other buttons (Y: Flurry Rush, Sneakstrike, ...)
					if (!placeholder)
						st.actions.push_back({ti.text, n[7] == 'B' ? Btn::B : n[7] == 'X' ? Btn::X : n[7] == 'Y' ? Btn::Y : Btn::L});
				}
				else if (n.rfind("T_Guide_", 0) == 0)
				{
					// action guide entry: the button is the glyph of the sibling icon text box with the same suffix
					Btn b = Btn::None;
					if (uint32 icon = FindSibling(pane, "T_Icon_" + n.substr(8)))
					{
						auto it = sTexts.find(icon);
						if (it != sTexts.end())
							b = GlyphToBtn(it->second.first);
					}
					st.actions.push_back({ti.text, b});
				}
				else if (n.rfind("T_A_", 0) == 0 && ti.text.size() > 1)
					st.actions.push_back({ti.text, Btn::A});
				else if (n.rfind("T_B_", 0) == 0 && ti.text.size() > 1)
					st.actions.push_back({ti.text, Btn::B});
				else if (n.rfind("T_BX_", 0) == 0 && ti.text.size() > 1)
					st.actions.push_back({ti.text, Btn::X});
				else if (n.rfind("T_Btn_", 0) == 0 && !ti.text.empty())
				{
					uint32 part = FindAncestorPart(pane);
					if (!part || partSeen[part])
						continue;
					partSeen[part] = true;
					MenuButton mb;
					mb.pane = part;
					mb.label = ti.text;
					if (PaneRectImpl(part, mb.rect))
						st.buttons.push_back(mb);
					// answers of a conversation ("MessageDialog": Pa_Btn_nn parts with a T_Btn_00 label)
					if (!st.choice && ScreenOf(pane).rfind("Message", 0) == 0)
						st.choice = true;
				}
			}
			// the camera rune's view: not while it asks about a picture ("Save in album?": Save / Delete, a menu then)
			if (st.cameraRune && !st.buttons.empty())
			{
				st.cameraRune = false;
				st.cameraDialog = true;
			}
		}
	}

	namespace
	{
		PointerSnapshot sPointer;
		uint32 sPointerFrame = 0xFFFFFFFF;

		// one depth-first walk with inherited visibility and transform
		struct PartInfo
		{
			bool hasHit = false;
			Rect hit{};
			bool hasText = false;
			Rect text{};
			std::string label;
			uint8 labelAlpha = 0;
			int order = 0;
			int level = 0; // what covers what (ModalLevel of the screen)
			uint32 group = 0;
			bool isTab = false;
			bool questTab = false;
			bool tabCurrent = false;
			int arrow = 0; // page arrow part: 1 left, 2 right
			bool arrowShown = false;
			Rect arrowRect{};
		};

		bool IsModalScreen(const std::string& name)
		{
			return name.rfind("SystemWindow", 0) == 0 || name.rfind("GameOver", 0) == 0 || name.rfind("ShopBtnList", 0) == 0 ||
				   name == "AppSystemWindow_00" || name == "OptionWindow_00" || name == "End_00";
		}

		// Only the topmost level is clickable: the answers of a conversation lie on top of everything (a Great Fairy asks
		// "Enhance it?" over her armour list, which is a shop list), then dialogs and shop lists, then the rest.
		int ModalLevel(const std::string& name)
		{
			if (name.rfind("MessageDialog", 0) == 0)
				return 2;
			return IsModalScreen(name) ? 1 : 0;
		}

		void WalkLayout(uint32 root, bool boxCursor, PointerSnapshot& ps, std::vector<std::pair<float, float>>& boxCorners,
						std::unordered_map<uint32, PartInfo>& parts, int level = 0)
		{
			struct Item
			{
				uint32 pane;
				float px, py, sx, sy;
				uint32 part;
				int depth;
				uint32 group;
			};
			std::vector<Item> stack;
			stack.push_back({root, 0, 0, 1, 1, 0, 0, 0});
			int guard = 0;
			while (!stack.empty() && guard++ < 20000)
			{
				Item it = stack.back();
				stack.pop_back();
				const uint32 pane = it.pane;
				if (!PaneOk(pane) || it.depth > 30)
					continue;
				const uint8 flags = memory_readU8(pane + kFlag);
				const uint8 alpha = memory_readU8(pane + kFlag + 1);
				if (!(flags & 1) || alpha == 0)
					continue;
				const float px = it.px + it.sx * RF(pane + kTranslate);
				const float py = it.py + it.sy * RF(pane + kTranslate + 4);
				const float sx = it.sx * RF(pane + kScale);
				const float sy = it.sy * RF(pane + kScale + 4);
				const std::string name = ReadName(pane);
				uint32 part = it.part;
				uint32 group = it.group;
				if (name.rfind("Pa_PagePorch", 0) == 0)
					group = pane;
				else if (name.rfind("Pa_", 0) == 0 && name.rfind("Pa_Rstick", 0) != 0)
				{
					part = pane;
					auto& pi = parts[pane];
					pi.group = group;
					// category tabs: inventory ("Pa_CategoryS_nn") and adventure log ("Pa_ChallengeCategory_nn": main quests,
					// shrine quests, side quests, memories; empty categories have no hit pane and the game skips them)
					pi.questTab = name.rfind("Pa_ChallengeCategory", 0) == 0;
					pi.isTab = pi.questTab || name.rfind("Pa_Category", 0) == 0;
				}
				if (name == "N_Check_00" && part)
				{
					auto pit = parts.find(part);
					if (pit != parts.end() && pit->second.isTab)
						pit->second.tabCurrent = true; // only the highlighted category shows its check frame
				}
				auto rectOf = [&](Rect& r) {
					const float w = RF(pane + kSize) * fabsf(sx), h = RF(pane + kSize + 4) * fabsf(sy);
					const uint8 bp = memory_readU8(pane + kFlag + 3);
					const int hpos = bp % 3, vpos = bp / 3;
					const float x0 = hpos == 0 ? px - w * 0.5f : (hpos == 1 ? px : px - w);
					const float y1 = vpos == 0 ? py + h * 0.5f : (vpos == 1 ? py : py + h);
					r = {0.5f + x0 / kLayoutW, 0.5f - y1 / kLayoutH, 0.5f + (x0 + w) / kLayoutW, 0.5f - (y1 - h) / kLayoutH};
					return w > 0 && h > 0;
				};
				if (name == "Pa_ArrowL_00" || name == "Pa_ArrowR_00")
				{
					// page arrow part ("<" / ">" next to the inventory grid)
					auto& pi = parts[pane];
					pi.arrow = name[8] == 'L' ? 1 : 2;
					rectOf(pi.arrowRect);
				}
				else if ((name == "N_ArrowL_00" || name == "N_ArrowR_00") && part)
				{
					// each arrow part holds both shapes; its own one is hidden / transparent while there is no page in that
					// direction (such panes are not walked)
					auto pit = parts.find(part);
					if (pit != parts.end() && pit->second.arrow == (name[7] == 'L' ? 1 : 2))
						pit->second.arrowShown = true;
				}
				if (boxCursor)
				{
					if (name == "N_LT_00" || name == "N_RT_00" || name == "N_LB_00" || name == "N_RB_00")
						boxCorners.emplace_back(0.5f + px / kLayoutW, 0.5f - py / kLayoutH);
				}
				else if (name.rfind("B_Hit", 0) == 0 || name.rfind("B_Btn", 0) == 0)
				{
					Rect r;
					auto& pi = parts[part ? part : pane];
					if (!pi.hasHit && rectOf(r))
					{
						pi.hasHit = true;
						pi.hit = r;
						pi.order = (int)parts.size();
						pi.level = level;
					}
				}
				else if (name == "N_MapT_00")
				{
					ps.hasMap = true;
					ps.mapX = 0.5f + px / kLayoutW;
					ps.mapY = 0.5f - py / kLayoutH;
					ps.mapScaleX = it.sx;
					ps.mapScaleY = it.sy;
					// map controller of the screen that owns this pane: [screen+0x478]+0x648 (U-King v1.5.0)
					const uint32 obj = sMapObj.load();
					if (obj && Input::GetDebug().frameCounter.load() - sMapObjFrame.load() <= 4 && ValidPtr(obj, 0x480) &&
						memory_readU32(obj + 0xA8) == pane)
					{
						const uint32 ctrl = memory_readU32(obj + 0x478);
						if (ValidPtr(ctrl + 0x648, 0x40))
							ps.mapCtrl = ctrl + 0x648;
					}
				}
				else if (name.rfind("T_", 0) == 0 && part && alpha >= 16)
				{
					auto ti = sTexts.find(pane);
					Rect r;
					if (ti != sTexts.end() && !ti->second.text.empty() && rectOf(r))
					{
						auto& pi = parts[part];
						if (!pi.hasText)
							pi.text = r;
						else
							pi.text = {std::min(pi.text.x0, r.x0), std::min(pi.text.y0, r.y0), std::max(pi.text.x1, r.x1), std::max(pi.text.y1, r.y1)};
						pi.hasText = true;
						if (name.rfind("T_Btn", 0) == 0 && alpha >= pi.labelAlpha)
						{
							pi.label = ti->second.text;
							pi.labelAlpha = alpha;
						}
					}
				}
				const uint32 head = pane + kChildHead;
				uint32 node = memory_readU32(head);
				for (int i = 0; i < 512 && node && node != head; i++)
				{
					if (!PaneOk(node))
						break;
					stack.push_back({node, px, py, sx, sy, part, it.depth + 1, group});
					node = memory_readU32(node + kNext);
				}
			}
		}

		// How much wider than 16:9 the picture is (1 for 16:9, 1.33 for 21:9, 2 for 32:9). With the ultrawide HUD fix of
		// the graphics pack the menus keep their shape, so their entries are that much narrower as fractions of the picture.
		float PictureWiden()
		{
			int x, y, w, h, ww, wh;
			if (!Graphics::GetPictureArea(x, y, w, h, ww, wh))
				return 1.0f;
			return std::max(1.0f, ((float)w / (float)h) / (16.0f / 9.0f));
		}

		void ComputePointer(PointerSnapshot& ps)
		{
			RebuildRoots();
			ps = PointerSnapshot{};
			ps.valid = true;
			std::unordered_map<uint32, PartInfo> parts;
			for (auto& [root, shown] : sRootShown)
			{
				if (!shown)
					continue;
				const std::string& scr = sRootScreen[root];
				if (scr == "BoxCursorTV")
				{
					std::vector<std::pair<float, float>> corners;
					PointerSnapshot dummy;
					WalkLayout(root, true, dummy, corners, parts);
					if (corners.size() >= 2)
					{
						float x = 0, y = 0;
						for (auto& c : corners)
						{
							x += c.first;
							y += c.second;
						}
						ps.hasBoxCursor = true;
						ps.boxX = x / corners.size();
						ps.boxY = y / corners.size();
					}
					continue;
				}
				if (scr.rfind("MainScreen", 0) == 0 || scr == "DoCommand_00" || scr == "MainShortCut_00" || scr == "PauseMenuBG_00")
					continue; // HUD layers: no pointer targets
				std::vector<std::pair<float, float>> none;
				WalkLayout(root, false, ps, none, parts, ModalLevel(scr));
			}
			auto onScreen = [](const Rect& r) { return r.cx() > 0.0f && r.cx() < 1.0f && r.cy() > 0.0f && r.cy() < 1.0f; };
			const float widen = PictureWiden();
			std::vector<std::pair<int, Target>> ordered;
			int topLevel = 0;
			for (auto& [part, pi] : parts)
			{
				if (pi.hasHit)
					topLevel = std::max(topLevel, pi.level);
			}
			for (auto& [part, pi] : parts)
				if (pi.arrow && pi.arrowShown && topLevel == 0 && onScreen(pi.arrowRect))
				{
					ps.pageArrow[pi.arrow - 1] = true;
					ps.pageArrowRect[pi.arrow - 1] = pi.arrowRect;
				}
			for (auto& [part, pi] : parts)
			{
				if (!pi.hasHit || pi.level < topLevel)
					continue; // something lies on top: the screens behind it are not clickable
				Target t;
				t.part = part;
				t.hit = pi.hit;
				t.label = pi.label;
				t.textAlpha = pi.labelAlpha;
				t.group = pi.group;
				t.tab = pi.isTab ? 0 : -1;
				const float hw = pi.hit.x1 - pi.hit.x0, hh = pi.hit.y1 - pi.hit.y0;
				if (onScreen(pi.hit) && hw * widen > 0.02f && hh > 0.02f)
					t.rect = pi.hit;
				else if (pi.hasText && onScreen(pi.text))
				{
					// bounding pane is a tiny/off-screen anchor (title menu): click on the button's text instead
					const float padY = (pi.text.y1 - pi.text.y0) * 0.2f;
					t.rect = {pi.text.x0 - 0.01f, pi.text.y0 - padY, pi.text.x1 + 0.01f, pi.text.y1 + padY};
				}
				else
					continue;
				ordered.emplace_back(pi.order, t);
			}
			std::sort(ordered.begin(), ordered.end(), [](auto& a, auto& b) { return a.first < b.first; });
			for (auto& [o, t] : ordered)
				ps.targets.push_back(t);
			// category tabs: index them left to right
			std::vector<int> tabs;
			for (int i = 0; i < (int)ps.targets.size(); i++)
				if (ps.targets[i].tab >= 0)
					tabs.push_back(i);
			std::sort(tabs.begin(), tabs.end(), [&](int a, int b) { return ps.targets[a].rect.cx() < ps.targets[b].rect.cx(); });
			ps.tabCount = (int)tabs.size();
			for (int k = 0; k < (int)tabs.size(); k++)
			{
				ps.targets[tabs[k]].tab = k;
				const PartInfo& pi = parts[ps.targets[tabs[k]].part];
				if (pi.tabCurrent)
					ps.currentTab = k;
				ps.questTabs |= pi.questTab;
			}
			// the target the game's own cursor is on: its entry's hit pane lies below the target's part pane
			uint32 pane = CursorFocusPane();
			for (int up = 0; up < 4 && PaneOk(pane) && ps.gameSel < 0; up++)
			{
				for (int i = 0; i < (int)ps.targets.size(); i++)
				{
					if (ps.targets[i].part == pane)
					{
						ps.gameSel = i;
						break;
					}
				}
				pane = memory_readU32(pane + kParent);
			}
		}
	}

	std::string ShownScreens()
	{
		std::lock_guard lock(sMutex);
		std::string r;
		for (auto& [name, obj] : sScreens)
			if (ScreenShownObj(name, obj) && name != "BoxCursorTV" && name.rfind("MainScreen", 0) != 0 && name != "DoCommand_00" &&
				name != "MainShortCut_00" && name != "SeekPadMenuBG_00")
				r += (r.empty() ? "" : ",") + name;
		return r;
	}

	bool GetAmiiboFrame(Rect& r)
	{
		std::lock_guard lock(sTipMutex);
		r = sAmiiboFrame;
		return sAmiiboFrameShown;
	}

	std::vector<TipKeys> GetTipKeys()
	{
		std::lock_guard lock(sTipMutex);
		return sTipKeys;
	}

	MinimapInfo GetMinimapInfo()
	{
		std::lock_guard lock(sMutex);
		MinimapInfo info;

		// Default 16:9 positioning in 1280x720 layout space: center at (1150, 600), radius 79
		info.normCenterX = 1150.0f / kLayoutW;
		info.normCenterY = 600.0f / kLayoutH;
		info.normRadius = 79.0f / kLayoutH;

		// the panes are found once per layout (it is read every frame: the walk is not repeated while they stay valid)
		static uint32 sCacheRoot = 0, sCacheIconArea = 0, sCacheNorth = 0, sCacheRotate = 0, sCacheMiniLine = 0, sCacheMapSR = 0, sCacheMapT = 0;
		uint32 iconAreaPane = 0;
		uint32 northPane = 0;
		uint32 rotatePane = 0;
		uint32 miniLinePane = 0;
		uint32 mapSRPane = 0; // the map's scale and rotation
		uint32 mapTPane = 0;  // the map moved by Link's position

		auto searchRoot = [&](const std::string& scrName) {
			auto it = sScreens.find(scrName);
			if (it == sScreens.end() || !ValidPtr(it->second, 0x40))
				return;
			const uint32 lyt = memory_readU32(it->second + 0x18);
			if (!ValidPtr(lyt, 0x10))
				return;
			const uint32 root = memory_readU32(lyt + 0x0C);
			if (!root || !PaneOk(root))
				return;
			auto still = [](uint32 pane, const char* name) { return pane == 0 || (PaneOk(pane) && ReadName(pane) == name); };
			if (root == sCacheRoot && (sCacheIconArea || sCacheRotate) && still(sCacheIconArea, "N_IconArea_00") && still(sCacheNorth, "Pa_North_00") &&
				still(sCacheRotate, "N_Rotate_00") && still(sCacheMiniLine, "N_MiniMapLine_00") && still(sCacheMapSR, "N_MapSR_00") &&
				still(sCacheMapT, "N_MapT_00"))
			{
				iconAreaPane = sCacheIconArea;
				northPane = sCacheNorth;
				rotatePane = sCacheRotate;
				miniLinePane = sCacheMiniLine;
				mapSRPane = sCacheMapSR;
				mapTPane = sCacheMapT;
				return;
			}

			std::function<void(uint32, int)> walk = [&](uint32 pane, int depth) {
				if (depth > 20 || !PaneOk(pane))
					return;
				const std::string n = ReadName(pane);
				if (n == "N_IconArea_00")
					iconAreaPane = pane;
				else if (n == "Pa_North_00")
					northPane = pane;
				else if (n == "N_Rotate_00" && !rotatePane)
					rotatePane = pane;
				else if (n == "N_MiniMapLine_00")
					miniLinePane = pane;
				else if (n == "N_MapSR_00" && !mapSRPane)
					mapSRPane = pane;
				else if (n == "N_MapT_00" && !mapTPane)
					mapTPane = pane;

				if (iconAreaPane && northPane && rotatePane && mapSRPane && mapTPane)
					return;

				const uint32 head = pane + kChildHead;
				uint32 node = memory_readU32(head);
				for (int i = 0; i < 256 && node && node != head; i++)
				{
					if (!PaneOk(node))
						break;
					walk(node, depth + 1);
					node = memory_readU32(node + kNext);
				}
			};
			walk(root, 0);
			sCacheRoot = root;
			sCacheIconArea = iconAreaPane;
			sCacheNorth = northPane;
			sCacheRotate = rotatePane;
			sCacheMiniLine = miniLinePane;
			sCacheMapSR = mapSRPane;
			sCacheMapT = mapTPane;
		};

		searchRoot("AppMap_00");

		const uint32 geomPane = iconAreaPane ? iconAreaPane : (northPane ? northPane : miniLinePane);
		if (geomPane && PaneOk(geomPane))
		{
			const float effAlpha = AlphaChain(geomPane, false);
			if (effAlpha > 0.05f)
			{
				Rect r{};
				if (PaneRectImpl(geomPane, r))
				{
					info.visible = true;
					info.normCenterX = r.cx();
					info.normCenterY = r.cy();
					info.normRadius = (r.y1 - r.y0) * 0.5f;
					info.alpha = effAlpha;
				}
			}
		}

		// The map itself (measured): its texture has 2 units per meter (Pa_Map_00 is 24000 x 20000 for Hyrule's 12000 x 10000
		// m), x east, y north; N_MapT_00 moves it by minus Link's point, N_MapSR_00 turns it (the same angle as the compass)
		// and the panes above scale it (Pa_Map_00: 0.9, and the minimap's appearing animation).
		if (mapTPane && mapSRPane && PaneOk(mapTPane) && PaneOk(mapSRPane))
		{
			float scale = 2.0f;
			for (uint32 p = mapTPane, guard = 0; p && PaneOk(p) && guard < 32; p = memory_readU32(p + kParent), guard++)
				scale *= RF(p + kScale);
			if (scale > 0.01f)
			{
				info.hasMapTransform = true;
				info.mapCenterX = -RF(mapTPane + kTranslate) / 2.0f;
				info.mapCenterZ = RF(mapTPane + kTranslate + 4) / 2.0f;
				info.mapLayoutPerMeter = scale;
				info.compassAngle = RF(mapSRPane + 0x30) * (3.14159265358979323846f / 180.0f);
				info.hasNorthRotation = true;
			}
		}
		if (!info.hasMapTransform && rotatePane && PaneOk(rotatePane))
		{
			const float rotDeg = RF(rotatePane + 0x30);
			info.compassAngle = rotDeg * (3.14159265358979323846f / 180.0f);
			info.hasNorthRotation = true;
		}
		else if (!info.hasMapTransform && northPane && PaneOk(northPane))
		{
			const float rotDeg = RF(northPane + 0x30);
			info.compassAngle = rotDeg * (3.14159265358979323846f / 180.0f);
			info.hasNorthRotation = true;
		}

		// Hide during screen transitions / loading screens
		if (sFadeNow.load() > 0.05f || FadeAmount() > 0.05f || ScreenVisible("LoadingWeapon_00") || ScreenVisible("LoadSaveIcon_00"))
		{
			info.visible = false;
		}

		return info;
	}

	std::vector<HudPrompt> GetHudPrompts()
	{
		std::lock_guard lock(sHudMutex);
		return sHudPrompts;
	}

	void OnGameFrame()
	{
		std::vector<HudPrompt> prompts;
		const bool hud = Prompts::Active();
		if (hud)
		{
			std::lock_guard lock(sMutex);
			RefreshDoCommand(sState, prompts);
		}
		MinimapFrame mini;
		mini.info = GetMinimapInfo();
		mini.hasPose = Input::GetPlayerPose(mini.px, mini.py, mini.pz, mini.camYaw);
		std::lock_guard lock(sHudMutex);
		// The image the game hands over now was drawn before this frame's UI update (measured: the panes in memory are one
		// update ahead of the picture), so it gets the snapshot taken one frame ago.
		++sHudSeqCpu;
		if (hud)
			sHudQueue.emplace_back(sHudSeqCpu, std::move(sHudLast));
		sHudLast = std::move(prompts);
		while (sHudQueue.size() > 16)
			sHudQueue.pop_front();
		sMiniQueue.emplace_back(sHudSeqCpu, sMiniLast);
		sMiniLast = mini;
		while (sMiniQueue.size() > 16)
			sMiniQueue.pop_front();
	}

	void OnGpuFrame()
	{
		std::lock_guard lock(sHudMutex);
		++sHudSeqGpu;
		while (!sHudQueue.empty() && sHudQueue.front().first <= sHudSeqGpu)
		{
			sHudPrompts = std::move(sHudQueue.front().second);
			sHudQueue.pop_front();
		}
		while (!sMiniQueue.empty() && sMiniQueue.front().first <= sHudSeqGpu)
		{
			sMiniShown = sMiniQueue.front().second;
			sMiniShownValid = true;
			sMiniQueue.pop_front();
		}
	}

	bool GetMinimapFrame(MinimapFrame& f)
	{
		std::lock_guard lock(sHudMutex);
		f = sMiniShown;
		return sMiniShownValid;
	}

	std::string PaneChain(const std::string& screen, const std::string& name)
	{
		std::lock_guard lock(sMutex);
		std::string r;
		for (auto& [pane, ti] : sTexts)
		{
			if (ti.name != name || !PaneOk(pane) || ReadName(pane) != name || ScreenOf(pane) != screen)
				continue;
			r += fmt::format("{:08x} '{}' alpha chain {:.2f} visible {}:", pane, ti.text, AlphaChain(pane, false), Visible(pane));
			for (uint32 q = pane; PaneOk(q); q = memory_readU32(q + kParent))
				r += fmt::format(" {} f={:02x} a={} g={}", ReadName(q), memory_readU8(q + kFlag), memory_readU8(q + kFlag + 1), memory_readU8(q + kFlag + 2));
			r += "|";
		}
		return r;
	}

	std::string HudTrace(const std::string& cmd)
	{
		std::lock_guard lock(sMutex);
		if (cmd == "start")
		{
			sHudTrace.clear();
			sHudTraceOn = true;
			return "ok";
		}
		if (cmd == "chain")
		{
			// ancestors of the A label: name, flags, alpha, global alpha
			std::string r;
			for (auto& [pane, ti] : sTexts)
			{
				if (ti.name != "T_GuideA_00" || !PaneOk(pane) || ScreenOf(pane) != "DoCommand_00")
					continue;
				for (uint32 q = pane; PaneOk(q); q = memory_readU32(q + kParent))
					r += fmt::format("{} f={:02x} a={} g={}|", ReadName(q), memory_readU8(q + kFlag), memory_readU8(q + kFlag + 1), memory_readU8(q + kFlag + 2));
				break;
			}
			return r;
		}
		sHudTraceOn = false;
		std::string r;
		for (auto& e : sHudTrace)
			r += fmt::format("{} {} '{}' chain={:.2f} global={:.2f} vis={} rect=({:.3f},{:.3f})-({:.3f},{:.3f})|", e.seq, e.name, e.label, e.chain, e.global,
							 e.visible, e.rect.x0, e.rect.y0, e.rect.x1, e.rect.y1);
		sHudTrace.clear();
		return r;
	}

	std::string HudSyncStats()
	{
		std::lock_guard lock(sHudMutex);
		return fmt::format("fade {:.2f} | queue {} prompts {}", sFadeNow.load(), sHudQueue.size(), sHudPrompts.size());
	}

	FocusResult RequestFocus(uint32 part, const Rect& area, uint32 frame)
	{
		std::lock_guard lock(sFocusMutex);
		if (frame - sFocusReq.frame > 4 || sFocusReq.part != part)
			sFocusReq.result = 0;
		sFocusReq.part = part;
		sFocusReq.area = area;
		sFocusReq.frame = frame;
		if (frame - sCursorFrame.load() > 3 || sFocusReq.result == 2 || sFocusReq.result == 4)
			return FocusResult::Unavailable;
		return sFocusReq.result == 3 ? FocusResult::Focused : FocusResult::Requested;
	}

	std::string CursorInfo()
	{
		const uint32 frame = Input::GetDebug().frameCounter.load();
		const uint32 mgr = sCursorMgr.load();
		std::lock_guard lock(sFocusMutex);
		std::string r = fmt::format("mgr={:08x} age={} req_age={} req_result={} requested={:08x} owned_age={}", mgr, frame - sCursorFrame.load(),
									frame - sFocusReq.frame, sFocusReq.result, sCursorRequested.load(), frame - sCursorOwnedFrame.load());
		if (ValidPtr(mgr, 0x30))
		{
			const uint32 focus = memory_readU32(mgr + 0x24);
			r += fmt::format(" focus={:08x}", focus);
			if (ValidPtr(focus, 0x30) && ValidPtr(memory_readU32(focus + 0x08), 0x3C))
			{
				const uint32 owner = memory_readU32(focus + 0x08), pane = memory_readU32(owner + 0x38);
				if (PaneOk(pane))
					r += fmt::format("({:.3f},{:.3f}) idx={}", 0.5f + RF(pane + 0x54) / kLayoutW, 0.5f - RF(pane + 0x64) / kLayoutH, memory_readU32(owner + 0x28));
			}
		}
		return r;
	}

	bool SetPauseMenuStartPage(int page)
	{
		// U-King v1.5.0: the pause menu data singleton (pointer at 0x10469978) keeps the page the menu was closed on at
		// +0x37ED8 (setters at 0x03076D0C / 0x03076D7C / 0x03076D2C: 0 inventory, 1 system, 2 adventure log)
		constexpr uint32 kInstancePtr = 0x10469978, kLastPage = 0x37ED8;
		if (page < 0 || page > 2 || !memory_isAddressRangeAccessible(kInstancePtr, 4))
			return false;
		const uint32 inst = memory_readU32(kInstancePtr);
		if (!ValidPtr(inst, 0x10) || !ValidPtr(inst + kLastPage, 4) || memory_readU32(inst + kLastPage) > 2)
			return false;
		memory_writeU32(inst + kLastPage, (uint32)page);
		return true;
	}

	void SetBoxCursorHidden(bool hidden)
	{
		static uint32 sHiddenRoot = 0;
		std::lock_guard lock(sMutex);
		uint32 root = 0;
		if (auto it = sScreens.find("BoxCursorTV"); it != sScreens.end() && ValidPtr(it->second, 0x40))
		{
			const uint32 lyt = memory_readU32(it->second + 0x18);
			if (ValidPtr(lyt, 0x10))
				root = memory_readU32(lyt + 0x0C);
		}
		if (!PaneOk(root))
			root = 0;
		if (hidden && root)
		{
			const uint8 f = memory_readU8(root + kFlag);
			if (f & 1)
			{
				memory_writeU8(root + kFlag, f & ~1);
				sHiddenRoot = root;
			}
		}
		else if (!hidden && sHiddenRoot)
		{
			// (only if it still is the box cursor's root: the layout may have been freed meanwhile)
			if (root == sHiddenRoot && PaneOk(sHiddenRoot))
				memory_writeU8(sHiddenRoot + kFlag, memory_readU8(sHiddenRoot + kFlag) | 1);
			sHiddenRoot = 0;
		}
	}

	namespace
	{
		// a limit rectangle of the map controller (x0, y0, x1, y1, map units): the cursor's at +0x60 (the scroll's for
		// the current zoom is at +0x70, its bottom open)
		struct MapRect
		{
			float x0, y0, x1, y1;
		};

		bool ReadMapRect(uint32 addr, MapRect& r)
		{
			if (!ValidPtr(addr, 0x10))
				return false;
			r = {memory_readFloat(addr), memory_readFloat(addr + 4), memory_readFloat(addr + 8), memory_readFloat(addr + 12)};
			return !std::isnan(r.x0) && !std::isnan(r.y0) && !std::isnan(r.x1) && !std::isnan(r.y1);
		}

		float Limit(float v, float lo, float hi)
		{
			return lo <= hi ? std::clamp(v, lo, hi) : v;
		}
	}

	bool SetMapOrigin(const PointerSnapshot& ps, float normX, float normY)
	{
		if (!ps.mapCtrl || std::fabs(ps.mapScaleX) < 1e-4f || std::fabs(ps.mapScaleY) < 1e-4f)
			return false;
		const float dx = (normX - ps.mapX) * kLayoutW / ps.mapScaleX;
		const float dy = -(normY - ps.mapY) * kLayoutH / ps.mapScaleY;
		// (not held to the game's scroll limits: zoomed out they pin the map sideways and a drag would not follow the mouse)
		const float x = memory_readFloat(ps.mapCtrl + 0x20) + dx;
		const float y = memory_readFloat(ps.mapCtrl + 0x24) + dy;
		if (!std::isfinite(x) || !std::isfinite(y))
			return false;
		// scroll position (layout units, current + previous); the scaled copies at +0x08/+0x10 follow by themselves
		for (uint32 off : {0x20u, 0x30u})
		{
			memory_writeFloat(ps.mapCtrl + off, x);
			memory_writeFloat(ps.mapCtrl + off + 4, y);
		}
		// map cursor position (controller + 0x3160, negated scroll; also the target it eases to): stamps, pins and
		// fast travel use it, the stick input normally keeps both in sync
		const uint32 cursor = ps.mapCtrl - 0x648 + 0x3160;
		if (ValidPtr(cursor, 0x10))
		{
			for (uint32 off : {0x0u, 0x8u})
			{
				memory_writeFloat(cursor + off, -x);
				memory_writeFloat(cursor + off + 4, -y);
			}
		}
		return true;
	}

	bool MapPointAt(const PointerSnapshot& ps, float normX, float normY, float& x, float& y)
	{
		if (!ps.mapCtrl || std::fabs(ps.mapScaleX) < 1e-4f || std::fabs(ps.mapScaleY) < 1e-4f)
			return false;
		// the map's own units: layout units / scale, from the map origin, y up
		x = (normX - ps.mapX) * kLayoutW / ps.mapScaleX;
		y = (ps.mapY - normY) * kLayoutH / ps.mapScaleY;
		return std::isfinite(x) && std::isfinite(y);
	}

	bool SetMapCursor(const PointerSnapshot& ps, float x, float y)
	{
		// cursor (controller + 0x3160) and the target it eases to (+0x3168); the game keeps them where they are put
		// (the scroll follows only within its limits), except that it pulls the target back onto an icon the cursor
		// was on for a frame or two
		const uint32 cursor = ps.mapCtrl - 0x648 + 0x3160;
		if (!ps.mapCtrl || !std::isfinite(x) || !std::isfinite(y) || !ValidPtr(cursor, 0x10))
			return false;
		MapRect cur;
		if (ReadMapRect(ps.mapCtrl + 0x60, cur))
		{
			x = Limit(x, cur.x0, cur.x1);
			y = Limit(y, cur.y0, cur.y1);
		}
		for (uint32 off : {0x0u, 0x8u})
		{
			memory_writeFloat(cursor + off, x);
			memory_writeFloat(cursor + off + 4, y);
		}
		return true;
	}

	bool SetMapWorldCenter(const PointerSnapshot& ps, float wx, float wz)
	{
		if (!ps.mapCtrl || !std::isfinite(wx) || !std::isfinite(wz))
			return false;
		const float sx = -2.0f * wx;
		const float sy = 2.0f * wz;
		for (uint32 off : {0x20u, 0x30u})
		{
			memory_writeFloat(ps.mapCtrl + off, sx);
			memory_writeFloat(ps.mapCtrl + off + 4, sy);
		}
		const uint32 cursor = ps.mapCtrl - 0x648 + 0x3160;
		if (ValidPtr(cursor, 0x10))
		{
			for (uint32 off : {0x0u, 0x8u})
			{
				memory_writeFloat(cursor + off, -sx);
				memory_writeFloat(cursor + off + 4, -sy);
			}
		}
		return true;
	}

	int PointerSnapshot::At(float x, float y, uint32 onlyGroup) const
	{
		int best = -1;
		float bestArea = 1e9f;
		for (int i = 0; i < (int)targets.size(); i++)
		{
			const Rect& r = targets[i].rect;
			if (!r.Contains(x, y))
				continue;
			if (onlyGroup && targets[i].group && targets[i].group != onlyGroup)
				continue;
			const float area = (r.x1 - r.x0) * (r.y1 - r.y0);
			if (area < bestArea)
			{
				bestArea = area;
				best = i;
			}
		}
		return best;
	}

	int PointerSnapshot::Selected() const
	{
		if (targets.empty())
			return -1;
		if (gameSel >= 0)
			return gameSel; // the game's own cursor says so
		if (hasBoxCursor)
		{
			// the box cursor snaps onto the selected entry's bounding pane
			int in = -1;
			float inArea = 1e9f;
			for (int i = 0; i < (int)targets.size(); i++)
			{
				const Rect& h = targets[i].hit;
				const float a = (h.x1 - h.x0) * (h.y1 - h.y0);
				if (h.Contains(boxX, boxY) && a < inArea)
				{
					in = i;
					inArea = a;
				}
			}
			if (in < 0)
				in = At(boxX, boxY);
			if (in >= 0)
				return in;
			int best = -1;
			float bd = 1e9f;
			for (int i = 0; i < (int)targets.size(); i++)
			{
				const float dx = targets[i].hit.cx() - boxX, dy = targets[i].hit.cy() - boxY;
				const float d = dx * dx + dy * dy;
				if (d < bd)
				{
					bd = d;
					best = i;
				}
			}
			return bd < 0.01f ? best : -1;
		}
		// title-like menus: the selected entry's text is fully opaque, the others are dimmed
		int best = -1, count255 = 0;
		uint8 maxA = 0;
		for (int i = 0; i < (int)targets.size(); i++)
		{
			if (targets[i].label.empty())
				continue;
			if (targets[i].textAlpha >= 250)
				count255++;
			if (targets[i].textAlpha > maxA)
			{
				maxA = targets[i].textAlpha;
				best = i;
			}
		}
		return count255 == 1 ? best : -1;
	}

	int PointerSnapshot::FindLabel(std::string_view label) const
	{
		for (int i = 0; i < (int)targets.size(); i++)
			if (targets[i].label == label)
				return i;
		return -1;
	}

	const PointerSnapshot& GetPointer(uint32 frame)
	{
		std::lock_guard lock(sMutex);
		if (frame != sPointerFrame)
		{
			ComputePointer(sPointer);
			sPointerFrame = frame;
		}
		return sPointer;
	}

	bool PointerScreenShown(const State& st)
	{
		return st.title || st.inventory || st.map || st.dialog || st.choice || st.cameraDialog;
	}

	uint32 BtnToVPAD(Btn b)
	{
		switch (b)
		{
		case Btn::A: return 0x8000;
		case Btn::B: return 0x4000;
		case Btn::X: return 0x2000;
		case Btn::Y: return 0x1000;
		case Btn::L: return 0x0020;
		case Btn::R: return 0x0010;
		case Btn::ZL: return 0x0080;
		case Btn::ZR: return 0x0040;
		case Btn::Plus: return 0x0008;
		case Btn::Minus: return 0x0004;
		case Btn::Up: return 0x0200;
		case Btn::Down: return 0x0100;
		case Btn::Left: return 0x0800;
		case Btn::Right: return 0x0400;
		case Btn::LStick: return 0x00040000;
		case Btn::RStick: return 0x00020000;
		default: return 0;
		}
	}

	bool State::HasButton(Btn b) const
	{
		for (auto& a : actions)
			if (a.button == b)
				return true;
		return false;
	}

	void SuppressStuckScreens()
	{
		std::lock_guard lock(sMutex);
		for (auto& [name, obj] : sScreens)
		{
			// only screens that put the game into cursor / menu mode (not the always-on background layers)
			const bool menuLike = name.rfind("SystemWindow", 0) == 0 || name.rfind("GameOver", 0) == 0 || name.rfind("ShopBtnList", 0) == 0 ||
								  name == "PauseMenu_00" || name == "AppHome_00" || name == "AppSystemWindow_00" ||
								  name == "OptionWindow_00" || name == "End_00" || name == "BoxCursorTV";
			if (menuLike && ScreenShownObj(name, obj))
			{
				cemuLog_log(LogType::Force, "PCE ui: ignoring stuck screen {}", name);
				sSuppressed[obj] = true;
			}
		}
		sStateFrame = 0xFFFFFFFF;
		sPointerFrame = 0xFFFFFFFF;
	}

	bool State::Has(std::string_view text) const
	{
		for (auto& a : actions)
			if (a.text == text)
				return true;
		return false;
	}

	bool State::HeldItems() const
	{
		// ("Put away" is the same word as "Cancel" in some languages and then arrives as "Cancel": German "Abbrechen")
		return !anyMenu && Has("Drop") && !Has("Throw") && (Has("Put away") || Has("Far") || Has("Near"));
	}

	Btn State::ButtonFor(std::string_view text) const
	{
		for (auto& a : actions)
			if (a.text == text && a.button != Btn::None)
				return a.button;
		return Btn::None;
	}

	void RegisterHLE()
	{
		osLib_addFunction("coreinit", "PCE_UIText", HLE_UIText);
		osLib_addFunction("coreinit", "PCE_UIScreen", HLE_UIScreen);
		osLib_addFunction("coreinit", "PCE_MapObj", HLE_MapObj);
		osLib_addFunction("coreinit", "PCE_CursorTick", HLE_CursorTick);
		osLib_addFunction("coreinit", "PCE_ListTick", HLE_ListTick);
		osLib_addFunction("coreinit", "PCE_RegionLanguage", HLE_RegionLanguage);
	}

	std::atomic<uint64> sComputeTotalUs{0}, sComputeCount{0}, sComputeMaxUs{0};

	const State& Get(uint32 frame)
	{
		std::lock_guard lock(sMutex);
		if (frame != sStateFrame)
		{
			const auto t0 = std::chrono::steady_clock::now();
			Compute(sState);
			sStateFrame = frame;
			sHeldItems = sState.HeldItems();
			sThrowShown = sState.Has("Throw");
			sActionOffered = !sState.actionA.empty();
			const uint64 us = (uint64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
			sComputeTotalUs += us;
			sComputeCount++;
			if (us > sComputeMaxUs)
				sComputeMaxUs = us;
		}
		return sState;
	}

	std::string PerfStats()
	{
		const uint64 n = sComputeCount.exchange(0), t = sComputeTotalUs.exchange(0), m = sComputeMaxUs.exchange(0);
		std::lock_guard lock(sMutex);
		return fmt::format("ui compute avg {}us max {}us over {} frames, texts {}, prompt panes {}", n ? t / n : 0, m, n, sTexts.size(),
						   sPromptGlyphs.size());
	}

	std::string DumpTexts()
	{
		std::lock_guard lock(sMutex);
		RebuildRoots();
		std::ostringstream o;
		for (auto& [pane, ti] : sTexts)
		{
			if (!PaneOk(pane) || ReadName(pane) != ti.name || !Visible(pane))
				continue;
			Rect r{};
			PaneRectImpl(pane, r);
			std::string scr;
			Visible(pane, &scr);
			o << fmt::format("{:08x} {} [{}] '{}' first={:04x} rect=({:.3f},{:.3f})-({:.3f},{:.3f})|", pane, ti.name, scr, ti.text, ti.first, r.x0, r.y0, r.x1, r.y1);
		}
		o << "screens:";
		for (auto& [n, obj] : sScreens)
			o << fmt::format(" {}={:08x}{}", n, obj, ScreenVisible(n) ? "*" : "");
		return o.str();
	}

	uint32 ScreenObject(const std::string& name)
	{
		std::lock_guard lock(sMutex);
		auto it = sScreens.find(name);
		return it == sScreens.end() ? 0 : it->second;
	}

	std::string DumpTree(const std::string& query)
	{
		std::lock_guard lock(sMutex);
		uint32 start = 0;
		if (auto it = sScreens.find(query); it != sScreens.end() && ValidPtr(it->second, 0x40))
		{
			const uint32 lyt = memory_readU32(it->second + 0x18);
			if (ValidPtr(lyt, 0x10))
				start = memory_readU32(lyt + 0x0C);
		}
		for (auto& [pane, ti] : sTexts)
			if (start)
				break;
			else
			if ((ti.text.find(query) != std::string::npos || ti.name == query) && PaneOk(pane) && ReadName(pane) == ti.name)
			{
				start = pane;
				break;
			}
		if (!start)
			return "err notfound";
		uint32 root = start;
		for (int i = 0; i < 32; i++)
		{
			uint32 p = memory_readU32(root + kParent);
			if (!p || !PaneOk(p))
				break;
			root = p;
		}
		std::ostringstream o;
		std::function<void(uint32, int)> walk = [&](uint32 pane, int depth) {
			if (depth > 24 || !PaneOk(pane))
				return;
			Rect r{};
			PaneRectImpl(pane, r);
			auto it = sTexts.find(pane);
			o << fmt::format("{}{:08x} {} v={} a={} rect=({:.3f},{:.3f})-({:.3f},{:.3f}){}|", std::string(depth, '.'), pane, ReadName(pane),
							 memory_readU8(pane + kFlag) & 1, memory_readU8(pane + kFlag + 1), r.x0, r.y0, r.x1, r.y1,
							 it != sTexts.end() ? " '" + it->second.text + "'" : "");
			const uint32 head = pane + kChildHead;
			uint32 node = memory_readU32(head);
			for (int i = 0; i < 512 && node && node != head; i++)
			{
				if (!PaneOk(node))
					break;
				walk(node, depth + 1);
				node = memory_readU32(node + kNext);
			}
		};
		walk(root, 0);
		return o.str();
	}
}
