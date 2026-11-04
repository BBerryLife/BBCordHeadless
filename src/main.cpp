// BBCord headless service.
//
//  - While the BBCord UI is closed it keeps the Discord gateway connected (the same client
//    code as the app, compiled from the BBCord project) so DMs and mentions still reach
//    BlackBerry Hub.
//  - While the UI is open it disconnects: the UI owns the gateway connection (see
//    ServiceHandoff.hpp).
//  - It is the Hub account's invoke target: Mark Read / Mark Unread / Delete run here in the
//    background, and a tap on an item is forwarded to the UI.
#include "core/ServiceController.hpp"

#include <bb/Application>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

using namespace bb;

static QFile *g_logFile = 0;
static int g_logFd = -1;
static char g_runMarkerPath[512] = {0};

// Writes a line straight to the log file. Only write()/unlink() are used so it is safe from a
// signal handler.
static void rawLog(const char *text)
{
    if (g_logFd < 0) return;
    size_t n = 0;
    while (text[n]) ++n;
    ssize_t r = ::write(g_logFd, text, n);
    (void)r;
}

// A headless process that dies leaves nothing behind unless it is told to. This logs which
// signal ended it (SIGSEGV = crash, SIGTERM = the system asked it to stop). A SIGKILL cannot be
// caught: that case is detected on the next start through the run marker (see main()).
static void fatalSignalHandler(int sig, siginfo_t *info, void *)
{
    const char *name = (sig == SIGSEGV) ? "SIGSEGV (crash: invalid memory access)"
                     : (sig == SIGBUS)  ? "SIGBUS (crash)"
                     : (sig == SIGFPE)  ? "SIGFPE (crash)"
                     : (sig == SIGILL)  ? "SIGILL (crash)"
                     : (sig == SIGABRT) ? "SIGABRT (abort / failed assertion)"
                     : (sig == SIGTERM) ? "SIGTERM (the system asked the service to stop)"
                     : (sig == SIGHUP)  ? "SIGHUP"
                     : "signal";
    rawLog("\n[FATAL] service ended by ");
    rawLog(name);
    if (info && (sig == SIGSEGV || sig == SIGBUS)) {
        char hex[32]; int p = 0;
        unsigned long a = (unsigned long)info->si_addr;
        hex[p++] = ' '; hex[p++] = '@'; hex[p++] = '0'; hex[p++] = 'x';
        bool started = false;
        for (int shift = (int)sizeof(a) * 8 - 4; shift >= 0; shift -= 4) {
            int d = (int)((a >> shift) & 0xF);
            if (d || started || shift == 0) { started = true; hex[p++] = "0123456789abcdef"[d]; }
        }
        hex[p] = 0;
        rawLog(hex);
    }
    rawLog("\n");
    if (sig == SIGTERM || sig == SIGHUP) {
        // A requested stop is not a crash: do not report it as one on the next start.
        if (g_runMarkerPath[0]) ::unlink(g_runMarkerPath);
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

static QString limitText(rlim_t v)
{
    if (v == RLIM_INFINITY) return QString("unlimited");
    return QString("%1 MB").arg(static_cast<qulonglong>(v) / (1024 * 1024));
}

// A headless process may get a lower memory limit than the UI. Log the limits so an out-of-memory
// failure can be explained, and raise each soft limit to its hard limit where the system allows.
static void logAndRaiseMemoryLimits()
{
    struct Item { int res; const char *name; bool raise; };
    const Item items[] = {
        { RLIMIT_DATA,  "DATA",  true  },
#ifdef RLIMIT_AS
        { RLIMIT_AS,    "AS",    true  },
#endif
        { RLIMIT_STACK, "STACK", false }
    };
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        struct rlimit rl;
        if (::getrlimit(items[i].res, &rl) != 0) {
            qWarning() << "[Service] getrlimit" << items[i].name << "failed";
            continue;
        }
        QString line = QString("[Service] limit %1: soft %2, hard %3")
                           .arg(items[i].name).arg(limitText(rl.rlim_cur)).arg(limitText(rl.rlim_max));
        if (items[i].raise && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            if (::setrlimit(items[i].res, &rl) == 0) {
                line += QString(" -> raised soft to %1").arg(limitText(rl.rlim_cur));
            } else {
                line += " (could not raise)";
            }
        }
        qDebug() << line.toUtf8().constData();
    }
}

static void installSignalHandlers()
{
    // Writing to a socket the peer already closed raises SIGPIPE, whose default action kills
    // the process silently. A headless process does not get the UI toolkit's protection.
    ::signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    sa.sa_sigaction = fatalSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTERM, SIGHUP };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i) {
        ::sigaction(sigs[i], &sa, 0);
    }
}

