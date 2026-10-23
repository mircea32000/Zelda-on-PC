#pragma once

// Zelda on PC - graphics options backed by Cemu graphic packs.

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pce::Graphics
{
	struct ResolutionOption
	{
		int width, height;
		std::string label;
	};
	// Aspect ratio the game renders in: 1 16:9, 2 16:10, 3 21:9, 4 32:9. For Settings::aspectRatio = 0 it follows the
	// main display, for the ultrawide formats only (a 16:10 display keeps 16:9 unless 16:10 is chosen).
	int EffectiveAspect(int setting);
	const std::vector<std::string>& GetAspectOptions();
	// the resolutions of an aspect ratio (same number of entries and the same heights for all of them)
	const std::vector<ResolutionOption>& GetResolutionOptions(int aspect = 1);
	// The picture inside the game window (it has bars when the window has another shape), in pixels of the window's
	// client area. false while there is no picture yet.
	bool GetPictureArea(int& x, int& y, int& width, int& height, int& windowWidth, int& windowHeight);
	const std::vector<std::string>& GetShadowOptions();
	const std::vector<std::string>& GetAAOptions();
	const std::vector<int>& GetFpsOptions();

	// copies the community packs of the game (read-only source: the user's Cemu install) and writes the PC Edition pack
	void PrepareGraphicPacks();
	// after GraphicPack2::LoadAll(): enable packs and select presets from settings.ini
	void ApplySettingsToPacks();
	// at runtime from the settings menu: re-apply live-capable options (FPS limit, AA, vsync)
	// returns true if some change requires a restart (resolution/shadows)
	bool ApplyLive();
	bool RestartPending();
	// directory of the PC Edition graphic pack (patches + content replacements)
	fs::path GetEditionPackDir();

	// mods: the graphic packs for Breath of the Wild that are not the edition's own (Graphics, FPS++, PC Edition): the
	// community packs and the player's own, with their presets
	struct UserPackPresets
	{
		std::string category;           // empty: the pack's presets have no category
		std::vector<std::string> names; // the presets shown now (the conditions of the other categories applied)
		int active = 0;                 // index into names
	};
	struct UserPack
	{
		std::string id;          // rules.txt path relative to the data folder, '/' separators (stored in settings)
		std::string name;
		std::string description;
		std::string group;       // "Enhancements", "Mods", "Cheats", "Workarounds" or "Your Mods"
		bool live = true;        // changes apply while the game runs (else at the next start)
		std::vector<UserPackPresets> presets;
	};
	std::vector<UserPack> ListUserPacks();
	// after the mods or their presets changed in the settings: applied to the running game where the pack allows it
	// (patches, shaders), else RestartPending() until the next start
	void ApplyModsLive();
	fs::path GetModsFolder();    // <data>\graphicPacks
	void OpenModsFolder();
	// presets chosen for the mods (Settings::userPackPresets): (pack id, category) -> preset
	using PackPresets = std::map<std::pair<std::string, std::string>, std::string>;
	PackPresets ParsePackPresets(const std::string& value);
	std::string JoinPackPresets(const PackPresets& presets);
	// a preset chosen on the Mods page: set on the loaded pack (the presets shown for its other categories follow their
	// conditions); the game uses it when it starts next
	void SetUserPackPreset(const std::string& id, const std::string& category, const std::string& preset);

	// display mode the main window is in (Settings::displayMode: 0 windowed, 1 borderless, 2 exclusive fullscreen); set by
	// the window, read by the swapchain (exclusive fullscreen is a property of the swapchain)
	void SetDisplayModeApplied(int mode);
	int GetDisplayModeApplied();
}
