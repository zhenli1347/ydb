#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

namespace NKikimr {
namespace NKqp {

using namespace NYdb;
using namespace NYdb::NTable;

static const ui32 LargeTableShards = 8;
static const ui32 LargeTableKeysPerShard = 1000000;

static void CreateLargeTable(TKikimrRunner& kikimr, ui32 rowsPerShard, ui32 keyTextSize,
    ui32 dataTextSize, ui32 batchSizeRows = 100, ui32 fillShardsCount = LargeTableShards)
{
    kikimr.GetTestClient().CreateTable("/Root", R"(
        Name: "LargeTable"
        Columns { Name: "Key", Type: "Uint64" }
        Columns { Name: "KeyText", Type: "String" }
        Columns { Name: "Data", Type: "Int64" }
        Columns { Name: "DataText", Type: "String" }
        KeyColumnNames: ["Key", "KeyText"],
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 1000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 2000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 3000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 4000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 5000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 6000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 7000000 } } } }
    )");

    auto client = kikimr.GetTableClient();

    for (ui32 shardIdx = 0; shardIdx < fillShardsCount; ++shardIdx) {
        ui32 rowIndex = 0;
        while (rowIndex < rowsPerShard) {

            auto rowsBuilder = TValueBuilder();
            rowsBuilder.BeginList();
            for (ui32 i = 0; i < batchSizeRows; ++i) {
                rowsBuilder.AddListItem()
                    .BeginStruct()
                    .AddMember("Key")
                        .OptionalUint64(shardIdx * LargeTableKeysPerShard + rowIndex)
                    .AddMember("KeyText")
                        .OptionalString(TString(keyTextSize, '0' + (i + shardIdx) % 10))
                    .AddMember("Data")
                        .OptionalInt64(rowIndex)
                    .AddMember("DataText")
                        .OptionalString(TString(dataTextSize, '0' + (i + shardIdx + 1) % 10))
                    .EndStruct();

                ++rowIndex;
                if (rowIndex == rowsPerShard) {
                    break;
                }
            }
            rowsBuilder.EndList();

            auto result = client.BulkUpsert("/Root/LargeTable", rowsBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }
    }
}

