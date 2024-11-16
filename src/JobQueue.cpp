#include "orchestrator/JobQueue.h"
#include <chrono>
#include <algorithm>
#include <ranges>

namespace orchestrator
{

namespace job_queue
{

/// @brief Take a new job and register it with the queue store, giving it a unique ID
/// @param job Job to be registered and given an ID
/// @param paused Whether or not the program is currently paused
/// @return A globally unique, monotonically increasing ID
int64_t Store::addAndRegisterNewJob(Job& job, bool paused)
{
    auto id = initializeJobData(job, paused);

    // If the ID is already in pendingJobs, then throw an error
    if (std::find_if(pendingJobs.begin(), pendingJobs.end(), [&](Job& j) { return j.id == id; }) != pendingJobs.end())
    {
        throw std::runtime_error("Duplicate job ID would be inserted in the Job Queue");
    }

    pendingJobs.push_back(std::move(job));
    sortJobs();

    return id;
}

/// @brief Assign a unique ID and job statuses to a job
/// @param job Job to be given an ID
/// @param paused Whether or not the program is currently paused
/// @return A globally unique, monotinically increasing ID
int64_t Store::initializeJobData(Job& job, bool paused)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();

    job.spawnTimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    int64_t spawnMicrosId =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count() * 1e3 + static_cast<int64_t>(subCounter);

    subCounter++;

    job.id = spawnMicrosId;

    if (job.numBlockers() == 0)
    {
        job.status         = (paused) ? aapis::orchestrator::v1::JobStatus::JOB_STATUS_PAUSED
                                      : aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED;
        job.prePauseStatus = aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED;
    }
    else
    {
        job.status         = (paused) ? aapis::orchestrator::v1::JobStatus::JOB_STATUS_PAUSED
                                      : aapis::orchestrator::v1::JobStatus::JOB_STATUS_BLOCKED;
        job.prePauseStatus = aapis::orchestrator::v1::JobStatus::JOB_STATUS_BLOCKED;
    }

    return spawnMicrosId;
}

/// @brief Sort all registered jobs in the store according to blocking status, priority, and ID
void Store::sortJobs()
{
    std::sort(pendingJobs.begin(), pendingJobs.end(), [](const Job& a, const Job& b) {
        // Dependencies ultimately supersede priority; we don't want to get stuck
        if (std::find(a.independentBlockers.begin(), a.independentBlockers.end(), b.id) != a.independentBlockers.end())
        {
            return false;
        }
        else if (std::find(b.independentBlockers.begin(), b.independentBlockers.end(), a.id) !=
                 b.independentBlockers.end())
        {
            return true;
        }
        if (std::find(a.relevantBlockers.begin(), a.relevantBlockers.end(), b.id) != a.relevantBlockers.end())
        {
            return false;
        }
        else if (std::find(b.relevantBlockers.begin(), b.relevantBlockers.end(), a.id) != b.relevantBlockers.end())
        {
            return true;
        }
        if (a.priority < b.priority)
        {
            return true;
        }
        if (a.numBlockers() != b.numBlockers())
        {
            return a.numBlockers() < b.numBlockers();
        }
        return a.id < b.id;
    });
}

/// @brief Give all registered jobs a paused status, storing their previous statuses
void Store::pauseJobs()
{
    for (auto& job : pendingJobs)
    {
        job.prePauseStatus = job.status;
        job.status         = aapis::orchestrator::v1::JobStatus::JOB_STATUS_PAUSED;
    }
}

/// @brief Restore all registered paused jobs to their pre-paused statuses
void Store::unpauseJobs()
{
    for (auto& job : pendingJobs)
    {
        job.status = job.prePauseStatus;
    }
}

/// @brief Send as many jobs to the job executor as possible within the allotted time budget
/// @param timeBudget Allotted time budget
/// @param jobs Job pool to process
/// @param c Access point for the job executor
/// @param fJobDrainCriterion Criterion to determine if a job is ready for the executor
/// @return Whether or not all jobs were sent to the executor within the time budget
bool Store::timedJobDrain(const std::chrono::milliseconds&       timeBudget,
                          std::vector<Job>&                      jobs,
                          const Container&                       c,
                          const std::function<bool(const Job&)>& fJobDrainCriterion)
{
    static constexpr int kExecuteInputWaitMultiplier = 4;

    // Each loaded job ID must be passed to the executor to get a future back
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now   = std::chrono::steady_clock::now();
    for (auto it = jobs.begin(); it != jobs.end();)
    {
        // Don't consider jobs that don't meet the drain criteria
        if (!fJobDrainCriterion(*it))
        {
            ++it;
            continue;
        }

        // Prepare the job for execution
        auto tryExecKey    = it->id;
        auto tryExecInput  = job_executor::ExecuteInput{.job = *it};
        auto tryExecFuture = tryExecInput.getFuture();

        // Only attempt to queue this job if we have enough time budget to wait for an answer
        const auto tryExecInputWaitTime = kExecuteInputWaitMultiplier * tryExecInput.duration();
        now                             = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start) + tryExecInputWaitTime > timeBudget)
        {
            return false;
        }

