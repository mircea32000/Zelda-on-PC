#include "Cafe/OS/libs/nn_nfp/nn_nfp.h"
#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "config/CemuConfig.h"
#include "pcedition/PCDevBridge.h"
#include "pcedition/PCInput.h"
#include "pcedition/PCCamera.h"
#include "pcedition/PCOverlay.h"
#include "pcedition/PCUI.h"
#include "pcedition/PCMenus.h"
#include "pcedition/PCProfiler.h"
#include "pcedition/PCGraphics.h"
#include "pcedition/PCMapIcons.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif
#include <sstream>
#include <map>
#include <cmath>
#include <thread>
#include <boost/algorithm/string.hpp>

#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include <fstream>

#pragma comment(lib, "Ws2_32.lib")

void LatteFrameGen_GetStats(float& baseFps, float& shownFps, bool& generating); // LatteRenderTarget.cpp
void LatteFrameGen_GetPacing(double& meanMs, double& stdMs, double& p95Ms);
void LatteFrameTrace_Request(const std::string& path);
std::string LatteFSR_Command(const std::vector<std::string>& a);

#if !BOOST_OS_WINDOWS
#pragma pack(push, 2)
struct BITMAPFILEHEADER {
	uint16 bfType;
	uint32 bfSize;
	uint16 bfReserved1;
	uint16 bfReserved2;
	uint32 bfOffBits;
};
#pragma pack(pop)

struct BITMAPINFOHEADER {
	uint32 biSize;
	int32_t biWidth;
	int32_t biHeight;
	uint16 biPlanes;
	uint16 biBitCount;
	uint32 biCompression;
	uint32 biSizeImage;
	int32_t biXPelsPerMeter;
	int32_t biYPelsPerMeter;
	uint32 biClrUsed;
	uint32 biClrImportant;
};
#endif

namespace pce::DevBridge
{
	namespace
	{
		std::thread sThread;
		std::atomic<bool> sQuit{false};
		SOCKET sListen = INVALID_SOCKET;

		void WriteBMP(const std::string& path, const std::vector<uint8>& rgb, int w, int h)
		{
			const int rowPitch = (w * 3 + 3) & ~3;
			BITMAPFILEHEADER fh{};
			BITMAPINFOHEADER ih{};
			fh.bfType = 0x4d42;
			fh.bfOffBits = sizeof(fh) + sizeof(ih);
			fh.bfSize = fh.bfOffBits + rowPitch * h;
			ih.biSize = sizeof(ih);
			ih.biWidth = w;
			ih.biHeight = h;
			ih.biPlanes = 1;
			ih.biBitCount = 24;
			std::ofstream f(_utf8ToPath(path), std::ios::binary | std::ios::trunc);
			f.write((const char*)&fh, sizeof(fh));
			f.write((const char*)&ih, sizeof(ih));
			std::vector<uint8> row(rowPitch, 0);
			for (int y = h - 1; y >= 0; y--)
			{
				const uint8* src = rgb.data() + (size_t)y * w * 3;
				for (int x = 0; x < w; x++)
				{
					row[x * 3 + 0] = src[x * 3 + 2];
					row[x * 3 + 1] = src[x * 3 + 1];
					row[x * 3 + 2] = src[x * 3 + 0];
				}
				f.write((const char*)row.data(), rowPitch);
			}
		}

		// memory snapshots for reverse engineering (snap / sdiff / sfind)
		struct Snapshot
		{
			uint32 base = 0;
			std::vector<uint8> data;
		};
		std::map<int, Snapshot> sSnaps;

		void TakeSnapshot(Snapshot& sn, uint32 lo, uint32 size)
		{
			sn.base = lo;
			sn.data.assign(size, 0);
			for (uint32 off = 0; off < size; off += 0x1000)
			{
				const uint32 n = std::min<uint32>(0x1000, size - off);
				if (memory_isAddressRangeAccessible(lo + off, n))
					memcpy(sn.data.data() + off, memory_getPointerFromVirtualOffset(lo + off), n);
			}
		}

