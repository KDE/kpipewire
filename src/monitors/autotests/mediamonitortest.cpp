/*
    SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#define QT_FORCE_ASSERTS 1

#include <QProcess>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTest>

namespace
{
template<typename T>
[[nodiscard]] T evaluate(QObject *scope, const QString &expression)
{
    QQmlExpression expr(qmlContext(scope), scope, expression);
    QVariant result = expr.evaluate();
    Q_ASSERT_X(!expr.hasError(), "MediaMonitorTest", qUtf8Printable(expr.error().toString()));
    return result.value<T>();
}

template<>
void evaluate<void>(QObject *scope, const QString &expression)
{
    QQmlExpression expr(qmlContext(scope), scope, expression);
    expr.evaluate();
    Q_ASSERT_X(!expr.hasError(), "MediaMonitorTest", qUtf8Printable(expr.error().toString()));
}

bool initView(QQuickView *view, const QString &urlStr)
{
    view->setSource(QUrl(urlStr));

    QSignalSpy statusSpy(view, &QQuickView::statusChanged);
    if (view->status() == QQuickView::Loading) {
        statusSpy.wait();
    } else if (view->status() != QQuickView::Ready) {
        qCritical() << "Not loading" << view->errors();
        return false;
    }

    if (view->status() != QQuickView::Ready) {
        qCritical() << view->errors();
        return false;
    }

    return true;
}
}

class MediaMonitorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /**
     * Loads MediaMonitor in QML, and tests it can monitor music streams
     */
    void test_MediaMonitor();
};

void MediaMonitorTest::test_MediaMonitor()
{
    QProcess pidof;
    pidof.setProgram(QStringLiteral("pidof"));
    pidof.setArguments({QStringLiteral("pipewire")});
    pidof.start();
    pidof.waitForFinished();

    QProcess pipewire;
    if (QString::fromUtf8(pidof.readAllStandardOutput()).toInt() == 0) {
        pipewire.setProgram(QStringLiteral("pipewire"));
        pipewire.start();
        QVERIFY2(pipewire.waitForStarted(), pipewire.readAllStandardError().constData());
    }

    auto view = std::make_unique<QQuickView>();
    QByteArray errorMessage;
    QVERIFY(initView(view.get(), QFINDTESTDATA(QStringLiteral("mediamonitortest.qml"))));

    QQuickItem *rootObject = view->rootObject();
    QSignalSpy countSpy(rootObject, SIGNAL(countChanged()));
    // To make sure modelData is not null after count changes
    QSignalSpy modelDataChangedSpy(rootObject, SIGNAL(modelDataChanged()));

    QProcess player;
    player.setProgram(QStringLiteral("pw-play"));
    player.setArguments({QStringLiteral("-v"), QFINDTESTDATA(QStringLiteral("alarm-clock-elapsed.oga"))});
    player.start();
    QVERIFY2(player.waitForStarted(), player.readAllStandardError().constData());

    if (evaluate<int>(rootObject, QStringLiteral("root.count")) == 0) {
        countSpy.wait();
    }
    if (evaluate<QObject *>(rootObject, QStringLiteral("root.modelData")) == nullptr) {
        modelDataChangedSpy.wait();
    }

    QVERIFY(evaluate<bool>(rootObject, QStringLiteral("monitor.detectionAvailable")));
    QCOMPARE(evaluate<int>(rootObject, QStringLiteral("root.count")), 1);
    if (qEnvironmentVariableIsSet("KDECI_BUILD")) {
        // There is no output in CI
        const int suspendedInt = evaluate<int>(rootObject, QStringLiteral("Monitor.NodeState.Suspended"));
        QTRY_COMPARE(evaluate<int>(rootObject, QStringLiteral("root.modelData.state")), suspendedInt);
    } else {
        const int runningInt = evaluate<int>(rootObject, QStringLiteral("Monitor.NodeState.Running"));
        QTRY_COMPARE(evaluate<int>(rootObject, QStringLiteral("root.modelData.state")), runningInt);
    }

    // Changing role will trigger reconnecting
    QObject *monitor = evaluate<QObject *>(rootObject, QStringLiteral("monitor"));
    QVERIFY(monitor);
    QSignalSpy detectionAvailableChangedSpy(monitor, SIGNAL(detectionAvailableChanged()));
    QSignalSpy roleChangedSpy(monitor, SIGNAL(roleChanged()));
    evaluate<void>(rootObject, QStringLiteral("monitor.role = Monitor.MediaMonitor.Camera"));
    if (roleChangedSpy.empty()) {
        QVERIFY(roleChangedSpy.wait());
    }
    QVERIFY(evaluate<bool>(rootObject, QStringLiteral("monitor.detectionAvailable")));
    QCOMPARE(detectionAvailableChangedSpy.size(), 2); // True -> False -> True
    QCOMPARE(evaluate<int>(rootObject, QStringLiteral("root.count")), 0);

    // Change back to Music role
    roleChangedSpy.clear();
    detectionAvailableChangedSpy.clear();
    evaluate<void>(rootObject, QStringLiteral("monitor.role = Monitor.MediaMonitor.Music"));
    if (roleChangedSpy.empty()) {
        QVERIFY(roleChangedSpy.wait());
    }
    QVERIFY(evaluate<bool>(rootObject, QStringLiteral("monitor.detectionAvailable")));
    QCOMPARE(detectionAvailableChangedSpy.size(), 2); // True -> False -> True
    if (evaluate<int>(rootObject, QStringLiteral("root.count")) == 0) {
        QVERIFY(countSpy.wait());
    }
    QCOMPARE(evaluate<int>(rootObject, QStringLiteral("root.count")), 1);

    // Now close the player, and check if the count changes
    player.terminate();
    if (evaluate<int>(rootObject, QStringLiteral("root.count")) > 0) {
        QVERIFY(countSpy.wait());
    }
    QCOMPARE(evaluate<int>(rootObject, QStringLiteral("root.count")), 0);

    if (pipewire.state() == QProcess::Running) {
        pipewire.terminate();
        QTRY_VERIFY(!evaluate<bool>(rootObject, QStringLiteral("monitor.detectionAvailable")));
    }
}

QTEST_MAIN(MediaMonitorTest)

#include "mediamonitortest.moc"
