#include "pcedition/PCMapIcons.h"
#include "pcedition/PCEdition.h"
#include "pcedition/PCSettings.h"
#include "pcedition/PCInput.h"
#include "pcedition/PCGraphics.h"
#include "pcedition/PCOverlay.h"
#include "pcedition/PCUI.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <boost/algorithm/string.hpp>

#include "imgui.h"
#include "imgui_internal.h"
#include "Cafe/HW/MMU/MMU.h"

namespace pce::MapIcons
{
	namespace
	{
		// ---------------------------------------------------------------- byte & hash helpers
		uint16 RB16(const uint8* p) { return (uint16)((p[0] << 8) | p[1]); }
		uint32 RB32(const uint8* p) { return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3]; }
		sint32 RBS32(const uint8* p) { return (sint32)RB32(p); }
		float RBF32(const uint8* p)
		{
			const uint32 u = RB32(p);
			float f;
			memcpy(&f, &u, 4);
			return f;
		}

		uint32 Crc32(std::string_view s)
		{
			static uint32 table[256];
			static bool init = false;
			if (!init)
			{
				for (uint32 i = 0; i < 256; i++)
				{
					uint32 c = i;
					for (int k = 0; k < 8; k++)
						c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
					table[i] = c;
				}
				init = true;
			}
			uint32 crc = 0xFFFFFFFFu;
			for (char ch : s)
				crc = table[(crc ^ (uint8)ch) & 0xFF] ^ (crc >> 8);
			return crc ^ 0xFFFFFFFFu;
		}

		std::string ToLowerAscii(std::string_view s)
		{
			std::string r(s);
			for (char& c : r)
				if (c >= 'A' && c <= 'Z')
					c = (char)(c - 'A' + 'a');
			return r;
		}

		bool ReadFile(const fs::path& p, std::vector<uint8>& out)
		{
			std::ifstream f(p, std::ios::binary);
			if (!f)
				return false;
			f.seekg(0, std::ios::end);
			const auto sz = f.tellg();
			if (sz <= 0)
				return false;
			out.resize((size_t)sz);
			f.seekg(0);
			f.read((char*)out.data(), out.size());
			return (bool)f;
		}

		// ---------------------------------------------------------------- Yaz0 & SARC
		bool Yaz0Decode(const std::vector<uint8>& in, std::vector<uint8>& out)
		{
			if (in.size() < 16 || memcmp(in.data(), "Yaz0", 4) != 0)
				return false;
			const uint32 size = RB32(in.data() + 4);
			out.assign(size, 0);
			size_t src = 16, dst = 0;
			while (dst < size && src < in.size())
			{
				const uint8 flags = in[src++];
				for (int bit = 7; bit >= 0 && dst < size; bit--)
				{
					if (flags & (1 << bit))
					{
						if (src >= in.size())
							return false;
						out[dst++] = in[src++];
					}
					else
					{
						if (src + 1 >= in.size())
							return false;
						const uint8 b1 = in[src++], b2 = in[src++];
						const size_t dist = (((size_t)(b1 & 0x0F) << 8) | b2) + 1;
						size_t len = b1 >> 4;
						if (len == 0)
						{
							if (src >= in.size())
								return false;
							len = (size_t)in[src++] + 0x12;
						}
						else
							len += 2;
						if (dist > dst)
							return false;
						for (size_t i = 0; i < len && dst < size; i++, dst++)
							out[dst] = out[dst - dist];
					}
				}
			}
			return dst == size;
		}

		struct SarcEntryView
		{
			std::string name;
			const uint8* data = nullptr;
			size_t size = 0;
		};

		bool SarcList(const std::vector<uint8>& d, std::vector<SarcEntryView>& out)
		{
			if (d.size() < 0x20 || memcmp(d.data(), "SARC", 4) != 0 || RB16(d.data() + 6) != 0xFEFF)
				return false;
			const uint32 dataOff = RB32(d.data() + 0x0C);
			const uint16 sfatOff = RB16(d.data() + 4);
			if (sfatOff + 0x0C > d.size())
				return false;
			const uint8* sfat = d.data() + sfatOff;
			if (memcmp(sfat, "SFAT", 4) != 0)
				return false;
			const uint16 count = RB16(sfat + 6);
			const uint8* nodes = sfat + 0x0C;
			const uint8* sfnt = nodes + (size_t)count * 0x10;
			if ((size_t)(sfnt - d.data()) + 8 > d.size() || memcmp(sfnt, "SFNT", 4) != 0)
				return false;
			const uint8* names = sfnt + RB16(sfnt + 4);
			out.clear();
			out.reserve(count);
			for (uint16 i = 0; i < count; i++)
			{
				const uint8* n = nodes + (size_t)i * 0x10;
				const uint32 attr = RB32(n + 4);
				const uint32 b = RB32(n + 8), e = RB32(n + 12);
				if (b > e || dataOff + e > d.size())
					continue;
				SarcEntryView v;
				if (attr & 0x01000000)
				{
					const uint8* np = names + (size_t)(attr & 0xFFFF) * 4;
					if (np < d.data() + d.size())
						v.name = (const char*)np;
				}
				v.data = d.data() + dataOff + b;
				v.size = e - b;
				out.push_back(std::move(v));
			}
			return true;
		}

		bool SarcExtractFile(const std::vector<uint8>& d, std::string_view targetName, std::vector<uint8>& out)
		{
			std::vector<SarcEntryView> entries;
			if (!SarcList(d, entries))
				return false;
			for (const auto& e : entries)
			{
				if (e.name == targetName)
				{
					out.assign(e.data, e.data + e.size);
					return true;
				}
			}
			return false;
		}

		// ---------------------------------------------------------------- MSBT reader
		void ParseMsbt(const uint8* d, size_t dSize, std::unordered_map<std::string, std::string>& out, bool stripNameSuffix = false)
		{
			if (dSize < 0x30 || memcmp(d, "MsgStdBn", 8) != 0 || d[8] != 0xFE || d[9] != 0xFF)
				return;
			const uint8 *labels = nullptr, *texts = nullptr;
			uint32 labelsSize = 0, textsSize = 0;
			size_t off = 0x20;
			for (uint32 i = 0, n = RB16(d + 0x0E); i < n && off + 0x10 <= dSize; i++)
			{
				const uint32 sz = RB32(d + off + 4);
				if (off + 0x10 + sz > dSize)
					return;
				if (memcmp(d + off, "LBL1", 4) == 0)
				{
					labels = d + off + 0x10;
					labelsSize = sz;
				}
				else if (memcmp(d + off, "TXT2", 4) == 0)
				{
					texts = d + off + 0x10;
					textsSize = sz;
				}
				off = (off + 0x10 + sz + 0x0F) & ~(size_t)0x0F;
			}
			if (!labels || !texts || labelsSize < 4 || textsSize < 4)
				return;
			const uint32 textCount = RB32(texts);
			auto textOf = [&](uint32 index) {
				std::string o;
				if (index >= textCount || 4 + (index + 1) * 4 > textsSize)
					return o;
				const uint32 begin = RB32(texts + 4 + index * 4);
				const uint32 end = index + 1 < textCount ? RB32(texts + 4 + (index + 1) * 4) : textsSize;
				for (uint32 p = begin; p + 1 < end && p + 1 < textsSize; p += 2)
				{
					const uint32 c = RB16(texts + p);
					if (c == 0)
						break;
					if (c == 0x0E)
					{
						if (p + 7 < textsSize)
							p += 6 + RB16(texts + p + 6);
						continue;
					}
					if (c == 0x0F)
					{
						p += 4;
						continue;
					}
					if (c < 0x80)
						o.push_back((char)c);
					else if (c < 0x800)
					{
						o.push_back((char)(0xC0 | (c >> 6)));
						o.push_back((char)(0x80 | (c & 0x3F)));
					}
					else
					{
						o.push_back((char)(0xE0 | (c >> 12)));
						o.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
						o.push_back((char)(0x80 | (c & 0x3F)));
					}
				}
				boost::trim(o);
				return o;
			};
			const uint32 slots = RB32(labels);
			for (uint32 s = 0; s < slots && 4 + (s + 1) * 8 <= labelsSize; s++)
			{
				const uint32 count = RB32(labels + 4 + s * 8);
				uint32 p = RB32(labels + 8 + s * 8);
				for (uint32 k = 0; k < count && p < labelsSize; k++)
				{
					const uint32 len = labels[p];
					if (p + 1 + len + 4 > labelsSize)
						return;
					std::string lbl((const char*)labels + p + 1, len);
					const uint32 idx = RB32(labels + p + 1 + len);
					p += 1 + len + 4;
					if (stripNameSuffix)
					{
						if (lbl.size() <= 5 || lbl.compare(lbl.size() - 5, 5, "_Name") != 0)
							continue;
						lbl.resize(lbl.size() - 5);
					}
					std::string txt = textOf(idx);
					if (!txt.empty())
						out[std::move(lbl)] = std::move(txt);
				}
			}
		}

		// ---------------------------------------------------------------- minimal BYML v2 reader (big endian)
		class BymlView
		{
		public:
			bool Init(const std::vector<uint8>& buf)
			{
				d = buf.data();
				sz = buf.size();
				if (sz < 16 || d[0] != 'B' || d[1] != 'Y')
					return false;
				keyOff = RB32(d + 4);
				strOff = RB32(d + 8);
				rootOff = RB32(d + 12);
				if (keyOff && !ReadStringTable(keyOff, keys))
					return false;
				if (strOff && !ReadStringTable(strOff, strs))
					return false;
				return rootOff > 0 && rootOff + 4 <= sz;
			}

			uint32 RootOffset() const { return rootOff; }

			uint32 ArraySize(uint32 off) const
			{
				if (off + 4 > sz || d[off] != 0xC0)
					return 0;
				return RB32(d + off) & 0x00FFFFFFu;
			}

			bool ArrayElem(uint32 off, uint32 idx, uint8& outType, uint32& outVal) const
			{
				const uint32 cnt = ArraySize(off);
				if (idx >= cnt)
					return false;
				const uint32 typesOff = off + 4;
				const uint32 valsOff = typesOff + ((cnt + 3u) & ~3u);
				if (valsOff + (idx + 1) * 4 > sz)
					return false;
				outType = d[typesOff + idx];
				outVal = RB32(d + valsOff + idx * 4);
				return true;
			}

			bool DictGet(uint32 off, std::string_view key, uint8& outType, uint32& outVal) const
			{
				if (off + 4 > sz || d[off] != 0xC1)
					return false;
				const uint32 cnt = RB32(d + off) & 0x00FFFFFFu;
				if (off + 4 + cnt * 8 > sz)
					return false;
				// Dictionary keys in BYML are sorted in ascending ASCII order -> binary search
				sint32 lo = 0, hi = (sint32)cnt - 1;
				while (lo <= hi)
				{
					const sint32 mid = (lo + hi) >> 1;
					const uint32 p = off + 4 + (uint32)mid * 8;
					const uint32 kw = RB32(d + p);
					const uint32 kidx = kw >> 8;
					if (kidx >= keys.size())
						return false;
					const int cmp = keys[kidx].compare(key);
					if (cmp == 0)
					{
						outType = (uint8)(kw & 0xFF);
						outVal = RB32(d + p + 4);
						return true;
					}
					if (cmp < 0)
						lo = mid + 1;
					else
						hi = mid - 1;
				}
				return false;
			}

			std::string_view GetString(uint8 t, uint32 v) const
			{
				if (t == 0xA0 && v < strs.size())
					return strs[v];
				return {};
			}

			bool GetBool(uint8 t, uint32 v, bool def = false) const
			{
				return t == 0xD0 ? (v != 0) : def;
			}

			float GetFloat(uint8 t, uint32 v, float def = 0.0f) const
			{
				if (t == 0xD2)
				{
					float f;
					memcpy(&f, &v, 4);
					return f;
				}
				if (t == 0xD1)
					return (float)(sint32)v;
				if (t == 0xD3)
					return (float)v;
				return def;
			}

			uint32 GetUInt(uint8 t, uint32 v, uint32 def = 0) const
			{
				if (t == 0xD3 || t == 0xD1)
					return v;
				return def;
			}

			bool GetTranslate(uint8 t, uint32 v, float& x, float& y, float& z) const
			{
				if (t == 0xC0 && ArraySize(v) >= 3)
				{
					uint8 tx, ty, tz;
					uint32 vx, vy, vz;
					if (ArrayElem(v, 0, tx, vx) && ArrayElem(v, 1, ty, vy) && ArrayElem(v, 2, tz, vz))
					{
						x = GetFloat(tx, vx);
						y = GetFloat(ty, vy);
						z = GetFloat(tz, vz);
						return true;
					}
				}
				else if (t == 0xC1)
				{
					uint8 tx, ty, tz;
					uint32 vx, vy, vz;
					if (DictGet(v, "X", tx, vx) && DictGet(v, "Y", ty, vy) && DictGet(v, "Z", tz, vz))
					{
						x = GetFloat(tx, vx);
						y = GetFloat(ty, vy);
						z = GetFloat(tz, vz);
						return true;
					}
				}
				return false;
			}

		private:
			bool ReadStringTable(uint32 off, std::vector<std::string_view>& out)
			{
				if (off + 4 > sz || d[off] != 0xC2)
					return false;
				const uint32 cnt = RB32(d + off) & 0x00FFFFFFu;
				if (off + 4 + (cnt + 1) * 4 > sz)
					return false;
				out.resize(cnt);
				for (uint32 i = 0; i < cnt; i++)
				{
					const uint32 rel = RB32(d + off + 4 + i * 4);
					const uint32 pos = off + rel;
					if (pos >= sz)
						return false;
					size_t len = 0;
					while (pos + len < sz && d[pos + len] != 0)
						len++;
					out[i] = std::string_view((const char*)d + pos, len);
				}
				return true;
			}

			const uint8* d = nullptr;
			size_t sz = 0;
			uint32 keyOff = 0, strOff = 0, rootOff = 0;
			std::vector<std::string_view> keys;
			std::vector<std::string_view> strs;
		};

