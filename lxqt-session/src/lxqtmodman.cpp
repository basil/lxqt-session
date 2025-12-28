/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * LXQt - a lightweight, Qt based, desktop toolset
 * https://lxqt.org/
 *
 * Copyright: 2010-2011 LXQt team
 * Authors:
 *   Petr Vanek <petr@scribus.info>
 *   Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 *
 * This program or library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 *
 * END_COMMON_COPYRIGHT_HEADER */

#include "lxqtmodman.h"
#include "meta_types.h"
#include <LXQt/Globals>
#include <LXQt/Settings>
#include <XdgAutoStart>
#include <XdgDirs>
#include <unistd.h>

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QRegularExpression>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QFileSystemWatcher>
#include <QDateTime>
#include <QPointer>
#include "wmselectdialog.h"
#include "windowmanager.h"
#include <wordexp.h>
#include "log.h"

#include <KWindowSystem>
#include <NETWM>

using namespace LXQt;

/**
 * @brief the constructor, needs a valid modules.conf
 */
LXQtModuleManager::LXQtModuleManager(QObject* parent)
    : QObject(parent),
      mWmProcess(new QProcess(this)),
      mThemeWatcher(new QFileSystemWatcher(this))
{
    connect(mThemeWatcher, &QFileSystemWatcher::directoryChanged, this, &LXQtModuleManager::themeFolderChanged);
    connect(LXQt::Settings::globalSettings(), &LXQt::GlobalSettings::lxqtThemeChanged, this, &LXQtModuleManager::themeChanged);

    mProcReaper.start();
}

void LXQtModuleManager::setWindowManager(const QString & windowManager)
{
    mWindowManager = windowManager;
}

void LXQtModuleManager::startup(LXQt::Settings& s)
{
    // The lxqt-confupdate can update the settings of the WM, so run it first.
    startConfUpdate();

    // Start window manager
    if (QGuiApplication::platformName() == QStringLiteral("xcb"))
        startWm(&s);

    startAutostartApps();

    QStringList paths;
    paths << XdgDirs::dataHome(false);
    paths << XdgDirs::dataDirs();

    for(const QString &path : std::as_const(paths))
    {
        QFileInfo fi(QString::fromLatin1("%1/lxqt/themes").arg(path));
        if (fi.exists())
            mThemeWatcher->addPath(fi.absoluteFilePath());
    }

    themeChanged();
}

void LXQtModuleManager::startAutostartApps()
{
    // XDG autostart
    const XdgDesktopFileList fileList = XdgAutoStart::desktopFileList();
    QList<const XdgDesktopFile*> trayApps;
    bool isWayland((QGuiApplication::platformName() == QLatin1String("wayland")));
    for (XdgDesktopFileList::const_iterator i = fileList.constBegin(); i != fileList.constEnd(); ++i)
    {
        if (i->value(QSL("X-LXQt-Autostart-disabled"), false).toBool()
            || (isWayland && i->value(QSL("X-LXQt-X11-Only"), false).toBool()))
        {
            continue;
        }
        if (i->value(QSL("X-LXQt-Need-Tray"), false).toBool())
            trayApps.append(&(*i));
        else
        {
            startProcess(*i);
            qCDebug(SESSION) << "start" << i->fileName();
        }
    }

    if (!trayApps.isEmpty())
    {
        QPointer<QTimer> t{new QTimer};
        auto starter = [this, fileList, trayApps, t] (const bool forceStart = false) {
            if (!t)
                return;

            if (QSystemTrayIcon::isSystemTrayAvailable())
                qCDebug(SESSION) << "System Tray started";
            else if (forceStart)
                qCWarning(SESSION) << "System Tray haven't stared yet! Starting tray apps anyway...";
            else
                return;

            QScopedPointer<QTimer> releaser{t};
            for (const XdgDesktopFile* const f : std::as_const(trayApps))
            {
                qCDebug(SESSION) << "start tray app" << f->fileName();
                startProcess(*f);
            }
            t->stop();
        };
        connect(t, &QTimer::timeout, this, starter);
        t->setSingleShot(false);
        t->start(1000);
        // try to start instantly, no need to wait the 1st sec
        starter();
        // start the apps anyway after a timeout
        QTimer::singleShot(15 * 1000, this, std::bind(starter, true));
    }
}

