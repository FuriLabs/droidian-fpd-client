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
    int result = wlrdisplay(0, NULL);
    return result != 0;
}

extern "C" int delay(double seconds) {
    return usleep(seconds * 1000000);
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
    QString sessionId;
    FILE *fp = NULL;

    while (true) {
        fp = popen("loginctl list-sessions | awk '/tty7/{print $1}'", "r");
        if (fp == NULL) {
            qWarning() << "Failed to run command using popen.";
            delay(1);
            continue;
        }

        char buffer[64];
        if (fgets(buffer, sizeof(buffer), fp) != NULL) {
            sessionId = QString(buffer).trimmed();
            pclose(fp);
            break;
        } else {
            qWarning() << "Failed to read output";
            pclose(fp);
            delay(1);
        }
    }

    return sessionId;
}

static void unlockSession(QString &sessionId, int &exitStatus) {
    QDBusInterface interface("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", QDBusConnection::systemBus());
    if (interface.isValid()) {
        QDBusReply<void> reply = interface.call("UnlockSession", sessionId);
        if (reply.isValid()) {
            exitStatus = 0;
        } else {
            qWarning() << "DBus call failed: " << reply.error().message();
            if (reply.error().message().contains("No session") && reply.error().message().contains("known")) {
                qWarning() << "Session ID invalid, re-probing for a new session ID.";
                sessionId = getSessionId();

                reply = interface.call("UnlockSession", sessionId);
                if (reply.isValid()) {
                    exitStatus = 0;
                } else {
                    qWarning() << "Retrying DBus call failed: " << reply.error().message();
                    exitStatus = 1;
                }
            } else {
                exitStatus = 1;
            }
        }
    } else {
        qWarning() << "DBus interface is invalid";
        exitStatus = 1;
    }
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

void fpdunlocker(QString &sessionId, int &exitStatus) {
    FPDInterface fpdInterface;
    QEventLoop loop;
    exitStatus = 0;

    QObject::connect(&fpdInterface, &FPDInterface::identified, [&](const QString &finger) {
        qDebug() << "Identified finger: " << finger;

        bool locked = isCollectionLocked();
        if (wlrdisplay_status() == 0 && !locked) {
            sendFeedback("button-released");
            unlockSession(sessionId, exitStatus);
        } else {
            if (locked) {
                qDebug() << "Keyring is still locked, discarding fingerprint request";
            }
            exitStatus = 0;
        }

        loop.quit();
    });

    QObject::connect(&fpdInterface, &FPDInterface::errorInfo, [&](const QString &info) {
        qDebug() << "Error info:" << info;

        if (info.contains("FINGER_NOT_RECOGNIZED") && wlrdisplay_status() == 0) {
            sendFeedback("window-close");
            exitStatus = 1;
        } else if (info.contains("ERROR_CANCELED") && wlrdisplay_status() != 0) {
            exitStatus = 1;
        } else {
            exitStatus = 0;
        }

        loop.quit();
    });

    qDebug() << "Waiting for finger identification...";

    fpdInterface.identify();

    loop.exec();
}

extern "C" void fpdrunner(const char *initialSessionId) {
    QString sessionId(initialSessionId);
    int oldStat = -1;

    while (true) {
        int dispStat = wlrdisplay_status() == 0 ? 1 : 0;
        int exitStatus = 0;

        if (oldStat != dispStat) {
            oldStat = dispStat;

            if (dispStat == 1) {
                int unlocked = 0;
                while (unlocked == 0) {
                    fpdunlocker(sessionId, exitStatus);
                    if (exitStatus == 0) {
                        unlocked = 1;
                    } else {
                        delay(0.5);
                    }
                }
            }
        } else {
            delay(0.2);
        }
    }
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    system("/usr/bin/binder-wait android.hardware.biometrics.fingerprint@2.1::IBiometricsFingerprint/default");

    QString sessionId = getSessionId();

    QThread *mainLoopThread = QThread::create([=]() { fpdrunner(sessionId.toUtf8().constData()); });
    QObject::connect(mainLoopThread, &QThread::finished, mainLoopThread, &QThread::deleteLater);

    mainLoopThread->start();

    return app.exec();
}
