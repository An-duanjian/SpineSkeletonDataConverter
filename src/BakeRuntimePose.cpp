#include "SkeletonData.h"

#include <spine/Animation.h>
#include <spine/AnimationState.h>
#include <spine/AnimationStateData.h>
#include <spine/Atlas.h>
#include <spine/Attachment.h>
#include <spine/Bone.h>
#include <spine/BoneData.h>
#include <spine/Extension.h>
#include <spine/Physics.h>
#include <spine/PhysicsConstraintData.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonBinary.h>
#include <spine/SkeletonData.h>
#include <spine/SkeletonJson.h>
#include <spine/Skin.h>
#include <spine/Slot.h>
#include <spine/SlotData.h>
#include <spine/TextureLoader.h>
#include <spine/VertexAttachment.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

bool isWeighted(const std::vector<float> &vertices, int vertexCount) {
    return vertexCount > 0 && vertices.size() != static_cast<size_t>(vertexCount) * 2;
}

int meshVertexCount(const MeshAttachment &mesh) {
    if (!mesh.uvs.empty()) return static_cast<int>(mesh.uvs.size() / 2);
    if (mesh.hullLength > 0) return mesh.hullLength;
    return static_cast<int>(mesh.vertices.size() / 2);
}

std::set<int> vertexBoneIndices(const std::vector<float> &vertices, int vertexCount) {
    std::set<int> out;
    if (!isWeighted(vertices, vertexCount)) return out;
    size_t i = 0;
    for (int v = 0; v < vertexCount && i < vertices.size(); ++v) {
        int boneCount = static_cast<int>(vertices[i++]);
        for (int b = 0; b < boneCount && i + 3 < vertices.size(); ++b) {
            out.insert(static_cast<int>(std::lround(vertices[i])));
            i += 4;
        }
    }
    return out;
}

std::pair<std::vector<float> *, int> weightedVerts(Attachment &attachment) {
    if (attachment.type == AttachmentType_Mesh) {
        auto &mesh = std::get<MeshAttachment>(attachment.data);
        return {&mesh.vertices, meshVertexCount(mesh)};
    }
    if (attachment.type == AttachmentType_Path) {
        auto &path = std::get<PathAttachment>(attachment.data);
        return {&path.vertices, path.vertexCount};
    }
    if (attachment.type == AttachmentType_Clipping) {
        auto &clip = std::get<ClippingAttachment>(attachment.data);
        return {&clip.vertices, clip.vertexCount};
    }
    if (attachment.type == AttachmentType_Boundingbox) {
        auto &box = std::get<BoundingboxAttachment>(attachment.data);
        return {&box.vertices, box.vertexCount};
    }
    return {nullptr, 0};
}

bool rebindVerticesToRuntimeBones(std::vector<float> &vertices, int vertexCount,
                                  const std::vector<float> &world,
                                  spine::Vector<spine::Bone *> &bones) {
    if (!isWeighted(vertices, vertexCount)) return false;
    if (world.size() < static_cast<size_t>(vertexCount) * 2) return false;
    size_t i = 0;
    for (int v = 0; v < vertexCount && i < vertices.size(); ++v) {
        int boneCount = static_cast<int>(vertices[i++]);
        float wx = world[static_cast<size_t>(v) * 2];
        float wy = world[static_cast<size_t>(v) * 2 + 1];
        if (!std::isfinite(wx) || !std::isfinite(wy)) {
            i += static_cast<size_t>(std::max(boneCount, 0)) * 4;
            continue;
        }
        for (int b = 0; b < boneCount && i + 3 < vertices.size(); ++b) {
            int boneIndex = static_cast<int>(std::lround(vertices[i]));
            if (boneIndex >= 0 && boneIndex < static_cast<int>(bones.size()) && bones[boneIndex]) {
                float nx = 0, ny = 0;
                bones[boneIndex]->worldToLocal(wx, wy, nx, ny);
                if (std::isfinite(nx) && std::isfinite(ny) &&
                    std::fabs(nx) < 20000.0f && std::fabs(ny) < 20000.0f) {
                    vertices[i + 1] = nx;
                    vertices[i + 2] = ny;
                }
            }
            i += 4;
        }
    }
    return true;
}

}

