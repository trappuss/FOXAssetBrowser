// FcnpFile.h — Fox connect points (.fcnp): the model's attachment sockets
// (CNP_RIGHT_HAND, CNP_EYE, CNP_ASRROOT…). A FoxData sibling chain, one node
// per point: 48-byte payload {pos.xyz,1, quat.xyzw, scale.xyz,1} and a param
// record whose "Parent" entry names the bone the point hangs off.
// (Layout reverse-engineered against sna4_enem0_def.fcnp and validated on
// every fcnp in the test replicas.)
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

namespace fox {

struct ConnectPoint {
    QString name;          // "CNP_*"
    QString parentBone;    // "SKL_*" (empty when the file omits it)
    float pos[3] = {0, 0, 0};
    float quat[4] = {0, 0, 0, 1};
    float scale[3] = {1, 1, 1};
};

class FcnpFile {
public:
    bool parse(const QByteArray& data);
    bool valid() const { return !m_points.isEmpty(); }
    const QVector<ConnectPoint>& points() const { return m_points; }
    const ConnectPoint* find(const QString& name) const;

private:
    QVector<ConnectPoint> m_points;
};

}  // namespace fox
