# THE RUNTIME'S SHAPE, WITH THE TRANSPORT IN IT.
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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 RemoteObjects)

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
    Qt6::RemoteObjects
)

# The archive is consumed as a plain path (there is no exported package config
# yet), so the embind flag its INTERFACE would have carried is restated here.
target_link_options(logos_messageport_wasm_smoke PRIVATE "-lembind")
