// ZipWriter.h — a minimal, correct ZIP writer, so a mod folder can leave this
// tool as one file.
//
// Qt has no public ZIP API and shelling out to a zip program is not a thing a
// Windows build can rely on, so this writes the container itself. It is the
// 1989 format and nothing after it: local header, member data, central
// directory, end-of-central-directory. No ZIP64, no encryption, no data
// descriptors, no folder entries — every member is a file with a full path,
// which is what every unzip on every platform reads.
//
// Members are DEFLATED with the raw framing ZIP wants (zlibDeflateRaw), or
// STORED when deflate came out no smaller — which is the ordinary case for a
// .ftex, whose payload is already compressed chunk by chunk.
//
// The acceptance test is not "it opens here": it is Python's own zipfile
// module reading the archive back, checking every CRC and comparing every
// member against the bytes that went in. A ZIP writer that only its own reader
// accepts is a ZIP writer that does not work.
#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

namespace zipwriter {

struct Entry {
    QString name;        // path inside the archive, forward slashes, no leading /
    QByteArray data;
    // The member's modification time, which ZIP records per entry.
    //
    // IT IS NOT "NOW", AND THAT IS THE POINT. This project's standard is that
    // an export is byte-identical across runs, and a clock stamped into the
    // header breaks that for every archive ever written — two packages of an
    // unchanged mod folder would differ in eight bytes per member and nothing
    // else, which is exactly the shape of difference that makes a diff
    // worthless. So the caller passes the SOURCE file's own date: a real fact
    // about the file, stable while the file is, and the honest answer to "when
    // was this made". Invalid (the default) means the ZIP epoch, 1980-01-01,
    // which is deterministic in the same way.
    QDateTime modified;
};

// Write `entries` to `outPath`. Returns an empty string on success, or a
// sentence naming what stopped it.
QString write(const QString& outPath, const QVector<Entry>& entries);

}  // namespace zipwriter
