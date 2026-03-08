#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <mscpp/MicroServiceReactors.h>
#include <mscpp/Ports.h>
#include <mscpp/StateSet.h>
#include <mscpp/StepTrigger.h>
#include <mscpp/MicroServiceContainer.h>
#include <mscpp/Logging.h>

#include "orchestrator/Job.h"
#include "orchestrator/JobQueue.h"  // For JobResult and ControlRequest types

namespace orchestrator
{

namespace job_executor
{

// ══════════════════════════════════════════════════════════════════════════════
// Reactor Name Declaration
// ══════════════════════════════════════════════════════════════════════════════

inline constexpr char NameJobExecutor[] = "JobExecutor";

// ══════════════════════════════════════════════════════════════════════════════
// Port Definitions (Event-Driven Communication)
// ══════════════════════════════════════════════════════════════════════════════

/**
 * Port collection for JobExecutor reactor
 *
 * Inputs (Event-Triggered):
 *   - job_in: Job execution requests from JobQueue
 *   - control_in: Control commands (pause/resume)
 *
 * Outputs:
 *   - job_result_out: Job completion results to JobQueue
 *   - job_history_out: Job results to JobDatabase for persistence
 */
struct Ports : ::services::AutoClearPorts<Ports> {
    // Input from JobQueue (event-triggered)
    ::services::InputPort<Job> job_in;

    // Outputs to JobQueue (event-triggered)
    ::services::OutputPort<orchestrator::job_queue::JobResult> job_result_out;

    // Outputs to JobDatabase (event-triggered)
    ::services::OutputPort<orchestrator::job_queue::JobResult> job_history_out;

    // Control input (for pause/resume)
    ::services::InputPort<orchestrator::job_queue::ControlRequest> control_in;

    // Register input ports for automatic clearing
    REGISTER_INPUT_PORTS(job_in, control_in)
};

// ══════════════════════════════════════════════════════════════════════════════
// Store (Reactor State)
// ══════════════════════════════════════════════════════════════════════════════

/**
 * Worker thread metadata for tracking subprocess execution
 */
struct WorkerThread {
    int64_t job_id{-1};
    pid_t pid{-1};
    std::chrono::steady_clock::time_point start_time;
    int64_t timeout_seconds{0};
    std::vector<std::string> outputs;  // Captured stdout/stderr lines
    bool completed{false};
    int exit_code{-1};
    std::string script;  // Original job script for reference
    bool timeout_scheduled{false};  // Track if timeout action has been scheduled
};

/**
 * JobExecutor reactor state
 *
 * Manages thread pool for subprocess execution with:
 * - N worker threads (configurable limit)
 * - Active job tracking with PIDs
 * - Non-blocking subprocess polling
 * - Timeout enforcement
 */
struct Store {
    // Thread pool configuration
    size_t max_threads{4};  // Default, can be configured

    // Active jobs map (job_id -> worker metadata)
    std::map<int64_t, WorkerThread> active_jobs;

    // Polling state
    bool polling_active{false};

    // ──────────────────────────────────────────────────────────────────────────
    // Thread Pool Management
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Check if there's an available thread slot
     */
    bool hasAvailableThread() const {
        return active_jobs.size() < max_threads;
    }

