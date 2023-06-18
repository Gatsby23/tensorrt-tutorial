#ifndef __TRT_BUILDER_H_
#define __TRT_BUILDER_H_

#include <string>
#include <vector>
#include <functional>

namespace TRT
{
    enum class Mode : int
    {
        FP32,
        FP16
    };

    const char *mode_string(Mode type);
    bool compile(
        Mode mode,
        unsigned int maxBatchSize,
        const std::string &source,
        const std::string &saveto,
        const size_t maxWorkspaceSize = 1ul << 20 // 1ul << 30 = 1GB
    );
}

#endif
