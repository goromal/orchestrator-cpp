#include <boost/program_options.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <csignal>
#include <atomic>

#include <mscpp/ReactorScheduler.h>
#include <mscpp/Topology.h>
#include <mscpp/IOAdapter.h>
#include <mscpp/IOAdapters/GrpcAdapter.h>

#include <aapis/orchestrator/v2/orchestrator.grpc.pb.h>

#include "orchestrator/JobQueue.h"
#include "orchestrator/JobExecutor.h"
#include "orchestrator/JobDatabase.h"
#include "orchestrator/JobServer.h"

// Global shutdown flag for signal handling
std::atomic<bool> g_shutdown_requested{false};

void signalHandler(int signum)
{
    std::cout << "Received signal " << signum << ", initiating graceful shutdown..." << std::endl;
    g_shutdown_requested.store(true);
}

// ══════════════════════════════════════════════════════════════════════════════
// gRPC Service Implementation
// ══════════════════════════════════════════════════════════════════════════════

class OrchestratorServiceImpl : public aapis::orchestrator::v2::OrchestratorService::Service
{
public:
    explicit OrchestratorServiceImpl(std::shared_ptr<orchestrator::job_server::JobServer> job_server)
        : job_server_(job_server)
    {
    }

    grpc::Status DefineJob(grpc::ServerContext* context,
                          const aapis::orchestrator::v2::DefineJobRequest* request,
                          aapis::orchestrator::v2::DefineJobResponse* response) override
    {
        (void)context;
        return handleRpc(*request, response, "define_job_request",
                        job_server_->getPorts().define_job_response_out);
    }

    grpc::Status KickoffJob(grpc::ServerContext* context,
                           const aapis::orchestrator::v2::KickoffJobRequest* request,
                           aapis::orchestrator::v2::KickoffJobResponse* response) override
    {
        (void)context;
        return handleRpc(*request, response, "kickoff_job_request",
                        job_server_->getPorts().kickoff_job_response_out);
    }

    grpc::Status JobStatus(grpc::ServerContext* context,
                          const aapis::orchestrator::v2::JobStatusRequest* request,
                          aapis::orchestrator::v2::JobStatusResponse* response) override
    {
        (void)context;
        return handleRpc(*request, response, "job_status_request",
                        job_server_->getPorts().job_status_response_out);
    }

    grpc::Status JobsSummaryStatus(grpc::ServerContext* context,
                                   const aapis::orchestrator::v2::JobsSummaryStatusRequest* request,
                                   aapis::orchestrator::v2::JobsSummaryStatusResponse* response) override
    {
        (void)context;
        return handleRpc(*request, response, "jobs_summary_request",
                        job_server_->getPorts().jobs_summary_response_out);
    }

    grpc::Status PauseJobs(grpc::ServerContext* context,
                          const aapis::orchestrator::v2::PauseJobsRequest* request,
                          aapis::orchestrator::v2::PauseJobsResponse* response) override
    {
        (void)context;
        return handleRpc(*request, response, "pause_request",
                        job_server_->getPorts().pause_response_out);
    }

    grpc::Status ResumeJobs(grpc::ServerContext* context,
                           const aapis::orchestrator::v2::ResumeJobsRequest* request,
                           aapis::orchestrator::v2::ResumeJobsResponse* response) override
    {
        (void)context;
        return handleRpc(*request, response, "resume_request",
                        job_server_->getPorts().resume_response_out);
    }

    grpc::Status CancelJob(grpc::ServerContext* context,
                          const aapis::orchestrator::v2::CancelJobRequest* request,
                          aapis::orchestrator::v2::CancelJobResponse* response) override
    {
        (void)context;
        return handleRpc(*request, response, "cancel_request",
                        job_server_->getPorts().cancel_response_out);
    }

private:
    /**
     * Handle RPC with request-response polling pattern.
     *
     * This implementation directly polls the JobServer output port instead of
     * using the incomplete IOAdapter::waitForReactorResponse() mechanism.
     *
     * @tparam Request Request message type
     * @tparam Response Response message type
     * @tparam OutputPort Output port type
     * @param request The RPC request
     * @param response Pointer to response (will be filled)
     * @param action_name Logical action name to schedule
     * @param output_port Reference to reactor output port for response
     * @param timeout Maximum time to wait for response
     * @return gRPC status
     */
    template<typename Request, typename Response, typename OutputPort>
    grpc::Status handleRpc(
        const Request& request,
        Response* response,
        const std::string& action_name,
        OutputPort& output_port,
        std::chrono::milliseconds timeout = std::chrono::seconds(30))  // Increased for async reactor flow
    {
        // Clear any stale cached value from previous request
        output_port.clear_cache();

        // Schedule action with request data on reactor
        job_server_->scheduleLogicalActionWithData(action_name, request);

        // Poll output port for response with timeout
        // Note: This may take multiple reactor heartbeats for JobServer→JobQueue→JobServer flow
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            // Check if output port has data
            if (output_port.is_present())
            {
                *response = output_port.get();
                output_port.clear_cache();  // Clear after reading
                return grpc::Status::OK;
            }

            // Sleep briefly to avoid busy-wait
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Timeout
        return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                           "Reactor response timeout for action: " + action_name);
    }

    std::shared_ptr<orchestrator::job_server::JobServer> job_server_;
};

