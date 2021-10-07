import QtQuick 2.1
import QtQuick.Layouts 1.1
import QtQuick.Controls 2.1

import org.kde.taskmanager 0.1 as TaskManager

ApplicationWindow
{
    id: root
    width: 500
    height: 500
    visible: true
    readonly property int cursorMode: cursorCombo.model[cursorCombo.currentIndex].value

    function addStream(nodeid, displayText) {
        rep.model.append({nodeId: nodeid, uuid: "", display: displayText})
    }
    function removeStream(nodeid) {
        for(var i=0; i<rep.model.count; ++i) {
            if (rep.model.get(i).nodeId === nodeid) {
               rep.model.remove(i)
                break;
            }
        }
    }


    onCursorModeChanged: app.cursorMode = root.cursorMode
    ColumnLayout {
        id: pipes
        anchors.fill: parent

        ComboBox {
            id: cursorCombo
            Layout.fillWidth: true
            textRole: "text"
            currentIndex: 0
            model: [{
                    text: "Hidden",
                    value: TaskManager.Screencasting.Hidden
                }, {
                    text: "Embedded",
                    value: TaskManager.Screencasting.Embedded
                }, {
                    text: "Metadata",
                    value: TaskManager.Screencasting.Metadata
                }
            ]
        }
        Button {
            text: "Create"
            onClicked: {
                app.createVirtualOutput()
            }
        }

        Repeater {
            id: rep
            model: ListModel {}

            delegate: TaskManager.PipeWireSourceItem {
                id: vid
                Layout.fillWidth: true
                Layout.fillHeight: true
                nodeId: model.nodeId

                Text {
                    color: "blue"
                    text: display + " " + parent.nodeId + " " + model.nodeId
                }
            }
        }
    }
}
