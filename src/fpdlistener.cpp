// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Droidian Project
//
// Authors:
// Bardia Moshiri <fakeshell@bardia.tech>

#include <unistd.h>
#include <batman/wlrdisplay.h>
#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QDBusInterface>
#include <QDBusReply>
#include "fpdinterface.h"

static void unlockSession(const QString &sessionId, int &exitStatus) {
    QDBusInterface interface("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", QDBusConnection::systemBus());
    if (interface.isValid()) {
        QDBusReply<void> reply = interface.call("UnlockSession", sessionId);
        if (reply.isValid()) {
            exitStatus = 0;
        } else {
            qWarning() << "DBus call failed: " << reply.error().message();
            exitStatus = 1;
        }
    } else {
        qWarning() << "DBus interface is invalid";
        exitStatus = 1;
    }
}

extern "C" int wlrdisplay_status() {
    int result = wlrdisplay(0, NULL);
    return result != 0;
}

extern "C" int delay(double seconds) {
    return usleep(seconds * 1000000);
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

void fpdunlocker(const QString& sessionId, int &exitStatus) {
    FPDInterface fpdInterface;
    QEventLoop loop;
    exitStatus = 0;

    QObject::connect(&fpdInterface, &FPDInterface::identified, [&](const QString &finger) {
        qDebug() << "Identified finger: " << finger;

        if (wlrdisplay_status() == 0) {
            sendFeedback("button-released");
            unlockSession(sessionId, exitStatus);
        } else {
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

extern "C" void fpdrunner(const char *sessionId) {
    int oldStat = -1;

    while (1) {
        int dispStat = wlrdisplay_status() == 0 ? 1 : 0;
        int exitStatus = 0;

        if (oldStat != dispStat) {
            oldStat = dispStat;

            if (dispStat == 1) {
                int unlocked = 0;
                while (unlocked == 0) {
                    fpdunlocker(QString(sessionId), exitStatus);
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

    char sessionId[64] = {0};
    FILE *fp = NULL;

    while (1) {
        fp = popen("loginctl list-sessions | awk '/tty7/{print $1}'", "r");
        if (fp == NULL) {
            qWarning() << "Failed to run command using popen.";
            delay(1);
            continue;
        }

        if (fgets(sessionId, sizeof(sessionId), fp) != NULL) {
            sessionId[strcspn(sessionId, "\r\n")] = 0;
            pclose(fp);
            break;
        } else {
            qWarning() << "Failed to read output";
            pclose(fp);
            delay(1);
        }
    }

    QThread *mainLoopThread = QThread::create([=](){ fpdrunner(sessionId); });
    QObject::connect(mainLoopThread, &QThread::finished, mainLoopThread, &QThread::deleteLater);

    mainLoopThread->start();

    return app.exec();
}
