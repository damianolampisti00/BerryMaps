import bb.cascades 1.3
import bb.system 1.2

// BerryMaps main screen, laid out after the BlackBerry 10 UI Guidelines
// (PROGETTO.md §20):
//  - content is king: the map fills the screen, the action bar floats over it
//    and a single tap hides/shows it (immersive view);
//  - gestures instead of buttons (pinch, double tap, long press; I/O keys);
//  - signature action = Search (key S); Location (key M); Info in the
//    application menu (swipe down);
//  - search results grow from the bottom, next to the text field (thumb);
//  - transient errors as 3 s toasts with a fix, activity only after 3 s;
//  - sizes in design units (1 du = 9 px on the Q5).
// Gestures go to the C++ MapController ("map"), search to PlacesClient
// ("places"). Cascades has no QtQuick `Connections`.
Page {
    id: page

    property bool pinching: false
    property real lastX: 0
    property real lastY: 0
    property real lastT: 0
    property real velX: 0
    property real velY: 0
    property bool searching: false
    property bool placeShown: false
    property string placeName: ""
    property string placeAddress: ""
    property real placeLat: 0
    property real placeLon: 0
    // What a search result / long press is for: a place, or the route's start.
    property string searchTarget: "place"
    property bool originIsMe: true
    property string originName: "La mia posizione"
    property real originLat: 0
    property real originLon: 0
    // Space taken by the floating action bar at the bottom.
    property real chromeHeight: actionBarVisibility == ChromeVisibility.Overlay ? ui.du(14) : 0

    function openOriginSearch() {
        page.searchTarget = "origin";
        page.openSearch();
    }

    function openSearch() {
        var c = map.centerLatLon();
        places.setBias(c.lat, c.lon);
        page.searching = true;
        page.actionBarVisibility = ChromeVisibility.Hidden;
        searchField.text = "";
        sugModel.clear();
        searchField.requestFocus();
    }

    function closeSearch() {
        page.searching = false;
        places.cancel();
        searchField.text = "";
        searchField.loseFocus();
        page.actionBarVisibility = ChromeVisibility.Overlay;
    }

    function showPlace(name, address, lat, lon) {
        page.placeName = name;
        page.placeAddress = address;
        page.placeLat = lat;
        page.placeLon = lon;
        page.placeShown = true;
    }

    function closePlace() {
        page.placeShown = false;
        map.clearPin();
    }

    // Start of the route: the GPS position, or a place/point chosen by the user.
    function originPoint() {
        if (!page.originIsMe) return { lat: page.originLat, lon: page.originLon };
        if (!map.hasFix()) {
            if (!map.locating) map.locateMe();
            toast.body = "Attendo la tua posizione: riprova quando compare il punto blu, oppure scegli un'altra partenza.";
            toast.show();
            return null;
        }
        return map.myPosition();
    }

    function planRoute(mode) {
        var o = page.originPoint();
        if (!o) return;
        modeControl.selectedIndex = mode == "WALK" ? 1 : (mode == "BICYCLE" ? 2 : (mode == "TRANSIT" ? 3 : 0));
        routing.plan(o.lat, o.lon, page.placeLat, page.placeLon, mode);
    }

    function replanRoute() {
        var o = page.originPoint();
        if (o) routing.plan(o.lat, o.lon, page.placeLat, page.placeLon, modeControl.selectedValue);
    }

    function useMyPosition() {
        page.originIsMe = true;
        page.originName = "La mia posizione";
        if (page.searching) page.closeSearch();
        page.replanRoute();
    }

    function applyTime() {
        var kinds = ["now", "depart", "arrive"];
        routing.setTime(kinds[timeKind.selectedIndex], timePicker.value);
    }

    function closeRoute() {
        routing.clear();
        page.originIsMe = true;
        page.originName = "La mia posizione";
        timeKind.selectedIndex = 0;
        page.closePlace();
    }

    function distanceText(lat, lon) {
        var d = map.distanceTo(lat, lon);
        if (d < 0) return "";
        if (d < 1000) return "A " + Math.round(d) + " m da te";
        return "A " + (d / 1000).toFixed(d < 10000 ? 1 : 0).replace(".", ",") + " km da te";
    }

    actionBarVisibility: ChromeVisibility.Overlay

    Menu.definition: MenuDefinition {
        helpAction: HelpActionItem {
            title: "Info"
            onTriggered: infoSheet.open()
        }
    }

    actions: [
        ActionItem {
            title: "Cerca"
            enabled: !nav.active
            imageSource: "asset:///ic_search.png"
            ActionBar.placement: ActionBarPlacement.Signature
            shortcuts: [ Shortcut { key: "s" } ]
            onTriggered: page.openSearch()
        },
        ActionItem {
            title: map.following ? "Segue" : "Posizione"
            imageSource: map.following ? "asset:///ic_location_follow.png" : "asset:///ic_location.png"
            ActionBar.placement: ActionBarPlacement.OnBar
            shortcuts: [ Shortcut { key: "m" } ]
            onTriggered: map.locateMe()
        },
        InvokeActionItem {
            title: "Condividi"
            enabled: page.placeShown
            ActionBar.placement: ActionBarPlacement.InOverflow
            query {
                mimeType: "text/plain"
                invokeActionId: "bb.action.SHARE"
            }
            onTriggered: {
                data = page.placeName + "\n" + page.placeAddress +
                    "\nhttps://www.openstreetmap.org/?mlat=" + page.placeLat.toFixed(6) +
                    "&mlon=" + page.placeLon.toFixed(6) + "#map=17/" +
                    page.placeLat.toFixed(6) + "/" + page.placeLon.toFixed(6);
            }
        }
    ]

    // Zoom keys suggested by the guidelines; pinch and double tap are the
    // on-screen alternatives.
    shortcuts: [
        Shortcut { key: "i"; onTriggered: map.zoomIn() },
        Shortcut { key: "o"; onTriggered: map.zoomOut() }
    ]

    onCreationCompleted: {
        Application.themeSupport.setPrimaryColor(Color.create("#1a73e8"));
        map.attach(mapHost);
        map.toast.connect(function (text) {
            toast.body = text;
            toast.show();
        });
        places.error.connect(function (text) {
            toast.body = text;
            toast.show();
        });
        places.suggestionsChanged.connect(function () {
            sugModel.clear();
            sugModel.append(places.suggestions);
        });
        routing.error.connect(function (text) {
            toast.body = text;
            toast.show();
        });
        // Guidance: no action bar (glanceable screen, no complex input).
        nav.activeChanged.connect(function () {
            page.actionBarVisibility = nav.active ? ChromeVisibility.Hidden : ChromeVisibility.Overlay;
            // Active Frame: next manoeuvre while guiding, the normal snapshot otherwise.
            if (nav.active) Application.setCover(navCover);
            else Application.resetCover();
        });
        places.placeResolved.connect(function (name, address, lat, lon) {
            var target = page.searchTarget;
            page.searchTarget = "place";
            if (target == "origin") {
                if (page.searching) page.closeSearch();
                page.originIsMe = false;
                page.originName = name == "Punto selezionato" ? address : name;
                page.originLat = lat;
                page.originLon = lon;
                page.replanRoute();
                return;
            }
            if (page.searching) {
                page.closeSearch();
                map.showPin(lat, lon, true);
            }
            page.showPlace(name, address, lat, lon);
        });
    }

    Container {
        layout: DockLayout {}
        background: Color.create("#f2efe9")

        Container {
            id: mapHost
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            layout: AbsoluteLayout {}
            clipContentToBounds: true

            attachedObjects: [
                LayoutUpdateHandler {
                    onLayoutFrameChanged: map.setViewport(layoutFrame.width, layoutFrame.height)
                }
            ]

            onTouch: {
                if (page.pinching) return;
                var now = Date.now();
                if (event.isDown()) {
                    page.lastX = event.windowX;
                    page.lastY = event.windowY;
                    page.lastT = now;
                    page.velX = 0;
                    page.velY = 0;
                    map.panStart();
                } else if (event.isMove()) {
                    var dx = event.windowX - page.lastX;
                    var dy = event.windowY - page.lastY;
                    var dt = Math.max(1, now - page.lastT);
                    // Smoothed velocity in px/ms for the fling.
                    page.velX = 0.6 * (dx / dt) + 0.4 * page.velX;
                    page.velY = 0.6 * (dy / dt) + 0.4 * page.velY;
                    page.lastX = event.windowX;
                    page.lastY = event.windowY;
                    page.lastT = now;
                    map.panBy(dx, dy);
                } else if (event.isUp()) {
                    // A pause before lifting the finger means no fling.
                    if (now - page.lastT > 80) {
                        page.velX = 0;
                        page.velY = 0;
                    }
                    map.panEnd(page.velX, page.velY);
                } else {
                    map.panEnd(0, 0);
                }
            }

            gestureHandlers: [
                PinchHandler {
                    onPinchStarted: {
                        page.pinching = true;
                        map.pinchStart(event.midPointX, event.midPointY);
                    }
                    onPinchUpdated: map.pinchUpdate(event.pinchRatio)
                    onPinchEnded: {
                        map.pinchEnd();
                        page.pinching = false;
                    }
                    onPinchCancelled: {
                        map.pinchEnd();
                        page.pinching = false;
                    }
                },
                DoubleTapHandler {
                    onDoubleTapped: map.zoomInAt(event.x, event.y)
                },
                // Long press: "what is here" (reverse geocoding).
                LongPressHandler {
                    onLongPressed: {
                        if (page.searching || nav.active) return;
                        var p = map.screenToLatLon(event.x, event.y);
                        if (routing.hasRoute) {
                            // With a route open, a long press picks a new start.
                            page.searchTarget = "origin";
                            page.originName = "Cerco l'indirizzo…";
                            places.reverseGeocode(p.lat, p.lon);
                            return;
                        }
                        map.showPin(p.lat, p.lon, false);
                        page.showPlace("Punto selezionato", "Cerco l'indirizzo…", p.lat, p.lon);
                        places.reverseGeocode(p.lat, p.lon);
                    }
                },
                // Immersive view: a tap hides or shows the action bar.
                TapHandler {
                    onTapped: {
                        if (page.searching || nav.active) return;
                        page.actionBarVisibility =
                            page.actionBarVisibility == ChromeVisibility.Overlay
                                ? ChromeVisibility.Hidden : ChromeVisibility.Overlay;
                    }
                }
            ]
        }

        // Bottom-left, inline: GPS state and, after 3 s of waiting, activity.
        Container {
            horizontalAlignment: HorizontalAlignment.Left
            verticalAlignment: VerticalAlignment.Bottom
            visible: !page.searching && !page.placeShown
            leftPadding: ui.du(1)
            bottomPadding: page.chromeHeight + ui.du(4)
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

            ActivityIndicator {
                visible: map.loading
                running: map.loading
                preferredWidth: ui.du(5)
                preferredHeight: ui.du(5)
                verticalAlignment: VerticalAlignment.Center
            }
            Container {
                visible: map.locationStatus != ""
                verticalAlignment: VerticalAlignment.Center
                background: Color.create("#e6ffffff")
                leftPadding: ui.du(1)
                rightPadding: ui.du(1)
                Label {
                    text: map.locationStatus
                    textStyle.base: SystemDefaults.TextStyles.SmallText
                    textStyle.color: Color.create("#1a73e8")
                }
            }
        }

        // Required attribution (OpenStreetMap licence, CARTO free tier).
        Container {
            horizontalAlignment: HorizontalAlignment.Right
            verticalAlignment: VerticalAlignment.Bottom
            visible: !page.searching && !page.placeShown
            bottomPadding: page.chromeHeight
            Container {
                background: Color.create("#b3ffffff")
                leftPadding: ui.du(1)
                rightPadding: ui.du(1)
                Label {
                    text: map.attribution
                    textStyle.base: SystemDefaults.TextStyles.SubtitleText
                    textStyle.color: Color.create("#3c4043")
                }
            }
        }

        // Place card (search result or long press), above the action bar.
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
            visible: page.placeShown && !page.searching && !routing.hasRoute
            bottomPadding: page.chromeHeight
            Container {
                horizontalAlignment: HorizontalAlignment.Fill
                background: Color.White
                leftPadding: ui.du(2)
                rightPadding: ui.du(1)
                topPadding: ui.du(1.5)
                bottomPadding: ui.du(1.5)
                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                Container {
                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                    Label {
                        text: page.placeName
                        textStyle.base: SystemDefaults.TextStyles.TitleText
                        textStyle.color: Color.create("#202124")
                    }
                    Label {
                        text: page.placeAddress
                        multiline: true
                        textStyle.base: SystemDefaults.TextStyles.BodyText
                        textStyle.color: Color.create("#5f6368")
                    }
                    Label {
                        text: page.distanceText(page.placeLat, page.placeLon) +
                              (map.distanceTo(page.placeLat, page.placeLon) >= 0 ? "  ·  " : "") + "Google Maps"
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#80868b")
                    }
                }
                Container {
                    verticalAlignment: VerticalAlignment.Top
                    Button {
                        text: "\u2715"
                        preferredWidth: ui.du(11)
                        horizontalAlignment: HorizontalAlignment.Right
                        onClicked: page.closePlace()
                    }
                    Button {
                        text: "Indicazioni"
                        appearance: ControlAppearance.Primary
                        enabled: !routing.busy
                        preferredWidth: ui.du(22)
                        onClicked: page.planRoute("DRIVE")
                    }
                }
            }
        }

        // Route preview: mode, summary, steps, start (dark like the theme).
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
            visible: (routing.hasRoute || routing.busy) && !nav.active && !page.searching
            bottomPadding: page.chromeHeight
            Container {
                horizontalAlignment: HorizontalAlignment.Fill
                background: Color.create("#202124")
                leftPadding: ui.du(1.5)
                rightPadding: ui.du(1.5)
                topPadding: ui.du(1)
                bottomPadding: ui.du(1.5)
                SegmentedControl {
                    id: modeControl
                    Option { text: "Auto"; value: "DRIVE" }
                    Option { text: "A piedi"; value: "WALK" }
                    Option { text: "Bici"; value: "BICYCLE" }
                    Option { text: "Mezzi"; value: "TRANSIT" }
                    onSelectedIndexChanged: {
                        if (!routing.hasRoute || routing.mode == selectedValue) return;
                        var o = page.originPoint();
                        if (o) routing.changeMode(o.lat, o.lon, selectedValue);
                    }
                }
                // Start: GPS position, a searched place, or a long-pressed point.
                Button {
                    horizontalAlignment: HorizontalAlignment.Fill
                    text: "Da: " + page.originName
                    onClicked: page.openOriginSearch()
                }
                // Public transport: leave now, leave at..., arrive by...
                Container {
                    visible: modeControl.selectedValue == "TRANSIT"
                    SegmentedControl {
                        id: timeKind
                        Option { text: "Adesso"; value: "now" }
                        Option { text: "Partenza"; value: "depart" }
                        Option { text: "Arrivo"; value: "arrive" }
                        onSelectedIndexChanged: page.applyTime()
                    }
                    DateTimePicker {
                        id: timePicker
                        visible: timeKind.selectedIndex > 0
                        mode: DateTimePickerMode.DateTime
                        title: timeKind.selectedIndex == 2 ? "Arrivo entro" : "Partenza alle"
                        value: new Date()
                        onValueChanged: if (timeKind.selectedIndex > 0) page.applyTime()
                    }
                }
                Label {
                    text: "A: " + page.placeName
                    textStyle.base: SystemDefaults.TextStyles.BodyText
                    textStyle.color: Color.create("#bdc1c6")
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                    Label {
                        text: routing.busy ? "Calcolo il percorso\u2026" : routing.summary
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        verticalAlignment: VerticalAlignment.Center
                        textStyle.base: SystemDefaults.TextStyles.TitleText
                        textStyle.color: Color.White
                    }
                    ActivityIndicator {
                        running: routing.busy
                        visible: routing.busy
                        preferredWidth: ui.du(5)
                        preferredHeight: ui.du(5)
                        verticalAlignment: VerticalAlignment.Center
                    }
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                    topPadding: ui.du(1)
                    Button {
                        text: "Avvia"
                        appearance: ControlAppearance.Primary
                        enabled: routing.hasRoute && !routing.busy
                        layoutProperties: StackLayoutProperties { spaceQuota: 2 }
                        onClicked: nav.start()
                    }
                    Button {
                        text: "Passi"
                        enabled: routing.hasRoute
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        onClicked: stepsSheet.open()
                    }
                    Button {
                        text: "\u2715"
                        preferredWidth: ui.du(11)
                        onClicked: page.closeRoute()
                    }
                }
            }
        }

        // Guidance: big glanceable banner on top (safety requirements).
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Top
            visible: nav.active
            background: nav.degraded ? Color.create("#b06000") : Color.create("#0b57d0")
            leftPadding: ui.du(2)
            rightPadding: ui.du(2)
            topPadding: ui.du(1.5)
            bottomPadding: ui.du(1.5)
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            Label {
                text: nav.glyph
                verticalAlignment: VerticalAlignment.Center
                textStyle.fontSize: FontSize.PointValue
                textStyle.fontSizeValue: 22
                textStyle.color: Color.White
            }
            Container {
                leftPadding: ui.du(2)
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                verticalAlignment: VerticalAlignment.Center
                Label {
                    visible: nav.distanceText != ""
                    text: nav.distanceText
                    textStyle.fontSize: FontSize.PointValue
                    textStyle.fontSizeValue: 12
                    textStyle.fontWeight: FontWeight.Bold
                    textStyle.color: Color.White
                }
                Label {
                    text: nav.instruction
                    multiline: true
                    textStyle.base: SystemDefaults.TextStyles.TitleText
                    textStyle.color: Color.White
                }
            }
        }
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
            visible: nav.active
            background: Color.create("#202124")
            leftPadding: ui.du(2)
            rightPadding: ui.du(1)
            topPadding: ui.du(1)
            bottomPadding: ui.du(1)
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            Label {
                text: nav.remainingText
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                verticalAlignment: VerticalAlignment.Center
                textStyle.base: SystemDefaults.TextStyles.BodyText
                textStyle.color: Color.White
            }
            // Voice prompts on/off (BerryAssistant server, see VoiceGuide).
            Button {
                visible: voice.available
                text: voice.enabled ? "Silenzia" : "Voce"
                preferredWidth: ui.du(18)
                onClicked: voice.enabled = !voice.enabled
            }
            Button {
                text: nav.arrived ? "Fine" : "Termina"
                preferredWidth: ui.du(20)
                onClicked: {
                    nav.stop();
                    if (nav.arrived) page.closeRoute();
                }
            }
        }

        // Search panel: suggestions grow upwards from the text field, so the
        // best match is the one nearest the thumb (guidelines).
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
            visible: page.searching
            // Dark like the app theme: StandardListItem text is white in the
            // dark theme (a white panel made the suggestions unreadable).
            background: Color.create("#202124")

            ListView {
                id: suggestionList
                preferredHeight: ui.du(44)
                layout: StackListLayout { orientation: LayoutOrientation.BottomToTop }
                dataModel: ArrayDataModel { id: sugModel }
                listItemComponents: [
                    ListItemComponent {
                        StandardListItem {
                            title: ListItemData.main
                            description: ListItemData.secondary
                        }
                    }
                ]
                onTriggered: places.choose(indexPath[0])
            }
            Container {
                horizontalAlignment: HorizontalAlignment.Right
                rightPadding: ui.du(2)
                Label {
                    text: "Google Maps"
                    textStyle.base: SystemDefaults.TextStyles.SmallText
                    textStyle.color: Color.create("#9aa0a6")
                }
            }
            Container {
                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                leftPadding: ui.du(1)
                rightPadding: ui.du(1)
                bottomPadding: ui.du(1)
                TextField {
                    id: searchField
                    hintText: page.searchTarget == "origin" ? "Cerca il punto di partenza" : "Cerca luoghi o indirizzi"
                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                    verticalAlignment: VerticalAlignment.Center
                    input.submitKey: SubmitKey.Search
                    input.onSubmitted: {
                        // Enter = first (nearest) suggestion.
                        if (sugModel.size() > 0) places.choose(0);
                    }
                    onTextChanging: places.autocomplete(text)
                }
                ActivityIndicator {
                    running: places.busy
                    visible: places.busy
                    preferredWidth: ui.du(5)
                    preferredHeight: ui.du(5)
                    verticalAlignment: VerticalAlignment.Center
                }
                Button {
                    text: page.searchTarget == "origin" ? "Qui" : "Annulla"
                    preferredWidth: ui.du(18)
                    verticalAlignment: VerticalAlignment.Center
                    // Choosing the start: "Qui" = back to the GPS position.
                    onClicked: {
                        if (page.searchTarget == "origin") {
                            page.searchTarget = "place";
                            page.useMyPosition();
                        } else {
                            page.closeSearch();
                        }
                    }
                }
            }
        }

        // Persistent problems only (missing key, daily cap), with the fix.
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Top
            visible: map.status != ""
            background: Color.create("#e6202124")
            leftPadding: ui.du(2)
            rightPadding: ui.du(2)
            topPadding: ui.du(1)
            bottomPadding: ui.du(1)
            Label {
                text: map.status
                multiline: true
                textStyle.base: SystemDefaults.TextStyles.SmallText
                textStyle.color: Color.White
            }
        }
    }

    attachedObjects: [
        SystemToast {
            id: toast
        },
        // Active Frame during guidance (720x720 devices: 310x211, title footer
        // added by the system). Updated with the guidance, nothing else.
        SceneCover {
            id: navCover
            content: Container {
                background: nav.degraded ? Color.create("#b06000") : Color.create("#0b57d0")
                leftPadding: ui.du(1.5)
                rightPadding: ui.du(1.5)
                topPadding: ui.du(1)
                layout: StackLayout {}
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                    Label {
                        text: nav.glyph
                        verticalAlignment: VerticalAlignment.Center
                        textStyle.fontSize: FontSize.PointValue
                        textStyle.fontSizeValue: 14
                        textStyle.color: Color.White
                    }
                    Label {
                        text: nav.distanceText
                        leftMargin: ui.du(1)
                        verticalAlignment: VerticalAlignment.Center
                        textStyle.fontSize: FontSize.PointValue
                        textStyle.fontSizeValue: 10
                        textStyle.fontWeight: FontWeight.Bold
                        textStyle.color: Color.White
                    }
                }
                Label {
                    text: nav.instruction
                    multiline: true
                    textStyle.base: SystemDefaults.TextStyles.BodyText
                    textStyle.color: Color.White
                }
                Label {
                    text: nav.remainingText
                    textStyle.base: SystemDefaults.TextStyles.SmallText
                    textStyle.color: Color.create("#d2e3fc")
                }
            }
        },
        Sheet {
            id: stepsSheet
            Page {
                titleBar: TitleBar {
                    title: "Indicazioni"
                    dismissAction: ActionItem {
                        title: "Chiudi"
                        onTriggered: stepsSheet.close()
                    }
                }
                Container {
                    Label {
                        text: routing.summary + "  \u00b7  Google Maps"
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        leftPadding: ui.du(2)
                    }
                    ListView {
                        dataModel: ArrayDataModel { id: stepsModel }
                        listItemComponents: [
                            ListItemComponent {
                                StandardListItem {
                                    title: ListItemData.instruction
                                    description: ListItemData.distance
                                }
                            }
                        ]
                    }
                }
                onCreationCompleted: {
                    routing.routeChanged.connect(function () {
                        stepsModel.clear();
                        stepsModel.append(routing.steps);
                    });
                }
            }
        },
        Sheet {
            id: infoSheet
            Page {
                titleBar: TitleBar {
                    title: "Info"
                    dismissAction: ActionItem {
                        title: "Chiudi"
                        onTriggered: infoSheet.close()
                    }
                }
                ScrollView {
                    Container {
                        leftPadding: ui.du(2)
                        rightPadding: ui.du(2)
                        topPadding: ui.du(2)
                        bottomPadding: ui.du(2)
                        Label {
                            text: "BerryMaps " + map.version
                            textStyle.base: SystemDefaults.TextStyles.TitleText
                        }
                        Header { title: "Gesti" }
                        Label {
                            multiline: true
                            text: "Trascina per spostare la mappa, lancia per scorrere.\nPizzica o tocca due volte per ingrandire.\nTieni premuto per sapere l'indirizzo di un punto.\nIndicazioni (nella scheda di un luogo) calcola il percorso; Avvia la guida.\nDurante la guida: indicazioni a voce (Silenzia per spegnerle) e prossima manovra anche nell'Active Frame.\nUn tocco mostra o nasconde la barra in basso."
                        }
                        Header { title: "Tastiera" }
                        Label {
                            multiline: true
                            text: "S  cerca\nI  ingrandisci\nO  riduci\nM  la mia posizione\nInvio (nella ricerca)  primo risultato"
                        }
                        Header { title: "Dati" }
                        Label {
                            multiline: true
                            text: "Mappa: © OpenStreetMap contributors, © CARTO.\nRicerca, indirizzi, percorsi e orari dei mezzi: Google Maps."
                        }
                    }
                }
            }
        }
    ]
}
