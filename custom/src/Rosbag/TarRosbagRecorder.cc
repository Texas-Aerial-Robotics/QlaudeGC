#include "TarRosbagRecorder.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTime>

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(TarRosbagLog, "Custom.TarRosbag")

namespace {

constexpr const char *kSettingsGroup = "TarRosbag";
constexpr const char *kScriptResource = ":/tar/rosbag/rosbag_ctl.sh";

QString formatBytes(double bytes)
{
    if (bytes >= 1024.0 * 1024 * 1024) {
        return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
    }
    if (bytes >= 1024.0 * 1024) {
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    }
    if (bytes >= 1024.0) {
        return QStringLiteral("%1 kB").arg(bytes / 1024.0, 0, 'f', 0);
    }
    return QStringLiteral("%1 B").arg(bytes, 0, 'f', 0);
}

QString formatDuration(qint64 seconds)
{
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600)
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

TarRosbagRecorder::Defaults TarRosbagRecorder::_defaults(const QString &vehicleId)
{
    if (vehicleId == QLatin1String("ugv")) {
        // Rover: its own stack on domain 2 (ros_env.sh also sets the Fast DDS peer profile); tar_ws has its
        // message packages. FAST-LIO's registered clouds and map are large; /path grows without bound.
        return {QStringLiteral("2"), QStringLiteral("~/bags"), QStringLiteral("ugv"),
                QStringLiteral("^(/Laser_map|/cloud_registered|/cloud_registered_body|/cloud_effected|/path)$"),
                QStringLiteral("~/LIDARSLAM/autostart/ros_env.sh ~/tar_ws/install/setup.bash")};
    }
    // Drone: domain 1, workspace raytheon-2027-drone (px4_msgs, dice_msgs). The raw camera overruns the eMMC;
    // Point-LIO's /path and high-rate /aft_mapped_to_init are left out.
    return {QStringLiteral("1"), QStringLiteral("~/bags"), QStringLiteral("uav"),
            QStringLiteral("^(/camera/image_raw|/path|/aft_mapped_to_init)$"),
            QStringLiteral("~/raytheon-2027-drone/install/setup.bash")};
}

TarRosbagRecorder::TarRosbagRecorder(const TarFieldVehicle &vehicle, TarFieldNetwork *network, QObject *parent)
    : QObject(parent)
    , _vehicle(vehicle)
    , _network(network)
{
    const Defaults d = _defaults(_vehicle.id);
    QSettings settings;
    _sshKey = settings.value(_settingsKey("sshKey")).toString();
    _domainId = settings.value(_settingsKey("domainId"), d.domainId).toString();
    _bagFolder = settings.value(_settingsKey("bagFolder"), d.bagFolder).toString();
    _bagPrefix = settings.value(_settingsKey("bagPrefix"), d.bagPrefix).toString();
    _excludeRegex = settings.value(_settingsKey("excludeRegex"), d.excludeRegex).toString();
    _setupScript = settings.value(_settingsKey("setupScript"), d.setupScript).toString();
    _extraArgs = settings.value(_settingsKey("extraArgs")).toString();
    _downloadFolder = settings.value(_settingsKey("downloadFolder"), _defaultDownloadFolder()).toString();
    _lastHost = host();

    _downloadTimer.setInterval(1000);
    (void) connect(&_downloadTimer, &QTimer::timeout, this, &TarRosbagRecorder::_downloadProgress);

    _timeout.setSingleShot(true);
    _timeout.setInterval(kTimeoutMs);
    (void) connect(&_timeout, &QTimer::timeout, this, [this]() {
        if (_process) {
            _appendLog(tr("%1 timed out after %2 s").arg(_commandName(_current.command)).arg(kTimeoutMs / 1000));
            _process->kill();
        }
    });

    (void) connect(&_poll, &QTimer::timeout, this, &TarRosbagRecorder::refresh);
    if (_network) {
        (void) connect(_network, &TarFieldNetwork::changed, this, &TarRosbagRecorder::_networkChanged);
    }

    // Pick up a recording that is already running (e.g. QGC was restarted mid-flight).
    QTimer::singleShot(2000, this, &TarRosbagRecorder::refresh);
}

QString TarRosbagRecorder::_settingsKey(const char *key) const
{
    return QStringLiteral("%1/%2/%3").arg(QLatin1String(kSettingsGroup), _vehicle.id, QLatin1String(key));
}

void TarRosbagRecorder::_setSetting(QString &member, const char *key, const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed == member) {
        return;
    }
    member = trimmed;
    QSettings().setValue(_settingsKey(key), member);
    emit settingsChanged();
}

