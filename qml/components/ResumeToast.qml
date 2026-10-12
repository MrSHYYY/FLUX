import QtQuick
import QtQuick.Layouts
import "Theme.js" as Theme

// Briefly confirms the saved playback position after resuming.
Rectangle {
    id: root

    property string text: ""

    implicitHeight: 46
    implicitWidth: toastRow.implicitWidth + 36
    radius: 23
    color: "#F2101015"
    border.width: 1
    border.color: "#2EFFFFFF"

    RowLayout {
        id: toastRow
        anchors.centerIn: parent
        spacing: 14

        Rectangle {
            implicitWidth: 8
            implicitHeight: 8
            radius: 4
            color: Theme.accent
            Layout.alignment: Qt.AlignVCenter
        }

        Text {
            text: root.text
            color: Theme.text
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }

    }
}
