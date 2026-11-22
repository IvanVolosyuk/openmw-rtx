#include "worldmirror.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Matrixd>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/PositionAttitudeTransform>
#include <osg/Referenced>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <apps/openmw/mwrender/sceneframe.hpp>
#include <apps/openmw/mwrender/searules.hpp>
#include <apps/openmw/mwrender/sky.hpp>
#include <apps/openmw/mwrender/vismask.hpp>
#include <apps/openmw/mwworld/cell.hpp>
#include <apps/openmw/mwworld/cellstore.hpp>
#include <apps/openmw/mwworld/weather.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/misc/result.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/frame/camera.hpp>
#include <components/rtx/image/imagedescription.hpp>
#include <components/rtx/mirror/cells/cellgrid.hpp>
#include <components/rtx/mirror/cells/cellworld.hpp>
#include <components/rtx/mirror/cells/nightday.hpp>
#include <components/rtx/mirror/extractionstats.hpp>
#include <components/rtx/renderer/slot.hpp>
#include <components/rtx/scene/light.hpp>
#include <components/rtx/scene/mesh.hpp>
#include <components/rtx/scene/meshtable.hpp>
#include <components/rtx/shaders/look.h>
#include <components/rtx/shaders/scene.h>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/terrain/storage.hpp>
#include <components/terrain/world.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include "classmasks.hpp"
#include "mirrorknobs.hpp"
#include "skyreader.hpp"
#include "tracedgroundcover.hpp"

namespace MWRender
{
    namespace
    {
        /// The game's own models and images, as `Rtx::CellReader` asks for them.
        class SceneContent final : public Rtx::ContentSource
        {
        public:
            explicit SceneContent(Resource::SceneManager& scenes)
                : mScenes(scenes)
            {
            }

            osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path) override
            {
                // Uncompiled, because nothing here has a context to compile for: `compile` queues
                // the model's OpenGL objects, and this renderer initialises no OpenGL.
                return mScenes.getTemplate(path, false);
            }

            Misc::Result<osg::ref_ptr<const osg::Image>, std::string> getImage(VFS::Path::NormalizedView path) override
            {
                return Rtx::openImage(*mScenes.getImageManager(), path);
            }

        private:
            Resource::SceneManager& mScenes;
        };

        /// Which child the ring's day-night switches show: the game's mode, whose value is the
        /// child's index as the ring's is, or the child each file opens on where the game drives
        /// no switch.
        Rtx::NightDayMode nightDayOf(const WorldState& world)
        {
            static_assert(static_cast<unsigned int>(Rtx::NightDayMode::Default) == MWWorld::Default
                && static_cast<unsigned int>(Rtx::NightDayMode::ExteriorNight) == MWWorld::ExteriorNight
                && static_cast<unsigned int>(Rtx::NightDayMode::InteriorDay) == MWWorld::InteriorDay);

            if (!world.mNightDayMode.has_value())
                return Rtx::NightDayMode::Authored;

            assert(*world.mNightDayMode <= MWWorld::InteriorDay && "a day-night mode the ring has no name for");
            return static_cast<Rtx::NightDayMode>(*world.mNightDayMode);
        }

