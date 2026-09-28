import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: root

    width: 1100
    height: 720
    visible: true
    color: "#101418"
    title: "Sorting Camera"

    property int controlPort: 80
    property int streamPort: 81
    property string selectedDeviceId: ""
    // What the host field held at startup, so auto-selection only happens
    // while the user has not typed an address of their own.
    property string initialHost: prefs.host
    // Display-only zoom (23). Scaling happens in the rendering path; the
    // frame in FrameBus - and therefore every snapshot and recording - is
    // untouched.
    property real zoom: 1.0
    property real panX: 0
    property real panY: 0

    function clampZoom(v) { return Math.min(12, Math.max(1, v)) }

    function nudgeZoom(factor) {
        zoom = clampZoom(zoom * factor)
        if (zoom === 1.0) {
            panX = 0
            panY = 0
        }
    }

    function toggleFullScreen() {
        visibility = (visibility === Window.FullScreen) ? Window.Windowed
                                                        : Window.FullScreen
    }

    function deviceMeta() {
        const m = {
            "host": hostField.text,
            "control_port": root.controlPort,
            "stream_port": root.streamPort,
            "app_version": Qt.application.version
        }
        const idx = devicePicker.currentIndex
        if (idx >= 0) {
            const d = discovery.deviceAt(idx)
            if (d && d.deviceId) {
                m["device_id"] = d.deviceId
                m["device_name"] = d.name
                m["firmware"] = d.firmware
                m["sensor"] = d.sensor
            }
        }
        const st = deviceStatus.status
        if (st["framesize"] !== undefined) m["framesize"] = st["framesize"]
        if (st["quality"] !== undefined) m["quality"] = st["quality"]
        return m
    }

    function applyDevice(index) {
        const d = discovery.deviceAt(index)
        if (!d || !d.address)
            return
        console.log("[qml] select device", d.deviceId, d.address, d.controlPort, d.streamPort)
        hostField.text = d.address
        root.controlPort = d.controlPort
        root.streamPort = d.streamPort
        root.selectedDeviceId = d.deviceId
        prefs.deviceId = d.deviceId
        prefs.host = d.address
        prefs.controlPort = d.controlPort
        prefs.streamPort = d.streamPort
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
            prefs.host = hostField.text
            prefs.controlPort = root.controlPort
            prefs.streamPort = root.streamPort
        }
    }

    Connections {
        target: discovery
        function onCountChanged() {
            if (discovery.count === 0)
                return
            if (stream.active || stream.connecting || stream.reconnecting)
                return
            if (hostField.text !== root.initialHost)
                return
            // Prefer the camera this machine was last talking to: its address
            // can change, its device id cannot.
            let idx = prefs.deviceId.length > 0 ? discovery.indexOfDevice(prefs.deviceId) : -1
            if (idx < 0)
                idx = 0
            console.log("[qml] select remembered device, index", idx)
            root.applyDevice(idx)
        }
    }

    Shortcut {
        sequence: "F11"
        onActivated: root.toggleFullScreen()
    }
    Shortcut {
        sequence: "Esc"
        enabled: root.visibility === Window.FullScreen
        onActivated: root.visibility = Window.Windowed
    }
    Shortcut {
        sequence: "Ctrl+0"
        onActivated: {
            root.zoom = 1.0
            root.panX = 0
            root.panY = 0
        }
    }
    Shortcut {
        sequence: "Ctrl+Plus"
        onActivated: root.nudgeZoom(1.25)
    }
    Shortcut {
        sequence: "Ctrl+Minus"
        onActivated: root.nudgeZoom(0.8)
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
                text: prefs.host
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
                text: "Snapshot"
                enabled: snapshotWriter.available
                onClicked: {
                    const p = snapshotWriter.save(captureRoot + "/snapshots",
                                                  root.selectedDeviceId)
                    console.log("[qml] snapshot ->", p !== "" ? p : snapshotWriter.errorString)
                }
                ToolTip.visible: hovered
                ToolTip.text: snapshotWriter.available
                    ? "Save the current frame exactly as the camera sent it"
                    : "Waiting for a frame"
            }

            Button {
                text: recorder.recording ? "Stop recording" : "Record"
                enabled: stream.active || recorder.recording
                onClicked: {
                    if (recorder.recording) {
                        console.log("[qml] stop recording ->", recorder.path)
                        recorder.stop()
                    } else {
                        const ok = recorder.start(captureRoot + "/recordings", root.deviceMeta())
                        console.log("[qml] record ->", ok ? recorder.path : recorder.errorString)
                    }
                }
                contentItem: Row {
                    spacing: 6
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8; height: 8; radius: 4
                        color: recorder.recording ? "#f85149" : "#6b7684"
                    }
                    Label {
                        text: recorder.recording ? "Stop recording" : "Record"
                        font.pixelSize: 13
                    }
                }
                ToolTip.visible: hovered
                ToolTip.text: recorder.recording
                    ? recorder.framesWritten + " frames written to " + recorder.path
                    : "Record the incoming JPEGs without re-encoding"
            }

            Button {
                text: "Zoom reset"
                visible: root.zoom > 1
                onClicked: {
                    root.zoom = 1.0
                    root.panX = 0
                    root.panY = 0
                }
                ToolTip.visible: hovered
                ToolTip.text: "Back to fit-to-window"
            }

            Button {
                text: root.visibility === Window.FullScreen ? "Windowed" : "Fullscreen"
                onClicked: root.toggleFullScreen()
                ToolTip.visible: hovered
                ToolTip.text: "F11"
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
                visible: streamStats.hasFrame
                text: streamStats.bitrateMbps.toFixed(2) + " Mbps"
                color: "#8b949e"
                font.pixelSize: 12
                font.family: "Consolas"
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    ToolTip.visible: containsMouse
                    ToolTip.text: streamStats.bitrateBps.toFixed(0) + " B/s · "
                                  + (stream.bytesReceived / 1048576).toFixed(1) + " MB received"
                }
            }
            Label {
                visible: streamStats.hasFrame
                text: "age " + streamStats.frameAgeMs + " ms"
                color: streamStats.frameAgeMs > 500 ? "#d29922" : "#8b949e"
                font.pixelSize: 12
                font.family: "Consolas"
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    ToolTip.visible: containsMouse
                    ToolTip.text: "How old the picture on screen is"
                }
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
                visible: recorder.recording
                text: "REC " + recorder.framesWritten + " / "
                      + (recorder.bytesWritten / 1048576).toFixed(1) + " MB"
                color: "#f85149"
                font.pixelSize: 12
                font.family: "Consolas"
            }

            Label {
                visible: recorder.errorString.length > 0
                text: recorder.errorString
                color: "#f85149"
                font.pixelSize: 11
            }

            Label {
                visible: snapshotWriter.errorString.length > 0
                text: snapshotWriter.errorString
                color: "#f85149"
                font.pixelSize: 11
            }

            Label {
                visible: snapIndicatorTimer.running
                text: "saved " + snapshotWriter.lastPath
                color: "#3fb950"
                font.pixelSize: 11
            }

            Timer {
                id: snapIndicatorTimer
                interval: 5000
            }

            Connections {
                target: snapshotWriter
                function onSaved() { snapIndicatorTimer.restart() }
                function onErrorStringChanged() {
                    if (snapshotWriter.errorString.length === 0)
                        snapIndicatorTimer.stop()
                }
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

            Item {
                id: viewport
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Image {
                    id: frameImage
                    anchors.fill: parent
                    fillMode: Image.PreserveAspectFit
                    cache: false
                    source: "image://frame/live?v=" + frameBus.version
                    asynchronous: false
                    transform: [
                        Scale {
                            xScale: root.zoom
                            yScale: root.zoom
                            origin.x: viewport.width / 2
                            origin.y: viewport.height / 2
                        },
                        Translate {
                            x: root.panX
                            y: root.panY
                        }
                    ]
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: root.zoom > 1 ? Qt.SizeAllCursor : Qt.CrossCursor
                    property real lastX: 0
                    property real lastY: 0
                    onPressed: (mouse) => {
                        lastX = mouse.x
                        lastY = mouse.y
                    }
                    onPositionChanged: (mouse) => {
                        if (pressed && root.zoom > 1) {
                            root.panX += mouse.x - lastX
                            root.panY += mouse.y - lastY
                            lastX = mouse.x
                            lastY = mouse.y
                        }
                    }
                    onWheel: (wheel) => root.nudgeZoom(wheel.angleDelta.y > 0 ? 1.25 : 0.8)
                    onDoubleClicked: {
                        root.zoom = 1.0
                        root.panX = 0
                        root.panY = 0
                    }
                }

                Label {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: 12
                    visible: root.zoom > 1
                    text: root.zoom.toFixed(1) + "x"
                    color: "#e6edf3"
                    background: Rectangle {
                        color: "#99101418"
                        radius: 4
                        anchors.fill: parent
                        anchors.margins: -4
                    }
                    font.pixelSize: 12
                    font.family: "Consolas"
                }
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

                        onActivated: (index) => {
            const key = resModel.get(index).key
            deviceStatus.setResolution(key)
            prefs.framesize = key
        }
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
                if (!qualitySlider.pressed) {
                    const q = Math.round(qualitySlider.value)
                    deviceStatus.setQuality(q)
                    prefs.quality = q
                }
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
