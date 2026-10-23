#pragma once

// Zelda on PC - user settings (settings.ini next to the executable)

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "pcedition/PCKeys.h"

namespace pce
{
	// Every bindable game action. Order defines menu order.
	enum class Action : uint32
	{
		MoveForward,
		MoveBack,
		MoveLeft,
		MoveRight,
		Walk,
		Sprint,       // B (hold)
		Jump,         // X
		Crouch,       // L-stick click
		Attack,       // Y
		Interact,     // A
		Bow,          // ZR
		Target,       // ZL (lock on / shield)
		UseRune,      // L
		Throw,        // R
		Scope,        // R-stick click
		Whistle,      // D-pad down
		WeaponSelect, // hold D-pad right
		BowSelect,    // hold ZR + D-pad right
		ShieldSelect, // hold D-pad left
		RuneSelect,   // hold D-pad up
		WeaponNext,   // wheel: next weapon (or arrow while aiming)
		WeaponPrev,
		Inventory,    // +
		Map,          // -
		Journal,      // + then the Adventure Log page
		MenuUp,       // D-pad
		MenuDown,
		MenuLeft,
		MenuRight,
		Confirm,      // A (menus)
		Back,         // B (menus)
		CameraUp,     // optional keyboard camera
		CameraDown,
		CameraLeft,
		CameraRight,
		TiltMode,
		TiltLevel,    // while Motion Control is active: the slate back to flat
		Settings,
		MenuPrevPage, // L (inventory: Adventure Log / Inventory / System)
		MenuNextPage, // R
		Count
	};
	constexpr size_t kActionCount = (size_t)Action::Count;

	struct ActionInfo
	{
		const char* id;    // ini key
		const char* name;  // display name
		const char* group; // display group
		Key defaults[2];
	};
	const ActionInfo& GetActionInfo(Action a);

	enum class SelectorMode : int
	{
		Weapons = 0,
	};

	struct Binding
	{
		std::array<Key, 2> keys{ Key::None, Key::None };
	};

	struct Settings
	{
		// mouse camera
		float mouseSensitivity = 2.0f;      // multiplier; base = 0.022 deg per count (Source-style)
		float mouseSensitivityY = 1.0f;     // vertical multiplier relative to horizontal
		float aimSensitivity = 0.8f;        // multiplier while aiming (bow/scope/first person)
		bool invertX = false;
		bool invertY = false;
		float velocityWindowMs = 16.0f;     // window over which mouse velocity is measured
		bool rawInput = true;

		// keyboard camera (CameraUp/Down/Left/Right actions)
		float keyCameraSpeed = 0.75f;       // stick deflection

		// movement
		float walkSpeed = 0.45f;            // stick magnitude while Walk is held
		bool parryWithAttack = true;        // shield raised + attack key = perfect guard
		bool toggleSprint = false;          // one press keeps sprinting (until pressed again or standing still)
		bool toggleShield = false;          // one press keeps Lock On / Shield held until pressed again
		bool bombKeepsBow = false;          // a remote bomb is out: the bow key still draws the bow (Use Rune detonates)

		// weapon wheel
		float wheelReleaseDelayMs = 450.0f; // after the last wheel notch, release the selector (equips item)
		float selectorMouseStep = 60.0f;    // mouse counts per selection step while a selector key is held
		bool selectorMoveKeys = false;      // Move Left / Right step through an open selector (and do not move Link then)
		bool inventoryMoveKeys = false;     // Move Left / Right move the inventory cursor instead of switching its pages

		// tilt (gyro emulation)
		float tiltSensitivity = 1.0f;       // multiplier; base = 0.05 deg per count
		bool tiltInvertX = false;
		bool tiltInvertY = false;
		bool tiltToggle = true;             // true: key toggles tilt mode, false: hold
		float tiltMaxAngle = 60.0f;         // degrees (forward / back and left / right; up to 180: upside down)
		float tiltReturnSpeed = 0.0f;       // deg/s auto-centering (0 = off)
		float tiltTurnStep = 15.0f;         // degrees the GamePad turns flat (yaw) per mouse wheel notch
		bool tiltInvertTurn = false;
		float tiltSmoothing = 35.0f;        // ms: time constant of the spring the slate follows the mouse with (0 = at once)
		float tiltMaxSpeed = 300.0f;        // deg/s the slate turns at most (quicker mouse movement is not used)

