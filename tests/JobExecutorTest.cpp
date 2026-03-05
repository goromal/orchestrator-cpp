#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include "orchestrator/JobExecutor.h"
#include "orchestrator/JobQueue.h"

#include <chrono>
#include <thread>
#include <fstream>

using namespace orchestrator;
using namespace orchestrator::job_executor;

// ══════════════════════════════════════════════════════════════════════════════
// Store Unit Tests - Pure Business Logic
// ══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Store: Thread availability check", "[JobExecutor][Store]")
{
    Store store;
    store.max_threads = 4;

    SECTION("Empty store has available threads")
    {
        REQUIRE(store.hasAvailableThread());
        REQUIRE_FALSE(store.hasActiveJobs());
    }

    SECTION("Full store has no available threads")
    {
        // Simulate 4 active jobs
        for (int i = 0; i < 4; ++i)
        {
            WorkerThread worker;
            worker.job_id = i;
            worker.pid = 1000 + i;
            store.active_jobs[i] = worker;
        }

        REQUIRE_FALSE(store.hasAvailableThread());
        REQUIRE(store.hasActiveJobs());
    }

    SECTION("Partial store has available threads")
    {
        // Simulate 2 active jobs
        for (int i = 0; i < 2; ++i)
        {
            WorkerThread worker;
            worker.job_id = i;
            worker.pid = 1000 + i;
            store.active_jobs[i] = worker;
        }

        REQUIRE(store.hasAvailableThread());
        REQUIRE(store.hasActiveJobs());
    }
}

TEST_CASE("Store: Bash variable substitution", "[JobExecutor][Store]")
{
    SECTION("Substitute INPUT_IDS with blockers")
    {
        Job job;
        job.independentBlockers = {1, 2, 3};
        job.relevantBlockers = {4, 5};
        job.script = "echo $INPUT_IDS";

        std::string result = Store::substituteVariables(job.script, job);

        REQUIRE(result == "echo (1 2 3 4 5)");
    }

    SECTION("Substitute INPUT_ARGS with inputs")
    {
        Job job;
        job.inputs = {"hello", "world", "test"};
        job.script = "echo $INPUT_ARGS";

        std::string result = Store::substituteVariables(job.script, job);

        REQUIRE(result == "echo (\"hello\" \"world\" \"test\")");
    }

    SECTION("Substitute both INPUT_IDS and INPUT_ARGS")
    {
        Job job;
        job.independentBlockers = {1, 2};
        job.inputs = {"arg1", "arg2"};
        job.script = "process $INPUT_IDS $INPUT_ARGS";

        std::string result = Store::substituteVariables(job.script, job);

        REQUIRE(result == "process (1 2) (\"arg1\" \"arg2\")");
    }

    SECTION("Substitute with empty arrays")
    {
        Job job;
        job.script = "echo $INPUT_IDS $INPUT_ARGS";

        std::string result = Store::substituteVariables(job.script, job);

        REQUIRE(result == "echo () ()");
    }

    SECTION("Shell escape special characters")
    {
        Job job;
        job.inputs = {"hello\"world", "test$var", "back\\slash", "tick`cmd`"};
        job.script = "echo $INPUT_ARGS";

        std::string result = Store::substituteVariables(job.script, job);

        // Check that special characters are escaped
        REQUIRE(result.find("hello\\\"world") != std::string::npos);
        REQUIRE(result.find("test\\$var") != std::string::npos);
        REQUIRE(result.find("back\\\\slash") != std::string::npos);
        REQUIRE(result.find("tick\\`cmd\\`") != std::string::npos);
    }
}

