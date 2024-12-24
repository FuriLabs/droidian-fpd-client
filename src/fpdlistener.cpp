// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidian Project
//
// Authors:
// Bardia Moshiri <fakeshell@bardia.tech>
// Jesús Higueras <jesus@dabbleam.com>

#include <stdio.h>
#include <unistd.h>
#include <batman/wlrdisplay.h>
#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QDBusInterface>
#include <QDBusReply>
#include "fpdinterface.h"

extern "C" int wlrdisplay_status() {
    int result = get_wlroots_screen_status();
    return result != 0;
}

static bool isCollectionLocked() {
    const QString service = "org.freedesktop.secrets";
    const QString path = "/org/freedesktop/secrets/collection/login";
    const QString interface = "org.freedesktop.Secret.Collection";
    const QString property = "Locked";

    QDBusInterface dbusInterface(service, path, "org.freedesktop.DBus.Properties", QDBusConnection::sessionBus());
    if (!dbusInterface.isValid()) {
        qWarning() << "D-Bus interface is not valid!";
        return false;
    }

    QDBusReply<QVariant> reply = dbusInterface.call("Get", interface, property);
    if (reply.isValid()) {
        bool isLocked = reply.value().toBool();
        return isLocked;
    } else {
        qWarning() << "Failed to get property:" << reply.error().message();
        return false;
    }
}

static QString getSessionId() {
    while (true) {
        QDBusInterface manager("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", QDBusConnection::systemBus());

        if (!manager.isValid()) {
            qDebug() << "Login1 service not available, retrying in 1 second...";
            QThread::sleep(1);
            continue;
        }

        QDBusMessage reply = manager.call("ListSessionsEx");
        if (reply.type() == QDBusMessage::ErrorMessage) {
            qDebug() << "D-Bus call failed:" << reply.errorMessage() << ", retrying in 1 second...";
            QThread::sleep(1);
            continue;
        }

        try {
            const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();
            arg.beginArray();

            while (!arg.atEnd()) {
                arg.beginStructure();
                QString sessionId;
                uint32_t uid;
                QString seat, displayName;
                uint32_t vtnr;
                QString name, tty;
                bool remote;
                quint64 timestamp;
                QDBusObjectPath objPath;

                arg >> sessionId >> uid >> seat >> displayName >> vtnr >> name >> tty >> remote >> timestamp >> objPath;
                arg.endStructure();

                if (tty == "tty7") {
                    qDebug() << "Found tty7 session:" << sessionId;
                    return sessionId;
                }
            }
            arg.endArray();
        } catch (const std::exception& e) {
            qDebug() << "Error parsing D-Bus response:" << e.what() << ", retrying in 1 second...";
            QThread::sleep(1);
            continue;
        }

        qDebug() << "No tty7 session found, retrying in 1 second...";
        QThread::sleep(1);
    }
}

static bool isScreenLocked(QString &sessionId) {
    QString sessionPath = QString("/org/freedesktop/login1/session/%1").arg(sessionId);
    QDBusInterface props("org.freedesktop.login1", sessionPath, "org.freedesktop.DBus.Properties", QDBusConnection::systemBus());

    QDBusReply<QVariant> reply = props.call("Get", "org.freedesktop.login1.Session",  "LockedHint");
    if (reply.isValid()) {
        return reply.value().toBool();
    } else {
        sessionId = getSessionId();
        qDebug() << "Got a new session id:" << sessionId;
        sessionPath = QString("/org/freedesktop/login1/session/%1").arg(sessionId);

        QDBusInterface props("org.freedesktop.login1", sessionPath, "org.freedesktop.DBus.Properties", QDBusConnection::systemBus());
        QDBusReply<QVariant> reply = props.call("Get", "org.freedesktop.login1.Session", "LockedHint");

        if (reply.isValid()) {
            return reply.value().toBool();
        } else {
            qDebug() << "The newly acquired session id is not valid:" << sessionId;
        }
    }

    return false;
}

static int unlockSession(QString &sessionId) {
    QDBusInterface interface("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", QDBusConnection::systemBus());
    if (interface.isValid()) {
        QDBusReply<void> reply = interface.call("UnlockSession", sessionId);
        if (reply.isValid()) {
            return 0;
        } else {
            qWarning() << "DBus call failed: " << reply.error().message();
            if (reply.error().message().contains("No session") && reply.error().message().contains("known")) {
                qWarning() << "Session ID invalid, re-probing for a new session ID.";
                sessionId = getSessionId();

                reply = interface.call("UnlockSession", sessionId);
                if (reply.isValid()) {
                    return 0;
                } else {
                    qWarning() << "Retrying DBus call failed: " << reply.error().message();
                    return 1;
                }
            } else {
                qWarning() << "UnlockSession reply is not valid.";
                return 1;
            }
        }
    } else {
        qWarning() << "DBus interface is invalid";
        return 0;
    }

    return 0;
}

