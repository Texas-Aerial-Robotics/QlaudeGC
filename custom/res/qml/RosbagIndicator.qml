import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

// Toolbar indicator for ROS bag recording on the UAV and UGV: per vehicle a dot (red and blinking while
// recording, outlined when idle, grey when its companion can't be reached) and the elapsed time.
Item {
    id:             control
    width:          row.width
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    property bool showIndicator: true

    property double _now:   Date.now()
    property bool   _blink: true

    QGCPalette { id: qgcPal }

    function _elapsedText(recorder) {
        var s = Math.max(0, Math.floor((_now - recorder.startedAtMs) / 1000))
        var h = Math.floor(s / 3600)
        var m = Math.floor(s / 60) % 60
        s = s % 60
        return (h > 0 ? h + ":" : "") + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    Timer {
        interval:   500
        running:    tarRosbag.anyRecording
        repeat:     true
        onTriggered: {
            control._now = Date.now()
            control._blink = !control._blink
        }
        onRunningChanged: control._blink = true
    }

    Row {
        id:             row
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        spacing:        ScreenTools.defaultFontPixelWidth

        Repeater {
            model: tarRosbag.recorders

            Row {
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                spacing:        ScreenTools.defaultFontPixelWidth / 2

                property var _r: modelData

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width:                  ScreenTools.defaultFontPixelHeight * 0.8
                    height:                 width
                    radius:                 width / 2
                    color:                  _r.recording ? qgcPal.colorRed : "transparent"
                    opacity:                _r.recording && !control._blink ? 0.25 : 1
                    border.width:           _r.recording ? 0 : Math.max(1, width / 8)
                    border.color:           _r.reachable ? qgcPal.text : qgcPal.colorGrey
                }

                QGCLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    text:                   _r.recording ? _r.label + " " + control._elapsedText(_r) : _r.label
                    color:                  _r.recording ? qgcPal.colorRed : (_r.reachable ? qgcPal.text : qgcPal.colorGrey)
                    font.bold:              _r.recording
                }
            }
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
