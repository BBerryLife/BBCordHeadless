#include "core/ServiceController.hpp"

#include "core/AppStore.hpp"
#include "core/Client.hpp"
#include "core/HubShared.hpp"
#include "core/ServiceHandoff.hpp"

#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>

#include <QByteArray>
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
      m_store(0),
      m_client(0),
      m_lastUiAlive(false),
      m_sessionDead(false),
      m_retryDelayMs(RETRY_MIN_MS)
{
    connect(m_invoke, SIGNAL(invoked(const bb::system::InvokeRequest&)),
            this,     SLOT(onInvoked(const bb::system::InvokeRequest&)));

    m_pollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_pollTimer, SIGNAL(timeout()), this, SLOT(evaluate()));
    m_pollTimer->start();

    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, SIGNAL(timeout()), this, SLOT(evaluate()));

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
        }
        m_lastUiAlive = uiAlive;
    }

    if (uiAlive) {
        // The UI owns the gateway connection while it is alive.
        m_retryTimer->stop();
        if (m_client) stopClient();
        return;
    }

    if (m_sessionDead || m_client || m_retryTimer->isActive()) return;
    startClient();
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
    m_client->autoLogin();
}

void ServiceController::stopClient()
{
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

void ServiceController::onLoginSucceeded()
{
    qDebug() << "[Service] background gateway connected";
    m_retryDelayMs = RETRY_MIN_MS;
}

void ServiceController::onLoginFailed(const QString &message)
{
    qWarning() << "[Service] login failed:" << message;
    // Cannot delete the client from inside its own signal: do it right after.
    if (message.contains(QLatin1String("Invalid Discord token"))) {
        m_sessionDead = true;
        qWarning() << "[Service] the saved token was rejected - idle until the app runs again";
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
