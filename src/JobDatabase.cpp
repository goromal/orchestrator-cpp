#include "orchestrator/JobDatabase.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>

namespace orchestrator
{

namespace job_database
{

// ══════════════════════════════════════════════════════════════════════════════
// SQLiteDatabase Implementation - Pure Database Logic
// ══════════════════════════════════════════════════════════════════════════════

SQLiteDatabase::~SQLiteDatabase()
{
    close();
}

bool SQLiteDatabase::open(const std::string& db_path)
{
    if (isOpen())
    {
        SPDLOG_WARN("Database already open at {}", db_path_);
        return true;
    }

    db_path_ = db_path;

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK)
    {
        SPDLOG_ERROR("Cannot open database {}: {}", db_path, sqlite3_errmsg(db_));
        db_ = nullptr;
        return false;
    }

    SPDLOG_INFO("Opened database at {}", db_path);

    // Create schema if needed
    if (!createSchema())
    {
        SPDLOG_ERROR("Failed to create database schema");
        close();
        return false;
    }

    // Generate boot ID
    boot_id_ = generateBootId();
    SPDLOG_INFO("Generated boot ID: {}", boot_id_);

    return true;
}

void SQLiteDatabase::close()
{
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
        SPDLOG_DEBUG("Closed database at {}", db_path_);
    }
}

bool SQLiteDatabase::createSchema()
{
    // Enable foreign keys
    if (!executeSql("PRAGMA foreign_keys = ON;"))
    {
        return false;
    }

    // Create job_definitions table
    const char* create_job_definitions = R"(
        CREATE TABLE IF NOT EXISTS job_definitions (
            job_type TEXT PRIMARY KEY,
            job_definition TEXT NOT NULL,
            timeout_seconds INTEGER NOT NULL DEFAULT 3600,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );
    )";

    if (!executeSql(create_job_definitions))
    {
        return false;
    }

    // Create queue_snapshots table
    const char* create_queue_snapshots = R"(
        CREATE TABLE IF NOT EXISTS queue_snapshots (
            snapshot_id INTEGER PRIMARY KEY AUTOINCREMENT,
            boot_id TEXT NOT NULL,
            snapshot_time_seconds INTEGER NOT NULL,
            pending_jobs_json TEXT NOT NULL,
            active_job_ids_json TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );
    )";

    if (!executeSql(create_queue_snapshots))
    {
        return false;
    }

    // Create index on boot_id and snapshot_time for efficient queries
    if (!executeSql("CREATE INDEX IF NOT EXISTS idx_snapshots_boot_time ON queue_snapshots(boot_id, snapshot_time_seconds);"))
    {
        return false;
    }

    // Create job_history table
    const char* create_job_history = R"(
        CREATE TABLE IF NOT EXISTS job_history (
            job_id INTEGER PRIMARY KEY,
            status INTEGER NOT NULL,
            outputs_json TEXT,
            completion_timestamp_seconds INTEGER NOT NULL,
            created_at INTEGER NOT NULL
        );
    )";

    if (!executeSql(create_job_history))
    {
        return false;
    }

    // Create index on completion timestamp for time-range queries
    if (!executeSql("CREATE INDEX IF NOT EXISTS idx_history_completion ON job_history(completion_timestamp_seconds);"))
    {
        return false;
    }

    SPDLOG_DEBUG("Database schema created successfully");
    return true;
}

std::string SQLiteDatabase::generateBootId()
{
    // Generate UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    // Generate 8-4-4-4-12 hex digits
    oss << std::setw(8) << (dis(gen) & 0xFFFFFFFF) << "-";
    oss << std::setw(4) << (dis(gen) & 0xFFFF) << "-";
    oss << std::setw(4) << ((dis(gen) & 0x0FFF) | 0x4000) << "-";  // Version 4
    oss << std::setw(4) << ((dis(gen) & 0x3FFF) | 0x8000) << "-";  // Variant 1
    oss << std::setw(12) << (dis(gen) & 0xFFFFFFFFFFFF);

    return oss.str();
}

bool SQLiteDatabase::executeSql(const std::string& sql, std::string* error_msg)
{
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);

    if (rc != SQLITE_OK)
    {
        std::string error_str = err ? err : "unknown error";
        SPDLOG_ERROR("SQL error: {}", error_str);

        if (error_msg)
        {
            *error_msg = error_str;
        }

        if (err)
        {
            sqlite3_free(err);
        }

        return false;
    }

    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// Job Definition Operations
// ──────────────────────────────────────────────────────────────────────────────

