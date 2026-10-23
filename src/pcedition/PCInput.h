#pragma once

// Zelda on PC - native keyboard & mouse input.
// A dedicated thread receives Windows Raw Input (mouse deltas, buttons, wheel, keyboard) and the game's
// VPADRead() is served directly from that state, bypassing Cemu's controller-profile system.

#include <atomic>
#include <optional>

#include "pcedition/PCKeys.h"
#include "pcedition/PCSettings.h"

struct VPADStatus;

namespace pce
{
	namespace Input
	{
		// registers the HLE functions called by the PC Edition camera patch (must run before the game is loaded)
		void RegisterHLE();
		void Start();
		void Shutdown();

		// set by the window system so we can test focus
		void SetMainWindow(void* hwnd);
		bool IsGameWindowFocused();
		void SetWindowFocus(bool focused);
		void SetClientMousePos(float x, float y, float clientW, float clientH);
		void HandleMouseMove(sint32 dx, sint32 dy);
		void HandleKey(uint16 vk, bool down);
		void HandleWheel(sint32 delta);

		// called from VPADRead (emulated CPU thread) once per game frame
		void FillVPAD(VPADStatus& status);
		void SetButtonRepeat(sint32 delayMs, sint32 pulseMs);

		// key state queries (for overlay)
		bool IsKeyDown(Key k);
		// returns the next key/button press since the last call (for rebinding & overlay navigation)
		std::optional<Key> PopKeyPress();
		void ClearKeyPresses();
		// accumulated wheel notches for the overlay while it is open
		int PopOverlayWheel();

		// overlay integration: while the overlay is open, game receives neutral input and the cursor is free
		void SetOverlayOpen(bool open);
		bool IsOverlayOpen();
		// true when the cursor should be hidden and clipped to the game window
		bool WantsCursorCaptured();
		// a game menu, the map or the scope is shown (Esc acts as Back there)
		bool GameMenuOpen();

		// ---- the PC settings in the System page of the pause menu (see PCOverlay) ----
		// where the game would show its picture of the GamePad (normalized picture coordinates), from the last game frame
		struct SystemPanel
		{
			bool shown = false;
			float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		};
		SystemPanel GetSystemPanel();
		// the settings have the keyboard (and the wheel): the game gets no input meanwhile
		void SetEmbeddedFocus(bool focus);
		bool EmbeddedFocus();
		// the mouse is over the settings / map panel: the wheel scrolls them instead of switching the menu's pages
		void SetPanelHover(bool hover);
		bool PanelHover();
		// the PC settings key during play: the pause menu opens on its System page (the PC menu when it cannot open)
		void RequestSystemMenu();
		// keys held right now count for the game only after they have been released
		void LatchHeldKeys();
		// a popup of the PC layer (amiibo) is open over the game: it has keyboard, wheel and a free cursor
		void SetPopupOpen(bool open);
		// the game's buttons (VPAD mask) held for some frames, from the PC layer (e.g. cancel when its popup is closed)
		void PressGameButtons(uint32 mask, int frames);

		// dev bridge: a raw mouse movement as if it came from the device (goes through the same path as real input)
		void DebugRawMouse(sint32 dx, sint32 dy);

		// tilt (gyro) mode
		bool IsTiltActive();
		void SetTiltActive(bool active);

		// player world position and camera yaw (radians, 0 = North, +PI/2 = East)
		bool GetPlayerPose(float& px, float& py, float& pz, float& camYaw);

		// diagnostics (dev bridge)
		struct Debug
		{
			std::atomic<float> lastStickRX{0}, lastStickRY{0};
			std::atomic<float> lastDesiredYawDeg{0}, lastDesiredPitchDeg{0};
			std::atomic<uint32> frameCounter{0};
			std::atomic<bool> overrideRightStick{false};
			std::atomic<bool> forceFocus{false}; // dev: treat the game window as focused (mouse tests)
			std::atomic<float> overrideRX{0}, overrideRY{0};
			std::atomic<bool> overrideLeftStick{false};
			std::atomic<float> overrideLX{0}, overrideLY{0};
			std::atomic<uint32> overrideButtons{0};
			std::atomic<sint64> injectMouseDX{0}, injectMouseDY{0};
			std::atomic<uint32> cameraObject{0};
			std::atomic<uint32> lastHold{0};
			// virtual pointer for automated menu tests (bypasses focus)
			std::atomic<bool> overrideMouse{false};
			std::atomic<float> overrideMouseX{0}, overrideMouseY{0};
			std::atomic<bool> overrideLmb{false}, overrideRmb{false};
			std::atomic<sint32> injectWheel{0};
			// Magnesis: how fast the held object follows the view (degrees per second, and metres per second along its
			// circle, whichever is less)
			std::atomic<float> magnetFollowDeg{360.0f}, magnetFollowMetres{40.0f};
			std::atomic<uint64> overrideKeysAction{0}; // bit per Action, held as if pressed
			std::atomic<bool> lastPointerMode{false}, lastGameMenu{false};
			// motion tests: fixed tilt pose (degrees) instead of the mouse driven one
			std::atomic<bool> overrideTilt{false};
			std::atomic<float> overrideTiltPitch{0}, overrideTiltRoll{0};
		};
		Debug& GetDebug();
		// diagnostics: who reads the GamePad (return address) and how many samples it asks for
		void NoteVPADCaller(uint32 lr, uint32 length);
		// diagnostics: frames since the chase / aim camera hooks last ran
		std::string CameraAges();
		// dev: a client area position mapped to the game's picture, and the picture's place in the window
		std::string DebugPictureMap(float x, float y, float clientW, float clientH);
		// dev: a wheel movement taken like one from the mouse (WHEEL_DELTA = one notch)
		void DebugRawWheel(sint32 delta);
		// dev: a key going down / up like one from the keyboard (virtual key code)
		void DebugRawKey(uint16 vk, bool down);
		std::string VPADCallers();
		// diagnostics: the motion values the game was given last (pose, attitude matrix, angles, rates)
		std::string MotionInfo();
	}
}
