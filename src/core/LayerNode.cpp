#include <utility>
#include <QMatrix4x4>
#include <QOpenGLFunctions>
#include <QOpenGLContext>
#include "XC.h"
#include "gl/Global.h"
#include "gl/ExtendShader.h"
#include "gl/Util.h"
#include "img/ResourceNode.h"
#include "img/BlendMode.h"
#include "core/LayerNode.h"
#include "core/ObjectNodeUtil.h"
#include "core/TimeKeyExpans.h"
#include "core/DepthKey.h"
#include "core/ResourceEvent.h"
#include "core/ResourceUpdatingWorkspace.h"
#include "core/FFDKeyUpdater.h"
#include "core/ImageKeyUpdater.h"
#include "core/ClippingFrame.h"
#include "core/DestinationTexturizer.h"

namespace core
{

LayerNode::LayerNode(const QString& aName, ShaderHolder& aShaderHolder)
    : mName(aName)
    , mIsVisible(true)
    , mIsSlimmedDown()
    , mInitialRect()
    , mTimeLine()
    , mShaderHolder(aShaderHolder)
    , mIsClipped()
    , mMeshTransformer("./data/shader/MeshTransform.glslex")
    , mCurrentMesh()
    , mClippees()
{
}

LayerNode::LayerNode(const LayerNode &aRhs)
    : mName(aRhs.mName)
    , mIsVisible(aRhs.mIsVisible)
    , mIsSlimmedDown(aRhs.mIsSlimmedDown)
    , mInitialRect(aRhs.mInitialRect)
    , mTimeLine(aRhs.mTimeLine)
    , mShaderHolder(aRhs.mShaderHolder)
    , mIsClipped(aRhs.isClipped())
    , mMeshTransformer("./data/shader/MeshTransform.glslex")
    , mCurrentMesh()
    , mClippees()
{

}

void LayerNode::setDefaultImage(const img::ResourceHandle& aHandle)
{
    setDefaultImage(aHandle, aHandle->blendMode());
}

void LayerNode::setDefaultImage(const img::ResourceHandle& aHandle, img::BlendMode aBlendMode)
{
    XC_ASSERT(aHandle);
    XC_PTR_ASSERT(aHandle->image().data());
    XC_ASSERT(aHandle->image().pixelSize().isValid());
    if (!aHandle || !aHandle->image().data()) return; // fail safe

    auto key = new ImageKey();
    mTimeLine.grabDefaultKey(TimeKeyType_Image, key);
    key->setImage(aHandle, aBlendMode);
    key->resetGridMesh();
    key->setImageOffsetByCenter();

    mShaderHolder.reserveShaders(aBlendMode);
    mShaderHolder.reserveGridShader();
    mShaderHolder.reserveClipperShaders();
}

void LayerNode::setDefaultPosture(const QVector2D& aPos)
{
    {
        auto key = (MoveKey*)mTimeLine.defaultKey(TimeKeyType_Move);
        if (!key)
        {
            key = new MoveKey();
            mTimeLine.grabDefaultKey(TimeKeyType_Move, key);
        }
        key->data().setPos(aPos);
    }
    {
        auto key = (RotateKey*)mTimeLine.defaultKey(TimeKeyType_Rotate);
        if (!key)
        {
            key = new RotateKey();
            mTimeLine.grabDefaultKey(TimeKeyType_Rotate, key);
        }
    }
    {
        auto key = (ScaleKey*)mTimeLine.defaultKey(TimeKeyType_Scale);
        if (!key)
        {
            key = new ScaleKey();
            mTimeLine.grabDefaultKey(TimeKeyType_Scale, key);
        }
    }
}

void LayerNode::setDefaultDepth(float aValue)
{
    auto key = (DepthKey*)mTimeLine.defaultKey(TimeKeyType_Depth);
    if (!key)
    {
        key = new DepthKey();
        mTimeLine.grabDefaultKey(TimeKeyType_Depth, key);
    }
    key->setDepth(aValue);
}

void LayerNode::setDefaultOpacity(float aValue)
{
    auto key = (OpaKey*)mTimeLine.defaultKey(TimeKeyType_Opa);
    if (!key)
    {
        key = new OpaKey();
        mTimeLine.grabDefaultKey(TimeKeyType_Opa, key);
    }
    key->setOpacity(aValue);
}

float LayerNode::initialDepth() const
{
    auto key = (DepthKey*)mTimeLine.defaultKey(TimeKeyType_Depth);
    return key ? key->depth() : 0.0f;
}

void LayerNode::setClipped(bool aIsClipped)
{
    mIsClipped = aIsClipped;
}

bool LayerNode::isClipper() const
{
    if (mIsClipped) return false;

    auto prev = this->prevSib();
    if (!prev || !prev->renderer() || !prev->renderer()->isClipped())
    {
        return false;
    }
    return true;
}

img::BlendMode LayerNode::blendMode() const
{
    auto key = (ImageKey*)mTimeLine.defaultKey(TimeKeyType_Image);
    return key ? key->data().blendMode() : img::BlendMode_Normal;
}

void LayerNode::setBlendMode(img::BlendMode aMode)
{
    auto key = (ImageKey*)mTimeLine.defaultKey(TimeKeyType_Image);
    if (key)
    {
        key->data().setBlendMode(aMode);
        mShaderHolder.reserveShaders(aMode);
    }
}

void LayerNode::prerender(const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor)
{
    mCurrentMesh = nullptr;

    if (!mIsVisible) return;

    // transform shape by current keys
    transformShape(aInfo, aAccessor);
}

void LayerNode::render(const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor)
{
    if (!mIsVisible) return;

    if (aAccessor.get(mTimeLine).opa().isZero()) return;

    // render shape
    renderShape(aInfo, aAccessor);

    if (aInfo.isGrid) return;

    // render clippees
    renderClippees(aInfo, aAccessor);
}

void LayerNode::renderClippees(
        const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor)
{
    if (!aInfo.clippingFrame || !isClipper()) return;

    // reset clippees
    ObjectNodeUtil::collectRenderClippees(*this, mClippees, aAccessor);

    // clipping frame
    auto& frame = *aInfo.clippingFrame;

    const uint8 clippingId = frame.forwardClippingId();

    RenderInfo childInfo = aInfo;
    childInfo.clippingId = clippingId;

    uint32 stamp = frame.renderStamp() + 1;

    for (auto clippee : mClippees)
    {
        XC_PTR_ASSERT(clippee.renderer);

        // write clipper as necessary
        if (stamp != frame.renderStamp())
        {
            renderClipper(aInfo, aAccessor, clippingId);
            stamp = frame.renderStamp();
        }

        // render child
        clippee.renderer->render(childInfo, aAccessor);
    }
}

void LayerNode::renderClipper(
        const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor, uint8 aClipperId)
{
    if (!mIsVisible || !aInfo.clippingFrame) return;
    if (!mCurrentMesh) return;

    gl::Global::Functions& ggl = gl::Global::functions();
    auto& shader = mShaderHolder.clipperShader(aInfo.clippingId != 0);
    auto& expans = aAccessor.get(mTimeLine);

    if (!expans.areaTexture()) return;
    auto textureId = expans.areaTexture()->id();
    auto textureSize = expans.areaTexture()->size();
    auto texCoordOffset = mCurrentMesh->originOffset() - expans.imageOffset();

    core::ClippingFrame& frame = *aInfo.clippingFrame;
    frame.updateRenderStamp();

    // bind framebuffer
    frame.bind();
    frame.setupDrawBuffers();

    // bind textures
    ggl.glActiveTexture(GL_TEXTURE0);
    ggl.glBindTexture(GL_TEXTURE_2D, textureId);
    ggl.glActiveTexture(GL_TEXTURE1);
    ggl.glBindTexture(GL_TEXTURE_2D, frame.texture().id());

    // view matrix
    const QMatrix4x4 viewMatrix = aInfo.camera.viewMatrix();
    // color
    const float opacity = expans.worldOpacity();
    QColor color(255, 255, 255, xc_clamp((int)(255 * opacity), 0, 255));
    {
        shader.bind();

        shader.setAttributeBuffer("inPosition", mMeshTransformer.positions(), GL_FLOAT, 3);
        shader.setAttributeArray("inTexCoord", mCurrentMesh->texCoords(), mCurrentMesh->vertexCount());

        shader.setUniformValue("uViewMatrix", viewMatrix);
        shader.setUniformValue("uScreenSize", QSizeF(aInfo.camera.deviceScreenSize()));
        shader.setUniformValue("uImageSize", QSizeF(textureSize));
        shader.setUniformValue("uTexCoordOffset", texCoordOffset);
        shader.setUniformValue("uColor", color);
        shader.setUniformValue("uClipperId", (int)aClipperId);
        shader.setUniformValue("uTexture", 0);
        shader.setUniformValue("uDestTexture", 1);

        if (aInfo.clippingId != 0)
        {
            shader.setUniformValue("uClippingId", (int)aInfo.clippingId);
        }

        gl::Util::drawElements(
                    mCurrentMesh->primitiveMode(),
                    GL_UNSIGNED_INT,
                    mCurrentMesh->getIndexBuffer());

        shader.release();
    }
    // unbind texture
    ggl.glActiveTexture(GL_TEXTURE0);
    ggl.glBindTexture(GL_TEXTURE_2D, 0);

    // release framebuffer
    frame.release();

    // bind default framebuffer
    ggl.glBindFramebuffer(GL_FRAMEBUFFER, aInfo.framebuffer);

    ggl.glFlush();
}

void LayerNode::transformShape(
        const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor)
{
    auto& expans = aAccessor.get(mTimeLine);

    // current mesh
    LayerMesh* mesh = expans.ffdMesh();
    if (!mesh || mesh->vertexCount() <= 0) return;

    // positions
    util::ArrayBlock<const gl::Vector3> positions;
    bool useInfluence = true;

    if (aInfo.originMesh) // ignore mesh deforming
    {
        useInfluence = false;
        if (expans.areaImageKey())
        {
            mesh = &(expans.areaImageKey()->data().gridMesh());
            if (!mesh || mesh->vertexCount() <= 0) return;
        }
        positions = util::ArrayBlock<const gl::Vector3>(
                    mesh->positions(), mesh->vertexCount());
    }
    else
    {
        useInfluence = (mesh == expans.bone().targetMesh());
        positions = util::ArrayBlock<const gl::Vector3>(
                    expans.ffd().positions(), expans.ffd().count());
        XC_MSG_ASSERT(mesh->vertexCount() == positions.count(),
                      "vtx count = %d, %d", mesh->vertexCount(), positions.count());
    }
    XC_ASSERT(positions);

    // transform
    mMeshTransformer.callGL(
                expans, mesh->getMeshBuffer(), mesh->originOffset(),
                positions, aInfo.nonPosed, useInfluence);

    mCurrentMesh = mesh;
}

void LayerNode::renderShape(
        const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor)
{
    if (!mCurrentMesh) return;

    gl::Global::Functions& ggl = gl::Global::functions();
    const bool isClippee = (aInfo.clippingFrame && aInfo.clippingId != 0);

    auto& expans = aAccessor.get(mTimeLine);
    if (!expans.areaImageKey() || !expans.areaTexture()) return;

    auto textureId = expans.areaTexture()->id();
    auto textureSize = expans.areaTexture()->size();
    auto texCoordOffset = mCurrentMesh->originOffset() - expans.imageOffset();
    auto blendMode = expans.blendMode();
    const QMatrix4x4 viewMatrix = aInfo.camera.viewMatrix();


    auto& shader = aInfo.isGrid ?
                mShaderHolder.gridShader() :
                mShaderHolder.shader(blendMode, isClippee);

    // update destination color
    XC_PTR_ASSERT(aInfo.destTexturizer);
    auto destTextureId = aInfo.destTexturizer->texture().id();
    if (!aInfo.isGrid && blendMode != img::BlendMode_Normal)
    {
        aInfo.destTexturizer->update(
                    aInfo.framebuffer, aInfo.dest, viewMatrix,
                    *mCurrentMesh, mMeshTransformer.positions());
    }

    if (aInfo.isGrid)
    {
        ggl.glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    else
    {
        // blend func
        ggl.glEnable(GL_BLEND);
        ggl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);

        ggl.glActiveTexture(GL_TEXTURE0);
        ggl.glBindTexture(GL_TEXTURE_2D, textureId);
        ggl.glActiveTexture(GL_TEXTURE1);
        ggl.glBindTexture(GL_TEXTURE_2D, destTextureId);

        if (isClippee)
        {
            ggl.glActiveTexture(GL_TEXTURE2);
            ggl.glBindTexture(GL_TEXTURE_2D, aInfo.clippingFrame->texture().id());
        }
    }

    {
        const float opacity = expans.worldOpacity();
        QColor color(255, 255, 255, xc_clamp((int)(255 * opacity), 0, 255));
        if (aInfo.isGrid) color = QColor(Qt::black);

        shader.bind();

        shader.setAttributeBuffer("inPosition", mMeshTransformer.positions(), GL_FLOAT, 3);
        shader.setAttributeArray("inTexCoord", mCurrentMesh->texCoords(), mCurrentMesh->vertexCount());

        shader.setUniformValue("uViewMatrix", viewMatrix);
        shader.setUniformValue("uScreenSize", QSizeF(aInfo.camera.deviceScreenSize()));
        shader.setUniformValue("uImageSize", QSizeF(textureSize));
        shader.setUniformValue("uTexCoordOffset", texCoordOffset);
        shader.setUniformValue("uColor", color);
        shader.setUniformValue("uTexture", 0);
        shader.setUniformValue("uDestTexture", 1);

        if (isClippee)
        {
            shader.setUniformValue("uClippingId", (int)aInfo.clippingId);
            shader.setUniformValue("uClippingTexture", 2);
        }

        gl::Util::drawElements(
                    mCurrentMesh->primitiveMode(),
                    GL_UNSIGNED_INT,
                    mCurrentMesh->getIndexBuffer());

        shader.release();
    }

    if (aInfo.isGrid)
    {
        ggl.glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    else
    {
        ggl.glActiveTexture(GL_TEXTURE0);
        ggl.glBindTexture(GL_TEXTURE_2D, 0);

        // blend func
        ggl.glDisable(GL_BLEND);
    }

    ggl.glFlush();
}

cmnd::Vector LayerNode::createResourceUpdater(const ResourceEvent& aEvent)
{
    cmnd::Vector result;

    if (aEvent.type() != ResourceEvent::Type_Reload)
    {
        return result;
    }

    ResourceUpdatingWorkspacePtr workspace = std::make_shared<ResourceUpdatingWorkspace>();
    const bool createTransitions = !mTimeLine.isEmpty(TimeKeyType_FFD);

    // image key
    result.push(ImageKeyUpdater::createResourceUpdater(*this, aEvent, workspace, createTransitions));

    // ffd key should be called finally
    if (createTransitions)
    {
        result.push(FFDKeyUpdater::createResourceUpdater(*this, workspace));
    }

    return result;
}

ObjectNode *LayerNode::createClone() const
{
    return new LayerNode(*this);
}

bool LayerNode::serialize(Serializer& aOut) const
{
    static const std::array<uint8, 8> kSignature =
        { 'L', 'a', 'y', 'e', 'r', 'N', 'd', '_' };

    // block begin
    auto pos = aOut.beginBlock(kSignature);

    // name
    aOut.write(mName);
    // visibility
    aOut.write(mIsVisible);
    // slim-down
    aOut.write(mIsSlimmedDown);
    // initial rect
    aOut.write(mInitialRect);
    // clipping
    aOut.write(mIsClipped);
    // timeline
    if (!mTimeLine.serialize(aOut))
    {
        return false;
    }

    // block end
    aOut.endBlock(pos);

    return !aOut.failure();
}

bool LayerNode::deserialize(Deserializer& aIn)
{
    // check block begin
    if (!aIn.beginBlock("LayerNd_"))
        return aIn.errored("invalid signature of layer node");

    // name
    aIn.read(mName);
    // visibility
    aIn.read(mIsVisible);
    // slim-down
    aIn.read(mIsSlimmedDown);
    // initial rect
    aIn.read(mInitialRect);
    // clipping
    aIn.read(mIsClipped);
    // timeline
    if (!mTimeLine.deserialize(aIn))
        return aIn.errored("failed to deserialize time line");

    // reserve shaders
    {
        mShaderHolder.reserveGridShader();
        mShaderHolder.reserveClipperShaders();

        auto defaultKey = (ImageKey*)mTimeLine.defaultKey(TimeKeyType_Image);
        if (defaultKey)
        {
            mShaderHolder.reserveShaders(defaultKey->data().blendMode());
        }

        auto& map = mTimeLine.map(TimeKeyType_Image);
        for (auto key : map)
        {
            mShaderHolder.reserveShaders(((ImageKey*)key)->data().blendMode());
        }
    }

    // check block end
    if (!aIn.endBlock())
        return aIn.errored("invalid end of layer node");

    return aIn.checkStream();
}

} // namespace core