void LXQtModuleManager::themeFolderChanged(const QString& /*path*/)
{
    QString newTheme;
    if (!QFileInfo::exists(mCurrentThemePath))
    {
        const QList<LXQtTheme> &allThemes = lxqtTheme.allThemes();
        if (!allThemes.isEmpty())
            newTheme = allThemes[0].name();
        else
            return;
    }
    else
        newTheme = lxqtTheme.currentTheme().name();

    LXQt::Settings settings(QSL("lxqt"));
    if (newTheme == settings.value(QL1S("theme")))
    {
        // force the same theme to be updated
        settings.setValue(QSL("__theme_updated__"), QDateTime::currentMSecsSinceEpoch());
    }
    else
        settings.setValue(QL1S("theme"), newTheme);

    sync();
}

void LXQtModuleManager::themeChanged()
{
    if (!mCurrentThemePath.isEmpty())
        mThemeWatcher->removePath(mCurrentThemePath);

    if (lxqtTheme.currentTheme().isValid())
    {
        mCurrentThemePath = lxqtTheme.currentTheme().path();
        mThemeWatcher->addPath(mCurrentThemePath);
    }
}

void LXQtModuleManager::startWm(LXQt::Settings *settings)
{
    // if the WM is active do not run WM.
    // all window managers must set their name according to the spec
    if (auto x11NativeInterface = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
        if (!QString::fromUtf8(NETRootInfo(x11NativeInterface->connection(), NET::SupportingWMCheck).wmName()).isEmpty())
        {
            return;
        }
    }

    if (mWindowManager.isEmpty())
    {
        mWindowManager = settings->value(QL1S("window_manager")).toString();
    }

    // If previuos WM was removed, we show dialog.
    if (mWindowManager.isEmpty() || ! findProgram(mWindowManager.split(QL1C(' '))[0]))
    {
        mWindowManager = showWmSelectDialog();
        settings->setValue(QL1S("window_manager"), mWindowManager);
        settings->sync();
    }

    mWmProcess->start(mWindowManager, QStringList());

    // other autostart apps will be handled after the WM becomes available

    // Wait until the WM loads
    QEventLoop waitLoop;
    auto checker = [&waitLoop] {
        // all window managers must set their name according to the spec
        if (auto x11NativeInterface = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
            if (!QString::fromUtf8(NETRootInfo(x11NativeInterface->connection(), NET::SupportingWMCheck).wmName()).isEmpty())
            {
                qCDebug(SESSION) << "Window Manager started";
                waitLoop.exit();
            }
        }
    };
    QTimer t;
    connect(&t, &QTimer::timeout, this, checker);
    t.setSingleShot(false);
    t.start(500);
    // add a timeout to avoid infinite blocking if a WM fail to execute.
    QTimer::singleShot(30 * 1000, &waitLoop, &QEventLoop::quit);
    // Note: the timer object is destructed sooner than the wait loop upon finishing this method
    waitLoop.exec();
    // FIXME: blocking is a bad idea. We need to start as many apps as possible and
    //         only wait for the start of WM when it's absolutely needed.
    //         Maybe we can add a X-Wait-WM=true key in the desktop entry file?
}

