#include "RouterCore.h"

#include <QtCore/QVariantMap>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include "RouterEndpoint.h"

RouterCore::RouterCore(QObject *parent)
    : QObject(parent)
{
    _statsTimer = new QTimer(this);
    _statsTimer->setInterval(1000);
    (void) connect(_statsTimer, &QTimer::timeout, this, [this]() { emit statsUpdated(_snapshot()); });
}

RouterCore::~RouterCore()
{
    stop();
}

void RouterCore::start(const RouterConfig &config)
{
    stop();

    const auto onFrame = [this](RouterEndpoint *source, const MavFrame &frame) { _route(source, frame); };

    for (const RouterEndpointConfig &ep : config.endpoints) {
        RouterEndpoint *endpoint = nullptr;
        switch (ep.type) {
        case RouterEndpointConfig::Type::UdpServer:
            endpoint = new UdpServerEndpoint(ep, onFrame, this);
            break;
        case RouterEndpointConfig::Type::UdpClient:
            endpoint = new UdpClientEndpoint(ep, onFrame, 0, QStringLiteral("UDP client"), this);
            break;
        case RouterEndpointConfig::Type::TcpClient:
            endpoint = new TcpEndpoint(ep, onFrame, this);
            break;
        case RouterEndpointConfig::Type::Uart:
#ifndef QGC_NO_SERIAL_LINK
            endpoint = new UartEndpoint(ep, onFrame, this);
#else
            emit logMessage(QStringLiteral("%1: serial endpoints not supported on this platform").arg(ep.name));
#endif
            break;
        }
        if (endpoint) {
            endpoint->setOutFilters(ep);
            _addEndpoint(endpoint);
        }
    }

    if (config.tcpServerPort != 0) {
        _tcpServer = new QTcpServer(this);
        (void) connect(_tcpServer, &QTcpServer::newConnection, this, &RouterCore::_acceptTcpClients);
        if (_tcpServer->listen(QHostAddress::AnyIPv4, config.tcpServerPort)) {
            emit logMessage(QStringLiteral("TCP server listening on port %1").arg(config.tcpServerPort));
        } else {
            emit logMessage(QStringLiteral("TCP server cannot listen on port %1: %2")
                                .arg(config.tcpServerPort)
                                .arg(_tcpServer->errorString()));
        }
    }

    _statsTimer->start();
    _active = true;
    emit started();
    emit statsUpdated(_snapshot());
}

void RouterCore::stop()
{
    const bool wasRunning = _active;
    _active = false;
    _statsTimer->stop();
    if (_tcpServer) {
        _tcpServer->close();
        delete _tcpServer;
        _tcpServer = nullptr;
    }
    qDeleteAll(_endpoints);
    _endpoints.clear();
    _qgcLinks.clear();
    _lastCounts.clear();
    if (wasRunning) {
        emit statsUpdated({});
        emit stopped();
    }
}

void RouterCore::injectQgcLinkFrame(quint64 linkKey, const QString &linkName, const QByteArray &frame)
{
    if (!_active) {
        return;
    }
    QgcLinkEndpoint *endpoint = _qgcLinks.value(linkKey);
    if (!endpoint) {
        endpoint = new QgcLinkEndpoint(
            linkKey, linkName, [this](RouterEndpoint *source, const MavFrame &f) { _route(source, f); },
            [this](quint64 key, const QByteArray &bytes) { emit writeToQgcLink(key, bytes); }, this);
        _qgcLinks.insert(linkKey, endpoint);
        emit logMessage(QStringLiteral("bridging QGC link '%1'").arg(linkName));
        _addEndpoint(endpoint);
    }
    endpoint->inject(frame);
}

void RouterCore::removeQgcLink(quint64 linkKey)
{
    QgcLinkEndpoint *endpoint = _qgcLinks.take(linkKey);
    if (endpoint && _endpoints.removeOne(endpoint)) {
        emit logMessage(QStringLiteral("QGC link '%1' closed").arg(endpoint->name()));
        _lastCounts.remove(endpoint);
        endpoint->deleteLater();
    }
}

void RouterCore::_addEndpoint(RouterEndpoint *endpoint)
{
    (void) connect(endpoint, &RouterEndpoint::logMessage, this, &RouterCore::logMessage);
    _endpoints.append(endpoint);
    endpoint->open();
}

bool RouterCore::_isQgcLink(const RouterEndpoint *endpoint) const
{
    for (const QgcLinkEndpoint *link : _qgcLinks) {
        if (static_cast<const RouterEndpoint *>(link) == endpoint) {
            return true;
        }
    }
    return false;
}