TEST_CASE("Store: Job submission with subprocess", "[JobExecutor][Store]")
{
    Store store;
    store.max_threads = 4;

    SECTION("Submit simple job successfully")
    {
        Job job;
        job.id = 100;
        job.script = "echo 'test'";
        job.timeoutSeconds = 5;

        bool submitted = store.submitJob(job);

        REQUIRE(submitted);
        REQUIRE(store.active_jobs.count(100) == 1);
        REQUIRE(store.active_jobs[100].pid > 0);
        REQUIRE(store.active_jobs[100].job_id == 100);
        REQUIRE(store.active_jobs[100].timeout_seconds == 5);
        REQUIRE_FALSE(store.active_jobs[100].completed);

        // Wait for job to complete and reap
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        store.pollCompletedJobs();
    }

    SECTION("Reject job when no threads available")
    {
        // Fill all threads
        for (int i = 0; i < 4; ++i)
        {
            WorkerThread worker;
            worker.job_id = i;
            worker.pid = 1000 + i;
            store.active_jobs[i] = worker;
        }

        Job job;
        job.id = 999;
        job.script = "echo 'test'";

        bool submitted = store.submitJob(job);

        REQUIRE_FALSE(submitted);
        REQUIRE(store.active_jobs.count(999) == 0);
    }

    SECTION("Submit job with variable substitution")
    {
        Job job;
        job.id = 101;
        job.script = "echo $INPUT_ARGS";
        job.inputs = {"hello", "world"};
        job.timeoutSeconds = 5;

        bool submitted = store.submitJob(job);

        REQUIRE(submitted);
        REQUIRE(store.active_jobs.count(101) == 1);

        // Wait and reap
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        store.pollCompletedJobs();
    }
}

TEST_CASE("Store: Polling for completed jobs", "[JobExecutor][Store]")
{
    Store store;
    store.max_threads = 4;

    SECTION("Poll with no active jobs")
    {
        auto completed = store.pollCompletedJobs();
        REQUIRE(completed.empty());
    }

    SECTION("Poll with running jobs returns empty")
    {
        Job job;
        job.id = 200;
        job.script = "sleep 10";  // Long-running job
        job.timeoutSeconds = 15;

        store.submitJob(job);

        // Poll immediately - job still running
        auto completed = store.pollCompletedJobs();
        REQUIRE(completed.empty());

        // Cleanup: kill the job
        store.cancelJob(200);
    }

    SECTION("Poll detects completed job")
    {
        Job job;
        job.id = 201;
        job.script = "exit 0";  // Immediate exit
        job.timeoutSeconds = 5;

        store.submitJob(job);

        // Wait for job to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto completed = store.pollCompletedJobs();

        REQUIRE(completed.size() == 1);
        REQUIRE(completed[0].job_id == 201);
        REQUIRE(completed[0].completed);
        REQUIRE(completed[0].exit_code == 0);
    }

    SECTION("Poll detects multiple completed jobs")
    {
        // Submit multiple quick jobs
        for (int i = 0; i < 3; ++i)
        {
            Job job;
            job.id = 300 + i;
            job.script = "exit " + std::to_string(i);
            job.timeoutSeconds = 5;
            store.submitJob(job);
        }

        // Wait for all to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto completed = store.pollCompletedJobs();

        REQUIRE(completed.size() == 3);

        // Check all were detected
        std::set<int64_t> found_ids;
        for (const auto& worker : completed)
        {
            found_ids.insert(worker.job_id);
            REQUIRE(worker.completed);
        }

        REQUIRE(found_ids.count(300) == 1);
        REQUIRE(found_ids.count(301) == 1);
        REQUIRE(found_ids.count(302) == 1);
    }

    SECTION("Poll detects non-zero exit code")
    {
        Job job;
        job.id = 202;
        job.script = "exit 42";
        job.timeoutSeconds = 5;

        store.submitJob(job);

        // Wait for completion
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto completed = store.pollCompletedJobs();

        REQUIRE(completed.size() == 1);
        REQUIRE(completed[0].job_id == 202);
        REQUIRE(completed[0].exit_code == 42);
    }
}

