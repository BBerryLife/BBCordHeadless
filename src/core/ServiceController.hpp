#ifndef SERVICECONTROLLER_HPP
#define SERVICECONTROLLER_HPP

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QTimer;
class AppStore;
class DiscordClient;
class DiscordGateway;
class GatewayHandler;
class HubIntegration;
namespace bb { namespace system { class InvokeManager; class InvokeRequest; } }

// Owns the headless Discord client and the handoff with the UI:
//  - UI closed -> create AppStore + DiscordClient (headless mode) and log in with the saved
//    token, so MESSAGE_CREATE events turn into Hub items while the app is closed;
//  - UI open   -> destroy them (the UI owns the gateway).
// It also receives what Hub sends to the account target:
//  - MARKREAD / MARKUNREAD / DELETE: updates the Hub item in the background (HubShared);
//  - OPEN / VIEW (short tap): forwarded to the UI target.
class ServiceController : public QObject
{
    Q_OBJECT
public:
    explicit ServiceController(QObject *parent = 0);
    virtual ~ServiceController();

private slots:
    void onInvoked(const bb::system::InvokeRequest &request);
    // Decides whether the headless client should be running (UI pid file, saved token).
    void evaluate();
    void onLoginSucceeded();
    void onLoginFailed(const QString &message);
    void onStopRequested();
    // Logs memory use while the client runs, so a memory-related kill can be seen coming.
    void onHeartbeat();
    // RESUME mode (default): events from the session the UI left behind.
    void onGatewayDispatch(const QString &eventName, const QVariantMap &payload);
    void onGatewayDown(const QString &reason);
    void onGatewayClosed();
    void onSessionInvalid();
    void retryResume();
    void startIdentifyFallback();

private:
    void startClient();
    void startResumeSession();
    void reconnectGateway();
    void stopClient();
    void scheduleRetry();
    void handleItemAction(const QString &action, const QString &channelId);

    bb::system::InvokeManager *m_invoke;
    QTimer *m_pollTimer;
    QTimer *m_retryTimer;
    QTimer *m_heartbeatTimer;
    int m_heartbeatTicks;
    AppStore *m_store;
    DiscordClient *m_client;
    bool m_lastUiAlive;
    // The saved token was rejected (or is missing): stay idle until the UI has run again,
    // since only the user can fix that.
    bool m_sessionDead;
    bool m_gatewayNoticeLogged;
    int m_retryDelayMs;
    // Consecutive out-of-memory failures while receiving gateway data (see onLoginFailed()).
    int m_oomFailures;

    // RESUME mode objects (no DiscordClient: no worker threads, REST, avatars or models).
    DiscordGateway *m_gateway;
    GatewayHandler *m_notifier;
    HubIntegration *m_hub;
    QString m_token;
    QString m_userId;
    bool m_gatewayNeedsReconnect;
    // The session handed over by the UI, kept to retry a refused RESUME once.
    QString m_resumeSid;
    QString m_resumeUrl;
    int m_resumeSeq;
    bool m_resumeRetried;
    // Fallback when Discord refuses the RESUME for good: a fresh IDENTIFY whose READY is
    // streamed through a scanner instead of being stored (see DiscordGateway light mode).
    bool m_identifyTried;
    QStringList m_resumeSubs;
    QStringList m_recentMessageIds; // replayed events after a RESUME must not ping twice
};

#endif
