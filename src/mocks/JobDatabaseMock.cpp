#include "orchestrator/JobDatabase.h"

namespace orchestrator
{

namespace job_database
{

const std::string JobDatabase::name() const
{
    return "JobDatabaseMock";
}

size_t ForeverState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    LOG(debug) << "db hb";
    return ForeverState::index();
}

size_t ForeverState::step(Store& s, const Container& c, DumpQueueData& i)
{
    LOG(debug) << "db dqd";
    return ForeverState::index();
}

size_t ForeverState::step(Store& s, const Container& c, LoadQueueData& i)
{
    LOG(debug) << "db lqd";
    // Queue up the load request and kick off asynchronous data loading
    s.loadRequest = std::move(i);
    // i.setResult() ^^^^ TODO this is next for unlocking
    return ForeverState::index();
}

} // namespace job_database

} // namespace orchestrator
