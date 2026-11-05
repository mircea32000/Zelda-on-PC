#pragma once

// BotW PC Edition - pre-game launcher window (Play / Options / Mods / Exit) and GitHub auto-updater.

#include <string>
#include <vector>

namespace pce::Launcher
{
	enum class UpdateStatus
	{
		Idle,
		Checking,
		UpToDate,
		UpdateAvailable,
		Downloading,
		Verifying,
		Installing,
		Error
	};

	struct UpdateState
	{
		UpdateStatus status = UpdateStatus::Idle;
		std::string latestVersion;
		std::string downloadUrl;
		std::string assetName;
		std::string expectedSha256;
		std::string statusText;
		uint64 downloadedBytes = 0;
		uint64 totalBytes = 0;
		float progress = 0.0f;
	};

	// Starts an asynchronous GitHub release check against mircea32000/Zelda-on-PC.
	void StartUpdateCheck(bool forceManual = false);

	// Thread-safe snapshot of the current update check / download state.
	UpdateState GetUpdateState();

	// Starts downloading and silently installing the available update.
	void TriggerDownloadAndInstall();

	// Runs the pre-game launcher window. Returns true if the user clicked Play, false to exit the process.
	// In --pce-dev mode, returns true immediately unless <data_dev>/show_launcher exists.
	bool Run();

	// Developer bridge commands ("launcher status", "launcher shot <bmp>", "launcher view main|options|mods", "launcher play", "launcher exit", "launcher update").
	std::string DebugCommand(const std::vector<std::string>& args, const std::string& rawLine);
}
