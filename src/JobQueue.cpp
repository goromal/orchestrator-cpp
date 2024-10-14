#include "orchestrator/JobQueue.h"
#include <chrono>
#include <algorithm>
#include <ranges>

namespace orchestrator
{

namespace job_queue
{

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
}

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

void Store::pauseJobs()
{
    for (auto& job : pendingJobs)
    {
        job.prePauseStatus = job.status;
        job.status         = aapis::orchestrator::v1::JobStatus::JOB_STATUS_PAUSED;
    }
}

void Store::unpauseJobs()
{
    for (auto& job : pendingJobs)
    {
        job.status = job.prePauseStatus;
    }
}

std::vector<Job> Store::processPendingJobResults(bool paused)
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
    // ^^^^ TODO do we need a second init state for when we're waiting for acknowledgements that the previous pending
    // jobs were successfully put up for execution?
    //      TODO it's the QUEUE's responsibility to dump pendingJobResults JOBIDs on shutdown
    //           it's the EXECUTOR's responsibility to dump all the info about running jobs on shutdown
    //           it's the QUEUE's responsibility to reload all pendingJobResult JOBIDs on startup and REQUEST new
    //           futures from JobExecutor::initState while in the initState
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

size_t RunningState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    // There's a lot going on in this step, so time things to ensure we can fall within our time budget
    static constexpr std::chrono::milliseconds kCheckFuturesBudget         = std::chrono::milliseconds(900);
    static constexpr int                       kExecuteInputWaitMultiplier = 4;

    // Part 1: Check futures for results and propagate the results to all queued jobs
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (auto spawnedJob : s.processPendingJobResults(false))
    {
        s.addAndRegisterNewJob(spawnedJob, false);
    }

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
    start = std::chrono::steady_clock::now();
    for (auto it = s.pendingJobs.begin(); it != s.pendingJobs.end();)
    {
        // Don't consider any jobs that have outstanding blockers.
        if (it->numBlockers() > 0)
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
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start) + tryExecInputWaitTime >
            kCheckFuturesBudget)
        {
            return RunningState::index();
        }

        // The executor will tell us if there was room for our pending job.
        // Wait for "as long as it takes" to get this information.
        if (!c.get<job_executor::JobExecutor>()->sendInput(std::move(tryExecInput)))
        {
            return RunningState::index();
        }
        if (tryExecFuture.wait_for(tryExecInputWaitTime) != std::future_status::ready)
        {
            // Our timeout underestimated how slow JobExecutor is; see if we can try again
            continue;
        }
        // If there was no room, then exit. Else, store the future result, remove the pending job from the list, and
        // move on to trying to jump another job.
        auto tryExecResult = tryExecFuture.get();
        if (std::holds_alternative<services::ErrorResult>)
        {
            return RunningState::index();
        }
        else
        {
            s.pendingJobResults.emplace(
                std::make_pair(tryExecKey, std::move(std::get<result::FutureJobResult>(tryExecResult))));
            it = s.pendingJobs.erase(it); // this increments the iterator
        }
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
    for (auto spawnedJob : s.processPendingJobResults(true))
    {
        s.addAndRegisterNewJob(spawnedJob, true);
    }
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
