#ifndef CTRL_IMAGEFILELOADER_H
#define CTRL_IMAGEFILELOADER_H

#include <QFileInfo>
#include <QString>
#include <QScopedPointer>
#include "util/IProgressReporter.h"
#include "gl/DeviceInfo.h"
#include "core/Project.h"
#include "core/ObjectTree.h"
#include "core/FolderNode.h"

namespace ctrl {

class SourceDocument;

class ImageFileLoader {
public:
    explicit ImageFileLoader(gl::DeviceInfo  aDeviceInfo);

    void setCanvasSize(const QSize& aSize, bool aForce);

    bool load(const QString& aPath, core::Project& aProject, util::IProgressReporter& aReporter);

    // How to import an .ora: Ask shows the interactive layered/merged prompt (the default,
    // the app behavior); Layered/Merged skip the dialog entirely (headless callers such as
    // the test harness have no user to answer it).
    enum class OraImportMode { Ask, Layered, Merged };
    void setOraImportMode(OraImportMode aMode) { mOraImportMode = aMode; }

    [[nodiscard]] const QString& log() const { return mLog; }

private:
    bool createEmptyCanvas(core::Project& aProject, const QString& aTopName, const QSize& aCanvasSize);

    // Appends what the file asks for and this build cannot draw (ctrl::undrawnEffectNotes) to
    // the log, so a "success" says what the picture is missing instead of implying it is the
    // file's. Every path that finishes an import calls it.
    void appendUndrawnEffects(const SourceDocument& aDocument);

    bool loadPsd(core::Project& aProject, util::IProgressReporter& aReporter);

    bool loadOra(core::Project& aProject, util::IProgressReporter& aReporter);

    bool loadImage(core::Project& aProject, util::IProgressReporter& aReporter);

    bool checkTextureSizeError(uint32 aWidth, uint32 aHeight);

    QString mLog;
    QFileInfo mFileInfo;
    gl::DeviceInfo mGLDeviceInfo;
    QSize mCanvasSize;
    bool mForceCanvasSize;
    OraImportMode mOraImportMode = OraImportMode::Ask;
};

} // namespace ctrl

#endif // CTRL_IMAGEFILELOADER_H
