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

const std::string JobQueue::name() const
{
    return "JobQueue";
}

size_t RunningState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    // ^^^^ TODO A job CANNOT be queued for execution until all of its blockers (and all of their children!) have been
    // confirmed as
    // complete(grabbing their outputs) via a query

    // ^^^^ TODO store futures in the store. when future returns a completion status, execute a store function to
    // eliminate blockers and add child blockers and child outputs
    // so future needs to return completion status as well as outputs
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

size_t RunningState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    s.pauseJobs();

    i.setResult(result::BooleanResult{true});
    return PausedState::index();
}

size_t PausedState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    // ^^^^ TODO
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

} // namespace job_queue

} // end namespace orchestrator
