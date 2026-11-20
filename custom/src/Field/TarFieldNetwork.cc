#include "TarFieldNetwork.h"

#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

#include "LinkConfiguration.h"
#include "LinkManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "UDPLink.h"

QGC_LOGGING_CATEGORY(TarFieldNetworkLog, "Custom.TarFieldNetwork")

namespace {

// Ground-station Wi-Fi networks on which the vehicles are reached over their field-router addresses.
const QStringList kRouterSsids = {QStringLiteral("tar-ravc")};
// Campus Wi-Fi: reach the vehicles over Tailscale (also the fallback for any other network).
const QStringList kTailSsids = {QStringLiteral("utexas"), QStringLiteral("utexas-iot")};

QList<LinkConfiguration *> linkConfigurations()
{
    QList<LinkConfiguration *> out;
    LinkManager *lm = LinkManager::instance();
    auto *list = lm ? lm->property("linkConfigurations").value<QmlObjectListModel *>() : nullptr;
    for (int i = 0; list && i < list->count(); i++) {
        if (auto *c = qobject_cast<LinkConfiguration *>(list->get(i))) {
            out << c;
        }
    }
    return out;
}

LinkConfiguration *findLink(const QString &name)
{
    for (LinkConfiguration *c : linkConfigurations()) {
        if (c->name().compare(name, Qt::CaseInsensitive) == 0) {
            return c;
        }
    }
    return nullptr;
}

QString hostEntry(const QString &ip)
{
    return QStringLiteral("%1:%2").arg(ip).arg(TarFieldNetwork::kVehicleMavlinkPort);
}

void applyUdpSettings(UDPConfiguration *udp, quint16 localPort, const QString &ip)
{
    udp->setLocalPort(localPort);
    const QStringList hosts = udp->hostList();
    for (const QString &h : hosts) {
        udp->removeHost(h);
    }
    udp->addHost(ip, TarFieldNetwork::kVehicleMavlinkPort);
}

} // namespace

TarFieldNetwork::TarFieldNetwork(QObject *parent)
    : QObject(parent)
{
    _timer.setInterval(kDetectIntervalMs);
    (void) connect(&_timer, &QTimer::timeout, this, &TarFieldNetwork::_detect);
}

const QList<TarFieldVehicle> &TarFieldNetwork::vehicles()
{
    static const QList<TarFieldVehicle> list = {
        {QStringLiteral("uav"), QStringLiteral("UAV"), QStringLiteral("100.66.89.102"), QStringLiteral("192.168.0.100"), 14561, 14562},
        {QStringLiteral("ugv"), QStringLiteral("UGV"), QStringLiteral("100.81.151.45"), QStringLiteral("192.168.0.200"), 14571, 14572},
    };
    return list;
}

const TarFieldVehicle *TarFieldNetwork::vehicle(const QString &id)
{
    for (const TarFieldVehicle &v : vehicles()) {
        if (v.id.compare(id, Qt::CaseInsensitive) == 0) {
            return &v;
        }
    }
    return nullptr;
}

void TarFieldNetwork::init()
{
    _ensureLinks();
    _detect();
    _timer.start();
}

QString TarFieldNetwork::modeText() const
{
    const QString network = _ssid.isEmpty() ? tr("no Wi-Fi detected") : _ssid;
    return _mode == Mode::Router ? tr("%1: field router addresses (-router)").arg(network)
                                 : tr("%1: Tailscale addresses (-tail)").arg(network);
}

QString TarFieldNetwork::host(const TarFieldVehicle &vehicle) const
{
    return QStringLiteral("%1@%2").arg(QLatin1String(kSshUser), _mode == Mode::Router ? vehicle.routerIp : vehicle.tailIp);
}

TarFieldNetwork::Mode TarFieldNetwork::_modeForSsid(const QString &ssid)
{
    for (const QString &s : kRouterSsids) {
        if (ssid.compare(s, Qt::CaseInsensitive) == 0) {
            return Mode::Router;
        }
    }
    return Mode::Tail;   // kTailSsids, other networks and no Wi-Fi
}

