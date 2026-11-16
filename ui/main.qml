import QtQuick
import net.asivery.AppLoad 1.0

Rectangle {
    id: appRoot
    anchors.fill: parent
    color: "#FFFFFF"

    // Required AppLoad lifecycle hooks
    signal close
    function unloading() {
        bootstrapRetryTimer.stop();
    }

    // IPC message constants
    readonly property int reqBootstrap: 1
    readonly property int reqRefreshLibrary: 2
    readonly property int reqSearch: 3
    readonly property int reqMangaDetails: 4
    readonly property int reqToggleLibrary: 5
    readonly property int reqOpenChapter: 6
    readonly property int reqSaveProgress: 7
    readonly property int reqMarkChapterRead: 8
    readonly property int reqDownloadChapters: 9
    readonly property int reqCancelDownload: 10
    readonly property int reqDeleteDownload: 11
    readonly property int reqListSources: 12
    readonly property int reqInstallSource: 13
    readonly property int reqUpdateSettings: 14
    readonly property int reqSaveSourceConfig: 15

    readonly property int evtBootstrap: 100
    readonly property int evtLibraryUpdated: 101
    readonly property int evtSearchResults: 102
    readonly property int evtMangaDetails: 103
    readonly property int evtReaderLoading: 104
    readonly property int evtChapterPages: 105
    readonly property int evtDownloadsUpdated: 106
    readonly property int evtSourcesList: 107
    readonly property int evtCoverUpdated: 108
    readonly property int evtToast: 109

    // Global state
    property bool bootstrapped: false
    property string activeTab: "library"   // "library" | "recent" | "browse" | "downloads"
    property string activeOverlay: "none"  // "none" | "detail" | "reader"
    property string returnOverlayAfterReader: "detail"

    property var libraryItems: []
    property var continueReading: null
    property var recentItems: []
    property var downloadQueue: []
    property var downloadedItems: []
    property real cacheBytes: 0
    property var installedSources: []
    property var availableSources: []
    property bool sourcesLoading: false
    property string sourcesError: ""
    property var sourceConfig: ({
        source_lists: [],
        languages: ["en"],
        config_path: "",
        raw_json: "{\n  \"source_lists\": [],\n  \"languages\": [\"en\"]\n}"
    })
    property bool aidokuOnline: false

    property var searchResults: []
    property bool searchLoading: false
    property string searchError: ""

    property string readingMode: "rtl"
    property string fitMode: "contain"
    property bool invertSwipe: false
    property int ghostRefreshPages: 20

    function sendReq(msgType, payloadObj) {
        endpoint.sendMessage(msgType, JSON.stringify(payloadObj || {}));
    }

    function showToast(msg) {
        if (!msg || msg.length === 0) return;
        toastLabel.text = msg;
        toastBox.visible = true;
        toastTimer.restart();
    }

    function navigateToManga(mangaObj) {
        if (!mangaObj) return;
        let sid = mangaObj.source_id || (mangaObj.source ? mangaObj.source.id : "");
        let sname = mangaObj.source_name || (mangaObj.source ? mangaObj.source.name : sid);
        let mid = mangaObj.manga_id || mangaObj.id || "";
        let title = mangaObj.title || mangaObj.manga_title || mid;
        if (!sid || !mid) return;

        mangaDetailView.sourceId = sid;
        mangaDetailView.sourceName = sname;
        mangaDetailView.mangaId = mid;
        mangaDetailView.mangaTitle = title;
        mangaDetailView.author = mangaObj.author || "";
        mangaDetailView.description = mangaObj.description || "";
        mangaDetailView.statusText = mangaObj.status || "";
        mangaDetailView.coverPath = mangaObj.cover_path || "";
        mangaDetailView.coverUrl = mangaObj.cover_url || "";
        mangaDetailView.inLibrary = Boolean(mangaObj.in_library);
        mangaDetailView.chapters = [];
        mangaDetailView.loading = true;
        mangaDetailView.errorMessage = "";
        activeOverlay = "detail";

        sendReq(reqMangaDetails, {
            source_id: sid,
            source_name: sname,
            manga_id: mid,
            manga_title: title,
            cover_url: mangaObj.cover_url || "",
            author: mangaObj.author || "",
            description: mangaObj.description || "",
            force_refresh: false
        });
    }

    function launchChapter(sid, sname, mid, mtitle, cid, cnum, ctitle, startAtEnd, fromOverlay) {
        returnOverlayAfterReader = fromOverlay || "none";
        readerView.sourceId = sid;
        readerView.sourceName = sname || sid;
        readerView.mangaId = mid;
        readerView.mangaTitle = mtitle || mid;
        readerView.chapterId = cid;
        readerView.chapterNum = cnum;
        readerView.chapterTitle = ctitle || "";
        readerView.pages = [];
        readerView.pageIndex = 0;
        readerView.loading = true;
        readerView.loadingStatus = "Opening chapter...";
        readerView.loadingPct = 10;
        readerView.errorMessage = "";
        readerView.hudVisible = false;
        readerView.resetPageView();
        activeOverlay = "reader";

        let payload = {
            source_id: sid,
            source_name: sname || sid,
            manga_id: mid,
            manga_title: mtitle || mid,
            chapter_id: cid,
            chapter_title: ctitle || "",
            start_at_end: Boolean(startAtEnd)
        };
        if (cnum !== undefined && cnum !== null) {
            payload.chapter_num = Number(cnum);
        }
        sendReq(reqOpenChapter, payload);
    }

    function applyCoverUpdate(sid, mid, coverUri) {
        if (libraryItems && libraryItems.length > 0) {
            let updatedLib = libraryItems.slice();
            let changed = false;
            for (let i = 0; i < updatedLib.length; ++i) {
                if (updatedLib[i].source_id === sid && updatedLib[i].manga_id === mid) {
                    updatedLib[i] = Object.assign({}, updatedLib[i], { cover_path: coverUri });
                    changed = true;
                }
            }
            if (changed) libraryItems = updatedLib;
        }
        if (continueReading && continueReading.source_id === sid && continueReading.manga_id === mid) {
            continueReading = Object.assign({}, continueReading, { cover_path: coverUri });
        }
        if (searchResults && searchResults.length > 0) {
            let updatedRes = searchResults.slice();
            let rChanged = false;
            for (let j = 0; j < updatedRes.length; ++j) {
                let rSid = updatedRes[j].source_id || (updatedRes[j].source ? updatedRes[j].source.id : "");
                let rMid = updatedRes[j].id || updatedRes[j].manga_id || "";
                if (rSid === sid && rMid === mid) {
                    updatedRes[j] = Object.assign({}, updatedRes[j], { cover_path: coverUri });
                    rChanged = true;
                }
            }
            if (rChanged) searchResults = updatedRes;
        }
        if (mangaDetailView.sourceId === sid && mangaDetailView.mangaId === mid) {
            mangaDetailView.coverPath = coverUri;
        }
    }

    AppLoad {
        id: endpoint
        applicationID: "rm2manga"

        onMessageReceived: (type, contents) => {
            let data = {};
            try {
                if (contents && contents.length > 0) {
                    data = JSON.parse(contents);
                }
            } catch (e) {
                console.log("[rm2manga] JSON parse error:", e);
                return;
            }

            switch (type) {
                case appRoot.evtBootstrap:
                    appRoot.bootstrapped = true;
                    bootstrapRetryTimer.stop();
                    appRoot.libraryItems = data.library || [];
                    appRoot.continueReading = (data.continue_reading && data.continue_reading.manga_id) ? data.continue_reading : null;
                    appRoot.recentItems = data.recent || [];
                    appRoot.downloadedItems = data.downloads || [];
                    appRoot.downloadQueue = data.queue || [];
                    appRoot.installedSources = data.installed_sources || [];
                    if (data.source_config) {
                        appRoot.sourceConfig = data.source_config;
                    }
                    appRoot.aidokuOnline = Boolean(data.aidoku_online);
                    if (data.settings) {
                        appRoot.readingMode = data.settings.readingMode || "rtl";
                        appRoot.fitMode = data.settings.fitMode || "contain";
                        appRoot.invertSwipe = Boolean(data.settings.invertSwipe);
                        appRoot.ghostRefreshPages = data.settings.ghostRefreshPages !== undefined ? data.settings.ghostRefreshPages : 20;
                    }
                    break;

                case appRoot.evtLibraryUpdated:
                    appRoot.libraryItems = data.library || [];
                    appRoot.continueReading = (data.continue_reading && data.continue_reading.manga_id) ? data.continue_reading : null;
                    appRoot.recentItems = data.recent || [];
                    // Keep search result in_library badges synced
                    if (appRoot.searchResults && appRoot.searchResults.length > 0) {
                        let libMap = {};
                        for (let i = 0; i < appRoot.libraryItems.length; ++i) {
                            let li = appRoot.libraryItems[i];
                            libMap[li.source_id + "::" + li.manga_id] = true;
                        }
                        let nextRes = appRoot.searchResults.slice();
                        for (let j = 0; j < nextRes.length; ++j) {
                            let rSid = nextRes[j].source_id || (nextRes[j].source ? nextRes[j].source.id : "");
                            let rMid = nextRes[j].id || nextRes[j].manga_id || "";
                            let saved = Boolean(libMap[rSid + "::" + rMid]);
                            if (Boolean(nextRes[j].in_library) !== saved) {
                                nextRes[j] = Object.assign({}, nextRes[j], { in_library: saved });
                            }
                        }
                        appRoot.searchResults = nextRes;
                    }
                    break;

                case appRoot.evtSearchResults:
                    appRoot.searchResults = data.results || [];
                    appRoot.searchLoading = Boolean(data.loading);
                    appRoot.searchError = data.error || "";
                    break;

                case appRoot.evtMangaDetails:
                    if (mangaDetailView.sourceId === data.source_id && mangaDetailView.mangaId === data.manga_id) {
                        mangaDetailView.mangaTitle = data.manga_title || mangaDetailView.mangaTitle;
                        mangaDetailView.author = data.author || mangaDetailView.author;
                        mangaDetailView.description = data.description || mangaDetailView.description;
                        mangaDetailView.statusText = data.status || mangaDetailView.statusText;
                        if (data.cover_path) mangaDetailView.coverPath = data.cover_path;
                        mangaDetailView.inLibrary = Boolean(data.in_library);
                        mangaDetailView.chapters = data.chapters || [];
                        mangaDetailView.resumeChapterId = data.resume_chapter_id || "";
                        mangaDetailView.resumePage = data.resume_page || 0;
                        mangaDetailView.loading = Boolean(data.loading);
                        mangaDetailView.errorMessage = data.error || "";
                    }
                    break;

                case appRoot.evtReaderLoading:
                    if (appRoot.activeOverlay === "reader") {
                        readerView.loading = true;
                        readerView.loadingStatus = data.status_text || "Loading chapter...";
                        readerView.loadingPct = data.progress_pct || 25;
                    }
                    break;

                case appRoot.evtChapterPages:
                    if (appRoot.activeOverlay === "reader") {
                        readerView.sourceId = data.source_id || readerView.sourceId;
                        readerView.sourceName = data.source_name || readerView.sourceName;
                        readerView.mangaId = data.manga_id || readerView.mangaId;
                        readerView.mangaTitle = data.manga_title || readerView.mangaTitle;
                        readerView.chapterId = data.chapter_id || readerView.chapterId;
                        readerView.chapterNum = data.chapter_num;
                        readerView.chapterTitle = data.chapter_title || "";
                        readerView.pages = data.pages || [];
                        readerView.pageIndex = data.initial_page || 0;
                        readerView.prevChapter = (data.prev_chapter && data.prev_chapter.id) ? data.prev_chapter : null;
                        readerView.nextChapter = (data.next_chapter && data.next_chapter.id) ? data.next_chapter : null;
                        readerView.loading = false;
                        readerView.errorMessage = (readerView.pages.length === 0)
                                                  ? (data.error || "No readable pages found in chapter")
                                                  : "";
                        readerView.forceActiveFocus();
                    }
                    break;

                case appRoot.evtDownloadsUpdated:
                    appRoot.downloadQueue = data.queue || [];
                    appRoot.downloadedItems = data.downloads || [];
                    appRoot.cacheBytes = data.cache_bytes || 0;
                    break;

                case appRoot.evtSourcesList:
                    appRoot.installedSources = data.installed || [];
                    appRoot.availableSources = data.available || [];
                    if (data.source_config) {
                        appRoot.sourceConfig = data.source_config;
                    }
                    appRoot.sourcesLoading = Boolean(data.loading);
                    appRoot.sourcesError = data.error || "";
                    break;

                case appRoot.evtCoverUpdated:
                    if (data.source_id && data.manga_id && data.cover_path) {
                        appRoot.applyCoverUpdate(data.source_id, data.manga_id, data.cover_path);
                    }
                    break;

                case appRoot.evtToast:
                    appRoot.showToast(data.message || "");
                    break;
            }
        }
    }

    // Fallback bootstrap retry in case backend socket finished connecting milliseconds after QML mounted
    Timer {
        id: bootstrapRetryTimer
        interval: 350
        repeat: true
        running: true
        onTriggered: () => {
            if (!appRoot.bootstrapped) {
                appRoot.sendReq(appRoot.reqBootstrap, {});
            } else {
                bootstrapRetryTimer.stop();
            }
        }
    }

    Component.onCompleted: {
        sendReq(reqBootstrap, {});
    }

    // 1. Top Application Header (hidden when Reader is fullscreen)
    Rectangle {
        id: appHeader
        visible: appRoot.activeOverlay === "none"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? 88 : 0
        color: "#FFFFFF"
        z: 10

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 3
            color: "#000000"
        }

        // Top-left Exit to AppLoad Launcher button
        Rectangle {
            id: exitAppBtn
            anchors.left: parent.left
            anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            width: 126
            height: 58
            color: exitArea.pressed ? "#000000" : "#FFFFFF"
            border.width: 2
            border.color: "#000000"

            IconLabel {
                anchors.centerIn: parent
                icon: "close"
                text: "Exit"
                pixelSize: 22
                color: exitArea.pressed ? "#FFFFFF" : "#000000"
            }
            MouseArea {
                id: exitArea
                anchors.fill: parent
                onClicked: (mouse) => appRoot.close()
            }
        }

        // App Title & Source Engine Status
        Row {
            anchors.centerIn: parent
            spacing: 14

            Text {
                text: "rm2manga"
                font.pixelSize: 32
                font.bold: true
                color: "#000000"
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: engineLbl.implicitWidth + 24
                height: 36
                color: hdrSrcArea.pressed ? "#000000" : "#F4F4F4"
                border.width: 1
                border.color: "#000000"

                Text {
                    id: engineLbl
                    anchors.centerIn: parent
                    text: {
                        let cnt = appRoot.installedSources ? appRoot.installedSources.length : 0;
                        if (cnt > 0) return cnt + (cnt === 1 ? " Source" : " Sources");
                        return appRoot.aidokuOnline ? "Configure Sources" : "Offline";
                    }
                    font.pixelSize: 17
                    font.bold: true
                    color: hdrSrcArea.pressed ? "#FFFFFF" : "#000000"
                }

                MouseArea {
                    id: hdrSrcArea
                    anchors.fill: parent
                    onClicked: (mouse) => {
                        appRoot.activeOverlay = "none";
                        appRoot.activeTab = "browse";
                        browseView.openSourcesModal("");
                    }
                }
            }
        }

        // Quick Resume Pill when browsing non-Library tabs
        Rectangle {
            visible: appRoot.activeTab !== "library" && appRoot.continueReading !== null &&
                     appRoot.continueReading.last_chapter_id !== undefined &&
                     appRoot.continueReading.last_chapter_id !== ""
            anchors.right: parent.right
            anchors.rightMargin: 120
            anchors.verticalCenter: parent.verticalCenter
            width: 184
            height: 56
            color: headerResumeArea.pressed ? "#FFFFFF" : "#000000"
            border.width: 2
            border.color: "#000000"

            IconLabel {
                anchors.centerIn: parent
                icon: "play"
                text: "Resume"
                pixelSize: 21
                color: headerResumeArea.pressed ? "#000000" : "#FFFFFF"
            }
            MouseArea {
                id: headerResumeArea
                anchors.fill: parent
                onClicked: (mouse) => {
                    let cr = appRoot.continueReading;
                    if (cr && cr.last_chapter_id) {
                        appRoot.launchChapter(
                            cr.source_id, cr.source_name, cr.manga_id, cr.title,
                            cr.last_chapter_id, cr.last_chapter_num, cr.last_chapter_title,
                            false, "none"
                        );
                    }
                }
            }
        }
    }

    // 2. Main Tab Views Container
    Item {
        id: mainTabArea
        visible: appRoot.activeOverlay === "none"
        anchors.top: appHeader.bottom
        anchors.bottom: bottomNavBar.top
        anchors.left: parent.left
        anchors.right: parent.right

        LibraryView {
            anchors.fill: parent
            visible: appRoot.activeTab === "library"
            libraryItems: appRoot.libraryItems
            continueItem: appRoot.continueReading

            onOpenManga: (m) => appRoot.navigateToManga(m)
            onResumeReading: (cr) => {
                if (cr && cr.last_chapter_id) {
                    appRoot.launchChapter(
                        cr.source_id, cr.source_name, cr.manga_id, cr.title,
                        cr.last_chapter_id, cr.last_chapter_num, cr.last_chapter_title,
                        false, "none"
                    );
                } else if (cr) {
                    appRoot.navigateToManga(cr);
                }
            }
            onSyncRequested: (deep) => {
                appRoot.showToast(deep ? "Checking sources for updates..." : "Syncing library...");
                appRoot.sendReq(appRoot.reqRefreshLibrary, { deep: deep });
            }
            onJumpToBrowse: () => {
                appRoot.activeTab = "browse";
                browseView.ensureInitialCatalog();
            }
        }

        RecentView {
            anchors.fill: parent
            visible: appRoot.activeTab === "recent"
            recentItems: appRoot.recentItems

            onOpenManga: (it) => appRoot.navigateToManga(it)
            onResumeItem: (it) => {
                if (it && it.last_chapter_id) {
                    appRoot.launchChapter(
                        it.source_id, it.source_name, it.manga_id, it.title,
                        it.last_chapter_id, it.last_chapter_num, it.last_chapter_title,
                        false, "none"
                    );
                }
            }
        }

        BrowseView {
            id: browseView
            anchors.fill: parent
            visible: appRoot.activeTab === "browse"
            searchResults: appRoot.searchResults
            searchLoading: appRoot.searchLoading
            searchError: appRoot.searchError
            installedSources: appRoot.installedSources
            availableSources: appRoot.availableSources
            sourcesLoading: appRoot.sourcesLoading
            sourcesError: appRoot.sourcesError
            sourceConfig: appRoot.sourceConfig

            onRequestSearch: (q, filter, mode, forceRefresh) => {
                appRoot.searchLoading = true;
                appRoot.searchError = "";
                appRoot.sendReq(appRoot.reqSearch, {
                    query: q,
                    source_filter: filter,
                    mode: mode,
                    force_refresh: Boolean(forceRefresh)
                });
            }
            onOpenManga: (m) => appRoot.navigateToManga(m)
            onToggleLibrary: (m, add) => {
                let sid = m.source_id || (m.source ? m.source.id : "");
                let sname = m.source_name || (m.source ? m.source.name : sid);
                let mid = m.id || m.manga_id || "";
                appRoot.sendReq(appRoot.reqToggleLibrary, {
                    source_id: sid,
                    source_name: sname,
                    manga_id: mid,
                    manga_title: m.title || mid,
                    cover_url: m.cover_url || "",
                    author: m.author || "",
                    description: m.description || "",
                    add: add
                });
            }
            onRequestSourcesList: () => {
                appRoot.sourcesLoading = true;
                appRoot.sourcesError = "";
                appRoot.sendReq(appRoot.reqListSources, { include_available: true });
            }
            onRequestInstallSource: (sid, install) => {
                appRoot.sourcesLoading = true;
                appRoot.sourcesError = "";
                appRoot.sendReq(appRoot.reqInstallSource, { source_id: sid, install: install });
            }
            onRequestSaveSourceConfig: (cfgPayload) => {
                appRoot.sourcesLoading = true;
                appRoot.sourcesError = "";
                appRoot.sendReq(appRoot.reqSaveSourceConfig, cfgPayload);
            }
        }

        DownloadsView {
            anchors.fill: parent
            visible: appRoot.activeTab === "downloads"
            queueItems: appRoot.downloadQueue
            downloadedItems: appRoot.downloadedItems
            cacheBytes: appRoot.cacheBytes

            onOpenDownloadedChapter: (dl) => {
                appRoot.launchChapter(
                    dl.source_id, dl.source_name, dl.manga_id, dl.manga_title,
                    dl.chapter_id, dl.chapter_num, dl.chapter_title,
                    false, "none"
                );
            }
            onCancelDownload: (key, all) => {
                appRoot.sendReq(appRoot.reqCancelDownload, { key: key, all: all });
            }
            onDeleteDownload: (dl) => {
                appRoot.sendReq(appRoot.reqDeleteDownload, {
                    source_id: dl.source_id,
                    manga_id: dl.manga_id,
                    chapter_id: dl.chapter_id
                });
            }
            onClearTempCache: () => {
                appRoot.sendReq(appRoot.reqDeleteDownload, { clear_cache: true });
            }
        }
    }

    // 3. Manga Detail Full View Overlay
    MangaDetailView {
        id: mangaDetailView
        visible: appRoot.activeOverlay === "detail"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: bottomNavBar.top
        z: 15

        onBackRequested: () => { appRoot.activeOverlay = "none"; }
        onRefreshRequested: () => {
            mangaDetailView.loading = true;
            appRoot.sendReq(appRoot.reqMangaDetails, {
                source_id: mangaDetailView.sourceId,
                source_name: mangaDetailView.sourceName,
                manga_id: mangaDetailView.mangaId,
                manga_title: mangaDetailView.mangaTitle,
                cover_url: mangaDetailView.coverUrl,
                force_refresh: true
            });
        }
        onToggleLibraryRequested: (add) => {
            mangaDetailView.inLibrary = add;
            appRoot.sendReq(appRoot.reqToggleLibrary, {
                source_id: mangaDetailView.sourceId,
                source_name: mangaDetailView.sourceName,
                manga_id: mangaDetailView.mangaId,
                manga_title: mangaDetailView.mangaTitle,
                cover_url: mangaDetailView.coverUrl,
                author: mangaDetailView.author,
                description: mangaDetailView.description,
                add: add
            });
        }
        onOpenChapterRequested: (ch) => {
            appRoot.launchChapter(
                mangaDetailView.sourceId,
                mangaDetailView.sourceName,
                mangaDetailView.mangaId,
                mangaDetailView.mangaTitle,
                ch.id,
                ch.chapter_num,
                ch.title || "",
                false,
                "detail"
            );
        }
        onDownloadChaptersRequested: (items) => {
            appRoot.sendReq(appRoot.reqDownloadChapters, { items: items });
        }
        onToggleChapterReadRequested: (ch, markRead) => {
            // Optimistically update UI chapter row immediately
            let list = mangaDetailView.chapters.slice();
            for (let i = 0; i < list.length; ++i) {
                if (list[i].id === ch.id) {
                    list[i] = Object.assign({}, list[i], { read: markRead });
                    break;
                }
            }
            mangaDetailView.chapters = list;
            appRoot.sendReq(appRoot.reqMarkChapterRead, {
                source_id: mangaDetailView.sourceId,
                manga_id: mangaDetailView.mangaId,
                chapter_id: ch.id,
                read: markRead
            });
        }
    }

    // 4. Fullscreen Reader Overlay (1404 x 1872 full-bleed)
    ReaderView {
        id: readerView
        visible: appRoot.activeOverlay === "reader"
        anchors.fill: parent
        z: 25

        readingMode: appRoot.readingMode
        fitMode: appRoot.fitMode
        invertSwipe: appRoot.invertSwipe
        ghostRefreshPages: appRoot.ghostRefreshPages

        onExitReader: () => {
            appRoot.activeOverlay = appRoot.returnOverlayAfterReader;
            if (appRoot.activeOverlay === "detail") {
                // Refresh cached chapter read/progress indicators from local store
                appRoot.sendReq(appRoot.reqMangaDetails, {
                    source_id: mangaDetailView.sourceId,
                    source_name: mangaDetailView.sourceName,
                    manga_id: mangaDetailView.mangaId,
                    manga_title: mangaDetailView.mangaTitle,
                    force_refresh: false
                });
            }
        }

        onSaveProgress: (curPage, totalPages, completed) => {
            let payload = {
                source_id: readerView.sourceId,
                source_name: readerView.sourceName,
                manga_id: readerView.mangaId,
                manga_title: readerView.mangaTitle,
                chapter_id: readerView.chapterId,
                chapter_title: readerView.chapterTitle,
                page_index: curPage,
                page_count: totalPages,
                completed: completed
            };
            if (readerView.chapterNum !== undefined && readerView.chapterNum !== null) {
                payload.chapter_num = Number(readerView.chapterNum);
            }
            appRoot.sendReq(appRoot.reqSaveProgress, payload);
        }

        onJumpChapter: (targetCh, startAtEnd) => {
            if (!targetCh || !targetCh.id) return;
            appRoot.launchChapter(
                readerView.sourceId,
                readerView.sourceName,
                readerView.mangaId,
                readerView.mangaTitle,
                targetCh.id,
                targetCh.chapter_num,
                targetCh.title || "",
                startAtEnd,
                appRoot.returnOverlayAfterReader
            );
        }

        onSettingsModified: (newDir, newFit, newInvertSwipe) => {
            appRoot.readingMode = newDir;
            appRoot.fitMode = newFit;
            appRoot.invertSwipe = Boolean(newInvertSwipe);
            appRoot.sendReq(appRoot.reqUpdateSettings, {
                readingMode: newDir,
                fitMode: newFit,
                invertSwipe: Boolean(newInvertSwipe)
            });
        }
    }

    // 5. Mihon-Style Bottom Tab Bar (hidden inside fullscreen Reader)
    Rectangle {
        id: bottomNavBar
        visible: appRoot.activeOverlay !== "reader"
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? 96 : 0
        color: "#FFFFFF"
        z: 18

        Rectangle {
            anchors.top: parent.top
            width: parent.width
            height: 3
            color: "#000000"
        }

        Row {
            anchors.fill: parent
            anchors.topMargin: 3

            Repeater {
                model: [
                    { id: "library", label: "Library", icon: "library" },
                    { id: "recent", label: "Recent", icon: "clock" },
                    { id: "browse", label: "Browse", icon: "search" },
                    { id: "downloads", label: "Downloads", icon: "download" }
                ]
                delegate: Rectangle {
                    id: navTabBtn
                    readonly property bool selected: appRoot.activeTab === modelData.id && appRoot.activeOverlay === "none"
                    width: appRoot.width / 4
                    height: parent.height
                    color: (navTabBtn.selected || navTabArea.pressed) ? "#000000" : "#FFFFFF"

                    // Vertical separator between tabs
                    Rectangle {
                        visible: index > 0
                        anchors.left: parent.left
                        width: 1
                        height: parent.height
                        color: "#000000"
                    }

                    Row {
                        anchors.centerIn: parent
                        spacing: 10

                        AppIcon {
                            name: modelData.icon
                            width: 24
                            height: 24
                            anchors.verticalCenter: parent.verticalCenter
                            color: (navTabBtn.selected || navTabArea.pressed) ? "#FFFFFF" : "#000000"
                        }

                        Text {
                            text: {
                                if (modelData.id === "downloads" && appRoot.downloadQueue && appRoot.downloadQueue.length > 0) {
                                    return modelData.label + " (" + appRoot.downloadQueue.length + ")";
                                }
                                return modelData.label;
                            }
                            font.pixelSize: 24
                            font.bold: true
                            color: (navTabBtn.selected || navTabArea.pressed) ? "#FFFFFF" : "#000000"
                        }
                    }

                    MouseArea {
                        id: navTabArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            appRoot.activeOverlay = "none";
                            appRoot.activeTab = modelData.id;
                            if (modelData.id === "browse") {
                                browseView.ensureInitialCatalog();
                            }
                        }
                    }
                }
            }
        }
    }

    // 6. Crisp E-Ink Toast Notification Banner
    Rectangle {
        id: toastBox
        visible: false
        anchors.top: parent.top
        anchors.topMargin: 104
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 80, toastLabel.implicitWidth + 52)
        height: 62
        color: "#000000"
        border.width: 2
        border.color: "#FFFFFF"
        z: 60

        Text {
            id: toastLabel
            anchors.centerIn: parent
            width: parent.width - 36
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: ""
            font.pixelSize: 22
            font.bold: true
            color: "#FFFFFF"
        }

        Timer {
            id: toastTimer
            interval: 2600
            repeat: false
            onTriggered: () => { toastBox.visible = false; }
        }
    }
}
