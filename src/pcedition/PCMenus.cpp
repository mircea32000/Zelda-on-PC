#include "pcedition/PCMenus.h"
#include "pcedition/PCSettings.h"

#include <atomic>
#include <chrono>

namespace pce::Menus
{
	namespace
	{
		constexpr uint32 kA = 0x8000, kB = 0x4000, kR = 0x0010, kL = 0x0020, kPlus = 0x0008;
		constexpr uint32 kUp = 0x0200, kDown = 0x0100, kLeft = 0x0800, kRight = 0x0400;

		// button pulses (press for a few frames, then a gap so the game sees a new trigger)
		struct Pulse
		{
			uint32 buttons = 0;
			int on = 0, off = 0;
			bool Busy() const { return on > 0 || off > 0; }
			void Start(uint32 b, int onFrames = 3, int offFrames = 3)
			{
				buttons = b;
				on = onFrames;
				off = offFrames;
			}
			uint32 Step()
			{
				if (on > 0)
				{
					on--;
					return buttons;
				}
				if (off > 0)
					off--;
				return 0;
			}
		};

		Pulse sNav, sConfirm;

		// pointer navigation
		uint32 sPendingPart = 0; // click queued on this target
		uint32 sPendingFrame = 0;
		uint32 sLastMouseMoveFrame = 0;
		float sLastMx = -1, sLastMy = -1;
		int sLastSelected = -2;
		uint32 sNavWaitUntil = 0;
		// direct focus (UI::RequestFocus) of the wanted entry: which one and since when
		uint32 sFocusPart = 0, sFocusSince = 0;
		bool sHoverChase = false; // the hovered entry is not selected yet
		// The entry that was selected by hovering. It is not selected a second time while the mouse stays on it: the
		// keyboard (or the game) may move the selection away, and hovering must never fight with either.
		uint32 sHoverDonePart = 0;

		// inventory categories (switched with the right stick)
		int sTabTarget = -1;
		uint32 sTabUntil = 0;
		int sTabSteps = 0;      // flicks still to send (signed)
		int sTabPhase = 0;      // position inside the current flick (1 frame on, 1 off)
		uint32 sTabVerifyAt = 0;
		int sTabRetries = 0;
		constexpr uint32 kTabCheckFrames = 3; // frames from a flick until the shown category is read again (measured)

		void RequestTab(int target, int current, uint32 frame)
		{
			const int from = sTabTarget >= 0 ? sTabTarget : current;
			sTabSteps += target - from;
			sTabTarget = target;
			sTabRetries = 0;
			sTabUntil = frame + 600;
			sTabVerifyAt = frame + kTabCheckFrames;
		}

		// inventory: a category click jumps straight to the category's first page (the game's own page jump, applied on
		// the game thread by HLE_InvJump); the check afterwards falls back to flicks if the category was not reached
		std::atomic<int> sInvJump{-1};
		std::atomic<int> sInvFlick{0}; // set by HLE_InvJump: the sliding last step
		std::atomic<int> sInvJumpPages{0}; // several wheel notches: pages to jump (HLE_InvJump)
		void JumpTab(int target, uint32 frame)
		{
			sInvJump = target;
			sInvJumpPages = 0;
			sInvFlick = 0;
			sTabTarget = target;
			sTabSteps = 0;
			sTabPhase = 0;
			sTabRetries = 0;
			sTabUntil = frame + 600;
			sTabVerifyAt = frame + 12;
		}

		// inventory: the mouse wheel turns one page per notch with the game's slide (eased, not snapped); notches queue
		// up and each page starts once the previous slide has ended
		int sWheelPages = 0;
		uint32 sWheelNextAt = 0;
		uint32 sSmoothUntil = 0;
		std::atomic<bool> sSmoothSlide{false};
		std::atomic<sint64> sLastSlideMs{0}; // last game frame with a page slide running (HLE_SlideSnap)
		sint64 NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		bool InventorySliding()
		{
			return NowMs() - sLastSlideMs.load() < 40;
		}

		// inventory: rotate Link by dragging (middle button anywhere, left button on empty space)
		bool sRotDrag = false;
		float sRotLastX = 0;

