#include "pcedition/PCOverlay.h"
#include "pcedition/PCSettings.h"
#include "pcedition/PCInput.h"
#include "pcedition/PCGraphics.h"
#include "pcedition/PCEdition.h"
#include "pcedition/PCMenus.h"
#include "pcedition/PCUI.h"
#include "pcedition/PCKeys.h"
#include "pcedition/PCPrompts.h"
#include "pcedition/PCMapIcons.h"

#if BOOST_OS_WINDOWS
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#pragma comment(lib, "comdlg32.lib")
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif
#include <mutex>
#include <deque>
#include <set>
#include <unordered_set>
#include <boost/algorithm/string.hpp>
#include <fstream>
#include <thread>

#include "imgui.h"
#include "imgui_internal.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "gui/interface/WindowSystem.h"
#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/OS/libs/nn_nfp/nn_nfp.h"
#include "audio/IAudioAPI.h"
#include "config/CemuConfig.h"

void LatteFrameGen_GetStats(float& baseFps, float& shownFps, bool& generating); // LatteRenderTarget.cpp

namespace pce::Overlay
{
	namespace
	{
		std::atomic<bool> sOpen{false};
		std::atomic<bool> sOpenRequest{false}; // RequestOpen (game thread) -> opened when the next frame is drawn
		std::atomic<float> sDebugMouseX{-1.0f}, sDebugMouseY{0.0f};
		std::atomic<bool> sDebugMouseDown{false};
		bool sEmbedShown = false;              // the settings were drawn in the System page last frame
		bool sPagesEmbedded = false;           // sPages were built for the System page (no Resume / Save / Load)
		float sAnim = 0.0f;
		double sLastTime = 0.0;

		// --- fonts ---
		std::vector<uint8> sFontData;
		struct FontSet
		{
			int height = 0;
			ImFont* title = nullptr;
			ImFont* item = nullptr;
			ImFont* small = nullptr;
			ImFont* key = nullptr;   // keycap labels of HUD prompts (bold, large: scaled down when drawn)
		} sFonts;
		std::vector<uint8> sKeyFontData;
		int sRequestedHeight = 0;

		// --- style (BotW inspired palette) ---
		const ImU32 kText = IM_COL32(237, 232, 208, 255);
		const ImU32 kTextDim = IM_COL32(160, 155, 140, 255);
		// accent: the yellow of the game's own menus (place names in the Adventure Log, ...)
		const ImU32 kAccent = IM_COL32(248, 200, 0, 255);
		const ImU32 kAccentDim = IM_COL32(248, 200, 0, 70);
		const ImU32 kWarn = IM_COL32(255, 120, 90, 255);
		const ImU32 kPanel = IM_COL32(8, 10, 12, 215);
		const ImU32 kBorder = IM_COL32(232, 225, 200, 150);

		enum class ItemType
		{
			Slider,
			Toggle,
			Choice,
			Binding,
			Button,
			Header
		};

		struct Item
		{
			ItemType type;
			std::string label;
			std::string help;
			float* fval = nullptr;
			float fmin = 0, fmax = 1, fstep = 0.1f;
			const char* fmt = "%.2f";
			bool* bval = nullptr;
			int* ival = nullptr;
			std::vector<std::string> options;
			Action action = Action::Count;
			std::function<void()> onPress;
			bool restart = false;
			bool rebuild = false; // other items depend on this one: the pages are built again after a change
		};

		struct Page
		{
			std::string name;
			std::vector<Item> items;
		};

		Settings sEdit; // working copy
		std::vector<std::string> sModIds;
		std::deque<bool> sModOn; // one per listed mod (deque: stable addresses)
		// one per preset list shown on the Mods page (for the packs that are on)
		struct ModPresetRow
		{
			std::string id, category;
			std::vector<std::string> names;
			int shown = 0; // index the row was built with
		};
		std::vector<ModPresetRow> sModPresetRows;
		std::deque<int> sModPresetIdx;
		int sPage = 0;
		int sSel = 0;
		float sScroll = 0.0f;        // shown scroll offset of the list (eases towards the target)
		float sScrollTarget = 0.0f;
		bool sEnsureVisible = false; // the selection was moved with the keyboard: scroll it into view
		bool sRebinding = false;
		int sRebindSlot = 0;
		Action sRebindAction = Action::Count;
		double sRebindStart = 0.0;
		std::string sToast;
		double sToastTime = 0.0;
		bool sConfirmQuit = false;
		bool sMouseWasDown = false;
		int sFpsIndex = 1;
		int sLanguageIndex = 0; // into sLanguages
		// audio output devices of Cemu's audio API: [0] is the system's default (empty identifier)
		std::vector<std::string> sAudioDeviceIds, sAudioDeviceNames;
		int sAudioDeviceIndex = 0;

		void LoadAudioDevices()
		{
			sAudioDeviceIds = {""};
			sAudioDeviceNames = {"Default"};
			const auto api = (IAudioAPI::AudioAPI)GetConfig().audio_api;
			if (IAudioAPI::IsAudioAPIAvailable(api))
			{
				for (const auto& d : IAudioAPI::GetDevices(api))
				{
					const std::wstring id = d->GetIdentifier();
					if (id.empty() || id == L"default")
						continue;
					sAudioDeviceIds.push_back(boost::nowide::narrow(id));
					sAudioDeviceNames.push_back(boost::nowide::narrow(d->GetName()));
				}
			}
			sAudioDeviceIndex = 0;
			for (int i = 0; i < (int)sAudioDeviceIds.size(); i++)
				if (sAudioDeviceIds[i] == sEdit.audioDevice)
					sAudioDeviceIndex = i;
			if (sAudioDeviceIndex == 0 && !sEdit.audioDevice.empty())
			{
				// a device that is not connected now: keep it
				sAudioDeviceIds.push_back(sEdit.audioDevice);
				sAudioDeviceNames.push_back("(not connected)");
				sAudioDeviceIndex = (int)sAudioDeviceIds.size() - 1;
			}
		}

		// the volume of the game's sound, at once
		void ApplyAudioLive(const Settings& s)
		{
			const int volume = std::clamp((int)std::lround(s.volume), 0, 100);
			GetConfig().tv_volume = volume;
			std::shared_lock lock(g_audioMutex);
			if (g_tvAudio)
				g_tvAudio->SetVolume(volume);
		}
		std::vector<GameLanguage> sLanguages;
		const int kFpsValues[] = {30, 60, 90, 120, 144, 165, 240};

		double Now()
		{
#if BOOST_OS_WINDOWS
			return (double)GetTickCount64() / 1000.0;
#else
			using namespace std::chrono;
			return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
#endif
		}

		void Toast(const std::string& t)
		{
			sToast = t;
			sToastTime = Now();
		}

		void Commit()
		{
			if (!sModIds.empty())
			{
				std::set<std::string> mods;
				std::vector<std::string> oldParts;
				boost::split(oldParts, sEdit.userGraphicPacks, boost::is_any_of(";"));
				for (const auto& part : oldParts)
				{
					if (!part.empty())
						mods.insert(part);
				}
				for (size_t i = 0; i < sModIds.size() && i < sModOn.size(); i++)
				{
					if (sModOn[i])
						mods.insert(sModIds[i]);
					else
						mods.erase(sModIds[i]);
				}
				sEdit.userGraphicPacks = boost::join(mods, ";");
			}
			if (!sModPresetRows.empty())
			{
				// presets chosen on the Mods page: kept in the settings, set on the loaded pack (its other preset lists follow)
				Graphics::PackPresets presets = Graphics::ParsePackPresets(sEdit.userPackPresets);
				for (size_t j = 0; j < sModPresetRows.size() && j < sModPresetIdx.size(); j++)
				{
					ModPresetRow& row = sModPresetRows[j];
					const int idx = std::clamp(sModPresetIdx[j], 0, (int)row.names.size() - 1);
					if (idx == row.shown)
						continue;
					row.shown = idx;
					presets[{row.id, row.category}] = row.names[idx];
					Graphics::SetUserPackPreset(row.id, row.category, row.names[idx]);
				}
				sEdit.userPackPresets = Graphics::JoinPackPresets(presets);
			}
			if (!sAudioDeviceIds.empty())
				sEdit.audioDevice = sAudioDeviceIds[std::clamp(sAudioDeviceIndex, 0, (int)sAudioDeviceIds.size() - 1)];
			const Settings old = GetSettings();
			Settings copy = sEdit;
			UpdateSettings([&](Settings& s) { s = copy; });
			const bool gfxChanged = old.fpsLimit != copy.fpsLimit || old.vsync != copy.vsync ||
			                        old.antiAliasing != copy.antiAliasing || old.resolutionPreset != copy.resolutionPreset ||
			                        old.shadowPreset != copy.shadowPreset || old.aspectRatio != copy.aspectRatio ||
			                        old.ultrawideHud != copy.ultrawideHud || old.frameGeneration != copy.frameGeneration;
			if (gfxChanged)
				Graphics::ApplyLive();
			if (old.volume != copy.volume || old.speakers != copy.speakers || old.audioDevice != copy.audioDevice)
				ApplyAudioLive(copy);
			if (old.userGraphicPacks != copy.userGraphicPacks || old.userPackPresets != copy.userPackPresets)
				Graphics::ApplyModsLive();
		}

		// ---- amiibo: files of the player's amiibo (dumps of their figures) in <data>/amiibo; when the game looks for an
		// amiibo (the amiibo rune), one of them is chosen in a popup and put on Cemu's emulated NFC reader ----
		bool sAmiiboOpen = false;      // the popup is shown
		bool sAmiiboDismissed = false; // closed by the player while the game still looks: not again until it stops looking
		int sAmiiboSel = 0;
		std::vector<fs::path> sAmiiboFiles;
		std::string sAmiiboError;
		std::mutex sAmiiboMutex;
		std::atomic<bool> sAmiiboDialog{false}; // the file dialog is open (on its own thread)
		fs::path sAmiiboAdded;                  // what the dialog added (under sAmiiboMutex)
		std::string sAmiiboAddError;

		fs::path AmiiboDir()
		{
			return GetDataDir() / "amiibo";
		}

		void RefreshAmiiboFiles()
		{
			sAmiiboFiles.clear();
			std::error_code ec;
			fs::create_directories(AmiiboDir(), ec);
			for (const auto& e : fs::directory_iterator(AmiiboDir(), ec))
			{
				if (!e.is_regular_file(ec))
					continue;
				std::string ext = _pathToUtf8(e.path().extension());
				std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
				if (ext == ".bin" || ext == ".nfc" || ext == ".dat")
					sAmiiboFiles.push_back(e.path());
			}
			std::sort(sAmiiboFiles.begin(), sAmiiboFiles.end());
		}

		void OpenAmiiboFolder();

		// Windows' file dialog on a thread of its own (the game keeps running); the chosen file is copied into the folder
		void AddAmiiboFile()
		{
#if BOOST_OS_WINDOWS
			if (sAmiiboDialog.exchange(true))
				return;
			HWND owner = (HWND)WindowSystem::GetWindowInfo().window_main.surface;
			std::thread([owner]() {
				wchar_t path[2048] = {};
				OPENFILENAMEW ofn{};
				ofn.lStructSize = sizeof(ofn);
				ofn.hwndOwner = owner;
				ofn.lpstrFilter = L"amiibo files (*.bin, *.nfc, *.dat)\0*.bin;*.nfc;*.dat\0All files\0*.*\0";
				ofn.lpstrFile = path;
				ofn.nMaxFile = (DWORD)std::size(path);
				ofn.lpstrTitle = L"Add an amiibo file";
				ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
				if (GetOpenFileNameW(&ofn))
				{
					std::error_code ec;
					const fs::path src(path);
					fs::create_directories(AmiiboDir(), ec);
					const fs::path dst = AmiiboDir() / src.filename();
					if (!fs::equivalent(src, dst, ec))
						fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
					std::lock_guard lock(sAmiiboMutex);
					sAmiiboAdded = dst;
					sAmiiboAddError = ec ? "The file could not be copied." : "";
				}
				sAmiiboDialog = false;
			}).detach();
#else
			OpenAmiiboFolder();
#endif
		}

