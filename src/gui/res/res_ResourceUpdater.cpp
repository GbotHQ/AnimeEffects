#include <utility>
#include <iostream>
#include <QFileInfo>
#include <QMessageBox>
#include "util/TextUtil.h"
#include "util/TreeUtil.h"
#include "img/PSDReader.h"
#include "img/PSDUtil.h"
#include "img/BlendMode.h"
#include "cmnd/BasicCommands.h"
#include "cmnd/ScopedMacro.h"
#include "cmnd/BasicCommands.h"
#include "ctrl/CmndName.h"
#include "core/ImageKey.h"
#include "core/LayerNode.h"
#include "core/OpaKey.h"
#include "core/TimeKey.h"
#include "gui/res/res_ImageSetter.h"
#include "gui/res/res_ResourceUpdater.h"
#include "img/Util.h"
#include "ctrl/SourceDocument.h"
#include "deps/zip_file.h"

// #define RESOURCE_UPDATER_DUMP(...) XC_DEBUG_REPORT(__VA_ARGS__)
#define RESOURCE_UPDATER_DUMP(...)

namespace gui::res {

    //-------------------------------------------------------------------------------------------------
    class NewTreePusher: public cmnd::Stable {
        core::ResourceHolder& mHolder;
        img::ResourceNode* mTree;
        QString mAbsFilePath;
        bool mDone;

    public:
        NewTreePusher(core::ResourceHolder& aHolder, img::ResourceNode* aTree, const QString& aFilePath):
            mHolder(aHolder), mTree(aTree), mAbsFilePath(QFileInfo(aFilePath).absoluteFilePath()), mDone(false) {
            XC_PTR_ASSERT(aTree);
        }

        ~NewTreePusher() {
            if (!mDone) {
                delete mTree;
            }
        }

        virtual void undo() {
            mHolder.popImageTree();
            mDone = false;
        }

        virtual void redo() {
            mHolder.pushImageTree(*mTree, mAbsFilePath);
            mDone = true;
        }
    };

    //-------------------------------------------------------------------------------------------------
    class GUINewTreePusher: public cmnd::Stable {
        QTreeWidget& mTree;
        QTreeWidgetItem* mItem;
        bool mDone;

    public:
        GUINewTreePusher(QTreeWidget& aTree, QTreeWidgetItem* aItem): mTree(aTree), mItem(aItem), mDone(false) {
            XC_PTR_ASSERT(aItem);
        }

        ~GUINewTreePusher() {
            if (!mDone) {
                delete mItem;
            }
        }

        virtual void undo() {
            bool success = false;
            for (int i = 0; i < mTree.topLevelItemCount(); ++i) {
                if (mItem == mTree.topLevelItem(i)) {
                    mTree.takeTopLevelItem(i);
                    success = true;
                    break;
                }
            }
            XC_ASSERT(success);
            mDone = false;
        }

        virtual void redo() {
            mTree.addTopLevelItem(mItem);
            mDone = true;
        }
    };

    //-------------------------------------------------------------------------------------------------
    class TreeDeleter: public cmnd::Stable {
        core::ResourceHolder& mHolder;
        core::ResourceHolder::ImageTree mTree;
        int mIndex;
        bool mDone;

    public:
        TreeDeleter(core::ResourceHolder& aHolder, int aIndex):
            mHolder(aHolder), mTree(), mIndex(aIndex), mDone(false) {}

        ~TreeDeleter() {
            if (mDone) {
                delete mTree.topNode;
            }
        }

        virtual void exec() {
            mTree = mHolder.imageTree(mIndex);
            XC_PTR_ASSERT(mTree.topNode);
            redo();
        }

        virtual void undo() {
            mHolder.insertImageTree(mTree, mIndex);
            mDone = false;
        }

        virtual void redo() {
            mHolder.removeImageTree(mIndex);
            mDone = true;
        }
    };

    //-------------------------------------------------------------------------------------------------
    class GUITreeDeleter: public cmnd::Stable {
        QTreeWidget& mTree;
        QTreeWidgetItem* mTopItem;
        int mIndex;