        // The executor will tell us if there was room for our pending job.
        // Wait for "as long as it takes" to get this information.
        if (!c.get<job_executor::JobExecutor>()->sendInput(std::move(tryExecInput)))
        {
            return false;
        }
        while (tryExecFuture.wait_for(tryExecInputWaitTime) != std::future_status::ready)
        {
            // Our timeout underestimated how slow JobExecutor is; see if we can try again
            // TODO log a warning
        }
        // If there was no room, then exit. Else, store the future result, remove the pending job from the list, and
        // move on to trying to jump another job.
        auto tryExecResult = tryExecFuture.get();
        if (std::holds_alternative<services::ErrorResult>)
        {
            // TODO log error
            return false;
        }
        else
        {
            pendingJobResults.emplace(
                std::make_pair(tryExecKey, std::move(std::get<result::FutureJobResult>(tryExecResult))));
            it = jobs.erase(it); // this increments the iterator
        }
    }

    return jobs.size() == 0;
}

/// @brief Poll pending jobs for results and clear blockers and add child jobs as necessary
/// @param paused Whether or not the program is currently paused
void Store::processPendingJobResults(bool paused)
{
    static constexpr std::chrono::milliseconds kFutureCheckTimeout = std::chrono::milliseconds(1);

    for (auto& futJobResult : pendingJobResults)
    {
        auto jobId = futJobResult.first;
        if (futJobResult.second.wait_for(kFutureCheckTimeout) == std::future_status::ready)
        {
            auto jobResult = futJobResult.second.get();
            // If the job was unsuccessful, then mark all dependent jobs as canceled and move on. The executor will deal
            // with them.
            if (jobResult.resultStatus == aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR)
            {
                std::ranges::for_each(pendingJobs, [&](Job& j) {
                    if (std::find(j.independentBlockers.begin(), j.independentBlockers.end(), jobId) !=
                            j.independentBlockers.end() ||
                        std::find(j.relevantBlockers.begin(), j.relevantBlockers.end(), jobId) !=
                            j.relevantBlockers.end())
                    {
                        j.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED;
                    }
                });
            }
            else
            {
                // If the job returned outputs, then remove the blocker from any blocked jobs and add all outputs as
                // inputs for the case of relevantBlockers.
                if (std::holds_alternative<std::vector<std::string>>(jobResult.outputs))
                {
                    auto outputs = std::get<std::vector<std::string>>(jobResult.outputs);
                    std::ranges::for_each(pendingJobs, [&](Job& j) {
                        auto indBlockerIt =
                            std::find(j.independentBlockers.begin(), j.independentBlockers.end(), jobId);
                        if (indBlockerIt != j.independentBlockers.end())
                        {
                            j.independentBlockers.erase(indBlockerIt);
                        }
                        auto relBlockerIt = std::find(j.relevantBlockers.begin(), j.relevantBlockers.end(), jobId);
                        if (relBlockerIt != j.relevantBlockers.end())
                        {
                            j.relevantBlockers.erase(relBlockerIt);
                            std::move(outputs.begin(), outputs.end(), std::back_inserter(j.inputs));
                        }
                    });
                }
                // If the job returned child jobs, then add each child job to pendingJobs. Then, remove the parent ID
                // from any blocked jobs but add the child job IDs to the corresponding blockers list.
                else
                {
                    auto                 childJobs = std::get<std::vector<Job>>(jobResult.outputs);
                    std::vector<int64_t> childJobIds(childJobs.size());
                    std::transform(childJobs.begin(), childJobs.end(), childJobIds.begin(), [&](Job j) {
                        return addAndRegisterNewJob(j, paused);
                    });
                    std::ranges::for_each(pendingJobs, [&](Job& j) {
                        auto indBlockerIt =
                            std::find(j.independentBlockers.begin(), j.independentBlockers.end(), jobId);
                        if (indBlockerIt != j.independentBlockers.end())
                        {
                            j.independentBlockers.erase(indBlockerIt);
                            std::copy(childJobIds.begin(),
                                      childJobIds.end(),
                                      std::back_inserter(j.independentBlockers));
                        }
                        auto relBlockerIt = std::find(j.relevantBlockers.begin(), j.relevantBlockers.end(), jobId);
                        if (relBlockerIt != j.relevantBlockers.end())
                        {
                            j.relevantBlockers.erase(relBlockerIt);
                            std::copy(childJobIds.begin(), childJobIds.end(), std::back_inserter(j.relevantBlockers));
                        }
                    });
                }
            }
        }
    }
}

