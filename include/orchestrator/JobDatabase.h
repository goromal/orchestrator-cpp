#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <sqlite3.h>

#include <mscpp/MicroServiceReactors.h>
#include <mscpp/Ports.h>
#include <mscpp/StateSet.h>
#include <mscpp/StepTrigger.h>
#include <mscpp/MicroServiceContainer.h>
#include <mscpp/Logging.h>

#include "orchestrator/Job.h"
#include "orchestrator/JobQueue.h"  // For QueueSnapshot and JobResult types

namespace orchestrator
{

namespace job_database
{

// ══════════════════════════════════════════════════════════════════════════════
// Reactor Name Declaration
// ══════════════════════════════════════════════════════════════════════════════

inline constexpr char NameJobDatabase[] = "JobDatabase";

// ══════════════════════════════════════════════════════════════════════════════
// Port Definitions (Event-Driven Communication)
// ══════════════════════════════════════════════════════════════════════════════

/**
 * Job definition structure for storing job type definitions
 */
struct JobDefinition {
    std::string job_type;
    std::string job_definition;  // Bash script with $INPUT_IDS[], $INPUT_ARGS[]
    int64_t timeout_seconds{3600};  // Default 1 hour timeout
};

/**
 * Port collection for JobDatabase reactor
 *
 * Inputs (Event-Triggered):
 *   - define_job_in: Job type definitions from JobServer
 *   - save_snapshot_in: Queue snapshots from JobQueue
 *   - job_history_in: Completed job results from JobExecutor
 *
 * Outputs:
 *   - definition_ack_out: Acknowledgment to JobServer
 *   - load_snapshot_out: Restored snapshot to JobQueue (init only)
 */
struct Ports : ::services::AutoClearPorts<Ports> {
    // Input from JobServer (event-triggered)
    ::services::InputPort<JobDefinition> define_job_in;

    // Output to JobServer (response)
    ::services::OutputPort<bool> definition_ack_out;

    // Input from JobQueue (event-triggered)
    ::services::InputPort<orchestrator::job_queue::QueueSnapshot> save_snapshot_in;

    // Output to JobQueue (one-time at init)
    ::services::OutputPort<orchestrator::job_queue::QueueSnapshot> load_snapshot_out;

    // Input from JobExecutor (event-triggered)
    ::services::InputPort<orchestrator::job_queue::JobResult> job_history_in;

    // Register input ports for automatic clearing
    REGISTER_INPUT_PORTS(define_job_in, save_snapshot_in, job_history_in)
};

// ══════════════════════════════════════════════════════════════════════════════
// Store (Reactor State)
// ══════════════════════════════════════════════════════════════════════════════

/**
 * SQLite database wrapper for persistent storage
 */
class SQLiteDatabase {
public:
    SQLiteDatabase() = default;
    ~SQLiteDatabase();

    // Disable copy/move
    SQLiteDatabase(const SQLiteDatabase&) = delete;
    SQLiteDatabase& operator=(const SQLiteDatabase&) = delete;
    SQLiteDatabase(SQLiteDatabase&&) = delete;
    SQLiteDatabase& operator=(SQLiteDatabase&&) = delete;

    // ──────────────────────────────────────────────────────────────────────────
    // Database Lifecycle
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Open database connection and create schema if needed
     *
     * @param db_path Path to SQLite database file
     * @return true if successful, false otherwise
     */
    bool open(const std::string& db_path);

    /**
     * Close database connection
     */
    void close();

    /**
     * Check if database is currently open
     */
    bool isOpen() const { return db_ != nullptr; }

    // ──────────────────────────────────────────────────────────────────────────
    // Job Definition Operations
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Insert or update a job type definition
     *
     * @param def Job definition to store
     * @return true if successful, false otherwise
     */
    bool insertJobDefinition(const JobDefinition& def);

