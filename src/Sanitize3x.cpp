#include "SkeletonData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <cstddef>

namespace {

constexpr float kCurveMin = 0.0f;
constexpr float kCurveMax = 1.0f;

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(hi, value));
}

unsigned char floatToByte(float value) {
    float scaled = std::round(clampf(value, 0.0f, 1.0f) * 255.0f);
    return static_cast<unsigned char>(scaled);
}

float holdValue1(const Timeline& timeline, float time, float defaultValue) {
    if (timeline.empty()) return defaultValue;
    const TimelineFrame* previous = &timeline.front();
    for (const auto& frame : timeline) {
        if (frame.time > time) break;
        previous = &frame;
    }
    return previous->value1;
}

Color holdColor(const Timeline& timeline, float time, const Color& defaultColor) {
    if (timeline.empty()) return defaultColor;
    const TimelineFrame* previous = &timeline.front();
    for (const auto& frame : timeline) {
        if (frame.time > time) break;
        previous = &frame;
    }
    return previous->color1.value_or(defaultColor);
}

void clampBezier(Timeline& timeline) {
    for (auto& frame : timeline) {
        if (frame.curveType != CurveType::CURVE_BEZIER) continue;
        if (frame.curve.size() < 4) {
            frame.curve = {0.0f, 0.0f, 1.0f, 1.0f};
            continue;
        }
        for (size_t i = 0; i < 4; ++i) {
            float value = frame.curve[i];
            if (!std::isfinite(value)) {
                value = (i == 0 || i == 1) ? 0.0f : 1.0f;
            }
            frame.curve[i] = clampf(value, kCurveMin, kCurveMax);
        }
        frame.curve.resize(4);
    }
}

void mergeSingleAxisTimelines(MultiTimeline& timelines, const std::string& combined,
                              const std::string& axisX, const std::string& axisY,
                              float defaultX, float defaultY) {
    const bool hasCombined = timelines.contains(combined);
    const bool hasX = timelines.contains(axisX);
    const bool hasY = timelines.contains(axisY);
    if (!hasX && !hasY) return;

    if (hasCombined) {
        timelines.erase(axisX);
        timelines.erase(axisY);
        return;
    }

    std::set<float> times;
    if (hasX) {
        for (const auto& frame : timelines[axisX]) times.insert(frame.time);
    }
    if (hasY) {
        for (const auto& frame : timelines[axisY]) times.insert(frame.time);
    }

    Timeline merged;
    merged.reserve(times.size());
    for (float time : times) {
        TimelineFrame frame;
        frame.time = time;
        frame.value1 = hasX ? holdValue1(timelines[axisX], time, defaultX) : defaultX;
        frame.value2 = hasY ? holdValue1(timelines[axisY], time, defaultY) : defaultY;
        bool copiedCurve = false;
        if (hasX) {
            for (const auto& src : timelines[axisX]) {
                if (src.time == time) {
                    frame.curveType = src.curveType;
                    frame.curve = src.curve;
                    copiedCurve = true;
                    break;
                }
            }
        }
        if (!copiedCurve && hasY) {
            for (const auto& src : timelines[axisY]) {
                if (src.time == time) {
                    frame.curveType = src.curveType;
                    frame.curve = src.curve;
                    break;
                }
            }
        }
        merged.push_back(std::move(frame));
    }

    timelines[combined] = std::move(merged);
    timelines.erase(axisX);
    timelines.erase(axisY);
}

void bakeAlphaTimeline(MultiTimeline& slotTimelines) {
    if (!slotTimelines.contains("alpha")) return;

    const Timeline& alpha = slotTimelines["alpha"];
    const std::string colorKey = slotTimelines.contains("rgba") ? "rgba" :
                                 (slotTimelines.contains("rgb") ? "rgb" : "");

    if (colorKey.empty()) {
        Timeline rgba;
        rgba.reserve(alpha.size());
        for (const auto& frame : alpha) {
            TimelineFrame out = frame;
            Color color{255, 255, 255, floatToByte(frame.value1)};
            out.color1 = color;
            rgba.push_back(std::move(out));
        }
        slotTimelines["rgba"] = std::move(rgba);
        slotTimelines.erase("alpha");
        return;
    }

    Timeline& colorTimeline = slotTimelines[colorKey];
    std::set<float> times;
    for (const auto& frame : colorTimeline) times.insert(frame.time);
    for (const auto& frame : alpha) times.insert(frame.time);

    Timeline baked;
    baked.reserve(times.size());
    for (float time : times) {
        TimelineFrame out;
        out.time = time;
        Color color = holdColor(colorTimeline, time, Color{255, 255, 255, 255});
        color.a = floatToByte(holdValue1(alpha, time, color.a / 255.0f));
        out.color1 = color;
        for (const auto& src : colorTimeline) {
            if (src.time == time) {
                out.curveType = src.curveType;
                out.curve = src.curve;
                if (src.color2) out.color2 = src.color2;
                break;
            }
        }
        baked.push_back(std::move(out));
    }
    colorTimeline = std::move(baked);
    slotTimelines.erase("alpha");
}

bool isBoneTimelineSupported(const std::string& name) {
    return name == "rotate" || name == "translate" || name == "scale" || name == "shear";
}

