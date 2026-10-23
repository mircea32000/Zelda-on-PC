#include "pcedition/PCInput.h"
#include "pcedition/PCEdition.h"
#include "pcedition/PCProfiler.h"
#include "pcedition/PCCamera.h"
#include "pcedition/PCUI.h"
#include "pcedition/PCMenus.h"
#include "pcedition/PCOverlay.h"
#include "pcedition/PCGraphics.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

#if BOOST_OS_WINDOWS
#include <windows.h>
#else
#include <chrono>
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define WHEEL_DELTA 120
using HWND = void*;
using DWORD = uint32;
#endif
#include <deque>
#include <thread>

#include "Cafe/OS/libs/vpad/vpad.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "input/motion/MotionSample.h"
#include "Cafe/HW/MMU/MMU.h"
#include "input/motion/MotionHandler.h"
#include "util/math/quaternion.h"

std::atomic<bool> g_pceProbeArmed{false};   // latency probe (LatteRenderTarget.cpp reads it)
std::atomic<uint64> g_pceProbeInputTick{0};

namespace pce::Input
{
	namespace
	{
		bool KeyDownNow(Key k);
		enum : uint32
		{
			VPAD_A = 0x8000,
			VPAD_B = 0x4000,
			VPAD_X = 0x2000,
			VPAD_Y = 0x1000,
			VPAD_L = 0x0020,
			VPAD_R = 0x0010,
			VPAD_ZL = 0x0080,
			VPAD_ZR = 0x0040,
			VPAD_PLUS = 0x0008,
			VPAD_MINUS = 0x0004,
			VPAD_HOME = 0x0002,
			VPAD_UP = 0x0200,
			VPAD_DOWN = 0x0100,
			VPAD_LEFT = 0x0800,
			VPAD_RIGHT = 0x0400,
			VPAD_STICK_R = 0x00020000,
			VPAD_STICK_L = 0x00040000,
			VPAD_STICK_L_UP = 0x10000000,
			VPAD_STICK_L_DOWN = 0x08000000,
			VPAD_STICK_L_LEFT = 0x40000000,
			VPAD_STICK_L_RIGHT = 0x20000000,
			VPAD_STICK_R_UP = 0x01000000,
			VPAD_STICK_R_DOWN = 0x00800000,
			VPAD_STICK_R_LEFT = 0x04000000,
			VPAD_STICK_R_RIGHT = 0x02000000,
			VPAD_REPEAT = 0x80000000,
		};

		Debug sDebug;

		// ---------------- raw input thread state ----------------
		std::thread sThread;
		std::atomic<bool> sQuit{false};
		std::atomic<HWND> sMsgWnd{nullptr};
		std::atomic<HWND> sMainWnd{nullptr};
		std::atomic<DWORD> sThreadId{0};

		std::array<std::atomic<bool>, 256> sKeyDown{};
		std::mutex sPressMutex;
		std::deque<Key> sPresses;

		struct MouseSample
		{
			sint64 t; // QPC ticks
			sint32 dx, dy;
		};
		std::mutex sMouseMutex;
		std::deque<MouseSample> sMouseSamples; // samples not yet consumed by the game
		std::atomic<sint32> sWheelGame{0};
		std::atomic<sint32> sWheelOverlay{0};
		sint32 sWheelRemainder = 0;

		std::atomic<bool> sOverlayOpen{false};
		std::atomic<bool> sPointerMode{false};  // a game menu wants the mouse pointer
		std::atomic<bool> sGameMenuOpen{false}; // any game menu / scope is shown (Esc = Back)
		std::atomic<bool> sEmbedFocus{false};   // the PC settings in the System page have the keyboard
		std::atomic<bool> sPanelHover{false};   // the mouse is over them
		std::atomic<bool> sPopupOpen{false};    // a popup of the PC layer is open (amiibo)
		std::atomic<uint32> sPressMask{0};      // PressGameButtons
		std::atomic<int> sPressFrames{0};
		std::atomic<bool> sSystemMenuRequest{false};
		std::array<std::atomic<bool>, 256> sKeyLatched{};
		std::mutex sPanelMutex;
		Input::SystemPanel sPanel;
		std::atomic<sint64> sEscGraceUntil{0};  // a menu was just closed with Esc: Esc does not open the PC menu yet
		std::atomic<bool> sTiltActive{false};
		std::atomic<bool> sFocused{false};

		sint64 QpcNow()
		{
#if BOOST_OS_WINDOWS
			LARGE_INTEGER li;
			QueryPerformanceCounter(&li);
			return li.QuadPart;
#else
			return (sint64)std::chrono::steady_clock::now().time_since_epoch().count();
#endif
		}

		double QpcFreq()
		{
#if BOOST_OS_WINDOWS
			static double f = []() { LARGE_INTEGER li; QueryPerformanceFrequency(&li); return (double)li.QuadPart; }();
			return f;
#else
			using Period = std::chrono::steady_clock::period;
			static double f = (double)Period::den / (double)Period::num;
			return f;
#endif
		}

		void ReleaseAllKeys()
		{
			for (auto& k : sKeyDown)
				k.store(false);
			ClearKeyPresses();
		}

		void PushPress(Key k)
		{
			if (k == Key::Tab && (KeyDownNow(Key::LAlt) || KeyDownNow(Key::RAlt)))
				return;
			std::lock_guard lock(sPressMutex);
			if (sPresses.size() > 64)
				sPresses.pop_front();
			sPresses.push_back(k);
		}

		void OnWheel(sint32 delta)
		{
			// accumulate (supports high resolution wheels)
			sWheelRemainder += delta;
			const sint32 notches = sWheelRemainder / WHEEL_DELTA;
			sWheelRemainder -= notches * WHEEL_DELTA;
			if (notches == 0)
				return;
			if (!sOverlayOpen.load() && !sEmbedFocus.load() && !sPanelHover.load() && !sPopupOpen.load())
				sWheelGame += notches; // (the game is paused under the PC menu: what is scrolled there is not for it)
			sWheelOverlay += notches;
			for (sint32 i = 0; i < std::abs(notches); i++)
				PushPress(notches > 0 ? Key::WheelUp : Key::WheelDown);
		}

		void SetKey(uint16 vk, bool down)
		{
			if (vk >= 256)
				return;
			bool was = sKeyDown[vk].exchange(down);
			if (down && !was)
				PushPress((Key)vk);
		}

		std::atomic<float> sClientMouseX{-1.0f}, sClientMouseY{-1.0f};
		std::atomic<float> sClientW{1920.0f}, sClientH{1080.0f};

		void LogFocusChange(bool focused)
		{
#if BOOST_OS_WINDOWS
			HWND fg = GetForegroundWindow();
			wchar_t cls[128]{}, title[128]{};
			if (fg)
			{
				GetClassNameW(fg, cls, 127);
				GetWindowTextW(fg, title, 127);
			}
			DWORD pid = 0;
			if (fg)
				GetWindowThreadProcessId(fg, &pid);
			cemuLog_log(LogType::Force, "PCE input: focus {} (foreground hwnd={:x} pid={} class='{}' title='{}')", focused ? "gained" : "lost",
						(uintptr_t)fg, pid, boost::nowide::narrow(cls), boost::nowide::narrow(title));
#else
			cemuLog_log(LogType::Force, "PCE input: focus {}", focused ? "gained" : "lost");
#endif
		}

		bool ComputeFocus()
		{
#if BOOST_OS_WINDOWS
			HWND main = sMainWnd.load();
			if (!main)
				return false;
			HWND fg = GetForegroundWindow();
			if (!fg)
				return false;
			return fg == main || IsChild(main, fg) || GetAncestor(fg, GA_ROOTOWNER) == main;
#else
			return sFocused.load() || sDebug.forceFocus.load();
#endif
		}

#if BOOST_OS_WINDOWS
		void HandleRawInput(HRAWINPUT hri)
		{
			alignas(8) BYTE buffer[256];
			UINT size = sizeof(buffer);
			if (GetRawInputData(hri, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
				return;
			const RAWINPUT* ri = (const RAWINPUT*)buffer;
			const bool focused = ComputeFocus();
			if (focused != sFocused.load())
			{
				sFocused = focused;
				LogFocusChange(focused);
				ReleaseAllKeys();
				ClearKeyPresses();
			}
			if (!focused)
				return;
			if (ri->header.dwType == RIM_TYPEMOUSE)
			{
				const RAWMOUSE& m = ri->data.mouse;
				if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && (m.lLastX != 0 || m.lLastY != 0) && !sOverlayOpen.load() && !sEmbedFocus.load() &&
					!sPopupOpen.load())
				{
					std::lock_guard lock(sMouseMutex);
					const sint64 t = QpcNow();
					sMouseSamples.push_back({t, (sint32)m.lLastX, (sint32)m.lLastY});
					if (sMouseSamples.size() > 20000)
						sMouseSamples.pop_front();
				}
				const USHORT f = m.usButtonFlags;
				if (f & RI_MOUSE_LEFT_BUTTON_DOWN) SetKey(VK_LBUTTON, true);
				if (f & RI_MOUSE_LEFT_BUTTON_UP) SetKey(VK_LBUTTON, false);
				if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) SetKey(VK_RBUTTON, true);
				if (f & RI_MOUSE_RIGHT_BUTTON_UP) SetKey(VK_RBUTTON, false);
				if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) SetKey(VK_MBUTTON, true);
				if (f & RI_MOUSE_MIDDLE_BUTTON_UP) SetKey(VK_MBUTTON, false);
				if (f & RI_MOUSE_BUTTON_4_DOWN) SetKey(VK_XBUTTON1, true);
				if (f & RI_MOUSE_BUTTON_4_UP) SetKey(VK_XBUTTON1, false);
				if (f & RI_MOUSE_BUTTON_5_DOWN) SetKey(VK_XBUTTON2, true);
				if (f & RI_MOUSE_BUTTON_5_UP) SetKey(VK_XBUTTON2, false);
				if (f & RI_MOUSE_WHEEL)
					OnWheel((SHORT)m.usButtonData);
			}
			else if (ri->header.dwType == RIM_TYPEKEYBOARD)
			{
				const RAWKEYBOARD& kb = ri->data.keyboard;
				USHORT vk = kb.VKey;
				if (vk == 0 || vk >= 255)
					return;
				const bool down = (kb.Flags & RI_KEY_BREAK) == 0;
				const bool e0 = (kb.Flags & RI_KEY_E0) != 0;
				switch (vk)
				{
				case VK_SHIFT: vk = (kb.MakeCode == 0x36) ? VK_RSHIFT : VK_LSHIFT; break;
				case VK_CONTROL: vk = e0 ? VK_RCONTROL : VK_LCONTROL; break;
				case VK_MENU: vk = e0 ? VK_RMENU : VK_LMENU; break;
				default: break;
				}
				SetKey(vk, down);
			}
		}

		LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			if (msg == WM_INPUT)
			{
				HandleRawInput((HRAWINPUT)lp);
				return DefWindowProcW(hwnd, msg, wp, lp);
			}
			return DefWindowProcW(hwnd, msg, wp, lp);
		}

		void InputThread()
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
			sThreadId = GetCurrentThreadId();
			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = MsgWndProc;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = L"BotWPCInput";
			RegisterClassExW(&wc);
			HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
			sMsgWnd = hwnd;
			RAWINPUTDEVICE rid[2]{};
			rid[0].usUsagePage = 0x01;
			rid[0].usUsage = 0x02; // mouse
			rid[0].dwFlags = RIDEV_INPUTSINK;
			rid[0].hwndTarget = hwnd;
			rid[1].usUsagePage = 0x01;
			rid[1].usUsage = 0x06; // keyboard
			rid[1].dwFlags = RIDEV_INPUTSINK;
			rid[1].hwndTarget = hwnd;
			if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
				cemuLog_log(LogType::Force, "PCInput: RegisterRawInputDevices failed ({})", GetLastError());
			MSG msg;
			while (!sQuit.load())
			{
				// wake up periodically to refresh focus state even without input
				DWORD r = MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
				{
					if (msg.message == WM_QUIT)
						sQuit = true;
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}
				const bool focused = ComputeFocus();
				if (focused != sFocused.load())
				{
					sFocused = focused;
					LogFocusChange(focused);
					if (!focused)
						ReleaseAllKeys();
				}
				(void)r;
			}
			rid[0].dwFlags = RIDEV_REMOVE;
			rid[0].hwndTarget = nullptr;
			rid[1].dwFlags = RIDEV_REMOVE;
			rid[1].hwndTarget = nullptr;
			RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
			DestroyWindow(hwnd);
			sMsgWnd = nullptr;
		}
#else
		void InputThread()
		{
			while (!sQuit.load())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				const bool focused = ComputeFocus();
				if (focused != sFocused.load())
				{
					sFocused = focused;
					LogFocusChange(focused);
					if (!focused)
						ReleaseAllKeys();
				}
			}
		}