		void OpenAmiiboFolder()
		{
			std::error_code ec;
			fs::create_directories(AmiiboDir(), ec);
#if BOOST_OS_WINDOWS
			ShellExecuteW(nullptr, L"open", AmiiboDir().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
			// xdg-open started directly (no shell: the path is passed as it is), reaped on a thread of its own
			const std::string dir = AmiiboDir().string();
			char* argv[] = {(char*)"xdg-open", (char*)dir.c_str(), nullptr};
			pid_t pid = 0;
			if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0)
				std::thread([pid]() { int status = 0; waitpid(pid, &status, 0); }).detach();
#endif
		}

		std::vector<Page> BuildPages(bool embedded = false)
		{
			std::vector<Page> pages;
			auto slider = [](const char* label, float* v, float mn, float mx, float step, const char* fmt, const char* help) {
				Item it{ItemType::Slider, label, help};
				it.fval = v;
				it.fmin = mn;
				it.fmax = mx;
				it.fstep = step;
				it.fmt = fmt;
				return it;
			};
			auto toggle = [](const char* label, bool* v, const char* help) {
				Item it{ItemType::Toggle, label, help};
				it.bval = v;
				return it;
			};
			auto choice = [](const char* label, int* v, std::vector<std::string> opts, const char* help, bool restart = false) {
				Item it{ItemType::Choice, label, help};
				it.ival = v;
				it.options = std::move(opts);
				it.restart = restart;
				return it;
			};
			auto header = [](const char* label) { return Item{ItemType::Header, label, ""}; };
			auto button = [](const char* label, const char* help, std::function<void()> fn) {
				Item it{ItemType::Button, label, help};
				it.onPress = std::move(fn);
				return it;
			};

			{
				Page p{"Camera"};
				p.items.push_back(slider("Mouse Sensitivity", &sEdit.mouseSensitivity, 0.05f, 6.0f, 0.05f, "%.2f", "Camera rotation per mouse movement: 1:1, no acceleration. Same scale as Source engine games (0.022 degrees per count)."));
				p.items.push_back(slider("Vertical Sensitivity", &sEdit.mouseSensitivityY, 0.25f, 2.0f, 0.05f, "%.2fx", "Vertical sensitivity relative to horizontal."));
				p.items.push_back(slider("Aiming Sensitivity", &sEdit.aimSensitivity, 0.1f, 2.0f, 0.05f, "%.2fx", "Sensitivity multiplier while drawing the bow or using the scope."));
				p.items.push_back(toggle("Invert Horizontal", &sEdit.invertX, "Invert left / right camera movement."));
				p.items.push_back(toggle("Invert Vertical", &sEdit.invertY, "Invert up / down camera movement."));
				p.items.push_back(slider("Keyboard Camera Speed", &sEdit.keyCameraSpeed, 0.1f, 1.0f, 0.05f, "%.2f", "Speed of the optional keyboard camera keys."));
				p.items.push_back(slider("Walk Speed", &sEdit.walkSpeed, 0.2f, 0.8f, 0.05f, "%.2f", "Movement speed while the Walk key is held."));
				p.items.push_back(toggle("Parry with Attack", &sEdit.parryWithAttack, "While the shield is raised (Lock On held), the Attack key performs a perfect guard."));
				p.items.push_back(toggle("Remote Bombs: Keep the Bow", &sEdit.bombKeepsBow,
										 "On: while a remote bomb is out, Draw Bow still draws the bow and the Use Rune key detonates the bomb. "
										 "Off: Draw Bow detonates it."));
				p.items.push_back(toggle("Toggle Sprint", &sEdit.toggleSprint, "On: press the Sprint key once to keep sprinting while you move. Press it again or stand still to stop. Off: hold the key."));
				p.items.push_back(toggle("Toggle Lock On / Shield", &sEdit.toggleShield, "On: press the Lock On key once to keep the shield raised and the target locked, press it again to lower it. Off: hold the key. Crouching is always a toggle in the game."));
				p.items.push_back(slider("Weapon Wheel Delay", &sEdit.wheelReleaseDelayMs, 150.0f, 1200.0f, 25.0f, "%.0f ms", "How long the weapon selector stays open after the last mouse wheel step."));
				p.items.push_back(toggle("Selectors: Move Keys", &sEdit.selectorMoveKeys, "On: while a weapon, bow, shield or rune selector is open, the Move Left / Move Right keys step through it. The mouse and the wheel keep working."));
				p.items.push_back(toggle("Inventory: Move Keys", &sEdit.inventoryMoveKeys, "On: Move Left / Move Right move the cursor in the inventory, like Move Forward / Back do. The pages (Adventure Log, Inventory, System) are then switched with the Inventory: Previous / Next Page keys (Controls). Off: Move Left / Move Right switch the pages."));
				pages.push_back(std::move(p));
			}
			{
				Page p{"Controls"};
				const char* lastGroup = "";
				for (size_t i = 0; i < kActionCount; i++)
				{
					const auto& info = GetActionInfo((Action)i);
					if (strcmp(info.group, lastGroup) != 0)
					{
						p.items.push_back(header(info.group));
						lastGroup = info.group;
					}
					Item it{ItemType::Binding, info.name, "Enter: change key   Del: clear   Two keys can be assigned to every action."};
					it.action = (Action)i;
					p.items.push_back(it);
				}
				p.items.push_back(button("Reset All Controls", "Restore the default key assignments.", []() {
					sEdit.ResetBindings();
					Commit();
					Toast("Controls reset to default");
				}));
				pages.push_back(std::move(p));
			}
			{
				Page p{"Motion"};
				p.items.push_back(slider("Tilt Sensitivity", &sEdit.tiltSensitivity, 0.1f, 5.0f, 0.05f, "%.2f", "How much the mouse tilts the Sheikah Slate while Motion Control is active."));
				p.items.push_back(toggle("Toggle Mode", &sEdit.tiltToggle, "ON: the Motion Control key toggles tilting. OFF: hold the key to tilt."));
				p.items.push_back(toggle("Invert Tilt Left / Right", &sEdit.tiltInvertX, ""));
				p.items.push_back(toggle("Invert Tilt Forward / Back", &sEdit.tiltInvertY, ""));
				p.items.push_back(slider("Maximum Tilt", &sEdit.tiltMaxAngle, 15.0f, 90.0f, 1.0f, "%.0f\xC2\xB0", "Largest tilt angle reachable with the mouse."));
				p.items.push_back(slider("Auto Level Speed", &sEdit.tiltReturnSpeed, 0.0f, 120.0f, 5.0f, "%.0f\xC2\xB0/s", "Slowly levels the slate when the mouse rests. 0 = off."));
				pages.push_back(std::move(p));
			}
			{
				Page p{"Graphics"};
				p.items.push_back(choice("Display Mode", &sEdit.displayMode, {"Windowed", "Borderless", "Fullscreen"},
										 "Borderless: a window covering the screen, quick to switch away from. Fullscreen: the game takes the "
										 "display for itself, which can lower input lag. Alt+Enter switches to Windowed and back."));
				{
					Item it = choice("Aspect Ratio", &sEdit.aspectRatio, Graphics::GetAspectOptions(),
									 "Shape of the picture. Automatic: 21:9 or 32:9 on such a display, 16:9 on every other. With another "
									 "shape than the window's, the picture gets bars. Applied on next launch.", true);
					it.rebuild = true;
					p.items.push_back(it);
				}
				const int aspect = Graphics::EffectiveAspect(sEdit.aspectRatio);
				std::vector<std::string> res;
				for (auto& r : Graphics::GetResolutionOptions(aspect))
					res.push_back(r.label);
				p.items.push_back(choice("Resolution", &sEdit.resolutionPreset, res, "Internal rendering resolution. Applied on next launch.", true));
				if (aspect >= 3)
					p.items.push_back(choice("Ultrawide HUD", &sEdit.ultrawideHud, {"At the screen edges", "Centred"},
											 "Where hearts, map and prompts are on an ultrawide picture: at its edges, or within a 16:9 area "
											 "in the middle. Applied on next launch.", true));
				p.items.push_back(choice("Frame Rate", &sFpsIndex, {"30 FPS", "60 FPS", "90 FPS", "120 FPS", "144 FPS", "165 FPS", "240 FPS"},
										 "Frame rate limit. Game speed and physics are corrected automatically. Above 60 FPS, turn V-Sync off "
										 "unless your display runs at that refresh rate."));
				p.items.push_back(toggle("V-Sync", &sEdit.vsync, "Synchronize with the display refresh rate."));
				p.items.push_back(choice("Frame Generation", &sEdit.frameGeneration, {"Off", "AMD FSR 3"},
										 "AMD FSR 3: a generated frame between every two real ones. The Frame Rate limit then counts the frames "
										 "you see: the game runs at half of it (120 FPS = 60 real + 60 generated). Adds about one game frame of "
										 "input lag. Game logic and physics are never affected."));
				p.items.push_back(toggle("CPU Thread Pinning", &sEdit.cpuPinning,
										 "Experimental. Ties the emulated Wii U CPU cores and the GPU thread to fixed processor cores that share one "
										 "cache. A small gain on some CPUs, a large loss on others (Ryzen X3D with two cache groups): leave it off "
										 "unless it helps on yours."));
				p.items.push_back(toggle("Show FPS", &sEdit.showFps, "Shows the game frame rate and, with Frame Generation, the displayed frame rate."));
				p.items.push_back(choice("Anti-Aliasing", &sEdit.antiAliasing, Graphics::GetAAOptions(), ""));
				p.items.push_back(choice("Shadow Quality", &sEdit.shadowPreset, Graphics::GetShadowOptions(), "Shadow resolution and distance. Applied on next launch.", true));
				pages.push_back(std::move(p));
			}
			{
				Page p{"Audio"};
				p.items.push_back(slider("Volume", &sEdit.volume, 0.0f, 100.0f, 1.0f, "%.0f%%", "Volume of the game's sound."));
				p.items.push_back(choice("Speakers", &sEdit.speakers, {"Mono", "Stereo", "Surround 5.1"},
										 "Speaker setup the game mixes its sound for. Applied on next launch.", true));
				p.items.push_back(choice("Output Device", &sAudioDeviceIndex, sAudioDeviceNames,
										 "Where the sound goes. Default: the device Windows uses. Applied on next launch.", true));
				pages.push_back(std::move(p));
			}
			{
				Page p{"Game"};
				if (!embedded)
				{
					// (the System page of the pause menu has them itself)
					p.items.push_back(button("Resume", "Return to the game.", []() { SetOpen(false); }));
					p.items.push_back(button("Save Game", "Opens the game's save prompt.", []() {
						SetOpen(false);
						Menus::RequestSave();
					}));
					p.items.push_back(button("Load Game", "Opens the list of saves.", []() {
						SetOpen(false);
						Menus::RequestLoad();
					}));
				}
				if (sLanguages.size() > 1)
				{
					std::vector<std::string> names;
					for (const GameLanguage& l : sLanguages)
						names.emplace_back(l.name);
					p.items.push_back(choice("Language", &sLanguageIndex, names,
											 "Language of the game's texts (the languages your copy of the game has). The voices are chosen in the game's own "
											 "Options on the title screen. Applied on next launch.",
											 true));
				}
				p.items.push_back(choice("Temperature Unit", &sEdit.temperatureUnit, {"Automatic", "Celsius", "Fahrenheit"},
										 "Unit of the temperature on the map. Automatic: as the game decides (Fahrenheit for US English, Celsius "
										 "otherwise)."));
				p.items.push_back(choice("Inventory Page Turn", &sEdit.inventoryPageTurn, {"Instant", "Smooth", "Slow"},
										 "How the inventory turns pages when you click a category or use the mouse wheel. Instant: no animation. "
										 "Smooth: a short slide. Slow: the game's own slide. A category click always goes straight to the "
										 "category, however many pages lie in between."));
				p.items.push_back(toggle("Interactive Map", &sEdit.mapEnabled,
										 "Completely enable or disable the interactive map and all its UI on the Sheikah Slate world map. (Shortcut: H on map)"));
				p.items.push_back(toggle("Interactive Map Overlay", &sEdit.mapOverlay,
										 "Shows interactive markers, item search and category filters directly on the Sheikah Slate world map."));
				p.items.push_back(toggle("Minimap Markers", &sEdit.mapMinimap,
										 "Shows nearby interactive map markers directly on the HUD minimap compass during gameplay."));
				p.items.push_back(toggle("Map: Auto-Hide Claimed", &sEdit.mapAutoHide,
										 "Automatically hides opened treasure chests, completed shrines, activated towers and collected Korok seeds."));
				p.items.push_back(header("amiibo"));
				p.items.push_back(button("Add amiibo File...", "Copies the .bin file of an amiibo (a dump of your figure) into the game's amiibo folder. "
															   "When the amiibo rune looks for an amiibo, you choose one of them. The rune is turned on in the "
															   "game's own Options (System page).",
										 []() {
											 AddAmiiboFile();
											 Toast("Choose the file in the window that opened");
										 }));
				p.items.push_back(button("Open amiibo Folder", "The folder with your amiibo files. Files put there by hand are offered as well.",
										 []() { OpenAmiiboFolder(); }));
				p.items.push_back(button("Quit to Desktop", "Unsaved progress since the last save will be lost.", []() { sConfirmQuit = true; }));
				pages.push_back(std::move(p));
			}
			{
				Page p{"Mods"};
				p.items.push_back(header("Graphic Packs"));
				p.items.push_back(button("Open Mods Folder",
										 "Mods are graphic packs (folders with a rules.txt, as in Cemu): put them here or export them with a mod manager such as BCML, they are listed from the next start. Mods of your Cemu installation are listed too.",
										 []() {
											 Graphics::OpenModsFolder();
											 Toast("The mods folder opened in a window");
										 }));
				sModIds.clear();
				sModOn.clear();
				sModPresetRows.clear();
				sModPresetIdx.clear();
				std::unordered_set<std::string> activeMods;
				std::vector<std::string> tokens;
				boost::split(tokens, sEdit.userGraphicPacks, boost::is_any_of(";"));
				for (const auto& t : tokens)
				{
					if (!t.empty())
						activeMods.insert(t);
				}
				// the help line holds two lines of text: long descriptions are cut
				auto shortHelp = [](std::string text) {
					boost::replace_all(text, "\\n", " ");
					boost::replace_all(text, "\n", " ");
					boost::trim(text);
					if (text.size() > 200)
						text = text.substr(0, text.find_last_of(' ', 197)) + "...";
					return text;
				};
				const auto userPacks = Graphics::ListUserPacks();
				for (const auto& up : userPacks)
				{
					sModIds.push_back(up.id);
					sModOn.push_back(activeMods.contains(up.id));
				}
				// the community packs by kind, then the player's own; a pack that is on shows its presets below it
				for (const char* group : {"Enhancements", "Mods", "Cheats", "Workarounds", "Your Mods"})
				{
					bool headed = false;
					for (size_t i = 0; i < userPacks.size(); i++)
					{
						const Graphics::UserPack& up = userPacks[i];
						if (up.group != group)
							continue;
						if (!headed)
						{
							p.items.push_back(header(group));
							headed = true;
						}
						std::string help = up.description.empty() ? fmt::format("Graphic pack: {}", up.id) : shortHelp(up.description);
						if (!up.live)
							help = shortHelp(help + " Applied the next time the game starts.");
						Item it{ItemType::Toggle, up.name, std::move(help)};
						it.bval = &sModOn[i];
						it.restart = !up.live; // the others are applied at once
						it.rebuild = true;     // its presets appear / disappear
						p.items.push_back(std::move(it));
						if (!sModOn[i])
							continue;
						for (const Graphics::UserPackPresets& cat : up.presets)
						{
							sModPresetRows.push_back({up.id, cat.category, cat.names, cat.active});
							sModPresetIdx.push_back(cat.active);
							Item pr{ItemType::Choice, fmt::format("    {}", cat.category.empty() ? std::string("Preset") : cat.category),
									shortHelp(fmt::format("{}: {}.{}", up.name, cat.category.empty() ? "preset" : cat.category,
														  up.live ? "" : " Applied the next time the game starts."))};
							pr.ival = &sModPresetIdx.back();
							pr.options = cat.names;
							pr.restart = !up.live;
							pr.rebuild = true; // the presets shown in its other lists can depend on this one
							p.items.push_back(std::move(pr));
						}
					}
					if (!headed && std::string_view(group) == "Your Mods")
					{
						p.items.push_back(header("Your Mods"));
						p.items.push_back(header("None yet: put graphic packs into the mods folder"));
					}
				}
				pages.push_back(std::move(p));
			}
			return pages;
		}

