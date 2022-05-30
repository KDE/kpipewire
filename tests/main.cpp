/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "PlasmaRecordMe.h"
#include <QCommandLineParser>
#include <QGuiApplication>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    {
        QCommandLineParser parser;
        QCommandLineOption duration("duration", "seconds length of the video", "duration");
        QCommandLineOption kwaylandSource("source", "use KWayland::Screencasting to record instead of xdg-desktop-portals", "source", {});
        parser.addOption(duration);
        parser.addOption(kwaylandSource);
        parser.process(app);

        PlasmaRecordMe *me = new PlasmaRecordMe(parser.value(kwaylandSource), &app);
        if (parser.isSet(duration)) {
            me->setDuration(parser.value(duration).toInt() * 1000);
        }
    }

    return app.exec();
}