		// ---------------------------------------------------------------- dump paths
		fs::path FindTitleContent(const char* idHex1, const char* idHex2, const char* idHex3, const char* mlcSub)
		{
			std::error_code ec;
			const fs::path root = GetGameRoot();
			if (!root.empty())
			{
				for (auto& e : fs::directory_iterator(root, ec))
				{
					const fs::path app = e.path() / "code" / "app.xml";
					std::vector<uint8> m;
					if (!ReadFile(app, m))
						continue;
					std::string s(m.begin(), m.end());
					boost::to_lower(s);
					if (s.find(idHex1) != std::string::npos || s.find(idHex2) != std::string::npos || s.find(idHex3) != std::string::npos)
						return e.path() / "content";
				}
			}
			for (const char* region : {"101c9500", "101c9400", "101c9300"})
			{
				const fs::path p = GetDataDir() / "mlc01" / "usr" / "title" / mlcSub / region / "content";
				if (fs::exists(p, ec))
					return p;
			}
			return {};
		}

		fs::path FindUpdateContent()
		{
			return FindTitleContent("0005000e101c9500", "0005000e101c9400", "0005000e101c9300", "0005000e");
		}

		fs::path FindAocContent()
		{
			fs::path c = FindTitleContent("0005000c101c9500", "0005000c101c9400", "0005000c101c9300", "0005000c");
			std::error_code ec;
			if (!c.empty() && fs::exists(c / "0010", ec))
				return c / "0010";
			return {};
		}

		// ---------------------------------------------------------------- marker data model
		enum MarkerFlags : uint8
		{
			Flag_HardMode = 1u << 0,
			Flag_InGround = 1u << 1,
			Flag_DLC      = 1u << 2,
		};

		struct Marker
		{
			uint8 catBit = 0;       // 0..10 (1u << catBit)
			uint8 subKind = 0;      // icon subtype within category
			uint8 flags = 0;        // MarkerFlags
			float wx = 0, wy = 0, wz = 0;
			uint32 clearFlagCrc = 0;
			uint32 discFlagCrc = 0;
			std::string name;
			std::string sub;

			// runtime fields (not serialized)
			std::string searchKey;
			uint32 clearFlagAddr = 0;
			uint32 discFlagAddr = 0;
			bool claimed = false;
			bool discovered = false;
		};

		struct CategoryMeta
		{
			const char* name;
			const char* keyword;
			ImU32 color;
			bool hasClaimFlags;
		};

		constexpr CategoryMeta kCatMeta[Cat_Count] = {
			{"Shrines",                  "shrine dungeon trial",          IM_COL32(56, 216, 255, 255),  true},
			{"Sheikah Towers",           "tower",                         IM_COL32(85, 230, 255, 255),  true},
			{"Korok Seeds",              "korok seed",                    IM_COL32(82, 228, 108, 255),  true},
			{"Treasure Chests",          "chest tbox treasure",           IM_COL32(248, 192, 56, 255),  true},
			{"Overworld Bosses",         "boss lynel hinox stalnox talus molduga", IM_COL32(255, 82, 72, 255), false},
			{"Guardians",                "guardian stalker skywatcher turret", IM_COL32(255, 68, 145, 255), false},
			{"Towns, Stables & Statues", "village stable lab fairy statue cooking pot", IM_COL32(245, 224, 148, 255), false},
			{"Weapons, Bows & Shields",  "weapon sword claymore spear bow shield", IM_COL32(140, 200, 255, 255), false},
			{"Ore Deposits",             "ore mineral rare luminous gem", IM_COL32(80, 240, 215, 255),  false},
			{"Plants, Mushrooms & Fish", "material plant mushroom fruit fish insect critter", IM_COL32(130, 230, 150, 255), false},
			{"Enemies (Equipment)",      "enemy bokoblin moblin lizalfos wizzrobe yiga", IM_COL32(215, 110, 220, 255), false},
		};

		std::mutex sDbMutex;
		std::vector<Marker> sMarkers;
		std::atomic<bool> sDbReady{false};
		std::atomic<bool> sDbLoading{false};
		std::string sLoadedLang;

		// Live GDT flag state
		uint32 sBoundBoolArr = 0;
		uint32 sBoundBoolCnt = 0;
		uint32 sLastFlagRefreshFrame = 0;
		std::array<int, Cat_Count> sCatTotal{};
		std::array<int, Cat_Count> sCatClaimed{};

		// Interactive UI state
		bool sSearchFocused = false;
		std::string sSearchQuery;
		std::vector<std::string> sSearchTokens;
		std::vector<int> sSearchMatches;
		int sSearchScroll = 0;
		bool sMouseWasDown = false;
		int sHighlightMarker = -1;
		double sHighlightUntil = 0.0;

		void BuildSearchKey(Marker& m)
		{
			std::string s = m.name + " " + m.sub + " " + kCatMeta[m.catBit].keyword;
			if (m.flags & Flag_DLC)
				s += " dlc ex";
			if (m.flags & Flag_InGround)
				s += " buried underground";
			if (m.flags & Flag_HardMode)
				s += " master mode";
			m.searchKey = ToLowerAscii(s);
		}

		// ---------------------------------------------------------------- binary cache I/O
		constexpr uint32 kCacheMagic = 0x50434D34u; // "PCM4"

		bool LoadCache(const fs::path& path, std::vector<Marker>& out)
		{
			std::vector<uint8> buf;
			if (!ReadFile(path, buf) || buf.size() < 8)
				return false;
			size_t pos = 0;
			auto r32 = [&]() -> uint32 {
				uint32 v;
				memcpy(&v, buf.data() + pos, 4);
				pos += 4;
				return v;
			};
			auto rf = [&]() -> float {
				float v;
				memcpy(&v, buf.data() + pos, 4);
				pos += 4;
				return v;
			};
			auto rstr = [&](std::string& s) -> bool {
				if (pos + 2 > buf.size())
					return false;
				uint16 len;
				memcpy(&len, buf.data() + pos, 2);
				pos += 2;
				if (pos + len > buf.size())
					return false;
				s.assign((const char*)buf.data() + pos, len);
				pos += len;
				return true;
			};
			if (r32() != kCacheMagic)
				return false;
			const uint32 count = r32();
			if (count == 0 || count > 100000)
				return false;
			out.clear();
			out.reserve(count);
			for (uint32 i = 0; i < count; i++)
			{
				if (pos + 23 > buf.size())
					return false;
				Marker m;
				m.catBit = buf[pos++];
				m.subKind = buf[pos++];
				m.flags = buf[pos++];
				if (m.catBit >= Cat_Count)
					return false;
				m.wx = rf();
				m.wy = rf();
				m.wz = rf();
				m.clearFlagCrc = r32();
				m.discFlagCrc = r32();
				if (!rstr(m.name) || !rstr(m.sub))
					return false;
				BuildSearchKey(m);
				out.push_back(std::move(m));
			}
			return true;
		}

		void SaveCache(const fs::path& path, const std::vector<Marker>& markers)
		{
			std::vector<uint8> buf;
			buf.reserve(markers.size() * 48);
			auto w32 = [&](uint32 v) {
				const uint8* p = (const uint8*)&v;
				buf.insert(buf.end(), p, p + 4);
			};
			auto wf = [&](float v) {
				const uint8* p = (const uint8*)&v;
				buf.insert(buf.end(), p, p + 4);
			};
			auto wstr = [&](const std::string& s) {
				const uint16 len = (uint16)std::min<size_t>(s.size(), 0xFFFF);
				const uint8* p = (const uint8*)&len;
				buf.insert(buf.end(), p, p + 2);
				buf.insert(buf.end(), s.begin(), s.begin() + len);
			};
			w32(kCacheMagic);
			w32((uint32)markers.size());
			for (const Marker& m : markers)
			{
				buf.push_back(m.catBit);
				buf.push_back(m.subKind);
				buf.push_back(m.flags);
				wf(m.wx);
				wf(m.wy);
				wf(m.wz);
				w32(m.clearFlagCrc);
				w32(m.discFlagCrc);
				wstr(m.name);
				wstr(m.sub);
			}
			std::error_code ec;
			fs::create_directories(path.parent_path(), ec);
			fs::path tmp = path;
			tmp += ".tmp";
			{
				std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
				if (!f)
					return;
				f.write((const char*)buf.data(), buf.size());
			}
			fs::rename(tmp, path, ec);
			if (ec)
			{
				fs::remove(path, ec);
				fs::rename(tmp, path, ec);
			}
		}

		// ---------------------------------------------------------------- actor name resolution
		std::string ResolveActorName(std::string_view u, const std::unordered_map<std::string, std::string>& actors)
		{
			if (u.empty())
				return {};
			std::string key(u);
			auto look = [&](const std::string& k) -> std::string {
				auto it = actors.find(k);
				return (it != actors.end() && it->second != "???") ? it->second : std::string{};
			};
			if (std::string r = look(key); !r.empty())
				return r;
			if (key.rfind("Item_Mushroom_", 0) == 0)
				if (std::string r = look("Item_MushroomGet_" + key.substr(14)); !r.empty())
					return r;
			if (key.rfind("Item_Plant_", 0) == 0)
				if (std::string r = look("Item_PlantGet_" + key.substr(11)); !r.empty())
					return r;
			if (key.rfind("Animal_Fish_", 0) == 0)
				if (std::string r = look("Item_FishGet_" + key.substr(12)); !r.empty())
					return r;
			if (key == "Animal_Insect_K") return look("Item_Crab_A");
			if (key == "Animal_Insect_O") return look("Item_Crab_B");
			if (key == "Animal_Insect_Z") return look("Item_Crab_C");
			if (key == "Animal_Insect_EP") return look("Animal_Insect_E");
			if (key == "Item_Enemy_Put_57") return look("Item_Enemy_57");
			if (key == "Obj_AncientArrow_A_01") return look("AncientArrow");
			if (key == "Obj_BombArrow_A_01") return look("BombArrow_A");
			if (key == "Obj_FireArrow_A_01") return look("FireArrow");
			if (key == "Obj_IceArrow_A_01") return look("IceArrow");
			if (key == "Obj_ElectricArrow_A_01") return look("ElectricArrow");
			if (key == "GreatFairyBeforeRevival") return "Great Fairy Fountain";
			if (key.rfind("TwnObj_GoddesStatue", 0) == 0) return "Goddess Statue";
			if (key.rfind("TwnObj_DemonStatue", 0) == 0) return "Horned Statue";
			if (key.rfind("Item_CookSet", 0) == 0) return "Cooking Pot";
			if (key.rfind("TBox_Field_Iron_NoReaction_Aoc", 0) == 0 || key.rfind("TBox_Field_Iron_God", 0) == 0) return "EX Treasure Chest";
			if (key.rfind("TBox_Field_Iron", 0) == 0) return "Iron Treasure Chest";
			if (key.rfind("TBox_Field_Wood", 0) == 0) return "Wooden Treasure Chest";
			if (key.rfind("TBox_Field_Stone", 0) == 0) return "Stone Treasure Chest";
			if (key.rfind("TBox_Field_Enemy", 0) == 0) return "Monster Treasure Chest";
			if (key.rfind("Enemy_Assassin_Shooter", 0) == 0) return look("Enemy_Assassin_Junior");
			if (key.rfind("Enemy_Assassin_", 0) == 0) return look("Enemy_Assassin_Middle");
			if (key.rfind("Enemy_Keese_Swarm", 0) == 0) return look("Enemy_Keese");
			if (key.rfind("Enemy_Guardian_A_Fixed", 0) == 0) return look("Enemy_Guardian_A_Fixed");

			std::string s = key;
			boost::replace_all(s, "_Guard_", "_");
			static constexpr const char* kSuffixes[] = {
				"_AllDay", "_LongVisibility", "_TreeHouseTop", "_Mountain",
				"_Fixed_Moss", "_Fixed_Snow", "_Fixed_Sand",
				"_Moss", "_Snow", "_Sand", "_Clean", "_Ambush", "_Near",
				"_Volcano", "_ForestTower", "_Dark"
			};
			for (const char* suf : kSuffixes)
			{
				const size_t slen = strlen(suf);
				if (s.size() > slen && s.compare(s.size() - slen, slen, suf) == 0)
					s.resize(s.size() - slen);
			}
			if (std::string r = look(s); !r.empty())
				return r;
			for (const char* rank : {"_Middle", "_Senior"})
			{
				const size_t rlen = strlen(rank);
				if (s.size() > rlen && s.compare(s.size() - rlen, rlen, rank) == 0)
					if (std::string r = look(s.substr(0, s.size() - rlen) + "_Junior"); !r.empty())
						return r;
			}
			if (std::string r = look(s + "_Junior"); !r.empty())
				return r;
			return key;
		}

