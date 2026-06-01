#include "orchestrator/JobExecutor.h"

#include <algorithm>
#include <sstream>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

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

    // Create pipe for capturing stdout/stderr
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1)
    {
        SPDLOG_ERROR("Failed to create pipe for job {}: {}", job.id, strerror(errno));
        return false;
    }

    std::cout << "DEBUG: About to fork() for job " << job.id << std::endl;
    // Fork subprocess
    pid_t pid = fork();
    std::cout << "DEBUG: fork() returned pid=" << pid << " for job " << job.id << std::endl;

    std::cout << "DEBUG: Checking fork result, pid=" << pid << std::endl;
    if (pid == -1)
    {
        // Fork failed
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        SPDLOG_ERROR("Fork failed for job {}: {}", job.id, strerror(errno));
        return false;
    }
    else if (pid == 0)
    {
        // Child process: redirect stdin to /dev/null
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        // Redirect stdout and stderr to the write end of the pipe
        close(pipe_fd[0]);  // Close read end in child
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);

        // Child process: exec the script directly using sh with explicit path
        const char* shell = "/bin/sh";
        execlp(shell, "sh", "-c", substituted_script.c_str(), (char*)nullptr);

        // If execlp returns, it failed
        _exit(127);  // Standard exit code for exec failure
    }

    // Parent process: close write end of pipe and set read end to non-blocking
    close(pipe_fd[1]);

    // Set pipe read end to non-blocking mode
    int flags = fcntl(pipe_fd[0], F_GETFL, 0);
    fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);

    // Parent process: track job
    WorkerThread worker;
    worker.job_id = job.id;
    worker.pid = pid;
    worker.pipe_fd = pipe_fd[0];  // Store pipe read end
    worker.start_time = std::chrono::steady_clock::now();
    worker.timeout_seconds = job.timeoutSeconds;
    worker.script = job.script;  // Store original for reference
    worker.completed = false;
    worker.exit_code = -1;

    // Store job metadata for history tracking
    worker.job_type = job.job_type;
    worker.priority = job.priority;
    worker.spawn_time_seconds = job.spawnTimeSeconds;

    active_jobs[job.id] = worker;

    std::cout << "DEBUG: Job " << job.id << " started with PID " << pid << " (timeout: " << worker.timeout_seconds << "s)" << std::endl;
    SPDLOG_INFO("Job {} started with PID {} (timeout: {}s, pipe_fd: {})",
                job.id, pid, worker.timeout_seconds, pipe_fd[0]);

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

        // Read available output from pipe (non-blocking)
        if (worker.pipe_fd >= 0)
        {
            char buffer[4096];
            ssize_t bytes_read;

            while ((bytes_read = read(worker.pipe_fd, buffer, sizeof(buffer) - 1)) > 0)
            {
                buffer[bytes_read] = '\0';
                worker.output_buffer.append(buffer, bytes_read);
            }

            // Process complete lines from buffer
            size_t pos;
            while ((pos = worker.output_buffer.find('\n')) != std::string::npos)
            {
                std::string line = worker.output_buffer.substr(0, pos);
                worker.outputs.push_back(line);
                worker.output_buffer.erase(0, pos + 1);
            }
        }

        // Check for timeout (physical time based)
        if (worker.timeout_seconds > 0)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - worker.start_time).count();

            if (elapsed >= worker.timeout_seconds)
            {
                // Job has timed out
                SPDLOG_WARN("Job {} timed out after {}s (limit: {}s) - killing",
                           job_id, elapsed, worker.timeout_seconds);

                // Kill the process
                kill(worker.pid, SIGKILL);
                waitpid(worker.pid, nullptr, 0);  // Clean up zombie

                // Close pipe and capture any remaining output
                if (worker.pipe_fd >= 0)
                {
                    // Add any remaining buffer content as final line
                    if (!worker.output_buffer.empty())
                    {
                        worker.outputs.push_back(worker.output_buffer);
                        worker.output_buffer.clear();
                    }
                    close(worker.pipe_fd);
                    worker.pipe_fd = -1;
                }

                worker.completed = true;
                worker.exit_code = -1;
                worker.timed_out = true;

                completed.push_back(worker);
                continue;  // Skip waitpid check since we already handled it
            }
        }

        int status;
        pid_t result = waitpid(worker.pid, &status, WNOHANG);

        // Debug logging for specific job ranges
        if (job_id % 1000 == 3)  // Jobs ending in 003
        {
            fprintf(stderr, "DEBUG POLL[%ld]: waitpid(%d) returned %d\n", job_id, worker.pid, result);
            fflush(stderr);
        }

        if (result == worker.pid)
        {
            // Job completed - read any final output from pipe
            if (worker.pipe_fd >= 0)
            {
                char buffer[4096];
                ssize_t bytes_read;

                // Final read to capture any remaining output
                while ((bytes_read = read(worker.pipe_fd, buffer, sizeof(buffer) - 1)) > 0)
                {
                    buffer[bytes_read] = '\0';
                    worker.output_buffer.append(buffer, bytes_read);
                }

                // Process complete lines
                size_t pos;
                while ((pos = worker.output_buffer.find('\n')) != std::string::npos)
                {
                    std::string line = worker.output_buffer.substr(0, pos);
                    worker.outputs.push_back(line);
                    worker.output_buffer.erase(0, pos + 1);
                }

                // Add any remaining buffer content as final line
                if (!worker.output_buffer.empty())
                {
                    worker.outputs.push_back(worker.output_buffer);
                    worker.output_buffer.clear();
                }

                close(worker.pipe_fd);
                worker.pipe_fd = -1;
            }

            worker.completed = true;

            if (WIFEXITED(status))
            {
                worker.exit_code = WEXITSTATUS(status);
                SPDLOG_INFO("Job {} completed with exit code {} ({} output lines)",
                           job_id, worker.exit_code, worker.outputs.size());
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

    // Close pipe
    if (worker.pipe_fd >= 0)
    {
        close(worker.pipe_fd);
        worker.pipe_fd = -1;
    }

    // Mark as completed (will be cleaned up later)
    worker.completed = true;
    worker.exit_code = -1;

    return true;
}

std::string Store::substituteVariables(const std::string& script, const Job& job)
{
    std::cout << "DEBUG: substituteVariables input script: " << script << std::endl;
    std::cout << "DEBUG: job.inputs.size() = " << job.inputs.size() << std::endl;
    for (size_t i = 0; i < job.inputs.size(); ++i)
    {
        std::cout << "DEBUG: job.inputs[" << i << "] = " << job.inputs[i] << std::endl;
    }

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

    // Substitute individual input arguments: {input_0}, {input_1}, etc.
    // Match pattern: {input_N} where N is a digit
    for (size_t i = 0; i < job.inputs.size(); ++i)
    {
        std::string placeholder = "{input_" + std::to_string(i) + "}";
        pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos)
        {
            std::cout << "DEBUG: Replacing " << placeholder << " with " << job.inputs[i] << std::endl;
            result.replace(pos, placeholder.length(), job.inputs[i]);
            pos += job.inputs[i].length();
        }
    }

    std::cout << "DEBUG: substituteVariables output script: " << result << std::endl;

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

                    // Populate metadata for history tracking
                    result.job_type = job.job_type;
                    result.priority = job.priority;
                    result.submitted_at = job.spawnTimeSeconds;
                    result.completed_at = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
                    result.exec_duration_secs = 0.0;  // Never started

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
                            // Get worker metadata before removing
                            auto it = s.active_jobs.find(control.target_job_id);

                            // Report cancellation
                            orchestrator::job_queue::JobResult result;
                            result.job_id = control.target_job_id;
                            result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED;
                            result.outputs = std::vector<std::string>{"Job cancelled by user"};

                            // Populate metadata for history tracking
                            if (it != s.active_jobs.end()) {
                                const WorkerThread& worker = it->second;
                                result.job_type = worker.job_type;
                                result.priority = worker.priority;
                                result.submitted_at = worker.spawn_time_seconds;
                                result.completed_at = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
                                auto elapsed = std::chrono::steady_clock::now() - worker.start_time;
                                result.exec_duration_secs = std::chrono::duration<double>(elapsed).count();
                            }

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

                // Populate metadata for history tracking
                result.job_type = worker.job_type;
                result.priority = worker.priority;
                result.submitted_at = worker.spawn_time_seconds;
                result.completed_at = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
                auto elapsed = std::chrono::steady_clock::now() - worker.start_time;
                result.exec_duration_secs = std::chrono::duration<double>(elapsed).count();

                // Check if job timed out
                if (worker.timed_out)
                {
                    result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
                    result.outputs = std::vector<std::string>{
                        "Job timed out after " + std::to_string(worker.timeout_seconds) + " seconds"
                    };
                }
                // Determine status from exit code
                else if (worker.exit_code == 0)
                {
                    result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE;
                    // For now, outputs are just the exit code message
                    if (worker.outputs.empty())
                    {
                        result.outputs = std::vector<std::string>{"Job completed successfully"};
                    }
                    else
                    {
                        result.outputs = worker.outputs;
                    }
                }
                else
                {
                    result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
                    // For now, outputs are just the exit code message
                    if (worker.outputs.empty())
                    {
                        result.outputs = std::vector<std::string>{
                            "Job exited with code " + std::to_string(worker.exit_code)
                        };
                    }
                    else
                    {
                        result.outputs = worker.outputs;
                    }
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
        // Note: Timeout handling now happens in pollCompletedJobs() based on physical time
        // This old logical-time-based timeout handler is no longer used
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

                // Populate metadata for history tracking
                result.job_type = job.job_type;
                result.priority = job.priority;
                result.submitted_at = job.spawnTimeSeconds;
                result.completed_at = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
                result.exec_duration_secs = 0.0;  // Never started

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
                            // Get worker metadata before removing
                            auto it = s.active_jobs.find(control.target_job_id);

                            orchestrator::job_queue::JobResult result;
                            result.job_id = control.target_job_id;
                            result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED;
                            result.outputs = std::vector<std::string>{"Job cancelled by user"};

                            // Populate metadata for history tracking
                            if (it != s.active_jobs.end()) {
                                const WorkerThread& worker = it->second;
                                result.job_type = worker.job_type;
                                result.priority = worker.priority;
                                result.submitted_at = worker.spawn_time_seconds;
                                result.completed_at = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
                                auto elapsed = std::chrono::steady_clock::now() - worker.start_time;
                                result.exec_duration_secs = std::chrono::duration<double>(elapsed).count();
                            }

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

                // Populate metadata for history tracking
                result.job_type = worker.job_type;
                result.priority = worker.priority;
                result.submitted_at = worker.spawn_time_seconds;
                result.completed_at = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
                auto elapsed = std::chrono::steady_clock::now() - worker.start_time;
                result.exec_duration_secs = std::chrono::duration<double>(elapsed).count();

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

                const WorkerThread& worker = it->second;

                orchestrator::job_queue::JobResult result;
                result.job_id = job_id;
                result.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR;
                result.outputs = std::vector<std::string>{
                    "Job timed out after " + std::to_string(worker.timeout_seconds) + " seconds"
                };

                // Populate metadata for history tracking
                result.job_type = worker.job_type;
                result.priority = worker.priority;
                result.submitted_at = worker.spawn_time_seconds;
                result.completed_at = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
                auto elapsed = std::chrono::steady_clock::now() - worker.start_time;
                result.exec_duration_secs = std::chrono::duration<double>(elapsed).count();

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

    // Note: Timeout checking is now done in pollCompletedJobs() using physical time
    // This is more reliable than logical-time-based scheduling
}

} // namespace job_executor

} // namespace orchestrator
