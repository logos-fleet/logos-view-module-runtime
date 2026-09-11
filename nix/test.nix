{ pkgs, logosSdk, logosQtHost, logosProtocol }:

pkgs.stdenv.mkDerivation {
  pname = "logos-view-module-runtime-check";
  version = "1.0.0";

  src = ../.;

  # Required wherever the Qt wrapper hooks are absent (see below).
  dontWrapQtApps = true;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
  ]
  # wrapQtAppsHook fails to EVALUATE for a mingw host, and wrap-qt-apps-hook.sh
  # would skip a PE anyway (`isELF || isMachO || continue`). Gating it off is
  # only half the fix -- qtbase's setup hook hard-errors unless
  # dontWrapQtApps is also set below.
  ++ pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows) pkgs.qt6.wrapQtAppsHook;

  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    pkgs.qt6.qtdeclarative
    logosSdk
    logosQtHost
    logosProtocol
  ];

  dontStrip = true;

  preConfigure = ''
    export MACOSX_DEPLOYMENT_TARGET=12.0

    # Point CMake at the SDK store path directly. CMakeLists.txt
    # uses `find_package(logos-cpp-sdk CONFIG PATHS
    # $LOGOS_CPP_SDK_ROOT/lib/cmake/logos-cpp-sdk)`, which carries
    # include dirs + the link interface (OpenSSL, Boost, nlohmann)
    # via the imported target — no need to stage a vendored copy.
    cmakeFlagsArray+=("-DLOGOS_CPP_SDK_ROOT=${logosSdk}")
    cmakeFlagsArray+=("-DLOGOS_QT_HOST_ROOT=${logosQtHost}")
    cmakeFlagsArray+=("-DLOGOS_PROTOCOL_ROOT=${logosProtocol}")
    # Qt splits its host TOOLS into separate packages that must run on the
    # BUILD machine; -DQT_HOST_PATH=<qtbase> cannot reach them. Empty natively.
    ${pkgs.lib.concatMapStringsSep "\n    "
        (f: "cmakeFlagsArray+=(\"" + f + "\")")
        (pkgs.logosQtCrossCmakeFlags or [ ])}
  '';

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Debug"
    "-DLOGOS_VIEW_RUNTIME_BUILD_TESTS=ON"
  ];

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export QT_QPA_PLATFORM=offscreen

    # WHERE THE QML MODULES ARE. ctest runs the test binaries straight out of
    # the build tree, so wrapQtAppsHook -- which only ever touches what gets
    # installed -- has set nothing for them, and a test that COMPILES a QML
    # document (test_web_runtime does, the way the runtime compiles a module's)
    # fails with `module "QtQml" is not installed`. Both spellings: Qt 6 reads
    # QML_IMPORT_PATH, and QML2_IMPORT_PATH is still honoured and still what
    # most of the ecosystem sets.
    export QML_IMPORT_PATH="${pkgs.qt6.qtdeclarative}/lib/qt-6/qml"
    export QML2_IMPORT_PATH="$QML_IMPORT_PATH"

    ctest --output-on-failure
    runHook postCheck
  '';

  # Emit a marker so Nix has something to install; the real purpose of this
  # derivation is to run the unit tests during checkPhase.
  installPhase = ''
    mkdir -p $out
    echo "tests-passed" > $out/result
  '';

  meta = with pkgs.lib; {
    description = "Unit tests for logos-view-module-runtime (LogosQmlBridge)";
    platforms = platforms.unix ++ platforms.windows;
    license = licenses.mit;
  };
}