void bakeRuntimePoseFor3x(SkeletonData &skeleton, const std::string &inputFile) {
    std::string atlasPath = findSiblingAtlas(inputFile);
    if (atlasPath.empty()) {
        std::cout << "Skipping physics rest bake: no sibling atlas for " << inputFile << "\n";
        return;
    }

    NullTextureLoader loader;
    spine::Atlas atlas(atlasPath.c_str(), &loader, false);
    if (atlas.getPages().size() == 0) {
        std::cout << "Skipping physics rest bake: failed to parse atlas " << atlasPath << "\n";
        return;
    }

    spine::SkeletonData *runtimeData = nullptr;
    std::filesystem::path input(inputFile);
    std::string ext = input.extension().string();
    if (ext == ".skel") {
        spine::SkeletonBinary binary(&atlas);
        runtimeData = binary.readSkeletonDataFile(inputFile.c_str());
        if (!runtimeData) {
            std::cerr << "Physics rest bake: binary read failed: " << binary.getError().buffer() << "\n";
            return;
        }
    } else {
        spine::SkeletonJson json(&atlas);
        runtimeData = json.readSkeletonDataFile(inputFile.c_str());
        if (!runtimeData) {
            std::cerr << "Physics rest bake: json read failed: " << json.getError().buffer() << "\n";
            return;
        }
    }

    if (runtimeData->getPhysicsConstraints().size() == 0) {
        delete runtimeData;
        return;
    }

    std::set<std::string> liveConstraintBones;
    for (const auto &tc : skeleton.transformConstraints) {
        for (const auto &boneName : tc.bones) liveConstraintBones.insert(boneName);
    }
    for (const auto &ik : skeleton.ikConstraints) {
        for (const auto &boneName : ik.bones) liveConstraintBones.insert(boneName);
    }

    std::set<std::string> physicsNames;
    for (size_t i = 0; i < runtimeData->getPhysicsConstraints().size(); ++i) {
        auto *pc = runtimeData->getPhysicsConstraints()[i];
        if (!pc || !pc->getBone()) continue;
        std::string name = pc->getBone()->getName().buffer();
        if (liveConstraintBones.contains(name)) continue;
        physicsNames.insert(std::move(name));
    }
    if (physicsNames.empty()) {
        delete runtimeData;
        std::cout << "Skipping physics rest bake: all physics bones are IK/transform subjects.\n";
        return;
    }

    int bakedBones = 0;
    int rebakedMeshes = 0;
    std::map<std::string, PoseDelta> deltas;
    {
        spine::Skeleton skel(runtimeData);
        skel.setToSetupPose();
        spine::AnimationStateData stateData(runtimeData);
        spine::AnimationState state(&stateData);
        const char *animName = runtimeData->findAnimation("idle") ? "idle" : nullptr;
        if (animName) state.setAnimation(0, animName, true);
        const float dt = 1.0f / 30.0f;
        float duration = 4.0f;
        if (animName) {
            spine::Animation *anim = runtimeData->findAnimation(animName);
            if (anim && anim->getDuration() > 1.0f) duration = anim->getDuration();
        }
        const int stepsPerLoop = std::max(1, static_cast<int>(std::lround(duration / dt)));
        skel.updateWorldTransform(spine::Physics_Reset);
        for (int i = 0; i < stepsPerLoop * 2; ++i) {
            state.update(dt);
            state.apply(skel);
            skel.update(dt);
            skel.updateWorldTransform(spine::Physics_Update);
        }

        std::map<std::string, int> boneIndex;
        for (int i = 0; i < static_cast<int>(skeleton.bones.size()); ++i) {
            if (skeleton.bones[static_cast<size_t>(i)].name)
                boneIndex[*skeleton.bones[static_cast<size_t>(i)].name] = i;
        }
        std::set<int> physicsIdx;
        for (const auto &name : physicsNames) {
            auto it = boneIndex.find(name);
            if (it != boneIndex.end()) physicsIdx.insert(it->second);
        }

        std::map<std::pair<std::string, std::string>, std::vector<float>> worldVerts;
        std::set<std::pair<std::string, std::string>> rebindKeys;
        spine::Skin *skin = skel.getSkin();
        if (!skin) skin = runtimeData->getDefaultSkin();
        if (skin) {
            auto entries = skin->getAttachments();
            while (entries.hasNext()) {
                auto &entry = entries.next();
                if (!entry._attachment ||
                    !entry._attachment->getRTTI().instanceOf(spine::VertexAttachment::rtti)) {
                    continue;
                }
                auto *va = static_cast<spine::VertexAttachment *>(entry._attachment);
                if (va->getBones().size() == 0 || va->getWorldVerticesLength() < 2) continue;
                if (entry._slotIndex >= skel.getSlots().size()) continue;
                spine::Slot *slot = skel.getSlots()[entry._slotIndex];
                spine::Attachment *saved = slot->getAttachment();
                slot->setAttachment(entry._attachment);
                spine::Vector<float> world;
                world.setSize(va->getWorldVerticesLength(), 0);
                va->computeWorldVertices(*slot, 0, va->getWorldVerticesLength(), world, 0, 2);
                slot->setAttachment(saved);

                std::string slotName = slot->getData().getName().buffer();
                std::string attName = entry._name.buffer();
                std::vector<float> out(world.size());
                bool ok = true;
                for (size_t i = 0; i < world.size(); ++i) {
                    if (!std::isfinite(world[i])) { ok = false; break; }
                    out[i] = world[i];
                }
                if (ok) worldVerts[{slotName, attName}] = std::move(out);
            }
        }

        for (auto &sk : skeleton.skins) {
            for (auto &[slotName, attachments] : sk.attachments) {
                for (auto &[attName, attachment] : attachments) {
                    auto [verts, vc] = weightedVerts(attachment);
                    if (!verts || vc <= 0) continue;
                    auto used = vertexBoneIndices(*verts, vc);
                    bool usesPhysics = false;
                    for (int idx : used) {
                        if (physicsIdx.contains(idx)) { usesPhysics = true; break; }
                    }
                    if (usesPhysics) rebindKeys.insert({slotName, attName});
                }
            }
        }

        auto &runtimeBones = skel.getBones();
        for (auto &sk : skeleton.skins) {
            for (auto &[slotName, attachments] : sk.attachments) {
                for (auto &[attName, attachment] : attachments) {
                    if (!rebindKeys.contains({slotName, attName})) continue;
                    auto it = worldVerts.find({slotName, attName});
                    if (it == worldVerts.end()) continue;
                    auto [verts, vc] = weightedVerts(attachment);
                    if (!verts || vc <= 0) continue;
                    if (rebindVerticesToRuntimeBones(*verts, vc, it->second, runtimeBones))
                        rebakedMeshes++;
                }
            }
        }

        for (auto &bone : skeleton.bones) {
            if (!bone.name || liveConstraintBones.contains(*bone.name)) continue;
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

    std::cout << "Baked idle+physics into 3.8 setup (IK/transform subjects kept live): "
              << bakedBones << " bones, " << rebakedMeshes << " meshes.\n";
    delete runtimeData;
}
