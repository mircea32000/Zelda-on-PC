#include "Cafe/OS/RPL/COSModule.h"

void swkbd_render(bool mainWindow);
bool swkbd_hasKeyboardInputHook();
void swkbd_keyInput(uint32 keyCode);
std::string swkbd_debugForm(int show); // PC Edition dev bridge: 1 show, 0 hide, -1 query

namespace swkbd
{
	COSModule* GetModule();
}