    /**
     * Retrieve a job definition by type
     *
     * @param job_type Job type identifier
     * @return Job definition if found, nullopt otherwise
     */
    std::optional<JobDefinition> getJobDefinition(const std::string& job_type) const;

    /**
     * Get all job definitions
     *
     * @return Vector of all stored job definitions
     */
    std::vector<JobDefinition> getAllJobDefinitions() const;

    // ──────────────────────────────────────────────────────────────────────────
    // Queue Snapshot Operations
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Save a queue snapshot with the current boot ID
     *
     * @param snapshot Queue snapshot to persist
     * @return true if successful, false otherwise
     */
    bool saveSnapshot(const orchestrator::job_queue::QueueSnapshot& snapshot);

    /**
     * Load the most recent snapshot for the current boot ID
     *
     * If this is a new boot (boot ID changed), returns the previous boot's
     * snapshot for recovery. If boot ID matches, returns the latest snapshot.
     *
     * @return Most recent snapshot, or empty snapshot if none found
     */
    orchestrator::job_queue::QueueSnapshot loadMostRecentSnapshot();

    /**
     * Get the current boot ID
     *
     * Boot ID is generated once per database instance and stored in memory.
     * It's used to detect service restarts.
     *
     * @return Current boot ID string (UUID format)
     */
    const std::string& getBootId() const { return boot_id_; }

    // ──────────────────────────────────────────────────────────────────────────
    // Job History Operations
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Insert a completed job result into history
     *
     * @param result Job result to store
     * @return true if successful, false otherwise
     */
    bool insertJobHistory(const orchestrator::job_queue::JobResult& result);

    /**
     * Query job history by job ID
     *
     * @param job_id Job ID to search for
     * @return Job result if found, nullopt otherwise
     */
    std::optional<orchestrator::job_queue::JobResult> getJobHistory(int64_t job_id) const;

    /**
     * Query job history within a time range
     *
     * @param start_time_seconds Start of time range (Unix timestamp)
     * @param end_time_seconds End of time range (Unix timestamp)
     * @return Vector of job results in time range
     */
    std::vector<orchestrator::job_queue::JobResult> getJobHistoryByTimeRange(
        int64_t start_time_seconds,
        int64_t end_time_seconds
    ) const;

    // ──────────────────────────────────────────────────────────────────────────
    // Maintenance Operations
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Vacuum database to reclaim space and optimize performance
     */
    void vacuum();

    /**
     * Clean up old snapshots (keep only last N)
     *
     * @param keep_count Number of recent snapshots to keep
     */
    void cleanupOldSnapshots(size_t keep_count = 10);

    /**
     * Clean up old job history (older than N days)
     *
     * @param days_to_keep Number of days of history to retain
     */
    void cleanupOldHistory(int days_to_keep = 30);

private:
    sqlite3* db_{nullptr};
    std::string boot_id_;
    std::string db_path_;

    // ──────────────────────────────────────────────────────────────────────────
    // Schema Creation
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Create database schema (tables, indices)
     *
     * Tables:
     *   - job_definitions: Stores job type definitions
     *   - queue_snapshots: Stores periodic queue state snapshots
     *   - job_history: Stores completed job results
     *   - boot_metadata: Stores boot ID and timestamps
     *
     * @return true if successful, false otherwise
     */
    bool createSchema();

    /**
     * Generate a unique boot ID (UUID v4 format)
     */
    std::string generateBootId();

    /**
     * Execute a SQL statement
     *
     * @param sql SQL statement to execute
     * @param error_msg Optional error message output
     * @return true if successful, false otherwise
     */
    bool executeSql(const std::string& sql, std::string* error_msg = nullptr);
};

/**
 * JobDatabase reactor state
 *
 * Maintains SQLite database connection and handles all persistence operations:
 * - Job type definitions from JobServer
 * - Queue snapshots from JobQueue
 * - Job completion history from JobExecutor
 */
struct Store {
    // SQLite database instance
    SQLiteDatabase db;

    // Database file path
    std::string db_path{"/tmp/orchestrator.db"};

