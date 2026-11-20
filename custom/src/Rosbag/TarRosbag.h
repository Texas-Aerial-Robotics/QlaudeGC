#pragma once

#include <functional>

#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>

class QProcess;

Q_DECLARE_LOGGING_CATEGORY(TarRosbagLog)

/// Starts and stops `ros2 bag record` on the companion computer over ssh (key auth, BatchMode).
/// Each command sends res/rosbag/rosbag_ctl.sh to `bash -s` with the settings prepended as shell variables.
/// The recorder runs detached on the companion and its state lives there (~/.qgc_rosbag.*), so a restarted
/// QGC picks up a recording that is already running. Exposed to QML as the `tarRosbag` context property.
class TarRosbag : public QObject
{
    Q_OBJECT

    // Settings (persisted)
    Q_PROPERTY(QString host         READ host           WRITE setHost           NOTIFY settingsChanged)
    Q_PROPERTY(QString sshKey       READ sshKey         WRITE setSshKey         NOTIFY settingsChanged)
    Q_PROPERTY(QString domainId     READ domainId       WRITE setDomainId       NOTIFY settingsChanged)
    Q_PROPERTY(QString bagFolder    READ bagFolder      WRITE setBagFolder      NOTIFY settingsChanged)
    Q_PROPERTY(QString bagPrefix    READ bagPrefix      WRITE setBagPrefix      NOTIFY settingsChanged)
    Q_PROPERTY(QString excludeRegex READ excludeRegex   WRITE setExcludeRegex   NOTIFY settingsChanged)
    Q_PROPERTY(QString setupScript  READ setupScript    WRITE setSetupScript    NOTIFY settingsChanged)
    Q_PROPERTY(QString extraArgs    READ extraArgs      WRITE setExtraArgs      NOTIFY settingsChanged)

    // State (from the last command)
    Q_PROPERTY(bool     recording   READ recording      NOTIFY stateChanged)
    Q_PROPERTY(bool     busy        READ busy           NOTIFY busyChanged)
    Q_PROPERTY(bool     reachable   READ reachable      NOTIFY stateChanged)
    Q_PROPERTY(bool     known       READ known          NOTIFY stateChanged)
    Q_PROPERTY(QString  bagPath     READ bagPath        NOTIFY stateChanged)
    Q_PROPERTY(double   bagBytes    READ bagBytes       NOTIFY stateChanged)
    Q_PROPERTY(double   startedAtMs READ startedAtMs    NOTIFY stateChanged)
    Q_PROPERTY(double   freeBytes   READ freeBytes      NOTIFY stateChanged)
    Q_PROPERTY(QString  lastError   READ lastError      NOTIFY stateChanged)
    Q_PROPERTY(QStringList log      READ log            NOTIFY logChanged)

public:
    enum class Command { Status, Start, Stop };

    /// ok: the command did what was asked. message: one-line result for the user (or the error).
    using Callback = std::function<void(bool ok, const QString &message)>;

    explicit TarRosbag(QObject *parent = nullptr);

    Q_INVOKABLE void start() { run(Command::Start); }
    Q_INVOKABLE void stop() { run(Command::Stop); }
    Q_INVOKABLE void refresh() { run(Command::Status); }
    Q_INVOKABLE void restoreDefaults();

    /// Queues a command; done (optional) runs when it finishes.
    void run(Command command, Callback done = {});
    /// One-paragraph state description (assistant context and the rosbag_status tool).
    QString statusText() const;

    QString host() const { return _host; }
    void setHost(const QString &value);
    QString sshKey() const { return _sshKey; }
    void setSshKey(const QString &value);
    QString domainId() const { return _domainId; }
    void setDomainId(const QString &value);
    QString bagFolder() const { return _bagFolder; }
    void setBagFolder(const QString &value);
    QString bagPrefix() const { return _bagPrefix; }
    void setBagPrefix(const QString &value);
    QString excludeRegex() const { return _excludeRegex; }
    void setExcludeRegex(const QString &value);
    QString setupScript() const { return _setupScript; }
    void setSetupScript(const QString &value);
    QString extraArgs() const { return _extraArgs; }
    void setExtraArgs(const QString &value);

    bool recording() const { return _recording; }
    bool busy() const { return _process != nullptr; }
    bool reachable() const { return _reachable; }
    bool known() const { return _known; }
    QString bagPath() const { return _bagPath; }
    double bagBytes() const { return _bagBytes; }
    double startedAtMs() const { return _startedAtMs; }
    double freeBytes() const { return _freeBytes; }
    QString lastError() const { return _lastError; }
    QStringList log() const { return _log; }

signals:
    void settingsChanged();
    void stateChanged();
    void busyChanged();
    void logChanged();

private:
    struct Job {
        Command command;
        Callback done;
    };

    void _setSetting(QString &member, const char *key, const QString &value);
    void _startNext();
    void _finished(int exitCode, bool crashed);
    QString _script(Command command) const;
    QStringList _sshArguments() const;
    void _applyStatus(const QVariantMap &values);
    void _appendLog(const QString &message);
    void _updatePolling();

    static QString _sshProgram();
    static QString _quote(const QString &value);
    static QString _commandName(Command command);

    static constexpr int kTimeoutMs = 45000;   ///< stop waits up to 30 s on the companion
    static constexpr int kPollRecordingMs = 5000;
    static constexpr int kMaxLogLines = 200;

    QString _host;
    QString _sshKey;
    QString _domainId;
    QString _bagFolder;
    QString _bagPrefix;
    QString _excludeRegex;
    QString _setupScript;
    QString _extraArgs;

    bool _recording = false;
    bool _reachable = false;
    bool _known = false;
    QString _bagPath;
    double _bagBytes = 0;
    double _startedAtMs = 0;
    double _freeBytes = 0;
    QString _lastError;
    QStringList _log;

    QList<Job> _queue;
    Job _current{Command::Status, {}};
    QProcess *_process = nullptr;
    QTimer _timeout;
    QTimer _poll;
};
