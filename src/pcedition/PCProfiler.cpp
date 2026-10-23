#include "pcedition/PCProfiler.h"
#include "Cafe/HW/MMU/MMU.h"

#if BOOST_OS_WINDOWS
#include <windows.h>
#include <tlhelp32.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include <map>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <fstream>
#include <algorithm>

const char* PPCInterpreter_getHLEName(sint32 funcIndex); // PPCInterpreterHLE.cpp

namespace pce::Profiler
{
	volatile uint32 gHle[4]{};
	volatile uint32 gHleLr[4]{};
	volatile uint32 gIdle[4]{};
	volatile uint32 gThread[4]{};
	std::atomic<bool> gYieldFastPath{true};

	namespace
	{
		struct Func
		{
			size_t size;
			uint32 ppc;
			std::vector<std::pair<uint32, uint32>> segs; // host offset -> PPC address, sorted by host offset
		};
		std::mutex sFuncMtx;
		std::map<uintptr_t, Func> sFuncs;

		struct Module
		{
			uintptr_t base;
			size_t size;
			std::string name;
		};

#if BOOST_OS_WINDOWS
		struct Target
		{
			std::string name;
			HANDLE h = nullptr;
			int core = -1;
			FILETIME k0{}, u0{};
			uint64 total = 0, idle = 0, jit = 0, hle = 0, native = 0, failed = 0;
			std::unordered_map<uint32, uint64> blocks, funcs, threads;
			std::unordered_map<uint64, uint64> hles, idleWhy; // (hle index << 32) | caller LR
			std::unordered_map<uint64, uint64> natives; // (module index << 40) | rva
		};

		std::mutex sCtlMtx;
		std::thread sThread;
		std::atomic<bool> sRun{false};
		std::vector<Target> sTargets;
		std::vector<Module> sModules;
		LARGE_INTEGER sStart{}, sStop{};
		int sHz = 1000;
#endif

		bool LookupJit(uintptr_t rip, uint32& block, uint32& func)
		{
			std::lock_guard l(sFuncMtx);
			auto it = sFuncs.upper_bound(rip);
			if (it == sFuncs.begin())
				return false;
			--it;
			if (rip >= it->first + it->second.size)
				return false;
			func = it->second.ppc;
			block = func;
			const uint32 off = (uint32)(rip - it->first);
			const auto& s = it->second.segs;
			auto sit = std::upper_bound(s.begin(), s.end(), std::make_pair(off, 0xFFFFFFFFu));
			while (sit != s.begin())
			{
				--sit;
				if (sit->second != 0xFFFFFFFF)
				{
					block = sit->second;
					break;
				}
			}
			return true;
		}

#if BOOST_OS_WINDOWS
		void ListModules()
		{
			sModules.clear();
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			if (snap == INVALID_HANDLE_VALUE)
				return;
			MODULEENTRY32W me{};
			me.dwSize = sizeof(me);
			if (Module32FirstW(snap, &me))
			{
				do
				{
					char name[260]{};
					WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, sizeof(name) - 1, nullptr, nullptr);
					sModules.push_back({(uintptr_t)me.modBaseAddr, (size_t)me.modBaseSize, name});
				} while (Module32NextW(snap, &me));
			}
			CloseHandle(snap);
		}

		void ListTargets()
		{
			for (auto& t : sTargets)
				if (t.h)
					CloseHandle(t.h);
			sTargets.clear();
			using GetThreadDescription_t = HRESULT(WINAPI*)(HANDLE, PWSTR*);
			static auto gtd = (GetThreadDescription_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
			if (!gtd)
				return;
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE)
				return;
			THREADENTRY32 te{};
			te.dwSize = sizeof(te);
			const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
			if (Thread32First(snap, &te))
			{
				do
				{
					if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
						continue;
					HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
					if (!h)
						continue;
					PWSTR desc = nullptr;
					std::string name;
					if (SUCCEEDED(gtd(h, &desc)) && desc)
					{
						char buf[256]{};
						WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
						name = buf;
						LocalFree(desc);
					}
					Target t;
					t.name = name.empty() ? fmt::format("tid{}", te.th32ThreadID) : fmt::format("{} ({})", name, te.th32ThreadID);
					if (name.rfind("OSSched[core=", 0) == 0 && name.size() > 13)
						t.core = name[13] - '0';
					else if (name != "LatteThread")
						t.core = -2; // CPU time only, not sampled
					t.h = h;
					FILETIME c, e;
					GetThreadTimes(h, &c, &e, &t.k0, &t.u0);
					sTargets.push_back(std::move(t));
				} while (Thread32Next(snap, &te));
			}
			CloseHandle(snap);
		}

