#include "pcedition/PCLauncher.h"
#include "pcedition/PCEdition.h"
#include "pcedition/PCSettings.h"
#include "pcedition/PCGraphics.h"
#include "pcedition/PCPrompts.h"
#include "pcedition/PCDevBridge.h"

#if BOOST_OS_WINDOWS
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <boost/algorithm/string.hpp>
#include <boost/nowide/convert.hpp>
#include <rapidjson/document.h>

#if BOOST_OS_WINDOWS
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "dwmapi.lib")

namespace pce::Launcher
{
	namespace
	{
		constexpr int kWinW = 900;
		constexpr int kWinH = 520;

		std::mutex sStateMutex;
		UpdateState sState;
		std::atomic<bool> sCheckRunning{false};
		std::atomic<bool> sDownloadRunning{false};

		// Launcher window state
		enum class ViewMode : int
		{
			Main = 0,
			Options = 1,
			Mods = 2
		};

		HWND sHwnd = nullptr;
		std::atomic<bool> sLauncherOpen{false};
		std::atomic<bool> sShouldClose{false};
		std::atomic<bool> sResultPlay{true};
		std::atomic<int> sViewMode{(int)ViewMode::Main};

		// Backbuffer (32-bit top-down DIB)
		std::mutex sRenderMutex;
		HBITMAP sDib = nullptr;
		HDC sMemDC = nullptr;
		uint32* sPixels = nullptr;
		HFONT sFontTitle = nullptr;
		HFONT sFontSub = nullptr;
		HFONT sFontBtn = nullptr;
		HFONT sFontBody = nullptr;
		HFONT sFontSmall = nullptr;

		int sMouseX = -1;
		int sMouseY = -1;
		bool sMouseClicked = false;
		int sScrollOffset = 0;

		struct ClickRect
		{
			RECT rc;
			std::string id;
			int index = -1;
			int dir = 0;
		};
		std::vector<ClickRect> sHitRects;

		std::array<int, 3> ParseVersion(const std::string& v)
		{
			std::array<int, 3> out{0, 0, 0};
			std::string clean = v;
			boost::trim(clean);
			if (!clean.empty() && (clean[0] == 'v' || clean[0] == 'V'))
				clean.erase(0, 1);
			std::vector<std::string> parts;
			boost::split(parts, clean, boost::is_any_of(".-"));
			for (size_t i = 0; i < std::min<size_t>(3, parts.size()); i++)
			{
				try
				{
					out[i] = std::stoi(parts[i]);
				}
				catch (...)
				{
				}
			}
			return out;
		}

		bool IsNewerVersion(const std::string& candidate, const std::string& current)
		{
			return ParseVersion(candidate) > ParseVersion(current);
		}

		bool IsHexString64(const std::string& s)
		{
			if (s.size() != 64)
				return false;
			for (char c : s)
			{
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
					return false;
			}
			return true;
		}

		std::string ExtractSha256FromText(const std::string& text)
		{
			for (size_t i = 0; i + 64 <= text.size(); i++)
			{
				std::string sub = text.substr(i, 64);
				if (IsHexString64(sub))
				{
					bool leftBoundary = (i == 0) || !std::isalnum((unsigned char)text[i - 1]);
					bool rightBoundary = (i + 64 == text.size()) || !std::isalnum((unsigned char)text[i + 64]);
					if (leftBoundary && rightBoundary)
						return boost::to_lower_copy(sub);
				}
			}
			return {};
		}

		bool HttpGetString(const std::wstring& host, const std::wstring& path, std::string& outBody)
		{
			HINTERNET hSession = WinHttpOpen(L"ZeldaOnPC-Updater/1.4", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!hSession)
				return false;
			WinHttpSetTimeouts(hSession, 6000, 6000, 10000, 10000);
			HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
			if (!hConnect)
			{
				WinHttpCloseHandle(hSession);
				return false;
			}
			HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
			if (!hRequest)
			{
				WinHttpCloseHandle(hConnect);
				WinHttpCloseHandle(hSession);
				return false;
			}
			LPCWSTR headers = L"Accept: application/vnd.github+json\r\nUser-Agent: ZeldaOnPC-Updater/1.4\r\n";
			bool ok = false;
			if (WinHttpSendRequest(hRequest, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
				WinHttpReceiveResponse(hRequest, nullptr))
			{
				DWORD statusCode = 0;
				DWORD sz = sizeof(statusCode);
				WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
				if (statusCode == 200)
				{
					outBody.clear();
					DWORD avail = 0;
					while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0)
					{
						std::string chunk(avail, '\0');
						DWORD read = 0;
						if (!WinHttpReadData(hRequest, chunk.data(), avail, &read))
							break;
						chunk.resize(read);
						outBody.append(chunk);
					}
					ok = !outBody.empty();
				}
			}
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return ok;
		}

		struct Sha256Hasher
		{
			BCRYPT_ALG_HANDLE hAlg = nullptr;
			BCRYPT_HASH_HANDLE hHash = nullptr;
			std::vector<uint8> objBuf;

			bool Init()
			{
				if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
					return false;
				DWORD objLen = 0, cbData = 0;
				if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cbData, 0) != 0)
					return false;
				objBuf.resize(objLen);
				if (BCryptCreateHash(hAlg, &hHash, objBuf.data(), objLen, nullptr, 0, 0) != 0)
					return false;
				return true;
			}

