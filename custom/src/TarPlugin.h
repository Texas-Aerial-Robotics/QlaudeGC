#pragma once

#include <QtCore/QLoggingCategory>

#include "QGCCorePlugin.h"

class TarAssistant;
class TarFieldNetwork;
class TarRosbag;
class TarRouter;
class QQmlApplicationEngine;

Q_DECLARE_LOGGING_CATEGORY(TarPluginLog)

/// TAR custom build core plugin: stock QGC plus a built-in MAVLink router, a Claude chat sidebar,
/// hard-coded comm links to the TAR UAV and UGV (picked by the ground station's network) and
/// ROS bag recording on both vehicles' companion computers.
class TarPlugin : public QGCCorePlugin
{
    Q_OBJECT

public:
    explicit TarPlugin(QObject *parent = nullptr);

    static QGCCorePlugin *instance();

    void init() final;
    void cleanup() final;
    QQmlApplicationEngine *createQmlApplicationEngine(QObject *parent) final;
    void createRootWindow(QQmlApplicationEngine *qmlEngine) final;
    const QVariantList &toolBarIndicators() final;

    TarRouter *router() const { return _router; }

private:
    TarRouter *_router = nullptr;
    TarFieldNetwork *_network = nullptr;
    TarRosbag *_rosbag = nullptr;
    TarAssistant *_assistant = nullptr;
};
