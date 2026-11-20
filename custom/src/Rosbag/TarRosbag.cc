#include "TarRosbag.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
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

const QString kDefaultHost = QStringLiteral("tar@100.66.89.102");
const QString kDefaultDomain = QStringLiteral("1");
const QString kDefaultFolder = QStringLiteral("~/bags");
const QString kDefaultPrefix = QStringLiteral("qgc");
// Raw camera overruns the Orin Nano eMMC; Point-LIO's /path and /aft_mapped_to_init run at ~16 kHz.
const QString kDefaultExclude = QStringLiteral("^(/camera/image_raw|/path|/aft_mapped_to_init)$");
const QString kDefaultSetup = QStringLiteral("~/raytheon-2027-drone/install/setup.bash");

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

TarRosbag::TarRosbag(QObject *parent)
    : QObject(parent)
{
    QSettings settings;
    settings.beginGroup(QLatin1String(kSettingsGroup));
    _host = settings.value(QStringLiteral("host"), kDefaultHost).toString();
    _sshKey = settings.value(QStringLiteral("sshKey")).toString();
    _domainId = settings.value(QStringLiteral("domainId"), kDefaultDomain).toString();
    _bagFolder = settings.value(QStringLiteral("bagFolder"), kDefaultFolder).toString();
    _bagPrefix = settings.value(QStringLiteral("bagPrefix"), kDefaultPrefix).toString();
    _excludeRegex = settings.value(QStringLiteral("excludeRegex"), kDefaultExclude).toString();
    _setupScript = settings.value(QStringLiteral("setupScript"), kDefaultSetup).toString();
    _extraArgs = settings.value(QStringLiteral("extraArgs")).toString();
    settings.endGroup();

    _timeout.setSingleShot(true);
    _timeout.setInterval(kTimeoutMs);
    (void) connect(&_timeout, &QTimer::timeout, this, [this]() {
        if (_process) {
            _appendLog(tr("%1 timed out after %2 s").arg(_commandName(_current.command)).arg(kTimeoutMs / 1000));
            _process->kill();
        }
    });

    (void) connect(&_poll, &QTimer::timeout, this, &TarRosbag::refresh);

    // Pick up a recording that is already running (e.g. QGC was restarted mid-flight).
    QTimer::singleShot(2000, this, &TarRosbag::refresh);
}

void TarRosbag::_setSetting(QString &member, const char *key, const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed == member) {
        return;
    }
    member = trimmed;
    QSettings().setValue(QStringLiteral("%1/%2").arg(QLatin1String(kSettingsGroup), QLatin1String(key)), member);
    emit settingsChanged();
}

void TarRosbag::setHost(const QString &value)
{
    const QString previous = _host;
    _setSetting(_host, "host", value);
    if (_host != previous) {
        _known = false;
        _reachable = false;
        emit stateChanged();
        _updatePolling();
    }
}

void TarRosbag::setSshKey(const QString &value) { _setSetting(_sshKey, "sshKey", value); }
void TarRosbag::setDomainId(const QString &value) { _setSetting(_domainId, "domainId", value); }
void TarRosbag::setBagFolder(const QString &value) { _setSetting(_bagFolder, "bagFolder", value); }
void TarRosbag::setBagPrefix(const QString &value) { _setSetting(_bagPrefix, "bagPrefix", value); }
void TarRosbag::setExcludeRegex(const QString &value) { _setSetting(_excludeRegex, "excludeRegex", value); }
void TarRosbag::setSetupScript(const QString &value) { _setSetting(_setupScript, "setupScript", value); }
void TarRosbag::setExtraArgs(const QString &value) { _setSetting(_extraArgs, "extraArgs", value); }

void TarRosbag::restoreDefaults()
{
    setHost(kDefaultHost);
    setSshKey(QString());
    setDomainId(kDefaultDomain);
    setBagFolder(kDefaultFolder);
    setBagPrefix(kDefaultPrefix);
    setExcludeRegex(kDefaultExclude);
    setSetupScript(kDefaultSetup);
    setExtraArgs(QString());
}

