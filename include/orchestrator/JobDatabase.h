#pragma once

#include <atomic>
#include <variant>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <mscpp/InputSet.h>
#include <mscpp/StateSet.h>
#include <mscpp/MicroService.h>
#include <mscpp/MicroServiceContainer.h>

#include "orchestrator/Result.h"
#include "orchestrator/Job.h"

namespace orchestrator
{

namespace job_database
{

struct HeartbeatInput : public services::Input<HeartbeatInput, result::EmptyResult, 0, 1000>
{
};

struct DumpQueueData : public services::Input<DumpQueueData, result::BooleanResult, 1, 100>
{
    std::vector<Job>     pendingJobs;
    std::vector<int64_t> awaitedJobIds;
};

struct LoadQueueData : public services::Input<LoadQueueData, result::JobQueueDataResult, 1, 100>
{
};

using Inputs = services::InputSet<HeartbeatInput, DumpQueueData, LoadQueueData>;

using Container = services::MicroServiceContainer<>;

struct Store
{
};

struct ForeverState : public services::State<ForeverState, 0>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, DumpQueueData& i);
    size_t step(Store& s, const Container& c, LoadQueueData& i);
};

using States = services::StateSet<ForeverState>;

using JobDatabaseBase = services::MicroService<Store, Container, States, Inputs>;

class JobDatabase : public JobDatabaseBase
{
public:
    JobDatabase(const Container& container) : JobDatabaseBase(container) {}
    const std::string name() const override;
};

} // namespace job_database

} // namespace orchestrator