bool animationHasContent(const Animation& animation) {
    if (!animation.slots.empty()) return true;
    if (!animation.bones.empty()) return true;
    if (!animation.ik.empty()) return true;
    if (!animation.transform.empty()) return true;
    if (!animation.path.empty()) return true;
    if (!animation.drawOrder.empty()) return true;
    if (!animation.events.empty()) return true;
    for (const auto& [skinName, skinMap] : animation.attachments) {
        for (const auto& [slotName, slotMap] : skinMap) {
            for (const auto& [attachmentName, timelines] : slotMap) {
                if (timelines.contains("deform") && !timelines.at("deform").empty()) {
                    return true;
                }
            }
        }
    }
    return false;
}

void pruneEmptyTimelines(MultiTimeline& timelines) {
    for (auto it = timelines.begin(); it != timelines.end();) {
        if (it->second.empty()) it = timelines.erase(it);
        else ++it;
    }
}

}

void normalizeMeshHullFor3x(SkeletonData& skeleton) {
    int clamped = 0;
    for (auto& skin : skeleton.skins) {
        for (auto& [slotName, slotMap] : skin.attachments) {
            for (auto& [attachmentName, attachment] : slotMap) {
                if (attachment.type != AttachmentType_Mesh) continue;
                auto& mesh = std::get<MeshAttachment>(attachment.data);
                int vertexCount = static_cast<int>(mesh.uvs.size() / 2);
                if (vertexCount <= 0 || mesh.hullLength <= 0) continue;
                // 4.2 binary hull is H (T = 2V - H - 2). 3.8.75 JSON hull is also
                // H; the editor then does H << 1. Do not halve.
                if (mesh.hullLength > vertexCount) {
                    mesh.hullLength = vertexCount;
                    clamped++;
                }
                if (mesh.hullLength > 0 && mesh.hullLength < 3) {
                    mesh.hullLength = vertexCount >= 3 ? vertexCount : 0;
                    clamped++;
                }
            }
        }
    }
    if (clamped > 0) {
        std::cout << "Clamped " << clamped << " mesh hull values for 3.8 JSON vertex counts.\n";
    }
}

void generateMissingMeshEdges(SkeletonData& skeleton) {
    int generated = 0;
    for (auto& skin : skeleton.skins) {
        for (auto& [slotName, slotMap] : skin.attachments) {
            for (auto& [attachmentName, attachment] : slotMap) {
                if (attachment.type != AttachmentType_Mesh) continue;
                auto& mesh = std::get<MeshAttachment>(attachment.data);
                if (!mesh.edges.empty()) continue;
                int vertexCount = static_cast<int>(mesh.uvs.size() / 2);
                if (vertexCount < 3) continue;

                int hull = mesh.hullLength;
                if (hull < 3 || hull > vertexCount) hull = vertexCount;

                std::set<std::pair<int, int>> uniqueEdges;
                auto addEdge = [&](int a, int b) {
                    if (a == b) return;
                    if (a > b) std::swap(a, b);
                    uniqueEdges.emplace(a, b);
                };

                for (int i = 0; i < hull; ++i) {
                    addEdge(i, (i + 1) % hull);
                }
                for (size_t i = 0; i + 2 < mesh.triangles.size(); i += 3) {
                    int a = mesh.triangles[i];
                    int b = mesh.triangles[i + 1];
                    int c = mesh.triangles[i + 2];
                    if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
                    addEdge(a, b);
                    addEdge(b, c);
                    addEdge(c, a);
                }

                mesh.edges.reserve(uniqueEdges.size() * 2);
                for (const auto& [a, b] : uniqueEdges) {
                    mesh.edges.push_back(static_cast<unsigned short>(a * 2));
                    mesh.edges.push_back(static_cast<unsigned short>(b * 2));
                }
                generated++;
            }
        }
    }
    std::cout << "Generated mesh edges for " << generated << " attachments (editor hull/internal edges).\n";
}

bool isWeightedVertices(const std::vector<float>& vertices, int vertexCount) {
    return vertexCount > 0 && vertices.size() != static_cast<size_t>(vertexCount * 2);
}

bool meshNeedsWeightBake(const std::vector<float>& vertices, int vertexCount) {
    if (!isWeightedVertices(vertices, vertexCount)) return false;
    size_t i = 0;
    for (int v = 0; v < vertexCount && i < vertices.size(); ++v) {
        int boneCount = static_cast<int>(vertices[i++]);
        if (boneCount > 4) return true;
        i += static_cast<size_t>(std::max(boneCount, 0)) * 4;
    }
    return false;
}

float cosDeg(float deg) { return std::cos(deg * static_cast<float>(3.14159265358979323846 / 180.0)); }
float sinDeg(float deg) { return std::sin(deg * static_cast<float>(3.14159265358979323846 / 180.0)); }