bool SQLiteDatabase::insertJobDefinition(const JobDefinition& def)
{
    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return false;
    }

    const char* sql = R"(
        INSERT OR REPLACE INTO job_definitions
        (job_type, job_definition, timeout_seconds, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db_));
        return false;
    }

    auto now = std::chrono::system_clock::now().time_since_epoch().count();

    sqlite3_bind_text(stmt, 1, def.job_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, def.job_definition.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, def.timeout_seconds);
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_int64(stmt, 5, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        SPDLOG_ERROR("Failed to insert job definition: {}", sqlite3_errmsg(db_));
        return false;
    }

    SPDLOG_INFO("Stored job definition for type '{}'", def.job_type);
    return true;
}

std::optional<JobDefinition> SQLiteDatabase::getJobDefinition(const std::string& job_type) const
{
    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return std::nullopt;
    }

    const char* sql = "SELECT job_type, job_definition, timeout_seconds FROM job_definitions WHERE job_type = ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db_));
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, job_type.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW)
    {
        JobDefinition def;
        def.job_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        def.job_definition = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        def.timeout_seconds = sqlite3_column_int64(stmt, 2);

        sqlite3_finalize(stmt);
        return def;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::vector<JobDefinition> SQLiteDatabase::getAllJobDefinitions() const
{
    std::vector<JobDefinition> results;

    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return results;
    }

    const char* sql = "SELECT job_type, job_definition, timeout_seconds FROM job_definitions";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db_));
        return results;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        JobDefinition def;
        def.job_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        def.job_definition = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        def.timeout_seconds = sqlite3_column_int64(stmt, 2);
        results.push_back(def);
    }

    sqlite3_finalize(stmt);
    return results;
}

// ──────────────────────────────────────────────────────────────────────────────
// Queue Snapshot Operations
// ──────────────────────────────────────────────────────────────────────────────

bool SQLiteDatabase::saveSnapshot(const orchestrator::job_queue::QueueSnapshot& snapshot)
{
    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return false;
    }

    // Serialize pending jobs to JSON (simplified - full serialization TODO for Stage 6/7)
    std::ostringstream pending_json;
    pending_json << "[";
    for (size_t i = 0; i < snapshot.pending_jobs.size(); ++i)
    {
        if (i > 0) pending_json << ",";
        const auto& job = snapshot.pending_jobs[i];
        pending_json << "{\"id\":" << job.id
                    << ",\"status\":" << static_cast<int>(job.status)
                    << ",\"priority\":" << job.priority
                    << ",\"spawn_time\":" << job.spawnTimeSeconds
                    << "}";
    }
    pending_json << "]";

    // Serialize active job IDs to JSON
    std::ostringstream active_json;
    active_json << "[";
    for (size_t i = 0; i < snapshot.active_job_ids.size(); ++i)
    {
        if (i > 0) active_json << ",";
        active_json << snapshot.active_job_ids[i];
    }
    active_json << "]";

    const char* sql = R"(
        INSERT INTO queue_snapshots
        (boot_id, snapshot_time_seconds, pending_jobs_json, active_job_ids_json, created_at)
        VALUES (?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db_));
        return false;
    }

    auto now = std::chrono::system_clock::now().time_since_epoch().count();

    sqlite3_bind_text(stmt, 1, snapshot.boot_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, snapshot.snapshot_time_seconds);
    sqlite3_bind_text(stmt, 3, pending_json.str().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, active_json.str().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        SPDLOG_ERROR("Failed to save snapshot: {}", sqlite3_errmsg(db_));
        return false;
    }

    SPDLOG_DEBUG("Saved queue snapshot (boot: {}, {} pending jobs, {} active jobs)",
                snapshot.boot_id, snapshot.pending_jobs.size(), snapshot.active_job_ids.size());
    return true;
}

orchestrator::job_queue::QueueSnapshot SQLiteDatabase::loadMostRecentSnapshot()
{
    orchestrator::job_queue::QueueSnapshot snapshot;
    snapshot.boot_id = boot_id_;
    snapshot.snapshot_time_seconds = 0;

    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return snapshot;
    }

    // Query most recent snapshot from ANY boot (for crash recovery)
    const char* sql = R"(
        SELECT boot_id, snapshot_time_seconds, pending_jobs_json, active_job_ids_json
        FROM queue_snapshots
        ORDER BY snapshot_time_seconds DESC
        LIMIT 1
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db_));
        return snapshot;
    }

    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW)
    {
        std::string prev_boot_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        snapshot.snapshot_time_seconds = sqlite3_column_int64(stmt, 1);

        // TODO: Deserialize JSON to reconstruct pending_jobs and active_job_ids
        // For now, return empty snapshot (Stage 6/7 will need full deserialization)

        SPDLOG_INFO("Loaded snapshot from boot {} (time: {})", prev_boot_id, snapshot.snapshot_time_seconds);

        if (prev_boot_id != boot_id_)
        {
            SPDLOG_WARN("Boot ID changed ({} -> {}), previous jobs will be re-queued",
                       prev_boot_id, boot_id_);
        }
    }
    else
    {
        SPDLOG_INFO("No previous snapshot found, starting fresh");
    }

    sqlite3_finalize(stmt);
    return snapshot;
}