void sendFeedback(const QString &event) {
    QDBusInterface feedbackdInterface("org.sigxcpu.Feedback", "/org/sigxcpu/Feedback", "org.sigxcpu.Feedback", QDBusConnection::sessionBus());
    if (feedbackdInterface.isValid()) {
        QDBusReply<void> reply = feedbackdInterface.call("TriggerFeedback", "fpdlistener", event, QVariantMap(), -1);
        if (!reply.isValid()) {
            qWarning() << "DBus call failed: " << reply.error().message();
        }
    } else {
        qWarning() << "DBus interface for feedbackd is invalid";
    }
}

class FpdUnlocker : public QObject {
    Q_OBJECT
public:
    FpdUnlocker(QString sessionId, QObject *parent = nullptr)
        : QObject(parent), m_sessionId(sessionId), m_inProgress(false) {}

    void startUnlockAttempt() {
        if (m_inProgress) {
            qDebug() << "Unlock attempt already in progress, skipping...";
            return;
        }
        m_inProgress = true;
        attemptUnlock();
    }

private:
    void attemptUnlock() {
        FPDInterface *fpdInterface = new FPDInterface(this);
        QEventLoop *loop = new QEventLoop(this);

        QObject::connect(fpdInterface, &FPDInterface::identified, this, [=](const QString &finger) {
            qDebug() << "Identified finger:" << finger;

            bool keyring_locked = isCollectionLocked();
            bool screen_locked = isScreenLocked(m_sessionId);
            if (wlrdisplay_status() == 0 && screen_locked && !keyring_locked) {
                sendFeedback("button-released");
                unlockSession(m_sessionId);
                loop->quit();
            } else {
                if (keyring_locked)
                    qDebug() << "Keyring is still locked, discarding fingerprint request";
                if (!screen_locked)
                    qDebug() << "Screen is unlocked, discarding fingerprint request";
                if (wlrdisplay_status() != 0)
                    qDebug() << "Display is off, discarding fingerprint request";
                loop->quit();
            }
        });

        QObject::connect(fpdInterface, &FPDInterface::errorInfo, this, [=](const QString &info) {
            qDebug() << "Error info:" << info;
            bool screen_locked = isScreenLocked(m_sessionId);

            // Handle errors:
            // - If finger not recognized and display is on and screen is still locked: try again (no quit)
            // - If canceled and display is off: end (quit)
            // - If canceled and display is on and screen is still locked (timeout): try again (no quit)
            // - Anything else: end (quit)
            if (info.contains("FINGER_NOT_RECOGNIZED") && wlrdisplay_status() == 0 && screen_locked) {
                sendFeedback("window-close");
                qDebug() << "Finger not recognized. Asking again...";
                fpdInterface->identify();
            } else if (info.contains("ERROR_CANCELED") && wlrdisplay_status() != 0) {
                // Display is off, no point in continuing
                qDebug() << "Operation canceled and display is off. Stopping attempts.";
                loop->quit();
            } else if (info.contains("ERROR_CANCELED") && wlrdisplay_status() == 0 && screen_locked) {
                // Display is on, means it timed out, let's try again
                qDebug() << "Fingerprint timed out. Waiting for finger identification again...";
                fpdInterface->identify();
            } else {
                if (!screen_locked)
                    qDebug() << "Operation canceled and display is unlocked. Stopping attempts.";
                loop->quit();
            }
        });

        qDebug() << "Waiting for finger identification...";
        fpdInterface->identify();

        loop->exec();

        m_inProgress = false;
        fpdInterface->deleteLater();
        loop->deleteLater();
    }

    QString m_sessionId;
    bool m_inProgress;
};

class IdleHintListener : public QObject {
    Q_OBJECT
public:
    IdleHintListener(const QString &sessionId, QObject *parent = nullptr)
        : QObject(parent), m_sessionId(sessionId), m_unlocker(new FpdUnlocker(sessionId, this)) {}

public slots:
    void onPropertiesChanged(const QString &interface_name, const QVariantMap &changed_properties, const QStringList &invalidated_properties) {
        Q_UNUSED(invalidated_properties)
        if (interface_name == "org.freedesktop.login1.Session" && changed_properties.contains("IdleHint")) {
            bool idleHint = changed_properties.value("IdleHint").toBool();
            qDebug() << "IdleHint changed:" << idleHint;

            if (!idleHint)
                m_unlocker->startUnlockAttempt();
        }
    }

private:
    QString m_sessionId;
    FpdUnlocker *m_unlocker;
};

void listenForIdleHint(const QString &sessionId) {
    QString sessionPath = QString("/org/freedesktop/login1/session/%1").arg(sessionId);

    IdleHintListener *listener = new IdleHintListener(sessionId);

    bool connected = false;
    while (!connected) {
        connected = QDBusConnection::systemBus().connect(
            "org.freedesktop.login1",
            sessionPath,
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            "sa{sv}as",
            listener,
            SLOT(onPropertiesChanged(QString,QVariantMap,QStringList))
        );

        if (!connected) {
            qWarning() << "Failed to connect to PropertiesChanged signal for IdleHint. Retrying in 1 second...";
            QThread::sleep(1);
        }
    }

    qDebug() << "Listening for IdleHint changes...";
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    system("/usr/bin/binder-wait android.hardware.biometrics.fingerprint@2.1::IBiometricsFingerprint/default");

    QString sessionId = getSessionId();

    listenForIdleHint(sessionId);

    return app.exec();
}

#include "fpdlistener.moc"
