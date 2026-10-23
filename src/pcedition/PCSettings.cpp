#include "pcedition/PCSettings.h"

#include <fstream>
#include <sstream>
#include <map>
#include <boost/algorithm/string.hpp>

namespace pce
{
	namespace
	{
		constexpr Key K(char c) { return KeyChar(c); }

		const ActionInfo kActions[kActionCount] = {
			{"MoveForward", "Move Forward", "Movement", {K('W'), Key::None}},
			{"MoveBack", "Move Back", "Movement", {K('S'), Key::None}},
			{"MoveLeft", "Move Left", "Movement", {K('A'), Key::None}},
			{"MoveRight", "Move Right", "Movement", {K('D'), Key::None}},
			{"Walk", "Walk (hold)", "Movement", {Key::LCtrl, Key::None}},
			{"Sprint", "Sprint / Cancel", "Movement", {Key::LShift, Key::None}},
			{"Jump", "Jump", "Movement", {Key::Space, Key::None}},
			{"Crouch", "Crouch", "Movement", {K('C'), Key::None}},
			{"Attack", "Attack", "Combat", {Key::MouseLeft, Key::None}},
			{"Interact", "Action / Interact", "Combat", {K('E'), Key::None}},
			{"Bow", "Draw Bow", "Combat", {Key::MouseRight, Key::None}},
			{"Target", "Lock On / Shield", "Combat", {K('Q'), Key::Mouse4}},
			{"UseRune", "Use Rune", "Combat", {K('F'), Key::None}},
			{"Throw", "Throw Weapon", "Combat", {K('R'), Key::None}},
			{"Scope", "Scope / Look", "Combat", {Key::MouseMiddle, K('Z')}},
			{"Whistle", "Whistle", "Combat", {K('X'), Key::None}},
			{"WeaponSelect", "Weapon Selector (hold)", "Equipment", {K('1'), Key::None}},
			{"BowSelect", "Bow Selector (hold)", "Equipment", {K('2'), Key::None}},
			{"ShieldSelect", "Shield Selector (hold)", "Equipment", {K('3'), Key::None}},
			{"RuneSelect", "Rune Selector (hold)", "Equipment", {K('4'), Key::None}},
			{"WeaponNext", "Next Weapon / Arrow", "Equipment", {Key::WheelDown, Key::None}},
			{"WeaponPrev", "Previous Weapon / Arrow", "Equipment", {Key::WheelUp, Key::None}},
			{"Inventory", "Inventory", "Menus", {Key::Tab, K('I')}},
			{"Map", "Map", "Menus", {K('M'), Key::None}},
			{"Journal", "Adventure Log (Quests)", "Menus", {K('J'), Key::None}},
			{"MenuUp", "Menu Up", "Menus", {Key::Up, Key::None}},
			{"MenuDown", "Menu Down", "Menus", {Key::Down, Key::None}},
			{"MenuLeft", "Menu Left", "Menus", {Key::Left, Key::None}},
			{"MenuRight", "Menu Right", "Menus", {Key::Right, Key::None}},
			{"Confirm", "Confirm", "Menus", {Key::Enter, Key::None}},
			{"Back", "Back", "Menus", {Key::Backspace, Key::None}},
			{"CameraUp", "Camera Up", "Camera", {Key::None, Key::None}},
			{"CameraDown", "Camera Down", "Camera", {Key::None, Key::None}},
			{"CameraLeft", "Camera Left", "Camera", {Key::None, Key::None}},
			{"CameraRight", "Camera Right", "Camera", {Key::None, Key::None}},
			{"TiltMode", "Motion Control (Tilt)", "Camera", {K('T'), Key::None}},
			{"Settings", "PC Settings", "Menus", {Key::Escape, Key::F1}},
			{"MenuPrevPage", "Inventory: Previous Page", "Menus", {Key::PageUp, Key::None}},
			{"MenuNextPage", "Inventory: Next Page", "Menus", {Key::PageDown, Key::None}},
		};

		std::mutex sMutex;
		Settings sSettings;
		std::atomic<uint64> sRevision{1};
		fs::path sPath;

		// --- tiny ini helpers ---
		using IniMap = std::map<std::string, std::map<std::string, std::string>>;

