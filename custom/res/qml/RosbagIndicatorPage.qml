import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

// ROS bag recording on the UAV and UGV: each vehicle starts/stops on its own (one can record while the
// other is idle), or both together. Addresses follow the ground station's Wi-Fi (TarFieldNetwork).
ToolIndicatorPage {
    showExpand: true

    property real _fieldWidth: ScreenTools.defaultFontPixelWidth * 34

    QGCPalette { id: qgcPal }

    Component.onCompleted: tarRosbag.refreshAll()

    function _formatBytes(bytes) {
        if (bytes >= 1073741824) {
            return (bytes / 1073741824).toFixed(2) + " GB"
        }
        if (bytes >= 1048576) {
            return (bytes / 1048576).toFixed(1) + " MB"
        }
        if (bytes >= 1024) {
            return (bytes / 1024).toFixed(0) + " kB"
        }
        return bytes + " B"
    }

    function _stateText(r) {
        if (r.busy && !r.known) {
            return qsTr("Connecting...")
        }
        if (!r.known) {
            return qsTr("Not reachable")
        }
        return r.recording ? qsTr("Recording") : qsTr("Idle")
    }

    contentComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            QGCLabel {
                Layout.fillWidth:       true
                Layout.preferredWidth:  _fieldWidth
                text:                   qsTr("Network: %1").arg(tarRosbag.network.modeText)
                wrapMode:               Text.WordWrap
                font.pointSize:         ScreenTools.smallFontPointSize
            }

            Repeater {
                model: tarRosbag.recorders

                SettingsGroupLayout {
                    property var _r: modelData

                    heading:            qsTr("%1 ROS Bag").arg(_r.label)
                    headingDescription: qsTr("ros2 bag record on %1").arg(_r.host)

                    RowLayout {
                        QGCLabel {
                            Layout.fillWidth:   true
                            text:               _stateText(_r)
                            color:              _r.recording ? qgcPal.colorRed : (_r.known ? qgcPal.text : qgcPal.colorGrey)
                            font.bold:          true
                        }

                        QGCButton {
                            text:       _r.recording ? qsTr("Stop") : qsTr("Start")
                            enabled:    !_r.busy && _r.known
                            onClicked:  _r.recording ? _r.stop() : _r.start()
                        }

                        QGCButton {
                            text:       qsTr("Refresh")
                            enabled:    !_r.busy
                            onClicked:  _r.refresh()
                        }
                    }

                    QGCLabel {
                        Layout.fillWidth:       true
                        Layout.preferredWidth:  _fieldWidth
                        visible:                _r.lastError !== ""
                        text:                   _r.lastError
                        color:                  qgcPal.colorRed
                        wrapMode:               Text.WordWrap
                    }

                    QGCLabel {
                        visible:    _r.busy
                        text:       qsTr("Working...")
                    }

                    LabelledLabel {
                        visible:    _r.bagPath !== ""
                        label:      _r.recording ? qsTr("Bag") : qsTr("Last bag")
                        labelText:  _r.bagPath
                    }

                    LabelledLabel {
                        visible:    _r.bagPath !== ""
                        label:      qsTr("Size")
                        labelText:  _formatBytes(_r.bagBytes)
                    }

                    LabelledLabel {
                        visible:    _r.known
                        label:      qsTr("Free on companion")
                        labelText:  _formatBytes(_r.freeBytes)
                    }

                    RowLayout {
                        QGCButton {
                            text:       _r.downloading ? qsTr("Cancel Download") : qsTr("Download Last Bag")
                            enabled:    _r.downloading || (!_r.recording && !_r.busy && _r.known && _r.bagPath !== "")
                            onClicked:  _r.downloading ? _r.cancelDownload() : _r.downloadLastBag()
                        }

                        QGCButton {
                            text:       qsTr("Open Folder")
                            onClicked:  _r.openDownloadFolder()
                        }
                    }

                    QGCLabel {
                        Layout.fillWidth:       true
                        Layout.preferredWidth:  _fieldWidth
                        visible:                _r.downloadText !== ""
                        text:                   _r.downloadText
                        wrapMode:               Text.WrapAnywhere
                        font.pointSize:         ScreenTools.smallFontPointSize
                    }
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter

                QGCButton {
                    text:       qsTr("Start Both")
                    enabled:    !tarRosbag.allRecording && !tarRosbag.anyBusy
                    onClicked:  tarRosbag.startAll()
                }

                QGCButton {
                    text:       qsTr("Stop Both")
                    enabled:    tarRosbag.anyRecording && !tarRosbag.anyBusy
                    onClicked:  tarRosbag.stopAll()
                }
            }
        }
    }

    expandedComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            Repeater {
                model: tarRosbag.recorders

                SettingsGroupLayout {
                    property var _r: modelData

                    heading:            qsTr("%1 Settings").arg(_r.label)
                    headingDescription: qsTr("Tailscale or field-router address is chosen from the Wi-Fi. Uses your ssh key (no password prompts). Applied to the next command.")

                    RowLayout {
                        QGCLabel { text: qsTr("ssh key (blank = default)"); Layout.fillWidth: true }
                        QGCTextField {
                            Layout.preferredWidth:  _fieldWidth
                            text:                   _r.sshKey
                            placeholderText:        "~/.ssh/id_ed25519"
                            onEditingFinished:      _r.sshKey = text
                        }
                    }

                    RowLayout {
                        QGCLabel { text: qsTr("ROS_DOMAIN_ID"); Layout.fillWidth: true }
                        QGCTextField {
                            Layout.preferredWidth:  _fieldWidth
                            text:                   _r.domainId
                            numericValuesOnly:      true
                            onEditingFinished:      _r.domainId = text
                        }
                    }

                    RowLayout {
                        QGCLabel { text: qsTr("Bag folder"); Layout.fillWidth: true }
                        QGCTextField {
                            Layout.preferredWidth:  _fieldWidth
                            text:                   _r.bagFolder
                            onEditingFinished:      _r.bagFolder = text
                        }
                    }

                    RowLayout {
                        QGCLabel { text: qsTr("Bag name prefix"); Layout.fillWidth: true }
                        QGCTextField {
                            Layout.preferredWidth:  _fieldWidth
                            text:                   _r.bagPrefix
                            onEditingFinished:      _r.bagPrefix = text
                        }
                    }

                    RowLayout {
                        QGCLabel { text: qsTr("Exclude topics (regex)"); Layout.fillWidth: true }
                        QGCTextField {
                            Layout.preferredWidth:  _fieldWidth
                            text:                   _r.excludeRegex
                            placeholderText:        qsTr("blank = record everything")
                            onEditingFinished:      _r.excludeRegex = text
                        }
                    }

                    RowLayout {
                        QGCLabel { text: qsTr("Setup scripts (space-separated)"); Layout.fillWidth: true }
                        QGCTextField {
                            Layout.preferredWidth:  _fieldWidth
                            text:                   _r.setupScript
                            onEditingFinished:      _r.setupScript = text
                        }
                    }

                    RowLayout {
                        QGCLabel { text: qsTr("Extra record arguments"); Layout.fillWidth: true }
                        QGCTextField {
                            Layout.preferredWidth:  _fieldWidth
                            text:                   _r.extraArgs
                            placeholderText:        "--max-bag-size 2000000000"
                            onEditingFinished:      _r.extraArgs = text
                        }
                    }

                    RowLayout {
                        QGCLabel { text: qsTr("Download folder (this PC)"); Layout.fillWidth: true }
                        QGCTextField {
                            Layout.preferredWidth:  _fieldWidth
                            text:                   _r.downloadFolder
                            onEditingFinished:      _r.downloadFolder = text
                        }
                    }

                    QGCButton {
                        text:       qsTr("Restore Defaults")
                        onClicked:  _r.restoreDefaults()
                    }

                    QGCLabel { text: qsTr("Log"); font.bold: true }

                    QGCFlickable {
                        Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 60
                        Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 6
                        contentHeight:          logText.height
                        clip:                   true

                        Component.onCompleted: contentY = Math.max(0, contentHeight - height)

                        QGCLabel {
                            id:             logText
                            width:          parent.width
                            text:           _r.log.length > 0 ? _r.log.join("\n") : qsTr("Nothing yet")
                            font.family:    ScreenTools.fixedFontFamily
                            font.pointSize: ScreenTools.smallFontPointSize
                            wrapMode:       Text.WrapAnywhere
                        }
                    }
                }
            }
        }
    }
}
