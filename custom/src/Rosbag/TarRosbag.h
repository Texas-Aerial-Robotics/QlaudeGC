#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QVariantList>

#include "TarRosbagRecorder.h"

class TarFieldNetwork;

/// ROS bag recording on both TAR vehicles (UAV and UGV), each with its own TarRosbagRecorder, so either can
/// start or stop while the other records, and both can be started/stopped together.
/// Exposed to QML as the `tarRosbag` context property.
class TarRosbag : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *uav             READ uav            CONSTANT)
    Q_PROPERTY(QObject *ugv             READ ugv            CONSTANT)
    Q_PROPERTY(QVariantList recorders   READ recordersList  CONSTANT)
    Q_PROPERTY(QObject *network         READ network        CONSTANT)
    Q_PROPERTY(bool anyRecording        READ anyRecording   NOTIFY stateChanged)
    Q_PROPERTY(bool allRecording        READ allRecording   NOTIFY stateChanged)
    Q_PROPERTY(bool anyBusy             READ anyBusy        NOTIFY stateChanged)

public:
    explicit TarRosbag(TarFieldNetwork *network, QObject *parent = nullptr);

    /// Starts every recorder that is not recording / stops every recorder that is.
    Q_INVOKABLE void startAll();
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE void refreshAll();

    QObject *uav() const { return _recorders.value(0); }
    QObject *ugv() const { return _recorders.value(1); }
    QObject *network() const;
    QVariantList recordersList() const;
    const QList<TarRosbagRecorder *> &recorders() const { return _recorders; }
    /// "uav" / "ugv" (case-insensitive), or nullptr.
    TarRosbagRecorder *recorder(const QString &vehicleId) const;

    bool anyRecording() const;
    bool allRecording() const;
    bool anyBusy() const;

    /// Network plus each vehicle's state (assistant context and the rosbag_status tool).
    QString statusText() const;

signals:
    void stateChanged();

private:
    TarFieldNetwork *_network = nullptr;
    QList<TarRosbagRecorder *> _recorders;
};