		void Sample(Target& t)
		{
			if (SuspendThread(t.h) == (DWORD)-1)
			{
				t.failed++;
				return;
			}
			CONTEXT ctx{};
			ctx.ContextFlags = CONTEXT_CONTROL;
			const BOOL ok = GetThreadContext(t.h, &ctx);
			const uint32 idle = t.core >= 0 ? gIdle[t.core] : 0;
			const uint32 hle = t.core >= 0 ? gHle[t.core] : 0;
			const uint32 hleLr = t.core >= 0 ? gHleLr[t.core] : 0;
			const uint32 thread = t.core >= 0 ? gThread[t.core] : 0;
			ResumeThread(t.h);
			if (!ok)
			{
				t.failed++;
				return;
			}
			t.total++;
			if (idle)
			{
				t.idle++;
				t.idleWhy[hle ? ((uint64)(hle - 1) << 32) | hleLr : 0xFFFFFFFFull << 32]++;
				return;
			}
			t.threads[thread]++;
			uint32 block, func;
			if (LookupJit((uintptr_t)ctx.Rip, block, func))
			{
				t.jit++;
				t.blocks[block]++;
				t.funcs[func]++;
				return;
			}
			if (hle)
			{
				t.hle++;
				t.hles[((uint64)(hle - 1) << 32) | hleLr]++;
			}
			else
				t.native++;
			for (size_t i = 0; i < sModules.size(); i++)
			{
				const auto& m = sModules[i];
				if (ctx.Rip >= m.base && ctx.Rip < m.base + m.size)
				{
					t.natives[((uint64)i << 40) | (ctx.Rip - m.base)]++;
					return;
				}
			}
			t.natives[0xFFFFull << 40]++;
		}

		void SamplerMain()
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			timeBeginPeriod(1);
			LARGE_INTEGER f, now, next;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&next);
			const sint64 step = f.QuadPart / std::max(sHz, 50);
			while (sRun.load())
			{
				for (auto& t : sTargets)
					if (t.core != -2)
						Sample(t);
				next.QuadPart += step;
				QueryPerformanceCounter(&now);
				if (next.QuadPart > now.QuadPart)
				{
					const sint64 ms = (next.QuadPart - now.QuadPart) * 1000 / f.QuadPart;
					Sleep((DWORD)std::max<sint64>(ms, 0));
				}
				else
					next = now;
			}
			timeEndPeriod(1);
		}

		double Pct(uint64 a, uint64 b) { return b ? 100.0 * (double)a / (double)b : 0.0; }

		template<typename K>
		std::vector<std::pair<K, uint64>> Top(const std::unordered_map<K, uint64>& m, size_t n)
		{
			std::vector<std::pair<K, uint64>> v(m.begin(), m.end());
			std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
			if (v.size() > n)
				v.resize(n);
			return v;
		}

		uint64 FtU64(const FILETIME& ft) { return ((uint64)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }
#endif
	}

	void RegisterFunction(const void* hostCode, size_t hostSize, uint32 ppcAddress, std::vector<std::pair<uint32, uint32>>&& segments)
	{
		std::sort(segments.begin(), segments.end());
		std::lock_guard l(sFuncMtx);
		sFuncs[(uintptr_t)hostCode] = Func{hostSize, ppcAddress, std::move(segments)};
	}

	void UnregisterFunction(const void* hostCode)
	{
		std::lock_guard l(sFuncMtx);
		sFuncs.erase((uintptr_t)hostCode);
	}

