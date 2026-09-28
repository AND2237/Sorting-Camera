pragma Singleton
import QtQuick
import QtQuick.Controls

// Master Prompt 25 asks for a modern industrial visual language, clean
// typography, clear hierarchy, polished spacing, and dark/light theme
// readiness. All of that needs one place that owns the design tokens, because
// scattered literals are exactly what makes an application impossible to
// re-theme: there were 105 hex colours across 16 distinct values in Main.qml
// before this existed, and no way to answer "what is our warning colour"
// without grepping.
//
// Nothing here decides policy. It holds the palette, the type scale, the
// spacing scale and the metrics, and it follows the mode the user chose in
// settings, which is why mode is a plain property rather than a constant.
QtObject {
    id: theme

    property string mode: "dark"

    readonly property bool dark: mode !== "light"
    readonly property bool light: !dark

    function setMode(value) {
        theme.mode = (value === "light") ? "light" : "dark"
    }

    function toggleMode() {
        theme.mode = theme.dark ? "light" : "dark"
    }

    // ---- palette ----------------------------------------------------------
    // The dark values are the ones the application already shipped with,
    // kept exactly as they were so introducing this file changed no pixels.
    readonly property color background: dark ? "#101418" : "#f2f4f7"
    readonly property color surface: dark ? "#171c22" : "#ffffff"
    readonly property color surfaceRaised: dark ? "#21262d" : "#ffffff"
    readonly property color surfaceSunken: dark ? "#0d1117" : "#e8ebef"
    readonly property color border: dark ? "#30363d" : "#d0d7de"
    readonly property color borderStrong: dark ? "#3fb950" : "#1a7f37"

    readonly property color text: dark ? "#e6edf3" : "#1f2328"
    readonly property color textSecondary: dark ? "#c9d1d9" : "#3d444d"
    readonly property color textMuted: dark ? "#8b949e" : "#57606a"
    readonly property color textFaint: dark ? "#6b7684" : "#6e7781"
    readonly property color textOnAccent: dark ? "#101418" : "#ffffff"

    readonly property color accent: dark ? "#3fb950" : "#1a7f37"
    readonly property color accentSoft: dark ? "#1d3d29" : "#dafbe1"
    readonly property color success: dark ? "#3fb950" : "#1a7f37"
    readonly property color warning: dark ? "#d29922" : "#9a6700"
    readonly property color danger: dark ? "#f85149" : "#cf222e"
    readonly property color dangerSoft: dark ? "#991014" : "#ffebe9"
    readonly property color warningSoft: dark ? "#3a2f14" : "#fff8c5"
    readonly property color noticeSoft: dark ? "#1c2430" : "#ddf4ff"
    readonly property color focusRing: dark ? "#58a6ff" : "#0969da"
    // #AARRGGBB: the window background at partial alpha, so a badge floating
    // over the picture keeps the surface behind it visible.
    readonly property color scrim: dark ? "#99101418" : "#991f2328"

    // ---- typography -------------------------------------------------------
    // The UI text family comes from the platform rather than being hard-coded,
    // so the application inherits whatever the host HMI actually has installed.
    // The monospace family is for counters and telemetry, where digits must
    // not shift width as they change; "Consolas" is the Windows face and
    // "monospace" is the fallback anywhere else, which is what lets this ship
    // on an unknown HMI without editing the QML.
    readonly property string fontFamily: Qt.application.font.family
    readonly property string fontMonoFamily: Qt.platform.os === "windows" ? "Consolas" : "monospace"

    // The steps are the sizes the interface actually used before this file
    // existed, named so the rhythm can be reasoned about rather than guessed.
    readonly property int fontDisplay: 18
    readonly property int fontTitle: 16
    readonly property int fontSubtitle: 14
    readonly property int fontBody: 13
    readonly property int fontSmall: 12
    readonly property int fontCaption: 11
    readonly property int fontMicro: 10

    // ---- spacing ----------------------------------------------------------
    // Named so the rhythm is stated once instead of being re-guessed per
    // layout. The steps are the gaps the interface already used, kept at
    // those values so introducing this file changed no pixels; anything new
    // should pick the nearest step rather than invent a number.
    readonly property int spaceXs: 4
    readonly property int spaceSm: 6
    readonly property int spaceMd: 8
    readonly property int spaceLg: 12
    readonly property int spaceXl: 18
    readonly property int radius: 4

    // ---- interaction metrics ---------------------------------------------
    // Section 25 requires targets large enough for a future touchscreen HMI
    // and forbids interactions that only work with a mouse. 44 px is the
    // common floor for a comfortable touch target, so every control in the
    // application is sized from here rather than from its text.
    readonly property int touchTarget: 44
    readonly property int controlHeight: 32
    readonly property int controlHeightTouch: touchTarget

    // ---- motion -----------------------------------------------------------
    // Section 25 asks for smooth transitions where useful and *restrained*
    // animation. These are short enough to read as instant and long enough
    // not to snap. Nothing animates that the user did not just cause.
    readonly property int motionFast: 90
    readonly property int motionNormal: 160
}
