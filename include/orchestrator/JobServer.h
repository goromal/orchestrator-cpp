#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <mscpp/MicroServiceReactors.h>
#include <mscpp/Ports.h>
#include <mscpp/StateSet.h>
#include <mscpp/MicroServiceContainer.h>
#include <mscpp/Logging.h>
#include <mscpp/StepTrigger.h>

#include <aapis/orchestrator/v2/orchestrator.pb.h>

#include "orchestrator/Job.h"
#include "orchestrator/JobQueue.h"

namespace orchestrator
{

namespace job_server
{

// ══════════════════════════════════════════════════════════════════════════════
// Reactor Name Declaration
// ══════════════════════════════════════════════════════════════════════════════

inline constexpr char NameJobServer[] = "JobServer";

// ══════════════════════════════════════════════════════════════════════════════
// Port Definitions (gRPC→Reactor→JobQueue Communication)
// ══════════════════════════════════════════════════════════════════════════════

/**
 * JobDefinition structure for DefineJob RPC
 */
struct JobDefinition {
    std::string job_type;
    std::string job_definition;
};

/**
 * Port collection for JobServer reactor
 *
 * This reactor bridges gRPC requests (via IOAdapter) to the orchestrator's
 * internal job queue reactor.
 *
 * Inputs (from gRPC Adapter via logical actions):
 *   - define_job_request_in: DefineJob RPC requests
 *   - kickoff_job_request_in: KickoffJob RPC requests
 *   - job_status_request_in: JobStatus RPC requests
 *   - jobs_summary_request_in: JobsSummaryStatus RPC requests
 *   - pause_request_in: PauseJobs RPC requests
 *   - resume_request_in: ResumeJobs RPC requests
 *   - cancel_request_in: CancelJob RPC requests
 *
 * Outputs (to gRPC Adapter):
 *   - define_job_response_out: DefineJob RPC responses
 *   - kickoff_job_response_out: KickoffJob RPC responses
 *   - job_status_response_out: JobStatus RPC responses
 *   - jobs_summary_response_out: JobsSummaryStatus RPC responses
 *   - pause_response_out: PauseJobs RPC responses
 *   - resume_response_out: ResumeJobs RPC responses
 *   - cancel_response_out: CancelJob RPC responses
 *
 * Outputs (to JobQueue):
 *   - new_job_out: New job submissions
 *   - query_out: Job query requests
 *   - control_out: Control commands (pause/resume/cancel)
 *
 * Inputs (from JobQueue):
 *   - new_job_id_in: Assigned job IDs
 *   - query_response_in: Query responses
 *   - control_response_in: Control command acknowledgments
 */
struct Ports : ::services::AutoClearPorts<Ports> {
    // ──────────────────────────────────────────────────────────────────
    // Inputs from gRPC Adapter (RPC requests)
    // ──────────────────────────────────────────────────────────────────
    ::services::InputPort<aapis::orchestrator::v2::DefineJobRequest> define_job_request_in;
    ::services::InputPort<aapis::orchestrator::v2::KickoffJobRequest> kickoff_job_request_in;
    ::services::InputPort<aapis::orchestrator::v2::JobStatusRequest> job_status_request_in;
    ::services::InputPort<aapis::orchestrator::v2::JobsSummaryStatusRequest> jobs_summary_request_in;
    ::services::InputPort<aapis::orchestrator::v2::PauseJobsRequest> pause_request_in;
    ::services::InputPort<aapis::orchestrator::v2::ResumeJobsRequest> resume_request_in;
    ::services::InputPort<aapis::orchestrator::v2::CancelJobRequest> cancel_request_in;