		IniMap ReadIni(const fs::path& p)
		{
			IniMap m;
			std::ifstream f(p);
			if (!f)
				return m;
			std::string line, section;
			while (std::getline(f, line))
			{
				boost::trim(line);
				if (line.empty() || line[0] == ';' || line[0] == '#')
					continue;
				if (line.front() == '[' && line.back() == ']')
				{
					section = line.substr(1, line.size() - 2);
					boost::trim(section);
					boost::to_lower(section);
					continue;
				}
				auto eq = line.find('=');
				if (eq == std::string::npos)
					continue;
				std::string k = line.substr(0, eq), v = line.substr(eq + 1);
				boost::trim(k);
				boost::trim(v);
				boost::to_lower(k);
				m[section][k] = v;
			}
			return m;
		}

		template<typename T>
		void Get(const IniMap& m, const char* sec, const char* key, T& out)
		{
			std::string s(sec), k(key);
			boost::to_lower(s);
			boost::to_lower(k);
			auto si = m.find(s);
			if (si == m.end())
				return;
			auto ki = si->second.find(k);
			if (ki == si->second.end())
				return;
			const std::string& v = ki->second;
			try
			{
				if constexpr (std::is_same_v<T, bool>)
					out = (v == "1" || boost::iequals(v, "true") || boost::iequals(v, "on") || boost::iequals(v, "yes"));
				else if constexpr (std::is_same_v<T, int>)
					out = std::stoi(v);
				else if constexpr (std::is_same_v<T, float>)
					out = std::stof(v);
				else if constexpr (std::is_same_v<T, std::string>)
					out = v;
			}
			catch (...)
			{
			}
		}
	}

	const ActionInfo& GetActionInfo(Action a)
	{
		return kActions[(size_t)a];
	}

	void Settings::ResetBindings()
	{
		for (size_t i = 0; i < kActionCount; i++)
		{
			bindings[i].keys[0] = kActions[i].defaults[0];
			bindings[i].keys[1] = kActions[i].defaults[1];
		}
	}

	void Settings::ResetAll()
	{
		std::string keepPath = gamePath;
		*this = Settings();
		gamePath = keepPath;
		ResetBindings();
	}

	Settings GetSettings()
	{
		std::lock_guard lock(sMutex);
		return sSettings;
	}

	uint64 GetSettingsRevision()
	{
		return sRevision.load();
	}

	void UpdateSettings(const std::function<void(Settings&)>& fn, bool save)
	{
		{
			std::lock_guard lock(sMutex);
			fn(sSettings);
			sRevision++;
		}
		if (save)
			SaveSettings();
	}

	fs::path GetSettingsPath()
	{
		return sPath;
	}

