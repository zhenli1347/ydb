LIBRARY()

OWNER(
    vvvv
    g:kikimr
    g:yql
    g:yql_ydb_core
)

SRCS(
    aligned_page_pool.cpp
    aligned_page_pool.h
    compact_hash.cpp
    compact_hash.h
    defs.h
    mkql_alloc.cpp
    mkql_function_metadata.h 
    mkql_function_registry.cpp
    mkql_function_registry.h
    mkql_node.cpp
    mkql_node.h
    mkql_node_builder.cpp
    mkql_node_builder.h
    mkql_node_cast.cpp
    mkql_node_cast.h
    mkql_node_printer.cpp
    mkql_node_printer.h
    mkql_node_serialization.cpp
    mkql_node_serialization.h
    mkql_node_visitor.cpp
    mkql_node_visitor.h
    mkql_opt_literal.cpp
    mkql_opt_literal.h
    mkql_program_builder.cpp
    mkql_program_builder.h
    mkql_runtime_version.cpp
    mkql_runtime_version.h
    mkql_stats_registry.cpp
    mkql_string_util.cpp 
    mkql_string_util.h 
    mkql_terminator.cpp 
    mkql_terminator.h 
    mkql_type_builder.cpp
    mkql_type_builder.h
    mkql_type_ops.cpp
    mkql_type_ops.h
    mkql_unboxed_value_stream.cpp 
    mkql_unboxed_value_stream.h 
    primes.cpp
    primes.h
    watermark_tracker.cpp
    watermark_tracker.h
)

PEERDIR(
    contrib/libs/apache/arrow
    contrib/libs/cctz/tzdata
    library/cpp/actors/util
    library/cpp/deprecated/enum_codegen
    library/cpp/enumbitset
    library/cpp/monlib/dynamic_counters
    library/cpp/packedtypes
    library/cpp/resource
    library/cpp/yson
    ydb/library/binary_json
    ydb/library/dynumber
    ydb/library/yql/minikql/dom
    ydb/library/yql/public/udf
    ydb/library/yql/public/udf/tz
    ydb/library/yql/utils
)

IF (MKQL_RUNTIME_VERSION)
    CFLAGS(
        -DMKQL_RUNTIME_VERSION=$MKQL_RUNTIME_VERSION
    )
ENDIF()

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
