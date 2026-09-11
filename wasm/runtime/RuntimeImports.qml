pragma Singleton

// THE QML MODULE SURFACE THIS RUNTIME OFFERS, declared by importing it.
//
// Nothing instantiates this file and nothing should. It exists because of one
// property of a STATIC Qt: a QML module is reachable only if its plugin was
// LINKED IN, and what gets linked in is decided by `qmlimportscanner` reading
// the imports out of this target's own QML at build time. A module's document
// arrives at RUNTIME — that is the whole point of the Web container — so the
// scanner never sees `import QtQuick.Layouts` in it, the plugin is not linked,
// and the document fails to instantiate in a browser with
// `plugin "qtquicklayoutsplugin" not found`.
//
// Linking `Qt6::QuickLayouts` is NOT enough and looks exactly like enough: the
// library is in the image, the build succeeds, and only the QML type
// registration is missing. The import below is what fixes it.
//
// So: everything a module's QML may import has to be named here, and this file
// is therefore also the answer to "what can a `web` variant's QML use?".
// Adding to it costs image size; removing from it breaks modules already
// published. Both are decisions, which is why they are one file.
import QtQml
import QtCore            // Settings — Logos.Theme persists the theme with it
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

import Logos.Theme
import Logos.Icons
import Logos.Controls

QtObject {
}
