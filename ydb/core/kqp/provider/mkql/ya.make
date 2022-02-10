LIBRARY() 
 
OWNER(spuchin) 
 
SRCS( 
) 
 
PEERDIR( 
) 
 
SRCDIR( 
    ydb/library/yql/core/expr_nodes_gen
) 
 
RUN_PROGRAM( 
    ydb/library/yql/core/expr_nodes_gen/gen
        yql_expr_nodes_gen.jnj 
        yql_kikimr_mkql_expr_nodes.json 
        yql_kikimr_mkql_expr_nodes.gen.h 
        yql_kikimr_mkql_expr_nodes.decl.inl.h 
        yql_kikimr_mkql_expr_nodes.defs.inl.h 
    IN yql_expr_nodes_gen.jnj 
    IN yql_kikimr_mkql_expr_nodes.json 
    OUT yql_kikimr_mkql_expr_nodes.gen.h 
    OUT yql_kikimr_mkql_expr_nodes.decl.inl.h 
    OUT yql_kikimr_mkql_expr_nodes.defs.inl.h 
    OUTPUT_INCLUDES 
    ${ARCADIA_ROOT}/ydb/library/yql/core/expr_nodes_gen/yql_expr_nodes_gen.h
    ${ARCADIA_ROOT}/util/generic/hash_set.h 
) 
 
END() 
 
