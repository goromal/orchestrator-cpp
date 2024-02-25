#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
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

    int64_t push(std::unique_ptr<Job> job);

    // TODO query
    // TODO request
    // TODO pop
    // TODO dump
    // TODO load

private:
    const Container mContainer;

    std::atomic_uint8_t mSubCounter{0};

    std::mutex                        mPendingJobsMtx;
    std::vector<std::unique_ptr<Job>> mPendingJobs;
    // TODO

    void sortJobs();
};

} // end namespace orchestrator
