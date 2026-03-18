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
            SPDLOG_INFO("RunningState: Processing define_job_request action");
            if (p.define_job_request_in.is_present()) {
                SPDLOG_INFO("RunningState: define_job_request_in port is PRESENT");
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
                SPDLOG_INFO("RunningState: Set define_job_response_out port (success={}, message={})",
                           response.success(), response.message());
            }
            else {
                SPDLOG_WARN("RunningState: define_job_request_in port NOT PRESENT!");
            }
        }

        // ──────────────────────────────────────────────────────────────────
        // KickoffJob RPC
        // ──────────────────────────────────────────────────────────────────
        else if (trigger.action_name == "kickoff_job_request") {
            if (p.kickoff_job_request_in.is_present()) {
                auto request = p.kickoff_job_request_in.get();

                // Validate request
                auto validation = s.validateKickoffJobRequest(request);
                if (!validation.is_valid) {
                    aapis::orchestrator::v2::KickoffJobResponse response;
                    response.set_success(false);
                    response.set_message(validation.error_message);
                    response.set_job_id(-1);
                    SPDLOG_WARN("KickoffJob validation failed: {}", validation.error_message);
                    p.kickoff_job_response_out.set(response);
                } else {
                    // Convert to internal Job structure
                    Job job = s.convertToJob(request);

                    // Send to JobQueue
                    p.new_job_out.set(job);

                    // Track pending request - response will be sent when new_job_id_in arrives
                    s.pending_kickoff = true;
                    s.pending_kickoff_request = request;
                    SPDLOG_INFO("Kicked off job type: {} (awaiting job ID from JobQueue)", request.job_type());
                    // NOTE: Response NOT sent here - will be sent when new_job_id_in is present
                }

                s.requests_handled++;
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

                // Track pending request - response will be sent when query_response_in arrives
                s.pending_query_type = Store::PendingQueryType::JOB_STATUS;
                s.pending_job_status_id = request.job_id();
                SPDLOG_INFO("Job status query for job_id: {} (awaiting JobQueue response)", request.job_id());

                s.requests_handled++;
                // NOTE: Response NOT sent here - will be sent when query_response_in is present
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

                // Track pending request - response will be sent when query_response_in arrives
                s.pending_query_type = Store::PendingQueryType::JOBS_SUMMARY;
                SPDLOG_INFO("Jobs summary status query (awaiting JobQueue response)");

                s.requests_handled++;
                // NOTE: Response NOT sent here - will be sent when query_response_in is present
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

    }

    // Handle job ID assignment from JobQueue (arrival via port)
    if (p.new_job_id_in.is_present()) {
        auto job_id = p.new_job_id_in.get();
        SPDLOG_INFO("Received job ID from JobQueue: {}", job_id);

        if (s.pending_kickoff) {
            aapis::orchestrator::v2::KickoffJobResponse response;
            response.set_success(true);
            response.set_message("Job queued successfully");
            response.set_job_id(job_id);
            SPDLOG_INFO("KickoffJob response: job_id={}", job_id);

            p.kickoff_job_response_out.set(response);
            s.pending_kickoff = false;
        } else {
            SPDLOG_WARN("Received unexpected job ID {} with no pending kickoff", job_id);
        }
    }

    // Handle query responses from JobQueue (arrival via port)
    if (p.query_response_in.is_present()) {
        auto query_response = p.query_response_in.get();
        SPDLOG_INFO("Received query response from JobQueue (success={}, {} jobs)",
                   query_response.success, query_response.jobs.size());

        // Match pending request and send appropriate RPC response
        if (s.pending_query_type == Store::PendingQueryType::JOB_STATUS) {
            // Handle JobStatus query response
            aapis::orchestrator::v2::JobStatusResponse response;

            if (query_response.success && !query_response.jobs.empty()) {
                const Job& job = query_response.jobs[0];

                // Convert v1::JobStatus to v2::JobStatus
                switch (job.status) {
                    case aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED:
                        response.set_status(aapis::orchestrator::v2::JobStatus::JOB_STATUS_QUEUED);
                        break;
                    case aapis::orchestrator::v1::JobStatus::JOB_STATUS_ACTIVE:
                        response.set_status(aapis::orchestrator::v2::JobStatus::JOB_STATUS_ACTIVE);
                        break;
                    case aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE:
                        response.set_status(aapis::orchestrator::v2::JobStatus::JOB_STATUS_COMPLETE);
                        break;
                    case aapis::orchestrator::v1::JobStatus::JOB_STATUS_ERROR:
                        response.set_status(aapis::orchestrator::v2::JobStatus::JOB_STATUS_ERROR);
                        break;
                    case aapis::orchestrator::v1::JobStatus::JOB_STATUS_CANCELED:
                        response.set_status(aapis::orchestrator::v2::JobStatus::JOB_STATUS_CANCELED);
                        break;
                    default:
                        response.set_status(aapis::orchestrator::v2::JobStatus::JOB_STATUS_UNSPECIFIED);
                        break;
                }

                response.set_message("Job found");
                SPDLOG_INFO("JobStatus response: job_id={} status={}", s.pending_job_status_id, job.status);
            } else {
                response.set_status(aapis::orchestrator::v2::JobStatus::JOB_STATUS_UNSPECIFIED);
                response.set_message(query_response.error_message.empty()
                                    ? "Job not found"
                                    : query_response.error_message);
                SPDLOG_WARN("JobStatus query failed: {}", response.message());
            }

            p.job_status_response_out.set(response);
            s.pending_query_type = Store::PendingQueryType::NONE;
        }
        else if (s.pending_query_type == Store::PendingQueryType::JOBS_SUMMARY) {
            // Handle JobsSummary query response
            aapis::orchestrator::v2::JobsSummaryStatusResponse response;

            if (query_response.success) {
                // Count jobs by status
                int num_queued = 0;
                int num_active = 0;
                int num_completed = 0;

                for (const auto& job : query_response.jobs) {
                    switch (job.status) {
                        case aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED:
                            num_queued++;
                            break;
                        case aapis::orchestrator::v1::JobStatus::JOB_STATUS_ACTIVE:
                            num_active++;
                            break;
                        case aapis::orchestrator::v1::JobStatus::JOB_STATUS_COMPLETE:
                            num_completed++;
                            break;
                        default:
                            break;
                    }
                }

                response.set_num_queued_jobs(num_queued);
                response.set_num_active_jobs(num_active);
                response.set_num_completed_jobs(num_completed);
                SPDLOG_INFO("JobsSummary response: queued={} active={} completed={}",
                           num_queued, num_active, num_completed);
            } else {
                response.set_num_queued_jobs(0);
                response.set_num_active_jobs(0);
                response.set_num_completed_jobs(0);
                SPDLOG_WARN("JobsSummary query failed: {}", query_response.error_message);
            }

            p.jobs_summary_response_out.set(response);
            s.pending_query_type = Store::PendingQueryType::NONE;
        }
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

void JobServer::processActionData(const std::string& action_name, const std::any& action_data)
{
    // Transfer action data to appropriate input ports
    // This fixes the missing data flow in mscpp's IOAdapter implementation

    SPDLOG_INFO("JobServer::processActionData called for action: {}", action_name);

    try {
        if (action_name == "define_job_request") {
            auto request = std::any_cast<aapis::orchestrator::v2::DefineJobRequest>(action_data);
            getPorts().define_job_request_in.set(request);
            SPDLOG_INFO("Set define_job_request_in port with job_type={}", request.job_type());
        }
        else if (action_name == "kickoff_job_request") {
            auto request = std::any_cast<aapis::orchestrator::v2::KickoffJobRequest>(action_data);
            getPorts().kickoff_job_request_in.set(request);
        }
        else if (action_name == "job_status_request") {
            auto request = std::any_cast<aapis::orchestrator::v2::JobStatusRequest>(action_data);
            getPorts().job_status_request_in.set(request);
        }
        else if (action_name == "jobs_summary_request") {
            auto request = std::any_cast<aapis::orchestrator::v2::JobsSummaryStatusRequest>(action_data);
            getPorts().jobs_summary_request_in.set(request);
        }
        else if (action_name == "pause_request") {
            auto request = std::any_cast<aapis::orchestrator::v2::PauseJobsRequest>(action_data);
            getPorts().pause_request_in.set(request);
        }
        else if (action_name == "resume_request") {
            auto request = std::any_cast<aapis::orchestrator::v2::ResumeJobsRequest>(action_data);
            getPorts().resume_request_in.set(request);
        }
        else if (action_name == "cancel_request") {
            auto request = std::any_cast<aapis::orchestrator::v2::CancelJobRequest>(action_data);
            getPorts().cancel_request_in.set(request);
        }
        else {
            SPDLOG_WARN("Unknown action name in processActionData: {}", action_name);
        }
    }
    catch (const std::bad_any_cast& e) {
        SPDLOG_ERROR("Failed to cast action data for action '{}': {}", action_name, e.what());
    }
}

} // namespace job_server

} // namespace orchestrator