void TarRosbagRecorder::setSshKey(const QString &value) { _setSetting(_sshKey, "sshKey", value); }
void TarRosbagRecorder::setDomainId(const QString &value) { _setSetting(_domainId, "domainId", value); }
void TarRosbagRecorder::setBagFolder(const QString &value) { _setSetting(_bagFolder, "bagFolder", value); }
void TarRosbagRecorder::setBagPrefix(const QString &value) { _setSetting(_bagPrefix, "bagPrefix", value); }
void TarRosbagRecorder::setExcludeRegex(const QString &value) { _setSetting(_excludeRegex, "excludeRegex", value); }
void TarRosbagRecorder::setSetupScript(const QString &value) { _setSetting(_setupScript, "setupScript", value); }
void TarRosbagRecorder::setExtraArgs(const QString &value) { _setSetting(_extraArgs, "extraArgs", value); }
void TarRosbagRecorder::setDownloadFolder(const QString &value) { _setSetting(_downloadFolder, "downloadFolder", value); }

QString TarRosbagRecorder::_defaultDownloadFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/TAR bags");
}

void TarRosbagRecorder::downloadLastBag()
{
    if (_download) {
        return;
    }
    if (_recording || _bagPath.isEmpty() || !_known) {
        _downloadText = tr("No finished bag to download.");
        emit downloadChanged();
        return;
    }
    const QString name = QFileInfo(_bagPath).fileName();
    QDir dir(_downloadFolder.isEmpty() ? _defaultDownloadFolder() : _downloadFolder);
    if (!dir.mkpath(QStringLiteral("."))) {
        _downloadText = tr("Cannot create %1").arg(dir.absolutePath());
        emit downloadChanged();
        return;
    }
    _downloadTarget = dir.filePath(name);
    if (QFileInfo::exists(_downloadTarget)) {
        _downloadedPath = _downloadTarget;
        _downloadText = tr("Already downloaded: %1").arg(QDir::toNativeSeparators(_downloadTarget));
        emit downloadChanged();
        return;
    }
    _downloadTotal = _bagBytes;
    _downloadCancelled = false;
    _downloadedPath.clear();

    // Run inside the destination and copy to "." so a Windows drive letter is never mistaken for a host.
    const QString remote = host() + QLatin1Char(':');
    QStringList args{
        QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
        QStringLiteral("-o"), QStringLiteral("ConnectTimeout=8"),
        QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=accept-new"),
        QStringLiteral("-r"),
    };
    if (!_sshKey.isEmpty()) {
        args << QStringLiteral("-i") << _sshKey;
    }
    args << remote + _bagPath << remote + _bagPath + QStringLiteral(".log") << QStringLiteral(".");

    _download = new QProcess(this);
    _download->setProgram(_scpProgram());
    _download->setArguments(args);
    _download->setWorkingDirectory(dir.absolutePath());
    (void) connect(_download, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        _downloadFinished(exitCode, status == QProcess::CrashExit);
    });
    (void) connect(_download, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            _downloadFinished(-1, true);
        }
    });
    _download->start();
    _downloadText = tr("Downloading %1 (%2)...").arg(name, formatBytes(_downloadTotal));
    _appendLog(tr("Downloading %1 to %2").arg(_bagPath, QDir::toNativeSeparators(dir.absolutePath())));
    _downloadTimer.start();
    emit downloadChanged();
}

void TarRosbagRecorder::_downloadProgress()
{
    if (!_download) {
        return;
    }
    double bytes = 0;
    QDirIterator it(_downloadTarget, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        bytes += static_cast<double>(it.nextFileInfo().size());
    }
    _downloadText = (_downloadTotal > 0)
        ? tr("Downloading %1: %2 of %3 (%4%)").arg(QFileInfo(_downloadTarget).fileName(), formatBytes(bytes), formatBytes(_downloadTotal))
              .arg(qMin(100.0, 100.0 * bytes / _downloadTotal), 0, 'f', 0)
        : tr("Downloading %1: %2").arg(QFileInfo(_downloadTarget).fileName(), formatBytes(bytes));
    emit downloadChanged();
}

