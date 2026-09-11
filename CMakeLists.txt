cmake_minimum_required(VERSION 3.16)
project(LogosViewModuleRuntime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

option(LOGOS_VIEW_RUNTIME_BUILD_TESTS "Build unit tests for logos-view-module-runtime" OFF)

# THE WASM SUBSET: the MessagePort QtRO transport and the QML runtime over it.
# Those two are the only parts of this repo that have to compile for
# wasm32-emscripten, and the only parts that can — everything else links
# logos-qt-host, which brings the whole native SDK stack with it. Turning this
# on builds `logos_messageport` and `logos_web_runtime` and stops, which is what
# nix/wasm.nix hands to a Qt-for-WebAssembly toolchain.
#
# It is also a useful DESKTOP configuration, and the tests use it as one: this
# half needs no SDK roots, so it configures and runs anywhere Qt does.
option(LOGOS_WEB_ONLY "Build only the Web container's half: the MessagePort transport and the QML runtime" OFF)

# Split in two: the first line is everything the Web container's half needs,
# and a wasm build stops after it.
find_package(Qt6 REQUIRED COMPONENTS Core Network Qml RemoteObjects)

# ── MessagePort QtRO transport ──────────────────────────────────────────────
#
# Its own target, and deliberately Qt-only. In the Web container this code runs
# inside a Qt-for-WebAssembly image where LogosAPI, the token manager and the
# whole native SDK do not exist; making it a separate library is what keeps that
# true, rather than a comment asking people to keep it true.
add_library(logos_messageport STATIC
    src/LogosMessagePort.cpp
    src/LogosMessagePortDevice.cpp
    src/LogosMessagePortTransport.cpp
    include/LogosMessagePort.h
    include/LogosMessagePortDevice.h
    include/LogosMessagePortTransport.h
)

target_include_directories(logos_messageport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(logos_messageport PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
)

set_target_properties(logos_messageport PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The real port, added only where there is a browser to have one. embind is what
# carries the two calls that cross the language boundary
# (Module.logosAdoptMessagePort in, Module.logosMessagePortDeliver out); it is
# INTERFACE because the flag belongs to whoever links the final image.
if(EMSCRIPTEN)
    target_sources(logos_messageport PRIVATE
        src/LogosEmscriptenMessagePort.cpp
        include/LogosEmscriptenMessagePort.h
    )
    target_link_options(logos_messageport INTERFACE "-lembind")
endif()

install(TARGETS logos_messageport ARCHIVE DESTINATION lib)

# ── the Web container's QML runtime ─────────────────────────────────────────
#
# ADR 0004's bundled runtime, as a library: one QML engine, one QtRO node over
# the transport above, `logos` in the engine's root context, and a module's QML
# loaded into it as text at install time. Qt Qml and NOT Qt Quick on purpose —
# where a view goes on screen is the runtime app's business, and keeping the
# scene out of here is what lets the whole thing be tested on a desktop with no
# window (tests/test_web_runtime.cpp).
#
# Separate from logos_messageport because they answer to different consumers: a
# module backend inside its Wasm host links the transport and has no QML engine
# in it at all.
add_library(logos_web_runtime STATIC
    src/LogosWebBridge.cpp
    src/LogosWebCallRouter.cpp
    src/LogosWebRuntime.cpp
    include/LogosWebBridge.h
    include/LogosWebCallRouter.h
    include/LogosWebRuntime.h
)

target_include_directories(logos_web_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(logos_web_runtime PUBLIC
    logos_messageport
    Qt6::Core
    Qt6::Qml
    Qt6::RemoteObjects
)

set_target_properties(logos_web_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

install(TARGETS logos_web_runtime ARCHIVE DESTINATION lib)

if(LOGOS_WEB_ONLY)
    install(FILES
        include/LogosMessagePort.h
        include/LogosMessagePortDevice.h
        include/LogosMessagePortTransport.h
        include/LogosEmscriptenMessagePort.h
        include/LogosWebBridge.h
        include/LogosWebCallRouter.h
        include/LogosWebRuntime.h
        DESTINATION include)

    # The Web half's own tests, which need nothing this configuration skipped.
    if(LOGOS_VIEW_RUNTIME_BUILD_TESTS)
        find_package(Qt6 REQUIRED COMPONENTS Test)
        enable_testing()
        add_subdirectory(tests)
    endif()
    return()
endif()

find_package(Qt6 REQUIRED COMPONENTS Gui Quick)
if(LOGOS_VIEW_RUNTIME_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()

if(NOT DEFINED LOGOS_CPP_SDK_ROOT)
    message(FATAL_ERROR "LOGOS_CPP_SDK_ROOT must be defined")
endif()
if(NOT DEFINED LOGOS_QT_HOST_ROOT)
    message(FATAL_ERROR "LOGOS_QT_HOST_ROOT must be defined")
endif()
if(NOT DEFINED LOGOS_PROTOCOL_ROOT)
    message(FATAL_ERROR "LOGOS_PROTOCOL_ROOT must be defined")
endif()

message(STATUS "logos-cpp-sdk : ${LOGOS_CPP_SDK_ROOT}")
message(STATUS "logos-qt-host : ${LOGOS_QT_HOST_ROOT}")
message(STATUS "logos-protocol: ${LOGOS_PROTOCOL_ROOT}")

# Link the SDK stack via exported CMake targets so the consumer inherits
# INTERFACE_INCLUDE_DIRECTORIES + INTERFACE_LINK_LIBRARIES (OpenSSL,
# Boost::system, nlohmann_json). Since the B1 split the Qt HOST RUNTIME
# (LogosAPI and the provider objects) lives in logos-plugin-qt as
# logos-qt-host, which carries the logos-protocol library transitively; the
# Qt-free base SDK contributes header-only types via
# logos-cpp-sdk::logos_headers.
find_package(logos-protocol REQUIRED CONFIG
    PATHS ${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol
    NO_DEFAULT_PATH)
find_package(logos-qt-host REQUIRED CONFIG
    PATHS ${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host
    NO_DEFAULT_PATH)
find_package(logos-cpp-sdk REQUIRED CONFIG
    PATHS ${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk
    NO_DEFAULT_PATH)

# A REQUIRED find_package that resolves to the wrong prefix would still leave
# the target undefined and every LogosAPI symbol undefined at link time — with
# an error far from the cause. Say it here instead.
if(NOT TARGET logos-qt-host::logos_qt_host)
    message(FATAL_ERROR
        "logos-qt-host::logos_qt_host is not a target after "
        "find_package(logos-qt-host) at ${LOGOS_QT_HOST_ROOT}. "
        "LOGOS_QT_HOST_ROOT must point at a logos-plugin-qt "
        "packages.<system>.logos-qt-host prefix.")
endif()

# ── Static library: LogosQmlBridge + ViewModuleHost ─────────────────────────
add_library(logos_view_module_runtime STATIC
    src/LogosQmlBridge.cpp
    src/ViewModuleHost.cpp
    include/LogosQmlBridge.h
    include/ViewModuleHost.h
    include/LogosViewPlugin.h
    include/LogosViewReplicaFactory.h
    include/LogosIntent.h
    include/LogosIntentRouter.h
)

target_include_directories(logos_view_module_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(logos_view_module_runtime PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
    Qt6::Qml
    logos_messageport
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
)

set_target_properties(logos_view_module_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── ui-host executable ──────────────────────────────────────────────────────
add_executable(ui-host ui-host/main.cpp)

target_include_directories(ui-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ui-host PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
)

set_target_properties(ui-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(APPLE)
    set_target_properties(ui-host PROPERTIES
        INSTALL_RPATH "@executable_path/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
elseif(UNIX)
    set_target_properties(ui-host PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# ── Install ─────────────────────────────────────────────────────────────────
install(TARGETS logos_view_module_runtime ARCHIVE DESTINATION lib)
install(TARGETS ui-host RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

# ── Tests ───────────────────────────────────────────────────────────────────
if(LOGOS_VIEW_RUNTIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
