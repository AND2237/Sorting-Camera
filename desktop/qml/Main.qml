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

    property int controlPort: 80
    property int streamPort: 81

    function applyDevice(index) {
        const d = discovery.deviceAt(index)
        if (!d || !d.address)
            return
        console.log("[qml] select device", d.deviceId, d.address, d.controlPort, d.streamPort)
        hostField.text = d.address
        root.controlPort = d.controlPort
        root.streamPort = d.streamPort
    }

    function toggleConnection() {
        if (stream.active || stream.connecting || stream.reconnecting) {
            console.log("[qml] disconnect")
            stream.stop()
            deviceStatus.stopPolling()
        } else {
            console.log("[qml] connect", hostField.text, root.controlPort, root.streamPort)
            stream.start(hostField.text, root.streamPort)
            deviceStatus.startPolling(hostField.text, root.controlPort)
        }
    }

    Connections {
        target: discovery
        function onCountChanged() {
            if (discovery.count === 0)
                return
            if (stream.active || stream.connecting || stream.reconnecting)
                return
            if (hostField.text !== "192.168.4.1")
                return
            console.log("[qml] auto-selecting discovered device")
            root.applyDevice(0)
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

            ComboBox {
                id: devicePicker
                Layout.preferredWidth: 250
                Layout.preferredHeight: 30
                model: discovery
                textRole: "name"
                displayText: {
                    if (discovery.count === 0)
                        return discovery.scanning ? "Searching…" : "No device found"
                    const d = discovery.deviceAt(currentIndex)
                    if (!d || !d.address)
                        return discovery.count + " device(s)"
                    return d.name + "  (" + d.address + ")"
                }
                onActivated: (index) => root.applyDevice(index)
                enabled: !stream.active && !stream.connecting && !stream.reconnecting

                delegate: ItemDelegate {
                    required property string name
                    required property string address
                    required property string firmware
                    required property string sensor
                    required property bool authRequired
                    width: devicePicker.width
                    highlighted: devicePicker.highlightedIndex === index
                    contentItem: Column {
                        spacing: 1
                        Label {
                            text: name
                            font.pixelSize: 13
                            color: highlighted ? "#101418" : "#e6edf3"
                        }
                        Label {
                            text: address + "  ·  " + sensor + "  ·  " + firmware
                                  + (authRequired ? "  ·  sign-in" : "")
                            font.pixelSize: 11
                            color: highlighted ? "#30363d" : "#8b949e"
                        }
                    }
                    background: Rectangle {
                        color: highlighted ? "#3fb950" : (hovered ? "#21262d" : "transparent")
                    }
                }
            }

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
                text: stream.active ? "Disconnect"
                     : stream.reconnecting ? "Cancel (" + stream.retryAttempt + "/5)"
                     : stream.connecting ? "Connecting…" : "Connect"
                onClicked: root.toggleConnection()
            }

            Button {
                text: "Recover camera"
                visible: deviceStatus.online && !stream.active
                onClicked: deviceStatus.requestCameraRecovery()
                ToolTip.visible: hovered
                ToolTip.text: "Re-initialise the camera (fixes a wedged sensor)"
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
                text: discovery.count > 0
                      ? discovery.count + " device(s)"
                      : (discovery.scanning ? "searching…" : "no device found")
                color: discovery.count > 0 ? "#3fb950" : "#8b949e"
                font.pixelSize: 12
            }

            Label {
                id: sessionLabel
                text: sessionState.label
                color: sessionState.severity === "ok" ? "#3fb950"
                    : sessionState.severity === "warn" ? "#d29922"
                    : sessionState.severity === "err" ? "#f85149"
                    : "#8b949e"
                font.pixelSize: 12
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    ToolTip.delay: 300
                    ToolTip.visible: containsMouse && sessionState.detail.length > 0
                    ToolTip.text: sessionState.detail
                }
            }
            Label {
                text: "[" + sessionState.state + "]"
                color: "#6b7684"
                font.pixelSize: 11
                font.family: "Consolas"
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
                visible: deviceStatus.polling
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

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: deviceStatus.authRequired || !deviceStatus.authenticated

                        Label {
                            text: deviceStatus.authenticated ? "Authenticated" : "Sign in required"
                            color: deviceStatus.authenticated ? "#3fb950" : "#d29922"
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !deviceStatus.authenticated
                            text: deviceStatus.authError.length > 0
                                  ? deviceStatus.authError
                                  : "The camera requires a control password."
                            color: deviceStatus.authError.length > 0 ? "#f85149" : "#8b949e"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }

                        TextField {
                            id: passwordField
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            visible: !deviceStatus.authenticated
                            echoMode: TextInput.Password
                            placeholderText: "Control password"
                            enabled: !deviceStatus.authBusy
                            color: "#e6edf3"
                            placeholderTextColor: "#6b7684"
                            onAccepted: signInButton.clicked()
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: !deviceStatus.authenticated
                            spacing: 8

                            Button {
                                id: signInButton
                                text: deviceStatus.authBusy ? "Signing in…" : "Sign in"
                                enabled: passwordField.text.length > 0 && !deviceStatus.authBusy
                                onClicked: deviceStatus.signIn(passwordField.text,
                                                                rememberCheck.checked)
                            }

                            CheckBox {
                                id: rememberCheck
                                text: "Remember"
                                enabled: deviceStatus.credentialStorageAvailable
                                checked: true
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: deviceStatus.credentialStored
                                     && !deviceStatus.authenticated
                            text: deviceStatus.credentialStorageAvailable
                                  ? "A saved password is available for this camera."
                                  : "Password storage is not available on this system."
                            color: "#6b7684"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#30363d"
                        visible: deviceStatus.authRequired || !deviceStatus.authenticated
                    }

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
                        enabled: deviceStatus.online && !stream.active && !stream.connecting
                                 && !stream.reconnecting && !deviceStatus.configBusy

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
                              + (deviceStatus.status["quality_floor"] !== undefined
                                 ? "  (device floor " + deviceStatus.status["quality_floor"] + ")" : "")
                        color: "#8b949e"
                        font.pixelSize: 12
                    }

                    Slider {
                        id: qualitySlider
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        from: deviceStatus.status["quality_floor"] !== undefined
                              ? deviceStatus.status["quality_floor"] : 0
                        to: 63
                        stepSize: 1
                        enabled: deviceStatus.online && !stream.active && !stream.connecting
                                 && !stream.reconnecting && !deviceStatus.configBusy

                        function syncFromStatus() {
                            const q = deviceStatus.status["quality"]
                            if (q !== undefined && !pressed && Math.round(value) !== q)
                                value = q
                        }

                        onMoved: qualityDebounce.restart()
                        onPressedChanged: {
                            if (!pressed)
                                qualityDebounce.restart()
                        }

                        Timer {
                            id: qualityDebounce
                            interval: 400
                            onTriggered: {
                                if (!qualitySlider.pressed)
                                    deviceStatus.setQuality(Math.round(qualitySlider.value))
                            }
                        }
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
            text: stream.recoveryHint.length > 0 ? stream.recoveryHint
                : sessionState.state === "discovering" ? "Searching for a camera…"
                : sessionState.state === "error" ? sessionState.detail
                : sessionState.label + "…"
            color: sessionState.severity === "err" ? "#f85149" : "#6b7684"
            font.pixelSize: 18
        }

        Label {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 14
            visible: sessionState.state === "reconnecting"
            text: "reconnecting… (" + sessionState.retryAttempt + "/"
                  + sessionState.maxRetries + ") in "
                  + sessionState.retryDelayMs + " ms"
            color: "#d29922"
            font.pixelSize: 12
            font.family: "Consolas"
        }

        Label {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 14
            visible: deviceStatus.online
                 && deviceStatus.status["camera_recoveries"] !== undefined
                 && deviceStatus.status["camera_recoveries"] > 0
            text: "camera recoveries: " + deviceStatus.status["camera_recoveries"]
            color: "#d29922"
            font.pixelSize: 12
            font.family: "Consolas"
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
