#pragma once

// Zelda on PC - live model of the game's own UI (nn::ui2d / nw::lyt layouts of U-King.rpx v1.5.0).
//
// Patched game code reports screen layouts (ui screen load) and every text box string change to us.
// From any known pane the whole layout tree can be walked (sibling links +0x00/+0x04, parent +0x0C,
// child list head +0x14), so we can tell which screens/panes are visible, where they are and what they say.
// This drives context-sensitive controls (the game's own on-screen action guides) and mouse support in menus.

#include <string>
#include <vector>

namespace pce::UI
{
	void RegisterHLE();

	// buttons as the game names them in its guides
	enum class Btn : uint8
	{
		None, A, B, X, Y, L, R, ZL, ZR, Plus, Minus, Up, Down, Left, Right, LStick, RStick
	};
	uint32 BtnToVPAD(Btn b);

	struct Action
	{
		std::string text; // English label as displayed ("Dash", "Let go", "Detonate", ...)
		Btn button = Btn::None;
	};

	struct Rect
	{
		float x0, y0, x1, y1; // normalized window coordinates (0..1, origin top-left)
		bool Contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
		float cx() const { return (x0 + x1) * 0.5f; }
		float cy() const { return (y0 + y1) * 0.5f; }
	};

	struct MenuButton
	{
		uint32 pane = 0;    // button part pane
		std::string label;  // its text
		Rect rect{};
	};

	struct State
	{
		bool valid = false;
		bool inventory = false;    // pause menu (inventory / system)
		bool map = false;          // sheikah slate map
		bool title = false;        // title screen menu
		bool dialog = false;       // system window / game over / other modal screen
		bool choice = false;       // a conversation shows its answer buttons (message dialog)
		bool scope = false;        // scope / camera view (guides "Place pin" / "Quit")
		bool cameraRune = false;   // the Sheikah Slate's camera (AppCamera_00) looking: Capture, Zoom, Album, Self-Portrait, Quit
		bool cameraDialog = false; // ... asking about the picture just taken ("Save in album?": Save / Delete, pointer)
		bool anyMenu = false;      // gameplay HUD is suspended by a menu
		bool hud = false;          // gameplay HUD screen is shown (hidden in cutscenes / events)
		std::vector<Action> actions;      // visible context actions (guides, do-command diamond)
		std::string actionA;              // visible A action of the do-command prompt ("Take", "Stop Time", ...)
		bool shortcut = false;            // a weapon / bow / shield / rune selector is shown
		bool systemPanel = false;         // the System page of the pause menu is shown: its picture of the GamePad (hidden)
		Rect systemRect{};                //   lies here; the PC settings are drawn in its place
		std::string shortcutName;         // its highlighted entry (English for "Remove"; empty: the slot is empty)
		std::vector<MenuButton> buttons;  // visible menu buttons (title, system windows, game over, ...)

		bool Has(std::string_view text) const;
		bool HasButton(Btn b) const;
		// Materials are held in the hands (taken with "Hold" in the inventory): the guides are "Drop", "Put away" and
		// "Far" / "Near" (camera). A bomb or a carried object has "Throw" as well.
		bool HeldItems() const;
		Btn ButtonFor(std::string_view text) const;
	};

	// recomputed at most once per game frame (cheap enough to call from VPADRead)
	const State& Get(uint32 frame);