	void LoadSettings(const fs::path& iniPath)
	{
		sPath = iniPath;
		Settings s;
		s.ResetBindings();
		std::error_code ec;
		if (fs::exists(iniPath, ec))
		{
			IniMap m = ReadIni(iniPath);
			Get(m, "Mouse", "Sensitivity", s.mouseSensitivity);
			Get(m, "Mouse", "VerticalRatio", s.mouseSensitivityY);
			Get(m, "Mouse", "AimSensitivity", s.aimSensitivity);
			Get(m, "Mouse", "InvertX", s.invertX);
			Get(m, "Mouse", "InvertY", s.invertY);
			Get(m, "Mouse", "VelocityWindowMs", s.velocityWindowMs);
			Get(m, "Mouse", "RawInput", s.rawInput);
			Get(m, "Mouse", "KeyboardCameraSpeed", s.keyCameraSpeed);
			Get(m, "Movement", "WalkSpeed", s.walkSpeed);
			Get(m, "Movement", "ParryWithAttack", s.parryWithAttack);
			Get(m, "Movement", "ToggleSprint", s.toggleSprint);
			Get(m, "Movement", "ToggleShield", s.toggleShield);
			Get(m, "Movement", "BombKeepsBow", s.bombKeepsBow);
			Get(m, "Equipment", "WheelReleaseDelayMs", s.wheelReleaseDelayMs);
			Get(m, "Equipment", "SelectorMouseStep", s.selectorMouseStep);
			Get(m, "Equipment", "SelectorMoveKeys", s.selectorMoveKeys);
			Get(m, "Equipment", "InventoryMoveKeys", s.inventoryMoveKeys);
			Get(m, "Tilt", "Sensitivity", s.tiltSensitivity);
			Get(m, "Tilt", "InvertX", s.tiltInvertX);
			Get(m, "Tilt", "InvertY", s.tiltInvertY);
			Get(m, "Tilt", "Toggle", s.tiltToggle);
			Get(m, "Tilt", "MaxAngle", s.tiltMaxAngle);
			Get(m, "Tilt", "ReturnSpeed", s.tiltReturnSpeed);
			Get(m, "Graphics", "Resolution", s.resolutionPreset);
			Get(m, "Graphics", "AspectRatio", s.aspectRatio);
			Get(m, "Graphics", "UltrawideHUD", s.ultrawideHud);
			Get(m, "Graphics", "FPSLimit", s.fpsLimit);
			Get(m, "Graphics", "VSync", s.vsync);
			Get(m, "Graphics", "Shadows", s.shadowPreset);
			Get(m, "Graphics", "AntiAliasing", s.antiAliasing);
			Get(m, "Graphics", "FOV", s.fov);
			Get(m, "Graphics", "ShowFPS", s.showFps);
			Get(m, "Graphics", "FrameGeneration", s.frameGeneration);
			s.frameGeneration = std::clamp(s.frameGeneration, 0, 1); // 2 was the removed camera warp
			// "CPUPinning" was the key while the option was on by default: it is not read any more, so everybody starts
			// with the new default
			Get(m, "Graphics", "PinThreads", s.cpuPinning);
			bool fullscreen = true; // until 1.2.2: Fullscreen=1 was borderless, 0 windowed
			Get(m, "Display", "Fullscreen", fullscreen);
			s.displayMode = fullscreen ? 1 : 0;
			Get(m, "Display", "DisplayMode", s.displayMode);
			s.displayMode = std::clamp(s.displayMode, 0, 2);
			Get(m, "Audio", "Volume", s.volume);
			s.volume = std::clamp(s.volume, 0.0f, 100.0f);
			Get(m, "Audio", "Speakers", s.speakers);
			s.speakers = std::clamp(s.speakers, 0, 2);
			Get(m, "Audio", "Device", s.audioDevice);
			Get(m, "Game", "Path", s.gamePath);
			Get(m, "Game", "Language", s.language);
			Get(m, "Game", "TemperatureUnit", s.temperatureUnit);
			s.temperatureUnit = std::clamp(s.temperatureUnit, 0, 2);
			Get(m, "Game", "InventoryPageTurn", s.inventoryPageTurn);
			s.inventoryPageTurn = std::clamp(s.inventoryPageTurn, 0, 2);
			Get(m, "Map", "Enabled", s.mapEnabled);
			Get(m, "Map", "Overlay", s.mapOverlay);
			Get(m, "Map", "Minimap", s.mapMinimap);
			Get(m, "Map", "PanelOpen", s.mapPanelOpen);
			Get(m, "Map", "AutoHide", s.mapAutoHide);
			Get(m, "Map", "DimClaimed", s.mapDimClaimed);
			Get(m, "Map", "MasterMode", s.mapMasterMode);
			int mapCats = (int)s.mapCategories;
			Get(m, "Map", "Categories", mapCats);
			s.mapCategories = (uint32)mapCats & 0x7FFu;
			Get(m, "Mods", "GraphicPacks", s.userGraphicPacks);
			Get(m, "Mods", "Presets", s.userPackPresets);
			for (size_t i = 0; i < kActionCount; i++)
			{
				std::string v;
				Get(m, "Controls", kActions[i].id, v);
				if (v.empty())
					continue;
				std::vector<std::string> parts;
				boost::split(parts, v, boost::is_any_of(","));
				s.bindings[i].keys[0] = parts.size() > 0 ? KeyFromId(parts[0]) : Key::None;
				s.bindings[i].keys[1] = parts.size() > 1 ? KeyFromId(parts[1]) : Key::None;
			}
		}
		{
			std::lock_guard lock(sMutex);
			sSettings = s;
			sRevision++;
		}
		if (!fs::exists(iniPath, ec))
			SaveSettings();
	}