		// ---------------------------------------------------------------- build full database from game dump
		bool BuildDatabase(const std::string& lang, std::vector<Marker>& outMarkers)
		{
			const fs::path baseContent = GetBaseTitlePath() / "content";
			const fs::path updContent = FindUpdateContent();
			const fs::path aocContent = FindAocContent();

			auto readContentFile = [&](const std::string& rel, std::vector<uint8>& out) -> bool {
				return (!updContent.empty() && ReadFile(updContent / rel, out)) || ReadFile(baseContent / rel, out);
			};

			// 1. Load MSBT strings for the user's selected language (fallback to EUen / USen)
			std::unordered_map<std::string, std::string> actorNames, dungeonNames, locNames;
			auto tryLoadLang = [&](const std::string& l) -> bool {
				for (const char* region : {"EU", "US", "JP"})
				{
					std::vector<uint8> raw, pack, msgSarc;
					if (!readContentFile(fmt::format("Pack/Bootup_{}{}.pack", region, l), raw))
						continue;
					const std::vector<uint8>& packRef = Yaz0Decode(raw, pack) ? pack : raw;
					std::vector<SarcEntryView> packFiles;
					if (!SarcList(packRef, packFiles))
						continue;
					for (const auto& pf : packFiles)
					{
						if (pf.name.rfind("Message/Msg_", 0) == 0 && pf.name.find(".product.ssarc") != std::string::npos)
						{
							std::vector<uint8> comp(pf.data, pf.data + pf.size);
							if (!Yaz0Decode(comp, msgSarc))
								msgSarc = std::move(comp);
							break;
						}
					}
					if (msgSarc.empty())
						continue;
					std::vector<SarcEntryView> msbts;
					if (!SarcList(msgSarc, msbts))
						continue;
					for (const auto& mf : msbts)
					{
						if (mf.name.rfind("ActorType/", 0) == 0 && mf.name.size() > 5 && mf.name.compare(mf.name.size() - 5, 5, ".msbt") == 0)
							ParseMsbt(mf.data, mf.size, actorNames, true);
						else if (mf.name == "StaticMsg/Dungeon.msbt")
							ParseMsbt(mf.data, mf.size, dungeonNames, false);
						else if (mf.name == "StaticMsg/LocationMarker.msbt")
							ParseMsbt(mf.data, mf.size, locNames, false);
					}
					if (!actorNames.empty())
						return true;
				}
				return false;
			};
			if (!tryLoadLang(lang))
				tryLoadLang("en");

			outMarkers.clear();
			outMarkers.reserve(20000);

			// 2. Parse Map/MainField/Static.smubin (DLC 0010 overrides Bootup.pack)
			{
				std::vector<uint8> rawStatic, decStatic;
				bool gotStatic = !aocContent.empty() && ReadFile(aocContent / "Map/MainField/Static.smubin", rawStatic);
				if (!gotStatic)
				{
					std::vector<uint8> bootRaw, bootPack;
					if (readContentFile("Pack/Bootup.pack", bootRaw))
					{
						const std::vector<uint8>& bp = Yaz0Decode(bootRaw, bootPack) ? bootPack : bootRaw;
						gotStatic = SarcExtractFile(bp, "Map/MainField/Static.smubin", rawStatic);
					}
				}
				if (gotStatic)
				{
					const std::vector<uint8>& stBytes = Yaz0Decode(rawStatic, decStatic) ? decStatic : rawStatic;
					BymlView bv;
					if (bv.Init(stBytes))
					{
						const uint32 root = bv.RootOffset();
						uint8 t = 0;
						uint32 v = 0;

						// LocationMarker (Shrines, Towers, Villages, Stables, Labs, Divine Beasts, Castle)
						if (bv.DictGet(root, "LocationMarker", t, v) && t == 0xC0)
						{
							std::unordered_set<std::string> seenSettlements;
							const uint32 cnt = bv.ArraySize(v);
							for (uint32 i = 0; i < cnt; i++)
							{
								uint8 et = 0;
								uint32 ev = 0;
								if (!bv.ArrayElem(v, i, et, ev) || et != 0xC1)
									continue;
								uint8 it = 0, mt = 0, trt = 0;
								uint32 iv = 0, mv = 0, trv = 0;
								const std::string_view icon = bv.DictGet(ev, "Icon", it, iv) ? bv.GetString(it, iv) : std::string_view{};
								const std::string_view msgId = (bv.DictGet(ev, "MessageID", mt, mv) || bv.DictGet(ev, "MessageId", mt, mv))
																   ? bv.GetString(mt, mv)
																   : std::string_view{};
								float wx = 0, wy = 0, wz = 0;
								if (!bv.DictGet(ev, "Translate", trt, trv) || !bv.GetTranslate(trt, trv, wx, wy, wz))
									continue;

								if (icon == "Dungeon" && msgId.rfind("Dungeon", 0) == 0 && msgId.size() >= 10)
								{
									const int dIdx = std::atoi(std::string(msgId.substr(7)).c_str());
									Marker m;
									m.catBit = 0; // Cat_Shrine
									m.subKind = dIdx >= 120 ? 1 : 0;
									if (dIdx >= 120)
										m.flags |= Flag_DLC;
									m.wx = wx;
									m.wy = wy;
									m.wz = wz;
									m.clearFlagCrc = Crc32(fmt::format("Clear_Dungeon{:03d}", dIdx));
									m.discFlagCrc = Crc32(fmt::format("Enter_Dungeon{:03d}", dIdx));
									const std::string key(msgId);
									auto nit = dungeonNames.find(key);
									m.name = nit != dungeonNames.end() ? nit->second : key;
									auto sit = dungeonNames.find(key + "_sub");
									m.sub = sit != dungeonNames.end() ? sit->second : (dIdx >= 120 ? "Champions' Ballad Shrine" : "Ancient Shrine");
									BuildSearchKey(m);
									outMarkers.push_back(std::move(m));
								}
								else if (icon == "Tower" && msgId.rfind("Tower", 0) == 0 && msgId.size() >= 7)
								{
									const int tIdx = std::atoi(std::string(msgId.substr(5)).c_str());
									Marker m;
									m.catBit = 1; // Cat_Tower
									m.subKind = 0;
									m.wx = wx;
									m.wy = wy;
									m.wz = wz;
									m.clearFlagCrc = Crc32(fmt::format("MapTower_{:02d}", tIdx));
									const std::string key(msgId);
									auto nit = locNames.find(key);
									m.name = nit != locNames.end() ? nit->second : key;
									m.sub = "Sheikah Tower";
									BuildSearchKey(m);
									outMarkers.push_back(std::move(m));
								}
								else if (icon == "Village" || icon == "Hatago" || icon == "Labo" || icon == "Castle" ||
										 icon.rfind("Remains", 0) == 0)
								{
									const std::string dedupKey = std::string(icon) + ":" + std::string(msgId);
									if (!seenSettlements.insert(dedupKey).second)
										continue;
									Marker m;
									m.catBit = 6; // Cat_Settlement
									m.subKind = icon == "Hatago" ? 1 : icon == "Labo" ? 2 : icon.rfind("Remains", 0) == 0 ? 6 : 0;
									m.wx = wx;
									m.wy = wy;
									m.wz = wz;
									const std::string key(msgId);
									auto nit = locNames.find(key);
									m.name = nit != locNames.end() ? nit->second : key;
									m.sub = icon == "Hatago" ? "Stable" : icon == "Labo" ? "Ancient Tech Lab"
										  : icon == "Castle" ? "Landmark" : icon.rfind("Remains", 0) == 0 ? "Divine Beast" : "Village";
									BuildSearchKey(m);
									outMarkers.push_back(std::move(m));
								}
							}
						}

						// KorokLocation (all 900 Korok Seeds)
						if (bv.DictGet(root, "KorokLocation", t, v) && t == 0xC0)
						{
							const uint32 cnt = bv.ArraySize(v);
							for (uint32 i = 0; i < cnt; i++)
							{
								uint8 et = 0;
								uint32 ev = 0;
								if (!bv.ArrayElem(v, i, et, ev) || et != 0xC1)
									continue;
								uint8 ft = 0, trt = 0;
								uint32 fv = 0, trv = 0;
								float wx = 0, wy = 0, wz = 0;
								if (!bv.DictGet(ev, "Translate", trt, trv) || !bv.GetTranslate(trt, trv, wx, wy, wz))
									continue;
								const std::string_view flagStr = bv.DictGet(ev, "Flag", ft, fv) ? bv.GetString(ft, fv) : std::string_view{};
								Marker m;
								m.catBit = 2; // Cat_Korok
								m.subKind = 0;
								m.wx = wx;
								m.wy = wy;
								m.wz = wz;
								if (!flagStr.empty())
									m.clearFlagCrc = Crc32(flagStr);
								m.name = "Korok Seed";
								m.sub = fmt::format("Korok #{:03d}", i + 1);
								BuildSearchKey(m);
								outMarkers.push_back(std::move(m));
							}
						}
					}
				}
			}

			// 3. Scan all 80 MainField sections (A-1 .. J-8, Static & Dynamic)
			std::vector<uint8> titleBgSarc;
			bool titleBgLoaded = false;
			auto ensureTitleBg = [&]() -> const std::vector<uint8>& {
				if (!titleBgLoaded)
				{
					titleBgLoaded = true;
					std::vector<uint8> raw;
					if (readContentFile("Pack/TitleBG.pack", raw))
					{
						if (!Yaz0Decode(raw, titleBgSarc))
							titleBgSarc = std::move(raw);
					}
				}
				return titleBgSarc;
			};

			std::unordered_set<uint32> seenHashes;
			seenHashes.reserve(65536);

			for (char col = 'A'; col <= 'J'; col++)
			{
				for (int row = 1; row <= 8; row++)
				{
					const std::string sec = fmt::format("{}-{}", col, row);
					for (const char* kind : {"Static", "Dynamic"})
					{
						const std::string rel = fmt::format("Map/MainField/{}/{}_{}.smubin", sec, sec, kind);
						std::vector<uint8> raw, dec;
						bool ok = (!aocContent.empty() && ReadFile(aocContent / rel, raw)) || readContentFile(rel, raw);
						if (!ok && strcmp(kind, "Static") == 0)
							ok = SarcExtractFile(ensureTitleBg(), rel, raw);
						if (!ok)
							continue;
						const std::vector<uint8>& bytes = Yaz0Decode(raw, dec) ? dec : raw;
						BymlView bv;
						if (!bv.Init(bytes))
							continue;
						uint8 ot = 0;
						uint32 ov = 0;
						if (!bv.DictGet(bv.RootOffset(), "Objs", ot, ov) || ot != 0xC0)
							continue;
						const uint32 cnt = bv.ArraySize(ov);
						for (uint32 i = 0; i < cnt; i++)
						{
							uint8 et = 0;
							uint32 ev = 0;
							if (!bv.ArrayElem(ov, i, et, ev) || et != 0xC1)
								continue;
							uint8 ut = 0;
							uint32 uv = 0;
							if (!bv.DictGet(ev, "UnitConfigName", ut, uv))
								continue;
							const std::string_view u = bv.GetString(ut, uv);
							if (u.empty())
								continue;

							// Fast filter by actor prefix before reading coordinates/parameters
							uint8 catBit = 0xFF;
							uint8 subKind = 0;
							uint8 mflags = 0;
							bool readEquips = false;
							bool readDrop = false;

							if (u.rfind("TBox_Field_", 0) == 0)
							{
								catBit = 3; // Cat_Chest
								readDrop = true;
								if (u.find("Aoc") != std::string_view::npos || u.find("God") != std::string_view::npos)
								{
									subKind = 4;
									mflags |= Flag_DLC;
								}
								else if (u.find("Enemy") != std::string_view::npos)
									subKind = 3;
								else if (u.find("Stone") != std::string_view::npos)
									subKind = 2;
								else if (u.find("Iron") != std::string_view::npos)
									subKind = 1;
								else
									subKind = 0;
							}
							else if (u.rfind("Enemy_Lynel_", 0) == 0)
							{
								catBit = 4; // Cat_Boss
								subKind = 0;
								readEquips = true;
							}
							else if (u.rfind("Enemy_Giant_", 0) == 0)
							{
								catBit = 4; // Cat_Boss
								subKind = 1;
								readEquips = true;
							}
							else if (u.rfind("Enemy_Golem_", 0) == 0 && u.rfind("Enemy_Golem_Little", 0) != 0)
							{
								catBit = 4; // Cat_Boss
								subKind = u.find("Fire") != std::string_view::npos ? 3 : u.find("Ice") != std::string_view::npos ? 4 : 2;
							}
							else if (u.rfind("Enemy_Sandworm", 0) == 0)
							{
								catBit = 4; // Cat_Boss
								subKind = 5;
							}
							else if (u.rfind("Enemy_Guardian_A", 0) == 0 || u.rfind("Enemy_Guardian_B", 0) == 0 || u.rfind("Enemy_Guardian_C", 0) == 0)
							{
								catBit = 5; // Cat_Guardian
								subKind = u.rfind("Enemy_Guardian_C", 0) == 0 ? 1
										: u.rfind("Enemy_Guardian_B", 0) == 0 ? 2
										: u.find("Fixed") != std::string_view::npos ? 3 : 0;
							}
							else if (u == "GreatFairyBeforeRevival" || u.rfind("TwnObj_GoddesStatue", 0) == 0 ||
									 u.rfind("TwnObj_DemonStatue", 0) == 0 || u.rfind("Item_CookSet", 0) == 0)
							{
								catBit = 6; // Cat_Settlement
								subKind = u == "GreatFairyBeforeRevival" ? 3
										: u.rfind("Item_CookSet", 0) == 0 ? 5 : 4;
							}
							else if (u.rfind("Weapon_", 0) == 0)
							{
								catBit = 7; // Cat_Weapon
								subKind = u.rfind("Weapon_Spear_", 0) == 0 ? 1
										: u.rfind("Weapon_Bow_", 0) == 0 ? 2
										: u.rfind("Weapon_Shield_", 0) == 0 ? 3 : 0;
							}
							else if (u == "Obj_Mineral_A_01" || u == "Obj_Mineral_B_01" || u == "Obj_Mineral_C_01" || u.rfind("Item_Ore_", 0) == 0)
							{
								catBit = 8; // Cat_Ore
								subKind = u == "Obj_Mineral_B_01" ? 1 : u == "Obj_Mineral_C_01" ? 2 : 0;
							}
							else if (u.rfind("Item_Fruit_", 0) == 0 || u.rfind("Item_Mushroom_", 0) == 0 ||
									 u.rfind("Item_Plant_", 0) == 0 || u.rfind("Item_Material_", 0) == 0 ||
									 u.rfind("Animal_Fish_", 0) == 0 || u.rfind("Animal_Insect_", 0) == 0 ||
									 u == "Animal_RupeeRabbit_A")
							{
								catBit = 9; // Cat_Material
								subKind = u.rfind("Item_Mushroom_", 0) == 0 ? 1
										: u == "Animal_RupeeRabbit_A" ? 3
										: u.rfind("Animal_", 0) == 0 ? 2 : 0;
							}
							else if (u.rfind("Enemy_Bokoblin_", 0) == 0 || u.rfind("Enemy_Moriblin_", 0) == 0 ||
									 u.rfind("Enemy_Lizalfos_", 0) == 0 || u.rfind("Enemy_Wizzrobe_", 0) == 0 ||
									 u.rfind("Enemy_Assassin_", 0) == 0)
							{
								catBit = 10; // Cat_Enemy
								subKind = 0;
								readEquips = true;
							}

							if (catBit == 0xFF)
								continue;

							uint8 ht = 0, trt = 0;
							uint32 hv = 0, trv = 0;
							const uint32 hashId = bv.DictGet(ev, "HashId", ht, hv) ? bv.GetUInt(ht, hv) : 0;
							if (hashId != 0 && !seenHashes.insert(hashId).second)
								continue;
							float wx = 0, wy = 0, wz = 0;
							if (!bv.DictGet(ev, "Translate", trt, trv) || !bv.GetTranslate(trt, trv, wx, wy, wz))
								continue;

							std::string subText;
							uint8 pt = 0;
							uint32 pv = 0;
							if (bv.DictGet(ev, "!Parameters", pt, pv) && pt == 0xC1)
							{
								uint8 ftype = 0;
								uint32 fval = 0;
								if (bv.DictGet(pv, "IsHardModeActor", ftype, fval) && bv.GetBool(ftype, fval))
									mflags |= Flag_HardMode;
								if (bv.DictGet(pv, "IsInGround", ftype, fval) && bv.GetBool(ftype, fval))
									mflags |= Flag_InGround;
								if (bv.DictGet(pv, "IsIchigekiActor", ftype, fval) && bv.GetBool(ftype, fval))
									mflags |= Flag_DLC;

								if (readDrop && bv.DictGet(pv, "DropActor", ftype, fval))
								{
									const std::string_view drop = bv.GetString(ftype, fval);
									if (!drop.empty() && drop != "Normal")
										subText = ResolveActorName(drop, actorNames);
								}
								if (readEquips)
								{
									for (const char* eqKey : {"EquipItem1", "EquipItem2", "EquipItem3", "EquipItem4"})
									{
										if (bv.DictGet(pv, eqKey, ftype, fval))
										{
											const std::string_view eq = bv.GetString(ftype, fval);
											if (!eq.empty() && eq != "Default")
											{
												const std::string eqName = ResolveActorName(eq, actorNames);
												if (!eqName.empty())
												{
													if (!subText.empty())
														subText += ", ";
													subText += eqName;
												}
											}
										}
									}
								}
							}

							Marker m;
							m.catBit = catBit;
							m.subKind = subKind;
							m.flags = mflags;
							m.wx = wx;
							m.wy = wy;
							m.wz = wz;
							m.name = ResolveActorName(u, actorNames);
							if (catBit == 3) // Treasure Chest
							{
								if (hashId != 0)
									m.clearFlagCrc = Crc32(fmt::format("MainField_{}_{}", u, hashId));
								if (subText.empty())
									subText = "Random / Table Drop";
							}
							else if (subText.empty())
							{
								subText = kCatMeta[catBit].name;
							}
							m.sub = std::move(subText);
							BuildSearchKey(m);
							outMarkers.push_back(std::move(m));
						}
					}
				}
			}
			return !outMarkers.empty();
		}