    // ──────────────────────────────────────────────────────────────────
    // Outputs to gRPC Adapter (RPC responses)
    // ──────────────────────────────────────────────────────────────────
    ::services::OutputPort<aapis::orchestrator::v2::DefineJobResponse> define_job_response_out;
    ::services::OutputPort<aapis::orchestrator::v2::KickoffJobResponse> kickoff_job_response_out;
    ::services::OutputPort<aapis::orchestrator::v2::JobStatusResponse> job_status_response_out;
    ::services::OutputPort<aapis::orchestrator::v2::JobsSummaryStatusResponse> jobs_summary_response_out;
    ::services::OutputPort<aapis::orchestrator::v2::PauseJobsResponse> pause_response_out;
    ::services::OutputPort<aapis::orchestrator::v2::ResumeJobsResponse> resume_response_out;
    ::services::OutputPort<aapis::orchestrator::v2::CancelJobResponse> cancel_response_out;

    // ──────────────────────────────────────────────────────────────────
    // Outputs to JobQueue
    // ──────────────────────────────────────────────────────────────────
    ::services::OutputPort<Job> new_job_out;
    ::services::OutputPort<job_queue::JobQuery> query_out;
    ::services::OutputPort<job_queue::ControlRequest> control_out;

    // ──────────────────────────────────────────────────────────────────
    // Inputs from JobQueue
    // ──────────────────────────────────────────────────────────────────
    ::services::InputPort<int64_t> new_job_id_in;
    ::services::InputPort<job_queue::JobQueryResponse> query_response_in;
    ::services::InputPort<job_queue::ControlResponse> control_response_in;

    // Register input ports for automatic clearing
    REGISTER_INPUT_PORTS(define_job_request_in, kickoff_job_request_in,
                        job_status_request_in, jobs_summary_request_in,
                        pause_request_in, resume_request_in, cancel_request_in,
                        new_job_id_in, query_response_in, control_response_in)
};

// ══════════════════════════════════════════════════════════════════════════════
// Store (Reactor State)
// ══════════════════════════════════════════════════════════════════════════════

/**
 * JobServer reactor state
 *
 * Stores job type definitions submitted via DefineJob RPC.
 * This allows JobServer to validate job types before submission
 * and retrieve job definitions when converting KickoffJobRequest → Job.
 */
struct Store {
    // Job type definitions (job_type → job_definition)
    std::map<std::string, JobDefinition> job_definitions;

    // Request counter for debugging
    uint64_t requests_handled{0};

    // ──────────────────────────────────────────────────────────────────
    // Pure Functions: Job Definition Management
    // ──────────────────────────────────────────────────────────────────

    /**
     * Validate job definition request
     */
    struct ValidationResult {
        bool is_valid{true};
        std::string error_message;
    };

    ValidationResult validateDefineJobRequest(
        const aapis::orchestrator::v2::DefineJobRequest& request) const {
        if (request.job_type().empty()) {
            return {false, "job_type cannot be empty"};
        }
        if (request.job_definition().empty()) {
            return {false, "job_definition cannot be empty"};
        }
        // Check if job type already exists
        if (job_definitions.find(request.job_type()) != job_definitions.end()) {
            return {false, "job_type already defined: " + request.job_type()};
        }
        return {true, ""};
    }

    /**
     * Validate kickoff job request
     */
    ValidationResult validateKickoffJobRequest(
        const aapis::orchestrator::v2::KickoffJobRequest& request) const {
        if (request.job_type().empty()) {
            return {false, "job_type cannot be empty"};
        }
        // Check if job type is defined
        if (job_definitions.find(request.job_type()) == job_definitions.end()) {
            return {false, "unknown job_type: " + request.job_type()};
        }
        if (request.priority() < 0) {
            return {false, "priority must be non-negative"};
        }
        return {true, ""};
    }

    // ──────────────────────────────────────────────────────────────────
    // Mutation Helpers: Job Definition Management
    // ──────────────────────────────────────────────────────────────────

    /**
     * Add a job definition
     */
    void addJobDefinition(const JobDefinition& def) {
        job_definitions[def.job_type] = def;
    }

