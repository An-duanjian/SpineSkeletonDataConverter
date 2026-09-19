#include "SkeletonData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

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
