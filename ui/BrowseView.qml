import QtQuick

Item {
    id: browseRoot

    property var searchResults: []
    property bool searchLoading: false
    property string searchError: ""
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

    property string queryText: ""
    property string sourceFilter: "all"
    property string browseMode: "discover" // "discover" | "search"
    property bool keyboardOpen: false
    property string activeInputTarget: "search" // "search" | "src_filter" | "new_url" | "raw_json"

    property bool extensionsModalOpen: false
    property string modalTab: "catalog"         // "catalog" | "config"
    property string configEditMode: "visual"    // "visual" | "json"
    property var draftSourceLists: []
    property var draftLanguages: ["en"]
    property string draftRawJson: ""
    property string newUrlText: ""
    property string catalogFilterText: ""
    property bool initialCatalogLoaded: false

    readonly property var commonLanguageCodes: [
        "en", "multi", "ja", "zh", "ko", "es", "fr", "pt", "de", "it", "ru", "id", "vi"
    ]

    signal requestSearch(string query, string sourceFilter, string mode, bool forceRefresh)
    signal openManga(var mangaObj)
    signal toggleLibrary(var mangaObj, bool add)
    signal requestSourcesList()
    signal requestInstallSource(string sourceId, bool install)
    signal requestSaveSourceConfig(var configPayload)

    function formatDraftJson(lists, langs) {
        let cleanLists = lists ? lists.slice() : [];
        let cleanLangs = (langs && langs.length > 0) ? langs.slice() : ["en"];
        return JSON.stringify({
            source_lists: cleanLists,
            languages: cleanLangs
        }, null, 2);
    }

    function syncDraftFromSourceConfig() {
        let cfg = sourceConfig || {};
        draftSourceLists = cfg.source_lists ? cfg.source_lists.slice() : [];
        draftLanguages = (cfg.languages && cfg.languages.length > 0) ? cfg.languages.slice() : ["en"];
        draftRawJson = (cfg.raw_json && cfg.raw_json.length > 0)
                       ? cfg.raw_json
                       : formatDraftJson(draftSourceLists, draftLanguages);
        rawJsonEdit.text = draftRawJson;
    }

    function openSourcesModal(preferredTab) {
        keyboardOpen = false;
        syncDraftFromSourceConfig();
        let hasConfiguredLists = sourceConfig && sourceConfig.source_lists && sourceConfig.source_lists.length > 0;
        let hasInstalled = installedSources && installedSources.length > 0;
        if (preferredTab && preferredTab.length > 0) {
            modalTab = preferredTab;
        } else if (!hasConfiguredLists && !hasInstalled) {
            modalTab = "config";
        } else {
            modalTab = "catalog";
        }
        extensionsModalOpen = true;
        requestSourcesList();
    }

    function isSourceInstalled(sid) {
        if (!installedSources) return false;
        for (let i = 0; i < installedSources.length; ++i) {
            if (installedSources[i].id === sid) return true;
        }
        return false;
    }

    function ensureInitialCatalog() {
        if (!installedSources || installedSources.length === 0) return;
        if (!initialCatalogLoaded && (!searchResults || searchResults.length === 0) && !searchLoading) {
            initialCatalogLoaded = true;
            browseMode = "discover";
            requestSearch("", sourceFilter, "discover", false);
        }
    }

    onInstalledSourcesChanged: {
        if (sourceFilter !== "all" && !isSourceInstalled(sourceFilter)) {
            sourceFilter = "all";
        }
        if (visible && installedSources && installedSources.length > 0 &&
            (!searchResults || searchResults.length === 0) && !searchLoading) {
            initialCatalogLoaded = true;
            browseMode = "discover";
            requestSearch("", sourceFilter, "discover", false);
        }
    }

    function submitSearch() {
        keyboardOpen = false;
        searchInput.focus = false;
        if (!installedSources || installedSources.length === 0) {
            openSourcesModal("config");
            return;
        }
        let q = queryText.trim();
        if (q.length === 0) {
            browseMode = "discover";
            requestSearch("", sourceFilter, "discover", false);
        } else {
            browseMode = "search";
            requestSearch(q, sourceFilter, "search", false);
        }
    }

    function addDraftUrl() {
        let trimmed = newUrlText.trim();
        if (trimmed.length === 0) return;
        let next = draftSourceLists ? draftSourceLists.slice() : [];
        if (next.indexOf(trimmed) < 0) {
            next.push(trimmed);
            draftSourceLists = next;
        }
        newUrlText = "";
        newUrlInput.text = "";
        draftRawJson = formatDraftJson(draftSourceLists, draftLanguages);
    }

    function removeDraftUrlAt(idx) {
        let next = draftSourceLists ? draftSourceLists.slice() : [];
        if (idx >= 0 && idx < next.length) {
            next.splice(idx, 1);
            draftSourceLists = next;
            draftRawJson = formatDraftJson(draftSourceLists, draftLanguages);
        }
    }

    function toggleDraftLanguage(code) {
        let next = draftLanguages ? draftLanguages.slice() : [];
        let pos = next.indexOf(code);
        if (pos >= 0) {
            if (next.length > 1) {
                next.splice(pos, 1);
            }
        } else {
            next.push(code);
        }
        draftLanguages = next;
        draftRawJson = formatDraftJson(draftSourceLists, draftLanguages);
    }

    function saveSourceConfigAction() {
        keyboardOpen = false;
        if (configEditMode === "json") {
            requestSaveSourceConfig({ raw_json: rawJsonEdit.text });
        } else {
            if (newUrlText.trim().length > 0) {
                addDraftUrl();
            }
            requestSaveSourceConfig({
                source_lists: draftSourceLists,
                languages: draftLanguages
            });
        }
        modalTab = "catalog";
    }

    function browseSpecificSource(sid) {
        keyboardOpen = false;
        extensionsModalOpen = false;
        sourceFilter = sid;
        queryText = "";
        searchInput.text = "";
        browseMode = "discover";
        initialCatalogLoaded = true;
        requestSearch("", sid, "discover", false);
    }

    Column {
        id: topBarCol
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 24
        spacing: 14

        // 1. Search Input Bar + Sources Button
        Row {
            width: parent.width
            spacing: 12

            Rectangle {
                id: searchBox
                width: parent.width - 220 - 180 - 24
                height: 74
                color: "#FFFFFF"
                border.width: (browseRoot.keyboardOpen && browseRoot.activeInputTarget === "search") ? 3 : 2
                border.color: "#000000"

                AppIcon {
                    anchors.left: parent.left
                    anchors.leftMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    width: 26
                    height: 26
                    color: "#000000"
                }

                TextInput {
                    id: searchInput
                    anchors.left: parent.left
                    anchors.leftMargin: 60
                    anchors.right: clearQueryBtn.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    font.pixelSize: 26
                    color: "#000000"
                    clip: true
                    text: browseRoot.queryText
                    onTextChanged: () => {
                        if (browseRoot.queryText !== searchInput.text) {
                            browseRoot.queryText = searchInput.text;
                        }
                    }
                    onAccepted: () => browseRoot.submitSearch()
                }

                Text {
                    visible: browseRoot.queryText.length === 0
                    anchors.left: parent.left
                    anchors.leftMargin: 60
                    anchors.verticalCenter: parent.verticalCenter
                    text: (browseRoot.installedSources && browseRoot.installedSources.length > 0)
                          ? "Search installed manga sources..."
                          : "Configure source lists first (Sources)..."
                    font.pixelSize: 24
                    color: "#666666"
                }

                MouseArea {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.right: clearQueryBtn.left
                    onClicked: (mouse) => {
                        browseRoot.activeInputTarget = "search";
                        searchInput.forceActiveFocus();
                        browseRoot.keyboardOpen = true;
                    }
                }

                Rectangle {
                    id: clearQueryBtn
                    visible: browseRoot.queryText.length > 0
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 54
                    height: 54
                    color: clrInArea.pressed ? "#000000" : "#F0F0F0"
                    border.width: 1
                    border.color: "#000000"

                    AppIcon {
                        anchors.centerIn: parent
                        name: "close"
                        width: 20
                        height: 20
                        color: clrInArea.pressed ? "#FFFFFF" : "#000000"
                    }
                    MouseArea {
                        id: clrInArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            browseRoot.queryText = "";
                            searchInput.text = "";
                            if (browseRoot.installedSources && browseRoot.installedSources.length > 0) {
                                browseRoot.browseMode = "discover";
                                browseRoot.requestSearch("", browseRoot.sourceFilter, "discover", false);
                            }
                        }
                    }
                }
            }

            // Search / Discover Trigger Button
            Rectangle {
                width: 180
                height: 74
                color: goSearchArea.pressed ? "#FFFFFF" : "#000000"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    text: browseRoot.searchLoading
                          ? "Loading..."
                          : (browseRoot.queryText.trim().length > 0 ? "Search" : "Discover")
                    font.pixelSize: 24
                    font.bold: true
                    color: goSearchArea.pressed ? "#000000" : "#FFFFFF"
                }
                MouseArea {
                    id: goSearchArea
                    anchors.fill: parent
                    onClicked: (mouse) => browseRoot.submitSearch()
                }
            }

            // Sources & Source Lists Manager Button
            Rectangle {
                width: 220
                height: 74
                color: extBtnArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                IconLabel {
                    anchors.centerIn: parent
                    icon: "settings"
                    text: "Sources (" + (browseRoot.installedSources ? browseRoot.installedSources.length : 0) + ")"
                    pixelSize: 22
                    color: extBtnArea.pressed ? "#FFFFFF" : "#000000"
                }
                MouseArea {
                    id: extBtnArea
                    anchors.fill: parent
                    onClicked: (mouse) => browseRoot.openSourcesModal("")
                }
            }
        }

        // 2. Discover & Installed Source Filter Pills
        Flickable {
            visible: browseRoot.installedSources && browseRoot.installedSources.length > 0
            width: parent.width
            height: visible ? 60 : 0
            contentWidth: pillsRow.width
            contentHeight: 60
            flickableDirection: Flickable.HorizontalFlick
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: pillsRow
                spacing: 10

                // Discover Feed Pill
                Rectangle {
                    id: discPill
                    readonly property bool activePill: browseRoot.browseMode === "discover" && browseRoot.queryText.trim().length === 0
                    width: discTxt.implicitWidth + 34
                    height: 56
                    color: (discPill.activePill || discArea.pressed) ? "#000000" : "#FFFFFF"
                    border.width: 2
                    border.color: "#000000"

                    IconLabel {
                        id: discTxt
                        anchors.centerIn: parent
                        icon: "sparkle"
                        text: "Discover"
                        pixelSize: 21
                        bold: discPill.activePill
                        color: (discPill.activePill || discArea.pressed) ? "#FFFFFF" : "#000000"
                    }
                    MouseArea {
                        id: discArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            browseRoot.queryText = "";
                            searchInput.text = "";
                            browseRoot.browseMode = "discover";
                            browseRoot.keyboardOpen = false;
                            browseRoot.requestSearch("", browseRoot.sourceFilter, "discover", false);
                        }
                    }
                }

                // Force Refresh Discover Pill
                Rectangle {
                    width: 56
                    height: 56
                    color: refDiscArea.pressed ? "#000000" : "#FFFFFF"
                    border.width: 2
                    border.color: "#000000"

                    AppIcon {
                        id: refDiscTxt
                        anchors.centerIn: parent
                        name: "refresh"
                        width: 22
                        height: 22
                        color: refDiscArea.pressed ? "#FFFFFF" : "#000000"
                    }
                    MouseArea {
                        id: refDiscArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            browseRoot.keyboardOpen = false;
                            let q = browseRoot.queryText.trim();
                            browseRoot.requestSearch(q, browseRoot.sourceFilter, q.length > 0 ? "search" : "discover", true);
                        }
                    }
                }

                // Vertical separator
                Rectangle {
                    width: 2
                    height: 40
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#000000"
                }

                // All Installed Sources filter
                Rectangle {
                    id: allSrcPill
                    readonly property bool sel: browseRoot.sourceFilter === "all"
                    width: allSrcTxt.implicitWidth + 32
                    height: 56
                    color: (allSrcPill.sel || allSrcArea.pressed) ? "#000000" : "#FFFFFF"
                    border.width: 2
                    border.color: "#000000"

                    Text {
                        id: allSrcTxt
                        anchors.centerIn: parent
                        text: "All Installed"
                        font.pixelSize: 21
                        font.bold: allSrcPill.sel
                        color: (allSrcPill.sel || allSrcArea.pressed) ? "#FFFFFF" : "#000000"
                    }
                    MouseArea {
                        id: allSrcArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            browseRoot.sourceFilter = "all";
                            browseRoot.keyboardOpen = false;
                            let q = browseRoot.queryText.trim();
                            if (q.length > 0) {
                                browseRoot.browseMode = "search";
                                browseRoot.requestSearch(q, "all", "search", false);
                            } else {
                                browseRoot.browseMode = "discover";
                                browseRoot.requestSearch("", "all", "discover", false);
                            }
                        }
                    }
                }

                // Per-Source Discover / Search Pills
                Repeater {
                    model: browseRoot.installedSources
                    delegate: Rectangle {
                        id: dynSrcPill
                        readonly property bool sel: browseRoot.sourceFilter === modelData.id
                        width: dynTxt.implicitWidth + 32
                        height: 56
                        color: (dynSrcPill.sel || dynArea.pressed) ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            id: dynTxt
                            anchors.centerIn: parent
                            text: modelData.name || modelData.id
                            font.pixelSize: 21
                            font.bold: dynSrcPill.sel
                            color: (dynSrcPill.sel || dynArea.pressed) ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: dynArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                browseRoot.sourceFilter = modelData.id;
                                browseRoot.keyboardOpen = false;
                                let q = browseRoot.queryText.trim();
                                if (q.length > 0) {
                                    browseRoot.browseMode = "search";
                                    browseRoot.requestSearch(q, modelData.id, "search", false);
                                } else {
                                    browseRoot.browseMode = "discover";
                                    browseRoot.requestSearch("", modelData.id, "discover", false);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 3. Results Area
    Flickable {
        id: resultsFlick
        anchors.top: topBarCol.bottom
        anchors.topMargin: 16
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: touchKeyboard.top
        contentWidth: width
        contentHeight: resultsCol.height + 40
        flickableDirection: Flickable.VerticalFlick
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: resultsCol
            x: 24
            width: parent.width - 48
            spacing: 20

            // Status / Loading Banner
            Rectangle {
                visible: browseRoot.searchLoading || browseRoot.searchError.length > 0
                width: parent.width
                height: 64
                color: "#F4F4F4"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    width: parent.width - 36
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    text: browseRoot.searchLoading
                          ? "Loading manga from installed sources..."
                          : ("Note: " + browseRoot.searchError)
                    font.pixelSize: 22
                    font.bold: true
                    color: "#000000"
                }
            }

            // Onboarding Card when zero sources are installed
            Rectangle {
                visible: !browseRoot.searchLoading && (!browseRoot.installedSources || browseRoot.installedSources.length === 0)
                width: parent.width
                height: 360
                color: "#F8F8F8"
                border.width: 2
                border.color: "#000000"

                Column {
                    anchors.centerIn: parent
                    width: parent.width - 80
                    spacing: 18

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "No Manga Sources Installed"
                        font.pixelSize: 30
                        font.bold: true
                        color: "#000000"
                    }

                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: "rm2manga ships with zero built-in sources. Enter your own Aidoku source_lists JSON configuration to install sources and discover manga."
                        font.pixelSize: 22
                        color: "#333333"
                    }

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 390
                        height: 76
                        color: cfgOnboardingArea.pressed ? "#FFFFFF" : "#000000"
                        border.width: 2
                        border.color: "#000000"

                        IconLabel {
                            anchors.centerIn: parent
                            icon: "settings"
                            text: "Configure Source Lists  →"
                            pixelSize: 24
                            color: cfgOnboardingArea.pressed ? "#000000" : "#FFFFFF"
                        }
                        MouseArea {
                            id: cfgOnboardingArea
                            anchors.fill: parent
                            onClicked: (mouse) => browseRoot.openSourcesModal("config")
                        }
                    }
                }
            }

            // Empty search/discover result box when sources ARE installed
            Rectangle {
                visible: !browseRoot.searchLoading &&
                         browseRoot.installedSources && browseRoot.installedSources.length > 0 &&
                         (!browseRoot.searchResults || browseRoot.searchResults.length === 0)
                width: parent.width
                height: 240
                color: "#F8F8F8"
                border.width: 2
                border.color: "#000000"

                Column {
                    anchors.centerIn: parent
                    spacing: 14

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: browseRoot.queryText.trim().length > 0
                              ? "No manga matched \"" + browseRoot.queryText.trim() + "\"."
                              : "Tap Discover or enter a title above to browse installed sources."
                        font.pixelSize: 23
                        color: "#333333"
                    }

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 240
                        height: 62
                        color: retryDiscArea.pressed ? "#FFFFFF" : "#000000"
                        border.width: 2
                        border.color: "#000000"

                        IconLabel {
                            anchors.centerIn: parent
                            icon: "sparkle"
                            text: "Discover Manga"
                            pixelSize: 21
                            color: retryDiscArea.pressed ? "#000000" : "#FFFFFF"
                        }
                        MouseArea {
                            id: retryDiscArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                browseRoot.queryText = "";
                                searchInput.text = "";
                                browseRoot.browseMode = "discover";
                                browseRoot.requestSearch("", browseRoot.sourceFilter, "discover", true);
                            }
                        }
                    }
                }
            }

            // 3-Column Results Grid
            Grid {
                id: resGrid
                visible: browseRoot.searchResults && browseRoot.searchResults.length > 0
                width: parent.width
                columns: 3
                columnSpacing: 24
                rowSpacing: 28

                readonly property real cardWidth: Math.floor((width - 48) / 3)

                Repeater {
                    model: browseRoot.searchResults
                    delegate: Item {
                        width: resGrid.cardWidth
                        height: resCover.height + 98

                        Rectangle {
                            id: resCover
                            width: parent.width
                            height: Math.floor(parent.width * 1.38)
                            color: resCardArea.pressed ? "#E0E0E0" : "#F4F4F4"
                            border.width: 2
                            border.color: "#000000"
                            clip: true

                            Column {
                                anchors.centerIn: parent
                                width: parent.width - 36
                                spacing: 12
                                visible: resImg.status !== Image.Ready

                                Rectangle {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: 76
                                    height: 76
                                    color: "#FFFFFF"
                                    border.width: 2
                                    border.color: "#000000"

                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.title ? modelData.title.slice(0, 1).toUpperCase() : "M"
                                        font.pixelSize: 38
                                        font.bold: true
                                        color: "#000000"
                                    }
                                }

                                Text {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    text: modelData.title || ""
                                    font.pixelSize: 23
                                    font.bold: true
                                    color: "#000000"
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 4
                                    elide: Text.ElideRight
                                }
                            }

                            Image {
                                id: resImg
                                anchors.fill: parent
                                anchors.margins: 2
                                source: modelData.cover_path ? modelData.cover_path : ""
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: true
                            }

                            // 1-Tap Quick Library Toggle Badge (top-right)
                            Rectangle {
                                id: quickLibBtn
                                z: 5
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 10
                                width: modelData.in_library ? 132 : 56
                                height: 48
                                color: (modelData.in_library || quickLibArea.pressed) ? "#000000" : "#FFFFFF"
                                border.width: 2
                                border.color: modelData.in_library ? "#FFFFFF" : "#000000"

                                IconLabel {
                                    anchors.centerIn: parent
                                    icon: modelData.in_library ? "check" : ""
                                    text: modelData.in_library ? "Saved" : "+"
                                    pixelSize: modelData.in_library ? 19 : 28
                                    spacing: 6
                                    color: (modelData.in_library || quickLibArea.pressed) ? "#FFFFFF" : "#000000"
                                }

                                MouseArea {
                                    id: quickLibArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => {
                                        browseRoot.toggleLibrary(modelData, !modelData.in_library);
                                    }
                                }
                            }
                        }

                        Text {
                            id: rTitle
                            anchors.top: resCover.bottom
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
                            anchors.top: rTitle.bottom
                            anchors.topMargin: 4
                            width: parent.width
                            text: modelData.source_name || (modelData.source ? modelData.source.name : "") || modelData.source_id || ""
                            font.pixelSize: 19
                            color: "#555555"
                            elide: Text.ElideRight
                        }

                        MouseArea {
                            id: resCardArea
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.top: parent.top
                            anchors.topMargin: 64
                            onClicked: (mouse) => {
                                browseRoot.keyboardOpen = false;
                                browseRoot.openManga(modelData);
                            }
                        }
                    }
                }
            }
        }
    }

    // 4. Sources & User Source Lists Manager Modal
    Rectangle {
        visible: browseRoot.extensionsModalOpen
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: touchKeyboard.top
        color: "#FFFFFF"
        z: 50

        MouseArea {
            anchors.fill: parent
        }

        Column {
            id: modalCol
            anchors.fill: parent
            anchors.margins: 26
            spacing: 14

            // Modal Top Header
            Item {
                width: parent.width
                height: 68

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Manga Sources"
                    font.pixelSize: 32
                    font.bold: true
                    color: "#000000"
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12

                    Rectangle {
                        width: 176
                        height: 60
                        color: reloadModalArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        IconLabel {
                            anchors.centerIn: parent
                            icon: browseRoot.sourcesLoading ? "" : "refresh"
                            text: browseRoot.sourcesLoading ? "Fetching..." : "Refresh"
                            pixelSize: 21
                            color: reloadModalArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: reloadModalArea
                            anchors.fill: parent
                            onClicked: (mouse) => browseRoot.requestSourcesList()
                        }
                    }

                    Rectangle {
                        width: 156
                        height: 60
                        color: closeSrcArea.pressed ? "#FFFFFF" : "#000000"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: "Done"
                            font.pixelSize: 22
                            font.bold: true
                            color: closeSrcArea.pressed ? "#000000" : "#FFFFFF"
                        }
                        MouseArea {
                            id: closeSrcArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                browseRoot.keyboardOpen = false;
                                browseRoot.extensionsModalOpen = false;
                            }
                        }
                    }
                }
            }

            // Modal Primary Sub-Tabs: [Installed & Available] | [Source Lists (JSON)]
            Row {
                width: parent.width
                spacing: 12

                Rectangle {
                    id: tabCatBtn
                    readonly property bool activeSub: browseRoot.modalTab === "catalog"
                    width: Math.floor((parent.width - 12) / 2)
                    height: 62
                    color: (tabCatBtn.activeSub || tabCatArea.pressed) ? "#000000" : "#FFFFFF"
                    border.width: 2
                    border.color: "#000000"

                    Text {
                        anchors.centerIn: parent
                        text: "Installed & Available (" +
                              ((browseRoot.installedSources ? browseRoot.installedSources.length : 0) +
                               (browseRoot.availableSources ? browseRoot.availableSources.length : 0)) + ")"
                        font.pixelSize: 22
                        font.bold: true
                        color: (tabCatBtn.activeSub || tabCatArea.pressed) ? "#FFFFFF" : "#000000"
                    }
                    MouseArea {
                        id: tabCatArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            browseRoot.keyboardOpen = false;
                            browseRoot.modalTab = "catalog";
                        }
                    }
                }

                Rectangle {
                    id: tabCfgBtn
                    readonly property bool activeSub: browseRoot.modalTab === "config"
                    width: Math.floor((parent.width - 12) / 2)
                    height: 62
                    color: (tabCfgBtn.activeSub || tabCfgArea.pressed) ? "#000000" : "#FFFFFF"
                    border.width: 2
                    border.color: "#000000"

                    Text {
                        anchors.centerIn: parent
                        text: "Source Lists & Languages (JSON)"
                        font.pixelSize: 22
                        font.bold: true
                        color: (tabCfgBtn.activeSub || tabCfgArea.pressed) ? "#FFFFFF" : "#000000"
                    }
                    MouseArea {
                        id: tabCfgArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            browseRoot.keyboardOpen = false;
                            browseRoot.syncDraftFromSourceConfig();
                            browseRoot.modalTab = "config";
                        }
                    }
                }
            }

            // Error / Loading Banner inside Modal
            Rectangle {
                visible: browseRoot.sourcesLoading || browseRoot.sourcesError.length > 0
                width: parent.width
                height: visible ? 54 : 0
                color: "#F4F4F4"
                border.width: 2
                border.color: "#000000"

                Text {
                    anchors.centerIn: parent
                    width: parent.width - 28
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    text: browseRoot.sourcesLoading
                          ? "Fetching source lists & checking installed plugins..."
                          : ("Error: " + browseRoot.sourcesError)
                    font.pixelSize: 20
                    font.bold: true
                    color: "#000000"
                }
            }

            // TAB A: INSTALLED & AVAILABLE SOURCES CATALOG
            Column {
                id: catalogPane
                visible: browseRoot.modalTab === "catalog"
                width: parent.width
                height: parent.height - 68 - 62 - (browseRoot.sourcesLoading || browseRoot.sourcesError.length > 0 ? 68 : 0) - 28
                spacing: 12

                // Filter Sources Input
                Rectangle {
                    width: parent.width
                    height: 62
                    color: "#FFFFFF"
                    border.width: (browseRoot.keyboardOpen && browseRoot.activeInputTarget === "src_filter") ? 3 : 2
                    border.color: "#000000"

                    AppIcon {
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        name: "search"
                        width: 22
                        height: 22
                        color: "#000000"
                    }

                    TextInput {
                        id: filterSrcInput
                        anchors.left: parent.left
                        anchors.leftMargin: 52
                        anchors.right: clearFilterBtn.left
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 22
                        color: "#000000"
                        clip: true
                        text: browseRoot.catalogFilterText
                        onTextChanged: () => {
                            if (browseRoot.catalogFilterText !== filterSrcInput.text) {
                                browseRoot.catalogFilterText = filterSrcInput.text;
                            }
                        }
                        onAccepted: () => { browseRoot.keyboardOpen = false; }
                    }

                    Text {
                        visible: browseRoot.catalogFilterText.length === 0
                        anchors.left: parent.left
                        anchors.leftMargin: 52
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Filter available or installed sources by name or ID..."
                        font.pixelSize: 21
                        color: "#666666"
                    }

                    MouseArea {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.right: clearFilterBtn.left
                        onClicked: (mouse) => {
                            browseRoot.activeInputTarget = "src_filter";
                            filterSrcInput.forceActiveFocus();
                            browseRoot.keyboardOpen = true;
                        }
                    }

                    Rectangle {
                        id: clearFilterBtn
                        visible: browseRoot.catalogFilterText.length > 0
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 46
                        height: 46
                        color: "#F0F0F0"
                        border.width: 1
                        border.color: "#000000"

                        AppIcon {
                            anchors.centerIn: parent
                            name: "close"
                            width: 18
                            height: 18
                            color: "#000000"
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                browseRoot.catalogFilterText = "";
                                filterSrcInput.text = "";
                            }
                        }
                    }
                }

                // Empty Source Lists Prompt inside Catalog Tab
                Rectangle {
                    visible: !browseRoot.sourcesLoading &&
                             (!browseRoot.installedSources || browseRoot.installedSources.length === 0) &&
                             (!browseRoot.availableSources || browseRoot.availableSources.length === 0)
                    width: parent.width
                    height: 240
                    color: "#F8F8F8"
                    border.width: 2
                    border.color: "#000000"

                    Column {
                        anchors.centerIn: parent
                        spacing: 14

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "No Source Lists Configured"
                            font.pixelSize: 26
                            font.bold: true
                            color: "#000000"
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "Add an Aidoku index.min.json URL in the Source Lists tab to fetch installable sources."
                            font.pixelSize: 20
                            color: "#444444"
                        }

                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 340
                            height: 62
                            color: promptCfgArea.pressed ? "#FFFFFF" : "#000000"
                            border.width: 2
                            border.color: "#000000"

                            Text {
                                anchors.centerIn: parent
                                text: "Configure Source Lists  →"
                                font.pixelSize: 21
                                font.bold: true
                                color: promptCfgArea.pressed ? "#000000" : "#FFFFFF"
                            }
                            MouseArea {
                                id: promptCfgArea
                                anchors.fill: parent
                                onClicked: (mouse) => {
                                    browseRoot.syncDraftFromSourceConfig();
                                    browseRoot.modalTab = "config";
                                }
                            }
                        }
                    }
                }

                ListView {
                    id: srcListView
                    visible: model.length > 0
                    width: parent.width
                    height: parent.height - 74
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    spacing: 10

                    model: {
                        let combined = [];
                        let needle = browseRoot.catalogFilterText.trim().toLowerCase();
                        let availById = {};
                        if (browseRoot.availableSources) {
                            for (let k = 0; k < browseRoot.availableSources.length; ++k) {
                                availById[browseRoot.availableSources[k].id] = browseRoot.availableSources[k];
                            }
                        }

                        if (browseRoot.installedSources) {
                            for (let i = 0; i < browseRoot.installedSources.length; ++i) {
                                let s = Object.assign({}, browseRoot.installedSources[i]);
                                s._installed = true;
                                let avMatch = availById[s.id];
                                s._hasUpdate = Boolean(avMatch && Number(avMatch.version || 0) > Number(s.version || 0));
                                s._availVersion = avMatch ? avMatch.version : s.version;
                                let hay = ((s.name || "") + " " + (s.id || "")).toLowerCase();
                                if (needle.length === 0 || hay.indexOf(needle) >= 0) {
                                    combined.push(s);
                                }
                            }
                        }
                        if (browseRoot.availableSources) {
                            for (let j = 0; j < browseRoot.availableSources.length; ++j) {
                                let av = browseRoot.availableSources[j];
                                if (!browseRoot.isSourceInstalled(av.id)) {
                                    let copy = Object.assign({}, av);
                                    copy._installed = false;
                                    copy._hasUpdate = false;
                                    let hay2 = ((copy.name || "") + " " + (copy.id || "")).toLowerCase();
                                    if (needle.length === 0 || hay2.indexOf(needle) >= 0) {
                                        combined.push(copy);
                                    }
                                }
                            }
                        }
                        return combined;
                    }

                    delegate: Rectangle {
                        width: srcListView.width
                        height: 96
                        color: "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 22
                            anchors.right: srcActionsRow.left
                            anchors.rightMargin: 16
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4

                            Row {
                                spacing: 10

                                Text {
                                    text: (modelData.name || modelData.id) + (modelData.version ? ("  v" + modelData.version) : "")
                                    font.pixelSize: 24
                                    font.bold: true
                                    color: "#000000"
                                }

                                Rectangle {
                                    visible: modelData._installed
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: instBadgeTxt.implicitWidth + 18
                                    height: 30
                                    color: "#000000"

                                    Text {
                                        id: instBadgeTxt
                                        anchors.centerIn: parent
                                        text: "INSTALLED"
                                        font.pixelSize: 15
                                        font.bold: true
                                        color: "#FFFFFF"
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                text: modelData._hasUpdate
                                      ? ((modelData.id || "") + "  ·  Update v" + modelData._availVersion + " available")
                                      : (modelData.id || "")
                                font.pixelSize: 19
                                color: "#555555"
                                elide: Text.ElideRight
                            }
                        }

                        Row {
                            id: srcActionsRow
                            anchors.right: parent.right
                            anchors.rightMargin: 18
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 10

                            // 1-Tap Browse / Discover from this installed source
                            Rectangle {
                                visible: modelData._installed
                                width: 156
                                height: 58
                                color: browseSrcArea.pressed ? "#FFFFFF" : "#000000"
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.centerIn: parent
                                    text: "Browse →"
                                    font.pixelSize: 20
                                    font.bold: true
                                    color: browseSrcArea.pressed ? "#000000" : "#FFFFFF"
                                }
                                MouseArea {
                                    id: browseSrcArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => browseRoot.browseSpecificSource(modelData.id)
                                }
                            }

                            // Update button when newer .aix version is available
                            Rectangle {
                                visible: modelData._installed && modelData._hasUpdate
                                width: 130
                                height: 58
                                color: updSrcArea.pressed ? "#000000" : "#FFFFFF"
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.centerIn: parent
                                    text: "Update"
                                    font.pixelSize: 20
                                    font.bold: true
                                    color: updSrcArea.pressed ? "#FFFFFF" : "#000000"
                                }
                                MouseArea {
                                    id: updSrcArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => browseRoot.requestInstallSource(modelData.id, true)
                                }
                            }

                            // Install / Remove Button
                            Rectangle {
                                width: 150
                                height: 58
                                color: modelData._installed
                                       ? (srcActArea.pressed ? "#000000" : "#FFFFFF")
                                       : (srcActArea.pressed ? "#FFFFFF" : "#000000")
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.centerIn: parent
                                    text: modelData._installed ? "Remove" : "Install"
                                    font.pixelSize: 20
                                    font.bold: true
                                    color: modelData._installed
                                           ? (srcActArea.pressed ? "#FFFFFF" : "#000000")
                                           : (srcActArea.pressed ? "#000000" : "#FFFFFF")
                                }
                                MouseArea {
                                    id: srcActArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => {
                                        browseRoot.requestInstallSource(modelData.id, !modelData._installed);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // TAB B: USER SOURCE LISTS & LANGUAGES (VISUAL + RAW JSON EDITOR)
            Flickable {
                id: configFlick
                visible: browseRoot.modalTab === "config"
                width: parent.width
                height: parent.height - 68 - 62 - (browseRoot.sourcesLoading || browseRoot.sourcesError.length > 0 ? 68 : 0) - 28
                contentWidth: width
                contentHeight: configInnerCol.height + 32
                flickableDirection: Flickable.VerticalFlick
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: configInnerCol
                    width: parent.width
                    spacing: 18

                    // Mode Switcher: Visual List Builder vs Raw JSON Editor + Save CTA
                    Item {
                        width: parent.width
                        height: 62

                        Row {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 10

                            Rectangle {
                                id: visModeBtn
                                readonly property bool sel: browseRoot.configEditMode === "visual"
                                width: 210
                                height: 56
                                color: (visModeBtn.sel || visModeArea.pressed) ? "#000000" : "#FFFFFF"
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.centerIn: parent
                                    text: "Visual Builder"
                                    font.pixelSize: 20
                                    font.bold: true
                                    color: (visModeBtn.sel || visModeArea.pressed) ? "#FFFFFF" : "#000000"
                                }
                                MouseArea {
                                    id: visModeArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => {
                                        if (browseRoot.configEditMode === "json") {
                                            try {
                                                let parsed = JSON.parse(rawJsonEdit.text);
                                                if (parsed && Array.isArray(parsed.source_lists)) {
                                                    browseRoot.draftSourceLists = parsed.source_lists;
                                                }
                                                if (parsed && Array.isArray(parsed.languages) && parsed.languages.length > 0) {
                                                    browseRoot.draftLanguages = parsed.languages;
                                                }
                                            } catch (e) {}
                                        }
                                        browseRoot.keyboardOpen = false;
                                        browseRoot.configEditMode = "visual";
                                    }
                                }
                            }

                            Rectangle {
                                id: jsonModeBtn
                                readonly property bool sel: browseRoot.configEditMode === "json"
                                width: 210
                                height: 56
                                color: (jsonModeBtn.sel || jsonModeArea.pressed) ? "#000000" : "#FFFFFF"
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.centerIn: parent
                                    text: "{ } Raw JSON"
                                    font.pixelSize: 20
                                    font.bold: true
                                    color: (jsonModeBtn.sel || jsonModeArea.pressed) ? "#FFFFFF" : "#000000"
                                }
                                MouseArea {
                                    id: jsonModeArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => {
                                        browseRoot.draftRawJson = browseRoot.formatDraftJson(
                                            browseRoot.draftSourceLists,
                                            browseRoot.draftLanguages
                                        );
                                        rawJsonEdit.text = browseRoot.draftRawJson;
                                        browseRoot.configEditMode = "json";
                                    }
                                }
                            }
                        }

                        Row {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 12

                            Rectangle {
                                width: 220
                                height: 58
                                color: reloadDiskArea.pressed ? "#000000" : "#FFFFFF"
                                border.width: 2
                                border.color: "#000000"

                                IconLabel {
                                    anchors.centerIn: parent
                                    icon: "refresh"
                                    text: "Reload Disk File"
                                    pixelSize: 19
                                    color: reloadDiskArea.pressed ? "#FFFFFF" : "#000000"
                                }
                                MouseArea {
                                    id: reloadDiskArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => {
                                        browseRoot.keyboardOpen = false;
                                        browseRoot.requestSaveSourceConfig({ reload_disk: true });
                                        browseRoot.modalTab = "catalog";
                                    }
                                }
                            }

                            Rectangle {
                                width: 280
                                height: 58
                                color: saveCfgArea.pressed ? "#FFFFFF" : "#000000"
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.centerIn: parent
                                    text: "Save & Fetch Sources"
                                    font.pixelSize: 21
                                    font.bold: true
                                    color: saveCfgArea.pressed ? "#000000" : "#FFFFFF"
                                }
                                MouseArea {
                                    id: saveCfgArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => browseRoot.saveSourceConfigAction()
                                }
                            }
                        }
                    }

                    // VISUAL BUILDER MODE
                    Column {
                        visible: browseRoot.configEditMode === "visual"
                        width: parent.width
                        spacing: 16

                        Text {
                            text: "SOURCE LIST URLS (source_lists)"
                            font.pixelSize: 19
                            font.bold: true
                            font.letterSpacing: 1.2
                            color: "#333333"
                        }

                        // Add URL Input + Add Button
                        Row {
                            width: parent.width
                            spacing: 12

                            Rectangle {
                                width: parent.width - 156 - 12
                                height: 66
                                color: "#FFFFFF"
                                border.width: (browseRoot.keyboardOpen && browseRoot.activeInputTarget === "new_url") ? 3 : 2
                                border.color: "#000000"

                                TextInput {
                                    id: newUrlInput
                                    anchors.fill: parent
                                    anchors.leftMargin: 18
                                    anchors.rightMargin: 18
                                    verticalAlignment: TextInput.AlignVCenter
                                    font.pixelSize: 21
                                    color: "#000000"
                                    clip: true
                                    text: browseRoot.newUrlText
                                    onTextChanged: () => {
                                        if (browseRoot.newUrlText !== newUrlInput.text) {
                                            browseRoot.newUrlText = newUrlInput.text;
                                        }
                                    }
                                    onAccepted: () => browseRoot.addDraftUrl()
                                }

                                Text {
                                    visible: browseRoot.newUrlText.length === 0
                                    anchors.left: parent.left
                                    anchors.leftMargin: 18
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "https://.../index.min.json"
                                    font.pixelSize: 21
                                    color: "#777777"
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: (mouse) => {
                                        browseRoot.activeInputTarget = "new_url";
                                        newUrlInput.forceActiveFocus();
                                        browseRoot.keyboardOpen = true;
                                    }
                                }
                            }

                            Rectangle {
                                width: 156
                                height: 66
                                color: addUrlArea.pressed ? "#FFFFFF" : "#000000"
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.centerIn: parent
                                    text: "+ Add URL"
                                    font.pixelSize: 21
                                    font.bold: true
                                    color: addUrlArea.pressed ? "#000000" : "#FFFFFF"
                                }
                                MouseArea {
                                    id: addUrlArea
                                    anchors.fill: parent
                                    onClicked: (mouse) => browseRoot.addDraftUrl()
                                }
                            }
                        }

                        // Configured URLs List
                        Rectangle {
                            visible: !browseRoot.draftSourceLists || browseRoot.draftSourceLists.length === 0
                            width: parent.width
                            height: 82
                            color: "#F8F8F8"
                            border.width: 2
                            border.color: "#000000"

                            Text {
                                anchors.centerIn: parent
                                text: "No source list URLs added yet. Enter a repository index.min.json URL above."
                                font.pixelSize: 20
                                color: "#444444"
                            }
                        }

                        Repeater {
                            model: browseRoot.draftSourceLists
                            delegate: Rectangle {
                                width: configInnerCol.width
                                height: 74
                                color: "#F8F8F8"
                                border.width: 2
                                border.color: "#000000"

                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 18
                                    anchors.right: rmUrlBtn.left
                                    anchors.rightMargin: 14
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData
                                    font.pixelSize: 20
                                    color: "#000000"
                                    elide: Text.ElideMiddle
                                }

                                Rectangle {
                                    id: rmUrlBtn
                                    anchors.right: parent.right
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 126
                                    height: 50
                                    color: rmUrlArea.pressed ? "#000000" : "#FFFFFF"
                                    border.width: 2
                                    border.color: "#000000"

                                    Text {
                                        anchors.centerIn: parent
                                        text: "Remove"
                                        font.pixelSize: 19
                                        font.bold: true
                                        color: rmUrlArea.pressed ? "#FFFFFF" : "#000000"
                                    }
                                    MouseArea {
                                        id: rmUrlArea
                                        anchors.fill: parent
                                        onClicked: (mouse) => browseRoot.removeDraftUrlAt(index)
                                    }
                                }
                            }
                        }

                        // Languages Multi-Select Grid
                        Text {
                            text: "LANGUAGES (languages)"
                            font.pixelSize: 19
                            font.bold: true
                            font.letterSpacing: 1.2
                            color: "#333333"
                        }

                        Flow {
                            width: parent.width
                            spacing: 10

                            Repeater {
                                model: browseRoot.commonLanguageCodes
                                delegate: Rectangle {
                                    id: langChipBtn
                                    readonly property bool langOn: browseRoot.draftLanguages && browseRoot.draftLanguages.indexOf(modelData) >= 0
                                    width: 96
                                    height: 54
                                    color: (langChipBtn.langOn || langChipArea.pressed) ? "#000000" : "#FFFFFF"
                                    border.width: 2
                                    border.color: "#000000"

                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData
                                        font.pixelSize: 21
                                        font.bold: true
                                        color: (langChipBtn.langOn || langChipArea.pressed) ? "#FFFFFF" : "#000000"
                                    }
                                    MouseArea {
                                        id: langChipArea
                                        anchors.fill: parent
                                        onClicked: (mouse) => browseRoot.toggleDraftLanguage(modelData)
                                    }
                                }
                            }
                        }
                    }

                    // RAW JSON EDITOR MODE
                    Column {
                        visible: browseRoot.configEditMode === "json"
                        width: parent.width
                        spacing: 12

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: "Enter or edit your Aidoku settings.json object directly (must contain \"source_lists\" array and \"languages\" array):"
                            font.pixelSize: 20
                            color: "#333333"
                        }

                        Rectangle {
                            width: parent.width
                            height: 290
                            color: "#FFFFFF"
                            border.width: (browseRoot.keyboardOpen && browseRoot.activeInputTarget === "raw_json") ? 3 : 2
                            border.color: "#000000"

                            TextEdit {
                                id: rawJsonEdit
                                anchors.fill: parent
                                anchors.margins: 18
                                font.pixelSize: 21
                                font.family: "monospace"
                                color: "#000000"
                                wrapMode: TextEdit.WrapAnywhere
                                text: browseRoot.draftRawJson
                                onTextChanged: () => {
                                    if (browseRoot.draftRawJson !== rawJsonEdit.text) {
                                        browseRoot.draftRawJson = rawJsonEdit.text;
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: (mouse) => {
                                    browseRoot.activeInputTarget = "raw_json";
                                    rawJsonEdit.forceActiveFocus();
                                    let pos = rawJsonEdit.positionAt(mouse.x - 18, mouse.y - 18);
                                    if (pos >= 0) rawJsonEdit.cursorPosition = pos;
                                    browseRoot.keyboardOpen = true;
                                }
                            }
                        }
                    }

                    // SSH / On-Device File Path Hint Box
                    Rectangle {
                        width: parent.width
                        height: 92
                        color: "#F4F4F4"
                        border.width: 1
                        border.color: "#000000"

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: 16
                            spacing: 4

                            Text {
                                text: "Tip: You can also edit settings.json over SSH and tap \"Reload Disk File\":"
                                font.pixelSize: 18
                                font.bold: true
                                color: "#222222"
                            }

                            Text {
                                width: parent.width
                                text: (browseRoot.sourceConfig && browseRoot.sourceConfig.config_path)
                                      ? browseRoot.sourceConfig.config_path
                                      : "data/aidoku/settings.json"
                                font.pixelSize: 18
                                font.family: "monospace"
                                color: "#444444"
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }
            }
        }
    }

    // 5. Shared Built-in Touch E-Ink QWERTY + URL/JSON Keyboard (sits above both Browse and Sources Modal)
    TouchKeyboard {
        id: touchKeyboard
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        active: browseRoot.keyboardOpen
        urlMode: browseRoot.activeInputTarget === "new_url" || browseRoot.activeInputTarget === "raw_json"
        submitLabel: {
            if (browseRoot.activeInputTarget === "new_url") return "Add URL";
            if (browseRoot.activeInputTarget === "raw_json") return "Save JSON";
            if (browseRoot.activeInputTarget === "src_filter") return "Done";
            return "Search";
        }
        z: 80

        onKeyTyped: (txt) => {
            if (browseRoot.activeInputTarget === "new_url") {
                newUrlInput.insert(newUrlInput.cursorPosition, txt);
            } else if (browseRoot.activeInputTarget === "raw_json") {
                rawJsonEdit.insert(rawJsonEdit.cursorPosition, txt);
            } else if (browseRoot.activeInputTarget === "src_filter") {
                filterSrcInput.insert(filterSrcInput.cursorPosition, txt);
            } else {
                searchInput.insert(searchInput.cursorPosition, txt);
            }
        }
        onBackspace: () => {
            if (browseRoot.activeInputTarget === "new_url") {
                if (newUrlInput.cursorPosition > 0) {
                    newUrlInput.remove(newUrlInput.cursorPosition - 1, newUrlInput.cursorPosition);
                }
            } else if (browseRoot.activeInputTarget === "raw_json") {
                if (rawJsonEdit.cursorPosition > 0) {
                    rawJsonEdit.remove(rawJsonEdit.cursorPosition - 1, rawJsonEdit.cursorPosition);
                }
            } else if (browseRoot.activeInputTarget === "src_filter") {
                if (filterSrcInput.cursorPosition > 0) {
                    filterSrcInput.remove(filterSrcInput.cursorPosition - 1, filterSrcInput.cursorPosition);
                }
            } else {
                if (searchInput.cursorPosition > 0) {
                    searchInput.remove(searchInput.cursorPosition - 1, searchInput.cursorPosition);
                }
            }
        }
        onClearAll: () => {
            if (browseRoot.activeInputTarget === "new_url") {
                browseRoot.newUrlText = "";
                newUrlInput.text = "";
            } else if (browseRoot.activeInputTarget === "raw_json") {
                rawJsonEdit.text = "{\n  \"source_lists\": [\n    \"\"\n  ],\n  \"languages\": [\"en\"]\n}";
                rawJsonEdit.cursorPosition = 27;
            } else if (browseRoot.activeInputTarget === "src_filter") {
                browseRoot.catalogFilterText = "";
                filterSrcInput.text = "";
            } else {
                browseRoot.queryText = "";
                searchInput.text = "";
            }
        }
        onSubmitted: () => {
            if (browseRoot.activeInputTarget === "new_url") {
                browseRoot.addDraftUrl();
                browseRoot.keyboardOpen = false;
            } else if (browseRoot.activeInputTarget === "raw_json") {
                browseRoot.saveSourceConfigAction();
            } else if (browseRoot.activeInputTarget === "src_filter") {
                browseRoot.keyboardOpen = false;
            } else {
                browseRoot.submitSearch();
            }
        }
        onDismissed: () => {
            browseRoot.keyboardOpen = false;
            searchInput.focus = false;
            newUrlInput.focus = false;
            rawJsonEdit.focus = false;
            filterSrcInput.focus = false;
        }
    }
}
