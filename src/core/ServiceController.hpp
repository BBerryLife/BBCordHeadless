#ifndef SERVICECONTROLLER_HPP
#define SERVICECONTROLLER_HPP

#include <QObject>
#include <QString>

class QTimer;
class AppStore;
class DiscordClient;
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

private:
    void startClient();
    void stopClient();
    void scheduleRetry();
    void handleItemAction(const QString &action, const QString &channelId);

    bb::system::InvokeManager *m_invoke;
    QTimer *m_pollTimer;
    QTimer *m_retryTimer;
    AppStore *m_store;
    DiscordClient *m_client;
    bool m_lastUiAlive;
    // The saved token was rejected (or is missing): stay idle until the UI has run again,
    // since only the user can fix that.
    bool m_sessionDead;
    int m_retryDelayMs;
};

#endif