		// map
		bool sMapDragging = false, sMapClick = false, sMapTargetActive = false, sMapConfirm = false;
		float sDragMx = 0, sDragMy = 0, sDragOx = 0, sDragOy = 0;
		float sMapTx = 0, sMapTy = 0;
		uint32 sMapTargetFrame = 0;
		int sMapSettle = 0;
		int sMapCursorFrames = 0; // frames the map cursor is still held on the clicked spot before the confirm
		float sMapCx = 0, sMapCy = 0; // the clicked spot (map units)
		int sZoomFrames = 0;
		float sZoomDir = 0;

		// pause menu page navigation (I = inventory, J = adventure log)
		std::atomic<int> sPageRequest{-1};
		int sPageTarget = -1;
		uint32 sPageStart = 0, sPageNext = 0;
		int sPageSteps = 0;
		std::atomic<int> sCurrentPage{-1};

		Page DetectPage(const UI::State& st, const UI::PointerSnapshot& ps)
		{
			if (!st.inventory)
				return Page::None;
			if (ps.tabCount > 0)
				return ps.questTabs ? Page::AdventureLog : Page::Inventory;
			if (ps.FindLabel("Save") >= 0 || ps.FindLabel("Load") >= 0)
				return Page::System;
			return Page::AdventureLog;
		}

		// the game's code for a page (see UI::SetPauseMenuStartPage)
		int StartPageCode(Page p)
		{
			return p == Page::Inventory ? 0 : p == Page::System ? 1 : 2;
		}

		// macros
		enum class Macro
		{
			None,
			OpenMenu,
			FindButton,
			Select,
			Verify
		};
		std::atomic<int> sRequested{0}; // 1 save, 2 load
		Macro sMacro = Macro::None;
		std::string sMacroLabel;
		uint32 sMacroFrame = 0;
		int sMacroTries = 0;

		void Navigate(const UI::PointerSnapshot& ps, int from, int to, uint32 frame)
		{
			if (sNav.Busy() || frame < sNavWaitUntil)
				return;
			const auto& a = ps.targets[from].rect;
			const auto& b = ps.targets[to].rect;
			const float dx = b.cx() - a.cx(), dy = b.cy() - a.cy();
			uint32 dir = std::fabs(dx) > std::fabs(dy) ? (dx > 0 ? kRight : kLeft) : (dy > 0 ? kDown : kUp);
			sNav.Start(dir, 2, 2);
			sNavWaitUntil = frame + 8; // give the menu time to move its cursor
		}

		int FindPart(const UI::PointerSnapshot& ps, uint32 part)
		{
			for (int i = 0; i < (int)ps.targets.size(); i++)
				if (ps.targets[i].part == part)
					return i;
			return -1;
		}

