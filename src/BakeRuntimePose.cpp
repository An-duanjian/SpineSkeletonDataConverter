#include "SkeletonData.h"

#include <spine/Animation.h>
#include <spine/AnimationState.h>
#include <spine/AnimationStateData.h>
#include <spine/Atlas.h>
#include <spine/Attachment.h>
#include <spine/Bone.h>
#include <spine/BoneData.h>
#include <spine/Extension.h>
#include <spine/IkConstraintData.h>
#include <spine/Physics.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonBinary.h>
#include <spine/SkeletonData.h>
#include <spine/SkeletonJson.h>
#include <spine/Skin.h>
#include <spine/Slot.h>
#include <spine/SlotData.h>
#include <spine/TextureLoader.h>
#include <spine/TransformConstraintData.h>
#include <spine/VertexAttachment.h>

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

const char *pickPoseAnimation(spine::SkeletonData *data) {
    if (data->findAnimation("idle")) return "idle";
    if (data->getAnimations().size() > 0) return data->getAnimations()[0]->getName().buffer();
    return nullptr;
}

bool isWeighted(const std::vector<float> &vertices, int vertexCount) {
    return vertexCount > 0 && vertices.size() != static_cast<size_t>(vertexCount) * 2;
}

void stripDeform(SkeletonData &skeleton, const std::set<std::pair<std::string, std::string>> &slotAtt) {
    if (slotAtt.empty()) return;
    for (auto &animation : skeleton.animations) {
        for (auto skinIt = animation.attachments.begin(); skinIt != animation.attachments.end();) {
            for (auto slotIt = skinIt->second.begin(); slotIt != skinIt->second.end();) {
                for (auto attIt = slotIt->second.begin(); attIt != slotIt->second.end();) {
                    if (slotAtt.contains({slotIt->first, attIt->first})) attIt = slotIt->second.erase(attIt);
                    else ++attIt;
                }
                if (slotIt->second.empty()) slotIt = skinIt->second.erase(slotIt);
                else ++slotIt;
            }
            if (skinIt->second.empty()) skinIt = animation.attachments.erase(skinIt);
            else ++skinIt;
        }
    }
}

int meshVertexCount(const MeshAttachment &mesh) {
    if (!mesh.uvs.empty()) return static_cast<int>(mesh.uvs.size() / 2);
    if (mesh.hullLength > 0) return mesh.hullLength;
    return static_cast<int>(mesh.vertices.size() / 2);
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
    int rebakedMeshes = 0;
    std::map<std::string, PoseDelta> deltas;
    std::set<std::pair<std::string, std::string>> rebaked;
    {
        spine::Skeleton skel(runtimeData);
        skel.setToSetupPose();
        spine::AnimationStateData stateData(runtimeData);
        spine::AnimationState state(&stateData);
        state.setAnimation(0, animName, true);

        const float dt = 1.0f / 30.0f;
        for (int i = 0; i < 240; ++i) {
            state.update(dt);
            state.apply(skel);
            skel.update(dt);
            skel.updateWorldTransform(spine::Physics_Update);
        }

        std::map<std::pair<std::string, std::string>, std::vector<float>> worldVerts;
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

        auto rebindVertices = [&](std::vector<float> &vertices, int vertexCount,
                                  const std::vector<float> &world) -> bool {
            if (!isWeighted(vertices, vertexCount)) return false;
            if (world.size() < static_cast<size_t>(vertexCount) * 2) return false;
            size_t i = 0;
            for (int v = 0; v < vertexCount && i < vertices.size(); ++v) {
                int boneCount = static_cast<int>(vertices[i++]);
                float wx = world[static_cast<size_t>(v) * 2];
                float wy = world[static_cast<size_t>(v) * 2 + 1];
                for (int b = 0; b < boneCount && i + 3 < vertices.size(); ++b) {
                    int boneIndex = static_cast<int>(std::lround(vertices[i]));
                    if (boneIndex >= 0 && boneIndex < static_cast<int>(skel.getBones().size())) {
                        float nx, ny;
                        skel.getBones()[static_cast<size_t>(boneIndex)]->worldToLocal(wx, wy, nx, ny);
                        if (std::isfinite(nx) && std::isfinite(ny)) {
                            vertices[i + 1] = nx;
                            vertices[i + 2] = ny;
                        }
                    }
                    i += 4;
                }
            }
            return true;
        };

        for (auto &sk : skeleton.skins) {
            for (auto &[slotName, attachments] : sk.attachments) {
                for (auto &[attName, attachment] : attachments) {
                    auto it = worldVerts.find({slotName, attName});
                    if (it == worldVerts.end()) continue;
                    bool did = false;
                    if (attachment.type == AttachmentType_Mesh) {
                        auto &mesh = std::get<MeshAttachment>(attachment.data);
                        did = rebindVertices(mesh.vertices, meshVertexCount(mesh), it->second);
                    } else if (attachment.type == AttachmentType_Path) {
                        auto &path = std::get<PathAttachment>(attachment.data);
                        did = rebindVertices(path.vertices, path.vertexCount, it->second);
                    } else if (attachment.type == AttachmentType_Clipping) {
                        auto &clip = std::get<ClippingAttachment>(attachment.data);
                        did = rebindVertices(clip.vertices, clip.vertexCount, it->second);
                    } else if (attachment.type == AttachmentType_Boundingbox) {
                        auto &box = std::get<BoundingboxAttachment>(attachment.data);
                        did = rebindVertices(box.vertices, box.vertexCount, it->second);
                    }
                    if (did) {
                        rebaked.insert({slotName, attName});
                        rebakedMeshes++;
                    }
                }
            }
        }
    }

    stripDeform(skeleton, rebaked);

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
              << bakedBones << " bones, " << rebakedMeshes << " weighted attachments.\n";
    delete runtimeData;
}