void TarRosbagRecorder::_downloadFinished(int exitCode, bool crashed)
{
    if (!_download) {
        return;
    }
    _downloadTimer.stop();
    QProcess *process = _download;
    _download = nullptr;
    const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
    const bool failedToStart = process->error() == QProcess::FailedToStart;
    process->deleteLater();

    // The bag folder is what matters; a missing .log alone still counts as a successful download.
    const bool gotBag = QFileInfo::exists(_downloadTarget + QStringLiteral("/metadata.yaml"));
    if (_downloadCancelled) {
        (void) QDir(_downloadTarget).removeRecursively();
        (void) QFile::remove(_downloadTarget + QStringLiteral(".log"));
        _downloadText = tr("Download cancelled.");
    } else if (gotBag) {
        _downloadedPath = _downloadTarget;
        double bytes = 0;
        QDirIterator it(_downloadTarget, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            bytes += static_cast<double>(it.nextFileInfo().size());
        }
        _downloadText = tr("Saved to %1 (%2)").arg(QDir::toNativeSeparators(_downloadTarget), formatBytes(bytes));
    } else {
        QString detail = failedToStart ? tr("could not run %1").arg(_scpProgram()) : err.split(QLatin1Char('\n')).constLast();
        if (detail.isEmpty()) {
            detail = crashed ? tr("interrupted") : tr("exit code %1").arg(exitCode);
        }
        (void) QDir(_downloadTarget).removeRecursively();
        _downloadText = tr("Download failed: %1").arg(detail);
    }
    _appendLog(_downloadText);
    emit downloadChanged();
}

void TarRosbagRecorder::cancelDownload()
{
    if (_download) {
        _downloadCancelled = true;
        _download->kill();
    }
}

void TarRosbagRecorder::openDownloadFolder() const
{
    const QString path = _downloadedPath.isEmpty() ? (_downloadFolder.isEmpty() ? _defaultDownloadFolder() : _downloadFolder) : _downloadedPath;
    (void) QDir().mkpath(path);
    (void) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void TarRosbagRecorder::restoreDefaults()
{
    const Defaults d = _defaults(_vehicle.id);
    setSshKey(QString());
    setDomainId(d.domainId);
    setBagFolder(d.bagFolder);
    setBagPrefix(d.bagPrefix);
    setExcludeRegex(d.excludeRegex);
    setSetupScript(d.setupScript);
    setExtraArgs(QString());
    setDownloadFolder(_defaultDownloadFolder());
}

void TarRosbagRecorder::_networkChanged()
{
    const QString h = host();
    if (h == _lastHost) {
        return;
    }
    _appendLog(tr("Network changed: now using %1").arg(h));
    _lastHost = h;
    // The recorder state belongs to the vehicle, not the path to it; only reachability must be re-checked.
    _reachable = false;
    _known = false;
    _lastError.clear();
    emit hostChanged();
    emit stateChanged();
    _updatePolling();
    refresh();
}

void TarRosbagRecorder::run(Command command, Callback done)
{
    if (host().isEmpty()) {
        if (done) {
            done(false, tr("%1: no address.").arg(_vehicle.label));
        }
        return;
    }
    // Status requests are idempotent: fold a new one into one that is already waiting.
    if (command == Command::Status) {
        for (Job &job : _queue) {
            if (job.command == Command::Status) {
                if (done) {
                    Callback previous = job.done;
                    job.done = [previous, done](bool ok, const QString &message) {
                        if (previous) {
                            previous(ok, message);
                        }
                        done(ok, message);
                    };
                }
                return;
            }
        }
        if (_process && _current.command == Command::Status && !done) {
            return;
        }
    }
    _queue.append(Job{command, std::move(done)});
    _startNext();
}

void TarRosbagRecorder::_startNext()
{
    if (_process || _queue.isEmpty()) {
        return;
    }
    _current = _queue.takeFirst();
    _currentHost = host();
    if (_current.command != Command::Status) {
        _appendLog(tr("%1 on %2...").arg(_commandName(_current.command), _currentHost));
    }

    _process = new QProcess(this);
    _process->setProgram(_sshProgram());
    _process->setArguments(_sshArguments(_currentHost));
    (void) connect(_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        _finished(exitCode, status == QProcess::CrashExit);
    });
    (void) connect(_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            _finished(-1, true);
        }
    });
    _process->start();
    _process->write(_script(_current.command).toUtf8());
    _process->closeWriteChannel();
    _timeout.start();
    emit busyChanged();
}

