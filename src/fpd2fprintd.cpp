// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 FuriLabs
//
// Authors:
// Bardia Moshiri <fakeshell@bardia.tech>

#include <QCoreApplication>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusAbstractAdaptor>
#include <QtDBus/QDBusObjectPath>
#include <QtCore/QObject>
#include <QtDBus/QtDBus>
#include <QDebug>
#include <QList>
#include "fpdinterface.h"

class FprintManagerService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "net.reactivated.Fprint.Manager")

public:
    explicit FprintManagerService(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    QList<QDBusObjectPath> GetDevices() {
        // qDebug() << "GetDevices called";
        QList<QDBusObjectPath> devices;
        devices.append(QDBusObjectPath("/net/reactivated/Fprint/Device/0"));
        return devices;
    }

    QDBusObjectPath GetDefaultDevice() {
        // qDebug() << "GetDefaultDevice called";
        return QDBusObjectPath("/net/reactivated/Fprint/Device/0");
    }
};

class FprintDeviceService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "net.reactivated.Fprint.Device")
    Q_PROPERTY(QString name READ getName CONSTANT)
    Q_PROPERTY(int num_enroll_stages READ getNumEnrollStages CONSTANT)
    Q_PROPERTY(QString scan_type READ getScanType CONSTANT)
    Q_PROPERTY(bool finger_present READ isFingerPresent CONSTANT)
    Q_PROPERTY(bool finger_needed READ isFingerNeeded CONSTANT)

public:
    explicit FprintDeviceService(QObject *parent = nullptr) : QObject(parent) {}

    QString getName() const { return QStringLiteral("droidian-fpd"); }
    int getNumEnrollStages() const { return 9; }
    QString getScanType() const { return QStringLiteral("press"); }
    bool isFingerPresent() const { return false; }
    bool isFingerNeeded() const { return false; }

public slots:
    QStringList ListEnrolledFingers(const QString &username) { // username is unused, fpd is a single user daemon
        // qDebug() << "ListEnrolledFingers called for user:" << username;

        FPDInterface fpdInterface;
        QStringList fingers = fpdInterface.fingerprints();
        return fingers;
    }

    void DeleteEnrolledFingers(const QString &username) { // username is unused, fpd is a single user daemon
        // qDebug() << "DeleteEnrolledFingers called for user:" << username;

        FPDInterface fpdInterface;
        QStringList fingers = fpdInterface.fingerprints();
        for (const QString &finger : fingers) {
            fpdInterface.remove(finger);
            // qDebug() << "Deleted finger:" << finger << "for user:" << username;
        }
    }

    void DeleteEnrolledFingers2() {
        // qDebug() << "DeleteEnrolledFingers2 called";

        FPDInterface fpdInterface;
        QStringList fingers = fpdInterface.fingerprints();
        for (const QString &finger : fingers) {
            fpdInterface.remove(finger);
            // qDebug() << "Deleted finger:" << finger;
        }
    }

    void DeleteEnrolledFinger(const QString &fingerName) {
        qDebug() << "DeleteEnrolledFinger called for finger:" << fingerName;

        FPDInterface fpdInterface;
        if (fpdInterface.fingerprints().contains(fingerName)) {
            fpdInterface.remove(fingerName);
        } else {
            QString errorMessage = "Invalid finger name";
            QString errorName = "net.reactivated.Fprint.Error.InvalidFingername";
        }
    }

    void Claim(const QString &username) {
        // qDebug() << "Claim called for user:" << username;
        // maybe do something? is this actually needed when fpd from the ground up is built as a single user daemon?
    }

    void Release() {
        // qDebug() << "Release called";
        // maybe do something? is this actually needed when fpd from the ground up is built as a single user daemon?
    }

    void VerifyStart(const QString &fingerName) {
        // qDebug() << "VerifyStart called for finger:" << fingerName;

        FPDInterface fpdInterface;
        QObject::connect(&fpdInterface, &FPDInterface::identified, [this, fingerName](const QString &identifiedFinger) {
            // qDebug() << "Identification process completed for finger:" << fingerName;
            if (identifiedFinger == fingerName) {
                // qDebug() << "Finger verified successfully:" << fingerName;
                emit VerifyStatus("verify-match", true);
            } else {
                // qDebug() << "Failed to verify finger:" << fingerName;
                emit VerifyStatus("verify-no-match", false);
            }
        });

        // exits early, so signals never get called and it gets stuck
        fpdInterface.identify();
    }

    void VerifyStop() {
        // qDebug() << "VerifyStop called";
        // not possible with fpd
    }

    void EnrollStart(const QString &fingerName) {
        // qDebug() << "EnrollStart called for finger:" << fingerName;

        FPDInterface fpdInterface;
        QObject::connect(&fpdInterface, &FPDInterface::enrollProgressChanged,
            [](int progress) {
                if (progress == 100) {
                    return;
                } else {
                    qDebug() << progress;
                }
            }
        );

        fpdInterface.enroll(fingerName);
    }

    void EnrollStop() {
        // qDebug() << "EnrollStop called";
        // not possible with fpd
    }

signals:
    void VerifyFingerSelected(const QString &fingerName);
    void VerifyStatus(const QString &result, bool done);
    void EnrollStatus(const QString &result, bool done);
};

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    if (!QDBusConnection::systemBus().registerService("net.reactivated.Fprint")) {
        qDebug() << "Failed to register D-Bus service on the system bus";
        return -1;
    }

    FprintManagerService managerService;
    if (!QDBusConnection::systemBus().registerObject("/net/reactivated/Fprint/Manager", &managerService, QDBusConnection::ExportAllSlots)) {
        qDebug() << "Failed to register Manager object on the system bus";
        return -1;
    }

    FprintDeviceService deviceService;
    if (!QDBusConnection::systemBus().registerObject("/net/reactivated/Fprint/Device/0", &deviceService, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals | QDBusConnection::ExportAllProperties)) {
        qDebug() << "Failed to register Device object on the system bus";
        return -1;
    }

    return a.exec();
}

#include "fpd2fprintd.moc"
