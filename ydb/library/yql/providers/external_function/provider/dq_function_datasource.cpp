#include "dq_function_provider_impl.h"

#include <ydb/library/yql/core/expr_nodes/yql_expr_nodes.h>
#include <ydb/library/yql/core/yql_expr_type_annotation.h>
#include <ydb/library/yql/providers/common/provider/yql_data_provider_impl.h>
#include <ydb/library/yql/providers/common/provider/yql_provider_names.h>

namespace NYql::NDqFunction {

namespace {

using namespace NNodes;

class TDqFunctionDataSource : public TDataProviderBase {
public:
    TDqFunctionDataSource(TDqFunctionState::TPtr state)
        : State(std::move(state))
        , LoadMetaDataTransformer(CreateDqFunctionMetaLoader(State))
        , IntentDeterminationTransformer(CreateDqFunctionIntentTransformer(State))
    {}

    TStringBuf GetName() const override {
        return FunctionProviderName;
    }

    bool ValidateParameters(TExprNode& node, TExprContext& ctx, TMaybe<TString>& cluster) override {
        if (node.IsCallable(TCoDataSource::CallableName())) {
            //(DataSource 'ExternalFunction 'CloudFunction 'my_function_name 'connection_name)
            if (!EnsureArgsCount(node, 4, ctx)) {
                return false;
            }
            if (node.Head().Content() == FunctionProviderName) {
                TDqFunctionType functionType{node.Child(1)->Content()};
                if (!State->GatewayFactory->IsKnownFunctionType(functionType)) {
                    ctx.AddError(TIssue(ctx.GetPosition(node.Pos()), TStringBuilder() <<
                        "Unknown EXTERNAL FUNCTION type '" << functionType << "'"));
                    return false;
                }
                cluster = Nothing();
                return true;
            }
        }

        ctx.AddError(TIssue(ctx.GetPosition(node.Pos()), "Invalid ExternalFunction DataSource parameters"));
        return false;
    }

    IGraphTransformer& GetLoadTableMetadataTransformer() override {
        return *LoadMetaDataTransformer;
    }

    IGraphTransformer& GetIntentDeterminationTransformer() override {
        return *IntentDeterminationTransformer;
    }

private:
    const TDqFunctionState::TPtr State;
    const THolder<IGraphTransformer> LoadMetaDataTransformer;
    const THolder<TVisitorTransformerBase> IntentDeterminationTransformer;
};
}


TIntrusivePtr<IDataProvider> CreateDqFunctionDataSource(TDqFunctionState::TPtr state) {
    return new TDqFunctionDataSource(std::move(state));
}



}