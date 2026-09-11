# THE MESSAGEPORT TRANSPORT, BUILT FOR WEBASSEMBLY.
#
# In the Web container the QML runtime and a module's backend are two wasm
# images in one page and the wire between them is an HTML MessagePort (ADR
# 0004). This derivation is what proves the Qt side of that wire exists: it
# compiles `logos_messageport` for wasm32-emscripten against logos-nix'
# Qt-for-WebAssembly, installs it as a prefix, and then LINKS A QT QUICK IMAGE
# against that prefix — which is the shape the runtime will have.
#
# WHAT IT CANNOT CHECK, and says so rather than implying otherwise: whether the
# bytes move. A Qt-wasm image needs a canvas, a page and a peer, so the
# transport's behaviour is checked on the desktop against a real
# QRemoteObjectHost (tests/test_messageport_transport.cpp) over a loopback port
# pair with the same four properties a MessagePort has. What is left here is
# exactly the part a desktop test cannot reach: that the code COMPILES under
# emscripten, that the JS glue is well-formed (emscripten compiles an EM_JS body
# into the image's JS library, so a syntax error fails this link), and that both
# halves of the page-facing API survive into the image.
{ pkgs, lib, qtWasm }:

pkgs.stdenv.mkDerivation {
  pname = "logos-messageport-wasm";
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

    # LOGOS_MESSAGEPORT_ONLY stops the top-level CMakeLists before it asks for
    # logos-qt-host, logos-protocol and the rest of the native SDK stack — none
    # of which exists for wasm, and none of which the transport needs.
    cmake -S . -B build-lib -GNinja ${lib.escapeShellArgs qtWasm.cmakeFlags} \
      -DCMAKE_BUILD_TYPE=Release \
      -DLOGOS_MESSAGEPORT_ONLY=ON \
      -DCMAKE_INSTALL_PREFIX=$PWD/prefix
    cmake --build build-lib --parallel $NIX_BUILD_CORES
    cmake --install build-lib

    # Against the INSTALLED prefix, not the source tree: a header the install
    # forgot is a failure a consumer would otherwise be the first to find.
    cmake -S wasm/smoke -B build-smoke -GNinja ${lib.escapeShellArgs qtWasm.cmakeFlags} \
      -DCMAKE_BUILD_TYPE=Release \
      -DLOGOS_MESSAGEPORT_ROOT=$PWD/prefix
    cmake --build build-smoke --parallel $NIX_BUILD_CORES

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out
    cp -r prefix/lib prefix/include $out/

    image=build-smoke/logos_messageport_wasm_smoke.wasm
    for f in logos_messageport_wasm_smoke.wasm logos_messageport_wasm_smoke.js \
             logos_messageport_wasm_smoke.html qtloader.js; do
      [ -f "build-smoke/$f" ] || { echo "the wasm link produced no $f" >&2; exit 1; }
    done

    # THE TWO CALLS THAT CROSS THE LANGUAGE BOUNDARY, read back off the linked
    # JS. They are embind exports, which means a linker that dropped the
    # translation unit — the ordinary fate of a static-library object nothing
    # references, and NOTHING references these, because only JS calls them —
    # leaves an image that builds, loads, renders, and can never be handed a
    # port. Asserting the names is the only thing that catches it.
    for symbol in logosAdoptMessagePort logosMessagePortDeliver; do
      grep -a -q "$symbol" build-smoke/logos_messageport_wasm_smoke.js "$image" || {
        echo "the image does not export $symbol: the emscripten port was dropped" >&2
        echo "  from the link (embind registrations live in an object nothing" >&2
        echo "  references from C++)." >&2
        exit 1
      }
    done

    mkdir -p $out/www
    cp build-smoke/logos_messageport_wasm_smoke.wasm \
       build-smoke/logos_messageport_wasm_smoke.js \
       build-smoke/logos_messageport_wasm_smoke.html \
       build-smoke/qtloader.js $out/www/

    raw=$(wc -c < "$image")
    echo "logos-messageport-wasm: Qt ${qtWasm.version}, emsdk ${pkgs.logosEmscriptenVersion}"
    echo "  smoke image raw $raw B"

    runHook postInstall
  '';

  meta = with lib; {
    description = "The MessagePort QtRO transport built for wasm32-emscripten, linked into a Qt Quick image";
    platforms = platforms.unix;
  };
}