		void EnsureDatabaseLoaded()
		{
			const std::string lang = GetSettings().language.empty() ? "en" : GetSettings().language;
			if (sDbReady.load() && sLoadedLang == lang)
				return;
			if (sDbLoading.exchange(true))
				return;

			std::thread([lang]() {
				const auto t0 = std::chrono::steady_clock::now();
				const fs::path cachePath = GetDataDir() / fmt::format("map_cache_v4_{}.bin", lang);
				std::vector<Marker> loaded;
				bool fromCache = LoadCache(cachePath, loaded);
				if (!fromCache)
				{
					if (BuildDatabase(lang, loaded))
						SaveCache(cachePath, loaded);
				}
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
				cemuLog_log(LogType::Force, "PCE map: loaded {} markers ({}) in {} ms", loaded.size(), fromCache ? "cache" : "dump", ms);
				{
					std::lock_guard lock(sDbMutex);
					sMarkers = std::move(loaded);
					sLoadedLang = lang;
					sBoundBoolArr = 0;
					sBoundBoolCnt = 0;
					sLastFlagRefreshFrame = 0;
					sSearchMatches.clear();
					sDbReady = true;
				}
				sDbLoading = false;
			}).detach();
		}

		// ---------------------------------------------------------------- live ksys::gdt::Manager save flag reader
		bool ValidRamPtr(uint32 a, uint32 len = 4)
		{
			return a >= 0x10000000u && a < 0xF0000000u && memory_isAddressRangeAccessible(a, len);
		}

		void RefreshLiveFlags(uint32 frame)
		{
			if (!sDbReady.load())
				return;
			if (sBoundBoolArr != 0 && frame - sLastFlagRefreshFrame < 12)
				return;
			sLastFlagRefreshFrame = frame;

			// U-King v1.5.0: ksys::gdt::Manager::sInstance at 0x1046D5B0 -> +0x70C -> working TriggerParam (tp1)
			constexpr uint32 kGdtInstancePtr = 0x1046D5B0u;
			if (!ValidRamPtr(kGdtInstancePtr, 4))
				return;
			const uint32 gdtMgr = memory_readU32(kGdtInstancePtr);
			if (!ValidRamPtr(gdtMgr + 0x70C, 8))
				return;
			const uint32 tpPtr = memory_readU32(gdtMgr + 0x70C);
			if (!ValidRamPtr(tpPtr, 4))
				return;
			const uint32 tp1 = memory_readU32(tpPtr);
			if (!ValidRamPtr(tp1, 0x20))
				return;
			const uint32 boolCnt = memory_readU32(tp1 + 0x04);
			const uint32 boolArr = memory_readU32(tp1 + 0x0C);
			if (boolCnt < 1000 || boolCnt > 100000 || !ValidRamPtr(boolArr, boolCnt * 4))
				return;

			// Bind each marker's CRC32 to its live FlagBool value address (fp + 0x08) when boolArr changes
			if (boolArr != sBoundBoolArr || boolCnt != sBoundBoolCnt)
			{
				std::unordered_map<uint32, uint32> crcToValAddr;
				crcToValAddr.reserve(boolCnt);
				for (uint32 i = 0; i < boolCnt; i++)
				{
					const uint32 fp = memory_readU32(boolArr + i * 4);
					if (!ValidRamPtr(fp, 12))
						continue;
					const uint32 def = memory_readU32(fp + 0x04);
					if (!ValidRamPtr(def, 12))
						continue;
					const uint32 crc = memory_readU32(def + 0x08);
					crcToValAddr[crc] = fp + 0x08;
				}
				for (Marker& m : sMarkers)
				{
					m.clearFlagAddr = 0;
					m.discFlagAddr = 0;
					if (m.clearFlagCrc)
					{
						auto it = crcToValAddr.find(m.clearFlagCrc);
						if (it != crcToValAddr.end())
							m.clearFlagAddr = it->second;
					}
					if (m.discFlagCrc)
					{
						auto it = crcToValAddr.find(m.discFlagCrc);
						if (it != crcToValAddr.end())
							m.discFlagAddr = it->second;
					}
				}
				sBoundBoolArr = boolArr;
				sBoundBoolCnt = boolCnt;
			}

			const bool showMaster = GetSettings().mapMasterMode;
			sCatTotal.fill(0);
			sCatClaimed.fill(0);
			for (Marker& m : sMarkers)
			{
				if ((m.flags & Flag_HardMode) && !showMaster)
					continue;
				sCatTotal[m.catBit]++;
				if (m.clearFlagAddr && ValidRamPtr(m.clearFlagAddr, 1))
					m.claimed = (memory_readU8(m.clearFlagAddr) & 1) != 0;
				else
					m.claimed = false;
				if (m.discFlagAddr && ValidRamPtr(m.discFlagAddr, 1))
					m.discovered = (memory_readU8(m.discFlagAddr) & 1) != 0;
				else
					m.discovered = false;
				if (m.claimed)
					sCatClaimed[m.catBit]++;
			}
		}

		// ---------------------------------------------------------------- custom vector icon rendering
		ImU32 MulAlpha(ImU32 c, float a)
		{
			const uint32 ca = (c >> 24) & 0xFF;
			const uint32 na = (uint32)std::clamp(ca * a, 0.0f, 255.0f);
			return (c & 0x00FFFFFFu) | (na << 24);
		}

		void DrawDiamond(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 fill, ImU32 border, float thick = 1.5f)
		{
			const ImVec2 pts[4] = {ImVec2(c.x, c.y - ry), ImVec2(c.x + rx, c.y), ImVec2(c.x, c.y + ry), ImVec2(c.x - rx, c.y)};
			if (fill)
				dl->AddConvexPolyFilled(pts, 4, fill);
			if (border)
				dl->AddPolyline(pts, 4, border, ImDrawFlags_Closed, thick);
		}

