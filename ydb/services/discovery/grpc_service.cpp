#include "grpc_service.h"

#include <ydb/core/grpc_services/grpc_helper.h>
#include <ydb/core/grpc_services/grpc_request_proxy.h>
#include <ydb/core/grpc_services/rpc_calls.h>

namespace NKikimr {
namespace NGRpcService {

static TString GetSdkBuildInfo(NGrpc::IRequestContextBase* reqCtx) {
    const auto& res = reqCtx->GetPeerMetaValues(NYdb::YDB_SDK_BUILD_INFO_HEADER); 
    if (res.empty()) { 
        return {}; 
    } 
    return TString{res[0]};
} 
 
TGRpcDiscoveryService::TGRpcDiscoveryService(NActors::TActorSystem *system,
                                 TIntrusivePtr<NMonitoring::TDynamicCounters> counters,
                                 NActors::TActorId id)
     : ActorSystem_(system)
     , Counters_(counters)
     , GRpcRequestProxyId_(id)
 {
 }

 void TGRpcDiscoveryService::InitService(grpc::ServerCompletionQueue *cq, NGrpc::TLoggerPtr logger) {
     CQ_ = cq;
     SetupIncomingRequests(std::move(logger));
 }

 void TGRpcDiscoveryService::SetGlobalLimiterHandle(NGrpc::TGlobalLimiter *limiter) {
     Limiter_ = limiter;
 }

 bool TGRpcDiscoveryService::IncRequest() {
     return Limiter_->Inc();
 }

 void TGRpcDiscoveryService::DecRequest() {
     Limiter_->Dec();
     Y_ASSERT(Limiter_->GetCurrentInFlight() >= 0);
 }

 void TGRpcDiscoveryService::SetupIncomingRequests(NGrpc::TLoggerPtr logger) {
     auto getCounterBlock = CreateCounterCb(Counters_, ActorSystem_);
 #ifdef ADD_REQUEST
 #error ADD_REQUEST macro already defined
 #endif
 #define ADD_REQUEST(NAME, IN, OUT, ACTION) \
     MakeIntrusive<TGRpcRequest<Ydb::Discovery::IN, Ydb::Discovery::OUT, TGRpcDiscoveryService>>(this, &Service_, CQ_, \ 
         [this](NGrpc::IRequestContextBase *reqCtx) { \
            NGRpcService::ReportGrpcReqToMon(*ActorSystem_, reqCtx->GetPeer(), GetSdkBuildInfo(reqCtx)); \ 
            ACTION; \
         }, &Ydb::Discovery::V1::DiscoveryService::AsyncService::Request ## NAME, \
         #NAME, logger, getCounterBlock("discovery", #NAME))->Run();

     ADD_REQUEST(ListEndpoints, ListEndpointsRequest, ListEndpointsResponse, {
         ActorSystem_->Send(GRpcRequestProxyId_, new TEvListEndpointsRequest(reqCtx));
     })
     ADD_REQUEST(WhoAmI, WhoAmIRequest, WhoAmIResponse, {
         ActorSystem_->Send(GRpcRequestProxyId_, new TEvWhoAmIRequest(reqCtx));
     })

 #undef ADD_REQUEST
 }

} // namespace NGRpcService
} // namespace NKikimr