namespace {

// Valid chars for the "unit name prefix": ASCII letters, digits, ':', '-', '_', '.', '\\'
static bool isValidUnitChar(QChar c)
{
    const ushort u = c.unicode();
    if ((u >= 'a' && u <= 'z') ||
        (u >= 'A' && u <= 'Z') ||
        (u >= '0' && u <= '9')) {
        return true;
    }

    return c == QLatin1Char(':') ||
           c == QLatin1Char('-') ||
           c == QLatin1Char('_') ||
           c == QLatin1Char('.') ||
           c == QLatin1Char('\\');
}

static QString systemdEscape(const QString &input)
{
    QString out = input;
    for (QChar &ch : out) {
        if (!isValidUnitChar(ch)) {
            ch = QLatin1Char('_');
        }
    }
    return out;
}

// Stable name for LXQt modules: lxqt-module-<desktop-id>.service
static QString moduleUnitName(const XdgDesktopFile &file)
{
    // Prefer desktop file ID if available; otherwise use basename
    QString id = XdgDesktopFile::id(file.fileName(), /*checkFileExists*/ false);
    if (id.isEmpty())
        id = QFileInfo(file.fileName()).completeBaseName();

    QString prefix = QStringLiteral("lxqt-module-%1").arg(id);
    prefix = systemdEscape(prefix);

    // Keep margin for ".service" suffix, systemd's 255-char limit
    constexpr qsizetype maxPrefixLen = 240;
    if (prefix.size() > maxPrefixLen)
        prefix.truncate(maxPrefixLen);

    return prefix + QStringLiteral(".service");
}

// Start a transient service unit for a module under session.slice.
// Returns true and fills unitPathOut on success.
static bool startModuleTransientUnit(const QString &unitName,
                                     const QString &program,
                                     const QStringList &arguments,
                                     const QString &workingDirectory,
                                     const XdgDesktopFile &file,
                                     QDBusObjectPath &unitPathOut)
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCWarning(SESSION) << "Cannot connect to D-Bus session bus to start module" << unitName;
        return false;
    }

    QDBusConnectionInterface *iface = bus.interface();
    if (!iface || !iface->isServiceRegistered(QStringLiteral("org.freedesktop.systemd1"))) {
        qCWarning(SESSION) << "org.freedesktop.systemd1 is not available on the session bus";
        return false;
    }

    SystemdDBusPropertyList properties;

    // Description from Name / GenericName
    const QString name = file.localizedValue(QStringLiteral("Name")).toString();
    const QString genericName = file.localizedValue(QStringLiteral("GenericName")).toString();
    QString description;
    if (name.isEmpty())
        description = genericName;
    else if (genericName.isEmpty())
        description = name;
    else
        description = name + QStringLiteral(" - ") + genericName;

    if (description.isEmpty())
        description = QFileInfo(program).fileName();

    properties.append({QStringLiteral("Description"), description});
    properties.append({QStringLiteral("Slice"), QStringLiteral("session.slice")});
    properties.append({QStringLiteral("CollectMode"), QStringLiteral("inactive-or-failed")});
    properties.append({QStringLiteral("Type"), QStringLiteral("exec")});
    properties.append({QStringLiteral("ExitType"), QStringLiteral("cgroup")});

    if (!workingDirectory.isEmpty())
        properties.append({QStringLiteral("WorkingDirectory"), workingDirectory});

    // Delegate crash handling to systemd:
    // - Restart on failure
    // - Apply a modest start limit (roughly 5 crashes per 60 seconds)
    properties.append({QStringLiteral("Restart"), QStringLiteral("on-failure")});
    properties.append({QStringLiteral("StartLimitIntervalUSec"), quint64(60ull * 1000000ull)}); // 60s
    properties.append({QStringLiteral("StartLimitBurst"), quint32(5)});

    SystemdDBusExecCommand execCmd;
    execCmd.path = program;
    execCmd.args = QStringList{program} + arguments;
    execCmd.ignoreFailure = false;

    SystemdDBusExecCommandList commands;
    commands.append(execCmd);
    properties.append({
        QStringLiteral("ExecStart"),
        QVariant::fromValue(commands)
    });

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.systemd1"),
        QStringLiteral("/org/freedesktop/systemd1"),
        QStringLiteral("org.freedesktop.systemd1.Manager"),
        QStringLiteral("StartTransientUnit")
    );

    msg << unitName << QStringLiteral("fail");
    msg << QVariant::fromValue(properties);

    SystemdDBusAuxUnitList auxUnits;
    msg << QVariant::fromValue(auxUnits);

    QDBusReply<QDBusObjectPath> reply = bus.call(msg);
    if (!reply.isValid()) {
        qCWarning(SESSION) << "Failed to StartTransientUnit for" << unitName
                           << ":" << reply.error().message();
        return false;
    }

    unitPathOut = reply.value();
    return true;
}

}