void TarRosbagRecorder::_finished(int exitCode, bool crashed)
{
    if (!_process) {
        return;
    }
    _timeout.stop();
    QProcess *process = _process;
    _process = nullptr;
    const QString out = QString::fromUtf8(process->readAllStandardOutput());
    const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
    const bool failedToStart = process->error() == QProcess::FailedToStart;
    process->deleteLater();

    const Job job = _current;
    const QString what = _commandName(job.command);
    bool ok = false;
    QString message;

    QVariantMap values;
    for (const QString &line : out.split(QLatin1Char('\n'))) {
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq > 0) {
            values.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
        }
    }

    if (failedToStart) {
        message = tr("Could not run ssh (%1). Install the Windows OpenSSH client.").arg(_sshProgram());
        _reachable = false;
        _known = false;
    } else if (!values.contains(QStringLiteral("RECORDING"))) {
        // ssh exits 255 on connection/auth failures; anything else means the script did not run.
        QString detail = err.split(QLatin1Char('\n')).constLast();
        if (detail.isEmpty()) {
            detail = crashed ? tr("timed out") : tr("exit code %1").arg(exitCode);
        }
        message = tr("Cannot reach %1: %2").arg(_currentHost, detail);
        _reachable = false;
        _known = false;
    } else {
        _reachable = true;
        _known = true;
        const bool wasRecording = _recording;
        _applyStatus(values);
        const QString error = values.value(QStringLiteral("ERROR")).toString();
        switch (job.command) {
        case Command::Start:
            ok = values.value(QStringLiteral("STARTED")).toString() == QLatin1String("1");
            message = ok ? tr("Recording to %1").arg(_bagPath) : error;
            break;
        case Command::Stop: {
            ok = values.value(QStringLiteral("STOPPED")).toString() == QLatin1String("1");
            const QString last = values.value(QStringLiteral("LAST")).toString();
            message = ok ? tr("Stopped. Saved %1 (%2)").arg(last, formatBytes(values.value(QStringLiteral("LASTBYTES")).toDouble()))
                         : error;
            const QString warning = values.value(QStringLiteral("WARNING")).toString();
            if (ok && !warning.isEmpty()) {
                message += QStringLiteral(". Warning: ") + warning;
            }
            break;
        }
        case Command::Status:
            ok = true;
            message = statusText();
            if (wasRecording && !_recording && !values.contains(QStringLiteral("DIED"))) {
                _appendLog(tr("Recording ended: %1").arg(values.value(QStringLiteral("LAST")).toString()));
            }
            break;
        }
    }

    if (job.command == Command::Status) {
        // Polls stay quiet in the log unless something changed.
        if (!ok && _lastError != message) {
            _appendLog(message);
        }
        if (!ok || _lastError.startsWith(tr("Cannot reach")) || _lastError.startsWith(tr("Could not run"))) {
            _lastError = ok ? QString() : message;
        }
    } else {
        _appendLog(ok ? message : tr("%1 failed: %2").arg(what, message));
        _lastError = ok ? QString() : message;
    }
    emit stateChanged();
    emit busyChanged();
    _updatePolling();

    if (job.done) {
        // statusText() already names the vehicle; start/stop results get the label prepended.
        job.done(ok, job.command == Command::Status ? message : QStringLiteral("%1: %2").arg(_vehicle.label, message));
    }
    _startNext();
}

void TarRosbagRecorder::_applyStatus(const QVariantMap &values)
{
    _recording = values.value(QStringLiteral("RECORDING")).toString() == QLatin1String("1");
    _freeBytes = values.value(QStringLiteral("FREE")).toDouble();
    if (_recording) {
        _bagPath = values.value(QStringLiteral("DIR")).toString();
        _bagBytes = values.value(QStringLiteral("BYTES")).toDouble();
        _startedAtMs = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) -
                       values.value(QStringLiteral("ELAPSED")).toDouble() * 1000.0;
    } else {
        _bagPath = values.value(QStringLiteral("LAST")).toString();
        _bagBytes = values.value(QStringLiteral("LASTBYTES")).toDouble();
        _startedAtMs = 0;
    }
    if (values.contains(QStringLiteral("DIED"))) {
        _lastError = tr("Recording stopped unexpectedly. %1").arg(values.value(QStringLiteral("DIED")).toString());
        _appendLog(_lastError);
    }
}

void TarRosbagRecorder::_updatePolling()
{
    // Fast while recording (size/elapsed, notice a crashed recorder); slow while idle and reachable;
    // no background ssh at all while the companion is unreachable (opening the indicator refreshes).
    const int interval = _recording ? kPollRecordingMs : (_reachable ? kPollIdleMs : 0);
    if (interval == 0) {
        _poll.stop();
    } else if (!_poll.isActive() || _poll.interval() != interval) {
        _poll.start(interval);
    }
}