			void Update(const void* data, size_t len)
			{
				if (hHash && len > 0)
					BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0);
			}

			std::string FinalHex()
			{
				if (!hHash)
					return {};
				uint8 digest[32]{};
				if (BCryptFinishHash(hHash, digest, sizeof(digest), 0) != 0)
					return {};
				static const char* kHex = "0123456789abcdef";
				std::string out;
				out.reserve(64);
				for (uint8 b : digest)
				{
					out.push_back(kHex[b >> 4]);
					out.push_back(kHex[b & 0x0F]);
				}
				return out;
			}

			~Sha256Hasher()
			{
				if (hHash)
					BCryptDestroyHash(hHash);
				if (hAlg)
					BCryptCloseAlgorithmProvider(hAlg, 0);
			}
		};

		void DownloadAndInstallWorker()
		{
			UpdateState snap = GetUpdateState();
			if (snap.downloadUrl.empty() || snap.assetName.empty())
			{
				sDownloadRunning = false;
				return;
			}

			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Downloading;
				sState.downloadedBytes = 0;
				sState.progress = 0.0f;
				sState.statusText = fmt::format("Downloading {}...", sState.latestVersion);
			}

			std::error_code ec;
			fs::path tempDir = fs::temp_directory_path(ec);
			if (tempDir.empty())
				tempDir = GetDataDir();
			fs::path partPath = tempDir / (snap.assetName + ".part");
			fs::path finalPath = tempDir / snap.assetName;
			fs::remove(partPath, ec);

			Sha256Hasher hasher;
			if (!hasher.Init())
			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Error;
				sState.statusText = "Could not initialize SHA-256 verifier";
				sDownloadRunning = false;
				return;
			}

			bool downloadOk = false;

			// Support local file path for deterministic dev testing
			if (!boost::istarts_with(snap.downloadUrl, "http://") && !boost::istarts_with(snap.downloadUrl, "https://"))
			{
				fs::path srcPath = _utf8ToPath(snap.downloadUrl);
				std::ifstream in(srcPath, std::ios::binary);
				std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
				if (in && out)
				{
					uint64 total = snap.totalBytes > 0 ? snap.totalBytes : (uint64)fs::file_size(srcPath, ec);
					std::vector<char> buf(64 * 1024);
					uint64 done = 0;
					while (in.good())
					{
						in.read(buf.data(), buf.size());
						std::streamsize n = in.gcount();
						if (n <= 0)
							break;
						out.write(buf.data(), n);
						hasher.Update(buf.data(), (size_t)n);
						done += (uint64)n;
						std::lock_guard lock(sStateMutex);
						sState.downloadedBytes = done;
						sState.totalBytes = total;
						sState.progress = total > 0 ? (float)done / (float)total : 0.5f;
					}
					out.close();
					downloadOk = true;
				}
			}
			else
			{
				std::wstring wUrl = boost::nowide::widen(snap.downloadUrl);
				URL_COMPONENTS uc{};
				uc.dwStructSize = sizeof(uc);
				wchar_t hostBuf[256]{};
				wchar_t pathBuf[2048]{};
				uc.lpszHostName = hostBuf;
				uc.dwHostNameLength = (DWORD)std::size(hostBuf);
				uc.lpszUrlPath = pathBuf;
				uc.dwUrlPathLength = (DWORD)std::size(pathBuf);

				if (WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.size(), 0, &uc))
				{
					HINTERNET hSession = WinHttpOpen(L"ZeldaOnPC-Updater/1.4", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
					if (hSession)
					{
						WinHttpSetTimeouts(hSession, 8000, 8000, 30000, 30000);
						HINTERNET hConnect = WinHttpConnect(hSession, hostBuf, uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT, 0);
						if (hConnect)
						{
							DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
							HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathBuf, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
							if (hRequest)
							{
								if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
									WinHttpReceiveResponse(hRequest, nullptr))
								{
									DWORD statusCode = 0, sz = sizeof(statusCode);
									WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
									if (statusCode == 200)
									{
										std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
										if (out)
										{
											uint64 done = 0;
											uint64 total = snap.totalBytes;
											DWORD avail = 0;
											std::vector<char> buf(64 * 1024);
											while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0)
											{
												DWORD toRead = std::min<DWORD>(avail, (DWORD)buf.size());
												DWORD read = 0;
												if (!WinHttpReadData(hRequest, buf.data(), toRead, &read) || read == 0)
													break;
												out.write(buf.data(), read);
												hasher.Update(buf.data(), read);
												done += read;
												{
													std::lock_guard lock(sStateMutex);
													sState.downloadedBytes = done;
													if (total > 0)
														sState.progress = std::clamp((float)done / (float)total, 0.0f, 1.0f);
													int pct = (int)std::lround(sState.progress * 100.0f);
													sState.statusText = fmt::format("Downloading {}... {}% ({:.1f} / {:.1f} MB)",
																					sState.latestVersion, pct,
																					done / (1024.0 * 1024.0),
																					total / (1024.0 * 1024.0));
												}
											}
											out.close();
											downloadOk = done > 0 && (total == 0 || done == total);
										}
									}
								}
								WinHttpCloseHandle(hRequest);
							}
							WinHttpCloseHandle(hConnect);
						}
						WinHttpCloseHandle(hSession);
					}
				}
			}

			if (!downloadOk)
			{
				fs::remove(partPath, ec);
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Error;
				sState.statusText = "Update download failed";
				sDownloadRunning = false;
				return;
			}

			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Verifying;
				sState.progress = 1.0f;
				sState.statusText = "Verifying update integrity (SHA-256)...";
			}

			std::string actualSha = hasher.FinalHex();
			if (!snap.expectedSha256.empty() && !boost::iequals(actualSha, snap.expectedSha256))
			{
				fs::remove(partPath, ec);
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Error;
				sState.statusText = "Update verification failed (SHA-256 mismatch)";
				cemuLog_log(LogType::Force, "PCE updater: SHA-256 mismatch! expected={}, actual={}", snap.expectedSha256, actualSha);
				sDownloadRunning = false;
				return;
			}

			fs::remove(finalPath, ec);
			fs::rename(partPath, finalPath, ec);
			if (ec)
			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Error;
				sState.statusText = "Could not finalize update installer file";
				sDownloadRunning = false;
				return;
			}

			// In dev mode with mock_release.json, record the silent install command for verification instead of overwriting the dev binary
			if (IsDevMode() && fs::exists(GetDataDir() / "mock_release.json", ec))
			{
				std::ofstream verifyOut(GetDataDir() / "mock_install_verified.txt", std::ios::trunc);
				verifyOut << "installer=" << _pathToUtf8(finalPath) << "\n";
				verifyOut << "sha256=" << actualSha << "\n";
				verifyOut << "args=/VERYSILENT /SUPPRESSMSGBOXES /NORESTART \"/DIR=" << _pathToUtf8(GetExeDir()) << "\"\n";
				verifyOut.close();

				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Installing;
				sState.statusText = fmt::format("Update {} verified and ready to install", sState.latestVersion);
				sDownloadRunning = false;
				return;
			}

			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Installing;
				sState.statusText = fmt::format("Installing {} and restarting...", sState.latestVersion);
			}

			wchar_t exeBuf[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
			fs::path exePath(exeBuf);
			fs::path exeDir = GetExeDir();
			fs::path cmdPath = tempDir / "zelda_on_pc_update.cmd";

			{
				std::ofstream cmd(cmdPath, std::ios::trunc);
				cmd << "@echo off\r\n";
				cmd << "timeout /t 1 /nobreak >nul\r\n";
				cmd << "\"" << _pathToUtf8(finalPath) << "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART \"/DIR=" << _pathToUtf8(exeDir) << "\"\r\n";
				cmd << "del /f /q \"" << _pathToUtf8(finalPath) << "\" >nul 2>&1\r\n";
				cmd << "start \"\" \"" << _pathToUtf8(exePath) << "\"\r\n";
				cmd << "del \"%~f0\" >nul 2>&1\r\n";
			}

			std::wstring cmdLine = L"cmd.exe /c \"" + cmdPath.wstring() + L"\"";
			STARTUPINFOW si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION pi{};
			if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, exeDir.wstring().c_str(), &si, &pi))
			{
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
				sDownloadRunning = false;
				if (sLauncherOpen.load())
				{
					sResultPlay = false;
					sShouldClose = true;
				}
				else
				{
					std::exit(0);
				}
				return;
			}

			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Error;
				sState.statusText = "Could not launch silent update installer";
			}
			sDownloadRunning = false;
		}

		void CheckWorker(bool forceManual)
		{
			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Checking;
				sState.statusText = "Checking for updates...";
			}

			std::string jsonBody;
			std::error_code ec;
			fs::path mockPath = GetDataDir() / "mock_release.json";
			if (IsDevMode() && fs::exists(mockPath, ec))
			{
				std::ifstream in(mockPath, std::ios::binary);
				std::ostringstream ss;
				ss << in.rdbuf();
				jsonBody = ss.str();
			}
			else
			{
				HttpGetString(L"api.github.com", L"/repos/mircea32000/Zelda-on-PC/releases/latest", jsonBody);
			}

			if (jsonBody.empty())
			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Error;
				sState.statusText = fmt::format("PC Edition {} (offline)", kEditionVersion);
				sCheckRunning = false;
				return;
			}

			rapidjson::Document doc;
			doc.Parse(jsonBody.c_str());
			if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("tag_name") || !doc["tag_name"].IsString())
			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::Error;
				sState.statusText = fmt::format("PC Edition {}", kEditionVersion);
				sCheckRunning = false;
				return;
			}

			std::string tag = doc["tag_name"].GetString();
			std::string body = (doc.HasMember("body") && doc["body"].IsString()) ? doc["body"].GetString() : "";
			std::string dlUrl;
			std::string assetName;
			std::string sha256;
			uint64 assetSize = 0;

			if (doc.HasMember("assets") && doc["assets"].IsArray())
			{
				for (const auto& a : doc["assets"].GetArray())
				{
					if (!a.IsObject() || !a.HasMember("name") || !a["name"].IsString() || !a.HasMember("browser_download_url") || !a["browser_download_url"].IsString())
						continue;
					std::string nm = a["name"].GetString();
					if (boost::iends_with(nm, ".exe"))
					{
						assetName = nm;
						dlUrl = a["browser_download_url"].GetString();
						if (a.HasMember("size") && a["size"].IsUint64())
							assetSize = a["size"].GetUint64();
						if (a.HasMember("digest") && a["digest"].IsString())
						{
							std::string dig = a["digest"].GetString();
							if (boost::istarts_with(dig, "sha256:") && dig.size() == 7 + 64)
								sha256 = boost::to_lower_copy(dig.substr(7));
						}
						if (boost::istarts_with(nm, "ZeldaOnPC-Setup-"))
							break;
					}
				}
			}

			if (sha256.empty() && !body.empty())
				sha256 = ExtractSha256FromText(body);

			if (!IsNewerVersion(tag, kEditionVersion) || dlUrl.empty())
			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::UpToDate;
				sState.latestVersion = tag;
				sState.statusText = fmt::format("Up to date ({})", kEditionVersion);
				sCheckRunning = false;
				return;
			}

			{
				std::lock_guard lock(sStateMutex);
				sState.status = UpdateStatus::UpdateAvailable;
				sState.latestVersion = tag;
				sState.downloadUrl = dlUrl;
				sState.assetName = assetName;
				sState.expectedSha256 = sha256;
				sState.totalBytes = assetSize;
				sState.downloadedBytes = 0;
				sState.progress = 0.0f;
				sState.statusText = fmt::format("Update {} available", tag);
			}

			sCheckRunning = false;

			if (GetSettings().autoUpdate || forceManual)
				TriggerDownloadAndInstall();
		}

		// ============================================================================
		// Software Renderer (100% Copyright-Safe Procedural Twilight Mountain Landscape)
		// ============================================================================

		constexpr uint32 MakeRGB(int r, int g, int b)
		{
			return 0xFF000000u | ((uint32)(r & 0xFF) << 16) | ((uint32)(g & 0xFF) << 8) | (uint32)(b & 0xFF);
		}

		uint32 LerpColor(uint32 c0, uint32 c1, float t)
		{
			t = std::clamp(t, 0.0f, 1.0f);
			int r0 = (c0 >> 16) & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = c0 & 0xFF;
			int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
			int r = (int)(r0 + (r1 - r0) * t);
			int g = (int)(g0 + (g1 - g0) * t);
			int b = (int)(b0 + (b1 - b0) * t);
			return MakeRGB(r, g, b);
		}

		void BlendPixel(int x, int y, int r, int g, int b, int a)
		{
			if ((unsigned)x >= (unsigned)kWinW || (unsigned)y >= (unsigned)kWinH || a <= 0)
				return;
			uint32& dst = sPixels[y * kWinW + x];
			if (a >= 255)
			{
				dst = MakeRGB(r, g, b);
				return;
			}
			int dr = (dst >> 16) & 0xFF;
			int dg = (dst >> 8) & 0xFF;
			int db = dst & 0xFF;
			int inv = 255 - a;
			dst = MakeRGB((r * a + dr * inv) >> 8, (g * a + dg * inv) >> 8, (b * a + db * inv) >> 8);
		}

		float SdfRoundedBox(float px, float py, float x0, float y0, float x1, float y1, float rad)
		{
			float cx = (x0 + x1) * 0.5f;
			float cy = (y0 + y1) * 0.5f;
			float hx = (x1 - x0) * 0.5f - rad;
			float hy = (y1 - y0) * 0.5f - rad;
			float qx = std::fabs(px - cx) - hx;
			float qy = std::fabs(py - cy) - hy;
			float outDist = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f));
			float inDist = std::min(std::max(qx, qy), 0.0f);
			return outDist + inDist - rad;
		}

		void FillRectAlpha(int x0, int y0, int x1, int y1, int r, int g, int b, int a, int radius = 0)
		{
			int cx0 = std::max(0, x0), cy0 = std::max(0, y0);
			int cx1 = std::min(kWinW, x1), cy1 = std::min(kWinH, y1);
			if (radius <= 0)
			{
				for (int y = cy0; y < cy1; y++)
					for (int x = cx0; x < cx1; x++)
						BlendPixel(x, y, r, g, b, a);
				return;
			}
			float fx0 = (float)x0, fy0 = (float)y0, fx1 = (float)x1, fy1 = (float)y1, frad = (float)radius;
			for (int y = cy0; y < cy1; y++)
			{
				for (int x = cx0; x < cx1; x++)
				{
					if (x >= x0 + radius + 1 && x < x1 - radius - 1 && y >= y0 + radius + 1 && y < y1 - radius - 1)
					{
						BlendPixel(x, y, r, g, b, a);
						continue;
					}
					float d = SdfRoundedBox(x + 0.5f, y + 0.5f, fx0, fy0, fx1, fy1, frad);
					float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
					if (cov > 0.0f)
						BlendPixel(x, y, r, g, b, (int)std::lround(a * cov));
				}
			}
		}

		void DrawBorderAlpha(int x0, int y0, int x1, int y1, int r, int g, int b, int a, int radius = 0, float thickness = 1.5f)
		{
			int cx0 = std::max(0, x0), cy0 = std::max(0, y0);
			int cx1 = std::min(kWinW, x1), cy1 = std::min(kWinH, y1);
			float fx0 = (float)x0, fy0 = (float)y0, fx1 = (float)x1, fy1 = (float)y1;
			float frad = (float)std::max(0, radius);
			int margin = std::max(radius + 3, (int)std::ceil(thickness) + 2);
			for (int y = cy0; y < cy1; y++)
			{
				bool yInner = (y >= y0 + margin && y < y1 - margin);
				for (int x = cx0; x < cx1; x++)
				{
					if (yInner && x >= x0 + margin && x < x1 - margin)
					{
						x = x1 - margin - 1;
						continue;
					}
					float d = SdfRoundedBox(x + 0.5f, y + 0.5f, fx0, fy0, fx1, fy1, frad);
					float outerCov = std::clamp(0.5f - d, 0.0f, 1.0f);
					float innerCov = std::clamp(0.5f + (d + thickness), 0.0f, 1.0f);
					float cov = outerCov * innerCov;
					if (cov > 0.0f)
						BlendPixel(x, y, r, g, b, (int)std::lround(a * cov));
				}
			}
		}

		float RidgeHeight(int layer, float x, float timeSec)
		{
			float u = x * 0.0045f + layer * 1.7f + timeSec * (0.002f * (layer + 1));
			switch (layer)
			{
			case 0: // Distant high peaks
				return 245.0f - 58.0f * std::sin(u * 1.1f + 0.4f) - 32.0f * std::cos(u * 2.3f - 0.8f) - 14.0f * std::sin(u * 5.1f);
			case 1: // Mid-far ridge
				return 305.0f - 44.0f * std::sin(u * 1.4f + 2.1f) - 24.0f * std::cos(u * 3.1f + 0.5f) - 10.0f * std::sin(u * 6.4f);
			case 2: // Mid-near pine ridge
				return 365.0f - 34.0f * std::cos(u * 1.2f - 1.2f) - 18.0f * std::sin(u * 2.8f + 1.7f) - 8.0f * std::cos(u * 7.2f);
			default: // Foreground hill slope
				return 430.0f - 28.0f * std::sin(u * 0.9f + 0.7f) - 12.0f * std::cos(u * 2.2f);
			}
		}

		void RenderScenicBackground(float t)
		{
			// 1. Twilight sky vertical gradient + horizon warm glow
			const uint32 skyTop = MakeRGB(10, 15, 30);
			const uint32 skyMid = MakeRGB(22, 34, 58);
			const uint32 skyLow = MakeRGB(58, 52, 78);
			const uint32 skyHor = MakeRGB(124, 82, 74);

			for (int y = 0; y < kWinH; y++)
			{
				float fy = (float)y / (float)kWinH;
				uint32 col;
				if (fy < 0.32f)
					col = LerpColor(skyTop, skyMid, fy / 0.32f);
				else if (fy < 0.58f)
					col = LerpColor(skyMid, skyLow, (fy - 0.32f) / 0.26f);
				else
					col = LerpColor(skyLow, skyHor, std::min(1.0f, (fy - 0.58f) / 0.25f));

				uint32* row = &sPixels[y * kWinW];
				for (int x = 0; x < kWinW; x++)
					row[x] = col;
			}

			// 2. Soft golden-amber sunset / moon glow on the left vista
			const float glowCx = 235.0f;
			const float glowCy = 250.0f;
			for (int y = 60; y < 400; y++)
			{
				float dy = (float)y - glowCy;
				for (int x = 0; x < 540; x++)
				{
					float dx = (float)x - glowCx;
					float d = std::sqrt(dx * dx + dy * dy);
					if (d < 220.0f)
					{
						float k = 1.0f - d / 220.0f;
						k = k * k;
						BlendPixel(x, y, 245, 190, 110, (int)(k * 68.0f));
					}
				}
			}

			// 3. Twinkling stars in the upper twilight sky
			for (int i = 0; i < 85; i++)
			{
				int sx = (i * 173 + 47) % kWinW;
				int sy = (i * 97 + 19) % 210;
				float phase = t * (1.4f + (i % 5) * 0.35f) + i * 0.9f;
				float bright = 0.45f + 0.55f * std::sin(phase);
				int a = (int)(bright * (1.0f - sy / 220.0f) * 190.0f);
				BlendPixel(sx, sy, 240, 244, 255, a);
				if (i % 4 == 0 && a > 90)
				{
					BlendPixel(sx - 1, sy, 220, 230, 255, a / 3);
					BlendPixel(sx + 1, sy, 220, 230, 255, a / 3);
					BlendPixel(sx, sy - 1, 220, 230, 255, a / 3);
					BlendPixel(sx, sy + 1, 220, 230, 255, a / 3);
				}
			}

			// 4. Four parallax mountain layers with valley mist
			const int layerColors[4][3] = {
				{36, 42, 66}, // layer 0: distant slate-violet peaks
				{25, 32, 50}, // layer 1: mid-far indigo ridge
				{16, 22, 36}, // layer 2: mid-near dark pine ridge
				{9, 13, 21}   // layer 3: foreground silhouette
			};

			for (int layer = 0; layer < 4; layer++)
			{
				const int r = layerColors[layer][0];
				const int g = layerColors[layer][1];
				const int b = layerColors[layer][2];
				for (int x = 0; x < kWinW; x++)
				{
					float ry = RidgeHeight(layer, (float)x, t);
					// Add subtle evergreen tree tops on layer 2
					if (layer == 2)
					{
						int treeIdx = x / 9;
						int treeLocal = x % 9;
						float treeH = (float)((treeIdx * 37 + 11) % 10 + 4);
						float distCenter = std::fabs(treeLocal - 4.0f) / 4.5f;
						ry -= treeH * (1.0f - distCenter);
					}
					int yFloor = (int)std::floor(ry);
					float frac = 1.0f - (ry - (float)yFloor);
					if (yFloor >= 0 && yFloor < kWinH)
						BlendPixel(x, yFloor, r, g, b, (int)std::lround(frac * 255.0f));
					int yStart = std::max(0, yFloor + 1);
					for (int y = yStart; y < kWinH; y++)
					{
						// Subtle valley mist near the bottom of distant layers
						int dy = y - yStart;
						if (layer < 3 && dy > 28)
						{
							float mist = std::min(1.0f, (dy - 28) / 70.0f) * 0.25f;
							int mr = (int)(r + (90 - r) * mist);
							int mg = (int)(g + (95 - g) * mist);
							int mb = (int)(b + (120 - b) * mist);
							sPixels[y * kWinW + x] = MakeRGB(mr, mg, mb);
						}
						else
						{
							sPixels[y * kWinW + x] = MakeRGB(r, g, b);
						}
					}
				}
			}

			// 5. Swaying foreground grass silhouettes along the bottom left (sub-pixel anti-aliased)
			for (int i = 0; i < 140; i++)
			{
				float baseX = (float)((i * 19) % 560);
				float baseY = RidgeHeight(3, baseX, t) + (float)((i * 23) % 75);
				if (baseY > kWinH + 10)
					continue;
				float bladeH = 14.0f + (float)(i % 12);
				float sway = std::sin(t * 2.1f + baseX * 0.03f + i * 0.5f) * 4.5f;
				for (int s = 0; s < (int)bladeH; s++)
				{
					float frac = (float)s / bladeH;
					float fx = baseX + sway * frac * frac;
					int gx = (int)std::floor(fx);
					float xfrac = fx - (float)gx;
					int gy = (int)(baseY - s);
					float baseA = (1.0f - frac * 0.65f) * 210.0f;
					BlendPixel(gx, gy, 18, 26, 38, (int)(baseA * (1.0f - xfrac)));
					BlendPixel(gx + 1, gy, 18, 26, 38, (int)(baseA * xfrac));
				}
			}

			// 6. Drifting golden fireflies / forest embers
			for (int i = 0; i < 36; i++)
			{
				float seed = (float)i * 17.3f;
				float fx = std::fmod(seed * 31.0f + t * (12.0f + (i % 7) * 3.0f) + std::sin(t * 1.3f + seed) * 18.0f, (float)kWinW);
				if (fx < 0) fx += kWinW;
				float fy = kWinH - std::fmod(seed * 23.0f + t * (9.0f + (i % 5) * 2.5f), (float)(kWinH - 90));
				float pulse = 0.4f + 0.6f * std::sin(t * 2.8f + seed);
				int rad = (i % 3 == 0) ? 4 : 3;
				for (int dy = -rad; dy <= rad; dy++)
				{
					for (int dx = -rad; dx <= rad; dx++)
					{
						float d = std::sqrt((float)(dx * dx + dy * dy));
						if (d <= (float)rad)
						{
							float a = (1.0f - d / (float)(rad + 0.5f)) * pulse;
							BlendPixel((int)fx + dx, (int)fy + dy, 250, 205, 85, (int)(a * 175.0f));
						}
					}
				}
			}

		}

		void DrawGdiText(const std::string& text, int x, int y, int w, int h, HFONT font, COLORREF color, UINT align = DT_LEFT | DT_VCENTER | DT_SINGLELINE)
		{
			if (!sMemDC || text.empty())
				return;
			std::wstring wtext = boost::nowide::widen(text);
			SelectObject(sMemDC, font);
			SetBkMode(sMemDC, TRANSPARENT);
			SetTextColor(sMemDC, color);
			RECT rc{x, y, x + w, y + h};
			DrawTextW(sMemDC, wtext.c_str(), (int)wtext.size(), &rc, align | DT_NOPREFIX);
		}

		bool PointInRect(int x, int y, const RECT& r)
		{
			return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
		}

		void DrawButton(const std::string& id, const std::string& label, int x, int y, int w, int h, bool primary = false)
		{
			RECT rc{x, y, x + w, y + h};
			sHitRects.push_back({rc, id, -1, 0});
			bool hover = PointInRect(sMouseX, sMouseY, rc);

			if (primary)
			{
				FillRectAlpha(x, y, x + w, y + h, hover ? 245 : 220, hover ? 195 : 170, hover ? 70 : 50, hover ? 240 : 215, 8);
				DrawBorderAlpha(x, y, x + w, y + h, 255, 228, 140, 245, 8, 1.5f);
				DrawGdiText(label, x, y, w, h, sFontBtn, RGB(14, 18, 26), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}
			else
			{
				FillRectAlpha(x, y, x + w, y + h, hover ? 45 : 24, hover ? 56 : 32, hover ? 78 : 48, hover ? 230 : 195, 8);
				DrawBorderAlpha(x, y, x + w, y + h, hover ? 235 : 135, hover ? 190 : 150, hover ? 85 : 180, hover ? 235 : 135, 8, 1.5f);
				DrawGdiText(label, x, y, w, h, sFontBtn, hover ? RGB(255, 240, 195) : RGB(225, 230, 240), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}
		}

		// ============================================================================
		// Options & Mods Helpers
		// ============================================================================

		int FpsToIndex(int fps)
		{
			const auto& opts = Graphics::GetFpsOptions();
			for (size_t i = 0; i < opts.size(); i++)
				if (opts[i] == fps)
					return (int)i;
			return 1; // 60 FPS
		}

		void StepOptionRow(int row, int dir)
		{
			Settings s = GetSettings();
			auto cycle = [&](int cur, int count) {
				if (count <= 0) return 0;
				return (cur + dir % count + count) % count;
			};

			switch (row)
			{
			case 0: // Display Mode
				s.displayMode = cycle(s.displayMode, 3);
				break;
			case 1: // Aspect Ratio
			{
				int oldAspect = s.aspectRatio;
				s.aspectRatio = cycle(s.aspectRatio, (int)Graphics::GetAspectOptions().size());
				s.resolutionPreset = Graphics::MapResolutionForAspect(oldAspect, s.resolutionPreset, s.aspectRatio);
				break;
			}
			case 2: // Resolution
			{
				int aspect = Graphics::EffectiveAspect(s.aspectRatio);
				int n = (int)Graphics::GetResolutionOptions(aspect).size();
				s.resolutionPreset = cycle(s.resolutionPreset, n);
				break;
			}
			case 3: // Frame Rate Limit
			{
				const auto& fpsOpts = Graphics::GetFpsOptions();
				int idx = cycle(FpsToIndex(s.fpsLimit), (int)fpsOpts.size());
				s.fpsLimit = fpsOpts[idx];
				break;
			}
			case 4: // Frame Generation
				s.frameGeneration = cycle(s.frameGeneration, 2);
				break;
			case 5: // V-Sync
				s.vsync = !s.vsync;
				break;
			case 6: // Shadow Quality
				s.shadowPreset = cycle(s.shadowPreset, (int)Graphics::GetShadowOptions().size());
				break;
			case 7: // Anti-Aliasing
				s.antiAliasing = cycle(s.antiAliasing, (int)Graphics::GetAAOptions().size());
				break;
			case 8: // Master Volume
				s.volume = std::clamp(std::round((s.volume + dir * 10.0f) / 10.0f) * 10.0f, 0.0f, 100.0f);
				break;
			case 9: // Language
			{
				auto langs = GetGameLanguages();
				if (!langs.empty())
				{
					int cur = 0;
					for (size_t i = 0; i < langs.size(); i++)
						if (langs[i].code == s.language)
							cur = (int)i;
					s.language = langs[cycle(cur, (int)langs.size())].code;
				}
				break;
			}
			case 10: // Automatic Updates
				s.autoUpdate = !s.autoUpdate;
				break;
			}

			UpdateSettings([&](Settings& dst) { dst = s; }, true);
			ReapplyForcedConfig();
			Prompts::LoadGuideLanguage();
			Graphics::ApplySettingsToPacks();

			if (row == 10 && s.autoUpdate && GetUpdateState().status == UpdateStatus::UpdateAvailable)
				TriggerDownloadAndInstall();
		}

		void ToggleModByIndex(int modIdx)
		{
			const auto packs = Graphics::ListUserPacks();
			if (modIdx < 0 || modIdx >= (int)packs.size())
				return;
			const std::string& targetId = packs[modIdx].id;

			Settings s = GetSettings();
			std::unordered_set<std::string> active;
			std::vector<std::string> tokens;
			boost::split(tokens, s.userGraphicPacks, boost::is_any_of(";"));
			for (const auto& t : tokens)
				if (!t.empty())
					active.insert(t);

			if (active.contains(targetId))
				active.erase(targetId);
			else
				active.insert(targetId);

			std::string joined;
			for (const auto& p : packs)
			{
				if (active.contains(p.id))
				{
					if (!joined.empty())
						joined += ";";
					joined += p.id;
				}
			}
			UpdateSettings([&](Settings& dst) { dst.userGraphicPacks = joined; }, true);
			Graphics::ApplySettingsToPacks();
		}

		// ============================================================================
		// View Rendering
		// ============================================================================

		void RenderMainView(const UpdateState& st)
		{
			// Frosted dark glass panel on the right
			FillRectAlpha(520, 0, kWinW, kWinH, 10, 14, 22, 215);
			for (int y = 0; y < kWinH; y++)
				BlendPixel(520, y, 235, 190, 80, 95);

			// Left vista title block (copyright-safe typography)
			DrawGdiText("ZELDA ON PC", 46, 40, 450, 48, sFontTitle, RGB(248, 244, 232));
			DrawGdiText(fmt::format("PC EDITION   \xE2\x80\xA2   {}", kEditionVersion), 48, 88, 450, 26, sFontSub, RGB(235, 190, 85));
			DrawGdiText("Native Mouse & Keyboard  \xE2\x80\xA2  High Frame Rate  \xE2\x80\xA2  Mod Support", 48, 118, 450, 22, sFontSmall, RGB(185, 195, 215));

			// Play time badge
			std::string playTimeStr = fmt::format("PLAY TIME   \xE2\x80\xA2   {}", GetPlayTimeSeconds() > 0 ? FormatPlayTime() : "0h 00m");
			FillRectAlpha(48, 148, 238, 176, 12, 18, 30, 190, 6);
			DrawBorderAlpha(48, 148, 238, 176, 235, 190, 85, 95, 6);
			DrawGdiText(playTimeStr, 48, 148, 190, 28, sFontSmall, RGB(238, 205, 120), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			// Count enabled mods for the Mods button badge
			const auto packs = Graphics::ListUserPacks();
			Settings s = GetSettings();
			std::unordered_set<std::string> activeMods;
			std::vector<std::string> tokens;
			boost::split(tokens, s.userGraphicPacks, boost::is_any_of(";"));
			for (const auto& tok : tokens)
				if (!tok.empty())
					activeMods.insert(tok);
			int activeCount = 0;
			for (const auto& p : packs)
				if (activeMods.contains(p.id))
					activeCount++;

			const int bx = 558;
			const int bw = 304;
			const int bh = 48;
			int by = 82;
			const int gap = 62;

			// PLAY button (or live auto-update progress bar while downloading/installing)
			if (st.status == UpdateStatus::Downloading || st.status == UpdateStatus::Verifying || st.status == UpdateStatus::Installing)
			{
				FillRectAlpha(bx, by, bx + bw, by + bh, 22, 30, 44, 230, 6);
				int fillW = (int)std::lround(std::clamp(st.progress, 0.05f, 1.0f) * (bw - 4));
				FillRectAlpha(bx + 2, by + 2, bx + 2 + fillW, by + bh - 2, 225, 175, 55, 210, 5);
				DrawBorderAlpha(bx, by, bx + bw, by + bh, 245, 200, 90, 230, 6);
				int pct = (int)std::lround(st.progress * 100.0f);
				std::string lbl = st.status == UpdateStatus::Installing
									  ? fmt::format("INSTALLING {}...", st.latestVersion)
									  : fmt::format("UPDATING {} ({}%)", st.latestVersion, pct);
				DrawGdiText(lbl, bx, by, bw, bh, sFontBtn, RGB(255, 250, 235), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}
			else
			{
				DrawButton("play", "PLAY", bx, by, bw, bh, true);
			}
			by += gap;

			DrawButton("view_options", "OPTIONS", bx, by, bw, bh, false);
			by += gap;

			std::string modsLabel = activeCount > 0 ? fmt::format("MODS  ({} ACTIVE)", activeCount) : "MODS";
			DrawButton("view_mods", modsLabel, bx, by, bw, bh, false);
			by += gap;

			DrawButton("open_mods_folder", "MODS FOLDER", bx, by, bw, bh, false);
			by += gap;

			DrawButton("exit", "EXIT", bx, by, bw, bh, false);

			// Bottom status bar across the left vista
			FillRectAlpha(0, kWinH - 42, 520, kWinH, 8, 12, 20, 195);
			for (int x = 0; x < 520; x++)
				BlendPixel(x, kWinH - 42, 235, 190, 80, 55);

			COLORREF dotCol = RGB(140, 210, 140);
			if (st.status == UpdateStatus::UpdateAvailable || st.status == UpdateStatus::Downloading || st.status == UpdateStatus::Installing)
				dotCol = RGB(245, 195, 70);
			else if (st.status == UpdateStatus::Error)
				dotCol = RGB(230, 110, 100);

			FillRectAlpha(22, kWinH - 25, 30, kWinH - 17, GetRValue(dotCol), GetGValue(dotCol), GetBValue(dotCol), 235, 4);
			DrawGdiText(st.statusText.empty() ? fmt::format("PC Edition {}", kEditionVersion) : st.statusText,
						38, kWinH - 42, 340, 42, sFontSmall, RGB(205, 212, 225));

			if (st.status == UpdateStatus::UpdateAvailable)
				DrawButton("update_now", "UPDATE NOW", 380, kWinH - 35, 124, 28, true);
		}

		void RenderOptionsView()
		{
			FillRectAlpha(40, 28, kWinW - 40, kWinH - 28, 10, 14, 22, 235, 8);
			DrawBorderAlpha(40, 28, kWinW - 40, kWinH - 28, 235, 190, 80, 120, 8);

			DrawGdiText("OPTIONS", 68, 42, 300, 36, sFontSub, RGB(245, 200, 90));
			DrawButton("view_main", "BACK", kWinW - 260, 40, 96, 36, false);
			DrawButton("play", "PLAY", kWinW - 152, 40, 96, 36, true);

			for (int x = 64; x < kWinW - 64; x++)
				BlendPixel(x, 86, 235, 190, 80, 75);

			Settings s = GetSettings();
			const char* dispModes[] = {"Windowed", "Borderless", "Fullscreen"};
			const auto& aspects = Graphics::GetAspectOptions();
			int effAspect = Graphics::EffectiveAspect(s.aspectRatio);
			const auto& resOpts = Graphics::GetResolutionOptions(effAspect);
			const auto& shadowOpts = Graphics::GetShadowOptions();
			const auto& aaOpts = Graphics::GetAAOptions();

			std::string resLabel = (s.resolutionPreset >= 0 && s.resolutionPreset < (int)resOpts.size()) ? resOpts[s.resolutionPreset].label : "1080p";
			std::string aspLabel = (s.aspectRatio >= 0 && s.aspectRatio < (int)aspects.size()) ? aspects[s.aspectRatio] : "Automatic";
			std::string shdLabel = (s.shadowPreset >= 0 && s.shadowPreset < (int)shadowOpts.size()) ? shadowOpts[s.shadowPreset] : "Medium";
			std::string aaLabel = (s.antiAliasing >= 0 && s.antiAliasing < (int)aaOpts.size()) ? aaOpts[s.antiAliasing] : "FXAA";
			std::string langLabel = "English";
			for (const auto& l : GetGameLanguages())
				if (l.code == s.language)
					langLabel = l.name;

			struct Row
			{
				const char* name;
				std::string val;
			};
			constexpr int kRowCount = 11;
			Row rows[kRowCount] = {
				{"Display Mode", dispModes[std::clamp(s.displayMode, 0, 2)]},
				{"Aspect Ratio", aspLabel},
				{"Resolution", resLabel},
				{"Frame Rate Limit", fmt::format("{} FPS", s.fpsLimit)},
				{"Frame Generation", s.frameGeneration ? "AMD FSR 3" : "Off"},
				{"V-Sync", s.vsync ? "On" : "Off"},
				{"Shadow Quality", shdLabel},
				{"Anti-Aliasing", aaLabel},
				{"Master Volume", fmt::format("{:.0f}%", s.volume)},
				{"Language", langLabel},
				{"Automatic Updates", s.autoUpdate ? "On (Hands-Free)" : "Off (Manual)"}
			};

			const int startY = 94;
			const int rowH = 35;
			for (int i = 0; i < kRowCount; i++)
			{
				int ry = startY + i * rowH;
				RECT rowRc{68, ry, kWinW - 68, ry + rowH - 4};
				bool hover = PointInRect(sMouseX, sMouseY, rowRc);
				FillRectAlpha(rowRc.left, rowRc.top, rowRc.right, rowRc.bottom, hover ? 32 : 18, hover ? 42 : 24, hover ? 60 : 36, 195, 4);
				if (hover)
					DrawBorderAlpha(rowRc.left, rowRc.top, rowRc.right, rowRc.bottom, 235, 190, 80, 140, 4);

				DrawGdiText(rows[i].name, 84, ry, 340, rowH - 4, sFontBody, RGB(225, 230, 240));

				// Left / Right step buttons
				RECT prevRc{kWinW - 360, ry + 3, kWinW - 328, ry + rowH - 7};
				RECT nextRc{kWinW - 108, ry + 3, kWinW - 76, ry + rowH - 7};
				RECT valRc{kWinW - 328, ry + 3, kWinW - 108, ry + rowH - 7};

				sHitRects.push_back({prevRc, "opt_step", i, -1});
				sHitRects.push_back({nextRc, "opt_step", i, +1});
				sHitRects.push_back({valRc, "opt_step", i, +1});

				bool hPrev = PointInRect(sMouseX, sMouseY, prevRc);
				bool hNext = PointInRect(sMouseX, sMouseY, nextRc);
				FillRectAlpha(prevRc.left, prevRc.top, prevRc.right, prevRc.bottom, hPrev ? 65 : 35, hPrev ? 78 : 44, hPrev ? 105 : 64, 220, 4);
				FillRectAlpha(nextRc.left, nextRc.top, nextRc.right, nextRc.bottom, hNext ? 65 : 35, hNext ? 78 : 44, hNext ? 105 : 64, 220, 4);

				DrawGdiText("<", prevRc.left, prevRc.top, 32, prevRc.bottom - prevRc.top, sFontBtn, RGB(245, 200, 90), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				DrawGdiText(">", nextRc.left, nextRc.top, 32, nextRc.bottom - nextRc.top, sFontBtn, RGB(245, 200, 90), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				DrawGdiText(rows[i].val, valRc.left, valRc.top, valRc.right - valRc.left, valRc.bottom - valRc.top, sFontBody, RGB(250, 225, 145), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}
		}

		void RenderModsView()
		{
			FillRectAlpha(40, 28, kWinW - 40, kWinH - 28, 10, 14, 22, 235, 8);
			DrawBorderAlpha(40, 28, kWinW - 40, kWinH - 28, 235, 190, 80, 120, 8);

			DrawGdiText("MODS  \xE2\x80\xA2  GRAPHIC PACKS", 68, 42, 340, 36, sFontSub, RGB(245, 200, 90));
			DrawButton("open_mods_folder", "OPEN FOLDER", kWinW - 412, 40, 140, 36, false);
			DrawButton("view_main", "BACK", kWinW - 260, 40, 96, 36, false);
			DrawButton("play", "PLAY", kWinW - 152, 40, 96, 36, true);

			for (int x = 64; x < kWinW - 64; x++)
				BlendPixel(x, 86, 235, 190, 80, 75);

			const auto packs = Graphics::ListUserPacks();
			Settings s = GetSettings();
			std::unordered_set<std::string> activeMods;
			std::vector<std::string> tokens;
			boost::split(tokens, s.userGraphicPacks, boost::is_any_of(";"));
			for (const auto& tok : tokens)
				if (!tok.empty())
					activeMods.insert(tok);

			if (packs.empty())
			{
				DrawGdiText("No mods found. Place graphic pack folders in the Mods Folder to enable them here.",
							68, 180, kWinW - 136, 40, sFontBody, RGB(180, 190, 205), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				return;
			}

			const int visibleRows = 9;
			int maxScroll = std::max(0, (int)packs.size() - visibleRows);
			sScrollOffset = std::clamp(sScrollOffset, 0, maxScroll);

			const int startY = 98;
			const int rowH = 40;
			for (int v = 0; v < visibleRows && (v + sScrollOffset) < (int)packs.size(); v++)
			{
				int idx = v + sScrollOffset;
				const auto& p = packs[idx];
				bool isOn = activeMods.contains(p.id);

				int ry = startY + v * rowH;
				RECT rowRc{68, ry, kWinW - 84, ry + rowH - 4};
				sHitRects.push_back({rowRc, "mod_toggle", idx, 0});

				bool hover = PointInRect(sMouseX, sMouseY, rowRc);
				FillRectAlpha(rowRc.left, rowRc.top, rowRc.right, rowRc.bottom, hover ? 32 : 18, hover ? 42 : 24, hover ? 60 : 36, 195, 4);
				if (hover || isOn)
					DrawBorderAlpha(rowRc.left, rowRc.top, rowRc.right, rowRc.bottom, 235, 190, 80, isOn ? 160 : 100, 4);

				DrawGdiText(p.name, 84, ry, 430, rowH - 4, sFontBody, isOn ? RGB(255, 240, 195) : RGB(215, 222, 232));
				DrawGdiText(p.group, 520, ry, 160, rowH - 4, sFontSmall, RGB(150, 162, 182));

				RECT pillRc{rowRc.right - 92, ry + 5, rowRc.right - 14, ry + rowH - 9};
				if (isOn)
				{
					FillRectAlpha(pillRc.left, pillRc.top, pillRc.right, pillRc.bottom, 230, 180, 55, 230, 5);
					DrawGdiText("ON", pillRc.left, pillRc.top, pillRc.right - pillRc.left, pillRc.bottom - pillRc.top, sFontBtn, RGB(14, 18, 26), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				}
				else
				{
					FillRectAlpha(pillRc.left, pillRc.top, pillRc.right, pillRc.bottom, 38, 46, 62, 210, 5);
					DrawGdiText("OFF", pillRc.left, pillRc.top, pillRc.right - pillRc.left, pillRc.bottom - pillRc.top, sFontSmall, RGB(170, 180, 195), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				}
			}

			if (maxScroll > 0)
			{
				DrawGdiText(fmt::format("Showing {}-{} of {} mods (scroll with mouse wheel)",
										sScrollOffset + 1, std::min((int)packs.size(), sScrollOffset + visibleRows), (int)packs.size()),
							68, kWinH - 58, 500, 24, sFontSmall, RGB(165, 175, 195));
			}
		}

		void HandleAction(const ClickRect& hit)
		{
			if (hit.id == "play")
			{
				sResultPlay = true;
				sShouldClose = true;
			}
			else if (hit.id == "exit")
			{
				sResultPlay = false;
				sShouldClose = true;
			}
			else if (hit.id == "view_main")
			{
				sViewMode = (int)ViewMode::Main;
			}
			else if (hit.id == "view_options")
			{
				sViewMode = (int)ViewMode::Options;
			}
			else if (hit.id == "view_mods")
			{
				sScrollOffset = 0;
				sViewMode = (int)ViewMode::Mods;
			}
			else if (hit.id == "open_mods_folder")
			{
				Graphics::OpenModsFolder();
			}
			else if (hit.id == "update_now")
			{
				TriggerDownloadAndInstall();
			}
			else if (hit.id == "opt_step")
			{
				StepOptionRow(hit.index, hit.dir);
			}
			else if (hit.id == "mod_toggle")
			{
				ToggleModByIndex(hit.index);
			}
		}

		void RenderFrame(float timeSec)
		{
			std::lock_guard lock(sRenderMutex);
			if (!sPixels || !sMemDC)
				return;

			sHitRects.clear();
			RenderScenicBackground(timeSec);

			UpdateState st = GetUpdateState();
			ViewMode vm = (ViewMode)sViewMode.load();
			if (vm == ViewMode::Options)
				RenderOptionsView();
			else if (vm == ViewMode::Mods)
				RenderModsView();
			else
				RenderMainView(st);

			GdiFlush();

			if (sMouseClicked)
			{
				sMouseClicked = false;
				for (const auto& hr : sHitRects)
				{
					if (PointInRect(sMouseX, sMouseY, hr.rc))
					{
						HandleAction(hr);
						break;
					}
				}
			}
		}

		bool WriteBackbufferBMP(const std::string& path)
		{
			std::lock_guard lock(sRenderMutex);
			if (!sPixels)
				return false;
			const int rowPitch = (kWinW * 3 + 3) & ~3;
			BITMAPFILEHEADER fh{};
			BITMAPINFOHEADER ih{};
			fh.bfType = 0x4D42;
			fh.bfOffBits = sizeof(fh) + sizeof(ih);
			fh.bfSize = fh.bfOffBits + rowPitch * kWinH;
			ih.biSize = sizeof(ih);
			ih.biWidth = kWinW;
			ih.biHeight = kWinH; // bottom-up BMP
			ih.biPlanes = 1;
			ih.biBitCount = 24;

			std::ofstream f(_utf8ToPath(path), std::ios::binary | std::ios::trunc);
			if (!f)
				return false;
			f.write((const char*)&fh, sizeof(fh));
			f.write((const char*)&ih, sizeof(ih));
			std::vector<uint8> row(rowPitch, 0);
			for (int y = kWinH - 1; y >= 0; y--)
			{
				const uint32* src = &sPixels[y * kWinW];
				for (int x = 0; x < kWinW; x++)
				{
					uint32 px = src[x];
					row[x * 3 + 0] = (uint8)(px & 0xFF);
					row[x * 3 + 1] = (uint8)((px >> 8) & 0xFF);
					row[x * 3 + 2] = (uint8)((px >> 16) & 0xFF);
				}
				f.write((const char*)row.data(), rowPitch);
			}
			return true;
		}

		LRESULT CALLBACK LauncherWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			switch (msg)
			{
			case WM_ERASEBKGND:
				return 1;
			case WM_PAINT:
			{
				PAINTSTRUCT ps;
				HDC hdc = BeginPaint(hwnd, &ps);
				{
					std::lock_guard lock(sRenderMutex);
					if (sMemDC)
						BitBlt(hdc, 0, 0, kWinW, kWinH, sMemDC, 0, 0, SRCCOPY);
				}
				EndPaint(hwnd, &ps);
				return 0;
			}
			case WM_MOUSEMOVE:
				sMouseX = GET_X_LPARAM(lParam);
				sMouseY = GET_Y_LPARAM(lParam);
				return 0;
			case WM_LBUTTONDOWN:
			{
				sMouseX = GET_X_LPARAM(lParam);
				sMouseY = GET_Y_LPARAM(lParam);
				bool overControl = false;
				{
					std::lock_guard lock(sRenderMutex);
					for (const auto& hr : sHitRects)
					{
						if (PointInRect(sMouseX, sMouseY, hr.rc))
						{
							overControl = true;
							break;
						}
					}
				}
				if (overControl)
				{
					sMouseClicked = true;
				}
				else
				{
					ReleaseCapture();
					SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
				}
				return 0;
			}
			case WM_MOUSEWHEEL:
			{
				int delta = GET_WHEEL_DELTA_WPARAM(wParam);
				if ((ViewMode)sViewMode.load() == ViewMode::Mods)
					sScrollOffset -= (delta > 0 ? 1 : -1);
				return 0;
			}
			case WM_KEYDOWN:
				if (wParam == VK_RETURN || wParam == VK_SPACE)
				{
					sResultPlay = true;
					sShouldClose = true;
				}
				else if (wParam == VK_ESCAPE)
				{
					if ((ViewMode)sViewMode.load() != ViewMode::Main)
						sViewMode = (int)ViewMode::Main;
					else
					{
						sResultPlay = false;
						sShouldClose = true;
					}
				}
				return 0;
			case WM_CLOSE:
				sResultPlay = false;
				sShouldClose = true;
				return 0;
			}
			return DefWindowProcW(hwnd, msg, wParam, lParam);
		}
	}

	void StartUpdateCheck(bool forceManual)
	{
		if (sCheckRunning.exchange(true))
			return;
		std::thread(CheckWorker, forceManual).detach();
	}

	UpdateState GetUpdateState()
	{
		std::lock_guard lock(sStateMutex);
		return sState;
	}

	void TriggerDownloadAndInstall()
	{
		if (sDownloadRunning.exchange(true))
			return;
		std::thread(DownloadAndInstallWorker).detach();
	}

	bool Run()
	{
		std::error_code ec;
		if (IsDevMode() && !fs::exists(GetDataDir() / "show_launcher", ec))
			return true;

		if (IsDevMode())
			DevBridge::Start();

		// Ensure update check is running
		if (GetUpdateState().status == UpdateStatus::Idle)
			StartUpdateCheck(false);

		HINSTANCE hInst = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = LauncherWndProc;
		wc.hInstance = hInst;
		wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
		wc.hIcon = LoadIconW(hInst, L"AAAAA_MAINICON");
		wc.lpszClassName = L"ZeldaOnPCLauncherWnd";
		RegisterClassExW(&wc);

		int scrW = GetSystemMetrics(SM_CXSCREEN);
		int scrH = GetSystemMetrics(SM_CYSCREEN);
		int winX = (scrW - kWinW) / 2;
		int winY = (scrH - kWinH) / 2;

		{
			std::lock_guard lock(sRenderMutex);
			HDC screenDC = GetDC(nullptr);
			sMemDC = CreateCompatibleDC(screenDC);
			BITMAPINFO bmi{};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = kWinW;
			bmi.bmiHeader.biHeight = -kWinH; // top-down
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;
			sDib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, (void**)&sPixels, nullptr, 0);
			SelectObject(sMemDC, sDib);
			ReleaseDC(nullptr, screenDC);

			sFontTitle = CreateFontW(-36, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
			sFontSub = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
			sFontBtn = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
			sFontBody = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
			sFontSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
		}

		sShouldClose = false;
		sResultPlay = true;
		sViewMode = (int)ViewMode::Main;

		sHwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"The Legend of Zelda: Breath of the Wild - Launcher",
								WS_POPUP | WS_VISIBLE | WS_SYSMENU | WS_MINIMIZEBOX,
								winX, winY, kWinW, kWinH, nullptr, nullptr, hInst, nullptr);

		// Enable Windows 11 rounded corners and subtle warm gold-bronze DWM border
		DWORD cornerPref = 2; // DWMWCP_ROUND
		DwmSetWindowAttribute(sHwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
		COLORREF borderCol = RGB(72, 64, 48);
		DwmSetWindowAttribute(sHwnd, 34 /* DWMWA_BORDER_COLOR */, &borderCol, sizeof(borderCol));

		sLauncherOpen = true;
		ShowWindow(sHwnd, SW_SHOW);
		UpdateWindow(sHwnd);

		auto t0 = std::chrono::steady_clock::now();
		while (!sShouldClose.load())
		{
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT)
				{
					sResultPlay = false;
					sShouldClose = true;
					break;
				}
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			if (sShouldClose.load())
				break;

			float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
			RenderFrame(elapsed);
			InvalidateRect(sHwnd, nullptr, FALSE);
			UpdateWindow(sHwnd);
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}

		sLauncherOpen = false;
		if (sHwnd)
		{
			DestroyWindow(sHwnd);
			sHwnd = nullptr;
		}

		{
			std::lock_guard lock(sRenderMutex);
			if (sFontTitle) DeleteObject(sFontTitle);
			if (sFontSub) DeleteObject(sFontSub);
			if (sFontBtn) DeleteObject(sFontBtn);
			if (sFontBody) DeleteObject(sFontBody);
			if (sFontSmall) DeleteObject(sFontSmall);
			sFontTitle = sFontSub = sFontBtn = sFontBody = sFontSmall = nullptr;
			if (sMemDC) DeleteDC(sMemDC);
			if (sDib) DeleteObject(sDib);
			sMemDC = nullptr;
			sDib = nullptr;
			sPixels = nullptr;
		}

		return sResultPlay.load();
	}

	std::string DebugCommand(const std::vector<std::string>& a, const std::string& rawLine)
	{
		if (a.size() < 2 || a[1] == "status")
		{
			UpdateState st = GetUpdateState();
			const char* stNames[] = {"idle", "checking", "up_to_date", "update_available", "downloading", "verifying", "installing", "error"};
			const char* vmNames[] = {"main", "options", "mods"};
			return fmt::format("open={} view={} status={} version={} latest={} playtime={} progress={:.2f} text={}",
							   sLauncherOpen.load() ? 1 : 0,
							   vmNames[std::clamp(sViewMode.load(), 0, 2)],
							   stNames[(int)st.status],
							   kEditionVersion,
							   st.latestVersion.empty() ? "-" : st.latestVersion,
							   FormatPlayTime(),
							   st.progress,
							   st.statusText);
		}
		if (a[1] == "view" && a.size() >= 3)
		{
			if (a[2] == "options") sViewMode = (int)ViewMode::Options;
			else if (a[2] == "mods") sViewMode = (int)ViewMode::Mods;
			else sViewMode = (int)ViewMode::Main;
			return "ok";
		}
		if (a[1] == "shot" && a.size() >= 3)
		{
			size_t pos = rawLine.find("shot");
			std::string path = rawLine.substr(pos + 4);
			boost::trim(path);
			return WriteBackbufferBMP(path) ? "ok" : "err write";
		}
		if (a[1] == "check")
		{
			StartUpdateCheck(a.size() >= 3 && a[2] == "force");
			return "ok";
		}
		if (a[1] == "update")
		{
			TriggerDownloadAndInstall();
			return "ok";
		}
		if (a[1] == "play")
		{
			sResultPlay = true;
			sShouldClose = true;
			return "ok";
		}
		if (a[1] == "exit")
		{
			sResultPlay = false;
			sShouldClose = true;
			return "ok";
		}
		return "err launcher cmd";
	}
}
#else
namespace pce::Launcher
{
	void StartUpdateCheck(bool) {}
	UpdateState GetUpdateState()
	{
		UpdateState st;
		st.status = UpdateStatus::Idle;
		st.statusText = "Download new Linux releases from GitHub Releases";
		return st;
	}
	void TriggerDownloadAndInstall() {}
	bool Run() { return true; }
	std::string DebugCommand(const std::vector<std::string>&, const std::string&) { return "unsupported on linux"; }
}
#endif
