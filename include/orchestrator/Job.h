#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <ctime>

#include <aapis/orchestrator/v1/orchestrator.pb.h>

namespace orchestrator
{

struct Job
{
    std::string mJobKey;

    int64_t id{-1};
    // int64_t mCounter;

    aapis::orchestrator::v1::JobStatus status;

    int64_t priority;
    int64_t numBlockers;

    std::vector<int64_t> blockedChildren;
    std::vector<int64_t> dependentChildren;

    int64_t executionMillis;
    int64_t completionMillis;

    std::vector<std::string> outputs;
};

} // end namespace orchestrator