QString TarRosbagRecorder::statusText() const
{
    QString text = tr("%1 bag recorder (ssh %2, ROS_DOMAIN_ID %3, folder %4, excluding %5): ")
                       .arg(_vehicle.label, host(), _domainId.isEmpty() ? tr("unset") : _domainId, _bagFolder,
                            _excludeRegex.isEmpty() ? tr("nothing") : _excludeRegex);
    if (!_known) {
        text += _lastError.isEmpty() ? tr("state not checked yet.") : tr("unreachable - %1.").arg(_lastError);
        return text;
    }
    if (_recording) {
        const qint64 elapsed = (QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(_startedAtMs)) / 1000;
        text += tr("RECORDING to %1, %2 elapsed, %3 so far").arg(_bagPath, formatDuration(qMax<qint64>(0, elapsed)), formatBytes(_bagBytes));
    } else {
        text += tr("idle");
        if (!_bagPath.isEmpty()) {
            text += tr(", last bag %1 (%2)").arg(_bagPath, formatBytes(_bagBytes));
        }
    }
    text += tr(", %1 free on the companion.").arg(formatBytes(_freeBytes));
    if (!_lastError.isEmpty()) {
        text += tr(" Last error: %1").arg(_lastError);
    }
    return text;
}

QString TarRosbagRecorder::_script(Command command) const
{
    QFile file(QString::fromLatin1(kScriptResource));
    QString body;
    if (file.open(QIODevice::ReadOnly)) {
        body = QString::fromUtf8(file.readAll());
    }
    // A Windows checkout may hand us CRLF, which bash would read as part of every command.
    body.remove(QLatin1Char('\r'));

    const Defaults d = _defaults(_vehicle.id);
    QString header;
    header += QStringLiteral("QGC_CMD=%1\n").arg(_quote(_commandName(command).toLower()));
    header += QStringLiteral("QGC_DOMAIN=%1\n").arg(_quote(_domainId));
    header += QStringLiteral("QGC_DIR=%1\n").arg(_quote(_bagFolder.isEmpty() ? d.bagFolder : _bagFolder));
    header += QStringLiteral("QGC_PREFIX=%1\n").arg(_quote(_bagPrefix.isEmpty() ? d.bagPrefix : _bagPrefix));
    header += QStringLiteral("QGC_EXCLUDE=%1\n").arg(_quote(_excludeRegex));
    header += QStringLiteral("QGC_SETUP=%1\n").arg(_quote(_setupScript));
    header += QStringLiteral("QGC_EXTRA=%1\n").arg(_quote(_extraArgs));
    return header + body;
}

QStringList TarRosbagRecorder::_sshArguments(const QString &host) const
{
    QStringList args{
        QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
        QStringLiteral("-o"), QStringLiteral("ConnectTimeout=8"),
        QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=accept-new"),
        QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=5"),
        QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=3"),
    };
    if (!_sshKey.isEmpty()) {
        args << QStringLiteral("-i") << _sshKey;
    }
    args << host << QStringLiteral("bash -s");
    return args;
}

QString TarRosbagRecorder::_sshProgram()
{
#ifdef Q_OS_WIN
    // Prefer the Windows OpenSSH client (uses %USERPROFILE%\.ssh) over any ssh found first on PATH.
    const QString system = QStringLiteral("C:/Windows/System32/OpenSSH/ssh.exe");
    if (QFileInfo::exists(system)) {
        return system;
    }
#endif
    const QString found = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    return found.isEmpty() ? QStringLiteral("ssh") : found;
}

QString TarRosbagRecorder::_scpProgram()
{
#ifdef Q_OS_WIN
    const QString system = QStringLiteral("C:/Windows/System32/OpenSSH/scp.exe");
    if (QFileInfo::exists(system)) {
        return system;
    }
#endif
    const QString found = QStandardPaths::findExecutable(QStringLiteral("scp"));
    return found.isEmpty() ? QStringLiteral("scp") : found;
}

QString TarRosbagRecorder::_quote(const QString &value)
{
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString TarRosbagRecorder::_commandName(Command command)
{
    switch (command) {
    case Command::Start:
        return QStringLiteral("Start");
    case Command::Stop:
        return QStringLiteral("Stop");
    case Command::Status:
        return QStringLiteral("Status");
    }
    return QStringLiteral("Status");
}

void TarRosbagRecorder::_appendLog(const QString &message)
{
    qCDebug(TarRosbagLog) << _vehicle.label << message;
    _log << QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), message);
    if (_log.size() > kMaxLogLines) {
        _log = _log.mid(_log.size() - kMaxLogLines);
    }
    emit logChanged();
}
