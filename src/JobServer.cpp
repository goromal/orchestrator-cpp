#include "orchestrator/JobServer.h"

namespace orchestrator
{

namespace job_server
{

// ══════════════════════════════════════════════════════════════════════════════
// FSM State Implementations
// ══════════════════════════════════════════════════════════════════════════════

size_t InitState::step(Store& s, Ports& p,
                       const ::services::MicroServiceContainer<>& c,
                       const ::services::LogicalTag& tag,
                       const ::services::StepTrigger& trigger)
{
    (void)c;
    (void)tag;
    [[maybe_unused]] Store& store = s;
    [[maybe_unused]] Ports& ports = p;

    if (trigger.type == ::services::StepTrigger::Type::HEARTBEAT) {
        SPDLOG_INFO("JobServer initialized successfully");
        return RunningState::index();
    }

    return InitState::index();
}

size_t RunningState::step(Store& s, Ports& p,
                          const ::services::MicroServiceContainer<>& c,
                          const ::services::LogicalTag& tag,
                          const ::services::StepTrigger& trigger)
{
    (void)c;
    (void)tag;

    if (trigger.type == ::services::StepTrigger::Type::LOGICAL_ACTION) {

        // ──────────────────────────────────────────────────────────────────
        // DefineJob RPC
        // ──────────────────────────────────────────────────────────────────
        if (trigger.action_name == "define_job_request") {
            if (p.define_job_request_in.is_present()) {
                auto request = p.define_job_request_in.get();
                aapis::orchestrator::v2::DefineJobResponse response;

                // Validate request
                auto validation = s.validateDefineJobRequest(request);
                if (!validation.is_valid) {
                    response.set_success(false);
                    response.set_message(validation.error_message);
                    SPDLOG_WARN("DefineJob validation failed: {}", validation.error_message);
                } else {
                    // Add job definition
                    JobDefinition def;
                    def.job_type = request.job_type();
                    def.job_definition = request.job_definition();
                    s.addJobDefinition(def);

                    response.set_success(true);
                    response.set_message("Job type defined successfully");
                    SPDLOG_INFO("Defined job type: {}", def.job_type);
                }

                s.requests_handled++;
                p.define_job_response_out.set(response);
            }
        }

        // ──────────────────────────────────────────────────────────────────
        // KickoffJob RPC
        // ──────────────────────────────────────────────────────────────────
        else if (trigger.action_name == "kickoff_job_request") {
            if (p.kickoff_job_request_in.is_present()) {
                auto request = p.kickoff_job_request_in.get();
                aapis::orchestrator::v2::KickoffJobResponse response;

                // Validate request
                auto validation = s.validateKickoffJobRequest(request);
                if (!validation.is_valid) {
                    response.set_success(false);
                    response.set_message(validation.error_message);
                    response.set_job_id(-1);
                    SPDLOG_WARN("KickoffJob validation failed: {}", validation.error_message);
                } else {
                    // Convert to internal Job structure
                    Job job = s.convertToJob(request);

                    // Send to JobQueue
                    p.new_job_out.set(job);

                    // Wait for job ID assignment (handled in next heartbeat)
                    // For now, return success with placeholder
                    response.set_success(true);
                    response.set_message("Job queued successfully");
                    response.set_job_id(0);  // Will be assigned by JobQueue
                    SPDLOG_INFO("Kicked off job type: {}", request.job_type());
                }

                s.requests_handled++;
                p.kickoff_job_response_out.set(response);
            }
        }

        // ──────────────────────────────────────────────────────────────────
        // JobStatus RPC
        // ──────────────────────────────────────────────────────────────────
        else if (trigger.action_name == "job_status_request") {
            if (p.job_status_request_in.is_present()) {
                auto request = p.job_status_request_in.get();

                // Create query for JobQueue
                job_queue::JobQuery query;
                query.type = job_queue::JobQuery::Type::GET_BY_ID;
                query.id = request.job_id();
                p.query_out.set(query);

                // Response will be handled when query_response_in is present
                // For now, send a placeholder response
                aapis::orchestrator::v2::JobStatusResponse response;
                response.set_status(aapis::orchestrator::v2::JobStatus::JOB_STATUS_UNSPECIFIED);
                response.set_message("Query sent to JobQueue");
                SPDLOG_INFO("Job status query for job_id: {}", request.job_id());

                s.requests_handled++;
                p.job_status_response_out.set(response);
            }
        }

        // ──────────────────────────────────────────────────────────────────
        // JobsSummaryStatus RPC
        // ──────────────────────────────────────────────────────────────────
        else if (trigger.action_name == "jobs_summary_request") {
            if (p.jobs_summary_request_in.is_present()) {
                [[maybe_unused]] auto request = p.jobs_summary_request_in.get();

                // Create query for JobQueue (get all queued jobs)
                job_queue::JobQuery query;
                query.type = job_queue::JobQuery::Type::GET_ALL_QUEUED;
                p.query_out.set(query);

                // Response will be aggregated from query response
                // For now, send a placeholder response
                aapis::orchestrator::v2::JobsSummaryStatusResponse response;
                response.set_num_queued_jobs(0);
                response.set_num_active_jobs(0);
                response.set_num_completed_jobs(0);
                SPDLOG_INFO("Jobs summary status query");

                s.requests_handled++;
                p.jobs_summary_response_out.set(response);
            }
        }

        // ──────────────────────────────────────────────────────────────────
        // PauseJobs RPC
        // ──────────────────────────────────────────────────────────────────
        else if (trigger.action_name == "pause_request") {
            if (p.pause_request_in.is_present()) {
                [[maybe_unused]] auto request = p.pause_request_in.get();

                // Send control command to JobQueue
                job_queue::ControlRequest control;
                control.command = job_queue::ControlRequest::Command::PAUSE;
                control.target_job_id = -1;  // All jobs
                p.control_out.set(control);

                aapis::orchestrator::v2::PauseJobsResponse response;
                response.set_success(true);
                response.set_message("Pause command sent to JobQueue");
                SPDLOG_INFO("Pause jobs command");

                s.requests_handled++;
                p.pause_response_out.set(response);
            }
        }

        // ──────────────────────────────────────────────────────────────────
        // ResumeJobs RPC
        // ──────────────────────────────────────────────────────────────────
        else if (trigger.action_name == "resume_request") {
            if (p.resume_request_in.is_present()) {
                [[maybe_unused]] auto request = p.resume_request_in.get();

                // Send control command to JobQueue
                job_queue::ControlRequest control;
                control.command = job_queue::ControlRequest::Command::RESUME;
                control.target_job_id = -1;  // All jobs
                p.control_out.set(control);

                aapis::orchestrator::v2::ResumeJobsResponse response;
                response.set_success(true);
                response.set_message("Resume command sent to JobQueue");
                SPDLOG_INFO("Resume jobs command");

                s.requests_handled++;
                p.resume_response_out.set(response);
            }
        }

        // ──────────────────────────────────────────────────────────────────
        // CancelJob RPC
        // ──────────────────────────────────────────────────────────────────
        else if (trigger.action_name == "cancel_request") {
            if (p.cancel_request_in.is_present()) {
                auto request = p.cancel_request_in.get();

                // Send control command to JobQueue
                job_queue::ControlRequest control;
                control.command = job_queue::ControlRequest::Command::CANCEL;
                control.target_job_id = request.job_id();
                p.control_out.set(control);

                aapis::orchestrator::v2::CancelJobResponse response;
                response.set_success(true);
                response.set_message("Cancel command sent to JobQueue");
                SPDLOG_INFO("Cancel job {} command", request.job_id());

                s.requests_handled++;
                p.cancel_response_out.set(response);
            }
        }

        // ──────────────────────────────────────────────────────────────────
        // Handle responses from JobQueue
        // ──────────────────────────────────────────────────────────────────

        // Note: The current implementation sends responses immediately.
        // A more sophisticated implementation would track pending requests
        // and match them with JobQueue responses.

    }

    return RunningState::index();
}

// ══════════════════════════════════════════════════════════════════════════════
// Reactor Implementation
// ══════════════════════════════════════════════════════════════════════════════

JobServer::JobServer(const ::services::MicroServiceContainer<>& container)
    : Base(container)
{
}

} // namespace job_server

} // namespace orchestrator
