/*
    SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import org.kde.pipewire.monitor as Monitor

Item {
    id: root
    readonly property alias monitor: monitor
    readonly property alias count: repeater.count
    property QtObject modelData
    Repeater {
        id: repeater
        model: Monitor.MediaMonitor {
            id: monitor
            role: Monitor.MediaMonitor.Music
        }
        Item {
            Component.onCompleted: {
                root.modelData = model;
            }
        }
    }
}