    public:
        GUITreeDeleter(QTreeWidget& aTree, int aIndex): mTree(aTree), mTopItem(), mIndex(aIndex) {}

        ~GUITreeDeleter() {
            if (mTopItem) {
                delete mTopItem;
            }
        }

        virtual void undo() {
            mTree.insertTopLevelItem(mIndex, mTopItem);
            mTopItem = nullptr;
        }

        virtual void redo() { mTopItem = mTree.takeTopLevelItem(mIndex); }
    };

    //-------------------------------------------------------------------------------------------------
    class GUITreeItemAppender: public cmnd::Stable {
        QTreeWidgetItem& mParent;
        QTreeWidgetItem* mItem;
        bool mDone;

    public:
        GUITreeItemAppender(QTreeWidgetItem& aParent, QTreeWidgetItem* aItem):
            mParent(aParent), mItem(aItem), mDone(false) {}

        ~GUITreeItemAppender() {
            if (!mDone) {
                delete mItem;
            }
        }

        virtual void undo() {
            bool success = false;
            for (int i = 0; i < mParent.childCount(); ++i) {
                if (mItem == mParent.child(i)) {
                    mParent.takeChild(i);
                    success = true;
                    break;
                }
            }
            XC_ASSERT(success);
            mDone = false;
        }

        virtual void redo() {
            mParent.addChild(mItem);
            mDone = true;
        }
    };

    //-------------------------------------------------------------------------------------------------
    class GUITreeItemDeleter: public cmnd::Stable {
        QTreeWidgetItem& mParent;
        QTreeWidgetItem* mItem;
        int mIndex;
        bool mDone;

    public:
        GUITreeItemDeleter(QTreeWidgetItem& aParent, QTreeWidgetItem* aItem):
            mParent(aParent), mItem(aItem), mIndex(), mDone(false) {}

        ~GUITreeItemDeleter() {
            if (mDone) {
                delete mItem;
            }
        }

        virtual void exec() {
            for (mIndex = 0; mIndex < mParent.childCount(); ++mIndex) {
                if (mItem == mParent.child(mIndex))
                    break;
            }
            XC_ASSERT(mIndex < mParent.childCount());
            redo();
        }

        virtual void undo() {
            mParent.insertChild(mIndex, mItem);
            mDone = false;
        }

        virtual void redo() {
            mParent.takeChild(mIndex);
            mDone = true;
        }
    };

    //-------------------------------------------------------------------------------------------------
    void addTreeItemRecursive(const QTreeWidget& aTree, QTreeWidgetItem& aItem, img::ResourceNode& aNode) {
        for (auto childNode : aNode.children()) {
            auto childItem = new res::Item(aTree, *childNode, childNode->data().identifier());
            aItem.addChild(childItem);

            // recursive call
            addTreeItemRecursive(aTree, *childItem, *childNode);
        }
    }

    res::Item*
    ResourceUpdater::createGUITree(const QTreeWidget& aTree, img::ResourceNode& aNode, const QString& aIdentifier) {
        auto item = new res::Item(aTree, aNode, aIdentifier);
        addTreeItemRecursive(aTree, *item, aNode);
        return item;
    }

    //-------------------------------------------------------------------------------------------------
    ResourceUpdater::ResourceUpdater(ViaPoint& aViaPoint, core::Project& aProject):
        mViaPoint(aViaPoint), mProject(aProject) {}

    //-------------------------------------------------------------------------------------------------
    void ResourceUpdater::load(QTreeWidget& aTree, const QString& aFilePath) {
        if (aFilePath.isEmpty())
            return;

        auto newTree = createResourceTree(aFilePath, true);
        if (!newTree)
            return;

        auto& holder = mProject.resourceHolder();

        auto guiTree = createGUITree(aTree, *newTree, holder.relativeFilePath(aFilePath));
        XC_PTR_ASSERT(guiTree);

        {
            auto& stack = mProject.commandStack();

            cmnd::ScopedMacro macro(stack, CmndName::tr("Add new asset"));

            // notifier
            auto notifier = new AddNewOneNotifier(mViaPoint, mProject);
            notifier->event().setSingleTarget(*newTree);
            macro.grabListener(notifier);

            stack.push(new NewTreePusher(holder, newTree, aFilePath));
            stack.push(new GUINewTreePusher(aTree, guiTree));
        }
    }

