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

#pragma once

#include <QObject>
#include <QQmlParserStatus>
#include <gst/gst.h>
#include "gstpointer.h"

class PipelineItem : public QObject, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(uint fd MEMBER m_fd)
    Q_PROPERTY(uint nodeid MEMBER m_nodeid)
    Q_PROPERTY(QObject* widget MEMBER m_widget)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
public:
    PipelineItem(QObject* parent = nullptr);

    ~PipelineItem() override;

    void onBusMessage(GstMessage* message);

    void setPlaying(bool playing);

    bool playing() const;

    void stop() {
        setPlaying(false);
        Q_EMIT close();
    }

    void classBegin() override {}
    void componentComplete() override;

Q_SIGNALS:
    void playingChanged(bool playing);
    void surfaceChanged();
    void descriptionChanged();
    void failed();
    void close();

private:
    uint m_fd;
    uint m_nodeid;
    QObject* m_widget = nullptr;
    bool m_playing = false;
    GstPointer<GstElement> m_pipeline;
};
