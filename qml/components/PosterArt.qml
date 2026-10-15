import QtQuick
import "Theme.js" as Theme

// Typographic "poster" artwork. There is no image data in the library, so each title gets a
// stable, hand-composed layout (one of six) built from simple shapes + a giant outlined
// year / episode / initial. Drawn on top of the card's palette gradient.
Item {
    id: root

    property string seed: ""
    property string bigText: ""
    property bool isFolder: false
    property bool hovered: false

    readonly property int composition: Theme.posterStyle(root.seed)

    // Folder art proportions: Maintain a classic landscape aspect ratio (~1.48:1)
    // so it never stretches vertically regardless of whether the card is wide or tall.
    readonly property real folderW: Math.round(Math.max(76, Math.min(root.width * 0.54, 108)))
    readonly property real folderH: Math.round(folderW * 0.68)

    // Everything drifts slightly on hover for a subtle parallax
    Item {
        id: art

        anchors.fill: parent
        transformOrigin: Item.Center
        scale: root.hovered ? 1.06 : 1.0
        x: root.hovered ? -4 : 0

        Behavior on scale { NumberAnimation { duration: 500; easing.type: Easing.OutCubic } }
        Behavior on x { NumberAnimation { duration: 500; easing.type: Easing.OutCubic } }

        // ---- 0: Bokeh ----------------------------------------------------------------
        Item {
            visible: !root.isFolder && root.composition === 0
            anchors.fill: parent

            Rectangle {
                width: root.height * 1.15
                height: width
                radius: width / 2
                x: root.width * 0.52
                y: -root.height * 0.3
                color: "#0DFFFFFF"
            }

            Rectangle {
                width: root.height * 0.75
                height: width
                radius: width / 2
                x: root.width * 0.68
                y: root.height * 0.42
                color: "#265865F2"
            }

            Rectangle {
                width: root.height * 0.34
                height: width
                radius: width / 2
                x: root.width * 0.40
                y: root.height * 0.1
                color: "#0AFFFFFF"
            }
        }

        // ---- 1: Diagonal stripes -----------------------------------------------------
        Item {
            visible: !root.isFolder && root.composition === 1
            anchors.fill: parent

            Repeater {
                model: (!root.isFolder && root.composition === 1) ? 7 : 0

                delegate: Rectangle {
                    required property int index

                    width: root.height * 2.6
                    height: (index % 2 === 0) ? 10 : 4
                    rotation: -32
                    x: root.width * 0.72 + (index - 3) * 12.7 - width / 2
                    y: root.height * 0.5 + (index - 3) * 20.4 - height / 2
                    color: "#0DFFFFFF"
                }
            }
        }

        // ---- 2: Concentric rings -----------------------------------------------------
        Item {
            visible: !root.isFolder && root.composition === 2
            anchors.fill: parent

            Repeater {
                model: (!root.isFolder && root.composition === 2) ? 4 : 0

                delegate: Rectangle {
                    required property int index

                    width: root.height * (0.35 + index * 0.3)
                    height: width
                    radius: width / 2
                    x: root.width * 0.80 - width / 2
                    y: root.height * 0.42 - height / 2
                    color: "transparent"
                    border.width: 1
                    border.color: index === 0 ? "#4D5865F2" : "#16FFFFFF"
                }
            }
        }

        // ---- 3: Horizon / sliced sun -------------------------------------------------
        Item {
            visible: !root.isFolder && root.composition === 3
            anchors.fill: parent

            Rectangle {
                id: sun
                width: root.height * 0.66
                height: width
                radius: width / 2
                x: root.width * 0.72 - width / 2
                y: root.height * 0.42 - height / 2
                color: "#18FFFFFF"
            }

            Repeater {
                model: (!root.isFolder && root.composition === 3) ? 4 : 0

                delegate: Rectangle {
                    required property int index

                    x: sun.x - 2
                    width: sun.width + 4
                    height: 2 + index * 2
                    y: sun.y + sun.height * (0.5 + index * 0.13)
                    color: Theme.posterBottom(root.seed)
                }
            }
        }

        // ---- 4: Monolith -------------------------------------------------------------
        Item {
            visible: !root.isFolder && root.composition === 4
            anchors.fill: parent

            Rectangle {
                x: root.width * 0.70
                width: root.width * 0.18
                height: root.height
                color: "#0AFFFFFF"
            }

            Rectangle {
                x: root.width * 0.70
                width: 2
                height: root.height
                color: "#665865F2"
            }

            Rectangle {
                x: root.width * 0.52
                width: root.width * 0.05
                height: root.height
                color: "#06FFFFFF"
            }
        }

        // ---- 5: Dot grid -------------------------------------------------------------
        Item {
            visible: !root.isFolder && root.composition === 5
            anchors.fill: parent

            Repeater {
                model: (!root.isFolder && root.composition === 5) ? 30 : 0

                delegate: Rectangle {
                    required property int index

                    width: 4
                    height: 4
                    radius: 2
                    x: root.width * 0.58 + (index % 6) * 17
                    y: 18 + Math.floor(index / 6) * 17
                    color: ((index % 7) === 0) ? "#665865F2" : "#1AFFFFFF"
                }
            }
        }

        // ---- Giant outlined year / episode / initial ----------------------------------
        Text {
            visible: !root.isFolder && root.bigText.length > 0
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: -root.height * 0.1
            text: root.bigText
            color: "#0DFFFFFF"
            style: Text.Outline
            styleColor: "#2EFFFFFF"
            font.weight: Font.Black
            font.pixelSize: Math.round(root.height * (root.bigText.length <= 1 ? 0.95
                                                      : root.bigText.length <= 2 ? 0.78
                                                      : root.bigText.length <= 4 ? 0.5 : 0.36))
        }

        // ---- Folder: stacked sheets ---------------------------------------------------
        Item {
            visible: root.isFolder
            anchors.fill: parent

            // Back tilted folder sheet for visual depth
            Rectangle {
                width: Math.round(root.folderW * 0.94)
                height: root.folderH
                radius: 8
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.horizontalCenterOffset: 6
                y: folderBody.y - Math.round(root.folderH * 0.14)
                rotation: 4.5
                color: "#0EFFFFFF"
                border.width: 1
                border.color: "#18FFFFFF"
            }

            // Document sheet peeking from behind the folder tab
            Rectangle {
                width: Math.round(root.folderW * 0.78)
                height: Math.round(root.folderH * 0.58)
                radius: 5
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.horizontalCenterOffset: -2
                y: folderBody.y - Math.round(root.folderH * 0.20)
                color: "#14FFFFFF"
                border.width: 1
                border.color: "#24FFFFFF"
            }

            // Main front folder body
            Rectangle {
                id: folderBody

                width: root.folderW
                height: root.folderH
                radius: 8
                anchors.horizontalCenter: parent.horizontalCenter
                y: Math.round(root.height * 0.35 - root.folderH / 2)
                color: "#1CFFFFFF"
                border.width: 1
                border.color: "#30FFFFFF"

                // Folder tab (top-left)
                Rectangle {
                    x: 10
                    y: -Math.round(root.folderH * 0.16)
                    width: Math.round(root.folderW * 0.36)
                    height: Math.round(root.folderH * 0.22)
                    radius: 5
                    color: "#1CFFFFFF"
                    border.width: 1
                    border.color: "#30FFFFFF"
                }

                // Seamless blend cover for the tab's bottom border
                Rectangle {
                    x: 11
                    y: 0
                    width: Math.round(root.folderW * 0.36) - 2
                    height: 2
                    color: folderBody.color
                }

                // Centered folder icon vector
                FluxIcon {
                    name: "folder"
                    size: Math.round(root.folderH * 0.44)
                    color: "#5AFFFFFF"
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: 1
                }
            }
        }
    }
}
