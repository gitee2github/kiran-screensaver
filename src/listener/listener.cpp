/**
 * Copyright (c) 2020 ~ 2021 KylinSec Co., Ltd.
 * kiran-screensaver is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 *
 * Author:     liuxinhao <liuxinhao@kylinos.com.cn>
 */

#include "listener.h"
#include "logind-session-monitor.h"

#include <qt5-log-i.h>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDateTime>
#include <QDebug>
#include <QString>

#define NOT_SUPPORTED_METHOD                                        \
    {                                                               \
        KLOG_DEBUG("isn't supported!,ignore method call");          \
        sendErrorReply(QDBusError::NotSupported, "not supported!"); \
    }

#define SESSION_NAME "org.gnome.SessionManager"
#define SESSION_PATH "/org/gnome/SessionManager"
#define SESSION_INTERFACE "org.gnome.SessionManager"

using namespace Kiran::ScreenSaver;

// 由于mate-session未提供抑制器类型枚举在头文件中
// 故从gsm-inhibitor.h拷贝该枚举定义
typedef enum
{
    GSM_INHIBITOR_FLAG_LOGOUT = 1 << 0,
    GSM_INHIBITOR_FLAG_SWITCH_USER = 1 << 1,
    GSM_INHIBITOR_FLAG_SUSPEND = 1 << 2,
    GSM_INHIBITOR_FLAG_IDLE = 1 << 3
} GsmInhibitorFlag;

namespace Kiran
{
namespace ScreenSaver
{
QDebug operator<<(QDebug debug, const Listener::InhibitedEntry &entry)
{
    QString sinceString = QDateTime::fromSecsSinceEpoch(entry.since).toString("yyyy-MM-dd HH:mm::ss");
    QString entryDesc = QString("application(%1),reason(%2),connection(%3),cookie(%4),foreign_cookie(%5),since(%6)")
                            .arg(entry.application)
                            .arg(entry.reason)
                            .arg(entry.connection)
                            .arg(entry.cookie)
                            .arg(entry.foreign_cookie)
                            .arg(sinceString);
    debug << "inhibitor " << entryDesc;
    return debug;
}
}  // namespace ScreenSaver
}  // namespace Kiran

Listener::Listener(QObject *parent) : QObject(parent)
{
}

Listener::~Listener()
{
}

bool Listener::init()
{
    // 初始化logind session监控类
    m_sessionMonitor = new LogindSessionMonitor(this);

    connect(m_sessionMonitor, &LogindSessionMonitor::Lock,
            this, &Listener::handleLogindSessionLock);
    connect(m_sessionMonitor, &LogindSessionMonitor::Unlock,
            this, &Listener::handleLogindSessionUnlock);

    m_sessionMonitor->init();

    // 监控Session DBus NameLost，移除抑制器
    if (!QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.DBus"),
                                               QStringLiteral("/org/freedesktop/DBus"),
                                               QStringLiteral("org.freedesktop.DBus"),
                                               "NameOwnerChanged",
                                               this,
                                               SLOT(handleDBusNameOwnerChanged(QString, QString, QString))))
    {
        KLOG_WARNING() << "can't connect to DBus Session Daemon NameOwnerChanged signal:" << QDBusConnection::sessionBus().lastError();
    }

    return true;
}

bool Listener::isInhibited()
{
    return !m_inhibitedEntries.empty();
}

bool Listener::setSessionIdle(bool idle)
{
    bool res;

    KLOG_DEBUG("set session %s", (idle ? "idle" : "not idle"));

    ///相同状态忽略
    if (m_sessionIdle == idle)
    {
        KLOG_DEBUG("trying to set idle state when already %s", (idle ? "idle" : "not idle"));
        return false;
    }

    if (idle)
    {
        if (isInhibited())
        {
            ///由非空闲->空闲被抑制，设置会话空闲失败
            KLOG_DEBUG("try to set session idle failed,inhibited!");
            foreach (auto entry, m_inhibitedEntries)
            {
                KLOG_DEBUG() << "\t" << entry;
            }
            return false;
        }
    }

    m_sessionIdle = idle;
    res = setActiveStatus(m_sessionIdle);
    if (res)
    {
        m_sessionIdleStart = m_sessionIdle ? time(nullptr) : 0;
    }
    else
    {
        m_sessionIdle = !idle;
    }

    return res;
}

void Listener::Cycle(){
    NOT_SUPPORTED_METHOD}

uint Listener::Throttle(const QString &application_name, const QString &reason)
{
    NOT_SUPPORTED_METHOD
    return 0;
}

void Listener::UnThrottle(uint cookie)
{
    NOT_SUPPORTED_METHOD
}

void Listener::ShowMessage(const QString &summary, const QString &body, const QString &icon)
{
    NOT_SUPPORTED_METHOD
}

void Listener::SetActive(bool value)
{
    setActiveStatus(value);
}

bool Listener::GetActive()
{
    return m_isActive;
}

uint Listener::GetActiveTime()
{
    //获取当前时间戳-激活时间的时间戳
    return m_activeStart;
}

QStringList Listener::GetInhibitors()
{
    QStringList ret;

    foreach (auto iter, m_inhibitedEntries)
    {
        QString since = QDateTime::fromSecsSinceEpoch(iter.since).toString(Qt::ISODate);
        QString inhibitorEntry = R"(Application="%1"; Since="%2"; Reason="%3";)";
        inhibitorEntry = inhibitorEntry.arg(iter.application, since, iter.reason);
        ret << inhibitorEntry;
    }

    return ret;
}

