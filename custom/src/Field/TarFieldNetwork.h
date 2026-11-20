#pragma once

#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

class QProcess;

Q_DECLARE_LOGGING_CATEGORY(TarFieldNetworkLog)

/// One TAR vehicle and its two hard-coded addresses: Tailscale ("-tail") and the field router ("-router").
struct TarFieldVehicle
{
    QString id;             ///< "uav" / "ugv"
    QString label;          ///< "UAV" / "UGV"
    QString tailIp;         ///< Tailscale address
    QString routerIp;       ///< static address on the field router (tar-ravc)
    quint16 tailLocalPort;  ///< local UDP port of the <id>-tail comm link
    quint16 routerLocalPort;///< local UDP port of the <id>-router comm link

    QString tailLinkName() const { return id + QStringLiteral("-tail"); }
    QString routerLinkName() const { return id + QStringLiteral("-router"); }
};

/// Which network the ground station is on, and the TAR comm links that go with it.
///
/// The ground station's Wi-Fi decides how the vehicles are reached: on the campus Wi-Fi (utexas, utexas-iot)
/// over Tailscale, on the field router (tar-ravc) over its LAN. Anything else (Ethernet, other Wi-Fi) uses
/// Tailscale. Four UDP comm links (uav-tail, uav-router, ugv-tail, ugv-router) to each vehicle's
/// mavlink-router (port 14550) are created at startup if missing; when the network changes, the matching
/// pair is connected and the other pair disconnected. Manual connects/disconnects are left alone until the
/// network changes again. The ROS bag recorders use the same address choice (host()).
class TarFieldNetwork : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString ssid     READ ssid       NOTIFY changed)
    Q_PROPERTY(QString mode     READ mode       NOTIFY changed)   ///< "tail" or "router"
    Q_PROPERTY(QString modeText READ modeText   NOTIFY changed)

public:
    enum class Mode { Tail, Router };

    explicit TarFieldNetwork(QObject *parent = nullptr);

    static const QList<TarFieldVehicle> &vehicles();
    static const TarFieldVehicle *vehicle(const QString &id);
    static constexpr quint16 kVehicleMavlinkPort = 14550;
    static constexpr const char *kSshUser = "tar";

    /// Creates the comm links and starts watching the network (call once LinkManager has loaded its links).
    void init();

    QString ssid() const { return _ssid; }
    Mode modeValue() const { return _mode; }
    QString mode() const { return _mode == Mode::Router ? QStringLiteral("router") : QStringLiteral("tail"); }
    QString modeText() const;
    /// ssh destination (user@ip) for a vehicle on the current network.
    QString host(const TarFieldVehicle &vehicle) const;

signals:
    void changed();

private:
    void _detect();
    void _detected(const QString &ssid);
    void _ensureLinks();
    void _connectLinksForMode();

    static Mode _modeForSsid(const QString &ssid);

    static constexpr int kDetectIntervalMs = 10000;

    QString _ssid;
    Mode _mode = Mode::Tail;
    bool _detectedOnce = false;
    QProcess *_process = nullptr;
    QTimer _timer;
};
