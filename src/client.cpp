#include <boost/program_options.hpp>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <aapis/orchestrator/v2/orchestrator.grpc.pb.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using aapis::orchestrator::v2::OrchestratorService;
using aapis::orchestrator::v2::DefineJobRequest;
using aapis::orchestrator::v2::DefineJobResponse;
using aapis::orchestrator::v2::KickoffJobRequest;
using aapis::orchestrator::v2::KickoffJobResponse;
using aapis::orchestrator::v2::JobStatusRequest;
using aapis::orchestrator::v2::JobStatusResponse;
using aapis::orchestrator::v2::JobsSummaryStatusRequest;
using aapis::orchestrator::v2::JobsSummaryStatusResponse;
using aapis::orchestrator::v2::PauseJobsRequest;
using aapis::orchestrator::v2::PauseJobsResponse;
using aapis::orchestrator::v2::ResumeJobsRequest;
using aapis::orchestrator::v2::ResumeJobsResponse;
using aapis::orchestrator::v2::CancelJobRequest;
using aapis::orchestrator::v2::CancelJobResponse;
using aapis::orchestrator::v2::ListJobDefinitionsRequest;
using aapis::orchestrator::v2::ListJobDefinitionsResponse;
using aapis::orchestrator::v2::DeleteJobDefinitionRequest;
using aapis::orchestrator::v2::DeleteJobDefinitionResponse;
using aapis::orchestrator::v2::QueryJobsRequest;
using aapis::orchestrator::v2::QueryJobsResponse;
using aapis::orchestrator::v2::JobStatus;

class OrchestratorClient
{
public:
    OrchestratorClient(std::shared_ptr<Channel> channel)
        : stub_(OrchestratorService::NewStub(channel))
    {
    }