		void DrawMarkerIcon(ImDrawList* dl, ImVec2 c, float r, uint8 catBit, uint8 subKind, bool discoveredIncomplete, float alpha)
		{
			const ImU32 shadow = MulAlpha(IM_COL32(0, 0, 0, 195), alpha);
			const ImU32 darkBg = MulAlpha(IM_COL32(12, 18, 26, 235), alpha);
			const ImU32 white  = MulAlpha(IM_COL32(250, 248, 238, 255), alpha);
			const float lw = std::max(1.2f, r * 0.16f);
			const int seg = r < 8.5f ? 8 : 12;

			switch (catBit)
			{
			case 0: // Shrine (Sheikah diamond emblem + arch/eye)
			{
				const ImU32 accent = subKind == 1 ? MulAlpha(IM_COL32(255, 195, 55, 255), alpha) : MulAlpha(IM_COL32(55, 218, 255, 255), alpha);
				const ImU32 core   = discoveredIncomplete ? MulAlpha(IM_COL32(255, 140, 35, 255), alpha) : accent;
				DrawDiamond(dl, ImVec2(c.x, c.y + 1.0f), r * 1.02f, r * 1.02f, shadow, 0);
				DrawDiamond(dl, c, r * 0.95f, r * 0.95f, darkBg, accent, lw);
				if (r >= 6.0f)
				{
					DrawDiamond(dl, c, r * 0.55f, r * 0.55f, 0, MulAlpha(core, 0.85f), lw * 0.8f);
					dl->AddCircleFilled(c, r * 0.22f, core, 8);
				}
				break;
			}
			case 1: // Sheikah Tower (tapered spire inside glowing crest)
			{
				const ImU32 cyan = MulAlpha(IM_COL32(75, 230, 255, 255), alpha);
				DrawDiamond(dl, ImVec2(c.x, c.y + 1.0f), r * 1.02f, r * 1.02f, shadow, 0);
				DrawDiamond(dl, c, r * 0.95f, r * 0.95f, darkBg, cyan, lw);
				const ImVec2 spire[3] = {ImVec2(c.x, c.y - r * 0.70f), ImVec2(c.x + r * 0.38f, c.y + r * 0.55f), ImVec2(c.x - r * 0.38f, c.y + r * 0.55f)};
				dl->AddConvexPolyFilled(spire, 3, MulAlpha(cyan, 0.45f));
				dl->AddPolyline(spire, 3, cyan, ImDrawFlags_Closed, lw * 0.85f);
				dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.70f), r * 0.18f, white, 8);
				break;
			}
			case 2: // Korok Seed (notched emerald leaf + stem)
			{
				const ImU32 green = MulAlpha(IM_COL32(78, 228, 102, 255), alpha);
				const ImU32 darkG = MulAlpha(IM_COL32(16, 48, 24, 240), alpha);
				dl->AddCircleFilled(ImVec2(c.x, c.y + 1.0f), r * 1.02f, shadow, seg);
				dl->AddCircleFilled(c, r * 0.94f, darkG, seg);
				dl->AddCircle(c, r * 0.94f, green, seg, lw);
				const ImVec2 leaf[5] = {
					ImVec2(c.x + r * 0.48f, c.y - r * 0.52f),
					ImVec2(c.x + r * 0.45f, c.y + r * 0.35f),
					ImVec2(c.x - r * 0.15f, c.y + r * 0.55f),
					ImVec2(c.x - r * 0.52f, c.y + r * 0.05f),
					ImVec2(c.x - r * 0.25f, c.y - r * 0.48f),
				};
				dl->AddConvexPolyFilled(leaf, 5, green);
				dl->AddLine(ImVec2(c.x - r * 0.48f, c.y + r * 0.52f), ImVec2(c.x + r * 0.25f, c.y - r * 0.25f), darkG, lw * 0.9f);
				break;
			}
			case 3: // Treasure Chest (box + arched lid + gold clasp)
			{
				const ImU32 bodyCol = subKind == 4 ? MulAlpha(IM_COL32(190, 65, 235, 255), alpha)  // EX / DLC
									: subKind == 3 ? MulAlpha(IM_COL32(215, 70, 130, 255), alpha)  // Enemy chest
									: subKind == 2 ? MulAlpha(IM_COL32(150, 175, 195, 255), alpha) // Stone chest
									: subKind == 1 ? MulAlpha(IM_COL32(85, 180, 235, 255), alpha)  // Iron chest
												   : MulAlpha(IM_COL32(228, 155, 52, 255), alpha); // Wooden chest
				const ImU32 trimCol = subKind == 4 ? MulAlpha(IM_COL32(255, 215, 65, 255), alpha) : white;
				const float bw = r * 0.92f, bh = r * 0.72f;
				dl->AddRectFilled(ImVec2(c.x - bw - 1, c.y - bh), ImVec2(c.x + bw + 1, c.y + bh + 2), shadow, r * 0.25f);
				dl->AddRectFilled(ImVec2(c.x - bw, c.y - bh), ImVec2(c.x + bw, c.y + bh), bodyCol, r * 0.22f);
				dl->AddRect(ImVec2(c.x - bw, c.y - bh), ImVec2(c.x + bw, c.y + bh), trimCol, r * 0.22f, 0, lw * 0.9f);
				dl->AddLine(ImVec2(c.x - bw, c.y - bh * 0.1f), ImVec2(c.x + bw, c.y - bh * 0.1f), darkBg, lw);
				dl->AddRectFilled(ImVec2(c.x - r * 0.20f, c.y - r * 0.26f), ImVec2(c.x + r * 0.20f, c.y + r * 0.16f), trimCol, 1.0f);
				break;
			}
			case 4: // Overworld Boss (horned skull / golem crest)
			{
				const ImU32 col = subKind == 0 ? MulAlpha(IM_COL32(255, 65, 65, 255), alpha)   // Lynel
								: subKind == 1 ? MulAlpha(IM_COL32(250, 125, 45, 255), alpha)  // Hinox
								: subKind == 3 ? MulAlpha(IM_COL32(255, 95, 40, 255), alpha)   // Igneo Talus
								: subKind == 4 ? MulAlpha(IM_COL32(85, 220, 255, 255), alpha)  // Frost Talus
								: subKind == 5 ? MulAlpha(IM_COL32(240, 195, 65, 255), alpha)  // Molduga
											   : MulAlpha(IM_COL32(210, 165, 105, 255), alpha);// Stone Talus
				dl->AddCircleFilled(ImVec2(c.x, c.y + 1.0f), r * 1.12f, shadow, 8);
				dl->AddCircleFilled(c, r * 1.04f, darkBg, 8);
				dl->AddCircle(c, r * 1.04f, col, 8, lw * 1.1f);
				// Horns + fierce eyes / cyclops eye
				dl->AddLine(ImVec2(c.x - r * 0.35f, c.y - r * 0.25f), ImVec2(c.x - r * 0.78f, c.y - r * 0.78f), col, lw);
				dl->AddLine(ImVec2(c.x + r * 0.35f, c.y - r * 0.25f), ImVec2(c.x + r * 0.78f, c.y - r * 0.78f), col, lw);
				if (subKind == 1) // Hinox single eye
				{
					dl->AddCircleFilled(c, r * 0.36f, col, 8);
					dl->AddCircleFilled(c, r * 0.15f, white, 8);
				}
				else
				{
					const ImVec2 head[4] = {ImVec2(c.x, c.y - r * 0.55f), ImVec2(c.x + r * 0.52f, c.y), ImVec2(c.x, c.y + r * 0.58f), ImVec2(c.x - r * 0.52f, c.y)};
					dl->AddConvexPolyFilled(head, 4, col);
					dl->AddCircleFilled(ImVec2(c.x - r * 0.20f, c.y - r * 0.05f), r * 0.13f, darkBg, 6);
					dl->AddCircleFilled(ImVec2(c.x + r * 0.20f, c.y - r * 0.05f), r * 0.13f, darkBg, 6);
				}
				break;
			}
			case 5: // Guardian (magenta reticle + glowing Sheikah eye)
			{
				const ImU32 pink = MulAlpha(IM_COL32(255, 65, 145, 255), alpha);
				const ImU32 cyan = MulAlpha(IM_COL32(70, 235, 255, 255), alpha);
				dl->AddCircleFilled(ImVec2(c.x, c.y + 1.0f), r * 1.04f, shadow, seg);
				dl->AddCircleFilled(c, r * 0.96f, darkBg, seg);
				dl->AddCircle(c, r * 0.96f, pink, seg, lw);
				const ImVec2 tri[3] = {ImVec2(c.x, c.y - r * 0.68f), ImVec2(c.x + r * 0.62f, c.y + r * 0.45f), ImVec2(c.x - r * 0.62f, c.y + r * 0.45f)};
				dl->AddPolyline(tri, 3, pink, ImDrawFlags_Closed, lw * 0.85f);
				dl->AddCircleFilled(c, r * 0.28f, cyan, 8);
				dl->AddCircleFilled(c, r * 0.12f, pink, 6);
				break;
			}
			case 6: // Towns, Stables, Fairy Fountains, Goddess Statues, Cooking Pots
			{
				const ImU32 col = subKind == 3 ? MulAlpha(IM_COL32(255, 130, 210, 255), alpha) // Great Fairy
								: subKind == 4 ? MulAlpha(IM_COL32(255, 232, 145, 255), alpha) // Goddess Statue
								: subKind == 5 ? MulAlpha(IM_COL32(255, 145, 65, 255), alpha)  // Cooking Pot
											   : MulAlpha(IM_COL32(245, 215, 125, 255), alpha);// Village / Stable / Lab
				dl->AddCircleFilled(ImVec2(c.x, c.y + 1.0f), r * 1.02f, shadow, seg);
				dl->AddCircleFilled(c, r * 0.94f, darkBg, seg);
				dl->AddCircle(c, r * 0.94f, col, seg, lw);
				if (subKind == 5) // Cooking Pot bowl + flame
				{
					dl->AddRectFilled(ImVec2(c.x - r * 0.52f, c.y - r * 0.18f), ImVec2(c.x + r * 0.52f, c.y + r * 0.35f), col, r * 0.25f);
					dl->AddLine(ImVec2(c.x - r * 0.35f, c.y + r * 0.52f), ImVec2(c.x + r * 0.35f, c.y + r * 0.52f), MulAlpha(IM_COL32(255, 90, 40, 255), alpha), lw);
				}
				else // Four-point star / roof crest
				{
					DrawDiamond(dl, c, r * 0.55f, r * 0.55f, col, 0);
					dl->AddCircleFilled(c, r * 0.20f, darkBg, 8);
				}
				break;
			}
			case 7: // Weapons, Bows & Shields
			{
				const ImU32 steel = MulAlpha(IM_COL32(135, 205, 255, 255), alpha);
				dl->AddCircleFilled(ImVec2(c.x, c.y + 1.0f), r * 0.98f, shadow, seg);
				dl->AddCircleFilled(c, r * 0.90f, darkBg, seg);
				dl->AddCircle(c, r * 0.90f, steel, seg, lw * 0.9f);
				if (subKind == 3) // Shield
				{
					const ImVec2 sh[5] = {
						ImVec2(c.x - r * 0.44f, c.y - r * 0.45f), ImVec2(c.x + r * 0.44f, c.y - r * 0.45f),
						ImVec2(c.x + r * 0.40f, c.y + r * 0.10f), ImVec2(c.x, c.y + r * 0.56f),
						ImVec2(c.x - r * 0.40f, c.y + r * 0.10f)
					};
					dl->AddConvexPolyFilled(sh, 5, steel);
				}
				else if (subKind == 2) // Bow
				{
					dl->PathArcTo(c, r * 0.52f, -1.1f, 1.1f, 8);
					dl->PathStroke(steel, 0, lw);
					dl->AddLine(ImVec2(c.x - r * 0.48f, c.y), ImVec2(c.x + r * 0.55f, c.y), white, lw * 0.85f);
				}
				else // Sword / Spear blade
				{
					dl->AddLine(ImVec2(c.x - r * 0.46f, c.y + r * 0.46f), ImVec2(c.x + r * 0.48f, c.y - r * 0.48f), white, lw * 1.15f);
					dl->AddLine(ImVec2(c.x - r * 0.42f, c.y + r * 0.05f), ImVec2(c.x - r * 0.05f, c.y + r * 0.42f), steel, lw);
				}
				break;
			}
			case 8: // Ore Deposits (faceted crystal gem)
			{
				const ImU32 gem = subKind == 1 ? MulAlpha(IM_COL32(255, 212, 55, 255), alpha)  // Rare Ore (Gold)
								: subKind == 2 ? MulAlpha(IM_COL32(55, 245, 215, 255), alpha)  // Luminous Stone (Aqua)
											   : MulAlpha(IM_COL32(175, 205, 235, 255), alpha);// Standard Ore
				const ImVec2 hex[6] = {
					ImVec2(c.x, c.y - r * 0.95f), ImVec2(c.x + r * 0.78f, c.y - r * 0.38f),
					ImVec2(c.x + r * 0.78f, c.y + r * 0.38f), ImVec2(c.x, c.y + r * 0.95f),
					ImVec2(c.x - r * 0.78f, c.y + r * 0.38f), ImVec2(c.x - r * 0.78f, c.y - r * 0.38f)
				};
				dl->AddConvexPolyFilled(hex, 6, darkBg);
				dl->AddPolyline(hex, 6, gem, ImDrawFlags_Closed, lw);
				DrawDiamond(dl, c, r * 0.44f, r * 0.58f, gem, white, lw * 0.6f);
				break;
			}
			case 9: // Materials, Plants, Mushrooms, Fish
			{
				const ImU32 col = subKind == 1 ? MulAlpha(IM_COL32(245, 145, 95, 255), alpha)  // Mushroom
								: subKind == 2 ? MulAlpha(IM_COL32(95, 210, 250, 255), alpha)  // Critter / Fish
								: subKind == 3 ? MulAlpha(IM_COL32(120, 240, 255, 255), alpha) // Blupee
											   : MulAlpha(IM_COL32(125, 230, 140, 255), alpha);// Plant / Fruit
				dl->AddCircleFilled(c, r * 0.82f, darkBg, seg);
				dl->AddCircle(c, r * 0.82f, col, seg, lw * 0.9f);
				dl->AddCircleFilled(c, r * 0.40f, col, 8);
				break;
			}
			default: // Enemies (Weapon Carriers)
			{
				const ImU32 col = MulAlpha(IM_COL32(215, 105, 225, 255), alpha);
				dl->AddCircleFilled(c, r * 0.85f, darkBg, seg);
				dl->AddCircle(c, r * 0.85f, col, seg, lw * 0.9f);
				DrawDiamond(dl, c, r * 0.42f, r * 0.42f, col, 0);
				break;
			}
			}
		}

		// ---------------------------------------------------------------- search query matching
		void UpdateSearchQuery(const std::string& q, const UI::PointerSnapshot& ps)
		{
			sSearchQuery = q;
			sSearchScroll = 0;
			sSearchTokens.clear();
			sSearchMatches.clear();

			std::string low = ToLowerAscii(q);
			boost::trim(low);
			if (low.empty())
				return;
			boost::split(sSearchTokens, low, boost::is_any_of(" \t"), boost::token_compress_on);
			sSearchTokens.erase(std::remove_if(sSearchTokens.begin(), sSearchTokens.end(), [](const std::string& t) { return t.empty(); }), sSearchTokens.end());
			if (sSearchTokens.empty())
				return;

			const Settings s = GetSettings();
			for (int i = 0; i < (int)sMarkers.size(); i++)
			{
				const Marker& m = sMarkers[i];
				if ((m.flags & Flag_HardMode) && !s.mapMasterMode)
					continue;
				if (s.mapAutoHide && m.claimed)
					continue;
				bool allMatch = true;
				for (const std::string& tok : sSearchTokens)
				{
					if (m.searchKey.find(tok) == std::string::npos)
					{
						allMatch = false;
						break;
					}
				}
				if (allMatch)
					sSearchMatches.push_back(i);
			}

			// Sort search matches by distance from the current map center so nearby matches appear first
			float centerWx = 0.0f, centerWz = 0.0f;
			if (ps.mapCtrl && std::fabs(ps.mapScaleX) > 1e-4f && std::fabs(ps.mapScaleY) > 1e-4f)
			{
				centerWx = (0.5f - ps.mapX) * 1280.0f / (2.0f * ps.mapScaleX);
				centerWz = (0.5f - ps.mapY) * 720.0f / (2.0f * ps.mapScaleY);
			}
			std::sort(sSearchMatches.begin(), sSearchMatches.end(), [&](int a, int b) {
				const float dxa = sMarkers[a].wx - centerWx, dza = sMarkers[a].wz - centerWz;
				const float dxb = sMarkers[b].wx - centerWx, dzb = sMarkers[b].wz - centerWz;
				return (dxa * dxa + dza * dza) < (dxb * dxb + dzb * dzb);
			});
		}

