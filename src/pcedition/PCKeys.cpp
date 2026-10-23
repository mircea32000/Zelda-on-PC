#include "pcedition/PCKeys.h"

#if BOOST_OS_WINDOWS
#include <windows.h>
#else
#define VK_CAPITAL 0x14
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#endif
#include <boost/algorithm/string.hpp>

namespace pce
{
	namespace
	{
		struct NamedKey
		{
			Key key;
			const char* id;
			const char* display;
			const char* cap;
		};

		const NamedKey kNamedKeys[] = {
			{Key::MouseLeft, "MouseLeft", "Left Mouse", ""},
			{Key::MouseRight, "MouseRight", "Right Mouse", ""},
			{Key::MouseMiddle, "MouseMiddle", "Middle Mouse", ""},
			{Key::Mouse4, "Mouse4", "Mouse 4", ""},
			{Key::Mouse5, "Mouse5", "Mouse 5", ""},
			{Key::WheelUp, "WheelUp", "Wheel Up", ""},
			{Key::WheelDown, "WheelDown", "Wheel Down", ""},
			{Key::Backspace, "Backspace", "Backspace", "BKSP"},
			{Key::Tab, "Tab", "Tab", "TAB"},
			{Key::Enter, "Enter", "Enter", "ENTER"},
			{Key::Escape, "Escape", "Esc", "ESC"},
			{Key::Space, "Space", "Space", "SPACE"},
			{Key::PageUp, "PageUp", "Page Up", "PGUP"},
			{Key::PageDown, "PageDown", "Page Down", "PGDN"},
			{Key::End, "End", "End", "END"},
			{Key::Home, "Home", "Home", "HOME"},
			{Key::Left, "Left", "Left Arrow", "\xE2\x86\x90"},
			{Key::Up, "Up", "Up Arrow", "\xE2\x86\x91"},
			{Key::Right, "Right", "Right Arrow", "\xE2\x86\x92"},
			{Key::Down, "Down", "Down Arrow", "\xE2\x86\x93"},
			{Key::Insert, "Insert", "Insert", "INS"},
			{Key::Delete, "Delete", "Delete", "DEL"},
			{Key::LShift, "LShift", "Left Shift", "SHIFT"},
			{Key::RShift, "RShift", "Right Shift", "SHIFT"},
			{Key::LCtrl, "LCtrl", "Left Ctrl", "CTRL"},
			{Key::RCtrl, "RCtrl", "Right Ctrl", "CTRL"},
			{Key::LAlt, "LAlt", "Left Alt", "ALT"},
			{Key::RAlt, "RAlt", "Right Alt", "ALT"},
			{(Key)VK_CAPITAL, "CapsLock", "Caps Lock", "CAPS"},
			{(Key)VK_NUMPAD0, "Num0", "Num 0", "NUM0"},
			{(Key)VK_NUMPAD1, "Num1", "Num 1", "NUM1"},
			{(Key)VK_NUMPAD2, "Num2", "Num 2", "NUM2"},
			{(Key)VK_NUMPAD3, "Num3", "Num 3", "NUM3"},
			{(Key)VK_NUMPAD4, "Num4", "Num 4", "NUM4"},
			{(Key)VK_NUMPAD5, "Num5", "Num 5", "NUM5"},
			{(Key)VK_NUMPAD6, "Num6", "Num 6", "NUM6"},
			{(Key)VK_NUMPAD7, "Num7", "Num 7", "NUM7"},
			{(Key)VK_NUMPAD8, "Num8", "Num 8", "NUM8"},
			{(Key)VK_NUMPAD9, "Num9", "Num 9", "NUM9"},
			{(Key)VK_MULTIPLY, "NumMul", "Num *", "NUM*"},
			{(Key)VK_ADD, "NumAdd", "Num +", "NUM+"},
			{(Key)VK_SUBTRACT, "NumSub", "Num -", "NUM-"},
			{(Key)VK_DECIMAL, "NumDot", "Num .", "NUM."},
			{(Key)VK_DIVIDE, "NumDiv", "Num /", "NUM/"},
			{(Key)VK_OEM_1, "Semicolon", ";", ";"},
			{(Key)VK_OEM_PLUS, "Equals", "=", "="},
			{(Key)VK_OEM_COMMA, "Comma", ",", ","},
			{(Key)VK_OEM_MINUS, "Minus", "-", "-"},
			{(Key)VK_OEM_PERIOD, "Period", ".", "."},
			{(Key)VK_OEM_2, "Slash", "/", "/"},
			{(Key)VK_OEM_3, "Grave", "`", "`"},
			{(Key)VK_OEM_4, "LBracket", "[", "["},
			{(Key)VK_OEM_5, "Backslash", "\\", "\\"},
			{(Key)VK_OEM_6, "RBracket", "]", "]"},
			{(Key)VK_OEM_7, "Quote", "'", "'"},
		};

