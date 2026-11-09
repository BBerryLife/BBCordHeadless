#include "core/ServiceController.hpp"

#include "core/AppStore.hpp"
#include "core/Client.hpp"
#include "core/HubIntegration.hpp"
#include "core/HubShared.hpp"
#include "core/ServiceHandoff.hpp"
#include "core/ServiceSession.hpp"
#include "core/client/GatewayHandler.hpp"
#include "core/discord/Gateway.hpp"

#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>

#include <malloc.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QDebug>
#include <QSettings>
#include <QTimer>

// How often the UI pid file is checked. The UI_OPENED/UI_CLOSED invokes make the service react
// immediately; this only covers a lost invoke or a crashed UI.
static const int POLL_INTERVAL_MS = 4000;
// Retry schedule after a transient failure (no network, gateway down): 15 s, doubling up to
// 5 min. Reset by a successful login.
static const int RETRY_MIN_MS = 15000;
static const int RETRY_MAX_MS = 300000;
// An always-on background service has no reason to wake 100x/s like the UI's 10 ms loop.
// Notifications do not need sub-second latency.
static const int SERVICE_POLL_MS = 250;
static const int RECENT_IDS_MAX = 64;

// Markers in the app data folder guard against an out-of-memory crash loop: the "inflight"
// file exists while an IDENTIFY fallback is running; if the service finds it at startup the
// previous attempt died (almost certainly the 3 MB heap limit), so the fallback is disabled
// until the "crashed" marker is deleted (it is also deleted after any success).
static QString identifyInflightPath() { return QDir::homePath() + "/bbcord_identify_inflight"; }
static QString identifyCrashedPath() { return QDir::homePath() + "/bbcord_identify_crashed"; }


// The Hub invoke data is JSON like {"attributes": {"sourceId": "<channelId>", ...}}. Only
// that one field is needed, so it is read with a plain scan instead of a JSON library.
static QString extractSourceId(const QByteArray &data)
{
    const QByteArray key("\"sourceId\"");
    int k = data.indexOf(key);
    if (k < 0) return QString();
    int i = data.indexOf(':', k + key.size());
    if (i < 0) return QString();
    ++i;
    while (i < data.size() && (data.at(i) == ' ' || data.at(i) == '\t'
                               || data.at(i) == '\r' || data.at(i) == '\n')) ++i;
    bool quoted = (i < data.size() && data.at(i) == '"');
    if (quoted) ++i;

    QByteArray value;
    for (; i < data.size(); ++i) {
        char c = data.at(i);
        if (quoted ? (c == '"') : (c == ',' || c == '}' || c == ' ' || c == '\n')) break;
        value.append(c);
    }
    return QString::fromUtf8(value).trimmed();
}

ServiceController::ServiceController(QObject *parent)
    : QObject(parent),
      m_invoke(new bb::system::InvokeManager(this)),
      m_pollTimer(new QTimer(this)),
      m_retryTimer(new QTimer(this)),
      m_heartbeatTimer(new QTimer(this)),
      m_heartbeatTicks(0),
      m_store(0),
      m_client(0),
      m_lastUiAlive(false),
      m_sessionDead(false),
      m_gatewayNoticeLogged(false),
      m_retryDelayMs(RETRY_MIN_MS),
      m_oomFailures(0),
      m_gateway(0),
      m_notifier(0),
      m_hub(0),
      m_gatewayNeedsReconnect(false),
      m_resumeSeq(-1),
      m_resumeRetried(false),
      m_identifyTried(false)
{
    connect(m_invoke, SIGNAL(invoked(const bb::system::InvokeRequest&)),
            this,     SLOT(onInvoked(const bb::system::InvokeRequest&)));

    m_pollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_pollTimer, SIGNAL(timeout()), this, SLOT(evaluate()));
    m_pollTimer->start();

    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, SIGNAL(timeout()), this, SLOT(evaluate()));

    m_heartbeatTimer->setInterval(5000);
    connect(m_heartbeatTimer, SIGNAL(timeout()), this, SLOT(onHeartbeat()));
    m_heartbeatTimer->start();

    qDebug() << "[Service] started";
    // Not immediately: at boot the UI may be starting too.
    QTimer::singleShot(1500, this, SLOT(evaluate()));
}