// ══════════════════════════════════════════════════════════════════════════════
// Main Daemon
// ══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[])
{
    // ──────────────────────────────────────────────────────────────────────────
    // Parse CLI Arguments
    // ──────────────────────────────────────────────────────────────────────────

    uint32_t grpc_port = 50051;
    uint32_t num_threads = 4;
    std::string db_path = "./orchestrator.db";

    boost::program_options::options_description args_desc("Orchestrator Service Options");
    // clang-format off
    args_desc.add_options()
        ("help,h", "print usage")
        ("grpc-port,p", boost::program_options::value<uint32_t>(),
         "gRPC port to serve requests on (default: 50051)")
        ("threads,t", boost::program_options::value<uint32_t>(),
         "Number of executor threads (default: 4)")
        ("db-path,d", boost::program_options::value<std::string>(),
         "Path to job database file (default: ./orchestrator.db)");
    // clang-format on

    boost::program_options::variables_map vm;
    boost::program_options::store(boost::program_options::parse_command_line(argc, argv, args_desc), vm);
    boost::program_options::notify(vm);

    if (vm.count("help"))
    {
        std::cout << args_desc << std::endl;
        return 0;
    }

    if (vm.count("grpc-port"))
    {
        grpc_port = vm["grpc-port"].as<uint32_t>();
    }
    if (vm.count("threads"))
    {
        num_threads = vm["threads"].as<uint32_t>();
    }
    if (vm.count("db-path"))
    {
        db_path = vm["db-path"].as<std::string>();
    }

    std::cout << "Orchestrator Service Configuration:" << std::endl;
    std::cout << "  gRPC Port: " << grpc_port << std::endl;
    std::cout << "  Executor Threads: " << num_threads << std::endl;
    std::cout << "  Database Path: " << db_path << std::endl;

    // ──────────────────────────────────────────────────────────────────────────
    // Setup Signal Handling
    // ──────────────────────────────────────────────────────────────────────────

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ──────────────────────────────────────────────────────────────────────────
    // Create Reactor Scheduler
    // ──────────────────────────────────────────────────────────────────────────

    auto scheduler = std::make_shared<::services::ReactorScheduler>();

    // ──────────────────────────────────────────────────────────────────────────
    // Create Reactors
    // ──────────────────────────────────────────────────────────────────────────

    ::services::MicroServiceContainer<> container;

    auto job_queue = std::make_shared<orchestrator::job_queue::JobQueue>(container);
    auto job_executor = std::make_shared<orchestrator::job_executor::JobExecutor>(container);
    auto job_database = std::make_shared<orchestrator::job_database::JobDatabase>(container, db_path);
    auto job_server = std::make_shared<orchestrator::job_server::JobServer>(container);

    // ──────────────────────────────────────────────────────────────────────────
    // Register Reactors with Scheduler
    // ──────────────────────────────────────────────────────────────────────────

    job_queue->setScheduler(scheduler.get());
    job_executor->setScheduler(scheduler.get());
    job_database->setScheduler(scheduler.get());
    job_server->setScheduler(scheduler.get());

    scheduler->registerReactor(job_queue);
    scheduler->registerReactor(job_executor);
    scheduler->registerReactor(job_database);
    scheduler->registerReactor(job_server);

    // ──────────────────────────────────────────────────────────────────────────
    // Wire Ports via ConnectionManager
    // ──────────────────────────────────────────────────────────────────────────

    ::services::ConnectionManager connection_mgr(scheduler.get());
    connection_mgr.setAutoScheduleLogicalActions(true);

    size_t job_queue_id = job_queue->getId();
    size_t job_executor_id = job_executor->getId();
    size_t job_database_id = job_database->getId();
    size_t job_server_id = job_server->getId();

    // JobServer → JobQueue: New jobs, queries, control commands
    connection_mgr.connect(
        job_server->getPorts().new_job_out,
        job_queue->getPorts().new_job_in,
        job_server_id, job_queue_id,
        "job_server_to_queue_new_job",
        job_queue
    );
    connection_mgr.connect(
        job_server->getPorts().query_out,
        job_queue->getPorts().query_request_in,
        job_server_id, job_queue_id,
        "job_server_to_queue_query",
        job_queue
    );
    connection_mgr.connect(
        job_server->getPorts().control_out,
        job_queue->getPorts().control_request_in,
        job_server_id, job_queue_id,
        "job_server_to_queue_control",
        job_queue
    );

    // JobQueue → JobServer: Query responses, job ID assignments
    connection_mgr.connect(
        job_queue->getPorts().query_response_out,
        job_server->getPorts().query_response_in,
        job_queue_id, job_server_id,
        "queue_to_job_server_query_response",
        job_server
    );
    connection_mgr.connect(
        job_queue->getPorts().new_job_id_out,
        job_server->getPorts().new_job_id_in,
        job_queue_id, job_server_id,
        "queue_to_job_server_new_job_id",
        job_server
    );

    // JobQueue → JobExecutor: Jobs to execute
    connection_mgr.connect(
        job_queue->getPorts().execute_job_out,
        job_executor->getPorts().job_in,
        job_queue_id, job_executor_id,
        "queue_to_executor_job",
        job_executor
    );

    // JobExecutor → JobQueue: Execution results
    connection_mgr.connect(
        job_executor->getPorts().job_result_out,
        job_queue->getPorts().job_result_in,
        job_queue_id, job_executor_id,
        "executor_to_queue_result",
        job_queue
    );

    // JobQueue ↔ JobDatabase: Snapshots for persistence
    connection_mgr.connect(
        job_queue->getPorts().save_snapshot_out,
        job_database->getPorts().save_snapshot_in,
        job_queue_id, job_database_id,
        "queue_to_database_snapshot",
        job_database
    );
    connection_mgr.connect(
        job_database->getPorts().load_snapshot_out,
        job_queue->getPorts().load_snapshot_in,
        job_database_id, job_queue_id,
        "database_to_queue_restore",
        job_queue
    );

    std::cout << "Port connections established." << std::endl;

    // ──────────────────────────────────────────────────────────────────────────
    // Start Reactor Scheduler in Background Thread (BEFORE gRPC server!)
    // ──────────────────────────────────────────────────────────────────────────

    std::thread scheduler_thread([&scheduler]() {
        std::cout << "Reactor scheduler started." << std::endl;
        scheduler->run();
        std::cout << "Reactor scheduler stopped." << std::endl;
    });

    // Give scheduler a moment to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ──────────────────────────────────────────────────────────────────────────
    // Create gRPC Server for JobServer
    // ──────────────────────────────────────────────────────────────────────────

    std::string grpc_address = "0.0.0.0:" + std::to_string(grpc_port);
    OrchestratorServiceImpl service_impl(job_server);

    grpc::ServerBuilder server_builder;
    server_builder.AddListeningPort(grpc_address, grpc::InsecureServerCredentials());
    server_builder.RegisterService(&service_impl);

    std::unique_ptr<grpc::Server> grpc_server = server_builder.BuildAndStart();
    if (!grpc_server)
    {
        std::cerr << "ERROR: Failed to start gRPC server on " << grpc_address << std::endl;
        scheduler->stop();
        if (scheduler_thread.joinable())
        {
            scheduler_thread.join();
        }
        return 1;
    }

    std::cout << "Starting gRPC server on " + grpc_address << "..." << std::endl;
    std::cout << "Orchestrator service is running. Press Ctrl+C to shutdown." << std::endl;

    // ──────────────────────────────────────────────────────────────────────────
    // Main Loop: Wait for Shutdown Signal
    // ──────────────────────────────────────────────────────────────────────────

    while (!g_shutdown_requested.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Graceful Shutdown
    // ──────────────────────────────────────────────────────────────────────────

    std::cout << "Shutting down orchestrator service..." << std::endl;

    // Stop gRPC server first (stops accepting new requests)
    std::cout << "  Stopping gRPC server..." << std::endl;
    grpc_server->Shutdown();

    // Stop reactor scheduler (stops all reactors)
    std::cout << "  Stopping reactor scheduler..." << std::endl;
    scheduler->stop();

    // Wait for scheduler thread to complete
    if (scheduler_thread.joinable())
    {
        scheduler_thread.join();
    }

    std::cout << "Orchestrator service shutdown complete." << std::endl;

    return 0;
}
