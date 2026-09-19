#include "SkeletonData.h"

#include <spine/Animation.h>
#include <spine/AnimationState.h>
#include <spine/AnimationStateData.h>
#include <spine/Atlas.h>
#include <spine/Bone.h>
#include <spine/BoneData.h>
#include <spine/Extension.h>
#include <spine/Physics.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonBinary.h>
#include <spine/SkeletonData.h>
#include <spine/SkeletonJson.h>
#include <spine/TextureLoader.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace spine {
SpineExtension *getDefaultExtension() {
    return new DefaultSpineExtension();
}
}

namespace {

class NullTextureLoader : public spine::TextureLoader {
public:
    void load(spine::AtlasPage &, const spine::String &) override {}
    void unload(void *) override {}
};

float wrapDeg(float degrees) {
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees < -180.0f) degrees += 360.0f;
    return degrees;
}

bool nearlyEqual(float a, float b, float eps = 0.0005f) {
    return std::fabs(a - b) <= eps;
}

struct PoseDelta {
    float dx = 0, dy = 0, drot = 0;
    float oldScaleX = 1, oldScaleY = 1, newScaleX = 1, newScaleY = 1;
    float dshearX = 0, dshearY = 0;
};

const char *pickPoseAnimation(spine::SkeletonData *data) {
    if (data->findAnimation("idle")) return "idle";
    if (data->getAnimations().size() > 0) return data->getAnimations()[0]->getName().buffer();
    return nullptr;
}

}

