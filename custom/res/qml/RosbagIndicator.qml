import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

// Toolbar indicator for the companion-computer ROS bag recorder: red blinking dot + elapsed time while
// recording, grey dot when the companion can't be reached.
Item {
    id:             control
    width:          row.width
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    property bool showIndicator: true

    property bool   _recording: tarRosbag.recording
    property double _now:       Date.now()

    QGCPalette { id: qgcPal }

    function _elapsedText() {
        var s = Math.max(0, Math.floor((_now - tarRosbag.startedAtMs) / 1000))
        var h = Math.floor(s / 3600)
        var m = Math.floor(s / 60) % 60
        s = s % 60
        return (h > 0 ? h + ":" : "") + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    Timer {
        interval:   500
        running:    control._recording
        repeat:     true
        onTriggered: {
            control._now = Date.now()
            dot.opacity = dot.opacity > 0.5 ? 0.25 : 1
        }
        onRunningChanged: dot.opacity = 1
    }

    Row {
        id:             row
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        spacing:        ScreenTools.defaultFontPixelWidth / 2

        Rectangle {
            id:                     dot
            anchors.verticalCenter: parent.verticalCenter
            width:                  ScreenTools.defaultFontPixelHeight * 0.9
            height:                 width
            radius:                 width / 2
            color:                  control._recording ? qgcPal.colorRed : "transparent"
            border.width:           control._recording ? 0 : Math.max(1, width / 8)
            border.color:           tarRosbag.reachable ? qgcPal.text : qgcPal.colorGrey
        }

        QGCLabel {
            anchors.verticalCenter: parent.verticalCenter
            text:                   control._recording ? control._elapsedText() : qsTr("Bag")
            color:                  control._recording ? qgcPal.colorRed : (tarRosbag.reachable ? qgcPal.text : qgcPal.colorGrey)
            font.bold:              control._recording
        }
    }

    MouseArea {
        anchors.fill:   parent
        onClicked:      mainWindow.showIndicatorDrawer(rosbagPage, control)
    }

    Component {
        id: rosbagPage

        RosbagIndicatorPage { }
    }
}
