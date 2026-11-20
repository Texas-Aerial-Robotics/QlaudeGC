#include "TarMcpServer.h"

#include <memory>

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include "Fact.h"
#include "FactMetaData.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "TarRosbag.h"
#include "Vehicle.h"

QGC_LOGGING_CATEGORY(TarMcpServerLog, "Custom.TarMcpServer")

namespace {

constexpr int kMaxRequestBytes = 1024 * 1024;
constexpr int kApprovalTimeoutMs = 10 * 60 * 1000;
constexpr int kWriteConfirmTimeoutMs = 8000;
constexpr int kMaxChangesPerCall = 50;

QJsonObject schema(const QJsonObject &properties, const QStringList &required = {})
{
    QJsonObject s{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), properties}};
    if (!required.isEmpty()) {
        s.insert(QStringLiteral("required"), QJsonArray::fromStringList(required));
    }
    return s;
}

QJsonObject prop(const QString &type, const QString &description)
{
    return {{QStringLiteral("type"), type}, {QStringLiteral("description"), description}};
}

QString variantText(const QVariant &v)
{
    if (v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Float) {
        return QString::number(v.toDouble(), 'g', 9);
    }
    return v.toString();
}

QJsonObject describeFact(Fact *fact, bool full)
{
    QJsonObject o{
        {QStringLiteral("name"), fact->name()},
        {QStringLiteral("value"), fact->rawValueStringFullPrecision()},
    };
    if (!fact->rawUnits().isEmpty()) {
        o.insert(QStringLiteral("units"), fact->rawUnits());
    }
    if (!fact->shortDescription().isEmpty()) {
        o.insert(QStringLiteral("description"), fact->shortDescription());
    }
    const QStringList enumStrings = fact->enumStrings();
    const QVariantList enumValues = fact->enumValues();
    if (!enumStrings.isEmpty() && enumStrings.size() == enumValues.size()) {
        const int idx = static_cast<int>(enumValues.indexOf(fact->rawValue()));
        if (idx >= 0) {
            o.insert(QStringLiteral("value_label"), enumStrings.at(idx));
        }
    }
    if (!full) {
        return o;
    }
    o.insert(QStringLiteral("type"), FactMetaData::typeToString(fact->type()));
    if (!fact->longDescription().isEmpty() && fact->longDescription() != fact->shortDescription()) {
        o.insert(QStringLiteral("long_description"), fact->longDescription());
    }
    if (fact->rawMin().isValid()) {
        o.insert(QStringLiteral("min"), variantText(fact->rawMin()));
    }
    if (fact->rawMax().isValid()) {
        o.insert(QStringLiteral("max"), variantText(fact->rawMax()));
    }
    if (fact->defaultValueAvailable()) {
        o.insert(QStringLiteral("default"), variantText(fact->rawDefaultValue()));
    }
    if (!enumStrings.isEmpty() && enumStrings.size() == enumValues.size()) {
        QJsonArray options;
        for (int i = 0; i < enumStrings.size() && i < 60; i++) {
            options.append(QStringLiteral("%1 = %2").arg(variantText(enumValues.at(i)), enumStrings.at(i)));
        }
        o.insert(QStringLiteral("options"), options);
    }
    const QStringList bitStrings = fact->bitmaskStrings();
    const QVariantList bitValues = fact->bitmaskValues();
    if (!bitStrings.isEmpty() && bitStrings.size() == bitValues.size()) {
        QJsonArray bits;
        for (int i = 0; i < bitStrings.size(); i++) {
            bits.append(QStringLiteral("%1 = %2").arg(variantText(bitValues.at(i)), bitStrings.at(i)));
        }
        o.insert(QStringLiteral("bitmask"), bits);
    }
    if (fact->vehicleRebootRequired()) {
        o.insert(QStringLiteral("reboot_required"), true);
    }
    if (fact->readOnly()) {
        o.insert(QStringLiteral("read_only"), true);
    }
    return o;
}

QString toJsonText(const QJsonValue &v)
{
    if (v.isObject()) {
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented));
    }
    return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented));
}

} // namespace

TarMcpServer::TarMcpServer(QObject *parent)
    : QObject(parent)
    , _server(new QTcpServer(this))
{
    QByteArray bytes(24, Qt::Uninitialized);
    for (char &c : bytes) {
        c = static_cast<char>(QRandomGenerator::system()->bounded(256));
    }
    _token = QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

    (void) connect(_server, &QTcpServer::newConnection, this, &TarMcpServer::_onNewConnection);
    if (!_server->listen(QHostAddress::LocalHost, 0)) {
        qCWarning(TarMcpServerLog) << "MCP server failed to listen:" << _server->errorString();
    } else {
        qCDebug(TarMcpServerLog) << "MCP server on" << url();
    }
}

bool TarMcpServer::listening() const
{
    return _server->isListening();
}

QString TarMcpServer::url() const
{
    return QStringLiteral("http://127.0.0.1:%1/mcp").arg(_server->serverPort());
}

// ---------------------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------------------

void TarMcpServer::_onNewConnection()
{
    while (QTcpSocket *socket = _server->nextPendingConnection()) {
        _connections.insert(socket, HttpConnection{});
        (void) connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { _onReadyRead(socket); });
        (void) connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            _connections.remove(socket);
            socket->deleteLater();
        });
    }
}

void TarMcpServer::_onReadyRead(QTcpSocket *socket)
{
    auto it = _connections.find(socket);
    if (it == _connections.end()) {
        return;
    }
    it->buffer += socket->readAll();
    if (it->buffer.size() > kMaxRequestBytes) {
        _sendHttp(socket, 413, QByteArray());
        socket->disconnectFromHost();
        return;
    }

    while (true) {
        const qsizetype headerEnd = it->buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        const QList<QByteArray> lines = it->buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        QHash<QByteArray, QByteArray> headers;
        for (qsizetype i = 1; i < lines.size(); i++) {
            const qsizetype colon = lines.at(i).indexOf(':');
            if (colon > 0) {
                headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
            }
        }
        const qsizetype length = headers.value("content-length", "0").toLongLong();
        if (it->buffer.size() < headerEnd + 4 + length) {
            return;
        }
        const QByteArray body = it->buffer.mid(headerEnd + 4, length);
        it->buffer.remove(0, headerEnd + 4 + length);
        _handleHttpRequest(socket, requestLine.value(0), requestLine.value(1), headers, body);
        it = _connections.find(socket);
        if (it == _connections.end()) {
            return;
        }
    }
}

