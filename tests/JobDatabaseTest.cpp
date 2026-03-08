#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include "orchestrator/JobDatabase.h"
#include "orchestrator/JobQueue.h"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace orchestrator;
using namespace orchestrator::job_database;

// ══════════════════════════════════════════════════════════════════════════════
// Testable JobDatabase - Exposes State Machine for Testing
// ══════════════════════════════════════════════════════════════════════════════

class TestableJobDatabase : public JobDatabase {
public:
    using JobDatabase::JobDatabase;

    void setStateToRunning() {
        this->mStateMachine.mActiveState = RunningState::index();
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// Helper Functions
// ══════════════════════════════════════════════════════════════════════════════

std::string getTempDbPath()
{
    static int counter = 0;
    return "/tmp/orchestrator_test_" + std::to_string(counter++) + ".db";
}

void cleanupDb(const std::string& path)
{
    if (std::filesystem::exists(path))
    {
        std::filesystem::remove(path);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// SQLiteDatabase Unit Tests - Pure Business Logic
// ══════════════════════════════════════════════════════════════════════════════

TEST_CASE("SQLiteDatabase: Open and close database", "[JobDatabase][SQLiteDatabase]")
{
    SQLiteDatabase db;
    std::string db_path = getTempDbPath();

    SECTION("Open creates new database file")
    {
        REQUIRE_FALSE(db.isOpen());
        REQUIRE(db.open(db_path));
        REQUIRE(db.isOpen());
        REQUIRE(std::filesystem::exists(db_path));

        db.close();
        REQUIRE_FALSE(db.isOpen());
        cleanupDb(db_path);
    }

    SECTION("Open existing database")
    {
        // Create database
        REQUIRE(db.open(db_path));
        db.close();

        // Re-open
        REQUIRE(db.open(db_path));
        REQUIRE(db.isOpen());

        db.close();
        cleanupDb(db_path);
    }

    SECTION("Close without opening is safe")
    {
        db.close();
        REQUIRE_FALSE(db.isOpen());
    }
}

TEST_CASE("SQLiteDatabase: Job definition operations", "[JobDatabase][SQLiteDatabase]")
{
    SQLiteDatabase db;
    std::string db_path = getTempDbPath();
    REQUIRE(db.open(db_path));

    SECTION("Insert and retrieve job definition")
    {
        JobDefinition def;
        def.job_type = "test_job";
        def.job_definition = "echo 'Hello World'";
        def.timeout_seconds = 300;

        REQUIRE(db.insertJobDefinition(def));

        auto retrieved = db.getJobDefinition("test_job");
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->job_type == "test_job");
        REQUIRE(retrieved->job_definition == "echo 'Hello World'");
        REQUIRE(retrieved->timeout_seconds == 300);
    }

    SECTION("Update existing job definition")
    {
        JobDefinition def1;
        def1.job_type = "update_test";
        def1.job_definition = "version 1";
        def1.timeout_seconds = 100;

        REQUIRE(db.insertJobDefinition(def1));

        // Update
        JobDefinition def2;
        def2.job_type = "update_test";
        def2.job_definition = "version 2";
        def2.timeout_seconds = 200;

        REQUIRE(db.insertJobDefinition(def2));

        auto retrieved = db.getJobDefinition("update_test");
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->job_definition == "version 2");
        REQUIRE(retrieved->timeout_seconds == 200);
    }

    SECTION("Get non-existent job definition")
    {
        auto retrieved = db.getJobDefinition("nonexistent");
        REQUIRE_FALSE(retrieved.has_value());
    }

    SECTION("Get all job definitions")
    {
        JobDefinition def1{"job1", "script1", 100};
        JobDefinition def2{"job2", "script2", 200};
        JobDefinition def3{"job3", "script3", 300};

        REQUIRE(db.insertJobDefinition(def1));
        REQUIRE(db.insertJobDefinition(def2));
        REQUIRE(db.insertJobDefinition(def3));

        auto all_defs = db.getAllJobDefinitions();
        REQUIRE(all_defs.size() == 3);

        // Check that all definitions are present
        bool found_job1 = false, found_job2 = false, found_job3 = false;
        for (const auto& def : all_defs)
        {
            if (def.job_type == "job1") found_job1 = true;
            if (def.job_type == "job2") found_job2 = true;
            if (def.job_type == "job3") found_job3 = true;
        }
        REQUIRE(found_job1);
        REQUIRE(found_job2);
        REQUIRE(found_job3);
    }

    db.close();
    cleanupDb(db_path);
}

TEST_CASE("SQLiteDatabase: Queue snapshot operations", "[JobDatabase][SQLiteDatabase]")
{
    SQLiteDatabase db;
    std::string db_path = getTempDbPath();
    REQUIRE(db.open(db_path));

    SECTION("Save and load queue snapshot")
    {
        orchestrator::job_queue::QueueSnapshot snapshot;
        snapshot.boot_id = db.getBootId();
        snapshot.snapshot_time_seconds = 1234567890;

        // Add pending jobs
        Job job1;
        job1.id = 1;
        job1.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED;
        job1.priority = 5;
        job1.spawnTimeSeconds = 1234567800;

        snapshot.pending_jobs.push_back(job1);
        snapshot.active_job_ids.push_back(2);
        snapshot.active_job_ids.push_back(3);

        REQUIRE(db.saveSnapshot(snapshot));

        auto loaded = db.loadMostRecentSnapshot();
        REQUIRE(loaded.snapshot_time_seconds == 1234567890);
        // Note: Full deserialization is TODO, but time is saved
    }

    SECTION("Load snapshot from empty database")
    {
        auto loaded = db.loadMostRecentSnapshot();
        REQUIRE(loaded.snapshot_time_seconds == 0);
        REQUIRE(loaded.boot_id == db.getBootId());
    }

    SECTION("Multiple snapshots - load most recent")
    {
        orchestrator::job_queue::QueueSnapshot snap1;
        snap1.boot_id = db.getBootId();
        snap1.snapshot_time_seconds = 100;

        orchestrator::job_queue::QueueSnapshot snap2;
        snap2.boot_id = db.getBootId();
        snap2.snapshot_time_seconds = 200;

        orchestrator::job_queue::QueueSnapshot snap3;
        snap3.boot_id = db.getBootId();
        snap3.snapshot_time_seconds = 300;

        REQUIRE(db.saveSnapshot(snap1));
        REQUIRE(db.saveSnapshot(snap2));
        REQUIRE(db.saveSnapshot(snap3));

        auto loaded = db.loadMostRecentSnapshot();
        REQUIRE(loaded.snapshot_time_seconds == 300);
    }

    db.close();
    cleanupDb(db_path);
}

TEST_CASE("SQLiteDatabase: Boot ID tracking", "[JobDatabase][SQLiteDatabase]")
{
    std::string db_path = getTempDbPath();

    SECTION("Each database instance gets unique boot ID")
    {
        SQLiteDatabase db1;
        REQUIRE(db1.open(db_path));
        std::string boot_id1 = db1.getBootId();
        REQUIRE_FALSE(boot_id1.empty());
        REQUIRE(boot_id1.length() == 36);  // UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        db1.close();

        SQLiteDatabase db2;
        REQUIRE(db2.open(db_path));
        std::string boot_id2 = db2.getBootId();
        REQUIRE_FALSE(boot_id2.empty());
        REQUIRE(boot_id2.length() == 36);

        // Different instances should have different boot IDs
        REQUIRE(boot_id1 != boot_id2);
        db2.close();
    }

    SECTION("Boot ID format is valid UUID")
    {
        SQLiteDatabase db;
        REQUIRE(db.open(db_path));
        std::string boot_id = db.getBootId();

        // Check format: 8-4-4-4-12 hex digits
        REQUIRE(boot_id[8] == '-');
        REQUIRE(boot_id[13] == '-');
        REQUIRE(boot_id[18] == '-');
        REQUIRE(boot_id[23] == '-');

        db.close();
    }

    cleanupDb(db_path);
}

TEST_CASE("SQLiteDatabase: Job history operations", "[JobDatabase][SQLiteDatabase]")
{
    SQLiteDatabase db;
    std::string db_path = getTempDbPath();
    REQUIRE(db.open(db_path));

    SECTION("Insert and retrieve job history")
    {
        orchestrator::job_queue::JobResult result;
        result.job_id = 42;
        result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE;

        REQUIRE(db.insertJobHistory(result));

        auto retrieved = db.getJobHistory(42);
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->job_id == 42);
        REQUIRE(retrieved->status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE);
    }

    SECTION("Get non-existent job history")
    {
        auto retrieved = db.getJobHistory(999);
        REQUIRE_FALSE(retrieved.has_value());
    }

    SECTION("Query job history by time range")
    {
        // Insert multiple job results
        for (int i = 0; i < 5; ++i)
        {
            orchestrator::job_queue::JobResult result;
            result.job_id = i;
            result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE;
            REQUIRE(db.insertJobHistory(result));

            // Small delay to ensure different timestamps
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Query recent history
        auto now = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
        auto one_hour_ago = now - 3600;

        auto results = db.getJobHistoryByTimeRange(one_hour_ago, now + 1);
        REQUIRE(results.size() == 5);
    }

    SECTION("Update job history (replace existing)")
    {
        orchestrator::job_queue::JobResult result1;
        result1.job_id = 100;
        result1.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ACTIVE;

        REQUIRE(db.insertJobHistory(result1));

        // Update with new status
        orchestrator::job_queue::JobResult result2;
        result2.job_id = 100;
        result2.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE;

        REQUIRE(db.insertJobHistory(result2));

        auto retrieved = db.getJobHistory(100);
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE);
    }

    db.close();
    cleanupDb(db_path);
}

TEST_CASE("SQLiteDatabase: Maintenance operations", "[JobDatabase][SQLiteDatabase]")
{
    SQLiteDatabase db;
    std::string db_path = getTempDbPath();
    REQUIRE(db.open(db_path));

    SECTION("Vacuum database")
    {
        // Insert some data
        JobDefinition def{"test", "script", 100};
        REQUIRE(db.insertJobDefinition(def));

        // Vacuum should not fail
        db.vacuum();
        REQUIRE(db.isOpen());
    }

    SECTION("Cleanup old snapshots")
    {
        // Insert 15 snapshots
        for (int i = 0; i < 15; ++i)
        {
            orchestrator::job_queue::QueueSnapshot snap;
            snap.boot_id = db.getBootId();
            snap.snapshot_time_seconds = 1000 + i;
            REQUIRE(db.saveSnapshot(snap));
        }

        // Keep only last 5
        db.cleanupOldSnapshots(5);

        // Most recent should still be accessible
        auto loaded = db.loadMostRecentSnapshot();
        REQUIRE(loaded.snapshot_time_seconds == 1014);  // Last one (1000 + 14)
    }

    SECTION("Cleanup old history")
    {
        // Cleanup should not fail even with no data
        db.cleanupOldHistory(30);
        REQUIRE(db.isOpen());
    }

    db.close();
    cleanupDb(db_path);
}

// ══════════════════════════════════════════════════════════════════════════════
// Store Unit Tests
// ══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Store: Initialization", "[JobDatabase][Store]")
{
    Store store;
    std::string db_path = getTempDbPath();
    store.setDatabasePath(db_path);

    SECTION("Initialize opens database")
    {
        REQUIRE_FALSE(store.initialized);
        REQUIRE(store.initialize());
        REQUIRE(store.initialized);
        REQUIRE(store.db.isOpen());

        store.db.close();
        cleanupDb(db_path);
    }

    SECTION("Multiple initializations are safe")
    {
        REQUIRE(store.initialize());
        REQUIRE(store.initialize());  // Should return true
        REQUIRE(store.initialized);

        store.db.close();
        cleanupDb(db_path);
    }

    SECTION("Default database path")
    {
        Store default_store;
        REQUIRE(default_store.db_path == "/tmp/orchestrator.db");
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// FSM State Tests
// ══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FSM States: InitState transitions to RunningState", "[JobDatabase][FSM]")
{
    Store store;
    Ports ports;
    Container container;
    std::string db_path = getTempDbPath();
    store.setDatabasePath(db_path);

    SECTION("Successful initialization transitions to RunningState")
    {
        InitState init_state;
        ::services::LogicalTag tag{::services::LogicalTime{1'000'000'000}};
        ::services::StepTrigger trigger;
        trigger.type = ::services::StepTrigger::Type::HEARTBEAT;

        size_t next_state = init_state.step(store, ports, container, tag, trigger);

        REQUIRE(next_state == RunningState::index());
        REQUIRE(store.initialized);
        REQUIRE(store.db.isOpen());

        store.db.close();
        cleanupDb(db_path);
    }
}

TEST_CASE("FSM States: RunningState remains in RunningState", "[JobDatabase][FSM]")
{
    Store store;
    Ports ports;
    Container container;
    std::string db_path = getTempDbPath();
    store.setDatabasePath(db_path);
    store.initialize();

    SECTION("RunningState is stable")
    {
        RunningState running_state;
        ::services::LogicalTag tag{::services::LogicalTime{1'000'000'000}};
        ::services::StepTrigger trigger;
        trigger.type = ::services::StepTrigger::Type::HEARTBEAT;

        size_t next_state = running_state.step(store, ports, container, tag, trigger);

        REQUIRE(next_state == RunningState::index());

        store.db.close();
        cleanupDb(db_path);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Integration Tests (Reactor-level)
// ══════════════════════════════════════════════════════════════════════════════

TEST_CASE("JobDatabase Reactor: Initialization", "[JobDatabase][Reactor]")
{
    std::string db_path = getTempDbPath();
    Container container;
    TestableJobDatabase reactor;
    reactor.setDatabasePath(db_path);

    SECTION("Heartbeat frequency is 10 seconds")
    {
        auto duration = reactor.heartbeatDuration();
        REQUIRE(duration.count() == 10'000'000'000);
    }

    cleanupDb(db_path);
}

TEST_CASE("JobDatabase Reactor: Job definition handling", "[JobDatabase][Reactor]")
{
    std::string db_path = getTempDbPath();
    TestableJobDatabase reactor;
    reactor.setDatabasePath(db_path);

    // Initialize and set to running state
    reactor.getStore().initialize();
    reactor.setStateToRunning();

    SECTION("Handle job definition via ports")
    {
        JobDefinition def{"test_type", "echo test", 600};
        reactor.getPorts().define_job_in.set(def);

        // Manually call RunningState step with logical action trigger
        ::services::LogicalTag tag{::services::LogicalTime{1'000'000'000}};
        ::services::StepTrigger trigger;
        trigger.type = ::services::StepTrigger::Type::LOGICAL_ACTION;
        trigger.action_name = "on_port_define_job";

        RunningState{}.step(reactor.getStore(), reactor.getPorts(), Container{}, tag, trigger);

        // Verify stored in database
        auto retrieved = reactor.getStore().db.getJobDefinition("test_type");
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->job_definition == "echo test");
    }

    reactor.getStore().db.close();
    cleanupDb(db_path);
}

TEST_CASE("JobDatabase Reactor: Snapshot handling", "[JobDatabase][Reactor]")
{
    std::string db_path = getTempDbPath();
    TestableJobDatabase reactor;
    reactor.setDatabasePath(db_path);

    reactor.getStore().initialize();
    reactor.setStateToRunning();

    SECTION("Handle snapshot save via ports")
    {
        orchestrator::job_queue::QueueSnapshot snapshot;
        snapshot.boot_id = "test-boot-id";
        snapshot.snapshot_time_seconds = 9999;

        Job job;
        job.id = 1;
        job.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED;
        snapshot.pending_jobs.push_back(job);

        reactor.getPorts().save_snapshot_in.set(snapshot);

        // Manually call RunningState step with logical action trigger
        ::services::LogicalTag tag{::services::LogicalTime{1'000'000'000}};
        ::services::StepTrigger trigger;
        trigger.type = ::services::StepTrigger::Type::LOGICAL_ACTION;
        trigger.action_name = "on_port_save_snapshot";

        RunningState{}.step(reactor.getStore(), reactor.getPorts(), Container{}, tag, trigger);

        // Verify snapshot was saved (boot ID should be overridden with reactor's boot ID)
        auto loaded = reactor.getStore().db.loadMostRecentSnapshot();
        REQUIRE(loaded.snapshot_time_seconds > 0);
    }

    SECTION("Handle snapshot load on initialization")
    {
        // Save a snapshot first
        orchestrator::job_queue::QueueSnapshot snapshot;
        snapshot.boot_id = reactor.getStore().db.getBootId();
        snapshot.snapshot_time_seconds = 5555;
        reactor.getStore().db.saveSnapshot(snapshot);

        // Verify via database that snapshot exists
        auto loaded = reactor.getStore().db.loadMostRecentSnapshot();
        REQUIRE(loaded.snapshot_time_seconds == 5555);
    }

    reactor.getStore().db.close();
    cleanupDb(db_path);
}

TEST_CASE("JobDatabase Reactor: Job history handling", "[JobDatabase][Reactor]")
{
    std::string db_path = getTempDbPath();
    TestableJobDatabase reactor;
    reactor.setDatabasePath(db_path);

    reactor.getStore().initialize();
    reactor.setStateToRunning();

    SECTION("Handle job history via ports")
    {
        orchestrator::job_queue::JobResult result;
        result.job_id = 777;
        result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE;

        reactor.getPorts().job_history_in.set(result);

        // Manually call RunningState step with logical action trigger
        ::services::LogicalTag tag{::services::LogicalTime{1'000'000'000}};
        ::services::StepTrigger trigger;
        trigger.type = ::services::StepTrigger::Type::LOGICAL_ACTION;
        trigger.action_name = "on_port_job_history";

        RunningState{}.step(reactor.getStore(), reactor.getPorts(), Container{}, tag, trigger);

        // Verify stored in database
        auto retrieved = reactor.getStore().db.getJobHistory(777);
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->job_id == 777);
        REQUIRE(retrieved->status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE);
    }

    reactor.getStore().db.close();
    cleanupDb(db_path);
}
