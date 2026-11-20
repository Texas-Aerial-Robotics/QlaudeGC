#include "TarPlugin.h"

#include <QtCore/QApplicationStatic>
#include <QtCore/QTimer>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include "QGCLoggingCategory.h"
#include "TarAssistant.h"
#include "TarRosbag.h"
#include "TarRouter.h"

QGC_LOGGING_CATEGORY(TarPluginLog, "Custom.TarPlugin")

Q_APPLICATION_STATIC(TarPlugin, _tarPluginInstance);

TarPlugin::TarPlugin(QObject *parent)
    : QGCCorePlugin(parent)
    , _router(new TarRouter(this))
    , _rosbag(new TarRosbag(this))
    , _assistant(new TarAssistant(_router, this))
{
    _assistant->setRosbag(_rosbag);
    qCDebug(TarPluginLog) << this;
}

QGCCorePlugin *TarPlugin::instance()
{
    return _tarPluginInstance();
}

void TarPlugin::init()
{
    QGCCorePlugin::init();
    // Defer until the event loop runs so LinkManager has finished its own startup.
    QTimer::singleShot(0, _router, &TarRouter::init);
}

void TarPlugin::cleanup()
{
    _router->shutdown();
    QGCCorePlugin::cleanup();
}

QQmlApplicationEngine *TarPlugin::createQmlApplicationEngine(QObject *parent)
{
    QQmlApplicationEngine *engine = QGCCorePlugin::createQmlApplicationEngine(parent);
    engine->rootContext()->setContextProperty(QStringLiteral("tarRouter"), _router);
    engine->rootContext()->setContextProperty(QStringLiteral("tarAssistant"), _assistant);
    engine->rootContext()->setContextProperty(QStringLiteral("tarRosbag"), _rosbag);
    return engine;
}

void TarPlugin::createRootWindow(QQmlApplicationEngine *qmlEngine)
{
    QGCCorePlugin::createRootWindow(qmlEngine);
    _assistant->setQmlEngine(qmlEngine);

    // The chat sidebar is added to the main window's content item so it stays put while views and toolbar
    // indicators are recreated; the chat itself lives in TarAssistant.
    QQuickWindow *window = qobject_cast<QQuickWindow *>(qmlEngine->rootObjects().value(0));
    if (!window) {
        qCWarning(TarPluginLog) << "No root window; chat sidebar not created";
        return;
    }
    QQmlComponent component(qmlEngine, QUrl(QStringLiteral("qrc:/tar/qml/AssistantPanel.qml")));
    QObject *object = component.beginCreate(qmlEngine->rootContext());
    QQuickItem *panel = qobject_cast<QQuickItem *>(object);
    if (!panel) {
        qCWarning(TarPluginLog) << "Chat sidebar failed to load:" << component.errors();
        delete object;
        return;
    }
    panel->setParentItem(window->contentItem());
    panel->setParent(window->contentItem());
    component.completeCreate();
}

const QVariantList &TarPlugin::toolBarIndicators()
{
    static const QVariantList indicators = []() {
        QVariantList list = QGCCorePlugin::instance()->QGCCorePlugin::toolBarIndicators();
        list.prepend(QVariant::fromValue(QUrl::fromUserInput(QStringLiteral("qrc:/tar/qml/RouterIndicator.qml"))));
        list.prepend(QVariant::fromValue(QUrl::fromUserInput(QStringLiteral("qrc:/tar/qml/RosbagIndicator.qml"))));
        list.prepend(QVariant::fromValue(QUrl::fromUserInput(QStringLiteral("qrc:/tar/qml/AssistantIndicator.qml"))));
        return list;
    }();
    return indicators;
}
