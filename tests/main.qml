/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

import QtQuick 2.1
import QtQuick.Layouts 1.1
import QtQuick.Controls 2.1
import org.kde.kirigami 2.15 as Kirigami
import org.kde.taskmanager

import org.kde.pipewire 0.1 as PipeWire

Kirigami.ApplicationWindow
{
    id: root
    width: 500
    height: 500
    visible: true
    property QtObject app

    function addStream(nodeid, displayText, fd, allowDmaBuf) {
        if (fd == null)
            fd = 0;
        rep.model.append({nodeId: nodeid, uuid: "", display: displayText, fd: fd, allowDmaBuf: allowDmaBuf })
    }
    function removeStream(nodeid) {
        for(var i=0; i<rep.model.count; ++i) {
            if (rep.model.get(i).nodeId === nodeid) {
               rep.model.remove(i)
                break;
            }
        }
    }

    Instantiator {
        model: TasksModel {
            groupMode: TasksModel.GroupDisabled
        }
        delegate: Item {
            property var uuid: model.WinIdList
            property var appId: model.AppId
        }
        onObjectAdded: (index, object) => {
           app.addWindow(object.uuid, object.appId)
        }
    }

    ColumnLayout {
        id: pipes
        anchors.fill: parent

        Button {
            text: "Add Virtual Monitor"
            onClicked: app.createVirtualMonitor()
        }

        Button {
            text: "Add Region"
            onClicked: app.requestSelection()
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
                    anchors.fill: parent
                    allowDmaBuf: model.allowDmaBuf
                }

                RowLayout {
                    Kirigami.Icon {
                        id: butt
                        source: sourceItem.usingDmaBuf ? "speedometer" : "delete"
                    }
                    Label {
                        text: model.display
                    }
                }
            }
        }
    }
}