    // One place that turns a source file into a description, whatever the container: a PSD is
    // read by img::PSDReader, an ORA by the zip reader + its stack.xml, anything else by QImage.
    // The importer and the reload both go through it, so a format is interpreted in exactly one
    // place and the two paths cannot drift apart.
    ctrl::SourceDocument ResourceUpdater::describeSource(const QString& aFilePath, bool aWantPixels) {
        const QFileInfo fileInfo(aFilePath);
        if (!fileInfo.isFile()) {
            return {};
        }
        const QString suffix = fileInfo.suffix();

        if (suffix == "psd") {
            std::ifstream file(aFilePath.toLocal8Bit(), std::ios::binary);
            if (file.fail()) {
                QMessageBox::warning(nullptr, tr("FileIO Error"), tr("PSD file not found."));
                return {};
            }
            img::PSDReader reader(file);
            if (reader.resultCode() != img::PSDReader::ResultCode_Success) {
                QMessageBox::warning(
                    nullptr, tr("PSD parse error"),
                    "error(" + QString::number(reader.resultCode()) + ") "
                        + QString::fromStdString(reader.resultMessage()));
                return {};
            }
            file.close();
            ctrl::SourceDocument document =
                ctrl::describePsd(*reader.format(), fileInfo.baseName(), aWantPixels);
            if (document.isValid()) {
                document.canvasSize =
                    QSize((int)reader.format()->header().width, (int)reader.format()->header().height);
            }
            return document;
        }

        if (suffix == "ora") {
            // The ORA format has two import modes and the file does not record which one was
            // used, so the artist is asked - the same question the importer asks.
            QMessageBox loadMerged;
            loadMerged.setWindowTitle(tr("Select ORA type"));
            loadMerged.setText(tr("How do you wish to load this ORA file?"));
            loadMerged.addButton(tr("Load layered"), QMessageBox::YesRole);
            QAbstractButton* mergeButton = loadMerged.addButton(tr("Load merged"), QMessageBox::YesRole);
            QAbstractButton* cancelButton = loadMerged.addButton(tr("Cancel file load"), QMessageBox::NoRole);
            loadMerged.exec();
            if (loadMerged.clickedButton() == cancelButton) {
                return {};
            }
            const bool merged = loadMerged.clickedButton() == mergeButton;
            return ctrl::describeOra(aFilePath.toStdString(), merged, fileInfo.baseName(), aWantPixels);
        }

        QImage image(aFilePath);
        if (image.isNull()) {
            QMessageBox::warning(nullptr, tr("QImage Error"), tr("Failed to load image file."));
            return {};
        }
        return ctrl::describeImage(image, fileInfo.baseName(), aWantPixels);
    }

    img::ResourceNode* ResourceUpdater::createResourceTree(const QString& aFilePath, bool aLoadImage) {
        ctrl::SourceDocument document = describeSource(aFilePath, aLoadImage);
        if (!document.isValid()) {
            if (!document.error.isEmpty()) {
                QMessageBox::warning(nullptr, tr("Parse error"), document.error);
            }
            return nullptr;
        }
        // Say what the imported picture is missing, at the moment the file is added: the
        // effects this build cannot draw would otherwise be silently absent from the render.
        pushEffectNotes(document);
        return ctrl::buildResourceTree(document);
    }

    void ResourceUpdater::pushEffectNotes(const ctrl::SourceDocument& aDocument) {
        for (const QString& note : ctrl::undrawnEffectNotes(aDocument)) {
            mViaPoint.pushLog(note, ctrl::UILogType_Warn);
        }
    }

