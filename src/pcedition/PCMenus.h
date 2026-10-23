#pragma once

// Zelda on PC - mouse pointer in the game's own menus + menu macros (save / load from the PC menu).
//
// The game's menus are driven by d-pad / stick navigation. We read the shown layouts (PCUI), find the hit areas
// the Wii U touch screen would have used (bounding panes "B_Hit*") and the current selection (menu box cursor or
// dimmed entries), then ask the game's cursor manager to focus the entry under the mouse (UI::RequestFocus; d-pad
// steps where that is not possible); a click confirms it.
// The world map keeps its cursor in the centre and scrolls: dragging pans the map 1:1, a click centres the clicked
// spot and confirms (pins, fast travel), the wheel zooms.

#include "pcedition/PCUI.h"

namespace pce::Menus
{
	struct FrameIO
	{
		// in
		uint32 frame = 0;
		bool focused = false;
		bool mouseInside = false;
		float mouseX = 0, mouseY = 0; // normalized client coordinates
		bool lmbDown = false, lmbPressed = false, lmbReleased = false;
		bool rmbPressed = false;
		bool mmbDown = false;
		int wheel = 0;
		// out
		bool pointerMode = false;   // show the OS cursor, mouse does not move the camera
		uint32 holdAdd = 0;         // buttons pressed by the pointer / macros this frame
		bool overrideLStick = false;
		float lx = 0, ly = 0;
		bool overrideRStick = false;
		float rx = 0, ry = 0;
		bool consumeMouseButtons = false; // LMB/RMB/wheel are used by the pointer, not by gameplay bindings
	};

	void Update(const UI::State& st, FrameIO& io);

	// pause menu pages (L / R order)
	enum class Page : int
	{
		None = -1,
		AdventureLog = 0,
		Inventory = 1,
		System = 2
	};
	// after the pause menu opens (or while it is open), switch to this page
	void RequestPage(Page p);
	// page currently shown by the pause menu (None when it is closed)
	Page CurrentPage();

	// queued from the PC settings menu
	void RequestSave();
	void RequestLoad();
	bool MacroActive();

	// inventory page carousel, called on the game thread (PCInput HLE_InvJump / HLE_SlideSnap)
	// the category tab (left to right, as the tabs are shown) a click asked to jump to, or -1; taken once
	int TakeInventoryJump();
	// several mouse wheel notches at once: the pages to turn (signed), or 0; taken once
	int TakeInventoryJumpPages();
	// after a jump: one page turn in this direction that slides (the last step into the clicked category)
	void RequestInventoryFlick(int dir);
	// a page slide runs this frame: 0 apply it at once, 1 short slide, 2 the game's slide (category jump step, mouse wheel
	// as the Inventory Page Turn setting says; every other page change at once)
	int InventorySlideMode();
}
