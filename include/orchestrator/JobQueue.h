#pragma once

#include <atomic>
#include <variant>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <mscpp/InputSet.h>
#include <mscpp/StateSet.h>
#include <mscpp/MicroService.h>
#include <mscpp/MicroServiceContainer.h>

#include "orchestrator/Result.h"
#include "orchestrator/Job.h"

#include "orchestrator/JobExecutor.h"

namespace orchestrator
{

namespace job_queue
{

struct HeartbeatInput : public services::Input<HeartbeatInput, result::EmptyResult, 0, 1000>
{
};

struct PushInput : public services::Input<PushInput, result::JobIdResult, 0, 100>
{
    Job job;
};

struct QueryInput : public services::Input<QueryInput, result::JobsListResult, 1, 10>
{
    // ^^^^ TODO
};

struct TogglePauseInput : public services::Input<TogglePauseInput, result::BooleanResult, 1, 5>
{
};

using Inputs = services::InputSet<HeartbeatInput, PushInput, QueryInput, TogglePauseInput>;

using Container = services::MicroServiceContainer<>; // ^^^^ TODO dependencies

struct Store // ^^^^ TODO
{
    std::atomic_uint8_t subCounter{0};
    std::vector<Job>    pendingJobs;
    int64_t             initializeJobData(Job& job, bool paused);
    void                sortJobs();
    void                pauseJobs();
    void                unpauseJobs();
};

struct RunningState : public services::State<RunningState, 0>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
};

struct PausedState : public services::State<PausedState, 0>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
};

using States = services::StateSet<RunningState, PausedState>;

using JobQueueBase = services::MicroService<Store, Container, States, Inputs>;

class JobQueue : public JobQueueBase
{
public:
    JobQueue(const Container& container) : JobQueueBase(container) {}
    const std::string name() const override;
};

} // namespace job_queue

} // end namespace orchestrator
