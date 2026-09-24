import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    width: 1100
    height: 720
    visible: true
    color: "#101418"
    title: "Sorting Camera"

    function toggleConnection() {
        if (stream.active) {
            stream.stop()
        } else {
            stream.start(hostField.text, 81)
            deviceStatus.startPolling(hostField.text, 80)
        }
    }

    header: ToolBar {
        height: 48

        background: Rectangle {
            color: "#171c22"
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 10

            Label {
                text: "Sorting Camera"
                color: "#e6edf3"
                font.pixelSize: 16
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            TextField {
                id: hostField
                text: "192.168.4.1"
                placeholderText: "Camera AP IP (default 192.168.4.1)"
                color: "#e6edf3"
                placeholderTextColor: "#6b7684"
                width: 240
                font.pixelSize: 13
                background: Rectangle {
                    color: "#0d1117"
                    border.color: "#30363d"
                    radius: 4
                }
                onAccepted: root.toggleConnection()
            }

            Button {
                text: stream.active ? "Disconnect" : "Connect"
                onClicked: root.toggleConnection()
            }
        }
    }

    footer: ToolBar {
        height: 34

        background: Rectangle {
            color: "#171c22"
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 18

            Label {
                text: frameBus.fps.toFixed(1) + " fps"
                color: "#3fb950"
                font.pixelSize: 12
                font.family: "Consolas"
            }
            Label {
                text: "recv " + stream.framesReceived
                color: "#8b949e"
                font.pixelSize: 12
                font.family: "Consolas"
            }
            Label {
                text: "drop " + stream.framesDropped
                color: stream.framesDropped > 0 ? "#f85149" : "#8b949e"
                font.pixelSize: 12
                font.family: "Consolas"
            }
            Label {
                text: (stream.bytesReceived / 1048576).toFixed(1) + " MB"
                color: "#8b949e"
                font.pixelSize: 12
                font.family: "Consolas"
            }

            Item { Layout.fillWidth: true }

            Label {
                text: deviceStatus.online ? "device online" : "device offline"
                color: deviceStatus.online ? "#3fb950" : "#f85149"
                font.pixelSize: 12
            }
            Label {
                visible: deviceStatus.online
                text: "rssi " + (deviceStatus.status["rssi"] !== undefined ? deviceStatus.status["rssi"] : "?") + " dBm"
                color: "#8b949e"
                font.pixelSize: 12
                font.family: "Consolas"
            }
            Label {
                visible: deviceStatus.online
                text: deviceStatus.status["resolution"] !== undefined ? deviceStatus.status["resolution"] : ""
                color: "#8b949e"
                font.pixelSize: 12
                font.family: "Consolas"
            }
            Label {
                visible: deviceStatus.online
                text: "fw " + (deviceStatus.status["fw_version"] !== undefined ? deviceStatus.status["fw_version"] : "")
                color: "#8b949e"
                font.pixelSize: 12
                font.family: "Consolas"
            }
        }
    }

    Item {
        anchors.fill: parent

        RowLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 8

            Image {
                id: frameImage
                Layout.fillWidth: true
                Layout.fillHeight: true
                fillMode: Image.PreserveAspectFit
                cache: false
                source: "image://frame/live?v=" + frameBus.version
                asynchronous: false
            }

            Rectangle {
                id: configPanel
                Layout.fillHeight: true
                Layout.preferredWidth: 250
                visible: deviceStatus.online
                color: "#171c22"
                radius: 6
                border.color: "#30363d"

                ListModel {
                    id: resModel
                    ListElement { key: "qqvga"; dims: "160x120"; label: "160 × 120 (QQVGA)" }
                    ListElement { key: "qvga";  dims: "320x240"; label: "320 × 240 (QVGA)" }
                    ListElement { key: "vga";   dims: "640x480"; label: "640 × 480 (VGA)" }
                    ListElement { key: "svga";  dims: "800x600"; label: "800 × 600 (SVGA)" }
                    ListElement { key: "xga";   dims: "1024x768"; label: "1024 × 768 (XGA)" }
                    ListElement { key: "hd";    dims: "1280x720"; label: "1280 × 720 (HD)" }
                    ListElement { key: "sxga";  dims: "1280x1024"; label: "1280 × 1024 (SXGA)" }
                    ListElement { key: "uxga";  dims: "1600x1200"; label: "1600 × 1200 (UXGA)" }
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    Label {
                        text: "Camera settings"
                        color: "#e6edf3"
                        font.pixelSize: 14
                        font.bold: true
                    }

                    Label {
                        text: "Resolution"
                        color: "#8b949e"
                        font.pixelSize: 12
                    }

                    ComboBox {
                        id: resCombo
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        model: resModel
                        textRole: "label"
                        enabled: deviceStatus.online && !stream.active && !deviceStatus.configBusy

                        function syncFromStatus() {
                            const r = deviceStatus.status["resolution"]
                            for (let i = 0; i < resModel.count; i++) {
                                if (resModel.get(i).dims === r) {
                                    if (currentIndex !== i)
                                        currentIndex = i
                                    return
                                }
                            }
                        }

                        onActivated: (index) => deviceStatus.setResolution(resModel.get(index).key)
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: stream.active
                        Label {
                            Layout.fillWidth: true
                            text: "Stop the stream to change settings"
                            color: "#d29922"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: "JPEG quality: " + Math.round(qualitySlider.value)
                        color: "#8b949e"
                        font.pixelSize: 12
                    }

                    Slider {
                        id: qualitySlider
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        from: 0
                        to: 63
                        stepSize: 1
                        enabled: deviceStatus.online && !stream.active && !deviceStatus.configBusy

                        function syncFromStatus() {
                            const q = deviceStatus.status["quality"]
                            if (q !== undefined && !pressed && Math.round(value) !== q)
                                value = q
                        }

                        onMoved: deviceStatus.setQuality(Math.round(value))
                    }

                    Label {
                        Layout.fillWidth: true
                        text: "Lower = better image, larger files"
                        color: "#6b7684"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: deviceStatus.configError.length > 0
                        text: deviceStatus.configError
                        color: "#f85149"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }

                    Item { Layout.fillHeight: true }
                }

                Connections {
                    target: deviceStatus
                    function onStatusChanged() {
                        resCombo.syncFromStatus()
                        qualitySlider.syncFromStatus()
                    }
                }

                Component.onCompleted: {
                    resCombo.syncFromStatus()
                    qualitySlider.syncFromStatus()
                }
            }
        }

        Label {
            anchors.centerIn: parent
            visible: frameImage.status !== Image.Ready || frameBus.version === 0
            text: stream.active ? "Waiting for frames…" : "Enter camera IP and press Connect"
            color: "#6b7684"
            font.pixelSize: 18
        }

        Label {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 14
            visible: stream.errorString.length > 0
            text: stream.errorString
            color: "#f85149"
            font.pixelSize: 12
            font.family: "Consolas"
        }
    }
}