#ifndef VK_OEM_MINUS
#define VK_OEM_MINUS 0xBD
#endif
#ifndef VK_OEM_7
#define VK_OEM_7 0xDE
#endif

		char KeyToSearchChar(Key k, bool shift)
		{
			const uint16 v = (uint16)k;
			if (v >= 'A' && v <= 'Z')
				return shift ? (char)v : (char)(v - 'A' + 'a');
			if (v >= '0' && v <= '9')
				return (char)v;
			if (k == Key::Space)
				return ' ';
			if (v == VK_OEM_MINUS)
				return '_';
			if (v == VK_OEM_7)
				return '\'';
			return 0;
		}
	}

	// ---------------------------------------------------------------- main render & interaction
	bool Render(double now, float dt)
	{
		(void)dt;
		const uint32 frame = Input::GetDebug().frameCounter.load();
		const UI::State& st = UI::Get(frame);
		const UI::PointerSnapshot& ps = UI::GetPointer(frame);

		static bool sWasMapOpen = false;
		if (!st.map || !ps.hasMap)
		{
			sWasMapOpen = false;
			if (sSearchFocused)
			{
				sSearchFocused = false;
				Input::SetEmbeddedFocus(false);
			}
			return false;
		}
		if (!sWasMapOpen)
		{
			sWasMapOpen = true;
			Input::ClearKeyPresses();
			Input::PopOverlayWheel();
		}

		EnsureDatabaseLoaded();

		ImGuiIO& io = ImGui::GetIO();
		ImDrawList* bgDl = ImGui::GetBackgroundDrawList();
		ImDrawList* fgDl = ImGui::GetForegroundDrawList();

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

		ImFont* fontTitle = Overlay::GetTitleFont();
		ImFont* fontItem  = Overlay::GetItemFont();
		ImFont* fontSmall = Overlay::GetSmallFont();
		const float fs = std::max(13.0f, Overlay::GetFontHeight() * 0.78f);

		Settings s = GetSettings();
		const bool modalActive = (ps.hasBoxCursor && ps.Selected() >= 0) || st.dialog;
		const bool mouseClicked = io.MouseDown[0] && !sMouseWasDown;
		sMouseWasDown = io.MouseDown[0];

		// ---- handle keyboard input when search box is focused (or global shortcuts on map) ----
		{
			const bool shift = Input::IsKeyDown(Key::LShift) || Input::IsKeyDown(Key::RShift);
			const bool ctrl  = Input::IsKeyDown(Key::LCtrl)  || Input::IsKeyDown(Key::RCtrl);
			std::optional<Key> k;
			while ((k = Input::PopKeyPress()))
			{
				if (sSearchFocused)
				{
					if (*k == Key::Escape || *k == Key::Enter)
					{
						sSearchFocused = false;
						Input::SetEmbeddedFocus(false);
						break;
					}
					else if (*k == Key::Backspace)
					{
						if (!sSearchQuery.empty())
						{
							if (ctrl)
								sSearchQuery.clear();
							else
								sSearchQuery.pop_back();
							std::lock_guard lock(sDbMutex);
							UpdateSearchQuery(sSearchQuery, ps);
						}
					}
					else if (char ch = KeyToSearchChar(*k, shift))
					{
						if (sSearchQuery.size() < 42)
						{
							sSearchQuery.push_back(ch);
							std::lock_guard lock(sDbMutex);
							UpdateSearchQuery(sSearchQuery, ps);
						}
					}
				}
				else if (!modalActive)
				{
					if (*k == KeyChar('H') || (ctrl && *k == KeyChar('M')))
					{
						// Master toggle for interactive map (completely disable/enable rendering of markers & panel)
						UpdateSettings([](Settings& stg) { stg.mapEnabled = !stg.mapEnabled; });
						break;
					}
					else if (s.mapEnabled && (*k == Key::Tab || (ctrl && *k == KeyChar('F'))))
					{
						// search: only while the Interactive Map is on. Turning it on stays on H / the PC menu, so a Tab
						// bound to the game (e.g. open / close the map) never brings it back.
						if (!s.mapOverlay || !s.mapPanelOpen)
							UpdateSettings([](Settings& stg) { stg.mapOverlay = true; stg.mapPanelOpen = true; });
						sSearchFocused = true;
						Input::SetEmbeddedFocus(true);
						break;
					}
				}
			}
		}

		if (!s.mapEnabled)
		{
			if (sSearchFocused)
			{
				sSearchFocused = false;
				Input::SetEmbeddedFocus(false);
			}
			Input::SetPanelHover(false);
			return false;
		}

		std::lock_guard lock(sDbMutex);
		if (sDbReady.load())
			RefreshLiveFlags(frame);

		// ---- 1. Draw Map Markers on the Sheikah Slate Map ----
		int hoveredMarkerIdx = -1;
		float hoveredDistSq = 1e9f;
		std::vector<int> stackedMarkers;
		int drawnCount = 0;

		// Compute panel rect up front so markers underneath the UI panel don't draw or trigger hover tooltips
		const float panelX = X + W * 0.016f;
		const float panelY = Y + H * 0.024f;
		const float panelW = std::clamp(W * 0.235f, fs * 16.5f, fs * 22.0f);
		const float headerH = fs * 2.15f;
		ImRect panelRect(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + headerH));

		const bool hasSearch = !sSearchTokens.empty();
		if (s.mapOverlay && s.mapPanelOpen)
		{
			const int maxSearchRows = hasSearch ? std::min<int>(5, (int)sSearchMatches.size()) : 0;
			const float searchBoxH = fs * 1.75f;
			const float searchListH = hasSearch ? (maxSearchRows > 0 ? maxSearchRows * (fs * 1.45f) + fs * 0.4f : fs * 1.5f) : 0.0f;
			const float modeRowH = fs * 1.65f;
			const float catRowH = fs * 1.48f;
			const float footerH = fs * 1.95f;
			const float totalH = headerH + fs * 0.5f + searchBoxH + searchListH + modeRowH + (int)Cat_Count * catRowH + footerH + fs * 0.6f;
			panelRect.Max.y = panelY + totalH;
		}

		const bool panelHovered = !modalActive && panelRect.Contains(io.MousePos);
		Input::SetPanelHover(panelHovered);

		if (mouseClicked && !panelHovered && sSearchFocused)
		{
			sSearchFocused = false;
			Input::SetEmbeddedFocus(false);
		}

		const int wheel = panelHovered ? Input::PopOverlayWheel() : 0;

		const float zoomFactor = std::clamp(std::sqrt(std::max(0.05f, ps.mapScaleX) / 0.10f), 0.75f, 2.4f);
		const float baseRadius = std::clamp(H * 0.0082f * zoomFactor, 5.0f, 16.0f);

		const float clipMinX = X + W * 0.045f;
		const float clipMaxX = X + W * 0.950f;
		const float clipMinY = Y + H * 0.096f;
		// Clip to Sheikah Slate map viewport:
		// When Teleport window (AppSystemWindow_00: Travel / Cancel) is active, clip strictly above it at 0.610
		// When Stamp Box or Sensor Box drawer is active, clip strictly above it at 0.562
		// Otherwise, clip strictly above the bottom controls bar at 0.898
		const float clipMaxY = st.dialog ? (Y + H * 0.610f) : (ps.hasBoxCursor ? (Y + H * 0.562f) : (Y + H * 0.898f));

		if (s.mapOverlay && sDbReady.load() && std::fabs(ps.mapScaleX) > 1e-4f && std::fabs(ps.mapScaleY) > 1e-4f)
		{
			bgDl->PushClipRect(ImVec2(clipMinX, clipMinY), ImVec2(clipMaxX, clipMaxY), true);

			for (int i = 0; i < (int)sMarkers.size(); i++)
			{
				const Marker& m = sMarkers[i];
				if ((m.flags & Flag_HardMode) && !s.mapMasterMode)
					continue;

				const uint32 catMask = 1u << m.catBit;
				if (!hasSearch && !(s.mapCategories & catMask))
					continue;

				if (m.claimed && s.mapAutoHide)
					continue;

				if (hasSearch)
				{
					bool match = true;
					for (const std::string& tok : sSearchTokens)
					{
						if (m.searchKey.find(tok) == std::string::npos)
						{
							match = false;
							break;
						}
					}
					if (!match)
						continue;
				}

				// World (wx, wz) -> normalized screen coordinates (fx, fy)
				// Verified 1:1 with U-King v1.5.0 AppMap_00 N_MapT_00: 1 world metre = 2.0 layout map units, +Z is South (+fy)
				const float fx = ps.mapX + (2.0f * m.wx) * ps.mapScaleX / 1280.0f;
				const float fy = ps.mapY + (2.0f * m.wz) * ps.mapScaleY / 720.0f;

				// Strictly constrain to Sheikah Slate map canvas (never draw over HUD elements)
				if (fx < 0.045f || fx > 0.950f || fy < 0.096f || fy > 0.898f)
					continue;
				if (st.dialog && fy > 0.610f)
					continue;
				if (ps.hasBoxCursor && fy > 0.562f)
					continue;

				const ImVec2 pos(X + fx * W, Y + fy * H);
				float r = baseRadius;
				if (m.catBit == 0 || m.catBit == 1 || m.catBit == 4)
					r *= 1.15f; // Shrines, Towers, Bosses slightly larger
				else if (m.catBit == 2 || m.catBit == 9 || m.catBit == 10)
					r *= 0.85f; // Koroks, Materials, Enemies slightly more compact

				// 1. Never draw underneath our interactive search & filter panel
				const float pad = r + 3.0f;
				const ImRect panelExclude(ImVec2(panelRect.Min.x - pad, panelRect.Min.y - pad), ImVec2(panelRect.Max.x + pad, panelRect.Max.y + pad));
				if (panelExclude.Contains(pos))
					continue;

				// 2. Never draw over the bottom-left HUD (Clock, Region Name, Temperature, Completion %)
				if (fx < 0.170f && fy > 0.740f)
					continue;

				// 3. Never draw over the bottom-right Sheikah Sensor HUD
				if (fx > 0.740f && fy > 0.800f)
					continue;

				// 4. Never draw over the ornate Sheikah Slate stone corner coils
				if ((fx > 0.870f && fy < 0.240f) || (fx > 0.915f && fy < 0.360f))
					continue; // top-right
				if (fx > 0.890f && fy > 0.750f)
					continue; // bottom-right
				if ((fx < 0.120f && fy < 0.240f) || (fx < 0.080f && fy < 0.360f))
					continue; // top-left



				float alpha = modalActive ? 0.35f : 0.95f;
				if (m.claimed && s.mapDimClaimed)
					alpha *= 0.35f;

				const bool discInc = (m.catBit == 0 && m.discovered && !m.claimed);
				DrawMarkerIcon(bgDl, pos, r, m.catBit, m.subKind, discInc, alpha);
				drawnCount++;

				if (i == sHighlightMarker && now < sHighlightUntil)
				{
					const float pulse = 0.5f + 0.5f * std::sin((float)now * 8.0f);
					const float ringR = r * (1.8f + 0.7f * pulse);
					bgDl->AddCircle(pos, ringR, IM_COL32(255, 215, 45, (int)(240 * (1.0f - 0.3f * pulse))), 24, 2.5f);
					bgDl->AddCircle(pos, r * 1.35f, IM_COL32(70, 235, 255, 220), 24, 1.8f);
				}

				if (!modalActive && pos.y >= clipMinY && pos.y <= clipMaxY && pos.x >= clipMinX && pos.x <= clipMaxX)
				{
					const float dx = io.MousePos.x - pos.x, dy = io.MousePos.y - pos.y;
					const float d2 = dx * dx + dy * dy;
					const float hitR = std::max(r * 1.45f, 11.0f);
					if (d2 <= hitR * hitR)
					{
						stackedMarkers.push_back(i);
						if (d2 < hoveredDistSq)
						{
							hoveredDistSq = d2;
							hoveredMarkerIdx = i;
						}
					}
				}
			}

			bgDl->PopClipRect();
		}

		// ---- 2. Draw Interactive Filter & Search Panel on the Map ----
		const ImU32 colPanel   = IM_COL32(8, 12, 18, 244);
		const ImU32 colBorder  = IM_COL32(195, 215, 225, 135);
		const ImU32 colCyan    = IM_COL32(65, 220, 255, 255);
		const ImU32 colGold    = IM_COL32(248, 200, 0, 255);
		const ImU32 colText    = IM_COL32(238, 234, 214, 255);
		const ImU32 colTextDim = IM_COL32(158, 164, 168, 255);

		// Panel background & Sheikah frame
		fgDl->AddRectFilled(panelRect.Min, panelRect.Max, colPanel, 6.0f);
		fgDl->AddRect(panelRect.Min, panelRect.Max, colBorder, 6.0f, 0, 1.5f);

		// Header bar
		{
			const ImRect hdrRect(panelRect.Min, ImVec2(panelRect.Max.x, panelY + headerH));
			DrawDiamond(fgDl, ImVec2(panelX + fs * 1.0f, panelY + headerH * 0.5f), fs * 0.44f, fs * 0.44f, colCyan, colGold, 1.2f);
			fgDl->AddText(fontTitle, fs * 1.00f, ImVec2(panelX + fs * 1.7f, panelY + (headerH - fs * 1.00f) * 0.5f), colText, "Interactive Map");

			// Toggle Overlay button [On/Off]
			const char* ovLabel = s.mapOverlay ? "ON" : "OFF";
			const float btnW = fs * 2.3f, btnH = fs * 1.25f;
			const ImRect ovBtn(ImVec2(panelRect.Max.x - fs * 6.8f, panelY + (headerH - btnH) * 0.5f),
							   ImVec2(panelRect.Max.x - fs * 6.8f + btnW, panelY + (headerH + btnH) * 0.5f));
			const bool ovHov = !modalActive && ovBtn.Contains(io.MousePos);
			fgDl->AddRectFilled(ovBtn.Min, ovBtn.Max, s.mapOverlay ? IM_COL32(55, 215, 255, ovHov ? 75 : 45) : IM_COL32(255, 255, 255, ovHov ? 35 : 15), 4.0f);
			fgDl->AddRect(ovBtn.Min, ovBtn.Max, s.mapOverlay ? colCyan : colBorder, 4.0f);
			const ImVec2 ovTs = fontSmall->CalcTextSizeA(fs * 0.70f, FLT_MAX, 0, ovLabel);
			fgDl->AddText(fontSmall, fs * 0.70f, ImVec2(ovBtn.GetCenter().x - ovTs.x * 0.5f, ovBtn.GetCenter().y - ovTs.y * 0.5f),
						  s.mapOverlay ? colCyan : colTextDim, ovLabel);
			if (ovHov && mouseClicked)
				UpdateSettings([](Settings& stg) { stg.mapOverlay = !stg.mapOverlay; });

			// Master Hide button [HIDE (H)] - completely disables interactive map rendering (restore with H key)
			const float hideW = fs * 2.5f;
			const ImRect hideBtn(ImVec2(panelRect.Max.x - fs * 4.2f, panelY + (headerH - btnH) * 0.5f),
								 ImVec2(panelRect.Max.x - fs * 4.2f + hideW, panelY + (headerH + btnH) * 0.5f));
			const bool hideHov = !modalActive && hideBtn.Contains(io.MousePos);
			fgDl->AddRectFilled(hideBtn.Min, hideBtn.Max, hideHov ? IM_COL32(255, 80, 80, 55) : IM_COL32(255, 255, 255, 15), 4.0f);
			fgDl->AddRect(hideBtn.Min, hideBtn.Max, hideHov ? IM_COL32(255, 95, 95, 220) : colBorder, 4.0f);
			const ImVec2 hdTs = fontSmall->CalcTextSizeA(fs * 0.68f, FLT_MAX, 0, "HIDE");
			fgDl->AddText(fontSmall, fs * 0.68f, ImVec2(hideBtn.GetCenter().x - hdTs.x * 0.5f, hideBtn.GetCenter().y - hdTs.y * 0.5f),
						  hideHov ? IM_COL32(255, 110, 110, 255) : colTextDim, "HIDE");
			if (hideHov && mouseClicked)
				UpdateSettings([](Settings& stg) { stg.mapEnabled = false; });

			// Expand / Collapse chevron button (vector triangle)
			const ImRect expBtn(ImVec2(panelRect.Max.x - fs * 1.5f, panelY + (headerH - btnH) * 0.5f),
								ImVec2(panelRect.Max.x - fs * 0.4f, panelY + (headerH + btnH) * 0.5f));
			const bool expHov = !modalActive && expBtn.Contains(io.MousePos);
			fgDl->AddRectFilled(expBtn.Min, expBtn.Max, expHov ? IM_COL32(248, 200, 0, 45) : IM_COL32(255, 255, 255, 15), 4.0f);
			const ImVec2 cc = expBtn.GetCenter();
			const float cw = fs * 0.28f, ch = fs * 0.20f;
			const ImU32 chevCol = expHov ? colGold : colText;
			if (s.mapOverlay && s.mapPanelOpen)
				fgDl->AddTriangleFilled(ImVec2(cc.x, cc.y - ch), ImVec2(cc.x + cw, cc.y + ch), ImVec2(cc.x - cw, cc.y + ch), chevCol);
			else
				fgDl->AddTriangleFilled(ImVec2(cc.x - cw, cc.y - ch), ImVec2(cc.x + cw, cc.y - ch), ImVec2(cc.x, cc.y + ch), chevCol);
			if (expHov && mouseClicked)
			{
				UpdateSettings([](Settings& stg) {
					if (!stg.mapOverlay)
					{
						stg.mapOverlay = true;
						stg.mapPanelOpen = true;
					}
					else
						stg.mapPanelOpen = !stg.mapPanelOpen;
				});
			}
		}

		// Expanded panel body
		if (s.mapOverlay && s.mapPanelOpen)
		{
			float curY = panelY + headerH;
			fgDl->AddLine(ImVec2(panelX + fs * 0.6f, curY), ImVec2(panelRect.Max.x - fs * 0.6f, curY), IM_COL32(195, 215, 225, 65), 1.0f);
			curY += fs * 0.35f;

			// 1. Search Input Box
			const float padX = fs * 0.65f;
			const float searchH = fs * 1.55f;
			const ImRect searchRect(ImVec2(panelX + padX, curY), ImVec2(panelRect.Max.x - padX, curY + searchH));
			const bool searchHov = !modalActive && searchRect.Contains(io.MousePos);

			// Clear [X] button inside search box
			const ImRect clearBtn(ImVec2(searchRect.Max.x - fs * 1.55f, searchRect.Min.y), searchRect.Max);
			const bool clearHov = hasSearch && !modalActive && clearBtn.Contains(io.MousePos);

			if (mouseClicked && searchHov)
			{
				if (clearHov)
				{
					sSearchQuery.clear();
					UpdateSearchQuery("", ps);
				}
				else
				{
					sSearchFocused = true;
					Input::SetEmbeddedFocus(true);
				}
			}

			fgDl->AddRectFilled(searchRect.Min, searchRect.Max, sSearchFocused ? IM_COL32(22, 34, 48, 250) : IM_COL32(16, 22, 30, 220), 4.0f);
			fgDl->AddRect(searchRect.Min, searchRect.Max, sSearchFocused ? colGold : (searchHov ? colCyan : colBorder), 4.0f, 0, sSearchFocused ? 1.6f : 1.0f);

			const std::string displaySearch = sSearchQuery.empty() && !sSearchFocused
												  ? "Search items, chests, shrines... (Tab)"
												  : (sSearchQuery + (sSearchFocused && ((int)(now * 2.0) & 1) ? "|" : ""));
			fgDl->PushClipRect(searchRect.Min, ImVec2(hasSearch ? clearBtn.Min.x : searchRect.Max.x - 4.0f, searchRect.Max.y), true);
			fgDl->AddText(fontSmall, fs * 0.78f, ImVec2(searchRect.Min.x + fs * 0.45f, searchRect.Min.y + (searchH - fs * 0.78f) * 0.5f),
						  (sSearchQuery.empty() && !sSearchFocused) ? colTextDim : colText, displaySearch.c_str());
			fgDl->PopClipRect();

			if (hasSearch)
			{
				fgDl->AddText(fontSmall, fs * 0.78f, ImVec2(clearBtn.Min.x + fs * 0.45f, clearBtn.Min.y + (searchH - fs * 0.78f) * 0.5f),
							  clearHov ? colGold : colTextDim, "X");
			}
			curY += searchH + fs * 0.25f;

			// 2. Interactive Search Results List (click to center map on marker!)
			if (hasSearch)
			{
				if (sSearchMatches.empty())
				{
					fgDl->AddText(fontSmall, fs * 0.74f, ImVec2(panelX + padX + fs * 0.2f, curY + fs * 0.2f), colTextDim, "No matching markers found.");
					curY += fs * 1.45f;
				}
				else
				{
					const int totalMatches = (int)sSearchMatches.size();
					const int showRows = std::min(5, totalMatches);
					if (wheel != 0)
						sSearchScroll = std::clamp(sSearchScroll - wheel, 0, std::max(0, totalMatches - showRows));

					const float rowH = fs * 1.42f;
					for (int r = 0; r < showRows; r++)
					{
						const int mIdx = sSearchMatches[sSearchScroll + r];
						const Marker& m = sMarkers[mIdx];
						const ImRect rr(ImVec2(panelX + padX, curY), ImVec2(panelRect.Max.x - padX, curY + rowH - 2.0f));
						const bool rHov = !modalActive && rr.Contains(io.MousePos);
						fgDl->AddRectFilled(rr.Min, rr.Max, rHov ? IM_COL32(248, 200, 0, 42) : IM_COL32(255, 255, 255, 10), 3.0f);
						if (rHov)
							fgDl->AddRect(rr.Min, rr.Max, colGold, 3.0f);

						DrawMarkerIcon(fgDl, ImVec2(rr.Min.x + fs * 0.65f, rr.GetCenter().y), fs * 0.42f, m.catBit, m.subKind, false, 1.0f);
						const std::string line = m.sub.empty() || m.sub == kCatMeta[m.catBit].name ? m.name : fmt::format("{} - {}", m.name, m.sub);
						fgDl->PushClipRect(ImVec2(rr.Min.x + fs * 1.3f, rr.Min.y), ImVec2(rr.Max.x - fs * 0.3f, rr.Max.y), true);
						fgDl->AddText(fontSmall, fs * 0.74f, ImVec2(rr.Min.x + fs * 1.35f, rr.Min.y + (rowH - fs * 0.74f) * 0.5f),
									  rHov ? colGold : colText, line.c_str());
						fgDl->PopClipRect();

						if (rHov && mouseClicked)
						{
							UI::SetMapWorldCenter(ps, m.wx, m.wz);
							sHighlightMarker = mIdx;
							sHighlightUntil = now + 4.0;
						}
						curY += rowH;
					}
					curY += fs * 0.15f;
				}
			}

			// 3. Auto-Hide Claimed & Dim Claimed toggles
			{
				const float rowH = fs * 1.45f;
				const float halfW = (panelW - padX * 2.0f - fs * 0.3f) * 0.58f;
				const ImRect hideBtn(ImVec2(panelX + padX, curY), ImVec2(panelX + padX + halfW, curY + rowH));
				const ImRect dimBtn(ImVec2(hideBtn.Max.x + fs * 0.3f, curY), ImVec2(panelRect.Max.x - padX, curY + rowH));
				const bool hideHov = !modalActive && hideBtn.Contains(io.MousePos);
				const bool dimHov  = !modalActive && dimBtn.Contains(io.MousePos);

				fgDl->AddRectFilled(hideBtn.Min, hideBtn.Max, s.mapAutoHide ? IM_COL32(65, 220, 255, hideHov ? 65 : 40) : IM_COL32(255, 255, 255, hideHov ? 28 : 12), 4.0f);
				fgDl->AddRect(hideBtn.Min, hideBtn.Max, s.mapAutoHide ? colCyan : colBorder, 4.0f);
				fgDl->AddText(fontSmall, fs * 0.72f, ImVec2(hideBtn.Min.x + fs * 0.4f, hideBtn.Min.y + (rowH - fs * 0.72f) * 0.5f),
							  s.mapAutoHide ? colCyan : colTextDim, s.mapAutoHide ? "[x] Auto-Hide Claimed" : "[ ] Auto-Hide Claimed");
				if (hideHov && mouseClicked)
					UpdateSettings([](Settings& stg) { stg.mapAutoHide = !stg.mapAutoHide; });

				const bool dimActive = !s.mapAutoHide && s.mapDimClaimed;
				fgDl->AddRectFilled(dimBtn.Min, dimBtn.Max, dimActive ? IM_COL32(248, 200, 0, dimHov ? 60 : 35) : IM_COL32(255, 255, 255, dimHov ? 28 : 12), 4.0f);
				fgDl->AddRect(dimBtn.Min, dimBtn.Max, dimActive ? colGold : colBorder, 4.0f);
				fgDl->AddText(fontSmall, fs * 0.72f, ImVec2(dimBtn.Min.x + fs * 0.35f, dimBtn.Min.y + (rowH - fs * 0.72f) * 0.5f),
							  dimActive ? colGold : colTextDim, "Dim Claimed");
				if (dimHov && mouseClicked)
					UpdateSettings([](Settings& stg) { stg.mapAutoHide = false; stg.mapDimClaimed = !stg.mapDimClaimed; });

				curY += rowH + fs * 0.22f;
			}

			// 4. Category Toggle Rows (with vector icon + live claimed/total counter)
			const float catRowH = fs * 1.46f;
			for (uint8 c = 0; c < Cat_Count; c++)
			{
				const uint32 mask = 1u << c;
				const bool enabled = (s.mapCategories & mask) != 0;
				const ImRect cr(ImVec2(panelX + padX, curY), ImVec2(panelRect.Max.x - padX, curY + catRowH - 2.0f));
				const bool cHov = !modalActive && cr.Contains(io.MousePos);

				fgDl->AddRectFilled(cr.Min, cr.Max, cHov ? IM_COL32(248, 200, 0, 35) : (enabled ? IM_COL32(55, 210, 255, 20) : IM_COL32(255, 255, 255, 6)), 3.5f);
				if (cHov || enabled)
					fgDl->AddRect(cr.Min, cr.Max, cHov ? colGold : IM_COL32(65, 220, 255, 85), 3.5f);

				DrawMarkerIcon(fgDl, ImVec2(cr.Min.x + fs * 0.72f, cr.GetCenter().y), fs * 0.44f, c, 0, false, enabled ? 1.0f : 0.40f);
				fgDl->AddText(fontItem, fs * 0.80f, ImVec2(cr.Min.x + fs * 1.50f, cr.Min.y + (catRowH - fs * 0.80f) * 0.5f),
							  enabled ? colText : colTextDim, kCatMeta[c].name);

				if (sDbReady.load())
				{
					const std::string cntStr = kCatMeta[c].hasClaimFlags
												   ? fmt::format("{}/{}", sCatClaimed[c], sCatTotal[c])
												   : fmt::format("{}", sCatTotal[c]);
					const ImVec2 cts = fontSmall->CalcTextSizeA(fs * 0.72f, FLT_MAX, 0, cntStr.c_str());
					fgDl->AddText(fontSmall, fs * 0.72f, ImVec2(cr.Max.x - cts.x - fs * 0.45f, cr.Min.y + (catRowH - cts.y) * 0.5f),
								  enabled ? colCyan : colTextDim, cntStr.c_str());
				}

				if (cHov && mouseClicked)
				{
					UpdateSettings([mask](Settings& stg) { stg.mapCategories ^= mask; });
				}
				curY += catRowH;
			}

			// 5. Footer quick buttons: [Default] [All] [None] [Master Mode]
			curY += fs * 0.20f;
			const float fBtnH = fs * 1.45f;
			struct QuickBtn { const char* label; int id; bool active; };
			const QuickBtn qbtns[] = {
				{"Default", 0, s.mapCategories == Cat_Default},
				{"All",     1, s.mapCategories == Cat_All},
				{"None",    2, s.mapCategories == 0},
				{"Master",  3, s.mapMasterMode},
			};
			const float qGap = fs * 0.25f;
			const float qBtnW = (panelW - padX * 2.0f - qGap * 3.0f) / 4.0f;
			for (int qi = 0; qi < 4; qi++)
			{
				const float qx = panelX + padX + qi * (qBtnW + qGap);
				const ImRect qr(ImVec2(qx, curY), ImVec2(qx + qBtnW, curY + fBtnH));
				const bool qHov = !modalActive && qr.Contains(io.MousePos);
				fgDl->AddRectFilled(qr.Min, qr.Max, qbtns[qi].active ? IM_COL32(248, 200, 0, 45) : (qHov ? IM_COL32(255, 255, 255, 30) : IM_COL32(255, 255, 255, 12)), 3.5f);
				fgDl->AddRect(qr.Min, qr.Max, qHov || qbtns[qi].active ? colGold : colBorder, 3.5f);
				const ImVec2 qts = fontSmall->CalcTextSizeA(fs * 0.70f, FLT_MAX, 0, qbtns[qi].label);
				fgDl->AddText(fontSmall, fs * 0.70f, ImVec2(qr.GetCenter().x - qts.x * 0.5f, qr.GetCenter().y - qts.y * 0.5f),
							  qbtns[qi].active ? colGold : colText, qbtns[qi].label);
				if (qHov && mouseClicked)
				{
					const int id = qbtns[qi].id;
					UpdateSettings([id](Settings& stg) {
						if (id == 0) stg.mapCategories = Cat_Default;
						else if (id == 1) stg.mapCategories = Cat_All;
						else if (id == 2) stg.mapCategories = 0;
						else if (id == 3) stg.mapMasterMode = !stg.mapMasterMode;
					});
					if (id == 3)
					{
						sLastFlagRefreshFrame = 0;
						if (hasSearch)
							UpdateSearchQuery(sSearchQuery, ps);
					}
				}
			}
		}

		// Loading indicator toast on first-ever database build
		if (sDbLoading.load())
		{
			const char* loadMsg = "Building Interactive Map Database...";
			const ImVec2 ts = fontItem->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0, loadMsg);
			const ImVec2 p0(X + (W - ts.x) * 0.5f - fs, Y + H * 0.06f);
			const ImVec2 p1(p0.x + ts.x + fs * 2.0f, p0.y + ts.y + fs * 0.8f);
			fgDl->AddRectFilled(p0, p1, colPanel, 6.0f);
			fgDl->AddRect(p0, p1, colCyan, 6.0f, 0, 1.5f);
			fgDl->AddText(fontItem, fs * 0.9f, ImVec2(p0.x + fs, p0.y + fs * 0.4f), colCyan, loadMsg);
		}

		// ---- 3. Hover Tooltip for Map Markers ----
		if (!panelHovered && !modalActive && hoveredMarkerIdx >= 0 && hoveredMarkerIdx < (int)sMarkers.size())
		{
			const Marker& hm = sMarkers[hoveredMarkerIdx];
			const float fx = ps.mapX + (2.0f * hm.wx) * ps.mapScaleX / 1280.0f;
			const float fy = ps.mapY + (2.0f * hm.wz) * ps.mapScaleY / 720.0f;
			const ImVec2 mpos(X + fx * W, Y + fy * H);

			// Highlight ring around hovered marker
			fgDl->AddCircle(mpos, baseRadius * 1.65f, colGold, 24, 2.2f);

			std::string statusTag;
			ImU32 statusCol = colCyan;
			if (kCatMeta[hm.catBit].hasClaimFlags)
			{
				if (hm.claimed)
				{
					statusTag = "Claimed / Completed";
					statusCol = IM_COL32(90, 235, 115, 255);
				}
				else if (hm.catBit == 0 && hm.discovered)
				{
					statusTag = "Discovered (Incomplete)";
					statusCol = IM_COL32(255, 155, 45, 255);
				}
				else
				{
					statusTag = "Unclaimed";
					statusCol = colCyan;
				}
			}
			if (hm.flags & Flag_InGround)
				statusTag += (statusTag.empty() ? "" : "  |  ") + std::string("Buried Underground");
			if (hm.flags & Flag_HardMode)
				statusTag += (statusTag.empty() ? "" : "  |  ") + std::string("Master Mode");

			const std::string coordLine = fmt::format("X: {:.0f}, Z: {:.0f}{}", hm.wx, hm.wz,
													  stackedMarkers.size() > 1 ? fmt::format("  (+{} nearby)", stackedMarkers.size() - 1) : "");

			const ImVec2 nameSz = fontItem->CalcTextSizeA(fs * 0.92f, FLT_MAX, 0, hm.name.c_str());
			const ImVec2 subSz  = fontSmall->CalcTextSizeA(fs * 0.80f, FLT_MAX, 0, hm.sub.c_str());
			const ImVec2 stSz   = statusTag.empty() ? ImVec2(0, 0) : fontSmall->CalcTextSizeA(fs * 0.74f, FLT_MAX, 0, statusTag.c_str());
			const ImVec2 crdSz  = fontSmall->CalcTextSizeA(fs * 0.70f, FLT_MAX, 0, coordLine.c_str());

			const float pad = fs * 0.65f;
			const float boxW = std::max({nameSz.x + fs * 1.6f, subSz.x, stSz.x, crdSz.x}) + pad * 2.0f;
			const float boxH = pad * 1.8f + nameSz.y + subSz.y + (statusTag.empty() ? 0.0f : stSz.y + fs * 0.15f) + crdSz.y + fs * 0.45f;

			float tx = mpos.x + baseRadius * 1.8f;
			float ty = mpos.y - boxH * 0.5f;
			if (tx + boxW > X + W - fs)
				tx = mpos.x - baseRadius * 1.8f - boxW;
			ty = std::clamp(ty, Y + fs, Y + H - boxH - fs);

			const ImVec2 t0(tx, ty), t1(tx + boxW, ty + boxH);
			fgDl->AddRectFilled(t0, t1, IM_COL32(8, 12, 18, 242), 5.0f);
			fgDl->AddRect(t0, t1, colGold, 5.0f, 0, 1.5f);

			float cy = ty + pad * 0.85f;
			DrawMarkerIcon(fgDl, ImVec2(tx + pad + fs * 0.48f, cy + nameSz.y * 0.5f), fs * 0.48f, hm.catBit, hm.subKind, false, 1.0f);
			fgDl->AddText(fontItem, fs * 0.92f, ImVec2(tx + pad + fs * 1.25f, cy), colText, hm.name.c_str());
			cy += nameSz.y + fs * 0.18f;

			fgDl->AddText(fontSmall, fs * 0.80f, ImVec2(tx + pad, cy), colGold, hm.sub.c_str());
			cy += subSz.y + fs * 0.15f;

			if (!statusTag.empty())
			{
				fgDl->AddText(fontSmall, fs * 0.74f, ImVec2(tx + pad, cy), statusCol, statusTag.c_str());
				cy += stSz.y + fs * 0.15f;
			}
			fgDl->AddText(fontSmall, fs * 0.70f, ImVec2(tx + pad, cy), colTextDim, coordLine.c_str());
		}

		return true;
	}

	void RenderMinimap(double now, float dt)
	{
		(void)now;
		(void)dt;
		const Settings s = GetSettings();
		if (!s.mapEnabled || !s.mapMinimap)
			return;

		EnsureDatabaseLoaded();
		if (!sDbReady.load())
			return;

		const uint32 frame = Input::GetDebug().frameCounter.load();
		const auto& ui = UI::Get(frame);
		if (!ui.hud || ui.anyMenu || ui.inventory || ui.map || ui.dialog || ui.title || ui.scope || ui.cameraRune)
			return;

		// the minimap and Link's position of the very frame shown (read now, they would be a frame or two ahead of the
		// picture: the icons trailed the minimap while turning)
		UI::MinimapFrame mf;
		if (!UI::GetMinimapFrame(mf))
		{
			mf.info = UI::GetMinimapInfo();
			mf.hasPose = Input::GetPlayerPose(mf.px, mf.py, mf.pz, mf.camYaw);
		}
		const UI::MinimapInfo& minfo = mf.info;
		if (!mf.hasPose && !minfo.hasMapTransform)
			return;
		// the point the game's map is centred on (its own, so the icons move with the map exactly), else Link's
		const float px = minfo.hasMapTransform ? minfo.mapCenterX : mf.px;
		const float pz = minfo.hasMapTransform ? minfo.mapCenterZ : mf.pz;
		const float camYaw = mf.camYaw;
		const float compassAngle = minfo.hasNorthRotation ? minfo.compassAngle : camYaw;

		ImGuiIO& io = ImGui::GetIO();
		const float W = io.DisplaySize.x, H = io.DisplaySize.y;
		if (W <= 0.0f || H <= 0.0f)
			return;

		float picX = 0.0f, picY = 0.0f, picW = io.DisplaySize.x, picH = io.DisplaySize.y;
		int px_area, py_area, pw_area, ph_area, ww_area, wh_area;
		if (Graphics::GetPictureArea(px_area, py_area, pw_area, ph_area, ww_area, wh_area))
		{
			const float kx = io.DisplaySize.x / (float)ww_area, ky = io.DisplaySize.y / (float)wh_area;
			picX = px_area * kx;
			picY = py_area * ky;
			picW = pw_area * kx;
			picH = ph_area * ky;
		}

		if (!minfo.visible || minfo.normRadius <= 0.01f || minfo.alpha <= 0.05f)
			return;

		const float centerX = picX + minfo.normCenterX * picW;
		const float centerY = picY + minfo.normCenterY * picH;
		const float radiusPix = minfo.normRadius * picH;

		if (radiusPix < 10.0f)
			return;

		RefreshLiveFlags(frame);

		// the game's scale (1.8 layout pixels per meter on foot: the minimap shows about 44 m around Link); without it the
		// old estimate
		const float radarPixelRadius = radiusPix * 0.90f;
		const float meterToPixel = minfo.hasMapTransform ? minfo.mapLayoutPerMeter * (picH / 720.0f) : radarPixelRadius / 120.0f;
		const float kMinimapRangeMeters = radarPixelRadius / meterToPixel;
		const float iconRadius = std::clamp(5.5f * (picH / 720.0f), 4.0f, 9.0f);

		const float cosA = cosf(compassAngle);
		const float sinA = sinf(compassAngle);

		ImDrawList* fgDl = ImGui::GetForegroundDrawList();
		fgDl->PushClipRect(ImVec2(centerX - radiusPix * 1.05f, centerY - radiusPix * 1.05f),
						   ImVec2(centerX + radiusPix * 1.05f, centerY + radiusPix * 1.05f), true);

		std::lock_guard lock(sDbMutex);
		for (const Marker& m : sMarkers)
		{
			if ((m.flags & Flag_HardMode) && !s.mapMasterMode)
				continue;

			const uint32 catMask = 1u << m.catBit;
			if (!(s.mapCategories & catMask))
				continue;

			if (s.mapAutoHide && m.claimed)
				continue;

			const float relX = m.wx - px;
			const float relZ = m.wz - pz;
			if (fabsf(relX) > kMinimapRangeMeters || fabsf(relZ) > kMinimapRangeMeters)
				continue;

			const float rotX =  relX * cosA + relZ * sinA;
			const float rotY = -relX * sinA + relZ * cosA;
			const float distSq = rotX * rotX + rotY * rotY;
			if (distSq > kMinimapRangeMeters * kMinimapRangeMeters)
				continue;

			const float dist = sqrtf(distSq);
			const float pixelDist = dist * meterToPixel;

			// Smooth edge fading near the rim (between 78% and 100% of radar radius)
			float alpha = 1.0f;
			const float fadeStart = radarPixelRadius * 0.78f;
			if (pixelDist > fadeStart)
				alpha = std::clamp((radarPixelRadius - pixelDist) / (radarPixelRadius - fadeStart), 0.0f, 1.0f);

			alpha *= minfo.alpha;
			if (alpha <= 0.02f)
				continue;

			if (!s.mapAutoHide && s.mapDimClaimed && m.claimed)
				alpha *= 0.35f;

			const float iconX = centerX + rotX * meterToPixel;
			const float iconY = centerY + rotY * meterToPixel;

			DrawMarkerIcon(fgDl, ImVec2(iconX, iconY), iconRadius, m.catBit, m.subKind, false, alpha);
		}

		fgDl->PopClipRect();
	}

	// ---------------------------------------------------------------- dev bridge command
	std::string DebugCommand(const std::vector<std::string>& a)
	{
		EnsureDatabaseLoaded();
		std::lock_guard lock(sDbMutex);
		if (a.size() >= 2 && a[1] == "status")
		{
			RefreshLiveFlags(Input::GetDebug().frameCounter.load());
			const Settings s = GetSettings();
			std::ostringstream o;
			o << fmt::format("ready={} total={} enabled={} overlay={} panel={} autohide={} dim={} master={} cats={:#x} search='{}' matches={}",
							 sDbReady.load(), sMarkers.size(), s.mapEnabled, s.mapOverlay, s.mapPanelOpen, s.mapAutoHide, s.mapDimClaimed,
							 s.mapMasterMode, s.mapCategories, sSearchQuery, sSearchMatches.size());
			for (int c = 0; c < Cat_Count; c++)
				o << fmt::format(" | {}: {}/{}", kCatMeta[c].name, sCatClaimed[c], sCatTotal[c]);
			return o.str();
		}
		if (a.size() >= 2 && a[1] == "search")
		{
			std::string q;
			for (size_t i = 2; i < a.size(); i++)
				q += (i > 2 ? " " : "") + a[i];
			if (q == "off" || q == "clear" || q == "none")
				q.clear();
			const UI::PointerSnapshot& ps = UI::GetPointer(Input::GetDebug().frameCounter.load());
			UpdateSearchQuery(q, ps);
			std::ostringstream o;
			o << fmt::format("matches={}", sSearchMatches.size());
			for (size_t i = 0; i < std::min<size_t>(10, sSearchMatches.size()); i++)
			{
				const Marker& m = sMarkers[sSearchMatches[i]];
				o << fmt::format(" | [{}] {} ({}) @ ({:.0f},{:.0f}) claimed={}", kCatMeta[m.catBit].name, m.name, m.sub, m.wx, m.wz, m.claimed);
			}
			return o.str();
		}
		if (a.size() >= 3 && a[1] == "center")
		{
			const int idx = std::stoi(a[2]);
			if (idx >= 0 && idx < (int)sSearchMatches.size())
			{
				const Marker& m = sMarkers[sSearchMatches[idx]];
				const UI::PointerSnapshot& ps = UI::GetPointer(Input::GetDebug().frameCounter.load());
				return UI::SetMapWorldCenter(ps, m.wx, m.wz) ? "ok" : "err map not open";
			}
			return "err invalid match index";
		}
		if (a.size() >= 4 && a[1] == "goto")
		{
			const float wx = std::stof(a[2]), wz = std::stof(a[3]);
			const UI::PointerSnapshot& ps = UI::GetPointer(Input::GetDebug().frameCounter.load());
			return UI::SetMapWorldCenter(ps, wx, wz) ? "ok" : "err map not open";
		}
		if (a.size() >= 3 && (a[1] == "master" || a[1] == "enable"))
		{
			const bool v = std::stoul(a[2], nullptr, 0) != 0;
			UpdateSettings([v](Settings& s) { s.mapEnabled = v; });
			return "ok";
		}
		if (a.size() >= 3 && a[1] == "overlay")
		{
			const bool v = std::stoul(a[2], nullptr, 0) != 0;
			UpdateSettings([v](Settings& s) { s.mapOverlay = v; });
			return "ok";
		}
		if (a.size() >= 3 && a[1] == "panel")
		{
			const bool v = a[2] == "on" || a[2] == "1";
			UpdateSettings([v](Settings& s) { s.mapPanelOpen = v; });
			return "ok";
		}
		if (a.size() >= 3 && a[1] == "cats")
		{
			const uint32 mask = (uint32)std::stoul(a[2], nullptr, 0);
			UpdateSettings([mask](Settings& s) { s.mapCategories = mask & Cat_All; });
			return "ok";
		}
		if (a.size() >= 3 && a[1] == "autohide")
		{
			const bool v = a[2] == "on" || a[2] == "1" || std::stoul(a[2], nullptr, 0) != 0;
			UpdateSettings([v](Settings& s) { s.mapAutoHide = v; });
			return "ok";
		}
		return "err mapicons status|master <0|1>|overlay <0|1>|panel <on|off>|search <q>|center <idx>|goto <wx> <wz>|cats <mask>|autohide <0|1>";
	}
}
