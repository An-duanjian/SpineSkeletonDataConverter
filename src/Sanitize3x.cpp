#include "SkeletonData.h"

#include <algorithm>
#include <cmath>
#include <set>

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

void sanitizeSkeletonDataFor3x(SkeletonData& skeleton) {
    skeleton.physicsConstraints.clear();

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