		const NamedKey* FindNamed(Key k)
		{
			for (auto& nk : kNamedKeys)
				if (nk.key == k)
					return &nk;
			return nullptr;
		}
	}

	std::string KeyToId(Key k)
	{
		if (k == Key::None)
			return "None";
		if (auto nk = FindNamed(k))
			return nk->id;
		const uint16 v = (uint16)k;
		if ((v >= '0' && v <= '9') || (v >= 'A' && v <= 'Z'))
			return std::string(1, (char)v);
		if (v >= (uint16)Key::F1 && v <= (uint16)Key::F1 + 23)
			return fmt::format("F{}", v - (uint16)Key::F1 + 1);
		return fmt::format("VK{:02X}", v);
	}

	Key KeyFromId(std::string_view idIn)
	{
		std::string id(idIn);
		boost::trim(id);
		if (id.empty() || boost::iequals(id, "None"))
			return Key::None;
		for (auto& nk : kNamedKeys)
			if (boost::iequals(id, nk.id))
				return nk.key;
		if (id.size() == 1)
		{
			char c = (char)toupper((unsigned char)id[0]);
			if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))
				return (Key)(uint16)c;
		}
		if ((id[0] == 'F' || id[0] == 'f') && id.size() <= 3)
		{
			int n = atoi(id.c_str() + 1);
			if (n >= 1 && n <= 24)
				return (Key)((uint16)Key::F1 + n - 1);
		}
		if (id.size() == 4 && boost::istarts_with(id, "VK"))
			return (Key)(uint16)strtoul(id.c_str() + 2, nullptr, 16);
		return Key::None;
	}

	std::string KeyDisplayName(Key k)
	{
		if (k == Key::None)
			return "-";
		if (auto nk = FindNamed(k))
			return nk->display;
		const uint16 v = (uint16)k;
		if (v >= (uint16)Key::F1 && v <= (uint16)Key::F1 + 23)
			return fmt::format("F{}", v - (uint16)Key::F1 + 1);
#if BOOST_OS_WINDOWS
		// layout-aware character for printable keys
		UINT ch = MapVirtualKeyW(v, MAPVK_VK_TO_CHAR) & 0x7FFF;
		if (ch >= 0x20)
		{
			wchar_t w[2] = { (wchar_t)towupper((wint_t)ch), 0 };
			return boost::nowide::narrow(w);
		}
		UINT sc = MapVirtualKeyW(v, MAPVK_VK_TO_VSC);
		wchar_t name[64]{};
		if (sc && GetKeyNameTextW((LONG)(sc << 16), name, 64) > 0)
			return boost::nowide::narrow(name);
#endif
		return KeyToId(k);
	}

	std::string KeyCapLabel(Key k)
	{
		if (k == Key::None || IsMouseKey(k))
			return "";
		if (auto nk = FindNamed(k))
			return nk->cap;
		std::string s = KeyDisplayName(k);
		boost::to_upper(s);
		return s;
	}
}