#endif

		// Position in the client area of the game window -> position in the game's picture (0 - 1). The picture keeps
		// its shape: in a window of another shape (16:10 display, 16:9 picture on an ultrawide display, ...) it has bars,
		// and menu entries are where the picture shows them, not at the same fraction of the window.
		void MapToPicture(float x, float y, float clientW, float clientH, float& outX, float& outY, bool& inside)
		{
			float px = 0.0f, py = 0.0f, pw = clientW, ph = clientH;
			int ix, iy, iw, ih, ww, wh;
			if (Graphics::GetPictureArea(ix, iy, iw, ih, ww, wh))
			{
				// (the renderer counts in physical pixels; the same thing as the client area for this DPI aware process)
				const float kx = clientW / (float)ww, ky = clientH / (float)wh;
				px = ix * kx;
				py = iy * ky;
				pw = iw * kx;
				ph = ih * ky;
			}
			outX = (x - px) / pw;
			outY = (y - py) / ph;
			inside = outX >= 0.0f && outX <= 1.0f && outY >= 0.0f && outY <= 1.0f;
		}

		// ---------------- per-frame game input state (emulated CPU thread) ----------------
		struct FrameState
		{
			bool initialized = false;
			sint64 lastQpc = 0;
			uint32 lastHold = 0;
			std::chrono::steady_clock::time_point lastHoldChange{}, lastPulse{};
			sint32 repeatDelay = 40000, repeatPulse = 0;
			uint64 settingsRev = 0;
			Settings settings;
			std::array<bool, kActionCount> prevAction{};
			// camera
			CameraConverter camera;
			// selector state machine
			struct Selector
			{
				bool active = false;
				bool keyDriven = false;   // opened by a hold key (vs. wheel)
				Action sourceAction = Action::Count;
				uint32 holdButtons = 0;   // buttons held while the selector is open
				uint32 preButtons = 0;    // buttons that must be held before the d-pad (bow selector: ZR)
				sint32 frame = 0;         // frames since opening
				sint32 pendingSteps = 0;  // queued stick flicks (+ right, - left)
				sint32 flickFrame = 0;    // progress of current flick
				sint32 flickDir = 0;
				double mouseAccum = 0.0;
				sint64 lastActivityQpc = 0;
				sint32 keyDir = 0;        // movement keys stepping through it (Settings::selectorMoveKeys)
				sint32 keyFrames = 0;
				sint32 closeFrame = -1;   // bow selector: the shot is being cancelled before the aim button is let go
				sint32 cancelFrame = -1;  // bow selector: frames the cancel button has been held
				bool closeEmpty = false;  // bow selector: no bow drawn at the end (closed on "Remove", or the slot was empty)
				bool shown = false;       // the game has shown the selector
				bool openedEmpty = false; // it opened on an empty slot (no bow: the aim button drew nothing)
			} sel;
			sint64 selClosedQpc = 0;      // when a selector closed by itself (the game's menu takes a moment longer)
			bool systemMenuWait = false;  // the pause menu is being opened on its System page (PC settings key)
			uint32 systemMenuFrame = 0;
			// tilt
			float tiltPitch = 0.0f, tiltRoll = 0.0f; // degrees
			// direct camera: keeps a tiny stick deflection while the mouse is moving
			float manualDirX = 0.0f, manualDirY = 0.0f;
			sint64 manualHoldUntil = 0;
			// pointer / context state
			bool prevLmb = false, prevRmb = false;
			bool prevPointer = false, prevMenu = false;
			sint64 lastDodgeQpc = 0; // lock-on + jump (side hop / backflip): Flurry Rush window
			int closeMenuFrames = 0; // pulse of + that closes the pause menu
			// attack clicks are held for a minimum time (mouse clicks are much shorter than GamePad presses and the game can
			// miss them, e.g. Flurry Rush in slow motion); rapid clicks still get a release in between
			bool atkPhys = false;
			bool atkVirtual = false;
			sint64 atkExtendUntil = 0;
			sint64 atkPressQpc = 0;
			int atkGapFrames = 0;
			sint64 flurryPulseStart = 0, flurryPulseUntil = 0; // attack pulse train during a Flurry Rush window
			// Esc as Back in game menus
			std::array<sint64, 3> escTimes{};  // last presses (safety net)
			std::array<uint64, 3> escPrints{}; // menu fingerprints at those presses
			sint64 escLastPress = 0;
			sint64 escDialogPress = 0;         // last press while a dialog was shown
			sint64 escPendingUntil = 0;        // a press made while that dialog was closing, delivered when it is gone
			int escPendingFrames = 0;
			uint32 escPendingFrame = 0;        // frame of that press
			sint64 dialogSeenQpc = 0;          // last time a dialog was shown
			int dialogGoneFrames = 0;
			sint64 escNetCheckAt = 0;          // safety net: decided a moment after the third press
			uint64 escNetPrint = 0;
			bool prevEscIsBack = false;
			bool glideClose = false;           // the jump key is closing the paraglider (until it is released)
			bool climbTake = false;            // the interact key was pressed for what the game offered while climbing (until it is released)
			// toggle modes (Settings::toggleSprint / toggleShield): the action stays "held" after one press
			bool sprintLatch = false, shieldLatch = false;
			bool prevSprintKey = false, prevShieldKey = false;
			int sprintGapFrames = 0;           // Sprint key pressed again while latched: release briefly, the game needs a new press
			sint64 sprintStillQpc = 0;         // since when the player stands still while sprint is latched
			bool mouseLatched = false; // mouse buttons used by a menu stay inert until released
			std::array<uint32, kActionCount> latchSuppress{}; // context remaps: original button stays off until release
			int pushPullFrames = 0;
			uint32 pushPullButton = 0;
		};
		FrameState sFS;
		std::mutex sFSMutex;

		// ---------------- direct camera injection (main chase camera) ----------------
		// Patched game code calls these HLE functions every frame (see PCGraphics: patch_PCCamera.asm).
		std::mutex sCamMutex;
		double sPendingYaw = 0.0, sPendingPitch = 0.0; // degrees, right/up positive
		std::atomic<uint32> sCamTickFrame{0xF0000000};
		// sign of the game's per-frame deltas relative to "camera turns right / looks up"
		constexpr double kGameYawSign = -1.0;
		constexpr double kGamePitchSign = -1.0;

		int sYawAge = 0, sPitchAge = 0; // camera ticks since the pending value was last consumed

		void HLE_CamTick(PPCInterpreter_t* hCPU)
		{
			sCamTickFrame = sDebug.frameCounter.load();
			sDebug.cameraObject = hCPU->gpr[3];
			{
				// drop mouse rotation the camera did not ask for (e.g. pitch locked, scripted camera)
				std::lock_guard lock(sCamMutex);
				if (++sYawAge > 2)
					sPendingYaw = 0.0;
				if (++sPitchAge > 2)
					sPendingPitch = 0.0;
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// paraglider: the glide state of the player ticks this every frame while Link is gliding
		std::atomic<uint32> sGlideTickFrame{0xF0000000};
		void HLE_GlideTick(PPCInterpreter_t* hCPU)
		{
			sGlideTickFrame = sDebug.frameCounter.load();
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// aim cameras (the zoomed one of the scope and the bow, and the one of the runes): same pending values, own sign convention
		std::atomic<uint32> sAimTickFrame{0xF0000000};
		constexpr double kAimYawSign = -1.0;
		constexpr double kAimPitchSign = -1.0;

		void HLE_AimTick(PPCInterpreter_t* hCPU)
		{
			sAimTickFrame = sDebug.frameCounter.load();
			HLE_CamTick(hCPU);
		}

		// The zoomed camera alone (0x02B9250C: the scope, and the bow while it is drawn). The other aim camera
		// (0x02B94E30) is the one of the runes that are aimed: Magnesis, Stasis, Cryonis.
		std::atomic<uint32> sScopeTickFrame{0xF0000000};

		void HLE_ScopeTick(PPCInterpreter_t* hCPU)
		{
			sScopeTickFrame = sDebug.frameCounter.load();
			HLE_AimTick(hCPU);
		}

		double TakePending(double& v, int& age)
		{
			std::lock_guard lock(sCamMutex);
			double r = v;
			v = 0.0;
			age = 0;
			return r;
		}

		// Camera rune (the Sheikah Slate's camera). Its own camera class, update 0x02BCED3C (r3 = camera), called from the
		// camera manager: the chase camera hooks do not run and the mouse went through right stick emulation. U-King v1.5.0:
		// pitch at +0x28 (degrees, stick up lowers it, the game limits it to -70..75), added to before the update runs; the
		// yaw is the angle the update turns by the stick (0x02BCF698: f1 = stick x * rate + current yaw, degrees, stick
		// right lowers it), hooked like the aim cameras (PCE_AimYawAbs). Zoom (field of view at +0x54) is on the d-pad.
		// A zoomed camera: the aiming sensitivity applies, as for the scope.
		std::atomic<uint32> sRuneCamFrame{0xF0000000};

		void HLE_RuneCamTick(PPCInterpreter_t* hCPU)
		{
			const uint32 cam = hCPU->gpr[3];
			const uint32 frame = sDebug.frameCounter.load();
			sRuneCamFrame = frame;
			sScopeTickFrame = frame;
			sAimTickFrame = frame;
			sCamTickFrame = frame;
			sDebug.cameraObject = cam;
			const double pitch = TakePending(sPendingPitch, sPitchAge);
			if (pitch != 0.0 && memory_isAddressRangeAccessible(cam + 0x28, 4))
				memory_writeFloat(cam + 0x28, memory_readFloat(cam + 0x28) + (float)(kGamePitchSign * pitch));
			{
				// drop mouse rotation the camera did not take (the yaw is taken only while the stick is deflected)
				std::lock_guard lock(sCamMutex);
				if (++sYawAge > 2)
					sPendingYaw = 0.0;
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		double Wrap180(double a)
		{
			a = fmod(a + 180.0, 360.0);
			if (a < 0.0)
				a += 360.0;
			return a - 180.0;
		}

		// Other orbit cameras. Climbing (walls, ladders) and conversations have their own camera classes: the chase
		// camera hooks do not run there, and the right stick emulation is limited to the stick's top speed (the camera
		// trails behind the mouse). U-King v1.5.0:
		//   climbing      update 0x02BEE0F8, yaw at +0x98, pitch at +0x8C (clamped to +-70 by the game)
		//   conversation  update 0x02BCB3A0, yaw at +0x48, pitch at +0x40 (clamped to -40..60 by the game)
		// Both keep their angles in degrees (stick right / up lower them) and add the stick to them, so the mouse
		// rotation is added to the angles right before the update runs; the update normalises and clamps them itself.
		std::atomic<uint32> sClimbCamFrame{0xF0000000}, sTalkCamFrame{0xF0000000};

		void OrbitCamTick(PPCInterpreter_t* hCPU, uint32 yawOffset, uint32 pitchOffset)
		{
			const uint32 cam = hCPU->gpr[3];
			sCamTickFrame = sDebug.frameCounter.load();
			const double yaw = TakePending(sPendingYaw, sYawAge);
			const double pitch = TakePending(sPendingPitch, sPitchAge);
			if ((yaw != 0.0 || pitch != 0.0) && memory_isAddressRangeAccessible(cam, 0x100))
			{
				memory_writeFloat(cam + yawOffset, memory_readFloat(cam + yawOffset) + (float)(kGameYawSign * yaw));
				memory_writeFloat(cam + pitchOffset, std::clamp(memory_readFloat(cam + pitchOffset) + (float)(kGamePitchSign * pitch), -89.0f, 89.0f));
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_ClimbCamTick(PPCInterpreter_t* hCPU)
		{
			sClimbCamFrame = sDebug.frameCounter.load();
			OrbitCamTick(hCPU, 0x98, 0x8C);
		}

		// Horse camera (riding), update 0x02BD2A44 (r3 = camera). It keeps yaw / pitch in degrees too (+0x40 / +0x3C, copies
		// at +0x88 / +0x80), but takes them from its pose every frame: the direction of the eye (pose +0x550) from the
		// look-at point (pose +0x55C), pose = [[camera+0x10]]. So the mouse rotation turns the eye around the look-at
		// point before the update. Same conventions as the angles of the other cameras (stick right / up lower them); the
		// game limits the pitch to -40..60. The negligible stick deflection of the direct camera keeps the camera from
		// swinging back behind the horse, as a touch of the stick does (+0x28: time left of manual control).
		std::atomic<uint32> sHorseCamFrame{0xF0000000};
		std::atomic<uint32> sCycleCamFrame{0xF0000000};

		// turns the eye of a camera's pose ([[camera+0x10]]) around its look-at point by the mouse rotation (degrees,
		// right / up positive), the pitch of the eye kept within the limits
		void TurnEye(uint32 cam, double yaw, double pitch, double minPitch, double maxPitch)
		{
			const uint32 holder = memory_isAddressRangeAccessible(cam + 0x10, 4) ? memory_readU32(cam + 0x10) : 0;
			const uint32 pose = holder && memory_isAddressRangeAccessible(holder, 4) ? memory_readU32(holder) : 0;
			if ((yaw == 0.0 && pitch == 0.0) || !pose || !memory_isAddressRangeAccessible(pose + 0x550, 0x18))
				return;
			const double ex = memory_readFloat(pose + 0x550), ey = memory_readFloat(pose + 0x554), ez = memory_readFloat(pose + 0x558);
			const double ax = memory_readFloat(pose + 0x55C), ay = memory_readFloat(pose + 0x560), az = memory_readFloat(pose + 0x564);
			const double dx = ex - ax, dy = ey - ay, dz = ez - az;
			const double dist = sqrt(dx * dx + dy * dy + dz * dz);
			if (!std::isfinite(dist) || dist <= 0.1 || dist >= 100.0)
				return;
			constexpr double kDeg = 3.14159265358979323846 / 180.0;
			const double y = atan2(dx, dz) + kGameYawSign * yaw * kDeg;
			const double p = std::clamp(asin(std::clamp(dy / dist, -1.0, 1.0)) / kDeg + kGamePitchSign * pitch, minPitch, maxPitch) * kDeg;
			memory_writeFloat(pose + 0x550, (float)(ax + dist * cos(p) * sin(y)));
			memory_writeFloat(pose + 0x554, (float)(ay + dist * sin(p)));
			memory_writeFloat(pose + 0x558, (float)(az + dist * cos(p) * cos(y)));
		}

		void HLE_HorseCamTick(PPCInterpreter_t* hCPU)
		{
			const uint32 cam = hCPU->gpr[3];
			sHorseCamFrame = sDebug.frameCounter.load();
			sCamTickFrame = sDebug.frameCounter.load();
			const double yaw = TakePending(sPendingYaw, sYawAge);
			const double pitch = TakePending(sPendingPitch, sPitchAge);
			TurnEye(cam, yaw, pitch, -40.0, 60.0);
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_CycleCamTick(PPCInterpreter_t* hCPU)
		{
			const uint32 cam = hCPU->gpr[3];
			sCycleCamFrame = sDebug.frameCounter.load();
			sCamTickFrame = sDebug.frameCounter.load();
			sDebug.cameraObject = cam;
			const double yaw = TakePending(sPendingYaw, sYawAge);
			const double pitch = TakePending(sPendingPitch, sPitchAge);
			TurnEye(cam, yaw, pitch, -40.0, 60.0);
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_TalkCamTick(PPCInterpreter_t* hCPU)
		{
			sTalkCamFrame = sDebug.frameCounter.load();
			OrbitCamTick(hCPU, 0x48, 0x40);
		}

		// Magnesis with an object held. No camera reads the stick here: the rune's action (update 0x020D72D0) keeps
		// where the object is held around Link: direction at +0xA8 (radians), height at +0xA4 and distance at +0xA0
		// (metres). The stick turns the direction by 60 degrees per second at most, and that was all the mouse could
		// do through stick emulation. The camera (class 0x101BAB4C, update 0x02BF0234) frames Link and the object: it
		// asks where the object is (0x02BD8A30: three copies of the position at camera+0x40 / +0x4C / +0x58), follows
		// its direction with a dead zone of 5 degrees (smoothed direction at camera+0x90, degrees) and eases its eye /
		// look-at point (pose +0x550 / +0x55C) after that.
		// With the mouse the view turns at once and the object swings after it:
		//  - the camera rig (eye, look-at point, smoothed direction) is turned by the mouse rotation around Link
		//    (pose +0x7D4) before the camera's own update, so the view follows the mouse 1:1;
		//  - the action's direction follows the view at a limited speed (kMagnetFollow*). The object itself is always
		//    one frame behind its direction, and the game takes an object that is far behind for one that is stuck:
		//    it pulls it closer and finally lets go of it;
		//  - the position the camera is given is turned by what the object still has to do to reach the view
		//    (MagnetState::lead), so the camera's own smoothing sees nothing to correct and does not pull the view back;
		//  - the game shortens the distance when the object is more than 1 m nearer than it is held at (something is in
		//    its way). It measures that to a smoothed position of the object (time constant about 0.2 s), which cuts
		//    the corner when the object is turned around Link faster than the stick can do it: the distance shrank
		//    with every quick turn and the object was dropped in the end. While the mouse turns the object, the
		//    measurement is not allowed to be shorter than the distance to where the object really is.
		// Up / down moves the object by the same angle as seen from Link, at the same speed (the game limits the height).
		std::atomic<uint32> sMagnetFrame{0xF0000000};
		struct MagnetState // under sCamMutex
		{
			double rigYaw = 0.0;       // degrees the view was turned by the mouse and the camera rig not yet
			double backlogYaw = 0.0;   // degrees the view is ahead of the action's direction (shortest way)
			double backlogPitch = 0.0; // the same for up / down
			double direction = 0.0;    // the action's direction (degrees) as it was left by the last update
			double pending = 0.0;      // change of the view's direction the camera has not been told about yet
			double lead = 0.0;         // degrees the view is ahead of the object, as the camera sees them
			double objectYaw = 0.0;    // direction of the object from Link when the camera last asked
			bool hasDirection = false, hasObject = false;
			sint64 lastQpc = 0;
			sint64 lastTurnQpc = 0;    // when the mouse last turned the action's direction
			uint32 pose = 0;           // pose of the camera in the update that is running
		} sMagnet;

		// turns a point (x, y, z floats) around a vertical axis; angles grow from +z towards +x like the game's atan2(x, z)
		void TurnAround(uint32 point, double cx, double cz, double degrees)
		{
			const double a = degrees * 3.14159265358979 / 180.0, c = cos(a), s = sin(a);
			const double x = memory_readFloat(point) - cx, z = memory_readFloat(point + 8) - cz;
			memory_writeFloat(point, (float)(cx + x * c + z * s));
			memory_writeFloat(point + 8, (float)(cz + z * c - x * s));
		}

		// entry of the action's update, r3 = action
		void HLE_MagnetTick(PPCInterpreter_t* hCPU)
		{
			constexpr double kDegToRad = 3.14159265358979 / 180.0;
			const uint32 action = hCPU->gpr[3];
			const uint32 frame = sDebug.frameCounter.load();
			const bool resumed = frame - sMagnetFrame.exchange(frame) > 2;
			sCamTickFrame = frame;
			const double yaw = kGameYawSign * TakePending(sPendingYaw, sYawAge);
			const double pitch = TakePending(sPendingPitch, sPitchAge);
			if (memory_isAddressRangeAccessible(action, 0xB0))
			{
				std::lock_guard lock(sCamMutex);
				const sint64 now = QpcNow();
				if (resumed)
				{
					sMagnet = {};
					sMagnet.lastQpc = now;
				}
				const double dt = std::clamp((double)(now - sMagnet.lastQpc) / QpcFreq(), 1.0 / 240.0, 1.0 / 20.0);
				sMagnet.lastQpc = now;
				const double distance = std::clamp((double)memory_readFloat(action + 0xA0), 1.0, 100.0);
				// how far the direction may go in this update: an angle, and less for a far object (it moves faster)
				const double followDeg = sDebug.magnetFollowDeg.load(), followMetres = sDebug.magnetFollowMetres.load();
				const double maxStep = std::min(followDeg, followMetres / distance / kDegToRad) * dt;

				// what the stick (or motion control) has turned in the last update belongs to the view as well
				const double before = (double)memory_readFloat(action + 0xA8) / kDegToRad;
				const double stick = sMagnet.hasDirection ? before - sMagnet.direction : 0.0;
				sMagnet.backlogYaw = Wrap180(sMagnet.backlogYaw + yaw);
				const double stepYaw = std::clamp(sMagnet.backlogYaw, -maxStep, maxStep);
				sMagnet.backlogYaw -= stepYaw;
				if (stepYaw != 0.0)
				{
					memory_writeFloat(action + 0xA8, (float)((before + stepYaw) * kDegToRad));
					sMagnet.lastTurnQpc = now;
				}
				sMagnet.direction = (double)memory_readFloat(action + 0xA8) / kDegToRad;
				sMagnet.hasDirection = true;
				sMagnet.pending += yaw + stick;
				sMagnet.rigYaw += yaw;

				sMagnet.backlogPitch = std::clamp(sMagnet.backlogPitch + pitch, -60.0, 60.0);
				const double stepPitch = std::clamp(sMagnet.backlogPitch, -maxStep, maxStep);
				sMagnet.backlogPitch -= stepPitch;
				if (stepPitch != 0.0)
					memory_writeFloat(action + 0xA4, memory_readFloat(action + 0xA4) + (float)(distance * tan(stepPitch * kDegToRad)));
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// start of the camera's update: r3 = pose of the camera (0 if it has none), r4 = camera
		// Lock-on camera (a target held with the lock-on button: an enemy, a person). It is the class of the Magnesis camera
		// (update 0x02BF0234, r3 = camera), called from the camera manager instead of the chase camera, so the mouse went
		// through right stick emulation. Yaw at +0xAC, pitch at +0xA8: the direction of the eye from the look-at point
		// (degrees, stick right / up lower them, they stay where the stick leaves them), and the camera eases its eye towards
		// them over ~8 frames. So both are turned: the angles, and the eye around the look-at point (as on horseback), the
		// easing has nothing left to do. With Magnesis the rune's own handling turns this camera (HLE_MagnetCamTick).
		std::atomic<uint32> sLockCamFrame{0xF0000000};

		void HLE_LockCamTick(PPCInterpreter_t* hCPU)
		{
			const uint32 cam = hCPU->gpr[3];
			const uint32 frame = sDebug.frameCounter.load();
			if (frame - sMagnetFrame.load() <= 2)
			{
				hCPU->instructionPointer = hCPU->spr.LR;
				return;
			}
			sLockCamFrame = frame;
			sCamTickFrame = frame;
			const double yaw = TakePending(sPendingYaw, sYawAge);
			const double pitch = TakePending(sPendingPitch, sPitchAge);
			if ((yaw != 0.0 || pitch != 0.0) && memory_isAddressRangeAccessible(cam, 0xB0))
			{
				memory_writeFloat(cam + 0xAC, (float)Wrap180(memory_readFloat(cam + 0xAC) + kGameYawSign * yaw));
				memory_writeFloat(cam + 0xA8, std::clamp(memory_readFloat(cam + 0xA8) + (float)(kGamePitchSign * pitch), -80.0f, 80.0f));
				TurnEye(cam, yaw, pitch, -80.0, 80.0);
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// Shield surf camera (update 0x02BE8B30, r3 = camera), called from the camera manager instead of the chase camera, so
		// the mouse went through right stick emulation (the view trailed the mouse by ~10 frames). U-King v1.5.0: state byte
		// +0x1C2 is 5 while the stick is deflected; then the yaw is the current direction of the eye from the look-at point
		// plus the stick's turn (0x02BE9698: f1 = f30 + f0, degrees), and the mouse yaw is added there (PCE_SurfCamYaw).
		// The stick's negligible deflection of the direct camera keeps the camera in that state while the mouse moves. When
		// the stick is let go, the camera keeps the yaw as an offset (+0xC0) from Link's smoothed heading (+0xBC) and eases
		// back behind him, as with the GamePad. Pitch at +0xA4 (degrees, stick up lowers it): set from the view when the
		// state changes, so the mouse pitch is added where the update takes it to turn it by the stick (0x02BE8EF8, in
		// state 5 only: the other states do not use that value); the game keeps it within its own soft limits.
		// The sand seal camera (towed on the shield, update 0x02BF5ACC) is the same kind of camera with other offsets:
		// state byte +0x1EA (4 while the stick is deflected), pitch +0xA0 read at 0x02BF5E80, yaw at 0x02BF65E8.
		std::atomic<uint32> sSurfCamFrame{0xF0000000}, sSealCamFrame{0xF0000000};
		struct SurfCamKind
		{
			uint32 stateOffset;
			uint8 manualState; // state while the stick is deflected
		};
		constexpr SurfCamKind kSurfCamKinds[] = {{0x1C2, 5}, {0x1EA, 4}}; // shield surf, sand seal (r4 of PCE_SurfCamPitch)

		void HLE_SurfCamTick(PPCInterpreter_t* hCPU)
		{
			const uint32 frame = sDebug.frameCounter.load();
			sSurfCamFrame = frame;
			sCamTickFrame = frame;
			sDebug.cameraObject = hCPU->gpr[3];
			{
				// drop mouse rotation the camera did not take (it is taken only while the stick is deflected)
				std::lock_guard lock(sCamMutex);
				if (++sYawAge > 2)
					sPendingYaw = 0.0;
				if (++sPitchAge > 2)
					sPendingPitch = 0.0;
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_SealCamTick(PPCInterpreter_t* hCPU)
		{
			sSealCamFrame = sDebug.frameCounter.load();
			HLE_SurfCamTick(hCPU);
		}

		// Waterfall camera (swimming up a waterfall with the Zora armour, update 0x02BF8F88, r3 = camera): it turns only the
		// yaw (the stick's up / down does nothing there), by the stick while mode +0x28 is 2 (set while the stick is not
		// zero): 0x02BF914C, f1 = current yaw (from the eye) + stick turn, degrees, where the mouse yaw is added
		// (PCE_SurfCamYaw). When the stick is let go the camera keeps its direction (mode 0) unless the game asks it to
		// follow Link again (mode 1).
		std::atomic<uint32> sFallCamFrame{0xF0000000};

		void HLE_FallCamTick(PPCInterpreter_t* hCPU)
		{
			sFallCamFrame = sDebug.frameCounter.load();
			HLE_SurfCamTick(hCPU);
			std::lock_guard lock(sCamMutex);
			sPendingPitch = 0.0; // no pitch control on this camera
		}

		void HLE_SurfCamPitch(PPCInterpreter_t* hCPU)
		{
			const uint32 cam = hCPU->gpr[3];
			const SurfCamKind& kind = kSurfCamKinds[hCPU->gpr[4] == 1 ? 1 : 0];
			if (memory_isAddressRangeAccessible(cam + kind.stateOffset, 1) && memory_readU8(cam + kind.stateOffset) == kind.manualState)
			{
				const double pitch = TakePending(sPendingPitch, sPitchAge);
				if (pitch != 0.0)
					hCPU->fpr[1].fpr = (double)(float)std::clamp(hCPU->fpr[1].fpr + kGamePitchSign * pitch, -80.0, 80.0);
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_SurfCamYaw(PPCInterpreter_t* hCPU)
		{
			const double yaw = TakePending(sPendingYaw, sYawAge);
			if (yaw != 0.0)
				hCPU->fpr[1].fpr = (double)(float)(hCPU->fpr[1].fpr + kGameYawSign * yaw);
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_MagnetCamTick(PPCInterpreter_t* hCPU)
		{
			const uint32 pose = hCPU->gpr[3], cam = hCPU->gpr[4];
			const bool active = sDebug.frameCounter.load() - sMagnetFrame.load() <= 2 && pose != 0 && memory_isAddressRangeAccessible(pose + 0x550, 0x290) &&
								memory_isAddressRangeAccessible(cam, 0x94);
			double turn;
			{
				std::lock_guard lock(sCamMutex);
				turn = sMagnet.rigYaw;
				sMagnet.rigYaw = 0.0;
				sMagnet.pose = active ? pose : 0;
			}
			if (active && turn != 0.0)
			{
				const double cx = memory_readFloat(pose + 0x7D4), cz = memory_readFloat(pose + 0x7DC);
				TurnAround(pose + 0x550, cx, cz, turn);
				TurnAround(pose + 0x55C, cx, cz, turn);
				memory_writeFloat(cam + 0x90, (float)Wrap180(memory_readFloat(cam + 0x90) + turn));
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// The action has measured how far the object is from Link (f1, horizontal, to the smoothed position of the object);
		// r3 = Link's position as it was used for that. The object itself: the game's magnet state (0x10467BE0) keeps
		// two positions at +0xB0, the current one is chosen by +0x11C.
		void HLE_MagnetDistance(PPCInterpreter_t* hCPU)
		{
			const uint32 link = hCPU->gpr[3];
			bool turning;
			{
				std::lock_guard lock(sCamMutex);
				turning = sMagnet.lastTurnQpc != 0 && (double)(QpcNow() - sMagnet.lastTurnQpc) / QpcFreq() < 1.0;
			}
			const uint32 state = turning && sDebug.frameCounter.load() - sMagnetFrame.load() <= 2 ? memory_readU32(0x10467BE0) : 0;
			if (state != 0 && memory_isAddressRangeAccessible(state, 0x120) && memory_isAddressRangeAccessible(link, 12))
			{
				const uint32 index = memory_readU32(state + 0x11C);
				const uint32 object = state + 0xB0 + (index < 2 ? index * 0xC : 0);
				const double dx = (double)memory_readFloat(object) - memory_readFloat(link), dz = (double)memory_readFloat(object + 8) - memory_readFloat(link + 8);
				const double real = sqrt(dx * dx + dz * dz);
				if (real > hCPU->fpr[1].fpr && real < 200.0)
					hCPU->fpr[1].fpr = (double)(float)real;
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// the camera has just been given the position of the object: r4 = camera
		void HLE_MagnetTarget(PPCInterpreter_t* hCPU)
		{
			const uint32 cam = hCPU->gpr[4];
			double lead = 0.0, cx = 0.0, cz = 0.0;
			{
				std::lock_guard lock(sCamMutex);
				const uint32 pose = sMagnet.pose;
				if (pose != 0 && memory_isAddressRangeAccessible(cam, 0x64))
				{
					cx = memory_readFloat(pose + 0x7D4);
					cz = memory_readFloat(pose + 0x7DC);
					const double objectYaw = atan2((double)memory_readFloat(cam + 0x58) - cx, (double)memory_readFloat(cam + 0x60) - cz) * 180.0 / 3.14159265358979;
					// what the view has turned since the last time, less what the object has done of it
					sMagnet.lead = Wrap180(sMagnet.lead + sMagnet.pending - (sMagnet.hasObject ? Wrap180(objectYaw - sMagnet.objectYaw) : 0.0));
					sMagnet.pending = 0.0;
					sMagnet.objectYaw = objectYaw;
					sMagnet.hasObject = true;
					if (std::fabs(sMagnet.lead) < 0.001)
						sMagnet.lead = 0.0;
					lead = sMagnet.lead;
				}
			}
			if (lead != 0.0)
			{
				for (const uint32 copy : {cam + 0x40, cam + 0x4C, cam + 0x58})
					TurnAround(copy, cx, cz, lead);
			}
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_AimYaw(PPCInterpreter_t* hCPU)
		{
			hCPU->fpr[1].fpr = (double)(float)(kAimYawSign * TakePending(sPendingYaw, sYawAge));
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		// Aim cameras rotate relative to the current (smoothed) facing, so per-frame deltas lag behind under
		// continuous movement. We keep an absolute target yaw and return target - base every frame.
		double sAimTargetYaw = 0.0;
		uint32 sAimYawFrame = 0xF0000000;

		void HLE_AimYawAbs(PPCInterpreter_t* hCPU)
		{
			const double stickDelta = hCPU->fpr[1].fpr;
			const double base = hCPU->fpr[2].fpr;
			const uint32 frame = sDebug.frameCounter.load();
			if (frame - sAimYawFrame > 2)
				sAimTargetYaw = base; // aim camera (re)started
			sAimYawFrame = frame;
			sAimTargetYaw += stickDelta + kAimYawSign * TakePending(sPendingYaw, sYawAge);
			const double out = Wrap180(sAimTargetYaw - base);
			sAimTargetYaw = base + out;
			hCPU->fpr[1].fpr = (double)(float)out;
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_AimPitch(PPCInterpreter_t* hCPU)
		{
			hCPU->fpr[1].fpr = (double)(float)(kAimPitchSign * TakePending(sPendingPitch, sPitchAge));
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_CamYaw(PPCInterpreter_t* hCPU)
		{
			double add;
			{
				std::lock_guard lock(sCamMutex);
				add = sPendingYaw;
				sPendingYaw = 0.0;
				sYawAge = 0;
			}
			hCPU->fpr[1].fpr = (double)(float)(kGameYawSign * add);
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		void HLE_CamPitch(PPCInterpreter_t* hCPU)
		{
			double add;
			{
				std::lock_guard lock(sCamMutex);
				add = sPendingPitch;
				sPendingPitch = 0.0;
				sPitchAge = 0;
			}
			hCPU->fpr[1].fpr = (double)(float)(kGamePitchSign * add);
			hCPU->instructionPointer = hCPU->spr.LR;
		}

		bool KeyDownNow(Key k)
		{
			if (k == Key::None || k == Key::WheelUp || k == Key::WheelDown)
				return false;
			return sKeyDown[(uint16)k & 0xFF].load();
		}

		// a key as the game sees it: not while it is latched (held since the PC settings had it, see LatchHeldKeys)
		bool KeyDownForGame(Key k)
		{
			if (k == Key::Tab && (KeyDownNow(Key::LAlt) || KeyDownNow(Key::RAlt)))
				return false;
			return KeyDownNow(k) && !sKeyLatched[(uint16)k & 0xFF].load();
		}

		bool ActionDown(const Settings& s, Action a)
		{
			if (a == Action::Inventory && (KeyDownNow(Key::LAlt) || KeyDownNow(Key::RAlt)))
				return false;
			const auto& b = s.bind(a);
			return KeyDownForGame(b.keys[0]) || KeyDownForGame(b.keys[1]);
		}

		bool ActionDownNoMouse(const Settings& s, Action a)
		{
			const auto& b = s.bind(a);
			return (!IsMouseKey(b.keys[0]) && KeyDownForGame(b.keys[0])) || (!IsMouseKey(b.keys[1]) && KeyDownForGame(b.keys[1]));
		}

		// wheel notches routed to an action (positive count)
		sint32 ActionWheel(const Settings& s, Action a, sint32 wheel)
		{
			sint32 n = 0;
			for (Key k : s.bind(a).keys)
			{
				if (k == Key::WheelUp && wheel > 0)
					n += wheel;
				if (k == Key::WheelDown && wheel < 0)
					n += -wheel;
			}
			return n;
		}

		void SetStickButtons(uint32& hold, uint32 lastHold, float x, float y, bool left)
		{
			constexpr float kAxisThreshold = 0.5f;
			constexpr float kHoldAxisThreshold = 0.1f;
			const uint32 L = left ? VPAD_STICK_L_LEFT : VPAD_STICK_R_LEFT;
			const uint32 R = left ? VPAD_STICK_L_RIGHT : VPAD_STICK_R_RIGHT;
			const uint32 U = left ? VPAD_STICK_L_UP : VPAD_STICK_R_UP;
			const uint32 D = left ? VPAD_STICK_L_DOWN : VPAD_STICK_R_DOWN;
			if (x <= -kAxisThreshold || (HAS_FLAG(lastHold, L) && x <= -kHoldAxisThreshold))
				hold |= L;
			else if (x >= kAxisThreshold || (HAS_FLAG(lastHold, R) && x >= kHoldAxisThreshold))
				hold |= R;
			if (y <= -kAxisThreshold || (HAS_FLAG(lastHold, D) && y <= -kHoldAxisThreshold))
				hold |= D;
			else if (y >= kAxisThreshold || (HAS_FLAG(lastHold, U) && y >= kHoldAxisThreshold))
				hold |= U;
		}

		// consume all mouse samples up to now; returns total counts and the timespan covered
		void ConsumeMouse(sint64 now, double& dx, double& dy)
		{
			dx = dy = 0.0;
			std::lock_guard lock(sMouseMutex);
			while (!sMouseSamples.empty() && sMouseSamples.front().t <= now)
			{
				dx += sMouseSamples.front().dx;
				dy += sMouseSamples.front().dy;
				sMouseSamples.pop_front();
			}
		}

		// --- motion (GamePad gyro / accelerometer) ---
		// The virtual GamePad has a pose: pitch and roll away from the neutral pose (lying flat, screen up). The game is
		// not handed hand-made VPAD values. A simulated IMU (the angular rate in the device frame and the direction of
		// gravity for the pose) goes through the sensor fusion Cemu uses for real gyro controllers (WiiUMotionHandler),
		// so the attitude matrix, the accumulated angles (with their winding), the rates and the accelerometer always
		// agree with each other, exactly as with a physical controller. The rate is the one that takes the fusion's own
		// orientation to the pose (closed loop: nothing drifts), with tilt off the values are perfectly static.
		WiiUMotionHandler sMotion;
		std::mutex sMotionInfoMutex;
		std::string sMotionInfo;

		// rotation by an angle (radians, either sign) around a unit axis.
		// (Quaternionf::FromAngleAxis drops the sign of the angle: it rescales the axis to a positive length.)
		Quaternionf AxisRotation(float angle, float x, float y, float z)
		{
			const float s = sinf(angle * 0.5f);
			return Quaternionf(cosf(angle * 0.5f), x * s, y * s, z * s);
		}

		void FillMotion(VPADStatus& st, float pitchDeg, float rollDeg, float dt)
		{
			const float kDegToRad = 3.14159265f / 180.0f;
			// neutral pose of the fusion: GamePad lying flat, screen facing up
			Quaternionf pose(sqrtf(0.5f), sqrtf(0.5f), 0.0f, 0.0f);
			pose *= AxisRotation(pitchDeg * kDegToRad, 1.0f, 0.0f, 0.0f); // pitch: around the GamePad's left-right axis
			pose *= AxisRotation(rollDeg * kDegToRad, 0.0f, 0.0f, 1.0f);  // roll: around the axis pointing at the player
			pose.NormalizeXYZW();

			// where the fusion is now
			float qf[4];
			sMotion.getMotionSample().getQuaternion(qf);
			// rate that turns it into the pose within this sample: the fusion integrates q' = q * (1, rate * dt / 2)
			const Quaternionf d = Quaternionf(qf[0], -qf[1], -qf[2], -qf[3]) * pose;
			float rate[3] = {0.0f, 0.0f, 0.0f};
			dt = std::clamp(dt, 1.0f / 1000.0f, 0.2f);
			if (std::fabs(d.w) > 0.01f)
			{
				const float k = 2.0f / (d.w * dt);
				rate[0] = d.x * k;
				rate[1] = d.y * k;
				rate[2] = d.z * k;
			}
			for (float& r : rate)
			{
				r = std::clamp(r, -60.0f, 60.0f);
				if (std::fabs(r) < 0.0005f)
					r = 0.0f; // settled: exactly static
			}
			const Vector3f grav = pose.GetVectorZ(); // accelerometer reading at rest
			sMotion.processMotionSample(dt, rate[0], rate[1], rate[2], grav.x, grav.y, grav.z);

			// VPAD values, the same way VPADController fills them for a controller with motion sensors
			MotionSample ms = sMotion.getMotionSample();
			float v[3];
			ms.getVPADAccelerometer(v);
			st.acc.x = v[0];
			st.acc.y = v[1];
			st.acc.z = v[2];
			st.accMagnitude = ms.getVPADAccMagnitude();
			st.accAcceleration = ms.getVPADAccAcceleration();
			st.accXY.x = 1.0f;
			st.accXY.y = 0.0f;
			float g[3] = {1.0f, 1.0f, 1.0f};
			ms.getVPADGyroChange(g);
			st.gyroChange.x = g[0];
			st.gyroChange.y = g[1];
			st.gyroChange.z = g[2];
			float o[3];
			ms.getVPADOrientation(o);
			st.gyroOrientation.x = o[0];
			st.gyroOrientation.y = o[1];
			st.gyroOrientation.z = o[2];
			float att[9];
			ms.getVPADAttitudeMatrix(att);
			st.dir.x.x = att[0];
			st.dir.x.y = att[1];
			st.dir.x.z = att[2];
			st.dir.y.x = att[3];
			st.dir.y.y = att[4];
			st.dir.y.z = att[5];
			st.dir.z.x = att[6];
			st.dir.z.y = att[7];
			st.dir.z.z = att[8];
			if (IsDevMode())
			{
				std::lock_guard lock(sMotionInfoMutex);
				sMotionInfo = fmt::format("pose pitch {:.2f} roll {:.2f} | rate {:.4f} {:.4f} {:.4f} | gyro {:.5f} {:.5f} {:.5f} | angle {:.5f} {:.5f} {:.5f} | "
										  "acc {:.3f} {:.3f} {:.3f} | dir [{:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f}]",
										  pitchDeg, rollDeg, rate[0], rate[1], rate[2], g[0], g[1], g[2], o[0], o[1], o[2], v[0], v[1], v[2], att[0], att[1],
										  att[2], att[3], att[4], att[5], att[6], att[7], att[8]);
			}
		}
	}

	void RegisterHLE()
	{
		osLib_addFunction("coreinit", "PCE_CamTick", HLE_CamTick);
		osLib_addFunction("coreinit", "PCE_GlideTick", HLE_GlideTick);
		osLib_addFunction("coreinit", "PCE_ClimbCamTick", HLE_ClimbCamTick);
		osLib_addFunction("coreinit", "PCE_HorseCamTick", HLE_HorseCamTick);
		osLib_addFunction("coreinit", "PCE_CycleCamTick", HLE_CycleCamTick);
		osLib_addFunction("coreinit", "PCE_TalkCamTick", HLE_TalkCamTick);
		osLib_addFunction("coreinit", "PCE_MagnetTick", HLE_MagnetTick);
		osLib_addFunction("coreinit", "PCE_MagnetCamTick", HLE_MagnetCamTick);
		osLib_addFunction("coreinit", "PCE_MagnetTarget", HLE_MagnetTarget);
		osLib_addFunction("coreinit", "PCE_MagnetDistance", HLE_MagnetDistance);
		osLib_addFunction("coreinit", "PCE_CamYaw", HLE_CamYaw);
		osLib_addFunction("coreinit", "PCE_CamPitch", HLE_CamPitch);
		osLib_addFunction("coreinit", "PCE_AimTick", HLE_AimTick);
		osLib_addFunction("coreinit", "PCE_ScopeTick", HLE_ScopeTick);
		osLib_addFunction("coreinit", "PCE_AimYaw", HLE_AimYaw);
		osLib_addFunction("coreinit", "PCE_AimYawAbs", HLE_AimYawAbs);
		osLib_addFunction("coreinit", "PCE_AimPitch", HLE_AimPitch);
		osLib_addFunction("coreinit", "PCE_RuneCamTick", HLE_RuneCamTick);
		osLib_addFunction("coreinit", "PCE_LockCamTick", HLE_LockCamTick);
		osLib_addFunction("coreinit", "PCE_SurfCamTick", HLE_SurfCamTick);
		osLib_addFunction("coreinit", "PCE_SealCamTick", HLE_SealCamTick);
		osLib_addFunction("coreinit", "PCE_FallCamTick", HLE_FallCamTick);
		osLib_addFunction("coreinit", "PCE_SurfCamPitch", HLE_SurfCamPitch);
		osLib_addFunction("coreinit", "PCE_SurfCamYaw", HLE_SurfCamYaw);
	}

	void Start()
	{
		if (sThread.joinable())
			return;
		sQuit = false;
		sThread = std::thread(InputThread);
	}

	void Shutdown()
	{
		if (!sThread.joinable())
			return;
		sQuit = true;
#if BOOST_OS_WINDOWS
		if (DWORD tid = sThreadId.load())
			PostThreadMessageW(tid, WM_QUIT, 0, 0);
#endif
		sThread.join();
	}

	void SetMainWindow(void* hwnd)
	{
		sMainWnd = (HWND)hwnd;
#if !BOOST_OS_WINDOWS
		sFocused = true;
#endif
	}

	void SetWindowFocus(bool focused)
	{
		sFocused = focused;
	}

	void SetClientMousePos(float x, float y, float clientW, float clientH)
	{
		sClientMouseX = x;
		sClientMouseY = y;
		sClientW = clientW;
		sClientH = clientH;
	}

	void HandleMouseMove(sint32 dx, sint32 dy)
	{
		if (dx == 0 && dy == 0)
			return;
		if (!sOverlayOpen.load() && !sEmbedFocus.load() && !sPopupOpen.load())
		{
			std::lock_guard lock(sMouseMutex);
			const sint64 t = QpcNow();
			sMouseSamples.push_back({t, dx, dy});
			if (sMouseSamples.size() > 20000)
				sMouseSamples.pop_front();
		}
	}

	void HandleKey(uint16 vk, bool down)
	{
		SetKey(vk, down);
	}

	void HandleWheel(sint32 delta)
	{
		OnWheel(delta);
	}

	bool IsGameWindowFocused()
	{
		return sFocused.load();
	}

	bool IsKeyDown(Key k)
	{
		return KeyDownNow(k);
	}

	std::optional<Key> PopKeyPress()
	{
		std::lock_guard lock(sPressMutex);
		if (sPresses.empty())
			return std::nullopt;
		Key k = sPresses.front();
		sPresses.pop_front();
		return k;
	}

	void ClearKeyPresses()
	{
		std::lock_guard lock(sPressMutex);
		sPresses.clear();
	}

	int PopOverlayWheel()
	{
		return sWheelOverlay.exchange(0);
	}

	void SetOverlayOpen(bool open)
	{
		sOverlayOpen = open;
		sWheelOverlay = 0;
		sWheelGame = 0;
		ClearKeyPresses();
	}

	bool IsOverlayOpen()
	{
		return sOverlayOpen.load();
	}

	bool WantsCursorCaptured()
	{
		return sFocused.load() && !sOverlayOpen.load() && !sPointerMode.load() && !sPopupOpen.load();
	}

	SystemPanel GetSystemPanel()
	{
		std::lock_guard lock(sPanelMutex);
		return sPanel;
	}

	void SetEmbeddedFocus(bool focus)
	{
		if (sEmbedFocus.exchange(focus) != focus)
		{
			ClearKeyPresses();
			sWheelOverlay = 0;
			LatchHeldKeys(); // (Esc that leaves the settings must not close the menu, Enter must not confirm in it, ...)
		}
	}

	bool EmbeddedFocus()
	{
		return sEmbedFocus.load();
	}

	void SetPanelHover(bool hover)
	{
		sPanelHover = hover;
	}

	bool PanelHover()
	{
		return sPanelHover.load();
	}

	void RequestSystemMenu()
	{
		sSystemMenuRequest = true;
	}

	void PressGameButtons(uint32 mask, int frames)
	{
		sPressMask = mask;
		sPressFrames = frames;
	}

	void SetPopupOpen(bool open)
	{
		if (sPopupOpen.exchange(open) != open)
		{
			ClearKeyPresses();
			sWheelOverlay = 0;
			LatchHeldKeys();
		}
	}

	void LatchHeldKeys()
	{
		for (size_t i = 0; i < sKeyLatched.size(); i++)
			if (sKeyDown[i].load())
				sKeyLatched[i] = true;
	}

	bool GameMenuOpen()
	{
		return sGameMenuOpen.load() || QpcNow() < sEscGraceUntil.load();
	}

	bool IsTiltActive()
	{
		return sTiltActive.load();
	}

	void SetTiltActive(bool active)
	{
		sTiltActive = active;
	}

	void SetButtonRepeat(sint32 delayMs, sint32 pulseMs)
	{
		std::lock_guard lock(sFSMutex);
		sFS.repeatDelay = delayMs;
		sFS.repeatPulse = pulseMs;
	}

	bool GetPlayerPose(float& px, float& py, float& pz, float& camYaw)
	{
		const uint32 cam = sDebug.cameraObject.load();
		if (!cam || !memory_isAddressRangeAccessible(cam + 0x10, 4))
			return false;
		const uint32 holder = memory_readU32(cam + 0x10);
		if (!holder || !memory_isAddressRangeAccessible(holder, 4))
			return false;
		const uint32 pose = memory_readU32(holder);
		if (!pose || !memory_isAddressRangeAccessible(pose + 0x550, 0x290))
			return false;
		px = memory_readFloat(pose + 0x7D4);
		py = memory_readFloat(pose + 0x7D8);
		pz = memory_readFloat(pose + 0x7DC);
		const float ex = memory_readFloat(pose + 0x550);
		const float ez = memory_readFloat(pose + 0x558);
		const float ax = memory_readFloat(pose + 0x55C);
		const float az = memory_readFloat(pose + 0x564);
		const float dx = ax - ex;
		const float dz = az - ez;
		if (dx == 0.0f && dz == 0.0f)
			return false;
		camYaw = atan2f(dx, -dz);
		return true;
	}

	Debug& GetDebug()
	{
		return sDebug;
	}

	namespace
	{
		std::mutex sCallerMutex;
		std::map<std::pair<uint32, uint32>, uint32> sCallers;
	}

	void DebugRawWheel(sint32 delta)
	{
		OnWheel(delta);
	}

	void DebugRawKey(uint16 vk, bool down)
	{
		SetKey(vk, down);
	}

	std::string DebugPictureMap(float x, float y, float clientW, float clientH)
	{
		float ox, oy;
		bool inside;
		MapToPicture(x, y, clientW, clientH, ox, oy, inside);
		int px, py, pw, ph, ww, wh;
		const bool ok = Graphics::GetPictureArea(px, py, pw, ph, ww, wh);
		return fmt::format("{:.4f} {:.4f} inside={} picture={}({},{} {}x{} in {}x{})", ox, oy, inside, ok, px, py, pw, ph, ww, wh);
	}

	std::string CameraAges()
	{
		const uint32 f = sDebug.frameCounter.load();
		return fmt::format("cam {} aim {} scope {} glide {} climbcam {} talkcam {} magnet {} horsecam {} cyclecam {} runecam {} lockcam {} surfcam {} sealcam {} fallcam {}", f - sCamTickFrame.load(),
						   f - sAimTickFrame.load(), f - sScopeTickFrame.load(), f - sGlideTickFrame.load(), f - sClimbCamFrame.load(),
						   f - sTalkCamFrame.load(), f - sMagnetFrame.load(), f - sHorseCamFrame.load(), f - sCycleCamFrame.load(), f - sRuneCamFrame.load(),
						   f - sLockCamFrame.load(), f - sSurfCamFrame.load(), f - sSealCamFrame.load(), f - sFallCamFrame.load());
	}

	void NoteVPADCaller(uint32 lr, uint32 length)
	{
		std::lock_guard lock(sCallerMutex);
		if (sCallers.size() < 64)
			sCallers[{lr, length}]++;
	}

	std::string MotionInfo()
	{
		std::lock_guard lock(sMotionInfoMutex);
		return sMotionInfo;
	}

	std::string VPADCallers()
	{
		std::lock_guard lock(sCallerMutex);
		std::string r;
		for (auto& [k, n] : sCallers)
			r += fmt::format("lr={:08x} len={} calls={}|", k.first, k.second, n);
		sCallers.clear();
		return r;
	}

	namespace
	{
		// everything a Back press changes in a game menu: menu flags, shown screens, buttons and pointer targets
		uint64 MenuFingerprint(const UI::State& ui, bool pointerMode, uint32 frame)
		{
			uint64 h = 1469598103934665603ull;
			auto mix = [&](uint64 v) { h = (h ^ v) * 1099511628211ull; };
			mix((uint64)ui.inventory | (uint64)ui.map << 1 | (uint64)ui.title << 2 | (uint64)ui.dialog << 3 | (uint64)ui.scope << 4 |
				(uint64)ui.anyMenu << 5);
			for (char c : UI::ShownScreens())
				mix((uint8)c);
			for (auto& b : ui.buttons)
			{
				for (char c : b.label)
					mix((uint8)c);
				mix(0xFF);
			}
			if (pointerMode)
			{
				const auto& ps = UI::GetPointer(frame);
				mix(ps.targets.size());
				for (auto& t : ps.targets)
					mix(t.part);
			}
			return h;
		}
	}

	void FillVPAD(VPADStatus& st)
	{
		std::lock_guard lock(sFSMutex);
		FrameState& fs = sFS;
		const sint64 now = QpcNow();
		double dt = fs.initialized ? (double)(now - fs.lastQpc) / QpcFreq() : (1.0 / 30.0);
		fs.lastQpc = now;
		fs.initialized = true;
		dt = std::clamp(dt, 1.0 / 1000.0, 0.25);
		sDebug.frameCounter++;

		if (fs.settingsRev != GetSettingsRevision())
		{
			fs.settings = GetSettings();
			fs.settingsRev = GetSettingsRevision();
			fs.camera.Configure(fs.settings);
		}
		const Settings& s = fs.settings;
		// CPU thread pinning (once the emulation threads all exist, and whenever the setting changes)
		{
			static bool pinnedInit = false;
			static bool lastPinState = false;
			if (sDebug.frameCounter.load() > 120 && (!pinnedInit || lastPinState != s.cpuPinning))
			{
				pinnedInit = true;
				lastPinState = s.cpuPinning;
				cemuLog_log(LogType::Force, "PCE: thread pinning {}: {}", s.cpuPinning ? "on" : "off", Profiler::PinEmulationThreads(s.cpuPinning));
			}
		}

		double mdx, mdy;
		ConsumeMouse(now, mdx, mdy);
		sint32 wheel = sWheelGame.exchange(0);

		for (size_t i = 0; i < sKeyLatched.size(); i++)
			if (sKeyLatched[i].load() && !sKeyDown[i].load())
				sKeyLatched[i] = false;
		const bool inputEnabled = (sFocused.load() || sDebug.forceFocus.load()) && !sOverlayOpen.load() && !sEmbedFocus.load() && !sPopupOpen.load();

		std::array<bool, kActionCount> act{};
		if (inputEnabled)
		{
			for (size_t i = 0; i < kActionCount; i++)
				act[i] = ActionDown(s, (Action)i);
		}
		else
		{
			mdx = mdy = 0.0;
			wheel = 0;
		}
		// ---- game UI state (menus, context guides) and the mouse pointer in menus ----
		const uint32 frameNo = sDebug.frameCounter.load();
		const UI::State& ui = UI::Get(frameNo);
		{
			std::lock_guard panelLock(sPanelMutex);
			sPanel.shown = ui.systemPanel && !ui.dialog;
			sPanel.x0 = ui.systemRect.x0;
			sPanel.y0 = ui.systemRect.y0;
			sPanel.x1 = ui.systemRect.x1;
			sPanel.y1 = ui.systemRect.y1;
		}
		if ((!ui.systemPanel && !ui.map) || ui.dialog)
		{
			if (sEmbedFocus.exchange(false))
				ClearKeyPresses();
			sPanelHover = false;
		}
		UI::SetBoxCursorHidden(sEmbedFocus.load() && ui.systemPanel);
		Menus::FrameIO mio;
		mio.frame = frameNo;
		mio.focused = inputEnabled;
#if BOOST_OS_WINDOWS
		if (HWND main = sMainWnd.load())
		{
			POINT pt;
			RECT rc;
			if (GetCursorPos(&pt) && ScreenToClient(main, &pt) && GetClientRect(main, &rc) && rc.right > 0 && rc.bottom > 0)
				MapToPicture((float)pt.x, (float)pt.y, (float)rc.right, (float)rc.bottom, mio.mouseX, mio.mouseY, mio.mouseInside);
		}
#else
		if (sClientMouseX.load() >= 0 && sClientW.load() > 0 && sClientH.load() > 0)
		{
			MapToPicture(sClientMouseX.load(), sClientMouseY.load(), sClientW.load(), sClientH.load(), mio.mouseX, mio.mouseY, mio.mouseInside);
		}
#endif
		if (sDebug.overrideMouse.load())
		{
			mio.mouseX = sDebug.overrideMouseX.load();
			mio.mouseY = sDebug.overrideMouseY.load();
			mio.mouseInside = true;
		}
		if (sint32 w = sDebug.injectWheel.exchange(0))
			wheel += w;
		if (uint64 ka = sDebug.overrideKeysAction.load())
		{
			for (size_t i = 0; i < std::min<size_t>(64, kActionCount); i++)
				if (ka & (1ull << i))
					act[i] = true;
		}
		{
			const bool panelBlock = ui.map && sPanelHover.load();
			const bool focusOrMap = inputEnabled || (ui.map && (sFocused.load() || sDebug.forceFocus.load()) && !sOverlayOpen.load() && !sPopupOpen.load());
			const bool lmb = !panelBlock && ((focusOrMap && KeyDownNow(Key::MouseLeft)) || sDebug.overrideLmb.load());
			const bool rmb = !panelBlock && ((focusOrMap && KeyDownNow(Key::MouseRight)) || sDebug.overrideRmb.load());
			mio.lmbDown = lmb;
			mio.lmbPressed = lmb && !fs.prevLmb;
			mio.lmbReleased = !lmb && fs.prevLmb;
			mio.rmbPressed = rmb && !fs.prevRmb;
			mio.mmbDown = !panelBlock && focusOrMap && KeyDownNow(Key::MouseMiddle);
			fs.prevLmb = lmb;
			fs.prevRmb = rmb;
		}
		mio.wheel = (ui.map && sPanelHover.load()) ? 0 : wheel;
		Menus::Update(ui, mio);
		sPointerMode = mio.pointerMode;
		const bool gameMenu = mio.pointerMode || ui.scope || ui.cameraRune || ui.anyMenu || ui.inventory || ui.map;
		// Esc acts as Back in game menus; on the title screen's main menu it opens the PC settings
		const bool escIsBack = gameMenu && !(ui.title && !ui.dialog);
		sGameMenuOpen = escIsBack;
		// a menu just closed with Esc: presses during its closing animation must not open the PC menu
		if (fs.prevEscIsBack && !escIsBack && fs.escLastPress != 0 && (double)(now - fs.escLastPress) / QpcFreq() < 1.0)
			sEscGraceUntil = now + (sint64)(QpcFreq() * 0.45);
		fs.prevEscIsBack = escIsBack;
		if (mio.pointerMode != fs.prevPointer || gameMenu != fs.prevMenu)
		{
			cemuLog_log(LogType::Force, "PCE ui: cursor={} menu={} (inv={} map={} title={} dialog={} scope={} anyMenu={} hud={}) screens=[{}]",
						mio.pointerMode, gameMenu, ui.inventory, ui.map, ui.title, ui.dialog, ui.scope, ui.anyMenu, ui.hud,
						UI::ShownScreens());
			fs.prevPointer = mio.pointerMode;
			fs.prevMenu = gameMenu;
		}
		sDebug.lastPointerMode = mio.pointerMode;
		sDebug.lastGameMenu = gameMenu;
		{
			const bool anyMouse = KeyDownNow(Key::MouseLeft) || KeyDownNow(Key::MouseRight) || KeyDownNow(Key::MouseMiddle) ||
								  KeyDownNow(Key::Mouse4) || KeyDownNow(Key::Mouse5);
			if (mio.consumeMouseButtons || !inputEnabled)
				fs.mouseLatched = true; // also: a click that focuses the window does not attack
			else if (fs.mouseLatched && !anyMouse)
				fs.mouseLatched = false;
		}
		if (mio.consumeMouseButtons || fs.mouseLatched)
		{
			// mouse buttons and wheel belong to the pointer in menus (and stay inert until released after a menu closes)
			const uint64 ka = sDebug.overrideKeysAction.load();
			for (size_t i = 0; i < kActionCount; i++)
				act[i] = (inputEnabled && ActionDownNoMouse(s, (Action)i)) || (i < 64 && (ka & (1ull << i)));
			if (mio.consumeMouseButtons)
				wheel = 0;
		}
		if (mio.pointerMode)
			mdx = mdy = 0.0; // the mouse moves the pointer, not the camera

		// developer bridge injection (works without focus)
		mdx += (double)sDebug.injectMouseDX.exchange(0);
		mdy += (double)sDebug.injectMouseDY.exchange(0);

		// minimum attack press (see FrameState::atkExtendUntil)
		{
			// While the game offers an action on the attack button (Flurry Rush after a perfect dodge, Sneakstrike) time
			// runs slowed down and the game only samples the button now and then: a press has to last ~0.35 s to be seen
			// (measured: 250 ms fails, 350 ms works) and clicks in between must not break it into several short presses.
			const bool slowAction = ui.HasButton(UI::Btn::Y);
			const bool phys = act[(size_t)Action::Attack];
			const bool physPressed = phys && !fs.atkPhys;
			if (!phys && fs.atkPhys && fs.atkPressQpc)
				cemuLog_log(LogType::Force, "PCE attack: released after {} ms", (sint64)((double)(now - fs.atkPressQpc) * 1000.0 / QpcFreq()));
			fs.atkPhys = phys;
			bool virt;
			if (slowAction)
			{
				// Flurry Rush window (slow motion): the game ignores presses made during the dodge animation and needs a
				// clear release before a new press, so clicks / holding are turned into a pulse train of 0.3 s pressed /
				// 0.3 s released for as long as the player keeps clicking (measured with perfect dodges on a Bokoblin)
				if (physPressed)
				{
					fs.atkPressQpc = now;
					if (now >= fs.flurryPulseUntil)
						fs.flurryPulseStart = now;
					fs.flurryPulseUntil = now + (sint64)(QpcFreq() * 0.75);
				}
				if (phys)
					fs.flurryPulseUntil = std::max(fs.flurryPulseUntil, now + (sint64)(QpcFreq() * 0.3));
				const double t = (double)(now - fs.flurryPulseStart) / QpcFreq();
				virt = now < fs.flurryPulseUntil && std::fmod(t, 0.6) < 0.3;
				fs.atkExtendUntil = 0;
			}
			else
			{
				fs.flurryPulseUntil = 0;
				if (physPressed)
				{
					fs.atkPressQpc = now;
					if (fs.atkVirtual)
						fs.atkGapFrames = 2; // still held from the previous click: release briefly so this click is a new press
					fs.atkExtendUntil = now + (sint64)(QpcFreq() * 0.12);
				}
				virt = phys || now < fs.atkExtendUntil;
				if (fs.atkGapFrames > 0)
				{
					fs.atkGapFrames--;
					virt = false;
				}
			}
			fs.atkVirtual = virt;
			act[(size_t)Action::Attack] = virt;
		}
		// ---- toggle modes: Sprint and Lock On / Shield stay held after one press (gameplay only) ----
		{
			const bool gameplay = inputEnabled && !mio.pointerMode && !ui.anyMenu && !ui.dialog;
			const bool sprintKey = act[(size_t)Action::Sprint], shieldKey = act[(size_t)Action::Target];
			// while swimming the Sprint key is the swim dash, which must not stay held (it would drain the stamina)
			if (!s.toggleSprint || !gameplay || ui.Has("Dash"))
				fs.sprintLatch = false;
			else
			{
				const bool moving = act[(size_t)Action::MoveForward] || act[(size_t)Action::MoveBack] || act[(size_t)Action::MoveLeft] ||
									act[(size_t)Action::MoveRight];
				if (sprintKey && !fs.prevSprintKey)
				{
					// the same button also cancels / lets go: pressed while sprinting, the game has to see a new press
					if (fs.sprintLatch)
						fs.sprintGapFrames = 2;
					fs.sprintLatch = !fs.sprintLatch;
				}
				if (moving || !fs.sprintLatch)
					fs.sprintStillQpc = 0;
				else if (fs.sprintStillQpc == 0)
					fs.sprintStillQpc = now;
				else if ((double)(now - fs.sprintStillQpc) / QpcFreq() > 0.4)
					fs.sprintLatch = false; // standing still ends the sprint
				if (fs.sprintLatch && moving)
					act[(size_t)Action::Sprint] = true;
			}
			if (fs.sprintGapFrames > 0)
			{
				fs.sprintGapFrames--;
				act[(size_t)Action::Sprint] = false;
			}
			fs.prevSprintKey = sprintKey;
			if (!s.toggleShield || !gameplay)
				fs.shieldLatch = false;
			else
			{
				if (shieldKey && !fs.prevShieldKey)
					fs.shieldLatch = !fs.shieldLatch;
				if (fs.shieldLatch)
					act[(size_t)Action::Target] = true;
			}
			fs.prevShieldKey = shieldKey;
		}
		auto pressed = [&](Action a) { return act[(size_t)a] && !fs.prevAction[(size_t)a]; };
		auto down = [&](Action a) { return act[(size_t)a]; };

		// ---- tilt mode toggle ----
		if (s.tiltToggle)
		{
			if (pressed(Action::TiltMode))
				sTiltActive = !sTiltActive.load();
		}
		else
			sTiltActive = down(Action::TiltMode);
		if (!inputEnabled && !s.tiltToggle)
			sTiltActive = false;
		const bool tilt = sTiltActive.load();

		uint32 hold = 0;
		// ---- buttons ----
		if (inputEnabled || sDebug.overrideButtons.load() || sDebug.overrideKeysAction.load() || sDebug.overrideMouse.load())
		{
			if (down(Action::Sprint) || down(Action::Back)) hold |= VPAD_B;
			if (down(Action::Jump)) hold |= VPAD_X;
			if (down(Action::Crouch)) hold |= VPAD_STICK_L;
			if (down(Action::Attack)) hold |= VPAD_Y;
			if (down(Action::Interact) || down(Action::Confirm)) hold |= VPAD_A;
			if (down(Action::Bow)) hold |= VPAD_ZR;
			if (down(Action::Target)) hold |= VPAD_ZL;
			if (down(Action::UseRune)) hold |= VPAD_L;
			if (down(Action::Throw)) hold |= VPAD_R;
			if (down(Action::Scope)) hold |= VPAD_STICK_R;
			if (down(Action::Whistle) || down(Action::MenuDown)) hold |= VPAD_DOWN;
			if (down(Action::MenuUp)) hold |= VPAD_UP;
			if (down(Action::MenuLeft)) hold |= VPAD_LEFT;
			if (down(Action::MenuRight)) hold |= VPAD_RIGHT;
			if (down(Action::Inventory)) hold |= VPAD_PLUS;
			if (down(Action::Map)) hold |= VPAD_MINUS;
		}
		hold |= sDebug.overrideButtons.load();
		hold |= mio.holdAdd;
		if (sPressFrames.load() > 0)
		{
			hold |= sPressMask.load();
			sPressFrames--;
		}

		// pause menu pages: I always lands on the inventory, J on the adventure log (quests)
		if (inputEnabled || sDebug.overrideKeysAction.load())
		{
			if (pressed(Action::Inventory) && !ui.inventory)
				Menus::RequestPage(Menus::Page::Inventory);
			if (down(Action::Journal) && !ui.inventory)
				hold |= VPAD_PLUS;
			if (pressed(Action::Journal))
			{
				if (!ui.inventory)
					Menus::RequestPage(Menus::Page::AdventureLog);
				else if (Menus::CurrentPage() == Menus::Page::AdventureLog)
					fs.closeMenuFrames = 4; // J again closes the journal
				else
					Menus::RequestPage(Menus::Page::AdventureLog);
			}
		}
		if (fs.closeMenuFrames > 0)
		{
			fs.closeMenuFrames--;
			hold |= VPAD_PLUS;
		}
		// the PC settings key during play: the pause menu opens on its System page, where the PC settings are. Where the
		// pause menu cannot open (title screen, cutscenes, conversations, ...) the PC menu opens instead.
		if (sSystemMenuRequest.exchange(false))
		{
			const bool play = ui.hud && !ui.anyMenu && !ui.dialog && !ui.title && !ui.scope && !ui.map && !ui.inventory &&
							  !mio.pointerMode && !ui.Has("Skip");
			if (play)
			{
				Menus::RequestPage(Menus::Page::System);
				fs.systemMenuFrame = frameNo;
				fs.systemMenuWait = true;
			}
			else
				Overlay::RequestOpen();
		}
		if (fs.systemMenuWait)
		{
			if (ui.inventory)
				fs.systemMenuWait = false;
			else if (frameNo - fs.systemMenuFrame < 4)
				hold |= VPAD_PLUS;
			else if (frameNo - fs.systemMenuFrame > 45)
			{
				fs.systemMenuWait = false;
				Overlay::RequestOpen(); // the pause menu did not come
			}
		}
		if (down(Action::Target) && pressed(Action::Jump))
			fs.lastDodgeQpc = now;
		const bool afterDodge = fs.lastDodgeQpc != 0 && (double)(now - fs.lastDodgeQpc) / QpcFreq() < 3.0;

		// ---- context sensitive controls, driven by the action guides the game itself displays ----
		bool pushPullContext = false;
		if (inputEnabled || sDebug.overrideKeysAction.load() || sDebug.overrideMouse.load())
		{
			std::array<bool, kActionCount> remapped{};
			// remap the buttons an action produces; with latch, the original button stays suppressed until the key is
			// released (closing the scope with RMB must not draw the bow when the scope is gone, ...)
			auto remap = [&](Action a, uint32 remove, uint32 add, bool latch = true) {
				hold = (hold & ~remove) | add;
				remapped[(size_t)a] = true;
				fs.latchSuppress[(size_t)a] = latch ? remove : 0;
			};
			auto btnOr = [&](std::string_view text, uint32 fallback) {
				uint32 b = UI::BtnToVPAD(ui.ButtonFor(text));
				return b ? b : fallback;
			};
			if (ui.Has("Skip") && down(Action::Interact) && !mio.pointerMode)
				remap(Action::Interact, VPAD_A, btnOr("Skip", VPAD_X)); // skip cutscene (Space also works)
			// Esc pressed again while a dialog is still closing (map: "Travel / Cancel"): the game ignores Back for the
			// 0.4 s of that animation, but the press is meant for the menu below, so it is delivered when the dialog is gone
			if (ui.dialog)
				fs.dialogSeenQpc = now;
			fs.dialogGoneFrames = ui.dialog ? 0 : std::min(fs.dialogGoneFrames + 1, 1000);
			if (fs.escPendingUntil)
			{
				// (if the game did take the press itself, the menu is gone within 3 frames and nothing is delivered)
				if (!escIsBack || now > fs.escPendingUntil)
					fs.escPendingUntil = 0;
				else if (fs.dialogGoneFrames >= 2 && frameNo - fs.escPendingFrame >= 5 && !down(Action::Settings))
				{
					hold |= VPAD_B;
					if (++fs.escPendingFrames >= 3)
						fs.escPendingUntil = 0;
				}
			}
			if (fs.escNetCheckAt && now >= fs.escNetCheckAt)
			{
				fs.escNetCheckAt = 0;
				if (escIsBack && MenuFingerprint(ui, mio.pointerMode, frameNo) == fs.escNetPrint)
				{
					cemuLog_log(LogType::Force, "PCE ui: Esc safety net -> PC menu (screens=[{}])", UI::ShownScreens());
					fs.escTimes = {};
					UI::SuppressStuckScreens();
					Overlay::SetOpen(true);
				}
			}
			if (gameMenu)
			{
				// Esc / PC-settings key closes game menus, the map and the scope instead of opening the PC menu
				if (escIsBack && down(Action::Settings))
					hold |= VPAD_B;
				if (escIsBack && pressed(Action::Settings))
				{
					const uint64 print = MenuFingerprint(ui, mio.pointerMode, frameNo);
					// (the flag of the dialog clears a few frames before the game takes input again)
					const bool dialogClosing = ui.dialog || (fs.dialogSeenQpc != 0 && (double)(now - fs.dialogSeenQpc) / QpcFreq() < 0.15);
					if (dialogClosing && fs.escDialogPress != 0 && (double)(now - fs.escDialogPress) / QpcFreq() < 0.7)
					{
						fs.escPendingUntil = now + (sint64)(QpcFreq() * 0.9);
						fs.escPendingFrames = 0;
						fs.escPendingFrame = frameNo;
					}
					fs.escDialogPress = ui.dialog ? now : 0;
					fs.escLastPress = now;
					// safety net: Esc pressed 3 times within 2 s and the menu did not react to any of them (a screen stuck
					// "shown" while the game is running) -> open the PC menu anyway, if it still has not reacted a moment later
					std::rotate(fs.escTimes.begin(), fs.escTimes.begin() + 1, fs.escTimes.end());
					std::rotate(fs.escPrints.begin(), fs.escPrints.begin() + 1, fs.escPrints.end());
					fs.escTimes.back() = now;
					fs.escPrints.back() = print;
					const bool unchanged = fs.escPrints[0] == print && fs.escPrints[1] == print;
					if (fs.escTimes.front() != 0 && (double)(now - fs.escTimes.front()) / QpcFreq() < 2.0 && unchanged)
					{
						fs.escNetCheckAt = now + (sint64)(QpcFreq() * 0.6);
						fs.escNetPrint = print;
					}
					else
						fs.escNetCheckAt = 0;
				}
				if (down(Action::Crouch))
					remap(Action::Crouch, VPAD_STICK_L, VPAD_Y); // menus: Y (sort, ...) on the crouch key
				if (ui.scope)
				{
					if (down(Action::Attack)) remap(Action::Attack, VPAD_Y, VPAD_A);   // place pin
					if (down(Action::Bow)) remap(Action::Bow, VPAD_ZR, VPAD_B);     // quit scope
				}
				if (ui.cameraRune)
				{
					// the keys its prompts show: click = Capture, Esc (above) / right click = Quit, wheel = Zoom (d-pad up /
					// down, held a few frames per notch: 60 frames zoom all the way)
					if (down(Action::Attack)) remap(Action::Attack, VPAD_Y, VPAD_A);
					if (down(Action::Bow)) remap(Action::Bow, VPAD_ZR, VPAD_B);
					if (wheel != 0)
					{
						const uint32 button = wheel > 0 ? VPAD_UP : VPAD_DOWN;
						if (fs.pushPullButton != button)
							fs.pushPullFrames = 0;
						fs.pushPullButton = button;
						fs.pushPullFrames = std::min(60, fs.pushPullFrames + 5 * std::abs(wheel));
					}
				}
			}
			else
			{
				if (ui.Has("Dash") && (down(Action::Sprint) || down(Action::Bow)))
					remap(down(Action::Sprint) ? Action::Sprint : Action::Bow, VPAD_B | VPAD_ZR, btnOr("Dash", VPAD_X), false); // swim dash
				if (ui.Has("Let go"))
				{
					const uint32 letGo = btnOr("Let go", VPAD_B);
					// The interact key lets go of the wall, unless the game offers something on its own interact button
					// (a fruit in reach of a climbed tree: "Take"). That is decided when the key goes down: the offer is
					// gone as soon as the fruit is taken, and the same press must not let go then.
					if (pressed(Action::Interact))
						fs.climbTake = !ui.actionA.empty();
					if (down(Action::Interact) && !fs.climbTake)
						remap(Action::Interact, VPAD_A, letGo);
					if (!ui.Has("Jump") && down(Action::Jump))
						remap(Action::Jump, VPAD_X, letGo);
				}
				// paraglider: the jump key puts it away again (the game's B); the key stays inert until it is released, so
				// the next press in the air opens the paraglider again (the game's own X)
				// (only a new press counts: holding the key from opening the paraglider does not close it)
				// (not with something in the hands, "Throw": there is no paraglider then; a cucco carries Link down in
				// a glide of its own, and a press must stay the game's jump button there)
				if (pressed(Action::Jump) && (frameNo - sGlideTickFrame.load()) <= 2 && !ui.Has("Throw"))
					fs.glideClose = true;
				if (!down(Action::Jump))
					fs.glideClose = false;
				if (!down(Action::Interact))
					fs.climbTake = false;
				if (fs.glideClose)
					remap(Action::Jump, VPAD_X, VPAD_B);
				const bool cancel = ui.Has("Cancel");
				const bool detonate = ui.Has("Detonate");
				const bool throwIt = ui.Has("Throw");
				// materials in the hands: the attack button only lets them fall, the left click places them like the
				// game's "Drop" (an Octo Balloon attaches to what it is placed on), the right click puts them away
				const bool heldItems = ui.HeldItems();
				if (throwIt && down(Action::Attack))
					remap(Action::Attack, VPAD_Y, btnOr("Throw", VPAD_R)); // throw bomb / held object
				else if (heldItems && down(Action::Attack))
					remap(Action::Attack, VPAD_Y, btnOr("Drop", VPAD_A));
				else if (cancel && down(Action::Attack))
					remap(Action::Attack, VPAD_Y, VPAD_A); // rune: activate (stasis, magnesis grab, cryonis, ...)
				if (detonate && down(Action::Bow) && !s.bombKeepsBow)
					remap(Action::Bow, VPAD_ZR, btnOr("Detonate", VPAD_L)); // remote bomb (bombKeepsBow: Use Rune, the game's own)
				else if (cancel && down(Action::Bow))
					remap(Action::Bow, VPAD_ZR, btnOr("Cancel", VPAD_B)); // rune: cancel
				else if (heldItems && down(Action::Bow))
					remap(Action::Bow, VPAD_ZR, btnOr("Put away", VPAD_B));
				// magnesis: wheel pushes / pulls
				// (never with something to throw in the hands: a bomb's Drop must not become the d-pad, Link whistles)
				pushPullContext = (ui.Has("Push") || ui.Has("Pull") || ui.Has("Far") || ui.Has("Near")) && !throwIt;
				if (pushPullContext && wheel != 0)
				{
					fs.pushPullButton = wheel > 0 ? VPAD_UP : VPAD_DOWN;
					fs.pushPullFrames = 6 * std::min(4, std::abs(wheel));
				}
				// shield up + attack key = perfect guard (parry)
				// (not when the game offers an action on the attack button: Flurry Rush after a dodge, Sneakstrike, ...)
				// (and not in the 3 s after a dodge: Flurry Rush needs the attack button)
				if (s.parryWithAttack && down(Action::Target) && down(Action::Attack) && !cancel && !throwIt && !heldItems && !afterDodge &&
					!ui.HasButton(UI::Btn::Y))
					remap(Action::Attack, VPAD_Y, VPAD_A, false);
			}
			if (fs.pushPullFrames > 0)
			{
				fs.pushPullFrames--;
				hold |= fs.pushPullButton;
			}
			for (size_t i = 0; i < kActionCount; i++)
			{
				if (!act[i])
					fs.latchSuppress[i] = 0;
				else if (!remapped[i] && fs.latchSuppress[i])
					hold &= ~fs.latchSuppress[i];
			}
		}

		if (pressed(Action::Attack))
			cemuLog_log(LogType::Force, "PCE attack: buttons={:04x} lockOn={} afterDodge={} yAction={} cancel={} throw={} menu={} cursor={} latched={}",
						hold & 0xFFFF, down(Action::Target), afterDodge, ui.HasButton(UI::Btn::Y), ui.Has("Cancel"), ui.Has("Throw"),
						gameMenu, mio.pointerMode, fs.mouseLatched);

		// ---- left stick ----
		float lx = 0.0f, ly = 0.0f;
		if (down(Action::MoveRight)) lx += 1.0f;
		if (down(Action::MoveLeft)) lx -= 1.0f;
		if (down(Action::MoveForward)) ly += 1.0f;
		if (down(Action::MoveBack)) ly -= 1.0f;
		if (lx != 0.0f && ly != 0.0f)
		{
			lx *= 0.70710678f;
			ly *= 0.70710678f;
		}
		if (down(Action::Walk))
		{
			lx *= s.walkSpeed;
			ly *= s.walkSpeed;
		}
		if (ui.inventory)
		{
			// inventory: A / D switch pages, or move the cursor like W / S do (Settings::inventoryMoveKeys);
			// the pages also have keys of their own
			if (!s.inventoryMoveKeys)
			{
				if (down(Action::MoveLeft)) hold |= VPAD_L;
				if (down(Action::MoveRight)) hold |= VPAD_R;
				lx = 0.0f;
			}
			if (down(Action::MenuPrevPage)) hold |= VPAD_L;
			if (down(Action::MenuNextPage)) hold |= VPAD_R;
		}
		if (mio.overrideLStick)
		{
			lx = mio.lx;
			ly = mio.ly;
		}
		if (sDebug.overrideLeftStick.load())
		{
			lx = sDebug.overrideLX.load();
			ly = sDebug.overrideLY.load();
		}

		// ---- selectors (weapon/bow/shield/rune) ----
		auto& sel = fs.sel;
		auto openSelector = [&](Action src, bool keyDriven) {
			sel = {};
			sel.active = true;
			sel.keyDriven = keyDriven;
			sel.sourceAction = src;
			sel.lastActivityQpc = now;
			switch (src)
			{
			case Action::WeaponSelect: sel.holdButtons = VPAD_RIGHT; break;
			case Action::ShieldSelect: sel.holdButtons = VPAD_LEFT; break;
			case Action::RuneSelect: sel.holdButtons = VPAD_UP; break;
			// (while the aim button is held the game has the bows on d-pad right and the arrows on d-pad left)
			case Action::BowSelect: sel.preButtons = VPAD_ZR; sel.holdButtons = VPAD_RIGHT; break;
			case Action::WeaponNext: // arrows while the player aims, weapons otherwise
				sel.holdButtons = down(Action::Bow) ? VPAD_LEFT : VPAD_RIGHT;
				break;
			default: break;
			}
		};
		const bool selectorsAllowed = inputEnabled && !tilt && !gameMenu && !pushPullContext;
		if (!sel.active && selectorsAllowed)
		{
			if (pressed(Action::WeaponSelect)) openSelector(Action::WeaponSelect, true);
			else if (pressed(Action::BowSelect)) openSelector(Action::BowSelect, true);
			else if (pressed(Action::ShieldSelect)) openSelector(Action::ShieldSelect, true);
			else if (pressed(Action::RuneSelect)) openSelector(Action::RuneSelect, true);
		}
		const sint32 wheelNext = ActionWheel(s, Action::WeaponNext, wheel) + (pressed(Action::WeaponNext) ? 1 : 0);
		const sint32 wheelPrev = ActionWheel(s, Action::WeaponPrev, wheel) + (pressed(Action::WeaponPrev) ? 1 : 0);
		// A selector of ours that is open counts as a menu for the game, also for a moment after it has closed: the steps
		// that follow the one which opened it are taken all the same.
		const bool ownSelector = sel.active ? sel.closeFrame < 0 : (fs.selClosedQpc != 0 && (double)(now - fs.selClosedQpc) / QpcFreq() < 0.25);
		const bool selectorSteps = selectorsAllowed || (ownSelector && inputEnabled && !tilt && !pushPullContext && !mio.pointerMode &&
														!ui.scope && !ui.inventory && !ui.map);
		if ((wheelNext || wheelPrev) && selectorSteps)
		{
			if (!sel.active)
				openSelector(Action::WeaponNext, false);
			sel.pendingSteps += wheelNext - wheelPrev;
			sel.lastActivityQpc = now;
		}
		float selStickX = 0.0f;
		bool selectorOwnsStick = false;
		if (sel.active && sel.closeFrame >= 0)
		{
			// The bow selector holds the aim button for the game, and Link has nocked an arrow meanwhile (at once when
			// the bow was already in his hands): letting go of the button would shoot it. The d-pad is released first
			// (the selection is equipped), then the shot is cancelled with the game's cancel button: once the game's menu
			// has gone (a press while it closes is not taken, and on horseback the bow then fires), held for a few frames.
			selectorOwnsStick = true;
			hold |= sel.preButtons;
			if (sel.cancelFrame < 0 && sel.closeFrame >= 4 && (!ui.anyMenu || sel.closeFrame >= 30))
			{
				if (sel.closeEmpty)
					sel.active = false; // nothing drawn to cancel (on horseback the cancel button would dismount)
				else
					sel.cancelFrame = 0;
			}
			sel.closeFrame++;
			if (sel.active && sel.cancelFrame >= 0)
			{
				hold |= VPAD_B;
				if (++sel.cancelFrame >= 10)
					sel.active = false; // (aim and cancel are let go of together)
			}
		}
		else if (sel.active)
		{
			selectorOwnsStick = true;
			constexpr sint32 kPreFrames = 6;   // let ZR register (bow drawn) before pressing the d-pad
			constexpr sint32 kOpenFrames = 4;  // d-pad held before first flick so the carousel is shown
			constexpr sint32 kFlickOn = 2, kFlickOff = 2;
			const sint32 pre = sel.preButtons ? kPreFrames : 0;
			hold |= sel.preButtons;
			if (sel.frame >= pre)
				hold |= sel.holdButtons;
			if (s.selectorMoveKeys)
			{
				// the movement keys step through the selector (one step per press, repeating when held) and do not
				// move Link meanwhile
				const sint32 dir = (down(Action::MoveRight) ? 1 : 0) - (down(Action::MoveLeft) ? 1 : 0);
				bool step = false;
				if (dir != sel.keyDir)
				{
					sel.keyDir = dir;
					sel.keyFrames = 0;
					step = dir != 0;
				}
				else if (dir != 0 && ++sel.keyFrames >= 18 && (sel.keyFrames - 18) % 6 == 0)
					step = true;
				if (step)
				{
					sel.pendingSteps += dir;
					sel.lastActivityQpc = now;
				}
				lx = ly = 0.0f;
			}
			if (sel.keyDriven)
			{
				// mouse X scrolls the selection while the key is held
				sel.mouseAccum += mdx;
				const double step = std::max(1.0f, s.selectorMouseStep);
				while (sel.mouseAccum >= step) { sel.pendingSteps++; sel.mouseAccum -= step; }
				while (sel.mouseAccum <= -step) { sel.pendingSteps--; sel.mouseAccum += step; }
			}
			// emit flicks
			if (sel.frame >= pre + kOpenFrames)
			{
				if (sel.flickDir == 0 && sel.pendingSteps != 0)
				{
					sel.flickDir = sel.pendingSteps > 0 ? 1 : -1;
					sel.pendingSteps -= sel.flickDir;
					sel.flickFrame = 0;
				}
				if (sel.flickDir != 0)
				{
					if (sel.flickFrame < kFlickOn)
						selStickX = (float)sel.flickDir;
					sel.flickFrame++;
					if (sel.flickFrame >= kFlickOn + kFlickOff)
						sel.flickDir = 0;
				}
			}
			if (ui.shortcut && !sel.shown)
			{
				sel.shown = true;
				sel.openedEmpty = ui.shortcutName.empty() || ui.shortcutName == "Remove";
			}
			sel.frame++;
			// closing conditions
			bool wantClose;
			if (sel.keyDriven)
				wantClose = !down(sel.sourceAction) || !inputEnabled;
			else
				wantClose = ((double)(now - sel.lastActivityQpc) / QpcFreq()) * 1000.0 > s.wheelReleaseDelayMs || !inputEnabled;
			// the bow selector stays until the game has shown it (after a tap too): what it opened on tells whether the
			// aim button drew a bow
			if (sel.preButtons && !sel.shown && sel.frame < 40 && inputEnabled)
				wantClose = false;
			if (wantClose && sel.flickDir == 0 && sel.pendingSteps == 0)
			{
				// releasing the d-pad equips the highlighted item
				if (sel.preButtons && !down(Action::Bow) && inputEnabled)
					{
						sel.closeFrame = 0; // (not when the player aims himself: his own button keeps the bow drawn)
						sel.closeEmpty = sel.openedEmpty || (ui.shortcut && (ui.shortcutName.empty() || ui.shortcutName == "Remove"));
						cemuLog_log(LogType::Force, "PCE bow selector closes on '{}' (shown {}, opened empty {}): {}", ui.shortcutName, ui.shortcut, sel.openedEmpty, sel.closeEmpty ? "nothing to cancel" : "cancel");
					}
				else
				{
					sel.active = false;
					fs.selClosedQpc = now;
				}
			}
		}

		// ---- right stick (camera) ----
		float rx = 0.0f, ry = 0.0f;
		// "Aiming Sensitivity" is for the bow and the scope (the zoomed camera). Aiming a rune (Magnesis, Stasis,
		// Cryonis) is not zoomed and keeps the normal sensitivity.
		const bool scopeCamera = (sDebug.frameCounter.load() - sScopeTickFrame.load()) <= 2;
		const bool aiming = scopeCamera || down(Action::Bow) || down(Action::Scope);
		if (tilt)
		{
			// mouse drives the GamePad tilt
			const float tsens = 0.05f * s.tiltSensitivity;
			fs.tiltRoll += (float)mdx * tsens * (s.tiltInvertX ? -1.0f : 1.0f);
			fs.tiltPitch += (float)mdy * tsens * (s.tiltInvertY ? -1.0f : 1.0f);
			if (s.tiltReturnSpeed > 0.0f)
			{
				auto toward0 = [&](float v) { float d = s.tiltReturnSpeed * (float)dt; return v > 0 ? std::max(0.0f, v - d) : std::min(0.0f, v + d); };
				fs.tiltRoll = toward0(fs.tiltRoll);
				fs.tiltPitch = toward0(fs.tiltPitch);
			}
			fs.tiltRoll = std::clamp(fs.tiltRoll, -s.tiltMaxAngle, s.tiltMaxAngle);
			fs.tiltPitch = std::clamp(fs.tiltPitch, -s.tiltMaxAngle, s.tiltMaxAngle);
			fs.camera.Reset();
		}
		else
		{
			// tilt off: level the slate smoothly (no rotation rate is reported, so motion aiming never jumps);
			// once level the motion data is perfectly static
			auto level = [&](float v) { const float step = 400.0f * (float)dt; return v > 0 ? std::max(0.0f, v - step) : std::min(0.0f, v + step); };
			fs.tiltRoll = level(fs.tiltRoll);
			fs.tiltPitch = level(fs.tiltPitch);
			if (selectorOwnsStick)
			{
				fs.camera.Reset();
			}
			else
			{
				float kx = 0.0f, ky = 0.0f;
				if (down(Action::CameraRight)) kx += 1.0f;
				if (down(Action::CameraLeft)) kx -= 1.0f;
				if (down(Action::CameraUp)) ky += 1.0f;
				if (down(Action::CameraDown)) ky -= 1.0f;
				kx *= s.keyCameraSpeed;
				ky *= s.keyCameraSpeed;
				double yawDeg, pitchDeg;
				MouseToDegrees(s, mdx, mdy, aiming, yawDeg, pitchDeg);
				const uint32 frameNow = sDebug.frameCounter.load();
				const bool directCamera = (frameNow - sCamTickFrame.load()) <= 2;
				if (directCamera)
				{
					// main camera: exact degrees are injected into the camera code (see HLE_Cam*), the stick only
					// carries a negligible deflection that keeps the camera in manual-control mode
					fs.camera.Reset();
					double py, px;
					{
						std::lock_guard camLock(sCamMutex);
						sPendingYaw = std::clamp(sPendingYaw + yawDeg, -90.0, 90.0);
						sPendingPitch = std::clamp(sPendingPitch + pitchDeg, -90.0, 90.0);
						// fresh input: the camera gets two updates to consume it before it is considered stale
						if (yawDeg != 0.0)
							sYawAge = 0;
						if (pitchDeg != 0.0)
							sPitchAge = 0;
						px = sPendingYaw;
						py = sPendingPitch;
					}
					if (px != 0.0 || py != 0.0)
					{
						const double len = sqrt(px * px + py * py);
						fs.manualDirX = (float)(px / len);
						fs.manualDirY = (float)(py / len);
						fs.manualHoldUntil = now + (sint64)(QpcFreq() * 0.12);
					}
					if (now < fs.manualHoldUntil)
					{
						constexpr float kManualMagnitude = 0.0062f; // just above the game's 0.005 deadzone
						rx = fs.manualDirX * kManualMagnitude;
						ry = fs.manualDirY * kManualMagnitude;
					}
					rx += kx;
					ry += ky;
				}
				else
				{
					// other camera modes: drive the stick with the exact inverse of the game's response curve
					double pyaw = 0.0, ppitch = 0.0;
					{
						std::lock_guard camLock(sCamMutex);
						std::swap(pyaw, sPendingYaw);
						std::swap(ppitch, sPendingPitch);
					}
					if (!aiming && (!ui.hud || ui.anyMenu || ui.Has("Skip")))
					{
						// cutscene / event camera: the game ignores the stick, so nothing may pile up and be
						// replayed when the player camera returns
						fs.camera.Reset();
						yawDeg = pitchDeg = pyaw = ppitch = 0.0;
					}
					fs.camera.Update(yawDeg + pyaw, pitchDeg + ppitch, dt, kx, ky, rx, ry);
				}
				sDebug.lastDesiredYawDeg = (float)yawDeg;
				sDebug.lastDesiredPitchDeg = (float)pitchDeg;
			}
		}
		if (selectorOwnsStick)
			rx = selStickX;
		if (mio.overrideRStick)
		{
			rx = mio.rx;
			ry = mio.ry;
		}
		if (sDebug.overrideRightStick.load())
		{
			rx = sDebug.overrideRX.load();
			ry = sDebug.overrideRY.load();
		}
		// latency probe (dev bridge): time of the first VPAD read that turns the camera
		if (g_pceProbeArmed.load() && g_pceProbeInputTick.load() == 0 && std::fabs(rx) > 0.5f)
		{
			g_pceProbeInputTick = (uint64)HighResolutionTimer::now().getTick();
		}
		sDebug.lastStickRX = rx;
		sDebug.lastStickRY = ry;

		st.leftStick.x = lx;
		st.leftStick.y = ly;
		st.rightStick.x = rx;
		st.rightStick.y = ry;
		SetStickButtons(hold, fs.lastHold, lx, ly, true);
		SetStickButtons(hold, fs.lastHold, std::clamp(rx, -1.0f, 1.0f), std::clamp(ry, -1.0f, 1.0f), false);

		// ---- button repeat (same semantics as VPADController) ----
		const auto tnow = std::chrono::steady_clock::now();
		uint32 currentHold = hold;
		if (currentHold != fs.lastHold)
			fs.lastHoldChange = fs.lastPulse = tnow;
		if (fs.repeatPulse > 0 && currentHold != 0 && fs.lastHoldChange + std::chrono::milliseconds(fs.repeatDelay) <= tnow)
		{
			if (fs.lastPulse + std::chrono::milliseconds(fs.repeatPulse) < tnow)
			{
				fs.lastPulse = tnow;
				hold |= VPAD_REPEAT;
			}
		}
		st.hold = hold;
		sDebug.lastHold = hold;
		st.release = fs.lastHold & ~currentHold;
		st.trig = ~fs.lastHold & currentHold;
		fs.lastHold = currentHold;

		// ---- touch: none ----
		st.tpData.touch = 0;
		st.tpData.validity = 3;
		st.tpProcessed1 = st.tpData;
		st.tpProcessed2 = st.tpData;

		// ---- motion ----
		if (sDebug.overrideTilt.load())
		{
			fs.tiltPitch = sDebug.overrideTiltPitch.load();
			fs.tiltRoll = sDebug.overrideTiltRoll.load();
		}
		FillMotion(st, fs.tiltPitch, fs.tiltRoll, (float)dt);

		fs.prevAction = act;
	}

	void DebugRawMouse(sint32 dx, sint32 dy)
	{
		std::lock_guard lock(sMouseMutex);
		const sint64 t = QpcNow();
		sMouseSamples.push_back({t, dx, dy});
	}
}