void applySetupTransformConstraints(const SkeletonData& skeleton, std::vector<BoneWorld>& worlds) {
    std::map<std::string, int> index;
    for (int i = 0; i < static_cast<int>(skeleton.bones.size()); ++i) {
        if (skeleton.bones[static_cast<size_t>(i)].name)
            index[*skeleton.bones[static_cast<size_t>(i)].name] = i;
    }
    constexpr float pi = static_cast<float>(3.14159265358979323846);
    auto constraints = skeleton.transformConstraints;
    std::sort(constraints.begin(), constraints.end(), [](const TransformConstraintData& a, const TransformConstraintData& b) {
        return a.order < b.order;
    });
    for (const auto& tc : constraints) {
        if (tc.local || tc.relative) continue;
        if (!tc.target || !index.contains(*tc.target)) continue;
        const BoneWorld& target = worlds[static_cast<size_t>(index[*tc.target])];
        float ta = target.a, tb = target.b, tcA = target.c, td = target.d;
        float degRadReflect = (ta * td - tb * tcA > 0) ? (pi / 180.0f) : (-pi / 180.0f);
        float offsetRotation = tc.offsetRotation * degRadReflect;
        bool translate = tc.mixX != 0.0f || tc.mixY != 0.0f;
        for (const auto& boneName : tc.bones) {
            if (!index.contains(boneName)) continue;
            BoneWorld& bone = worlds[static_cast<size_t>(index[boneName])];
            if (tc.mixRotate != 0.0f) {
                float r = std::atan2(tcA, ta) - std::atan2(bone.c, bone.a) + offsetRotation;
                if (r > pi) r -= pi * 2.0f;
                else if (r < -pi) r += pi * 2.0f;
                r *= tc.mixRotate;
                float cosine = std::cos(r), sine = std::sin(r);
                float a = bone.a, b = bone.b, c = bone.c, d = bone.d;
                bone.a = cosine * a - sine * c;
                bone.b = cosine * b - sine * d;
                bone.c = sine * a + cosine * c;
                bone.d = sine * b + cosine * d;
            }
            if (translate) {
                float tx = ta * tc.offsetX + tb * tc.offsetY + target.x;
                float ty = tcA * tc.offsetX + td * tc.offsetY + target.y;
                bone.x += (tx - bone.x) * tc.mixX;
                bone.y += (ty - bone.y) * tc.mixY;
            }
            if (tc.mixScaleX > 0.0f) {
                float s = std::sqrt(bone.a * bone.a + bone.c * bone.c);
                if (s != 0.0f) {
                    s = (s + (std::sqrt(ta * ta + tcA * tcA) - s + tc.offsetScaleX) * tc.mixScaleX) / s;
                    bone.a *= s;
                    bone.c *= s;
                }
            }
            if (tc.mixScaleY > 0.0f) {
                float s = std::sqrt(bone.b * bone.b + bone.d * bone.d);
                if (s != 0.0f) {
                    s = (s + (std::sqrt(tb * tb + td * td) - s + tc.offsetScaleY) * tc.mixScaleY) / s;
                    bone.b *= s;
                    bone.d *= s;
                }
            }
        }
    }
}

std::vector<BoneWorld> computeBoneWorlds(const SkeletonData& skeleton) {
    std::map<std::string, int> index;
    for (int i = 0; i < static_cast<int>(skeleton.bones.size()); ++i) {
        if (skeleton.bones[static_cast<size_t>(i)].name) index[*skeleton.bones[static_cast<size_t>(i)].name] = i;
    }
    std::vector<BoneWorld> worlds(skeleton.bones.size());
    for (int i = 0; i < static_cast<int>(skeleton.bones.size()); ++i) {
        const BoneData& bone = skeleton.bones[static_cast<size_t>(i)];
        int parent = -1;
        if (bone.parent && index.contains(*bone.parent)) parent = index[*bone.parent];
        float rotation = bone.rotation, scaleX = bone.scaleX, scaleY = bone.scaleY;
        float shearX = bone.shearX, shearY = bone.shearY, x = bone.x, y = bone.y;
        if (parent < 0) {
            float rotationY = rotation + 90.0f + shearY;
            worlds[static_cast<size_t>(i)] = {
                cosDeg(rotation + shearX) * scaleX,
                cosDeg(rotationY) * scaleY,
                sinDeg(rotation + shearX) * scaleX,
                sinDeg(rotationY) * scaleY,
                x, y
            };
            continue;
        }
        const BoneWorld& pworld = worlds[static_cast<size_t>(parent)];
        BoneWorld w;
        w.x = pworld.a * x + pworld.b * y + pworld.x;
        w.y = pworld.c * x + pworld.d * y + pworld.y;
        float pa = pworld.a, pb = pworld.b, pc = pworld.c, pd = pworld.d;
        switch (bone.inherit) {
            case Inherit_OnlyTranslation: {
                float rotationY = rotation + 90.0f + shearY;
                w.a = cosDeg(rotation + shearX) * scaleX;
                w.b = cosDeg(rotationY) * scaleY;
                w.c = sinDeg(rotation + shearX) * scaleX;
                w.d = sinDeg(rotationY) * scaleY;
                break;
            }
            case Inherit_NoRotationOrReflection: {
                float s = pa * pa + pc * pc;
                float prx;
                if (s > 0.0001f) {
                    s = std::abs(pa * pd - pb * pc) / s;
                    pb = pc * s;
                    pd = pa * s;
                    prx = std::atan2(pc, pa) * static_cast<float>(180.0 / 3.14159265358979323846);
                } else {
                    pa = 0;
                    pc = 0;
                    prx = 90.0f - std::atan2(pd, pb) * static_cast<float>(180.0 / 3.14159265358979323846);
                }
                float rx = rotation + shearX - prx;
                float ry = rotation + shearY - prx + 90.0f;
                float la = cosDeg(rx) * scaleX;
                float lb = cosDeg(ry) * scaleY;
                float lc = sinDeg(rx) * scaleX;
                float ld = sinDeg(ry) * scaleY;
                w.a = pa * la - pb * lc;
                w.b = pa * lb - pb * ld;
                w.c = pc * la + pd * lc;
                w.d = pc * lb + pd * ld;
                break;
            }
            case Inherit_NoScale:
            case Inherit_NoScaleOrReflection: {
                float cosine = cosDeg(rotation);
                float sine = sinDeg(rotation);
                float za = pa * cosine + pb * sine;
                float zc = pc * cosine + pd * sine;
                float s = std::sqrt(za * za + zc * zc);
                if (s > 0.00001f) s = 1.0f / s;
                za *= s;
                zc *= s;
                s = std::sqrt(za * za + zc * zc);
                if (bone.inherit == Inherit_NoScale && (pa * pd - pb * pc < 0)) s = -s;
                float rot90 = static_cast<float>(3.14159265358979323846 / 2.0) + std::atan2(zc, za);
                float zb = std::cos(rot90) * s;
                float zd = std::sin(rot90) * s;
                float la = cosDeg(shearX) * scaleX;
                float lb = cosDeg(90.0f + shearY) * scaleY;
                float lc = sinDeg(shearX) * scaleX;
                float ld = sinDeg(90.0f + shearY) * scaleY;
                w.a = za * la + zb * lc;
                w.b = za * lb + zb * ld;
                w.c = zc * la + zd * lc;
                w.d = zc * lb + zd * ld;
                break;
            }
            case Inherit_Normal:
            default: {
                float rotationY = rotation + 90.0f + shearY;
                float la = cosDeg(rotation + shearX) * scaleX;
                float lb = cosDeg(rotationY) * scaleY;
                float lc = sinDeg(rotation + shearX) * scaleX;
                float ld = sinDeg(rotationY) * scaleY;
                w.a = pa * la + pb * lc;
                w.b = pa * lb + pb * ld;
                w.c = pc * la + pd * lc;
                w.d = pc * lb + pd * ld;
                break;
            }
        }
        worlds[static_cast<size_t>(i)] = w;
    }
    return worlds;
}

