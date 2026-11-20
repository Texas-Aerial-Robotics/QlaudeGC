#pragma once

#include <QtCore/QHash>
#include <QtCore/QMap>
#include <QtQml/QQmlApplicationEngine>
#include <functional>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariant>

#include "TarRosbag.h"

class QTcpServer;
class QTcpSocket;
class Fact;
class LinkConfiguration;
class Vehicle;

/// Minimal MCP server (streamable HTTP transport, JSON responses only) on 127.0.0.1 that gives the Claude
/// sidebar access to QGC: read vehicle parameters, and change them after the user approves in the chat.
/// Runs on the main thread so it can touch Vehicle/Fact objects directly.
class TarMcpServer : public QObject
{
    Q_OBJECT

public:
    explicit TarMcpServer(QObject *parent = nullptr);

    bool listening() const;
    QString url() const;
    QString token() const { return _token; }

    /// Gives the object tools access to the QGroundControl QML singleton and the main window.
    void setQmlEngine(QQmlApplicationEngine *engine) { _engine = engine; }
    /// Enables the rosbag_* tools (ROS bag recording on the companion computer).
    void setRosbag(TarRosbag *rosbag) { _rosbag = rosbag; }

    /// Answer a pending approval raised with approvalRequested().
    void resolveApproval(int approvalId, bool approved);
    /// Reject every pending approval (e.g. the Claude run was stopped).
    void rejectAllPending(const QString &reason);

signals:
    /// A tool wants to change the vehicle. `summary` lists the changes; `warning` is empty or a caution line.
    void approvalRequested(int approvalId, const QString &summary, const QString &warning);
    /// The approval was resolved: `outcome` is approved/rejected/expired plus a short result.
    void approvalFinished(int approvalId, const QString &outcome);

private:
    struct HttpConnection {
        QByteArray buffer;
    };

    struct ParamChange {
        QString name;
        QVariant oldValue;
        QVariant newValue;   ///< typed raw value
        QString units;
        bool rebootRequired = false;
        QVariant typedValue; ///< app settings: value to apply (old/newValue hold display text)
    };

    struct PendingApproval {
        QPointer<QTcpSocket> socket;
        QJsonValue rpcId;
        int vehicleId = 0;
        int componentId = 0;
        QList<ParamChange> changes;
        bool appSettings = false; ///< changes are QGC app settings, not vehicle parameters
        QStringList lines;        ///< card lines for generic actions (instead of changes)
        std::function<QString(bool &ok)> apply; ///< generic action run on approval
        /// generic action that finishes later: call done(ok, text) exactly once
        std::function<void(std::function<void(bool ok, const QString &text)> done)> applyAsync;
    };

    void _onNewConnection();
    void _onReadyRead(QTcpSocket *socket);
    void _handleHttpRequest(QTcpSocket *socket, const QByteArray &method, const QByteArray &path,
                            const QHash<QByteArray, QByteArray> &headers, const QByteArray &body);
    void _sendHttp(QTcpSocket *socket, int status, const QByteArray &body, const QByteArray &contentType = "application/json");
    void _sendRpcResult(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &result);
    void _sendRpcError(QTcpSocket *socket, const QJsonValue &id, int code, const QString &message);
    void _sendToolResult(QTcpSocket *socket, const QJsonValue &id, const QString &text, bool isError = false);

    QJsonArray _toolDefinitions() const;
    void _callTool(QTcpSocket *socket, const QJsonValue &id, const QString &name, const QJsonObject &args);

    Vehicle *_vehicle(const QJsonObject &args, QString &error);
    QString _toolListVehicles();
    QString _toolGetParameters(const QJsonObject &args, bool &isError);
    QString _toolSearchParameters(const QJsonObject &args, bool &isError);
    void _toolSetParameters(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args);
    void _applyApproval(int approvalId);

    /// App settings are addressed as "<group>.<name>", e.g. "video.rtspUrl".
    static QList<QPair<QString, Fact *>> _appSettings();
    static Fact *_appSetting(const QString &key);
    QString _toolSearchAppSettings(const QJsonObject &args);
    QString _toolGetAppSettings(const QJsonObject &args);
    void _toolSetAppSettings(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args);
    /// Validate one requested change; returns false with `problem` set, or fills `typed`.
    static bool _validateChange(Fact *fact, const QString &name, QVariant requested, QVariant &typed, QString &problem);
    // Object tree (TarMcpObjects.cc)
    static QJsonValue _toJson(const QVariant &value, int depth = 0);
    bool _fromJson(const QJsonValue &json, const QMetaType &type, QVariant &out, QString &error);
    QMap<QString, QObject *> _roots() const;
    QObject *_resolvePath(const QString &path, QString &error);
    QStringList _vehicleWarnings(QObject *target) const;
    QString _invoke(QObject *obj, const QString &methodName, const QJsonArray &jsonArgs, bool &ok);
    QString _toolInspect(const QJsonObject &args, bool &isError);
    void _toolSetProperty(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args);
    void _toolCall(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args);
    QString _toolListCommLinks();
    bool _applyLinkSettings(LinkConfiguration *config, const QJsonObject &settings, QString &error);
    void _toolSetCommLink(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args);
    void _toolCommLinkAction(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args);

    // ROS bag recording on the companion
    void _toolRosbagStatus(QTcpSocket *socket, const QJsonValue &id);
    void _toolRosbagStartStop(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args, bool start);

    void _requestApproval(const PendingApproval &pending, const QString &title, const QString &reason, QStringList warnings);

    QTcpServer *_server = nullptr;
    QString _token;
    QHash<QTcpSocket *, HttpConnection> _connections;
    QHash<int, PendingApproval> _pending;
    int _nextApprovalId = 1;
    QPointer<QQmlApplicationEngine> _engine;
    QPointer<TarRosbag> _rosbag;
    QHash<int, QPointer<QObject>> _handles; ///< objects returned by qgc_call, addressed as $N
    int _nextHandle = 1;
};
