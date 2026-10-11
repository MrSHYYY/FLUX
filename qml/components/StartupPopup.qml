import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import "Theme.js" as Theme

// Discord-style startup updater popup window.
// Opens first when FLUX launches:
//  - Checks GitHub Releases for a newer version ("CHECKING FOR UPDATES...")
//  - If an update is available, automatically downloads and installs it ("DOWNLOADING UPDATE...")
//  - If up to date (or offline), transitions directly into the main FLUX window ("STARTING...")
Window {
    id: popup

    signal finished()

    property string phase: "checking"   // "checking" | "downloading" | "installing" | "starting" | "error"
    property bool checkStarted: false
    property bool doneEmitted: false

    width: 300
    height: 350
    minimumWidth: 300
    maximumWidth: 300
    minimumHeight: 350
    maximumHeight: 350
    visible: true
    title: "FLUX"
    color: "transparent"
    flags: Qt.Window | Qt.FramelessWindowHint
    transientParent: null

    x: Math.round((Screen.width - width) / 2)
    y: Math.round((Screen.height - height) / 2)

    function proceedToApp() {
        if (popup.doneEmitted || popup.phase === "downloading" || popup.phase === "installing") {
            return
        }
        safetyTimer.stop()
        popup.phase = "starting"
        launchTimer.start()
    }

    function evaluateUpdaterState() {
        if (!fluxUpdater || popup.doneEmitted || popup.phase === "starting") {
            return
        }
        var st = fluxUpdater.state
        if (st === "available") {
            safetyTimer.stop()
            popup.phase = "downloading"
            fluxUpdater.startUpdate()
        } else if (st === "downloading") {
            safetyTimer.stop()
            popup.phase = "downloading"
        } else if (st === "installing") {
            safetyTimer.stop()
            popup.phase = "installing"
        } else if (st === "upToDate") {
            popup.proceedToApp()
        } else if (st === "idle" && popup.checkStarted && popup.phase === "checking") {
            // Silent check finished without an update (e.g. offline / local BDIX network only)
            popup.proceedToApp()
        } else if (st === "error") {
            safetyTimer.stop()
            if (popup.phase === "downloading" || popup.phase === "installing") {
                popup.phase = "error"
            } else {
                popup.proceedToApp()
            }
        }
    }

    Component.onCompleted: {
        if (!fluxUpdater) {
            popup.proceedToApp()
            return
        }
        startCheckTimer.start()
        safetyTimer.start()
    }

    // Brief initial beat so the popup renders smoothly before network activity begins
    Timer {
        id: startCheckTimer
        interval: 300
        repeat: false
        onTriggered: {
            if (!fluxUpdater) {
                popup.proceedToApp()
                return
            }
            popup.checkStarted = true
            fluxUpdater.checkForUpdates(true)
            popup.evaluateUpdaterState()
        }
    }

    // Never block startup if the user has no internet access
    Timer {
        id: safetyTimer
        interval: 6500
        repeat: false
        onTriggered: {
            if (popup.phase === "checking") {
                popup.proceedToApp()
            }
        }
    }

    // Smooth "STARTING..." transition before opening the main application window
    Timer {
        id: launchTimer
        interval: 320
        repeat: false
        onTriggered: {
            if (!popup.doneEmitted) {
                popup.doneEmitted = true
                popup.finished()
            }
        }
    }

    Connections {
        target: fluxUpdater

        function onStateChanged() {
            popup.evaluateUpdaterState()
        }
    }

    // ---- Card Surface ---------------------------------------------------------
    Rectangle {
        id: card
        anchors.fill: parent
        radius: 16
        color: Theme.bg
        border.width: 1
        border.color: Theme.borderHi
        clip: true

        // Subtle top crimson ambient glow
        Rectangle {
            width: 240
            height: 240
            radius: 120
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: -130
            color: "#1AE50914"
        }

        // Drag anywhere on the popup card
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: function(mouse) {
                if (mouse.button === Qt.LeftButton) {
                    popup.startSystemMove()
                }
            }
        }

        // Subtle close button in top-right corner
        Rectangle {
            width: 26
            height: 26
            radius: 13
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 10
            anchors.rightMargin: 10
            color: closeMouse.containsMouse ? Theme.surfaceHi : "transparent"
            z: 5

            Text {
                anchors.centerIn: parent
                text: "\u00D7"
                color: closeMouse.containsMouse ? Theme.text : Theme.textMute
                font.pixelSize: 16
                font.weight: Font.Medium
            }

            MouseArea {
                id: closeMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: Qt.quit()
            }
        }

        // ---- Main Vertical Content --------------------------------------------
        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 28
            anchors.rightMargin: 28
            anchors.topMargin: 40
            anchors.bottomMargin: 22
            spacing: 0

            // Animated Brand Emblem + Spinner Ring
            Item {
                Layout.alignment: Qt.AlignHCenter
                width: 92
                height: 92

                // Outer pulse ring
                Rectangle {
                    anchors.centerIn: parent
                    width: 88
                    height: 88
                    radius: 44
                    color: "transparent"
                    border.width: 1
                    border.color: "#22E50914"

                    SequentialAnimation on scale {
                        loops: Animation.Infinite
                        running: popup.visible && popup.phase !== "error"
                        NumberAnimation { from: 0.96; to: 1.06; duration: 1100; easing.type: Easing.InOutSine }
                        NumberAnimation { from: 1.06; to: 0.96; duration: 1100; easing.type: Easing.InOutSine }
                    }
                }

                // Rotating crimson spinner arc
                FluxSpinner {
                    anchors.centerIn: parent
                    size: 84
                    thickness: 3
                    color: popup.phase === "error" ? Theme.danger : Theme.accent
                    trackColor: "#18FFFFFF"
                    running: popup.visible && popup.phase !== "error"
                }

                // Inner dark badge with FLUX monogram
                Rectangle {
                    anchors.centerIn: parent
                    width: 62
                    height: 62
                    radius: 31
                    color: Theme.surface
                    border.width: 1
                    border.color: Theme.borderHi

                    Text {
                        anchors.centerIn: parent
                        text: "F"
                        color: Theme.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: 32
                        font.weight: Font.Black
                    }
                }
            }

            Item { Layout.preferredHeight: 16 }

            // Wordmark
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "FLUX"
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 22
                font.weight: Font.Black
                font.letterSpacing: 5
            }

            Item { Layout.preferredHeight: 22 }

            // Primary Discord-style status headline
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: {
                    if (popup.phase === "downloading") return "DOWNLOADING UPDATE\u2026"
                    if (popup.phase === "installing")  return "INSTALLING UPDATE\u2026"
                    if (popup.phase === "starting")    return "STARTING\u2026"
                    if (popup.phase === "error")       return "UPDATE INTERRUPTED"
                    return "CHECKING FOR UPDATES\u2026"
                }
                color: popup.phase === "error" ? Theme.danger : Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: 1.4
            }

            Item { Layout.preferredHeight: 6 }

            // Secondary detail line
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                text: {
                    if (popup.phase === "downloading") {
                        var pct = Math.round((fluxUpdater ? fluxUpdater.progress : 0) * 100)
                        var ver = fluxUpdater && fluxUpdater.latestVersion.length > 0
                                  ? ("v" + fluxUpdater.latestVersion) : "new version"
                        return "Updating to " + ver + " (" + pct + "%)"
                    }
                    if (popup.phase === "installing") {
                        return "Applying update and restarting FLUX"
                    }
                    if (popup.phase === "starting") {
                        return "Ready to stream"
                    }
                    if (popup.phase === "error") {
                        return fluxUpdater && fluxUpdater.errorText.length > 0
                               ? fluxUpdater.errorText
                               : "Could not download the update."
                    }
                    return "Verifying latest release"
                }
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            // Byte progress text during download
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 3
                visible: popup.phase === "downloading" && text.length > 0
                horizontalAlignment: Text.AlignHCenter
                text: fluxUpdater ? fluxUpdater.progressText : ""
                color: Theme.textMute
                font.family: Theme.monoFamily
                font.pixelSize: 10
            }

            Item { Layout.fillHeight: true }

            // Error recovery actions (only if an active download failed)
            RowLayout {
                Layout.fillWidth: true
                Layout.bottomMargin: 8
                visible: popup.phase === "error"
                spacing: 8

                FluxButton {
                    Layout.fillWidth: true
                    implicitHeight: 32
                    text: "Retry"
                    onClicked: {
                        if (fluxUpdater) {
                            popup.phase = "downloading"
                            fluxUpdater.startUpdate()
                        }
                    }
                }

                FluxButton {
                    Layout.fillWidth: true
                    implicitHeight: 32
                    variant: "secondary"
                    text: "Open App"
                    onClicked: popup.proceedToApp()
                }
            }

            // Progress Bar (indeterminate while checking, determinate while downloading/starting)
            Rectangle {
                id: progressTrack
                Layout.fillWidth: true
                implicitHeight: 5
                radius: 2.5
                color: Theme.surfaceTop
                visible: popup.phase !== "error"
                clip: true

                // Determinate bar (downloading / installing / starting)
                Rectangle {
                    visible: popup.phase !== "checking"
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accent
                    width: {
                        if (popup.phase === "downloading") {
                            return Math.max(6, parent.width * (fluxUpdater ? fluxUpdater.progress : 0))
                        }
                        return parent.width
                    }

                    Behavior on width { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                // Indeterminate sliding bar (checking)
                Rectangle {
                    id: indeterminateBar
                    visible: popup.phase === "checking"
                    width: parent.width * 0.38
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accent
                    x: -width

                    SequentialAnimation on x {
                        loops: Animation.Infinite
                        running: indeterminateBar.visible && popup.visible
                        NumberAnimation {
                            from: -indeterminateBar.width
                            to: progressTrack.width
                            duration: 1050
                            easing.type: Easing.InOutCubic
                        }
                    }
                }
            }

            Item { Layout.preferredHeight: 12 }

            // Version footer
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "v" + (fluxUpdater ? fluxUpdater.currentVersion : "")
                color: Theme.textMute
                font.family: Theme.monoFamily
                font.pixelSize: 10
                font.letterSpacing: 0.8
            }
        }
    }
}
