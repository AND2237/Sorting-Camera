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
    property int profilePending: 0
    property int profilePendingApply: 0
    property string profileHint: ""
    // Section 38's panel, on demand rather than always-on: a permanent log
    // window costs space the picture needs and hides the thing it explains.
    property bool diagnosticsOpen: false
    // Section 26 controls, derived from what the camera advertises. Grouped
    // here rather than hard-coded so the panel follows the firmware.
    readonly property var controlGroups: {
        const c = deviceStatus.capabilities["controls"]
        if (!c)
            return []
        const byGroup = {}
        for (const name of Object.keys(c)) {
            const e = c[name]
            if (!e.supported)
                continue
            if (!byGroup[e.group])
                byGroup[e.group] = []
            byGroup[e.group].push({
                name: name, min: e.min, max: e.max, def: e.default,
                note: e.note !== undefined ? e.note : ""
            })
        }
        const order = ["image", "auto", "white balance", "flip", "sharpening"]
        const keys = Object.keys(byGroup)
        keys.sort((a, b) => {
            const ia = order.indexOf(a), ib = order.indexOf(b)
            if (ia === ib) return a < b ? -1 : 1
            if (ia < 0) return 1
            if (ib < 0) return -1
            return ia - ib
        })
        return keys.map(g => ({ group: g, items: byGroup[g] }))
    }
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

    // Profiles are ordered by measured image quality, so a step down the list
    // is always a real, measured trade rather than a guess.
    function applyProfile(index) {
        const keys = ["uxga", "hd", "svga", "qvga", "qvga"]
        const quals = [4, 12, 24, 12, 36]
        if (index <= 0 || index > keys.length)
            return
        deviceStatus.setResolution(keys[index - 1])
        deviceStatus.setQuality(quals[index - 1])
        prefs.framesize = keys[index - 1]
        prefs.quality = quals[index - 1]
        root.profileHint = ""
    }

    function requestProfile(index) {
        if (index === 0) {
            root.profilePending = 0
            root.profileHint = "Automatic keeps the largest measured profile that clears its "
                               + "floor. On this camera that is High Quality, the production default."
            return
        }
        root.profilePending = index
        if (stream.active || stream.connecting || stream.reconnecting) {
            // Shown rather than done: dropping a live stream without asking
            // would be a worse surprise than the slower frame rate the user
            // chose to keep.
            return
        }
        applyProfile(index)
        root.profilePending = 0
    }

    function formatUptime(seconds) {
        if (seconds === undefined)
            return "—"
        const s = Math.floor(seconds)
        const d = Math.floor(s / 86400)
        const h = Math.floor((s % 86400) / 3600)
        const m = Math.floor((s % 3600) / 60)
        if (d > 0)
            return d + " d " + h + " h"
        if (h > 0)
            return h + " h " + m + " m"
        if (m > 0)
            return m + " m " + (s % 60) + " s"
        return s + " s"
    }

    function formatBytes(n) {
        if (n === undefined)
            return "—"
        if (n >= 1048576)
            return (n / 1048576).toFixed(1) + " MB"
        if (n >= 1024)
            return (n / 1024).toFixed(0) + " kB"
        return n + " B"
    }

    function applyDevice(index) {
        const d = discovery.deviceAt(index)
        if (!d || !d.address)
            return
        console.log("[qml] select device", d.deviceId, d.address, d.controlPort, d.streamPort)
        // The registry owns the pipeline for this camera; selecting a different
        // camera re-points the QML names at that camera's objects.
        registry.acquireDevice(d.deviceId, d.address, d.controlPort, d.streamPort)
        registry.setActiveDevice(d.deviceId)
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
    // Section 38's diagnostics view, on the same key people already reach for
    // on a second monitor.
    Shortcut {
        sequence: "F12"
        onActivated: root.diagnosticsOpen = !root.diagnosticsOpen
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

    // ---- notifications (section 26) ----
    // Over the footer, newest first. Errors and warnings wait to be dismissed
    // because nobody has acknowledged them; informational cards expire on their
    // own, since a user does not need to be told twice that the camera was
    // found.
    Column {
        id: notifyColumn
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 44
        anchors.margins: 12
        width: Math.min(360, parent.width - 24)
        spacing: 6
        z: 50

        Repeater {
            model: notify
            delegate: Rectangle {
                id: notifyCard
                required property int index
                required property int severity
                required property string category
                required property string text
                required property bool sticky

                width: notifyColumn.width
                implicitHeight: notifyText.implicitHeight + 30
                radius: 6
                border.width: 1
                color: severity >= 3 ? "#3d1d1d" : (severity === 2 ? "#3a2f14" : "#1c2430")
                border.color: severity >= 3 ? "#f85149" : (severity === 2 ? "#d29922" : "#30363d")

                Label {
                    id: notifyText
                    anchors.left: parent.left
                    anchors.right: dismissButton.left
                    anchors.top: parent.top
                    anchors.margins: 9
                    text: notifyCard.text
                    color: "#e6edf3"
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 9
                    anchors.top: notifyText.bottom
                    anchors.topMargin: 2
                    text: notifyCard.category
                    color: "#6b7684"
                    font.pixelSize: 10
                    font.family: "Consolas"
                }

                ToolButton {
                    id: dismissButton
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 4
                    width: 22
                    height: 22
                    text: "×"
                    onClicked: notify.dismiss(notifyCard.index)
                }

                // Keeps the card from swallowing wheel events meant for the
                // picture underneath while still letting the button work.
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                }
            }
        }
    }

    // ---- diagnostics (section 38) ----
    Rectangle {
        id: diagPanel
        visible: root.diagnosticsOpen
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 44
        anchors.margins: 8
        width: 380
        height: Math.min(360, root.height - 90)
        color: "#171c22"
        radius: 6
        border.color: "#30363d"
        z: 60

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: "Diagnostics"
                    color: "#e6edf3"
                    font.pixelSize: 14
                    font.bold: true
                }
                Button {
                    text: "×"
                    onClicked: root.diagnosticsOpen = false
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "min level"
                    color: "#8b949e"
                    font.pixelSize: 11
                }
                ComboBox {
                    id: levelCombo
                    Layout.fillWidth: true
                    model: ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]
                    currentIndex: 0
                    onActivated: (index) => diagnostics.setMinimumLevel(index)
                }
            }

            Label {
                Layout.fillWidth: true
                text: diagnostics.warningCount() + " warnings, "
                      + diagnostics.errorCount() + " errors buffered"
                color: "#8b949e"
                font.pixelSize: 11
            }

            Label {
                Layout.fillWidth: true
                text: diagnostics.statisticsText().length > 0
                      ? diagnostics.statisticsText() : "no frame statistics yet"
                color: "#6b7684"
                font.pixelSize: 10
                font.family: "Consolas"
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            }

            ListView {
                id: diagList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                // Rebuilt on demand rather than held as a model: the log is a
                // debugging view of a bounded ring buffer, not something worth
                // keeping live in step with every entry.
                model: diagnostics.recentEntries(120)
                spacing: 3

                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label {
                        text: modelData[1]
                        color: modelData[0] >= 3 ? "#f85149"
                             : (modelData[0] === 2 ? "#d29922"
                             : (modelData[0] === 1 ? "#8b949e" : "#6b7684"))
                        font.pixelSize: 10
                        font.family: "Consolas"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData[2] + "  " + modelData[3]
                        color: "#c9d1d9"
                        font.pixelSize: 10
                        font.family: "Consolas"
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                }
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

                // The panel scrolls: with the full sensor control set there is
                // more content here than fits, and clipping the bottom would
                // hide controls the firmware says exist.
                Flickable {
                    id: configFlick
                    anchors.fill: parent
                    clip: true
                    contentWidth: width
                    contentHeight: innerCol.implicitHeight + 28
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    ColumnLayout {
                        id: innerCol
                        x: 14
                        y: 14
                        width: configFlick.width - 34
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

                    // ---- operating profile (section 12 / 26) ----
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: "#30363d"
                        }

                        Label {
                            Layout.fillWidth: true
                            text: "Operating profile"
                            color: "#e6edf3"
                            font.pixelSize: 14
                            font.bold: true
                        }

                        ComboBox {
                            id: profileCombo
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                            // Automatic plus the measured ladder, best image
                            // quality first.
                            model: ["Automatic"] + profiles.names
                            currentIndex: profiles.mode

                            onActivated: (index) => {
                                profiles.mode = index
                                root.requestProfile(index)
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: deviceStatus.online
                            text: profiles.activeIsCustom
                                  ? "Custom configuration"
                                  : profiles.activeName
                                    + "  -  measured " + profiles.activeMeasuredFps.toFixed(2)
                                    + " fps against a " + profiles.activeFloorFps.toFixed(0)
                                    + " fps floor"
                            color: deviceStatus.online && !profiles.activeMeetsFloor
                                  ? "#d29922" : "#8b949e"
                            font.pixelSize: 11
                            font.family: "Consolas"
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: deviceStatus.online && !profiles.activeEvidence.length === 0
                                     && !profiles.activeIsCustom
                            text: profiles.activeEvidence
                            color: "#6b7684"
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: profiles.advice.length > 0
                            text: profiles.advice
                            color: "#d29922"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.profileHint.length > 0
                            text: root.profileHint
                            color: "#6b7684"
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.profilePending > 0
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                text: "The camera refuses a configuration change while a "
                                      + "stream is attached, so applying this profile "
                                      + "restarts the stream."
                                color: "#d29922"
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                            }

                            Button {
                                text: "Stop and apply"
                                onClicked: {
                                    root.profilePendingApply = root.profilePending
                                    root.profilePending = 0
                                    stream.stop()
                                    deviceStatus.stopPolling()
                                    profileApplyTimer.restart()
                                }
                            }

                            Button {
                                text: "Cancel"
                                onClicked: {
                                    root.profilePending = 0
                                    profileCombo.currentIndex = profiles.mode
                                }
                            }
                        }

                        // The gap between stopping the stream and sending the new
                        // configuration is the reason this exists. The camera
                        // answers 409 while a stream client is still attached,
                        // and stopPolling() has to land before /config does.
                        Timer {
                            id: profileApplyTimer
                            interval: 900
                            onTriggered: {
                                if (root.profilePendingApply > 0) {
                                    applyProfile(root.profilePendingApply)
                                    root.profilePendingApply = 0
                                }
                                toggleConnection()
                            }
                        }
                    }

                    // ---- camera controls (section 26) ----
                    // Built entirely from /api/v1/capabilities. The firmware
                    // advertises which controls it supports and over what
                    // range, so the panel adds or drops a control when the
                    // camera does rather than keeping a list that can go
                    // stale against a different board.
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: root.controlGroups.length > 0

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: "#30363d"
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: "Camera controls"
                                color: "#e6edf3"
                                font.pixelSize: 14
                                font.bold: true
                            }
                            Button {
                                text: "Defaults"
                                enabled: !deviceStatus.sensorBusy
                                       && deviceStatus.online
                                onClicked: deviceStatus.resetSensorControls()
                            }
                        }

                        Repeater {
                            model: root.controlGroups
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 2

                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.group
                                    color: "#8b949e"
                                    font.pixelSize: 11
                                    font.capitalization: Font.AllUppercase
                                    elide: Text.ElideRight
                                }

                                Repeater {
                                    model: modelData.items
                                    delegate: ColumnLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: 0

                                        readonly property var def: modelData
                                        readonly property string cname: modelData.name
                                        readonly property real current: {
                                            const v = deviceStatus.status[cname]
                                            return v !== undefined ? v : modelData.def
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label {
                                                Layout.fillWidth: true
                                                text: cname.replace(/_/g, " ")
                                                color: "#e6edf3"
                                                font.pixelSize: 12
                                                elide: Text.ElideRight
                                            }
                                            Label {
                                                text: modelData.min + " … " + modelData.max
                                                color: "#6b7684"
                                                font.pixelSize: 10
                                                font.family: "Consolas"
                                            }
                                        }

                                        Slider {
                                            id: ctrlSlider
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 30
                                            from: modelData.min
                                            to: modelData.max
                                            stepSize: 1
                                            snapMode: Slider.SnapAlways
                                            value: parent.current
                                            // Live while streaming: unlike
                                            // resolution and quality, a sensor
                                            // write does not restart the
                                            // camera or the stream.
                                            enabled: deviceStatus.online
                                                     && !deviceStatus.sensorBusy
                                            onMoved: deviceStatus.setSensorControl(
                                                         cname, Math.round(value))
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            visible: modelData.note !== undefined
                                                     && modelData.note !== ""
                                            text: modelData.note !== undefined
                                                  ? modelData.note : ""
                                            color: "#6b7684"
                                            font.pixelSize: 10
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: deviceStatus.sensorError.length > 0
                        text: deviceStatus.sensorError
                        color: "#f85149"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }

                    // ---- device information (section 26) ----
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: deviceStatus.online

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: "#30363d"
                        }

                        Label {
                            Layout.fillWidth: true
                            text: "Device"
                            color: "#e6edf3"
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Repeater {
                            model: [
                                { k: "device",   v: root.selectedDeviceId.length > 0
                                               ? root.selectedDeviceId : "—" },
                                { k: "name",     v: deviceStatus.status["device_name"] },
                                { k: "address",  v: hostField.text + ":" + root.streamPort },
                                { k: "sensor",   v: deviceStatus.status["resolution"] !== undefined
                                               ? "OV2640" : "—" },
                                { k: "firmware", v: deviceStatus.status["fw_version"] },
                                { k: "protocol", v: deviceStatus.status["proto_version"] },
                                { k: "uptime",   v: root.formatUptime(deviceStatus.status["uptime_s"]) },
                                { k: "rssi",     v: deviceStatus.status["rssi"] + " dBm" },
                                { k: "free heap", v: root.formatBytes(deviceStatus.status["free_heap"]) },
                                { k: "psram free", v: root.formatBytes(deviceStatus.status["free_spiram"]) },
                                { k: "frames",   v: deviceStatus.status["frames_captured"] + " captured, "
                                                + deviceStatus.status["capture_failures"] + " failed" },
                                { k: "clients",  v: deviceStatus.status["stream_clients"] },
                                { k: "capture",  v: deviceStatus.status["avg_capture_ms"] + " ms avg" }
                            ]
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.k
                                    color: "#8b949e"
                                    font.pixelSize: 11
                                }
                                Label {
                                    text: modelData.v === undefined ? "—" : String(modelData.v)
                                    color: "#c9d1d9"
                                    font.pixelSize: 11
                                    font.family: "Consolas"
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: 150
                                }
                            }
                        }
                    }
                }

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