        /// What every walk this renderer makes takes: an exclusion of what the ray tracer draws
        /// itself, never a selection of what a walk is interested in. A node mask is AND-ed at
        /// every node, so naming `Mask_WeatherParticles` to mean "the weather subtree" extracted
        /// every storm with its particles missing, because a blizzard's own particles are marked
        /// `Mask_ParticleSystem`. Which subtree is walked is answered by where the walk starts.
        /// The ground is the ring's: what `TracedTerrain` stands under `Mask_Terrain` is the
        /// intersector's, and walked it would place every loaded cell's ground a second time.
        ///
        /// **`Mask_UpdateVisitor` is what the content hides with, and it is named here rather than
        /// asked of `NifOsg::Loader`.** The loader is told that bit by `RenderingManager`, and this
        /// renderer is built before there is a rendering manager to tell it, so a mask that reads
        /// the loader's answer subtracts nought: every node a `NifOsg::VisController` hides is
        /// traced anyway, and the Heart of Lorkhan stands wearing the whole of its destruction at
        /// the frame that sequence opens on. The bit is the engine's own decision and a constant,
        /// so it is stated as one and no order can reach it — which is what
        /// `MWRender::ObjectPaging` does with the same bit for the same reason.
        /// `WorldMirror::mirror` checks the loader against it.
        constexpr osg::Node::NodeMask sWorldTraversal = ~static_cast<osg::Node::NodeMask>(
            Mask_Sky | Mask_Sun | Mask_SimpleWater | Mask_Terrain | Mask_UpdateVisitor);

        /// What each walk this renderer makes over the one extractor is anchored at. Four roots the
        /// walk cannot tell apart by structure, so each is named; the world's is nought, which is
        /// what `SceneExtractor::extract` calls a caller that walks one whole graph.
        enum Anchor : std::size_t
        {
            World = 0,
            Sea = 1,
            Rain = 2,
            Effect = 3,
        };