// stderr of a headless process is not visible on device, so log to a file in the app's data
// folder (bbcord_service.log); it is truncated once it passes 512 KB.
static void serviceMessageHandler(QtMsgType type, const char *msg)
{
    if (!g_logFile) {
        QString path = QDir::homePath() + "/bbcord_service.log";
        g_logFile = new QFile(path);
        QFileInfo fi(path);
        QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text;
        mode |= (fi.exists() && fi.size() > 512 * 1024) ? QIODevice::Truncate : QIODevice::Append;
        g_logFile->open(mode);
        g_logFd = g_logFile->handle();
        g_logFile->write(QString("\n===== BBCordService started %1 =====\n")
                         .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")).toUtf8());
    }
    const char *lvl = (type == QtWarningMsg) ? "WARN" : (type == QtCriticalMsg) ? "ERROR"
                    : (type == QtFatalMsg) ? "FATAL" : "DEBUG";
    QString line = QString("[%1] [%2] %3\n")
                   .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz")).arg(lvl)
                   .arg(QString::fromUtf8(msg));
    if (g_logFile->isOpen()) { g_logFile->write(line.toUtf8()); g_logFile->flush(); }
    fprintf(stderr, "%s", line.toUtf8().constData());
    if (type == QtFatalMsg) abort();
}

// Two services would mean two gateway sessions. The lock is released by the OS when the
// process dies, so a crash never leaves it stuck.
static bool acquireSingleInstanceLock()
{
    QByteArray path = (QDir::homePath() + "/bbcord_service.lock").toLocal8Bit();
    int fd = ::open(path.constData(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) return true; // cannot lock: better to run than to not run
    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return ::fcntl(fd, F_SETLK, &fl) == 0; // fd stays open for the life of the process
}

Q_DECL_EXPORT int main(int argc, char **argv)
{
    qInstallMsgHandler(serviceMessageHandler);

    // Do NOT set an organization/application name: the client reads its settings (saved
    // token, custom API url, ...) through a default QSettings(), and the UI does not set
    // one either. Both processes must resolve to the same settings file.
    Application app(argc, argv);

    if (!acquireSingleInstanceLock()) {
        qDebug() << "[Service] another BBCord service instance is running - exiting";
        return 0;
    }

    installSignalHandlers();
    logAndRaiseMemoryLimits();

    // Run marker: created at start, removed on a clean exit. If it is still there at the next
    // start the previous run ended without a clean exit and without a FATAL line above, which
    // means it was killed (SIGKILL), typically by the system for using too much memory.
    QString runMarker = QDir::homePath() + "/bbcord_service.run";
    QByteArray runMarkerLocal = runMarker.toLocal8Bit();
    qstrncpy(g_runMarkerPath, runMarkerLocal.constData(), sizeof(g_runMarkerPath));
    if (QFile::exists(runMarker)) {
        qWarning() << "[Service] the previous run did NOT exit cleanly and logged no signal:"
                   << "it was killed (SIGKILL) - usually out of memory, or stopped by the system";
    }
    QFile marker(runMarker);
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        marker.write(QByteArray::number(static_cast<qlonglong>(::getpid())));
    }

    ServiceController controller;
    int rc = Application::exec();
    QFile::remove(runMarker);
    qDebug() << "[Service] exited cleanly, code" << rc;
    return rc;
}
