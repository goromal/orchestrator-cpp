#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <functional>

#include <mscpp/MicroServiceReactors.h>
#include <mscpp/Ports.h>
#include <mscpp/StateSet.h>
#include <mscpp/MicroServiceContainer.h>
#include <mscpp/Logging.h>
#include <mscpp/StepTrigger.h>

#include "orchestrator/Job.h"

// Forward declarations for dependencies
namespace orchestrator::job_executor { class JobExecutor; }
namespace orchestrator::job_database { class JobDatabase; }

namespace orchestrator
{

namespace job_queue
{

// ══════════════════════════════════════════════════════════════════════════════
// Reactor Name Declaration
// ══════════════════════════════════════════════════════════════════════════════

inline constexpr char NameJobQueue[] = "JobQueue";

// ══════════════════════════════════════════════════════════════════════════════
// Port Definitions (Event-Driven Communication)
// ══════════════════════════════════════════════════════════════════════════════

/**
 * Query request structure - simplified from old variant-based approach
 */
struct JobQuery {
    enum class Type {
        GET_ALL_QUEUED,
        GET_BY_PRIORITY,
        GET_BY_ID
    };

    Type type{Type::GET_ALL_QUEUED};
    int64_t priority{-1};  // For GET_BY_PRIORITY
    int64_t id{-1};        // For GET_BY_ID
};

/**
 * Query response structure
 */
struct JobQueryResponse {
    bool success{true};
    std::vector<Job> jobs;
    std::string error_message;
};

/**
 * Control request structure
 */
struct ControlRequest {
    enum class Command {
        PAUSE,
        RESUME,
        CANCEL
    };

    Command command;
    int64_t target_job_id{-1};  // -1 for PAUSE/RESUME (all jobs)
};

/**
 * Control response structure
 */
struct ControlResponse {
    bool success{true};
    std::string message;
};

/**
 * Job result structure from executor
 */
struct JobResult {
    int64_t job_id;
    aapis::orchestrator::v1::JobStatus status;
    std::variant<std::vector<std::string>, std::vector<Job>> outputs;
};

/**
 * Queue snapshot for persistence
 */
struct QueueSnapshot {
    std::vector<Job> pending_jobs;
    std::vector<int64_t> active_job_ids;  // Jobs awaiting results from executor
    std::string boot_id;
    int64_t snapshot_time_seconds;
};

/**
 * Port collection for JobQueue reactor
 *
 * Inputs (Event-Triggered):
 *   - new_job_in: New job submissions from JobServer
 *   - query_request_in: Query requests from JobServer
 *   - control_request_in: Control commands (pause/resume/cancel)
 *   - job_result_in: Job completion results from JobExecutor
 *   - load_snapshot_in: Restored snapshot from JobDatabase (init only)
 *
 * Outputs:
 *   - new_job_id_out: Assigned job ID response to JobServer
 *   - query_response_out: Query results to JobServer
 *   - control_response_out: Control command acknowledgment
 *   - execute_job_out: Jobs ready for execution to JobExecutor
 *   - save_snapshot_out: Periodic snapshot to JobDatabase
 */
struct Ports : ::services::AutoClearPorts<Ports> {
    // Inputs from JobServer (event-triggered)
    ::services::InputPort<Job> new_job_in;
    ::services::InputPort<JobQuery> query_request_in;
    ::services::InputPort<ControlRequest> control_request_in;

    // Outputs to JobServer (responses)
    ::services::OutputPort<int64_t> new_job_id_out;
    ::services::OutputPort<JobQueryResponse> query_response_out;
    ::services::OutputPort<ControlResponse> control_response_out;

    // Output to JobExecutor (event-triggered)
    ::services::OutputPort<Job> execute_job_out;

    // Input from JobExecutor (event-triggered)
    ::services::InputPort<JobResult> job_result_in;

    // Output to JobDatabase (periodic)
    ::services::OutputPort<QueueSnapshot> save_snapshot_out;

    // Input from JobDatabase (one-time at init)
    ::services::InputPort<QueueSnapshot> load_snapshot_in;