ServiceController::~ServiceController()
{
    stopClient();
}

void ServiceController::evaluate()
{
    bool uiAlive = ServiceHandoff::isUiAlive();
    if (uiAlive != m_lastUiAlive) {
        qDebug() << "[Service] UI" << (uiAlive ? "opened" : "closed");
        if (!uiAlive) {
            // The user may have logged in again while the UI was up.
            m_sessionDead = false;
            m_retryDelayMs = RETRY_MIN_MS;
            m_oomFailures = 0;
        }
        m_lastUiAlive = uiAlive;
    }

    if (uiAlive) {
        // The UI owns the gateway connection while it is alive.
        m_retryTimer->stop();
        if (m_client) stopClient();
        return;
    }

    if (m_sessionDead || m_retryTimer->isActive()) return;

    if (m_gateway) {
        // Dropped without a close/error signal (or the retry timer just fired).
        if (m_gateway->state() == DiscordGateway::Disconnected) m_gatewayNeedsReconnect = true;
        if (m_gatewayNeedsReconnect) reconnectGateway();
        return;
    }
    if (m_client) return;

    // Default: take over the UI's gateway session with a RESUME. A RESUME produces no READY,
    // so it fits the ~3 MB heap limit of this process (RLIMIT_DATA, logged at start); an
    // IDENTIFY cannot (its ~5 MB READY ran out of memory every time).
    // QSettings("BBerryLife","BBCordHub") key service/gateway = true selects the old full
    // client instead, only useful on a system with a higher limit.
    QSettings hubSettings(QLatin1String("BBerryLife"), QLatin1String("BBCordHub"));
    hubSettings.sync();
    if (hubSettings.value(QLatin1String("service/gateway"), false).toBool()) {
        startClient();
    } else {
        startResumeSession();
    }
}

void ServiceController::startResumeSession()
{
    QSettings settings;
    settings.sync();
    m_token = settings.value("auth/token").toString().trimmed();
    if (m_token.isEmpty()) {
        qDebug() << "[Service] no saved Discord token - staying idle until the app logs in";
        m_sessionDead = true;
        return;
    }

    if (QFile::exists(identifyInflightPath())) {
        QFile::remove(identifyInflightPath());
        QFile crashed(identifyCrashedPath());
        if (crashed.open(QIODevice::WriteOnly)) { crashed.write("1"); crashed.close(); }
        qWarning() << "[Service] the previous IDENTIFY fallback did not finish (crash/OOM) - disabled";
    }

    ServiceSession::GatewayState gs;
    if (!ServiceSession::loadGateway(&gs)) {
        qDebug() << "[Service] no gateway session left by the app yet - idle until the app has run";
        m_sessionDead = true;
        return;
    }

    AppStore *store = new AppStore(this);
    if (!ServiceSession::loadProfile(store, &m_userId)) {
        qDebug() << "[Service] no notification profile left by the app yet - idle until the app has run";
        delete store;
        m_sessionDead = true;
        return;
    }
    m_store = store;

    qDebug() << "[Service] UI closed - resuming the gateway session, seq" << gs.seq
             << "age s" << (QDateTime::currentMSecsSinceEpoch() - gs.savedMs) / 1000;

    m_hub = new HubIntegration(this);
    m_hub->setServiceMode(true);
    m_hub->init();
    m_notifier = new GatewayHandler(0, m_store, this);

    m_gateway = new DiscordGateway(this);
    m_gateway->setPollIntervalMs(SERVICE_POLL_MS);
    m_gateway->setHeartbeatWatchdog(true);
    // Lets the light MESSAGE_CREATE parser flag mentions of us.
    m_gateway->updateMessageFilterState(QString(), QStringList(), m_userId);
    connect(m_gateway, SIGNAL(dispatchReceived(QString,QVariantMap)),
            this, SLOT(onGatewayDispatch(QString,QVariantMap)));
    connect(m_gateway, SIGNAL(error(QString)), this, SLOT(onGatewayDown(QString)));
    connect(m_gateway, SIGNAL(closed()), this, SLOT(onGatewayClosed()));
    connect(m_gateway, SIGNAL(sessionInvalid()), this, SLOT(onSessionInvalid()));
    connect(m_gateway, SIGNAL(ready(QString)), this, SLOT(onLoginSucceeded()));

    m_heartbeatTicks = 0;
    m_gatewayNeedsReconnect = false;
    m_resumeSid = gs.sessionId;
    m_resumeUrl = gs.resumeUrl;
    m_resumeSeq = gs.seq;
    m_resumeRetried = false;
    m_identifyTried = false;
    m_resumeSubs = gs.subs;
    m_gateway->resumeGateway(m_token, gs.sessionId, gs.resumeUrl, gs.seq);
}