Y_UNIT_TEST_SUITE(KqpLimits) {
    Y_UNIT_TEST_NEW_ENGINE(DatashardProgramSize) {
        auto app = NKikimrConfig::TAppConfig();
        app.MutableTableServiceConfig()->MutableResourceManager()->SetMkqlLightProgramMemoryLimit(1'000'000'000);

        TKikimrRunner kikimr(app);
        CreateLargeTable(kikimr, 0, 0, 0);

        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SLOW_LOG, NActors::NLog::PRI_ERROR);

        auto db = kikimr.GetTableClient(); 
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto paramsBuilder = db.GetParamsBuilder();
        auto& rowsParam = paramsBuilder.AddParam("$rows");

        rowsParam.BeginList();
        for (ui32 i = 0; i < 10000; ++i) {
            rowsParam.AddListItem()
                .BeginStruct()
                .AddMember("Key")
                    .OptionalUint64(i)
                .AddMember("KeyText")
                    .OptionalString(TString(5000, '0' + i % 10))
                .AddMember("Data")
                    .OptionalInt64(i)
                .AddMember("DataText")
                    .OptionalString(TString(16, '0' + (i + 1) % 10))
                .EndStruct();
        }
        rowsParam.EndList();
        rowsParam.Build();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $rows AS List<Struct<Key: Uint64?, KeyText: String?, Data: Int64?, DataText: String?>>;

            UPSERT INTO `/Root/LargeTable`
            SELECT * FROM AS_TABLE($rows);
        )"), TTxControl::BeginTx().CommitTx(), paramsBuilder.Build()).ExtractValueSync();
        result.GetIssues().PrintTo(Cerr);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::GENERIC_ERROR);
        UNIT_ASSERT(HasIssue(result.GetIssues(), NKikimrIssues::TIssuesIds::SHARD_PROGRAM_SIZE_EXCEEDED));
    }

    Y_UNIT_TEST_NEW_ENGINE(DatashardReplySize) {
        auto app = NKikimrConfig::TAppConfig();
        auto& queryLimits = *app.MutableTableServiceConfig()->MutableQueryLimits();
        queryLimits.MutablePhaseLimits()->SetComputeNodeMemoryLimitBytes(1'000'000'000);
        TKikimrRunner kikimr(app);
        CreateLargeTable(kikimr, 100, 10, 1'000'000, 1, 2);

        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SLOW_LOG, NActors::NLog::PRI_ERROR);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            SELECT * FROM `/Root/LargeTable`;
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        result.GetIssues().PrintTo(Cerr);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::UNDETERMINED);
        UNIT_ASSERT(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_RESULT_UNAVAILABLE));
    }

    Y_UNIT_TEST_NEW_ENGINE(QueryReplySize) {
        TKikimrRunner kikimr;
        CreateLargeTable(kikimr, 10, 10, 1'000'000, 1);

        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SLOW_LOG, NActors::NLog::PRI_ERROR);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            SELECT * FROM `/Root/LargeTable`;
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        result.GetIssues().PrintTo(Cerr);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::UNDETERMINED);
        UNIT_ASSERT(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_RESULT_UNAVAILABLE));
    }

    Y_UNIT_TEST(TooBigQuery) {
        auto app = NKikimrConfig::TAppConfig();
        app.MutableTableServiceConfig()->MutableResourceManager()->SetMkqlLightProgramMemoryLimit(1'000'000'000);
        app.MutableTableServiceConfig()->SetCompileTimeoutMs(TDuration::Minutes(5).MilliSeconds());

        TKikimrRunner kikimr(app);
        CreateLargeTable(kikimr, 0, 0, 0);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SLOW_LOG, NActors::NLog::PRI_ERROR);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        TStringBuilder query;
        query << R"(
            --!syntax_v1
            PRAGMA Kikimr.UseNewEngine = 'true';

            UPSERT INTO `/Root/LargeTable`
            SELECT * FROM AS_TABLE(AsList(
        )";

        ui32 count = 5000;
        for (ui32 i = 0; i < count; ++i) {
            query << "AsStruct("
                 << i << "UL AS Key, "
                 << "'" << CreateGuidAsString() << TString(5000, '0' + i % 10) << "' AS KeyText, "
                 << count + i << "L AS Data, "
                 << "'" << CreateGuidAsString() << "' AS DataText"
                 << ")";
            if (i + 1 != count) {
                query << ", ";
            }
        }
        query << "))";

        auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        result.GetIssues().PrintTo(Cerr);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::GENERIC_ERROR, result.GetIssues().ToString());
        UNIT_ASSERT(HasIssue(result.GetIssues(), NKikimrIssues::TIssuesIds::SHARD_PROGRAM_SIZE_EXCEEDED));
    }

    Y_UNIT_TEST_NEW_ENGINE(AffectedShardsLimit) {
        NKikimrConfig::TAppConfig appConfig;
        auto& queryLimits = *appConfig.MutableTableServiceConfig()->MutableQueryLimits();
        queryLimits.MutablePhaseLimits()->SetAffectedShardsLimit(20);

        TKikimrRunner kikimr(appConfig);

        kikimr.GetTestClient().CreateTable("/Root", R"(
            Name: "ManyShard20"
            Columns { Name: "Key", Type: "Uint32" }
            Columns { Name: "Value1", Type: "String" }
            Columns { Name: "Value2", Type: "Int32" }
            KeyColumnNames: ["Key"]
            UniformPartitionsCount: 20
        )");

        kikimr.GetTestClient().CreateTable("/Root", R"(
            Name: "ManyShard21"
            Columns { Name: "Key", Type: "Uint32" }
            Columns { Name: "Value1", Type: "String" }
            Columns { Name: "Value2", Type: "Int32" }
            KeyColumnNames: ["Key"]
            UniformPartitionsCount: 21
        )");

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q_(R"(
            SELECT COUNT(*) FROM `/Root/ManyShard20`
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        result = session.ExecuteDataQuery(Q_(R"(
            SELECT COUNT(*) FROM `/Root/ManyShard21`
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        result.GetIssues().PrintTo(Cerr);
        if (UseNewEngine) {
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::PRECONDITION_FAILED);
            UNIT_ASSERT(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED));
        } else {
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::BAD_REQUEST);
            UNIT_ASSERT(HasIssue(result.GetIssues(), NKikimrIssues::TIssuesIds::ENGINE_ERROR));
        }
    }

    Y_UNIT_TEST_NEW_ENGINE(ReadsetCountLimit) {
        NKikimrConfig::TAppConfig appConfig;
        auto& queryLimits = *appConfig.MutableTableServiceConfig()->MutableQueryLimits();
        queryLimits.MutablePhaseLimits()->SetReadsetCountLimit(50);

        TKikimrRunner kikimr(appConfig);
        CreateLargeTable(kikimr, 10, 10, 100);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q_(R"(
            UPDATE `/Root/LargeTable`
            SET Data = CAST(Key AS Int64) + 10
            WHERE Key < 7000000;
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        result = session.ExecuteDataQuery(Q_(R"(
            UPDATE `/Root/LargeTable`
            SET Data = CAST(Key AS Int64) + 10;
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        if (UseNewEngine) {
            // TODO: ???
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        } else {
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::BAD_REQUEST);
            UNIT_ASSERT(HasIssue(result.GetIssues(), NKikimrIssues::TIssuesIds::ENGINE_ERROR));
        }
    }

    Y_UNIT_TEST_NEW_ENGINE(TotalReadSizeLimit) {
        NKikimrConfig::TAppConfig appConfig;
        auto& queryLimits = *appConfig.MutableTableServiceConfig()->MutableQueryLimits();
        queryLimits.MutablePhaseLimits()->SetTotalReadSizeLimitBytes(100'000'000);

        TKikimrRunner kikimr(appConfig);
        CreateLargeTable(kikimr, 20, 10, 1'000'000, 1);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q_(R"(
            SELECT Key, KeyText, SUBSTRING(DataText, 0, 10) AS DataText
            FROM `/Root/LargeTable`;
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        result.GetIssues().PrintTo(Cerr);
        if (UseNewEngine) {
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::PRECONDITION_FAILED);
            UNIT_ASSERT(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED,
                [] (const NYql::TIssue& issue) {
                    return issue.Message.Contains("Transaction total read size");
                }));

        } else {
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::GENERIC_ERROR);
            UNIT_ASSERT(HasIssue(result.GetIssues(), NYql::TIssuesIds::DEFAULT_ERROR,
                [] (const NYql::TIssue& issue) {
                    return issue.Message.Contains("Transaction total read size");
            }));
        }

        result = session.ExecuteDataQuery(Q_(R"(
            SELECT Key, KeyText, SUBSTRING(DataText, 0, 10) AS DataText
            FROM `/Root/LargeTable`
            WHERE Key < 4000000;
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
    }

    Y_UNIT_TEST_NEW_ENGINE(ComputeNodeMemoryLimit) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->MutableResourceManager()->SetMkqlLightProgramMemoryLimit(1'000'000'000);
        auto& queryLimits = *appConfig.MutableTableServiceConfig()->MutableQueryLimits();
        queryLimits.MutablePhaseLimits()->SetComputeNodeMemoryLimitBytes(100'000'000);

        TKikimrRunner kikimr(appConfig);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q_(R"(
            SELECT ToDict(
                ListMap(
                    ListFromRange(0ul, 5000000ul),
                    ($x) -> { RETURN AsTuple($x, $x + 1); }
                )
            );
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        result.GetIssues().PrintTo(Cerr);

        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(),
            UseNewEngine ? EStatus::PRECONDITION_FAILED : EStatus::GENERIC_ERROR);

        auto issueId = UseNewEngine ? NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED : NYql::TIssuesIds::DEFAULT_ERROR;
        UNIT_ASSERT(HasIssue(result.GetIssues(), issueId,
            [] (const NYql::TIssue& issue) {
                return issue.Message.Contains("Memory limit exceeded");
            }));
    }

    Y_UNIT_TEST_NEW_ENGINE(QueryExecCancel) {
        TKikimrRunner kikimr;
        CreateLargeTable(kikimr, 500000, 10, 100, 5000, 1);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto prepareResult = session.PrepareDataQuery(Q_(R"(
            SELECT COUNT(*) FROM `/Root/LargeTable` WHERE SUBSTRING(DataText, 50, 5) = "11111";
        )")).GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(prepareResult.GetStatus(), EStatus::SUCCESS, prepareResult.GetIssues().ToString());
        auto dataQuery = prepareResult.GetQuery();

        auto settings = TExecDataQuerySettings()
            .CancelAfter(TDuration::MilliSeconds(100));

        auto result = dataQuery.Execute(TTxControl::BeginTx().CommitTx(), settings).GetValueSync();

        result.GetIssues().PrintTo(Cerr);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::CANCELLED);
    }

    Y_UNIT_TEST_NEW_ENGINE(QueryExecTimeout) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->MutableResourceManager()->SetMkqlLightProgramMemoryLimit(10'000'000'000);

        TKikimrRunner kikimr(appConfig);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto prepareResult = session.PrepareDataQuery(Q_(R"(
            SELECT ToDict(
                ListMap(
                    ListFromRange(0ul, 10000000ul),
                    ($x) -> { RETURN AsTuple($x, $x + 1); }
                )
            );
        )")).GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(prepareResult.GetStatus(), EStatus::SUCCESS, prepareResult.GetIssues().ToString());
        auto dataQuery = prepareResult.GetQuery();

        auto settings = TExecDataQuerySettings()
            .OperationTimeout(TDuration::MilliSeconds(500));
        auto result = dataQuery.Execute(TTxControl::BeginTx().CommitTx(), settings).GetValueSync();

        result.GetIssues().PrintTo(Cerr);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::TIMEOUT);
    }
}

} // namespace NKqp
} // namespace NKikimr
