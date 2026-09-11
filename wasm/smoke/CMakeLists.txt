# THE RUNTIME'S SHAPE: Qt Quick, the Logos design system and the MessagePort
# transport in ONE static wasm image — which is what ADR 0004's bundled QML
# runtime is, minus the module QML it will load at install time.
#
# A separate cmake project rather than another target next door, for the reason
# logos-design-system' wasm smoke is one: it consumes the INSTALLED library the
# way the QML runtime will — headers and a static archive off a prefix — so a
# header that was never installed, or an archive whose emscripten flags do not
# survive the export, fails here instead of inside the runtime months later.
cmake_minimum_required(VERSION 3.21)
project(logos_messageport_wasm_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Svg RemoteObjects)
find_package(LogosDesignSystem CONFIG REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

if(NOT DEFINED LOGOS_MESSAGEPORT_ROOT)
    message(FATAL_ERROR "LOGOS_MESSAGEPORT_ROOT must point at an installed logos_messageport prefix")
endif()

qt_add_executable(logos_messageport_wasm_smoke main.cpp)

qt_add_qml_module(logos_messageport_wasm_smoke
    URI LogosMessagePortSmoke
    VERSION 1.0
    QML_FILES Main.qml
)

target_include_directories(logos_messageport_wasm_smoke PRIVATE ${LOGOS_MESSAGEPORT_ROOT}/include)

target_link_libraries(logos_messageport_wasm_smoke PRIVATE
    ${LOGOS_MESSAGEPORT_ROOT}/lib/liblogos_messageport.a
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
    Qt6::RemoteObjects
    Logos::DesignSystem
)

# Static Qt: qt_add_executable's finalizer links the design system's QML plugins
# plainly while the umbrella links them WHOLE_ARCHIVE, and CMake refuses both.
# The design system's package config ships the one-line resolution.
logos_design_system_resolve_static_plugins(logos_messageport_wasm_smoke)

# The archive is consumed as a plain path (there is no exported package config
# yet), so the embind flag its INTERFACE would have carried is restated here.
target_link_options(logos_messageport_wasm_smoke PRIVATE "-lembind")