void TarMcpServer::_sendHttp(QTcpSocket *socket, int status, const QByteArray &body, const QByteArray &contentType)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    static const QHash<int, QByteArray> reasons = {
        {200, "OK"}, {202, "Accepted"}, {400, "Bad Request"}, {401, "Unauthorized"},
        {404, "Not Found"}, {405, "Method Not Allowed"}, {413, "Payload Too Large"},
    };
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reasons.value(status, "Error") + "\r\n";
    if (!body.isEmpty()) {
        response += "Content-Type: " + contentType + "\r\n";
    }
    if (status == 405) {
        response += "Allow: POST\r\n";
    }
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    (void) socket->write(response);
}

void TarMcpServer::_handleHttpRequest(QTcpSocket *socket, const QByteArray &method, const QByteArray &path,
                                      const QHash<QByteArray, QByteArray> &headers, const QByteArray &body)
{
    if (!path.startsWith("/mcp")) {
        _sendHttp(socket, 404, QByteArray());
        return;
    }
    if (headers.value("authorization") != "Bearer " + _token.toLatin1()) {
        _sendHttp(socket, 401, QByteArray());
        return;
    }
    if (method == "DELETE") {
        _sendHttp(socket, 200, QByteArray());
        return;
    }
    if (method != "POST") {
        _sendHttp(socket, 405, QByteArray()); // no server-initiated SSE stream
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        _sendRpcError(socket, QJsonValue(), -32700, QStringLiteral("Parse error"));
        return;
    }
    const QJsonObject req = doc.object();
    const QString rpcMethod = req.value(QStringLiteral("method")).toString();
    if (!req.contains(QStringLiteral("id"))) {
        _sendHttp(socket, 202, QByteArray()); // notification
        return;
    }
    const QJsonValue id = req.value(QStringLiteral("id"));
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();

    if (rpcMethod == QLatin1String("initialize")) {
        const QString version = params.value(QStringLiteral("protocolVersion")).toString(QStringLiteral("2025-06-18"));
        _sendRpcResult(socket, id, QJsonObject{
            {QStringLiteral("protocolVersion"), version},
            {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{}}}},
            {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("qgc")}, {QStringLiteral("version"), QStringLiteral("1.0")}}},
            {QStringLiteral("instructions"), QStringLiteral("Tools for the QlaudeGC ground station. Parameter values are raw values in the units the autopilot uses. Changes are only applied after the user approves them in QGC.")},
        });
    } else if (rpcMethod == QLatin1String("ping")) {
        _sendRpcResult(socket, id, QJsonObject{});
    } else if (rpcMethod == QLatin1String("tools/list")) {
        _sendRpcResult(socket, id, QJsonObject{{QStringLiteral("tools"), _toolDefinitions()}});
    } else if (rpcMethod == QLatin1String("tools/call")) {
        _callTool(socket, id, params.value(QStringLiteral("name")).toString(), params.value(QStringLiteral("arguments")).toObject());
    } else {
        _sendRpcError(socket, id, -32601, QStringLiteral("Method not found: %1").arg(rpcMethod));
    }
}