		// "scan": find a byte that follows a game state (like a memory scanner): snapshot in state A, keep the bytes that
		// differ in state B, then filter while switching between the states
		struct ScanCandidate
		{
			uint32 addr;
			uint8 base, state;
		};
		Snapshot sScanBase;
		std::vector<ScanCandidate> sScan;

		const uint8* ScanLive(uint32 addr)
		{
			return memory_isAddressRangeAccessible(addr & ~0xFFFu, 0x1000) ? (const uint8*)memory_getPointerFromVirtualOffset(addr) : nullptr;
		}

		std::string ScanCommand(const std::vector<std::string>& a)
		{
			auto num = [&](size_t i) { return i < a.size() ? (uint32)std::stoul(a[i], nullptr, 0) : 0u; };
			if (a.size() >= 4 && a[1] == "base")
			{
				TakeSnapshot(sScanBase, num(2) & ~0xFFFu, num(3) & ~0xFFFu);
				sScan.clear();
				return fmt::format("ok {} bytes", sScanBase.data.size());
			}
			if (a.size() >= 2 && a[1] == "first")
			{
				// bytes that differ from the snapshot now
				sScan.clear();
				for (uint32 off = 0; off < sScanBase.data.size(); off += 0x1000)
				{
					const uint8* live = ScanLive(sScanBase.base + off);
					if (!live)
						continue;
					const uint8* base = sScanBase.data.data() + off;
					if (memcmp(live, base, 0x1000) == 0)
						continue;
					for (uint32 i = 0; i < 0x1000; i++)
						if (live[i] != base[i])
							sScan.push_back({sScanBase.base + off + i, base[i], live[i]});
				}
				return fmt::format("n={}", sScan.size());
			}
			if (a.size() >= 3 && a[1] == "keep")
			{
				// keep state: still the value seen in state B; keep base: back to the snapshot value
				const bool wantState = a[2] == "state";
				std::vector<ScanCandidate> out;
				for (auto& c : sScan)
				{
					const uint8* live = ScanLive(c.addr);
					if (live && *live == (wantState ? c.state : c.base))
						out.push_back(c);
				}
				sScan.swap(out);
				return fmt::format("n={}", sScan.size());
			}
			if (a.size() >= 2 && a[1] == "list")
			{
				std::string r = fmt::format("n={}|", sScan.size());
				const size_t limit = a.size() > 2 ? num(2) : 200;
				for (size_t i = 0; i < sScan.size() && i < limit; i++)
				{
					const uint8* live = ScanLive(sScan[i].addr);
					r += fmt::format("{:08x} {} {} {}|", sScan[i].addr, sScan[i].base, sScan[i].state, live ? *live : 0);
				}
				return r;
			}
			if (a.size() >= 5 && a[1] == "find32")
			{
				// scan find32 <value> <lo> <size>: aligned 32 bit values (pointers)
				const uint32 value = num(2), lo = num(3) & ~0xFFFu, size = num(4);
				std::string r;
				int count = 0;
				for (uint32 off = 0; off < size && count < 400; off += 0x1000)
				{
					const uint8* live = ScanLive(lo + off);
					if (!live)
						continue;
					for (uint32 i = 0; i < 0x1000; i += 4)
					{
						const uint32 v = ((uint32)live[i] << 24) | ((uint32)live[i + 1] << 16) | ((uint32)live[i + 2] << 8) | live[i + 3];
						if (v == value && count < 400)
						{
							r += fmt::format("{:08x}|", lo + off + i);
							count++;
						}
					}
				}
				return fmt::format("n={}|", count) + r;
			}
			if (a.size() >= 2 && a[1] == "free")
			{
				sScanBase = Snapshot{};
				sScan.clear();
				sScan.shrink_to_fit();
				return "ok";
			}
			return "err scan base|first|keep state|keep base|list|find32|free";
		}

		uint32 SnapU32(const Snapshot& sn, uint32 off)
		{
			const uint8* p = sn.data.data() + off;
			return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
		}

