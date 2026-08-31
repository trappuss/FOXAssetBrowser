// MaterialInspector.h — the material debug panel for the 3D viewports.
//
// What it is for: when a surface renders wrong, the question is always "which
// map is bound here, and what is actually IN it". That question used to need a
// devshot and a terminal. This answers it in the app: every material in the
// current scene, its shader and what that shader asks for, the meshes and mesh
// groups that use it, and every texture map split into its channels with the
// mean value of each — because a roughness channel that reads 0.02 everywhere
// is a bug you can see in one number and cannot see at all in a thumbnail.
//
// Each material is drawn as ONE painted card rather than a nest of widgets.
// Fifty materials times six maps times five channels is fifteen hundred
// QLabels, which is slow to build and impossible to lay out precisely; a card
// painted with QPainter costs one widget and gives exact control over the
// alignment that makes the panel readable.
//
// A Source carries its material metadata BY VALUE and never a pointer into the
// caller's model. The panel rebuilds on a timer (a Customize slot change tears
// the scene down one part at a time and would otherwise rebuild it a dozen
// times for one click), and a deferred rebuild holding a pointer into a
// QVector<Part> that has since been appended to is a use-after-free. Copying a
// few hundred bytes of names and hashes per material removes the whole class.
#pragma once
#include <QHash>
#include <QSet>
#include <QImage>
#include <QString>
#include <QVector>
#include <QWidget>

#include "fox/FmdlFile.h"
#include "gl/GLModelWidget.h"

class QLabel;
class QLineEdit;
class QTimer;
class QVBoxLayout;
class QScrollArea;

class MaterialInspector : public QWidget {
    Q_OBJECT
public:
    explicit MaterialInspector(QWidget* parent = nullptr);

    // Which meshes use one material. The submesh view: a Fox material is
    // routinely shared by several meshes sitting in different groups, and
    // "the head is untextured" is usually one of those meshes and not the
    // material.
    struct Use {
        int meshIndex = -1;
        QString group;
        int tris = 0;
    };

    struct MaterialEntry {
        QString name;
        QString shader;
        QVector<fox::FmdlTextureRef> textures;
        // The material's NON-texture parameters: named float4s the shader
        // reads. They were parsed all along and dropped one struct short of
        // this panel — the renderer consumes MatParamIndex_* and the
        // Incidence pair, and nothing ever showed a person the rest. They are
        // the only place the Tension* and Edge* values appear at all, which is
        // what made "Tension*/Edge* material leftovers" an open question no
        // one could look at.
        QVector<fox::FmdlMaterialParam> params;
        QVector<Use> uses;
    };

    // One SOURCE of materials — one model, or one part of a composed scene.
    // `label` is what the panel calls it ("hdm0_main0_def.fmdl", "part 2 ·
    // arm"); `slotBase` is where this source's materials START in the scene's
    // combined material list, so a card names the slot the viewport actually
    // binds rather than the index within its own model.
    struct Source {
        QString label;
        QVector<MaterialEntry> materials;
        QVector<QImage> base;
        QVector<QImage> normals;
        QVector<GLPbrMaterial> pbr;
        int slotBase = 0;
        // Whether this model came out of a Ground Zeroes archive. Needed
        // because the panel decodes the maps the RENDERER does not load —
        // dirt, the material-ID map, sub-normals — for itself, and the GZ
        // texture lookup goes by path rather than by stored hash.
        bool gz = false;
    };

    // Build the by-value material metadata for one parsed model. One helper so
    // every caller describes a model the same way.
    static QVector<MaterialEntry> entriesFor(const fox::FmdlFile& model);

    // Replace what the panel shows. The repaint itself is deferred by a short
    // timer, so a burst of scene changes costs one rebuild rather than one per
    // change; the Sources are copied immediately, so the caller is free to
    // mutate whatever they came from as soon as this returns.
    void setSources(const QVector<Source>& sources);
    void clear();

    // Type into the filter box. For the screenshot harness, which cannot
    // click — and for a "show me only this material" action later.
    void setFilterText(const QString& text);

    // A one-line summary of the whole scene, shown above the cards.
    QString summary() const { return m_summary; }

    // What an export of one selected card needs. Held beside the card rather
    // than re-derived from the sources: a click gives us a card index, and
    // walking back from that to a (source, material) pair through a filtered
    // list is the kind of parallel indexing that goes wrong the first time
    // anything is hidden.
    struct CardTexture {
        QString role;       // "Base_Tex_SRGB"
        QString path;       // the texture's real path, when the name is known
        quint64 pathHash = 0;
    };
    struct CardInfo {
        QString material;   // the folder an export makes
        QString source;     // which model or part it came from
        QVector<CardTexture> textures;
    };
    // The selected cards, in card order. Empty when nothing is selected.
    QVector<CardInfo> selectedMaterials() const;
    // Select (and scroll to) the card for the material called `name`, as the
    // viewport's two-way selection asks: double-clicking a submesh highlights
    // it in the parts list AND its material here. Matching by NAME rather than
    // by slot on purpose — a composed scene re-bases every source's slots, so a
    // slot number means different materials in the two tabs that use this
    // panel, while the name is the same string in both. False when no card
    // carries that name, which is the honest answer for a submesh whose
    // material the panel is filtering out.
    bool selectMaterialNamed(const QString& name);
    int selectedCount() const { return m_selected.size(); }

Q_SIGNALS:
    // "Export these materials' images…". The panel knows which materials and
    // which textures; it does NOT know where the user wants them or how to
    // decode an .ftex, so it says what and stops. The owning tab does the rest.
    void exportImagesRequested(const QVector<CardInfo>& materials);

private:
    void selectCard(int idx, Qt::KeyboardModifiers mods);
    void syncSelection();
    void showContextMenu(const QPoint& at);
    void rebuild();
    void applyFilter();

    QVector<Source> m_sources;
    QString m_summary;
    QLabel* m_header = nullptr;
    QLineEdit* m_filter = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_cards = nullptr;
    QVBoxLayout* m_cardsLayout = nullptr;
    QTimer* m_rebuildTimer = nullptr;
    // Decoded thumbnails for the maps the RENDERER does not load — dirt, the
    // material-ID map, sub-normals. Keyed by PathCode64 and kept only while
    // the panel is open: a Customize slot change rebuilds the scene many times
    // over, and re-reading and re-decoding the same dirt map on each of them
    // froze the window for seconds.
    // Keyed by a STRING, not by PathCode64: a Ground Zeroes model carries a
    // string table rather than path hashes, so every one of its texture refs
    // has pathHash == 0 and a hash-keyed cache collapsed all of them onto one
    // entry — the first map decoded was then drawn, with its channel means,
    // under every other map's name. The game flag is in the key too, since two
    // mounted installs can share a hash.
    QHash<QString, QImage> m_extraCache;
    // Parallel to the cards in m_cardsLayout: the text each card is searched
    // by. Held separately so filtering never has to re-paint anything.
    QVector<QString> m_cardText;
    QVector<QWidget*> m_cardWidgets;
    QVector<CardInfo> m_cardInfo;
    QSet<int> m_selected;
    // The last plainly-clicked card — what a Shift-click measures a range
    // from. -1 until something has been clicked.
    int m_anchor = -1;
};
