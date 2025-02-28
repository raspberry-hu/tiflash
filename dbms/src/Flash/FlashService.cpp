// Copyright 2022 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/CPUAffinityManager.h>
#include <Common/Stopwatch.h>
#include <Common/ThreadMetricUtil.h>
#include <Common/TiFlashMetrics.h>
#include <Common/VariantOp.h>
#include <Common/setThreadName.h>
#include <Flash/BatchCoprocessorHandler.h>
#include <Flash/FlashService.h>
#include <Flash/Management/ManualCompact.h>
#include <Flash/Mpp/MPPHandler.h>
#include <Flash/Mpp/MPPTaskManager.h>
#include <Flash/Mpp/Utils.h>
#include <Flash/ServiceUtils.h>
#include <Interpreters/Context.h>
#include <Server/IServer.h>
#include <Storages/IManageableStorage.h>
#include <Storages/Transaction/TMTContext.h>
#include <grpcpp/server_builder.h>

#include <ext/scope_guard.h>

namespace DB
{
namespace ErrorCodes
{
extern const int NOT_IMPLEMENTED;
}

#define CATCH_FLASHSERVICE_EXCEPTION                                                                                                        \
    catch (Exception & e)                                                                                                                   \
    {                                                                                                                                       \
        LOG_FMT_ERROR(log, "DB Exception: {}", e.message());                                                                                \
        return std::make_tuple(std::make_shared<Context>(*context), grpc::Status(tiflashErrorCodeToGrpcStatusCode(e.code()), e.message())); \
    }                                                                                                                                       \
    catch (const std::exception & e)                                                                                                        \
    {                                                                                                                                       \
        LOG_FMT_ERROR(log, "std exception: {}", e.what());                                                                                  \
        return std::make_tuple(std::make_shared<Context>(*context), grpc::Status(grpc::StatusCode::INTERNAL, e.what()));                    \
    }                                                                                                                                       \
    catch (...)                                                                                                                             \
    {                                                                                                                                       \
        LOG_FMT_ERROR(log, "other exception");                                                                                              \
        return std::make_tuple(std::make_shared<Context>(*context), grpc::Status(grpc::StatusCode::INTERNAL, "other exception"));           \
    }

constexpr char tls_err_msg[] = "common name check is failed";

FlashService::FlashService() = default;

void FlashService::init(const TiFlashSecurityConfig & security_config_, Context & context_)
{
    security_config = &security_config_;
    context = &context_;
    log = &Poco::Logger::get("FlashService");
    manual_compact_manager = std::make_unique<Management::ManualCompactManager>(
        context->getGlobalContext(),
        context->getGlobalContext().getSettingsRef());

    auto settings = context->getSettingsRef();
    enable_local_tunnel = settings.enable_local_tunnel;
    enable_async_grpc_client = settings.enable_async_grpc_client;
    const size_t default_size = 2 * getNumberOfPhysicalCPUCores();

    auto cop_pool_size = static_cast<size_t>(settings.cop_pool_size);
    cop_pool_size = cop_pool_size ? cop_pool_size : default_size;
    LOG_FMT_INFO(log, "Use a thread pool with {} threads to handle cop requests.", cop_pool_size);
    cop_pool = std::make_unique<ThreadPool>(cop_pool_size, [] { setThreadName("cop-pool"); });

    auto batch_cop_pool_size = static_cast<size_t>(settings.batch_cop_pool_size);
    batch_cop_pool_size = batch_cop_pool_size ? batch_cop_pool_size : default_size;
    LOG_FMT_INFO(log, "Use a thread pool with {} threads to handle batch cop requests.", batch_cop_pool_size);
    batch_cop_pool = std::make_unique<ThreadPool>(batch_cop_pool_size, [] { setThreadName("batch-cop-pool"); });
}

FlashService::~FlashService() = default;

// Use executeInThreadPool to submit job to thread pool which return grpc::Status.
grpc::Status executeInThreadPool(ThreadPool & pool, std::function<grpc::Status()> job)
{
    std::packaged_task<grpc::Status()> task(job);
    std::future<grpc::Status> future = task.get_future();
    pool.schedule([&task] { task(); });
    return future.get();
}

grpc::Status FlashService::Coprocessor(
    grpc::ServerContext * grpc_context,
    const coprocessor::Request * request,
    coprocessor::Response * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    LOG_FMT_DEBUG(log, "Handling coprocessor request: {}", request->DebugString());

    // For coprocessor test, we don't care about security config.
    if (unlikely(!context->isCopTest() && !security_config->checkGrpcContext(grpc_context)))
    {
        return grpc::Status(grpc::PERMISSION_DENIED, tls_err_msg);
    }

    GET_METRIC(tiflash_coprocessor_request_count, type_cop).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_cop).Increment();
    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_cop).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_cop).Observe(watch.elapsedSeconds());
        GET_METRIC(tiflash_coprocessor_response_bytes).Increment(response->ByteSizeLong());
    });

    context->setMockStorage(mock_storage);

    grpc::Status ret = executeInThreadPool(*cop_pool, [&] {
        auto [db_context, status] = createDBContext(grpc_context);
        if (!status.ok())
        {
            return status;
        }
        CoprocessorContext cop_context(*db_context, request->context(), *grpc_context);
        CoprocessorHandler cop_handler(cop_context, request, response);
        return cop_handler.execute();
    });

    LOG_FMT_DEBUG(log, "Handle coprocessor request done: {}, {}", ret.error_code(), ret.error_message());
    return ret;
}

