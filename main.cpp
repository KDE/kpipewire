/*
 * App To Record systems using xdg-desktop-portal
 * Copyright 2020 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License or (at your option) version 3 or any later version
 * accepted by the membership of KDE e.V. (or its successor approved
 * by the membership of KDE e.V.), which shall act as a proxy
 * defined in Section 14 of version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PlasmaRecordMe.h"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <gst/gst.h>
#include <pipewire/pipewire.h>

int main(int argc, char **argv)
{
    gst_init (&argc, &argv);
    gst_element_factory_make ("qmlglsink", NULL);
//     pw_init(nullptr, nullptr);

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