/// @brief Return a copy of all jobs that match a query criterion
/// @param query Query criterion with which to filter jobs
/// @return Filtered list of jobs meeting the query criterion
std::vector<Job> Store::query(const QueryInput::QueryType& query)
{
    std::vector<Job> queryResult;

    if (std::holds_alternative<QueryInput::GetAllQueuedJobs>(query))
    {
        std::copy(pendingJobs.begin(), pendingJobs.end(), std::back_inserter(queryResult));
    }
    else if (std::holds_alternative<QueryInput::GetJobsAtPriorityLevel>(query))
    {
        std::copy_if(pendingJobs.begin(), pendingJobs.end(), std::back_inserter(queryResult), [&](Job& j) {
            j.priority == std::get<QueryInput::GetJobsAtPriorityLevel>(query).priority;
        });
    }

    return queryResult;
}

const std::string JobQueue::name() const
{
    return "JobQueue";
}

size_t InitState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    // Shoot off a load data request to the database, then move on to the waiting state
    job_database::LoadQueueData loadRequest;
    s.pendingInitLoad = std::move(loadRequest.getFuture());
    c.get<job_database::JobDatabase>()->sendInput(std::move(loadRequest));
    return InitWaitState::index();
}

size_t InitState::step(Store& s, const Container& c, PushInput& i)
{
    i.setResult(services::ErrorResult{"Cannot add a new job when the queue is still initializing"});
    return InitState::index();
}

size_t InitState::step(Store& s, const Container& c, QueryInput& i)
{
    i.setResult(services::ErrorResult{"Cannot query for state when the queue is still initializing"});
    return InitState::index();
}

size_t InitState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    i.setResult(services::ErrorResult{"Cannot toggle pause when the queue is still initializing"});
    return InitState::index();
}

size_t InitState::step(Store& s, const Container& c, DumpInput& i)
{
    // The recovery database will not be deleted until we have exited the InitState, so we can safely give up
    // mid-loading here
    i.setResult(result::BooleanResult{true});
    return InitState::index();
}

size_t InitWaitState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    static constexpr std::chrono::milliseconds kFutureCheckTimeout = std::chrono::milliseconds(1);

    // Continue waiting if the init load is not ready
    if (s.pendingInitLoad.wait_for(kFutureCheckTimeout) != std::future_status::ready)
    {
        return InitWaitState::index();
    }

    auto initLoadResult = s.pendingInitLoad.get();

    if (std::holds_alternative<services::ErrorResult>(initLoadResult))
    {
        // TODO log an error
        return RunningState::index();
    }

    result::JobQueueDataResult jobQueueData = std::get<result::JobQueueDataResult>(initLoadResult);

    // Set pending jobs directly equal to the loaded data set
    s.pendingJobs = jobQueueData.first.jobs;
    s.sortJobs();

    // If there are no in-progress jobs to re-request, then jump directly to the running state
    if (jobQueueData.second.jobs.size() == 0)
    {
        return RunningState::index();
    }

    // Store the pending in-progress jobs to re-request and move on to the final init state
    s.pendingInitExecs = jobQueueData.second.jobs;
    return InitFinalWaitState::index();
}

size_t InitWaitState::step(Store& s, const Container& c, PushInput& i)
{
    i.setResult(services::ErrorResult{"Cannot add a new job when the queue is still initializing"});
    return InitWaitState::index();
}

size_t InitWaitState::step(Store& s, const Container& c, QueryInput& i)
{
    i.setResult(services::ErrorResult{"Cannot query for state when the queue is still initializing"});
    return InitWaitState::index();
}

size_t InitWaitState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    i.setResult(services::ErrorResult{"Cannot toggle pause when the queue is still initializing"});
    return InitWaitState::index();
}

size_t InitWaitState::step(Store& s, const Container& c, DumpInput& i)
{
    // The recovery database will not be deleted until we have exited the InitWaitState, so we can safely give up
    // mid-loading here
    i.setResult(result::BooleanResult{true});
    return InitWaitState::index();
}

size_t InitFinalWaitState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    // There's a lot going on in this step, so time things to ensure we can fall within our time budget
    static constexpr std::chrono::milliseconds kCheckFuturesBudget = std::chrono::milliseconds(950);

    // Each loaded job ID must be passed to the executor to get a future back
    if (!s.timedJobDrain(kCheckFuturesBudget, s.pendingInitExecs, c, [](const Job& j) { return true; }))
    {
        return InitFinalWaitState::index();
    }

    return RunningState::index();
}

size_t InitFinalWaitState::step(Store& s, const Container& c, PushInput& i)
{
    i.setResult(services::ErrorResult{"Cannot add a new job when the queue is still initializing"});
    return InitFinalWaitState::index();
}

