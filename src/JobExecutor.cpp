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
    // Check thread availability
    if (!hasAvailableThread())
    {
        SPDLOG_ERROR("Cannot submit job {}: no threads available ({}/{})",
                     job.id, active_jobs.size(), max_threads);
        return false;
    }

    // Perform bash variable substitution
    std::string substituted_script = substituteVariables(job.script, job);

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

size_t InitState::step(Store& s, Ports& p, const Container& c)
{
    (void)s;  // Unused
    (void)p;  // Unused
    (void)c;  // Unused

    SPDLOG_INFO("JobExecutor initializing - transitioning to Running state");

    // No initialization needed for executor (JobQueue handles restart recovery)
    return RunningState::index();
}

size_t RunningState::step(Store& s, Ports& p, const Container& c)
{
    (void)s;  // Unused
    (void)p;  // Unused
    (void)c;  // Unused

    // State transitions happen via logical actions (control commands)
    // This step() is only called if FSM needs to re-evaluate state
    return RunningState::index();  // Stay in running
}

size_t PausedState::step(Store& s, Ports& p, const Container& c)
{
    (void)s;  // Unused
    (void)p;  // Unused
    (void)c;  // Unused

    // State transitions happen via logical actions (control commands)
    // This step() is only called if FSM needs to re-evaluate state
    return PausedState::index();  // Stay paused
}

// ══════════════════════════════════════════════════════════════════════════════
// JobExecutor Reactor Implementation
// ══════════════════════════════════════════════════════════════════════════════

void JobExecutor::initialize()
{
    SPDLOG_INFO("JobExecutor reactor initialized with {} max threads", mStore.max_threads);
    mCurrentState = InitState::index();

    // Transition to running immediately
    mCurrentState = InitState{}.step(mStore, mPorts, Container{});
}

void JobExecutor::doHeartbeat(const ::services::LogicalTag& tag)
{
    (void)tag;  // Unused

    // Initiate polling if jobs are active and polling not already scheduled
    if (mStore.hasActiveJobs() && !mStore.polling_active)
    {
        scheduleLogicalAction("poll_completions");
        mStore.polling_active = true;
    }
}