		void UpdateMap(const UI::PointerSnapshot& ps, FrameIO& io)
		{
			io.consumeMouseButtons = true;
			const float mx = io.mouseX, my = io.mouseY;
			if (io.lmbPressed && io.mouseInside)
			{
				sDragMx = mx;
				sDragMy = my;
				sDragOx = ps.mapX;
				sDragOy = ps.mapY;
				sMapDragging = false;
				sMapClick = true;
			}
			if (io.lmbDown && sMapClick)
			{
				if (std::fabs(mx - sDragMx) + std::fabs(my - sDragMy) > 0.01f)
					sMapDragging = true;
				if (sMapDragging)
				{
					// grab & drag: the map follows the mouse 1:1
					sMapTx = sDragOx + (mx - sDragMx);
					sMapTy = sDragOy + (my - sDragMy);
					sMapTargetActive = true;
					sMapConfirm = false;
					sMapTargetFrame = io.frame;
				}
			}
			if (io.lmbReleased && sMapClick)
			{
				sMapClick = false;
				if (!sMapDragging && io.mouseInside)
				{
					if (UI::MapPointAt(ps, mx, my, sMapCx, sMapCy) && UI::SetMapCursor(ps, sMapCx, sMapCy))
					{
						// the map cursor jumps to the clicked spot (the map stays still), then confirm
						sMapTargetActive = false;
						sMapCursorFrames = 4;
					}
					else
					{
						// fallback: steer the clicked spot under the map cursor (screen centre) with the stick, then confirm
						sMapTx = ps.mapX + (0.5f - mx);
						sMapTy = ps.mapY + (0.5f - my);
						sMapTargetActive = true;
						sMapConfirm = true;
						sMapTargetFrame = io.frame;
					}
				}
				else if (sMapDragging && ps.mapCtrl)
					sMapTargetActive = false; // the drag wrote the scroll directly: nothing left to reach (past its limits the game eases it back)
				sMapDragging = false;
			}
			if (sMapCursorFrames > 0)
			{
				// hold it there until the game lets go of the icon it was on (it pulls the cursor back to an icon
				// that was just selected) and the map picks up what is under it, then confirm
				UI::SetMapCursor(ps, sMapCx, sMapCy);
				if (--sMapCursorFrames == 0 && !sConfirm.Busy())
					sConfirm.Start(kA, 3, 3);
			}
			if (sMapTargetActive)
			{
				const float ex = sMapTx - ps.mapX, ey = sMapTy - ps.mapY;
				const bool reached = std::fabs(ex) < 0.004f && std::fabs(ey) < 0.004f;
				if (reached || io.frame - sMapTargetFrame > 300)
				{
					// let the map pick up what is under its cursor for a few frames before confirming
					if (sMapSettle > 0)
						sMapSettle--;
					else if (!io.lmbDown || !sMapDragging)
					{
						sMapTargetActive = false;
						if (sMapConfirm)
							sConfirm.Start(kA, 3, 3);
						sMapConfirm = false;
					}
				}
				else if (UI::SetMapOrigin(ps, sMapTx, sMapTy))
				{
					// instant: the map's scroll position is written directly
					sMapSettle = sMapConfirm ? 4 : 0;
				}
				else
				{
					// fallback: steer with the stick
					constexpr float kGain = 22.0f;
					io.overrideLStick = true;
					io.lx = std::clamp(-ex * kGain, -1.0f, 1.0f);
					io.ly = std::clamp(ey * kGain, -1.0f, 1.0f);
					sMapSettle = 0;
				}
			}
			if (io.rmbPressed && !sConfirm.Busy())
				sConfirm.Start(kB, 3, 3); // back / close the map
			if (io.wheel != 0)
			{
				sZoomDir = io.wheel > 0 ? 1.0f : -1.0f;
				sZoomFrames = 5 * std::min(4, std::abs(io.wheel));
			}
			if (sZoomFrames > 0)
			{
				sZoomFrames--;
				io.overrideRStick = true;
				io.rx = 0;
				io.ry = sZoomDir;
			}
		}

		void UpdateMacro(const UI::State& st, const UI::PointerSnapshot& ps, FrameIO& io)
		{
			if (int r = sRequested.exchange(0))
			{
				sMacro = Macro::OpenMenu;
				sMacroLabel = r == 1 ? "Save" : "Load";
				sMacroFrame = io.frame;
				sMacroTries = 0;
				// the pause menu opens directly on its System page (no sliding through the other pages)
				if (!st.inventory)
					UI::SetPauseMenuStartPage(StartPageCode(Page::System));
			}
			switch (sMacro)
			{
			case Macro::None:
				return;
			case Macro::OpenMenu:
				if (st.inventory)
				{
					sMacro = Macro::FindButton;
					sMacroFrame = io.frame;
				}
				else if (io.frame - sMacroFrame == 1)
					sConfirm.Start(kPlus, 3, 3);
				else if (io.frame - sMacroFrame > 150)
					sMacro = Macro::None; // pause menu not available right now
				break;
			case Macro::FindButton:
			{
				const int t = ps.FindLabel(sMacroLabel);
				if (t >= 0)
				{
					sPendingPart = ps.targets[t].part;
					sPendingFrame = io.frame;
					sMacro = Macro::Select;
				}
				else if (io.frame - sMacroFrame > 20)
				{
					// another page is shown (the menu was already open): go to the System page
					if (++sMacroTries > 6)
						sMacro = Macro::None;
					else
					{
						sConfirm.Start(kR, 3, 3);
						sMacroFrame = io.frame;
					}
				}
				break;
			}
			case Macro::Select:
				if (sPendingPart == 0)
				{
					// confirmed by the pointer logic; the game ignores it while the menu is still opening
					sMacro = Macro::Verify;
					sMacroFrame = io.frame;
				}
				break;
			case Macro::Verify:
				if (st.dialog || !st.inventory)
					sMacro = Macro::None; // the save / load window is up
				else if (io.frame - sMacroFrame > 14)
				{
					sMacro = ++sMacroTries > 8 ? Macro::None : Macro::FindButton;
					sMacroFrame = io.frame;
				}
				break;
			}
		}
	}