void LXQtModuleManager::startProcess(const XdgDesktopFile& file)
{
    if (!file.value(QL1S("X-LXQt-Module"), false).toBool())
    {
        file.startDetached(QStringList(), QStringLiteral("app.slice"));
        return;
    }
    QStringList args = file.expandExecString();
    if (args.isEmpty())
    {
        qCWarning(SESSION) << "Wrong desktop file" << file.fileName();
        return;
    }

    QString program = args.takeFirst();
    QString workingDir = file.value(QL1S("Path")).toString();
    if (!workingDir.isEmpty() && !QDir(workingDir).exists())
        workingDir.clear();

    const QString unitName = moduleUnitName(file);
    QDBusObjectPath unitPath;

    if (!startModuleTransientUnit(unitName, program, args, workingDir, file, unitPath)) {
        qCWarning(SESSION) << "Failed to start LXQt module as transient unit:" << file.fileName();
        return;
    }

    auto *module = new LXQtModule(file, unitName, unitPath, this);
    connect(module, &LXQtModule::moduleStateChanged,
            this, &LXQtModuleManager::moduleStateChanged);

    const QString name = QFileInfo(file.fileName()).fileName();

    // If we somehow already have a module object for this name, drop it
    if (auto old = mNameMap.value(name, nullptr)) {
        disconnect(old, nullptr, this, nullptr);
        old->deleteLater();
    }

    mNameMap[name] = module;

    // When the systemd unit fails, delegate restart to systemd via restartModules()
    connect(module, &LXQtModule::failed,
            this, &LXQtModuleManager::restartModules);
}

void LXQtModuleManager::startProcess(const QString& name)
{
    if (!mNameMap.contains(name))
    {
        const auto files = XdgAutoStart::desktopFileList(false);
        for (const XdgDesktopFile& file : files)
        {
            if (QFileInfo(file.fileName()).fileName() == name)
            {
                startProcess(file);
                return;
            }
        }
    }
}

void LXQtModuleManager::stopProcess(const QString &name)
{
    auto it = mNameMap.constFind(name);
    if (it == mNameMap.constEnd())
        return;

    LXQtModule *module = it.value();
    if (!module)
        return;

    module->stop();
}

void LXQtModuleManager::execDesktopFile(const QString& name)
{
    XdgDesktopFile xdg;
    if (!xdg.load(name))
    {
        qCWarning(SESSION) << "Desktop file" << name << "is not valid";
        return;
    }
    if (!xdg.isSuitable())
    {
        qCWarning(SESSION) << "Desktop file" << name << "is not applicable";
        return;
    }
    xdg.startDetached(QStringList(), QStringLiteral("app.slice"));
}

QStringList LXQtModuleManager::listModules() const
{
    return QStringList(mNameMap.keys());
}

void LXQtModuleManager::startConfUpdate()
{
    XdgDesktopFile desktop(XdgDesktopFile::ApplicationType, QSL(":lxqt-confupdate"), QSL("lxqt-confupdate --watch"));
    desktop.setValue(QSL("Name"), QSL("LXQt config updater"));
    desktop.setValue(QL1S("X-LXQt-Module"), true);
    startProcess(desktop);
}

void LXQtModuleManager::restartModules()
{
    LXQtModule *module = qobject_cast<LXQtModule*>(sender());
    if (!module) {
        qCWarning(SESSION) << "restartModules called without a valid LXQtModule sender";
        return;
    }

    // This will:
    //  - reset the failed state and counters (ResetFailedUnit)
    //  - ask systemd to restart the service (RestartUnit)
    module->restart();
}