void JobExecutor::executeLogicalAction(const ::services::LogicalTag& tag,
                                       const std::string& action)
{
    (void)tag;  // Unused for now

    if (action == "on_port_job_in")
    {
        handleJobSubmission();
    }
    else if (action == "on_port_control_in")
    {
        handleControl();
    }
    else if (action == "poll_completions")
    {
        pollCompletions();

        // Re-schedule polling if jobs still active
        if (mStore.hasActiveJobs())
        {
            schedulePhysicalAction(::services::LogicalTime{50'000'000}, "poll_completions");  // 50ms
        }
        else
        {
            mStore.polling_active = false;
        }
    }
    else if (action.substr(0, 8) == "timeout_")
    {
        // Parse job ID from action name
        int64_t job_id = std::stol(action.substr(8));
        handleTimeout(job_id);
    }
    else
    {
        SPDLOG_WARN("Unknown logical action: {}", action);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Protected Port I/O Orchestration Methods
// ══════════════════════════════════════════════════════════════════════════════

void JobExecutor::handleJobSubmission()
{
    if (!mPorts.job_in.is_present())
    {
        SPDLOG_WARN("handleJobSubmission called but no job on port");
        return;
    }

    const Job& job = mPorts.job_in.get();

    // Check if paused - reject new jobs
    if (isPaused())
    {
        SPDLOG_INFO("Rejecting job {} - executor is paused", job.id);

        orchestrator::job_queue::JobResult result;
        result.job_id = job.id;
        result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
        result.outputs = std::vector<std::string>{"Executor is paused"};

        mPorts.job_result_out.set(result);
        mPorts.job_history_out.set(result);
        return;
    }

    // Try to submit job (Store handles thread availability check)
    bool submitted = mStore.submitJob(job);

    if (submitted)
    {
        // Schedule timeout action if timeout is set
        if (job.timeoutSeconds > 0)
        {
            std::string timeout_action = "timeout_" + std::to_string(job.id);
            int64_t timeout_ns = job.timeoutSeconds * 1'000'000'000LL;
            schedulePhysicalAction(::services::LogicalTime{timeout_ns}, timeout_action);

            SPDLOG_DEBUG("Scheduled timeout for job {} in {}s", job.id, job.timeoutSeconds);
        }
    }
    else
    {
        // Submission failed - report error
        SPDLOG_ERROR("Failed to submit job {}", job.id);

        orchestrator::job_queue::JobResult result;
        result.job_id = job.id;
        result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
        result.outputs = std::vector<std::string>{"Failed to start job: no threads available"};

        mPorts.job_result_out.set(result);
        mPorts.job_history_out.set(result);
    }
}

void JobExecutor::handleControl()
{
    if (!mPorts.control_in.is_present())
    {
        SPDLOG_WARN("handleControl called but no control on port");
        return;
    }

    const orchestrator::job_queue::ControlRequest& control = mPorts.control_in.get();

    switch (control.command)
    {
        case orchestrator::job_queue::ControlRequest::Command::PAUSE:
            SPDLOG_INFO("Pausing executor - will not accept new jobs");
            mCurrentState = PausedState::index();
            break;

        case orchestrator::job_queue::ControlRequest::Command::RESUME:
            SPDLOG_INFO("Resuming executor - accepting new jobs");
            mCurrentState = RunningState::index();
            break;

        case orchestrator::job_queue::ControlRequest::Command::CANCEL:
            if (control.target_job_id >= 0)
            {
                SPDLOG_INFO("Cancelling job {}", control.target_job_id);
                bool cancelled = mStore.cancelJob(control.target_job_id);

                if (cancelled)
                {
                    // Report cancellation
                    orchestrator::job_queue::JobResult result;
                    result.job_id = control.target_job_id;
                    result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED;
                    result.outputs = std::vector<std::string>{"Job cancelled by user"};

                    mPorts.job_result_out.set(result);
                    mPorts.job_history_out.set(result);

                    // Remove from active jobs
                    mStore.active_jobs.erase(control.target_job_id);
                }
            }
            else
            {
                SPDLOG_WARN("Cancel command requires target_job_id >= 0");
            }
            break;

        default:
            SPDLOG_WARN("Unknown control command");
            break;
    }
}

void JobExecutor::pollCompletions()
{
    // Call Store business logic
    std::vector<WorkerThread> completed = mStore.pollCompletedJobs();

    // Report results for all completed jobs
    for (const WorkerThread& worker : completed)
    {
        orchestrator::job_queue::JobResult result = createResult(worker);

        mPorts.job_result_out.set(result);
        mPorts.job_history_out.set(result);

        // Remove from active jobs
        mStore.active_jobs.erase(worker.job_id);
    }

    if (!completed.empty())
    {
        SPDLOG_DEBUG("Polled {} completed jobs", completed.size());
    }
}

void JobExecutor::handleTimeout(int64_t job_id)
{
    auto it = mStore.active_jobs.find(job_id);

    if (it == mStore.active_jobs.end())
    {
        SPDLOG_DEBUG("Timeout for job {} but job not found (may have already completed)", job_id);
        return;
    }

    WorkerThread& worker = it->second;

    if (worker.completed)
    {
        SPDLOG_DEBUG("Timeout for job {} but job already completed", job_id);
        return;
    }

    // Job still running - kill it
    SPDLOG_WARN("Job {} timed out after {}s - killing", job_id, worker.timeout_seconds);

    bool killed = mStore.cancelJob(job_id);

    if (killed)
    {
        // Report timeout error
        orchestrator::job_queue::JobResult result;
        result.job_id = job_id;
        result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
        result.outputs = std::vector<std::string>{
            "Job timed out after " + std::to_string(worker.timeout_seconds) + " seconds"
        };

        mPorts.job_result_out.set(result);
        mPorts.job_history_out.set(result);

        // Remove from active jobs
        mStore.active_jobs.erase(job_id);
    }
}

orchestrator::job_queue::JobResult JobExecutor::createResult(const WorkerThread& worker)
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
    // TODO: Capture actual stdout/stderr in Stage 8
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

    return result;
}

} // namespace job_executor

} // namespace orchestrator
