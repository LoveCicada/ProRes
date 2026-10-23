#pragma once

#include <QString>
#include <QtGlobal>

struct GpuAdapter {
    qint64 luidHigh = 0;
    quint32 luidLow = 0;
    QString name;
    bool valid = false;
};