#if BOOST_OS_WINDOWS
	bool Running() { return sRun.load(); }

	std::string Start(int hz)
	{
		std::lock_guard l(sCtlMtx);
		if (sRun.load())
			return "already running";
		sHz = hz > 0 ? hz : 1000;
		ListModules();
		ListTargets();
		QueryPerformanceCounter(&sStart);
		sRun = true;
		sThread = std::thread(SamplerMain);
		size_t sampled = 0;
		for (auto& t : sTargets)
			sampled += t.core != -2;
		return fmt::format("profiling {} threads ({} sampled) at {} Hz, {} recompiled functions", sTargets.size(), sampled, sHz, sFuncs.size());
	}

	std::string Stop(const std::string& reportPath, int top)
	{
		std::lock_guard l(sCtlMtx);
		if (!sRun.load())
			return "not running";
		sRun = false;
		if (sThread.joinable())
			sThread.join();
		QueryPerformanceCounter(&sStop);
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		const double secs = (double)(sStop.QuadPart - sStart.QuadPart) / (double)f.QuadPart;
		top = std::clamp(top, 5, 400);

		std::string o = fmt::format("duration {:.2f} s\n\n== host thread CPU (user+kernel) ==\n", secs);
		std::vector<std::pair<double, std::string>> cpu;
		for (auto& t : sTargets)
		{
			FILETIME c, e, k1, u1;
			if (!GetThreadTimes(t.h, &c, &e, &k1, &u1))
				continue;
			const double used = (double)((FtU64(k1) - FtU64(t.k0)) + (FtU64(u1) - FtU64(t.u0))) / 1e7;
			if (used / secs >= 0.01)
				cpu.emplace_back(used / secs * 100.0, t.name);
		}
		std::sort(cpu.begin(), cpu.end(), [](auto& a, auto& b) { return a.first > b.first; });
		for (auto& [p, n] : cpu)
			o += fmt::format("{:6.1f}%  {}\n", p, n);

		std::string summary;
		for (auto& t : sTargets)
		{
			if (t.core == -2 || t.total == 0)
				continue;
			o += fmt::format("\n== {} : {} samples | idle {:.1f}% | jit {:.1f}% | hle {:.1f}% | native {:.1f}% | failed {} ==\n",
				t.name, t.total, Pct(t.idle, t.total), Pct(t.jit, t.total), Pct(t.hle, t.total), Pct(t.native, t.total), t.failed);
			summary += fmt::format("{}: idle {:.0f} jit {:.0f} hle {:.0f} native {:.0f}|", t.name, Pct(t.idle, t.total), Pct(t.jit, t.total), Pct(t.hle, t.total), Pct(t.native, t.total));
			o += "-- PPC threads (busy samples) --\n";
			for (auto& [th, n] : Top(t.threads, 40))
			{
				std::string nm = "?";
				if (th && memory_isAddressRangeAccessible(th + 0x5C0, 4))
				{
					const uint32 np = memory_readU32(th + 0x5C0);
					if (np && memory_isAddressRangeAccessible(np, 1))
					{
						const char* p = (const char*)memory_getPointerFromVirtualOffset(np);
						nm = std::string(p, strnlen(p, 63));
					}
				}
				o += fmt::format("{:6.2f}%  thread {:08x} {}\n", Pct(n, t.total), th, nm);
			}
			auto hleName = [](uint64 k) {
				if ((k >> 32) == 0xFFFFFFFF)
					return std::string("-");
				const char* nm = PPCInterpreter_getHLEName((sint32)(k >> 32));
				return fmt::format("{} @lr {:08x}", nm ? nm : "?", (uint32)k);
			};
			o += "-- HLE (incl. native code below it) --\n";
			for (auto& [k, n] : Top(t.hles, 60))
				o += fmt::format("{:6.2f}%  hle {}\n", Pct(n, t.total), hleName(k));
			o += "-- idle, entered through --\n";
			for (auto& [k, n] : Top(t.idleWhy, 30))
				o += fmt::format("{:6.2f}%  idle {}\n", Pct(n, t.total), hleName(k));
			o += "-- PPC functions (recompiler ranges) --\n";
			for (auto& [a, n] : Top(t.funcs, SIZE_MAX))
				o += fmt::format("{:6.2f}%  func {:08x}\n", Pct(n, t.total), a);
			o += "-- PPC blocks --\n";
			for (auto& [a, n] : Top(t.blocks, SIZE_MAX))
				o += fmt::format("{:6.2f}%  block {:08x}\n", Pct(n, t.total), a);
			o += "-- native (module+rva) --\n";
			for (auto& [k, n] : Top(t.natives, SIZE_MAX))
			{
				const size_t mi = (size_t)(k >> 40);
				const uint64 rva = k & ((1ull << 40) - 1);
				o += fmt::format("{:6.2f}%  native {}+{:x}\n", Pct(n, t.total), mi < sModules.size() ? sModules[mi].name : "?", rva);
			}
		}
		std::ofstream(_utf8ToPath(reportPath), std::ios::trunc) << o;
		for (auto& t : sTargets)
			if (t.h)
				CloseHandle(t.h);
		sTargets.clear();
		return summary;
	}

	std::string PinEmulationThreads(bool enable)
	{
		using GetThreadDescription_t = HRESULT(WINAPI*)(HANDLE, PWSTR*);
		static auto gtd = (GetThreadDescription_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
		if (!gtd)
			return "no GetThreadDescription";
		// L3 groups and physical cores
		auto query = [](LOGICAL_PROCESSOR_RELATIONSHIP rel) {
			DWORD len = 0;
			GetLogicalProcessorInformationEx(rel, nullptr, &len);
			std::vector<uint8> buf(len);
			if (!len || !GetLogicalProcessorInformationEx(rel, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len))
				buf.clear();
			return buf;
		};
		KAFFINITY l3 = 0;
		{
			auto buf = query(RelationCache);
			for (size_t off = 0; off < buf.size();)
			{
				auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
				if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == 0)
				{
					const KAFFINITY m = e->Cache.GroupMask.Mask;
					if (__popcnt64(m) > __popcnt64(l3) || (__popcnt64(m) == __popcnt64(l3) && m < l3))
						l3 = m;
				}
				off += e->Size;
			}
		}
		std::vector<KAFFINITY> cores; // first logical processor of each physical core inside the L3 group
		{
			auto buf = query(RelationProcessorCore);
			for (size_t off = 0; off < buf.size();)
			{
				auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
				if (e->Relationship == RelationProcessorCore && e->Processor.GroupMask[0].Group == 0)
				{
					const KAFFINITY m = e->Processor.GroupMask[0].Mask & l3;
					if (m)
						cores.push_back(m & (~m + 1));
				}
				off += e->Size;
			}
		}
		std::sort(cores.begin(), cores.end());
		const char* names[4] = {"OSSched[core=1]", "OSSched[core=0]", "OSSched[core=2]", "LatteThread"};
		if (enable && cores.size() < 4)
			return fmt::format("not pinned: L3 group {:x} has {} cores", (uint64)l3, cores.size());
		std::string r;
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return "no snapshot";
		THREADENTRY32 te{};
		te.dwSize = sizeof(te);
		const DWORD pid = GetCurrentProcessId();
		DWORD_PTR processMask = 0, systemMask = 0;
		GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
		if (Thread32First(snap, &te))
		{
			do
			{
				if (te.th32OwnerProcessID != pid)
					continue;
				HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
				if (!h)
					continue;
				PWSTR desc = nullptr;
				char name[128]{};
				if (SUCCEEDED(gtd(h, &desc)) && desc)
				{
					WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof(name) - 1, nullptr, nullptr);
					LocalFree(desc);
				}
				for (int i = 0; i < 4; i++)
				{
					if (strcmp(name, names[i]) != 0)
						continue;
					const KAFFINITY mask = enable ? cores[i] : (KAFFINITY)processMask;
					if (SetThreadAffinityMask(h, (DWORD_PTR)mask))
						r += fmt::format("{} -> {:x} ", name, (uint64)mask);
				}
				CloseHandle(h);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		return r.empty() ? "no threads found" : r;
	}
#else
	bool Running() { return false; }
	std::string Start(int) { return "profiler not supported on Linux"; }
	std::string Stop(const std::string&, int) { return "profiler not supported on Linux"; }
	std::string PinEmulationThreads(bool) { return "CPU pinning not supported on Linux"; }
#endif
}
