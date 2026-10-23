#pragma once

// Zelda on PC - top level integration with Cemu.

#include <set>
#include <string>
#include <vector>

namespace pce
{
	constexpr const char* kGameTitle = "The Legend of Zelda: Breath of the Wild";
	constexpr const char* kEditionVersion = "v1.3.4";
	// the game code this edition is written for: update 1.5.0 (title version 208, the same executable in all regions)
	constexpr uint32 kRequiredUpdateVersion = 208;

	// command line: --cemu-ui (plain Cemu UI for debugging), --pce-dev (developer bridge + verbose log)
	void ParseCommandLine(const std::vector<std::wstring>& args);
	// Command line tools that do their job and end the process without starting the game. Returns the exit code, or
	// nothing if the command line asks for none of them.
	//   --extract-icon <game folder> <out.ico>   used by the installer: icon for the shortcuts (see PCIcon.h)
	//   --check-game <game folder>               used by the installer: what is in that folder. The exit code is a
	//                                            set of bits: 1 base game, 2 update, 4 the update is 1.5.0, 8 DLC
	std::optional<int> RunCommandLineTool(const std::vector<std::wstring>& args);
	bool IsActive();   // PC Edition mode (kiosk, native input, ...)
	bool IsDevMode();
	bool ForceWindowed(); // --windowed (testing): start windowed without changing settings.ini

	// paths: <exe>/data holds all writable state (Cemu config, mlc, caches, graphic packs) + <exe>/settings.ini
	fs::path GetExeDir();
	fs::path GetDataDir();
	void DeterminePaths(const fs::path& exePath, std::set<fs::path>& failedWriteAccess);

	// called once after Cemu's config has been loaded (before CemuCommonInit)
	void InitBeforeCemu();
	// re-applies the forced Cemu configuration (Cemu reloads settings.xml during init)
	void ReapplyForcedConfig();
	// called after the main window exists
	void InitAfterWindow(void* mainHwnd);
	void Shutdown();

	// game location: the player's own dump, as title folders (code / content / meta) or as a Wii U archive (.wua, which
	// usually holds base game, update and DLC in one file). The titles may sit anywhere below the configured folder,
	// next to the base game, or in the title folder of an existing Cemu installation.
	fs::path GetGameRoot();        // configured folder (or .wua file)
	fs::path GetBaseTitlePath();   // base game: title folder, or the archive / disc image that holds it (what Cemu is told to launch)
	// a disc image (.wud / .wux) that was found while looking for the game and could not be opened: its key is missing
	fs::path GetDiscImageWithoutKey();
	fs::path GetUpdateTitlePath(); // update: title folder or archive (empty if not found)
	fs::path GetAocTitlePath();    // DLC: title folder or archive (empty if not found)
	// archives that have to be made known to Cemu's title list before the launch (update / DLC in an archive)
	std::vector<fs::path> GetGameArchives();
	// A file of the game the way the console sees it: from the update if it has the file, else from the base game.
	// `rel` is relative to the title ("content/Font/Font_EU.sbfarc"). Works for title folders and archives.
	bool ReadGameFile(std::string_view rel, std::vector<uint8>& out);
	// names the file ReadGameFile would return, with its size (for caches); empty if there is none
	std::string GameFileStamp(std::string_view rel);
	bool IsGameAvailable();

	// Text languages the player's copy of the game has (it brings one message pack per language: the European copy
	// seven, the American three, the Japanese one). The game shows the one the console is set to.
	struct GameLanguage
	{
		const char* code; // settings value ("en", "fr", ...)
		const char* name; // English name
		int console;      // CafeConsoleLanguage
	};
	std::vector<GameLanguage> GetGameLanguages();
	uint32 GetUpdateVersion();     // title version of the update that was found (0 if none)

	// fatal startup error shown in a minimal native message box (only used if the game cannot start at all)
	void FatalError(const std::string& message);
	void RequestExit();
	void SetExitCallback(void (*cb)());
}