	void SaveSettings()
	{
		if (sPath.empty())
			return;
		Settings s = GetSettings();
		std::ostringstream o;
		auto b = [](bool v) { return v ? "1" : "0"; };
		o << "; The Legend of Zelda: Breath of the Wild - PC settings\n";
		o << "; This file is written by the game. You can also change everything in-game (Esc).\n\n";
		o << "[Mouse]\n";
		o << "Sensitivity=" << s.mouseSensitivity << "\n";
		o << "VerticalRatio=" << s.mouseSensitivityY << "\n";
		o << "AimSensitivity=" << s.aimSensitivity << "\n";
		o << "InvertX=" << b(s.invertX) << "\n";
		o << "InvertY=" << b(s.invertY) << "\n";
		o << "VelocityWindowMs=" << s.velocityWindowMs << "\n";
		o << "RawInput=" << b(s.rawInput) << "\n";
		o << "KeyboardCameraSpeed=" << s.keyCameraSpeed << "\n\n";
		o << "[Movement]\n";
		o << "WalkSpeed=" << s.walkSpeed << "\n";
		o << "ParryWithAttack=" << b(s.parryWithAttack) << "\n";
		o << "ToggleSprint=" << b(s.toggleSprint) << "\n";
		o << "ToggleShield=" << b(s.toggleShield) << "\n";
		o << "BombKeepsBow=" << b(s.bombKeepsBow) << "\n\n";
		o << "[Equipment]\n";
		o << "WheelReleaseDelayMs=" << s.wheelReleaseDelayMs << "\n";
		o << "SelectorMouseStep=" << s.selectorMouseStep << "\n";
		o << "SelectorMoveKeys=" << b(s.selectorMoveKeys) << "\n";
		o << "InventoryMoveKeys=" << b(s.inventoryMoveKeys) << "\n\n";
		o << "[Tilt]\n";
		o << "Sensitivity=" << s.tiltSensitivity << "\n";
		o << "InvertX=" << b(s.tiltInvertX) << "\n";
		o << "InvertY=" << b(s.tiltInvertY) << "\n";
		o << "Toggle=" << b(s.tiltToggle) << "\n";
		o << "MaxAngle=" << s.tiltMaxAngle << "\n";
		o << "ReturnSpeed=" << s.tiltReturnSpeed << "\n\n";
		o << "[Graphics]\n";
		o << "Resolution=" << s.resolutionPreset << "\n";
		o << "AspectRatio=" << s.aspectRatio << "\n";
		o << "UltrawideHUD=" << s.ultrawideHud << "\n";
		o << "FPSLimit=" << s.fpsLimit << "\n";
		o << "VSync=" << b(s.vsync) << "\n";
		o << "Shadows=" << s.shadowPreset << "\n";
		o << "AntiAliasing=" << s.antiAliasing << "\n";
		o << "FOV=" << s.fov << "\n";
		o << "ShowFPS=" << b(s.showFps) << "\n";
		o << "PinThreads=" << b(s.cpuPinning) << "\n";
		o << "FrameGeneration=" << s.frameGeneration << "\n\n";
		o << "[Display]\n";
		o << "DisplayMode=" << s.displayMode << "\n\n";
		o << "[Audio]\n";
		o << "Volume=" << s.volume << "\n";
		o << "Speakers=" << s.speakers << "\n";
		o << "Device=" << s.audioDevice << "\n\n";
		o << "[Game]\n";
		o << "Path=" << s.gamePath << "\n";
		o << "Language=" << s.language << "\n";
		o << "TemperatureUnit=" << s.temperatureUnit << "\n";
		o << "InventoryPageTurn=" << s.inventoryPageTurn << "\n\n";
		o << "[Map]\n";
		o << "Enabled=" << b(s.mapEnabled) << "\n";
		o << "Overlay=" << b(s.mapOverlay) << "\n";
		o << "Minimap=" << b(s.mapMinimap) << "\n";
		o << "PanelOpen=" << b(s.mapPanelOpen) << "\n";
		o << "AutoHide=" << b(s.mapAutoHide) << "\n";
		o << "DimClaimed=" << b(s.mapDimClaimed) << "\n";
		o << "MasterMode=" << b(s.mapMasterMode) << "\n";
		o << "Categories=" << s.mapCategories << "\n\n";
		o << "[Mods]\n";
		o << "GraphicPacks=" << s.userGraphicPacks << "\n";
		o << "Presets=" << s.userPackPresets << "\n\n";
		o << "[Controls]\n";
		for (size_t i = 0; i < kActionCount; i++)
		{
			o << kActions[i].id << "=" << KeyToId(s.bindings[i].keys[0]);
			if (s.bindings[i].keys[1] != Key::None)
				o << "," << KeyToId(s.bindings[i].keys[1]);
			o << "\n";
		}
		// write atomically
		fs::path tmp = sPath;
		tmp += ".tmp";
		{
			std::ofstream f(tmp, std::ios::trunc);
			f << o.str();
		}
		std::error_code ec;
		fs::rename(tmp, sPath, ec);
		if (ec)
		{
			fs::remove(sPath, ec);
			fs::rename(tmp, sPath, ec);
		}
	}

	std::vector<std::pair<Action, Action>> FindBindingConflicts(const Settings& s)
	{
		std::vector<std::pair<Action, Action>> r;
		for (size_t a = 0; a < kActionCount; a++)
			for (size_t b = a + 1; b < kActionCount; b++)
				for (Key ka : s.bindings[a].keys)
				{
					if (ka == Key::None)
						continue;
					for (Key kb : s.bindings[b].keys)
						if (ka == kb)
							r.emplace_back((Action)a, (Action)b);
				}
		return r;
	}
}