    // Register input ports for automatic clearing
    REGISTER_INPUT_PORTS(new_job_in, query_request_in, control_request_in,
                        job_result_in, load_snapshot_in)
};

// ══════════════════════════════════════════════════════════════════════════════
// Store (Reactor State)
// ══════════════════════════════════════════════════════════════════════════════

/**
 * JobQueue reactor state
 *
 * Maintains the core queue data structures and business logic for:
 * - Job registration and unique ID assignment
 * - Priority-based topological sorting (Kahn's algorithm)
 * - Pause/unpause state management
 * - Job blocking and dependency resolution
 */
struct Store {
    // Job ID counter (atomic for thread safety)
    std::atomic_uint8_t subCounter{0};

    // Primary job queue (sorted by priority, dependencies, ID)
    std::vector<Job> pendingJobs;

    // Map of active job IDs awaiting results from executor
    std::map<int64_t, bool> activeJobIds;

    // Snapshot data for restoration after reboot
    std::vector<Job> pendingInitExecs;

    // ──────────────────────────────────────────────────────────────────────────
    // Job Registration and ID Assignment
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Take a new job and register it with the queue store, giving it a unique ID
     *
     * @param job Job to be registered and given an ID
     * @param paused Whether or not the program is currently paused
     * @return A globally unique, monotonically increasing ID
     */
    int64_t addAndRegisterNewJob(Job job, bool paused);

    /**
     * Assign a unique ID and job statuses to a job
     *
     * @param job Job to be given an ID
     * @param paused Whether or not the program is currently paused
     * @return A globally unique, monotonically increasing ID
     */
    int64_t initializeJobData(Job& job, bool paused);