TEST_CASE("Store: Job cancellation", "[JobExecutor][Store]")
{
    Store store;
    store.max_threads = 4;

    SECTION("Cancel non-existent job")
    {
        bool cancelled = store.cancelJob(999);
        REQUIRE_FALSE(cancelled);
    }

    SECTION("Cancel running job")
    {
        Job job;
        job.id = 400;
        job.script = "sleep 100";  // Long-running
        job.timeoutSeconds = 120;

        store.submitJob(job);

        // Wait a bit to ensure job is running
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        bool cancelled = store.cancelJob(400);

        REQUIRE(cancelled);
        REQUIRE(store.active_jobs[400].completed);
        REQUIRE(store.active_jobs[400].exit_code == -1);
    }

    SECTION("Cancel already completed job")
    {
        Job job;
        job.id = 401;
        job.script = "exit 0";
        job.timeoutSeconds = 5;

        store.submitJob(job);

        // Wait for completion
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Mark as completed via polling
        store.pollCompletedJobs();

        bool cancelled = store.cancelJob(401);

        REQUIRE_FALSE(cancelled);  // Already completed
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Reactor Event-Driven Tests
// ══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Reactor: Initialization and state", "[JobExecutor][Reactor]")
{
    JobExecutor executor(4);
    executor.initialize();

    REQUIRE(executor.getCurrentState() == RunningState::index());
    REQUIRE_FALSE(executor.isPaused());
    REQUIRE(executor.getStore().max_threads == 4);
}

TEST_CASE("Reactor: Job submission event", "[JobExecutor][Reactor]")
{
    JobExecutor executor(4);
    executor.initialize();

    Job job;
    job.id = 500;
    job.script = "echo 'test'";
    job.timeoutSeconds = 5;

    // Set job on input port
    executor.getPorts().job_in.set(job);

    // Trigger action directly
    ::services::LogicalTag tag(::services::LogicalTime(0), 0);
    executor.executeLogicalAction(tag, "on_port_job_in");

    // Verify job was submitted
    REQUIRE(executor.getStore().active_jobs.count(500) == 1);
    REQUIRE(executor.getStore().active_jobs[500].pid > 0);

    // Cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    executor.getStore().pollCompletedJobs();
}

TEST_CASE("Reactor: Pause and resume", "[JobExecutor][Reactor]")
{
    JobExecutor executor(4);
    executor.initialize();

    SECTION("Pause executor")
    {
        orchestrator::job_queue::ControlRequest control;
        control.command = orchestrator::job_queue::ControlRequest::Command::PAUSE;

        executor.getPorts().control_in.set(control);

        ::services::LogicalTag tag(::services::LogicalTime(0), 0);
        executor.executeLogicalAction(tag, "on_port_control_in");

        REQUIRE(executor.isPaused());
        REQUIRE(executor.getCurrentState() == PausedState::index());
    }

    SECTION("Resume executor")
    {
        // First pause
        orchestrator::job_queue::ControlRequest pause;
        pause.command = orchestrator::job_queue::ControlRequest::Command::PAUSE;
        executor.getPorts().control_in.set(pause);

        ::services::LogicalTag tag1(::services::LogicalTime(0), 0);
        executor.executeLogicalAction(tag1, "on_port_control_in");

        REQUIRE(executor.isPaused());

        // Then resume
        executor.getPorts().control_in.clear();
        orchestrator::job_queue::ControlRequest resume;
        resume.command = orchestrator::job_queue::ControlRequest::Command::RESUME;
        executor.getPorts().control_in.set(resume);

        ::services::LogicalTag tag2(::services::LogicalTime(0), 0);
        executor.executeLogicalAction(tag2, "on_port_control_in");

        REQUIRE_FALSE(executor.isPaused());
        REQUIRE(executor.getCurrentState() == RunningState::index());
    }

    SECTION("Reject job when paused")
    {
        // Pause executor
        orchestrator::job_queue::ControlRequest pause;
        pause.command = orchestrator::job_queue::ControlRequest::Command::PAUSE;
        executor.getPorts().control_in.set(pause);

        ::services::LogicalTag tag1(::services::LogicalTime(0), 0);
        executor.executeLogicalAction(tag1, "on_port_control_in");

        // Try to submit job
        executor.getPorts().job_in.clear();
        Job job;
        job.id = 600;
        job.script = "echo 'test'";
        executor.getPorts().job_in.set(job);

        ::services::LogicalTag tag2(::services::LogicalTime(0), 0);
        executor.executeLogicalAction(tag2, "on_port_job_in");

        // Verify job was rejected (not in active jobs)
        REQUIRE(executor.getStore().active_jobs.count(600) == 0);

        // Verify error result was produced
        REQUIRE(executor.getPorts().job_result_out.has_pending_value());
        auto result = executor.getPorts().job_result_out.get_pending_value().value();
        REQUIRE(result.job_id == 600);
        REQUIRE(result.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR);
    }
}

TEST_CASE("Reactor: Job completion polling", "[JobExecutor][Reactor]")
{
    JobExecutor executor(4);
    executor.initialize();

    // Submit a quick job
    Job job;
    job.id = 700;
    job.script = "exit 0";
    job.timeoutSeconds = 5;

    executor.getPorts().job_in.set(job);
    ::services::LogicalTag tag1(::services::LogicalTime(0), 0);
    executor.executeLogicalAction(tag1, "on_port_job_in");

    // Wait for job to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Trigger polling
    ::services::LogicalTag tag2(::services::LogicalTime(0), 0);
    executor.executeLogicalAction(tag2, "poll_completions");

    // Verify result was produced
    REQUIRE(executor.getPorts().job_result_out.has_pending_value());
    auto result = executor.getPorts().job_result_out.get_pending_value().value();
    REQUIRE(result.job_id == 700);
    REQUIRE(result.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE);

    // Verify job was removed from active jobs
    REQUIRE(executor.getStore().active_jobs.count(700) == 0);
}

TEST_CASE("Reactor: Job timeout handling", "[JobExecutor][Reactor]")
{
    JobExecutor executor(4);
    executor.initialize();

    // Submit a long-running job
    Job job;
    job.id = 800;
    job.script = "sleep 100";
    job.timeoutSeconds = 1;  // 1 second timeout

    executor.getPorts().job_in.set(job);
    ::services::LogicalTag tag1(::services::LogicalTime(0), 0);
    executor.executeLogicalAction(tag1, "on_port_job_in");

    REQUIRE(executor.getStore().active_jobs.count(800) == 1);

    // Wait a bit for job to start running
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Simulate timeout action
    ::services::LogicalTag tag2(::services::LogicalTime(0), 0);
    executor.executeLogicalAction(tag2, "timeout_800");

    // Verify timeout result was produced
    REQUIRE(executor.getPorts().job_result_out.has_pending_value());
    auto result = executor.getPorts().job_result_out.get_pending_value().value();
    REQUIRE(result.job_id == 800);
    REQUIRE(result.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR);

    // Verify error message mentions timeout
    auto& outputs = std::get<std::vector<std::string>>(result.outputs);
    REQUIRE_FALSE(outputs.empty());
    REQUIRE(outputs[0].find("timed out") != std::string::npos);

    // Verify job was removed from active jobs
    REQUIRE(executor.getStore().active_jobs.count(800) == 0);
}

TEST_CASE("Reactor: Job cancellation via control", "[JobExecutor][Reactor]")
{
    JobExecutor executor(4);
    executor.initialize();

    // Submit a long-running job
    Job job;
    job.id = 900;
    job.script = "sleep 100";
    job.timeoutSeconds = 120;

    executor.getPorts().job_in.set(job);
    ::services::LogicalTag tag1(::services::LogicalTime(0), 0);
    executor.executeLogicalAction(tag1, "on_port_job_in");

    // Wait for job to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send cancel command
    executor.getPorts().control_in.clear();
    orchestrator::job_queue::ControlRequest cancel;
    cancel.command = orchestrator::job_queue::ControlRequest::Command::CANCEL;
    cancel.target_job_id = 900;
    executor.getPorts().control_in.set(cancel);

    ::services::LogicalTag tag2(::services::LogicalTime(0), 0);
    executor.executeLogicalAction(tag2, "on_port_control_in");

    // Verify cancellation result was produced
    REQUIRE(executor.getPorts().job_result_out.has_pending_value());
    auto result = executor.getPorts().job_result_out.get_pending_value().value();
    REQUIRE(result.job_id == 900);
    REQUIRE(result.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED);

    // Verify job was removed from active jobs
    REQUIRE(executor.getStore().active_jobs.count(900) == 0);
}

TEST_CASE("Reactor: Thread pool limit enforcement", "[JobExecutor][Reactor]")
{
    JobExecutor executor(2);  // Only 2 threads
    executor.initialize();

    // Submit 3 jobs (one more than limit)
    for (int i = 0; i < 3; ++i)
    {
        executor.getPorts().job_in.clear();
        Job job;
        job.id = 1000 + i;
        job.script = "sleep 10";
        job.timeoutSeconds = 15;

        executor.getPorts().job_in.set(job);
        ::services::LogicalTag tag(::services::LogicalTime(0), 0);
        executor.executeLogicalAction(tag, "on_port_job_in");
    }

    // Verify only 2 jobs were accepted
    REQUIRE(executor.getStore().active_jobs.size() == 2);

    // Cleanup
    for (int i = 0; i < 2; ++i)
    {
        executor.getStore().cancelJob(1000 + i);
    }
}