LXQtModuleManager::~LXQtModuleManager()
{
    // We disconnect the finished signal before deleting the process. We do
    // this to prevent a crash that results from a state change signal being
    // emitted while deleting a crashing module.
    // If the module is still connect restartModules will be called with a
    // invalid sender.

    ModulesMapIterator i(mNameMap);
    while (i.hasNext())
    {
        i.next();
        LXQtModule *m = i.value();
        if (!m)
            continue;

        disconnect(m, nullptr, this, nullptr);
        // Do not try to stop modules here; logout() already handles that.
        delete m;
        mNameMap[i.key()] = nullptr;
    }

    delete mWmProcess;
}

/**
* @brief this logs us out by terminating our session
**/
void LXQtModuleManager::logout(bool doExit)
{
    // modules
    ModulesMapIterator i(mNameMap);
    while (i.hasNext())
    {
        i.next();
        qCDebug(SESSION) << "Module logout" << i.key();
        LXQtModule *m = i.value();
        if (m)
            m->stop();
    }

    // terminate all possible children except WM
    mProcReaper.stop({mWmProcess->processId()});

    mWmProcess->terminate();
    if (mWmProcess->state() != QProcess::NotRunning && !mWmProcess->waitForFinished(2000))
    {
        qCWarning(SESSION) << "Window Manager won't terminate ... killing.";
        mWmProcess->kill();
    }

    if (doExit)
        QCoreApplication::exit(0);
}

QString LXQtModuleManager::showWmSelectDialog()
{
    WindowManagerList availableWM = getWindowManagerList(true);
    if (availableWM.count() == 1)
        return availableWM.at(0).command;

    WmSelectDialog dlg(availableWM);
    dlg.exec();
    return dlg.windowManager();
}

void lxqt_setenv(const char *env, const QByteArray &value)
{
    wordexp_t p;

    switch (wordexp(value.constData(), &p, 0))
    {
    case 0:
        if (p.we_wordc == 1)
        {
            qCDebug(SESSION) << "Environment variable" << env << "=" << p.we_wordv[0];
            qputenv(env, p.we_wordv[0]);
            wordfree(&p);
            return;
        }
        wordfree(&p);
        break;
    case WRDE_NOSPACE:
        // wordfree needed: https://www.gnu.org/software/libc/manual/html_node/Wordexp-Example.html
        wordfree(&p);
        break;
    default:
        break;
    }
    qCWarning(SESSION) << "Error expanding environment variable" << env << "=" << value;
    qputenv(env, value);
}

void lxqt_setenv_prepend(const char *env, const QByteArray &value, const QByteArray &separator)
{
    QByteArray orig(qgetenv(env));
    orig = orig.prepend(separator);
    orig = orig.prepend(value);
    qCDebug(SESSION) << "Setting special" << env << " variable:" << orig;
    lxqt_setenv(env, orig);
}


LXQtModule::LXQtModule(const XdgDesktopFile &file,
                       const QString &unitName,
                       const QDBusObjectPath &unitPath,
                       QObject *parent)
    : QObject(parent)
    , mFile(file)
    , mFileName(QFileInfo(file.fileName()).fileName())
    , mUnitName(unitName)
    , mUnitPath(unitPath)
    , mState(State::Unknown)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCWarning(SESSION) << "No session bus; cannot monitor module" << mFileName;
        return;
    }

    // Connect to PropertiesChanged on the unit
    const bool ok = bus.connect(
        QStringLiteral("org.freedesktop.systemd1"),
        mUnitPath.path(),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("PropertiesChanged"),
        this,
        SLOT(onUnitPropertiesChanged(QString,QVariantMap,QStringList))
    );

    if (!ok) {
        qCWarning(SESSION) << "Failed to connect to PropertiesChanged for" << mUnitName;
    }

    // Fetch initial ActiveState
    QDBusInterface propsIface(
        QStringLiteral("org.freedesktop.systemd1"),
        mUnitPath.path(),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        bus
    );

    if (!propsIface.isValid()) {
        qCWarning(SESSION) << "Cannot create Properties interface for" << mUnitName;
        return;
    }

    QDBusReply<QVariant> reply = propsIface.call(
        QStringLiteral("Get"),
        QStringLiteral("org.freedesktop.systemd1.Unit"),
        QStringLiteral("ActiveState")
    );

    if (reply.isValid()) {
        mState = stateFromActiveState(reply.value().toString());
        emit stateChanged(mState);
        emit moduleStateChanged(mFileName, mState == State::Active);
    }
}

