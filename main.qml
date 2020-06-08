import QtQuick 2.1
import QtQuick.Layouts 1.1
import QtQuick.Controls 2.1

import org.freedesktop.gstreamer.GLVideoItem 1.0

import org.kde.recordme 1.0

ApplicationWindow
{
    width: 500
    height: 500
    visible: true

    function addPipeline(nodeid, displayText) {
        rep.model.append({node: nodeid, display: displayText})
    }
    function removePipeline(nodeid) {
        for(var i=0; i<rep.model.count; ++i) {
            if (rep.model.get(i).node === nodeid) {
            //    rep.model.remove(i)
                break;
            }
        }
    }

    Button {
        id: butt
        checkable: true
    }

    ColumnLayout {
        id: pipes
        anchors.fill: parent

        Repeater {
            id: rep
            model: ListModel {}

            delegate: GstGLVideoItem {
                id: vid
                Layout.fillWidth: true
                Layout.fillHeight: true

                Text {
                    color: "red"
                    text: display
                }

                PipelineItem {
                    nodeid: node
                    widget: vid
                    playing: butt.checked
                }
            }
        }
    }
}
