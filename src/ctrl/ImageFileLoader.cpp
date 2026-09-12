#include <fstream>
#include <utility>
#include "XC.h"
#include "util/TextUtil.h"
#include "img/PSDReader.h"
#include "img/PSDUtil.h"
#include "img/ResourceNode.h"
#include "core/LayerNode.h"
#include "core/FolderNode.h"
#include "core/DistanceField.h"
#include "core/LayerEffects.h"
#include "core/ObjectNodeUtil.h"
#include "ctrl/ImageFileLoader.h"
#include "ctrl/SourceDocument.h"
#include "gl/Global.h"


#include <QMessageBox>

using namespace core;

namespace ctrl {

//-------------------------------------------------------------------------------------------------
ImageFileLoader::ImageFileLoader(gl::DeviceInfo  aDeviceInfo):
    mLog(), mFileInfo(), mGLDeviceInfo(std::move(aDeviceInfo)), mCanvasSize(512, 512), mForceCanvasSize(false) {}

void ImageFileLoader::setCanvasSize(const QSize& aSize, bool aForce) {
    if (aSize.width() <= 0 || aSize.height() <= 0)
        return;

    mCanvasSize = aSize;
    mForceCanvasSize = aForce;
}

bool ImageFileLoader::load(const QString& aPath, core::Project& aProject, util::IProgressReporter& aReporter) {
    XC_DEBUG_REPORT("------------------------------------------");

    mFileInfo = QFileInfo(aPath);
    const QString suffix = mFileInfo.suffix();

    if (aPath.isEmpty() || !mFileInfo.isFile()) {
        return createEmptyCanvas(aProject, "topnode", mCanvasSize);
    }
    if (suffix == "psd") {
        return loadPsd(aProject, aReporter);
    }
    if (suffix == "ora") {
        return loadOra(aProject, aReporter);
    }
    return loadImage(aProject, aReporter);
}

//-------------------------------------------------------------------------------------------------
void ImageFileLoader::appendUndrawnEffects(const SourceDocument& aDocument) {
    // APPENDS to whatever the log already says, so a caller must set "success" BEFORE calling
    // this. An earlier revision of this change called it first, and the notes were wiped by the
    // very line the S20 suite now asserts them next to - which is why that assertion exists.
    const QStringList notes = undrawnEffectNotes(aDocument);
    for (const QString& note : notes) {
        mLog += "\n" + note;
    }
}

//-------------------------------------------------------------------------------------------------
bool ImageFileLoader::createEmptyCanvas(core::Project& aProject, const QString& aTopName, const QSize& aCanvasSize) {
    // check the image has valid size as a texture.
    if (!checkTextureSizeError(static_cast<uint32>(aCanvasSize.width()), static_cast<uint32>(aCanvasSize.height()))) {
        mLog = "invalid canvas size";
        return false;
    }
    XC_DEBUG_REPORT("canvas size = (%d, %d)", aCanvasSize.width(), aCanvasSize.height());

    // set canvas size
    aProject.attribute().setImageSize(aCanvasSize);

    // create tree top node
    FolderNode* topNode = createTopNode(aTopName, QRect(QPoint(0, 0), aCanvasSize));
    aProject.objectTree().grabTopNode(topNode);

    mLog = "success";
    return true;
}

//-------------------------------------------------------------------------------------------------
bool ImageFileLoader::loadImage(core::Project& aProject, util::IProgressReporter& aReporter) {
    aReporter.setSection("Loading the Image File...");
    aReporter.setMaximum(1);
    aReporter.setProgress(0);

    QImage image(mFileInfo.filePath());
    if (image.isNull()) {
        mLog = "Failed to load image file";
        return false;
    }

    // one description, then the same two builders the PSD import and every reload use
    SourceDocument document = describeImage(image, mFileInfo.baseName(), true);
    if (!document.isValid()) {
        mLog = document.error;
        return false;
    }
    if (mForceCanvasSize) {
        document.canvasSize = mCanvasSize;
    }
    if (!checkTextureSizeError((uint32)document.canvasSize.width(), (uint32)document.canvasSize.height())) {
        mLog = "invalid canvas size";
        return false;
    }

    aProject.attribute().setImageSize(document.canvasSize);
    FolderNode* topNode = createTopNode(document.documentName, QRect(QPoint(), document.canvasSize));
    aProject.objectTree().grabTopNode(topNode);

    auto* resTree = buildResourceTree(document);
    aProject.resourceHolder().pushImageTree(*resTree, mFileInfo.absoluteFilePath());
    buildObjectTree(document, *topNode, *resTree, aProject);
    setDefaultPosturesFromInitialRects(*topNode);

    aReporter.setProgress(1);
    mLog = "success";
    appendUndrawnEffects(document);
    return true;
}

//-------------------------------------------------------------------------------------------------
bool ImageFileLoader::loadPsd(core::Project& aProject, util::IProgressReporter& aReporter) {
    using img::PSDFormat;
    using img::PSDReader;

    aReporter.setSection("Loading PSD file...");
    aReporter.setMaximum(1);
    aReporter.setProgress(0);

    // open file
    QScopedPointer<std::ifstream> file;
    {
        auto path = mFileInfo.filePath();
        file.reset(new std::ifstream(path.toLocal8Bit(), std::ios::binary));
        XC_DEBUG_REPORT() << "image path =" << path;

        if (file->fail()) {
            mLog = "Can not find a file.";
            return false;
        }
    }

    // read psd
    PSDReader reader(*file);

    if (reader.resultCode() != PSDReader::ResultCode_Success) {
        mLog = "error(" + QString::number(reader.resultCode()) + ") " + QString::fromStdString(reader.resultMessage());
        return false;
    }
    aReporter.setProgress(1);
    file->close(); // do not use anymore

    // the renderer only produces 8-bit RGBA, so reject the color modes / bit depths
    // it cannot represent with a clear message instead of failing later on decode
    const auto& header = reader.format()->header();
    if (header.mode != PSDFormat::ColorMode_RGB) {
        mLog = QStringLiteral("Unsupported PSD color mode: %1. Only RGB is supported.")
                   .arg(QString::fromLatin1(PSDFormat::colorModeName((PSDFormat::ColorMode)header.mode)));
        return false;
    }
    if (header.depth != 8) {
        mLog = QStringLiteral("Unsupported PSD bit depth: %1-bit. Only 8-bit is supported.")
                   .arg(header.depth);
        return false;
    }

    auto canvasSize =
        mForceCanvasSize ? mCanvasSize : QSize((int)header.width, (int)header.height);
    if (!checkTextureSizeError((uint32)canvasSize.width(), (uint32)canvasSize.height())) {
        mLog = "invalid canvas size";
        return false;
    }
    XC_DEBUG_REPORT("image size = (%d, %d)", canvasSize.width(), canvasSize.height());

    // update reporter
    aReporter.setSection(QCoreApplication::translate("Image Loader", "Building an Object Tree..."));
    aReporter.setMaximum(reader.format()->layerAndMaskInfo().layerCount);
    aReporter.setProgress(0);

    // ONE description of the document, consumed here and by a source reload (see
    // ctrl/SourceDocument.h): the interpretation of the container lives there, so the two
    // paths cannot drift apart, and the description owns everything the trees need.
    SourceDocument document = describePsd(
        *reader.format(), mFileInfo.baseName(), true,
        [&](int aDone, int) { aReporter.setProgress(aDone); }
    );
    if (!document.isValid()) {
        mLog = document.error;
        return false;
    }
    document.canvasSize = canvasSize;

    aProject.attribute().setImageSize(canvasSize);

    // create tree top node
    FolderNode* topNode = createTopNode(mFileInfo.baseName(), QRect(QPoint(), canvasSize));
    aProject.objectTree().grabTopNode(topNode);

    // resource tree (the same builder a reload uses) then object tree
    auto* resTree = buildResourceTree(document);
    aProject.resourceHolder().pushImageTree(*resTree, mFileInfo.absoluteFilePath());
    buildObjectTree(document, *topNode, *resTree, aProject);

    // setup default positions
    setDefaultPosturesFromInitialRects(*topNode);

    XC_DEBUG_REPORT("------------------------------------------");

    mLog = "success";
    appendUndrawnEffects(document);
    return true;
}

QString tr(const QString& str){
    return QCoreApplication::translate("image_file_loader", str.toStdString().c_str());
}
bool ImageFileLoader::loadOra(Project& aProject, util::IProgressReporter& aReporter) {
    aReporter.setSection("Loading ORA file...");
    aReporter.setMaximum(100);
    aReporter.setProgress(0);

    const QString path = mFileInfo.filePath();
    miniz_cpp::zip_file ora(path.toStdString());
    try {
        if (!ora.has_file("mimetype")) {
            mLog = "Unable to find the mimetype of the selected file.";
            return false;
        }
        if (ora.read("mimetype") != "image/openraster") {
            mLog = "The MIME type is not valid.";
            return false;
        }
    } catch (...) {
        mLog = std::string("Unable to unzip " + path.toStdString() + " into memory, aborting.").c_str();
        return false;
    }

    // The layered/merged choice is interactive by default (mOraImportMode == Ask); headless
    // callers pre-select it via setOraImportMode so no dialog is shown.
    bool merged;
    if (mOraImportMode == OraImportMode::Ask) {
        QMessageBox loadMerged;
        loadMerged.setWindowTitle(tr("Select ORA file type"));
        loadMerged.setText(tr("How do you wish to load this ORA file?"));
        QAbstractButton* layerButton = loadMerged.addButton(tr("Load layered"), QMessageBox::YesRole);
        QAbstractButton* mergeButton = loadMerged.addButton(tr("Load merged"), QMessageBox::YesRole);
        QAbstractButton* cancelButton = loadMerged.addButton(tr("Cancel file load"), QMessageBox::NoRole);
        loadMerged.exec();
        if (loadMerged.clickedButton() != mergeButton && loadMerged.clickedButton() != layerButton) {
            mLog = "User cancelled image load";
            return false;
        }
        merged = loadMerged.clickedButton() == mergeButton;
    } else {
        merged = mOraImportMode == OraImportMode::Merged;
    }

    aReporter.setProgress(20);

    // one description (the reader interprets stack.xml and the zipped layers), then the shared
    // builders - the same ones the PSD import and every reload use
    SourceDocument document = describeOra(
        path.toStdString(), merged, mFileInfo.baseName(), true,
        [&](int aDone, int aTotal) { aReporter.setProgress(aTotal > 0 ? aDone * 40 / aTotal + 20 : 60); }
    );
    if (!document.isValid()) {
        mLog = document.error;
        return false;
    }
    if (mForceCanvasSize) {
        document.canvasSize = mCanvasSize;
    }
    if (!checkTextureSizeError((uint32)document.canvasSize.width(), (uint32)document.canvasSize.height())) {
        mLog = "invalid canvas size";
        return false;
    }
    aReporter.setProgress(60);

    aProject.attribute().setImageSize(document.canvasSize);
    FolderNode* topNode = createTopNode(document.documentName, QRect(QPoint(), document.canvasSize));
    aProject.objectTree().grabTopNode(topNode);

    auto* resTree = buildResourceTree(document);
    aProject.resourceHolder().pushImageTree(*resTree, mFileInfo.absoluteFilePath());
    buildObjectTree(document, *topNode, *resTree, aProject);
    setDefaultPosturesFromInitialRects(*topNode);

    aReporter.setProgress(100);
    mLog = "Success";
    appendUndrawnEffects(document);
    return true;
}

bool ImageFileLoader::checkTextureSizeError(uint32 aWidth, uint32 aHeight) {
    const auto maxSize = (uint32)mGLDeviceInfo.maxTextureSize;
    if (maxSize < aWidth || maxSize < aHeight) {
        mLog = QString("The image size over the max texture size of your current device. ") + "image size(" +
            QString::number(aWidth) + ", " + QString::number(aHeight) + "), " + "max size(" + QString::number(maxSize) +
            ", " + QString::number(maxSize) + ")";
        return false;
    }
    return true;
}

} // namespace ctrl
