import QtQuick

Item {
    id: dlRoot

    property var queueItems: []
    property var downloadedItems: []
    property real cacheBytes: 0

    signal openDownloadedChapter(var dlObj)
    signal cancelDownload(string key, bool cancelAll)
    signal deleteDownload(var dlObj)
    signal clearTempCache()

    function formatSize(bytes) {
        if (!bytes || bytes <= 0) return "0 MB";
        let mb = bytes / (1024 * 1024);
        if (mb < 1) return Math.round(bytes / 1024) + " KB";
        return mb.toFixed(1) + " MB";
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: dlCol.height + 48
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: dlCol
            x: 28
            y: 24
            width: parent.width - 56
            spacing: 24

            // 1. Active Queue Section
            Column {
                visible: dlRoot.queueItems && dlRoot.queueItems.length > 0
                width: parent.width
                spacing: 14

                Item {
                    width: parent.width
                    height: 60

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Download Queue (" + (dlRoot.queueItems ? dlRoot.queueItems.length : 0) + ")"
                        font.pixelSize: 28
                        font.bold: true
                        color: "#000000"
                    }

                    Rectangle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 176
                        height: 56
                        color: cancelAllArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: "Cancel All"
                            font.pixelSize: 21
                            font.bold: true
                            color: cancelAllArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: cancelAllArea
                            anchors.fill: parent
                            onClicked: (mouse) => dlRoot.cancelDownload("", true)
                        }
                    }
                }

                Repeater {
                    model: dlRoot.queueItems
                    delegate: Rectangle {
                        width: dlCol.width
                        height: 118
                        color: "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 20
                            anchors.right: qCancelBtn.left
                            anchors.rightMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8

                            Text {
                                width: parent.width
                                text: (modelData.manga_title || "") + "  —  " +
                                      (modelData.chapter_num !== undefined ? ("Ch. " + modelData.chapter_num) : (modelData.chapter_title || "Chapter"))
                                font.pixelSize: 24
                                font.bold: true
                                color: "#000000"
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width
                                text: modelData.status || "Queued"
                                font.pixelSize: 20
                                color: "#444444"
                                elide: Text.ElideRight
                            }

                            // Crisp monochrome progress bar
                            Rectangle {
                                width: parent.width
                                height: 14
                                color: "#F0F0F0"
                                border.width: 2
                                border.color: "#000000"

                                Rectangle {
                                    width: Math.max(0, Math.min(parent.width, parent.width * ((modelData.progress_pct || 0) / 100.0)))
                                    height: parent.height
                                    color: "#000000"
                                }
                            }
                        }

                        Rectangle {
                            id: qCancelBtn
                            anchors.right: parent.right
                            anchors.rightMargin: 18
                            anchors.verticalCenter: parent.verticalCenter
                            width: 64
                            height: 64
                            color: qCancelArea.pressed ? "#000000" : "#FFFFFF"
                            border.width: 2
                            border.color: "#000000"

                            AppIcon {
                                anchors.centerIn: parent
                                name: "close"
                                width: 22
                                height: 22
                                color: qCancelArea.pressed ? "#FFFFFF" : "#000000"
                            }
                            MouseArea {
                                id: qCancelArea
                                anchors.fill: parent
                                onClicked: (mouse) => dlRoot.cancelDownload(modelData.key || "", false)
                            }
                        }
                    }
                }
            }

            // 2. Saved Offline Chapters Header
            Item {
                width: parent.width
                height: 64

                Column {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Text {
                        text: "Offline Chapters (" + (dlRoot.downloadedItems ? dlRoot.downloadedItems.length : 0) + ")"
                        font.pixelSize: 28
                        font.bold: true
                        color: "#000000"
                    }

                    Text {
                        text: "Storage & cache: " + dlRoot.formatSize(dlRoot.cacheBytes)
                        font.pixelSize: 19
                        color: "#555555"
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 230
                    height: 58
                    color: clrCacheArea.pressed ? "#000000" : "#FFFFFF"
                    border.width: 2
                    border.color: "#000000"

                    Text {
                        anchors.centerIn: parent
                        text: "Clear Temp Cache"
                        font.pixelSize: 20
                        font.bold: true
                        color: clrCacheArea.pressed ? "#FFFFFF" : "#000000"
                    }
                    MouseArea {
                        id: clrCacheArea
                        anchors.fill: parent
                        onClicked: (mouse) => dlRoot.clearTempCache()
                    }
                }
            }

            // Empty Offline State
            Rectangle {
                visible: !dlRoot.downloadedItems || dlRoot.downloadedItems.length === 0
                width: parent.width
                height: 250
                color: "#F8F8F8"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    text: "No downloaded chapters yet. Tap Download on any manga chapter to read offline."
                    font.pixelSize: 22
                    color: "#444444"
                }
            }

            // Offline Chapters List
            Column {
                visible: dlRoot.downloadedItems && dlRoot.downloadedItems.length > 0
                width: parent.width
                spacing: 12

                Repeater {
                    model: dlRoot.downloadedItems
                    delegate: Rectangle {
                        width: dlCol.width
                        height: 108
                        color: "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 22
                            anchors.right: dlRowBtns.left
                            anchors.rightMargin: 18
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6

                            Text {
                                width: parent.width
                                text: modelData.manga_title || modelData.manga_id || ""
                                font.pixelSize: 25
                                font.bold: true
                                color: "#000000"
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width
                                text: {
                                    let c = (modelData.chapter_num !== undefined && modelData.chapter_num !== null)
                                            ? ("Chapter " + modelData.chapter_num)
                                            : (modelData.chapter_title || "Chapter");
                                    if (modelData.chapter_title && modelData.chapter_num !== undefined) {
                                        c += " — " + modelData.chapter_title;
                                    }
                                    return c + "  ·  " + (modelData.source_name || modelData.source_id || "");
                                }
                                font.pixelSize: 20
                                color: "#444444"
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.right: dlRowBtns.left
                            onClicked: (mouse) => dlRoot.openDownloadedChapter(modelData)
                        }

                        Row {
                            id: dlRowBtns
                            anchors.right: parent.right
                            anchors.rightMargin: 18
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 10

                            Rectangle {
                                width: 146
                                height: 62
                                color: readDlArea.pressed ? "#FFFFFF" : "#000000"
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.centerIn: parent
                                    text: "Read →"
                                    font.pixelSize: 21
                                    font.bold: true
                                    color: readDlArea.pressed ? "#000000" : "#FFFFFF"
                                }
                                MouseArea {
                                    id: readDlArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => dlRoot.openDownloadedChapter(modelData)
                                }
                            }

                            Rectangle {
                                width: 116
                                height: 62
                                color: delDlArea.pressed ? "#000000" : "#FFFFFF"
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.centerIn: parent
                                    text: "Delete"
                                    font.pixelSize: 20
                                    font.bold: true
                                    color: delDlArea.pressed ? "#FFFFFF" : "#000000"
                                }
                                MouseArea {
                                    id: delDlArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => dlRoot.deleteDownload(modelData)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
