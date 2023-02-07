/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

import QtQuick 2.1
import QtQuick.Window 2.15
import QtQuick.Layouts 1.1
import QtQuick.Controls 2.1

import org.kde.pipewire 0.1 as PipeWire
import org.kde.pipewire.record 0.1 as PWRec

Window
{
    id: root
    width: 500
    height: 500
    visible: true
    title: "Recording Bridge"
    property QtObject app

    function addStream(nodeid, displayText, fd) {
        if (fd == null)
            fd = 0;
        rep.model.append({nodeId: nodeid, uuid: "", display: displayText, fd: fd})
    }
    function removeStream(nodeid) {
        for(var i=0; i<rep.model.count; ++i) {
            if (rep.model.get(i).nodeId === nodeid) {
               rep.model.remove(i)
                break;
            }
        }
    }

    ColumnLayout {
        id: pipes
        anchors.fill: parent

        Repeater {
            id: rep
            model: ListModel {}

            delegate: Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                PipeWire.PipeWireSourceItem {
                    id: sourceItem
                    nodeId: model.nodeId
                    fd: model.fd
                    anchors.fill: parent

                }
            }
        }
    }
}