        /// What the world walk may see: `sWorldTraversal`, without the player or the actors where the
        /// eye's view mask leaves them out — `WorldMirror::setViewMask`. Those two only, because they
        /// share one class: the trace's ray mask hides every other class the view leaves out, and
        /// a walk that went on past an actor would stand it in the class the player keeps on.
        constexpr osg::Node::NodeMask worldTraversal(const unsigned int view)
        {
            constexpr osg::Node::NodeMask walkedByView = Mask_Player | Mask_Actor;

            return sWorldTraversal & ~(walkedByView & ~view);
        }
    }

    WorldMirror::WorldMirror(const MirrorKnobs& knobs)
        : mWalk{ .mSpecular = knobs.mSpecularLayout }
        , mGroundcoverReach(knobs.mGroundcoverReach)
        , mGroundcoverDensity(knobs.mGroundcoverDensity)
        , mGroundcoverLampLit(knobs.mGroundcoverLampLit)
        , mDistantStatics(knobs.mDistantStatics)
        , mInteriorWindowSunlight(knobs.mInteriorWindowSunlight)
        , mLamps(knobs.mLamps)
        , mExtractor(mScene, mWalk, Rtx::sWorldExtractorRoom)
        , mTraversal(worldTraversal(~0u))
        , mReach(knobs.mReach)
    {
        mRing.setMinSize(knobs.mMinSize);
        // The sky is not mirrored: the engine rebuilds it every frame, state sets and all, so
        // walking it churns the identity maps and makes every frame a full rebuild, and a ray that
        // reaches the sky gets this renderer's own. The simple water is the local map's copy of the
        // sea, which a mirror walking both would place twice. What the content hides is the one bit
        // `sWorldTraversal` names, rather than none, so the update traversal still reaches a hidden
        // bone.
        mExtractor.setTraversalMask(mTraversal);

        // Where the engine stamps its identities: the cell roots under the scene root and the
        // reference roots under those, and the player beside the cells (`MWRender::Objects`).
        mExtractor.setStampDepth(2);

        // What is left of the two is the sea, which this renderer stands: upstream's plane, as
        // `MWRender::Water` makes it, on a transform a frame moves.
        mExtractor.setWaterMask(Mask_Water);

        osg::ref_ptr<osg::Geometry> sea = createSeaGeometry();
        sea->setNodeMask(Mask_Water);
        sea->setName("Sea Geometry");
        mSea = new osg::PositionAttitudeTransform;
        mSea->setName("Sea Root");
        mSea->addChild(sea);

        // The roots the game marks, so a camera's cull mask can keep or leave out what stands
        // under them — `describeView` reads the same table the other way.
        for (const ClassMask& held : sClassMasks)
            if (held.mClass != Rtx::InstanceClass::Static)
                mExtractor.setClassMask(held.mClass, held.mNodes);
    }

    void WorldMirror::attach(Resource::ResourceSystem& resources, SceneUtil::UnrefQueue& released)
    {
        mContent = std::make_unique<SceneContent>(*resources.getSceneManager());
        mReleasing = &released;

        // Every texture the content holds, so the frame's cache of their facts never rehashes on
        // the frame that meets one more: one walk of the directory, at load.
        std::size_t textures = 0;
        for ([[maybe_unused]] const VFS::Path::Normalized& file :
            resources.getVFS()->getRecursiveDirectoryIterator(VFS::Path::NormalizedView("textures/")))
            ++textures;
        mWalk.mContent.mFacts.reserve(textures);
    }

    void WorldMirror::release(Rtx::Released& released)
    {
        if (mReleasing == nullptr)
        {
            released.clear();
            return;
        }

        // One handover for the whole batch, so a frame that lets go of many objects allocates the
        // batch and its share alone.
        if (!released.isEmpty())
            mReleasing->push(std::make_shared<std::vector<osg::ref_ptr<const osg::Referenced>>>(released.take()));
    }

    WorldMirror::~WorldMirror()
    {
        assert((std::uncaught_exceptions() > 0 || mScene.isEmpty()) && "a world detached and still standing rows");
    }

    void WorldMirror::detach()
    {
        // **The ring's thread reads the storages the world owns.** A world with nothing in it is
        // what stops the thread and drops what it held.
        mRing.follow(Rtx::WorldAround{});

        // What the ring held on the extractor's rows goes back, and the sweep frees them with
        // everything the walks stood: nothing of this world stays in the scene. Not on the way
        // out of an exception a frame threw, where the scene is whatever the throw left and the
        // assert in the retire would stand between the throw and its message.
        if (std::uncaught_exceptions() == 0)
        {
            mExtractor.detach(mRing);
            release(mExtractor.getReleased());
        }
        mReleasing = nullptr;

        mGroundcover.reset();
        mContent.reset();
    }

    void WorldMirror::growGroundcover(const MWWorld::GroundcoverStore& store)
    {
        assert(mGroundcover == nullptr && "the default worldspace's ground made twice for one world");

        if (mGroundcoverReach > 0.0f)
            mGroundcover = std::make_unique<TracedGroundcover>(store, mGroundcoverDensity, mGroundcoverLampLit);
    }

    Rtx::ContentMemory WorldMirror::getContentMemory()
    {
        const Rtx::MeshTable& meshes = mScene.meshes();
        const Rtx::DeformerTable& deformers = mScene.deformers();
        const auto count = [](const auto& run) { return static_cast<std::uint32_t>(run.size()); };
        return Rtx::ContentMemory{
            .mVertexEnd = meshes.getVertexRuns().getEnd(),
            .mVerticesUsed = meshes.getVertexRuns().getUsed(),
            .mIndexEnd = meshes.getIndexRuns().getEnd(),
            .mIndicesUsed = meshes.getIndexRuns().getUsed(),
            .mLayerEnd = count(mScene.materials().getLayers()),
            .mMaskEnd = count(mScene.materials().getMasks()),
            .mRigRunEnd = count(deformers.getRuns()),
            .mInfluenceEnd = count(deformers.getInfluences()),
            .mOffsetEnd = count(deformers.getMorphOffsets()),
            .mPoseEnd = count(deformers.getPoses()),
            .mBindVertexEnd = deformers.getBindVertexCount(),
            .mReader = mRing.getReaderMemory(),
        };
    }

    void WorldMirror::standSea(const MWWorld::CellStore& cell)
    {
        const MWWorld::Cell& stood = *cell.getCell();
        mSeaCentre = stood.isExterior() ? seaCentre(stood.getGridX(), stood.getGridY()) : osg::Vec2f(0.f, 0.f);
    }

    void WorldMirror::setViewMask(const unsigned int view)
    {
        mTraversal = worldTraversal(view);
        mExtractor.setTraversalMask(mTraversal);
    }

    Rtx::ExtractionStats WorldMirror::mirror(const SceneFrame& frame, const osg::Matrixd& view)
    {
        const std::size_t frameNumber = frame.mWhen.getFrameNumber();

        // **Whatever the loader hides with has to be inside what this mask excludes.**
        // `sWorldTraversal` names that bit rather than asking, because the loader is told it by
        // `RenderingManager` and this renderer is built first — so the two are checked against each
        // other here instead, where a walk is about to use the mask. A loader nobody has told yet
        // hides with nothing and passes: what this catches is the engine moving the bit.
        assert((NifOsg::Loader::getHiddenNodeMask() & ~static_cast<unsigned int>(Mask_UpdateVisitor)) == 0
            && "the loader hides with a bit the walk's mask does not exclude");

        // The world's clock and not this renderer's, or the controllers would run while the game
        // was paused.
        mExtractor.setLampSettings(mLamps);
        mExtractor.setInteriorWindowPortals(frame.mWorld.mLocation == Location::Interior && mInteriorWindowSunlight);
        mExtractor.setSimulationTime(
            frame.mWhen.getSimulationTime(), frame.mPaused ? 0.0 : static_cast<double>(frame.mDeltaTime));

        // What goes is the lists a walk refills wholesale; the meshes, materials and textures stay
        // because the structures were built from them, and the placements because they are
        // addressed by slot.
        mScene.clearPlacement();

        // What the weather drops, walked as a second root, because the sky's mask keeps the world
        // walk out of that subtree: the same systems the rasterizer draws, stood at the eye.
        const osg::Matrixd inverseView = osg::Matrixd::inverse(view);
        const osg::Vec3f eye = inverseView.getTrans();
        mEye = eye;

        // And the eye every billboard in the world turns to, which the rasterizer's cull hands its
        // `AutoTransform`s and this walk has to be told.
        mExtractor.setEye(Rtx::viewBasisOf(inverseView));
        mExtractor.extractPrecipitation(frame.mPrecipitation.getRainNode(), eye, Anchor::Rain, frameNumber);
        mExtractor.extractPrecipitation(frame.mPrecipitation.getParticleNode(), eye, Anchor::Effect, frameNumber);

        // The sea, where the frame says there is one: hidden by its mask otherwise, as the
        // rasterizer's `updateVisible` hid the same plane, so the walk leaves no placement of it.
        mSea->setPosition(osg::Vec3f(mSeaCentre.x(), mSeaCentre.y(), frame.mWorld.mWater.mHeight));
        mSea->setNodeMask(frame.mWorld.mWater.isShown() ? ~0u : 0u);
        mExtractor.extract(*mSea, osg::Matrixf::identity(), Anchor::Sea, frameNumber);

        // The eye, the reach, the world's own grid, the hour and the day-night mode, said once to
        // the ring: what the game has stood for itself is what the ring may not stand again, and
        // its lamps burn and its windows light at the world's clock as the graph's do.
        Terrain::Storage* const ground = frame.mTerrain.getStorage();
        const ESM::RefId worldspace = frame.mTerrain.getWorldspace();
        mGrid = ground != nullptr ? Rtx::CellGrid(ground->getCellWorldSize(worldspace)) : Rtx::CellGrid();

        const Rtx::WorldAround around{
            .mWorld = {
                .mStorage = &frame.mObjectStorage,
                .mGround = ground,
                .mContent = mContent.get(),
                // The rasterizer grows grass in the default worldspace alone (`GlRenderer::createGround`).
                .mGroundcover = worldspace == ESM::Cell::sDefaultWorldspaceId ? mGroundcover.get() : nullptr,
                .mWorldspace = worldspace,
                .mGrid = mGrid,
                // The world's mask and never the walk's own: a mask that moves makes
                // `Rtx::CellRing::forget` read the ring from nothing, and the player bit moves
                // while the camera settles.
                .mMask = sWorldTraversal,
                // What `extractWorld` walks every reference root under, below.
                .mAbove = frame.mScene.getStateSet(),
                .mStatics = mDistantStatics,
            },
            .mEye = eye,
            .mReach = getReach(),
            .mGroundcoverReach = mGroundcoverReach,
            .mActiveGrid = frame.mTerrain.getActiveGrid(),
            .mExterior = frame.mWorld.mLocation == Location::Exterior,
            .mSimulationTime = frame.mWhen.getSimulationTime(),
            .mLamps = mLamps,
            .mNightDay = nightDayOf(frame.mWorld),
        };

        // Told once a frame, because what the graph does not hold is the frame's to say. The
        // world walk asks it, and the precipitation walk above cannot: it is a subtree. Which
        // frame the ring is standing for is `extractWorld`'s to say, because that call has the
        // ring and the number both.
        mRing.follow(around);

        // One walk over the whole graph, where every path is already distinct, and the one that
        // stands the references the game said jumped: the other walks stand nothing of theirs.
        mExtractor.setJumped(frame.mJumped);
        Rtx::ExtractionStats found = mExtractor.extractWorld(frame.mScene, Anchor::World, frameNumber, mRing);
        mExtractor.setJumped({});

        // What the walks did not find has gone. The graph is the whole world every frame, which is
        // what makes mark and sweep sound; the identity maps hold their keys alive until it runs.
        // After every walk of the frame and never before one, because the sweep bumps the epoch
        // the next walk is measured against.
        mExtractor.retire();
        release(mExtractor.getReleased());

        // **Taken once, after every walk of the frame**, and not by a walk: the precipitation and
        // the sea are walks whose counts go nowhere, and the sky's sheets are read between walks,
        // so a count a walk took with it was a count the frame lost.
        found.mPreprocessed.mOnFrame += mWalk.mContent.mPreprocessor.takeStats();

        return found;
    }

    void WorldMirror::addRipples(std::span<const Rtx::RippleImpulse> impulses)
    {
        for (const Rtx::RippleImpulse& impulse : impulses)
            mScene.addRipple(impulse);
    }

    void WorldMirror::addWindowPortalLights(const WindowPortalLighting& lighting)
    {
        if (!lighting.mActive || mScene.placements().getCounts().mWindowPortals == 0)
            return;

        struct WindowCluster
        {
            osg::BoundingBoxf mBounds;
            osg::Vec3f mCentroidSum;
            osg::Vec3f mNormalSum;
            float mArea = 0.0f;
        };

        std::vector<WindowCluster> clusters;
        constexpr std::size_t maxClusters = 64;
        constexpr float joinMargin = 64.0f;

        const Rtx::MeshTable& meshes = mScene.meshes();
        mScene.placements().forEachPlaced([&](Rtx::Index, const Rtx::PlacementRow& row) {
            if (!row.mInstance.mWindowPortal)
                return;

            const std::span<const osg::Vec3f> positions = meshes.getMeshPositions(row.mInstance.mMesh);
            const std::span<const std::uint32_t> indices = meshes.getMeshIndices(row.mInstance.mMesh);
            if (positions.empty() || indices.size() < 3)
                return;

            const osg::Matrixf& xf = row.mInstance.mTransform;
            const osg::Vec3f row0(xf(0, 0), xf(0, 1), xf(0, 2));
            const osg::Vec3f row1(xf(1, 0), xf(1, 1), xf(1, 2));
            const osg::Vec3f row2(xf(2, 0), xf(2, 1), xf(2, 2));
            const float det = (row0 ^ row1) * row2;
            const bool flip = (det < 0.0f) != row.mInstance.mClockwise;

            for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                const std::uint32_t i0 = indices[i];
                const std::uint32_t i1 = indices[i + 1];
                const std::uint32_t i2 = indices[i + 2];
                if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size())
                    continue;

                const osg::Vec3f p0 = positions[i0] * xf;
                const osg::Vec3f p1 = positions[i1] * xf;
                const osg::Vec3f p2 = positions[i2] * xf;
                osg::Vec3f cross = (p1 - p0) ^ (p2 - p0);
                if (flip)
                    cross = -cross;

                const float len = cross.length();
                if (!(len > 1e-3f))
                    continue;

                const float area = 0.5f * len;
                const osg::Vec3f n = cross / len;
                const osg::Vec3f c = (p0 + p1 + p2) * (1.0f / 3.0f);

                WindowCluster* joined = nullptr;
                for (WindowCluster& cluster : clusters)
                {
                    if ((n * cluster.mNormalSum) > 0.7f * cluster.mNormalSum.length()
                        && c.x() >= cluster.mBounds.xMin() - joinMargin && c.x() <= cluster.mBounds.xMax() + joinMargin
                        && c.y() >= cluster.mBounds.yMin() - joinMargin && c.y() <= cluster.mBounds.yMax() + joinMargin
                        && c.z() >= cluster.mBounds.zMin() - joinMargin && c.z() <= cluster.mBounds.zMax() + joinMargin)
                    {
                        joined = &cluster;
                        break;
                    }
                }

                if (joined != nullptr)
                {
                    joined->mBounds.expandBy(p0);
                    joined->mBounds.expandBy(p1);
                    joined->mBounds.expandBy(p2);
                    joined->mCentroidSum += c * area;
                    joined->mNormalSum += n * area;
                    joined->mArea += area;
                }
                else if (clusters.size() < maxClusters)
                {
                    osg::BoundingBoxf bounds;
                    bounds.expandBy(p0);
                    bounds.expandBy(p1);
                    bounds.expandBy(p2);
                    clusters.push_back(WindowCluster{
                        .mBounds = bounds,
                        .mCentroidSum = c * area,
                        .mNormalSum = n * area,
                        .mArea = area,
                    });
                }
            }
        });

        for (const WindowCluster& cluster : clusters)
        {
            if (!(cluster.mArea > 16.0f))
                continue;
            const float nLen = cluster.mNormalSum.length();
            if (!(nLen > 1e-4f))
                continue;

            const osg::Vec3f normal = cluster.mNormalSum / nLen;
            const osg::Vec3f center = cluster.mCentroidSum / cluster.mArea;
            const float clampedArea = std::clamp(cluster.mArea, 600.0f, 36000.0f);
            const float radius = std::clamp(std::sqrt(clampedArea / Rtx::Shaders::PI), 22.0f, 96.0f);
            const float sunOnWindow = std::max(0.0f, -(normal * lighting.mSunDirection));
            const osg::Vec3f radiance = lighting.mSkyScatter + lighting.mSunScatter * sunOnWindow;
            if (!(radiance.x() > 1e-4f || radiance.y() > 1e-4f || radiance.z() > 1e-4f))
                continue;

            mScene.addLight(Rtx::Light{
                .mPosition = center + normal * 18.0f,
                .mIntensity = radiance * clampedArea,
                .mReach = std::clamp(radius * 40.0f, 1200.0f, 2600.0f),
                .mSourceRadius = radius * 0.75f,
                .mClearance = 14.0f,
                .mTraits
                = Rtx::Shaders::lightTraits(false, Rtx::Shaders::MASK_EVERY_CLASS) | Rtx::Shaders::LIGHT_WINDOW,
            });
        }
    }

    Rtx::SceneUpload WorldMirror::hand(Rtx::SceneHolder& scenes, Rtx::FrameSpend& spend)
    {
        return mUploader.hand(scenes,
            Rtx::SceneUploader::Handing{
                .mSlot = Rtx::SceneSlot::world(), .mScene = mScene, .mComposites = &mComposites, .mSpend = &spend });
    }

}
