#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>

#include "MavlinkFrameParser.h"
#include "RouterConfig.h"

class QTcpServer;
class QgcLinkEndpoint;
class RouterEndpoint;
class TcpEndpoint;

/// The routing engine. Runs on its own thread (owned by TarRouter, or by tar_router_cli).
///
/// Routing follows mavlink-router: a frame from endpoint S is written to every other endpoint E where
///  - the frame has no target (target system 0), or
///  - E has already seen traffic from the target system (and component, if one is set and E knows it).
/// If no endpoint has seen the target system yet, the frame is sent to all other endpoints so that
/// first contact (e.g. a GCS commanding a vehicle that has only spoken on one link) still works.
class RouterCore : public QObject
{
    Q_OBJECT

public:
    explicit RouterCore(QObject *parent = nullptr);
    ~RouterCore() override;

public slots:
    void start(const RouterConfig &config);
    void stop();
    /// A frame QGC received on one of its own links (drone on UDP 14550, radio, ...). Each QGC link
    /// becomes a router endpoint, created on first traffic.
    void injectQgcLinkFrame(quint64 linkKey, const QString &linkName, const QByteArray &frame);
    void removeQgcLink(quint64 linkKey);

signals:
    /// Emitted once per second: list of QVariantMap, one per endpoint (see _snapshot()).
    void statsUpdated(const QVariantList &endpoints);
    void logMessage(const QString &message);
    /// Bytes the router wants sent out of a QGC link (towards the vehicle). Handled on the main thread.
    void writeToQgcLink(quint64 linkKey, const QByteArray &bytes);
    void started();
    void stopped();

private:
    void _addEndpoint(RouterEndpoint *endpoint);
    void _route(RouterEndpoint *source, const MavFrame &frame);
    /// True when the endpoint is one of QGC's own links bridged into the router.
    bool _isQgcLink(const RouterEndpoint *endpoint) const;
    void _acceptTcpClients();
    void _removeTcpClient(TcpEndpoint *endpoint);
    QVariantList _snapshot();

    QList<RouterEndpoint *> _endpoints;
    QHash<quint64, QgcLinkEndpoint *> _qgcLinks;
    QTcpServer *_tcpServer = nullptr;
    /// Child (not a plain member) so moveToThread() carries it to the router thread.
    QTimer *_statsTimer = nullptr;
    bool _active = false;
    int _tcpClientCounter = 0;
    QHash<RouterEndpoint *, QPair<quint64, quint64>> _lastCounts; // for per-second rates
};