		float SnapF(const Snapshot& sn, uint32 off)
		{
			const uint32 v = SnapU32(sn, off);
			float f;
			memcpy(&f, &v, 4);
			return f;
		}

		std::string Handle(const std::string& line)
		{
			std::vector<std::string> a;
			boost::split(a, line, boost::is_any_of(" \t"), boost::token_compress_on);
			if (a.empty() || a[0].empty())
				return "err empty";
			auto& dbg = Input::GetDebug();
			const std::string& c = a[0];
			auto f = [&](size_t i) { return i < a.size() ? std::stof(a[i]) : 0.0f; };
			auto u = [&](size_t i) { return i < a.size() ? (uint32)std::stoul(a[i], nullptr, 0) : 0u; };
			try
			{
				if (c == "ping")
					return "pong";
				if (c == "running")
					return CafeSystem::IsTitleRunning() ? "1" : "0";
				if (c == "mapicons")
					return MapIcons::DebugCommand(a);
				if (c == "minimap")
				{
					const auto minfo = UI::GetMinimapInfo();
					float px = 0, py = 0, pz = 0, camYaw = 0;
					const bool hasPose = Input::GetPlayerPose(px, py, pz, camYaw);
					return fmt::format("pose={} pos=({:.1f},{:.1f},{:.1f}) yaw={:.4f} minfo:[vis={} c=({:.4f},{:.4f}) r={:.4f} angle={:.4f} hasNorth={}]",
						hasPose, px, py, pz, camYaw,
						minfo.visible, minfo.normCenterX, minfo.normCenterY, minfo.normRadius,
						minfo.compassAngle, minfo.hasNorthRotation);
				}
				if (c == "frame")
					return std::to_string(dbg.frameCounter.load());
				if (c == "stick" && a.size() >= 2)
				{
					if (a[1] == "off")
					{
						dbg.overrideRightStick = false;
						dbg.overrideLeftStick = false;
						return "ok";
					}
					if (a[1] == "r")
					{
						dbg.overrideRX = f(2);
						dbg.overrideRY = f(3);
						dbg.overrideRightStick = true;
						return "ok";
					}
					if (a[1] == "l")
					{
						dbg.overrideLX = f(2);
						dbg.overrideLY = f(3);
						dbg.overrideLeftStick = true;
						return "ok";
					}
				}
				if (c == "buttons")
				{
					dbg.overrideButtons = u(1);
					return "ok";
				}
				if (c == "forcefocus")
				{
					dbg.forceFocus = u(1) != 0;
					return "ok";
				}
				if (c == "mouseraw")
				{
					Input::DebugRawMouse((sint32)f(1), (sint32)f(2));
					return "ok";
				}
				if (c == "mousestream" && a.size() >= 4)
				{
					// constant velocity raw mouse movement (counts per second) for a duration, 1 kHz
					const double vx = f(1), vy = f(2), secs = f(3);
					std::thread([vx, vy, secs]() {
						const auto start = std::chrono::steady_clock::now();
						double ax = 0, ay = 0;
						auto last = start;
						while (std::chrono::steady_clock::now() - start < std::chrono::duration<double>(secs))
						{
							std::this_thread::sleep_for(std::chrono::milliseconds(1));
							const auto now = std::chrono::steady_clock::now();
							const double dt = std::chrono::duration<double>(now - last).count();
							last = now;
							ax += vx * dt;
							ay += vy * dt;
							const sint32 ix = (sint32)ax, iy = (sint32)ay;
							ax -= ix;
							ay -= iy;
							if (ix || iy)
								Input::DebugRawMouse(ix, iy);
						}
					}).detach();
					return "ok";
				}
				if (c == "mouse")
				{
					dbg.injectMouseDX += (sint64)f(1);
					dbg.injectMouseDY += (sint64)f(2);
					return "ok";
				}
				if (c == "uitexts")
					return UI::DumpTexts();
				if (c == "uitree" && a.size() >= 2)
					return UI::DumpTree(line.substr(line.find(' ') + 1));
				if (c == "screenobj" && a.size() >= 2)
					return fmt::format("{:08x}", UI::ScreenObject(a[1])); // screenobj <screen name>: the game's screen object
				if (c == "uistate")
				{
					const auto& st = UI::Get(dbg.frameCounter.load());
					std::string r = fmt::format("inv={} map={} title={} anyMenu={} A='{}' actions:", st.inventory, st.map, st.title, st.anyMenu, st.actionA);
					for (auto& ac : st.actions)
						r += fmt::format(" [{}:{}]", ac.text, (int)ac.button);
					if (st.systemPanel)
						r += fmt::format(" system=({:.3f},{:.3f})-({:.3f},{:.3f})", st.systemRect.x0, st.systemRect.y0, st.systemRect.x1, st.systemRect.y1);
					r += " buttons:";
					for (auto& mb : st.buttons)
						r += fmt::format(" [{} ({:.2f},{:.2f})-({:.2f},{:.2f})]", mb.label, mb.rect.x0, mb.rect.y0, mb.rect.x1, mb.rect.y1);
					return r;
				}
				if (c == "pointer")
				{
					const auto& st = UI::Get(dbg.frameCounter.load());
					const auto& ps = UI::GetPointer(dbg.frameCounter.load());
					std::string r = fmt::format("tabs={}/{} ", ps.currentTab, ps.tabCount) + fmt::format("pm={} gm={} inv={} map={} title={} dialog={} scope={} hud={} sel={} box={}({:.3f},{:.3f}) map={}({:.3f},{:.3f})",
						dbg.lastPointerMode.load(), dbg.lastGameMenu.load(), st.inventory, st.map, st.title, st.dialog, st.scope, st.hud,
						ps.Selected(), ps.hasBoxCursor, ps.boxX, ps.boxY, ps.hasMap, ps.mapX, ps.mapY);
					if (ps.mapCtrl)
					{
						const uint32 cur = ps.mapCtrl - 0x648 + 0x3160;
						r += fmt::format(" mapctrl={:08x} scroll=({:.1f},{:.1f}) prev=({:.1f},{:.1f}) cursor=({:.1f},{:.1f}) cprev=({:.1f},{:.1f}) scale=({:.3f},{:.3f})",
							ps.mapCtrl, memory_readFloat(ps.mapCtrl + 0x20), memory_readFloat(ps.mapCtrl + 0x24), memory_readFloat(ps.mapCtrl + 0x30),
							memory_readFloat(ps.mapCtrl + 0x34), memory_readFloat(cur), memory_readFloat(cur + 4), memory_readFloat(cur + 8),
							memory_readFloat(cur + 12), ps.mapScaleX, ps.mapScaleY);
					}
					for (int k = 0; k < 2; k++)
						r += fmt::format(" arrow{}={}({:.3f},{:.3f})-({:.3f},{:.3f})", k ? "R" : "L", ps.pageArrow[k], ps.pageArrowRect[k].x0, ps.pageArrowRect[k].y0,
										 ps.pageArrowRect[k].x1, ps.pageArrowRect[k].y1);
					r += " targets:";
					for (size_t i = 0; i < ps.targets.size(); i++)
					{
						const auto& t = ps.targets[i];
						r += fmt::format(" [{} {:08x} '{}' a{} g{:x} t{} ({:.3f},{:.3f})-({:.3f},{:.3f})]", i, t.part, t.label, t.textAlpha, t.group, t.tab, t.rect.x0, t.rect.y0, t.rect.x1, t.rect.y1);
					}
					return r;
				}
				if (c == "cursor")
					return UI::CursorInfo();
				if (c == "vmouse")
				{
					// vmouse off | vmouse x y [l] [r]
					if (a.size() >= 2 && a[1] == "off")
					{
						dbg.overrideMouse = false;
						dbg.overrideLmb = false;
						dbg.overrideRmb = false;
						return "ok";
					}
					dbg.overrideMouseX = f(1);
					dbg.overrideMouseY = f(2);
					dbg.overrideLmb = a.size() > 3 && a[3] == "1";
					dbg.overrideRmb = a.size() > 4 && a[4] == "1";
					dbg.overrideMouse = true;
					return "ok";
				}
				if (c == "macro" && a.size() >= 2)
				{
					if (a[1] == "save")
						Menus::RequestSave();
					else
						Menus::RequestLoad();
					return "ok";
				}
				if (c == "magfollow")
				{
					// magfollow [degrees per second] [metres per second]: speed of the object held with Magnesis
					if (a.size() > 1)
						dbg.magnetFollowDeg = f(1);
					if (a.size() > 2)
						dbg.magnetFollowMetres = f(2);
					return fmt::format("{} {}", dbg.magnetFollowDeg.load(), dbg.magnetFollowMetres.load());
				}
				if (c == "vwheel")
				{
					dbg.injectWheel += (sint32)f(1);
					return "ok";
				}
				if (c == "vkeys")
				{
					// vkeys <action index>... (none = release)
					uint64 m = 0;
					for (size_t i = 1; i < a.size(); i++)
						m |= 1ull << (u(i) & 63);
					dbg.overrideKeysAction = m;
					return fmt::format("{:016x}", m);
				}
				if (c == "snap" && a.size() >= 4)
				{
					// snap <id> <lo> <size>
					const uint32 size = std::min<uint32>(u(3), 0x10000000);
					TakeSnapshot(sSnaps[(int)u(1)], u(2) & ~3u, size & ~3u);
					return "ok";
				}
				if (c == "sdiff" && a.size() >= 4)
				{
					// sdiff <a> <b> f <minAbsDelta> <maxAbsDelta> [maxAbsValue]   -> changed floats
					// sdiff <a> <b> i <maxAbsDelta>                                -> changed small integers
					auto ia = sSnaps.find((int)u(1)), ib = sSnaps.find((int)u(2));
					if (ia == sSnaps.end() || ib == sSnaps.end() || ia->second.base != ib->second.base || ia->second.data.size() != ib->second.data.size())
						return "err snapshots";
					const Snapshot& A = ia->second;
					const Snapshot& B = ib->second;
					std::string r;
					int count = 0;
					const bool flt = a[3] == "f";
					const float minD = a.size() > 4 ? f(4) : 0.0f;
					const float maxD = a.size() > 5 ? f(5) : 1e9f;
					const float maxV = a.size() > 6 ? f(6) : 1e7f;
					for (uint32 off = 0; off + 4 <= A.data.size() && count < 4000; off += 4)
					{
						const uint32 va = SnapU32(A, off), vb = SnapU32(B, off);
						if (va == vb)
							continue;
						if (flt)
						{
							const float fa = SnapF(A, off), fb = SnapF(B, off);
							if (!std::isfinite(fa) || !std::isfinite(fb) || std::fabs(fa) > maxV || std::fabs(fb) > maxV)
								continue;
							const float d = std::fabs(fb - fa);
							if (d < minD || d > maxD)
								continue;
							r += fmt::format("{:08x} {} {}|", A.base + off, fa, fb);
						}
						else
						{
							const sint64 d = (sint64)(sint32)vb - (sint64)(sint32)va;
							if (std::llabs(d) > (sint64)(a.size() > 4 ? u(4) : 16))
								continue;
							r += fmt::format("{:08x} {} {}|", A.base + off, (sint32)va, (sint32)vb);
						}
						count++;
					}
					return fmt::format("n={}|", count) + r;
				}
				if (c == "scan")
					return ScanCommand(a);
				if (c == "sfree")
				{
					sSnaps.clear();
					return "ok";
				}
				if (c == "rawkey" && a.size() > 2)
				{
					Input::DebugRawKey((uint16)u(1), u(2) != 0); // rawkey <virtual key> <1 down | 0 up>
					return "ok";
				}
				if (c == "pcmouse" && a.size() > 1)
				{
					Overlay::DebugMouse(f(1), a.size() > 2 ? f(2) : 0.0f, a.size() > 3 && u(3) != 0); // pcmouse <x> <y> [down] | pcmouse -1
					return "ok";
				}
				if (c == "barriers" && a.size() > 1)
				{
					GetConfig().vk_accurate_barriers = u(1) != 0; // barriers <0|1>: Vulkan accurate barriers (for measurements)
					return "ok";
				}
				if (c == "nfpstate")
					return nnNfp_debugState();
				if (c == "nfctouch" && a.size() > 1)
				{
					// nfctouch <file>: put an amiibo file on the emulated reader (as the amiibo popup does)
					uint32 err = 0;
					const bool ok = nfc::TouchTagFromFile(_utf8ToPath(line.substr(line.find(' ') + 1)), &err);
					return ok ? "ok" : fmt::format("err {}", err);
				}
				if (c == "rawwheel")
				{
					Input::DebugRawWheel((sint32)f(1)); // rawwheel <delta>: as from the mouse, 120 = one notch up
					return "ok";
				}
				if (c == "picmap")
					return Input::DebugPictureMap(f(1), f(2), f(3), f(4)); // picmap <x> <y> <client width> <client height>
				if (c == "camage")
					return Input::CameraAges();
				if (c == "vpadcallers")
					return Input::VPADCallers();
				if (c == "uiperf")
					return UI::PerfStats();
				if (c == "hudsync")
					return UI::HudSyncStats();
				if (c == "panechain" && a.size() > 2)
					return UI::PaneChain(a[1], a[2]); // panechain <screen> <text pane name>
				if (c == "hudtrace" && a.size() > 1)
					return UI::HudTrace(a[1]);
				if (c == "pin" && a.size() > 1)
				{
					const bool v = u(1) != 0;
					UpdateSettings([v](Settings& st) { st.cpuPinning = v; }, false);
					return "ok";
				}
				if (c == "displaymode")
				{
					// displaymode [0 windowed | 1 borderless | 2 exclusive fullscreen]: sets it as the PC settings do
					if (a.size() > 1)
					{
						const int v = (int)u(1);
						UpdateSettings([v](Settings& st) { st.displayMode = v; }, false);
					}
					return fmt::format("setting {} applied {}", GetSettings().displayMode, Graphics::GetDisplayModeApplied());
				}
				if (c == "fsr" && a.size() > 2 && a[1] == "setting")
				{
					const int v = (int)u(2);
					UpdateSettings([v](Settings& st) { st.frameGeneration = v; }, false);
					Graphics::ApplyLive(); // FPS++ limit follows frame generation
					return "ok";
				}
				if (c == "fsr")
					return LatteFSR_Command(a);
				if (c == "frametrace" && a.size() > 1)
				{
					LatteFrameTrace_Request(a[1]);
					return "ok";
				}
				if (c == "framegen")
				{
					float base = 0, shown = 0;
					bool gen = false;
					LatteFrameGen_GetStats(base, shown, gen);
					double mean, sd, p95;
					LatteFrameGen_GetPacing(mean, sd, p95);
					return fmt::format("base {:.1f} shown {:.1f} generating {} | present interval mean {:.2f} ms sd {:.2f} p95 {:.2f}", base, shown, gen, mean, sd, p95);
				}
				if (c == "threads")
					return coreinit::PCE_DumpThreads();
				if (c == "perf")
				{
					if (a.size() > 2 && a[1] == "yield")
					{
						Profiler::gYieldFastPath = u(2) != 0;
						return "ok";
					}
					if (a.size() > 3 && a[1] == "affinity")
						return coreinit::PCE_SetAffinity(a[2], u(3));
					return "usage: perf yield 0|1 | perf affinity <namePrefix> <mask>";
				}
				if (c == "prof")
				{
					if (a.size() > 1 && a[1] == "start")
						return Profiler::Start(a.size() > 2 ? (int)u(2) : 1000);
					if (a.size() > 2 && a[1] == "stop")
						return Profiler::Stop(a[2], 60);
					return "usage: prof start [hz] | prof stop <reportPath>";
				}
				if (c == "gpuframes")
					return fmt::format("{} {}", LatteGPUState.frameCounter, LatteGPUState.flipCounter);
				if (c == "camobj")
					return fmt::format("{:08x}", dbg.cameraObject.load());
				if (c == "lasthold")
					return fmt::format("{:08x} {} {}", dbg.lastHold.load(), dbg.lastStickRX.load(), dbg.lastStickRY.load());
				if (c == "laststick")
					return fmt::format("{} {}", dbg.lastStickRX.load(), dbg.lastStickRY.load());
				if (c == "read32" || c == "readf")
				{
					uint32 addr = u(1);
					uint32 n = a.size() > 2 ? u(2) : 1;
					if (n > 4096 || !memory_isAddressRangeAccessible(addr, n * 4))
						return "err range";
					std::ostringstream o;
					for (uint32 i = 0; i < n; i++)
					{
						uint32 v = memory_readU32(addr + i * 4);
						if (c == "read32")
							o << fmt::format("{:08x} ", v);
						else
						{
							float fv;
							memcpy(&fv, &v, 4);
							o << fmt::format("{} ", fv);
						}
					}
					return o.str();
				}
				if (c == "readhex")
				{
					uint32 addr = u(1), n = u(2);
					if (n > 65536 || !memory_isAddressRangeAccessible(addr, n))
						return "err range";
					std::string r;
					r.reserve(n * 2);
					const uint8* p = memory_getPointerFromVirtualOffset(addr);
					for (uint32 i = 0; i < n; i++)
						r += fmt::format("{:02x}", p[i]);
					return r;
				}
				if (c == "write32" || c == "writef")
				{
					uint32 addr = u(1);
					if (!memory_isAddressRangeAccessible(addr, 4))
						return "err range";
					uint32 v;
					if (c == "write32")
						v = u(2);
					else
					{
						float fv = f(2);
						memcpy(&v, &fv, 4);
					}
					memory_writeU32(addr, v);
					return "ok";
				}
				if (c == "cal")
				{
					// cal <name> <value>
					auto cal = GetCameraCalibration();
					std::string n = a.size() > 1 ? a[1] : "";
					float v = f(2);
					if (n == "yawRateFull") cal.yawRateFull = v;
					else if (n == "pitchRateFull") cal.pitchRateFull = v;
					else if (n == "yawExtraExp") cal.yawExtraExp = v;
					else if (n == "pitchExtraExp") cal.pitchExtraExp = v;
					else if (n == "curveExp") cal.curveExp = v;
					else if (n == "maxMagnitude") cal.maxMagnitude = v;
					else if (n == "maxCarryDeg") cal.maxCarryDeg = v;
					else
						return "err name";
					SetCameraCalibration(cal);
					return "ok";
				}
				if (c == "reloadpack" && a.size() >= 2)
				{
					std::string name = line.substr(line.find(' ') + 1);
					for (auto& gp : GraphicPack2::GetGraphicPacks())
					{
						if (boost::icontains(gp->GetNormalizedPathString(), name))
						{
							if (!gp->IsEnabled())
								return "err disabled";
							bool r = gp->IsActivated() ? gp->Reload() : GraphicPack2::ActivateGraphicPack(gp);
							return r ? "ok" : "err reload";
						}
					}
					return "err notfound";
				}
				if (c == "logbp" && a.size() >= 2)
				{
					// logbp <addr> <comment with {rX} {fX} {sX} {wX}>
					uint32 addr = u(1);
					size_t p = line.find(' ', line.find(' ') + 1);
					std::string comment = p == std::string::npos ? "" : line.substr(p + 1);
					debugger_createCodeBreakpoint(addr, DEBUGGER_BP_T_LOGGING);
					for (auto bp = debugger_getFirstBP(addr); bp; bp = bp->next)
						if (bp->bpType == DEBUGGER_BP_T_LOGGING)
							bp->comment = boost::nowide::widen(comment);
					return "ok";
				}
				if (c == "membp" && a.size() >= 2)
				{
					// membp <addr>: log every write to addr (IP/LR) into log.txt
					debugger_setOptionLogOnlyMemoryBreakpoints(true);
					debugger_createMemoryBreakpoint(u(1), false, true);
					return "ok";
				}
				if (c == "bpdel")
				{
					uint32 addr = u(1);
					while (auto bp = debugger_getFirstBP(addr))
						debugger_deleteBreakpoint(bp->id);
					return "ok";
				}
				if (c == "tilt")
				{
					Input::SetTiltActive(u(1) != 0);
					return "ok";
				}
				if (c == "tiltset")
				{
					// tiltset <pitch> <roll> [yaw] (degrees) | tiltset off
					dbg.overrideTilt = a.size() >= 3;
					if (a.size() >= 3)
					{
						dbg.overrideTiltPitch = f(1);
						dbg.overrideTiltRoll = f(2);
						dbg.overrideTiltYaw = a.size() >= 4 ? f(3) : 0.0f;
					}
					return "ok";
				}
				if (c == "swkbd")
				{
					// swkbd 1 | 0 | -1: the system keyboard's input form shown / hidden / state and text
					return swkbd_debugForm(a.size() > 1 ? (int)std::stoi(a[1]) : -1);
				}
				if (c == "motion")
					return Input::MotionInfo();
				if (c == "overlay")
				{
					Overlay::SetOpen(u(1) != 0);
					if (a.size() > 2)
						Overlay::DebugSetPage((int)u(2));
					return "ok";
				}
				if (c == "screenshot" && a.size() >= 2)
				{
					if (!g_renderer)
						return "err renderer";
					std::string path = line.substr(line.find(' ') + 1);
					std::atomic<int> done{0};
					g_renderer->RequestScreenshot([path, &done](const std::vector<uint8>& rgb, int w, int h, bool main) -> std::optional<std::string> {
						if (main)
						{
							WriteBMP(path, rgb, w, h);
							done = 1;
						}
						return std::nullopt;
					});
					for (int i = 0; i < 300 && !done.load(); i++)
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					return done.load() ? "ok" : "err timeout";
				}
			}
			catch (const std::exception& e)
			{
				return std::string("err ") + e.what();
			}
			return "err unknown";
		}

