#include "grpc_request_proxy.h"

#include "rpc_calls.h"
#include "rpc_kqp_base.h"
#include "rpc_common.h" 

#include <ydb/library/yql/public/issue/yql_issue_message.h>
#include <ydb/library/yql/public/issue/yql_issue.h>

namespace NKikimr {
namespace NGRpcService {

using namespace NActors;
using namespace Ydb;
using namespace NKqp;

class TBeginTransactionRPC : public TRpcKqpRequestActor<TBeginTransactionRPC, TEvBeginTransactionRequest> {
    using TBase = TRpcKqpRequestActor<TBeginTransactionRPC, TEvBeginTransactionRequest>;

public:
    using TResult = Ydb::Table::BeginTransactionResult; 

    TBeginTransactionRPC(TEvBeginTransactionRequest* msg)
        : TBase(msg) {}

    void Bootstrap(const TActorContext& ctx) {
        TBase::Bootstrap(ctx);

        BeginTransactionImpl(ctx);
        Become(&TBeginTransactionRPC::StateWork);
    }

    void StateWork(TAutoPtr<IEventHandle>& ev, const TActorContext& ctx) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NKqp::TEvKqp::TEvQueryResponse, Handle);
            default: TBase::StateWork(ev, ctx);
        }
    }

private:
    void BeginTransactionImpl(const TActorContext &ctx) {
        const auto req = GetProtoRequest(); 
        const auto traceId = Request_->GetTraceId();

        TString sessionId;
        auto ev = MakeHolder<NKqp::TEvKqp::TEvQueryRequest>();
        SetAuthToken(ev, *Request_); 
        SetDatabase(ev, *Request_); 
 
        NYql::TIssues issues;
        if (CheckSession(req->session_id(), issues)) {
            ev->Record.MutableRequest()->SetSessionId(req->session_id());
        } else {
            return Reply(Ydb::StatusIds::BAD_REQUEST, issues, ctx);
        }

        if (traceId) {
            ev->Record.SetTraceId(traceId.GetRef());
        }

        if (!req->has_tx_settings()) {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Empty tx_settings."));
            return Reply(Ydb::StatusIds::BAD_REQUEST, issues, ctx);
        }

        switch (req->tx_settings().tx_mode_case()) {
            case Table::TransactionSettings::kOnlineReadOnly:
            case Table::TransactionSettings::kStaleReadOnly: {
                NYql::TIssues issues;
                issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, TStringBuilder()
                    << "Failed to begin transaction: open transactions not supported for transaction mode "
                    << GetTransactionModeName(req->tx_settings())
                    << ", use tx_control field in ExecuteDataQueryRequest to begin transaction with this mode."));
                return Reply(Ydb::StatusIds::BAD_REQUEST, issues, ctx);
            }
            default:
                break;
        }

        ev->Record.MutableRequest()->SetAction(NKikimrKqp::QUERY_ACTION_BEGIN_TX);
        ev->Record.MutableRequest()->MutableTxControl()->mutable_begin_tx()->CopyFrom(req->tx_settings());
        ctx.Send(NKqp::MakeKqpProxyID(ctx.SelfID.NodeId()), ev.Release());
    }

    void Handle(NKqp::TEvKqp::TEvQueryResponse::TPtr& ev, const TActorContext& ctx) {
        const auto& record = ev->Get()->Record.GetRef();
        SetCost(record.GetConsumedRu()); 
        AddServerHintsIfAny(record);
 
        if (record.GetYdbStatus() == Ydb::StatusIds::SUCCESS) {
            const auto& kqpResponse = record.GetResponse();
            const auto& issueMessage = kqpResponse.GetQueryIssues();

            auto beginTxResult = TEvBeginTransactionRequest::AllocateResult<Ydb::Table::BeginTransactionResult>(Request_); 
            if (kqpResponse.HasTxMeta()) {
                beginTxResult->mutable_tx_meta()->CopyFrom(kqpResponse.GetTxMeta());
            }

            ReplyWithResult(Ydb::StatusIds::SUCCESS, issueMessage, *beginTxResult, ctx);
        } else {
            return OnQueryResponseErrorWithTxMeta(record, ctx);
        }
    }
};

void TGRpcRequestProxy::Handle(TEvBeginTransactionRequest::TPtr& ev, const TActorContext& ctx) {
    ctx.Register(new TBeginTransactionRPC(ev->Release().Release()));
}

} // namespace NGRpcService
} // namespace NKikimr