    /**
     * Get a job definition by type
     */
    const JobDefinition* getJobDefinition(const std::string& job_type) const {
        auto it = job_definitions.find(job_type);
        if (it != job_definitions.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // ──────────────────────────────────────────────────────────────────
    // Conversion Helpers: Proto ↔ Internal Types
    // ──────────────────────────────────────────────────────────────────

    /**
     * Convert KickoffJobRequest to internal Job structure
     */
    Job convertToJob(const aapis::orchestrator::v2::KickoffJobRequest& request) const {
        const JobDefinition* def = getJobDefinition(request.job_type());
        if (!def) {
            // Should never happen if validation passed
            throw std::runtime_error("Job definition not found: " + request.job_type());
        }

        Job job;
        job.priority = request.priority();

        // Set job script from definition
        job.script = def->job_definition;

        // TODO: Parse timeout from job definition or add to DefineJobRequest
        job.timeoutSeconds = 300;  // Default 5 minutes

        // Map blocking job IDs to independentBlockers
        // Note: Currently treats all blockers as independent.
        // Future: Distinguish between independent and relevant blockers
        // based on whether their outputs are needed as inputs.
        job.independentBlockers.assign(
            request.blocking_job_ids().begin(),
            request.blocking_job_ids().end()
        );

        // Map input_job_ids to relevantBlockers (their outputs become inputs)
        job.relevantBlockers.assign(
            request.input_job_ids().begin(),
            request.input_job_ids().end()
        );

        // Map input_args to inputs (used for $INPUT_ARGS[] substitution)
        job.inputs.assign(
            request.input_args().begin(),
            request.input_args().end()
        );

        // Status starts as QUEUED
        job.status = aapis::orchestrator::v1::JobStatus::JOB_STATUS_QUEUED;

        return job;
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// FSM States
// ══════════════════════════════════════════════════════════════════════════════

/**
 * InitState - Initialization state for JobServer
 *
 * Responsibilities:
 * - Initialize JobServer on first heartbeat
 * - Transition to RunningState
 */
struct InitState : public ::services::State<InitState, 0> {
    size_t step(Store& s, Ports& p,
                const ::services::MicroServiceContainer<>& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

/**
 * RunningState - Main operational state for JobServer
 *
 * Responsibilities:
 * - Handle all RPC requests via logical actions
 * - Coordinate with JobQueue via ports
 * - Send responses back to gRPC adapter
 */
struct RunningState : public ::services::State<RunningState, 1> {
    size_t step(Store& s, Ports& p,
                const ::services::MicroServiceContainer<>& c,
                const ::services::LogicalTag& tag,
                const ::services::StepTrigger& trigger);
};

// ══════════════════════════════════════════════════════════════════════════════
// Reactor Definition
// ══════════════════════════════════════════════════════════════════════════════

/**
 * JobServer reactor
 *
 * Bridges gRPC OrchestratorService to orchestrator's internal job queue.
 * Uses IOAdapter pattern to maintain deterministic reactor execution
 * despite async gRPC RPC arrivals.
 *
 * Architecture:
 *   gRPC Client → GrpcAdapter (async) → JobServer (deterministic) → JobQueue
 *
 * The reactor remains single-threaded and deterministic, while the GrpcAdapter
 * handles all async gRPC operations in a separate thread.
 */
class JobServer : public ::services::MicroServiceFSMReactor<
    NameJobServer,
    Store,
    Ports,
    ::services::MicroServiceContainer<>,
    ::services::StateSet<InitState, RunningState>>
{
public:
    using Base = ::services::MicroServiceFSMReactor<
        NameJobServer,
        Store,
        Ports,
        ::services::MicroServiceContainer<>,
        ::services::StateSet<InitState, RunningState>>;

    /**
     * Construct JobServer reactor
     *
     * @param container Reactor container for dependency management
     */
    explicit JobServer(const ::services::MicroServiceContainer<>& container);

protected:
    /**
     * Override processActionData to transfer request data to input ports.
     *
     * This is called by processPendingActions() before executeLogicalAction().
     * We use std::any_cast to extract the typed request and write it to the
     * corresponding input port.
     *
     * @param action_name Name of the action (e.g., "define_job_request")
     * @param action_data Type-erased request data
     */
    void processActionData(const std::string& action_name, const std::any& action_data) override;
};

} // namespace job_server

} // namespace orchestrator