void RouterCore::_route(RouterEndpoint *source, const MavFrame &frame)
{
    // QGC already talks to every vehicle over that vehicle's own link; the bridge only exists so router clients
    // (MAVSDK, TCP, ...) can reach the vehicles. Never relay one QGC link's traffic into another: with several
    // vehicles connected, each would receive the others' full telemetry stream (e.g. ~350 msg/s of UAV
    // telemetry sent over Tailscale to the UGV).
    const bool fromQgcLink = _isQgcLink(source);
    const auto excluded = [this, source, fromQgcLink](const RouterEndpoint *endpoint) {
        return (endpoint == source) || (fromQgcLink && _isQgcLink(endpoint));
    };

    if (frame.targetSystem != 0) {
        bool anyKnowsTarget = false;
        for (RouterEndpoint *endpoint : std::as_const(_endpoints)) {
            if (!endpoint->hasSeenSystem(frame.targetSystem)) {
                continue;
            }
            // A target that is only reachable through another QGC link counts as known (so the frame is not
            // broadcast instead), but is not relayed there.
            anyKnowsTarget = anyKnowsTarget || (endpoint != source);
            if (excluded(endpoint)) {
                continue;
            }
            // A specific component is only enforced when this endpoint has actually seen that component;
            // otherwise components behind a companion computer would be unreachable.
            if ((frame.targetComponent != 0) && !endpoint->hasSeenComponent(frame.targetSystem, frame.targetComponent)) {
                bool knowsComponentElsewhere = false;
                for (RouterEndpoint *other : std::as_const(_endpoints)) {
                    if ((other != source) && other->hasSeenComponent(frame.targetSystem, frame.targetComponent)) {
                        knowsComponentElsewhere = true;
                        break;
                    }
                }
                if (knowsComponentElsewhere) {
                    continue;
                }
            }
            (void) endpoint->send(frame);
        }
        if (anyKnowsTarget) {
            return;
        }
    }

    for (RouterEndpoint *endpoint : std::as_const(_endpoints)) {
        if (!excluded(endpoint)) {
            (void) endpoint->send(frame);
        }
    }
}

void RouterCore::_acceptTcpClients()
{
    while (_tcpServer && _tcpServer->hasPendingConnections()) {
        QTcpSocket *socket = _tcpServer->nextPendingConnection();
        auto *endpoint = new TcpEndpoint(
            socket, QStringLiteral("tcp-client-%1").arg(++_tcpClientCounter),
            [this](RouterEndpoint *source, const MavFrame &frame) { _route(source, frame); }, this);
        (void) connect(endpoint, &TcpEndpoint::closed, this, &RouterCore::_removeTcpClient, Qt::QueuedConnection);
        emit logMessage(QStringLiteral("TCP server: client %1:%2 connected")
                            .arg(socket->peerAddress().toString())
                            .arg(socket->peerPort()));
        _addEndpoint(endpoint);
    }
}

void RouterCore::_removeTcpClient(TcpEndpoint *endpoint)
{
    if (_endpoints.removeOne(endpoint)) {
        _lastCounts.remove(endpoint);
        endpoint->deleteLater();
    }
}

QVariantList RouterCore::_snapshot()
{
    QVariantList list;
    for (RouterEndpoint *endpoint : std::as_const(_endpoints)) {
        const QPair<quint64, quint64> last = _lastCounts.value(endpoint, qMakePair(endpoint->rxMessages(), endpoint->txMessages()));
        QVariantMap map;
        map[QStringLiteral("name")] = endpoint->name();
        map[QStringLiteral("kind")] = endpoint->kind();
        map[QStringLiteral("detail")] = endpoint->detail();
        map[QStringLiteral("connected")] = endpoint->isConnected();
        map[QStringLiteral("systems")] = endpoint->seenSystemsText();
        map[QStringLiteral("sentSystems")] = endpoint->sentSystemsText();
        map[QStringLiteral("rxMessages")] = endpoint->rxMessages();
        map[QStringLiteral("txMessages")] = endpoint->txMessages();
        map[QStringLiteral("rxRate")] = endpoint->rxMessages() - last.first;
        map[QStringLiteral("txRate")] = endpoint->txMessages() - last.second;
        map[QStringLiteral("rxBytes")] = endpoint->rxBytes();
        map[QStringLiteral("txBytes")] = endpoint->txBytes();
        map[QStringLiteral("crcErrors")] = endpoint->crcErrors();
        map[QStringLiteral("txFiltered")] = endpoint->txFiltered();
        list.append(map);
        _lastCounts.insert(endpoint, qMakePair(endpoint->rxMessages(), endpoint->txMessages()));
    }
    return list;
}
