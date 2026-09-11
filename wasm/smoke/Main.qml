import QtQuick
import QtQuick.Controls

Window {
    visible: true
    width: 360
    height: 240

    Column {
        anchors.centerIn: parent
        spacing: 8
        Label { text: "logos-messageport-wasm-smoke" }
        Button { text: "press" }
    }
}