size_t InitFinalWaitState::step(Store& s, const Container& c, QueryInput& i)
{
    i.setResult(services::ErrorResult{"Cannot query for state when the queue is still initializing"});
    return InitFinalWaitState::index();
}

size_t InitFinalWaitState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    i.setResult(services::ErrorResult{"Cannot toggle pause when the queue is still initializing"});
    return InitFinalWaitState::index();
}

size_t InitFinalWaitState::step(Store& s, const Container& c, DumpInput& i)
{
    // The recovery database will not be deleted until we have exited the InitFinalWaitState, so we can safely give up
    // mid-loading here
    i.setResult(result::BooleanResult{true});
    return InitFinalWaitState::index();
}

size_t RunningState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    // There's a lot going on in this step, so time things to ensure we can fall within our time budget
    static constexpr std::chrono::milliseconds kCheckFuturesBudget = std::chrono::milliseconds(900);

    // Part 1: Check futures for results and propagate the results to all queued jobs
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    s.processPendingJobResults(false);

    // Do we have enough time to move onto Part 2? Calculate our Part 2 budget.
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    auto part1Duration                        = std::chrono::duration_cast<std::chrono::duration<double>>(now - start);
    if (part1Duration > kCheckFuturesBudget)
    {
        return RunningState::index();
    }
    std::chrono::milliseconds kDumpJobsBudget =
        kCheckFuturesBudget - std::chrono::duration_cast<std::chrono::milliseconds>(part1Duration);

    // Part 2: Dump as many "ready" jobs onto the execution stack as we can
    if (!s.timedJobDrain(kCheckFuturesBudget, s.pendingJobs, c, [](const Job& j) { return j.numBlockers() == 0; }))
    {
        return RunningState::index();
    }

    return RunningState::index();
}

// Add an externally created job to the execution queue
size_t RunningState::step(Store& s, const Container& c, PushInput& i)
{
    i.setResult(result::JobIdResult{s.addAndRegisterNewJob(i.job, false)});
    return RunningState::index();
}

size_t RunningState::step(Store& s, const Container& c, QueryInput& i)
{
    i.setResult(result::JobsListResult{s.query(i.query)});
    return RunningState::index();
}

// Only to be run to rescue data right before shutdown!
size_t RunningState::step(Store& s, const Container& c, DumpInput& i)
{
    auto                        kv = std::views::keys(s.pendingJobResults);
    std::vector<int64_t>        keys{kv.begin(), kv.end()};
    job_database::DumpQueueData dumpInput{.pendingJobs = s.pendingJobs, .awaitedJobIds = keys};
    auto                        dumpOutput = dumpInput.getFuture();
    c.get<job_database::JobDatabase>()->sendInput(std::move(dumpInput));
    auto dumpResult = dumpOutput.get();
    if (std::holds_alternative<services::ErrorResult>(dumpResult))
    {
        i.setResult(result::BooleanResult{false});
    }
    else
    {
        i.setResult(result::BooleanResult{std::get<result::BooleanResult>(dumpResult).result});
    }
    return RunningState::index();
}

size_t RunningState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    s.pauseJobs();

    i.setResult(result::BooleanResult{true});
    return PausedState::index();
}

size_t PausedState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    // If we're paused, then only worry about cleaning up any pending job results we have left
    s.processPendingJobResults(true);
    return PausedState::index();
}

size_t PausedState::step(Store& s, const Container& c, PushInput& i)
{
    i.setResult(result::JobIdResult{s.addAndRegisterNewJob(i.job, true)});
    return PausedState::index();
}

size_t PausedState::step(Store& s, const Container& c, QueryInput& i)
{
    i.setResult(result::JobsListResult{s.query(i.query)});
    return PausedState::index();
}

size_t PausedState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    s.unpauseJobs();

    i.setResult(result::BooleanResult{true});
    return RunningState::index();
}

size_t PausedState::step(Store& s, const Container& c, DumpInput& i)
{
    auto                        kv = std::views::keys(s.pendingJobResults);
    std::vector<int64_t>        keys{kv.begin(), kv.end()};
    job_database::DumpQueueData dumpInput{.pendingJobs = s.pendingJobs, .awaitedJobIds = keys};
    auto                        dumpOutput = dumpInput.getFuture();
    c.get<job_database::JobDatabase>()->sendInput(std::move(dumpInput));
    auto dumpResult = dumpOutput.get();
    if (std::holds_alternative<services::ErrorResult>(dumpResult))
    {
        i.setResult(result::BooleanResult{false});
    }
    else
    {
        i.setResult(result::BooleanResult{std::get<result::BooleanResult>(dumpResult).result});
    }
    return PausedState::index();
}

} // namespace job_queue

} // end namespace orchestrator