		std::vector<Page> sPages;

		bool Selectable(const Item& it)
		{
			return it.type != ItemType::Header;
		}

		void FixSelection(int dir)
		{
			auto& items = sPages[sPage].items;
			if (items.empty())
				return;
			sSel = std::clamp(sSel, 0, (int)items.size() - 1);
			int guard = 0;
			while (!Selectable(items[sSel]) && guard++ < (int)items.size())
			{
				sSel += dir == 0 ? 1 : dir;
				if (sSel >= (int)items.size()) sSel = 0;
				if (sSel < 0) sSel = (int)items.size() - 1;
			}
		}

		bool KeyHasConflict(Action a, Key k)
		{
			if (k == Key::None)
				return false;
			for (size_t i = 0; i < kActionCount; i++)
			{
				if ((Action)i == a)
					continue;
				for (Key o : sEdit.bindings[i].keys)
					if (o == k)
						return true;
			}
			return false;
		}

		std::string ConflictName(Action a, Key k)
		{
			for (size_t i = 0; i < kActionCount; i++)
			{
				if ((Action)i == a)
					continue;
				for (Key o : sEdit.bindings[i].keys)
					if (o == k)
						return GetActionInfo((Action)i).name;
			}
			return "";
		}

		void Adjust(Item& it, int dir)
		{
			switch (it.type)
			{
			case ItemType::Slider:
				*it.fval = std::clamp(*it.fval + dir * it.fstep, it.fmin, it.fmax);
				*it.fval = roundf(*it.fval / it.fstep) * it.fstep;
				break;
			case ItemType::Toggle:
				*it.bval = !*it.bval;
				break;
			case ItemType::Choice:
				*it.ival = std::clamp(*it.ival + dir, 0, (int)it.options.size() - 1);
				break;
			default:
				return;
			}
			sEdit.fpsLimit = kFpsValues[std::clamp(sFpsIndex, 0, (int)std::size(kFpsValues) - 1)];
			if (!sLanguages.empty())
				sEdit.language = sLanguages[std::clamp(sLanguageIndex, 0, (int)sLanguages.size() - 1)].code;
			Commit();
			if (it.restart)
				Toast("This change takes effect the next time the game starts");
			if (it.rebuild)
			{
				// (the item is gone after this: nothing may use it any more)
				const int selected = sSel;
				sPages = BuildPages(sPagesEmbedded);
				sSel = std::clamp(selected, 0, std::max(0, (int)sPages[sPage].items.size() - 1));
			}
		}

		void Activate(Item& it)
		{
			switch (it.type)
			{
			case ItemType::Toggle:
				Adjust(it, 1);
				break;
			case ItemType::Choice:
				*it.ival = (*it.ival + 1) % (int)it.options.size();
				Adjust(it, 0);
				break;
			case ItemType::Binding:
				sRebinding = true;
				sRebindAction = it.action;
				sRebindStart = Now();
				Input::ClearKeyPresses();
				break;
			case ItemType::Button:
				if (it.onPress)
					it.onPress();
				break;
			default:
				break;
			}
		}

		void FinishRebind(Key k)
		{
			auto& b = sEdit.bind(sRebindAction).keys;
			if (k == b[sRebindSlot ^ 1])
				b[sRebindSlot ^ 1] = Key::None; // same key in both slots -> keep one
			b[sRebindSlot] = k;
			sRebinding = false;
			if (KeyHasConflict(sRebindAction, k))
				Toast(fmt::format("{} is also used by \"{}\"", KeyDisplayName(k), ConflictName(sRebindAction, k)));
			Commit();
		}

		// ------------- drawing helpers -------------
		void TextAligned(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const std::string& s, float alignX = 0.0f)
		{
			ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str());
			dl->AddText(f, size, ImVec2(pos.x - sz.x * alignX, pos.y), col, s.c_str());
		}

