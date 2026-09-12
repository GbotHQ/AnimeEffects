#ifndef GUI_RES_RESOURCEUPDATER_H
#define GUI_RES_RESOURCEUPDATER_H

#include <vector>
#include <QStringList>
#include "cmnd/Stack.h"
#include "core/Project.h"
#include "ctrl/SourceDocument.h"
#include "gui/ViaPoint.h"
#include "gui/res/res_Item.h"
#include "gui/res/res_Notifier.h"

namespace gui::res {

    // Brings a source document (a PSD, an ORA, a bitmap) into a project's resource tree, and
    // brings the project back up to date when that file changes.
    //
    // A reload RECONCILES rather than refuses: it reads the file once into a
    // ctrl::SourceDocument (the same description the importer consumes), matches the project
    // against it by identity - one place, ctrl::matchesIdentity - and then applies a plan of
    // keep / update / append / remove. What the plan detects is what it applies: the resource
    // side (pixels, rect, mask, blend, visibility) and the object side (Blend If, layer effects,
    // fill opacity, opacity, visibility) are compared by ctrl::diffResource / ctrl::diffObject
    // and every difference found is applied or reported; nothing is dropped silently, and
    // nothing is applied that was not detected.
    class ResourceUpdater: public QObject {
        Q_OBJECT
    public:
        static res::Item* createGUITree(const QTreeWidget& aTree, img::ResourceNode& aNode, const QString& aIdentifier);

        ResourceUpdater(ViaPoint& aViaPoint, core::Project& aProject);
        void load(QTreeWidget& aTree, const QString& aFilePath);
        void reload(Item& aItem);
        // the same reconciliation, but the file's content is applied even where the comparison
        // says the two sides agree (the "paste keys" path asks for this)
        void forceReload(Item& aItem);
        void remove(QTreeWidget& aTree, Item& aTopItem);

    private:
        // the file, as a format-neutral description (see ctrl/SourceDocument.h): one reader per
        // container format, shared by the import path and by the reload
        ctrl::SourceDocument describeSource(const QString& aFilePath, bool aWantPixels);
        img::ResourceNode* createResourceTree(const QString& aFilePath, bool aLoadImage);

        // Reports the effects the file switches on that this build cannot draw (see
        // ctrl::undrawnEffectNotes) to the app's log: on import when the file is added, and on
        // each reload, so a wrong picture is never silent.
        void pushEffectNotes(const ctrl::SourceDocument& aDocument);

        // one entry of the reconciliation plan: what the file says and what the project has.
        // Both null never happens; either one null is an append (the file has it) or a removal
        // (the project has it).
        struct PlanNode {
            img::ResourceNode* current = nullptr;
            const ctrl::SourceDocument::Entry* entry = nullptr;
            std::vector<PlanNode> children;

            bool isAppend() const { return current == nullptr; }
            bool isRemove() const { return entry == nullptr; }
        };

        // A reload collects the commands it wants BEFORE pushing any of them: a reload that
        // finds nothing to do must not leave an empty undo step (or mark the project edited),
        // and the macro cannot be opened until it is known to be non-empty.
        struct ApplyContext {
            ModificationNotifier& notifier;
            ctrl::SourceDocument& document;
            std::vector<cmnd::Base*> commands;
            bool force = false;             // apply the file's content without comparing
            QStringList ambiguous;          // entries the identity rule could not tell apart
            QStringList renamed;            // resources whose name differs (reported, not applied)
            QStringList keptInUse;          // nodes the file dropped but the timeline still draws
            QStringList unmatchedGroups;    // groups with no object node to apply properties to
        };

        // matches the project's children against the entry's children by identity and recurses
        void buildPlanRecursive(
            const img::ResourceNode& aParent, const ctrl::SourceDocument::Entry& aEntry, PlanNode& aOut,
            QStringList& aOutAmbiguous
        );

        // applies one plan level: appends, removes, content updates and object-side updates
        void applyPlanRecursive(QTreeWidgetItem& aItem, PlanNode& aPlan, ApplyContext& aContext);

        // the whole reload, shared by reload() and forceReload()
        void reconcile(Item& aItem, bool aForce);

        ViaPoint& mViaPoint;
        core::Project& mProject;
    };

} // namespace gui::res


#endif // GUI_RES_RESOURCEUPDATER_H
