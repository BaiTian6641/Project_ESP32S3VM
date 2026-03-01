/****************************************************************************
** Meta object code from reading C++ file 'QemuController.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../backend/QemuController.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'QemuController.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.9.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN14QemuControllerE_t {};
} // unnamed namespace

template <> constexpr inline auto QemuController::qt_create_metaobjectdata<qt_meta_tag_ZN14QemuControllerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QemuController",
        "qemuStarted",
        "",
        "qemuStopped",
        "i2cTransferRequested",
        "request",
        "spiTransferRequested",
        "uartTxRequested",
        "serialLineReceived",
        "line",
        "cpuSnapshotUpdated",
        "pc",
        "scalarRegs",
        "vectorRegs",
        "memoryWords",
        "debugStatusUpdated",
        "status",
        "gdbAttachCommandUpdated",
        "command"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'qemuStarted'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'qemuStopped'
        QtMocHelpers::SignalData<void()>(3, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'i2cTransferRequested'
        QtMocHelpers::SignalData<void(const QJsonObject &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QJsonObject, 5 },
        }}),
        // Signal 'spiTransferRequested'
        QtMocHelpers::SignalData<void(const QJsonObject &)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QJsonObject, 5 },
        }}),
        // Signal 'uartTxRequested'
        QtMocHelpers::SignalData<void(const QJsonObject &)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QJsonObject, 5 },
        }}),
        // Signal 'serialLineReceived'
        QtMocHelpers::SignalData<void(const QString &)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 9 },
        }}),
        // Signal 'cpuSnapshotUpdated'
        QtMocHelpers::SignalData<void(const QString &, const QStringList &, const QStringList &, const QStringList &)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 11 }, { QMetaType::QStringList, 12 }, { QMetaType::QStringList, 13 }, { QMetaType::QStringList, 14 },
        }}),
        // Signal 'debugStatusUpdated'
        QtMocHelpers::SignalData<void(const QString &)>(15, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 16 },
        }}),
        // Signal 'gdbAttachCommandUpdated'
        QtMocHelpers::SignalData<void(const QString &)>(17, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 18 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<QemuController, qt_meta_tag_ZN14QemuControllerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject QemuController::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14QemuControllerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14QemuControllerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN14QemuControllerE_t>.metaTypes,
    nullptr
} };

void QemuController::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<QemuController *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->qemuStarted(); break;
        case 1: _t->qemuStopped(); break;
        case 2: _t->i2cTransferRequested((*reinterpret_cast< std::add_pointer_t<QJsonObject>>(_a[1]))); break;
        case 3: _t->spiTransferRequested((*reinterpret_cast< std::add_pointer_t<QJsonObject>>(_a[1]))); break;
        case 4: _t->uartTxRequested((*reinterpret_cast< std::add_pointer_t<QJsonObject>>(_a[1]))); break;
        case 5: _t->serialLineReceived((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 6: _t->cpuSnapshotUpdated((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[4]))); break;
        case 7: _t->debugStatusUpdated((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 8: _t->gdbAttachCommandUpdated((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (QemuController::*)()>(_a, &QemuController::qemuStarted, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (QemuController::*)()>(_a, &QemuController::qemuStopped, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (QemuController::*)(const QJsonObject & )>(_a, &QemuController::i2cTransferRequested, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (QemuController::*)(const QJsonObject & )>(_a, &QemuController::spiTransferRequested, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (QemuController::*)(const QJsonObject & )>(_a, &QemuController::uartTxRequested, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (QemuController::*)(const QString & )>(_a, &QemuController::serialLineReceived, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (QemuController::*)(const QString & , const QStringList & , const QStringList & , const QStringList & )>(_a, &QemuController::cpuSnapshotUpdated, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (QemuController::*)(const QString & )>(_a, &QemuController::debugStatusUpdated, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (QemuController::*)(const QString & )>(_a, &QemuController::gdbAttachCommandUpdated, 8))
            return;
    }
}

const QMetaObject *QemuController::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QemuController::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14QemuControllerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int QemuController::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void QemuController::qemuStarted()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void QemuController::qemuStopped()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void QemuController::i2cTransferRequested(const QJsonObject & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void QemuController::spiTransferRequested(const QJsonObject & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void QemuController::uartTxRequested(const QJsonObject & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void QemuController::serialLineReceived(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1);
}

// SIGNAL 6
void QemuController::cpuSnapshotUpdated(const QString & _t1, const QStringList & _t2, const QStringList & _t3, const QStringList & _t4)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1, _t2, _t3, _t4);
}

// SIGNAL 7
void QemuController::debugStatusUpdated(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1);
}

// SIGNAL 8
void QemuController::gdbAttachCommandUpdated(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 8, nullptr, _t1);
}
QT_WARNING_POP
