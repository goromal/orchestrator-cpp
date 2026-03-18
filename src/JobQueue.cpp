#include "orchestrator/JobQueue.h"
#include <chrono>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <ranges>

namespace orchestrator
{

namespace job_queue
{

// ══════════════════════════════════════════════════════════════════════════════
// Store Implementation - Core Business Logic (Preserved from Original)
// ══════════════════════════════════════════════════════════════════════════════

int64_t Store::addAndRegisterNewJob(Job job, bool paused)
{
    auto id = initializeJobData(job, paused);

    // If the ID is already in pendingJobs, then throw an error
    if (std::find_if(pendingJobs.begin(), pendingJobs.end(),
                     [&](Job& j) { return j.id == id; }) != pendingJobs.end())
    {
        throw std::runtime_error("Duplicate job ID would be inserted in the Job Queue");
    }

    pendingJobs.push_back(std::move(job));
    sortJobs();

    return id;
}

int64_t Store::initializeJobData(Job& job, bool paused)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();

    job.spawnTimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    int64_t spawnMicrosId =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count() * 1e3 +
        static_cast<int64_t>(subCounter);

    subCounter++;

    job.id = spawnMicrosId;

    if (job.numBlockers() == 0)
    {
        job.status = (paused) ? aapis::orchestrator::v1::JobStatus::JOB_STATUS_PAUSED
                              : aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED;
        job.prePauseStatus = aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED;
    }
    else
    {
        job.status = (paused) ? aapis::orchestrator::v1::JobStatus::JOB_STATUS_PAUSED
                              : aapis::orchestrator::v1::JobStatus::JOB_STATUS_BLOCKED;
        job.prePauseStatus = aapis::orchestrator::v1::JobStatus::JOB_STATUS_BLOCKED;
    }