void worldToLocal(const BoneWorld& bone, float worldX, float worldY, float& localX, float& localY) {
    float inv = bone.a * bone.d - bone.b * bone.c;
    float dx = worldX - bone.x;
    float dy = worldY - bone.y;
    if (std::abs(inv) < 1e-8f) {
        localX = dx;
        localY = dy;
        return;
    }
    inv = 1.0f / inv;
    localX = (dx * bone.d - dy * bone.b) * inv;
    localY = (dy * bone.a - dx * bone.c) * inv;
}

struct BoneInfluence {
    int bone = 0;
    float lx = 0, ly = 0, weight = 0;
};

void skinWorld(const std::vector<BoneInfluence>& inf, const std::vector<BoneWorld>& worlds, float& wx, float& wy) {
    wx = 0;
    wy = 0;
    for (const auto& infItem : inf) {
        if (infItem.bone < 0 || infItem.bone >= static_cast<int>(worlds.size())) continue;
        const BoneWorld& bw = worlds[static_cast<size_t>(infItem.bone)];
        wx += infItem.weight * (bw.a * infItem.lx + bw.b * infItem.ly + bw.x);
        wy += infItem.weight * (bw.c * infItem.lx + bw.d * infItem.ly + bw.y);
    }
}

void collapseInfluencesFor3x(std::vector<BoneInfluence>& inf, const std::vector<BoneWorld>& worlds, int fallbackBone) {
    if (inf.empty()) {
        BoneInfluence one;
        one.bone = std::max(0, fallbackBone);
        one.weight = 1.0f;
        inf.push_back(one);
        return;
    }
    float wx = 0, wy = 0;
    skinWorld(inf, worlds, wx, wy);
    struct Ranked {
        BoneInfluence item;
        float dist2 = 0;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(inf.size());
    for (const auto& item : inf) {
        if (item.weight <= 1e-6f) continue;
        int bone = item.bone;
        if (bone < 0 || bone >= static_cast<int>(worlds.size())) bone = fallbackBone;
        const BoneWorld& bw = worlds[static_cast<size_t>(std::max(0, bone))];
        float dx = bw.x - wx;
        float dy = bw.y - wy;
        Ranked r;
        r.item = item;
        r.item.bone = bone;
        r.dist2 = dx * dx + dy * dy;
        ranked.push_back(r);
    }
    if (ranked.empty()) {
        inf.clear();
        BoneInfluence one;
        one.bone = std::max(0, fallbackBone);
        one.weight = 1.0f;
        worldToLocal(worlds[static_cast<size_t>(one.bone)], wx, wy, one.lx, one.ly);
        inf.push_back(one);
        return;
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.dist2 < b.dist2;
    });
    float nearest = ranked.front().dist2;
    float maxDist2 = std::max(80.0f * 80.0f, nearest * 9.0f);
    inf.clear();
    for (const auto& r : ranked) {
        if (static_cast<int>(inf.size()) >= 4) break;
        if (r.dist2 > maxDist2 && !inf.empty()) break;
        inf.push_back(r.item);
    }
    float sum = 0;
    for (const auto& item : inf) sum += item.weight;
    if (sum <= 1e-8f) {
        inf.resize(1);
        inf[0].weight = 1.0f;
        worldToLocal(worlds[static_cast<size_t>(inf[0].bone)], wx, wy, inf[0].lx, inf[0].ly);
        return;
    }
    for (auto& item : inf) {
        item.weight /= sum;
        worldToLocal(worlds[static_cast<size_t>(item.bone)], wx, wy, item.lx, item.ly);
    }
}

