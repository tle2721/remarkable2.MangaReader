import QtQuick

FocusScope {
    id: readerRoot
    focus: visible

    property string sourceId: ""
    property string sourceName: ""
    property string mangaId: ""
    property string mangaTitle: ""
    property string chapterId: ""
    property var chapterNum: null
    property string chapterTitle: ""

    property var pages: []
    property int pageIndex: 0
    property var prevChapter: null
    property var nextChapter: null

    property bool loading: false
    property string loadingStatus: "Opening chapter..."
    property int loadingPct: 0
    property string errorMessage: ""

    // Settings synced with backend
    property string readingMode: "rtl"   // "rtl" | "ltr"
    property string fitMode: "contain"   // "contain" | "width" | "crop"
    property bool invertSwipe: false
    property int ghostRefreshPages: 20

    // In Manga (RTL), pages progress right-to-left [Next | Current | Prev], so dragging right (dx > 0)
    // pulls the left (Next) page into view unless the user flips invertSwipe.
    readonly property bool swipeRightIsNext: (readingMode === "rtl") !== invertSwipe

    // Internal UI state
    property bool hudVisible: false
    property bool zoomed: false
    property int turnsSinceRefresh: 0

    signal exitReader()
    signal saveProgress(int currentPage, int totalPages, bool completed)
    signal jumpChapter(var targetChapter, bool startAtEnd)
    signal settingsModified(string readingMode, string fitMode, bool invertSwipe)

    readonly property string currentPageUrl: (pages && pageIndex >= 0 && pageIndex < pages.length && pages[pageIndex]) ? String(pages[pageIndex]) : ""
    readonly property string nextPageUrl: (pages && pageIndex + 1 < pages.length && pages[pageIndex + 1]) ? String(pages[pageIndex + 1]) : ""
    readonly property string prevPageUrl: (pages && pageIndex - 1 >= 0 && pages[pageIndex - 1]) ? String(pages[pageIndex - 1]) : ""

    function triggerEinkGhostClear() {
        turnsSinceRefresh = 0;
        ghostFlashRect.color = "#000000";
        ghostFlashRect.visible = true;
        ghostStep1Timer.start();
    }

    function showChapterToast(text) {
        chapterBannerText.text = text;
        chapterBanner.visible = true;
        chapterBannerTimer.restart();
    }

    function resetPageView() {
        zoomed = false;
        pageFlick.contentX = 0;
        pageFlick.contentY = 0;
    }

    function goNextPage() {
        if (loading || !pages || pages.length === 0) return;

        if (pageIndex + 1 < pages.length) {
            pageIndex += 1;
            resetPageView();
            turnsSinceRefresh += 1;
            if (ghostRefreshPages > 0 && turnsSinceRefresh >= ghostRefreshPages) {
                triggerEinkGhostClear();
            }
            let reachedLast = (pageIndex === pages.length - 1);
            saveProgress(pageIndex, pages.length, reachedLast);
        } else {
            // Completed final page -> seamless transition to Next Chapter
            saveProgress(pageIndex, pages.length, true);
            if (nextChapter && nextChapter.id) {
                let lbl = (nextChapter.chapter_num !== undefined && nextChapter.chapter_num !== null)
                          ? ("Chapter " + nextChapter.chapter_num)
                          : (nextChapter.title || "Next Chapter");
                showChapterToast("Loading " + lbl + "...");
                jumpChapter(nextChapter, false);
            } else {
                showChapterToast("Reached end of latest available chapter");
                hudVisible = true;
            }
        }
    }

    function goPrevPage() {
        if (loading || !pages || pages.length === 0) return;

        if (pageIndex > 0) {
            pageIndex -= 1;
            resetPageView();
            saveProgress(pageIndex, pages.length, false);
        } else if (prevChapter && prevChapter.id) {
            let lbl = (prevChapter.chapter_num !== undefined && prevChapter.chapter_num !== null)
                      ? ("Chapter " + prevChapter.chapter_num)
                      : (prevChapter.title || "Previous Chapter");
            showChapterToast("Loading " + lbl + "...");
            jumpChapter(prevChapter, true);
        } else {
            showChapterToast("First page of chapter");
        }
    }

    function jumpToPage(targetIdx) {
        if (!pages || pages.length === 0) return;
        let clamped = Math.max(0, Math.min(pages.length - 1, targetIdx));
        if (clamped !== pageIndex) {
            pageIndex = clamped;
            resetPageView();
            saveProgress(pageIndex, pages.length, pageIndex === pages.length - 1);
        }
    }

    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Escape) {
            exitReader();
            event.accepted = true;
        } else if (event.key === Qt.Key_Left) {
            if (readingMode === "rtl") goNextPage(); else goPrevPage();
            event.accepted = true;
        } else if (event.key === Qt.Key_Right) {
            if (readingMode === "rtl") goPrevPage(); else goNextPage();
            event.accepted = true;
        } else if (event.key === Qt.Key_Space || event.key === Qt.Key_PageDown) {
            goNextPage();
            event.accepted = true;
        } else if (event.key === Qt.Key_PageUp) {
            goPrevPage();
            event.accepted = true;
        }
    }

    // Background paper surface
    Rectangle {
        anchors.fill: parent
        color: "#FFFFFF"
    }

    // Hidden neighbor page preloaders so page turns are instant in RAM
    Image {
        visible: false
        source: readerRoot.nextPageUrl
        asynchronous: true
        cache: true
    }
    Image {
        visible: false
        source: readerRoot.prevPageUrl
        asynchronous: true
        cache: true
    }

    // Main Pannable / Zoomable Page Canvas
    Flickable {
        id: pageFlick
        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: readerRoot.zoomed ? Flickable.HorizontalAndVerticalFlick : Flickable.VerticalFlick
        interactive: readerRoot.zoomed

        contentWidth: {
            if (readerRoot.zoomed) return width * 2.0;
            if (readerRoot.fitMode === "crop") return width * 1.07;
            return width;
        }
        contentHeight: {
            if (readerRoot.zoomed) return height * 2.0;
            if (readerRoot.fitMode === "width") {
                if (pageImage.implicitWidth > 0) {
                    return Math.max(height, Math.round(width * (pageImage.implicitHeight / pageImage.implicitWidth)));
                }
                return height;
            }
            if (readerRoot.fitMode === "crop") return height * 1.07;
            return height;
        }

        Image {
            id: pageImage
            width: pageFlick.contentWidth
            height: pageFlick.contentHeight
            x: readerRoot.fitMode === "crop" && !readerRoot.zoomed ? -Math.round(pageFlick.width * 0.035) : 0
            y: readerRoot.fitMode === "crop" && !readerRoot.zoomed ? -Math.round(pageFlick.height * 0.035) : 0
            source: readerRoot.currentPageUrl
            fillMode: readerRoot.fitMode === "width" ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            asynchronous: false
            cache: true
            smooth: true
        }
    }

    // Direct QTouchEvent + Stylus Gesture Layer (accepts raw finger touches on reMarkable 2 without Flickable stealing)
    MultiPointTouchArea {
        id: gestureOverlay
        anchors.fill: parent
        z: 10
        enabled: !readerRoot.loading && readerRoot.errorMessage.length === 0 && !readerRoot.zoomed
        mouseEnabled: true
        minimumTouchPoints: 1
        maximumTouchPoints: 2

        touchPoints: [
            TouchPoint { id: tp1 },
            TouchPoint { id: tp2 }
        ]

        property real startX: 0
        property real startY: 0
        property real lastX: 0
        property real lastY: 0
        property real maxMove: 0
        property real startContentY: 0
        property bool gestureDone: false

        function completeGesture(wasCanceled) {
            if (gestureDone) return;
            gestureDone = true;

            let dx = lastX - startX;
            let dy = lastY - startY;

            // Allow AppLoad's top-bezel system bar pull-down gesture to pass cleanly
            if (startY < 90 && dy > 35) {
                return;
            }

            // Horizontal swipe: 46px (~5mm on 226 DPI), or 65px if system gesture monitor canceled late
            let minSwipe = wasCanceled ? 65 : 46;
            if (Math.abs(dx) >= minSwipe && Math.abs(dx) > Math.abs(dy) * 1.2) {
                if (readerRoot.hudVisible) {
                    readerRoot.hudVisible = false;
                }
                let shouldGoNext = readerRoot.swipeRightIsNext ? (dx > 0) : (dx < 0);
                if (shouldGoNext) {
                    readerRoot.goNextPage();
                } else {
                    readerRoot.goPrevPage();
                }
                return;
            }

            if (wasCanceled) {
                return;
            }

            // Crisp single tap (< 22px finger travel; 22..46px is a deadzone so short drags never misfire opposite tap zones)
            if (maxMove < 22) {
                if (readerRoot.hudVisible) {
                    readerRoot.hudVisible = false;
                    return;
                }

                let ratio = startX / Math.max(1, width);
                if (ratio < 0.33) {
                    if (readerRoot.readingMode === "rtl") readerRoot.goNextPage();
                    else readerRoot.goPrevPage();
                } else if (ratio > 0.67) {
                    if (readerRoot.readingMode === "rtl") readerRoot.goPrevPage();
                    else readerRoot.goNextPage();
                } else {
                    readerRoot.hudVisible = true;
                }
            }
        }

        onPressed: (touchPoints) => {
            let pt = (touchPoints && touchPoints.length > 0) ? touchPoints[0] : tp1;
            startX = pt.x;
            startY = pt.y;
            lastX = pt.x;
            lastY = pt.y;
            maxMove = 0;
            startContentY = pageFlick.contentY;
            gestureDone = false;
        }

        onUpdated: (touchPoints) => {
            let pt = (touchPoints && touchPoints.length > 0) ? touchPoints[0] : tp1;
            if (!pt) return;
            lastX = pt.x;
            lastY = pt.y;
            let dx = lastX - startX;
            let dy = lastY - startY;
            let dist = Math.sqrt(dx * dx + dy * dy);
            if (dist > maxMove) {
                maxMove = dist;
            }

            // Smooth 1:1 vertical scroll when Fit: Full Width exceeds viewport height
            if (readerRoot.fitMode === "width" && pageFlick.contentHeight > pageFlick.height &&
                Math.abs(dy) > Math.abs(dx) * 1.2 && Math.abs(dy) > 14) {
                let maxY = pageFlick.contentHeight - pageFlick.height;
                pageFlick.contentY = Math.max(0, Math.min(maxY, startContentY - dy));
            }
        }

        onReleased: (touchPoints) => {
            if (touchPoints && touchPoints.length > 0 && touchPoints[0]) {
                lastX = touchPoints[0].x;
                lastY = touchPoints[0].y;
                let dx = lastX - startX;
                let dy = lastY - startY;
                let dist = Math.sqrt(dx * dx + dy * dy);
                if (dist > maxMove) {
                    maxMove = dist;
                }
            }
            completeGesture(false);
        }

        onCanceled: (touchPoints) => {
            completeGesture(true);
        }
    }

    // Floating 2x Zoom Exit & Controls Bar when zoomed in
    Row {
        visible: readerRoot.zoomed && !readerRoot.hudVisible && !readerRoot.loading
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 20
        spacing: 10
        z: 28

        Rectangle {
            width: 196
            height: 56
            color: exitZoomArea.pressed ? "#FFFFFF" : "#000000"
            border.width: 2
            border.color: "#000000"

            IconLabel {
                anchors.centerIn: parent
                icon: "close"
                text: "Exit 2x Zoom"
                pixelSize: 20
                color: exitZoomArea.pressed ? "#000000" : "#FFFFFF"
            }
            MouseArea {
                id: exitZoomArea
                anchors.fill: parent
                onClicked: (mouse) => readerRoot.resetPageView()
            }
        }

        Rectangle {
            width: 152
            height: 56
            color: zoomHudArea.pressed ? "#000000" : "#FFFFFF"
            border.width: 2
            border.color: "#000000"

            IconLabel {
                anchors.centerIn: parent
                icon: "menu"
                text: "Controls"
                pixelSize: 20
                color: zoomHudArea.pressed ? "#FFFFFF" : "#000000"
            }
            MouseArea {
                id: zoomHudArea
                anchors.fill: parent
                onClicked: (mouse) => { readerRoot.hudVisible = true; }
            }
        }
    }

    // Minimalist Page Progress Pill in bottom-right when HUD is hidden
    Rectangle {
        visible: !readerRoot.hudVisible && !readerRoot.loading && readerRoot.pages && readerRoot.pages.length > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 14
        width: miniCounterTxt.implicitWidth + 20
        height: 34
        color: "#FFFFFF"
        border.width: 1
        border.color: "#000000"
        opacity: 0.92
        z: 20

        Text {
            id: miniCounterTxt
            anchors.centerIn: parent
            text: (readerRoot.pageIndex + 1) + " / " + readerRoot.pages.length
            font.pixelSize: 17
            font.bold: true
            color: "#000000"
        }
    }

    // Transient Chapter Transition Banner (top-center)
    Rectangle {
        id: chapterBanner
        visible: false
        anchors.top: parent.top
        anchors.topMargin: 36
        anchors.horizontalCenter: parent.horizontalCenter
        width: chapterBannerText.implicitWidth + 48
        height: 60
        color: "#000000"
        border.width: 2
        border.color: "#FFFFFF"
        z: 40

        Text {
            id: chapterBannerText
            anchors.centerIn: parent
            text: ""
            font.pixelSize: 22
            font.bold: true
            color: "#FFFFFF"
        }

        Timer {
            id: chapterBannerTimer
            interval: 1800
            repeat: false
            onTriggered: () => { chapterBanner.visible = false; }
        }
    }

    // Loading / Error Modal Box
    Rectangle {
        visible: readerRoot.loading || readerRoot.errorMessage.length > 0
        anchors.fill: parent
        color: "#FFFFFF"
        z: 35

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 120, 680)
            spacing: 22

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: readerRoot.mangaTitle || ""
                font.pixelSize: 28
                font.bold: true
                color: "#000000"
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: readerRoot.errorMessage.length > 0
                      ? readerRoot.errorMessage
                      : readerRoot.loadingStatus
                font.pixelSize: 23
                color: "#222222"
                wrapMode: Text.WordWrap
            }

            Rectangle {
                visible: readerRoot.loading && readerRoot.errorMessage.length === 0
                width: parent.width
                height: 18
                color: "#F0F0F0"
                border.width: 2
                border.color: "#000000"

                Rectangle {
                    width: Math.max(24, Math.min(parent.width, parent.width * (readerRoot.loadingPct / 100.0)))
                    height: parent.height
                    color: "#000000"
                }
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 220
                height: 68
                color: cancelLoadArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    text: "← Back to Chapters"
                    font.pixelSize: 21
                    font.bold: true
                    color: cancelLoadArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: cancelLoadArea
                    anchors.fill: parent
                    onClicked: (mouse) => readerRoot.exitReader()
                }
            }
        }
    }

    // Reader HUD Overlay (Top Bar + Bottom Scrubber & Controls)
    Item {
        id: hudLayer
        visible: readerRoot.hudVisible && !readerRoot.loading
        anchors.fill: parent
        z: 30

        // Top HUD Bar
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 92
            color: "#FFFFFF"

            MouseArea { anchors.fill: parent }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 3
                color: "#000000"
            }

            Rectangle {
                id: hudBackBtn
                anchors.left: parent.left
                anchors.leftMargin: 22
                anchors.verticalCenter: parent.verticalCenter
                width: 170
                height: 62
                color: hudBackArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    text: "← Chapters"
                    font.pixelSize: 22
                    font.bold: true
                    color: hudBackArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: hudBackArea
                    anchors.fill: parent
                    onClicked: (mouse) => {
                        readerRoot.hudVisible = false;
                        readerRoot.exitReader();
                    }
                }
            }

            Column {
                anchors.left: hudBackBtn.right
                anchors.leftMargin: 20
                anchors.right: refreshFlashBtn.left
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    width: parent.width
                    text: readerRoot.mangaTitle || ""
                    font.pixelSize: 24
                    font.bold: true
                    color: "#000000"
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    text: {
                        let c = (readerRoot.chapterNum !== undefined && readerRoot.chapterNum !== null)
                                ? ("Chapter " + readerRoot.chapterNum) : "Chapter";
                        if (readerRoot.chapterTitle && readerRoot.chapterTitle.length > 0) {
                            c += " — " + readerRoot.chapterTitle;
                        }
                        return c;
                    }
                    font.pixelSize: 19
                    color: "#444444"
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                id: refreshFlashBtn
                anchors.right: parent.right
                anchors.rightMargin: 22
                anchors.verticalCenter: parent.verticalCenter
                width: 186
                height: 62
                color: flashArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                IconLabel {
                    anchors.centerIn: parent
                    icon: "refresh"
                    text: "Clear Ghost"
                    pixelSize: 20
                    color: flashArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: flashArea
                    anchors.fill: parent
                    onClicked: (mouse) => {
                        readerRoot.hudVisible = false;
                        readerRoot.triggerEinkGhostClear();
                    }
                }
            }
        }

        // Bottom HUD Panel
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 220
            color: "#FFFFFF"

            MouseArea { anchors.fill: parent }

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 3
                color: "#000000"
            }

            Column {
                anchors.fill: parent
                anchors.margins: 22
                spacing: 18

                // Row 1: Chapter Jump + Interactive RTL-Aware Page Scrubber
                Row {
                    width: parent.width
                    spacing: 14

                    // Left chapter button (Next Ch in RTL, Prev Ch in LTR)
                    Rectangle {
                        id: leftChapBtn
                        readonly property var targetCh: readerRoot.readingMode === "rtl" ? readerRoot.nextChapter : readerRoot.prevChapter
                        width: 170
                        height: 62
                        enabled: targetCh !== null && targetCh !== undefined && targetCh.id !== undefined
                        opacity: enabled ? 1.0 : 0.35
                        color: leftChArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: readerRoot.readingMode === "rtl" ? "« Next Ch" : "« Prev Ch"
                            font.pixelSize: 20
                            font.bold: true
                            color: leftChArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: leftChArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                if (leftChapBtn.targetCh) {
                                    readerRoot.hudVisible = false;
                                    readerRoot.jumpChapter(leftChapBtn.targetCh, false);
                                }
                            }
                        }
                    }

                    // Scrubber Track
                    Item {
                        id: scrubberTrack
                        width: parent.width - 170 - 170 - 28
                        height: 62

                        Text {
                            anchors.top: parent.top
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "Page " + (readerRoot.pageIndex + 1) + " of " + (readerRoot.pages ? readerRoot.pages.length : 0)
                            font.pixelSize: 20
                            font.bold: true
                            color: "#000000"
                        }

                        Rectangle {
                            id: trackBar
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 8
                            width: parent.width
                            height: 18
                            color: "#F0F0F0"
                            border.width: 2
                            border.color: "#000000"

                            readonly property real frac: (readerRoot.pages && readerRoot.pages.length > 1)
                                                         ? (readerRoot.pageIndex / (readerRoot.pages.length - 1))
                                                         : 0.0

                            // Fill from right in RTL, from left in LTR
                            Rectangle {
                                x: readerRoot.readingMode === "rtl"
                                   ? parent.width * (1.0 - trackBar.frac)
                                   : 0
                                width: parent.width * trackBar.frac
                                height: parent.height
                                color: "#000000"
                            }

                            // Thumb knob
                            Rectangle {
                                width: 26
                                height: 30
                                anchors.verticalCenter: parent.verticalCenter
                                x: {
                                    let pos = readerRoot.readingMode === "rtl"
                                              ? (1.0 - trackBar.frac) * (parent.width - width)
                                              : trackBar.frac * (parent.width - width);
                                    return Math.max(0, Math.min(parent.width - width, pos));
                                }
                                color: "#000000"
                                border.width: 2
                                border.color: "#FFFFFF"
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            function scrubAt(mx) {
                                if (!readerRoot.pages || readerRoot.pages.length <= 1) return;
                                let r = Math.max(0.0, Math.min(1.0, mx / width));
                                if (readerRoot.readingMode === "rtl") r = 1.0 - r;
                                let idx = Math.round(r * (readerRoot.pages.length - 1));
                                readerRoot.jumpToPage(idx);
                            }
                            onPressed: (mouse) => scrubAt(mouse.x)
                            onPositionChanged: (mouse) => { if (pressed) scrubAt(mouse.x); }
                        }
                    }

                    // Right chapter button (Prev Ch in RTL, Next Ch in LTR)
                    Rectangle {
                        id: rightChapBtn
                        readonly property var targetCh: readerRoot.readingMode === "rtl" ? readerRoot.prevChapter : readerRoot.nextChapter
                        width: 170
                        height: 62
                        enabled: targetCh !== null && targetCh !== undefined && targetCh.id !== undefined
                        opacity: enabled ? 1.0 : 0.35
                        color: rightChArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: readerRoot.readingMode === "rtl" ? "Prev Ch »" : "Next Ch »"
                            font.pixelSize: 20
                            font.bold: true
                            color: rightChArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: rightChArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                if (rightChapBtn.targetCh) {
                                    readerRoot.hudVisible = false;
                                    readerRoot.jumpChapter(rightChapBtn.targetCh, false);
                                }
                            }
                        }
                    }
                }

                // Row 2: Reading Direction, Swipe Direction, Fit Mode & Zoom Toggles
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 14

                    Rectangle {
                        width: 280
                        height: 62
                        color: dirArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: readerRoot.readingMode === "rtl"
                                  ? "Tap: Manga (RTL ←)"
                                  : "Tap: Comic (LTR →)"
                            font.pixelSize: 19
                            font.bold: true
                            color: dirArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: dirArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                let nextDir = readerRoot.readingMode === "rtl" ? "ltr" : "rtl";
                                readerRoot.readingMode = nextDir;
                                readerRoot.settingsModified(nextDir, readerRoot.fitMode, readerRoot.invertSwipe);
                            }
                        }
                    }

                    Rectangle {
                        width: 270
                        height: 62
                        color: swpArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: readerRoot.swipeRightIsNext
                                  ? "Swipe: → is Next"
                                  : "Swipe: ← is Next"
                            font.pixelSize: 19
                            font.bold: true
                            color: swpArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: swpArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                let nextInv = !readerRoot.invertSwipe;
                                readerRoot.invertSwipe = nextInv;
                                readerRoot.settingsModified(readerRoot.readingMode, readerRoot.fitMode, nextInv);
                                readerRoot.showChapterToast(readerRoot.swipeRightIsNext
                                    ? "Swipe Right (→) = Next Page"
                                    : "Swipe Left (←) = Next Page");
                            }
                        }
                    }

                    Rectangle {
                        width: 236
                        height: 62
                        color: fitArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: {
                                if (readerRoot.fitMode === "width") return "Fit: Full Width";
                                if (readerRoot.fitMode === "crop") return "Fit: Trim Margins";
                                return "Fit: Full Page";
                            }
                            font.pixelSize: 19
                            font.bold: true
                            color: fitArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: fitArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                let nextFit = "contain";
                                if (readerRoot.fitMode === "contain") nextFit = "crop";
                                else if (readerRoot.fitMode === "crop") nextFit = "width";
                                readerRoot.fitMode = nextFit;
                                readerRoot.resetPageView();
                                readerRoot.settingsModified(readerRoot.readingMode, nextFit, readerRoot.invertSwipe);
                            }
                        }
                    }

                    Rectangle {
                        width: 190
                        height: 62
                        color: (readerRoot.zoomed || zoomBtnArea.pressed) ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: readerRoot.zoomed ? "Zoom: 2x (On)" : "Zoom: 1x"
                            font.pixelSize: 19
                            font.bold: true
                            color: (readerRoot.zoomed || zoomBtnArea.pressed) ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: zoomBtnArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                if (readerRoot.zoomed) {
                                    readerRoot.resetPageView();
                                } else {
                                    readerRoot.zoomed = true;
                                    readerRoot.hudVisible = false;
                                    pageFlick.contentX = pageFlick.width * 0.5;
                                    pageFlick.contentY = pageFlick.height * 0.5;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Fullscreen E-Ink Waveform Ghost-Clearing Flash Rect
    Rectangle {
        id: ghostFlashRect
        visible: false
        anchors.fill: parent
        color: "#000000"
        z: 100

        Timer {
            id: ghostStep1Timer
            interval: 90
            repeat: false
            onTriggered: () => {
                ghostFlashRect.color = "#FFFFFF";
                ghostStep2Timer.start();
            }
        }

        Timer {
            id: ghostStep2Timer
            interval: 80
            repeat: false
            onTriggered: () => {
                ghostFlashRect.visible = false;
            }
        }
    }
}