// ──────────────────────────────────────────────────────────────────────────────
// Job History Operations
// ──────────────────────────────────────────────────────────────────────────────

bool SQLiteDatabase::insertJobHistory(const orchestrator::job_queue::JobResult& result)
{
    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return false;
    }

    // Serialize outputs to JSON (simplified for now)
    std::ostringstream outputs_json;
    outputs_json << "[]";  // TODO: Properly serialize variant<vector<string>, vector<Job>>

    const char* sql = R"(
        INSERT OR REPLACE INTO job_history
        (job_id, status, outputs_json, completion_timestamp_seconds, created_at)
        VALUES (?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db_));
        return false;
    }

    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    auto completion_time = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;  // Convert to seconds

    sqlite3_bind_int64(stmt, 1, result.job_id);
    sqlite3_bind_int(stmt, 2, static_cast<int>(result.status));
    sqlite3_bind_text(stmt, 3, outputs_json.str().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, completion_time);
    sqlite3_bind_int64(stmt, 5, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        SPDLOG_ERROR("Failed to insert job history: {}", sqlite3_errmsg(db_));
        return false;
    }

    SPDLOG_DEBUG("Stored job history for job {} (status: {})", result.job_id, static_cast<int>(result.status));
    return true;
}

std::optional<orchestrator::job_queue::JobResult> SQLiteDatabase::getJobHistory(int64_t job_id) const
{
    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return std::nullopt;
    }

    const char* sql = "SELECT job_id, status, outputs_json FROM job_history WHERE job_id = ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db_));
        return std::nullopt;
    }

    sqlite3_bind_int64(stmt, 1, job_id);

    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW)
    {
        orchestrator::job_queue::JobResult result;
        result.job_id = sqlite3_column_int64(stmt, 0);
        result.status = static_cast<aapis::orchestrator::v1::JobStatus>(sqlite3_column_int(stmt, 1));
        // TODO: Deserialize outputs_json

        sqlite3_finalize(stmt);
        return result;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::vector<orchestrator::job_queue::JobResult> SQLiteDatabase::getJobHistoryByTimeRange(
    int64_t start_time_seconds,
    int64_t end_time_seconds
) const
{
    std::vector<orchestrator::job_queue::JobResult> results;

    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return results;
    }

    const char* sql = R"(
        SELECT job_id, status, outputs_json
        FROM job_history
        WHERE completion_timestamp_seconds >= ? AND completion_timestamp_seconds <= ?
        ORDER BY completion_timestamp_seconds DESC
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db_));
        return results;
    }

    sqlite3_bind_int64(stmt, 1, start_time_seconds);
    sqlite3_bind_int64(stmt, 2, end_time_seconds);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        orchestrator::job_queue::JobResult result;
        result.job_id = sqlite3_column_int64(stmt, 0);
        result.status = static_cast<aapis::orchestrator::v1::JobStatus>(sqlite3_column_int(stmt, 1));
        // TODO: Deserialize outputs_json
        results.push_back(result);
    }

    sqlite3_finalize(stmt);
    return results;
}

// ──────────────────────────────────────────────────────────────────────────────
// Maintenance Operations
// ──────────────────────────────────────────────────────────────────────────────

void SQLiteDatabase::vacuum()
{
    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return;
    }

    SPDLOG_INFO("Running database VACUUM");
    executeSql("VACUUM;");
}

void SQLiteDatabase::cleanupOldSnapshots(size_t keep_count)
{
    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return;
    }

    std::ostringstream sql;
    sql << "DELETE FROM queue_snapshots WHERE snapshot_id NOT IN "
        << "(SELECT snapshot_id FROM queue_snapshots ORDER BY snapshot_time_seconds DESC LIMIT "
        << keep_count << ");";

    if (executeSql(sql.str()))
    {
        SPDLOG_DEBUG("Cleaned up old snapshots (kept last {})", keep_count);
    }
}

