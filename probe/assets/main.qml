import bb.cascades 1.3

// BerryProbe: buttons on top, live log below (newest line first). The full log
// is in /accounts/1000/shared/misc/berryprobe.log.
// Cascades has no QtQuick `Connections`: signals are connected in
// onCreationCompleted.
Page {
    id: page

    function addLine(s) {
        var t = s + "\n" + logArea.text;
        logArea.text = t.length > 12000 ? t.substring(0, 12000) : t;
    }

    titleBar: TitleBar {
        title: "BerryProbe"
    }

    Container {
        layout: StackLayout {}

        ScrollView {
            preferredHeight: 300
            scrollViewProperties.scrollMode: ScrollMode.Vertical

            Container {
                leftPadding: 10
                rightPadding: 10
                topPadding: 6

                Button {
                    text: "Tutti i test rapidi"
                    horizontalAlignment: HorizontalAlignment.Fill
                    onClicked: probe.runAllQuick(ivBox)
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                    Button { text: "Dispositivo"; onClicked: probe.runDeviceInfo() }
                    Button { text: "GPU"; onClicked: probe.runGlTest() }
                    Button { text: "Rete"; onClicked: probe.runNetTest() }
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                    Button { text: "Decodifica"; onClicked: probe.runDecodeTest() }
                    Button { text: "ImageView"; onClicked: probe.runImageViewTest(ivBox) }
                    Button { text: "Sensori"; onClicked: probe.runSensorTest() }
                }
                Label {
                    text: "GPS (uno alla volta, meglio all'aperto)"
                    textStyle.base: SystemDefaults.TextStyles.SmallText
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                    Button { text: "Normale"; onClicked: probe.startGps("default") }
                    Button { text: "Satelliti"; onClicked: probe.startGps("gnss") }
                    Button { text: "A freddo"; onClicked: probe.startGps("cold") }
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                    Button { text: "SUPL"; onClicked: probe.startGps("supl") }
                    Button { text: "Cella"; onClicked: probe.startGps("cellsite") }
                    Button { text: "Wi-Fi"; onClicked: probe.startGps("wifi") }
                }
                Button {
                    text: "Ferma GPS"
                    enabled: probe.gpsRunning
                    horizontalAlignment: HorizontalAlignment.Fill
                    onClicked: probe.stopGps()
                }
            }
        }

        // Filled with 40 small ImageViews during the ImageView test.
        Container {
            id: ivBox
            visible: false
            preferredHeight: 150
            layout: GridLayout { columnCount: 10 }
        }

        TextArea {
            id: logArea
            editable: false
            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.fontFamily: "Courier"
        }
    }

    actions: [
        ActionItem {
            title: probe.batteryLogging ? "Ferma registro batteria" : "Registro batteria"
            ActionBar.placement: ActionBarPlacement.OnBar
            onTriggered: probe.toggleBatteryLog()
        },
        ActionItem {
            title: "Pulisci schermo"
            ActionBar.placement: ActionBarPlacement.OnBar
            onTriggered: logArea.text = ""
        }
    ]

    onCreationCompleted: {
        probe.logLine.connect(page.addLine);
        probe.imageViewTestRunning.connect(function (running) {
            ivBox.visible = running;
        });
        probe.runDeviceInfo();
    }
}