    bool DefineJob(const std::string& job_type, const std::string& job_definition)
    {
        DefineJobRequest request;
        request.set_job_type(job_type);
        request.set_job_definition(job_definition);

        DefineJobResponse response;
        ClientContext context;

        Status status = stub_->DefineJob(&context, request, &response);

        if (status.ok())
        {
            if (response.success())
            {
                std::cout << "Job type '" << job_type << "' defined successfully" << std::endl;
                return true;
            }
            else
            {
                std::cerr << "Failed to define job: " << response.message() << std::endl;
                return false;
            }
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    int64_t KickoffJob(const std::string& job_type,
                       int64_t priority,
                       const std::vector<int64_t>& blocking_job_ids,
                       const std::vector<int64_t>& input_job_ids,
                       const std::vector<std::string>& input_args)
    {
        KickoffJobRequest request;
        request.set_job_type(job_type);
        request.set_priority(priority);

        for (auto id : blocking_job_ids)
        {
            request.add_blocking_job_ids(id);
        }

        for (auto id : input_job_ids)
        {
            request.add_input_job_ids(id);
        }

        for (const auto& arg : input_args)
        {
            request.add_input_args(arg);
        }

        KickoffJobResponse response;
        ClientContext context;

        Status status = stub_->KickoffJob(&context, request, &response);

        if (status.ok())
        {
            if (response.success())
            {
                std::cout << "Job kicked off with ID: " << response.job_id() << std::endl;
                return response.job_id();
            }
            else
            {
                std::cerr << "Failed to kickoff job: " << response.message() << std::endl;
                return -1;
            }
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return -1;
        }
    }

    bool GetJobStatus(int64_t job_id)
    {
        JobStatusRequest request;
        request.set_job_id(job_id);

        JobStatusResponse response;
        ClientContext context;

        Status status = stub_->JobStatus(&context, request, &response);

        if (status.ok())
        {
            std::cout << "Job ID: " << job_id << std::endl;
            std::cout << "Status: " << JobStatusString(response.status()) << std::endl;
            std::cout << "Exec: " << response.exec() << std::endl;
            std::cout << "Priority: " << response.priority() << std::endl;

            if (response.blockers_size() > 0)
            {
                std::cout << "Blockers: ";
                for (int i = 0; i < response.blockers_size(); ++i)
                {
                    if (i > 0) std::cout << ", ";
                    std::cout << response.blockers(i);
                }
                std::cout << std::endl;
            }

            if (response.outputs_size() > 0)
            {
                std::cout << "Outputs: ";
                for (int i = 0; i < response.outputs_size(); ++i)
                {
                    if (i > 0) std::cout << ", ";
                    std::cout << response.outputs(i);
                }
                std::cout << std::endl;
            }

            if (!response.message().empty())
            {
                std::cout << "Message: " << response.message() << std::endl;
            }

            if (!response.program_output().empty())
            {
                std::cout << "Program Output:\n" << response.program_output() << std::endl;
            }

            if (response.exec_duration_secs() > 0)
            {
                std::cout << "Execution Duration: " << response.exec_duration_secs() << " seconds" << std::endl;
            }

            return true;
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    bool GetJobsSummary()
    {
        JobsSummaryStatusRequest request;
        JobsSummaryStatusResponse response;
        ClientContext context;

        Status status = stub_->JobsSummaryStatus(&context, request, &response);

        if (status.ok())
        {
            std::cout << "Completed Jobs: " << response.num_completed_jobs() << std::endl;
            std::cout << "Queued Jobs: " << response.num_queued_jobs() << std::endl;
            std::cout << "Active Jobs: " << response.num_active_jobs() << std::endl;
            std::cout << "Blocked Jobs: " << response.num_blocked_jobs() << std::endl;
            std::cout << "Paused Jobs: " << response.num_paused_jobs() << std::endl;
            std::cout << "Discarded Jobs: " << response.num_discarded_jobs() << std::endl;

            if (response.num_completed_jobs() > 0)
            {
                std::cout << "Completed Job IDs: ";
                for (int i = 0; i < response.completed_jobs_size(); ++i)
                {
                    if (i > 0) std::cout << ", ";
                    std::cout << response.completed_jobs(i);
                }
                std::cout << std::endl;
            }

            if (response.num_queued_jobs() > 0)
            {
                std::cout << "Queued Job IDs: ";
                for (int i = 0; i < response.queued_jobs_size(); ++i)
                {
                    if (i > 0) std::cout << ", ";
                    std::cout << response.queued_jobs(i);
                }
                std::cout << std::endl;
            }

            if (response.num_active_jobs() > 0)
            {
                std::cout << "Active Job IDs: ";
                for (int i = 0; i < response.active_jobs_size(); ++i)
                {
                    if (i > 0) std::cout << ", ";
                    std::cout << response.active_jobs(i);
                }
                std::cout << std::endl;
            }

            return true;
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    bool PauseJobs()
    {
        PauseJobsRequest request;
        PauseJobsResponse response;
        ClientContext context;

        Status status = stub_->PauseJobs(&context, request, &response);

        if (status.ok())
        {
            if (response.success())
            {
                std::cout << "Jobs paused successfully" << std::endl;
                return true;
            }
            else
            {
                std::cerr << "Failed to pause jobs: " << response.message() << std::endl;
                return false;
            }
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    bool ResumeJobs()
    {
        ResumeJobsRequest request;
        ResumeJobsResponse response;
        ClientContext context;

        Status status = stub_->ResumeJobs(&context, request, &response);

        if (status.ok())
        {
            if (response.success())
            {
                std::cout << "Jobs resumed successfully" << std::endl;
                return true;
            }
            else
            {
                std::cerr << "Failed to resume jobs: " << response.message() << std::endl;
                return false;
            }
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    bool CancelJob(int64_t job_id)
    {
        CancelJobRequest request;
        request.set_job_id(job_id);

        CancelJobResponse response;
        ClientContext context;

        Status status = stub_->CancelJob(&context, request, &response);

        if (status.ok())
        {
            if (response.success())
            {
                std::cout << "Job " << job_id << " cancelled successfully" << std::endl;
                return true;
            }
            else
            {
                std::cerr << "Failed to cancel job: " << response.message() << std::endl;
                return false;
            }
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    bool ListJobDefinitions()
    {
        ListJobDefinitionsRequest request;
        ListJobDefinitionsResponse response;
        ClientContext context;

        Status status = stub_->ListJobDefinitions(&context, request, &response);

        if (status.ok())
        {
            if (response.definitions_size() == 0)
            {
                std::cout << "No job definitions found" << std::endl;
                return true;
            }

            std::cout << "Job Definitions (" << response.definitions_size() << "):" << std::endl;
            std::cout << "----------------------------------------" << std::endl;
            for (const auto& def : response.definitions())
            {
                std::cout << "Job Type: " << def.job_type() << std::endl;
                std::cout << "  Definition: " << def.job_definition() << std::endl;
                std::cout << "  Timeout: " << def.timeout_seconds() << "s" << std::endl;
                std::cout << "  Created: " << def.created_at() << std::endl;
                std::cout << "  Updated: " << def.updated_at() << std::endl;
                std::cout << "----------------------------------------" << std::endl;
            }
            return true;
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    bool DeleteJobDefinition(const std::string& job_type)
    {
        DeleteJobDefinitionRequest request;
        request.set_job_type(job_type);

        DeleteJobDefinitionResponse response;
        ClientContext context;

        Status status = stub_->DeleteJobDefinition(&context, request, &response);

        if (status.ok())
        {
            if (response.success())
            {
                std::cout << "Job definition '" << job_type << "' deleted successfully" << std::endl;
                return true;
            }
            else
            {
                std::cerr << "Failed to delete job definition: " << response.message() << std::endl;
                return false;
            }
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    bool QueryJobs(const std::string& job_type_filter,
                   QueryJobsRequest::StatusFilter status_filter,
                   QueryJobsRequest::SortBy sort_by,
                   int32_t limit,
                   int32_t offset)
    {
        QueryJobsRequest request;
        request.set_job_type_filter(job_type_filter);
        request.set_status_filter(status_filter);
        request.set_sort_by(sort_by);
        request.set_limit(limit);
        request.set_offset(offset);

        QueryJobsResponse response;
        ClientContext context;

        Status status = stub_->QueryJobs(&context, request, &response);

        if (status.ok())
        {
            if (response.jobs_size() == 0)
            {
                std::cout << "No jobs found matching criteria" << std::endl;
                std::cout << "Total: 0 jobs" << std::endl;
                return true;
            }

            std::cout << "Jobs (" << response.jobs_size() << " of " << response.total_count() << "):" << std::endl;
            std::cout << "============================================" << std::endl;
            for (const auto& job : response.jobs())
            {
                std::cout << "Job ID: " << job.job_id() << std::endl;
                std::cout << "  Type: " << job.job_type() << std::endl;
                std::cout << "  Status: " << JobStatusString(job.status()) << std::endl;
                std::cout << "  Priority: " << job.priority() << std::endl;
                std::cout << "  Submitted: " << job.submitted_at() << std::endl;
                if (job.completed_at() > 0)
                {
                    std::cout << "  Completed: " << job.completed_at() << std::endl;
                    std::cout << "  Duration: " << job.exec_duration_secs() << "s" << std::endl;
                }
                std::cout << "--------------------------------------------" << std::endl;
            }
            std::cout << "Total matching jobs: " << response.total_count() << std::endl;
            return true;
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

private:
    std::unique_ptr<OrchestratorService::Stub> stub_;

    std::string JobStatusString(JobStatus status)
    {
        switch (status)
        {
            case JobStatus::JOB_STATUS_INVALID: return "INVALID";
            case JobStatus::JOB_STATUS_COMPLETE: return "COMPLETE";
            case JobStatus::JOB_STATUS_QUEUED: return "QUEUED";
            case JobStatus::JOB_STATUS_ACTIVE: return "ACTIVE";
            case JobStatus::JOB_STATUS_ERROR: return "ERROR";
            case JobStatus::JOB_STATUS_BLOCKED: return "BLOCKED";
            case JobStatus::JOB_STATUS_PAUSED: return "PAUSED";
            case JobStatus::JOB_STATUS_CANCELED: return "CANCELED";
            default: return "UNSPECIFIED";
        }
    }
};

int main(int argc, char* argv[])
{
    uint32_t grpc_port = 50051;
    std::string command;
    std::vector<std::string> remaining_args;

    // Simple manual argument parsing
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: orchestratorctl [options] <command> [command-args...]" << std::endl;
            std::cout << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -h, --help           Print usage" << std::endl;
            std::cout << "  -p, --grpc-port <n>  gRPC port to connect to (default: 50051)" << std::endl;
            std::cout << std::endl;
            std::cout << "Commands:" << std::endl;
            std::cout << "  define <job-type> <job-definition>" << std::endl;
            std::cout << "  list-definitions" << std::endl;
            std::cout << "  delete-definition <job-type>" << std::endl;
            std::cout << "  kickoff <job-type> [--priority <n>] [--blocker <id>]... [--input-job <id>]... [--input <arg>]..." << std::endl;
            std::cout << "  status <job-id>" << std::endl;
            std::cout << "  summary" << std::endl;
            std::cout << "  query [--type <type>] [--status all|complete|incomplete|error|canceled]" << std::endl;
            std::cout << "        [--sort id|completion|priority] [--limit <n>] [--offset <n>]" << std::endl;
            std::cout << "  pause" << std::endl;
            std::cout << "  resume" << std::endl;
            std::cout << "  cancel <job-id>" << std::endl;
            return 0;
        }
        else if (arg == "-p" || arg == "--grpc-port")
        {
            if (i + 1 < argc)
            {
                grpc_port = std::stoul(argv[++i]);
            }
            else
            {
                std::cerr << "Error: " << arg << " requires an argument" << std::endl;
                return 1;
            }
        }
        else if (command.empty())
        {
            command = arg;
        }
        else
        {
            remaining_args.push_back(arg);
        }
    }

    if (command.empty())
    {
        std::cerr << "Error: No command specified" << std::endl;
        std::cerr << "Run 'orchestratorctl --help' for usage information" << std::endl;
        return 1;
    }

    std::string target = "localhost:" + std::to_string(grpc_port);
    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    OrchestratorClient client(channel);

    if (command == "define")
    {
        if (remaining_args.size() < 2)
        {
            std::cerr << "Usage: define <job-type> <job-definition>" << std::endl;
            return 1;
        }
        return client.DefineJob(remaining_args[0], remaining_args[1]) ? 0 : 1;
    }
    else if (command == "list-definitions")
    {
        return client.ListJobDefinitions() ? 0 : 1;
    }
    else if (command == "delete-definition")
    {
        if (remaining_args.empty())
        {
            std::cerr << "Usage: delete-definition <job-type>" << std::endl;
            return 1;
        }
        return client.DeleteJobDefinition(remaining_args[0]) ? 0 : 1;
    }
    else if (command == "kickoff")
    {
        if (remaining_args.empty())
        {
            std::cerr << "Usage: kickoff <job-type> [options]" << std::endl;
            return 1;
        }

        std::string job_type = remaining_args[0];
        int64_t priority = 0;
        std::vector<int64_t> blockers;
        std::vector<int64_t> input_jobs;
        std::vector<std::string> inputs;

        for (size_t i = 1; i < remaining_args.size(); ++i)
        {
            if (remaining_args[i] == "--priority" && i + 1 < remaining_args.size())
            {
                priority = std::stoll(remaining_args[++i]);
            }
            else if (remaining_args[i] == "--blocker" && i + 1 < remaining_args.size())
            {
                blockers.push_back(std::stoll(remaining_args[++i]));
            }
            else if (remaining_args[i] == "--input-job" && i + 1 < remaining_args.size())
            {
                input_jobs.push_back(std::stoll(remaining_args[++i]));
            }
            else if (remaining_args[i] == "--input" && i + 1 < remaining_args.size())
            {
                inputs.push_back(remaining_args[++i]);
            }
        }

        int64_t job_id = client.KickoffJob(job_type, priority, blockers, input_jobs, inputs);
        return job_id >= 0 ? 0 : 1;
    }
    else if (command == "status")
    {
        if (remaining_args.empty())
        {
            std::cerr << "Usage: status <job-id>" << std::endl;
            return 1;
        }
        int64_t job_id = std::stoll(remaining_args[0]);
        return client.GetJobStatus(job_id) ? 0 : 1;
    }
    else if (command == "summary")
    {
        return client.GetJobsSummary() ? 0 : 1;
    }
    else if (command == "query")
    {
        std::string job_type_filter;
        QueryJobsRequest::StatusFilter status_filter = QueryJobsRequest::ALL;
        QueryJobsRequest::SortBy sort_by = QueryJobsRequest::JOB_ID;
        int32_t limit = 50;
        int32_t offset = 0;

        for (size_t i = 0; i < remaining_args.size(); ++i)
        {
            if (remaining_args[i] == "--type" && i + 1 < remaining_args.size())
            {
                job_type_filter = remaining_args[++i];
            }
            else if (remaining_args[i] == "--status" && i + 1 < remaining_args.size())
            {
                std::string status_str = remaining_args[++i];
                if (status_str == "all") status_filter = QueryJobsRequest::ALL;
                else if (status_str == "complete") status_filter = QueryJobsRequest::COMPLETE;
                else if (status_str == "incomplete") status_filter = QueryJobsRequest::INCOMPLETE;
                else if (status_str == "error") status_filter = QueryJobsRequest::ERROR;
                else if (status_str == "canceled") status_filter = QueryJobsRequest::CANCELED;
                else
                {
                    std::cerr << "Invalid status filter: " << status_str << std::endl;
                    std::cerr << "Valid options: all, complete, incomplete, error, canceled" << std::endl;
                    return 1;
                }
            }
            else if (remaining_args[i] == "--sort" && i + 1 < remaining_args.size())
            {
                std::string sort_str = remaining_args[++i];
                if (sort_str == "id") sort_by = QueryJobsRequest::JOB_ID;
                else if (sort_str == "completion") sort_by = QueryJobsRequest::COMPLETION_TIME;
                else if (sort_str == "priority") sort_by = QueryJobsRequest::PRIORITY;
                else
                {
                    std::cerr << "Invalid sort option: " << sort_str << std::endl;
                    std::cerr << "Valid options: id, completion, priority" << std::endl;
                    return 1;
                }
            }
            else if (remaining_args[i] == "--limit" && i + 1 < remaining_args.size())
            {
                limit = std::stoi(remaining_args[++i]);
            }
            else if (remaining_args[i] == "--offset" && i + 1 < remaining_args.size())
            {
                offset = std::stoi(remaining_args[++i]);
            }
        }

        return client.QueryJobs(job_type_filter, status_filter, sort_by, limit, offset) ? 0 : 1;
    }
    else if (command == "pause")
    {
        return client.PauseJobs() ? 0 : 1;
    }
    else if (command == "resume")
    {
        return client.ResumeJobs() ? 0 : 1;
    }
    else if (command == "cancel")
    {
        if (remaining_args.empty())
        {
            std::cerr << "Usage: cancel <job-id>" << std::endl;
            return 1;
        }
        int64_t job_id = std::stoll(remaining_args[0]);
        return client.CancelJob(job_id) ? 0 : 1;
    }
    else
    {
        std::cerr << "Unknown command: " << command << std::endl;
        std::cerr << "Run 'orchestratorctl --help' for usage information" << std::endl;
        return 1;
    }
}
