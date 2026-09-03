# rexinput - Input system library
# Input system and drivers for Xbox 360 controller emulation
# Runtime-loaded via rex::system::LoadInputPlugin; consumers never link this
# target. The guest-facing types stay in include/rex/input/input.h, which the
# host reaches through rex/system/interfaces/input.h.

set(REXINPUT_XSB_SOURCES
    plugin_main.cpp
    input_system.cpp
    device_assignment.cpp
    state_merge.cpp
    mnk/mnk_input_driver.cpp
    nop/nop_input_driver.cpp
    sdl/sdl_input_driver.cpp
)

if(WIN32)
    list(APPEND REXINPUT_XSB_SOURCES xinput/xinput_input_driver.cpp)
endif()

add_library(rexinput_xsb SHARED ${REXINPUT_XSB_SOURCES})
add_library(rex::input-xsb ALIAS rexinput_xsb)

set_target_properties(rexinput_xsb PROPERTIES
    EXPORT_NAME input-xsb
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

rex_target_pch(rexinput_xsb
    <mutex>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread.h>
    <rex/thread/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
    <rex/system/xthread.h>
    <rex/ui/window.h>
    <rex/ui/presenter.h>
)

# Everything PRIVATE: the plugin has no consumers. Never link the OBJECT
# libraries (rexcore/rexui) here; rexruntime already contains those objects
# and relinking them would duplicate global state.
target_link_libraries(rexinput_xsb PRIVATE rexruntime SDL3::SDL3)
