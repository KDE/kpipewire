/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "RecordMe.h"
#include <QCommandLineParser>
#include <QGuiApplication>

int main(int argc, char **argv)
{
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    QGuiApplication app(argc, argv);
    qunsetenv("QT_XCB_GL_INTEGRATION");

    {
        QCommandLineParser parser;
        QCommandLineOption duration(QStringLiteral("duration"), QStringLiteral("seconds length of the video"), QStringLiteral("duration"));
        parser.addOption(duration);
        parser.process(app);

        RecordMe *me = new RecordMe(&app);
        if (parser.isSet(duration)) {
            me->setDuration(parser.value(duration).toInt() * 1000);
        }
    }

    return app.exec();
}