    //-------------------------------------------------------------------------------------------------
    // RECONCILIATION
    //
    // A reload used to descend the file and the project in parallel, require the GUI item's child
    // count to equal the model's (it aborted the whole reload with a modal error when they
    // disagreed), tag every node as abandoned and then un-tag the ones it visited, and apply only
    // pixels. It now computes a PLAN first - match by identity, then keep / update / append /
    // remove - and applies it, which is what makes the four guarantees hold:
    //
    //   - nothing depends on the GUI tree mirroring the model (items are looked up BY NODE);
    //   - the model carries no transient state, so stopping half way leaves nothing behind;
    //   - a structural change updates the project instead of refusing it;
    //   - what the plan detects is what it applies (ctrl::diffResource / diffObject), and the
    //     commands are collected before any are pushed, so a reload that finds nothing to do
    //     leaves the project unedited.
    namespace {

        // the GUI item that shows a given resource node. A reload must not assume the item tree
        // and the model agree by index, so this searches by node.
        res::Item* findItemFor(QTreeWidgetItem& aParent, const img::ResourceNode& aNode) {
            for (int i = 0; i < aParent.childCount(); ++i) {
                auto* child = dynamic_cast<res::Item*>(aParent.child(i));
                if (child && &child->node() == &aNode) {
                    return child;
                }
            }
            return nullptr;
        }

        // The object nodes a resource node stands for.
        //   - a LAYER resource is whatever draws it: the image key of every object node that holds
        //     this resource (the resource tree is a palette, so there can be several);
        //   - a FOLDER resource has no image key, so it is found by the object tree mirroring the
        //     resource tree's structure - the import builds both from one description, so the
        //     folder with the same name along the same path is it. Best effort by construction,
        //     which is why a miss is reported rather than silently ignored.
        QVector<core::ObjectNode*> correspondingObjectNodes(
            core::Project& aProject, const img::ResourceNode& aNode
        ) {
            QVector<core::ObjectNode*> result;
            auto* top = aProject.objectTree().topNode();
            if (!top) {
                return result;
            }

            if (aNode.data().isLayer()) {
                core::ObjectNode::Iterator itr(top);
                while (itr.hasNext()) {
                    auto node = itr.next();
                    if (auto* timeLine = node->timeLine()) {
                        if (auto* key =
                                dynamic_cast<core::ImageKey*>(timeLine->defaultKey(core::TimeKeyType_Image))) {
                            if (key->data().resource().get() == &aNode.data()) {
                                result.push_back(node);
                            }
                        }
                    }
                }
                return result;
            }

            // the names from the tree root down to this folder
            QStringList path;
            for (auto ptr = &aNode; ptr != nullptr; ptr = ptr->parent()) {
                path.push_front(ptr->data().identifier());
            }
            path.removeFirst(); // the tree root's name is the document's, not a node name

            auto* current = (core::ObjectNode*)top;
            for (const QString& name : path) {
                core::ObjectNode* next = nullptr;
                for (auto child : current->children()) {
                    if (child->name() == name && child->type() == core::ObjectType_Folder) {
                        next = child;
                        break;
                    }
                }
                if (!next) {
                    return {};
                }
                current = next;
            }
            result.push_back(current);
            return result;
        }

    }