    // Initialization state
    bool initialized{false};

    // ──────────────────────────────────────────────────────────────────────────
    // Initialization
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Initialize database connection and schema
     *
     * @return true if successful, false otherwise
     */
    bool initialize();

    /**
     * Configure database path (must be called before initialize)
     *
     * @param path Path to SQLite database file
     */
    void setDatabasePath(const std::string& path) {
        db_path = path;
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// Dependency Container
// ══════════════════════════════════════════════════════════════════════════════

// NOTE: Container is empty since JobDatabase communicates via ports (event-driven)
using Container = ::services::MicroServiceContainer<>;

// ══════════════════════════════════════════════════════════════════════════════
// FSM State Declarations
// ══════════════════════════════════════════════════════════════════════════════

/**
 * InitState - Initialize database and load most recent snapshot
 *
 * Transitions:
 *   → RunningState (after successful initialization)
 */
struct InitState : public ::services::State<InitState, 0> {
    /**
     * Entry action: Open database, create schema, load snapshot
     */
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

/**
 * RunningState - Normal operation, handle all database operations
 *
 * Transitions:
 *   → None (terminal state)
 */
struct RunningState : public ::services::State<RunningState, 1> {
    /**
     * Event-driven operation via logical actions
     */
    size_t step(Store& s, Ports& p, const Container& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

// State set for FSM
using States = ::services::StateSet<
    InitState,
    RunningState
>;

// ══════════════════════════════════════════════════════════════════════════════
// JobDatabase Reactor
// ══════════════════════════════════════════════════════════════════════════════

/**
 * JobDatabase - Event-driven SQLite persistence layer
 *
 * Responsibilities:
 * - Store job type definitions from JobServer
 * - Persist queue snapshots from JobQueue
 * - Record job completion history from JobExecutor
 * - Provide snapshot recovery on service restart
 *
 * Event-Driven Design:
 * - executeLogicalAction() handles all event-triggered database operations
 * - doHeartbeat() only handles periodic maintenance (vacuum, cleanup)
 * - Automatic logical action scheduling on port connections
 *
 * Logical Actions:
 * - "on_port_define_job": Job definition from JobServer
 * - "on_port_save_snapshot": Snapshot save from JobQueue
 * - "on_port_job_history": Job completion from JobExecutor
 * - "load_snapshot": Internal trigger at initialization
 */
class JobDatabase : public ::services::MicroServiceFSMReactor<
    NameJobDatabase,
    Store,
    Ports,
    Container,
    States
>
{
public:
    using Base = ::services::MicroServiceFSMReactor<
        NameJobDatabase,
        Store,
        Ports,
        Container,
        States
    >;

    // Inherit constructors
    using Base::Base;

    // Custom constructor for database initialization
    JobDatabase(const Container& container, const std::string& db_path);

    // ──────────────────────────────────────────────────────────────────────────
    // IReactor Interface
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * Periodic maintenance - database vacuum and cleanup
     *
     * Frequency: 10000ms (10 seconds)
     *
     * Operations:
     * - Database vacuum (every 1 hour)
     * - Cleanup old snapshots (every 1 hour)
     * - Cleanup old history (every 24 hours)
     */
    void doPeriodicMaintenance(const ::services::LogicalTag& tag) override;

    /**
     * Heartbeat frequency override
     *
     * @return 10 seconds (10,000,000,000 nanoseconds)
     */
    ::services::LogicalTime heartbeatDuration() const override {
        return ::services::LogicalTime{10'000'000'000};  // 10 seconds
    }

    /**
     * Configure database path (must be called before reactor starts)
     *
     * @param path Path to SQLite database file
     */
    void setDatabasePath(const std::string& path) {
        getStore().setDatabasePath(path);
    }
};

} // namespace job_database

} // namespace orchestrator

// Port clearing handled automatically via AutoClearPorts<Ports> + REGISTER_INPUT_PORTS
