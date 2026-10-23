add_library(CemuPCEdition
	PCEdition.cpp
	PCEdition.h
	PCSettings.cpp
	PCSettings.h
	PCKeys.cpp
	PCKeys.h
	PCInput.cpp
	PCInput.h
	PCCamera.cpp
	PCCamera.h
	PCGraphics.cpp
	PCGraphics.h
	PCOverlay.cpp
	PCOverlay.h
	PCDevBridge.cpp
	PCDevBridge.h
	PCUI.cpp
	PCUI.h
	PCMenus.cpp
	PCMenus.h
	PCPrompts.cpp
	PCPrompts.h
	PCProfiler.cpp
	PCProfiler.h
	PCIcon.cpp
	PCIcon.h
	PCMapIcons.cpp
	PCMapIcons.h
)

set_property(TARGET CemuPCEdition PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(CemuPCEdition)

target_link_libraries(CemuPCEdition PRIVATE
	CemuCafe
	CemuCommon
	CemuConfig
	CemuInput
	CemuUtil
	imguiImpl
	pugixml::pugixml
	ZArchive::zarchive
	Boost::headers
)

if (WIN32)
	target_link_libraries(CemuPCEdition PRIVATE ws2_32)
else()
	target_link_libraries(CemuPCEdition PRIVATE ${CMAKE_DL_LIBS})
endif()
