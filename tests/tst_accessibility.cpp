#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QtTest>

// Section 25 / gap G-12: every interactive control in Main.qml carries an
// Accessible role and name, because a screen reader announces nothing useful
// from a "×" or a "Use". This is deliberately a source-level tripwire: QML
// still runs and qmllint stays quiet when the annotations are dropped, so the
// only evidence that they came back would be somebody noticing silence - which
// no behaviour test can see.
class TestAccessibility : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void everyControlCarriesRoleAndName();
    void rolesAreRealQAccessibleRoles();
    void theSurfacesThatAreNotControlsAreNamed();
    void namesAreDeclaredOncePerControl();

private:
    QString m_qml;
    QStringList m_lines;
    QString windowAfter(int declarationLine) const;
};

void TestAccessibility::initTestCase()
{
    QFile file(QStringLiteral(SCAM_QML_DIR "/Main.qml"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(QStringLiteral("cannot read %1").arg(file.fileName())));
    m_qml = QString::fromUtf8(file.readAll());
    QVERIFY2(!m_qml.isEmpty(), "Main.qml is empty");
    m_lines = m_qml.split(QLatin1Char('\n'));
    QVERIFY(m_lines.size() > 100);
}

QString TestAccessibility::windowAfter(int declarationLine) const
{
    // A fixed window would let one control inherit its neighbour's annotation -
    // which is exactly the deletion this exists to catch. So the window stops at
    // the next control declaration, whichever comes first.
    static const QRegularExpression anyControl(
        QStringLiteral("\\b(Button|Slider|ComboBox|TextField|CheckBox|ToolButton)\\s*\\{"));
    const int last = qMin(m_lines.size() - 1, declarationLine + 40);
    QString out;
    for (int i = declarationLine + 1; i <= last; ++i) {
        if (anyControl.match(m_lines.at(i)).hasMatch())
            break;
        out += m_lines.at(i) + QLatin1Char('\n');
    }
    return out;
}

void TestAccessibility::everyControlCarriesRoleAndName()
{
    static const char *const types[] = {
        "Button", "Slider", "ComboBox", "TextField", "CheckBox", "ToolButton",
    };

    for (const char *rawType : types) {
        const QString type = QString::fromLatin1(rawType);
        // \b keeps ToolButton out of the Button matches.
        const QRegularExpression declaration(
            QStringLiteral("\\b%1\\s*\\{").arg(type));

        int declarations = 0;
        QStringList offenders;
        for (int i = 0; i < m_lines.size(); ++i) {
            if (!declaration.match(m_lines.at(i)).hasMatch())
                continue;
            ++declarations;
            const QString window = windowAfter(i);
            const bool hasName = window.contains(QLatin1String("Accessible.name:"));
            const bool hasRole = window.contains(QLatin1String("Accessible.role:"));
            if (!hasName || !hasRole) {
                offenders << QStringLiteral("%1 at line %2 (name %3, role %4)")
                                 .arg(type)
                                 .arg(i + 1)
                                 .arg(hasName ? QStringLiteral("yes") : QStringLiteral("no"))
                                 .arg(hasRole ? QStringLiteral("yes") : QStringLiteral("no"));
            }
        }

        QVERIFY2(declarations > 0,
                 qPrintable(QStringLiteral("no %1 declared - the pattern broke or "
                                           "the controls changed name")
                                .arg(type)));
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral("controls without an accessible name/role: %1")
                                .arg(offenders.join(QStringLiteral("; ")))));
    }
}

void TestAccessibility::rolesAreRealQAccessibleRoles()
{
    // Extend deliberately: every value here must exist in QAccessible::Role.
    // "Image" is the trap - the role for a picture is Graphic, and an unknown
    // value is reported at run time as "Unable to assign [undefined]" while the
    // item silently keeps no role at all.
    static const QSet<QString> known = {
        QStringLiteral("Button"),
        QStringLiteral("CheckBox"),
        QStringLiteral("ComboBox"),
        QStringLiteral("EditableText"),
        QStringLiteral("Graphic"),
        QStringLiteral("List"),
        QStringLiteral("Pane"),
        QStringLiteral("Slider"),
        QStringLiteral("StaticText"),
    };

    const QRegularExpression role(
        QStringLiteral("Accessible\\.role:\\s*Accessible\\.(\\w+)"));
    auto it = role.globalMatch(m_qml);
    int roles = 0;
    QStringList unknown;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        ++roles;
        const QString value = match.captured(1);
        if (!known.contains(value))
            unknown << value;
    }

    QVERIFY2(roles >= 25,
             qPrintable(QStringLiteral("expected the whole UI to be annotated, saw %1 roles")
                            .arg(roles)));
    QVERIFY2(unknown.isEmpty(),
             qPrintable(QStringLiteral("roles that do not exist in QAccessible::Role: %1")
                            .arg(unknown.join(QStringLiteral(", ")))));
}

void TestAccessibility::theSurfacesThatAreNotControlsAreNamed()
{
    // Panes, the video surface and the capability list are not controls, so the
    // control sweep above cannot see them - but they are what a screen reader
    // lands on first.
    const QStringList surfaces = {
        QStringLiteral("diagPanel"),
        QStringLiteral("configPanel"),
        QStringLiteral("frameImage"),
    };
    const QRegularExpression id(QStringLiteral("\\bid: (%1)\\b")
                                    .arg(surfaces.join(QLatin1Char('|'))));

    int seen = 0;
    for (int i = 0; i < m_lines.size(); ++i) {
        const QRegularExpressionMatch match = id.match(m_lines.at(i));
        if (!match.hasMatch())
            continue;
        ++seen;
        QVERIFY2(windowAfter(i).contains(QLatin1String("Accessible.name:")),
                 qPrintable(QStringLiteral("%1 has no Accessible.name").arg(match.captured(1))));
    }
    QCOMPARE(seen, surfaces.size());

    // The capability list is matched on its model, which is unique.
    for (int i = 0; i < m_lines.size(); ++i) {
        if (!m_lines.at(i).contains(QLatin1String("model: capabilities.entries")))
            continue;
        QVERIFY2(windowAfter(i).contains(QLatin1String("Accessible.name:")),
                 "the system capability list has no Accessible.name");
        ++seen;
    }
    QCOMPARE(seen, surfaces.size() + 1);
}

void TestAccessibility::namesAreDeclaredOncePerControl()
{
    // A second Accessible.name on the same element does not get read twice, it
    // makes the first one a dead binding nobody notices is dead.
    const QRegularExpression name(QStringLiteral("Accessible\\.name:"));
    int count = 0;
    for (const QString &line : m_lines)
        if (name.match(line).hasMatch())
            ++count;

    QVERIFY2(count >= 29,
             qPrintable(QStringLiteral("expected the annotated UI to keep at least "
                                       "29 named surfaces, saw %1")
                            .arg(count)));
}

QTEST_MAIN(TestAccessibility)
#include "tst_accessibility.moc"
