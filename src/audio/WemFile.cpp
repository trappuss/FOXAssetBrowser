// WemFile.cpp — see WemFile.h.
#include "audio/WemFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace audio {
namespace {

quint32 rdU32(const QByteArray& d, qsizetype at)
{
    return at + 4 <= d.size() ? qFromLittleEndian<quint32>(d.constData() + at) : 0;
}
quint16 rdU16(const QByteArray& d, qsizetype at)
{
    return at + 2 <= d.size() ? qFromLittleEndian<quint16>(d.constData() + at) : 0;
}

QString codecNameFor(quint16 tag)
{
    switch (tag) {
    case 0x0001: return QStringLiteral("PCM");
    case 0x0002: return QStringLiteral("Wwise IMA ADPCM");
    case 0x0069: return QStringLiteral("IMA ADPCM");
    case 0x0166: return QStringLiteral("XMA2");
    case 0xAAC0: return QStringLiteral("AAC");
    case 0xFFF0: return QStringLiteral("Wwise DSP");
    case 0xFFFE: return QStringLiteral("PCM (extensible)");
    case 0xFFFF: return QStringLiteral("Wwise Vorbis");
    case 0x3039: return QStringLiteral("Wwise Opus (NX)");
    case 0x3040: return QStringLiteral("Wwise Opus");
    default:
        return QStringLiteral("codec 0x%1").arg(tag, 4, 16, QLatin1Char('0'));
    }
}

}  // namespace

WemInfo parseWem(const QByteArray& d)
{
    WemInfo w;
    if (d.size() < 12 || !d.startsWith("RIFF") || d.mid(8, 4) != "WAVE") return w;
    w.riff = true;

    qsizetype at = 12;
    while (at + 8 <= d.size()) {
        const QByteArray id = d.mid(at, 4);
        const quint32 size = rdU32(d, at + 4);
        const qsizetype body = at + 8;
        if (id == "fmt ") {
            w.tag = rdU16(d, body);
            w.channels = rdU16(d, body + 2);
            w.sampleRate = rdU32(d, body + 4);
            w.bitsPerSample = rdU16(d, body + 14);
            // Wwise Vorbis keeps the total sample count in the fmt extra data
            // (fmt+0x18); sanity-cap at 10 hours.
            if (w.tag == 0xFFFF && size >= 0x1C) {
                const quint32 n = rdU32(d, body + 0x18);
                if (w.sampleRate > 0
                    && n / w.sampleRate < 36000)
                    w.numSamples = n;
            }
        } else if (id == "data") {
            w.dataOffset = static_cast<quint32>(body);
            w.dataSize = qMin<quint32>(size, static_cast<quint32>(
                                                 qMax<qsizetype>(0, d.size() - body)));
        } else if (id == "vorb" && size >= 4) {
            const quint32 n = rdU32(d, body);   // older Wwise: separate vorb chunk
            if (w.sampleRate > 0 && n / qMax(1u, w.sampleRate) < 36000)
                w.numSamples = n;
        }
        at = body + size + (size & 1);
    }

    w.codec = codecNameFor(w.tag);
    if (w.sampleRate > 0) {
        if (w.numSamples > 0)
            w.durationSec = double(w.numSamples) / w.sampleRate;
        else if (w.isPcm() && w.dataSize > 0 && w.bitsPerSample >= 8)
            w.durationSec = double(w.dataSize)
                / (double(w.sampleRate) * w.channels * (w.bitsPerSample / 8));
    }
    return w;
}

QByteArray wemToWav(const QByteArray& wem, const WemInfo& info)
{
    if (!info.isPcm() || info.dataSize == 0) return {};
    const QByteArray pcm = wem.mid(info.dataOffset, info.dataSize);
    QByteArray out;
    out.reserve(44 + pcm.size());
    const quint16 blockAlign = info.channels * (info.bitsPerSample / 8);
    const quint32 byteRate = info.sampleRate * blockAlign;

    const auto putU32 = [&out](quint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        out.append(b, 4);
    };
    const auto putU16 = [&out](quint16 v) {
        char b[2];
        qToLittleEndian(v, b);
        out.append(b, 2);
    };
    out.append("RIFF", 4);
    putU32(36 + static_cast<quint32>(pcm.size()));
    out.append("WAVEfmt ", 8);
    putU32(16);
    putU16(1);                       // plain PCM
    putU16(info.channels);
    putU32(info.sampleRate);
    putU32(byteRate);
    putU16(blockAlign);
    putU16(info.bitsPerSample);
    out.append("data", 4);
    putU32(static_cast<quint32>(pcm.size()));
    out.append(pcm);
    return out;
}

