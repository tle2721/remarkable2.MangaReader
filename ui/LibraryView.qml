import QtQuick

Item {
    id: libRoot
    property var libraryItems: []
    property var continueItem: null
    property string filterMode: "all" // "all" | "unread" | "downloaded"

    signal openManga(var mangaObj)
    signal resumeReading(var heroObj)
    signal syncRequested(bool deepRefresh)
    signal jumpToBrowse()

    readonly property var filteredLibrary: {
        let res = [];
        if (!libraryItems) return res;
        for (let i = 0; i < libraryItems.length; ++i) {
            let it = libraryItems[i];
            if (filterMode === "unread" && !(it.unread_count > 0)) continue;
            if (filterMode === "downloaded" && !(it.downloaded_count > 0)) continue;
            res.push(it);
        }
        return res;
    }

    function formatChapterLabel(obj) {
        if (!obj) return "";
        let parts = [];
        if (obj.last_chapter_num !== undefined && obj.last_chapter_num !== null) {
            parts.push("Chapter " + obj.last_chapter_num);
        } else if (obj.last_chapter_title) {
            parts.push(obj.last_chapter_title);
        } else {
            parts.push("Ready to read");
        }
        if (obj.last_page_count > 0) {
            parts.push("Page " + ((obj.last_page_index || 0) + 1) + " of " + obj.last_page_count);
        }
        return parts.join("  ·  ");
    }

    Flickable {
        id: libScroll
        anchors.fill: parent
        contentWidth: width
        contentHeight: contentCol.height + 48
        flickableDirection: Flickable.VerticalFlick
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: contentCol
            x: 28
            y: 22
            width: parent.width - 56
            spacing: 24

            // 1. Continue Reading Hero Banner
            Rectangle {
                id: heroCard
                visible: libRoot.continueItem !== null && libRoot.continueItem !== undefined &&
                         libRoot.continueItem.manga_id !== undefined && libRoot.continueItem.manga_id !== ""
                width: parent.width
                height: visible ? 196 : 0
                color: "#FFFFFF"
                border.width: 3
                border.color: "#000000"

                // Subtle solid offset accent bar at left
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 12
                    color: "#000000"
                }

                // Cover thumbnail
                Rectangle {
                    id: heroCoverBox
                    anchors.left: parent.left
                    anchors.leftMargin: 28
                    anchors.verticalCenter: parent.verticalCenter
                    width: 112
                    height: 156
                    color: "#F0F0F0"
                    border.width: 2
                    border.color: "#000000"
                    clip: true

                    Text {
                        anchors.centerIn: parent
                        text: (libRoot.continueItem && libRoot.continueItem.title)
                              ? libRoot.continueItem.title.slice(0, 2).toUpperCase() : "M"
                        font.pixelSize: 38
                        font.bold: true
                        color: "#000000"
                        visible: heroCoverImg.status !== Image.Ready
                    }

                    Image {
                        id: heroCoverImg
                        anchors.fill: parent
                        source: (libRoot.continueItem && libRoot.continueItem.cover_path) ? libRoot.continueItem.cover_path : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                    }
                }

                // Hero text info (tapping opens Manga details)
                Column {
                    anchors.left: heroCoverBox.right
                    anchors.leftMargin: 24
                    anchors.right: resumeBtn.left
                    anchors.rightMargin: 24
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    Text {
                        text: "CONTINUE READING"
                        font.pixelSize: 18
                        font.bold: true
                        font.letterSpacing: 1.5
                        color: "#444444"
                    }

                    Text {
                        width: parent.width
                        text: (libRoot.continueItem && libRoot.continueItem.title) ? libRoot.continueItem.title : ""
                        font.pixelSize: 30
                        font.bold: true
                        color: "#000000"
                        elide: Text.ElideRight
                        maximumLineCount: 2
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        width: parent.width
                        text: libRoot.formatChapterLabel(libRoot.continueItem)
                        font.pixelSize: 22
                        color: "#222222"
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text: (libRoot.continueItem && libRoot.continueItem.source_name)
                              ? libRoot.continueItem.source_name : ""
                        font.pixelSize: 19
                        color: "#555555"
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.right: resumeBtn.left
                    anchors.rightMargin: 12
                    onClicked: (mouse) => {
                        if (libRoot.continueItem) {
                            libRoot.openManga(libRoot.continueItem);
                        }
                    }
                }

                // 1-Tap Resume Button
                Rectangle {
                    id: resumeBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 24
                    anchors.verticalCenter: parent.verticalCenter
                    width: 236
                    height: 92
                    color: resumeBtnArea.pressed ? "#FFFFFF" : "#000000"
                    border.width: 3
                    border.color: "#000000"

                    Text {
                        anchors.centerIn: parent
                        text: "Resume  →"
                        font.pixelSize: 26
                        font.bold: true
                        color: resumeBtnArea.pressed ? "#000000" : "#FFFFFF"
                    }

                    MouseArea {
                        id: resumeBtnArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            if (libRoot.continueItem) {
                                libRoot.resumeReading(libRoot.continueItem);
                            }
                        }
                    }
                }
            }

            // 2. Filter & Sync Controls Bar
            Item {
                width: parent.width
                height: 68

                Row {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12

                    Repeater {
                        model: [
                            { id: "all", label: "All (" + (libRoot.libraryItems ? libRoot.libraryItems.length : 0) + ")" },
                            { id: "unread", label: "Unread" },
                            { id: "downloaded", label: "Downloaded" }
                        ]
                        delegate: Rectangle {
                            id: libFilterPill
                            readonly property bool selected: libRoot.filterMode === modelData.id
                            width: pillTxt.implicitWidth + 38
                            height: 58
                            color: (libFilterPill.selected || pillArea.pressed) ? "#000000" : "#FFFFFF"
                            border.width: 2
                            border.color: "#000000"

                            Text {
                                id: pillTxt
                                anchors.centerIn: parent
                                text: modelData.label
                                font.pixelSize: 22
                                font.bold: libFilterPill.selected
                                color: (libFilterPill.selected || pillArea.pressed) ? "#FFFFFF" : "#000000"
                            }

                            MouseArea {
                                id: pillArea
                                anchors.fill: parent
                                onClicked: (mouse) => { libRoot.filterMode = modelData.id; }
                            }
                        }
                    }
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12

                    Rectangle {
                        width: 154
                        height: 58
                        color: syncArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        IconLabel {
                            anchors.centerIn: parent
                            icon: "refresh"
                            text: "Sync"
                            pixelSize: 22
                            color: syncArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: syncArea
                            anchors.fill: parent
                            onClicked: (mouse) => libRoot.syncRequested(false)
                        }
                    }

                    Rectangle {
                        width: 196
                        height: 58
                        color: updArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        IconLabel {
                            anchors.centerIn: parent
                            icon: "sparkle"
                            text: "Updates"
                            pixelSize: 22
                            color: updArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: updArea
                            anchors.fill: parent
                            onClicked: (mouse) => libRoot.syncRequested(true)
                        }
                    }
                }
            }

            // 3. Empty Library State
            Rectangle {
                visible: libRoot.filteredLibrary.length === 0
                width: parent.width
                height: 360
                color: "#F8F8F8"
                border.width: 2
                border.color: "#000000"

                Column {
                    anchors.centerIn: parent
                    spacing: 18

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: libRoot.filterMode === "all"
                              ? "Your Library is Empty"
                              : "No matching manga in this filter"
                        font.pixelSize: 30
                        font.bold: true
                        color: "#000000"
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "Configure a source list and install Aidoku sources in Browse to add series."
                        font.pixelSize: 22
                        color: "#444444"
                    }

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 280
                        height: 76
                        color: emptyBrowseArea.pressed ? "#FFFFFF" : "#000000"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: "Browse Manga  →"
                            font.pixelSize: 24
                            font.bold: true
                            color: emptyBrowseArea.pressed ? "#000000" : "#FFFFFF"
                        }
                        MouseArea {
                            id: emptyBrowseArea
                            anchors.fill: parent
                            onClicked: (mouse) => libRoot.jumpToBrowse()
                        }
                    }
                }
            }

            // 4. 3-Column Mihon-Style Cover Grid
            Grid {
                id: coverGrid
                visible: libRoot.filteredLibrary.length > 0
                width: parent.width
                columns: 3
                columnSpacing: 24
                rowSpacing: 28

                readonly property real cardWidth: Math.floor((width - 48) / 3)

                Repeater {
                    model: libRoot.filteredLibrary
                    delegate: Item {
                        width: coverGrid.cardWidth
                        height: coverFrame.height + 98

                        Rectangle {
                            id: coverFrame
                            width: parent.width
                            height: Math.floor(parent.width * 1.38)
                            color: cardArea.pressed ? "#E0E0E0" : "#F4F4F4"
                            border.width: 2
                            border.color: "#000000"
                            clip: true

                            // Stylized tankobon fallback cover when cover art isn't cached yet
                            Column {
                                anchors.centerIn: parent
                                width: parent.width - 40
                                spacing: 14
                                visible: coverImg.status !== Image.Ready

                                Rectangle {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: 84
                                    height: 84
                                    color: "#FFFFFF"
                                    border.width: 2
                                    border.color: "#000000"

                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.title ? modelData.title.slice(0, 1).toUpperCase() : "M"
                                        font.pixelSize: 44
                                        font.bold: true
                                        color: "#000000"
                                    }
                                }

                                Text {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    text: modelData.title || ""
                                    font.pixelSize: 24
                                    font.bold: true
                                    color: "#000000"
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 4
                                    elide: Text.ElideRight
                                }
                            }

                            Image {
                                id: coverImg
                                anchors.fill: parent
                                anchors.margins: 2
                                source: modelData.cover_path ? modelData.cover_path : ""
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: true
                            }

                            // Top-left Unread Count Badge
                            Rectangle {
                                visible: (modelData.unread_count || 0) > 0
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.margins: 10
                                width: unreadTxt.implicitWidth + 22
                                height: 40
                                color: "#000000"
                                border.width: 2
                                border.color: "#FFFFFF"

                                Text {
                                    id: unreadTxt
                                    anchors.centerIn: parent
                                    text: String(modelData.unread_count || 0)
                                    font.pixelSize: 20
                                    font.bold: true
                                    color: "#FFFFFF"
                                }
                            }

                            // Top-right Downloaded Count Badge
                            Rectangle {
                                visible: (modelData.downloaded_count || 0) > 0
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 10
                                width: dlTxt.implicitWidth + 22
                                height: 40
                                color: "#FFFFFF"
                                border.width: 2
                                border.color: "#000000"

                                IconLabel {
                                    id: dlTxt
                                    anchors.centerIn: parent
                                    icon: "check"
                                    text: String(modelData.downloaded_count)
                                    pixelSize: 19
                                    spacing: 5
                                    color: "#000000"
                                }
                            }
                        }

                        Text {
                            id: titleLabel
                            anchors.top: coverFrame.bottom
                            anchors.topMargin: 10
                            width: parent.width
                            text: modelData.title || ""
                            font.pixelSize: 24
                            font.bold: true
                            color: "#000000"
                            maximumLineCount: 2
                            wrapMode: Text.WordWrap
                            elide: Text.ElideRight
                        }

                        Text {
                            anchors.top: titleLabel.bottom
                            anchors.topMargin: 4
                            width: parent.width
                            text: modelData.source_name || modelData.source_id || ""
                            font.pixelSize: 19
                            color: "#555555"
                            elide: Text.ElideRight
                        }

                        MouseArea {
                            id: cardArea
                            anchors.fill: parent
                            onClicked: (mouse) => libRoot.openManga(modelData)
                        }
                    }
                }
            }
        }
    }
}