void TarRosbag::run(Command command, Callback done)
{
    if (_host.isEmpty()) {
        if (done) {
            done(false, tr("No companion computer set (ROS bag indicator > settings)."));
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

void TarRosbag::_startNext()
{
    if (_process || _queue.isEmpty()) {
        return;
    }
    _current = _queue.takeFirst();
    if (_current.command != Command::Status) {
        _appendLog(tr("%1 on %2...").arg(_commandName(_current.command), _host));
    }

    _process = new QProcess(this);
    _process->setProgram(_sshProgram());
    _process->setArguments(_sshArguments());
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

void TarRosbag::_finished(int exitCode, bool crashed)
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
        message = tr("Cannot reach %1: %2").arg(_host, detail);
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
        job.done(ok, message);
    }
    _startNext();
}

void TarRosbag::_applyStatus(const QVariantMap &values)
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
    const QString died = values.value(QStringLiteral("DIED")).toString();
    if (values.contains(QStringLiteral("DIED"))) {
        _lastError = tr("Recording stopped unexpectedly. %1").arg(died);
        _appendLog(_lastError);
    }
}

void TarRosbag::_updatePolling()
{
    // Fast while recording (size/elapsed, notice a crashed recorder); slow while idle and reachable;
    // no background ssh at all while the companion is unreachable (opening the indicator refreshes).
    const int interval = _recording ? kPollRecordingMs : (_reachable ? 30000 : 0);
    if (interval == 0) {
        _poll.stop();
    } else if (!_poll.isActive() || _poll.interval() != interval) {
        _poll.start(interval);
    }
}

QString TarRosbag::statusText() const
{
    if (_host.isEmpty()) {
        return tr("ROS bag recorder: no companion computer configured.");
    }
    QString text = tr("ROS bag recorder (ssh %1, ROS_DOMAIN_ID %2, folder %3, excluding %4): ")
                       .arg(_host, _domainId.isEmpty() ? tr("unset") : _domainId, _bagFolder,
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

QString TarRosbag::_script(Command command) const
{
    QFile file(QString::fromLatin1(kScriptResource));
    QString body;
    if (file.open(QIODevice::ReadOnly)) {
        body = QString::fromUtf8(file.readAll());
    }
    // A Windows checkout may hand us CRLF, which bash would read as part of every command.
    body.remove(QLatin1Char('\r'));

    QString header;
    header += QStringLiteral("QGC_CMD=%1\n").arg(_quote(_commandName(command).toLower()));
    header += QStringLiteral("QGC_DOMAIN=%1\n").arg(_quote(_domainId));
    header += QStringLiteral("QGC_DIR=%1\n").arg(_quote(_bagFolder.isEmpty() ? kDefaultFolder : _bagFolder));
    header += QStringLiteral("QGC_PREFIX=%1\n").arg(_quote(_bagPrefix.isEmpty() ? kDefaultPrefix : _bagPrefix));
    header += QStringLiteral("QGC_EXCLUDE=%1\n").arg(_quote(_excludeRegex));
    header += QStringLiteral("QGC_SETUP=%1\n").arg(_quote(_setupScript));
    header += QStringLiteral("QGC_EXTRA=%1\n").arg(_quote(_extraArgs));
    return header + body;
}

QStringList TarRosbag::_sshArguments() const
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
    args << _host << QStringLiteral("bash -s");
    return args;
}

QString TarRosbag::_sshProgram()
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

QString TarRosbag::_quote(const QString &value)
{
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString TarRosbag::_commandName(Command command)
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

void TarRosbag::_appendLog(const QString &message)
{
    qCDebug(TarRosbagLog) << message;
    _log << QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), message);
    if (_log.size() > kMaxLogLines) {
        _log = _log.mid(_log.size() - kMaxLogLines);
    }
    emit logChanged();
}
