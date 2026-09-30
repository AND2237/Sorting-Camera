import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: root

    width: 1100
    height: 720
    visible: true
    color: Theme.background
    title: "Sorting Camera"

    // Section 25's dark/light readiness. The mode is a user choice, so it
    // comes from preferences like any other setting rather than being baked
    // into the build. The design tokens themselves live in Theme.qml; this
    // file only binds them to what it draws.
    function toggleTheme() {
        Theme.toggleMode()
        prefs.themeMode = Theme.mode
    }

    // Qt Quick Controls style themselves from QPalette. Mapping the Theme
    // tokens onto the standard roles is what makes one switch restyle every
    // combo box, slider, text field and button, rather than only the surfaces
    // this file paints by hand. The values all live in Theme.qml; this is the
    // one place they meet the controls' own vocabulary.
    //
    // Only the standard QPalette colour *roles* exist as properties here -
    // there is no palette.disabledButtonText, because disabled is a group
    // rather than a role, and naming one fails the whole component load.
    palette.window: Theme.surface
    palette.windowText: Theme.text
    palette.base: Theme.surfaceSunken
    palette.alternateBase: Theme.surface
    palette.text: Theme.text
    palette.brightText: Theme.danger
    palette.button: Theme.surfaceRaised
    palette.buttonText: Theme.text
    palette.light: Theme.textSecondary
    palette.midlight: Theme.border
    palette.mid: Theme.textFaint
    palette.dark: Theme.border
    palette.shadow: Theme.scrim
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.textOnAccent
    palette.link: Theme.focusRing
    palette.linkVisited: Theme.focusRing
    palette.placeholderText: Theme.textFaint
    palette.toolTipBase: Theme.surfaceRaised
    palette.toolTipText: Theme.text
    palette.accent: Theme.accent

    property int controlPort: 80
    property int streamPort: 81
    property string selectedDeviceId: ""
    // What the host field held at startup, so auto-selection only happens
    // while the user has not typed an address of their own.
    property string initialHost: prefs.host
    property int profilePending: 0
    property int profilePendingApply: 0
    property string profileHint: ""
    // Connect waits for the configuration write it just started. The camera
    // answers 409 to /config while a stream client is attached, so the video
    // has to come second - and if the write never reports back, the guard
    // timer below still opens the stream rather than leaving a spinner.
    property bool pendingConnect: false
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

    // Printed once so a session log states what the selector actually offers.
    // It also pins the one expression that decides that list: JavaScript's
    // plus operator stringifies arrays, so building it any other way turns
    // six named profiles into sixty single-character entries. The theme is
    // applied from the same handler because a QML object has exactly one.
    Component.onCompleted: {
        Theme.setMode(prefs.themeMode)
        console.log("[qml] profiles:",
                    ["Automatic"].concat(profiles.names).join(" | "))
    }

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

    // Every setting comes from the engine's own table rather than a list kept
    // here, so a profile cannot end up labelled one thing and configured as
    // another. The index is 1-based over profiles.names, matching modeIndex.
    function applyProfile(index) {
        // The ladder is 1..N. An index off either end used to reach here from
        // the profile selector and send an empty framesize, which the firmware
        // answers with a bad-request error the user then had to dismiss.
        if (index < 1 || index > profiles.names.length) {
            console.log("[qml] ignoring profile index", index)
            return
        }
        const key = profiles.framesizeAt(index)
        if (!key)
            return
        deviceStatus.setResolution(key)
        deviceStatus.setQuality(profiles.qualityAt(index))
        const xclk = profiles.xclkAt(index)
        if (xclk > 0)
            deviceStatus.setXclk(xclk)
        const fb = profiles.frameBufferCountAt(index)
        if (fb > 0)
            deviceStatus.setFrameBufferCount(fb)
        const grab = profiles.grabModeAt(index)
        if (grab.length > 0)
            deviceStatus.setGrabMode(grab)
        prefs.framesize = key
        prefs.quality = profiles.qualityAt(index)
        prefs.xclkMhz = xclk
        prefs.frameBufferCount = fb
        prefs.grabMode = grab
        root.profileHint = ""
    }

    function requestProfile(index) {
        if (index < 0 || index > profiles.names.length)
            return
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
        // The control session opens when a camera is chosen, not when the
        // video starts. Device information, sign-in and the settings panel all
        // hang off it and none of them are video - and starting it here is
        // what leaves the panel usable after the stream is stopped.
        deviceStatus.startPolling(d.address, d.controlPort)
    }

    function startStreamNow() {
        stream.start(hostField.text, root.streamPort)
        prefs.host = hostField.text
        prefs.controlPort = root.controlPort
        prefs.streamPort = root.streamPort
    }

    function finishConnect() {
        if (!root.pendingConnect)
            return
        root.pendingConnect = false
        connectTimeout.stop()
        console.log("[qml] config settled, opening the stream")
        startStreamNow()
    }

    function toggleConnection() {
        if (stream.active || stream.connecting || stream.reconnecting) {
            console.log("[qml] disconnect")
            // Only the video stops. The control session stays up, because the
            // settings panel is what the user reaches for the moment they stop
            // the stream - stopping the polls at the same time is what used to
            // make the panel vanish exactly when it was needed.
            root.pendingConnect = false
            connectTimeout.stop()
            stream.stop()
            return
        }

        console.log("[qml] connect", hostField.text, root.controlPort, root.streamPort)
        // The control session first: a configuration write issued before the
        // host is known comes back as "not connected", and that error then
        // parks the whole session in degraded.
        deviceStatus.startPolling(hostField.text, root.controlPort)

        // In Automatic the policy is applied, not just described - but only
        // when the camera is not already there. Sending the profile it is
        // already running bought nothing and cost a request the firmware
        // rejects while a stream client is attached.
        if (profiles.mode === 0) {
            const rec = profiles.recommendedIndex()
            if (rec > 0 && deviceStatus.status["resolution"] !== undefined
                    && profiles.activeIndex !== rec) {
                console.log("[qml] automatic profile", profiles.recommendedName())
                applyProfile(rec)
            }
        }

        // The firmware answers 409 to /config while a stream client is
        // attached, so the video opens after the write it depends on - not
        // into the middle of it.
        if (deviceStatus.configBusy) {
            root.pendingConnect = true
            connectTimeout.restart()
            return
        }
        startStreamNow()
    }

    Timer {
        id: connectTimeout
        interval: 5000
        onTriggered: {
            // A configuration write that never reports back must not cost the
            // user the picture. The error card explains what happened.
            if (root.pendingConnect) {
                console.log("[qml] config did not settle in 5 s, opening the stream anyway")
                root.finishConnect()
            }
        }
    }

    Connections {
        target: deviceStatus
        function onConfigBusyChanged() {
            if (root.pendingConnect && !deviceStatus.configBusy)
                root.finishConnect()
        }
        function onConfigErrorChanged() {
            // A rejected setting is reported, not obeyed: the connection
            // continues so the user reads the message over a live picture
            // rather than a blank one.
            if (root.pendingConnect && !deviceStatus.configBusy)
                root.finishConnect()
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
        sequence: "Ctrl+T"
        onActivated: root.toggleTheme()
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
            color: Theme.surface
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: Theme.spaceLg

            Label {
                text: "Sorting Camera"
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            ComboBox {
                implicitHeight: Theme.touchTarget
                id: devicePicker
                // Accessibility (section 25 / gap G-12): every control below
                // carries an explicit name and role. Where the visible label is
                // an icon or an abbreviation ("×", "Use") the name says what the
                // control actually does, which is what a screen reader reads out.
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Discovered camera"
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
                    // Required, like the rest: a delegate that declares
                    // required properties has no injected context, so reading
                    // an undeclared "index" threw a ReferenceError every time
                    // the highlight moved and left the row unstyled.
                    required property int index
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
                            font.pixelSize: Theme.fontBody
                            color: highlighted ? Theme.textOnAccent : Theme.text
                        }
                        Label {
                            text: address + "  ·  " + sensor + "  ·  " + firmware
                                  + (authRequired ? "  ·  sign-in" : "")
                            font.pixelSize: Theme.fontCaption
                            color: highlighted ? Theme.border : Theme.textMuted
                        }
                    }
                    background: Rectangle {
                        color: highlighted ? Theme.success : (hovered ? Theme.surfaceRaised : "transparent")
                    }
                }
            }

            TextField {
                implicitHeight: Theme.touchTarget
                id: hostField
                Accessible.role: Accessible.EditableText
                Accessible.name: "Camera access point IP address"
                Accessible.description: "The camera's own access point; 192.168.4.1 by default"
                text: prefs.host
                placeholderText: "Camera AP IP (default 192.168.4.1)"
                color: Theme.text
                placeholderTextColor: Theme.textFaint
                width: 240
                font.pixelSize: Theme.fontBody
                background: Rectangle {
                    color: Theme.surfaceSunken
                    border.color: Theme.border
                    radius: 4
                }
                onAccepted: root.toggleConnection()
            }

            Button {
                implicitHeight: Theme.touchTarget
                text: stream.active ? "Disconnect"
                     : stream.reconnecting ? "Cancel (" + stream.retryAttempt + "/5)"
                     : stream.connecting ? "Connecting…" : "Connect"
                Accessible.role: Accessible.Button
                Accessible.name: text
                onClicked: root.toggleConnection()
            }

            Button {
                implicitHeight: Theme.touchTarget
                text: "Snapshot"
                Accessible.role: Accessible.Button
                Accessible.name: "Snapshot"
                Accessible.description: "Save the current frame exactly as the camera sent it"
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
                implicitHeight: Theme.touchTarget
                text: recorder.recording ? "Stop recording" : "Record"
                enabled: stream.active || recorder.recording
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.description: "Record the incoming JPEGs without re-encoding"
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
                    spacing: Theme.spaceSm
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8; height: 8; radius: 4
                        color: recorder.recording ? Theme.danger : Theme.textFaint
                    }
                    Label {
                        text: recorder.recording ? "Stop recording" : "Record"
                        font.pixelSize: Theme.fontBody
                    }
                }
                ToolTip.visible: hovered
                ToolTip.text: recorder.recording
                    ? recorder.framesWritten + " frames written to " + recorder.path
                    : "Record the incoming JPEGs without re-encoding"
            }

            Button {
                implicitHeight: Theme.touchTarget
                text: "Zoom reset"
                Accessible.role: Accessible.Button
                Accessible.name: "Zoom reset"
                Accessible.description: "Back to fit-to-window"
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
                implicitHeight: Theme.touchTarget
                text: root.visibility === Window.FullScreen ? "Windowed" : "Fullscreen"
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.description: "F11 toggles full screen"
                onClicked: root.toggleFullScreen()
                ToolTip.visible: hovered
                ToolTip.text: "F11"
            }

            Button {
                implicitHeight: Theme.touchTarget
                text: Theme.dark ? "Light theme" : "Dark theme"
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.description: "Switch between the dark and light palettes (Ctrl+T)"
                onClicked: root.toggleTheme()
                ToolTip.visible: hovered
                ToolTip.text: "Switch between the dark and light palettes (Ctrl+T)"
            }

            Button {
                implicitHeight: Theme.touchTarget
                text: "Recover camera"
                Accessible.role: Accessible.Button
                Accessible.name: "Recover camera"
                Accessible.description: "Re-initialise the camera (fixes a wedged sensor)"
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
        spacing: Theme.spaceSm
        z: 50

        Repeater {
            model: notify
            delegate: Rectangle {
                id: notifyCard
                required property int index
                required property var itemId
                required property int severity
                required property string category
                required property string text
                required property bool sticky

                width: notifyColumn.width
                implicitHeight: notifyText.implicitHeight + 30
                radius: 6
                border.width: 1
                color: severity >= 3 ? Theme.dangerSoft : (severity === 2 ? Theme.warningSoft : Theme.noticeSoft)
                // Section 25 asks for smooth transitions where useful and restrained
                // animation. Fading a new card in says "this just changed" without
                // moving anything already on screen; a card that is merely re-laid-
                // out by a newer one does not animate at all. No scale, no
                // attention-grabbing motion.
                opacity: 0
                Behavior on opacity {
                    NumberAnimation { duration: Theme.motionFast; easing.type: Easing.OutCubic }
                }
                Component.onCompleted: opacity = 1

                Label {
                    id: notifyText
                    anchors.left: parent.left
                    anchors.right: dismissButton.left
                    anchors.top: parent.top
                    anchors.margins: 9
                    text: notifyCard.text
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 9
                    anchors.top: notifyText.bottom
                    anchors.topMargin: 2
                    text: notifyCard.category
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontMicro
                    font.family: Theme.fontMonoFamily
                }

                ToolButton {
                    id: dismissButton
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 4
                    width: 22
                    height: 22
                    text: "×"
                    Accessible.role: Accessible.Button
                    Accessible.name: "Dismiss notification"
                    // By identity, not by index: between this delegate being
                    // created and the operator pressing ×, a postOnce refresh
                    // or the 500 ms sweep may have shifted the rows, and the
                    // captured index would then close somebody else's card
                    // while this one stays on screen (CP-15).
                    onClicked: notify.dismissById(notifyCard.itemId)
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
        Accessible.role: Accessible.Pane
        Accessible.name: "Diagnostics"
        visible: root.diagnosticsOpen
        // Re-probe on open so the panel shows the machine as it is now - the
        // scene-graph backend name only exists after a window is up, and the
        // camera-AP flag only exists once the PC has joined it.
        onVisibleChanged: if (visible) capabilities.refresh()
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 44
        anchors.margins: 8
        width: 380
        height: Math.min(360, root.height - 90)
        color: Theme.surface
        radius: 6
        border.color: Theme.border
        z: 60

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: Theme.spaceMd

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: "Diagnostics"
                    color: Theme.text
                    font.pixelSize: Theme.fontSubtitle
                    font.bold: true
                }
                Button {
                    implicitHeight: Theme.touchTarget
                    text: "×"
                    onClicked: root.diagnosticsOpen = false
                    Accessible.role: Accessible.Button
                    Accessible.name: "Close diagnostics"
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "min level"
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontCaption
                }
                ComboBox {
                    implicitHeight: Theme.touchTarget
                    id: levelCombo
                    Accessible.role: Accessible.ComboBox
                    Accessible.name: "Minimum log level"
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
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
            }

            Label {
                Layout.fillWidth: true
                text: diagnostics.statisticsText().length > 0
                      ? diagnostics.statisticsText() : "no frame statistics yet"
                color: Theme.textFaint
                font.pixelSize: Theme.fontMicro
                font.family: Theme.fontMonoFamily
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            }

            // Section 28's hardware capabilities, one row per bullet, read from
            // C++ (detection lives in Capabilities, never in QML). Refreshed when
            // the panel opens: the scene-graph backend and the interfaces are only
            // meaningful once a window exists and the network is up.
            Label {
                Layout.fillWidth: true
                text: "system capabilities"
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.bold: true
            }

            ListView {
                Layout.fillWidth: true
                Layout.maximumHeight: Math.max(96, diagPanel.height * 0.45)
                implicitHeight: contentHeight
                clip: true
                interactive: contentHeight > height
                model: capabilities.entries
                spacing: 2
                Accessible.role: Accessible.List
                Accessible.name: "System capabilities"
                Accessible.description: "Properties of this computer as measured at runtime"

                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label {
                        Layout.preferredWidth: 118
                        text: modelData.label
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontMicro
                        font.family: Theme.fontMonoFamily
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.value
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontMicro
                        font.family: Theme.fontMonoFamily
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                }
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
                        color: modelData[0] >= 3 ? Theme.danger
                             : (modelData[0] === 2 ? Theme.warning
                             : (modelData[0] === 1 ? Theme.textMuted : Theme.textFaint))
                        font.pixelSize: Theme.fontMicro
                        font.family: Theme.fontMonoFamily
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData[2] + "  " + modelData[3]
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontMicro
                        font.family: Theme.fontMonoFamily
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                }
            }
        }
    }

    footer: ToolBar {
        height: 34

        background: Rectangle {
            color: Theme.surface
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: Theme.spaceXl

            Label {
                text: frameBus.fps.toFixed(1) + " fps"
                color: Theme.success
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
            }
            Label {
                text: "recv " + stream.framesReceived
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
            }
            Label {
                // A frame that arrived but could not be decoded. This is not a
                // transport loss and it was never one: stream.framesDropped is
                // incremented only in MjpegClient's decode-failure branch, so
                // showing it under "drop" alongside the bus counter read as
                // "frames the transport discarded" and understated both (CP-23).
                text: "dec " + stream.framesDropped
                color: stream.framesDropped > 0 ? Theme.danger : Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    ToolTip.visible: containsMouse
                    ToolTip.text: "Frames received but not decoded - a framing or codec fault, not a lost frame"
                }
            }
            Label {
                // Frames the bus replaced before the display could show them.
                // Latest-frame-wins is the right policy, but a drop nobody can
                // see is a drop nobody can fix, so it is counted rather than
                // hidden - and counted on its own, next to the number it is not.
                text: "drop " + frameBus.overwrittenCount
                color: frameBus.overwrittenCount > 0 ? Theme.danger : Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    ToolTip.visible: containsMouse
                    ToolTip.text: "Frames the display never got because a newer one replaced them first"
                }
            }
                        Label {
                            visible: streamStats.hasFrame
                            text: streamStats.bitrateMbps.toFixed(2) + " Mbps"
                            color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
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
                color: streamStats.frameAgeMs > 500 ? Theme.warning : Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    ToolTip.visible: containsMouse
                    ToolTip.text: "How old the picture on screen is"
                }
            }

            Item { Layout.fillWidth: true }

            Label {
                // statusText carries the only real network-layer fault
                // description the app has - a discovery socket it could not
                // bind - so it is preferred over a generic "no device found",
                // which would be true but would not say why.
                text: discovery.count > 0
                      ? discovery.count + " device(s)"
                      : (discovery.statusText.length > 0 ? discovery.statusText
                         : (discovery.scanning ? "searching…" : "no device found"))
                color: discovery.count > 0 ? Theme.success
                     : (discovery.statusText.length > 0 ? Theme.danger : Theme.textMuted)
                font.pixelSize: Theme.fontSmall
            }

            Label {
                id: sessionLabel
                text: sessionState.label
                color: sessionState.severity === "ok" ? Theme.success
                    : sessionState.severity === "warn" ? Theme.warning
                    : sessionState.severity === "err" ? Theme.danger
                    : Theme.textMuted
                font.pixelSize: Theme.fontSmall
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
                color: Theme.textFaint
                font.pixelSize: Theme.fontCaption
                font.family: Theme.fontMonoFamily
            }

            Label {
                visible: recorder.recording
                text: "REC " + recorder.framesWritten + " / "
                      + (recorder.bytesWritten / 1048576).toFixed(1) + " MB"
                color: Theme.danger
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
            }

            Label {
                visible: recorder.errorString.length > 0
                text: recorder.errorString
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
            }

            Label {
                visible: snapshotWriter.errorString.length > 0
                text: snapshotWriter.errorString
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
            }

            Label {
                visible: snapIndicatorTimer.running
                text: "saved " + snapshotWriter.lastPath
                color: Theme.success
                font.pixelSize: Theme.fontCaption
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
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
            }
            Label {
                visible: deviceStatus.online
                text: deviceStatus.status["resolution"] !== undefined ? deviceStatus.status["resolution"] : ""
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
            }
            Label {
                visible: deviceStatus.online
                text: "fw " + (deviceStatus.status["fw_version"] !== undefined ? deviceStatus.status["fw_version"] : "")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
            }
        }
    }

    Item {
        anchors.fill: parent

        RowLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: Theme.spaceMd

            Item {
                id: viewport
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Image {
                    id: frameImage
                    Accessible.role: Accessible.Graphic
                    Accessible.name: "Live camera video"
                    Accessible.description: "JPEG stream from the camera; the mouse wheel zooms"
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
                    color: Theme.text
                    background: Rectangle {
                        color: Theme.scrim
                        radius: 4
                        anchors.fill: parent
                        anchors.margins: -4
                    }
                    font.pixelSize: Theme.fontSmall
                    font.family: Theme.fontMonoFamily
                }
            }

            Rectangle {
                id: configPanel
                Accessible.role: Accessible.Pane
                Accessible.name: "Camera configuration"
                Layout.fillHeight: true
                Layout.preferredWidth: 250
                // Settings exist to be used while the picture is stopped: the
                // firmware refuses a config change with a stream attached, so
                // showing a panel of dead controls during playback only told
                // the user what they could not do. It is here while the
                // control session is up and the video is off, and gone while
                // the video runs.
                visible: deviceStatus.polling && !stream.active
                color: Theme.surface
                radius: 6
                border.color: Theme.border

                // The panel arrives when there is something to configure and
                // leaves when the picture starts, so the change is worth
                // softening. Opacity only - the visible binding above is left
                // exactly as it is, because that binding is what keeps the
                // panel available at the moment a user needs it.
                opacity: visible ? 1.0 : 0.0
                Behavior on opacity {
                    NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic }
                }

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
                        spacing: Theme.spaceLg

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceSm
                        visible: deviceStatus.authRequired || !deviceStatus.authenticated

                        Label {
                            text: deviceStatus.authenticated ? "Authenticated" : "Sign in required"
                            color: deviceStatus.authenticated ? Theme.success : Theme.warning
                            font.pixelSize: Theme.fontSubtitle
                            font.bold: true
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !deviceStatus.authenticated
                            text: deviceStatus.authError.length > 0
                                  ? deviceStatus.authError
                                  : "The camera requires a control password."
                            color: deviceStatus.authError.length > 0 ? Theme.danger : Theme.textMuted
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.WordWrap
                        }

                        TextField {
                            implicitHeight: Theme.touchTarget
                            id: passwordField
                            Accessible.role: Accessible.EditableText
                            Accessible.name: "Control password"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            visible: !deviceStatus.authenticated
                            echoMode: TextInput.Password
                            placeholderText: "Control password"
                            enabled: !deviceStatus.authBusy
                            color: Theme.text
                            placeholderTextColor: Theme.textFaint
                            onAccepted: signInButton.clicked()
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: !deviceStatus.authenticated
                            spacing: Theme.spaceMd

                            Button {
                                implicitHeight: Theme.touchTarget
                                id: signInButton
                                Accessible.role: Accessible.Button
                                Accessible.name: text
                                text: deviceStatus.authBusy ? "Signing in…" : "Sign in"
                                enabled: passwordField.text.length > 0 && !deviceStatus.authBusy
                                onClicked: deviceStatus.signIn(passwordField.text,
                                                                rememberCheck.checked)
                            }

                            CheckBox {
                                implicitHeight: Theme.touchTarget
                                id: rememberCheck
                                Accessible.role: Accessible.CheckBox
                                Accessible.name: "Remember this camera's password"
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
                            color: Theme.textFaint
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.WordWrap
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Theme.border
                        visible: deviceStatus.authRequired || !deviceStatus.authenticated
                    }

                    Label {
                        text: "Camera settings"
                        color: Theme.text
                        font.pixelSize: Theme.fontSubtitle
                        font.bold: true
                    }

                    Label {
                        text: "Resolution"
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    ComboBox {
                        implicitHeight: Theme.touchTarget
                        id: resCombo
                        Accessible.role: Accessible.ComboBox
                        Accessible.name: "Resolution"
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

                    Label {
                        Layout.fillWidth: true
                        text: "JPEG quality: " + Math.round(qualitySlider.value)
                              + (deviceStatus.status["quality_floor"] !== undefined
                                 ? "  (device floor " + deviceStatus.status["quality_floor"] + ")" : "")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    Slider {
                        implicitHeight: Theme.touchTarget
                        id: qualitySlider
                        Accessible.role: Accessible.Slider
                        Accessible.name: "JPEG quality"
                        Accessible.description: "Lower is a better image and larger files"
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
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: deviceStatus.configError.length > 0
                        text: deviceStatus.configError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WordWrap
                    }

                    Item { Layout.fillHeight: true }

                    // ---- operating profile (section 12 / 26) ----
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceMd

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Theme.border
                        }

                        Label {
                            Layout.fillWidth: true
                            text: "Operating profile"
                            color: Theme.text
                            font.pixelSize: Theme.fontSubtitle
                            font.bold: true
                        }

                        ComboBox {
                            implicitHeight: Theme.touchTarget
                            id: profileCombo
                            Accessible.role: Accessible.ComboBox
                            Accessible.name: "Operating profile"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                            // Automatic plus the measured ladder, best image
                            // quality first.
                            //
                            // concat, not "+": in JavaScript the plus operator
                            // stringifies both arrays, so the old expression
                            // built one long string and ComboBox read it as a
                            // list of characters - sixty single-letter options
                            // instead of six named profiles.
                            model: ["Automatic"].concat(profiles.names)
                            currentIndex: profiles.mode
                            enabled: !stream.active && !stream.connecting && !stream.reconnecting

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
                                  ? Theme.warning : Theme.textMuted
                            font.pixelSize: Theme.fontCaption
                            font.family: Theme.fontMonoFamily
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: deviceStatus.online && !profiles.activeIsCustom
                                     && profiles.activeEvidence.length > 0
                            text: profiles.activeEvidence
                            color: Theme.textFaint
                            font.pixelSize: Theme.fontMicro
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: profiles.advice.length > 0
                            text: profiles.advice
                            color: Theme.warning
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.profileHint.length > 0
                            text: root.profileHint
                            color: Theme.textFaint
                            font.pixelSize: Theme.fontMicro
                            wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.profilePending > 0
                            spacing: Theme.spaceMd

                            Label {
                                Layout.fillWidth: true
                                text: "The camera refuses a configuration change while a "
                                      + "stream is attached, so applying this profile "
                                      + "restarts the stream."
                                color: Theme.warning
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.WordWrap
                            }

                            Button {
                                implicitHeight: Theme.touchTarget
                                text: "Stop and apply"
                                Accessible.role: Accessible.Button
                                Accessible.name: "Stop and apply"
                                Accessible.description: "The camera refuses configuration changes while streaming, so this restarts the stream"
                                onClicked: {
                                    root.profilePendingApply = root.profilePending
                                    root.profilePending = 0
                                    // Video only. The control session stays up
                                    // so the write lands on a status the panel
                                    // can still show the result of.
                                    stream.stop()
                                    profileApplyTimer.restart()
                                }
                            }

                            Button {
                                implicitHeight: Theme.touchTarget
                                text: "Cancel"
                                Accessible.role: Accessible.Button
                                Accessible.name: "Cancel the pending profile change"
                                onClicked: {
                                    root.profilePending = 0
                                    profileCombo.currentIndex = profiles.mode
                                }
                            }
                        }

                        // The gap between stopping the stream and sending the
                        // configuration is the reason this exists: the camera
                        // counts a stream client until its socket closes, and
                        // answers 409 to /config until it does. The write
                        // itself is no longer fired blind - toggleConnection
                        // waits on configBusy before reopening the video.
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
                        spacing: Theme.spaceMd
                        visible: root.controlGroups.length > 0

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Theme.border
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: "Camera controls"
                                color: Theme.text
                                font.pixelSize: Theme.fontSubtitle
                                font.bold: true
                            }
                            Button {
                                implicitHeight: Theme.touchTarget
                                text: "Defaults"
                                enabled: !deviceStatus.sensorBusy
                                Accessible.role: Accessible.Button
                                Accessible.name: "Reset camera controls to defaults"
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
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontCaption
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

                                        // A drag writes Slider.value imperatively, which
                                        // destroys any binding on it - so the previous
                                        // `value: parent.current` worked exactly once and
                                        // then the handle kept showing what had been
                                        // dragged even when the camera reported something
                                        // else (CP-14). Push the device's value in by hand,
                                        // the way the quality slider already does, and
                                        // never while the slider is being held, so a 1 Hz
                                        // poll cannot yank it out from under a finger.
                                        function syncFromStatus() {
                                            if (!ctrlSlider.pressed
                                                && Math.round(ctrlSlider.value) !== current)
                                                ctrlSlider.value = current
                                        }
                                        onCurrentChanged: ctrlSlider.syncFromStatus()
                                        Component.onCompleted: ctrlSlider.syncFromStatus()

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label {
                                                Layout.fillWidth: true
                                                text: cname.replace(/_/g, " ")
                                                color: Theme.text
                                                font.pixelSize: Theme.fontSmall
                                                elide: Text.ElideRight
                                            }
                                            Label {
                                                text: modelData.min + " … " + modelData.max
                                                color: Theme.textFaint
                                                font.pixelSize: Theme.fontMicro
                                                font.family: Theme.fontMonoFamily
                                            }
                                        }

                                        Slider {
                                            implicitHeight: Theme.touchTarget
                                            id: ctrlSlider
                                            Accessible.role: Accessible.Slider
                                            Accessible.name: cname.replace(/_/g, " ")
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 30
                                            from: modelData.min
                                            to: modelData.max
                                            stepSize: 1
                                            snapMode: Slider.SnapAlways
                                            // Deliberately not `value: parent.current` -
                                            // see syncFromStatus() above.
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
                                            color: Theme.textFaint
                                            font.pixelSize: Theme.fontMicro
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
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WordWrap
                    }

                    // ---- settings (section 26 / 37) ----
                    // The three capture settings and the capture directory.
                    // They are all user preferences, so they are written through
                    // UserPreferences rather than sent to the camera: the
                    // directory is a property of this PC, not of the camera.
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceMd

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Theme.border
                        }

                        Label {
                            Layout.fillWidth: true
                            text: "Settings"
                            color: Theme.text
                            font.pixelSize: Theme.fontSubtitle
                            font.bold: true
                        }

                        Label {
                            Layout.fillWidth: true
                            text: "Capture folder"
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontCaption
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceSm
                            TextField {
                                implicitHeight: Theme.touchTarget
                                id: captureField
                                Accessible.role: Accessible.EditableText
                                Accessible.name: "Capture folder"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 32
                                text: prefs.captureDirectory.length > 0
                                      ? prefs.captureDirectory : captureRoot
                                color: Theme.text
                                selectByMouse: true
                            }
                            Button {
                                implicitHeight: Theme.touchTarget
                                text: "Use"
                                Accessible.role: Accessible.Button
                                Accessible.name: "Use this folder as the capture directory"
                                enabled: captureField.text.length > 0
                                onClicked: {
                                    prefs.captureDirectory = captureField.text
                                    captureField.text = prefs.captureDirectory
                                }
                            }
                            Button {
                                implicitHeight: Theme.touchTarget
                                text: "Reset"
                                Accessible.role: Accessible.Button
                                Accessible.name: "Reset the capture folder to the default"
                                onClicked: {
                                    prefs.captureDirectory = ""
                                    captureField.text = captureRoot
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: "Restore with this folder on the next launch. The current "
                                  + "session keeps using " + captureRoot + "."
                            color: Theme.textFaint
                            font.pixelSize: Theme.fontMicro
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            text: "Frame buffers: " + deviceStatus.status["fb_count"]
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontCaption
                            visible: deviceStatus.online
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: deviceStatus.online
                            spacing: Theme.spaceMd
                            Label {
                                Layout.fillWidth: true
                                text: "Grab mode: " + deviceStatus.status["grab_mode"]
                                      + "  ·  XCLK " + deviceStatus.status["xclk_mhz"] + " MHz"
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.WordWrap
                            }
                            Button {
                                implicitHeight: Theme.touchTarget
                                text: "Defaults"
                                enabled: !deviceStatus.configBusy && !stream.active
                                Accessible.role: Accessible.Button
                                Accessible.name: "Reset frame buffers, XCLK and grab mode to defaults"
                                // fb2 measured faster than fb3 at svga/q12 (15.91
                                // vs 11.15) but ADR-0010 kept fb3 as the default
                                // for latency; the user can still ask for it.
                                onClicked: {
                                    deviceStatus.setFrameBufferCount(2)
                                    deviceStatus.setXclk(18)
                                    deviceStatus.setGrabMode("latest")
                                }
                            }
                        }
                    }

                    // ---- device information (section 26) ----
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        visible: deviceStatus.online

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Theme.border
                        }

                        Label {
                            Layout.fillWidth: true
                            text: "Device"
                            color: Theme.text
                            font.pixelSize: Theme.fontSubtitle
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
                                spacing: Theme.spaceMd
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.k
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontCaption
                                }
                                Label {
                                    text: modelData.v === undefined ? "—" : String(modelData.v)
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontCaption
                                    font.family: Theme.fontMonoFamily
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
            color: sessionState.severity === "err" ? Theme.danger : Theme.textFaint
            font.pixelSize: Theme.fontDisplay
        }

        // All three of these used to be anchored to this same corner with the
        // same 14 px margin, one on top of another. Their conditions are
        // independent - camera_recoveries is cumulative for the boot, so after
        // any recovery a later stream error printed *underneath* the recovery
        // counter and which one the operator could read was pure z-order
        // (CP-10). A Column skips children whose visible is false, so exactly
        // the conditions that hold are stacked and the offset from the corner
        // is the same 14 px it always was.
        Column {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 14
            spacing: 2

            Label {
                visible: sessionState.state === "reconnecting"
                text: "reconnecting… (" + sessionState.retryAttempt + "/"
                      + sessionState.maxRetries + ") in "
                      + sessionState.retryDelayMs + " ms"
                color: Theme.warning
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
            }

            Label {
                visible: deviceStatus.online
                     && deviceStatus.status["camera_recoveries"] !== undefined
                     && deviceStatus.status["camera_recoveries"] > 0
                text: "camera recoveries: " + deviceStatus.status["camera_recoveries"]
                color: Theme.warning
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
            }

            Label {
                visible: stream.errorString.length > 0
                text: stream.errorString
                color: Theme.danger
                font.pixelSize: Theme.fontSmall
                font.family: Theme.fontMonoFamily
            }
        }
    }
}