void TarFieldNetwork::_detect()
{
    if (_process) {
        return;
    }
    _process = new QProcess(this);
#if defined(Q_OS_WIN)
    _process->setProgram(QStringLiteral("netsh"));
    _process->setArguments({QStringLiteral("wlan"), QStringLiteral("show"), QStringLiteral("interfaces")});
#elif defined(Q_OS_LINUX)
    _process->setProgram(QStringLiteral("nmcli"));
    _process->setArguments({QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("ACTIVE,SSID"), QStringLiteral("dev"), QStringLiteral("wifi")});
#else
    _process->deleteLater();
    _process = nullptr;
    _detected(QString());
    return;
#endif
    (void) connect(_process, &QProcess::finished, this, [this]() {
        const QString out = QString::fromLocal8Bit(_process->readAllStandardOutput());
        _process->deleteLater();
        _process = nullptr;
        QString ssid;
#if defined(Q_OS_WIN)
        // "    SSID                   : tar-ravc" (not "BSSID"); first connected interface wins.
        static const QRegularExpression re(QStringLiteral("^\\s*SSID\\s*:\\s*(.+?)\\s*$"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch m = re.match(out);
        if (m.hasMatch()) {
            ssid = m.captured(1);
        }
#else
        for (const QString &line : out.split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("yes:"))) {
                ssid = line.mid(4).trimmed();
                break;
            }
        }
#endif
        _detected(ssid);
    });
    (void) connect(_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && _process) {
            _process->deleteLater();
            _process = nullptr;
            _detected(QString());
        }
    });
    _process->start();
}

void TarFieldNetwork::_detected(const QString &ssid)
{
    const Mode mode = _modeForSsid(ssid);
    if (_detectedOnce && ssid == _ssid && mode == _mode) {
        return;
    }
    const bool modeChanged = !_detectedOnce || mode != _mode;
    _detectedOnce = true;
    _ssid = ssid;
    _mode = mode;
    qCInfo(TarFieldNetworkLog) << "Network:" << modeText();
    if (modeChanged) {
        _connectLinksForMode();
    }
    emit changed();
}

void TarFieldNetwork::_ensureLinks()
{
    LinkManager *lm = LinkManager::instance();
    if (!lm) {
        return;
    }
    for (const TarFieldVehicle &v : vehicles()) {
        const QList<QPair<QString, QPair<quint16, QString>>> wanted = {
            {v.tailLinkName(), {v.tailLocalPort, v.tailIp}},
            {v.routerLinkName(), {v.routerLocalPort, v.routerIp}},
        };
        for (const auto &w : wanted) {
            const QString &name = w.first;
            const quint16 port = w.second.first;
            const QString &ip = w.second.second;
            LinkConfiguration *existing = findLink(name);
            if (!existing) {
                auto *udp = qobject_cast<UDPConfiguration *>(lm->createConfiguration(LinkConfiguration::TypeUdp, name));
                if (!udp) {
                    qCWarning(TarFieldNetworkLog) << "Could not create comm link" << name;
                    continue;
                }
                applyUdpSettings(udp, port, ip);
                lm->endCreateConfiguration(udp);
                qCInfo(TarFieldNetworkLog) << "Created comm link" << name << "->" << hostEntry(ip) << "local port" << port;
                continue;
            }
            auto *udp = qobject_cast<UDPConfiguration *>(existing);
            if (!udp) {
                qCWarning(TarFieldNetworkLog) << "Comm link" << name << "exists but is not UDP; leaving it alone";
                continue;
            }
            if (udp->localPort() == port && udp->hostList() == QStringList{hostEntry(ip)}) {
                continue;
            }
            // Someone edited a hard-coded link: put it back (reconnecting it if it was up).
            const bool active = udp->linkActive();
            if (active) {
                lm->disconnectLinkConfiguration(udp);
            }
            auto *edited = qobject_cast<UDPConfiguration *>(lm->startConfigurationEditing(udp));
            if (!edited) {
                continue;
            }
            applyUdpSettings(edited, port, ip);
            lm->endConfigurationEditing(udp, edited);
            if (active) {
                lm->createConnectedLink(udp);
            }
            qCInfo(TarFieldNetworkLog) << "Restored comm link" << name << "->" << hostEntry(ip) << "local port" << port;
        }
    }
}

void TarFieldNetwork::_connectLinksForMode()
{
    LinkManager *lm = LinkManager::instance();
    if (!lm) {
        return;
    }
    for (const TarFieldVehicle &v : vehicles()) {
        const QString want = _mode == Mode::Router ? v.routerLinkName() : v.tailLinkName();
        const QString other = _mode == Mode::Router ? v.tailLinkName() : v.routerLinkName();
        if (LinkConfiguration *o = findLink(other); o && o->linkActive()) {
            lm->disconnectLinkConfiguration(o);
            qCInfo(TarFieldNetworkLog) << "Disconnected" << other;
        }
        if (LinkConfiguration *w = findLink(want); w && !w->linkActive()) {
            lm->createConnectedLink(w);
            qCInfo(TarFieldNetworkLog) << "Connecting" << want;
        }
    }
}