uint Listener::Inhibit(const QString &application_name, const QString &reason)
{
    QDBusConnection conn = connection();
    QDBusMessage msg = message();

    QString senderName = conn.interface()->serviceOwner(msg.service()).value();
    uint senderUid = conn.interface()->serviceUid(msg.service()).value();
    uint senderPid = conn.interface()->servicePid(msg.service()).value();

    InhibitedEntry entry;
    entry.connection = senderName;
    entry.application = application_name;
    entry.reason = reason;
    entry.cookie = generateCookie();
    entry.since = QDateTime::currentDateTime().toSecsSinceEpoch();

    addInhibitEntry(entry);
    return entry.cookie;
}

void Listener::UnInhibit(uint cookie)
{
    removeInhibitEntry(cookie);
}

void Listener::Lock()
{
    emit sigLock();
}

void Listener::Unlock()
{
    setActiveStatus(false);
}

void Listener::SimulateUserActivity()
{
    emit sigSimulateUserActivity();
}

quint64 Listener::generateCookie()
{
    time_t randomSeed = time(nullptr);
    qsrand(randomSeed);
    quint64 cookie = (quint64)qrand();
    return cookie;
}

void Listener::handleLogindSessionLock()
{
    emit sigLock();
}

void Listener::handleLogindSessionUnlock()
{
    setActiveStatus(false);
}

bool Listener::setActiveStatus(bool active)
{
    if (m_isActive == active)
    {
        KLOG_DEBUG("trying to set maskState state when already: %s", active ? "maskState" : "inactive");
        return false;
    }

    /// 发送信号，KSManager接收该信号(Qt::DirectConnection连接方式),
    /// 通过判断是否允许激活屏保或激活屏保成功
    /// 若激活失败，不对KSListener Active状态进行修改
    bool handled = false;
    emit sigActiveChanged(active, handled);
    if (!handled)
    {
        KLOG_DEBUG("faded changed signal not handled,update maskState(%s) failed!", active ? "true" : "false");
        return false;
    }

    m_isActive = active;
    if (m_sessionIdle != m_isActive)
    {
        m_sessionIdle = m_isActive;
    }

    m_activeStart = m_isActive ? time(nullptr) : 0;
    emit ActiveChanged(m_isActive);
    return true;
}

void Listener::handleDBusNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner)
{
    KLOG_DEBUG() << "name owner changed ->";
    KLOG_DEBUG() << "\tname:" << name;
    KLOG_DEBUG() << "\told owner:" << oldOwner;
    KLOG_DEBUG() << "\tnew owner:" << newOwner;

    // NameLost
    if (!oldOwner.isEmpty() && newOwner.isEmpty())
    {
        for (auto iter = m_inhibitedEntries.begin(); iter != m_inhibitedEntries.end();)
        {
            if (iter->connection == name)
            {
                auto removeInhibitItem = iter;
                iter++;
                // 抑制器的调用者退出，移除该调用者所创建的抑制器
                removeInhibitEntry(removeInhibitItem.key());
            }
            else
            {
                iter++;
            }
        }
    }
}

void Listener::addInhibitEntry(InhibitedEntry &entry)
{
    auto iter = m_inhibitedEntries.insert(entry.cookie, entry);
    addSessionInhibit(iter.value());

    KLOG_DEBUG() << "add inhibit entry ->" << iter.value();
}

void Listener::removeInhibitEntry(quint64 cookie)
{
    auto iter = m_inhibitedEntries.find(cookie);
    if (iter == m_inhibitedEntries.end())
    {
        KLOG_WARNING() << "can't find inhibit for cookie:" << cookie;
        return;
    }

    const InhibitedEntry entry = iter.value();
    m_inhibitedEntries.remove(cookie);
    removeSessionInhibit(entry);

    KLOG_DEBUG() << "remove inhibit entry ->" << entry;
}

void Listener::addSessionInhibit(Listener::InhibitedEntry &entry)
{
    QDBusMessage sessionInhibitMethod = QDBusMessage::createMethodCall(SESSION_NAME,
                                                                       SESSION_PATH,
                                                                       SESSION_INTERFACE,
                                                                       "Inhibit");
    QList<QVariant> inhibitArgs;
    inhibitArgs << entry.application << (quint32)0 << entry.reason << (quint32)GSM_INHIBITOR_FLAG_IDLE;
    sessionInhibitMethod.setArguments(inhibitArgs);

    QDBusReply<quint32> reply = QDBusConnection::sessionBus().call(sessionInhibitMethod);

    if (!reply.isValid())
    {
        KLOG_WARNING() << "can't add session inhibitor," << reply.error();
        entry.foreign_cookie = 0;
    }
    else
    {
        KLOG_DEBUG() << reply.value();
        entry.foreign_cookie = reply.value();
    }
}

void Listener::removeSessionInhibit(const Listener::InhibitedEntry &entry)
{
    QDBusMessage uninhibitMethod = QDBusMessage::createMethodCall(SESSION_NAME,
                                                                  SESSION_PATH,
                                                                  SESSION_INTERFACE,
                                                                  "Uninhibit");
    uninhibitMethod.setArguments(QList<QVariant>() << entry.foreign_cookie);

    QDBusReply<void> reply = QDBusConnection::sessionBus().call(uninhibitMethod);

    if (!reply.isValid())
    {
        KLOG_WARNING() << "can't uninhibit," << reply.error();
    }
}