	// ---- pointer (mouse) support: full walk of the shown layouts ----
	struct Target
	{
		uint32 part = 0;     // owning part pane (identity)
		Rect rect{};         // click area (bounding pane, or the button's text when the bounding pane is off-screen)
		Rect hit{};          // raw bounding pane (the menu box cursor snaps to it, even when it is off-screen)
		std::string label;   // button text if any
		uint8 textAlpha = 0; // alpha of the button text (title-like menus dim unselected entries)
		uint32 group = 0;    // page the entry belongs to (inventory pages slide in from the sides), 0 = none
		int tab = -1;        // category tab index (inventory), -1 = not a tab
	};
	struct PointerSnapshot
	{
		bool valid = false;
		std::vector<Target> targets;  // visible, on-screen hit areas
		bool hasBoxCursor = false;    // menu selection box (inventory, file select, system pages)
		float boxX = 0, boxY = 0;     // its centre (normalized)
		bool hasMap = false;          // world map shown on the TV
		float mapX = 0, mapY = 0;     // screen position of the map origin (normalized)
		uint32 mapCtrl = 0;           // map controller scroll state (vec2 target/current..., layout units), 0 = unknown
		float mapScaleX = 1, mapScaleY = 1; // layout scale applied to the map origin's translation
		int tabCount = 0;             // category tabs (inventory, adventure log), enabled ones only
		int currentTab = -1;          // highlighted category
		bool questTabs = false;       // the tabs are the quest categories of the adventure log
		int gameSel = -1;             // target the game's own menu cursor is on (-1: cursor not running / not on a target)
		bool pageArrow[2] = {false, false}; // inventory: "<" / ">" next to the item grid is shown (a page lies that way)
		Rect pageArrowRect[2]{};
		int Selected() const;         // index of the selected target or -1
		// target under the point; entries of other (partially visible) pages are ignored
		int At(float x, float y, uint32 onlyGroup = 0) const;
		int FindLabel(std::string_view label) const;
	};
	const PointerSnapshot& GetPointer(uint32 frame);
	// Put the menu selection box on the entry of a target (its part pane, or the entry that lies inside the area). The
	// game's own cursor update applies the request in its next run, exactly like a cursor move of the player.
	enum class FocusResult
	{
		Unavailable, // not possible: no cursor update running, the shown screen moves the cursor itself (lists do, every
					 // frame), or the target has no selectable entry. The caller steps with the d-pad instead.
		Requested,   // handed to the game
		Focused      // the game's cursor is on that entry
	};
	FocusResult RequestFocus(uint32 part, const Rect& area, uint32 frame);
	// diagnostics: state of the game's cursor manager
	std::string CursorInfo();
	struct MinimapInfo
	{
		bool visible = false;
		float normCenterX = 0.8984375f;
		float normCenterY = 0.8333333f;
		float normRadius = 0.109722f; // (79.0f / 720.0f)
		float compassAngle = 0.0f;    // radians
		bool hasNorthRotation = false;
		float alpha = 1.0f;
		uint32 paneAddr = 0;
		char paneName[32] = {0};
		// the map as the game draws it (N_MapT_00 / N_MapSR_00): the world point at the center and layout pixels
		// (1280 x 720) per meter, scales of the minimap's panes included
		bool hasMapTransform = false;
		float mapCenterX = 0.0f, mapCenterZ = 0.0f;
		float mapLayoutPerMeter = 0.0f;
	};
	MinimapInfo GetMinimapInfo();
	// the minimap and Link's position as in the frame the GPU thread is showing (taken per game frame with OnGameFrame,
	// like the HUD prompts); false before the first frame
	struct MinimapFrame
	{
		MinimapInfo info;
		bool hasPose = false;
		float px = 0.0f, py = 0.0f, pz = 0.0f, camYaw = 0.0f;
	};
	bool GetMinimapFrame(MinimapFrame& f);

	// HUD action buttons drawn by the overlay in place of the hidden GamePad artwork (rect normalized, glyph = prompt code)
	struct HudPrompt
	{
		Rect rect;
		uint16 glyph;
		float alpha;
	};
	std::vector<HudPrompt> GetHudPrompts();
	// "Ability Controls" (System page): the pages show pictures of the GamePad with the button to use lit. The pictures
	// are hidden; this is where they were and which buttons they showed (game button codes U+E040.., or one of the
	// composite glyphs of PCPrompts for a stick's two directions), for the PC keys drawn in their place
	struct TipKeys
	{
		Rect rect;
		std::vector<uint16> codes;
	};
	std::vector<TipKeys> GetTipKeys();
	// "Use amiibo" (the amiibo rune): the frame of the game's picture of the GamePad with its NFC touchpoint (hidden, and
	// the text asking to touch an amiibo to it); PCOverlay puts the choice of amiibo files there. False when not shown.
	bool GetAmiiboFrame(Rect& r);
	// frame synchronisation of the HUD prompts: the game finished a frame (CPU thread, GX2CopyColorBufferToScanBuffer of
	// the TV image) / the GPU thread is about to show that frame
	void OnGameFrame();
	void OnGpuFrame();
	std::string HudSyncStats();
	std::string HudTrace(const std::string& cmd); // developer: start | dump | chain
	std::string PaneChain(const std::string& screen, const std::string& name); // developer: flags / alpha of a text pane and its ancestors
	// Esc safety net: screens that look stuck "shown" are ignored until the game hides them
	void SuppressStuckScreens();
	// diagnostics: cost of the per-frame UI analysis
	std::string PerfStats();
	// diagnostics: names of the screens currently shown (HUD layers omitted)
	std::string ShownScreens();
	// scroll the world map so that its origin lands at the given normalized screen position (instant)
	bool SetMapOrigin(const PointerSnapshot& ps, float normX, float normY);
	// the map point (the map cursor's units) under a screen spot; false if unknown
	bool MapPointAt(const PointerSnapshot& ps, float normX, float normY, float& x, float& y);
	// puts the map's cursor and the target it eases to on a map point (the map does not move); false if unknown
	bool SetMapCursor(const PointerSnapshot& ps, float x, float y);
	// centres the world map and its cursor on the given world position (wx, wz in metres)
	bool SetMapWorldCenter(const PointerSnapshot& ps, float wx, float wz);
	// page the pause menu opens on next time (0 inventory, 1 system, 2 adventure log); the game stores the page it was
	// closed on and reads it back when the menu opens. False if the game's menu data is not available.
	bool SetPauseMenuStartPage(int page);
	// the menus' selection box (BoxCursorTV) off while the PC settings in the System page have the keyboard: one selection
	// on the screen, not two
	void SetBoxCursorHidden(bool hidden);
	// true when a game menu/screen that wants a mouse pointer is shown (title, file select, dialogs, inventory, map, game over)
	bool PointerScreenShown(const State& st);

	// developer: dump all panes of the layouts that contain a text pane with the given text / screen name
	std::string DumpTree(const std::string& screenOrText);
	std::string DumpTexts();
	// developer: the game's screen object of a screen name (0 when not open)
	uint32 ScreenObject(const std::string& name);
}
