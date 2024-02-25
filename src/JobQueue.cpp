#include "orchestrator/JobQueue.h"
#include <chrono>

namespace orchestrator
{

std::string JobQueue::name() const
{
    return "JobQueue";
}

int64_t JobQueue::push(std::unique_ptr<Job> job)
{
    std::scoped_lock l(mPendingJobsMtx);

    // Job ID = (milliseconds since epoch) * 1000 + subcount
    int64_t spawnMicrosId =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count() *
            1e3 +
        static_cast<int64_t>(mSubCounter);
    mSubCounter++;

    mPendingJobs.push_back(std::move(job));
    sortJobs();

    return spawnMicrosId;
}

void JobQueue::sortJobs()
{
    // TODO to be sorted with complex function
}

} // end namespace orchestrator