void TarMcpServer::_sendRpcResult(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &result)
{
    const QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("result"), result}};
    _sendHttp(socket, 200, QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void TarMcpServer::_sendRpcError(QTcpSocket *socket, const QJsonValue &id, int code, const QString &message)
{
    const QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
                          {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
    _sendHttp(socket, 200, QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void TarMcpServer::_sendToolResult(QTcpSocket *socket, const QJsonValue &id, const QString &text, bool isError)
{
    _sendRpcResult(socket, id, QJsonObject{
        {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}}}},
        {QStringLiteral("isError"), isError},
    });
}

// ---------------------------------------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------------------------------------

QJsonArray TarMcpServer::_toolDefinitions() const
{
    const QJsonObject vehicleProp = prop(QStringLiteral("integer"), QStringLiteral("MAVLink system id of the vehicle. Defaults to the active vehicle."));
    const QJsonObject componentProp = prop(QStringLiteral("integer"), QStringLiteral("Component id. Defaults to the autopilot."));

    QJsonObject namesProp = prop(QStringLiteral("array"), QStringLiteral("Exact parameter names, e.g. [\"MPC_XY_VEL_MAX\"]."));
    namesProp.insert(QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});

    QJsonObject changesProp = prop(QStringLiteral("array"), QStringLiteral("Parameters to change. value is the raw value (number), or an option label for enum parameters."));
    changesProp.insert(QStringLiteral("items"), schema({
        {QStringLiteral("name"), prop(QStringLiteral("string"), QStringLiteral("Exact parameter name"))},
        {QStringLiteral("value"), QJsonObject{{QStringLiteral("type"), QJsonArray{QStringLiteral("number"), QStringLiteral("string")}}, {QStringLiteral("description"), QStringLiteral("New raw value")}}},
    }, {QStringLiteral("name"), QStringLiteral("value")}));

    QJsonObject appNamesProp = prop(QStringLiteral("array"), QStringLiteral("Setting names as \"<group>.<name>\", e.g. [\"video.videoSource\", \"video.rtspUrl\"]."));
    appNamesProp.insert(QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
    QJsonObject appChangesProp = prop(QStringLiteral("array"), QStringLiteral("Settings to change."));
    appChangesProp.insert(QStringLiteral("items"), schema({
        {QStringLiteral("name"), prop(QStringLiteral("string"), QStringLiteral("Setting name, e.g. \"video.rtspUrl\""))},
        {QStringLiteral("value"), QJsonObject{{QStringLiteral("type"), QJsonArray{QStringLiteral("number"), QStringLiteral("string"), QStringLiteral("boolean")}}, {QStringLiteral("description"), QStringLiteral("New value, or an option label")}}},
    }, {QStringLiteral("name"), QStringLiteral("value")}));

    return QJsonArray{
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("list_vehicles")},
            {QStringLiteral("description"), QStringLiteral("List connected vehicles with firmware, type, armed state, flight mode and whether parameters are loaded.")},
            {QStringLiteral("inputSchema"), schema({})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("search_parameters")},
            {QStringLiteral("description"), QStringLiteral("Search the vehicle's parameters by name or description (case-insensitive substring; several words must all match). Returns current values and short descriptions.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("query"), prop(QStringLiteral("string"), QStringLiteral("Words to look for, e.g. \"takeoff altitude\" or \"MPC_\""))},
                {QStringLiteral("limit"), prop(QStringLiteral("integer"), QStringLiteral("Maximum results (default 40)"))},
                {QStringLiteral("vehicle_id"), vehicleProp},
                {QStringLiteral("component_id"), componentProp},
            }, {QStringLiteral("query")})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("get_parameters")},
            {QStringLiteral("description"), QStringLiteral("Get parameters with full metadata: value, units, min/max, default, options, bitmask bits, reboot requirement.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("names"), namesProp},
                {QStringLiteral("vehicle_id"), vehicleProp},
                {QStringLiteral("component_id"), componentProp},
            }, {QStringLiteral("names")})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("set_parameters")},
            {QStringLiteral("description"), QStringLiteral("Change one or more parameters on a vehicle. The user sees an approval card in QGC listing old and new values and must approve before anything is sent; the call waits for that decision (up to 10 minutes). Batch related changes into one call. Values are validated against min/max/type first.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("changes"), changesProp},
                {QStringLiteral("reason"), prop(QStringLiteral("string"), QStringLiteral("One sentence shown to the user explaining why"))},
                {QStringLiteral("vehicle_id"), vehicleProp},
                {QStringLiteral("component_id"), componentProp},
            }, {QStringLiteral("changes")})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("destructiveHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("search_app_settings")},
            {QStringLiteral("description"), QStringLiteral("Search QGroundControl's own application settings (video stream, units, maps, fly view, plan view, auto-connect, MAVLink, RTK, ADS-B, joystick, logging...). Settings are named \"<group>.<name>\", e.g. \"video.rtspUrl\". Several words must all match name or description. An empty query lists the groups.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("query"), prop(QStringLiteral("string"), QStringLiteral("Words to look for, e.g. \"rtsp\" or \"video source\""))},
                {QStringLiteral("limit"), prop(QStringLiteral("integer"), QStringLiteral("Maximum results (default 40)"))},
            })},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("get_app_settings")},
            {QStringLiteral("description"), QStringLiteral("Get QGroundControl app settings with full metadata (value, units, min/max, default, options).")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("names"), appNamesProp},
            }, {QStringLiteral("names")})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("set_app_settings")},
            {QStringLiteral("description"), QStringLiteral("Change QGroundControl app settings. The user sees an approval card in QGC and must approve; the call waits for that decision (up to 10 minutes). Applied immediately and saved. Enum settings accept the option label (e.g. \"RTSP Video Stream\").")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("changes"), appChangesProp},
                {QStringLiteral("reason"), prop(QStringLiteral("string"), QStringLiteral("One sentence shown to the user explaining why"))},
            }, {QStringLiteral("changes")})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("destructiveHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("list_comm_links")},
            {QStringLiteral("description"), QStringLiteral("List QGC Comm Links (Application Settings > Comm Links) with type, connected state and settings, plus available link types and serial ports.")},
            {QStringLiteral("inputSchema"), schema({})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("set_comm_link")},
            {QStringLiteral("description"), QStringLiteral("Create a Comm Link (give type) or edit an existing one by name, optionally connecting it. Needs the user's approval. settings keys: udp: localPort, hostList [\"ip:port\", ...] (replaces targets); tcp: host, port; serial: portName, baud, dataBits, parity, stopBits, flowControl; any: autoConnect, highLatency.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("name"), prop(QStringLiteral("string"), QStringLiteral("Link name"))},
                {QStringLiteral("type"), prop(QStringLiteral("string"), QStringLiteral("udp, tcp, serial or bluetooth (only when creating)"))},
                {QStringLiteral("settings"), prop(QStringLiteral("object"), QStringLiteral("Settings to set"))},
                {QStringLiteral("connect"), prop(QStringLiteral("boolean"), QStringLiteral("Connect after saving (default false)"))},
                {QStringLiteral("reason"), prop(QStringLiteral("string"), QStringLiteral("One sentence shown to the user"))},
            }, {QStringLiteral("name")})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("destructiveHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("comm_link_action")},
            {QStringLiteral("description"), QStringLiteral("Connect, disconnect or delete a Comm Link by name. Needs the user's approval.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("name"), prop(QStringLiteral("string"), QStringLiteral("Link name"))},
                {QStringLiteral("action"), prop(QStringLiteral("string"), QStringLiteral("connect, disconnect or delete"))},
                {QStringLiteral("reason"), prop(QStringLiteral("string"), QStringLiteral("One sentence shown to the user"))},
            }, {QStringLiteral("name"), QStringLiteral("action")})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("destructiveHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("rosbag_status")},
            {QStringLiteral("description"), QStringLiteral("Check ROS bag recording on the TAR vehicles' companion computers (UAV = drone Jetson, UGV = rover Jetson, over ssh): whether each is recording, bag path, size and duration, the last bag, free disk space, the recording settings, and which network/addresses are in use.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("vehicle"), prop(QStringLiteral("string"), QStringLiteral("uav, ugv or both (default both)"))},
            })},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("rosbag_start")},
            {QStringLiteral("description"), QStringLiteral("Start ros2 bag record on the UAV, the UGV or both, with each vehicle's settings from the Bag toolbar indicator (all topics minus its exclude regex). A vehicle that is already recording is left alone. Needs the user's approval; the call waits for the decision and for the recorders to start, and returns the bag paths.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("vehicle"), prop(QStringLiteral("string"), QStringLiteral("uav, ugv or both"))},
                {QStringLiteral("reason"), prop(QStringLiteral("string"), QStringLiteral("One sentence shown to the user"))},
            }, {QStringLiteral("vehicle")})},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("rosbag_stop")},
            {QStringLiteral("description"), QStringLiteral("Stop ROS bag recording on the UAV, the UGV or both (SIGINT, so each bag is closed cleanly with metadata.yaml). A vehicle that is not recording is left alone. Needs the user's approval; returns the saved bag paths and sizes.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("vehicle"), prop(QStringLiteral("string"), QStringLiteral("uav, ugv or both"))},
                {QStringLiteral("reason"), prop(QStringLiteral("string"), QStringLiteral("One sentence shown to the user"))},
            }, {QStringLiteral("vehicle")})},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("qgc_inspect")},
            {QStringLiteral("description"), QStringLiteral("Browse QGC's live object tree: properties (with values), writable properties, callable methods, list items and facts of any object. Empty path lists the roots (vehicle, multiVehicleManager, linkManager, settingsManager, videoManager, joystickManager, adsbVehicleManager, positionManager, corePlugin, QGroundControl, window). Paths use dots and [index], e.g. \"vehicle\", \"vehicle.gps\", \"multiVehicleManager.vehicles[1]\", \"vehicle1.missionManager\", \"$3\". Use filter to narrow large objects.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("path"), prop(QStringLiteral("string"), QStringLiteral("Object path; empty for roots"))},
                {QStringLiteral("filter"), prop(QStringLiteral("string"), QStringLiteral("Only show properties/methods whose name contains this"))},
            })},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("qgc_set_property")},
            {QStringLiteral("description"), QStringLiteral("Set a writable property on any QGC object (see qgc_inspect). For a Fact set rawValue. Coordinates are {lat, lon, alt}; enums take the key name or number; object arguments take a path. Needs the user's approval.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("path"), prop(QStringLiteral("string"), QStringLiteral("Object path"))},
                {QStringLiteral("property"), prop(QStringLiteral("string"), QStringLiteral("Property name"))},
                {QStringLiteral("value"), QJsonObject{{QStringLiteral("description"), QStringLiteral("New value")}}},
                {QStringLiteral("reason"), prop(QStringLiteral("string"), QStringLiteral("One sentence shown to the user"))},
            }, {QStringLiteral("path"), QStringLiteral("property"), QStringLiteral("value")})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("destructiveHint"), true}}},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("qgc_call")},
            {QStringLiteral("description"), QStringLiteral("Call a method on any QGC object (see qgc_inspect for signatures), e.g. vehicle.guidedModeRTL(false), vehicle.rebootVehicle(), vehicle.sendPlan(\"C:/path/x.plan\"), vehicle.setFlightMode? (set the flightMode property instead). Getter-style methods (get*, is*, has*, find*...) run immediately; anything else shows an approval card first. Returned objects become handles $N usable as paths.")},
            {QStringLiteral("inputSchema"), schema({
                {QStringLiteral("path"), prop(QStringLiteral("string"), QStringLiteral("Object path"))},
                {QStringLiteral("method"), prop(QStringLiteral("string"), QStringLiteral("Method name"))},
                {QStringLiteral("args"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("description"), QStringLiteral("Arguments in order")}}},
                {QStringLiteral("reason"), prop(QStringLiteral("string"), QStringLiteral("One sentence shown to the user"))},
            }, {QStringLiteral("path"), QStringLiteral("method")})},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("destructiveHint"), true}}},
        },
    };
}

