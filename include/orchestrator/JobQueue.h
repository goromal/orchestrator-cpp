#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <mscpp/MicroService.h>
#include <mscpp/MicroServiceContainer.h>

#include "orchestrator/Job.h"

namespace orchestrator
{

class JobQueue : public MicroService
{
public:
    using Container = MicroServiceContainer<>;

    JobQueue(const Container& container) : mContainer{container} {}
    ~JobQueue() {}

    std::string name() const override;

    // TODO

private:
    const Container mContainer;

    std::atomic_int64_t mGlobalCounter;
    std::vector<Job>    mPendingJobs;
    // TODO
};

} // end namespace orchestrator