	void RequestPage(Page p)
	{
		// closed menu: it opens directly on the page (the game reads the page it was "closed on" when it opens);
		// the L / R steps below remain as the way to switch while the menu is open
		if (sCurrentPage.load() == (int)Page::None && p != Page::None)
			UI::SetPauseMenuStartPage(StartPageCode(p));
		sPageRequest = (int)p;
	}

	Page CurrentPage()
	{
		return (Page)sCurrentPage.load();
	}

	void RequestSave()
	{
		sRequested = 1;
	}

	void RequestLoad()
	{
		sRequested = 2;
	}

	bool MacroActive()
	{
		return sMacro != Macro::None || sRequested.load() != 0;
	}

	int TakeInventoryJump()
	{
		return sInvJump.exchange(-1);
	}

	int TakeInventoryJumpPages()
	{
		return sInvJumpPages.exchange(0);
	}

	void RequestInventoryFlick(int dir)
	{
		sInvFlick = dir;
	}

	int InventorySlideMode()
	{
		sLastSlideMs = NowMs();
		return sSmoothSlide.load() ? GetSettings().inventoryPageTurn : 0;
	}

	void UpdatePages(const UI::State& st, FrameIO& io)
	{
		if (int r = sPageRequest.exchange(-1); r >= 0)
		{
			sPageTarget = r;
			sPageStart = io.frame;
			sPageNext = io.frame + 6;
			sPageSteps = 0;
		}
		const Page cur = st.inventory ? DetectPage(st, UI::GetPointer(io.frame)) : Page::None;
		sCurrentPage = (int)cur;
		if (sPageTarget < 0)
			return;
		if (!st.inventory)
		{
			if (io.frame - sPageStart > 150)
				sPageTarget = -1; // the pause menu did not open
			return;
		}
		if ((int)cur == sPageTarget || sPageSteps >= 4 || io.frame - sPageStart > 300)
		{
			sPageTarget = -1;
			return;
		}
		if (io.frame >= sPageNext && !sConfirm.Busy())
		{
			sConfirm.Start(sPageTarget < (int)cur ? kL : kR, 3, 3);
			sPageNext = io.frame + 24; // let the page slide in before checking again
			sPageSteps++;
		}
	}