void TarMcpServer::_callTool(QTcpSocket *socket, const QJsonValue &id, const QString &name, const QJsonObject &args)
{
    bool isError = false;
    if (name == QLatin1String("list_vehicles")) {
        _sendToolResult(socket, id, _toolListVehicles());
    } else if (name == QLatin1String("search_parameters")) {
        const QString text = _toolSearchParameters(args, isError);
        _sendToolResult(socket, id, text, isError);
    } else if (name == QLatin1String("get_parameters")) {
        const QString text = _toolGetParameters(args, isError);
        _sendToolResult(socket, id, text, isError);
    } else if (name == QLatin1String("set_parameters")) {
        _toolSetParameters(socket, id, args);
    } else if (name == QLatin1String("search_app_settings")) {
        _sendToolResult(socket, id, _toolSearchAppSettings(args));
    } else if (name == QLatin1String("get_app_settings")) {
        _sendToolResult(socket, id, _toolGetAppSettings(args));
    } else if (name == QLatin1String("set_app_settings")) {
        _toolSetAppSettings(socket, id, args);
    } else if (name == QLatin1String("list_comm_links")) {
        _sendToolResult(socket, id, _toolListCommLinks());
    } else if (name == QLatin1String("set_comm_link")) {
        _toolSetCommLink(socket, id, args);
    } else if (name == QLatin1String("comm_link_action")) {
        _toolCommLinkAction(socket, id, args);
    } else if (name == QLatin1String("rosbag_status")) {
        _toolRosbagStatus(socket, id, args);
    } else if (name == QLatin1String("rosbag_start")) {
        _toolRosbagStartStop(socket, id, args, true);
    } else if (name == QLatin1String("rosbag_stop")) {
        _toolRosbagStartStop(socket, id, args, false);
    } else if (name == QLatin1String("qgc_inspect")) {
        const QString text = _toolInspect(args, isError);
        _sendToolResult(socket, id, text, isError);
    } else if (name == QLatin1String("qgc_set_property")) {
        _toolSetProperty(socket, id, args);
    } else if (name == QLatin1String("qgc_call")) {
        _toolCall(socket, id, args);
    } else {
        _sendToolResult(socket, id, QStringLiteral("Unknown tool %1").arg(name), true);
    }
}

Vehicle *TarMcpServer::_vehicle(const QJsonObject &args, QString &error)
{
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    Vehicle *vehicle = nullptr;
    if (args.contains(QStringLiteral("vehicle_id"))) {
        const int id = args.value(QStringLiteral("vehicle_id")).toInt();
        vehicle = mvm ? mvm->getVehicleById(id) : nullptr;
        if (!vehicle) {
            error = QStringLiteral("No connected vehicle with system id %1.").arg(id);
            return nullptr;
        }
    } else {
        vehicle = mvm ? mvm->activeVehicle() : nullptr;
        if (!vehicle) {
            error = QStringLiteral("No vehicle is connected.");
            return nullptr;
        }
    }
    if (!vehicle->parameterManager() || !vehicle->parameterManager()->parametersReady()) {
        error = QStringLiteral("Parameters for vehicle %1 are not loaded yet.").arg(vehicle->id());
        return nullptr;
    }
    return vehicle;
}

