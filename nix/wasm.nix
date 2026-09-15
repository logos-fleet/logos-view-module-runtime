# THE WEB CONTAINER'S QML RUNTIME, BUILT FOR WEBASSEMBLY.
#
# ADR 0004's bundled runtime as one static image: Qt Quick, the Logos design
# system, the MessagePort QtRO transport and LogosWebRuntime — the thing an app
# ships and signs once, into which every Downloaded module's QML is loaded at
# install time. It compiles this repo's web half for wasm32-emscripten against
# logos-nix' Qt-for-WebAssembly, installs it as a prefix, and then LINKS THE
# RUNTIME APP against that prefix, the way a host would.
#
# WHAT IT CANNOT CHECK, and says so rather than implying otherwise: whether the
# bytes move and whether anything renders. A Qt-wasm image needs a canvas, a
# page and a peer. The transport's behaviour is checked on the desktop against a
# real QRemoteObjectHost (tests/test_messageport_transport.cpp) over a loopback
# port pair with the same four properties a MessagePort has, and the runtime's
# is checked the same way (tests/test_web_runtime.cpp) against a real QML engine.
# What is left here is exactly the part a desktop test cannot reach: that the
# code COMPILES under emscripten, that the JS glue is well-formed (emscripten
# compiles an EM_JS body into the image's JS library, so a syntax error fails
# this link), and that every page-facing export survives into the image.
{ pkgs, lib, qtWasm, designSystemWasm }:

pkgs.stdenv.mkDerivation {
  pname = "logos-qml-runtime-wasm";
  version = "1.0.0";

  src = ../.;

  nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];

  dontUseCmakeConfigure = true;
  dontWrapQtApps = true;
  # There is nothing in a wasm artifact for the Darwin/Linux fixup phases to
  # rewrite, and `strip` cannot read one.
  dontStrip = true;
  dontFixup = true;

  buildPhase = ''
    runHook preBuild
    ${pkgs.logosEmscriptenSetup}

    # LOGOS_WEB_ONLY stops the top-level CMakeLists before it asks for
    # logos-qt-host, logos-protocol and the rest of the native SDK stack — none
    # of which exists for wasm, and none of which the Web container's half
    # needs.
    cmake -S . -B build-lib -GNinja ${lib.escapeShellArgs qtWasm.cmakeFlags} \
      -DCMAKE_BUILD_TYPE=Release \
      -DLOGOS_WEB_ONLY=ON \
      -DCMAKE_INSTALL_PREFIX=$PWD/prefix
    cmake --build build-lib --parallel $NIX_BUILD_CORES
    cmake --install build-lib

    # Against the INSTALLED prefix, not the source tree: a header the install
    # forgot is a failure a consumer would otherwise be the first to find.
    #
    # The last two -D lines RESTATE the search paths qtWasm.cmakeFlags already
    # sets (a repeated -D wins), because this consumer needs the design system's
    # prefix alongside Qt's. Both variables, not just CMAKE_PREFIX_PATH: the
    # Emscripten toolchain sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE to ONLY, so a
    # prefix named only in CMAKE_PREFIX_PATH is never searched.
    cmake -S wasm/runtime -B build-runtime -GNinja ${lib.escapeShellArgs qtWasm.cmakeFlags} \
      -DCMAKE_BUILD_TYPE=Release \
      -DLOGOS_WEB_RUNTIME_ROOT=$PWD/prefix \
      -DCMAKE_PREFIX_PATH="${qtWasm.prefix};${designSystemWasm}" \
      -DCMAKE_FIND_ROOT_PATH="${qtWasm.prefix};${designSystemWasm}"
    cmake --build build-runtime --parallel $NIX_BUILD_CORES

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out
    cp -r prefix/lib prefix/include $out/

    # Everything a Qt-wasm link is supposed to leave behind, and the whole of
    # what a page needs to load it: checked here, copied to $out/www below.
    artifacts=(
      logos_qml_runtime.wasm
      logos_qml_runtime.js
      logos_qml_runtime.html
      qtloader.js
    )
    for f in "''${artifacts[@]}"; do
      [ -f "build-runtime/$f" ] || { echo "the wasm link produced no $f" >&2; exit 1; }
    done

    image=build-runtime/logos_qml_runtime.wasm
    glue=build-runtime/logos_qml_runtime.js

    # EVERY CALL THAT CROSSES THE LANGUAGE BOUNDARY, read back off the linked
    # JS. They are embind exports, which means a linker that dropped the
    # translation unit — the ordinary fate of a static-library object nothing
    # references, and NOTHING references these, because only JS calls them —
    # leaves an image that builds, loads, renders, and can never be handed a
    # port or a module. Asserting the names is the only thing that catches it.
    #
    #   logosAdoptMessagePort / logosMessagePortDeliver  the wire (the transport)
    #   logosInstallModuleView / logosRemoveModuleView   a module's QML
    #   logosConnectBackend / logosRuntimeLastError      the rest of the page API
    #   logosViewItem                                    where an item is, for
    #                                                    a host driving the scene
    for symbol in logosAdoptMessagePort logosMessagePortDeliver \
                  logosInstallModuleView logosRemoveModuleView \
                  logosConnectBackend logosRuntimeLastError logosViewItem; do
      grep -a -q "$symbol" "$glue" "$image" || {
        echo "the image does not export $symbol: the translation unit carrying" >&2
        echo "  it was dropped from the link (embind registrations live in an" >&2
        echo "  object nothing references from C++)." >&2
        exit 1
      }
    done

    # AND THE LOGOS TYPES ARE STILL IN IT. `Logos_<Mod>Plugin` is the RTTI name
    # of each design-system module's QQmlEngineExtensionPlugin, read as bytes
    # because a Release wasm link runs wasm-opt and leaves almost no symbol
    # table (logos-design-system' own smoke explains this at length). The claim
    # here is narrower and is the one this repo owns: adding the MessagePort
    # transport and the runtime to the link did not cost it its design system —
    # under a STATIC Qt the two compete for the same QML plugins, and the loser
    # is silent.
    for mod in Theme Icons Controls; do
      grep -a -q "Logos_''${mod}Plugin" "$image" || {
        echo "the image carries no Logos_''${mod}Plugin: the design system's" >&2
        echo "  static QML plugin was dropped from the runtime's link." >&2
        exit 1
      }
    done

    mkdir -p $out/www
    for f in "''${artifacts[@]}"; do
      cp "build-runtime/$f" $out/www/
    done

    raw=$(wc -c < "$image")
    ${pkgs.brotli}/bin/brotli -q 11 -c "$image" > "$TMPDIR/image.br"
    br=$(wc -c < "$TMPDIR/image.br")
    echo "logos-qml-runtime-wasm: Qt ${qtWasm.version}, emsdk ${pkgs.logosEmscriptenVersion}"
    echo "  the Web container's QML runtime (Qt Quick + Logos design system"
    echo "  + MessagePort QtRO + LogosWebRuntime)"
    echo "  raw $raw B"
    echo "  brotli $br B"

    runHook postInstall
  '';

  meta = with lib; {
    description = "The Web container's Qt-wasm QML runtime: Qt Quick, the Logos design system and QtRO over a MessagePort in one image";
    platforms = platforms.unix;
  };
}
