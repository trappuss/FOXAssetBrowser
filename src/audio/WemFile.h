// WemFile.h — Wwise .wem (RIFF/WAVE) inspection + conversion helpers.
// Parses the chunk list (fmt/data/vorb…), names the codec, estimates duration,
// and can re-wrap PCM payloads as a standard playable .wav. Wwise Vorbis and
// other packed codecs need vgmstream — see audio::vgmstream* below.
#pragma once
#include <QByteArray>
#include <QString>

namespace audio {

struct WemInfo {
    bool riff = false;
    quint16 tag = 0;            // fmt codec tag (0xFFFF = Wwise Vorbis)
    quint16 channels = 0;
    quint32 sampleRate = 0;
    quint16 bitsPerSample = 0;
    quint32 dataOffset = 0;
    quint32 dataSize = 0;
    quint32 numSamples = 0;     // 0 when unknown
    QString codec;              // human-readable codec name
    double durationSec = 0.0;   // 0 when unknown

    // True when the payload is plain PCM we can play without external tools.
    bool isPcm() const
    {
        return riff && (tag == 0x0001 || tag == 0xFFFE)
            && (bitsPerSample == 16 || bitsPerSample == 8) && channels > 0
            && sampleRate > 0;
    }
};

WemInfo parseWem(const QByteArray& data);

// PCM wem → standard RIFF/WAVE (empty when not PCM).
QByteArray wemToWav(const QByteArray& wem, const WemInfo& info);

// vgmstream-cli integration (optional external tool, any wem codec → wav).
// Looked up beside the executable and in <exe>/tools/. Empty when absent.
QString vgmstreamPath();
// Convert via vgmstream-cli; empty QByteArray + errOut on failure. timeoutMs
// bounds the external process (use a short timeout for UI-thread callers).
QByteArray convertWithVgmstream(const QByteArray& wem, QString* errOut = nullptr,
                                int timeoutMs = 20000);

// Fire-and-forget playback of a .wav blob (winmm on Windows, best-effort
// aplay/paplay elsewhere). stopPlayback() cancels an async play.
bool playWav(const QByteArray& wav);
void stopPlayback();

}  // namespace audio