		void ClientLoop(SOCKET s)
		{
			std::string buf;
			char tmp[4096];
			while (!sQuit.load())
			{
				int r = recv(s, tmp, sizeof(tmp), 0);
				if (r <= 0)
					break;
				buf.append(tmp, r);
				size_t pos;
				while ((pos = buf.find('\n')) != std::string::npos)
				{
					std::string line = buf.substr(0, pos);
					buf.erase(0, pos + 1);
					boost::trim(line);
					std::string resp = Handle(line);
					boost::replace_all(resp, "\r", "\\r");
					boost::replace_all(resp, "\n", "\\n");
					resp += "\n";
					send(s, resp.data(), (int)resp.size(), 0);
				}
			}
			closesocket(s);
		}

		void ServerThread()
		{
#ifdef _WIN32
			WSADATA wsa;
			WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
			sListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(28770);
			inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
			if (bind(sListen, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(sListen, 2) != 0)
			{
				cemuLog_log(LogType::Force, "PCE dev bridge: bind failed");
				return;
			}
			cemuLog_log(LogType::Force, "PCE dev bridge listening on 127.0.0.1:28770");
			while (!sQuit.load())
			{
				SOCKET c = accept(sListen, nullptr, nullptr);
				if (c == INVALID_SOCKET)
					break;
				std::thread(ClientLoop, c).detach();
			}
		}
	}

	void Start()
	{
		if (sThread.joinable())
			return;
		sQuit = false;
		sThread = std::thread(ServerThread);
	}

	void Stop()
	{
		if (!sThread.joinable())
			return;
		sQuit = true;
		closesocket(sListen);
		sThread.join();
	}
}