void ServiceController::reconnectGateway()
{
    if (!m_gateway) return;
    m_gatewayNeedsReconnect = false;
    const QString sid = m_gateway->sessionId();
    if (sid.isEmpty() || m_gateway->sequence() < 0) {
        // Nothing to resume (e.g. the session was refused): only the app can fix that.
        m_sessionDead = true;
        QTimer::singleShot(0, this, SLOT(onStopRequested()));
        return;
    }
    qDebug() << "[Service] reconnecting (RESUME), seq" << m_gateway->sequence();
    m_gateway->resumeGateway(m_token, sid, m_gateway->resumeGatewayUrl(), m_gateway->sequence());
}

void ServiceController::onGatewayDown(const QString &reason)
{
    qWarning() << "[Service] gateway error:" << reason;
    // MG_EV_CLOSE follows an error and emits closed(); reconnect from there / the poll.
    if (!m_retryTimer->isActive()) {
        m_gatewayNeedsReconnect = true;
        scheduleRetry();
    }
}

void ServiceController::onGatewayClosed()
{
    qDebug() << "[Service] gateway closed";
    if (!m_retryTimer->isActive() && m_gateway) {
        m_gatewayNeedsReconnect = true;
        scheduleRetry();
    }
}

void ServiceController::startIdentifyFallback()
{
    m_identifyTried = true;
    if (QFile::exists(identifyCrashedPath())) {
        qWarning() << "[Service] session lost; IDENTIFY fallback disabled (it crashed the service"
                   << "before). Open the app once to refresh the session.";
        ServiceSession::clearGateway();
        m_sessionDead = true;
        QTimer::singleShot(0, this, SLOT(onStopRequested()));
        return;
    }
    qWarning() << "[Service] session lost - new IDENTIFY, READY will be streamed, not stored";
    QFile marker(identifyInflightPath());
    if (marker.open(QIODevice::WriteOnly)) {
        marker.write("1");
        marker.close();
    }
    ServiceSession::clearGateway();
    m_gateway->setLightStreamMode(true);
    m_gateway->connectToGateway(m_token);
}

void ServiceController::retryResume()
{
    if (!m_gateway || m_sessionDead) return;
    qDebug() << "[Service] retrying the RESUME once, seq" << m_resumeSeq;
    m_gateway->resumeGateway(m_token, m_resumeSid, m_resumeUrl, m_resumeSeq);
}

void ServiceController::onSessionInvalid()
{
    // Op 9 with d:true means "wait and try again"; d:false means the session is gone for
    // good (a retry is pointless: Discord drops it when it refuses the first RESUME).
    if (!m_resumeRetried && m_resumeSeq >= 0 && m_gateway && m_gateway->lastInvalidResumable()) {
        m_resumeRetried = true;
        qWarning() << "[Service] RESUME refused but resumable - retrying once in 3 s";
        QTimer::singleShot(3000, this, SLOT(retryResume()));
        return;
    }
    if (!m_identifyTried && m_gateway) {
        startIdentifyFallback();
        return;
    }
    ServiceSession::clearGateway();
    // Discord no longer knows the session. A new IDENTIFY would bring a ~5 MB READY that
    // does not fit in this process, so stay idle until the app has run (it makes a new one).
    qWarning() << "[Service] the gateway session expired - idle until the app runs again";
    m_sessionDead = true;
    QTimer::singleShot(0, this, SLOT(onStopRequested()));
}