    return spawnMicrosId;
}

void Store::sortJobs()
{
    // Build lookup for O(1) access by ID
    std::unordered_map<int64_t, Job*> jobById;
    jobById.reserve(pendingJobs.size());
    for (auto& job : pendingJobs)
        jobById[job.id] = &job;

    // Build adjacency and indegree
    std::unordered_map<int64_t, std::unordered_set<int64_t>> adj;
    std::unordered_map<int64_t, int64_t> indegree;
    for (const auto& job : pendingJobs)
        indegree[job.id] = 0;

    for (const auto& job : pendingJobs)
    {
        for (int64_t dep : job.independentBlockers)
        {
            if (!jobById.count(dep))
                continue; // ignore missing blockers
            adj[dep].insert(job.id);
            indegree[job.id]++;
        }
        for (int64_t dep : job.relevantBlockers)
        {
            if (!jobById.count(dep))
                continue;
            adj[dep].insert(job.id);
            indegree[job.id]++;
        }
    }

    // Deterministic comparator for the priority queue
    auto cmp = [&](int64_t aId, int64_t bId) {
        const Job& a = *jobById.at(aId);
        const Job& b = *jobById.at(bId);
        // Reverse order for min-heap behavior
        return std::tuple(a.priority, a.numBlockers(), a.id) >
               std::tuple(b.priority, b.numBlockers(), b.id);
    };

    std::priority_queue<int64_t, std::vector<int64_t>, decltype(cmp)> ready(cmp);

    // Initialize queue with all zero-indegree jobs (Kahn's algorithm)
    for (const auto& [id, deg] : indegree)
        if (deg == 0)
            ready.push(id);

    std::vector<int64_t> sortedIds;
    sortedIds.reserve(pendingJobs.size());

    // Kahn's algorithm
    while (!ready.empty())
    {
        int64_t id = ready.top();
        ready.pop();
        sortedIds.push_back(id);

        for (int64_t next : adj[id])
        {
            if (--indegree[next] == 0)
                ready.push(next);
        }
    }

    // Handle cycles deterministically
    if (sortedIds.size() < pendingJobs.size())
    {
        std::vector<int64_t> remaining;
        remaining.reserve(pendingJobs.size() - sortedIds.size());
        for (const auto& [id, _] : indegree)
        {
            if (std::find(sortedIds.begin(), sortedIds.end(), id) == sortedIds.end())
                remaining.push_back(id);
        }

        std::sort(remaining.begin(), remaining.end(), [&](int64_t aId, int64_t bId) {
            const Job& a = *jobById.at(aId);
            const Job& b = *jobById.at(bId);
            return std::tuple(a.priority, a.numBlockers(), a.id) >
                   std::tuple(b.priority, b.numBlockers(), b.id);
        });

        sortedIds.insert(sortedIds.end(), remaining.begin(), remaining.end());
    }

    // Rebuild sorted vector
    std::vector<Job> result;
    result.reserve(pendingJobs.size());
    for (int64_t id : sortedIds)
        result.push_back(*jobById.at(id));

    pendingJobs = result;
}

void Store::pauseJobs()
{
    for (auto& job : pendingJobs)
    {
        job.prePauseStatus = job.status;
        job.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_PAUSED;
    }
}

void Store::unpauseJobs()
{
    for (auto& job : pendingJobs)
    {
        job.status = job.prePauseStatus;
    }
}

void Store::processJobResult(const JobResult& result, bool paused)
{
    int64_t jobId = result.job_id;

    // Remove from active jobs
    activeJobIds.erase(jobId);

    // If the job was unsuccessful, then mark all dependent jobs as canceled
    if (result.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR)
    {
        std::ranges::for_each(pendingJobs, [&](Job& j) {
            if (std::find(j.independentBlockers.begin(), j.independentBlockers.end(), jobId) !=
                    j.independentBlockers.end() ||
                std::find(j.relevantBlockers.begin(), j.relevantBlockers.end(), jobId) !=
                    j.relevantBlockers.end())
            {
                j.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED;
            }
        });
    }
    else
    {
        // If the job returned outputs, then remove the blocker from any blocked jobs
        // and add all outputs as inputs for the case of relevantBlockers.
        if (std::holds_alternative<std::vector<std::string>>(result.outputs))
        {
            auto outputs = std::get<std::vector<std::string>>(result.outputs);
            std::ranges::for_each(pendingJobs, [&](Job& j) {
                auto indBlockerIt =
                    std::find(j.independentBlockers.begin(), j.independentBlockers.end(), jobId);
                if (indBlockerIt != j.independentBlockers.end())
                {
                    j.independentBlockers.erase(indBlockerIt);
                }
                auto relBlockerIt =
                    std::find(j.relevantBlockers.begin(), j.relevantBlockers.end(), jobId);
                if (relBlockerIt != j.relevantBlockers.end())
                {
                    j.relevantBlockers.erase(relBlockerIt);
                    std::move(outputs.begin(), outputs.end(), std::back_inserter(j.inputs));
                }
            });
        }
        // If the job returned child jobs, then add each child job to pendingJobs.
        // Then, remove the parent ID from any blocked jobs but add the child job IDs
        // to the corresponding blockers list.
        else
        {
            auto childJobs = std::get<std::vector<Job>>(result.outputs);
            std::vector<int64_t> childJobIds(childJobs.size());
            std::transform(childJobs.begin(), childJobs.end(), childJobIds.begin(),
                          [&](Job j) { return addAndRegisterNewJob(j, paused); });
            std::ranges::for_each(pendingJobs, [&](Job& j) {
                auto indBlockerIt =
                    std::find(j.independentBlockers.begin(), j.independentBlockers.end(), jobId);
                if (indBlockerIt != j.independentBlockers.end())
                {
                    j.independentBlockers.erase(indBlockerIt);
                    std::copy(childJobIds.begin(), childJobIds.end(),
                             std::back_inserter(j.independentBlockers));
                }
                auto relBlockerIt =
                    std::find(j.relevantBlockers.begin(), j.relevantBlockers.end(), jobId);
                if (relBlockerIt != j.relevantBlockers.end())
                {
                    j.relevantBlockers.erase(relBlockerIt);
                    std::copy(childJobIds.begin(), childJobIds.end(),
                             std::back_inserter(j.relevantBlockers));
                }
            });
        }
    }

    // Update status of any jobs that became unblocked
    for (auto& job : pendingJobs)
    {
        if (job.numBlockers() == 0 &&
            job.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_BLOCKED)
        {
            job.status = paused ? aapis::orchestrator::v1::JobStatus::JOB_STATUS_PAUSED
                                : aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED;
        }
    }
}

std::vector<Job> Store::query(const JobQuery& query) const
{
    std::vector<Job> queryResult;

    switch (query.type)
    {
    case JobQuery::Type::GET_ALL_QUEUED:
        std::copy(pendingJobs.begin(), pendingJobs.end(), std::back_inserter(queryResult));
        break;

    case JobQuery::Type::GET_BY_PRIORITY:
        std::copy_if(pendingJobs.begin(), pendingJobs.end(), std::back_inserter(queryResult),
                    [&](const Job& j) { return j.priority == query.priority; });
        break;

    case JobQuery::Type::GET_BY_ID:
        std::copy_if(pendingJobs.begin(), pendingJobs.end(), std::back_inserter(queryResult),
                    [&](const Job& j) { return j.id == query.id; });
        break;
    }

    return queryResult;
}

QueueSnapshot Store::createSnapshot() const
{
    QueueSnapshot snapshot;
    snapshot.pending_jobs = pendingJobs;

    // Collect active job IDs
    for (const auto& [id, _] : activeJobIds)
    {
        snapshot.active_job_ids.push_back(id);
    }

    snapshot.snapshot_time_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    // TODO: Set boot_id from system

    return snapshot;
}

void Store::restoreFromSnapshot(const QueueSnapshot& snapshot)
{
    pendingJobs = snapshot.pending_jobs;
    sortJobs();

    // Restore active job IDs
    activeJobIds.clear();
    for (int64_t id : snapshot.active_job_ids)
    {
        activeJobIds[id] = true;
    }

    // Identify in-progress jobs that need to be re-executed
    pendingInitExecs.clear();
    for (const auto& job : pendingJobs)
    {
        if (job.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_ACTIVE)
        {
            pendingInitExecs.push_back(job);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// FSM State Implementations
// ══════════════════════════════════════════════════════════════════════════════

size_t InitState::step([[maybe_unused]] Store& s, [[maybe_unused]] Ports& p, [[maybe_unused]] const Container& c,
                       [[maybe_unused]] const ::services::LogicalTag& tag,
                       [[maybe_unused]] const ::services::StepTrigger& trigger)
{
    // Request snapshot load from database via logical action
    // The database will respond by writing to load_snapshot_in port
    // which will trigger "on_port_database_to_queue_restore" action

    SPDLOG_INFO("JobQueue entering InitState - requesting snapshot load");

    // Transition to wait state immediately
    // Snapshot will arrive asynchronously
    return InitWaitState::index();
}

size_t InitWaitState::step(Store& s, Ports& p, [[maybe_unused]] const Container& c,
                           [[maybe_unused]] const ::services::LogicalTag& tag,
                           const ::services::StepTrigger& trigger)
{
    // Only handle logical action for snapshot load
    if (trigger.type == ::services::StepTrigger::Type::LOGICAL_ACTION &&
        trigger.action_name == "on_port_database_to_queue_restore")
    {
        if (p.load_snapshot_in.is_present())
        {
            SPDLOG_INFO("JobQueue snapshot loaded, restoring state");

            auto snapshot = p.load_snapshot_in.get();
            s.restoreFromSnapshot(snapshot);

            // If there are in-progress jobs to re-execute, go to final init state
            if (!s.pendingInitExecs.empty())
            {
                SPDLOG_INFO("JobQueue has {} in-progress jobs to re-execute",
                        s.pendingInitExecs.size());
                return InitFinalWaitState::index();
            }

            // Otherwise, go directly to running
            SPDLOG_INFO("JobQueue transitioning to Running state");
            return RunningState::index();
        }
    }

    // Stay in wait state
    return InitWaitState::index();
}

size_t InitFinalWaitState::step(Store& s, Ports& p, [[maybe_unused]] const Container& c,
                                [[maybe_unused]] const ::services::LogicalTag& tag,
                                [[maybe_unused]] const ::services::StepTrigger& trigger)
{
    // Re-submit in-progress jobs to executor
    for (auto& job : s.pendingInitExecs)
    {
        SPDLOG_INFO("Re-submitting job {} to executor", job.id);
        p.execute_job_out.set(job);
        s.activeJobIds[job.id] = true;
    }

    s.pendingInitExecs.clear();

    SPDLOG_INFO("JobQueue transitioning to Running state");
    return RunningState::index();
}

size_t RunningState::step(Store& s, Ports& p, [[maybe_unused]] const Container& c,
                          [[maybe_unused]] const ::services::LogicalTag& tag,
                          const ::services::StepTrigger& trigger)
{
    (void)c;
    (void)tag;

    // DEBUG: Log all triggers
    if (trigger.type == ::services::StepTrigger::Type::HEARTBEAT)
    {
        std::cout << "DEBUG: JobQueue RunningState HEARTBEAT" << std::endl;
    }

    // Handle all logical actions based on StepTrigger
    if (trigger.type == ::services::StepTrigger::Type::LOGICAL_ACTION)
    {
        std::cout << "DEBUG: JobQueue ReadyState logical action: " << trigger.action_name << std::endl;
        if (trigger.action_name == "on_port_job_server_to_queue_new_job")
        {
            std::cout << "DEBUG: JobQueue handling on_new_job_in, port is_present=" << p.new_job_in.is_present() << std::endl;
            // Handle new job submission
            if (!p.new_job_in.is_present())
                return RunningState::index();

            auto job = p.new_job_in.get();
            std::cout << "DEBUG: JobQueue received new job with priority " << job.priority << std::endl;

            SPDLOG_INFO("JobQueue received new job with priority {}", job.priority);

            // Register job and assign ID (not paused in running state)
            int64_t id = s.addAndRegisterNewJob(job, false);

            // Send response
            p.new_job_id_out.set(id);

            SPDLOG_INFO("JobQueue assigned ID {} to new job", id);

            // If job is ready (no blockers), try to execute immediately
            if (job.numBlockers() == 0)
            {
                // Drain ready jobs inline
                for (auto& j : s.pendingJobs)
                {
                    if (j.numBlockers() == 0 &&
                        j.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED)
                    {
                        SPDLOG_INFO("JobQueue sending job {} to executor", j.id);
                        p.execute_job_out.set(j);
                        j.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ACTIVE;
                        s.activeJobIds[j.id] = true;
                    }
                }
            }
        }
        else if (trigger.action_name == "on_port_executor_to_queue_result")
        {
            // Handle job completion result
            if (!p.job_result_in.is_present())
                return RunningState::index();

            auto result = p.job_result_in.get();

            SPDLOG_INFO("JobQueue received result for job {}", result.job_id);

            // Process the result (unblock dependent jobs, handle outputs/children)
            s.processJobResult(result, false);

            // Re-sort queue after dependency changes
            s.sortJobs();

            // Drain ready jobs inline
            for (auto& j : s.pendingJobs)
            {
                if (j.numBlockers() == 0 &&
                    j.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED)
                {
                    SPDLOG_INFO("JobQueue sending job {} to executor", j.id);
                    p.execute_job_out.set(j);
                    j.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ACTIVE;
                    s.activeJobIds[j.id] = true;
                }
            }
        }
        else if (trigger.action_name == "on_port_job_server_to_queue_query")
        {
            // Handle query request
            if (!p.query_request_in.is_present())
                return RunningState::index();

            auto query = p.query_request_in.get();

            SPDLOG_DEBUG("JobQueue processing query");

            // Execute query
            auto jobs = s.query(query);

            // Send response
            JobQueryResponse response;
            response.success = true;
            response.jobs = jobs;
            p.query_response_out.set(response);
        }
        else if (trigger.action_name == "on_port_job_server_to_queue_control")
        {
            // Handle control command
            if (!p.control_request_in.is_present())
                return RunningState::index();

            auto ctrl = p.control_request_in.get();

            ControlResponse response;
            response.success = true;

            switch (ctrl.command)
            {
            case ControlRequest::Command::PAUSE:
                SPDLOG_INFO("JobQueue pausing all jobs");
                s.pauseJobs();
                response.message = "Jobs paused";
                p.control_response_out.set(response);
                return PausedState::index();

            case ControlRequest::Command::RESUME:
                SPDLOG_INFO("JobQueue already running");
                response.message = "Jobs already running";
                p.control_response_out.set(response);
                break;

            case ControlRequest::Command::CANCEL:
                SPDLOG_INFO("JobQueue canceling job {}", ctrl.target_job_id);
                // Find and cancel the job
                for (auto& job : s.pendingJobs)
                {
                    if (job.id == ctrl.target_job_id)
                    {
                        job.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED;
                        response.message = "Job canceled";
                        break;
                    }
                }
                p.control_response_out.set(response);
                break;
            }
        }
        else if (trigger.action_name == "try_execute_jobs")
        {
            // Drain ready jobs to executor
            // Send all ready jobs (no blockers, queued status) to executor
            for (auto& job : s.pendingJobs)
            {
                if (job.numBlockers() == 0 &&
                    job.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED)
                {
                    SPDLOG_INFO("JobQueue sending job {} to executor", job.id);

                    p.execute_job_out.set(job);
                    job.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ACTIVE;
                    s.activeJobIds[job.id] = true;
                }
            }
        }
    }

    // Handle heartbeat: dispatch ready jobs to executor
    if (trigger.type == ::services::StepTrigger::Type::HEARTBEAT)
    {
        // Check for jobs that are ready to execute (no blockers)
        for (auto& job : s.pendingJobs)
        {
            if (job.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED &&
                s.activeJobIds.find(job.id) == s.activeJobIds.end())
            {
                // Check if all blockers are complete
                bool ready = true;
                for (int64_t blocker_id : job.independentBlockers)
                {
                    auto blocker_it = std::find_if(s.pendingJobs.begin(), s.pendingJobs.end(),
                                                   [blocker_id](const Job& j) { return j.id == blocker_id; });
                    if (blocker_it != s.pendingJobs.end() &&
                        blocker_it->status != aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE)
                    {
                        ready = false;
                        break;
                    }
                }
                for (int64_t blocker_id : job.relevantBlockers)
                {
                    auto blocker_it = std::find_if(s.pendingJobs.begin(), s.pendingJobs.end(),
                                                   [blocker_id](const Job& j) { return j.id == blocker_id; });
                    if (blocker_it != s.pendingJobs.end() &&
                        blocker_it->status != aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE)
                    {
                        ready = false;
                        break;
                    }
                }

                if (ready)
                {
                    SPDLOG_INFO("JobQueue dispatching job {} to executor (heartbeat)", job.id);
                    std::cout << "DEBUG: JobQueue dispatching job " << job.id << " to executor" << std::endl;
                    p.execute_job_out.set(job);
                    job.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ACTIVE;
                    s.activeJobIds[job.id] = true;
                }
            }
        }
    }

    return RunningState::index();
}

size_t PausedState::step(Store& s, Ports& p, [[maybe_unused]] const Container& c,
                         [[maybe_unused]] const ::services::LogicalTag& tag,
                         const ::services::StepTrigger& trigger)
{
    (void)c;
    (void)tag;
    // Handle all logical actions based on StepTrigger
    if (trigger.type == ::services::StepTrigger::Type::LOGICAL_ACTION)
    {
        if (trigger.action_name == "on_new_job_in")
        {
            // Handle new job submission (paused)
            if (!p.new_job_in.is_present())
                return PausedState::index();

            auto job = p.new_job_in.get();

            SPDLOG_INFO("JobQueue received new job with priority {} (paused)", job.priority);

            // Register job and assign ID (paused state)
            int64_t id = s.addAndRegisterNewJob(job, true);

            // Send response
            p.new_job_id_out.set(id);

            SPDLOG_INFO("JobQueue assigned ID {} to new job (paused)", id);

            // Don't execute jobs in paused state
        }
        else if (trigger.action_name == "on_port_executor_to_queue_result")
        {
            // Handle job completion result
            if (!p.job_result_in.is_present())
                return PausedState::index();

            auto result = p.job_result_in.get();

            SPDLOG_INFO("JobQueue received result for job {} (paused)", result.job_id);

            // Process the result (unblock dependent jobs, handle outputs/children)
            s.processJobResult(result, true);

            // Re-sort queue after dependency changes
            s.sortJobs();

            // Don't try to execute jobs in paused state
        }
        else if (trigger.action_name == "on_port_job_server_to_queue_query")
        {
            // Handle query request
            if (!p.query_request_in.is_present())
                return PausedState::index();

            auto query = p.query_request_in.get();

            SPDLOG_DEBUG("JobQueue processing query (paused)");

            // Execute query
            auto jobs = s.query(query);

            // Send response
            JobQueryResponse response;
            response.success = true;
            response.jobs = jobs;
            p.query_response_out.set(response);
        }
        else if (trigger.action_name == "on_port_job_server_to_queue_control")
        {
            // Handle control command
            if (!p.control_request_in.is_present())
                return PausedState::index();

            auto ctrl = p.control_request_in.get();

            ControlResponse response;
            response.success = true;

            switch (ctrl.command)
            {
            case ControlRequest::Command::PAUSE:
                SPDLOG_INFO("JobQueue already paused");
                response.message = "Jobs already paused";
                p.control_response_out.set(response);
                break;

            case ControlRequest::Command::RESUME:
                SPDLOG_INFO("JobQueue resuming all jobs");
                s.unpauseJobs();
                response.message = "Jobs resumed";
                p.control_response_out.set(response);

                // Drain ready jobs inline before transitioning to running state
                for (auto& j : s.pendingJobs)
                {
                    if (j.numBlockers() == 0 &&
                        j.status == aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED)
                    {
                        SPDLOG_INFO("JobQueue sending job {} to executor", j.id);
                        p.execute_job_out.set(j);
                        j.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ACTIVE;
                        s.activeJobIds[j.id] = true;
                    }
                }

                return RunningState::index();

            case ControlRequest::Command::CANCEL:
                SPDLOG_INFO("JobQueue canceling job {}", ctrl.target_job_id);
                // Find and cancel the job
                for (auto& job : s.pendingJobs)
                {
                    if (job.id == ctrl.target_job_id)
                    {
                        job.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED;
                        response.message = "Job canceled";
                        break;
                    }
                }
                p.control_response_out.set(response);
                break;
            }
        }
        // Note: "try_execute_jobs" not handled in paused state - jobs should not be executed
    }

    return PausedState::index();
}

// ══════════════════════════════════════════════════════════════════════════════
// JobQueue Reactor Implementation
// ══════════════════════════════════════════════════════════════════════════════

void JobQueue::doPeriodicMaintenance(const ::services::LogicalTag& tag)
{
    // Save snapshot every 60 seconds
    auto time_s = tag.time.count() / 1'000'000'000;
    if (time_s % 60 == 0 && time_s > 0)
    {
        auto snapshot = getStore().createSnapshot();
        getPorts().save_snapshot_out.set(snapshot);
    }
}

bool JobQueue::isPaused() const
{
    return Base::getCurrentState() == PausedState::index();
}

} // namespace job_queue

} // namespace orchestrator
