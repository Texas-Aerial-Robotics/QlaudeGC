import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

ToolIndicatorPage {
    showExpand: true

    property real _fieldWidth: ScreenTools.defaultFontPixelWidth * 34

    QGCPalette { id: qgcPal }

    Component.onCompleted: tarRosbag.refresh()

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

    contentComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            SettingsGroupLayout {
                heading:            qsTr("ROS Bag Recording")
                headingDescription: qsTr("ros2 bag record on %1").arg(tarRosbag.host)

                QGCLabel {
                    Layout.fillWidth:   true
                    text: {
                        if (tarRosbag.busy && !tarRosbag.known) {
                            return qsTr("Connecting...")
                        }
                        if (!tarRosbag.known) {
                            return qsTr("Companion not reachable")
                        }
                        return tarRosbag.recording ? qsTr("Recording") : qsTr("Idle")
                    }
                    color:              tarRosbag.recording ? qgcPal.colorRed : (tarRosbag.known ? qgcPal.text : qgcPal.colorGrey)
                    font.bold:          true
                }

                QGCLabel {
                    Layout.fillWidth:       true
                    Layout.preferredWidth:  _fieldWidth
                    visible:                tarRosbag.lastError !== ""
                    text:                   tarRosbag.lastError
                    color:                  qgcPal.colorRed
                    wrapMode:               Text.WordWrap
                }

                RowLayout {
                    QGCButton {
                        text:       tarRosbag.recording ? qsTr("Stop Recording") : qsTr("Start Recording")
                        enabled:    !tarRosbag.busy && tarRosbag.known
                        onClicked:  tarRosbag.recording ? tarRosbag.stop() : tarRosbag.start()
                    }

                    QGCButton {
                        text:       qsTr("Refresh")
                        enabled:    !tarRosbag.busy
                        onClicked:  tarRosbag.refresh()
                    }

                    QGCLabel {
                        visible:    tarRosbag.busy
                        text:       qsTr("Working...")
                    }
                }

                LabelledLabel {
                    visible:    tarRosbag.bagPath !== ""
                    label:      tarRosbag.recording ? qsTr("Bag") : qsTr("Last bag")
                    labelText:  tarRosbag.bagPath
                }

                LabelledLabel {
                    visible:    tarRosbag.bagPath !== ""
                    label:      qsTr("Size")
                    labelText:  _formatBytes(tarRosbag.bagBytes)
                }

                LabelledLabel {
                    visible:    tarRosbag.known
                    label:      qsTr("Free on companion")
                    labelText:  _formatBytes(tarRosbag.freeBytes)
                }
            }
        }
    }

    expandedComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            SettingsGroupLayout {
                heading:            qsTr("Settings")
                headingDescription: qsTr("Uses your ssh key (no password prompts). Applied to the next command.")

                RowLayout {
                    QGCLabel { text: qsTr("Companion (user@host)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        text:                   tarRosbag.host
                        placeholderText:        "tar@100.66.89.102"
                        onEditingFinished:      tarRosbag.host = text
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("ssh key (blank = default)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        text:                   tarRosbag.sshKey
                        placeholderText:        "~/.ssh/id_ed25519"
                        onEditingFinished:      tarRosbag.sshKey = text
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("ROS_DOMAIN_ID"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        text:                   tarRosbag.domainId
                        numericValuesOnly:      true
                        onEditingFinished:      tarRosbag.domainId = text
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("Bag folder"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        text:                   tarRosbag.bagFolder
                        onEditingFinished:      tarRosbag.bagFolder = text
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("Bag name prefix"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        text:                   tarRosbag.bagPrefix
                        onEditingFinished:      tarRosbag.bagPrefix = text
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("Exclude topics (regex)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        text:                   tarRosbag.excludeRegex
                        placeholderText:        qsTr("blank = record everything")
                        onEditingFinished:      tarRosbag.excludeRegex = text
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("Extra setup.bash"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        text:                   tarRosbag.setupScript
                        onEditingFinished:      tarRosbag.setupScript = text
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("Extra record arguments"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        text:                   tarRosbag.extraArgs
                        placeholderText:        "--max-bag-size 2000000000"
                        onEditingFinished:      tarRosbag.extraArgs = text
                    }
                }

                QGCButton {
                    text:       qsTr("Restore Defaults")
                    onClicked:  tarRosbag.restoreDefaults()
                }
            }

            SettingsGroupLayout {
                heading: qsTr("Log")

                QGCFlickable {
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 60
                    Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 8
                    contentHeight:          logText.height
                    clip:                   true

                    Component.onCompleted: contentY = Math.max(0, contentHeight - height)

                    QGCLabel {
                        id:             logText
                        width:          parent.width
                        text:           tarRosbag.log.length > 0 ? tarRosbag.log.join("\n") : qsTr("Nothing yet")
                        font.family:    ScreenTools.fixedFontFamily
                        font.pointSize: ScreenTools.smallFontPointSize
                        wrapMode:       Text.WrapAnywhere
                    }
                }
            }
        }
    }
}
