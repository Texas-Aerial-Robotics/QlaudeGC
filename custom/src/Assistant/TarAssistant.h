#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QTimer>

class TarMcpServer;
class QQmlApplicationEngine;
class TarRosbag;
class TarRouter;

Q_DECLARE_LOGGING_CATEGORY(TarAssistantLog)

/// One row per chat entry: user prompt, assistant text, tool call, error or info line.
class TarChatModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles { KindRole = Qt::UserRole + 1, TextRole, DetailRole };

    struct Entry {
        QString kind;   ///< user | assistant | tool | error | info
        QString text;
        QString detail;
    };

    explicit TarChatModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int append(const QString &kind, const QString &text, const QString &detail = QString());
    void appendText(int row, const QString &text);
    void setDetail(int row, const QString &detail);
    void clear();
    const QList<Entry> &entries() const { return _entries; }
    void setEntries(const QList<Entry> &entries);

private:
    QList<Entry> _entries;
};

/// Chat sidebar backend: runs Claude Code (`claude -p`, stream-json) per turn, resumes the same session,
/// and prepends a snapshot of live QGC state (vehicles + built-in router) to each prompt.
class TarAssistant : public QObject
{
    Q_OBJECT
    Q_PROPERTY(TarChatModel *messages       READ messages                               CONSTANT)
    Q_PROPERTY(bool         busy            READ busy                                   NOTIFY busyChanged)
    Q_PROPERTY(QString      status          READ status                                 NOTIFY statusChanged)
    Q_PROPERTY(bool         panelVisible    READ panelVisible   WRITE setPanelVisible   NOTIFY panelVisibleChanged)
    Q_PROPERTY(double       panelWidth      READ panelWidth     WRITE setPanelWidth     NOTIFY panelWidthChanged)
    Q_PROPERTY(int          mode            READ mode           WRITE setMode           NOTIFY settingsChanged)
    Q_PROPERTY(bool         includeContext  READ includeContext WRITE setIncludeContext NOTIFY settingsChanged)
    Q_PROPERTY(QString      workingFolder   READ workingFolder  WRITE setWorkingFolder  NOTIFY settingsChanged)
    Q_PROPERTY(QString      model           READ model          WRITE setModel          NOTIFY settingsChanged)
    Q_PROPERTY(QString      effort          READ effort         WRITE setEffort         NOTIFY settingsChanged)
    Q_PROPERTY(QString      claudePath      READ claudePath                             NOTIFY settingsChanged)
    Q_PROPERTY(double       sessionCost     READ sessionCost                            NOTIFY sessionCostChanged)
    Q_PROPERTY(bool         hasSession      READ hasSession                             NOTIFY sessionCostChanged)

public:
    enum Mode { ReadOnly = 0, EditFiles = 1 };
    Q_ENUM(Mode)

    explicit TarAssistant(TarRouter *router, QObject *parent = nullptr);
    ~TarAssistant() override;

    TarChatModel *messages() const { return _messages; }
    bool busy() const { return _process != nullptr; }
    QString status() const { return _status; }
    bool panelVisible() const { return _panelVisible; }
    void setPanelVisible(bool visible);
    double panelWidth() const { return _panelWidth; }
    void setPanelWidth(double width);
    int mode() const { return _mode; }
    void setMode(int mode);
    bool includeContext() const { return _includeContext; }
    void setIncludeContext(bool include);
    QString workingFolder() const { return _workingFolder; }
    void setWorkingFolder(const QString &folder);
    QString model() const { return _model; }
    void setModel(const QString &model);
    QString effort() const { return _effort; }
    void setEffort(const QString &effort);
    QString claudePath() const;
    double sessionCost() const { return _sessionCost; }
    bool hasSession() const { return !_sessionId.isEmpty(); }

    Q_INVOKABLE void send(const QString &text);
    /// Messages you sent, oldest first, for Up/Down recall in the input box (kept across New chat).
    Q_INVOKABLE QStringList sentHistory() const { return _sentHistory; }
    Q_INVOKABLE void stop();
    Q_INVOKABLE void newChat();
    /// The snapshot that would be attached to the next message (for the "Preview context" view).
    Q_INVOKABLE QString contextSnapshot() const;
    Q_INVOKABLE void openWorkingFolder() const;
    /// Open Claude Code in a terminal window so the user can sign in.
    Q_INVOKABLE void openSignIn() const;

    /// Lets the MCP object tools reach QML singletons and the main window.
    void setQmlEngine(QQmlApplicationEngine *engine);
    /// Adds the companion ROS bag recorder to the context snapshot and the MCP tools.
    void setRosbag(TarRosbag *rosbag);
    /// Approve or reject the approval card at chat row `row`.
    Q_INVOKABLE void resolveApproval(int row, bool approved);

signals:
    void busyChanged();
    void statusChanged();
    void panelVisibleChanged();
    void panelWidthChanged();
    void settingsChanged();
    void sessionCostChanged();

private:
    void _setStatus(const QString &status);
    void _onReadyRead();
    void _handleEvent(const QJsonObject &event);
    void _onFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void _flushPendingText();
    QStringList _arguments() const;
    QString _systemPrompt() const;
    QString _routerConfigDir() const;
    QString _writeMcpConfig() const;
    void _loadChat();
    void _saveChat() const;
    void _appendError(const QString &message);
    void _startTurn(const QString &prompt);
    static QString _toolSummary(const QString &name, const QJsonObject &input);

    TarRouter *_router = nullptr;
    TarRosbag *_rosbag = nullptr;
    TarChatModel *_messages = nullptr;
    QProcess *_process = nullptr;
    QByteArray _stdoutBuffer;
    QByteArray _stderrTail;
    bool _gotResult = false;
    bool _gotText = false;
    bool _stopRequested = false;

    int _currentTextRow = -1;
    QString _pendingText;
    QTimer _flushTimer;
    QHash<QString, int> _toolRows;   ///< tool_use id -> chat row
    TarMcpServer *_mcp = nullptr;
    QHash<int, int> _approvalRows;   ///< approval id -> chat row

    QString _status;
    QString _sessionId;
    QString _lastPrompt;
    QStringList _sentHistory;
    bool _retryWithoutResume = false;
    QString _sessionIdBeforeTurn;   ///< restored if the turn fails before reaching Claude (e.g. signed out)
    double _sessionCost = 0;
    bool _panelVisible = false;
    double _panelWidth = 420;
    int _mode = ReadOnly;
    bool _includeContext = true;
    QString _workingFolder;
    QString _model;
    QString _effort; ///< empty = Claude Code's default
};
