#include "orchestrator/JobExecutor.h"

#include <algorithm>
#include <sstream>
#include <signal.h>
#include <sys/wait.h>

namespace orchestrator
{

namespace job_executor
{

// ══════════════════════════════════════════════════════════════════════════════
// Store Implementation - Pure Business Logic
// ══════════════════════════════════════════════════════════════════════════════

bool Store::submitJob(const Job& job)
{
    std::cout << "DEBUG: Store::submitJob called for job " << job.id << std::endl;

    // Check thread availability
    if (!hasAvailableThread())
    {
        std::cout << "DEBUG: No threads available for job " << job.id << std::endl;
        SPDLOG_ERROR("Cannot submit job {}: no threads available ({}/{})",
                     job.id, active_jobs.size(), max_threads);
        return false;
    }

    std::cout << "DEBUG: Thread available, substituting variables for job " << job.id << std::endl;

    // Perform bash variable substitution
    std::string substituted_script = substituteVariables(job.script, job);

    std::cout << "DEBUG: Submitting job " << job.id << " with script: " << substituted_script << std::endl;
    SPDLOG_DEBUG("Submitting job {} with script: {}", job.id, substituted_script);

    // Fork subprocess
    pid_t pid = fork();

    if (pid == -1)
    {
        // Fork failed
        SPDLOG_ERROR("Fork failed for job {}: {}", job.id, strerror(errno));
        return false;
    }
    else if (pid == 0)
    {
        // Child process: exec bash (uses PATH to find bash)
        execlp("bash", "bash", "-c", substituted_script.c_str(), nullptr);

        // If execlp returns, it failed
        SPDLOG_ERROR("Exec failed for job {}: {}", job.id, strerror(errno));
        _exit(127);  // Standard exit code for exec failure
    }

    // Parent process: track job
    WorkerThread worker;
    worker.job_id = job.id;
    worker.pid = pid;
    worker.start_time = std::chrono::steady_clock::now();
    worker.timeout_seconds = job.timeoutSeconds;
    worker.script = job.script;  // Store original for reference
    worker.completed = false;
    worker.exit_code = -1;

    active_jobs[job.id] = worker;

    SPDLOG_INFO("Job {} started with PID {} (timeout: {}s)",
                job.id, pid, worker.timeout_seconds);

    return true;
}

std::vector<WorkerThread> Store::pollCompletedJobs()
{
    std::vector<WorkerThread> completed;

    for (auto& [job_id, worker] : active_jobs)
    {
        if (worker.completed)
        {
            continue;  // Already marked complete, skip
        }

        int status;
        pid_t result = waitpid(worker.pid, &status, WNOHANG);

        if (result == worker.pid)
        {
            // Job completed
            worker.completed = true;

            if (WIFEXITED(status))
            {
                worker.exit_code = WEXITSTATUS(status);
                SPDLOG_INFO("Job {} completed with exit code {}", job_id, worker.exit_code);
            }
            else if (WIFSIGNALED(status))
            {
                worker.exit_code = -1;
                SPDLOG_WARN("Job {} terminated by signal {}", job_id, WTERMSIG(status));
            }
            else
            {
                worker.exit_code = -1;
                SPDLOG_WARN("Job {} exited abnormally", job_id);
            }

            // TODO: Capture stdout/stderr from pipe (Stage 8 enhancement)

            completed.push_back(worker);
        }
        else if (result == -1)
        {
            SPDLOG_ERROR("waitpid failed for job {}: {}", job_id, strerror(errno));
        }
        // result == 0 means still running
    }

    return completed;
}

bool Store::cancelJob(int64_t job_id)
{
    auto it = active_jobs.find(job_id);
    if (it == active_jobs.end())
    {
        SPDLOG_WARN("Cannot cancel job {}: not found in active jobs", job_id);
        return false;
    }

    WorkerThread& worker = it->second;

    if (worker.completed)
    {
        SPDLOG_DEBUG("Job {} already completed, skipping cancel", job_id);
        return false;
    }

    // Kill subprocess
    SPDLOG_INFO("Killing job {} (PID {})", job_id, worker.pid);

    if (kill(worker.pid, SIGKILL) == -1)
    {
        SPDLOG_ERROR("Failed to kill job {}: {}", job_id, strerror(errno));
        return false;
    }

    // Reap zombie process
    int status;
    waitpid(worker.pid, &status, 0);

    // Mark as completed (will be cleaned up later)
    worker.completed = true;
    worker.exit_code = -1;

    return true;
}

std::string Store::substituteVariables(const std::string& script, const Job& job)
{
    std::string result = script;

    // Build INPUT_IDS array from independentBlockers + relevantBlockers
    std::ostringstream ids_stream;
    ids_stream << "(";

    bool first = true;
    for (int64_t id : job.independentBlockers)
    {
        if (!first) ids_stream << " ";
        ids_stream << id;
        first = false;
    }
    for (int64_t id : job.relevantBlockers)
    {
        if (!first) ids_stream << " ";
        ids_stream << id;
        first = false;
    }

    ids_stream << ")";
    std::string ids_array = ids_stream.str();

    // Build INPUT_ARGS array from inputs (with shell escaping)
    std::ostringstream args_stream;
    args_stream << "(";

    first = true;
    for (const std::string& input : job.inputs)
    {
        if (!first) args_stream << " ";
        args_stream << "\"" << shellEscape(input) << "\"";
        first = false;
    }

    args_stream << ")";
    std::string args_array = args_stream.str();

    // Perform substitution
    size_t pos = 0;
    while ((pos = result.find("$INPUT_IDS", pos)) != std::string::npos)
    {
        result.replace(pos, 10, ids_array);
        pos += ids_array.length();
    }

    pos = 0;
    while ((pos = result.find("$INPUT_ARGS", pos)) != std::string::npos)
    {
        result.replace(pos, 11, args_array);
        pos += args_array.length();
    }

    return result;
}

std::string Store::shellEscape(const std::string& str)
{
    std::string escaped;
    escaped.reserve(str.size() * 2);  // Pre-allocate for efficiency

    for (char c : str)
    {
        // Escape special bash characters
        if (c == '"' || c == '\\' || c == '$' || c == '`' || c == '!')
        {
            escaped += '\\';
        }
        escaped += c;
    }

    return escaped;
}

// ══════════════════════════════════════════════════════════════════════════════
// FSM State Implementations
// ══════════════════════════════════════════════════════════════════════════════

size_t InitState::step(Store& s, Ports& p, const Container& c,
                       const ::services::LogicalTag& tag,
                       const ::services::StepTrigger& trigger)
{
    (void)s;
    (void)p;
    (void)c;
    (void)tag;
    (void)trigger;

    SPDLOG_INFO("JobExecutor initializing - transitioning to Running state");
    return RunningState::index();
}

size_t RunningState::step(Store& s, Ports& p, [[maybe_unused]] const Container& c,
                          [[maybe_unused]] const ::services::LogicalTag& tag,
                          const ::services::StepTrigger& trigger)
{
    (void)c;
    (void)tag;

    // Handle logical actions (port-triggered events)
    if (trigger.type == ::services::StepTrigger::Type::LOGICAL_ACTION)
    {
        std::cout << "DEBUG: JobExecutor RunningState logical action: " << trigger.action_name << std::endl;

        // Handle new job submission
        // Connection name is "queue_to_executor_job" so action is "on_port_queue_to_executor_job"
        if (trigger.action_name == "on_port_queue_to_executor_job")
        {
            std::cout << "DEBUG: JobExecutor received job from JobQueue, job_in.is_present=" << p.job_in.is_present() << std::endl;
            if (p.job_in.is_present())
            {
                const Job& job = p.job_in.get();
                std::cout << "DEBUG: JobExecutor submitting job " << job.id << std::endl;

                // Try to submit job (Store handles thread availability check)
                bool submitted = s.submitJob(job);

                if (submitted)
                {
                    // Schedule timeout action if timeout is set
                    if (job.timeoutSeconds > 0)
                    {
                        // NOTE: Cannot call schedulePhysicalAction from FSM state
                        // This will be handled via output port pattern
                        SPDLOG_DEBUG("Job {} submitted, timeout scheduling handled by reactor",
                                    job.id);
                    }
                }
                else
                {
                    // Submission failed - report error
                    SPDLOG_ERROR("Failed to submit job {}", job.id);

                    orchestrator::job_queue::JobResult result;
                    result.job_id = job.id;
                    result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
                    result.outputs = std::vector<std::string>{
                        "Failed to start job: no threads available"
                    };

                    p.job_result_out.set(result);
                    p.job_history_out.set(result);
                }
            }
        }
        // Handle control commands
        else if (trigger.action_name == "on_port_control_in")
        {
            if (p.control_in.is_present())
            {
                const orchestrator::job_queue::ControlRequest& control = p.control_in.get();

                if (control.command == orchestrator::job_queue::ControlRequest::Command::PAUSE)
                {
                    SPDLOG_INFO("Pausing executor - will not accept new jobs");
                    return PausedState::index();
                }
                else if (control.command == orchestrator::job_queue::ControlRequest::Command::CANCEL)
                {
                    if (control.target_job_id >= 0)
                    {
                        SPDLOG_INFO("Cancelling job {}", control.target_job_id);
                        bool cancelled = s.cancelJob(control.target_job_id);

                        if (cancelled)
                        {
                            // Report cancellation
                            orchestrator::job_queue::JobResult result;
                            result.job_id = control.target_job_id;
                            result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED;
                            result.outputs = std::vector<std::string>{"Job cancelled by user"};

                            p.job_result_out.set(result);
                            p.job_history_out.set(result);

                            // Remove from active jobs
                            s.active_jobs.erase(control.target_job_id);
                        }
                    }
                    else
                    {
                        SPDLOG_WARN("Cancel command requires target_job_id >= 0");
                    }
                }
            }
        }
        // Handle polling completions
        else if (trigger.action_name == "poll_completions")
        {
            // Call Store business logic
            std::vector<WorkerThread> completed = s.pollCompletedJobs();

            // Report results for all completed jobs
            for (const WorkerThread& worker : completed)
            {
                orchestrator::job_queue::JobResult result;
                result.job_id = worker.job_id;

                // Determine status from exit code
                if (worker.exit_code == 0)
                {
                    result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE;
                }
                else
                {
                    result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
                }

                // For now, outputs are just the exit code message
                if (worker.outputs.empty())
                {
                    if (worker.exit_code == 0)
                    {
                        result.outputs = std::vector<std::string>{"Job completed successfully"};
                    }
                    else
                    {
                        result.outputs = std::vector<std::string>{
                            "Job exited with code " + std::to_string(worker.exit_code)
                        };
                    }
                }
                else
                {
                    result.outputs = worker.outputs;
                }

                p.job_result_out.set(result);
                p.job_history_out.set(result);

                // Remove from active jobs
                s.active_jobs.erase(worker.job_id);
            }

            if (!completed.empty())
            {
                SPDLOG_DEBUG("Polled {} completed jobs", completed.size());
            }

            // NOTE: Re-scheduling poll_completions will be handled by doPeriodicMaintenance()
            // We don't set polling_active here since it's managed at the reactor level
        }
        // Handle job timeout
        else if (trigger.action_name.substr(0, 8) == "timeout_")
        {
            // Parse job ID from action name
            int64_t job_id = std::stol(trigger.action_name.substr(8));

            auto it = s.active_jobs.find(job_id);

            if (it == s.active_jobs.end())
            {
                SPDLOG_DEBUG("Timeout for job {} but job not found (may have already completed)",
                            job_id);
            }
            else
            {
                WorkerThread& worker = it->second;

                if (worker.completed)
                {
                    SPDLOG_DEBUG("Timeout for job {} but job already completed", job_id);
                }
                else
                {
                    // Job still running - kill it
                    SPDLOG_WARN("Job {} timed out after {}s - killing",
                               job_id, worker.timeout_seconds);

                    bool killed = s.cancelJob(job_id);

                    if (killed)
                    {
                        // Report timeout error
                        orchestrator::job_queue::JobResult result;
                        result.job_id = job_id;
                        result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
                        result.outputs = std::vector<std::string>{
                            "Job timed out after " + std::to_string(worker.timeout_seconds) +
                            " seconds"
                        };

                        p.job_result_out.set(result);
                        p.job_history_out.set(result);

                        // Remove from active jobs
                        s.active_jobs.erase(job_id);
                    }
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

    // Handle logical actions
    if (trigger.type == ::services::StepTrigger::Type::LOGICAL_ACTION)
    {
        // Reject new jobs when paused
        if (trigger.action_name == "on_port_job_in")
        {
            if (p.job_in.is_present())
            {
                const Job& job = p.job_in.get();

                SPDLOG_INFO("Rejecting job {} - executor is paused", job.id);

                orchestrator::job_queue::JobResult result;
                result.job_id = job.id;
                result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
                result.outputs = std::vector<std::string>{"Executor is paused"};

                p.job_result_out.set(result);
                p.job_history_out.set(result);
            }
        }
        // Handle control commands
        else if (trigger.action_name == "on_port_control_in")
        {
            if (p.control_in.is_present())
            {
                const orchestrator::job_queue::ControlRequest& control = p.control_in.get();

                if (control.command == orchestrator::job_queue::ControlRequest::Command::RESUME)
                {
                    SPDLOG_INFO("Resuming executor - accepting new jobs");
                    return RunningState::index();
                }
                else if (control.command == orchestrator::job_queue::ControlRequest::Command::CANCEL)
                {
                    if (control.target_job_id >= 0)
                    {
                        SPDLOG_INFO("Cancelling job {}", control.target_job_id);
                        bool cancelled = s.cancelJob(control.target_job_id);

                        if (cancelled)
                        {
                            orchestrator::job_queue::JobResult result;
                            result.job_id = control.target_job_id;
                            result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED;
                            result.outputs = std::vector<std::string>{"Job cancelled by user"};

                            p.job_result_out.set(result);
                            p.job_history_out.set(result);

                            s.active_jobs.erase(control.target_job_id);
                        }
                    }
                }
            }
        }
        // Still poll for completions even when paused
        else if (trigger.action_name == "poll_completions")
        {
            std::vector<WorkerThread> completed = s.pollCompletedJobs();

            for (const WorkerThread& worker : completed)
            {
                orchestrator::job_queue::JobResult result;
                result.job_id = worker.job_id;

                if (worker.exit_code == 0)
                {
                    result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE;
                    result.outputs = std::vector<std::string>{"Job completed successfully"};
                }
                else
                {
                    result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
                    result.outputs = std::vector<std::string>{
                        "Job exited with code " + std::to_string(worker.exit_code)
                    };
                }

                p.job_result_out.set(result);
                p.job_history_out.set(result);

                s.active_jobs.erase(worker.job_id);
            }
        }
        // Handle timeouts even when paused
        else if (trigger.action_name.substr(0, 8) == "timeout_")
        {
            int64_t job_id = std::stol(trigger.action_name.substr(8));

            auto it = s.active_jobs.find(job_id);

            if (it != s.active_jobs.end() && !it->second.completed)
            {
                SPDLOG_WARN("Job {} timed out (paused state) - killing", job_id);

                s.cancelJob(job_id);

                orchestrator::job_queue::JobResult result;
                result.job_id = job_id;
                result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
                result.outputs = std::vector<std::string>{
                    "Job timed out after " + std::to_string(it->second.timeout_seconds) + " seconds"
                };

                p.job_result_out.set(result);
                p.job_history_out.set(result);

                s.active_jobs.erase(job_id);
            }
        }
    }

    return PausedState::index();
}

// ══════════════════════════════════════════════════════════════════════════════
// JobExecutor Reactor Implementation
// ══════════════════════════════════════════════════════════════════════════════

void JobExecutor::doPeriodicMaintenance(const ::services::LogicalTag& tag)
{
    (void)tag;

    // Initiate polling if jobs are active and polling not already scheduled
    if (getStore().hasActiveJobs() && !getStore().polling_active)
    {
        Base::scheduleLogicalAction("poll_completions");
        getStore().polling_active = true;
    }

    // Re-schedule polling if jobs still active
    if (getStore().polling_active && getStore().hasActiveJobs())
    {
        Base::schedulePhysicalAction(::services::LogicalTime{50'000'000}, "poll_completions");
    }
    else if (getStore().polling_active && !getStore().hasActiveJobs())
    {
        getStore().polling_active = false;
    }

    // Schedule timeout actions for newly submitted jobs
    // NOTE: This is a workaround since FSM states can't call schedulePhysicalAction
    // We check for jobs that don't have a timeout scheduled yet
    for (auto& [job_id, worker] : getStore().active_jobs)
    {
        if (worker.timeout_seconds > 0 && !worker.timeout_scheduled)
        {
            std::string timeout_action = "timeout_" + std::to_string(job_id);
            int64_t timeout_ns = worker.timeout_seconds * 1'000'000'000LL;
            Base::schedulePhysicalAction(::services::LogicalTime{timeout_ns}, timeout_action);

            worker.timeout_scheduled = true;
            SPDLOG_DEBUG("Scheduled timeout for job {} in {}s", job_id, worker.timeout_seconds);
        }
    }
}

} // namespace job_executor

} // namespace orchestrator