void ServiceController::onGatewayDispatch(const QString &eventName, const QVariantMap &payload)
{
    if (eventName != QLatin1String("MESSAGE_CREATE") || !m_notifier || !m_hub) return;

    const QString msgId = payload.value("id").toString();
    if (!msgId.isEmpty()) {
        if (m_recentMessageIds.contains(msgId)) return;
        m_recentMessageIds.append(msgId);
        while (m_recentMessageIds.size() > RECENT_IDS_MAX) m_recentMessageIds.removeFirst();
    }

    MentionNotification n = m_notifier->buildMentionNotification(payload);
    if (!n.shouldNotify) return;
    m_hub->upsertThreadItem(n.sourceId, n.title, n.preview, n.timestampMs);
    qDebug() << "[Service] Hub item updated for channel" << n.sourceId;
}

void ServiceController::startClient()
{
    // A default QSettings(), like the client itself uses (same file as the UI).
    QSettings settings;
    settings.sync();
    QString token = settings.value("auth/token").toString().trimmed();
    if (token.isEmpty()) {
        qDebug() << "[Service] no saved Discord token - staying idle until the app logs in";
        m_sessionDead = true;
        return;
    }

    qDebug() << "[Service] UI closed - connecting the gateway in the background";
    m_store = new AppStore(this);
    m_client = new DiscordClient(m_store, this);
    m_client->setHeadlessMode(true);
    connect(m_client, SIGNAL(loginSucceeded()), this, SLOT(onLoginSucceeded()));
    connect(m_client, SIGNAL(loginFailed(QString)), this, SLOT(onLoginFailed(QString)));
    m_heartbeatTicks = 0;
    m_client->autoLogin();
}

void ServiceController::stopClient()
{
    if (m_gateway) {
        qDebug() << "[Service] dropping the gateway connection";
        m_gateway->disconnect(this);
        delete m_gateway; // no close frame, see ~DiscordGateway()
        m_gateway = 0;
        delete m_notifier; m_notifier = 0;
        delete m_hub; m_hub = 0;
    }
    m_gatewayNeedsReconnect = false;
    if (!m_client && !m_store) return;
    qDebug() << "[Service] disconnecting the headless client";
    if (m_client) {
        m_client->disconnect(this);
        delete m_client; // shuts its worker threads down
        m_client = 0;
    }
    delete m_store;
    m_store = 0;
}

void ServiceController::onHeartbeat()
{
    if (!m_client && !m_gateway) return;
    ++m_heartbeatTicks;
    // Every 5 s for the first 2 minutes (the heavy READY payload arrives then), then every 5 min.
    if (m_heartbeatTicks > 24 && (m_heartbeatTicks % 60) != 0) return;
    struct mallinfo mi = mallinfo();
    qDebug() << "[Service] alive, malloc heap in use ~" << (mi.uordblks + mi.hblkhd) / (1024 * 1024)
             << "MB (mallinfo: does not count every mapping)";
}

void ServiceController::onLoginSucceeded()
{
    if (m_gateway && m_identifyTried) {
        // Fresh session: Discord only sends guild-channel messages for channels subscribed
        // on THIS session, so repeat the subscriptions the app had made.
        QFile::remove(identifyInflightPath());
        QFile::remove(identifyCrashedPath());
        qDebug() << "[Service] IDENTIFY fallback succeeded; re-subscribing" << m_resumeSubs.size();
        for (int i = 0; i < m_resumeSubs.size(); ++i) {
            const QString key = m_resumeSubs.at(i);
            const int sep = key.indexOf(QChar(':'));
            if (sep > 0) m_gateway->sendLazyRequest(key.left(sep), key.mid(sep + 1));
        }
        m_resumeSubs.clear();
    }
    qDebug() << "[Service] background gateway connected";
    m_retryDelayMs = RETRY_MIN_MS;
    m_oomFailures = 0;
}

