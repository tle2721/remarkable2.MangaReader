import QtQuick

Rectangle {
    id: detailRoot
    color: "#FFFFFF"

    property string sourceId: ""
    property string sourceName: ""
    property string mangaId: ""
    property string mangaTitle: ""
    property string author: ""
    property string description: ""
    property string statusText: ""
    property string coverPath: ""
    property string coverUrl: ""
    property bool inLibrary: false
    property var chapters: []
    property string resumeChapterId: ""
    property int resumePage: 0
    property bool loading: false
    property string errorMessage: ""

    property bool sortDescending: true
    property string chapterFilter: "all" // "all" | "unread" | "downloaded"

    signal backRequested()
    signal refreshRequested()
    signal toggleLibraryRequested(bool add)
    signal openChapterRequested(var chapterObj)
    signal downloadChaptersRequested(var chapterList)
    signal toggleChapterReadRequested(var chapterObj, bool markRead)

    readonly property var chronologicalChapters: {
        if (!chapters || chapters.length === 0) return [];
        let arr = chapters.slice();
        let anyNum = false;
        for (let i = 0; i < arr.length; ++i) {
            if (arr[i].chapter_num !== undefined && arr[i].chapter_num !== null) {
                anyNum = true;
                break;
            }
        }
        if (anyNum) {
            arr.sort(function(a, b) {
                let na = (a.chapter_num !== undefined && a.chapter_num !== null) ? Number(a.chapter_num) : -1;
                let nb = (b.chapter_num !== undefined && b.chapter_num !== null) ? Number(b.chapter_num) : -1;
                if (Math.abs(na - nb) > 0.0001) return na - nb;
                return (Number(a.volume_num || 0) - Number(b.volume_num || 0));
            });
        }
        return arr;
    }

    readonly property var primaryActionChapter: {
        let asc = chronologicalChapters;
        if (asc.length === 0) return null;

        if (resumeChapterId && resumeChapterId.length > 0) {
            for (let i = 0; i < asc.length; ++i) {
                if (asc[i].id === resumeChapterId && !asc[i].read) {
                    return asc[i];
                }
            }
        }
        for (let j = 0; j < asc.length; ++j) {
            if (!asc[j].read) return asc[j];
        }
        return asc[asc.length - 1];
    }

    readonly property var displayedChapters: {
        let base = chronologicalChapters.slice();
        if (sortDescending) base.reverse();
        let out = [];
        for (let i = 0; i < base.length; ++i) {
            let c = base[i];
            if (chapterFilter === "unread" && c.read) continue;
            if (chapterFilter === "downloaded" && !c.downloaded) continue;
            out.push(c);
        }
        return out;
    }

    function queueUnreadDownloads(maxCount) {
        let asc = chronologicalChapters;
        let batch = [];
        let seenNums = {};
        for (let i = 0; i < asc.length; ++i) {
            let c = asc[i];
            if (c.read || c.downloaded) continue;
            let numKey = (c.chapter_num !== undefined && c.chapter_num !== null)
                         ? String(c.chapter_num) : c.id;
            if (seenNums[numKey]) continue;
            seenNums[numKey] = true;
            batch.push({
                source_id: detailRoot.sourceId,
                source_name: detailRoot.sourceName,
                manga_id: detailRoot.mangaId,
                manga_title: detailRoot.mangaTitle,
                chapter_id: c.id,
                chapter_num: c.chapter_num,
                chapter_title: c.title || ""
            });
            if (maxCount > 0 && batch.length >= maxCount) break;
        }
        if (batch.length > 0) {
            detailRoot.downloadChaptersRequested(batch);
        }
    }

    function formatChapterName(ch) {
        if (!ch) return "";
        let label = "";
        if (ch.volume_num !== undefined && ch.volume_num !== null && Number(ch.volume_num) > 0) {
            label += "Vol. " + ch.volume_num + " ";
        }
        if (ch.chapter_num !== undefined && ch.chapter_num !== null) {
            label += "Ch. " + ch.chapter_num;
        }
        if (ch.title && ch.title.length > 0) {
            label += (label.length > 0 ? " — " : "") + ch.title;
        }
        if (label.length === 0) label = "Chapter";
        return label;
    }

    // 1. Top Toolbar
    Rectangle {
        id: detailTopBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 84
        color: "#FFFFFF"

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 2
            color: "#000000"
        }

        Rectangle {
            id: backBtn
            anchors.left: parent.left
            anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            width: 154
            height: 58
            color: backArea.pressed ? "#000000" : "#FFFFFF"
            border.width: 2
            border.color: "#000000"

            Text {
                anchors.centerIn: parent
                text: "← Back"
                font.pixelSize: 23
                font.bold: true
                color: backArea.pressed ? "#FFFFFF" : "#000000"
            }
            MouseArea {
                id: backArea
                anchors.fill: parent
                onClicked: (mouse) => detailRoot.backRequested()
            }
        }

        Text {
            anchors.left: backBtn.right
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: detailRoot.sourceName || detailRoot.sourceId
            font.pixelSize: 22
            font.bold: true
            color: "#444444"
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Rectangle {
                width: 158
                height: 58
                color: dl5Area.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                IconLabel {
                    anchors.centerIn: parent
                    icon: "download"
                    text: "Next 5"
                    pixelSize: 20
                    color: dl5Area.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: dl5Area
                    anchors.fill: parent
                    onClicked: (mouse) => detailRoot.queueUnreadDownloads(5)
                }
            }

            Rectangle {
                width: 176
                height: 58
                color: dlAllArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                IconLabel {
                    anchors.centerIn: parent
                    icon: "download"
                    text: "All Unread"
                    pixelSize: 20
                    color: dlAllArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: dlAllArea
                    anchors.fill: parent
                    onClicked: (mouse) => detailRoot.queueUnreadDownloads(0)
                }
            }

            Rectangle {
                width: 164
                height: 58
                color: refArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                IconLabel {
                    anchors.centerIn: parent
                    icon: detailRoot.loading ? "" : "refresh"
                    text: detailRoot.loading ? "Loading..." : "Refresh"
                    pixelSize: 20
                    color: refArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: refArea
                    anchors.fill: parent
                    onClicked: (mouse) => detailRoot.refreshRequested()
                }
            }
        }
    }

    // 2. Manga Header Card + Controls
    Item {
        id: headerBlock
        anchors.top: detailTopBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 24
        height: 368

        // Cover Frame
        Rectangle {
            id: coverBox
            anchors.left: parent.left
            anchors.top: parent.top
            width: 200
            height: 278
            color: "#F4F4F4"
            border.width: 2
            border.color: "#000000"
            clip: true

            Text {
                anchors.centerIn: parent
                text: detailRoot.mangaTitle ? detailRoot.mangaTitle.slice(0, 1).toUpperCase() : "M"
                font.pixelSize: 64
                font.bold: true
                color: "#000000"
                visible: detailCoverImg.status !== Image.Ready
            }

            Image {
                id: detailCoverImg
                anchors.fill: parent
                anchors.margins: 2
                source: detailRoot.coverPath ? detailRoot.coverPath : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
            }
        }

        // Metadata + Primary Actions
        Column {
            anchors.left: coverBox.right
            anchors.leftMargin: 26
            anchors.right: parent.right
            anchors.top: parent.top
            spacing: 10

            Text {
                width: parent.width
                text: detailRoot.mangaTitle || ""
                font.pixelSize: 32
                font.bold: true
                color: "#000000"
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            Text {
                visible: (detailRoot.author && detailRoot.author.length > 0) ||
                         (detailRoot.statusText && detailRoot.statusText.length > 0)
                width: parent.width
                text: {
                    let s = [];
                    if (detailRoot.author) s.push(detailRoot.author);
                    if (detailRoot.statusText) s.push(detailRoot.statusText.toUpperCase());
                    return s.join("  ·  ");
                }
                font.pixelSize: 21
                color: "#444444"
                elide: Text.ElideRight
            }

            Text {
                visible: detailRoot.description && detailRoot.description.length > 0
                width: parent.width
                text: detailRoot.description
                font.pixelSize: 20
                color: "#333333"
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }

            Item { width: 1; height: 6 }

            // Action Buttons: Start/Continue + Library Toggle
            Row {
                spacing: 14

                Rectangle {
                    width: 360
                    height: 72
                    enabled: detailRoot.primaryActionChapter !== null
                    color: startReadArea.pressed ? "#FFFFFF" : "#000000"
                    border.width: 2
                    border.color: "#000000"
                    opacity: enabled ? 1.0 : 0.45

                    IconLabel {
                        anchors.centerIn: parent
                        icon: detailRoot.primaryActionChapter ? "play" : ""
                        pixelSize: 23
                        color: startReadArea.pressed ? "#000000" : "#FFFFFF"
                        text: {
                            let ch = detailRoot.primaryActionChapter;
                            if (!ch) return detailRoot.loading ? "Loading Chapters..." : "No Chapters";
                            let numStr = (ch.chapter_num !== undefined && ch.chapter_num !== null)
                                         ? ("Ch. " + ch.chapter_num) : "Chapter";
                            if (ch.id === detailRoot.resumeChapterId && detailRoot.resumePage > 0) {
                                return "Continue " + numStr + " (p. " + (detailRoot.resumePage + 1) + ")";
                            }
                            return "Read " + numStr;
                        }
                    }

                    MouseArea {
                        id: startReadArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            if (detailRoot.primaryActionChapter) {
                                detailRoot.openChapterRequested(detailRoot.primaryActionChapter);
                            }
                        }
                    }
                }

                Rectangle {
                    width: 224
                    height: 72
                    color: (detailRoot.inLibrary || libToggleArea.pressed) ? "#000000" : "#FFFFFF"
                    border.width: 2
                    border.color: "#000000"

                    IconLabel {
                        anchors.centerIn: parent
                        icon: detailRoot.inLibrary ? "check" : ""
                        text: detailRoot.inLibrary ? "In Library" : "+ Add Library"
                        pixelSize: 22
                        color: (detailRoot.inLibrary || libToggleArea.pressed) ? "#FFFFFF" : "#000000"
                    }

                    MouseArea {
                        id: libToggleArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            detailRoot.toggleLibraryRequested(!detailRoot.inLibrary);
                        }
                    }
                }
            }
        }

        // Chapter Filter & Sort Strip at bottom of headerBlock
        Item {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 64

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 2
                color: "#000000"
            }

            Row {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                spacing: 10

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: detailRoot.displayedChapters.length + " Chapters  "
                    font.pixelSize: 23
                    font.bold: true
                    color: "#000000"
                }

                Repeater {
                    model: [
                        { id: "all", label: "All" },
                        { id: "unread", label: "Unread" },
                        { id: "downloaded", label: "Offline" }
                    ]
                    delegate: Rectangle {
                        id: chFilterPill
                        readonly property bool sel: detailRoot.chapterFilter === modelData.id
                        width: cfTxt.implicitWidth + 30
                        height: 48
                        color: (chFilterPill.sel || cfArea.pressed) ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            id: cfTxt
                            anchors.centerIn: parent
                            text: modelData.label
                            font.pixelSize: 19
                            font.bold: chFilterPill.sel
                            color: (chFilterPill.sel || cfArea.pressed) ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: cfArea
                            anchors.fill: parent
                            onClicked: (mouse) => { detailRoot.chapterFilter = modelData.id; }
                        }
                    }
                }
            }

            Rectangle {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 190
                height: 48
                color: sortArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    text: detailRoot.sortDescending ? "Sort: Newest ↓" : "Sort: Oldest ↑"
                    font.pixelSize: 19
                    font.bold: true
                    color: sortArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: sortArea
                    anchors.fill: parent
                    onClicked: (mouse) => { detailRoot.sortDescending = !detailRoot.sortDescending; }
                }
            }
        }
    }

    // 3. Chapter List
    Rectangle {
        visible: detailRoot.loading && detailRoot.displayedChapters.length === 0
        anchors.top: headerBlock.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 24
        height: 180
        color: "#F8F8F8"
        border.width: 2
        border.color: "#000000"

        Text {
            anchors.centerIn: parent
            text: "Fetching chapter list..."
            font.pixelSize: 24
            font.bold: true
            color: "#000000"
        }
    }

    Rectangle {
        visible: !detailRoot.loading && detailRoot.errorMessage.length > 0 && detailRoot.displayedChapters.length === 0
        anchors.top: headerBlock.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 24
        height: 180
        color: "#F8F8F8"
        border.width: 2
        border.color: "#000000"

        Text {
            anchors.centerIn: parent
            width: parent.width - 48
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Could not load chapters: " + detailRoot.errorMessage
            font.pixelSize: 22
            color: "#000000"
        }
    }

    ListView {
        id: chapterListView
        visible: detailRoot.displayedChapters.length > 0
        anchors.top: headerBlock.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        anchors.bottomMargin: 12
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: detailRoot.displayedChapters

        delegate: Rectangle {
            width: chapterListView.width
            height: 92
            color: chRowArea.pressed ? "#EAEAEA" : "#FFFFFF"

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: "#BBBBBB"
            }

            // Unread dot indicator
            Rectangle {
                id: unreadDot
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                radius: 6
                color: modelData.read ? "transparent" : "#000000"
                border.width: modelData.read ? 1 : 0
                border.color: "#888888"
            }

            Column {
                anchors.left: unreadDot.right
                anchors.leftMargin: 18
                anchors.right: chActionsRow.left
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                Text {
                    width: parent.width
                    text: detailRoot.formatChapterName(modelData)
                    font.pixelSize: 24
                    font.bold: !modelData.read
                    color: modelData.read ? "#666666" : "#000000"
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    font.pixelSize: 19
                    color: "#666666"
                    elide: Text.ElideRight
                    text: {
                        let meta = [];
                        if (modelData.scanlator) meta.push(modelData.scanlator);
                        if ((modelData.saved_page || 0) > 0 && (modelData.page_count || 0) > 0) {
                            meta.push("Page " + (modelData.saved_page + 1) + " / " + modelData.page_count);
                        }
                        if (modelData.read) meta.push("Read");
                        return meta.length > 0 ? meta.join("  ·  ") : "Tap to read";
                    }
                }
            }

            MouseArea {
                id: chRowArea
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: chActionsRow.left
                onClicked: (mouse) => detailRoot.openChapterRequested(modelData)
            }

            Row {
                id: chActionsRow
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                // Mark Read / Unread toggle button
                Rectangle {
                    width: 58
                    height: 58
                    color: markArea.pressed ? "#000000" : "#FFFFFF"
                    border.width: 2
                    border.color: "#000000"

                    AppIcon {
                        anchors.centerIn: parent
                        name: modelData.read ? "check" : "circle"
                        width: 22
                        height: 22
                        color: markArea.pressed ? "#FFFFFF" : "#000000"
                    }
                    MouseArea {
                        id: markArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            detailRoot.toggleChapterReadRequested(modelData, !modelData.read);
                        }
                    }
                }

                // Download / Offline status button
                Rectangle {
                    width: modelData.downloaded ? 118 : 66
                    height: 58
                    color: modelData.downloaded
                           ? "#F0F0F0"
                           : (dlSingleArea.pressed ? "#000000" : "#FFFFFF")
                    border.width: 2
                    border.color: "#000000"

                    IconLabel {
                        anchors.centerIn: parent
                        icon: modelData.downloaded ? "check" : "download"
                        text: modelData.downloaded ? "Saved" : ""
                        pixelSize: modelData.downloaded ? 18 : 24
                        spacing: 6
                        color: (!modelData.downloaded && dlSingleArea.pressed) ? "#FFFFFF" : "#000000"
                    }

                    MouseArea {
                        id: dlSingleArea
                        anchors.fill: parent
                        enabled: !modelData.downloaded
                        onClicked: (mouse) => {
                            detailRoot.downloadChaptersRequested([{
                                source_id: detailRoot.sourceId,
                                source_name: detailRoot.sourceName,
                                manga_id: detailRoot.mangaId,
                                manga_title: detailRoot.mangaTitle,
                                chapter_id: modelData.id,
                                chapter_num: modelData.chapter_num,
                                chapter_title: modelData.title || ""
                            }]);
                        }
                    }
                }
            }
        }
    }
}
