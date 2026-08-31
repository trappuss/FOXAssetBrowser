// Fox2File.h — read-only parser for Fox2 entity binaries (.fox2 / .parts /
// .vfsm…, magic 0xf2"box"). Port of Fox_Parser's MgsvModBldr.Tools.Fox read
// path (FoxFile/FoxEntity/FoxProperty + the container/value types), producing
// a generic entity → property tree with values resolved through the file's
// OWN embedded string table (hash → literal pairs at the tail), so class,
// property and string values come out as readable text without any external
// dictionary. Unresolved hashes render as hex.
//
// This is the data behind the engine's entity system: a .parts file's
// PartsDesc wiring (model ↔ rig ↔ driver ↔ sim connections, variant sets),
// vfsm state machines, fox2 level entities.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVariant>
#include <QVector>
#include <cstdint>

namespace fox {

// fox::PropertyInfo::Type
enum class Fox2Type : quint8 {
    Int8 = 0, UInt8 = 1, Int16 = 2, UInt16 = 3, Int32 = 4, UInt32 = 5,
    Int64 = 6, UInt64 = 7, Float = 8, Double = 9, Bool = 10, String = 11,
    Path = 12, EntityPtr = 13, Vector3 = 14, Vector4 = 15, Quat = 16,
    Matrix3 = 17, Matrix4 = 18, Color = 19, FilePtr = 20, EntityHandle = 21,
    EntityLink = 22, PropertyInfo = 23, WideVector3 = 24,
};

enum class Fox2Container : quint8 {
    StaticArray = 0, DynamicArray = 1, StringMap = 2, List = 3,
};

struct Fox2Property {
    quint64 nameHash = 0;
    QString name;                    // resolved (or "0x…")
    Fox2Type type = Fox2Type::Int8;
    Fox2Container container = Fox2Container::StaticArray;
    // One entry per value. Strings/paths resolved; EntityLink rendered as a
    // "pkg|archive|name|handle" string; vectors/matrices as QVariantList of
    // doubles; ints as (u)longlong.
    QVector<QVariant> values;
    QVector<QString> mapKeys;        // StringMap only, parallel to values

    QString typeName() const;
    QString valueText(int i) const;  // display form of values[i]
};

struct Fox2Entity {
    quint64 classHash = 0;
    QString className;
    quint16 version = 0;
    quint32 address = 0;
    QVector<Fox2Property> statics;
    QVector<Fox2Property> dynamics;

    // First property with this (resolved) name, statics then dynamics.
    const Fox2Property* find(const QString& propName) const;
};

class Fox2File {
public:
    static bool isFox2(const QByteArray& data);

    bool parse(const QByteArray& data);
    QString errorString() const { return m_error; }

    const QVector<Fox2Entity>& entities() const { return m_entities; }
    // The file's own hash → literal table (also used to resolve names).
    QString lookup(quint64 hash) const;

private:
    QVector<Fox2Entity> m_entities;
    QHash<quint64, QString> m_strings;
    QString m_error;
};

}  // namespace fox
