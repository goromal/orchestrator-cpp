#include "orchestrator/JobQueue.h"
#include <chrono>

namespace orchestrator
{

namespace job_queue
{

void Store::sortJobs()
{
    // ^^^^ TODO to be sorted with complex function
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
    int64_t spawnMicrosId =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count() *
            1e3 +
        static_cast<int64_t>(s.subCounter);
    s.subCounter++;
    i.job.id = spawnMicrosId;
    s.pendingJobs.push_back(std::move(i.job));
    s.sortJobs();
    i.setResult(result::JobIdResult{spawnMicrosId});
    return index();
}

size_t InitState::step(Store& s, const Container& c, QueryInput& i)
{
    // ^^^^ TODO
}

} // namespace job_queue

} // end namespace orchestrator
