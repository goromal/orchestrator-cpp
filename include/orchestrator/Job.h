#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <ctime>

#include <aapis/orchestrator/v1/orchestrator.pb.h>

namespace orchestrator
{

class Job
{
private:
    int64_t mSpawnMillis;
    int64_t mCounter;

    aapis::orchestrator::v1::JobStatus mStatus;

    int64_t mPriority;
    int64_t mNumBlockers;

    std::vector<int64_t> mBlockedChildren;
    std::vector<int64_t> mDependentChildren;

    int64_t mExecutionMillis;
    int64_t mCompletionMillis;

    std::vector<std::string> mOutputs;
};

} // end namespace orchestrator