	void Update(const UI::State& st, FrameIO& io)
	{
		UpdatePages(st, io);
		bool pointerScreen = UI::PointerScreenShown(st) && !st.scope && !st.cameraRune;
		if (pointerScreen)
		{
			// only when the screen really offers something to point at (no cursor for a stale "shown" flag)
			const UI::PointerSnapshot& probe = UI::GetPointer(io.frame);
			pointerScreen = probe.hasMap || probe.hasBoxCursor || !probe.targets.empty();
		}
		const bool macro = MacroActive();
		if (!pointerScreen && !macro)
		{
			sPendingPart = 0;
			sMapTargetActive = false;
			sMapClick = false;
			sMapCursorFrames = 0;
			sTabTarget = -1;
			sTabSteps = 0;
			sTabPhase = 0;
			sInvJump = -1;
			sInvJumpPages = 0;
			sWheelPages = 0;
			sSmoothSlide = false;
			sHoverChase = false;
			sHoverDonePart = 0;
			io.holdAdd |= sNav.Step() | sConfirm.Step();
			return;
		}
		const UI::PointerSnapshot& ps = UI::GetPointer(io.frame);
		io.pointerMode = pointerScreen;
		UpdateMacro(st, ps, io);

		const int sel = ps.Selected();
		// world map: drag / click-to-centre, unless a sub menu (stamp box, travel prompt, ...) has the box cursor
		const bool mapMode = pointerScreen && st.map && ps.hasMap && !(ps.hasBoxCursor && sel >= 0);
		if (mapMode)
			UpdateMap(ps, io);
		else
			sMapCursorFrames = 0;
		const uint32 selGroup = sel >= 0 ? ps.targets[sel].group : 0;
		if (pointerScreen && !mapMode)
		{
			io.consumeMouseButtons = true;
			if (std::fabs(io.mouseX - sLastMx) + std::fabs(io.mouseY - sLastMy) > 0.002f)
			{
				if (io.frame - sLastMouseMoveFrame >= 20)
					sHoverDonePart = 0; // the mouse moves again after a rest: hovering selects again
				sLastMouseMoveFrame = io.frame;
			}
			sLastMx = io.mouseX;
			sLastMy = io.mouseY;
			const int hover = io.mouseInside ? ps.At(io.mouseX, io.mouseY, selGroup) : -1;
			// inventory: the white "<" / ">" next to the item grid turn the page, like a flick of the right stick (not
			// while an item's action list is open: its entries belong to no page)
			int arrow = 0;
			if (st.inventory && io.mouseInside && (sel < 0 || selGroup != 0))
				for (int k = 0; k < 2; k++)
				{
					const UI::Rect& r = ps.pageArrowRect[k];
					if (ps.pageArrow[k] && io.mouseX >= r.x0 - 0.012f && io.mouseX <= r.x1 + 0.012f && io.mouseY >= r.y0 - 0.02f && io.mouseY <= r.y1 + 0.02f)
						arrow = k == 0 ? -1 : 1;
				}
			if (io.lmbPressed && arrow != 0)
			{
				if (sTabTarget < 0 && std::abs(sTabSteps) < 3)
					sTabSteps += arrow;
			}
			else if (io.lmbPressed && hover >= 0)
			{
				if (ps.targets[hover].tab >= 0)
				{
					// category tab: the inventory jumps straight to the category, the adventure log switches with the right stick
					if (ps.currentTab >= 0)
					{
						sWheelPages = 0;
						sSmoothSlide = false;
						if (st.inventory && !ps.questTabs)
							JumpTab(ps.targets[hover].tab, io.frame);
						else
							RequestTab(ps.targets[hover].tab, ps.currentTab, io.frame);
					}
				}
				else
				{
					sPendingPart = ps.targets[hover].part;
					sPendingFrame = io.frame;
				}
			}
			if (io.rmbPressed && !sConfirm.Busy())
				sConfirm.Start(kB, 3, 3);
			if (st.inventory)
			{
				if (io.lmbPressed && hover < 0 && arrow == 0)
					sRotDrag = true;
				if (!io.lmbDown)
					sRotDrag = false;
				if (sRotDrag || io.mmbDown)
				{
					const float dx = io.mouseX - sRotLastX;
					io.overrideRStick = true;
					io.rx = std::clamp(dx * 90.0f, -1.0f, 1.0f);
					io.ry = 0;
				}
			}
			else
				sRotDrag = false;
			sRotLastX = io.mouseX;
			if (io.wheel != 0)
			{
				// adventure log: the wheel scrolls the quest list, and switches categories only over their icons
				bool overTabs = !ps.questTabs;
				for (auto& t : ps.targets)
					overTabs |= t.tab >= 0 && io.mouseY >= t.rect.y0 - 0.03f && io.mouseY <= t.rect.y1 + 0.05f;
				if (ps.tabCount > 0 && ps.currentTab >= 0 && overTabs && st.inventory && !ps.questTabs)
				{
					// inventory: the wheel turns the pages one by one, sliding like the game does (queued notches)
					if (sTabTarget < 0)
						sWheelPages = std::clamp(sWheelPages + (io.wheel > 0 ? -1 : 1) * std::abs(io.wheel), -12, 12);
				}
				else if (ps.tabCount > 0 && ps.currentTab >= 0 && overTabs)
				{
					// adventure log: the wheel flips through the categories
					const int base = sTabTarget >= 0 ? sTabTarget : ps.currentTab;
					RequestTab(std::clamp(base + (io.wheel > 0 ? -1 : 1) * std::min(3, std::abs(io.wheel)), 0, ps.tabCount - 1), ps.currentTab, io.frame);
				}
				else if (!sNav.Busy())
					sNav.Start(io.wheel > 0 ? kUp : kDown, 2, 3);
			}
		}
		// the last step of a category jump slides in (see HLE_InvJump)
		if (const int d = sInvFlick.exchange(0); d != 0)
		{
			sSmoothSlide = true;
			sTabSteps = d;
			sTabPhase = 0;
			sSmoothUntil = io.frame + 30;
		}
		// wheel pages, once the previous slide has ended: one notch slides one page; several notches waiting jump over the
		// pages in between and only the last one slides in (no catching up page by page). These slides follow the Inventory
		// Page Turn setting, every other page change stays snapped.
		if (sWheelPages != 0 && sTabSteps == 0 && sTabTarget < 0 && io.frame >= sWheelNextAt && !InventorySliding() &&
			sInvJumpPages.load() == 0)
		{
			sSmoothSlide = true;
			sWheelNextAt = io.frame + 4;     // the slide starts a frame or two after the flick
			sSmoothUntil = io.frame + 30;
			if (std::abs(sWheelPages) >= 2)
			{
				sInvJumpPages = sWheelPages;
				sWheelPages = 0;
			}
			else
			{
				sTabSteps = sWheelPages;
				sWheelPages = 0;
			}
		}
		else if (sSmoothSlide.load() && sWheelPages == 0 && sTabSteps == 0 && io.frame > sSmoothUntil && !InventorySliding())
			sSmoothSlide = false;
		// category / page switching: one flick per page; with the slide snap patch every page change is applied in the
		// frame of its flick (no slide animation), so a burst reaches any category in a few frames. A category request
		// is verified afterwards, a page arrow click is a single flick.
		if (sTabSteps != 0)
		{
			const int dir = sTabSteps > 0 ? 1 : -1;
			if (sTabPhase == 0)
			{
				io.overrideRStick = true;
				io.rx = (float)dir;
				io.ry = 0;
			}
			if (++sTabPhase >= 2)
			{
				sTabPhase = 0;
				sTabSteps -= dir;
				sTabVerifyAt = io.frame + (sSmoothSlide.load() ? 30 : kTabCheckFrames); // a sliding page turn: check once it ended
			}
		}
		else if (sTabTarget >= 0 && io.frame >= sTabVerifyAt && !InventorySliding())
		{
			if (ps.currentTab < 0 || ps.currentTab == sTabTarget || io.frame > sTabUntil || sTabRetries >= 40)
				sTabTarget = -1;
			else
			{
				sTabSteps = sTabTarget - ps.currentTab; // categories with several pages (Materials, Food...): continue until the category is reached
				sTabRetries++;
			}
		}

		// move the selection to the pending click target, or to the hovered entry while the mouse moves
		if (sel != sLastSelected)
		{
			sNavWaitUntil = std::min(sNavWaitUntil, io.frame + 2);
			sLastSelected = sel;
		}
		int desired = -1;
		if (sPendingPart)
		{
			desired = FindPart(ps, sPendingPart);
			if (desired < 0 || io.frame - sPendingFrame > 240)
				sPendingPart = 0;
		}
		else if (pointerScreen && !mapMode && io.mouseInside && sTabTarget < 0 && sTabSteps == 0 &&
				 (io.frame - sLastMouseMoveFrame < 20 || (sHoverChase && io.frame - sLastMouseMoveFrame < 90)))
		{
			// the selection follows the mouse while it moves, and until it has arrived on the entry the mouse stopped on
			// (afterwards the keyboard can move it away again)
			desired = ps.At(io.mouseX, io.mouseY, selGroup);
			if (desired >= 0 && ps.targets[desired].tab >= 0)
				desired = -1; // hovering a tab does not switch categories
			if (desired < 0 || ps.targets[desired].part != sHoverDonePart)
				sHoverDonePart = 0; // the mouse left that entry
			else
				desired = -1; // this entry was already selected by hovering
		}
		bool reached = desired >= 0 && desired == sel;
		if (desired >= 0 && !reached)
		{
			const uint32 part = ps.targets[desired].part;
			if (part != sFocusPart)
			{
				sFocusPart = part;
				sFocusSince = io.frame;
			}
			// The game's cursor jumps straight onto the entry. D-pad steps where that is not possible (lists move their
			// cursor themselves) or has no effect.
			const UI::FocusResult direct = UI::RequestFocus(part, ps.targets[desired].hit, io.frame);
			if (direct == UI::FocusResult::Focused)
				reached = true;
			else if (sel >= 0 && (direct == UI::FocusResult::Unavailable || io.frame - sFocusSince > 6))
				Navigate(ps, sel, desired, io.frame);
		}
		else
			sFocusPart = 0;
		if (reached)
		{
			if (!sPendingPart)
				sHoverDonePart = ps.targets[desired].part;
			else if (!sConfirm.Busy() && !sNav.Busy())
			{
				sConfirm.Start(kA, 3, 3);
				sPendingPart = 0;
			}
		}
		sHoverChase = !sPendingPart && desired >= 0 && !reached;
		io.holdAdd |= sNav.Step() | sConfirm.Step();
	}
}