QString vgmstreamPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    const QStringList candidates = {
        appDir + QStringLiteral("/tools/vgmstream/vgmstream-cli.exe"),
        appDir + QStringLiteral("/tools/vgmstream-cli.exe"),
        appDir + QStringLiteral("/vgmstream-cli.exe"),
        // Running from build\release inside the project tree.
        appDir + QStringLiteral("/../../tools/vgmstream/vgmstream-cli.exe"),
        appDir + QStringLiteral("/../../tools/vgmstream-cli.exe"),
    };
#else
    const QStringList candidates = {
        appDir + QStringLiteral("/tools/vgmstream-cli"),
        appDir + QStringLiteral("/vgmstream-cli"),
        QStringLiteral("/usr/bin/vgmstream-cli"),
        QStringLiteral("/usr/local/bin/vgmstream-cli"),
    };
#endif
    for (const QString& c : candidates)
        if (QFileInfo::exists(c)) return c;
    return {};
}

QByteArray convertWithVgmstream(const QByteArray& wem, QString* errOut,
                                int timeoutMs)
{
    const QString tool = vgmstreamPath();
    if (tool.isEmpty()) {
        if (errOut) *errOut = QStringLiteral("vgmstream-cli not found");
        return {};
    }
    QTemporaryDir dir;
    if (!dir.isValid()) {
        if (errOut) *errOut = QStringLiteral("no temp dir");
        return {};
    }
    const QString in = dir.filePath(QStringLiteral("in.wem"));
    const QString out = dir.filePath(QStringLiteral("out.wav"));
    {
        QFile f(in);
        if (!f.open(QIODevice::WriteOnly) || f.write(wem) != wem.size()) {
            if (errOut) *errOut = QStringLiteral("temp write failed");
            return {};
        }
    }
    QProcess p;
    p.start(tool, {QStringLiteral("-o"), out, in});
    if (!p.waitForFinished(timeoutMs) || p.exitCode() != 0) {
        if (errOut)
            *errOut = QStringLiteral("vgmstream failed: %1")
                          .arg(QString::fromUtf8(p.readAllStandardError()).left(200));
        return {};
    }
    QFile f(out);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = QStringLiteral("no vgmstream output");
        return {};
    }
    return f.readAll();
}

#ifdef Q_OS_WIN
namespace {
// PlaySound(SND_MEMORY|SND_ASYNC) reads the buffer for the whole playback —
// keep it alive in a static holder until the next play/stop.
QByteArray g_playing;
}  // namespace

bool playWav(const QByteArray& wav)
{
    if (wav.isEmpty()) return false;
    PlaySoundW(nullptr, nullptr, 0);   // stop BEFORE freeing the old buffer
    g_playing = wav;
    return PlaySoundW(reinterpret_cast<LPCWSTR>(g_playing.constData()), nullptr,
                      SND_MEMORY | SND_ASYNC) != FALSE;
}

void stopPlayback()
{
    PlaySoundW(nullptr, nullptr, 0);
    g_playing.clear();
}
#else
namespace {
QString g_tmpWav;
}  // namespace

bool playWav(const QByteArray& wav)
{
    if (wav.isEmpty()) return false;
    stopPlayback();
    g_tmpWav = QDir::temp().filePath(QStringLiteral("foxab_preview.wav"));
    QFile f(g_tmpWav);
    if (!f.open(QIODevice::WriteOnly) || f.write(wav) != wav.size()) return false;
    f.close();
    // Best-effort: aplay (ALSA) then paplay (Pulse).
    if (QProcess::startDetached(QStringLiteral("aplay"), {QStringLiteral("-q"), g_tmpWav}))
        return true;
    return QProcess::startDetached(QStringLiteral("paplay"), {g_tmpWav});
}

void stopPlayback()
{
    // Detached best-effort players are left to finish on non-Windows dev
    // builds; real stop control ships on the Windows (winmm) path.
}
#endif

}  // namespace audio