void ServiceController::onLoginFailed(const QString &message)
{
    qWarning() << "[Service] login failed:" << message;
    // Cannot delete the client from inside its own signal: do it right after.
    if (message.contains(QLatin1String("Invalid Discord token"))) {
        m_sessionDead = true;
        qWarning() << "[Service] the saved token was rejected - idle until the app runs again";
    } else if (message.contains(QLatin1String("OOM"))) {
        // Out of memory is repeatable: the same data will fail the same way, and every retry sends
        // a new IDENTIFY to Discord. Try once more, then stop until the app has run again.
        ++m_oomFailures;
        if (m_oomFailures >= 2) {
            m_sessionDead = true;
            qWarning() << "[Service] out of memory receiving gateway data" << m_oomFailures
                       << "times - giving up the background connection until the app runs again";
        } else {
            scheduleRetry();
        }
    } else {
        scheduleRetry();
    }
    QTimer::singleShot(0, this, SLOT(onStopRequested()));
}

void ServiceController::onStopRequested()
{
    stopClient();
}

void ServiceController::scheduleRetry()
{
    qDebug() << "[Service] retrying in" << m_retryDelayMs / 1000 << "s";
    m_retryTimer->start(m_retryDelayMs);
    m_retryDelayMs = qMin(m_retryDelayMs * 2, RETRY_MAX_MS);
}

void ServiceController::onInvoked(const bb::system::InvokeRequest &request)
{
    const QString action = request.action();
    qDebug() << "[Service] invoked action=" << action << "dataBytes=" << request.data().size();

    if (action == QLatin1String(ServiceHandoff::ACTION_UI_OPENED)
        || action == QLatin1String(ServiceHandoff::ACTION_UI_CLOSED)
        || action == QLatin1String("bb.action.system.STARTED")) {
        evaluate();
        return;
    }

    if (HubShared::isItemAction(action)) {
        QString channelId = extractSourceId(request.data());
        if (channelId.isEmpty()) {
            qWarning() << "[Service] no sourceId in invoke data - ignoring" << action;
            return;
        }
        handleItemAction(action, channelId);
        return;
    }

    if (action == QLatin1String(HubShared::ACTION_OPEN)
        || action == QLatin1String(HubShared::ACTION_VIEW)) {
        // Short tap on an item: hand over to the UI exactly as received.
        bb::system::InvokeRequest fwd;
        fwd.setTarget(HubShared::UI_TARGET);
        fwd.setAction(action);
        fwd.setMimeType(request.mimeType());
        fwd.setUri(request.uri());
        fwd.setData(request.data());
        m_invoke->invoke(fwd);
        qDebug() << "[Service] forwarded" << action << "to" << HubShared::UI_TARGET;
    }
}

void ServiceController::handleItemAction(const QString &action, const QString &channelId)
{
    void *uds = HubShared::openUds();
    if (!uds) {
        qWarning() << "[Service] Hub unavailable - cannot handle" << action << "for" << channelId;
        return;
    }

    bool done = false;
    if (action == QLatin1String(HubShared::ACTION_DELETE)) {
        // Removes the row from Hub only (nothing is deleted on Discord).
        HubShared::removeItem(uds, channelId);
        HubShared::forgetState(channelId);
        done = true; // the row may already be gone; the app should forget it either way
    } else {
        // uds_item_updated() replaces the whole record, so the title/preview/time stored
        // by whoever published the item are needed to rebuild it.
        HubShared::ItemState st;
        if (!HubShared::loadState(channelId, st)) {
            qWarning() << "[Service] no stored state for" << channelId << "- ignoring" << action;
        } else {
            if (action == QLatin1String(HubShared::ACTION_MARK_READ)) {
                st.unread = 0;
            } else if (st.unread < 1) { // MARK_UNREAD
                st.unread = 1;
            }
            if (st.total < 1) st.total = 1;
            HubShared::saveState(channelId, st);
            // notify=false: only the badge changes, no banner or sound.
            done = HubShared::publishItem(uds, channelId, st, false /* notify */,
                                          false /* allowAdd */);
        }
    }

    HubShared::closeUds(uds);

    if (done) {
        // Let a running UI mirror it in the app (unread dots).
        HubShared::postEvent(action, channelId);
    }
    qDebug() << "[Service]" << action << "for" << channelId << (done ? "done" : "NOT applied");
}