		void KeyCap(ImDrawList* dl, ImFont* f, float size, ImVec2 center, Key k, ImU32 col, float alpha)
		{
			std::string label = k == Key::None ? "-" : KeyDisplayName(k);
			ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str());
			float w = std::max(ts.x + size * 0.9f, size * 1.6f), h = size * 1.35f;
			ImVec2 a(center.x - w * 0.5f, center.y - h * 0.5f), b(center.x + w * 0.5f, center.y + h * 0.5f);
			if (k != Key::None)
			{
				dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, (int)(18 * alpha)), h * 0.22f);
				dl->AddRect(a, b, (col & 0x00FFFFFF) | ((uint32)(150 * alpha) << 24), h * 0.22f, 0, std::max(1.0f, size * 0.07f));
			}
			dl->AddText(f, size, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), col, label.c_str());
		}

		void SheikahEye(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t)
		{
			// stylized eye: almond + iris + tear drop, drawn with lines
			const int n = 32;
			ImVec2 top[33], bot[33];
			for (int i = 0; i <= n; i++)
			{
				float x = -1.0f + 2.0f * i / n;
				float y = 0.55f * (1.0f - x * x);
				top[i] = ImVec2(c.x + x * r, c.y - y * r);
				bot[i] = ImVec2(c.x + x * r, c.y + y * r);
			}
			dl->AddPolyline(top, n + 1, col, 0, r * 0.08f);
			dl->AddPolyline(bot, n + 1, col, 0, r * 0.08f);
			dl->AddCircle(c, r * 0.32f, col, 24, r * 0.08f);
			dl->AddCircleFilled(c, r * 0.12f, col, 16);
			ImVec2 tear[4] = {ImVec2(c.x, c.y + r * 0.62f), ImVec2(c.x - r * 0.12f, c.y + r * 0.85f), ImVec2(c.x, c.y + r * 1.05f), ImVec2(c.x + r * 0.12f, c.y + r * 0.85f)};
			dl->AddConvexPolyFilled(tear, 4, col);
			for (int i = 0; i < 3; i++)
			{
				float a = -1.5708f + (i - 1) * 0.55f;
				ImVec2 p0(c.x + cosf(a) * r * 0.7f, c.y + sinf(a) * r * 0.7f), p1(c.x + cosf(a) * r * 0.95f, c.y + sinf(a) * r * 0.95f);
				dl->AddLine(p0, p1, col, r * 0.07f);
			}
		}
	}

	void DebugSetPage(int page)
	{
		sPage = std::clamp(page, 0, std::max(0, (int)sPages.size() - 1));
		sSel = 0;
		sScroll = sScrollTarget = 0;
	}

	void SetOpen(bool open)
	{
		if (open == sOpen.load())
			return;
		if (open)
		{
			sEdit = GetSettings();
			sFpsIndex = 0;
			for (int i = 0; i < (int)std::size(kFpsValues); i++)
				if (sEdit.fpsLimit >= kFpsValues[i])
					sFpsIndex = i;
			sLanguages = GetGameLanguages();
			sLanguageIndex = 0;
			for (int i = 0; i < (int)sLanguages.size(); i++)
				if (sEdit.language == sLanguages[i].code)
					sLanguageIndex = i;
			LoadAudioDevices();
			sPages = BuildPages(false);
			sPagesEmbedded = false;
			sRebinding = false;
			sConfirmQuit = false;
			FixSelection(1);
		}
		sOpen = open;
		Input::SetOverlayOpen(open);
	}

	void RequestOpen()
	{
		sOpenRequest = true;
	}

	void DebugMouse(float x, float y, bool down)
	{
		sDebugMouseX = x;
		sDebugMouseY = y;
		sDebugMouseDown = down;
	}

	bool IsOpen()
	{
		return sOpen.load();
	}

	void Toggle()
	{
		SetOpen(!IsOpen());
	}

	bool WantsPause()
	{
		return sOpen.load();
	}

	namespace
	{
		std::atomic<bool> sGamePaused{false};
		std::atomic<bool> sShutdown{false};
	}

	void BlockWhileOpen()
	{
		if (!sOpen.load() || sShutdown.load())
			return;
		sGamePaused = true;
		while (sOpen.load() && !sShutdown.load())
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		sGamePaused = false;
	}

	bool IsGamePaused()
	{
		return sGamePaused.load();
	}

	void DrawLoadingIndicator(float progress)
	{
		ImGuiIO& io = ImGui::GetIO();
		ImDrawList* dl = ImGui::GetForegroundDrawList();
		const float W = io.DisplaySize.x, H = io.DisplaySize.y;
		const float r = H * 0.022f;
		const ImVec2 c(W - r * 3.2f, H - r * 2.6f);
		const float t = (float)Now();
		const float pulse = 0.55f + 0.45f * sinf(t * 3.0f);
		SheikahEye(dl, c, r, IM_COL32(110, 225, 255, (int)(255 * pulse)), t);
		// thin progress arc around the eye
		const float a0 = -1.5708f, a1 = a0 + 6.28318f * std::clamp(progress, 0.0f, 1.0f);
		dl->PathArcTo(c, r * 1.55f, a0, a1, 48);
		dl->PathStroke(IM_COL32(110, 225, 255, 160), 0, std::max(1.5f, r * 0.08f));
	}

	void ReleaseForShutdown()
	{
		sShutdown = true;
		sOpen = false;
		Input::SetOverlayOpen(false);
	}

	void PrepareFrame()
	{
		ImGuiIO& io = ImGui::GetIO();
		int h = (int)io.DisplaySize.y;
		if (h <= 0)
			return;
		int fh = std::max(12, h / 30);
		if (sFonts.height == fh && sFonts.item)
			return;
		if (sFontData.empty())
		{
			fs::path p = ActiveSettings::GetDataPath("resources/sharedFonts/CafeStd.ttf");
			std::ifstream f(p, std::ios::binary);
			sFontData.assign(std::istreambuf_iterator<char>(f), {});
			if (sFontData.empty())
				return;
		}
		ImFontConfig cfg{};
		cfg.FontDataOwnedByAtlas = false;
		static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2190, 0x2193, 0x25B6, 0x25C0, 0};
		sFonts.title = io.Fonts->AddFontFromMemoryTTF(sFontData.data(), (int)sFontData.size(), (float)fh * 1.45f, &cfg, ranges);
		sFonts.item = io.Fonts->AddFontFromMemoryTTF(sFontData.data(), (int)sFontData.size(), (float)fh, &cfg, ranges);
		sFonts.small = io.Fonts->AddFontFromMemoryTTF(sFontData.data(), (int)sFontData.size(), (float)fh * 0.72f, &cfg, ranges);
		if (sKeyFontData.empty())
		{
#if BOOST_OS_WINDOWS
			wchar_t windir[MAX_PATH]{};
			GetWindowsDirectoryW(windir, MAX_PATH);
			std::ifstream kf(fs::path(windir) / "Fonts" / "segoeuib.ttf", std::ios::binary);
			sKeyFontData.assign(std::istreambuf_iterator<char>(kf), {});
#else
			const char* linuxFontPaths[] = {
				"/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
				"/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
				"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
				"/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
				"/usr/share/fonts/google-noto/NotoSans-Bold.ttf"
			};
			for (const char* fpath : linuxFontPaths)
			{
				std::ifstream kf(fpath, std::ios::binary);
				if (kf.good())
				{
					sKeyFontData.assign(std::istreambuf_iterator<char>(kf), {});
					break;
				}
			}
#endif
		}
		static const ImWchar keyRanges[] = {0x0020, 0x007E, 0};
		sFonts.key = sKeyFontData.empty() ? sFonts.item
										  : io.Fonts->AddFontFromMemoryTTF(sKeyFontData.data(), (int)sKeyFontData.size(), (float)fh * 1.2f, &cfg, keyRanges);
		sFonts.height = fh;
	}

	namespace
	{
		// a PC key / mouse button in the style of BotW's button circles: white cap, dark label
		void DrawPromptKey(ImDrawList* dl, const ImVec2& center, float h, uint16 glyph, float alpha)
		{
			const int a = (int)(255 * std::clamp(alpha, 0.0f, 1.0f));
			const ImU32 cap = IM_COL32(255, 255, 255, a);
			const ImU32 ink = IM_COL32(28, 28, 26, a);
			if (glyph < 0xE100)
				return;
			const Key k = (Key)(glyph - 0xE100);
			if (IsMouseKey(k) && k != Key::WheelUp && k != Key::WheelDown)
			{
				// mouse: body outline, the pressed button filled
				const float w = h * 0.7f, r = w * 0.5f, t = std::max(1.5f, h * 0.085f);
				const ImVec2 p0(center.x - w * 0.5f, center.y - h * 0.5f), p1(center.x + w * 0.5f, center.y + h * 0.5f);
				const float split = p0.y + h * 0.42f;
				dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, a / 3), r);
				if (k == Key::MouseLeft)
					dl->AddRectFilled(p0, ImVec2(center.x, split), cap, r, ImDrawFlags_RoundCornersTopLeft);
				if (k == Key::MouseRight)
					dl->AddRectFilled(ImVec2(center.x, p0.y), ImVec2(p1.x, split), cap, r, ImDrawFlags_RoundCornersTopRight);
				dl->AddRect(p0, p1, cap, r, 0, t);
				dl->AddLine(ImVec2(p0.x, split), ImVec2(p1.x, split), cap, t);
				dl->AddLine(ImVec2(center.x, p0.y), ImVec2(center.x, split), cap, t);
				if (k == Key::MouseMiddle)
					dl->AddRectFilled(ImVec2(center.x - w * 0.13f, p0.y + h * 0.12f), ImVec2(center.x + w * 0.13f, split + h * 0.08f), cap, w * 0.13f);
				if (k == Key::Mouse4 || k == Key::Mouse5)
				{
					const float y = split + h * (k == Key::Mouse4 ? 0.22f : 0.08f);
					dl->AddRectFilled(ImVec2(p0.x - w * 0.1f, y), ImVec2(p0.x + w * 0.22f, y + h * 0.13f), cap, 2.0f);
				}
				return;
			}
			std::string label = KeyCapLabel(k);
			if (k == Key::WheelUp)
				label = "Wheel+";
			else if (k == Key::WheelDown)
				label = "Wheel-";
			if (label.empty())
				return;
			ImFont* font = sFonts.key ? sFonts.key : ImGui::GetFont();
			float fs = h * (label.size() == 1 ? 0.80f : 0.48f);
			ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str());
			const float w = std::max(h, ts.x + h * 0.45f);
			const ImVec2 p0(center.x - w * 0.5f, center.y - h * 0.5f), p1(center.x + w * 0.5f, center.y + h * 0.5f);
			dl->AddRectFilled(p0, p1, cap, h * 0.22f);
			dl->AddText(font, fs, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.52f), ink, label.c_str());
		}

		// a mouse in the style of the prompt keys: body, one part lit (0 none, 1 left, 2 right, 3 wheel)
		void DrawMouseBody(ImDrawList* dl, const ImVec2& center, float h, int lit, int a)
		{
			const ImU32 cap = IM_COL32(255, 255, 255, a);
			const float w = h * 0.7f, r = w * 0.5f, t = std::max(1.5f, h * 0.085f);
			const ImVec2 p0(center.x - w * 0.5f, center.y - h * 0.5f), p1(center.x + w * 0.5f, center.y + h * 0.5f);
			const float split = p0.y + h * 0.42f;
			dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, a / 3), r);
			if (lit == 1)
				dl->AddRectFilled(p0, ImVec2(center.x, split), cap, r, ImDrawFlags_RoundCornersTopLeft);
			if (lit == 2)
				dl->AddRectFilled(ImVec2(center.x, p0.y), ImVec2(p1.x, split), cap, r, ImDrawFlags_RoundCornersTopRight);
			dl->AddRect(p0, p1, cap, r, 0, t);
			dl->AddLine(ImVec2(p0.x, split), ImVec2(p1.x, split), cap, t);
			dl->AddLine(ImVec2(center.x, p0.y), ImVec2(center.x, split), cap, t);
			if (lit == 3)
				dl->AddRectFilled(ImVec2(center.x - w * 0.13f, p0.y + h * 0.12f), ImVec2(center.x + w * 0.13f, split + h * 0.08f), cap, w * 0.13f);
		}

		// width a key cap / mouse of height h takes (for placing several in a row)
		float GlyphWidth(uint16 glyph, float h)
		{
			if (glyph >= 0xE100)
			{
				const Key k = (Key)(glyph - 0xE100);
				if (IsMouseKey(k) && k != Key::WheelUp && k != Key::WheelDown)
					return h * 0.7f;
				const std::string label = KeyCapLabel(k);
				ImFont* font = sFonts.key ? sFonts.key : ImGui::GetFont();
				const float fsz = h * (label.size() == 1 ? 0.80f : 0.48f);
				return std::max(h, font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, label.c_str()).x + h * 0.45f);
			}
			return h * 1.6f;
		}

		// a PC glyph of PCPrompts: one key, or the composites (keys W A S D, a mouse moved, the wheel, 1 - 4, ...)
		void DrawGlyph(ImDrawList* dl, const ImVec2& c, float h, uint16 glyph, float alpha)
		{
			using namespace Prompts;
			const int a = (int)(255 * std::clamp(alpha, 0.0f, 1.0f));
			auto key = [&](ImVec2 p, float kh, Key k) { DrawPromptKey(dl, p, kh, KeyGlyph(k) ? KeyGlyph(k) : (uint16)(0xE100 + (uint16)k), alpha); };
			auto arrowText = [&](ImVec2 p, float sz, const char* t) {
				ImFont* f = sFonts.item;
				const ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, 0, t);
				dl->AddText(f, sz, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), IM_COL32(255, 255, 255, a), t);
			};
			if (glyph >= 0xE100)
			{
				DrawPromptKey(dl, c, h, glyph, alpha);
				return;
			}
			const float s = h * 0.55f, g = s * 1.08f;
			switch (glyph)
			{
			case kGlyphWASD:
				key(ImVec2(c.x, c.y - g * 0.5f), s, KeyChar('W'));
				key(ImVec2(c.x - g, c.y + g * 0.5f), s, KeyChar('A'));
				key(ImVec2(c.x, c.y + g * 0.5f), s, KeyChar('S'));
				key(ImVec2(c.x + g, c.y + g * 0.5f), s, KeyChar('D'));
				break;
			case kGlyphAD:
				key(ImVec2(c.x - g * 0.55f, c.y), s * 1.2f, KeyChar('A'));
				key(ImVec2(c.x + g * 0.55f, c.y), s * 1.2f, KeyChar('D'));
				break;
			case kGlyphWS:
				key(ImVec2(c.x, c.y - g * 0.5f), s, KeyChar('W'));
				key(ImVec2(c.x, c.y + g * 0.5f), s, KeyChar('S'));
				break;
			case kGlyphArrowKeys:
				key(ImVec2(c.x, c.y - g * 0.5f), s, Key::Up);
				key(ImVec2(c.x - g, c.y + g * 0.5f), s, Key::Left);
				key(ImVec2(c.x, c.y + g * 0.5f), s, Key::Down);
				key(ImVec2(c.x + g, c.y + g * 0.5f), s, Key::Right);
				break;
			case kGlyphOneToFour:
				for (int i = 0; i < 4; i++)
					key(ImVec2(c.x + (i - 1.5f) * g, c.y), s, KeyChar((char)('1' + i)));
				break;
			case kGlyphWheelUpDown:
				DrawMouseBody(dl, c, h, 3, a);
				arrowText(ImVec2(c.x + h * 0.62f, c.y), h * 0.45f, "\xE2\x86\x91\xE2\x86\x93");
				break;
			default:
			{
				// the mouse moved: all ways, sideways, up / down, one way
				DrawMouseBody(dl, c, h, 0, a);
				const char* t = glyph == kGlyphMouseX ? "\xE2\x86\x90\xE2\x86\x92" : glyph == kGlyphMouseY ? "\xE2\x86\x91\xE2\x86\x93"
							  : glyph == kGlyphMouseLeft ? "\xE2\x86\x90" : glyph == kGlyphMouseRight ? "\xE2\x86\x92"
							  : glyph == kGlyphMouseUp ? "\xE2\x86\x91" : glyph == kGlyphMouseDown ? "\xE2\x86\x93" : "\xE2\x86\x94";
				arrowText(ImVec2(c.x + h * 0.62f, c.y), h * 0.45f, t);
				break;
			}
			}
		}

		// "Ability Controls": the PC keys where the game shows pictures of the GamePad (hidden by PCUI)
		void DrawTipKeys()
		{
			const auto tips = UI::GetTipKeys();
			if (tips.empty() || !sFonts.item)
				return;
			ImGuiIO& io = ImGui::GetIO();
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			float X = 0.0f, Y = 0.0f, W = io.DisplaySize.x, H = io.DisplaySize.y;
			int px, py, pw, ph, ww, wh;
			if (Graphics::GetPictureArea(px, py, pw, ph, ww, wh))
			{
				const float kx = io.DisplaySize.x / (float)ww, ky = io.DisplaySize.y / (float)wh;
				X = px * kx;
				Y = py * ky;
				W = pw * kx;
				H = ph * ky;
			}
			const Settings s = GetSettings();
			for (const auto& t : tips)
			{
				if (t.codes.empty())
					continue;
				const ImVec2 c(X + t.rect.cx() * W, Y + t.rect.cy() * H);
				const float h = std::min((t.rect.y1 - t.rect.y0) * H, (t.rect.x1 - t.rect.x0) * W * 0.6f) * 0.34f;
				std::vector<uint16> glyphs;
				for (uint16 code : t.codes)
					glyphs.push_back(code >= 0xE0F0 && code < 0xE100 ? code : Prompts::DefaultGlyphFor(s, code));
				float total = 0.0f;
				for (uint16 g : glyphs)
					total += GlyphWidth(g, h);
				total += h * 0.6f * (glyphs.size() - 1);
				float x = c.x - total * 0.5f;
				for (size_t i = 0; i < glyphs.size(); i++)
				{
					const float w = GlyphWidth(glyphs[i], h);
					DrawGlyph(dl, ImVec2(x + w * 0.5f, c.y), h, glyphs[i], 1.0f);
					x += w;
					if (i + 1 < glyphs.size())
					{
						TextAligned(dl, sFonts.item, h * 0.5f, ImVec2(x + h * 0.3f, c.y - h * 0.27f), kText, "+", 0.5f);
						x += h * 0.6f;
					}
				}
			}
		}

		void DrawFpsCounter()
		{
			static uint64 rev = ~0ull;
			static bool show = false;
			if (GetSettingsRevision() != rev)
			{
				rev = GetSettingsRevision();
				show = GetSettings().showFps;
			}
			if (!show)
				return;
			float base = 0.0f, shown = 0.0f;
			bool gen = false;
			LatteFrameGen_GetStats(base, shown, gen);
			const std::string text = gen ? fmt::format("{:.0f} FPS  |  {:.0f} displayed", base, shown) : fmt::format("{:.0f} FPS", base);
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			const float fs = std::max(14.0f, ImGui::GetIO().DisplaySize.y * 0.018f);
			ImFont* font = ImGui::GetFont();
			const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
			const ImVec2 p(fs * 0.6f, fs * 0.5f);
			dl->AddRectFilled(ImVec2(p.x - fs * 0.35f, p.y - fs * 0.2f), ImVec2(p.x + ts.x + fs * 0.35f, p.y + ts.y + fs * 0.2f), IM_COL32(0, 0, 0, 140), fs * 0.3f);
			dl->AddText(font, fs, p, IM_COL32(235, 235, 220, 255), text.c_str());
		}

		// HUD action buttons (A/B/X/Y/L of the game's action diamond), see UI::GetHudPrompts
		void DrawHudPrompts()
		{
			if (!Prompts::Active())
				return;
			const auto prompts = UI::GetHudPrompts();
			if (prompts.empty())
				return;
			ImGuiIO& io = ImGui::GetIO();
			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			// (the prompts are positions in the game's picture, which has bars in a window of another shape)
			float X = 0.0f, Y = 0.0f, W = io.DisplaySize.x, H = io.DisplaySize.y;
			int px, py, pw, ph, ww, wh;
			if (Graphics::GetPictureArea(px, py, pw, ph, ww, wh))
			{
				const float kx = io.DisplaySize.x / (float)ww, ky = io.DisplaySize.y / (float)wh;
				X = px * kx;
				Y = py * ky;
				W = pw * kx;
				H = ph * ky;
			}
			std::vector<ImVec2> drawn;
			for (const auto& p : prompts)
			{
				const ImVec2 c(X + (p.rect.x0 + p.rect.x1) * 0.5f * W, Y + (p.rect.y0 + p.rect.y1) * 0.5f * H);
				bool dup = false;
				for (auto& d : drawn)
					dup |= std::fabs(d.x - c.x) < 4.0f && std::fabs(d.y - c.y) < 4.0f;
				if (dup)
					continue;
				drawn.push_back(c);
				const float h = (p.rect.y1 - p.rect.y0) * H * 0.70f;
				DrawPromptKey(dl, c, h, p.glyph, p.alpha);
			}
		}
	}

	namespace
	{
		bool IsSettingsKey(Key k)
		{
			const Settings cur = GetSettings();
			return k == cur.bind(Action::Settings).keys[0] || k == cur.bind(Action::Settings).keys[1];
		}

		void SelectPage(int page)
		{
			sPage = (page + (int)sPages.size()) % (int)sPages.size();
			sSel = 0;
			sScroll = sScrollTarget = 0;
			FixSelection(1);
		}

		// a key for the list (both menus); returns true for "back" (closes the PC menu, leaves the settings in the
		// System page)
		bool HandleKey(Key k)
		{
			auto& items = sPages[sPage].items;
			if (sConfirmQuit)
			{
				if (k == Key::Enter || k == KeyChar('Y'))
				{
#if BOOST_OS_WINDOWS
					PostMessageW((HWND)WindowSystem::GetWindowInfo().window_main.surface, WM_CLOSE, 0, 0);
#else
					pce::RequestExit();
#endif
				}
				sConfirmQuit = false;
				return false;
			}
			if (sRebinding)
			{
				if (k == Key::Escape)
					sRebinding = false;
				else if (!(k == Key::MouseLeft && Now() - sRebindStart < 0.25)) // (the click which started the rebinding)
					FinishRebind(k);
				return false;
			}
			if (k == Key::Escape || k == Key::Backspace || (IsSettingsKey(k) && k != Key::MouseLeft))
				return true;
			if (k == Key::Up || k == KeyChar('W'))
			{
				sSel--;
				sEnsureVisible = true;
				if (sSel < 0) sSel = (int)items.size() - 1;
				FixSelection(-1);
			}
			else if (k == Key::Down || k == KeyChar('S'))
			{
				sSel++;
				sEnsureVisible = true;
				if (sSel >= (int)items.size()) sSel = 0;
				FixSelection(1);
			}
			else if (k == Key::Left || k == KeyChar('A'))
			{
				if (!items.empty() && items[sSel].type == ItemType::Binding)
					sRebindSlot = 0;
				else if (!items.empty())
					Adjust(items[sSel], -1);
			}
			else if (k == Key::Right || k == KeyChar('D'))
			{
				if (!items.empty() && items[sSel].type == ItemType::Binding)
					sRebindSlot = 1;
				else if (!items.empty())
					Adjust(items[sSel], 1);
			}
			else if (k == Key::Enter || k == Key::Space)
			{
				if (!items.empty())
					Activate(items[sSel]);
			}
			else if (k == Key::Delete)
			{
				if (!items.empty() && items[sSel].type == ItemType::Binding)
				{
					sEdit.bind(items[sSel].action).keys[sRebindSlot] = Key::None;
					Commit();
				}
			}
			else if (k == KeyChar('Q') || k == Key::PageUp)
				SelectPage(sPage - 1);
			else if (k == KeyChar('E') || k == Key::PageDown || k == Key::Tab)
				SelectPage(sPage + 1);
			return false;
		}

		// the tab row; returns its width
		float DrawTabs(ImDrawList* dl, float right, float y, float fs, float a, bool interactive, bool alignCenter, float centerX)
		{
			ImGuiIO& io = ImGui::GetIO();
			auto alpha = [&](ImU32 c, float m = 1.0f) { return (c & 0x00FFFFFF) | ((uint32)(((c >> 24) & 0xFF) * a * m) << 24); };
			const float gap = fs * 1.4f;
			float total = 0.0f;
			for (auto& p : sPages)
				total += sFonts.item->CalcTextSizeA(fs, FLT_MAX, 0, p.name.c_str()).x;
			total += gap * (float)(sPages.size() - 1);
			float tx = alignCenter ? centerX - total * 0.5f : right - total;
			for (int i = 0; i < (int)sPages.size(); i++)
			{
				const std::string& n = sPages[i].name;
				const ImVec2 ts = sFonts.item->CalcTextSizeA(fs, FLT_MAX, 0, n.c_str());
				const ImVec2 tp(tx, y);
				const bool active = i == sPage;
				const ImRect r(ImVec2(tp.x - fs * 0.4f, tp.y - fs * 0.3f), ImVec2(tp.x + ts.x + fs * 0.4f, tp.y + ts.y + fs * 0.4f));
				const bool hover = interactive && !sRebinding && r.Contains(io.MousePos);
				dl->AddText(sFonts.item, fs, tp, alpha(active ? kAccent : (hover ? kText : kTextDim)), n.c_str());
				if (active)
					dl->AddLine(ImVec2(tp.x, tp.y + ts.y + fs * 0.15f), ImVec2(tp.x + ts.x, tp.y + ts.y + fs * 0.15f), alpha(kAccent), 2.0f);
				if (hover && io.MouseDown[0] && !sMouseWasDown)
					SelectPage(i);
				tx += ts.x + gap;
			}
			return total;
		}

		// the rows of the current page in `list` (scrolled); `interactive`: the mouse may use them, `focused`: the
		// selection is shown
		void DrawRows(ImDrawList* dl, const ImRect& list, float fs, float a, bool interactive, bool focused, int wheel, double now)
		{
			ImGuiIO& io = ImGui::GetIO();
			auto alpha = [&](ImU32 c, float m = 1.0f) { return (c & 0x00FFFFFF) | ((uint32)(((c >> 24) & 0xFF) * a * m) << 24); };
			auto& items = sPages[sPage].items;
			const float rowH = fs * 1.75f;
			const float listTop = list.Min.y, listBottom = list.Max.y, listH = listBottom - listTop;
			const float contentH = rowH * items.size();
			// Scrolling eases towards its target. The wheel moves the target; the keyboard scrolls the selection into view
			// once when it moves it (not every frame: that pulled the list back against the wheel).
			const float maxScroll = std::max(0.0f, contentH - listH);
			if (sEnsureVisible && focused)
			{
				const float selY = sSel * rowH;
				if (selY < sScrollTarget)
					sScrollTarget = selY;
				if (selY + rowH > sScrollTarget + listH)
					sScrollTarget = selY + rowH - listH;
			}
			sEnsureVisible = false;
			if (wheel != 0 && !sRebinding)
				sScrollTarget -= wheel * rowH * 1.5f;
			sScrollTarget = std::clamp(sScrollTarget, 0.0f, maxScroll);
			sScroll += (sScrollTarget - sScroll) * (1.0f - expf(-std::min(io.DeltaTime, 0.1f) * 18.0f));
			if (std::fabs(sScrollTarget - sScroll) < 0.5f)
				sScroll = sScrollTarget;
			sScroll = std::clamp(sScroll, 0.0f, maxScroll);

			const float x0 = list.Min.x, x1 = list.Max.x, w = x1 - x0;
			const float colLabel = x0 + fs * 0.8f;
			const float colValue = x0 + w * 0.58f;
			const float colKey1 = x0 + w * 0.58f, colKey2 = x0 + w * 0.79f;
			dl->PushClipRect(ImVec2(x0 - fs, listTop), ImVec2(x1 + fs, listBottom), true);
			const bool mouseClicked = interactive && io.MouseDown[0] && !sMouseWasDown;
			for (int i = 0; i < (int)items.size(); i++)
			{
				Item& it = items[i];
				const float y = listTop + i * rowH - sScroll;
				if (y + rowH < listTop || y > listBottom)
					continue;
				const ImVec2 r0(x0, y), r1(x1, y + rowH * 0.92f);
				const float ty = y + (rowH * 0.92f - fs) * 0.5f;
				if (it.type == ItemType::Header)
				{
					dl->AddText(sFonts.small, fs * 0.72f, ImVec2(colLabel, ty + fs * 0.2f), alpha(kAccent, 0.9f), it.label.c_str());
					dl->AddLine(ImVec2(colLabel, y + rowH * 0.85f), ImVec2(r1.x - fs, y + rowH * 0.85f), alpha(kAccentDim), 1.0f);
					continue;
				}
				const bool hover = interactive && !sRebinding && !sConfirmQuit && ImRect(r0, r1).Contains(io.MousePos) && io.MousePos.y >= listTop &&
								   io.MousePos.y <= listBottom;
				if (hover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || mouseClicked))
					sSel = i;
				const bool sel = i == sSel && (focused || hover);
				if (sel)
				{
					dl->AddRectFilled(r0, r1, alpha(IM_COL32(248, 200, 0, 22)));
					dl->AddRect(r0, r1, alpha(kAccent, 0.85f), 2.0f, 0, 1.5f);
					// glowing corner ticks
					const float tk = fs * 0.5f;
					dl->AddLine(r0, ImVec2(r0.x + tk, r0.y), alpha(kAccent), 3.0f);
					dl->AddLine(r0, ImVec2(r0.x, r0.y + tk), alpha(kAccent), 3.0f);
					dl->AddLine(r1, ImVec2(r1.x - tk, r1.y), alpha(kAccent), 3.0f);
					dl->AddLine(r1, ImVec2(r1.x, r1.y - tk), alpha(kAccent), 3.0f);
				}
				dl->AddText(sFonts.item, fs, ImVec2(colLabel, ty), alpha(sel ? kText : kTextDim, sel ? 1.0f : 0.95f), it.label.c_str());
				switch (it.type)
				{
				case ItemType::Slider:
				{
					const float bx0 = colValue, bx1 = r1.x - fs * 5.0f;
					const float by = y + rowH * 0.46f;
					const float t = (*it.fval - it.fmin) / (it.fmax - it.fmin);
					dl->AddLine(ImVec2(bx0, by), ImVec2(bx1, by), alpha(kBorder, 0.5f), 2.0f);
					dl->AddLine(ImVec2(bx0, by), ImVec2(bx0 + (bx1 - bx0) * t, by), alpha(kAccent), 3.0f);
					dl->AddCircleFilled(ImVec2(bx0 + (bx1 - bx0) * t, by), fs * 0.28f, alpha(sel ? kAccent : kText));
					char buf[64];
					snprintf(buf, sizeof(buf), it.fmt, *it.fval);
					TextAligned(dl, sFonts.item, fs, ImVec2(r1.x - fs * 0.6f, ty), alpha(kText), buf, 1.0f);
					if (hover && io.MouseDown[0] && io.MousePos.x >= bx0 - fs && io.MousePos.x <= bx1 + fs)
					{
						const float nt = std::clamp((io.MousePos.x - bx0) / (bx1 - bx0), 0.0f, 1.0f);
						float nv = it.fmin + nt * (it.fmax - it.fmin);
						nv = roundf(nv / it.fstep) * it.fstep;
						if (nv != *it.fval)
						{
							*it.fval = nv;
							Commit();
						}
					}
					break;
				}
				case ItemType::Toggle:
				{
					const char* v = *it.bval ? "ON" : "OFF";
					TextAligned(dl, sFonts.item, fs, ImVec2(r1.x - fs * 0.6f, ty), alpha(*it.bval ? kAccent : kText), v, 1.0f);
					if (sel)
						TextAligned(dl, sFonts.item, fs, ImVec2(r1.x - fs * 3.6f, ty), alpha(kTextDim), "\xE2\x97\x80", 1.0f);
					if (hover && mouseClicked)
						Adjust(it, 1);
					break;
				}
				case ItemType::Choice:
				{
					const std::string& v = it.options[std::clamp(*it.ival, 0, (int)it.options.size() - 1)];
					TextAligned(dl, sFonts.item, fs, ImVec2(r1.x - fs * 1.6f, ty), alpha(kText), v, 1.0f);
					if (sel)
					{
						const ImVec2 vs = sFonts.item->CalcTextSizeA(fs, FLT_MAX, 0, v.c_str());
						TextAligned(dl, sFonts.item, fs * 0.8f, ImVec2(r1.x - fs * 2.4f - vs.x, ty + fs * 0.1f), alpha(kAccent), "\xE2\x97\x80", 1.0f);
						TextAligned(dl, sFonts.item, fs * 0.8f, ImVec2(r1.x - fs * 0.6f, ty + fs * 0.1f), alpha(kAccent), "\xE2\x96\xB6", 1.0f);
					}
					if (hover && mouseClicked)
						Adjust(it, io.MousePos.x < r1.x - fs * 6.0f ? -1 : 1);
					break;
				}
				case ItemType::Binding:
				{
					const auto& b = sEdit.bind(it.action).keys;
					for (int slot = 0; slot < 2; slot++)
					{
						const float cx = slot == 0 ? colKey1 + (colKey2 - colKey1) * 0.5f : colKey2 + (r1.x - colKey2) * 0.5f;
						const bool editing = sRebinding && sRebindAction == it.action && sRebindSlot == slot;
						const bool conflict = KeyHasConflict(it.action, b[slot]);
						const ImU32 col = editing ? kAccent : (conflict ? kWarn : (sel ? kText : kTextDim));
						if (editing)
						{
							const float pulse = 0.55f + 0.45f * sinf((float)now * 6.0f);
							TextAligned(dl, sFonts.small, fs * 0.72f, ImVec2(cx, ty + fs * 0.12f), alpha(kAccent, pulse), "Press a key...", 0.5f);
						}
						else
							KeyCap(dl, sFonts.small, fs * 0.72f, ImVec2(cx, y + rowH * 0.46f), b[slot], alpha(col), a);
						if (sel && focused && sRebindSlot == slot && !editing)
						{
							const ImVec2 c(cx, y + rowH * 0.46f);
							dl->AddRect(ImVec2(c.x - (colKey2 - colKey1) * 0.45f, c.y - rowH * 0.38f), ImVec2(c.x + (colKey2 - colKey1) * 0.45f, c.y + rowH * 0.38f),
										alpha(kAccent, 0.6f), 3.0f);
						}
						const ImRect hr(ImVec2(cx - (colKey2 - colKey1) * 0.45f, y), ImVec2(cx + (colKey2 - colKey1) * 0.45f, y + rowH));
						if (hover && hr.Contains(io.MousePos) && mouseClicked)
						{
							sRebindSlot = slot;
							Activate(it);
						}
					}
					break;
				}
				case ItemType::Button:
					if (hover && mouseClicked)
						Activate(it);
					break;
				default:
					break;
				}
			}
			dl->PopClipRect();
			if (sPages[sPage].name == "Controls")
			{
				TextAligned(dl, sFonts.small, fs * 0.72f, ImVec2(colKey1 + (colKey2 - colKey1) * 0.5f, listTop - fs * 1.1f), alpha(kTextDim), "Primary", 0.5f);
				TextAligned(dl, sFonts.small, fs * 0.72f, ImVec2(colKey2 + (x1 - colKey2) * 0.5f, listTop - fs * 1.1f), alpha(kTextDim), "Secondary", 0.5f);
			}
		}

		std::string HelpText()
		{
			auto& items = sPages[sPage].items;
			if (items.empty() || sSel >= (int)items.size())
				return "";
			std::string help = items[sSel].help;
			if (items[sSel].type == ItemType::Binding)
			{
				for (Key kk : sEdit.bind(items[sSel].action).keys)
					if (KeyHasConflict(items[sSel].action, kk))
						help = fmt::format("Conflict: {} is also assigned to \"{}\".", KeyDisplayName(kk), ConflictName(items[sSel].action, kk));
			}
			return help;
		}

		// the key hints at the bottom, right aligned at `right`
		void DrawGuide(ImDrawList* dl, float right, float gy, float fs, float a, const std::vector<std::pair<const char*, const char*>>& guide)
		{
			auto alpha = [&](ImU32 c, float m = 1.0f) { return (c & 0x00FFFFFF) | ((uint32)(((c >> 24) & 0xFF) * a * m) << 24); };
			float gx = right;
			for (int i = (int)guide.size() - 1; i >= 0; i--)
			{
				const ImVec2 ts = sFonts.small->CalcTextSizeA(fs * 0.8f, FLT_MAX, 0, guide[i].second);
				gx -= ts.x;
				dl->AddText(sFonts.small, fs * 0.8f, ImVec2(gx, gy), alpha(kText), guide[i].second);
				const ImVec2 ks = sFonts.small->CalcTextSizeA(fs * 0.72f, FLT_MAX, 0, guide[i].first);
				const float kw = ks.x + fs * 0.8f;
				gx -= kw + fs * 0.35f;
				const ImVec2 k0(gx, gy - fs * 0.08f), k1(gx + kw, gy + fs * 0.92f);
				dl->AddRectFilled(k0, k1, alpha(IM_COL32(237, 232, 208, 235)), fs * 0.2f);
				dl->AddText(sFonts.small, fs * 0.72f, ImVec2(gx + fs * 0.4f, gy + fs * 0.05f), alpha(IM_COL32(20, 20, 20, 255)), guide[i].first);
				gx -= fs * 1.1f;
			}
		}

		std::vector<std::pair<const char*, const char*>> ListGuide()
		{
			auto& items = sPages[sPage].items;
			const bool bindingRow = !items.empty() && sSel < (int)items.size() && items[sSel].type == ItemType::Binding;
			if (sRebinding)
				return {{"Esc", "Cancel"}};
			std::vector<std::pair<const char*, const char*>> guide = {
				{"Enter", bindingRow ? "Change Key" : "Select"}, {"\xE2\x97\x80 \xE2\x96\xB6", bindingRow ? "Column" : "Change"}, {"Q / E", "Page"}, {"Esc", "Back"}};
			if (bindingRow)
				guide.insert(guide.begin() + 1, {"Del", "Clear"});
			return guide;
		}

		void DrawToast(ImDrawList* dl, float cx, float y, float fs, double now)
		{
			if (sToast.empty() || now - sToastTime >= 3.0)
				return;
			const float ta = (float)std::min(1.0, (3.0 - (now - sToastTime)) * 2.0);
			const ImVec2 ts = sFonts.small->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, sToast.c_str());
			dl->AddRectFilled(ImVec2(cx - ts.x * 0.5f - fs, y - fs * 0.5f), ImVec2(cx + ts.x * 0.5f + fs, y + ts.y + fs * 0.4f),
							  (IM_COL32(0, 0, 0, 200) & 0x00FFFFFF) | ((uint32)(200 * ta) << 24));
			dl->AddText(sFonts.small, fs * 0.85f, ImVec2(cx - ts.x * 0.5f, y - fs * 0.05f), (kText & 0x00FFFFFF) | ((uint32)(255 * ta) << 24),
						sToast.c_str());
		}

		// Motion Control (tilt): while it is on the mouse tilts the Sheikah Slate instead of turning the camera, so a badge at
		// the top shows it (it pops in when turned on); turning it off shows "OFF" fading out
		void DrawTiltIndicator(double now)
		{
			static bool last = false;
			static double changed = -10.0;
			const bool tilt = Input::IsTiltActive();
			if (tilt != last)
			{
				last = tilt;
				changed = now;
			}
			const double since = now - changed;
			if ((!tilt && since >= 1.0) || !sFonts.small)
				return;
			const Settings s = GetSettings();
			std::string text = "Motion Control OFF";
			if (tilt)
			{
				const Key k = s.bind(Action::TiltMode).keys[0];
				if (k == Key::None)
					text = "Motion Control ON";
				else if (s.tiltToggle)
					text = fmt::format("Motion Control ON  \xC2\xB7  {} to stop", KeyDisplayName(k));
				else
					text = fmt::format("Motion Control ON  \xC2\xB7  hold {}", KeyDisplayName(k));
			}
			const float a = tilt ? (float)std::min(1.0, since / 0.15) : (float)std::clamp(1.0 - since, 0.0, 1.0);
			const float pop = tilt ? 1.0f + 0.25f * (float)std::max(0.0, 1.0 - since / 0.25) : 1.0f;
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			const ImGuiIO& io = ImGui::GetIO();
			const float fs = std::max(16.0f, io.DisplaySize.y * 0.022f) * pop;
			const ImVec2 ts = sFonts.small->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
			const ImVec2 p(io.DisplaySize.x * 0.5f - ts.x * 0.5f, io.DisplaySize.y * 0.085f);
			const ImVec2 r0(p.x - fs * 0.8f, p.y - fs * 0.35f), r1(p.x + ts.x + fs * 0.8f, p.y + ts.y + fs * 0.35f);
			auto alpha = [&](ImU32 c) { return (c & 0x00FFFFFF) | ((uint32)(((c >> 24) & 0xFF) * a) << 24); };
			dl->AddRectFilled(r0, r1, alpha(IM_COL32(0, 0, 0, 180)), fs * 0.3f);
			dl->AddRect(r0, r1, alpha(tilt ? kAccent : IM_COL32(140, 140, 130, 200)), fs * 0.3f, 0, 1.5f);
			dl->AddText(sFonts.small, fs, p, alpha(tilt ? kAccent : kText), text.c_str());
		}

		void DrawQuitConfirmation(ImDrawList* dl, float W, float H, float fs)
		{
			if (!sConfirmQuit)
				return;
			dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 120));
			const ImVec2 c(W * 0.5f, H * 0.5f);
			const ImVec2 b0(c.x - fs * 12, c.y - fs * 3), b1(c.x + fs * 12, c.y + fs * 3);
			dl->AddRectFilled(b0, b1, IM_COL32(8, 10, 12, 240));
			dl->AddRect(b0, b1, kBorder, 0, 0, 1.5f);
			TextAligned(dl, sFonts.item, fs, ImVec2(c.x, c.y - fs * 1.6f), kText, "Quit to desktop?", 0.5f);
			TextAligned(dl, sFonts.small, fs * 0.8f, ImVec2(c.x, c.y - fs * 0.2f), kTextDim, "Progress since your last save will be lost.", 0.5f);
			TextAligned(dl, sFonts.small, fs * 0.8f, ImVec2(c.x, c.y + fs * 1.4f), kAccent, "Enter: Quit      Esc: Cancel", 0.5f);
		}

		// help text in at most two lines of `width`
		void DrawHelp(ImDrawList* dl, float x, float y, float width, float fs, float a, const std::string& help)
		{
			auto alpha = [&](ImU32 c, float m = 1.0f) { return (c & 0x00FFFFFF) | ((uint32)(((c >> 24) & 0xFF) * a * m) << 24); };
			dl->AddText(sFonts.small, fs * 0.8f, ImVec2(x, y), alpha(kText, 0.9f), help.c_str(), nullptr, width);
		}

		// ---- the PC settings in the System page of the game's pause menu, in place of its picture of the GamePad ----
		void EnterEmbedded()
		{
			sEdit = GetSettings();
			sFpsIndex = 0;
			for (int i = 0; i < (int)std::size(kFpsValues); i++)
				if (sEdit.fpsLimit >= kFpsValues[i])
					sFpsIndex = i;
			sLanguages = GetGameLanguages();
			sLanguageIndex = 0;
			for (int i = 0; i < (int)sLanguages.size(); i++)
				if (sEdit.language == sLanguages[i].code)
					sLanguageIndex = i;
			LoadAudioDevices();
			sPages = BuildPages(true);
			sPagesEmbedded = true;
			sPage = std::clamp(sPage, 0, (int)sPages.size() - 1);
			sSel = 0;
			sScroll = sScrollTarget = 0;
			sRebinding = false;
			sConfirmQuit = false;
			FixSelection(1);
			Input::PopOverlayWheel();
		}

		void LeaveEmbedded()
		{
			sRebinding = false;
			sConfirmQuit = false;
			Input::SetEmbeddedFocus(false);
			Input::SetPanelHover(false);
		}

		void RenderEmbedded(const Input::SystemPanel& panel, double now)
		{
			ImGuiIO& io = ImGui::GetIO();
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			// the panel is a place in the game's picture, which has bars in a window of another shape
			float X = 0.0f, Y = 0.0f, W = io.DisplaySize.x, H = io.DisplaySize.y;
			int px, py, pw, ph, ww, wh;
			if (Graphics::GetPictureArea(px, py, pw, ph, ww, wh))
			{
				const float kx = io.DisplaySize.x / (float)ww, ky = io.DisplaySize.y / (float)wh;
				X = px * kx;
				Y = py * ky;
				W = pw * kx;
				H = ph * ky;
			}
			// (the picture of the GamePad has its title at the top of this area and leaves the bottom free)
			const ImRect area(ImVec2(X + panel.x0 * W, Y + (panel.y0 + 0.115f) * H), ImVec2(X + panel.x1 * W, Y + (panel.y1 + 0.06f) * H));
			const float fs = std::min((float)sFonts.height, area.GetHeight() / 16.0f);
			const bool focused = Input::EmbeddedFocus();
			const bool hover = area.Contains(io.MousePos);
			Input::SetPanelHover(hover);
			const bool clicked = io.MouseDown[0] && !sMouseWasDown;
			if (clicked && hover && !focused)
				Input::SetEmbeddedFocus(true);
			else if (clicked && !hover && focused && !sRebinding && !sConfirmQuit)
				Input::SetEmbeddedFocus(false);

			// keys: the settings have them while focused; otherwise only the way in (Right arrow, from the menu's list)
			std::optional<Key> k;
			while ((k = Input::PopKeyPress()))
			{
				if (Input::EmbeddedFocus())
				{
					if (HandleKey(*k))
					{
						Input::SetEmbeddedFocus(false);
						break;
					}
				}
				else if (*k == Key::Right)
				{
					Input::SetEmbeddedFocus(true);
					break;
				}
			}
			const int wheel = Input::PopOverlayWheel();

			// tabs, centred where the picture's title was
			const float tabY = area.Min.y;
			DrawTabs(dl, area.Max.x, tabY, fs, 1.0f, true, true, area.GetCenter().x);
			const float headerBottom = tabY + fs * 1.9f;
			dl->AddLine(ImVec2(area.Min.x + fs, headerBottom), ImVec2(area.Max.x - fs, headerBottom), (kBorder & 0x00FFFFFF) | (90u << 24), 1.0f);
			const bool isControls = sPages[sPage].name == "Controls";
			const float listTop = headerBottom + fs * (isControls ? 1.7f : 0.6f);
			const float helpH = fs * 2.2f, guideH = fs * 1.5f;
			const ImRect list(ImVec2(area.Min.x + fs * 0.4f, listTop), ImVec2(area.Max.x - fs * 0.4f, area.Max.y - helpH - guideH - fs * 0.6f));
			DrawRows(dl, list, fs, 1.0f, !sConfirmQuit, Input::EmbeddedFocus(), (hover || Input::EmbeddedFocus()) ? wheel : 0, now);
			// help of the selected (or pointed at) row, and the keys
			const float helpY = list.Max.y + fs * 0.5f;
			dl->AddLine(ImVec2(area.Min.x + fs, helpY - fs * 0.2f), ImVec2(area.Max.x - fs, helpY - fs * 0.2f), (kBorder & 0x00FFFFFF) | (60u << 24), 1.0f);
			if (Input::EmbeddedFocus() || hover)
				DrawHelp(dl, area.Min.x + fs * 1.2f, helpY + fs * 0.2f, area.GetWidth() - fs * 2.4f, fs, 1.0f, HelpText());
			// the line below the help: the version, or that a restart is needed (in its place: the help takes two lines)
			const ImVec2 bottomLine(area.Min.x + fs * 1.2f, area.Max.y - guideH + fs * 0.1f);
			if (Graphics::RestartPending())
				TextAligned(dl, sFonts.small, fs * 0.72f, bottomLine, (kAccent & 0x00FFFFFF) | (230u << 24), "Some changes apply after restarting", 0.0f);
			else
				TextAligned(dl, sFonts.small, fs * 0.72f, bottomLine, (kTextDim & 0x00FFFFFF) | (180u << 24),
							std::string("PC Edition ") + kEditionVersion, 0.0f);
			if (Input::EmbeddedFocus())
				DrawGuide(dl, area.Max.x - fs * 0.6f, area.Max.y - guideH, fs, 1.0f, ListGuide());
			else
				DrawGuide(dl, area.Max.x - fs * 0.6f, area.Max.y - guideH, fs, 1.0f, {{"\xE2\x96\xB6", "PC Settings"}});
			DrawToast(dl, area.GetCenter().x, area.Min.y - fs * 1.4f, fs, now);
			DrawQuitConfirmation(dl, io.DisplaySize.x, io.DisplaySize.y, (float)sFonts.height);
		}

		void CloseAmiibo(bool dismissed)
		{
			sAmiiboOpen = false;
			sAmiiboDismissed = dismissed;
			Input::SetPopupOpen(false);
		}

		void TouchAmiibo(int index)
		{
			if (index < 0 || index >= (int)sAmiiboFiles.size())
				return;
			uint32 err = NFC_TOUCH_TAG_ERROR_NONE;
			if (nfc::TouchTagFromFile(sAmiiboFiles[index], &err))
			{
				cemuLog_log(LogType::Force, "PCE amiibo: {} put on the reader", _pathToUtf8(sAmiiboFiles[index].filename()));
				CloseAmiibo(false);
			}
			else
				sAmiiboError = err == NFC_TOUCH_TAG_ERROR_NO_ACCESS ? "This file cannot be opened." : "This file is not an amiibo.";
		}

		// The game's "Use amiibo" window (amiibo rune) is cancelled with its own button: after the choice was closed, the cancel
		// reaches the game too, so one press of Esc leaves both.
		void CancelAmiibo()
		{
			CloseAmiibo(true);
			Input::PressGameButtons(0x4000 /* B */, 4);
		}

		double sAmiiboWaitSince = 0.0; // when the game began to look for an amiibo
		bool sAmiiboFrameSeen = false; // the game's "Use amiibo" window was there since (the choice follows it)

		// returns true while the choice is shown (it has the input then)
		bool RenderAmiibo(double now)
		{
			const bool waiting = nnNfp_isWaitingForTag();
			if (!waiting)
			{
				if (sAmiiboOpen)
					CloseAmiibo(false);
				sAmiiboDismissed = false;
				sAmiiboWaitSince = 0.0;
				sAmiiboFrameSeen = false;
				return false;
			}
			if (sAmiiboWaitSince == 0.0)
				sAmiiboWaitSince = now;
			UI::Rect frame{};
			const bool inFrame = UI::GetAmiiboFrame(frame);
			// The choice comes and goes with the game's window. (The game keeps looking while the rune is held up after
			// the window was cancelled; the window comes back with the rune key.) Should the window never be found, the
			// choice is a box of its own after a moment.
			if (inFrame)
				sAmiiboFrameSeen = true;
			else if (sAmiiboFrameSeen)
			{
				if (sAmiiboOpen)
					CloseAmiibo(false);
				sAmiiboDismissed = false;
				return false;
			}
			if (!sAmiiboOpen && !sAmiiboDismissed && !sOpen.load() && sFonts.item && (inFrame || now - sAmiiboWaitSince > 1.5))
			{
				sAmiiboOpen = true;
				RefreshAmiiboFiles();
				sAmiiboSel = 0;
				sAmiiboError.clear();
				Input::SetPopupOpen(true);
			}
			if (!sAmiiboOpen)
				return false;
			{
				std::lock_guard lock(sAmiiboMutex);
				if (!sAmiiboAdded.empty())
				{
					RefreshAmiiboFiles();
					for (int i = 0; i < (int)sAmiiboFiles.size(); i++)
						if (sAmiiboFiles[i].filename() == sAmiiboAdded.filename())
							sAmiiboSel = i;
					sAmiiboError = sAmiiboAddError;
					sAmiiboAdded.clear();
				}
			}
			ImGuiIO& io = ImGui::GetIO();
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			const float W = io.DisplaySize.x, H = io.DisplaySize.y;
			const int count = (int)sAmiiboFiles.size();
			const bool dialog = sAmiiboDialog.load();
			std::optional<Key> k;
			while ((k = Input::PopKeyPress()))
			{
				if (dialog)
					continue;
				if (*k == Key::Escape || *k == Key::Backspace)
				{
					CancelAmiibo();
					return false;
				}
				if ((*k == Key::Up || *k == KeyChar('W')) && count)
					sAmiiboSel = (sAmiiboSel + count - 1) % count;
				else if ((*k == Key::Down || *k == KeyChar('S')) && count)
					sAmiiboSel = (sAmiiboSel + 1) % count;
				else if (*k == Key::Enter || *k == Key::Space)
				{
					if (count)
						TouchAmiibo(sAmiiboSel);
					else
						AddAmiiboFile();
				}
				if (!sAmiiboOpen)
					return false;
			}
			const int wheel = Input::PopOverlayWheel();
			if (wheel && count)
				sAmiiboSel = std::clamp(sAmiiboSel - wheel, 0, count - 1);

			// where: the frame of the game's picture (in the picture, which has bars in a window of another shape), or a box
			ImRect area;
			float fs = (float)sFonts.height;
			if (inFrame)
			{
				float X = 0.0f, Y = 0.0f, PW = W, PH = H;
				int px, py, pw, ph, ww, wh;
				if (Graphics::GetPictureArea(px, py, pw, ph, ww, wh))
				{
					const float kx = W / (float)ww, ky = H / (float)wh;
					X = px * kx;
					Y = py * ky;
					PW = pw * kx;
					PH = ph * ky;
				}
				area = ImRect(ImVec2(X + frame.x0 * PW, Y + frame.y0 * PH), ImVec2(X + frame.x1 * PW, Y + frame.y1 * PH));
				fs = std::min(fs, area.GetHeight() / 6.2f);
			}
			else
			{
				const float bw = std::max(fs * 24.0f, W * 0.34f), bh = fs * 7.0f + fs * 1.5f * 3;
				area = ImRect(ImVec2((W - bw) * 0.5f, std::min(H * 0.6f, H - bh - fs)), ImVec2((W + bw) * 0.5f, std::min(H * 0.6f, H - bh - fs) + bh));
				dl->AddRectFilled(area.Min, area.Max, IM_COL32(8, 10, 12, 235));
				dl->AddRect(area.Min, area.Max, kBorder, 0, 0, 1.5f);
			}
			const float pad = fs * 0.6f;
			const bool clicked = io.MouseDown[0] && !sMouseWasDown && !dialog;
			// line above the list
			const char* head = dialog ? "Choose the file in the window that opened." : (count ? "Choose an amiibo:" : "No amiibo files yet: add the .bin file of one.");
			TextAligned(dl, sFonts.small, fs * 0.8f, ImVec2(area.Min.x + pad, area.Min.y + pad * 0.7f), kTextDim, head);
			// rows (as many as fit, scrolled to the selection)
			const float rowH = fs * 1.45f;
			const float listTop = area.Min.y + pad * 0.7f + fs * 1.15f;
			const float btnH = fs * 1.3f;
			const float listBottom = area.Max.y - pad - btnH - (sAmiiboError.empty() ? 0.0f : fs * 0.9f) - fs * 0.3f;
			const int fit = std::max(1, (int)((listBottom - listTop) / rowH));
			const int first = std::clamp(sAmiiboSel - fit + 1, 0, std::max(0, count - fit));
			for (int i = first; i < count && i < first + fit; i++)
			{
				const float y = listTop + (i - first) * rowH;
				const ImRect r(ImVec2(area.Min.x + pad, y), ImVec2(area.Max.x - pad, y + rowH * 0.88f));
				const bool hover = r.Contains(io.MousePos) && !dialog;
				if (hover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || clicked))
					sAmiiboSel = i;
				if (i == sAmiiboSel)
				{
					dl->AddRectFilled(r.Min, r.Max, IM_COL32(248, 200, 0, 22));
					dl->AddRect(r.Min, r.Max, (kAccent & 0x00FFFFFF) | (215u << 24), 2.0f, 0, 1.5f);
				}
				dl->PushClipRect(r.Min, r.Max, true);
				TextAligned(dl, sFonts.item, fs, ImVec2(r.Min.x + fs * 0.5f, y + (rowH * 0.88f - fs) * 0.5f), i == sAmiiboSel ? kText : kTextDim,
							_pathToUtf8(sAmiiboFiles[i].stem()));
				dl->PopClipRect();
				if (hover && clicked)
				{
					TouchAmiibo(i);
					if (!sAmiiboOpen)
						return false;
				}
			}
			if (count > fit)
				TextAligned(dl, sFonts.small, fs * 0.7f, ImVec2(area.Max.x - pad, listTop - fs * 1.05f), kTextDim,
							fmt::format("{} / {}", sAmiiboSel + 1, count), 1.0f);
			if (!sAmiiboError.empty())
				TextAligned(dl, sFonts.small, fs * 0.8f, ImVec2(area.Min.x + pad, area.Max.y - pad - btnH - fs * 0.9f), kWarn, sAmiiboError);
			// buttons
			enum { kAdd, kFolder, kCancel };
			struct Btn { const char* label; int id; };
			const Btn btns[] = {{"Add amiibo File...", kAdd}, {"Open Folder", kFolder}, {"Cancel", kCancel}};
			float bx = area.Min.x + pad;
			const float by = area.Max.y - pad - btnH;
			for (const Btn& btn : btns)
			{
				const ImVec2 ts = sFonts.small->CalcTextSizeA(fs * 0.8f, FLT_MAX, 0, btn.label);
				const ImRect r(ImVec2(bx, by), ImVec2(bx + ts.x + fs * 1.1f, by + btnH));
				const bool hover = r.Contains(io.MousePos) && !dialog;
				dl->AddRectFilled(r.Min, r.Max, hover ? IM_COL32(248, 200, 0, 40) : IM_COL32(255, 255, 255, 14), fs * 0.15f);
				dl->AddRect(r.Min, r.Max, hover ? kAccent : (kBorder & 0x00FFFFFF) | (110u << 24), fs * 0.15f, 0, 1.0f);
				dl->AddText(sFonts.small, fs * 0.8f, ImVec2(bx + fs * 0.55f, by + (btnH - ts.y) * 0.5f), hover ? kText : kTextDim, btn.label);
				if (hover && clicked)
				{
					if (btn.id == kAdd)
						AddAmiiboFile();
					else if (btn.id == kFolder)
						OpenAmiiboFolder();
					else
					{
						CancelAmiibo();
						sMouseWasDown = io.MouseDown[0];
						return false;
					}
				}
				bx = r.Max.x + fs * 0.5f;
			}
			sMouseWasDown = io.MouseDown[0];
			return true;
		}
	}

	ImFont* GetTitleFont() { return sFonts.title ? sFonts.title : ImGui::GetFont(); }
	ImFont* GetItemFont() { return sFonts.item ? sFonts.item : ImGui::GetFont(); }
	ImFont* GetSmallFont() { return sFonts.small ? sFonts.small : ImGui::GetFont(); }
	float GetFontHeight() { return sFonts.height > 0 ? (float)sFonts.height : 18.0f; }

	void Render()
	{
		DrawHudPrompts();
		DrawTipKeys();
		DrawFpsCounter();
		const double now = Now();
		if (!sOpen.load())
			DrawTiltIndicator(now);
		const float dt = sLastTime > 0 ? (float)std::min(0.1, now - sLastTime) : 0.016f;
		sLastTime = now;
		if (sOpenRequest.exchange(false))
			SetOpen(true);
		if (RenderAmiibo(now))
			return;
		if (sDebugMouseX.load() >= 0.0f)
		{
			ImGuiIO& dio = ImGui::GetIO();
			dio.MousePos = ImVec2(sDebugMouseX.load(), sDebugMouseY.load());
			dio.MouseDown[0] = sDebugMouseDown.load();
		}

		// the System page of the pause menu: the PC settings in place of its picture of the GamePad
		const Input::SystemPanel panel = Input::GetSystemPanel();
		const bool embedded = !sOpen.load() && panel.shown && sFonts.item;
		if (embedded != sEmbedShown)
		{
			if (embedded)
				EnterEmbedded();
			else
				LeaveEmbedded();
			sEmbedShown = embedded;
		}
		if (embedded)
		{
			RenderEmbedded(panel, now);
			sMouseWasDown = ImGui::GetIO().MouseDown[0];
			return;
		}
		if (!sOpen.load() && sFonts.item && MapIcons::Render(now, dt))
		{
			sMouseWasDown = ImGui::GetIO().MouseDown[0];
			return;
		}
		if (!sOpen.load() && sFonts.item)
		{
			MapIcons::RenderMinimap(now, dt);
		}

		// global hotkey (works while playing): the pause menu on its System page (PC settings), or the PC menu
		if (!sOpen.load())
		{
			// handled through key presses so that rebinding mode can capture keys
			std::optional<Key> k;
			while ((k = Input::PopKeyPress()))
			{
				if (IsSettingsKey(*k) && !Input::GameMenuOpen())
				{
					Input::RequestSystemMenu();
					break;
				}
			}
		}

		const float target = sOpen.load() ? 1.0f : 0.0f;
		sAnim += (target - sAnim) * std::min(1.0f, dt * 12.0f);
		if (fabs(sAnim - target) < 0.003f)
			sAnim = target;
		if (sAnim <= 0.0f || !sFonts.item)
			return;

		ImGuiIO& io = ImGui::GetIO();
		ImDrawList* dl = ImGui::GetForegroundDrawList();
		const float W = io.DisplaySize.x, H = io.DisplaySize.y;
		const float a = sAnim;
		const float fs = (float)sFonts.height;
		auto alpha = [&](ImU32 c, float m = 1.0f) { return (c & 0x00FFFFFF) | ((uint32)(((c >> 24) & 0xFF) * a * m) << 24); };

		// ---------- input ----------
		if (sOpen.load())
		{
			std::optional<Key> k;
			while ((k = Input::PopKeyPress()))
			{
				if (HandleKey(*k))
				{
					SetOpen(false);
					break;
				}
			}
		}
		const int wheel = Input::PopOverlayWheel();

		// ---------- background ----------
		dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, (int)(150 * a)));
		const float pw = std::min(W * 0.70f, H * 1.25f), ph = H * 0.78f;
		const ImVec2 p0((W - pw) * 0.5f, (H - ph) * 0.5f + (1.0f - a) * H * 0.03f), p1(p0.x + pw, p0.y + ph);
		dl->AddRectFilled(p0, p1, alpha(kPanel));
		dl->AddRect(p0, p1, alpha(kBorder), 0, 0, 1.5f);
		dl->AddRect(ImVec2(p0.x + 6, p0.y + 6), ImVec2(p1.x - 6, p1.y - 6), alpha(kBorder, 0.35f), 0, 0, 1.0f);

		// header
		SheikahEye(dl, ImVec2(p0.x + fs * 2.0f, p0.y + fs * 1.6f), fs * 0.9f, alpha(kAccent), (float)now);
		dl->AddText(sFonts.title, fs * 1.45f, ImVec2(p0.x + fs * 3.6f, p0.y + fs * 0.85f), alpha(kText), "Settings");
		const std::string verText = std::string("PC Edition ") + kEditionVersion;
		const ImVec2 titleSize = sFonts.title->CalcTextSizeA(fs * 1.45f, FLT_MAX, 0.0f, "Settings");
		dl->AddText(sFonts.small, fs * 0.75f, ImVec2(p0.x + fs * 3.9f + titleSize.x, p0.y + fs * 1.35f), alpha(kTextDim, 0.75f), verText.c_str());
		DrawTabs(dl, p1.x - fs * 1.0f, p0.y + fs * 1.05f, fs, a, sOpen.load(), false, 0.0f);
		const float headerBottom = p0.y + fs * 3.0f;
		dl->AddLine(ImVec2(p0.x + fs, headerBottom), ImVec2(p1.x - fs, headerBottom), alpha(kBorder, 0.6f), 1.0f);

		// list
		const bool isControls = sPages[sPage].name == "Controls";
		const float listTop = headerBottom + fs * (isControls ? 1.7f : 0.6f);
		const float listBottom = p1.y - fs * 4.6f;
		DrawRows(dl, ImRect(ImVec2(p0.x + fs * 0.8f, listTop), ImVec2(p1.x - fs * 0.8f, listBottom)), fs, a, sOpen.load(), true, sOpen.load() ? wheel : 0, now);

		// description / help
		const float helpY = listBottom + fs * 0.5f;
		dl->AddLine(ImVec2(p0.x + fs, helpY), ImVec2(p1.x - fs, helpY), alpha(kBorder, 0.4f), 1.0f);
		DrawHelp(dl, p0.x + fs * 1.6f, helpY + fs * 0.5f, pw - fs * 3.2f, fs, a, HelpText());

		// guide bar; on its left the version (also in the header), or that a restart is needed (the help can take the
		// whole width)
		if (Graphics::RestartPending())
			TextAligned(dl, sFonts.small, fs * 0.72f, ImVec2(p0.x + fs * 1.6f, p1.y - fs * 1.5f), alpha(kAccent, 0.9f), "Some changes apply after restarting", 0.0f);
		else
			TextAligned(dl, sFonts.small, fs * 0.72f, ImVec2(p0.x + fs * 1.6f, p1.y - fs * 1.5f), alpha(kTextDim, 0.65f), verText, 0.0f);
		DrawGuide(dl, p1.x - fs * 1.2f, p1.y - fs * 1.7f, fs, a, ListGuide());
		DrawToast(dl, W * 0.5f, p0.y - fs * 1.2f, fs, now);
		DrawQuitConfirmation(dl, W, H, fs);
		sMouseWasDown = io.MouseDown[0];
	}
}
