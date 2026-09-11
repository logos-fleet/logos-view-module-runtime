import QtQuick

import Logos.Theme
import Logos.Controls

// Imports the design system rather than only linking it: a QML document that
// names Logos.Controls fails to COMPILE if the module's types are not
// registered, which is a stronger statement about the runtime than any amount
// of successful linking.
Window {
    visible: true
    width: 360
    height: 240
    color: Theme.palette.background

    Column {
        anchors.centerIn: parent
        spacing: 8
        LogosText { text: "logos-messageport-wasm-smoke" }
        LogosButton { text: "press" }
    }
}
