LIBRARY(run)

OWNER(
    ddoarn
    g:kikimr
)

IF (PROFILE_MEMORY_ALLOCATIONS)
    CFLAGS(
        -DPROFILE_MEMORY_ALLOCATIONS
    )
ENDIF()

IF (KIKIMR_UDF_DYNAMIC_LINK)
    CFLAGS(
        -DKIKIMR_UDF_DYNAMIC_LINK
    )
ENDIF()

SRCS(
    config.cpp
    config.h
    config_parser.cpp
    config_parser.h
    driver.h
    dummy.cpp
    dummy.h
    factories.h
    factories.cpp
    kikimr_services_initializers.cpp
    kikimr_services_initializers.h
    log_backend.cpp
    log_backend.h
    main.h
    main.cpp
    run.cpp
    run.h
    service_initializer.cpp
    service_initializer.h
    version.cpp
    version.h
)

PEERDIR(
    contrib/libs/protobuf
    library/cpp/actors/core
    library/cpp/actors/dnsresolver
    library/cpp/actors/interconnect
    library/cpp/actors/memory_log
    library/cpp/actors/prof
    library/cpp/actors/protos
    library/cpp/actors/util
    library/cpp/getopt/small
    library/cpp/grpc/client
    library/cpp/grpc/server
    library/cpp/grpc/server/actors
    library/cpp/logger
    library/cpp/malloc/api
    library/cpp/messagebus
    library/cpp/monlib/dynamic_counters
    library/cpp/monlib/messagebus
    library/cpp/sighandler
    library/cpp/string_utils/parse_size
    library/cpp/svnversion
    ydb/core/actorlib_impl
    ydb/core/base
    ydb/core/blobstorage
    ydb/core/blobstorage/backpressure
    ydb/core/blobstorage/nodewarden
    ydb/core/blobstorage/other
    ydb/core/blobstorage/pdisk
    ydb/core/blobstorage/testload
    ydb/core/blobstorage/vdisk/common
    ydb/core/client/minikql_compile
    ydb/core/client/scheme_cache_lib
    ydb/core/client/server
    ydb/core/cms
    ydb/core/cms/console
    ydb/core/control
    ydb/core/driver_lib/base_utils
    ydb/core/driver_lib/cli_config_base
    ydb/core/driver_lib/cli_utils
    ydb/core/formats
    ydb/core/grpc_services
    ydb/core/health_check
    ydb/core/kesus/proxy
    ydb/core/kesus/tablet
    ydb/core/keyvalue
    ydb/core/kqp
    ydb/core/kqp/rm
    ydb/core/metering
    ydb/core/mind
    ydb/core/mind/address_classification
    ydb/core/mind/bscontroller
    ydb/core/mind/hive
    ydb/core/mon
    ydb/core/mon_alloc
    ydb/core/node_whiteboard
    ydb/core/persqueue
    ydb/core/protos
    ydb/core/quoter
    ydb/core/scheme
    ydb/core/scheme_types
    ydb/core/security
    ydb/core/sys_view/processor
    ydb/core/sys_view/service
    ydb/core/tablet
    ydb/core/tablet_flat
    ydb/core/test_tablet
    ydb/core/tracing
    ydb/core/tx
    ydb/core/tx/columnshard
    ydb/core/tx/coordinator
    ydb/core/tx/datashard
    ydb/core/tx/long_tx_service
    ydb/core/tx/long_tx_service/public
    ydb/core/tx/mediator
    ydb/core/tx/replication/controller
    ydb/core/tx/scheme_board
    ydb/core/tx/schemeshard
    ydb/core/tx/sequenceproxy
    ydb/core/tx/sequenceshard
    ydb/core/tx/time_cast
    ydb/core/tx/tx_allocator
    ydb/core/tx/tx_proxy
    ydb/core/util
    ydb/core/viewer
    ydb/core/ymq/actor
    ydb/core/ymq/http
    ydb/core/yq/libs/init
    ydb/core/yq/libs/logs
    ydb/library/folder_service
    ydb/library/folder_service/proto
    ydb/library/pdisk_io
    ydb/library/security
    ydb/library/yql/minikql/comp_nodes
    ydb/library/yql/public/udf/service/exception_policy
    ydb/public/lib/base
    ydb/public/lib/deprecated/client
    ydb/services/auth
    ydb/services/cms
    ydb/services/datastreams
    ydb/services/discovery
    ydb/services/kesus
    ydb/services/monitoring
    ydb/services/persqueue_cluster_discovery
    ydb/services/persqueue_v1
    ydb/services/rate_limiter
    ydb/services/ydb
    ydb/services/yq
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
 
RECURSE_ROOT_RELATIVE( 
    ydb/core
    ydb/services
) 