    // ──────────────────────────────────────────────────────────────────────────
    // Job Sorting and Dependency Resolution
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Sort all registered jobs in the store according to:
     * 1. Blocking status (topological order via Kahn's algorithm)
     * 2. Priority (lower number = higher priority)
     * 3. ID (monotonically increasing timestamp)
     */
    void sortJobs();

    // ──────────────────────────────────────────────────────────────────────────
    // Pause/Unpause Management
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Give all registered jobs a paused status, storing their previous statuses
     */
    void pauseJobs();

    /**
     * Restore all registered paused jobs to their pre-paused statuses
     */
    void unpauseJobs();

    // ──────────────────────────────────────────────────────────────────────────
    // Job Result Processing
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Process a completed job result:
     * - Remove from active jobs
     * - Unblock dependent jobs
     * - Handle outputs (strings or spawned child jobs)
     * - Mark error-dependent jobs as canceled
     *
     * @param result Job completion result from executor
     * @param paused Whether system is currently paused
     */
    void processJobResult(const JobResult& result, bool paused);

    // ──────────────────────────────────────────────────────────────────────────
    // Query Operations
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Query jobs based on filter criteria
     *
     * @param query Query specification
     * @return Vector of matching jobs
     */
    std::vector<Job> query(const JobQuery& query) const;

    // ──────────────────────────────────────────────────────────────────────────
    // Snapshot Operations
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Create a snapshot of current queue state for persistence
     *
     * @return Snapshot containing pending and active jobs
     */
    QueueSnapshot createSnapshot() const;

    /**
     * Restore queue state from snapshot
     *
     * @param snapshot Previously saved snapshot
     */
    void restoreFromSnapshot(const QueueSnapshot& snapshot);
};

// ══════════════════════════════════════════════════════════════════════════════
// Dependency Container
// ══════════════════════════════════════════════════════════════════════════════

// NOTE: Container is empty for now since JobExecutor and JobDatabase
// will be connected via ports (event-driven), not via container dependencies
using Container = ::services::MicroServiceContainer<>;

// ══════════════════════════════════════════════════════════════════════════════
// FSM State Declarations
// ══════════════════════════════════════════════════════════════════════════════

/**
 * InitState - Request persistent memory load from database
 *
 * Transitions:
 *   → InitWaitState (after requesting snapshot load)
 */
struct InitState : public ::services::State<InitState, 0> {
    /**
     * Entry action: Request snapshot from database
     */
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

/**
 * InitWaitState - Wait for database to load snapshot
 *
 * Transitions:
 *   → InitFinalWaitState (if in-progress jobs exist)
 *   → RunningState (if no in-progress jobs)
 */
struct InitWaitState : public ::services::State<InitWaitState, 1> {
    /**
     * Check for snapshot load completion
     */
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

/**
 * InitFinalWaitState - Re-trigger formerly in-progress jobs
 *
 * Transitions:
 *   → RunningState (after all in-progress jobs re-queued)
 */
struct InitFinalWaitState : public ::services::State<InitFinalWaitState, 2> {
    /**
     * Re-submit in-progress jobs to executor
     */
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

/**
 * RunningState - Normal operation
 *
 * Transitions:
 *   → PausedState (on pause command)
 */
struct RunningState : public ::services::State<RunningState, 3> {
    /**
     * Process queue operations normally
     */
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

/**
 * PausedState - No new jobs sent to executor
 *
 * Transitions:
 *   → RunningState (on resume command)
 */
struct PausedState : public ::services::State<PausedState, 4> {
    /**
     * Process queries/control but don't execute jobs
     */
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

// State set for FSM
using States = ::services::StateSet<
    InitState,
    InitWaitState,
    InitFinalWaitState,
    RunningState,
    PausedState
>;

// ══════════════════════════════════════════════════════════════════════════════
// JobQueue Reactor
// ══════════════════════════════════════════════════════════════════════════════

/**
 * JobQueue - Event-driven job queue manager with FSM
 *
 * Responsibilities:
 * - Assign unique IDs to jobs (monotonically increasing)
 * - Maintain priority-sorted queue with topological ordering
 * - Track job dependencies and unblock when ready
 * - Handle pause/resume/cancel commands
 * - Persist queue state to database
 *
 * Event-Driven Design:
 * - executeLogicalAction() handles all event-triggered operations
 * - doHeartbeat() only handles periodic snapshot saves
 * - Automatic logical action scheduling on port connections
 *
 * Logical Actions:
 * - "on_port_new_job": Job submission from JobServer
 * - "on_port_job_result": Job completion from JobExecutor
 * - "on_port_query": Query request from JobServer
 * - "on_port_control": Control command from JobServer
 * - "on_port_load_snapshot": Snapshot loaded from JobDatabase
 * - "try_execute_jobs": Internal trigger to drain ready jobs
 */
class JobQueue : public ::services::MicroServiceFSMReactor<
    NameJobQueue,
    Store,
    Ports,
    Container,
    States
>
{
public:
    using Base = ::services::MicroServiceFSMReactor<
        NameJobQueue,
        Store,
        Ports,
        Container,
        States
    >;

    // Inherit constructors
    using Base::Base;

    // ──────────────────────────────────────────────────────────────────────────
    // Constructor
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Constructor - uses default container
     */
    JobQueue(const Container& container = Container{})
        : Base(container)
    {
        // Port action mappings are handled by the framework based on port names
        // Port connections will automatically trigger corresponding logical actions
    }

    // ──────────────────────────────────────────────────────────────────────────
    // IReactor Interface
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Heartbeat frequency override
     *
     * @return 1 second (1,000,000,000 nanoseconds)
     */
    ::services::LogicalTime heartbeatDuration() const override {
        return ::services::LogicalTime{1'000'000'000};  // 1 second
    }

protected:
    /**
     * Periodic maintenance (non-business-logic)
     *
     * Called during heartbeat for:
     * - Periodic snapshot saves (every 60 seconds)
     */
    void doPeriodicMaintenance(const ::services::LogicalTag& tag);

    // ──────────────────────────────────────────────────────────────────────────
    // Helper Methods for FSM States
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Check if system is currently paused
     */
    bool isPaused() const;
};

} // namespace job_queue

} // namespace orchestrator

// Port clearing handled automatically via AutoClearPorts<Ports> + REGISTER_INPUT_PORTS
