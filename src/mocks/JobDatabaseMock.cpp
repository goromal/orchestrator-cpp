#include "orchestrator/JobDatabase.h"

namespace orchestrator
{

namespace job_database
{

size_t ForeverState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    return ForeverState::index();
}

size_t ForeverState::step(Store& s, const Container& c, DumpQueueData& i)
{
    return ForeverState::index();
}

size_t ForeverState::step(Store& s, const Container& c, LoadQueueData& i)
{
    // Queue up the load request and kick off asynchronous data loading
    s.loadRequest = std::move(i);
    // i.setResult() ^^^^ TODO this is next for unlocking
    return ForeverState::index();
}

} // namespace job_database

} // namespace orchestrator
