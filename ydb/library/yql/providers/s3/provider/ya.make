LIBRARY() 
 
OWNER( 
    g:yq
    g:yql 
) 
 
SRCS( 
    yql_s3_datasink.cpp 
    yql_s3_datasink_execution.cpp 
    yql_s3_datasink_type_ann.cpp 
    yql_s3_datasource.cpp 
    yql_s3_datasource_type_ann.cpp 
    yql_s3_dq_integration.cpp 
    yql_s3_exec.cpp 
    yql_s3_io_discovery.cpp 
    yql_s3_logical_opt.cpp 
    yql_s3_mkql_compiler.cpp 
    yql_s3_provider.cpp 
    yql_s3_provider_impl.cpp 
    yql_s3_settings.cpp 
) 
 
PEERDIR( 
    contrib/libs/re2
    library/cpp/json
    library/cpp/random_provider
    library/cpp/string_utils/quote
    library/cpp/time_provider
    library/cpp/xml/document
    ydb/library/yql/ast
    ydb/library/yql/minikql
    ydb/library/yql/minikql/comp_nodes
    ydb/library/yql/minikql/computation
    ydb/library/yql/providers/common/structured_token
    ydb/library/yql/providers/common/token_accessor/client
    ydb/library/yql/core
    ydb/library/yql/core/type_ann
    ydb/library/yql/dq/expr_nodes
    ydb/library/yql/providers/common/config
    ydb/library/yql/providers/common/dq
    ydb/library/yql/providers/common/http_gateway
    ydb/library/yql/providers/common/mkql
    ydb/library/yql/providers/common/proto
    ydb/library/yql/providers/common/provider
    ydb/library/yql/providers/common/schema/expr
    ydb/library/yql/providers/common/transform
    ydb/library/yql/providers/dq/common
    ydb/library/yql/providers/dq/expr_nodes
    ydb/library/yql/providers/dq/interface
    ydb/library/yql/providers/result/expr_nodes
    ydb/library/yql/providers/s3/expr_nodes
    ydb/library/yql/providers/s3/proto
) 
 
YQL_LAST_ABI_VERSION() 
 
END() 
