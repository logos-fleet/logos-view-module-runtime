import QtQuick

import Logos.Theme
import Logos.Controls

// THE RUNTIME'S SHELL — everything on screen that is not a module.
//
// It is deliberately almost nothing: a background in the Logos palette, a line
// of text for the two states a module view cannot show for itself (none
// installed yet, and one that failed to compile), and a stage. A module's own
// QML is the UI; this is the frame it arrives in.
//
// It also imports the design system rather than only linking it: a QML document
// that names Logos.Controls fails to COMPILE if the module's types are not
// registered, which is a stronger statement about the image than any amount of
// successful linking.
Window {
    id: root
    visible: true
    width: 480
    height: 640
    color: Theme.palette.background

    // Where a module's view goes. One at a time on a phone — ADR 0004 keeps the
    // QML runtime alive only for the VISIBLE Downloaded module — but the
    // runtime holds however many are installed and the container decides which
    // is on screen.
    Item {
        id: stage
        anchors.fill: parent
    }

    LogosText {
        anchors.centerIn: parent
        visible: runtime.installedModules.length === 0
        text: root.failure.length > 0 ? root.failure : qsTr("Waiting for a module")
    }

    property string failure: ""

    Connections {
        target: runtime

        function onModuleViewInstalled(moduleName, view) {
            root.failure = ""
            // The module's root is an Item; the runtime created it but never
            // decided where it goes, which is this file's job.
            if (view && view.hasOwnProperty("parent"))
                view.parent = stage
        }

        function onModuleViewFailed(moduleName, error) {
            // A module whose QML does not compile looks exactly like one that
            // has not loaded yet. Say which.
            root.failure = moduleName + ": " + error
        }
    }
}