void bakeRuntimePoseFor3x(SkeletonData &skeleton, const std::string &inputFile) {
    std::string atlasPath = findSiblingAtlas(inputFile);
    if (atlasPath.empty()) {
        std::cout << "Skipping 4.x runtime pose bake: no sibling atlas for " << inputFile << "\n";
        return;
    }

    NullTextureLoader loader;
    spine::Atlas atlas(atlasPath.c_str(), &loader, false);
    if (atlas.getPages().size() == 0) {
        std::cout << "Skipping 4.x runtime pose bake: failed to parse atlas " << atlasPath << "\n";
        return;
    }

    spine::SkeletonData *runtimeData = nullptr;
    std::filesystem::path input(inputFile);
    std::string ext = input.extension().string();
    if (ext == ".skel") {
        spine::SkeletonBinary binary(&atlas);
        runtimeData = binary.readSkeletonDataFile(inputFile.c_str());
        if (!runtimeData) {
            std::cerr << "4.x runtime pose bake: binary read failed: " << binary.getError().buffer() << "\n";
            return;
        }
    } else {
        spine::SkeletonJson json(&atlas);
        runtimeData = json.readSkeletonDataFile(inputFile.c_str());
        if (!runtimeData) {
            std::cerr << "4.x runtime pose bake: json read failed: " << json.getError().buffer() << "\n";
            return;
        }
    }
    const char *animName = pickPoseAnimation(runtimeData);
    if (!animName) {
        delete runtimeData;
        std::cout << "Skipping 4.x runtime pose bake: skeleton has no animations.\n";
        return;
    }

    int bakedBones = 0;
    std::map<std::string, PoseDelta> deltas;
    {
        spine::Skeleton skel(runtimeData);
        skel.setToSetupPose();
        spine::AnimationStateData stateData(runtimeData);
        spine::AnimationState state(&stateData);
        state.setAnimation(0, animName, true);

        const float dt = 1.0f / 30.0f;
        spine::Animation *anim = runtimeData->findAnimation(animName);
        float duration = anim ? anim->getDuration() : 8.0f;
        if (duration < 1.0f) duration = 1.0f;
        // Two full loops of idle+physics, ending on the loop pose (t = 0 keys:
        // standing bottle, rest hair) after physics has settled.
        const int stepsPerLoop = std::max(1, static_cast<int>(std::lround(duration / dt)));
        const int steps = stepsPerLoop * 2;
        for (int i = 0; i < steps; ++i) {
            state.update(dt);
            state.apply(skel);
            skel.update(dt);
            skel.updateWorldTransform(spine::Physics_Update);
        }

        // Physics in 4.x only moves bones. Baking applied locals is enough for 3.8
        // skinning; rebinding weighted verts against a second world-transform pass
        // was collapsing hair/tree meshes. Bake every finite applied pose so idle
        // keys (bottle tilt, etc.) land in setup too — 3.8 Import Data shows setup.
        for (auto &bone : skeleton.bones) {
            if (!bone.name) continue;
            spine::Bone *rb = skel.findBone(bone.name->c_str());
            if (!rb) continue;

            const float ax = rb->getAX();
            const float ay = rb->getAY();
            const float arot = rb->getAppliedRotation();
            const float asx = rb->getAScaleX();
            const float asy = rb->getAScaleY();
            const float ashx = rb->getAShearX();
            const float ashy = rb->getAShearY();
            if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(arot) ||
                !std::isfinite(asx) || !std::isfinite(asy) ||
                !std::isfinite(ashx) || !std::isfinite(ashy)) {
                continue;
            }
            if (std::fabs(ax) > 20000.0f || std::fabs(ay) > 20000.0f) continue;

            float drot = wrapDeg(arot - bone.rotation);
            const bool rotationUnreliable = std::fabs(drot) > 90.0f &&
                                            bone.inherit != Inherit_Normal;
            if (rotationUnreliable) drot = 0.0f;

            PoseDelta d;
            d.dx = ax - bone.x;
            d.dy = ay - bone.y;
            d.drot = drot;
            d.oldScaleX = bone.scaleX;
            d.oldScaleY = bone.scaleY;
            d.newScaleX = asx != 0.0f ? asx : bone.scaleX;
            d.newScaleY = asy != 0.0f ? asy : bone.scaleY;
            d.dshearX = ashx - bone.shearX;
            d.dshearY = ashy - bone.shearY;
            if (nearlyEqual(d.dx, 0.0f) && nearlyEqual(d.dy, 0.0f) &&
                nearlyEqual(d.drot, 0.0f) &&
                nearlyEqual(d.newScaleX, d.oldScaleX) &&
                nearlyEqual(d.newScaleY, d.oldScaleY) &&
                nearlyEqual(d.dshearX, 0.0f) && nearlyEqual(d.dshearY, 0.0f)) {
                continue;
            }
            deltas[*bone.name] = d;

            bone.x = ax;
            bone.y = ay;
            if (!rotationUnreliable) bone.rotation = wrapDeg(arot);
            if (asx != 0.0f) bone.scaleX = asx;
            if (asy != 0.0f) bone.scaleY = asy;
            bone.shearX = ashx;
            bone.shearY = ashy;
            bakedBones++;
        }
    }

    for (auto &animation : skeleton.animations) {
        for (auto &[boneName, timelines] : animation.bones) {
            auto it = deltas.find(boneName);
            if (it == deltas.end()) continue;
            const PoseDelta &d = it->second;
            if (timelines.contains("rotate")) {
                for (auto &frame : timelines["rotate"]) {
                    frame.value1 = wrapDeg(frame.value1 - d.drot);
                }
            }
            if (timelines.contains("translate")) {
                for (auto &frame : timelines["translate"]) {
                    frame.value1 -= d.dx;
                    frame.value2 -= d.dy;
                }
            }
            if (timelines.contains("scale")) {
                for (auto &frame : timelines["scale"]) {
                    if (d.newScaleX != 0.0f) frame.value1 = frame.value1 * d.oldScaleX / d.newScaleX;
                    if (d.newScaleY != 0.0f) frame.value2 = frame.value2 * d.oldScaleY / d.newScaleY;
                }
            }
            if (timelines.contains("shear")) {
                for (auto &frame : timelines["shear"]) {
                    frame.value1 -= d.dshearX;
                    frame.value2 -= d.dshearY;
                }
            }
        }
    }

    // Solved idle+physics is already in setup. Zero leftover 3.8 constraints so
    // Import Data does not apply them again (especially mixX = -1).
    for (auto &ik : skeleton.ikConstraints) ik.mix = 0.0f;
    for (auto &tc : skeleton.transformConstraints) {
        tc.mixRotate = 0.0f;
        tc.mixX = 0.0f;
        tc.mixY = 0.0f;
        tc.mixScaleX = 0.0f;
        tc.mixScaleY = 0.0f;
        tc.mixShearY = 0.0f;
    }

    std::cout << "Froze 4.x " << animName << "+physics into 3.8 setup: "
              << bakedBones << " bones (mesh weights unchanged).\n";
    delete runtimeData;
}
