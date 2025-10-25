#pragma once

#include <atomic>
#include <variant>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <functional>
#include <mscpp/InputSet.h>
#include <mscpp/StateSet.h>
#include <mscpp/MicroService.h>
#include <mscpp/MicroServiceContainer.h>
#include <mscpp/Logging.h>

#include "orchestrator/Result.h"
#include "orchestrator/Job.h"

#include "orchestrator/JobDatabase.h"
#include "orchestrator/JobExecutor.h"

namespace orchestrator
{

namespace job_queue
{

inline constexpr char Name[] = "JobQueue";

struct HeartbeatInput : public services::Input<HeartbeatInput, result::EmptyResult, 0, 1000>
{
};

struct PushInput : public services::Input<PushInput, result::JobIdResult, 0, 100>
{
    Job job;
};

struct QueryInput : public services::Input<QueryInput, result::JobsListResult, 1, 10>
{
    struct GetAllQueuedJobs
    {
    };
    struct GetJobsAtPriorityLevel
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

struct Store // TODO clean up by making this a class to protect private members
{
    std::atomic_uint8_t                        subCounter{0};
    std::vector<Job>                           pendingJobs;
    std::map<int64_t, result::FutureJobResult> pendingJobResults;
    result::FutureJobQueueDataResult           pendingInitLoad;
    std::vector<Job>                           pendingInitExecs;

    /// @brief Take a new job and register it with the queue store, giving it a unique ID
    /// @param job Job to be registered and given an ID
    /// @param paused Whether or not the program is currently paused
    /// @return A globally unique, monotonically increasing ID
    int64_t addAndRegisterNewJob(Job& job, bool paused);

    /// @brief Assign a unique ID and job statuses to a job
    /// @param job Job to be given an ID
    /// @param paused Whether or not the program is currently paused
    /// @return A globally unique, monotinically increasing ID
    int64_t initializeJobData(Job& job, bool paused);

    /// @brief Sort all registered jobs in the store according to blocking status, priority, and ID
    void sortJobs();

    /// @brief Give all registered jobs a paused status, storing their previous statuses
    void pauseJobs();

    /// @brief Restore all registered paused jobs to their pre-paused statuses
    void unpauseJobs();

    /// @brief Send as many jobs to the job executor as possible within the allotted time budget
    /// @param timeBudget Allotted time budget
    /// @param jobs Job pool to process
    /// @param c Access point for the job executor
    /// @param fJobDrainCriterion Criterion to determine if a job is ready for the executor
    /// @return Whether or not all jobs were sent to the executor within the time budget
    bool timedJobDrain(const std::chrono::milliseconds&       timeBudget,
                       std::vector<Job>&                      jobs,
                       const Container&                       c,
                       const std::function<bool(const Job&)>& fJobDrainCriterion);

    /// @brief Poll pending jobs for results and clear blockers and add child jobs as necessary
    /// @param paused Whether or not the program is currently paused
    void processPendingJobResults(bool paused);

    /// @brief Return a copy of all jobs that match a query criterion
    /// @param query Query criterion with which to filter jobs
    /// @return Filtered list of jobs meeting the query criterion
    std::vector<Job> query(const QueryInput::QueryType& query);
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

// Final initial state in which formerly in-progress jobs are re-triggered
struct InitFinalWaitState : public services::State<InitFinalWaitState, 2>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

// Nominal running state
struct RunningState : public services::State<RunningState, 3>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

// Paused state in which no new active jobs get queued
struct PausedState : public services::State<PausedState, 4>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, PushInput& i);
    size_t step(Store& s, const Container& c, QueryInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

using States = services::StateSet<InitState, InitWaitState, InitFinalWaitState, RunningState, PausedState>;

using JobQueueBase = services::MicroService<Name, Store, Container, States, Inputs>;

class JobQueue : public JobQueueBase
{
public:
    JobQueue(const Container& container) : JobQueueBase(container) {}
};

} // namespace job_queue

} // end namespace orchestrator