		// graphics (applied through graphic packs / renderer)
		int resolutionPreset = 1;           // index into resolution list
		int aspectRatio = 0;                // 0 automatic (21:9 / 32:9 on such a display, else 16:9), 1 16:9, 2 16:10, 3 21:9, 4 32:9
		int ultrawideHud = 0;               // 21:9 / 32:9: 0 HUD at the screen edges, 1 HUD centred (16:9 area)
		int fpsLimit = 60;                  // 30, 60, 90, 120, 144, 165 or 240 (FPS++ limit)
		bool vsync = true;
		int shadowPreset = 1;
		int antiAliasing = 1;
		float fov = 1.0f;                   // field of view multiplier (if supported)
		bool showFps = false;
		// Pin the emulated CPU cores / GPU thread to four cores of one L3 cache group. Off by default: about +1 % on the
		// Ryzen 7 4800H it was measured on, but it broke the frame pacing on a Ryzen 9 9950X3D (two cache groups, Windows
		// already keeps the game on the V-Cache one), and a thread tied to one core cannot get out of the way of others.
		bool cpuPinning = false;
		int frameGeneration = 0;            // 0 off, 1 AMD FSR 3 frame generation

		// audio (Cemu's TV audio): the volume is applied at once, speakers and device when the game starts
		float volume = 50.0f;               // percent
		int speakers = 1;                   // 0 mono, 1 stereo, 2 surround 5.1
		std::string audioDevice;            // identifier of the output device (empty: the system's default)

		// display
		int displayMode = 1;                // 0 windowed, 1 borderless (a window covering the screen), 2 exclusive fullscreen

		// paths
		std::string gamePath;               // folder containing the base game (auto-detected if empty)

		// game
		std::string language = "en";        // text language (code of pce::GetGameLanguages), applied when the game starts
		int temperatureUnit = 0;            // 0 as the game decides (Fahrenheit for US English), 1 Celsius, 2 Fahrenheit
		int inventoryPageTurn = 1;          // category clicks / mouse wheel in the inventory: 0 instant, 1 short slide, 2 the game's slide

		// interactive map (Sheikah Slate map overlay)
		bool mapEnabled = true;             // master toggle: completely enable or disable the interactive map
		bool mapOverlay = true;             // show the interactive map overlay on the world map
		bool mapMinimap = true;             // show nearby interactive map markers on the HUD minimap compass
		bool mapPanelOpen = true;           // filter & search panel expanded on the map screen
		bool mapAutoHide = true;            // auto-hide claimed chests, completed shrines, activated towers, collected koroks
		bool mapDimClaimed = false;         // when auto-hide is off, dim claimed markers instead of showing at full opacity
		bool mapMasterMode = false;         // include Master Mode exclusive spawns and chests
		uint32 mapCategories = 0x001Fu;     // bitmask of enabled categories (default: Shrines, Towers, Koroks, Chests, Bosses)
		std::string userGraphicPacks; // enabled mods (graphic packs that are not the edition's own): ids separated by ';'
		std::string userPackPresets;  // presets chosen for the mods: "id|category|preset" entries separated by ';'

		std::array<Binding, kActionCount> bindings;

		Binding& bind(Action a) { return bindings[(size_t)a]; }
		const Binding& bind(Action a) const { return bindings[(size_t)a]; }
		void ResetBindings();
		void ResetAll();
	};

	// global settings access. Readers take a snapshot; writers go through Update().
	Settings GetSettings();
	void UpdateSettings(const std::function<void(Settings&)>& fn, bool save = true);
	uint64 GetSettingsRevision();

	void LoadSettings(const fs::path& iniPath);
	void SaveSettings();
	fs::path GetSettingsPath();

	// returns actions which share a key with another action (conflicts)
	std::vector<std::pair<Action, Action>> FindBindingConflicts(const Settings& s);
}