void SQLiteDatabase::cleanupOldHistory(int days_to_keep)
{
    if (!isOpen())
    {
        SPDLOG_ERROR("Database not open");
        return;
    }

    auto cutoff_time = std::chrono::system_clock::now() - std::chrono::hours(24 * days_to_keep);
    auto cutoff_seconds = std::chrono::duration_cast<std::chrono::seconds>(cutoff_time.time_since_epoch()).count();

    std::ostringstream sql;
    sql << "DELETE FROM job_history WHERE completion_timestamp_seconds < " << cutoff_seconds << ";";

    if (executeSql(sql.str()))
    {
        SPDLOG_DEBUG("Cleaned up job history older than {} days", days_to_keep);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Store Implementation
// ══════════════════════════════════════════════════════════════════════════════

bool Store::initialize()
{
    if (initialized)
    {
        return true;
    }

    if (!db.open(db_path))
    {
        SPDLOG_ERROR("Failed to open database at {}", db_path);
        return false;
    }

    initialized = true;
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// FSM State Implementations
// ══════════════════════════════════════════════════════════════════════════════

size_t InitState::step(Store& s, Ports& p, [[maybe_unused]] const Container& c,
                       [[maybe_unused]] const ::services::LogicalTag& tag,
                       const ::services::StepTrigger& trigger)
{
    // Initialize database on first heartbeat
    if (trigger.type == ::services::StepTrigger::Type::HEARTBEAT)
    {
        if (!s.initialize())
        {
            SPDLOG_ERROR("Failed to initialize database, staying in InitState");
            return InitState::index();
        }

        SPDLOG_INFO("JobDatabase initialized successfully");

        // Load most recent snapshot and send to JobQueue
        auto snapshot = s.db.loadMostRecentSnapshot();
        p.load_snapshot_out.set(snapshot);

        return RunningState::index();
    }

    return InitState::index();
}

size_t RunningState::step(Store& s, Ports& p, [[maybe_unused]] const Container& c,
                          [[maybe_unused]] const ::services::LogicalTag& tag,
                          const ::services::StepTrigger& trigger)
{
    // Handle event-driven logical actions
    if (trigger.type == ::services::StepTrigger::Type::LOGICAL_ACTION)
    {
        // Handle job definition storage
        if (trigger.action_name == "on_port_define_job")
        {
            if (p.define_job_in.is_present())
            {
                auto def = p.define_job_in.get();
                bool success = s.db.insertJobDefinition(def);
                p.definition_ack_out.set(success);
            }
        }

        // Handle snapshot save
        else if (trigger.action_name == "on_port_save_snapshot")
        {
            if (p.save_snapshot_in.is_present())
            {
                auto snapshot = p.save_snapshot_in.get();

                // Set boot ID from our database
                auto mutable_snapshot = snapshot;
                mutable_snapshot.boot_id = s.db.getBootId();

                s.db.saveSnapshot(mutable_snapshot);
            }
        }

        // Handle job history storage
        else if (trigger.action_name == "on_port_job_history")
        {
            if (p.job_history_in.is_present())
            {
                auto result = p.job_history_in.get();
                s.db.insertJobHistory(result);
            }
        }
    }

    return RunningState::index();
}

// ══════════════════════════════════════════════════════════════════════════════
// JobDatabase Reactor Implementation
// ══════════════════════════════════════════════════════════════════════════════

void JobDatabase::doPeriodicMaintenance(const ::services::LogicalTag& tag)
{
    (void)tag;

    // Periodic maintenance operations
    auto time_ns = tag.time;
    auto time_s = time_ns.count() / 1'000'000'000;

    // Vacuum every 1 hour
    if (time_s % 3600 == 0 && time_s > 0)
    {
        getStore().db.vacuum();
    }

    // Cleanup old snapshots every 1 hour
    if (time_s % 3600 == 0 && time_s > 0)
    {
        getStore().db.cleanupOldSnapshots(10);
    }

    // Cleanup old history every 24 hours
    if (time_s % 86400 == 0 && time_s > 0)
    {
        getStore().db.cleanupOldHistory(30);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// JobDatabase Constructor
// ══════════════════════════════════════════════════════════════════════════════

JobDatabase::JobDatabase(const Container& container, const std::string& db_path)
    : Base(container)
{
    // Initialize database with provided path
    if (!getStore().db.open(db_path))
    {
        SPDLOG_ERROR("Failed to open database at {}", db_path);
    }
}

} // namespace job_database

} // namespace orchestrator