grpc::Status FlashService::BatchCoprocessor(grpc::ServerContext * grpc_context, const coprocessor::BatchRequest * request, grpc::ServerWriter<coprocessor::BatchResponse> * writer)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    LOG_FMT_DEBUG(log, "Handling coprocessor request: {}", request->DebugString());

    if (!security_config->checkGrpcContext(grpc_context))
    {
        return grpc::Status(grpc::PERMISSION_DENIED, tls_err_msg);
    }

    GET_METRIC(tiflash_coprocessor_request_count, type_super_batch).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_super_batch).Increment();
    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_super_batch).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_super_batch).Observe(watch.elapsedSeconds());
        // TODO: update the value of metric tiflash_coprocessor_response_bytes.
    });

    grpc::Status ret = executeInThreadPool(*batch_cop_pool, [&] {
        auto [db_context, status] = createDBContext(grpc_context);
        if (!status.ok())
        {
            return status;
        }
        CoprocessorContext cop_context(*db_context, request->context(), *grpc_context);
        BatchCoprocessorHandler cop_handler(cop_context, request, writer);
        return cop_handler.execute();
    });

    LOG_FMT_DEBUG(log, "Handle coprocessor request done: {}, {}", ret.error_code(), ret.error_message());
    return ret;
}

grpc::Status FlashService::DispatchMPPTask(
    grpc::ServerContext * grpc_context,
    const mpp::DispatchTaskRequest * request,
    mpp::DispatchTaskResponse * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    LOG_FMT_DEBUG(log, "Handling mpp dispatch request: {}", request->DebugString());
    // For MPP test, we don't care about security config.
    if (!context->isMPPTest() && !security_config->checkGrpcContext(grpc_context))
    {
        return grpc::Status(grpc::PERMISSION_DENIED, tls_err_msg);
    }
    GET_METRIC(tiflash_coprocessor_request_count, type_dispatch_mpp_task).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_dispatch_mpp_task).Increment();
    GET_METRIC(tiflash_thread_count, type_active_threads_of_dispatch_mpp).Increment();
    GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Increment();
    if (!tryToResetMaxThreadsMetrics())
    {
        GET_METRIC(tiflash_thread_count, type_max_threads_of_dispatch_mpp).Set(std::max(GET_METRIC(tiflash_thread_count, type_max_threads_of_dispatch_mpp).Value(), GET_METRIC(tiflash_thread_count, type_active_threads_of_dispatch_mpp).Value()));
        GET_METRIC(tiflash_thread_count, type_max_threads_of_raw).Set(std::max(GET_METRIC(tiflash_thread_count, type_max_threads_of_raw).Value(), GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Value()));
    }

    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Decrement();
        GET_METRIC(tiflash_thread_count, type_active_threads_of_dispatch_mpp).Decrement();
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_dispatch_mpp_task).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_dispatch_mpp_task).Observe(watch.elapsedSeconds());
        GET_METRIC(tiflash_coprocessor_response_bytes).Increment(response->ByteSizeLong());
    });

    auto [db_context, status] = createDBContext(grpc_context);
    if (!status.ok())
    {
        return status;
    }
    db_context->setMockStorage(mock_storage);
    db_context->setMockMPPServerInfo(mpp_test_info);

    MPPHandler mpp_handler(*request);
    return mpp_handler.execute(db_context, response);
}

