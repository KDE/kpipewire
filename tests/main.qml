/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

import QtQuick 2.1
import QtQuick.Layouts 1.1
import QtQuick.Controls 2.1
import org.kde.kirigami 2.15 as Kirigami

import org.kde.pipewire 0.1 as PipeWire
import org.kde.pipewire.record 0.1 as PWRec

Kirigami.ApplicationWindow
{
    id: root
    width: 500
    height: 500
    visible: true
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

    signal record(int nodeId, bool capture)

    ColumnLayout {
        id: pipes
        anchors.fill: parent

        Button {
            text: "Add Virtual Monitor"
            onClicked: app.createVirtualMonitor()
        }

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
                    visible: record.state !== PWRec.PipeWireRecord.Recording
                    anchors.fill: parent

                }
                Button {
                    id: butt
                    icon.name: "media-record"
                    text: model.display + " " + model.nodeId
                    enabled: checked === (record.state !== PWRec.PipeWireRecord.Idle)
                    checkable: true

                    PWRec.PipeWireRecord {
                        id: record
                        nodeId: model.nodeId
                        fd: model.fd
                        output: "~/clementine.mp4"
                        active: butt.checked
                    }
                }
            }
        }
    }
}
