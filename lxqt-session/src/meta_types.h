/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * LXQt - a lightweight, Qt based, desktop toolset
 * https://lxqt.org
 *
 * Copyright: 2025
 * Authors:
 *   Basil Crow <me@basilcrow.com>
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
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 *
 * END_COMMON_COPYRIGHT_HEADER */

#ifndef QTXDG_SYSTEMD_META_TYPES__INCLUDED
#define QTXDG_SYSTEMD_META_TYPES__INCLUDED

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QVariant>

struct SystemdDBusExecCommand {
    QString path;
    QStringList args;
    bool ignoreFailure = false;
};

struct SystemdDBusProperty {
    QString name;
    QVariant value;
};

struct SystemdDBusAuxUnit {
    QString name;
    QList<SystemdDBusProperty> properties;
};

using SystemdDBusExecCommandList = QList<SystemdDBusExecCommand>;
using SystemdDBusPropertyList = QList<SystemdDBusProperty>;
using SystemdDBusAuxUnitList = QList<SystemdDBusAuxUnit>;

Q_DECLARE_METATYPE(SystemdDBusExecCommand)
Q_DECLARE_METATYPE(SystemdDBusExecCommandList)
Q_DECLARE_METATYPE(SystemdDBusProperty)
Q_DECLARE_METATYPE(SystemdDBusPropertyList)
Q_DECLARE_METATYPE(SystemdDBusAuxUnit)
Q_DECLARE_METATYPE(SystemdDBusAuxUnitList)

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdDBusExecCommand &cmd);
const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdDBusExecCommand &cmd);

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdDBusProperty &prop);
const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdDBusProperty &prop);

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdDBusAuxUnit &aux);
const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdDBusAuxUnit &aux);

#endif // QTXDG_SYSTEMD_META_TYPES__INCLUDED