grpc::Status FlashService::IsAlive(grpc::ServerContext * grpc_context [[maybe_unused]],
                                   const mpp::IsAliveRequest * request [[maybe_unused]],
                                   mpp::IsAliveResponse * response [[maybe_unused]])
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    if (!security_config->checkGrpcContext(grpc_context))
    {
        return grpc::Status(grpc::PERMISSION_DENIED, tls_err_msg);
    }

    auto [db_context, status] = createDBContext(grpc_context);
    if (!status.ok())
    {
        return status;
    }

    auto & tmt_context = db_context->getTMTContext();
    response->set_available(tmt_context.checkRunning());
    return grpc::Status::OK;
}

std::variant<grpc::Status, std::string> FlashService::establishMPPConnectionSyncOrAsync(grpc::ServerContext * grpc_context,
                                                                                        const mpp::EstablishMPPConnectionRequest * request,
                                                                                        grpc::ServerWriter<mpp::MPPDataPacket> * sync_writer,
                                                                                        IAsyncCallData * call_data)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    // Establish a pipe for data transferring. The pipes have registered by the task in advance.
    // We need to find it out and bind the grpc stream with it.
    LOG_FMT_DEBUG(log, "Handling establish mpp connection request: {}", request->DebugString());

    // For MPP test, we don't care about security config.
    if (!context->isMPPTest() && !security_config->checkGrpcContext(grpc_context))
    {
        return grpc::Status(grpc::PERMISSION_DENIED, tls_err_msg);
    }
    GET_METRIC(tiflash_coprocessor_request_count, type_mpp_establish_conn).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_mpp_establish_conn).Increment();
    GET_METRIC(tiflash_thread_count, type_active_threads_of_establish_mpp).Increment();
    GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Increment();
    if (!tryToResetMaxThreadsMetrics())
    {
        GET_METRIC(tiflash_thread_count, type_max_threads_of_establish_mpp).Set(std::max(GET_METRIC(tiflash_thread_count, type_max_threads_of_establish_mpp).Value(), GET_METRIC(tiflash_thread_count, type_active_threads_of_establish_mpp).Value()));
        GET_METRIC(tiflash_thread_count, type_max_threads_of_raw).Set(std::max(GET_METRIC(tiflash_thread_count, type_max_threads_of_raw).Value(), GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Value()));
    }
    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Decrement();
        GET_METRIC(tiflash_thread_count, type_active_threads_of_establish_mpp).Decrement();
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_mpp_establish_conn).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_mpp_establish_conn).Observe(watch.elapsedSeconds());
        // TODO: update the value of metric tiflash_coprocessor_response_bytes.
    });

    auto [db_context, status] = createDBContext(grpc_context);
    if (!status.ok())
    {
        return status;
    }

    auto & tmt_context = db_context->getTMTContext();
    auto task_manager = tmt_context.getMPPTaskManager();
    std::chrono::seconds timeout(10);
    auto [tunnel, err_msg] = task_manager->findTunnelWithTimeout(request, timeout);
    if (tunnel == nullptr)
    {
        LOG_ERROR(log, err_msg);
        return err_msg;
    }

    if (call_data)
    {
        // In async mode, this function won't wait for the request done and the finish event is handled in IAsyncCallData.
        tunnel->connectAsync(call_data);
    }
    else
    {
        Stopwatch stopwatch;
        SyncPacketWriter writer(sync_writer);
        tunnel->connect(&writer);
        tunnel->waitForFinish();
        LOG_FMT_INFO(tunnel->getLogger(), "connection for {} cost {} ms.", tunnel->id(), stopwatch.elapsedMilliseconds());
    }

    // TODO: Check if there are errors in task.

    return grpc::Status::OK;
}