    /**
     * Check if any jobs are currently active
     */
    bool hasActiveJobs() const {
        return !active_jobs.empty();
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Job Submission
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Submit a job for execution by forking a subprocess
     *
     * @param job Job to execute (with script and inputs)
     * @return true if successfully submitted, false otherwise
     */
    bool submitJob(const Job& job);

    // ──────────────────────────────────────────────────────────────────────────
    // Completion Polling
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Poll for completed jobs (non-blocking)
     *
     * Uses waitpid(WNOHANG) to check subprocess status without blocking
     *
     * @return Vector of completed workers (may be empty)
     */
    std::vector<WorkerThread> pollCompletedJobs();

    // ──────────────────────────────────────────────────────────────────────────
    // Job Cancellation
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Cancel a running job by killing its subprocess
     *
     * @param job_id ID of job to cancel
     * @return true if job was found and killed, false otherwise
     */
    bool cancelJob(int64_t job_id);

    // ──────────────────────────────────────────────────────────────────────────
    // Helper Methods
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Perform bash variable substitution on job script
     *
     * Replaces:
     *   - $INPUT_IDS with array of blocker job IDs
     *   - $INPUT_ARGS with array of input strings
     *
     * @param script Original bash script template
     * @param job Job containing inputs and blockers
     * @return Substituted script ready for execution
     */
    static std::string substituteVariables(const std::string& script, const Job& job);

private:
    /**
     * Shell escape a string for safe inclusion in bash script
     */
    static std::string shellEscape(const std::string& str);
};

// ══════════════════════════════════════════════════════════════════════════════
// Dependency Container
// ══════════════════════════════════════════════════════════════════════════════

// NOTE: Container is empty since JobExecutor communicates via ports (event-driven),
// not via container dependencies. All I/O is through job_in, job_result_out, etc.
using Container = ::services::MicroServiceContainer<>;

// ══════════════════════════════════════════════════════════════════════════════
// FSM State Declarations
// ══════════════════════════════════════════════════════════════════════════════

/**
 * InitState - Initial state, transitions immediately to Running
 *
 * Transitions:
 *   → RunningState (immediate)
 */
struct InitState : public ::services::State<InitState, 0> {
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

/**
 * RunningState - Normal operation, accepts jobs
 *
 * Transitions:
 *   → PausedState (on pause command)
 */
struct RunningState : public ::services::State<RunningState, 1> {
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

/**
 * PausedState - Don't accept new jobs, let active jobs complete
 *
 * Transitions:
 *   → RunningState (on resume command)
 */
struct PausedState : public ::services::State<PausedState, 2> {
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

// State set for FSM
using States = ::services::StateSet<InitState, RunningState, PausedState>;

// ══════════════════════════════════════════════════════════════════════════════
// JobExecutor Reactor
// ══════════════════════════════════════════════════════════════════════════════

/**
 * JobExecutor - Event-driven job execution manager with thread pool
 *
 * Responsibilities:
 * - Manage N worker threads for concurrent job execution
 * - Execute jobs as Linux subprocesses with bash scripts
 * - Perform bash variable substitution ($INPUT_IDS[], $INPUT_ARGS[])
 * - Enforce job timeouts with physical actions
 * - Poll for subprocess completions (non-blocking)
 * - Handle pause/resume commands
 *
 * Event-Driven Design:
 * - executeLogicalAction() handles all event-triggered operations
 * - Physical actions for timeouts and polling
 * - doHeartbeat() initiates polling when jobs are active
 *
 * Logical Actions:
 * - "on_port_job_in": Job submission from JobQueue
 * - "on_port_control": Pause/resume commands
 * - "timeout_<job_id>": Job timeout (physical action)
 * - "poll_completions": Poll for completed jobs (physical action)
 */
class JobExecutor : public ::services::MicroServiceFSMReactor<
    NameJobExecutor,
    Store,
    Ports,
    Container,
    States
>
{
public:
    using Base = ::services::MicroServiceFSMReactor<
        NameJobExecutor,
        Store,
        Ports,
        Container,
        States
    >;

    // Default constructor
    JobExecutor() : Base() {}

    // Constructor with configurable thread count
    explicit JobExecutor(size_t max_threads)
        : Base()
    {
        getStore().max_threads = max_threads;
    }

    // Inherit other base constructors
    using Base::Base;

    // ──────────────────────────────────────────────────────────────────────────
    // IReactor Interface
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Periodic maintenance - initiate polling when jobs are active
     *
     * Frequency: 100ms (from heartbeatDuration())
     *
     * Operations:
     * - Check if polling should be initiated
     * - Schedule "poll_completions" physical action if jobs are active
     */
    void doPeriodicMaintenance(const ::services::LogicalTag& tag) override;

    /**
     * Heartbeat frequency override
     *
     * @return 100ms (100,000,000 nanoseconds)
     */
    ::services::LogicalTime heartbeatDuration() const override {
        return ::services::LogicalTime{100'000'000};  // 100ms
    }

    /**
     * Check if system is currently paused (convenience method)
     */
    bool isPaused() const {
        return Base::getCurrentState() == PausedState::index();
    }
};

} // namespace job_executor

} // namespace orchestrator

// Port clearing handled automatically via AutoClearPorts<Ports> + REGISTER_INPUT_PORTS
