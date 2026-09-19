#include "SkeletonData.h"

#include <spine/Animation.h>
#include <spine/Attachment.h>
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
#include <spine/IkConstraintData.h>
#include <spine/PathConstraintData.h>
#include <spine/PhysicsConstraintData.h>
#include <spine/TransformConstraintData.h>
#include <spine/TextureLoader.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
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

bool isLiveConstraintSubject(const std::string &name,
                             const std::set<std::string> &ikSubjects,
                             const std::set<std::string> &transformSubjects) {
    // Live 3.8 IK/transform subjects must keep their authored setup. Baking
    // their constrained locals and then letting the constraint run again in
    // the editor double-applies (face14 / body13 jumped hundreds of px).
    // This includes physics bones that are also constraint subjects
    // (tousheng, hair_b): physics is stripped, but the transform still runs.
    return ikSubjects.contains(name) || transformSubjects.contains(name);
}

}

void bakeRuntimePoseFor3x(SkeletonData &skeleton, const std::string &inputFile) {
    namespace fs = std::filesystem;
    fs::path input(inputFile);
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

    std::set<std::string> physicsBones;
    std::set<std::string> ikSubjects;
    std::set<std::string> transformSubjects;
    auto addNamed = [](std::set<std::string> &into, spine::BoneData *data) {
        if (data && data->getName().length() > 0) into.insert(data->getName().buffer());
    };
    for (size_t i = 0; i < runtimeData->getPhysicsConstraints().size(); ++i) {
        addNamed(physicsBones, runtimeData->getPhysicsConstraints()[i]->getBone());
    }
    for (size_t i = 0; i < runtimeData->getIkConstraints().size(); ++i) {
        auto *ik = runtimeData->getIkConstraints()[i];
        if (std::fabs(ik->getMix()) < 1e-4f) continue;
        for (size_t b = 0; b < ik->getBones().size(); ++b) addNamed(ikSubjects, ik->getBones()[b]);
    }
    for (size_t i = 0; i < runtimeData->getTransformConstraints().size(); ++i) {
        auto *tc = runtimeData->getTransformConstraints()[i];
        if (std::fabs(tc->getMixRotate()) < 1e-4f &&
            std::fabs(tc->getMixX()) < 1e-4f &&
            std::fabs(tc->getMixY()) < 1e-4f &&
            std::fabs(tc->getMixScaleX()) < 1e-4f &&
            std::fabs(tc->getMixScaleY()) < 1e-4f &&
            std::fabs(tc->getMixShearY()) < 1e-4f) {
            continue;
        }
        for (size_t b = 0; b < tc->getBones().size(); ++b) addNamed(transformSubjects, tc->getBones()[b]);
    }

    int baked = 0;
    int skippedLive = 0;
    std::map<std::string, PoseDelta> deltas;
    {
        spine::Skeleton skel(runtimeData);
        skel.setToSetupPose();
        spine::AnimationStateData stateData(runtimeData);
        spine::AnimationState state(&stateData);
        state.setAnimation(0, animName, true);

        const float dt = 1.0f / 30.0f;
        const int frames = 240;
        for (int i = 0; i < frames; ++i) {
            state.update(dt);
            state.apply(skel);
            skel.update(dt);
            skel.updateWorldTransform(spine::Physics_Update);
        }

        for (auto &bone : skeleton.bones) {
            if (!bone.name) continue;
            const std::string &name = *bone.name;
            if (!physicsBones.contains(name)) continue;
            if (isLiveConstraintSubject(name, ikSubjects, transformSubjects)) {
                skippedLive++;
                continue;
            }
            spine::Bone *rb = skel.findBone(name.c_str());
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
            if (std::fabs(ax) > 8000.0f || std::fabs(ay) > 8000.0f) continue;

            if (nearlyEqual(ax, bone.x) && nearlyEqual(ay, bone.y) &&
                nearlyEqual(wrapDeg(arot - bone.rotation), 0.0f) &&
                nearlyEqual(asx, bone.scaleX) && nearlyEqual(asy, bone.scaleY) &&
                nearlyEqual(ashx, bone.shearX) && nearlyEqual(ashy, bone.shearY)) {
                continue;
            }

            float drot = wrapDeg(arot - bone.rotation);
            // Inherit modes other than Normal often make updateAppliedTransform
            // report a ~180° local rotation that is not a real physics swing.
            // Keep the authored rotation in that case; translation is still safe.
            const bool rotationUnreliable = std::fabs(drot) > 90.0f;
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
            deltas[name] = d;

            bone.x = ax;
            bone.y = ay;
            if (!rotationUnreliable) bone.rotation = wrapDeg(arot);
            if (asx != 0.0f) bone.scaleX = asx;
            if (asy != 0.0f) bone.scaleY = asy;
            bone.shearX = ashx;
            bone.shearY = ashy;
            baked++;
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

    std::cout << "Baked 4.x runtime pose (" << animName
              << " + physics) into 3.8 setup for " << baked << " physics bones"
              << " (skipped " << skippedLive << " live IK/transform subjects).\n";
    delete runtimeData;
}