    void ResourceUpdater::buildPlanRecursive(
        const img::ResourceNode& aParent, const ctrl::SourceDocument::Entry& aEntry, PlanNode& aOut,
        QStringList& aOutAmbiguous
    ) {
        for (const auto& child : aEntry.children) {
            QVector<img::ResourceNode*> candidates;
            for (auto current : aParent.children()) {
                if (ctrl::matchesIdentity(*current, child)) {
                    candidates.push_back(current);
                }
            }
            if (candidates.count() > 1) {
                // two siblings the identity rule cannot tell apart: reported, and this entry is
                // left alone rather than guessed at (the rest of the reload still happens)
                aOutAmbiguous.push_back(QString("%1 (%2 candidates)").arg(child.name).arg(candidates.count()));
                continue;
            }
            PlanNode node;
            node.entry = &child;
            node.current = candidates.isEmpty() ? nullptr : candidates.front();
            aOut.children.push_back(std::move(node));
        }

        // everything the file no longer mentions is a removal
        for (auto current : aParent.children()) {
            bool found = false;
            for (const auto& child : aEntry.children) {
                if (ctrl::matchesIdentity(*current, child)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                PlanNode node;
                node.current = current;
                aOut.children.push_back(std::move(node));
            }
        }

        // and recurse into the pairs
        for (auto& node : aOut.children) {
            if (node.current && node.entry) {
                buildPlanRecursive(*node.current, *node.entry, node, aOutAmbiguous);
            }
        }
    }

    void ResourceUpdater::applyPlanRecursive(QTreeWidgetItem& aItem, PlanNode& aPlan, ApplyContext& aContext) {
        for (auto& child : aPlan.children) {
            if (child.isAppend()) {
                // the file gained a node: build it exactly as the importer would (same masks,
                // same derivation hook, same declared derived-channel needs) and add it with its
                // whole subtree
                std::function<img::ResourceNode*(ctrl::SourceDocument::Entry&)> buildSubtree =
                    [&](ctrl::SourceDocument::Entry& aEntry) {
                        auto* node = ctrl::buildResourceNode(aContext.document, aEntry);
                        aContext.notifier.event().pushTarget(*node);
                        for (auto& grandChild : aEntry.children) {
                            node->children().pushBack(buildSubtree(grandChild));
                        }
                        return node;
                    };
                auto* newChild =
                    buildSubtree(const_cast<ctrl::SourceDocument::Entry&>(*child.entry));
                RESOURCE_UPDATER_DUMP("append %s", child.entry->name.toUtf8().constData());
                auto* parentItem = res::Item::cast(&aItem);
                XC_PTR_ASSERT(parentItem);
                aContext.commands.push_back(new cmnd::PushBackNewTreeObject<img::ResourceNode>(
                    &parentItem->node().children(), newChild));
                auto* newItem = new res::Item(*aItem.treeWidget(), *newChild, newChild->data().identifier());
                aContext.commands.push_back(new GUITreeItemAppender(aItem, newItem));
                if (child.entry->children.empty()) {
                    continue;
                }
                // the appended subtree's own items (its resources are already in the model)
                std::function<void(QTreeWidgetItem&, const img::ResourceNode&)> addItems =
                    [&](QTreeWidgetItem& aParentItem, const img::ResourceNode& aParentNode) {
                        for (auto grandChild : aParentNode.children()) {
                            auto* grandItem = new res::Item(
                                *aItem.treeWidget(), *grandChild, grandChild->data().identifier());
                            aContext.commands.push_back(new GUITreeItemAppender(aParentItem, grandItem));
                            addItems(*grandItem, *grandChild);
                        }
                    };
                addItems(*newItem, *newChild);
                continue;
            }

            auto* current = child.current;
            XC_PTR_ASSERT(current);
            auto* item = findItemFor(aItem, *current);
            if (!item) {
                // the model is authoritative: if the GUI never showed this node, show it now
                item = new res::Item(*aItem.treeWidget(), *current, current->data().identifier());
                aContext.commands.push_back(new GUITreeItemAppender(aItem, item));
            }
            // NOTE: the node is addressed from here on, never by index into the GUI tree

            if (child.isRemove()) {
                // the file no longer has it. Anything an object node still draws is KEPT: the
                // art may be placed in the timeline even though the source dropped it
                bool keeped = current->isKeeped();
                if (!keeped) {
                    img::ResourceNode::ConstIterator itr(current);
                    while (itr.hasNext()) {
                        if (itr.next()->isKeeped()) {
                            keeped = true;
                            break;
                        }
                    }
                }
                if (keeped) {
                    aContext.keptInUse.push_back(current->data().identifier());
                    continue;
                }
                XC_PTR_ASSERT(current->parent());
                XC_PTR_ASSERT(item->parent());
                aContext.commands.push_back(
                    new cmnd::RemoveTreeByObj<img::ResourceNode>(&current->parent()->children(), current));
                aContext.commands.push_back(new cmnd::GrabDeleteObject<img::ResourceNode>(current));
                RESOURCE_UPDATER_DUMP("remove %s", current->data().identifier().toUtf8().constData());
                aContext.commands.push_back(new GUITreeItemDeleter(*item->parent(), item));
                continue;
            }

            // ---- matched: apply the object side first (it is per object node), then the resource
            QVector<core::ObjectNode*> objects = correspondingObjectNodes(mProject, *current);
            if (objects.isEmpty() && !current->data().isLayer()) {
                // a group whose object node could not be found by name: its own properties
                // (blend mode, opacity, Blend If) cannot be applied, so say so instead of
                // pretending the two sides agree
                aContext.unmatchedGroups.push_back(current->data().identifier());
            }
            for (auto* objectNode : objects) {
                const ctrl::ObjectDiff objDiff = ctrl::diffObject(*objectNode, *child.entry);
                RESOURCE_UPDATER_DUMP(
                    "update %s: visible=%d blend=%d blendIf=%d clip=%d effects=%d fill=%d opacity=%d",
                    objectNode->name().toUtf8().constData(), (int)objDiff.visibility, (int)objDiff.blend,
                    (int)objDiff.blendIf, (int)objDiff.clipping, (int)objDiff.effects,
                    (int)objDiff.fillOpacity, (int)objDiff.opacity);
                if (!aContext.force && !objDiff.any()) {
                    continue;
                }
                auto* renderer = objectNode->renderer();
                if (!renderer) {
                    continue;
                }
                const bool wantVisible = child.entry->visible;
                if (aContext.force || objDiff.visibility) {
                    const bool prev = objectNode->isVisible();
                    aContext.commands.push_back(new cmnd::Delegatable(
                        [objectNode, wantVisible]() { objectNode->setVisibility(wantVisible); },
                        [objectNode, prev]() { objectNode->setVisibility(prev); }));
                }
                if (aContext.force || objDiff.blend) {
                    const img::BlendMode prev = renderer->blendMode();
                    const img::BlendMode next = child.entry->blendMode;
                    aContext.commands.push_back(new cmnd::Delegatable(
                        [renderer, next]() { renderer->setBlendMode(next); },
                        [renderer, prev]() { renderer->setBlendMode(prev); }));
                }
                if (aContext.force || objDiff.blendIf) {
                    const core::BlendIf prev = renderer->blendIf();
                    const core::BlendIf next = child.entry->blendIf;
                    aContext.commands.push_back(new cmnd::Delegatable(
                        [renderer, next]() { renderer->setBlendIf(next); },
                        [renderer, prev]() { renderer->setBlendIf(prev); }));
                }
                if (aContext.force || objDiff.clipping) {
                    const bool prev = renderer->isClipped();
                    const bool next = child.entry->clipped;
                    aContext.commands.push_back(new cmnd::Delegatable(
                        [renderer, next]() { renderer->setClipped(next); },
                        [renderer, prev]() { renderer->setClipped(prev); }));
                }
                if ((aContext.force || objDiff.effects) && objectNode->type() == core::ObjectType_Layer) {
                    const core::LayerEffects prev = renderer->layerEffects();
                    const core::LayerEffects next = child.entry->effects;
                    aContext.commands.push_back(new cmnd::Delegatable(
                        [renderer, next]() { renderer->setLayerEffects(next); },
                        [renderer, prev]() { renderer->setLayerEffects(prev); }));
                }
                if ((aContext.force || objDiff.fillOpacity) && objectNode->type() == core::ObjectType_Layer) {
                    auto* layer = static_cast<core::LayerNode*>(objectNode);
                    const float prev = layer->fillOpacity();
                    const float next = (float)child.entry->fillOpacity255 / 255.0f;
                    aContext.commands.push_back(new cmnd::Delegatable(
                        [layer, next]() { layer->setFillOpacity(next); },
                        [layer, prev]() { layer->setFillOpacity(prev); }));
                }
                if (aContext.force || objDiff.opacity) {
                    auto* timeLine = objectNode->timeLine();
                    auto* key = timeLine
                        ? dynamic_cast<core::OpaKey*>(timeLine->defaultKey(core::TimeKeyType_Opa))
                        : nullptr;
                    if (key) {
                        const float prev = key->opacity();
                        const float next = (float)child.entry->opacity255 / 255.0f;
                        aContext.commands.push_back(new cmnd::Delegatable(
                            [key, next]() { key->setOpacity(next); },
                            [key, prev]() { key->setOpacity(prev); }));
                    }
                }
            }

            // ---- the resource side
            const ctrl::ResourceDiff resDiff = ctrl::diffResource(*current, *child.entry);
            if (aContext.force || resDiff.any()) {
                auto& entry = const_cast<ctrl::SourceDocument::Entry&>(*child.entry);
                img::ResourceNode::Content content;
                content.rect = entry.rect;
                content.pixels = aContext.document.takePixels(entry);
                // The description is the truth about the mask only where it looked at the
                // channel (ctrl::SourceDocument::Entry::hasMaskData): for a PSD it did, so a
                // mask the file no longer has is removed and a changed one is replaced; for an
                // ORA or a plain bitmap it did not, and the mask the artist made in the app
                // stays. This used to be an unconditional `true`, which deleted that mask on
                // every reload of such a file.
                content.setMask = entry.hasMaskData;
                content.hasMask = !entry.mask.isEmpty();
                if (content.setMask && content.hasMask) {
                    content.mask = entry.mask.take();
                }
                content.maskEnabled = entry.maskEnabled;
                content.maskInverted = entry.maskInverted;
                content.setBlend = true;
                content.blend = entry.blendMode;
                content.setVisible = true;
                content.visible = entry.visible;
                if (content.pixels.data) {
                    RESOURCE_UPDATER_DUMP(
                        "content %s: pixels=%d mask=%d blend=%d visible=%d",
                        current->data().identifier().toUtf8().constData(), (int)resDiff.pixels,
                        (int)resDiff.mask, (int)resDiff.blend, (int)resDiff.visible);
                    aContext.notifier.event().pushTarget(*current);
                    aContext.commands.push_back(new ContentSetter(*current, std::move(content)));
                }
            }

            if (resDiff.name) {
                // reported, not applied: the resource's name is this app's own label for it (and
                // the fallback identity), so a rename in the file must not overwrite what the
                // artist called it here - the stable id keeps the correspondence either way
                aContext.renamed.push_back(
                    QString("%1 -> %2").arg(current->data().identifier()).arg(child.entry->name));
            }

            applyPlanRecursive(*item, child, aContext);
        }
    }

    void ResourceUpdater::reconcile(Item& aItem, bool aForce) {
        auto& holder = mProject.resourceHolder();

        img::ResourceNode& node = aItem.node();
        img::ResourceNode& topNode = util::TreeUtil::getTreeRoot(node);
        const QString filePath = holder.findAbsoluteFilePath(topNode);
        if (filePath.isEmpty())
            return;

        // the file, read ONCE into the description the importer consumes
        ctrl::SourceDocument document = describeSource(filePath, true);
        if (!document.isValid()) {
            if (!document.error.isEmpty()) {
                QMessageBox::warning(nullptr, tr("Parse error"), document.error);
            }
            return;
        }
        // the reload reports what it does not draw, exactly as the import does: the file may
        // have gained an effect this build has no model for
        pushEffectNotes(document);

        // the entry the reloaded node corresponds to: the path down from the tree root, matched
        // by identity at every step
        const ctrl::SourceDocument::Entry* targetEntry = &document.top;
        {
            std::list<img::ResourceNode*> path;
            for (auto ptr = &node; ptr->parent(); ptr = ptr->parent()) {
                path.push_front(ptr);
            }
            for (auto pathNode : path) {
                const ctrl::SourceDocument::Entry* found = nullptr;
                for (const auto& child : targetEntry->children) {
                    if (ctrl::matchesIdentity(*pathNode, child)) {
                        found = &child;
                        break;
                    }
                }
                if (!found) {
                    QMessageBox::warning(
                        nullptr, tr("Operation Error"),
                        tr("Failed to find a corresponding node.") + " (" + pathNode->data().identifier() + ")");
                    return;
                }
                targetEntry = found;
            }
        }

        RESOURCE_UPDATER_DUMP("begin reload");

        PlanNode plan;
        plan.current = &node;
        plan.entry = targetEntry;
        QStringList ambiguous;
        buildPlanRecursive(node, *targetEntry, plan, ambiguous);

        auto& stack = mProject.commandStack();
        auto* notifier = new ModificationNotifier(mViaPoint, mProject, aItem.treePos());
        notifier->event().setRoot(node);

        ApplyContext context{ *notifier, document, {}, aForce };
        context.ambiguous = ambiguous;
        applyPlanRecursive(aItem, plan, context);
        RESOURCE_UPDATER_DUMP(
            "reload command count %d (ambiguous %d, renamed %d, kept %d)", (int)context.commands.size(),
            (int)context.ambiguous.size(), (int)context.renamed.size(), (int)context.keptInUse.size());

        if (!context.commands.empty()) {
            cmnd::ScopedMacro macro(stack, CmndName::tr("Reload images"));
            macro.grabListener(notifier);
            for (auto* command : context.commands) {
                stack.push(command);
            }
            stack.push(mProject.objectTree().createResourceUpdater(notifier->event()));
        } else {
            // nothing to do: no command, no undo step, no edit to the project
            delete notifier;
        }

        RESOURCE_UPDATER_DUMP("end reload");

        // What the reload did NOT take from the file is reported - to the app's log, not to a
        // modal box (this is informational and can happen on every reload), and never silently:
        // a difference the plan detected but does not apply is exactly what this list is for.
        if (!context.ambiguous.isEmpty()) {
            mViaPoint.pushLog(
                tr("Reload: left alone, two siblings share an identity: ") + context.ambiguous.join(", "),
                ctrl::UILogType_Warn);
        }
        if (!context.renamed.isEmpty()) {
            mViaPoint.pushLog(
                tr("Reload: named differently in the file, the project keeps its own name: ")
                    + context.renamed.join(", "),
                ctrl::UILogType_Info);
        }
        if (!context.keptInUse.isEmpty()) {
            mViaPoint.pushLog(
                tr("Reload: no longer in the file but the timeline draws them, so they were kept: ")
                    + context.keptInUse.join(", "),
                ctrl::UILogType_Warn);
        }
        if (!context.unmatchedGroups.isEmpty()) {
            mViaPoint.pushLog(
                tr("Reload: no object node matches these groups, so their own properties were left "
                   "alone: ")
                    + context.unmatchedGroups.join(", "),
                ctrl::UILogType_Warn);
        }
    }

    void ResourceUpdater::reload(Item& aItem) { reconcile(aItem, false); }

    void ResourceUpdater::forceReload(Item& aItem) { reconcile(aItem, true); }

    //-------------------------------------------------------------------------------------------------
    void ResourceUpdater::remove(QTreeWidget& aTree, Item& aTopItem) {
        auto& holder = mProject.resourceHolder();

        img::ResourceNode& node = aTopItem.node();
        img::ResourceNode& topNode = util::TreeUtil::getTreeRoot(node);
        XC_ASSERT(&node == &topNode);

        // check reference
        {
            img::ResourceNode::ConstIterator itr(&topNode);
            while (itr.hasNext()) {
                if (itr.next()->isKeeped()) {
                    QMessageBox::warning(
                        nullptr, tr("Operation Error"), tr("Some layers are still referenced by objects.")
                    );
                    return;
                }
            }
        }

        // find index
        int index = 0;
        for (auto& tree : holder.imageTrees()) {
            if (tree.topNode == &topNode)
                break;
            ++index;
        }
        if (index >= (int)holder.imageTrees().size())
            return;


        // find gui index
        int guiIndex = 0;
        for (int i = 0; i < aTree.topLevelItemCount(); ++i) {
            if (aTree.topLevelItem(i) == (QTreeWidgetItem*)(&aTopItem))
                break;
            ++guiIndex;
        }
        XC_ASSERT(guiIndex < aTree.topLevelItemCount());

        // invoke
        {
            auto& stack = mProject.commandStack();
            auto& holder = mProject.resourceHolder();

            cmnd::ScopedMacro macro(stack, CmndName::tr("Delete images"));

            // notifier
            auto notifier = new DeleteNotifier(mViaPoint, mProject);
            notifier->event().setSingleTarget(node);
            macro.grabListener(notifier);

            stack.push(new TreeDeleter(holder, index));
            stack.push(new GUITreeDeleter(aTree, guiIndex));
        }
    }

} // namespace gui::res