grpc::Status FlashService::EstablishMPPConnection(grpc::ServerContext * grpc_context, const mpp::EstablishMPPConnectionRequest * request, grpc::ServerWriter<mpp::MPPDataPacket> * sync_writer)
{
    auto res = establishMPPConnectionSyncOrAsync(grpc_context, request, sync_writer, nullptr);
    grpc::Status status;
    std::visit(variant_op::overloaded{
                   [&](grpc::Status & stat) {
                       status = stat;
                   },
                   [&](std::string & err_msg) {
                       if (!sync_writer->Write(getPacketWithError(err_msg)))
                       {
                           status = grpc::Status::OK;
                       }
                       else
                       {
                           LOG_FMT_DEBUG(log, "Write error message failed for unknown reason.");
                           status = grpc::Status(grpc::StatusCode::UNKNOWN, "Write error message failed for unknown reason.");
                       }
                   }},
               res);
    return status;
}

grpc::Status FlashService::CancelMPPTask(
    grpc::ServerContext * grpc_context,
    const mpp::CancelTaskRequest * request,
    mpp::CancelTaskResponse * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    // CancelMPPTask cancels the query of the task.
    LOG_FMT_DEBUG(log, "cancel mpp task request: {}", request->DebugString());

    if (!security_config->checkGrpcContext(grpc_context))
    {
        return grpc::Status(grpc::PERMISSION_DENIED, tls_err_msg);
    }
    GET_METRIC(tiflash_coprocessor_request_count, type_cancel_mpp_task).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_cancel_mpp_task).Increment();
    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_cancel_mpp_task).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_cancel_mpp_task).Observe(watch.elapsedSeconds());
        GET_METRIC(tiflash_coprocessor_response_bytes).Increment(response->ByteSizeLong());
    });

    auto [db_context, status] = createDBContext(grpc_context);
    if (!status.ok())
    {
        auto err = std::make_unique<mpp::Error>();
        err->set_msg("error status");
        response->set_allocated_error(err.release());
        return status;
    }
    auto & tmt_context = db_context->getTMTContext();
    auto task_manager = tmt_context.getMPPTaskManager();
    task_manager->abortMPPQuery(request->meta().start_ts(), "Receive cancel request from TiDB", AbortType::ONCANCELLATION);
    return grpc::Status::OK;
}

std::tuple<ContextPtr, grpc::Status> FlashService::createDBContextForTest() const
{
    try
    {
        /// Create DB context.
        auto tmp_context = std::make_shared<Context>(*context);
        tmp_context->setGlobalContext(*context);

        String query_id;
        tmp_context->setCurrentQueryId(query_id);
        ClientInfo & client_info = tmp_context->getClientInfo();
        client_info.query_kind = ClientInfo::QueryKind::INITIAL_QUERY;
        client_info.interface = ClientInfo::Interface::GRPC;

        String max_threads;
        tmp_context->setSetting("enable_async_server", is_async ? "true" : "false");
        tmp_context->setSetting("enable_local_tunnel", enable_local_tunnel ? "true" : "false");
        tmp_context->setSetting("enable_async_grpc_client", enable_async_grpc_client ? "true" : "false");
        return std::make_tuple(tmp_context, grpc::Status::OK);
    }
    CATCH_FLASHSERVICE_EXCEPTION
}

