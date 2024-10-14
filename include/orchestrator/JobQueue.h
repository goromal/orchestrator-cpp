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

#include "orchestrator/JobDatabase.h"
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
    typedef struct GetAllQueuedJobs
    {
    };
    typedef struct GetJobsAtPriorityLevel
    {
        int64_t priority;
    };
    using QueryType = std::variant<GetAllQueuedJobs, GetJobsAtPriorityLevel>;
    QueryType query;
};

struct TogglePauseInput : public services::Input<TogglePauseInput, result::BooleanResult, 2, 5>
{
};

struct DumpInput : public services::Input<DumpInput, result::BooleanResult, 1, 500>
{
};

using Inputs = services::InputSet<HeartbeatInput, PushInput, QueryInput, TogglePauseInput, DumpInput>;

using Container = services::MicroServiceContainer<job_executor::JobExecutor, job_database::JobDatabase>;

struct Store // ^^^^ TODO make this a class with private members
{
    std::atomic_uint8_t                        subCounter{0};
    std::vector<Job>                           pendingJobs;
    std::map<int64_t, result::FutureJobResult> pendingJobResults;
    result::FutureJobQueueDataResult           pendingInitLoad;
    int64_t                                    addAndRegisterNewJob(Job& job, bool paused);
    int64_t                                    initializeJobData(Job& job, bool paused);
    void                                       sortJobs();
    void                                       pauseJobs();
    void                                       unpauseJobs();
    std::vector<Job>                           processPendingJobResults(bool paused);
    std::vector<Job>                           query(const QueryInput::QueryType& query);
};

// Initial state in which any persistent memory is requested to be loaded
struct InitState : public services::State<InitState, 0>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

// Follow-on initial state in which persistent memory is actually loaded
struct InitWaitState : public services::State<InitWaitState, 1>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

// Nominal running state
struct RunningState : public services::State<RunningState, 2>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

// Paused state in which no new active jobs get queued
struct PausedState : public services::State<PausedState, 3>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

using States = services::StateSet<InitState, RunningState, PausedState>;

using JobQueueBase = services::MicroService<Store, Container, States, Inputs>;

class JobQueue : public JobQueueBase
{
public:
    JobQueue(const Container& container) : JobQueueBase(container) {}
    const std::string name() const override;
};

} // namespace job_queue

} // end namespace orchestrator
