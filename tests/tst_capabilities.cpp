#include "Capabilities.h"

#include <QSignalSpy>
#include <QSysInfo>
#include <QtTest>

// Master Prompt §28 / gap G-1: the app must determine the machine's
// capabilities at startup and answer each bullet. This suite pins that the
// probe answers all of them, that every answer is an observation rather than a
// plausible-looking guess, and that the flags shown to QML agree with the list.
class TestCapabilities : public QObject
{
    Q_OBJECT

private slots:
    void answersEverySection28Bullet();
    void valuesAreObservationsNotGuesses();
    void networkInterfacesAreListed();
    void flagsAgreeWithTheList();
    void refreshReprobesAndReports();
};

namespace {

QVariantMap find(const QVariantList &entries, const QString &label)
{
    for (const QVariant &value : entries) {
        const QVariantMap map = value.toMap();
        if (map.value(QStringLiteral("label")).toString() == label) {
            return map;
        }
    }
    return {};
}

} // namespace

void TestCapabilities::answersEverySection28Bullet()
{
    const Capabilities capabilities;
    const QVariantList entries = capabilities.entries();
    QVERIFY(!entries.isEmpty());

    QStringList labels;
    for (const QVariant &value : entries) {
        labels << value.toMap().value(QStringLiteral("label")).toString();
    }

    const QStringList required = {
        QStringLiteral("CPU"),
        QStringLiteral("Memory"),
        QStringLiteral("Graphics backend"),
        QStringLiteral("Graphics acceleration"),
        QStringLiteral("Video decode acceleration"),
        QStringLiteral("Display"),
        QStringLiteral("Network interface"),
        QStringLiteral("Touch"),
    };
    for (const QString &bullet : required) {
        QVERIFY2(labels.contains(bullet),
                 qPrintable(QStringLiteral("section 28 bullet not answered: %1 (got: %2)")
                                .arg(bullet, labels.join(QStringLiteral(", ")))));
    }
}

void TestCapabilities::valuesAreObservationsNotGuesses()
{
    const Capabilities capabilities;
    const QVariantList entries = capabilities.entries();

    for (const QVariant &value : entries) {
        const QVariantMap map = value.toMap();
        QVERIFY2(!map.value(QStringLiteral("label")).toString().isEmpty(),
                 "an entry has no label");
        QVERIFY2(!map.value(QStringLiteral("value")).toString().isEmpty(),
                 qPrintable(QStringLiteral("empty value for %1")
                                .arg(map.value(QStringLiteral("label")).toString())));
    }

    // CPU names the architecture this build was compiled for and a core count
    // that is at least one - a probe that could not measure either must say so
    // rather than print a number.
    const QString cpu = find(entries, QStringLiteral("CPU")).value(QStringLiteral("value")).toString();
    QVERIFY2(cpu.contains(QSysInfo::buildCpuArchitecture()),
             qPrintable(QStringLiteral("CPU line does not name the architecture: %1").arg(cpu)));
    QVERIFY2(cpu.contains(QLatin1String("logical cores")), qPrintable(cpu));

#ifdef Q_OS_WIN
    // Memory must be the real figure on the platform that can read it.
    const QString memory =
        find(entries, QStringLiteral("Memory")).value(QStringLiteral("value")).toString();
    QVERIFY2(memory.contains(QStringLiteral("GiB physical")),
             qPrintable(QStringLiteral("memory not measured: %1").arg(memory)));
#endif

    // Video decode: this build genuinely has no hardware path, and §28 asks for
    // the fallback to be safe and stated. A future build with acceleration must
    // change this assertion, which is the point of having one.
    const QString decode =
        find(entries, QStringLiteral("Video decode acceleration")).value(QStringLiteral("value")).toString();
    QVERIFY2(decode.contains(QStringLiteral("no hardware decode path")),
             qPrintable(QStringLiteral("decode line changed without the test knowing: %1")
                            .arg(decode)));
}

void TestCapabilities::networkInterfacesAreListed()
{
    const Capabilities capabilities;
    const QVariantList entries = capabilities.entries();

    int interfaces = 0;
    for (const QVariant &value : entries) {
        if (value.toMap().value(QStringLiteral("label")).toString()
            == QStringLiteral("Network interface")) {
            ++interfaces;
            QVERIFY(!value.toMap().value(QStringLiteral("value")).toString().isEmpty());
        }
    }
    // Either a list of up interfaces, or the explicit "none is up" line - never
    // nothing.
    QVERIFY2(interfaces >= 1, "network section is missing");
}

void TestCapabilities::flagsAgreeWithTheList()
{
    const Capabilities capabilities;
    const QVariantList entries = capabilities.entries();

    const QString touch =
        find(entries, QStringLiteral("Touch")).value(QStringLiteral("value")).toString();
    QCOMPARE(capabilities.touch(), touch.contains(QStringLiteral("touchscreen")));

    const QString acceleration =
        find(entries, QStringLiteral("Graphics acceleration")).value(QStringLiteral("value")).toString();
    QCOMPARE(capabilities.hardwareGraphics(),
             acceleration.startsWith(QStringLiteral("hardware")));

    // A softAP address means the PC joined the camera - that is a capability of
    // the *link*, and the panel shows it as such.
    if (capabilities.onCameraAccessPoint()) {
        bool flagged = false;
        for (const QVariant &value : entries) {
            const QVariantMap map = value.toMap();
            if (map.value(QStringLiteral("label")).toString() == QStringLiteral("Network interface")
                && map.value(QStringLiteral("value")).toString().contains(
                    QStringLiteral("camera softAP"))) {
                flagged = true;
            }
        }
        QVERIFY2(flagged, "onCameraAccessPoint set but no interface says so");
    }
}

void TestCapabilities::refreshReprobesAndReports()
{
    Capabilities capabilities;
    const QVariantList before = capabilities.entries();

    QSignalSpy spy(&capabilities, &Capabilities::entriesChanged);
    QVERIFY(spy.isValid());
    capabilities.refresh();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(capabilities.entries().size(), before.size());
}

QTEST_MAIN(TestCapabilities)
#include "tst_capabilities.moc"
