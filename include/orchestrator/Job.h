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
    int64_t id{-1};
    // int64_t parentId{-1}; TODO shouldn't be necessary

    aapis::orchestrator::v1::JobStatus status;
    aapis::orchestrator::v1::JobStatus prePauseStatus{aapis::orchestrator::v1::JobStatus::JOB_STATUS_INVALID};

    int64_t priority{0};

    int64_t spawnTimeSeconds{-1};
    int64_t executionTimeSeconds{-1};
    int64_t completionTimestampSeconds{-1};

    // Blockers whose outputs have no relevance to this job
    std::vector<int64_t> independentBlockers;
    // Blockers whose outputs (and whose children's outputs) must become additional inputs to this job
    std::vector<int64_t> relevantBlockers;

    size_t numBlockers() const
    {
        return independentBlockers.size() + relevantBlockers.size();
    }

    // Populated using client-specified inputs as well as relevantBlockers' outputs obtained via database query
    std::vector<std::string> inputs;
};

} // end namespace orchestrator
