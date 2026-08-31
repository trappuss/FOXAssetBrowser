// FovaFile.h — Fox Engine FOVA variation table (".fv2", magic "FOV2").
//
// A variation is how MGSV expresses an appearance change — a weapon
// camouflage, a vehicle paint job, a costume, a piece of Survive gear. It is
// NOT a separate model. One .fv2 says some combination of:
//
//   • hide these mesh groups, show those mesh groups (by StrCode32 of the
//     group name, e.g. "MESH_mask");
//   • on material instance M, rebind texture role R to file T;
//   • attach this extra model, either to the base model's own bones or to a
//     named connection point.
//
// Layout (mgsvmoddingwiki.github.io/FV2, re-validated here against all 1,895
// .fv2 files in the staged TPP + MGO + Survive archives — every one parses,
// every texture index is in range, and every texture-role hash resolves to a
// real role name):
//
//   0x00  char[7]  "FOV2win"          0x18  u16  textures used
//   0x07  u8       version            0x20  u8   mesh groups to HIDE
//   0x08  u16      variable-data off  0x21  u8   mesh groups to SHOW
//   0x0A  u16      external-file off  0x22  u8   material references
//   0x0C  u16      variable entries   0x24  u8   models attached by bone
//   0x0E  u16      external files     0x25  u8   models attached by CNP
//   0x10  u32      static+variable length
//
// The static block begins at 0x28 and holds, in this order:
//   u32[hide]   StrCode32 of each mesh group to hide
//   u32[show]   StrCode32 of each mesh group to show
//   u32[mat] u32[mat] u16[mat] u16[mat]   — PARALLEL arrays, not records:
//               material-instance hash, texture-role hash, external-file
//               index (0xFFFF = leave this slot alone), then a second u16
//               array that is 0xFFFF throughout every shipped file.
//               (The wiki describes these as interleaved 12-byte records;
//               measured against the archives they are parallel — under the
//               record reading the role column decodes to garbage, under this
//               one it decodes to Base_Tex_SRGB, LayerMask_Tex_LIN and the
//               rest on all 1,895 files.)
//   attach-by-bone[bone]   12 bytes each
//   attach-by-cnp[cnp]     20 bytes each
//
// The external-file table at 0x0A is a flat list of PathCode64 — textures,
// but also the .fmdl/.frdv/.sim of any attached model, which is why it is
// called files() and not textures().
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>

namespace fox {

struct FovaSubstitution {
    quint32 materialHash32 = 0;   // which material instance to patch
    quint32 roleHash32 = 0;       // which texture role in it
    int textureIndex = -1;        // index into files(); -1 = no substitution
};

// An extra model this variation brings with it — a hat, a bag, a hair mesh.
struct FovaAttachment {
    bool byConnectPoint = false;  // false: rides the base model's own bones
    quint32 connectPointHash32 = 0;   // StrCode32, CNP form only
    int modelIndex = -1;          // index into files() — the .fmdl
    int frdvIndex = -1;           // index into files(), or -1
    int simIndex = -1;            // index into files(), or -1
};

class FovaFile {
public:
    static bool isFova(const QByteArray& data);

    bool parse(const QByteArray& data);
    QString errorString() const { return m_error; }

    quint8 version() const { return m_version; }
    const QVector<FovaSubstitution>& substitutions() const { return m_subs; }
    // StrCode32 of the mesh groups this variation turns off / on. A model's
    // group names hash the same way, so these match FmdlMeshGroup::name.
    const QVector<quint32>& hiddenMeshGroups() const { return m_hide; }
    const QVector<quint32>& shownMeshGroups() const { return m_show; }
    const QVector<FovaAttachment>& attachments() const { return m_attach; }
    // PathCode64 of every external file this table refers to.
    const QVector<quint64>& files() const { return m_files; }
    // Older name, kept because most call sites only ever want textures.
    const QVector<quint64>& textures() const { return m_files; }

    // How many variable-data entries the table declares. These pick ONE of
    // several sub-entries at random at spawn time (enemy uniforms use them);
    // the sub-entry bodies are not decoded here, so a non-zero count is the
    // honest way to say "this table does more than what is reported above".
    int variableEntryCount() const { return m_variableEntries; }

    // "3 substitutions over 4 files, 1 mesh group hidden" — for list panes.
    QString describe() const;

    // Rows whose file index pointed outside this table's own file list. Any
    // non-zero value means the table is not being read correctly.
    int droppedRows() const { return m_droppedRows; }

    // True when the file parsed structurally but the result cannot be right:
    // it declares files and substitution rows, yet not one row points at a
    // file and it hides, shows and attaches nothing. Callers should say so
    // rather than silently substituting nothing.
    bool unreadableLayout() const { return m_unreadable; }

private:
    quint8 m_version = 0;
    QVector<FovaSubstitution> m_subs;
    QVector<quint32> m_hide;
    QVector<quint32> m_show;
    QVector<FovaAttachment> m_attach;
    QVector<quint64> m_files;
    int m_variableEntries = 0;
    int m_droppedRows = 0;
    QString m_error;
    bool m_unreadable = false;
};

}  // namespace fox
