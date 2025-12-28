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

#include "meta_types.h"

#include <QDBusArgument>
#include <QDBusVariant>

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdDBusExecCommand &cmd) {
    argument.beginStructure();
    argument << cmd.path << cmd.args << cmd.ignoreFailure;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdDBusExecCommand &cmd) {
    argument.beginStructure();
    argument >> cmd.path >> cmd.args >> cmd.ignoreFailure;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdDBusProperty &prop) {
    argument.beginStructure();
    argument << prop.name << QDBusVariant{prop.value};
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdDBusProperty &prop) {
    argument.beginStructure();
    QDBusVariant v;
    argument >> prop.name >> v;
    argument.endStructure();
    prop.value = v.variant();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdDBusAuxUnit &aux) {
    argument.beginStructure();
    argument << aux.name << aux.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdDBusAuxUnit &aux) {
    argument.beginStructure();
    argument >> aux.name >> aux.properties;
    argument.endStructure();
    return argument;
}

namespace {
    class TypeRegistrator {
    public:
        TypeRegistrator() {
            qDBusRegisterMetaType<SystemdDBusExecCommand>();
            qDBusRegisterMetaType<SystemdDBusExecCommandList>();
            qDBusRegisterMetaType<SystemdDBusProperty>();
            qDBusRegisterMetaType<SystemdDBusPropertyList>();
            qDBusRegisterMetaType<SystemdDBusAuxUnit>();
            qDBusRegisterMetaType<SystemdDBusAuxUnitList>();
        }

        ~TypeRegistrator() = default;
    };

    static TypeRegistrator typeRegistrator;
}