LXQtModule::State LXQtModule::stateFromActiveState(const QString &activeState)
{
    if (activeState == QLatin1String("active"))
        return State::Active;
    if (activeState == QLatin1String("activating"))
        return State::Starting;
    if (activeState == QLatin1String("inactive") ||
        activeState == QLatin1String("deactivating"))
        return State::Inactive;
    if (activeState == QLatin1String("failed"))
        return State::Failed;

    return State::Unknown;
}

void LXQtModule::onUnitPropertiesChanged(const QString &interface,
                                         const QVariantMap &changed,
                                         const QStringList &invalidated)
{
    Q_UNUSED(invalidated);

    if (interface != QStringLiteral("org.freedesktop.systemd1.Unit"))
        return;

    auto it = changed.find(QStringLiteral("ActiveState"));
    if (it == changed.end())
        return;

    const QString activeState = it->toString();
    const State newState = stateFromActiveState(activeState);

    if (newState == mState)
        return;

    const bool wasRunning = (mState == State::Active);
    const bool isRunning  = (newState == State::Active);

    mState = newState;
    emit stateChanged(mState);

    if (wasRunning != isRunning)
        emit moduleStateChanged(mFileName, isRunning);

    if (mState == State::Failed)
        emit failed();
}

void LXQtModule::stop()
{
    QDBusInterface manager(
        QStringLiteral("org.freedesktop.systemd1"),
        QStringLiteral("/org/freedesktop/systemd1"),
        QStringLiteral("org.freedesktop.systemd1.Manager"),
        QDBusConnection::sessionBus()
    );

    if (!manager.isValid()) {
        qCWarning(SESSION) << "Cannot create systemd Manager interface to stop" << mUnitName;
        return;
    }

    QDBusReply<QDBusObjectPath> reply =
        manager.call(QStringLiteral("StopUnit"), mUnitName, QStringLiteral("replace"));
    if (!reply.isValid()) {
        qCWarning(SESSION) << "Failed to StopUnit" << mUnitName
                           << ":" << reply.error().message();
    }
}

void LXQtModule::restart()
{
    QDBusInterface manager(
        QStringLiteral("org.freedesktop.systemd1"),
        QStringLiteral("/org/freedesktop/systemd1"),
        QStringLiteral("org.freedesktop.systemd1.Manager"),
        QDBusConnection::sessionBus()
    );

    if (!manager.isValid()) {
        qCWarning(SESSION) << "Cannot create systemd Manager interface to restart" << mUnitName;
        return;
    }

    // Delegate crash handling to systemd: reset failed counters then restart.
    QDBusReply<void> resetReply =
        manager.call(QStringLiteral("ResetFailedUnit"), mUnitName);
    if (!resetReply.isValid()) {
        qCWarning(SESSION) << "ResetFailedUnit failed for" << mUnitName
                           << ":" << resetReply.error().message();
        // continue and try restart anyway
    }

    QDBusReply<QDBusObjectPath> reply =
        manager.call(QStringLiteral("RestartUnit"), mUnitName, QStringLiteral("replace"));
    if (!reply.isValid()) {
        qCWarning(SESSION) << "Failed to RestartUnit" << mUnitName
                           << ":" << reply.error().message();
    }
}