QString TarMcpServer::_toolListVehicles()
{
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    const QmlObjectListModel *vehicles = mvm ? mvm->vehicles() : nullptr;
    QJsonArray out;
    for (int i = 0; vehicles && i < vehicles->count(); i++) {
        Vehicle *v = vehicles->value<Vehicle *>(i);
        if (!v) {
            continue;
        }
        out.append(QJsonObject{
            {QStringLiteral("vehicle_id"), v->id()},
            {QStringLiteral("active"), v == mvm->activeVehicle()},
            {QStringLiteral("firmware"), v->property("firmwareTypeString").toString()},
            {QStringLiteral("type"), v->property("vehicleTypeString").toString()},
            {QStringLiteral("armed"), v->armed()},
            {QStringLiteral("flight_mode"), v->property("flightMode").toString()},
            {QStringLiteral("parameters_loaded"), v->parameterManager() && v->parameterManager()->parametersReady()},
        });
    }
    return out.isEmpty() ? QStringLiteral("No vehicles connected.") : toJsonText(out);
}

QString TarMcpServer::_toolSearchParameters(const QJsonObject &args, bool &isError)
{
    QString error;
    Vehicle *vehicle = _vehicle(args, error);
    if (!vehicle) {
        isError = true;
        return error;
    }
    const int comp = args.value(QStringLiteral("component_id")).toInt(ParameterManager::defaultComponentId);
    const QStringList words = args.value(QStringLiteral("query")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const int limit = qBound(1, args.value(QStringLiteral("limit")).toInt(40), 200);
    ParameterManager *pm = vehicle->parameterManager();

    QJsonArray results;
    int matches = 0;
    for (const QString &name : pm->parameterNames(comp)) {
        Fact *fact = pm->getParameter(comp, name);
        if (!fact) {
            continue;
        }
        const QString haystack = name + QLatin1Char(' ') + fact->shortDescription() + QLatin1Char(' ') + fact->group();
        bool all = true;
        for (const QString &w : words) {
            if (!haystack.contains(w, Qt::CaseInsensitive)) {
                all = false;
                break;
            }
        }
        if (all) {
            if (++matches <= limit) {
                results.append(describeFact(fact, false));
            }
        }
    }
    QJsonObject out{{QStringLiteral("vehicle_id"), vehicle->id()}, {QStringLiteral("matches"), matches}, {QStringLiteral("parameters"), results}};
    if (matches > limit) {
        out.insert(QStringLiteral("note"), QStringLiteral("Only the first %1 matches are shown; refine the query.").arg(limit));
    }
    return toJsonText(out);
}

QString TarMcpServer::_toolGetParameters(const QJsonObject &args, bool &isError)
{
    QString error;
    Vehicle *vehicle = _vehicle(args, error);
    if (!vehicle) {
        isError = true;
        return error;
    }
    const int comp = args.value(QStringLiteral("component_id")).toInt(ParameterManager::defaultComponentId);
    ParameterManager *pm = vehicle->parameterManager();
    QJsonArray results;
    QJsonArray missing;
    for (const QJsonValue &v : args.value(QStringLiteral("names")).toArray()) {
        const QString name = v.toString().trimmed();
        if (pm->parameterExists(comp, name)) {
            results.append(describeFact(pm->getParameter(comp, name), true));
        } else {
            missing.append(name);
        }
    }
    QJsonObject out{{QStringLiteral("vehicle_id"), vehicle->id()}, {QStringLiteral("parameters"), results}};
    if (!missing.isEmpty()) {
        out.insert(QStringLiteral("not_found"), missing);
    }
    return toJsonText(out);
}

void TarMcpServer::_toolSetParameters(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args)
{
    QString error;
    Vehicle *vehicle = _vehicle(args, error);
    if (!vehicle) {
        _sendToolResult(socket, id, error, true);
        return;
    }
    const int comp = args.value(QStringLiteral("component_id")).toInt(ParameterManager::defaultComponentId);
    ParameterManager *pm = vehicle->parameterManager();
    const QJsonArray changes = args.value(QStringLiteral("changes")).toArray();
    if (changes.isEmpty() || changes.size() > kMaxChangesPerCall) {
        _sendToolResult(socket, id, QStringLiteral("Provide between 1 and %1 changes.").arg(kMaxChangesPerCall), true);
        return;
    }

    PendingApproval pending;
    pending.socket = socket;
    pending.rpcId = id;
    pending.vehicleId = vehicle->id();
    pending.componentId = comp;
    QStringList problems;
    QStringList unchanged;

    for (const QJsonValue &cv : changes) {
        const QJsonObject c = cv.toObject();
        const QString name = c.value(QStringLiteral("name")).toString().trimmed();
        if (!pm->parameterExists(comp, name)) {
            problems << QStringLiteral("%1: no such parameter").arg(name);
            continue;
        }
        Fact *fact = pm->getParameter(comp, name);
        QVariant typed;
        QString problem;
        if (!_validateChange(fact, name, c.value(QStringLiteral("value")).toVariant(), typed, problem)) {
            problems << problem;
            continue;
        }
        if (typed == fact->rawValue()) {
            unchanged << name;
            continue;
        }
        pending.changes.append({name, fact->rawValue(), typed, fact->rawUnits(), fact->vehicleRebootRequired(), typed});
    }

    if (!problems.isEmpty()) {
        _sendToolResult(socket, id, QStringLiteral("Nothing was changed. Fix these and call again:\n") + problems.join(QLatin1Char('\n')), true);
        return;
    }
    if (pending.changes.isEmpty()) {
        _sendToolResult(socket, id, QStringLiteral("Nothing to change: %1 already have the requested values.").arg(unchanged.join(QStringLiteral(", "))));
        return;
    }

    QStringList warnings;
    if (vehicle->armed()) {
        warnings << QStringLiteral("Vehicle is ARMED.");
    }
    const QString title = QStringLiteral("Change %1 parameter%2 on vehicle %3")
                              .arg(pending.changes.size()).arg(pending.changes.size() == 1 ? QString() : QStringLiteral("s")).arg(vehicle->id());
    _requestApproval(pending, title, args.value(QStringLiteral("reason")).toString().trimmed(), warnings);
}

bool TarMcpServer::_validateChange(Fact *fact, const QString &name, QVariant requested, QVariant &typed, QString &problem)
{
    if (fact->readOnly()) {
        problem = QStringLiteral("%1: read-only").arg(name);
        return false;
    }
    // Enum values may be given by option label
    if (requested.typeId() == QMetaType::QString && !fact->enumStrings().isEmpty()) {
        bool numeric = false;
        (void) requested.toString().toDouble(&numeric);
        if (!numeric) {
            const int idx = static_cast<int>(fact->enumStrings().indexOf(requested.toString()));
            if (idx < 0 || idx >= fact->enumValues().size()) {
                problem = QStringLiteral("%1: '%2' is not a valid option (options: %3)")
                              .arg(name, requested.toString(), fact->enumStrings().join(QStringLiteral(", ")));
                return false;
            }
            requested = fact->enumValues().at(idx);
        }
    }
    QString convertError;
    if (!fact->metaData() || !fact->metaData()->convertAndValidateRaw(requested, false, typed, convertError)) {
        problem = QStringLiteral("%1: %2").arg(name, convertError.isEmpty() ? QStringLiteral("invalid value") : convertError);
        return false;
    }
    return true;
}

void TarMcpServer::_requestApproval(const PendingApproval &pending, const QString &title, const QString &reason, QStringList warnings)
{
    QStringList lines;
    lines << title;
    if (!reason.isEmpty()) {
        lines << reason;
    }
    lines << pending.lines;
    bool reboot = false;
    for (const ParamChange &ch : pending.changes) {
        const QString oldText = variantText(ch.oldValue);
        lines << QStringLiteral("%1:  %2 → %3%4").arg(ch.name, oldText.isEmpty() ? QStringLiteral("(empty)") : oldText, variantText(ch.newValue),
                                                          ch.units.isEmpty() ? QString() : QLatin1Char(' ') + ch.units);
        reboot |= ch.rebootRequired;
    }
    if (reboot) {
        warnings << QStringLiteral("A reboot is required for some of these to take effect.");
    }

    const int approvalId = _nextApprovalId++;
    _pending.insert(approvalId, pending);
    QTimer::singleShot(kApprovalTimeoutMs, this, [this, approvalId]() {
        if (_pending.contains(approvalId)) {
            const PendingApproval p = _pending.take(approvalId);
            _sendToolResult(p.socket, p.rpcId, QStringLiteral("The user did not respond within 10 minutes. Nothing was changed."), true);
            emit approvalFinished(approvalId, QStringLiteral("expired"));
        }
    });
    emit approvalRequested(approvalId, lines.join(QLatin1Char('\n')), warnings.join(QLatin1Char(' ')));
}

// ---------------------------------------------------------------------------------------------------------
// ROS bag recording on the companion computer
// ---------------------------------------------------------------------------------------------------------

QList<TarRosbagRecorder *> TarMcpServer::_rosbagTargets(const QJsonObject &args, bool defaultAll, QString &error) const
{
    QList<TarRosbagRecorder *> out;
    if (!_rosbag) {
        error = QStringLiteral("ROS bag recording is not available in this build.");
        return out;
    }
    const QString v = args.value(QStringLiteral("vehicle")).toString().trimmed().toLower();
    if (v == QLatin1String("both") || (v.isEmpty() && defaultAll)) {
        return _rosbag->recorders();
    }
    if (TarRosbagRecorder *r = _rosbag->recorder(v)) {
        out << r;
    } else {
        error = QStringLiteral("vehicle must be uav, ugv or both.");
    }
    return out;
}

void TarMcpServer::_toolRosbagStatus(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args)
{
    QString error;
    const QList<TarRosbagRecorder *> targets = _rosbagTargets(args, true, error);
    if (targets.isEmpty()) {
        _sendToolResult(socket, id, error, true);
        return;
    }
    // Ask every vehicle, answer once all have replied.
    QPointer<TarMcpServer> self(this);
    const QPointer<QTcpSocket> guard(socket);
    auto results = std::make_shared<QStringList>();
    auto remaining = std::make_shared<int>(static_cast<int>(targets.size()));
    const QString network = _rosbag->network() ? QStringLiteral("Ground station network: %1.").arg(static_cast<TarFieldNetwork *>(_rosbag->network())->modeText()) : QString();
    for (TarRosbagRecorder *r : targets) {
        r->run(TarRosbagRecorder::Command::Status, [self, guard, id, results, remaining, network](bool, const QString &message) {
            *results << message;
            if (--(*remaining) == 0 && self) {
                self->_sendToolResult(guard, id, (network.isEmpty() ? QString() : network + QLatin1Char('\n')) + results->join(QLatin1Char('\n')));
            }
        });
    }
}

void TarMcpServer::_toolRosbagStartStop(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args, bool start)
{
    QString error;
    const QList<TarRosbagRecorder *> targets = _rosbagTargets(args, false, error);
    if (targets.isEmpty()) {
        _sendToolResult(socket, id, error, true);
        return;
    }
    // Leave alone a vehicle that is already in the requested state.
    QList<QPointer<TarRosbagRecorder>> todo;
    QStringList unchanged;
    for (TarRosbagRecorder *r : targets) {
        if (r->known() && r->recording() == start) {
            unchanged << QStringLiteral("%1: %2").arg(r->label(), start ? QStringLiteral("already recording") : QStringLiteral("not recording"));
        } else {
            todo << r;
        }
    }
    if (todo.isEmpty()) {
        _sendToolResult(socket, id, QStringLiteral("Nothing to do. ") + unchanged.join(QStringLiteral("; ")) + QLatin1Char('\n') + _rosbag->statusText());
        return;
    }

    PendingApproval pending;
    pending.socket = socket;
    pending.rpcId = id;
    QStringList warnings;
    QStringList labels;
    for (const QPointer<TarRosbagRecorder> &r : todo) {
        labels << r->label();
        if (start) {
            pending.lines << QStringLiteral("%1: ros2 bag record on %2, folder %3, ROS_DOMAIN_ID %4").arg(r->label(), r->host(), r->bagFolder(), r->domainId())
                          << QStringLiteral("%1: excluding %2").arg(r->label(), r->excludeRegex().isEmpty() ? QStringLiteral("nothing (all topics)") : r->excludeRegex());
        } else {
            pending.lines << QStringLiteral("%1: stop ros2 bag record on %2%3").arg(r->label(), r->host(),
                                                                                    r->bagPath().isEmpty() ? QString() : QStringLiteral(" (%1)").arg(r->bagPath()));
        }
        if (!r->known()) {
            warnings << QStringLiteral("%1 has not answered yet; this may fail.").arg(r->label());
        }
    }
    for (const QString &u : unchanged) {
        pending.lines << QStringLiteral("(%1, left alone)").arg(u);
    }
    pending.applyAsync = [todo, start](std::function<void(bool, const QString &)> done) {
        auto results = std::make_shared<QStringList>();
        auto remaining = std::make_shared<int>(static_cast<int>(todo.size()));
        auto allOk = std::make_shared<bool>(true);
        for (const QPointer<TarRosbagRecorder> &r : todo) {
            auto finish = [results, remaining, allOk, done](bool ok, const QString &message) {
                *results << message;
                *allOk = *allOk && ok;
                if (--(*remaining) == 0) {
                    done(*allOk, results->join(QLatin1Char('\n')));
                }
            };
            if (!r) {
                finish(false, QStringLiteral("Recorder is gone."));
                continue;
            }
            r->run(start ? TarRosbagRecorder::Command::Start : TarRosbagRecorder::Command::Stop, finish);
        }
    };
    _requestApproval(pending, QStringLiteral("%1 ROS bag recording: %2").arg(start ? QStringLiteral("Start") : QStringLiteral("Stop"), labels.join(QStringLiteral(" + "))),
                     args.value(QStringLiteral("reason")).toString().trimmed(), warnings);
}

// ---------------------------------------------------------------------------------------------------------
// App settings
// ---------------------------------------------------------------------------------------------------------

QList<QPair<QString, Fact *>> TarMcpServer::_appSettings()
{
    QList<QPair<QString, Fact *>> out;
    SettingsManager *sm = SettingsManager::instance();
    if (!sm) {
        return out;
    }
    const QMetaObject *smMeta = sm->metaObject();
    for (int i = smMeta->propertyOffset(); i < smMeta->propertyCount(); i++) {
        const QMetaProperty groupProp = smMeta->property(i);
        QObject *group = groupProp.read(sm).value<QObject *>();
        if (!group || !group->property("userVisible").toBool()) {
            continue;
        }
        QString groupName = QString::fromLatin1(groupProp.name());
        if (groupName.endsWith(QLatin1String("Settings"))) {
            groupName.chop(8);
        }
        const QMetaObject *gMeta = group->metaObject();
        for (int j = 0; j < gMeta->propertyCount(); j++) {
            Fact *fact = qobject_cast<Fact *>(gMeta->property(j).read(group).value<QObject *>());
            if (!fact) {
                continue;
            }
            const QVariant visible = fact->property("userVisible");
            if (visible.isValid() && !visible.toBool()) {
                continue; // hidden by this build; its value is forced to the default
            }
            out.append({groupName + QLatin1Char('.') + QString::fromLatin1(gMeta->property(j).name()), fact});
        }
    }
    return out;
}

Fact *TarMcpServer::_appSetting(const QString &key)
{
    for (const auto &entry : _appSettings()) {
        if (entry.first.compare(key, Qt::CaseInsensitive) == 0) {
            return entry.second;
        }
    }
    return nullptr;
}

QString TarMcpServer::_toolSearchAppSettings(const QJsonObject &args)
{
    const QStringList words = args.value(QStringLiteral("query")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const int limit = qBound(1, args.value(QStringLiteral("limit")).toInt(40), 300);
    const auto all = _appSettings();
    if (words.isEmpty()) {
        QMap<QString, int> groups;
        for (const auto &entry : all) {
            groups[entry.first.section(QLatin1Char('.'), 0, 0)]++;
        }
        QJsonObject out;
        for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
            out.insert(it.key(), it.value());
        }
        return toJsonText(QJsonObject{{QStringLiteral("groups (settings count)"), out}});
    }
    QJsonArray results;
    int matches = 0;
    for (const auto &entry : all) {
        const QString haystack = entry.first + QLatin1Char(' ') + entry.second->shortDescription() + QLatin1Char(' ') + entry.second->longDescription();
        bool ok = true;
        for (const QString &w : words) {
            if (!haystack.contains(w, Qt::CaseInsensitive)) {
                ok = false;
                break;
            }
        }
        if (ok && ++matches <= limit) {
            QJsonObject o = describeFact(entry.second, false);
            o.insert(QStringLiteral("name"), entry.first);
            results.append(o);
        }
    }
    QJsonObject out{{QStringLiteral("matches"), matches}, {QStringLiteral("settings"), results}};
    if (matches > limit) {
        out.insert(QStringLiteral("note"), QStringLiteral("Only the first %1 matches are shown; refine the query.").arg(limit));
    }
    return toJsonText(out);
}

QString TarMcpServer::_toolGetAppSettings(const QJsonObject &args)
{
    QJsonArray results;
    QJsonArray missing;
    for (const QJsonValue &v : args.value(QStringLiteral("names")).toArray()) {
        const QString key = v.toString().trimmed();
        if (Fact *fact = _appSetting(key)) {
            QJsonObject o = describeFact(fact, true);
            o.insert(QStringLiteral("name"), key);
            o.remove(QStringLiteral("reboot_required"));
            results.append(o);
        } else {
            missing.append(key);
        }
    }
    QJsonObject out{{QStringLiteral("settings"), results}};
    if (!missing.isEmpty()) {
        out.insert(QStringLiteral("not_found"), missing);
    }
    return toJsonText(out);
}

void TarMcpServer::_toolSetAppSettings(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args)
{
    const QJsonArray changes = args.value(QStringLiteral("changes")).toArray();
    if (changes.isEmpty() || changes.size() > kMaxChangesPerCall) {
        _sendToolResult(socket, id, QStringLiteral("Provide between 1 and %1 changes.").arg(kMaxChangesPerCall), true);
        return;
    }
    PendingApproval pending;
    pending.socket = socket;
    pending.rpcId = id;
    pending.appSettings = true;
    QStringList problems;
    QStringList unchanged;
    for (const QJsonValue &cv : changes) {
        const QJsonObject c = cv.toObject();
        const QString name = c.value(QStringLiteral("name")).toString().trimmed();
        Fact *fact = _appSetting(name);
        if (!fact) {
            problems << QStringLiteral("%1: no such setting (use search_app_settings)").arg(name);
            continue;
        }
        QVariant typed;
        QString problem;
        if (!_validateChange(fact, name, c.value(QStringLiteral("value")).toVariant(), typed, problem)) {
            problems << problem;
            continue;
        }
        if (typed == fact->rawValue()) {
            unchanged << name;
            continue;
        }
        // Show option labels on the card for enum settings
        auto label = [fact](const QVariant &v) -> QVariant {
            const int idx = static_cast<int>(fact->enumValues().indexOf(v));
            return (idx >= 0 && idx < fact->enumStrings().size()) ? QVariant(fact->enumStrings().at(idx)) : v;
        };
        pending.changes.append({name, label(fact->rawValue()), label(typed), fact->rawUnits(), false, typed});
    }
    if (!problems.isEmpty()) {
        _sendToolResult(socket, id, QStringLiteral("Nothing was changed. Fix these and call again:\n") + problems.join(QLatin1Char('\n')), true);
        return;
    }
    if (pending.changes.isEmpty()) {
        _sendToolResult(socket, id, QStringLiteral("Nothing to change: %1 already have the requested values.").arg(unchanged.join(QStringLiteral(", "))));
        return;
    }
    const QString title = QStringLiteral("Change %1 QGC setting%2").arg(pending.changes.size()).arg(pending.changes.size() == 1 ? QString() : QStringLiteral("s"));
    _requestApproval(pending, title, args.value(QStringLiteral("reason")).toString().trimmed(), {});
}

void TarMcpServer::resolveApproval(int approvalId, bool approved)
{
    if (!_pending.contains(approvalId)) {
        return;
    }
    if (!approved) {
        const PendingApproval p = _pending.take(approvalId);
        _sendToolResult(p.socket, p.rpcId, QStringLiteral("The user rejected these changes. Nothing was changed. Ask what they want instead of retrying."), true);
        emit approvalFinished(approvalId, QStringLiteral("rejected"));
        return;
    }
    _applyApproval(approvalId);
}

void TarMcpServer::rejectAllPending(const QString &reason)
{
    const QList<int> ids = _pending.keys();
    for (int id : ids) {
        const PendingApproval p = _pending.take(id);
        _sendToolResult(p.socket, p.rpcId, reason, true);
        emit approvalFinished(id, QStringLiteral("cancelled"));
    }
}

void TarMcpServer::_applyApproval(int approvalId)
{
    const PendingApproval p = _pending.take(approvalId);
    if (p.applyAsync) {
        emit approvalFinished(approvalId, QStringLiteral("approved: running..."));
        QPointer<TarMcpServer> self(this);
        const QPointer<QTcpSocket> socket = p.socket;
        const QJsonValue rpcId = p.rpcId;
        p.applyAsync([self, socket, rpcId, approvalId](bool ok, const QString &text) {
            if (!self) {
                return;
            }
            self->_sendToolResult(socket, rpcId, text, !ok);
            emit self->approvalFinished(approvalId, ok ? QStringLiteral("done") : QStringLiteral("failed: ") + text.left(120));
        });
        return;
    }
    if (p.apply) {
        bool ok = false;
        const QString text = p.apply(ok);
        _sendToolResult(p.socket, p.rpcId, text, !ok);
        emit approvalFinished(approvalId, ok ? QStringLiteral("done") : QStringLiteral("failed: ") + text.left(120));
        return;
    }
    if (p.appSettings) {
        QStringList results;
        for (const ParamChange &ch : p.changes) {
            if (Fact *fact = _appSetting(ch.name)) {
                fact->setRawValue(ch.typedValue);
                results << QStringLiteral("%1 = %2").arg(ch.name, fact->rawValueString());
            }
        }
        _sendToolResult(p.socket, p.rpcId, QStringLiteral("Approved and applied (saved):\n") + results.join(QLatin1Char('\n')));
        emit approvalFinished(approvalId, QStringLiteral("done"));
        return;
    }
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    Vehicle *vehicle = mvm ? mvm->getVehicleById(p.vehicleId) : nullptr;
    if (!vehicle || !vehicle->parameterManager()) {
        _sendToolResult(p.socket, p.rpcId, QStringLiteral("Vehicle %1 disconnected before the change was applied.").arg(p.vehicleId), true);
        emit approvalFinished(approvalId, QStringLiteral("failed: vehicle disconnected"));
        return;
    }
    ParameterManager *pm = vehicle->parameterManager();
    for (const ParamChange &ch : p.changes) {
        if (Fact *fact = pm->getParameter(p.componentId, ch.name)) {
            fact->setRawValue(ch.newValue);
        }
    }
    emit approvalFinished(approvalId, QStringLiteral("approved: sending..."));

    // Wait for the vehicle to acknowledge the writes (ParameterManager retries on its own).
    QPointer<ParameterManager> pmGuard(pm);
    auto *timer = new QTimer(this);
    auto *elapsed = new QElapsedTimer();
    elapsed->start();
    timer->setInterval(200);
    (void) connect(timer, &QTimer::timeout, this, [this, timer, elapsed, pmGuard, p, approvalId]() {
        const bool done = !pmGuard || !pmGuard->pendingWrites();
        if (!done && elapsed->elapsed() < kWriteConfirmTimeoutMs) {
            return;
        }
        timer->stop();
        timer->deleteLater();
        delete elapsed;

        QStringList results;
        bool reboot = false;
        for (const ParamChange &ch : p.changes) {
            Fact *fact = pmGuard ? pmGuard->getParameter(p.componentId, ch.name) : nullptr;
            results << QStringLiteral("%1 = %2").arg(ch.name, fact ? fact->rawValueStringFullPrecision() : QStringLiteral("?"));
            reboot |= ch.rebootRequired;
        }
        QString text = done ? QStringLiteral("Approved and confirmed by the vehicle:\n") : QStringLiteral("Approved and sent, but the vehicle has not confirmed every write yet:\n");
        text += results.join(QLatin1Char('\n'));
        if (reboot) {
            text += QStringLiteral("\nSome changes need a vehicle reboot to take effect (the user can reboot from Vehicle Setup > Parameters > Tools).");
        }
        _sendToolResult(p.socket, p.rpcId, text, !done);
        emit approvalFinished(approvalId, done ? QStringLiteral("applied") : QStringLiteral("sent, not confirmed"));
    });
    timer->start();
}
