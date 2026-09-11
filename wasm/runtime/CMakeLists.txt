# THE WEB CONTAINER'S QML RUNTIME, as an image: Qt Quick, the Logos design
# system, the MessagePort QtRO transport and LogosWebRuntime in ONE static wasm
# binary. This is ADR 0004's bundled runtime — the thing the app ships and signs
# once, into which every Downloaded module's QML is loaded at install time.
#
# A separate cmake project rather than another target next door, for the reason
# logos-design-system' wasm smoke is one: it consumes the INSTALLED libraries
# the way a host will — headers and static archives off a prefix — so a header
# that was never installed, or an archive whose emscripten flags do not survive
# the export, fails here instead of inside a page months later.
cmake_minimum_required(VERSION 3.21)
project(logos_qml_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Svg RemoteObjects)
find_package(LogosDesignSystem CONFIG REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

if(NOT DEFINED LOGOS_WEB_RUNTIME_ROOT)
    message(FATAL_ERROR "LOGOS_WEB_RUNTIME_ROOT must point at an installed logos_web_runtime prefix")
endif()

qt_add_executable(logos_qml_runtime main.cpp)

qt_add_qml_module(logos_qml_runtime
    URI LogosQmlRuntime
    VERSION 1.0
    QML_FILES Main.qml
)

target_include_directories(logos_qml_runtime PRIVATE ${LOGOS_WEB_RUNTIME_ROOT}/include)

# Plain paths, and in this order: liblogos_web_runtime.a references the
# transport, and a static archive is searched only for symbols undefined when
# the linker reaches it.
target_link_libraries(logos_qml_runtime PRIVATE
    ${LOGOS_WEB_RUNTIME_ROOT}/lib/liblogos_web_runtime.a
    ${LOGOS_WEB_RUNTIME_ROOT}/lib/liblogos_messageport.a
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
    Qt6::RemoteObjects
    Logos::DesignSystem
)

# Static Qt: qt_add_executable's finalizer links the design system's QML plugins
# plainly while the umbrella links them WHOLE_ARCHIVE, and CMake refuses both.
# The design system's package config ships the one-line resolution.
logos_design_system_resolve_static_plugins(logos_qml_runtime)

# The archives are consumed as plain paths (there is no exported package config
# yet), so the embind flag their INTERFACE would have carried is restated here.
target_link_options(logos_qml_runtime PRIVATE "-lembind")
