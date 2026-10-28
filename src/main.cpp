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
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

using namespace bb;

static QFile *g_logFile = 0;

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

    ServiceController controller;
    return Application::exec();
}
