#include "orchestrator/JobExecutor.h"

namespace orchestrator
{

namespace job_executor
{

size_t InitState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    return 0;
}

size_t InitState::step(Store& s, const Container& c, ExecuteInput& i)
{
    return 0;
}

size_t InitState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    return 0;
}

size_t InitState::step(Store& s, const Container& c, DumpInput& i)
{
    return 0;
}

size_t RunningState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    return 0;
}

size_t RunningState::step(Store& s, const Container& c, ExecuteInput& i)
{
    return 0;
}

size_t RunningState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    return 0;
}

size_t RunningState::step(Store& s, const Container& c, DumpInput& i)
{
    return 0;
}

size_t PausedState::step(Store& s, const Container& c, HeartbeatInput& i)
{
    return 0;
}

size_t PausedState::step(Store& s, const Container& c, ExecuteInput& i)
{
    return 0;
}

size_t PausedState::step(Store& s, const Container& c, TogglePauseInput& i)
{
    return 0;
}

size_t PausedState::step(Store& s, const Container& c, DumpInput& i)
{
    return 0;
}

} // namespace job_executor

} // namespace orchestrator
