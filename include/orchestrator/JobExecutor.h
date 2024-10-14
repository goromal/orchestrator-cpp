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

namespace job_executor
{

struct HeartbeatInput : public services::Input<HeartbeatInput, result::EmptyResult, 0, 1000>
{
};

struct ExecuteInput : public services::Input<ExecuteInput, result::FutureJobResult, 0, 50>
{
    Job job;
};

struct TogglePauseInput : public services::Input<TogglePauseInput, result::BooleanResult, 1, 5>
{
};

struct DumpInput : public services::Input<DumpInput, result::BooleanResult, 1, 100>
{
};

using Inputs = services::InputSet<HeartbeatInput, ExecuteInput, TogglePauseInput, DumpInput>;

using Container = services::MicroServiceContainer<>; // ^^^^ TODO dependencies

struct Store // ^^^^ TODO
{
};

// Initial state in which any persistent memory is loaded
struct InitState : public services::State<InitState, 0>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, ExecuteInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

// Nominal running state
struct RunningState : public services::State<RunningState, 1>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, ExecuteInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

// Paused state in which no new active jobs get queued
struct PausedState : public services::State<PausedState, 2>
{
    size_t step(Store& s, const Container& c, HeartbeatInput& i);
    size_t step(Store& s, const Container& c, ExecuteInput& i);
    size_t step(Store& s, const Container& c, TogglePauseInput& i);
    size_t step(Store& s, const Container& c, DumpInput& i);
};

using States = services::StateSet<InitState, RunningState, PausedState>;

using JobExecutorBase = services::MicroService<Store, Container, States, Inputs>;

class JobExecutor : public JobExecutorBase
{
public:
    JobExecutor(const Container& container) : JobExecutorBase(container) {}
    const std::string name() const override;
};

} // namespace job_executor

} // namespace orchestrator