::grpc::Status FlashService::cancelMPPTaskForTest(const ::mpp::CancelTaskRequest * request, ::mpp::CancelTaskResponse * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    // CancelMPPTask cancels the query of the task.
    LOG_FMT_DEBUG(log, "cancel mpp task request: {}", request->DebugString());
    auto [context, status] = createDBContextForTest();
    if (!status.ok())
    {
        auto err = std::make_unique<mpp::Error>();
        err->set_msg("error status");
        response->set_allocated_error(err.release());
        return status;
    }
    auto & tmt_context = context->getTMTContext();
    auto task_manager = tmt_context.getMPPTaskManager();
    task_manager->abortMPPQuery(request->meta().start_ts(), "Receive cancel request from GTest", AbortType::ONCANCELLATION);
    return grpc::Status::OK;
}

String getClientMetaVarWithDefault(const grpc::ServerContext * grpc_context, const String & name, const String & default_val)
{
    if (auto it = grpc_context->client_metadata().find(name); it != grpc_context->client_metadata().end())
        return String(it->second.data(), it->second.size());

    return default_val;
}

std::tuple<ContextPtr, grpc::Status> FlashService::createDBContext(const grpc::ServerContext * grpc_context) const
{
    try
    {
        /// Create DB context.
        auto tmp_context = std::make_shared<Context>(*context);
        tmp_context->setGlobalContext(*context);

        /// Set a bunch of client information.
        std::string user = getClientMetaVarWithDefault(grpc_context, "user", "default");
        std::string password = getClientMetaVarWithDefault(grpc_context, "password", "");
        std::string quota_key = getClientMetaVarWithDefault(grpc_context, "quota_key", "");
        std::string peer = grpc_context->peer();
        Int64 pos = peer.find(':');
        if (pos == -1)
        {
            return std::make_tuple(tmp_context, ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "Invalid peer address: " + peer));
        }
        std::string client_ip = peer.substr(pos + 1);
        Poco::Net::SocketAddress client_address(client_ip);

        // For MPP or Cop test, we don't care about security config.
        if (likely(!context->isTest()))
            tmp_context->setUser(user, password, client_address, quota_key);

        String query_id = getClientMetaVarWithDefault(grpc_context, "query_id", "");
        tmp_context->setCurrentQueryId(query_id);

        ClientInfo & client_info = tmp_context->getClientInfo();
        client_info.query_kind = ClientInfo::QueryKind::INITIAL_QUERY;
        client_info.interface = ClientInfo::Interface::GRPC;

        /// Set DAG parameters.
        std::string dag_records_per_chunk_str = getClientMetaVarWithDefault(grpc_context, "dag_records_per_chunk", "");
        if (!dag_records_per_chunk_str.empty())
        {
            tmp_context->setSetting("dag_records_per_chunk", dag_records_per_chunk_str);
        }

        String max_threads = getClientMetaVarWithDefault(grpc_context, "tidb_max_tiflash_threads", "");
        if (!max_threads.empty())
        {
            tmp_context->setSetting("max_threads", max_threads);
            LOG_FMT_INFO(log, "set context setting max_threads to {}", max_threads);
        }

        tmp_context->setSetting("enable_async_server", is_async ? "true" : "false");
        tmp_context->setSetting("enable_local_tunnel", enable_local_tunnel ? "true" : "false");
        tmp_context->setSetting("enable_async_grpc_client", enable_async_grpc_client ? "true" : "false");
        return std::make_tuple(tmp_context, grpc::Status::OK);
    }
    CATCH_FLASHSERVICE_EXCEPTION
}

grpc::Status FlashService::Compact(grpc::ServerContext * grpc_context, const kvrpcpb::CompactRequest * request, kvrpcpb::CompactResponse * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    if (!security_config->checkGrpcContext(grpc_context))
    {
        return grpc::Status(grpc::PERMISSION_DENIED, tls_err_msg);
    }

    return manual_compact_manager->handleRequest(request, response);
}

void FlashService::setMockStorage(MockStorage & mock_storage_)
{
    mock_storage = mock_storage_;
}

void FlashService::setMockMPPServerInfo(MockMPPServerInfo & mpp_test_info_)
{
    mpp_test_info = mpp_test_info_;
}
} // namespace DB