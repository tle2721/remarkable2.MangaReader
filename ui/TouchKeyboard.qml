import QtQuick

Rectangle {
    id: kbRoot
    width: parent ? parent.width : 1404
    height: active ? (urlMode ? 434 : 368) : 0
    visible: active
    color: "#F4F4F4"
    clip: true

    property bool active: false
    property bool shifted: false
    property bool symbolsMode: false
    property bool urlMode: false
    property string submitLabel: "Search"

    signal keyTyped(string text)
    signal backspace()
    signal clearAll()
    signal submitted()
    signal dismissed()

    readonly property var rowsAlpha: [
        ["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"],
        ["a", "s", "d", "f", "g", "h", "j", "k", "l"],
        ["SHIFT", "z", "x", "c", "v", "b", "n", "m", "BKSP"]
    ]

    readonly property var rowsSymbols: [
        ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"],
        ["-", "_", "/", ":", ".", ",", "\"", "[", "]"],
        ["ABC", "{", "}", "(", ")", "?", "&", "=", "BKSP"]
    ]

    readonly property var urlTokens: [
        "https://",
        "raw.githubusercontent.com/",
        "/refs/heads/",
        "gh-pages/",
        "index.min.json",
        ".json",
        "/",
        "-",
        "_",
        "\"",
        "[",
        "]",
        "{",
        "}",
        ":",
        ","
    ]

    // Top divider border
    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 3
        color: "#000000"
    }

    Timer {
        id: bkspRepeat
        interval: 95
        repeat: true
        onTriggered: () => kbRoot.backspace()
    }

    Column {
        anchors.fill: parent
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 10

        // Optional URL / JSON quick-insert token strip
        Flickable {
            visible: kbRoot.urlMode
            width: parent.width
            height: visible ? 56 : 0
            contentWidth: tokenRow.width
            contentHeight: 56
            flickableDirection: Flickable.HorizontalFlick
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: tokenRow
                spacing: 8

                Repeater {
                    model: kbRoot.urlTokens
                    delegate: Rectangle {
                        width: Math.max(58, tokTxt.implicitWidth + 26)
                        height: 52
                        color: tokArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            id: tokTxt
                            anchors.centerIn: parent
                            text: modelData
                            font.pixelSize: 20
                            font.bold: true
                            color: tokArea.pressed ? "#FFFFFF" : "#000000"
                        }

                        MouseArea {
                            id: tokArea
                            anchors.fill: parent
                            onClicked: (mouse) => kbRoot.keyTyped(modelData)
                        }
                    }
                }
            }
        }

        Repeater {
            model: 3
            delegate: Row {
                id: keyRow
                readonly property int rowIndex: index
                readonly property var keysModel: kbRoot.symbolsMode ? kbRoot.rowsSymbols[keyRow.rowIndex] : kbRoot.rowsAlpha[keyRow.rowIndex]
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8

                Repeater {
                    model: keyRow.keysModel
                    delegate: Rectangle {
                        id: keyCell
                        readonly property string rawKey: modelData
                        readonly property bool isSpecial: keyCell.rawKey === "SHIFT" || keyCell.rawKey === "BKSP" || keyCell.rawKey === "ABC"
                        readonly property real baseKeyW: Math.floor((kbRoot.width - 28 - 9 * 8) / 10)
                        width: keyCell.isSpecial ? Math.floor(keyCell.baseKeyW * 1.45) : keyCell.baseKeyW
                        height: 76
                        color: (keyArea.pressed || (keyCell.rawKey === "SHIFT" && kbRoot.shifted)) ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        AppIcon {
                            visible: keyCell.rawKey === "SHIFT" || keyCell.rawKey === "BKSP"
                            anchors.centerIn: parent
                            name: keyCell.rawKey === "SHIFT" ? "shift" : "backspace"
                            width: 28
                            height: 28
                            color: (keyArea.pressed || (keyCell.rawKey === "SHIFT" && kbRoot.shifted)) ? "#FFFFFF" : "#000000"
                        }

                        Text {
                            visible: keyCell.rawKey !== "SHIFT" && keyCell.rawKey !== "BKSP"
                            anchors.centerIn: parent
                            font.pixelSize: keyCell.isSpecial ? 22 : 28
                            font.bold: true
                            color: (keyArea.pressed || (keyCell.rawKey === "SHIFT" && kbRoot.shifted)) ? "#FFFFFF" : "#000000"
                            text: {
                                if (keyCell.rawKey === "ABC") return "ABC";
                                return (!kbRoot.symbolsMode && kbRoot.shifted) ? keyCell.rawKey.toUpperCase() : keyCell.rawKey;
                            }
                        }

                        MouseArea {
                            id: keyArea
                            anchors.fill: parent
                            onPressed: (mouse) => {
                                if (keyCell.rawKey === "BKSP") {
                                    kbRoot.backspace();
                                    bkspRepeat.start();
                                }
                            }
                            onReleased: (mouse) => {
                                if (keyCell.rawKey === "BKSP") {
                                    bkspRepeat.stop();
                                }
                            }
                            onCanceled: () => {
                                bkspRepeat.stop();
                            }
                            onClicked: (mouse) => {
                                if (keyCell.rawKey === "BKSP") return;
                                if (keyCell.rawKey === "SHIFT") {
                                    kbRoot.shifted = !kbRoot.shifted;
                                    return;
                                }
                                if (keyCell.rawKey === "ABC") {
                                    kbRoot.symbolsMode = false;
                                    return;
                                }
                                let out = (!kbRoot.symbolsMode && kbRoot.shifted) ? keyCell.rawKey.toUpperCase() : keyCell.rawKey;
                                kbRoot.keyTyped(out);
                                if (kbRoot.shifted) kbRoot.shifted = false;
                            }
                        }
                    }
                }
            }
        }

        // Bottom control row: ?123 | Clear | Space | Hide | Submit
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8

            // ?123 / ABC toggle
            Rectangle {
                width: 140
                height: 76
                color: symArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    text: kbRoot.symbolsMode ? "ABC" : "?123"
                    font.pixelSize: 22
                    font.bold: true
                    color: symArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: symArea
                    anchors.fill: parent
                    onClicked: (mouse) => { kbRoot.symbolsMode = !kbRoot.symbolsMode; }
                }
            }

            // Clear button
            Rectangle {
                width: 140
                height: 76
                color: clrArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    text: "Clear"
                    font.pixelSize: 22
                    font.bold: true
                    color: clrArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: clrArea
                    anchors.fill: parent
                    onClicked: (mouse) => kbRoot.clearAll()
                }
            }

            // Spacebar / Newline
            Rectangle {
                width: Math.max(340, kbRoot.width - 28 - 140 - 140 - 140 - 230 - 32)
                height: 76
                color: spcArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    text: "space"
                    font.pixelSize: 22
                    color: spcArea.pressed ? "#FFFFFF" : "#444444"
                }
                MouseArea {
                    id: spcArea
                    anchors.fill: parent
                    onClicked: (mouse) => kbRoot.keyTyped(" ")
                }
            }

            // Hide keyboard
            Rectangle {
                width: 140
                height: 76
                color: hideArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                IconLabel {
                    anchors.centerIn: parent
                    text: "Hide"
                    rightIcon: "chevron-down"
                    pixelSize: 22
                    spacing: 6
                    color: hideArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: hideArea
                    anchors.fill: parent
                    onClicked: (mouse) => kbRoot.dismissed()
                }
            }

            // Primary Submit CTA
            Rectangle {
                width: 230
                height: 76
                color: submitArea.pressed ? "#FFFFFF" : "#000000"
                border.width: 2
                border.color: "#000000"

                IconLabel {
                    anchors.centerIn: parent
                    text: kbRoot.submitLabel
                    rightIcon: "enter"
                    pixelSize: 23
                    color: submitArea.pressed ? "#000000" : "#FFFFFF"
                }
                MouseArea {
                    id: submitArea
                    anchors.fill: parent
                    onClicked: (mouse) => kbRoot.submitted()
                }
            }
        }
    }
}
