#include "TarRosbag.h"

#include "TarFieldNetwork.h"

TarRosbag::TarRosbag(TarFieldNetwork *network, QObject *parent)
    : QObject(parent)
    , _network(network)
{
    for (const TarFieldVehicle &vehicle : TarFieldNetwork::vehicles()) {
        auto *r = new TarRosbagRecorder(vehicle, network, this);
        (void) connect(r, &TarRosbagRecorder::stateChanged, this, &TarRosbag::stateChanged);
        (void) connect(r, &TarRosbagRecorder::busyChanged, this, &TarRosbag::stateChanged);
        _recorders << r;
    }
}

QObject *TarRosbag::network() const
{
    return _network;
}

QVariantList TarRosbag::recordersList() const
{
    QVariantList list;
    for (TarRosbagRecorder *r : _recorders) {
        list << QVariant::fromValue(static_cast<QObject *>(r));
    }
    return list;
}

TarRosbagRecorder *TarRosbag::recorder(const QString &vehicleId) const
{
    for (TarRosbagRecorder *r : _recorders) {
        if (r->vehicleId().compare(vehicleId, Qt::CaseInsensitive) == 0) {
            return r;
        }
    }
    return nullptr;
}

void TarRosbag::startAll()
{
    for (TarRosbagRecorder *r : _recorders) {
        if (!r->recording()) {
            r->start();
        }
    }
}

void TarRosbag::stopAll()
{
    for (TarRosbagRecorder *r : _recorders) {
        if (r->recording()) {
            r->stop();
        }
    }
}

void TarRosbag::refreshAll()
{
    for (TarRosbagRecorder *r : _recorders) {
        r->refresh();
    }
}

bool TarRosbag::anyRecording() const
{
    for (const TarRosbagRecorder *r : _recorders) {
        if (r->recording()) {
            return true;
        }
    }
    return false;
}

bool TarRosbag::allRecording() const
{
    for (const TarRosbagRecorder *r : _recorders) {
        if (!r->recording()) {
            return false;
        }
    }
    return !_recorders.isEmpty();
}

bool TarRosbag::anyBusy() const
{
    for (const TarRosbagRecorder *r : _recorders) {
        if (r->busy()) {
            return true;
        }
    }
    return false;
}

QString TarRosbag::statusText() const
{
    QStringList lines;
    if (_network) {
        lines << tr("Ground station network: %1.").arg(_network->modeText());
    }
    for (const TarRosbagRecorder *r : _recorders) {
        lines << r->statusText();
    }
    return lines.join(QLatin1Char('\n'));
}
