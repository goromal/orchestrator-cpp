#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include "mscpp/InputSet.h"
#include "mscpp/StateSet.h"
#include <mscpp/MicroService.h>
#include <mscpp/MicroServiceContainer.h>

#include "orchestrator/Result.h"
#include "orchestrator/Job.h"

namespace orchestrator
{

namespace job_queue
{

struct HeartbeatInput : public services::Input<HeartbeatInput, result::EmptyResult, 0, 1000>
{
};

struct PushInput : public services::Input<PushInput, result::JobIdResult, 1, 10>
{
    Job job;
};

struct QueryInput : public services::Input<QueryInput, result::JobsListResult, 1, 10>
{
};

using Inputs = services::InputSet<HeartbeatInput, PushInput, QueryInput>;

using Container = services::MicroServiceContainer<>; // ^^^^ TODO dependencies

struct Store // ^^^^ TODO
{
    std::atomic_uint8_t subCounter{0};
    std::vector<Job>    pendingJobs;
    void                sortJobs();
};

struct InitState : public services::State<InitState, 0>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
};

using States = services::StateSet<InitState>;

using JobQueueBase = services::MicroService<Store, Container, States, Inputs>;

class JobQueue : public JobQueueBase
{
public:
    JobQueue(const Container& container) : JobQueueBase(container) {}
    const std::string name() const override;

    // int64_t push(std::unique_ptr<Job> job);

    // TODO query
    // TODO request
    // TODO pop
    // TODO dump
    // TODO load

    // private:
    // const Container mContainer;
    // TODO

    // void sortJobs();
};

} // namespace job_queue

} // end namespace orchestrator
