#include <QFile>
#include "gl/Global.h"
#include "gl/ExtendShader.h"
#include "core/BoneInfluenceMap.h"
#include "core/MeshTransformerResource.h"
#include "core/PosePalette.h"

namespace core {

//-------------------------------------------------------------------------------------------------
MeshTransformerResource::MeshTransformerResource(): mCode(), mPrograms() {}

void MeshTransformerResource::setup(const QString& aShaderPath) { loadFile(aShaderPath, mCode); }

int MeshTransformerResource::programIndex(bool aUseSkinning, bool aUseDualQuaternion, int aSlotCount) {
    if (!aUseSkinning)
        return 0;
    XC_ASSERT(aSlotCount >= 1 && aSlotCount <= BoneInfluenceMap::kBonePerVtxMaxAll);
    return 1 + (aUseDualQuaternion ? 8 : 0) + (aSlotCount - 1);
}

gl::EasyShaderProgram& MeshTransformerResource::program(
    bool aUseSkinning, bool aUseDualQuaternion, int aSlotCount
) {
    const int index = programIndex(aUseSkinning, aUseDualQuaternion, aUseSkinning ? aSlotCount : 1);
    std::unique_ptr<gl::EasyShaderProgram>& slot = mPrograms[index];
    if (!slot) {
        slot.reset(new gl::EasyShaderProgram());
        buildShader(*slot, mCode, aUseSkinning, aUseDualQuaternion, aUseSkinning ? aSlotCount : 1);
    }
    return *slot;
}

void MeshTransformerResource::loadFile(const QString& aPath, QString& aDstCode) {
    QFile file(aPath);

    if (!file.open(QIODevice::ReadOnly)) {
        XC_FATAL_ERROR("FileIO Error", file.errorString(), aPath);
    }
    QTextStream in(&file);
    aDstCode = in.readAll();
}

void MeshTransformerResource::buildShader(
    gl::EasyShaderProgram& aProgram,
    const QString& aCode,
    bool aUseSkinning,
    bool aUseDualQuaternion,
    int aSlotCount
) {
    gl::Global::Functions& ggl = gl::Global::functions();

    gl::ExtendShader source;

    // parse shader source
    source.openFromTextVert(aCode);

    // set variation
    source.setVariationValue("USE_SKINNING", QString::number(aUseSkinning ? 1 : 0));
    source.setVariationValue("USE_DUAL_QUATERNION", QString::number(aUseDualQuaternion ? 1 : 0));
    source.setVariationValue("INFLUENCE_SLOTS", QString::number(aSlotCount));
    // The uniform array is sized by the same constant that caps the CPU
    // palette; a literal in the shader would be a second place to change.
    source.setVariationValue("MAX_BONES", QString::number(PosePalette::kMaxCount));

    // resolve variation
    if (!source.resolveVariation()) {
        XC_FATAL_ERROR("OpenGL Error", "Failed to resolve shader variation.", source.log());
    }

    // set shader source
    if (!aProgram.setVertexSource(source)) {
        XC_FATAL_ERROR("OpenGL Error", "Failed to compile vertex shader.", aProgram.log());
    }

    // feedback
    {
        static const GLchar* kVaryings[] = {"outPosition", "outXArrow", "outYArrow"};
        ggl.glTransformFeedbackVaryings(aProgram.id(), 3, kVaryings, GL_SEPARATE_ATTRIBS);
    }

    // link shader
    if (!aProgram.link()) {
        XC_FATAL_ERROR("OpenGL Error", "Failed to link shader.", aProgram.log());
    }
    if(ggl.glGetError() != GL_NO_ERROR){
        qDebug() << ggl.glGetError();
    }
    XC_ASSERT(ggl.glGetError() == GL_NO_ERROR);
}

} // namespace core
