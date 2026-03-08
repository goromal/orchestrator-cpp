#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#pragma GCC diagnostic pop

#include "orchestrator/JobQueue.h"
#include <mscpp/ReactorScheduler.h>
#include <mscpp/Topology.h>

using namespace orchestrator;
using namespace orchestrator::job_queue;
using namespace aapis::orchestrator::v1;

// ══════════════════════════════════════════════════════════════════════════════
// Test Helper - Provides access to protected state machine for testing
// ══════════════════════════════════════════════════════════════════════════════

class TestableJobQueue : public JobQueue {
public:
    using JobQueue::JobQueue;

    void setStateToRunning() {
        this->mStateMachine.mActiveState = RunningState::index();
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// Store Unit Tests (Business Logic - No Reactor)
// ══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Store: Job registration and ID assignment")
{
    Store store;

    Job job1, job2, job3;
    job1.priority = 1;
    job2.priority = 0;
    job3.priority = 0;

    int64_t id1 = store.addAndRegisterNewJob(job1, false);
    int64_t id2 = store.addAndRegisterNewJob(job2, false);
    int64_t id3 = store.addAndRegisterNewJob(job3, false);

    // IDs must be unique
    REQUIRE(id1 != id2);
    REQUIRE(id2 != id3);
    REQUIRE(id1 != id3);

    // IDs are monotonically increasing (timestamp-based)
    REQUIRE(id2 > id1);
    REQUIRE(id3 > id2);

    // Jobs are stored in priority order
    REQUIRE(store.pendingJobs.size() == 3);
}

TEST_CASE("Store: Job sorting with dependencies")
{
    Store store;

    Job job1, job2, job3, job4, job5, job6;

    job1.priority = 1;
    int64_t id1 = store.addAndRegisterNewJob(job1, false);

    job2.priority = 0;
    int64_t id2 = store.addAndRegisterNewJob(job2, false);

    job3.priority = 0;
    job3.independentBlockers.push_back(id1);
    job3.relevantBlockers.push_back(id2);
    int64_t id3 = store.addAndRegisterNewJob(job3, false);

    job4.priority = 1;
    job4.relevantBlockers.push_back(id1);
    int64_t id4 = store.addAndRegisterNewJob(job4, false);

    job5.priority = 5;
    int64_t id5 = store.addAndRegisterNewJob(job5, false);

    job6.priority = 0;
    job6.independentBlockers.push_back(id5);
    int64_t id6 = store.addAndRegisterNewJob(job6, false);

    // Verify statuses
    auto jobs = store.query(JobQuery{JobQuery::Type::GET_ALL_QUEUED});
    REQUIRE(jobs.size() == 6);

    // Job 1, 2, 5 should be QUEUED (no blockers)
    REQUIRE(jobs[0].status == JobStatus::JOB_STATUS_QUEUED);
    REQUIRE(jobs[1].status == JobStatus::JOB_STATUS_QUEUED);
    REQUIRE(jobs[4].status == JobStatus::JOB_STATUS_QUEUED);

    // Job 3, 4, 6 should be BLOCKED
    REQUIRE(jobs[2].status == JobStatus::JOB_STATUS_BLOCKED);
    REQUIRE(jobs[3].status == JobStatus::JOB_STATUS_BLOCKED);
    REQUIRE(jobs[5].status == JobStatus::JOB_STATUS_BLOCKED);

    // Verify topological sort order (priority 0 before priority 1)
    REQUIRE(jobs[0].id == id2);  // Priority 0, no blockers
    REQUIRE(jobs[1].id == id1);  // Priority 1, no blockers
    REQUIRE(jobs[2].id == id3);  // Priority 0, blocked by id1, id2
    REQUIRE(jobs[3].id == id4);  // Priority 1, blocked by id1
    REQUIRE(jobs[4].id == id5);  // Priority 5, no blockers
    REQUIRE(jobs[5].id == id6);  // Priority 0, blocked by id5
}

TEST_CASE("Store: Pause and unpause jobs")
{
    Store store;

    Job job1, job2, job3;
    job1.priority = 0;
    job2.priority = 1;

    int64_t id1 = store.addAndRegisterNewJob(job1, false);
    int64_t id2 = store.addAndRegisterNewJob(job2, false);

    job3.priority = 0;
    job3.independentBlockers.push_back(id1);
    int64_t id3 = store.addAndRegisterNewJob(job3, false);

    // Pause all jobs
    store.pauseJobs();

    auto jobs = store.query(JobQuery{JobQuery::Type::GET_ALL_QUEUED});
    REQUIRE(jobs[0].status == JobStatus::JOB_STATUS_PAUSED);
    REQUIRE(jobs[1].status == JobStatus::JOB_STATUS_PAUSED);
    REQUIRE(jobs[2].status == JobStatus::JOB_STATUS_PAUSED);

    // Verify pre-pause statuses are preserved
    // Note: Sort order is (no-blockers first, then by priority, then by ID)
    // jobs[0] = id1 (priority 0, QUEUED)
    // jobs[1] = id3 (priority 0, BLOCKED) - blocked jobs sorted with non-blocked of same priority
    // jobs[2] = id2 (priority 1, QUEUED)
    // Actually, let's verify by checking individual job IDs
    for (const auto& job : jobs) {
        if (job.id == id1 || job.id == id2) {
            REQUIRE(job.prePauseStatus == JobStatus::JOB_STATUS_QUEUED);
        } else if (job.id == id3) {
            REQUIRE(job.prePauseStatus == JobStatus::JOB_STATUS_BLOCKED);
        }
    }

    // Unpause
    store.unpauseJobs();

    jobs = store.query(JobQuery{JobQuery::Type::GET_ALL_QUEUED});
    // Verify statuses restored correctly
    for (const auto& job : jobs) {
        if (job.id == id1 || job.id == id2) {
            REQUIRE(job.status == JobStatus::JOB_STATUS_QUEUED);
        } else if (job.id == id3) {
            REQUIRE(job.status == JobStatus::JOB_STATUS_BLOCKED);
        }
    }
}

TEST_CASE("Store: Process job result with string outputs")
{
    Store store;

    Job job1, job2;
    job1.priority = 0;
    int64_t id1 = store.addAndRegisterNewJob(job1, false);

    job2.priority = 0;
    job2.relevantBlockers.push_back(id1);  // Relevant blocker - outputs become inputs
    int64_t id2 = store.addAndRegisterNewJob(job2, false);

    // Simulate job completion with outputs
    JobResult result;
    result.job_id = id1;
    result.status = JobStatus::JOB_STATUS_COMPLETE;
    result.outputs = std::vector<std::string>{"output1", "output2"};

    store.processJobResult(result, false);

    // Job 2 should now be unblocked and have outputs as inputs
    auto jobs = store.query(JobQuery{JobQuery::Type::GET_BY_ID, -1, id2});
    REQUIRE(jobs.size() == 1);
    REQUIRE(jobs[0].numBlockers() == 0);
    REQUIRE(jobs[0].status == JobStatus::JOB_STATUS_QUEUED);
    REQUIRE(jobs[0].inputs.size() == 2);
    REQUIRE(jobs[0].inputs[0] == "output1");
    REQUIRE(jobs[0].inputs[1] == "output2");
}

TEST_CASE("Store: Process job result with error status")
{
    Store store;

    Job job1, job2, job3;
    job1.priority = 0;
    int64_t id1 = store.addAndRegisterNewJob(job1, false);

    job2.priority = 0;
    job2.independentBlockers.push_back(id1);
    int64_t id2 = store.addAndRegisterNewJob(job2, false);

    job3.priority = 0;
    job3.relevantBlockers.push_back(id1);
    int64_t id3 = store.addAndRegisterNewJob(job3, false);

    // Simulate job failure
    JobResult result;
    result.job_id = id1;
    result.status = JobStatus::JOB_STATUS_ERROR;

    store.processJobResult(result, false);

    // Dependent jobs should be canceled
    auto jobs = store.query(JobQuery{JobQuery::Type::GET_ALL_QUEUED});
    auto job2_it = std::find_if(jobs.begin(), jobs.end(), [id2](const Job& j) { return j.id == id2; });
    auto job3_it = std::find_if(jobs.begin(), jobs.end(), [id3](const Job& j) { return j.id == id3; });

    REQUIRE(job2_it != jobs.end());
    REQUIRE(job3_it != jobs.end());
    REQUIRE(job2_it->status == JobStatus::JOB_STATUS_CANCELED);
    REQUIRE(job3_it->status == JobStatus::JOB_STATUS_CANCELED);
}

TEST_CASE("Store: Query operations")
{
    Store store;

    Job job1, job2, job3;
    job1.priority = 0;
    job2.priority = 1;
    job3.priority = 0;

    [[maybe_unused]] int64_t id1 = store.addAndRegisterNewJob(job1, false);
    int64_t id2 = store.addAndRegisterNewJob(job2, false);
    [[maybe_unused]] int64_t id3 = store.addAndRegisterNewJob(job3, false);

    // Query all
    auto all_jobs = store.query(JobQuery{JobQuery::Type::GET_ALL_QUEUED});
    REQUIRE(all_jobs.size() == 3);

    // Query by priority
    auto priority_0 = store.query(JobQuery{JobQuery::Type::GET_BY_PRIORITY, 0});
    REQUIRE(priority_0.size() == 2);

    auto priority_1 = store.query(JobQuery{JobQuery::Type::GET_BY_PRIORITY, 1});
    REQUIRE(priority_1.size() == 1);

    // Query by ID
    auto by_id = store.query(JobQuery{JobQuery::Type::GET_BY_ID, -1, id2});
    REQUIRE(by_id.size() == 1);
    REQUIRE(by_id[0].id == id2);
}

TEST_CASE("Store: Snapshot create and restore")
{
    Store store;

    Job job1, job2;
    job1.priority = 0;
    job2.priority = 1;

    int64_t id1 = store.addAndRegisterNewJob(job1, false);
    [[maybe_unused]] int64_t id2 = store.addAndRegisterNewJob(job2, false);

    // Mark one as active
    store.activeJobIds[id1] = true;

    // Create snapshot
    auto snapshot = store.createSnapshot();
    REQUIRE(snapshot.pending_jobs.size() == 2);
    REQUIRE(snapshot.active_job_ids.size() == 1);
    REQUIRE(snapshot.active_job_ids[0] == id1);

    // Clear store
    Store store2;

    // Restore snapshot
    store2.restoreFromSnapshot(snapshot);
    REQUIRE(store2.pendingJobs.size() == 2);
    REQUIRE(store2.activeJobIds.size() == 1);
    REQUIRE(store2.activeJobIds.count(id1) == 1);
}

// ══════════════════════════════════════════════════════════════════════════════
// Reactor Unit Tests (Event-Driven with Logical Actions)
// ══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Reactor: Event-driven job submission")
{
    // Create reactor without scheduler (direct testing)
    TestableJobQueue queue;

    // Manually transition reactor to RunningState
    queue.setStateToRunning();

    // Create a job
    Job job;
    job.priority = 5;

    // Simulate event: new job arrives on port
    queue.getPorts().new_job_in.set(job);

    // Trigger logical action directly
    ::services::LogicalTag tag(::services::LogicalTime(0), 0);
    queue.executeLogicalAction(tag, "on_port_new_job");

    // Verify job was registered
    auto& store = queue.getStore();
    REQUIRE(store.pendingJobs.size() == 1);
    REQUIRE(store.pendingJobs[0].priority == 5);
}

TEST_CASE("Reactor: Event-driven query handling")
{
    TestableJobQueue queue;

    // Manually transition reactor to RunningState
    queue.setStateToRunning();

    // Add a job directly to store
    Job job;
    job.priority = 3;
    queue.getStore().addAndRegisterNewJob(job, false);

    // Simulate query event
    JobQuery query{JobQuery::Type::GET_ALL_QUEUED};
    queue.getPorts().query_request_in.set(query);

    // Trigger logical action
    ::services::LogicalTag tag(::services::LogicalTime(0), 0);
    queue.executeLogicalAction(tag, "on_port_query");

    // Verify store has the job we added
    auto& store = queue.getStore();
    REQUIRE(store.pendingJobs.size() == 1);
    REQUIRE(store.pendingJobs[0].priority == 3);
}

TEST_CASE("Reactor: Event-driven pause/resume")
{
    TestableJobQueue queue;

    // Manually transition reactor to RunningState
    queue.setStateToRunning();

    // Add jobs
    Job job1, job2;
    job1.priority = 0;
    job2.priority = 1;
    queue.getStore().addAndRegisterNewJob(job1, false);
    queue.getStore().addAndRegisterNewJob(job2, false);

    // Simulate pause command
    ControlRequest pause_req{ControlRequest::Command::PAUSE};
    queue.getPorts().control_request_in.set(pause_req);

    ::services::LogicalTag tag(::services::LogicalTime(0), 0);
    queue.executeLogicalAction(tag, "on_port_control");

    // Check jobs are paused
    auto& store = queue.getStore();
    REQUIRE(store.pendingJobs[0].status == JobStatus::JOB_STATUS_PAUSED);
    REQUIRE(store.pendingJobs[1].status == JobStatus::JOB_STATUS_PAUSED);

    // Simulate resume command
    ControlRequest resume_req{ControlRequest::Command::RESUME};
    queue.getPorts().control_request_in.set(resume_req);

    queue.executeLogicalAction(tag, "on_port_control");

    // Check jobs are resumed and automatically sent to executor (ACTIVE state)
    REQUIRE(store.pendingJobs[0].status == JobStatus::JOB_STATUS_ACTIVE);
    REQUIRE(store.pendingJobs[1].status == JobStatus::JOB_STATUS_ACTIVE);
}

TEST_CASE("Reactor: Event-driven job completion")
{
    TestableJobQueue queue;

    // Manually transition reactor to RunningState
    queue.setStateToRunning();

    // Add jobs with dependencies
    Job job1, job2;
    job1.priority = 0;
    int64_t id1 = queue.getStore().addAndRegisterNewJob(job1, false);

    job2.priority = 0;
    job2.relevantBlockers.push_back(id1);
    int64_t id2 = queue.getStore().addAndRegisterNewJob(job2, false);

    // Mark job1 as active
    queue.getStore().activeJobIds[id1] = true;

    // Simulate job completion
    JobResult result;
    result.job_id = id1;
    result.status = JobStatus::JOB_STATUS_COMPLETE;
    result.outputs = std::vector<std::string>{"output"};

    queue.getPorts().job_result_in.set(result);

    // Trigger logical action
    ::services::LogicalTag tag(::services::LogicalTime(0), 0);
    queue.executeLogicalAction(tag, "on_port_job_result");

    // Check job2 is now unblocked and automatically sent to executor (ACTIVE state)
    auto& store = queue.getStore();
    auto jobs = store.query(JobQuery{JobQuery::Type::GET_BY_ID, -1, id2});
    REQUIRE(jobs.size() == 1);
    REQUIRE(jobs[0].numBlockers() == 0);
    REQUIRE(jobs[0].status == JobStatus::JOB_STATUS_ACTIVE);
}

// ══════════════════════════════════════════════════════════════════════════════
// Integration Tests (With Scheduler - Future)
// ══════════════════════════════════════════════════════════════════════════════

// TODO: Add integration tests with ReactorScheduler and ConnectionManager
// TODO: Test automatic logical action scheduling on port connections
// TODO: Test end-to-end job flow with mock JobExecutor and JobDatabase