float boneWorldRotationDeg(const BoneWorld& bone) {
    return std::atan2(bone.c, bone.a) * static_cast<float>(180.0 / 3.14159265358979323846);
}

void transformRegionToBone(RegionAttachment& region, const BoneWorld& from, const BoneWorld& to) {
    float wx = from.a * region.x + from.b * region.y + from.x;
    float wy = from.c * region.x + from.d * region.y + from.y;
    float worldRot = boneWorldRotationDeg(from) + region.rotation;
    worldToLocal(to, wx, wy, region.x, region.y);
    region.rotation = worldRot - boneWorldRotationDeg(to);
}

void writeWeightedVertices(const std::vector<std::vector<BoneInfluence>>& vertices, std::vector<float>& out) {
    out.clear();
    for (const auto& inf : vertices) {
        out.push_back(static_cast<float>(inf.size()));
        for (const auto& item : inf) {
            out.push_back(static_cast<float>(item.bone));
            out.push_back(item.lx);
            out.push_back(item.ly);
            out.push_back(item.weight);
        }
    }
}

bool parseWeightedVertices(const std::vector<float>& vertices, int vertexCount, std::vector<std::vector<BoneInfluence>>& parsed) {
    parsed.assign(static_cast<size_t>(vertexCount), {});
    size_t i = 0;
    for (int v = 0; v < vertexCount && i < vertices.size(); ++v) {
        int boneCount = static_cast<int>(vertices[i++]);
        for (int b = 0; b < boneCount && i + 3 < vertices.size(); ++b) {
            BoneInfluence item;
            item.bone = static_cast<int>(std::lround(vertices[i++]));
            item.lx = vertices[i++];
            item.ly = vertices[i++];
            item.weight = vertices[i++];
            parsed[static_cast<size_t>(v)].push_back(item);
        }
    }
    return i <= vertices.size();
}

void bakeMeshToBoneLocal(MeshAttachment& mesh, int boneIndex, const std::vector<BoneWorld>& worlds) {
    int vertexCount = static_cast<int>(mesh.uvs.size() / 2);
    if (vertexCount <= 0 || boneIndex < 0 || boneIndex >= static_cast<int>(worlds.size())) return;
    const BoneWorld& slotBone = worlds[static_cast<size_t>(boneIndex)];
    std::vector<float> local(static_cast<size_t>(vertexCount) * 2, 0.0f);
    if (isWeightedVertices(mesh.vertices, vertexCount)) {
        std::vector<std::vector<BoneInfluence>> parsed;
        parseWeightedVertices(mesh.vertices, vertexCount, parsed);
        for (int v = 0; v < vertexCount; ++v) {
            float wx = 0, wy = 0;
            skinWorld(parsed[static_cast<size_t>(v)], worlds, wx, wy);
            worldToLocal(slotBone, wx, wy, local[static_cast<size_t>(v) * 2], local[static_cast<size_t>(v) * 2 + 1]);
        }
    } else if (mesh.vertices.size() >= static_cast<size_t>(vertexCount) * 2) {
        for (int v = 0; v < vertexCount; ++v) {
            local[static_cast<size_t>(v) * 2] = mesh.vertices[static_cast<size_t>(v) * 2];
            local[static_cast<size_t>(v) * 2 + 1] = mesh.vertices[static_cast<size_t>(v) * 2 + 1];
        }
    }
    mesh.vertices.swap(local);
}

