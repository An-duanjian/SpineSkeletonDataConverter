#include "SkeletonData.h"

#include <spine/Extension.h>

#include <iostream>
#include <string>

namespace spine {
SpineExtension *getDefaultExtension() {
    return new DefaultSpineExtension();
}
}

void bakeRuntimePoseFor3x(SkeletonData &skeleton, const std::string &inputFile) {
    // 3.8.75 Import Data must keep authored setup, bone keys, IK, and transform
    // mixes. Freezing 4.x idle+physics into setup remapped timelines and zeroed
    // constraints, which froze tousheng/hair motion and mis-skinned meshes.
    // 4.2 physics cannot run in 3.8; only keyed/constraint animation is preserved.
    (void)skeleton;
    (void)inputFile;
}
