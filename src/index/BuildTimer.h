// BuildTimer.h — how long a catalogue took, in one line, from one place.
//
// Five of the index catalogues logged NOTHING. They sweep every file in the
// install and cost whatever they cost, and the only way to find out was to
// diff timestamps between the lines on either side of them — which only works
// if the neighbours happen to log. "Optimise the index" is not a thing anyone
// can do to code that does not say what it costs, so every catalogue says.
//
// Scoped: declare one at the top of build() and it reports on the way out,
// including the early returns. `setNote` adds whatever the catalogue counted.
#pragma once
#include <QElapsedTimer>
#include <QString>
#include <QtGlobal>

namespace fox {

class BuildTimer {
public:
    explicit BuildTimer(const char* tag) : m_tag(tag) { m_timer.start(); }
    BuildTimer(const BuildTimer&) = delete;
    BuildTimer& operator=(const BuildTimer&) = delete;

    void setNote(const QString& note) { m_note = note; }

    ~BuildTimer()
    {
        if (m_note.isEmpty())
            qInfo("%s: built in %lld ms", m_tag,
                  static_cast<long long>(m_timer.elapsed()));
        else
            qInfo("%s: %s in %lld ms", m_tag, qUtf8Printable(m_note),
                  static_cast<long long>(m_timer.elapsed()));
    }

private:
    const char* m_tag;
    QString m_note;
    QElapsedTimer m_timer;
};

}  // namespace fox