void stripDeformForAttachments(SkeletonData& skeleton, const std::set<std::string>& bakedKeys) {
    if (bakedKeys.empty()) return;
    for (auto& animation : skeleton.animations) {
        for (auto skinIt = animation.attachments.begin(); skinIt != animation.attachments.end();) {
            for (auto slotIt = skinIt->second.begin(); slotIt != skinIt->second.end();) {
                for (auto attIt = slotIt->second.begin(); attIt != slotIt->second.end();) {
                    std::string key = skinIt->first + "\n" + slotIt->first + "\n" + attIt->first;
                    if (bakedKeys.contains(key)) attIt = slotIt->second.erase(attIt);
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

int weightedInfluenceCount(const std::vector<float>& vertices, int vertexCount) {
    if (vertexCount <= 0) return 0;
    if (vertices.size() == static_cast<size_t>(vertexCount) * 2) return 0;
    int influences = 0;
    size_t i = 0;
    for (int v = 0; v < vertexCount && i < vertices.size(); ++v) {
        int boneCount = static_cast<int>(vertices[i++]);
        influences += std::max(boneCount, 0);
        i += static_cast<size_t>(std::max(boneCount, 0)) * 4;
    }
    return influences;
}

int expectedDeformFloats(const Attachment& attachment, const SkeletonData& skeleton) {
    if (attachment.type == AttachmentType_Mesh) {
        const auto& mesh = std::get<MeshAttachment>(attachment.data);
        int vertexCount = mesh.uvs.empty() ? 0 : static_cast<int>(mesh.uvs.size() / 2);
        int influences = weightedInfluenceCount(mesh.vertices, vertexCount);
        if (influences > 0) return influences * 2;
        return vertexCount * 2;
    }
    if (attachment.type == AttachmentType_Linkedmesh) {
        const auto& linked = std::get<LinkedmeshAttachment>(attachment.data);
        for (const auto& skin : skeleton.skins) {
            for (const auto& [slotName, slotMap] : skin.attachments) {
                auto it = slotMap.find(linked.parentMesh);
                if (it == slotMap.end()) continue;
                if (it->second.type == AttachmentType_Linkedmesh) continue;
                return expectedDeformFloats(it->second, skeleton);
            }
        }
        return 0;
    }
    if (attachment.type == AttachmentType_Path) {
        const auto& path = std::get<PathAttachment>(attachment.data);
        int influences = weightedInfluenceCount(path.vertices, path.vertexCount);
        if (influences > 0) return influences * 2;
        return path.vertexCount * 2;
    }
    if (attachment.type == AttachmentType_Clipping) {
        const auto& clip = std::get<ClippingAttachment>(attachment.data);
        int influences = weightedInfluenceCount(clip.vertices, clip.vertexCount);
        if (influences > 0) return influences * 2;
        return clip.vertexCount * 2;
    }
    if (attachment.type == AttachmentType_Boundingbox) {
        const auto& box = std::get<BoundingboxAttachment>(attachment.data);
        int influences = weightedInfluenceCount(box.vertices, box.vertexCount);
        if (influences > 0) return influences * 2;
        return box.vertexCount * 2;
    }
    return 0;
}

void clampDeformTimelinesFor3x(SkeletonData& skeleton) {
    std::map<std::pair<std::string, std::string>, int> expected;
    for (const auto& skin : skeleton.skins) {
        for (const auto& [slotName, slotMap] : skin.attachments) {
            for (const auto& [attName, attachment] : slotMap) {
                int n = expectedDeformFloats(attachment, skeleton);
                if (n > 0) expected[{slotName, attName}] = n;
            }
        }
    }

    int clamped = 0;
    int stripped = 0;
    for (auto& animation : skeleton.animations) {
        for (auto skinIt = animation.attachments.begin(); skinIt != animation.attachments.end();) {
            for (auto slotIt = skinIt->second.begin(); slotIt != skinIt->second.end();) {
                for (auto attIt = slotIt->second.begin(); attIt != slotIt->second.end();) {
                    auto expIt = expected.find({slotIt->first, attIt->first});
                    if (expIt == expected.end() || !attIt->second.contains("deform")) {
                        ++attIt;
                        continue;
                    }
                    const int dest = expIt->second;
                    auto& frames = attIt->second["deform"];
                    bool any = false;
                    for (auto& frame : frames) {
                        int offset = frame.int1;
                        if (offset < 0) offset = 0;
                        if (offset >= dest) {
                            frame.int1 = 0;
                            frame.vertices.clear();
                            clamped++;
                            continue;
                        }
                        size_t allowed = static_cast<size_t>(dest - offset);
                        if (frame.vertices.size() > allowed) {
                            frame.vertices.resize(allowed);
                            clamped++;
                        }
                        if (!frame.vertices.empty()) any = true;
                    }
                    if (!any) {
                        attIt = slotIt->second.erase(attIt);
                        stripped++;
                    } else {
                        ++attIt;
                    }
                }
                if (slotIt->second.empty()) slotIt = skinIt->second.erase(slotIt);
                else ++slotIt;
            }
            if (skinIt->second.empty()) skinIt = animation.attachments.erase(skinIt);
            else ++skinIt;
        }
    }
    if (clamped > 0 || stripped > 0) {
        std::cout << "Clamped deform timelines for 3.8 import (" << clamped
                  << " frames, " << stripped << " empty attachments removed).\n";
    }
}

void bakeHighInfluenceMeshesFor3x(SkeletonData& skeleton) {
    // Spine 3.8.75 Import Data (decompiled ka/kR) accepts any number of bone
    // weights per vertex. It skins weighted meshes from the imported locals and
    // does not clamp to 4 influences. Rewriting weights or moving helper bones
    // is asset-specific and distorts setup. Physics is already stripped; 3.8 can
    // only show the authored bind pose, not a 4.2 physics rest pose.
    (void)skeleton;
}

void sanitizeSkeletonDataFor3x(SkeletonData& skeleton) {
    skeleton.physicsConstraints.clear();
    skeleton.nonessential = true;
    if (!skeleton.imagesPath || skeleton.imagesPath->empty()) {
        skeleton.imagesPath = "./images";
    }

    bool hasDefaultSkin = false;
    for (auto& skin : skeleton.skins) {
        skin.physics.clear();
        if (skin.name == "default") hasDefaultSkin = true;
    }
    if (!hasDefaultSkin) {
        Skin defaultSkin;
        defaultSkin.name = "default";
        skeleton.skins.insert(skeleton.skins.begin(), defaultSkin);
    }

    for (auto animationIt = skeleton.animations.begin(); animationIt != skeleton.animations.end();) {
        Animation& animation = *animationIt;
        animation.physics.clear();

        for (auto slotIt = animation.slots.begin(); slotIt != animation.slots.end();) {
            bakeAlphaTimeline(slotIt->second);
            pruneEmptyTimelines(slotIt->second);
            slotIt->second.erase("sequence");
            if (slotIt->second.empty()) slotIt = animation.slots.erase(slotIt);
            else ++slotIt;
        }

        for (auto boneIt = animation.bones.begin(); boneIt != animation.bones.end();) {
            mergeSingleAxisTimelines(boneIt->second, "translate", "translatex", "translatey", 0.0f, 0.0f);
            mergeSingleAxisTimelines(boneIt->second, "scale", "scalex", "scaley", 1.0f, 1.0f);
            mergeSingleAxisTimelines(boneIt->second, "shear", "shearx", "sheary", 0.0f, 0.0f);
            boneIt->second.erase("inherit");
            pruneEmptyTimelines(boneIt->second);
            for (auto tl = boneIt->second.begin(); tl != boneIt->second.end();) {
                if (!isBoneTimelineSupported(tl->first)) tl = boneIt->second.erase(tl);
                else ++tl;
            }
            if (boneIt->second.empty()) boneIt = animation.bones.erase(boneIt);
            else ++boneIt;
        }

        for (auto ikIt = animation.ik.begin(); ikIt != animation.ik.end();) {
            if (ikIt->second.empty()) ikIt = animation.ik.erase(ikIt);
            else ++ikIt;
        }
        for (auto transformIt = animation.transform.begin(); transformIt != animation.transform.end();) {
            if (transformIt->second.empty()) transformIt = animation.transform.erase(transformIt);
            else ++transformIt;
        }
        for (auto pathIt = animation.path.begin(); pathIt != animation.path.end();) {
            pruneEmptyTimelines(pathIt->second);
            if (pathIt->second.empty()) pathIt = animation.path.erase(pathIt);
            else ++pathIt;
        }

        for (auto skinIt = animation.attachments.begin(); skinIt != animation.attachments.end();) {
            for (auto slotIt = skinIt->second.begin(); slotIt != skinIt->second.end();) {
                for (auto attachmentIt = slotIt->second.begin(); attachmentIt != slotIt->second.end();) {
                    attachmentIt->second.erase("sequence");
                    pruneEmptyTimelines(attachmentIt->second);
                    if (!attachmentIt->second.contains("deform") || attachmentIt->second["deform"].empty()) {
                        attachmentIt = slotIt->second.erase(attachmentIt);
                    } else {
                        ++attachmentIt;
                    }
                }
                if (slotIt->second.empty()) slotIt = skinIt->second.erase(slotIt);
                else ++slotIt;
            }
            if (skinIt->second.empty()) skinIt = animation.attachments.erase(skinIt);
            else ++skinIt;
        }

        if (!animationHasContent(animation)) {
            animationIt = skeleton.animations.erase(animationIt);
        } else {
            ++animationIt;
        }
    }

    auto clampAll = [](Timeline& timeline) { clampBezier(timeline); };
    for (auto& animation : skeleton.animations) {
        for (auto& [name, timelines] : animation.slots)
            for (auto& [type, timeline] : timelines) clampAll(timeline);
        for (auto& [name, timelines] : animation.bones)
            for (auto& [type, timeline] : timelines) clampAll(timeline);
        for (auto& [name, timeline] : animation.ik) clampAll(timeline);
        for (auto& [name, timeline] : animation.transform) clampAll(timeline);
        for (auto& [name, timelines] : animation.path)
            for (auto& [type, timeline] : timelines) clampAll(timeline);
        for (auto& [skinName, skinMap] : animation.attachments)
            for (auto& [slotName, slotMap] : skinMap)
                for (auto& [attachmentName, timelines] : slotMap)
                    for (auto& [type, timeline] : timelines) clampAll(timeline);
    }

    convertOrder42ToBelow(skeleton);
}

namespace {

struct AtlasRegionSize {
    int width = 0;
    int height = 0;
};

std::string trimCopy(const std::string& input) {
    size_t start = 0;
    size_t end = input.size();
    while (start < end && std::isspace(static_cast<unsigned char>(input[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    return input.substr(start, end - start);
}

int roundDiv(int value, double scale) {
    if (scale == 1.0) return value;
    return static_cast<int>(std::lround(static_cast<double>(value) / scale));
}

bool isPlaceholderSize(float width, float height) {
    return (width == 32.0f && height == 32.0f) || (width == 0.0f && height == 0.0f);
}

std::map<std::string, AtlasRegionSize> parseAtlasRegionSizes(const std::string& atlasPath) {
    std::map<std::string, AtlasRegionSize> sizes;
    std::ifstream ifs(atlasPath);
    if (!ifs) return sizes;

    double pageScale = 1.0;
    std::string currentName;
    AtlasRegionSize current;
    bool hasRegion = false;
    bool seenPageName = false;

    auto flushRegion = [&]() {
        if (!hasRegion || currentName.empty()) return;
        if (current.width <= 0) current.width = 0;
        if (current.height <= 0) current.height = 0;
        if (current.width > 0 && current.height > 0) {
            sizes[currentName] = {
                roundDiv(current.width, pageScale),
                roundDiv(current.height, pageScale)
            };
        }
        hasRegion = false;
        current = {};
        currentName.clear();
    };

    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = trimCopy(line);
        if (trimmed.empty()) {
            flushRegion();
            seenPageName = false;
            pageScale = 1.0;
            continue;
        }

        auto colon = trimmed.find(':');
        if (colon == std::string::npos) {
            if (!seenPageName) {
                seenPageName = true;
                continue;
            }
            flushRegion();
            currentName = trimmed;
            hasRegion = true;
            current = {};
            continue;
        }

        std::string key = trimCopy(trimmed.substr(0, colon));
        std::string values = trimCopy(trimmed.substr(colon + 1));
        std::vector<int> ints;
        std::stringstream ss(values);
        std::string part;
        while (std::getline(ss, part, ',')) {
            try {
                ints.push_back(std::stoi(trimCopy(part)));
            } catch (...) {
            }
        }

        if (!hasRegion && key == "scale") {
            try {
                pageScale = std::stod(values);
                if (pageScale == 0.0) pageScale = 1.0;
            } catch (...) {
                pageScale = 1.0;
            }
            continue;
        }

        if (!hasRegion) continue;

        if (key == "offsets" && ints.size() >= 4) {
            current.width = ints[2];
            current.height = ints[3];
        } else if (key == "orig" && ints.size() >= 2) {
            current.width = ints[0];
            current.height = ints[1];
        } else if ((key == "bounds" || key == "size") && ints.size() >= (key == "bounds" ? 4 : 2) && current.width == 0) {
            if (key == "bounds" && ints.size() >= 4) {
                current.width = ints[2];
                current.height = ints[3];
            } else if (key == "size" && ints.size() >= 2) {
                current.width = ints[0];
                current.height = ints[1];
            }
        }
    }
    flushRegion();
    return sizes;
}

const AtlasRegionSize* findRegionSize(const std::map<std::string, AtlasRegionSize>& sizes, const Attachment& attachment, const std::string& keyName) {
    std::vector<std::string> candidates;
    if (!attachment.path.empty()) candidates.push_back(attachment.path);
    if (!attachment.name.empty()) candidates.push_back(attachment.name);
    if (!keyName.empty()) candidates.push_back(keyName);
    for (const auto& candidate : candidates) {
        auto it = sizes.find(candidate);
        if (it != sizes.end()) return &it->second;
    }
    return nullptr;
}

void applySize(float& width, float& height, const AtlasRegionSize& size) {
    if (size.width > 0) width = static_cast<float>(size.width);
    if (size.height > 0) height = static_cast<float>(size.height);
}

void fillUnweightedMeshSize(MeshAttachment& mesh) {
    if (!isPlaceholderSize(mesh.width, mesh.height)) return;
    int vertexCount = static_cast<int>(mesh.uvs.size() / 2);
    if (vertexCount <= 0 || mesh.vertices.size() != static_cast<size_t>(vertexCount * 2)) return;
    float minX = mesh.vertices[0], maxX = mesh.vertices[0];
    float minY = mesh.vertices[1], maxY = mesh.vertices[1];
    for (int i = 0; i < vertexCount; ++i) {
        minX = std::min(minX, mesh.vertices[i * 2]);
        maxX = std::max(maxX, mesh.vertices[i * 2]);
        minY = std::min(minY, mesh.vertices[i * 2 + 1]);
        maxY = std::max(maxY, mesh.vertices[i * 2 + 1]);
    }
    float w = maxX - minX;
    float h = maxY - minY;
    if (w > 1.0f && h > 1.0f) {
        mesh.width = w;
        mesh.height = h;
    }
}

}

void fillMeshSizesFromAtlas(SkeletonData& skeleton, const std::string& atlasPath) {
    auto sizes = parseAtlasRegionSizes(atlasPath);
    int filled = 0;
    for (auto& skin : skeleton.skins) {
        for (auto& [slotName, slotMap] : skin.attachments) {
            for (auto& [attachmentName, attachment] : slotMap) {
                const AtlasRegionSize* size = sizes.empty() ? nullptr : findRegionSize(sizes, attachment, attachmentName);
                if (attachment.type == AttachmentType_Mesh) {
                    auto& mesh = std::get<MeshAttachment>(attachment.data);
                    if (size && isPlaceholderSize(mesh.width, mesh.height)) {
                        applySize(mesh.width, mesh.height, *size);
                        filled++;
                    } else {
                        fillUnweightedMeshSize(mesh);
                    }
                } else if (attachment.type == AttachmentType_Linkedmesh) {
                    auto& linked = std::get<LinkedmeshAttachment>(attachment.data);
                    if (size && isPlaceholderSize(linked.width, linked.height)) {
                        applySize(linked.width, linked.height, *size);
                        filled++;
                    }
                }
            }
        }
    }
    if (!atlasPath.empty()) {
        std::cout << "Filled mesh/linkedmesh sizes from atlas (" << filled << " attachments): " << atlasPath << "\n";
    }
}

std::string findSiblingAtlas(const std::string& inputFile) {
    namespace fs = std::filesystem;
    fs::path input(inputFile);
    fs::path direct = input;
    direct.replace_extension(".atlas");
    if (fs::exists(direct)) return direct.string();

    fs::path dir = input.parent_path();
    if (dir.empty()) dir = fs::current_path();
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return "";
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".atlas") return entry.path().string();
    }
    return "";
}
