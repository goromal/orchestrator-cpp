#include "orchestrator/JobQueue.h"
#include <chrono>
#include <algorithm>

namespace orchestrator
{

namespace job_queue
{

int64_t Store::initializeJobData(Job& job, bool paused)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();

    job.spawnTimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    int64_t spawnMicrosId =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count() * 1e3 + static_cast<int64_t>(s.subCounter);

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

void Store::processPendingJobResults()
{
    // ^^^^ TODO check futures in the store. when future returns a completion status, execute a store function to
    // eliminate blockers and add child blockers and child outputs
    // so future needs to return completion status as well as outputs
}

const std::string JobQueue::name() const
{
    return "JobQueue";
}

size_t InitState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    // ^^^^ TODO
}

size_t InitState::step(Store& s, const Container& c, PushInput& i)
{
    // ^^^^ TODO
}

size_t InitState::step(Store& s, const Container& c, QueryInput& i)
{
    // ^^^^ TODO
}

size_t InitState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    // ^^^^ TODO
}

size_t InitState::step(Store& s, const Container& c, DumpInput& i)
{
    // ^^^^ TODO
}

size_t RunningState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    // There's a lot going on in this step, so time things to ensure we can fall within our time budget
    static constexpr std::chrono::milliseconds kCheckFuturesBudget         = std::chrono::milliseconds(900);
    static constexpr int                       kExecuteInputWaitMultiplier = 4;

    // Part 1: Check futures for results and propagate the results to all queued jobs
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    s.processPendingJobResults();

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
        auto tryExecInput  = job_executor::ExecuteInput{.job = *it}; // ^^^^ TODO make sure that job copies
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
            s.pendingJobResults.push_back(std::move(std::get<result::FutureJobResult>(tryExecResult)));
            it = s.pendingJobs.erase(it); // this increments the iterator
        }
    }

    return RunningState::index();
}

// This is the sole way to add a job (manually specified or automatically derived) to the execution queue
size_t RunningState::step(Store& s, const Container& c, PushInput& i)
{
    auto id = s.initializeJobData(i.job, false);

    s.pendingJobs.push_back(std::move(i.job));
    s.sortJobs();

    i.setResult(result::JobIdResult{id});
    return RunningState::index();
}

size_t RunningState::step(Store& s, const Container& c, QueryInput& i)
{
    // ^^^^ TODO
}

size_t RunningState::step(Store& s, const Container& c, DumpInput& i)
{
    // ^^^^ TODO
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
    s.processPendingJobResults();
    return PausedState::index();
}

size_t PausedState::step(Store& s, const Container& c, PushInput& i)
{
    auto id = s.initializeJobData(i.job, true);

    s.pendingJobs.push_back(std::move(i.job));
    s.sortJobs();

    i.setResult(result::JobIdResult{id});
    return PausedState::index();
}

size_t PausedState::step(Store& s, const Container& c, QueryInput& i)
{
    // ^^^^ TODO
}

size_t PausedState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    s.unpauseJobs();

    i.setResult(result::BooleanResult{true});
    return RunningState::index();
}

size_t PausedState::step(Store& s, const Container& c, DumpInput& i)
{
    // ^^^^ TODO
}

} // namespace job_queue

} // end namespace orchestrator
