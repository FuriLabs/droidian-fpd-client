// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2023 Droidian Project
//
// Authors:
// Bardia Moshiri <fakeshell@bardia.tech>

#include <QCoreApplication>
#include <QTextStream>
#include <QDebug>
#include "fpdinterface.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    FPDInterface fpdInterface;

    QObject::connect(&fpdInterface, &FPDInterface::identified, [](const QString &finger) {
        qDebug() << "Identified finger:" << finger;
        QCoreApplication::exit(0);
    });

    QObject::connect(&fpdInterface, &FPDInterface::errorInfo, [](const QString &info) {
        qDebug() << "Error info:" << info;

        if (info.contains("FINGER_NOT_RECOGNIZED")) {
            qDebug() << "Finger not recognized";
        }

        QCoreApplication::exit(1);
    });

    qDebug() << "Waiting for finger identification...";
    fpdInterface.identify();

    return app.exec();
}
