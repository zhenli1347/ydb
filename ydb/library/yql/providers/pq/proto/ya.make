PROTO_LIBRARY()

OWNER(
    galaxycrab
    g:yq
    g:yql
)

SRCS(
    dq_io.proto
    dq_io_state.proto
    dq_task_params.proto
)

EXCLUDE_TAGS(GO_PROTO)

END()
