#include "grpc_pq_actor.h"
#include <ydb/services/persqueue_v1/ut/pq_data_writer.h>
#include <ydb/services/persqueue_v1/ut/api_test_setup.h>
#include <ydb/services/persqueue_v1/ut/rate_limiter_test_setup.h>
#include <ydb/services/persqueue_v1/ut/test_utils.h>
#include <ydb/services/persqueue_v1/ut/persqueue_test_fixture.h>

#include <ydb/core/base/appdata.h>
#include <ydb/core/testlib/test_pq_client.h>
#include <ydb/core/protos/grpc_pq_old.pb.h>
#include <ydb/core/persqueue/cluster_tracker.h>

#include <ydb/core/tablet/tablet_counters_aggregator.h>

#include <ydb/library/aclib/aclib.h>
#include <ydb/library/persqueue/obfuscate/obfuscate.h>
#include <ydb/library/persqueue/tests/counters.h>
#include <ydb/library/persqueue/topic_parser/topic_parser.h>

#include <library/cpp/testing/unittest/tests_data.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/json/json_reader.h>
#include <library/cpp/monlib/dynamic_counters/encode.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/message_differencer.h>

#include <util/string/join.h>
#include <util/system/sanitizers.h>
#include <util/generic/guid.h>

#include <grpc++/client_context.h>
#include <grpc++/create_channel.h>

#include <ydb/public/api/grpc/draft/ydb_persqueue_v1.grpc.pb.h>
#include <ydb/public/api/protos/persqueue_error_codes_v1.pb.h>

#include <ydb/public/sdk/cpp/client/ydb_persqueue_public/persqueue.h>
#include <ydb/public/sdk/cpp/client/ydb_persqueue_core/ut/ut_utils/data_plane_helpers.h>


namespace NKikimr::NPersQueueTests {

using namespace Tests;
using namespace NKikimrClient;
using namespace Ydb::PersQueue;
using namespace Ydb::PersQueue::V1;
using namespace NThreading;
using namespace NNetClassifier;

TAutoPtr<IEventHandle> GetClassifierUpdate(TServer& server, const TActorId sender) {
    auto& actorSystem = *server.GetRuntime();
    actorSystem.Send(
            new IEventHandle(MakeNetClassifierID(), sender,
            new TEvNetClassifier::TEvSubscribe()
        ));

    TAutoPtr<IEventHandle> handle;
    actorSystem.GrabEdgeEvent<NNetClassifier::TEvNetClassifier::TEvClassifierUpdate>(handle);

    UNIT_ASSERT(handle);
    UNIT_ASSERT_VALUES_EQUAL(handle->Recipient, sender);

    return handle;
}

THolder<TTempFileHandle> CreateNetDataFile(const TString& content) {
    auto netDataFile = MakeHolder<TTempFileHandle>("data.tsv");

    netDataFile->Write(content.Data(), content.Size());
    netDataFile->FlushData();

    return netDataFile;
}


static TString FormNetData() {
    return "10.99.99.224/32\tSAS\n"
           "::1/128\tVLA\n";
}

namespace {
    const static TString DEFAULT_TOPIC_NAME = "rt3.dc1--topic1";
    const static TString SHORT_TOPIC_NAME = "topic1";
}

#define MAKE_INSECURE_STUB                                                \
    std::shared_ptr<grpc::Channel> Channel_;                              \
    std::unique_ptr<Ydb::PersQueue::V1::PersQueueService::Stub> StubP_;   \
                                                                          \
    {                                                                     \
        Channel_ = grpc::CreateChannel(                                   \
            "localhost:" + ToString(server.Server->GrpcPort),                    \
            grpc::InsecureChannelCredentials()                            \
        );                                                                \
        StubP_ = Ydb::PersQueue::V1::PersQueueService::NewStub(Channel_); \
    }                                                                     \
    grpc::ClientContext rcontext;



    Y_UNIT_TEST_SUITE(TPersQueueTest) {
    Y_UNIT_TEST(AllEqual) {
        using NGRpcProxy::V1::AllEqual;

        UNIT_ASSERT(AllEqual(0));
        UNIT_ASSERT(AllEqual(0, 0));
        UNIT_ASSERT(AllEqual(0, 0, 0));
        UNIT_ASSERT(AllEqual(1, 1, 1));
        UNIT_ASSERT(AllEqual(1, 1, 1, 1, 1, 1));
        UNIT_ASSERT(!AllEqual(1, 0));
        UNIT_ASSERT(!AllEqual(0, 1));
        UNIT_ASSERT(!AllEqual(0, 1, 0));
        UNIT_ASSERT(!AllEqual(1, 1, 0));
        UNIT_ASSERT(!AllEqual(0, 1, 1));
        UNIT_ASSERT(!AllEqual(1, 1, 1, 1, 1, 0));
    }

    Y_UNIT_TEST(SetupLockSession2) {
        TPersQueueV1TestServer server;
        SET_LOCALS;
        const TString topicPath = server.GetTopicPathMultipleDC();

        auto driver = server.Server->AnnoyingClient->GetDriver();

        NYdb::NPersQueue::TReadSessionSettings settings;
        settings.ConsumerName("shared/user").AppendTopics(topicPath).ReadMirrored("dc1");
        auto reader = CreateReader(*driver, settings);

        for (ui32 i = 0; i < 2; ++i) {
            auto msg = reader->GetEvent(true, 1);
            UNIT_ASSERT(msg);

            Cerr << "Got message: " << NYdb::NPersQueue::DebugString(*msg) << "\n";

            auto ev = std::get_if<NYdb::NPersQueue::TReadSessionEvent::TCreatePartitionStreamEvent>(&*msg);

            UNIT_ASSERT(ev);

            UNIT_ASSERT(ev->GetPartitionStream()->GetTopicPath() == topicPath);
            UNIT_ASSERT(ev->GetPartitionStream()->GetCluster() == "dc1" || ev->GetPartitionStream()->GetCluster() == "dc2");
            UNIT_ASSERT(ev->GetPartitionStream()->GetPartitionId() == 0);

        }
        auto wait = reader->WaitEvent();
        UNIT_ASSERT(!wait.Wait(TDuration::Seconds(1)));

        pqClient->AlterTopicNoLegacy("/Root/PQ/rt3.dc2--acc--topic2dc", 2);
        Cerr << "======Alter topic done\n";
        UNIT_ASSERT(wait.Wait(TDuration::Seconds(5)));

        auto msg = reader->GetEvent(true, 1);
        UNIT_ASSERT(msg);

        Cerr << NYdb::NPersQueue::DebugString(*msg) << "\n";

        auto ev = std::get_if<NYdb::NPersQueue::TReadSessionEvent::TCreatePartitionStreamEvent>(&*msg);

        UNIT_ASSERT(ev);

        UNIT_ASSERT(ev->GetPartitionStream()->GetTopicPath() == topicPath);
        UNIT_ASSERT(ev->GetPartitionStream()->GetCluster() == "dc2");
        UNIT_ASSERT(ev->GetPartitionStream()->GetPartitionId() == 1);
    }


    Y_UNIT_TEST(SetupLockSession) {
        TPersQueueV1TestServer server;
        SET_LOCALS;
        MAKE_INSECURE_STUB;
        auto readStream = StubP_->MigrationStreamingRead(&rcontext);
        UNIT_ASSERT(readStream);

        // init read session
        {
            MigrationStreamingReadClientMessage  req;
            MigrationStreamingReadServerMessage resp;

            req.mutable_init_request()->add_topics_read_settings()->set_topic("acc/topic1");

            req.mutable_init_request()->set_consumer("user");
            req.mutable_init_request()->set_read_only_original(true);
            req.mutable_init_request()->mutable_read_params()->set_max_read_messages_count(1);

            if (!readStream->Write(req)) {
                ythrow yexception() << "write fail";
            }
            UNIT_ASSERT(readStream->Read(&resp));
            Cerr << "===Got response: " << resp.ShortDebugString() << Endl;
            UNIT_ASSERT(resp.response_case() == MigrationStreamingReadServerMessage::kInitResponse);
            //send some reads
            req.Clear();
            req.mutable_read();
            for (ui32 i = 0; i < 10; ++i) {
                if (!readStream->Write(req)) {
                    ythrow yexception() << "write fail";
                }
            }
        }

        {
            Sleep(TDuration::Seconds(10));
            ReadInfoRequest request;
            ReadInfoResponse response;
            request.mutable_consumer()->set_path("user");
            request.set_get_only_original(true);
            request.add_topics()->set_path("acc/topic1");
            grpc::ClientContext rcontext;
            auto status = StubP_->GetReadSessionsInfo(&rcontext, request, &response);
            UNIT_ASSERT(status.ok());
            ReadInfoResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << "Read info response: " << response << Endl << res << Endl;
            UNIT_ASSERT_VALUES_EQUAL(res.topics_size(), 1);
            UNIT_ASSERT(res.topics(0).status() == Ydb::StatusIds::SUCCESS);
        }
        ui64 assignId = 0;
        {
            MigrationStreamingReadClientMessage  req;
            MigrationStreamingReadServerMessage resp;

            //lock partition
            UNIT_ASSERT(readStream->Read(&resp));
            UNIT_ASSERT(resp.response_case() == MigrationStreamingReadServerMessage::kAssigned);
            UNIT_ASSERT(resp.assigned().topic().path() == "acc/topic1");
            UNIT_ASSERT(resp.assigned().cluster() == "dc1");
            UNIT_ASSERT(resp.assigned().partition() == 0);

            assignId = resp.assigned().assign_id();
            req.Clear();
            req.mutable_start_read()->mutable_topic()->set_path("acc/topic1");
            req.mutable_start_read()->set_cluster("dc1");
            req.mutable_start_read()->set_partition(0);
            req.mutable_start_read()->set_assign_id(assignId);

            req.mutable_start_read()->set_read_offset(10);
            if (!readStream->Write(req)) {
                ythrow yexception() << "write fail";
            }

        }

        auto driver = server.Server->AnnoyingClient->GetDriver();

        {
            auto writer = CreateSimpleWriter(*driver, "acc/topic1", "source");
            for (int i = 1; i < 17; ++i) {
                bool res = writer->Write("valuevaluevalue" + ToString(i), i);
                UNIT_ASSERT(res);
            }
            bool res = writer->Close(TDuration::Seconds(10));
            UNIT_ASSERT(res);
        }

        //check read results
        MigrationStreamingReadServerMessage resp;
        for (ui32 i = 10; i < 16; ++i) {
            UNIT_ASSERT(readStream->Read(&resp));
            Cerr << "Got read response " << resp << "\n";
            UNIT_ASSERT_C(resp.response_case() == MigrationStreamingReadServerMessage::kDataBatch, resp);
            UNIT_ASSERT(resp.data_batch().partition_data_size() == 1);
            UNIT_ASSERT(resp.data_batch().partition_data(0).batches_size() == 1);
            UNIT_ASSERT(resp.data_batch().partition_data(0).batches(0).message_data_size() == 1);
            UNIT_ASSERT(resp.data_batch().partition_data(0).batches(0).message_data(0).offset() == i);
        }
        //TODO: restart here readSession and read from position 10
        {
            MigrationStreamingReadClientMessage  req;
            MigrationStreamingReadServerMessage resp;

            auto cookie = req.mutable_commit()->add_cookies();
            cookie->set_assign_id(assignId);
            cookie->set_partition_cookie(1);

            if (!readStream->Write(req)) {
                ythrow yexception() << "write fail";
            }
            UNIT_ASSERT(readStream->Read(&resp));
            UNIT_ASSERT_C(resp.response_case() == MigrationStreamingReadServerMessage::kCommitted, resp);
        }


        pqClient->AlterTopic("rt3.dc1--acc--topic1", 10);
        {
            ReadInfoRequest request;
            ReadInfoResponse response;
            request.mutable_consumer()->set_path("user");
            request.set_get_only_original(false);
            request.add_topics()->set_path("acc/topic1");
            grpc::ClientContext rcontext;
            auto status = StubP_->GetReadSessionsInfo(&rcontext, request, &response);
            UNIT_ASSERT(status.ok());
            ReadInfoResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << "Get read session info response: " << response << "\n" << res << "\n";
//            UNIT_ASSERT(res.sessions_size() == 1);
            UNIT_ASSERT_VALUES_EQUAL(res.topics_size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(res.topics(0).partitions_size(), 10);
        }

        {
            ReadInfoRequest request;
            ReadInfoResponse response;
            request.mutable_consumer()->set_path("user");
            request.set_get_only_original(false);
            request.add_topics()->set_path("acc/topic1");
            grpc::ClientContext rcontext;

            pqClient->MarkNodeInHive(runtime, 0, false);
            pqClient->MarkNodeInHive(runtime, 1, false);

            pqClient->RestartBalancerTablet(runtime, "rt3.dc1--acc--topic1");
            auto status = StubP_->GetReadSessionsInfo(&rcontext, request, &response);
            UNIT_ASSERT(status.ok());
            ReadInfoResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << "Read sessions info response: " << response << "\nResult: " << res << "\n";
            UNIT_ASSERT(res.topics().size() == 1);
            UNIT_ASSERT(res.topics(0).partitions(0).status() == Ydb::StatusIds::UNAVAILABLE);
        }
    }


    void SetupWriteSessionImpl(bool rr) {
        NPersQueue::TTestServer server{PQSettings(0, 2, rr), false};
        server.ServerSettings.SetEnableSystemViews(false);
        server.StartServer();

        server.EnableLogs({ NKikimrServices::PERSQUEUE });
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 10);

        TPQDataWriter writer("source", server);

        ui32 p = writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue1"});

        server.AnnoyingClient->AlterTopic(DEFAULT_TOPIC_NAME, 15);

        ui32 pp = writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue2"});
        UNIT_ASSERT_VALUES_EQUAL(p, pp);

        writer.Write(SHORT_TOPIC_NAME, {"1", "2", "3", "4", "5"});

        writer.Write("topic2", {"valuevaluevalue1"}, true);

        p = writer.InitSession("sid1", 2, true);
        pp = writer.InitSession("sid1", 0, true);

        UNIT_ASSERT(p = pp);
        UNIT_ASSERT(p == 1);

        {
            p = writer.InitSession("sidx", 0, true);
            pp = writer.InitSession("sidx", 0, true);

            UNIT_ASSERT(p == pp);
        }

        writer.InitSession("sid1", 3, false);

        //check round robin;
        TMap<ui32, ui32> ss;
        for (ui32 i = 0; i < 15*5; ++i) {
            ss[writer.InitSession("sid_rand_" + ToString<ui32>(i), 0, true)]++;
        }
        for (auto &s : ss) {
            Cerr << s.first << " " << s.second << "\n";
            if (rr) {
                UNIT_ASSERT(s.second >= 4 && s.second <= 6);
            }
        }
     }

    Y_UNIT_TEST(SetupWriteSession) {
        SetupWriteSessionImpl(false);
        SetupWriteSessionImpl(true);
    }

    Y_UNIT_TEST(StoreNoMoreThanXSourceIDs) {
        ui16 X = 4;
        ui64 SOURCEID_COUNT_DELETE_BATCH_SIZE = 100;
        NPersQueue::TTestServer server;
        server.EnableLogs({ NKikimrServices::PERSQUEUE, NKikimrServices::PQ_WRITE_PROXY });
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1, 8*1024*1024, 86400, 20000000, "", 200000000, {}, {}, {}, X, 86400);

        auto driver = server.AnnoyingClient->GetDriver();

        auto writer1 = CreateSimpleWriter(*driver, SHORT_TOPIC_NAME, TStringBuilder() << "test source ID " << 0, {}, {}, true);
        writer1->GetInitSeqNo();

        bool res = writer1->Write("x", 1);
        UNIT_ASSERT(res);

        Sleep(TDuration::Seconds(5));

        auto writer2 = CreateSimpleWriter(*driver, SHORT_TOPIC_NAME, TStringBuilder() << "test source ID Del " << 0);
        writer2->GetInitSeqNo();

        res = writer2->Write("x", 1);
        UNIT_ASSERT(res);

        Sleep(TDuration::Seconds(5));

        res = writer1->Write("x", 2);
        UNIT_ASSERT(res);

        Sleep(TDuration::Seconds(5));

        for (ui32 nProducer=1; nProducer < X + SOURCEID_COUNT_DELETE_BATCH_SIZE + 1; ++nProducer) {
            auto writer = CreateSimpleWriter(*driver, SHORT_TOPIC_NAME, TStringBuilder() << "test source ID " << nProducer);

            res = writer->Write("x", 1);
            UNIT_ASSERT(res);

            UNIT_ASSERT(writer->IsAlive());

            res = writer->Close(TDuration::Seconds(10));
            UNIT_ASSERT(res);

        }

        res = writer1->Write("x", 3);
        UNIT_ASSERT(res);
        res = writer1->Close(TDuration::Seconds(5));
        UNIT_ASSERT(res);

        res = writer2->Write("x", 4);
        UNIT_ASSERT(res);

        UNIT_ASSERT(!writer2->Close());
    }

    Y_UNIT_TEST(EachMessageGetsExactlyOneAcknowledgementInCorrectOrder) {
        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic("rt3.dc1--topic", 1);

        auto driver = server.AnnoyingClient->GetDriver();

        auto writer = CreateSimpleWriter(*driver, "topic", "test source ID");

        bool res = true;

        ui32 messageCount = 1000;

        for (ui32 sequenceNumber = 1; sequenceNumber <= messageCount; ++sequenceNumber) {
                res = writer->Write("x", sequenceNumber);
                UNIT_ASSERT(res);
        }
        UNIT_ASSERT(writer->IsAlive());
        res = writer->Close(TDuration::Seconds(10));
        UNIT_ASSERT(res);
    }

    Y_UNIT_TEST(SetupWriteSessionOnDisabledCluster) {
        TPersQueueV1TestServer server;
        SET_LOCALS;

        TPQDataWriter writer("source", *server.Server);

        pqClient->DisableDC();

        Sleep(TDuration::Seconds(5));
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue1"}, true);
    }

    Y_UNIT_TEST(CloseActiveWriteSessionOnClusterDisable) {
        NPersQueue::TTestServer server;

        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 10);

        server.EnableLogs({ NKikimrServices::PQ_WRITE_PROXY });


        TPQDataWriter writer2("source", server);

        auto driver = server.AnnoyingClient->GetDriver();

        auto writer = CreateWriter(*driver, SHORT_TOPIC_NAME, "123", 0, "raw");

        auto msg = writer->GetEvent(true);
        UNIT_ASSERT(msg); // ReadyToAcceptEvent

        Cerr << DebugString(*msg) << "\n";

        server.AnnoyingClient->DisableDC();

        UNIT_ASSERT(writer->WaitEvent().Wait(TDuration::Seconds(30)));
        msg = writer->GetEvent(true);
        UNIT_ASSERT(msg);


        Cerr << DebugString(*msg) << "\n";

        auto ev = std::get_if<NYdb::NPersQueue::TSessionClosedEvent>(&*msg);

        UNIT_ASSERT(ev);

        Cerr << "is dead res: " << ev->DebugString() << "\n";
        UNIT_ASSERT_EQUAL(ev->GetIssues().back().GetCode(), Ydb::PersQueue::ErrorCode::CLUSTER_DISABLED);
    }

    Y_UNIT_TEST(BadSids) {
        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 10);
        server.EnableLogs({ NKikimrServices::PQ_WRITE_PROXY });
        TPQDataWriter writer2("source", server);
        TString topic = SHORT_TOPIC_NAME;

        auto driver = server.AnnoyingClient->GetDriver();

        auto writer = CreateSimpleWriter(*driver, topic, "base64:a***");
        UNIT_ASSERT(!writer->Write("x"));
        writer = CreateSimpleWriter(*driver, topic, "base64:aa==");
        UNIT_ASSERT(!writer->Write("x"));
        writer = CreateSimpleWriter(*driver, topic, "base64:a");
        UNIT_ASSERT(!writer->Write("x"));
        writer = CreateSimpleWriter(*driver, topic, "base64:aa");
        UNIT_ASSERT(writer->Write("x"));
        UNIT_ASSERT(writer->Close());
    }

    Y_UNIT_TEST(ReadFromSeveralPartitions) {
        NPersQueue::TTestServer server;
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY, NKikimrServices::PQ_METACACHE });

        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 10);

        TPQDataWriter writer("source1", server);
        Cerr << "===Writer started\n";
        std::shared_ptr<grpc::Channel> Channel_;
        std::unique_ptr<Ydb::PersQueue::V1::PersQueueService::Stub> StubP_;

        Channel_ = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
        StubP_ = Ydb::PersQueue::V1::PersQueueService::NewStub(Channel_);


        //Write some data
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue1"});

        TPQDataWriter writer2("source2", server);
        writer2.Write(SHORT_TOPIC_NAME, {"valuevaluevalue2"});
        Cerr << "===Writer - writes done\n";

        grpc::ClientContext rcontext;
        auto readStream = StubP_->MigrationStreamingRead(&rcontext);
        UNIT_ASSERT(readStream);

        // init read session
        {
            MigrationStreamingReadClientMessage  req;
            MigrationStreamingReadServerMessage resp;

            req.mutable_init_request()->add_topics_read_settings()->set_topic(SHORT_TOPIC_NAME);

            req.mutable_init_request()->set_consumer("user");
            req.mutable_init_request()->set_read_only_original(true);

            req.mutable_init_request()->mutable_read_params()->set_max_read_messages_count(1000);

            if (!readStream->Write(req)) {
                ythrow yexception() << "write fail";
            }
            Cerr << "===Try to get read response\n";

            UNIT_ASSERT(readStream->Read(&resp));
            Cerr << "Read server response: " << resp.ShortDebugString() << Endl;
            UNIT_ASSERT(resp.response_case() == MigrationStreamingReadServerMessage::kInitResponse);

            //send some reads
            Sleep(TDuration::Seconds(5));
            for (ui32 i = 0; i < 10; ++i) {
                req.Clear();
                req.mutable_read();

                if (!readStream->Write(req)) {
                    ythrow yexception() << "write fail";
                }
            }
        }

        //check read results
        MigrationStreamingReadServerMessage resp;
        for (ui32 i = 0; i < 2;) {
            MigrationStreamingReadServerMessage resp;
            UNIT_ASSERT(readStream->Read(&resp));
            if (resp.response_case() == MigrationStreamingReadServerMessage::kAssigned) {
                auto assignId = resp.assigned().assign_id();
                MigrationStreamingReadClientMessage req;
                req.mutable_start_read()->mutable_topic()->set_path(SHORT_TOPIC_NAME);
                req.mutable_start_read()->set_cluster("dc1");
                req.mutable_start_read()->set_assign_id(assignId);
                UNIT_ASSERT(readStream->Write(req));
                continue;
            }

            UNIT_ASSERT_C(resp.response_case() == MigrationStreamingReadServerMessage::kDataBatch, resp);
            i += resp.data_batch().partition_data_size();
        }
    }


    void SetupReadSessionTest() {
        NPersQueue::TTestServer server;
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY });

        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);
        server.AnnoyingClient->CreateTopicNoLegacy("rt3.dc2--topic1", 2, true, false);

        TPQDataWriter writer("source1", server);

        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue0"});
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue1"});
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue2"});
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue3"});
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue4"});
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue5"});
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue6"});
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue7"});
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue8"});
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue9"});

        writer.Read(SHORT_TOPIC_NAME, "user1", "", false, false);
    }

    Y_UNIT_TEST(SetupReadSession) {
        SetupReadSessionTest();
    }


    Y_UNIT_TEST(WriteExisting) {
        NPersQueue::TTestServer server;
        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE });

        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);

        {
            THolder<NMsgBusProxy::TBusPersQueue> request = TRequestDescribePQ().GetRequest({});

            NKikimrClient::TResponse response;

            auto channel = grpc::CreateChannel("localhost:"+ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
            auto stub(NKikimrClient::TGRpcServer::NewStub(channel));
            grpc::ClientContext context;
            auto status = stub->PersQueueRequest(&context, request->Record, &response);

            UNIT_ASSERT(status.ok());
        }

        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 1, "abacaba", 1, "valuevaluevalue1", "", ETransport::GRpc);
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 1, "abacaba", 2, "valuevaluevalue1", "", ETransport::GRpc);
    }

    Y_UNIT_TEST(WriteExistingBigValue) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root").SetNodeCount(2));
        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE });
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2, 8*1024*1024, 86400, 100000);


        TInstant now(Now());

        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 1, "abacaba", 1, TString(1000000, 'a'));
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 1, "abacaba", 2, TString(1, 'a'));
        UNIT_ASSERT(TInstant::Now() - now > TDuration::MilliSeconds(5990)); //speed limit is 200kb/s and burst is 200kb, so to write 1mb it will take at least 4 seconds
    }

    Y_UNIT_TEST(WriteEmptyData) {
        NPersQueue::TTestServer server{PQSettings(0).SetDomainName("Root").SetNodeCount(2)};

        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);

        server.EnableLogs({ NKikimrServices::PERSQUEUE });

        // empty data and sourceId
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 1, "", 1, "", "", ETransport::MsgBus, NMsgBusProxy::MSTATUS_ERROR);
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 1, "a", 1, "", "", ETransport::MsgBus, NMsgBusProxy::MSTATUS_ERROR);
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 1, "", 1, "a", "", ETransport::MsgBus, NMsgBusProxy::MSTATUS_ERROR);
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 1, "a", 1, "a", "", ETransport::MsgBus, NMsgBusProxy::MSTATUS_OK);
    }


    Y_UNIT_TEST(WriteNonExistingPartition) {
        NPersQueue::TTestServer server{PQSettings(0).SetDomainName("Root").SetNodeCount(2)};
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);

        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE });

        server.AnnoyingClient->WriteToPQ(
                DEFAULT_TOPIC_NAME, 100500, "abacaba", 1, "valuevaluevalue1", "",
                ETransport::MsgBus, NMsgBusProxy::MSTATUS_ERROR, NMsgBusProxy::MSTATUS_ERROR
        );
    }

    Y_UNIT_TEST(WriteNonExistingTopic) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root").SetNodeCount(2));
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);
        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE });

        server.AnnoyingClient->WriteToPQ(
                DEFAULT_TOPIC_NAME + "000", 1, "abacaba", 1, "valuevaluevalue1", "",
                ETransport::MsgBus, NMsgBusProxy::MSTATUS_ERROR, NMsgBusProxy::MSTATUS_ERROR
        );
    }

    Y_UNIT_TEST(SchemeshardRestart) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root").SetNodeCount(1));
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);
        TString secondTopic = "rt3.dc1--topic2";
        server.AnnoyingClient->CreateTopic(secondTopic, 2);

        // force topic1 into cache and establish pipe from cache to schemeshard
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 1, "abacaba", 1, "valuevaluevalue1");

        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD,
            NKikimrServices::PERSQUEUE,
            NKikimrServices::PQ_METACACHE });

        server.AnnoyingClient->RestartSchemeshard(server.CleverServer->GetRuntime());

        server.AnnoyingClient->WriteToPQ(secondTopic, 1, "abacaba", 1, "valuevaluevalue1");
    }

    Y_UNIT_TEST(WriteAfterAlter) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root").SetNodeCount(2));
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);


        server.AnnoyingClient->WriteToPQ(
                DEFAULT_TOPIC_NAME, 5, "abacaba", 1, "valuevaluevalue1", "",
                ETransport::MsgBus, NMsgBusProxy::MSTATUS_ERROR,  NMsgBusProxy::MSTATUS_ERROR
        );

        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD,
            NKikimrServices::PERSQUEUE,
            NKikimrServices::PQ_METACACHE });

        server.AnnoyingClient->AlterTopic(DEFAULT_TOPIC_NAME, 10);
        Sleep(TDuration::Seconds(1));
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 5, "abacaba", 1, "valuevaluevalue1");
        server.AnnoyingClient->WriteToPQ(
                DEFAULT_TOPIC_NAME, 15, "abacaba", 1, "valuevaluevalue1", "",
                ETransport::MsgBus, NMsgBusProxy::MSTATUS_ERROR,  NMsgBusProxy::MSTATUS_ERROR
        );

        server.AnnoyingClient->AlterTopic(DEFAULT_TOPIC_NAME, 20);
        Sleep(TDuration::Seconds(1));
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 5, "abacaba", 1, "valuevaluevalue1");
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 15, "abacaba", 1, "valuevaluevalue1");
    }

    Y_UNIT_TEST(Delete) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root").SetNodeCount(2));

        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE});

        // Delete non-existing
        server.AnnoyingClient->DeleteTopic2(DEFAULT_TOPIC_NAME, NPersQueue::NErrorCode::UNKNOWN_TOPIC);

        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);

        // Delete existing
        server.AnnoyingClient->DeleteTopic2(DEFAULT_TOPIC_NAME);

        // Double delete - "What Is Dead May Never Die"
        server.AnnoyingClient->DeleteTopic2(DEFAULT_TOPIC_NAME, NPersQueue::NErrorCode::UNKNOWN_TOPIC);

        // Resurrect deleted topic
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);
        server.AnnoyingClient->DeleteTopic2(DEFAULT_TOPIC_NAME);
    }


    Y_UNIT_TEST(BigRead) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root"));
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1, 8*1024*1024, 86400, 20000000, "user1", 2000000);

        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE });

        TString value(1024*1024, 'x');
        for (ui32 i = 0; i < 32; ++i)
            server.AnnoyingClient->WriteToPQ({DEFAULT_TOPIC_NAME, 0, "source1", i}, value);

        // trying to read small PQ messages in a big messagebus event
        auto info = server.AnnoyingClient->ReadFromPQ({DEFAULT_TOPIC_NAME, 0, 0, 32, "user1"}, 23, "", NMsgBusProxy::MSTATUS_OK); //will read 21mb
        UNIT_ASSERT_VALUES_EQUAL(info.BlobsFromDisk, 0);
        UNIT_ASSERT_VALUES_EQUAL(info.BlobsFromCache, 4);

        TInstant now(TInstant::Now());
        info = server.AnnoyingClient->ReadFromPQ({DEFAULT_TOPIC_NAME, 0, 0, 32, "user1"}, 23, "", NMsgBusProxy::MSTATUS_OK); //will read 21mb
        TDuration dur = TInstant::Now() - now;
        UNIT_ASSERT_C(dur > TDuration::Seconds(7) && dur < TDuration::Seconds(20), "dur = " << dur); //speed limit is 2000kb/s and burst is 2000kb, so to read 24mb it will take at least 11 seconds

        server.AnnoyingClient->GetPartStatus({}, 1, true);

    }


    // expects that L2 size is 32Mb
    Y_UNIT_TEST(Cache) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root"));
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1, 8*1024*1024, 86400);

        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE });

        TString value(1024*1024, 'x');
        for (ui32 i = 0; i < 32; ++i)
            server.AnnoyingClient->WriteToPQ({DEFAULT_TOPIC_NAME, 0, "source1", i}, value);

        auto info0 = server.AnnoyingClient->ReadFromPQ({DEFAULT_TOPIC_NAME, 0, 0, 16, "user1"}, 16);
        auto info16 = server.AnnoyingClient->ReadFromPQ({DEFAULT_TOPIC_NAME, 0, 16, 16, "user1"}, 16);

        UNIT_ASSERT_VALUES_EQUAL(info0.BlobsFromCache, 3);
        UNIT_ASSERT_VALUES_EQUAL(info16.BlobsFromCache, 2);
        UNIT_ASSERT_VALUES_EQUAL(info0.BlobsFromDisk + info16.BlobsFromDisk, 0);

        for (ui32 i = 0; i < 8; ++i)
            server.AnnoyingClient->WriteToPQ({DEFAULT_TOPIC_NAME, 0, "source1", 32+i}, value);

        info0 = server.AnnoyingClient->ReadFromPQ({DEFAULT_TOPIC_NAME, 0, 0, 16, "user1"}, 16);
        info16 = server.AnnoyingClient->ReadFromPQ({DEFAULT_TOPIC_NAME, 0, 16, 16, "user1"}, 16);

        ui32 fromDisk = info0.BlobsFromDisk + info16.BlobsFromDisk;
        ui32 fromCache = info0.BlobsFromCache + info16.BlobsFromCache;
        UNIT_ASSERT(fromDisk > 0);
        UNIT_ASSERT(fromDisk < 5);
        UNIT_ASSERT(fromCache > 0);
        UNIT_ASSERT(fromCache < 5);
    }

    Y_UNIT_TEST(CacheHead) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root"));
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1, 6*1024*1024, 86400);

        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE });

        ui64 seqNo = 0;
        for (ui32 blobSizeKB = 256; blobSizeKB < 4096; blobSizeKB *= 2) {
            static const ui32 maxEventKB = 24*1024;
            ui32 blobSize = blobSizeKB * 1024;
            ui32 count = maxEventKB / blobSizeKB;
            count -= count%2;
            ui32 half = count/2;

            ui64 offset = seqNo;
            TString value(blobSize, 'a');
            for (ui32 i = 0; i < count; ++i)
                server.AnnoyingClient->WriteToPQ({DEFAULT_TOPIC_NAME, 0, "source1", seqNo++}, value);

            auto info_half1 = server.AnnoyingClient->ReadFromPQ({DEFAULT_TOPIC_NAME, 0, offset, half, "user1"}, half);
            auto info_half2 = server.AnnoyingClient->ReadFromPQ({DEFAULT_TOPIC_NAME, 0, offset, half, "user1"}, half);

            UNIT_ASSERT(info_half1.BlobsFromCache > 0);
            UNIT_ASSERT(info_half2.BlobsFromCache > 0);
            UNIT_ASSERT_VALUES_EQUAL(info_half1.BlobsFromDisk, 0);
            UNIT_ASSERT_VALUES_EQUAL(info_half2.BlobsFromDisk, 0);
        }
    }

    Y_UNIT_TEST(SameOffset) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root"));
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1, 6*1024*1024, 86400);
        TString secondTopic = DEFAULT_TOPIC_NAME + "2";
        server.AnnoyingClient->CreateTopic(secondTopic, 1, 6*1024*1024, 86400);

        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE });

        ui32 valueSize = 128;
        TString value1(valueSize, 'a');
        TString value2(valueSize, 'b');
        server.AnnoyingClient->WriteToPQ({DEFAULT_TOPIC_NAME, 0, "source1", 0}, value1);
        server.AnnoyingClient->WriteToPQ({secondTopic, 0, "source1", 0}, value2);

        // avoid reading from head
        TString mb(1024*1024, 'x');
        for (ui32 i = 1; i < 16; ++i) {
            server.AnnoyingClient->WriteToPQ({DEFAULT_TOPIC_NAME, 0, "source1", i}, mb);
            server.AnnoyingClient->WriteToPQ({secondTopic, 0, "source1", i}, mb);
        }

        auto info1 = server.AnnoyingClient->ReadFromPQ({DEFAULT_TOPIC_NAME, 0, 0, 1, "user1"}, 1);
        auto info2 = server.AnnoyingClient->ReadFromPQ({secondTopic, 0, 0, 1, "user1"}, 1);

        UNIT_ASSERT_VALUES_EQUAL(info1.BlobsFromCache, 1);
        UNIT_ASSERT_VALUES_EQUAL(info2.BlobsFromCache, 1);
        UNIT_ASSERT_VALUES_EQUAL(info1.Values.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(info2.Values.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(info1.Values[0].size(), valueSize);
        UNIT_ASSERT_VALUES_EQUAL(info2.Values[0].size(), valueSize);
        UNIT_ASSERT(info1.Values[0] == value1);
        UNIT_ASSERT(info2.Values[0] == value2);
    }


    Y_UNIT_TEST(FetchRequest) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root"));
        TString secondTopic = DEFAULT_TOPIC_NAME + "2";

        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 10);
        server.AnnoyingClient->CreateTopic(secondTopic, 10);

        ui32 valueSize = 128;
        TString value1(valueSize, 'a');
        TString value2(valueSize, 'b');
        server.AnnoyingClient->WriteToPQ({secondTopic, 5, "source1", 0}, value2);
        server.AnnoyingClient->WriteToPQ({DEFAULT_TOPIC_NAME, 1, "source1", 0}, value1);
        server.AnnoyingClient->WriteToPQ({DEFAULT_TOPIC_NAME, 1, "source1", 1}, value2);

        server.EnableLogs({ NKikimrServices::FLAT_TX_SCHEMESHARD, NKikimrServices::PERSQUEUE });
        TInstant tm(TInstant::Now());
        server.AnnoyingClient->FetchRequestPQ({{secondTopic, 5, 0, 400},{DEFAULT_TOPIC_NAME, 1, 0, 400},{DEFAULT_TOPIC_NAME, 3, 0, 400}}, 400, 1000000);
        UNIT_ASSERT((TInstant::Now() - tm).Seconds() < 1);
        tm = TInstant::Now();
        server.AnnoyingClient->FetchRequestPQ({{secondTopic, 5, 1, 400}}, 400, 5000);
        UNIT_ASSERT((TInstant::Now() - tm).Seconds() > 2);
        server.AnnoyingClient->FetchRequestPQ({{secondTopic, 5, 0, 400},{DEFAULT_TOPIC_NAME, 1, 0, 400},{DEFAULT_TOPIC_NAME, 3, 0, 400}}, 1, 1000000);
        server.AnnoyingClient->FetchRequestPQ({{secondTopic, 5, 500, 400},{secondTopic, 4, 0, 400},{DEFAULT_TOPIC_NAME, 1, 0, 400}}, 400, 1000000);
    }

    Y_UNIT_TEST(Init) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root").SetNodeCount(2));
        if (!true) {
            server.EnableLogs( {
                NKikimrServices::FLAT_TX_SCHEMESHARD,
                NKikimrServices::TX_DATASHARD,
                NKikimrServices::HIVE,
                NKikimrServices::PERSQUEUE,
                NKikimrServices::TABLET_MAIN,
                NKikimrServices::BS_PROXY_DISCOVER,
                NKikimrServices::PIPE_CLIENT,
                NKikimrServices::PQ_METACACHE });
        }

        server.AnnoyingClient->DescribeTopic({});
        server.AnnoyingClient->TestCase({}, 0, 0, true);

        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 10);
        server.AnnoyingClient->AlterTopic(DEFAULT_TOPIC_NAME, 20);
        TString secondTopic = DEFAULT_TOPIC_NAME + "2";
        TString thirdTopic = DEFAULT_TOPIC_NAME + "3";
        server.AnnoyingClient->CreateTopic(secondTopic, 25);

        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 5, "abacaba", 1, "valuevaluevalue1");
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 5, "abacaba", 2, "valuevaluevalue2");
        server.AnnoyingClient->WriteToPQ(DEFAULT_TOPIC_NAME, 5, "abacabae", 1, "valuevaluevalue3");
        server.AnnoyingClient->ReadFromPQ(DEFAULT_TOPIC_NAME, 5, 0, 10, 3);

        server.AnnoyingClient->SetClientOffsetPQ(DEFAULT_TOPIC_NAME, 5, 2);

        server.AnnoyingClient->TestCase({{DEFAULT_TOPIC_NAME, {5}}}, 1, 1, true);
        server.AnnoyingClient->TestCase({{DEFAULT_TOPIC_NAME, {0}}}, 1, 0, true);
        server.AnnoyingClient->TestCase({{DEFAULT_TOPIC_NAME, {}}}, 20, 1, true);
        server.AnnoyingClient->TestCase({{DEFAULT_TOPIC_NAME, {5, 5}}}, 0, 0, false);
        server.AnnoyingClient->TestCase({{DEFAULT_TOPIC_NAME, {111}}}, 0, 0, false);
        server.AnnoyingClient->TestCase({}, 45, 1, true);
        server.AnnoyingClient->TestCase({{thirdTopic, {}}}, 0, 0, false);
        server.AnnoyingClient->TestCase({{DEFAULT_TOPIC_NAME, {}}, {thirdTopic, {}}}, 0, 0, false);
        server.AnnoyingClient->TestCase({{DEFAULT_TOPIC_NAME, {}}, {secondTopic, {}}}, 45, 1, true);
        server.AnnoyingClient->TestCase({{DEFAULT_TOPIC_NAME, {0, 3, 5}}, {secondTopic, {1, 4, 6, 8}}}, 7, 1, true);

        server.AnnoyingClient->DescribeTopic({DEFAULT_TOPIC_NAME});
        server.AnnoyingClient->DescribeTopic({secondTopic});
        server.AnnoyingClient->DescribeTopic({secondTopic, DEFAULT_TOPIC_NAME});
        server.AnnoyingClient->DescribeTopic({});
        server.AnnoyingClient->DescribeTopic({thirdTopic}, true);
    }

    void WaitResolveSuccess(TFlatMsgBusPQClient& annoyingClient, TString topic, ui32 numParts) {
        const TInstant start = TInstant::Now();
        while (true) {
            TAutoPtr<NMsgBusProxy::TBusPersQueue> request(new NMsgBusProxy::TBusPersQueue);
            auto req = request->Record.MutableMetaRequest();
            auto partOff = req->MutableCmdGetPartitionLocations();
            auto treq = partOff->AddTopicRequest();
            treq->SetTopic(topic);
            for (ui32 i = 0; i < numParts; ++i)
                treq->AddPartition(i);

            TAutoPtr<NBus::TBusMessage> reply;
            NBus::EMessageStatus status = annoyingClient.SyncCall(request, reply);
            UNIT_ASSERT_VALUES_EQUAL(status, NBus::MESSAGE_OK);
            const NMsgBusProxy::TBusResponse* response = dynamic_cast<NMsgBusProxy::TBusResponse*>(reply.Get());
            UNIT_ASSERT(response);
            if (response->Record.GetStatus() == NMsgBusProxy::MSTATUS_OK)
                break;
            UNIT_ASSERT(TInstant::Now() - start < ::DEFAULT_DISPATCH_TIMEOUT);
            Sleep(TDuration::MilliSeconds(10));
        }
    }

    Y_UNIT_TEST(WhenDisableNodeAndCreateTopic_ThenAllPartitionsAreOnOtherNode) {
        NPersQueue::TTestServer server(PQSettings(0).SetDomainName("Root").SetNodeCount(2));
        server.EnableLogs({ NKikimrServices::PERSQUEUE, NKikimrServices::HIVE });
        TString unusedTopic = "rt3.dc1--unusedtopic";
        server.AnnoyingClient->CreateTopic(unusedTopic, 1);
        WaitResolveSuccess(*server.AnnoyingClient, unusedTopic, 1);

        // Act
        // Disable node #0
        server.AnnoyingClient->MarkNodeInHive(server.CleverServer->GetRuntime(), 0, false);
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 3);
        WaitResolveSuccess(*server.AnnoyingClient, DEFAULT_TOPIC_NAME, 3);

        // Assert that all partitions are on node #1
        const ui32 node1Id = server.CleverServer->GetRuntime()->GetNodeId(1);
        UNIT_ASSERT_VALUES_EQUAL(
            server.AnnoyingClient->GetPartLocation({{DEFAULT_TOPIC_NAME, {0, 1}}}, 2, true),
            TVector<ui32>({node1Id, node1Id})
        );
    }

    void PrepareForGrpc(NPersQueue::TTestServer& server) {
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 2);
    }

    class TTestCredentialsProvider : public NYdb::ICredentialsProvider {
        public:

        TTestCredentialsProvider(const NYdb::TStringType& token)
        : Token(token)
        {}

        virtual ~TTestCredentialsProvider()
        {}

        NYdb::TStringType GetAuthInfo() const override {
            return Token;
        }

        void SetToken(const NYdb::TStringType& token) {
            Token = token;
        }
        bool IsValid() const override {
            return true;
        }

        NYdb::TStringType Token;
    };

    class TTestCredentialsProviderFactory : public NYdb::ICredentialsProviderFactory {
    public:
        TTestCredentialsProviderFactory(const NYdb::TStringType& token)
        : CredentialsProvider(new TTestCredentialsProvider(token))
        {}

        TTestCredentialsProviderFactory(const TTestCredentialsProviderFactory&) = delete;
        TTestCredentialsProviderFactory& operator = (const TTestCredentialsProviderFactory&) = delete;

        virtual ~TTestCredentialsProviderFactory()
        {}

        std::shared_ptr<NYdb::ICredentialsProvider> CreateProvider() const override {
            return CredentialsProvider;
        }

        NYdb::TStringType GetClientIdentity() const override {
            return CreateGuidAsString();
        }

        void SetToken(const NYdb::TStringType& token) {
            CredentialsProvider->SetToken(token);
        }
    private:
        std::shared_ptr<TTestCredentialsProvider> CredentialsProvider;


    };


    Y_UNIT_TEST(CheckACLForGrpcWrite) {
        NPersQueue::TTestServer server(PQSettings(0, 1));
        server.EnableLogs({ NKikimrServices::PQ_WRITE_PROXY });
        PrepareForGrpc(server);

        TPQDataWriter writer("source1", server);
        TPQDataWriter writer2("source1", server);

        server.CleverServer->GetRuntime()->GetAppData().PQConfig.SetRequireCredentialsInNewProtocol(true);

        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue1"}, true, TString()); // Fail if user set empty token
        writer.Write(SHORT_TOPIC_NAME, {"valuevaluevalue1"}, true, "topic1@" BUILTIN_ACL_DOMAIN);

        NACLib::TDiffACL acl;
        acl.AddAccess(NACLib::EAccessType::Allow, NACLib::UpdateRow, "topic1@" BUILTIN_ACL_DOMAIN);
        server.AnnoyingClient->ModifyACL("/Root/PQ", DEFAULT_TOPIC_NAME, acl.SerializeAsString());
        WaitACLModification();

        writer2.Write(SHORT_TOPIC_NAME, {"valuevaluevalue1"}, false, "topic1@" BUILTIN_ACL_DOMAIN);
        writer2.Write(SHORT_TOPIC_NAME, {"valuevaluevalue1"}, true, "invalid_ticket");

        auto driver = server.AnnoyingClient->GetDriver();


        for (ui32 i = 0; i < 2; ++i) {
            std::shared_ptr<NYdb::ICredentialsProviderFactory> creds = std::make_shared<TTestCredentialsProviderFactory>(NYdb::TStringType("topic1@" BUILTIN_ACL_DOMAIN));
            dynamic_cast<TTestCredentialsProviderFactory*>(creds.get())->SetToken(NYdb::TStringType("topic1@" BUILTIN_ACL_DOMAIN));

            auto writer = CreateWriter(*driver, SHORT_TOPIC_NAME, "123", {}, {}, {}, creds);

            auto msg = writer->GetEvent(true);
            UNIT_ASSERT(msg); // ReadyToAcceptEvent

            Cerr << DebugString(*msg) << "\n";

            auto ev = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TReadyToAcceptEvent>(&*msg);
            UNIT_ASSERT(ev);

            writer->Write(std::move(ev->ContinuationToken), "a");

            msg = writer->GetEvent(true);
            UNIT_ASSERT(msg);
            Cerr << DebugString(*msg) << "\n";
            ev = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TReadyToAcceptEvent>(&*msg);
            UNIT_ASSERT(ev);

            msg = writer->GetEvent(true);
            UNIT_ASSERT(msg);
            Cerr << DebugString(*msg) << "\n";
            auto ack = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TAcksEvent>(&*msg);
            UNIT_ASSERT(ack);

            NYdb::TStringType token = i == 0 ? "user_without_rights@" BUILTIN_ACL_DOMAIN : "invalid_ticket";
            Cerr << "Set token " << token << "\n";

            dynamic_cast<TTestCredentialsProviderFactory*>(creds.get())->SetToken(token);

            writer->Write(std::move(ev->ContinuationToken), "a");
            ui32 events = 0;
            while(true) {
                UNIT_ASSERT(writer->WaitEvent().Wait(TDuration::Seconds(10)));
                msg = writer->GetEvent(true);
                UNIT_ASSERT(msg);
                Cerr << DebugString(*msg) << "\n";
                if (std::holds_alternative<NYdb::NPersQueue::TSessionClosedEvent>(*msg))
                    break;
                UNIT_ASSERT(++events <= 2); // Before close only one ack and one ready-to-accept can be received
            }
        }
    }


    Y_UNIT_TEST(CheckACLForGrpcRead) {
        NPersQueue::TTestServer server(PQSettings(0, 1));
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY });
        server.EnableLogs({ NKikimrServices::PERSQUEUE }, NActors::NLog::PRI_INFO);
        TString topic2 = DEFAULT_TOPIC_NAME + "2";
        TString shortTopic2Name = "topic12";
        PrepareForGrpc(server);

        server.AnnoyingClient->CreateTopic(topic2, 1, 8*1024*1024, 86400, 20000000, "", 200000000, {"user1", "user2"});
        server.WaitInit(shortTopic2Name);
        server.AnnoyingClient->CreateConsumer("user1");
        server.AnnoyingClient->CreateConsumer("user2");
        server.AnnoyingClient->CreateConsumer("user5");
        server.AnnoyingClient->GrantConsumerAccess("user1", "user2@" BUILTIN_ACL_DOMAIN);
        server.AnnoyingClient->GrantConsumerAccess("user1", "user3@" BUILTIN_ACL_DOMAIN);

        server.AnnoyingClient->GrantConsumerAccess("user1", "1@" BUILTIN_ACL_DOMAIN);
        server.AnnoyingClient->GrantConsumerAccess("user2", "2@" BUILTIN_ACL_DOMAIN);
        server.AnnoyingClient->GrantConsumerAccess("user5", "1@" BUILTIN_ACL_DOMAIN);
        server.AnnoyingClient->GrantConsumerAccess("user5", "2@" BUILTIN_ACL_DOMAIN);
        Cerr << "=== Create writer\n";
        TPQDataWriter writer("source1", server);

        server.CleverServer->GetRuntime()->GetAppData().PQConfig.SetRequireCredentialsInNewProtocol(true);

        NACLib::TDiffACL acl;
        acl.AddAccess(NACLib::EAccessType::Allow, NACLib::SelectRow, "1@" BUILTIN_ACL_DOMAIN);
        acl.AddAccess(NACLib::EAccessType::Allow, NACLib::SelectRow, "2@" BUILTIN_ACL_DOMAIN);
        acl.AddAccess(NACLib::EAccessType::Allow, NACLib::SelectRow, "user1@" BUILTIN_ACL_DOMAIN);
        acl.AddAccess(NACLib::EAccessType::Allow, NACLib::SelectRow, "user2@" BUILTIN_ACL_DOMAIN);
        server.AnnoyingClient->ModifyACL("/Root/PQ", topic2, acl.SerializeAsString());
        WaitACLModification();

        auto ticket1 = "1@" BUILTIN_ACL_DOMAIN;
        auto ticket2 = "2@" BUILTIN_ACL_DOMAIN;

        Cerr << "=== Writer - do reads\n";
        writer.Read(shortTopic2Name, "user1", ticket1, false, false, true);

        writer.Read(shortTopic2Name, "user1", "user2@" BUILTIN_ACL_DOMAIN, false, false, true);
        writer.Read(shortTopic2Name, "user1", "user3@" BUILTIN_ACL_DOMAIN, true, false, true); //for topic
        writer.Read(shortTopic2Name, "user1", "user1@" BUILTIN_ACL_DOMAIN, true, false, true); //for consumer
        writer.Read(shortTopic2Name, "user2", ticket1, true, false, true);
        writer.Read(shortTopic2Name, "user2", ticket2, false, false, true);

        writer.Read(shortTopic2Name, "user5", ticket1, true, false, true);
        writer.Read(shortTopic2Name, "user5", ticket2, true, false, true);

        acl.Clear();
        acl.AddAccess(NACLib::EAccessType::Allow, NACLib::SelectRow, "user3@" BUILTIN_ACL_DOMAIN);
        server.AnnoyingClient->ModifyACL("/Root/PQ", topic2, acl.SerializeAsString());
        WaitACLModification();

        Cerr << "==== Writer - read\n";
        writer.Read(shortTopic2Name, "user1", "user3@" BUILTIN_ACL_DOMAIN, false, true, true);

        auto Channel_ = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
        auto StubP_ = Ydb::PersQueue::V1::PersQueueService::NewStub(Channel_);

/*        auto driver = server.AnnoyingClient->GetDriver();

        Cerr << "==== Start consuming loop\n";
        for (ui32 i = 0; i < 2; ++i) {

            std::shared_ptr<NYdb::ICredentialsProviderFactory> creds = std::make_shared<TTestCredentialsProviderFactory>(NYdb::TStringType("user3@" BUILTIN_ACL_DOMAIN));

            NYdb::NPersQueue::TReadSessionSettings settings;
            settings.ConsumerName("user1").AppendTopics(shortTopic2Name).ReadOriginal({"dc1"});
            auto reader = CreateReader(*driver, settings, creds);

            auto msg = reader->GetEvent(true, 1);
            UNIT_ASSERT(msg);

            Cerr << NYdb::NPersQueue::DebugString(*msg) << "\n";

            auto ev = std::get_if<NYdb::NPersQueue::TReadSessionEvent::TCreatePartitionStreamEvent>(&*msg);

            UNIT_ASSERT(ev);
            dynamic_cast<TTestCredentialsProviderFactory*>(creds.get())->SetToken(i == 0 ? "user_without_rights@" BUILTIN_ACL_DOMAIN : "invalid_ticket");

            ev->Confirm();

            Cerr << "=== Wait for consumer death (" << i << ")" << Endl;

            msg = reader->GetEvent(true, 1);
            UNIT_ASSERT(msg);

            Cerr << NYdb::NPersQueue::DebugString(*msg) << "\n";

            auto closeEv = std::get_if<NYdb::NPersQueue::TSessionClosedEvent>(&*msg);

            UNIT_ASSERT(closeEv);
        }
*/
        Cerr << "==== Start second loop\n";
        for (ui32 i = 0; i < 3; ++i){
            server.AnnoyingClient->GetClientInfo({topic2}, "user1", true);

            ReadInfoRequest request;
            ReadInfoResponse response;
            request.mutable_consumer()->set_path("user1");
            request.set_get_only_original(true);
            request.add_topics()->set_path(shortTopic2Name);
            grpc::ClientContext rcontext;
            if (i == 0) {
                rcontext.AddMetadata("x-ydb-auth-ticket", "user_without_rights@" BUILTIN_ACL_DOMAIN);
            }
            if (i == 1) {
                rcontext.AddMetadata("x-ydb-auth-ticket", "invalid_ticket");
            }
            if (i == 2) {
                rcontext.AddMetadata("x-ydb-auth-ticket", "user3@" BUILTIN_ACL_DOMAIN);
            }
            auto status = StubP_->GetReadSessionsInfo(&rcontext, request, &response);
            UNIT_ASSERT(status.ok());
            ReadInfoResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT(response.operation().ready() == true);
            UNIT_ASSERT(response.operation().status() == (i < 2) ? Ydb::StatusIds::UNAUTHORIZED : Ydb::StatusIds::SUCCESS);
        }
    }

    Y_UNIT_TEST(CheckKillBalancer) {
        NPersQueue::TTestServer server;
        server.EnableLogs({ NKikimrServices::PQ_WRITE_PROXY, NKikimrServices::PQ_READ_PROXY});
        PrepareForGrpc(server);

        TPQDataWriter writer("source1", server);


        auto driver = server.AnnoyingClient->GetDriver();

        NYdb::NPersQueue::TReadSessionSettings settings;
        settings.ConsumerName("shared/user").AppendTopics(SHORT_TOPIC_NAME).ReadOriginal({"dc1"});
        auto reader = CreateReader(*driver, settings);
        for (ui32 i = 0; i < 2; ++i) {
            auto msg = reader->GetEvent(true, 1);
            UNIT_ASSERT(msg);

            Cerr << NYdb::NPersQueue::DebugString(*msg) << "\n";

            auto ev = std::get_if<NYdb::NPersQueue::TReadSessionEvent::TCreatePartitionStreamEvent>(&*msg);

            UNIT_ASSERT(ev);

            ev->Confirm();
        }

        server.AnnoyingClient->RestartBalancerTablet(server.CleverServer->GetRuntime(), "rt3.dc1--topic1");
        Cerr << "Balancer killed\n";


        ui32 createEv = 0, destroyEv = 0;
        for (ui32 i = 0; i < 4; ++i) {
            auto msg = reader->GetEvent(true, 1);
            UNIT_ASSERT(msg);

            Cerr << "Got message: " << NYdb::NPersQueue::DebugString(*msg) << "\n";

            auto ev1 = std::get_if<NYdb::NPersQueue::TReadSessionEvent::TPartitionStreamClosedEvent>(&*msg);
            auto ev2 = std::get_if<NYdb::NPersQueue::TReadSessionEvent::TCreatePartitionStreamEvent>(&*msg);

            UNIT_ASSERT(ev1 || ev2);

            if (ev1) {
                ++destroyEv;
            }
            if (ev2) {
                ev2->Confirm();
                ++createEv;
            }
        }
        UNIT_ASSERT(createEv == 2);
        UNIT_ASSERT(destroyEv == 2);

        UNIT_ASSERT(!reader->WaitEvent().Wait(TDuration::Seconds(1)));
    }


    Y_UNIT_TEST(TestWriteStat) {
        NPersQueue::TTestServer server(PQSettings(0, 1, true, "10"), false);
        auto netDataUpdated = server.PrepareNetDataFile(FormNetData());
        UNIT_ASSERT(netDataUpdated);
        server.StartServer();

        server.EnableLogs({ NKikimrServices::PQ_WRITE_PROXY, NKikimrServices::NET_CLASSIFIER });
        server.EnableLogs({ NKikimrServices::PERSQUEUE }, NActors::NLog::PRI_ERROR);

        auto sender = server.CleverServer->GetRuntime()->AllocateEdgeActor();

        GetClassifierUpdate(*server.CleverServer, sender); //wait for initializing

        server.AnnoyingClient->CreateTopic("rt3.dc1--account--topic1", 10, 10000, 10000, 2000);
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 10, 10000, 10000, 2000);

        auto driver = server.AnnoyingClient->GetDriver();

        auto writer = CreateWriter(*driver, "account/topic1", "base64:AAAAaaaa____----12", 0, "raw");

        auto msg = writer->GetEvent(true);
        UNIT_ASSERT(msg); // ReadyToAcceptEvent

        auto ev = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TReadyToAcceptEvent>(&*msg);
        UNIT_ASSERT(ev);

        TInstant st(TInstant::Now());
        for (ui32 i = 1; i <= 5; ++i) {
            writer->Write(std::move(ev->ContinuationToken), TString(2000, 'a'));
            msg = writer->GetEvent(true);
            UNIT_ASSERT(msg); // ReadyToAcceptEvent

            ev = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TReadyToAcceptEvent>(&*msg);
            UNIT_ASSERT(ev);

            msg = writer->GetEvent(true);

            Cerr << DebugString(*msg) << "\n";

            auto ack = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TAcksEvent>(&*msg);
            UNIT_ASSERT(ack);

            if (i == 5) {
                UNIT_ASSERT(TInstant::Now() - st > TDuration::Seconds(3));
                // TODO: Describe this assert in comment
                UNIT_ASSERT(!ack->Acks.empty());
                UNIT_ASSERT(ack->Acks.back().Stat);
                UNIT_ASSERT(ack->Acks.back().Stat->TotalTimeInPartitionQueue >= ack->Acks.back().Stat->PartitionQuotedTime);
                UNIT_ASSERT(ack->Acks.back().Stat->TotalTimeInPartitionQueue <= ack->Acks.back().Stat->PartitionQuotedTime + TDuration::Seconds(10));
            }
        }
        GetCounters(server.CleverServer->GetRuntime()->GetMonPort(), "pqproxy", "writeSession", "account/topic1", "Vla");
        {
            NYdb::NPersQueue::TReadSessionSettings settings;
            settings.ConsumerName("shared/user").AppendTopics(TString("account/topic1")).ReadOriginal({"dc1"});

            auto reader = CreateReader(*driver, settings);

            auto msg = GetNextMessageSkipAssignment(reader);
            UNIT_ASSERT(msg);

            Cerr << NYdb::NPersQueue::DebugString(*msg) << "\n";
        }
        GetCounters(server.CleverServer->GetRuntime()->GetMonPort(), "pqproxy", "readSession", "account/topic1", "Vla");
    }

    Y_UNIT_TEST(TestWriteSessionsConflicts) {
        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1);

        TPQDataWriter writer("source", server);

        server.EnableLogs({ NKikimrServices::PQ_WRITE_PROXY });

        TString topic = SHORT_TOPIC_NAME;
        TString sourceId = "123";


        auto driver = server.AnnoyingClient->GetDriver();

        auto writer1 = CreateWriter(*driver, topic, sourceId);

        auto msg = writer1->GetEvent(true);
        UNIT_ASSERT(msg); // ReadyToAcceptEvent

        Cerr << DebugString(*msg) << "\n";

        auto ev = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TReadyToAcceptEvent>(&*msg);
        UNIT_ASSERT(ev);

        auto writer2 = CreateWriter(*driver, topic, sourceId);

        msg = writer2->GetEvent(true);
        UNIT_ASSERT(msg); // ReadyToAcceptEvent

        Cerr << DebugString(*msg) << "\n";

        ev = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TReadyToAcceptEvent>(&*msg);
        UNIT_ASSERT(ev);

        // First session dies.
        UNIT_ASSERT(writer1->WaitEvent().Wait(TDuration::Seconds(10)));

        msg = writer1->GetEvent(true);
        UNIT_ASSERT(msg);

        Cerr << DebugString(*msg) << "\n";

        auto closeEv = std::get_if<NYdb::NPersQueue::TSessionClosedEvent>(&*msg);
        UNIT_ASSERT(closeEv);

        UNIT_ASSERT(!writer2->WaitEvent().Wait(TDuration::Seconds(1)));
    }

/*
    Y_UNIT_TEST(TestLockErrors) {
        return;  // Test is ignored. FIX: KIKIMR-7881

        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1);
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY });
        auto pqLib = TPQLib::WithCerrLogger();

        {
            auto [producer, pcResult] = CreateProducer(pqLib, server.GrpcPort, SHORT_TOPIC_NAME, "123");
            for (ui32 i = 1; i <= 11; ++i) {
                auto f = producer->Write(i,  TString(10, 'a'));
                f.Wait();
            }
        }

        TConsumerSettings ss;
        ss.Consumer = "user";
        ss.Server = TServerSetting{"localhost", server.GrpcPort};
        ss.Topics.push_back({SHORT_TOPIC_NAME, {}});
        ss.MaxCount = 1;
        ss.Unpack = false;

        auto [consumer, ccResult] = CreateConsumer(pqLib, ss);
        auto msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.assigned().partition() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().read_offset() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().end_offset() == 11);

        auto pp = msg.GetValue().StartRead;
        pp.SetValue(TAssignInfo{0, 5, false});
        auto future = consumer->IsDead();
        future.Wait();
        Cerr << future.GetValue() << "\n";

        std::tie(consumer, ccResult) = CreateConsumer(pqLib, ss);
        msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.assigned().partition() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().read_offset() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().end_offset() == 11);

        pp = msg.GetValue().StartRead;
        pp.SetValue(TAssignInfo{12, 12, false});
        future = consumer->IsDead();
        future.Wait();
        Cerr << future.GetValue() << "\n";

        std::tie(consumer, ccResult) = CreateConsumer(pqLib, ss);
        msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.assigned().partition() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().read_offset() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().end_offset() == 11);

        pp = msg.GetValue().StartRead;
        pp.SetValue(TAssignInfo{6, 7, false});
        future = consumer->IsDead();
        future.Wait();
        Cerr << future.GetValue() << "\n";

        std::tie(consumer, ccResult) = CreateConsumer(pqLib, ss);
        msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.assigned().partition() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().read_offset() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().end_offset() == 11);
        auto assignId = msg.GetValue().Response.assigned().assign_id();
        pp = msg.GetValue().StartRead;
        pp.SetValue(TAssignInfo{5, 0, false});
        consumer->Commit({{assignId, 0}});
        while (true) {
            msg = consumer->GetNextMessage();
            msg.Wait();
            Cerr << msg.GetValue().Response << "\n";
            if (msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kCommitted) {
                UNIT_ASSERT(msg.GetValue().Response.committed().cookies_size() == 1);
                UNIT_ASSERT(msg.GetValue().Response.committed().cookies(0).assign_id() == assignId);
                UNIT_ASSERT(msg.GetValue().Response.committed().cookies(0).partition_cookie() == 0);
                break;
            }
        }

        std::tie(consumer, ccResult) = CreateConsumer(pqLib, ss);
        msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.assigned().partition() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().read_offset() == 5);
        UNIT_ASSERT(msg.GetValue().Response.assigned().end_offset() == 11);

        pp = msg.GetValue().StartRead;
        pp.SetValue(TAssignInfo{11, 11, false});
        Sleep(TDuration::Seconds(5));

        std::tie(consumer, ccResult) = CreateConsumer(pqLib, ss);
        msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.assigned().partition() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().read_offset() == 11);
        UNIT_ASSERT(msg.GetValue().Response.assigned().end_offset() == 11);

        pp = msg.GetValue().StartRead;
        pp.SetValue(TAssignInfo{1, 0, true});
        future = consumer->IsDead();
        future.Wait();
        Cerr << future.GetValue() << "\n";

        std::tie(consumer, ccResult) = CreateConsumer(pqLib, ss);
        msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.assigned().partition() == 0);
        UNIT_ASSERT(msg.GetValue().Response.assigned().read_offset() == 11);
        UNIT_ASSERT(msg.GetValue().Response.assigned().end_offset() == 11);

        pp = msg.GetValue().StartRead;
        pp.SetValue(TAssignInfo{0, 0, false});
        future = consumer->IsDead();
        UNIT_ASSERT(!future.Wait(TDuration::Seconds(5)));
    }
*/

    Y_UNIT_TEST(TestBigMessage) {
        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1);

        server.EnableLogs({ NKikimrServices::PQ_WRITE_PROXY });

        TPQDataWriter writer2("source", server);

        auto driver = server.AnnoyingClient->GetDriver();

        auto writer = CreateWriter(*driver, SHORT_TOPIC_NAME, "123", 0, "raw");

        auto msg = writer->GetEvent(true);
        UNIT_ASSERT(msg); // ReadyToAcceptEvent

        auto ev = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TReadyToAcceptEvent>(&*msg);
        UNIT_ASSERT(ev);

        writer->Write(std::move(ev->ContinuationToken), TString((1 << 20) * 60, 'a')); //TODO: Increase GRPC_ARG_MAX_SEND_MESSAGE_LENGTH
        {
            msg = writer->GetEvent(true);
            UNIT_ASSERT(msg); // ReadyToAcceptEvent
            auto ev2 = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TReadyToAcceptEvent>(&*msg);
            UNIT_ASSERT(ev2);
        }
        {
            msg = writer->GetEvent(true);
            UNIT_ASSERT(msg); // Ack on write
            auto ev2 = std::get_if<NYdb::NPersQueue::TWriteSessionEvent::TAcksEvent>(&*msg);
            UNIT_ASSERT(ev2);
        }
    }

/*
    void TestRereadsWhenDataIsEmptyImpl(bool withWait) {
        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1);

        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY });
        TPQDataWriter writer("source", server);
        auto pqLib = TPQLib::WithCerrLogger();

        // Write nonempty data
        NKikimr::NPersQueueTests::TRequestWritePQ writeReq(DEFAULT_TOPIC_NAME, 0, "src", 4);

        auto write = [&](const TString& data, bool empty = false) {
            NKikimrPQClient::TDataChunk dataChunk;
            dataChunk.SetCreateTime(42);
            dataChunk.SetSeqNo(++writeReq.SeqNo);
            dataChunk.SetData(data);
            if (empty) {
                dataChunk.SetChunkType(NKikimrPQClient::TDataChunk::GROW); // this guarantees that data will be threated as empty
            }
            TString serialized;
            UNIT_ASSERT(dataChunk.SerializeToString(&serialized));
            server.AnnoyingClient->WriteToPQ(writeReq, serialized);
        };
        write("data1");
        write("data2", true);
        if (!withWait) {
            write("data3");
        }

        ui32 maxCount = 1;
        bool unpack = false;
        ui32 maxInflyRequests = 1;
        ui32 maxMemoryUsage = 1;
        auto [consumer, ccResult] = CreateConsumer(
                pqLib, server.GrpcPort, "user", {SHORT_TOPIC_NAME, {}},
                maxCount, unpack, {}, maxInflyRequests, maxMemoryUsage
        );
        UNIT_ASSERT_C(ccResult.Response.response_case() == MigrationStreamingReadServerMessage::kInitResponse, ccResult.Response);

        auto msg1 = GetNextMessageSkipAssignment(consumer).GetValueSync().Response;

        auto assertHasData = [](const MigrationStreamingReadServerMessage& msg, const TString& data) {
            const auto& d = msg.data_batch();
            UNIT_ASSERT_VALUES_EQUAL_C(d.partition_data_size(), 1, msg);
            UNIT_ASSERT_VALUES_EQUAL_C(d.partition_data(0).batches_size(), 1, msg);
            UNIT_ASSERT_VALUES_EQUAL_C(d.partition_data(0).batches(0).message_data_size(), 1, msg);
            UNIT_ASSERT_VALUES_EQUAL_C(d.partition_data(0).batches(0).message_data(0).data(), data, msg);
        };
        UNIT_ASSERT_VALUES_EQUAL_C(msg1.data_batch().partition_data(0).cookie().partition_cookie(), 1, msg1);
        assertHasData(msg1, "data1");

        auto resp2Future = consumer->GetNextMessage();
        if (withWait) {
            // no data
            UNIT_ASSERT(!resp2Future.HasValue());
            UNIT_ASSERT(!resp2Future.HasException());

            // waits and data doesn't arrive
            Sleep(TDuration::MilliSeconds(100));
            UNIT_ASSERT(!resp2Future.HasValue());
            UNIT_ASSERT(!resp2Future.HasException());

            // write data
            write("data3");
        }
        const auto& msg2 = resp2Future.GetValueSync().Response;
        UNIT_ASSERT_VALUES_EQUAL_C(msg2.data_batch().partition_data(0).cookie().partition_cookie(), 2, msg2);

        assertHasData(msg2, "data3");
    }

    Y_UNIT_TEST(TestRereadsWhenDataIsEmpty) {
        TestRereadsWhenDataIsEmptyImpl(false);
    }

    Y_UNIT_TEST(TestRereadsWhenDataIsEmptyWithWait) {
        TestRereadsWhenDataIsEmptyImpl(true);
    }


    Y_UNIT_TEST(TestLockAfterDrop) {
        NPersQueue::TTestServer server{false};
        server.GrpcServerOptions.SetMaxMessageSize(130*1024*1024);
        server.StartServer();
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1);
        server.WaitInit(SHORT_TOPIC_NAME);
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY });
        auto pqLib = TPQLib::WithCerrLogger();

        auto [producer, pcResult] = CreateProducer(pqLib, server.GrpcPort, SHORT_TOPIC_NAME, "123");
        auto f = producer->Write(1,  TString(1024, 'a'));
        f.Wait();

        ui32 maxCount = 1;
        bool unpack = false;
        auto [consumer, ccResult] = CreateConsumer(pqLib, server.GrpcPort, "user", {SHORT_TOPIC_NAME, {}}, maxCount, unpack);
        Cerr << ccResult.Response << "\n";

        auto msg = consumer->GetNextMessage();
        msg.Wait();
        UNIT_ASSERT_C(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned, msg.GetValue().Response);
        UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.assigned().partition() == 0);

        server.CleverServer->GetRuntime()->ResetScheduledCount();
        server.AnnoyingClient->RestartPartitionTablets(server.CleverServer->GetRuntime(), DEFAULT_TOPIC_NAME);

        msg.GetValue().StartRead.SetValue({0,0,false});

        msg = consumer->GetNextMessage();
        UNIT_ASSERT(msg.Wait(TDuration::Seconds(10)));

        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kDataBatch);
    }


    Y_UNIT_TEST(TestMaxNewTopicModel) {
        NPersQueue::TTestServer server;
        server.AnnoyingClient->AlterUserAttributes("/", "Root", {{"__extra_path_symbols_allowed", "@"}});
        server.AnnoyingClient->CreateTopic("rt3.dc1--aaa@bbb@ccc--topic", 1);
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1);

        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY });
        auto pqLib = TPQLib::WithCerrLogger();

        {
            auto [producer, pcResult] = CreateProducer(pqLib, server.GrpcPort, "aaa/bbb/ccc/topic", "123");
            UNIT_ASSERT_C(pcResult.Response.server_message_case() == StreamingWriteServerMessage::kInitResponse, pcResult.Response);
            for (ui32 i = 1; i <= 11; ++i) {
                auto f = producer->Write(i,  TString(10, 'a'));
                f.Wait();
                UNIT_ASSERT_C(f.GetValue().Response.server_message_case() == StreamingWriteServerMessage::kBatchWriteResponse, f.GetValue().Response);
            }
        }

        ui32 maxCount = 1;
        bool unpack = false;
        auto [consumer, ccResult] = CreateConsumer(pqLib, server.GrpcPort, "user", {"aaa/bbb/ccc/topic", {}}, maxCount, unpack);
        UNIT_ASSERT_C(ccResult.Response.response_case() == MigrationStreamingReadServerMessage::kInitResponse, ccResult.Response);

        auto msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
    }


    Y_UNIT_TEST(TestReleaseWithAssigns) {
        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 3);

        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY });
        auto pqLib = TPQLib::WithCerrLogger();

        TPQDataWriter writer("source", server);

        for (ui32 i = 1; i <= 3; ++i) {
            TString sourceId = "123" + ToString<int>(i);
            ui32 partitionGroup = i;
            auto [producer, pcResult] = CreateProducer(pqLib, server.GrpcPort, SHORT_TOPIC_NAME, sourceId, partitionGroup);

            UNIT_ASSERT(pcResult.Response.server_message_case() == StreamingWriteServerMessage::kInitResponse);
            auto f = producer->Write(i,  TString(10, 'a'));
            f.Wait();
        }

        TConsumerSettings ss;
        ss.Consumer = "user";
        ss.Server = TServerSetting{"localhost", server.GrpcPort};
        ss.Topics.push_back({SHORT_TOPIC_NAME, {}});
        ss.ReadMirroredPartitions = false;
        ss.MaxCount = 3;
        ss.Unpack = false;

        auto [consumer, ccResult] = CreateConsumer(pqLib, ss);
        Cerr << ccResult.Response << "\n";

        for (ui32 i = 1; i <= 3; ++i) {
            auto msg = consumer->GetNextMessage();
            msg.Wait();
            Cerr << msg.GetValue().Response << "\n";
            UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
            UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
            UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");
        }

        auto [consumer2, ccResult2] = CreateConsumer(pqLib, ss);
        Cerr << ccResult2.Response << "\n";

        auto msg = consumer->GetNextMessage();
        auto msg2 = consumer2->GetNextMessage();

        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kRelease);
        UNIT_ASSERT(msg.GetValue().Response.release().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.release().cluster() == "dc1");

        UNIT_ASSERT(!msg2.Wait(TDuration::Seconds(1)));

        msg.GetValue().Release.SetValue();

        msg2.Wait();
        Cerr << msg2.GetValue().Response << "\n";

        UNIT_ASSERT(msg2.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg2.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg2.GetValue().Response.assigned().cluster() == "dc1");
    }

    Y_UNIT_TEST(TestSilentRelease) {
        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 3);

        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY });
        auto pqLib = TPQLib::WithCerrLogger();

        TPQDataWriter writer("source", server);

        TVector<std::pair<ui64, ui64>> cookies;

        for (ui32 i = 1; i <= 3; ++i) {
            TString sourceId = "123" + ToString<int>(i);
            ui32 partitionGroup = i;

            auto [producer, pcResult] = CreateProducer(pqLib, server.GrpcPort, SHORT_TOPIC_NAME, sourceId, partitionGroup);
            Cerr << "===Response: " << pcResult.Response << Endl;
            UNIT_ASSERT(pcResult.Response.server_message_case() == StreamingWriteServerMessage::kInitResponse);
            auto f = producer->Write(i,  TString(10, 'a'));
            f.Wait();
        }

        TConsumerSettings ss;
        ss.Consumer = "user";
        ss.Server = TServerSetting{"localhost", server.GrpcPort};
        ss.Topics.push_back({SHORT_TOPIC_NAME, {}});
        ss.ReadMirroredPartitions = false;
        ss.MaxCount = 1;
        ss.Unpack = false;

        auto [consumer, ccResult] = CreateConsumer(pqLib, ss);
        Cerr << ccResult.Response << "\n";

        for (ui32 i = 1; i <= 3; ++i) {
            auto msg = GetNextMessageSkipAssignment(consumer);
            Cerr << msg.GetValueSync().Response << "\n";
            UNIT_ASSERT(msg.GetValueSync().Response.response_case() == MigrationStreamingReadServerMessage::kDataBatch);
            for (auto& p : msg.GetValue().Response.data_batch().partition_data()) {
                cookies.emplace_back(p.cookie().assign_id(), p.cookie().partition_cookie());
            }
        }

        auto [consumer2, ccResult2] = CreateConsumer(pqLib, ss);
        Cerr << ccResult2.Response << "\n";

        auto msg = consumer->GetNextMessage();
        auto msg2 = consumer2->GetNextMessage();
        UNIT_ASSERT(!msg2.Wait(TDuration::Seconds(1)));
        consumer->Commit(cookies);

        if (msg.GetValueSync().Release.Initialized()) {
            msg.GetValueSync().Release.SetValue();
        }

        msg2.Wait();
        Cerr << msg2.GetValue().Response << "\n";

        UNIT_ASSERT(msg2.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg2.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg2.GetValue().Response.assigned().cluster() == "dc1");
        UNIT_ASSERT(msg2.GetValue().Response.assigned().read_offset() == 1);
    }

*/

/*
    Y_UNIT_TEST(TestDoubleRelease) {
        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic(
                DEFAULT_TOPIC_NAME, 1, 8000000, 86400, 50000000, "", 50000000, {"user1"}, {"user1", "user2"}
        );
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY, NKikimrServices::TABLET_AGGREGATOR });
        auto pqLib = TPQLib::WithCerrLogger();
        TPQDataWriter writer("source", server);

        TConsumerSettings ss;
        ss.Consumer = "user";
        ss.Server = TServerSetting{"localhost", server.GrpcPort};
        ss.Topics.push_back({SHORT_TOPIC_NAME, {}});
        ss.ReadMirroredPartitions = false;
        ss.MaxCount = 3;
        ss.Unpack = false;

        auto [consumer, ccResult] = CreateConsumer(pqLib, ss);
        Cerr << ccResult.Response << "\n";

        auto msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kAssigned);
        UNIT_ASSERT(msg.GetValue().Response.assigned().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.assigned().cluster() == "dc1");

        msg = consumer->GetNextMessage();
        UNIT_ASSERT(!msg.Wait(TDuration::Seconds(1)));

        THolder<IConsumer> consumer2;
        do {
            std::tie(consumer2, ccResult) = CreateConsumer(pqLib, ss);
            Cerr << ccResult.Response << "\n";
        } while(!msg.Wait(TDuration::Seconds(1)));

        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kRelease);
        UNIT_ASSERT(msg.GetValue().Response.release().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.release().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.release().forceful_release() == false);

        msg = consumer->GetNextMessage();
        UNIT_ASSERT(!msg.Wait(TDuration::Seconds(1)));

        server.AnnoyingClient->RestartBalancerTablet(server.CleverServer->GetRuntime(), DEFAULT_TOPIC_NAME);
        UNIT_ASSERT(msg.Wait(TDuration::Seconds(1)));

        Cerr << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Response.response_case() == MigrationStreamingReadServerMessage::kRelease);
        UNIT_ASSERT(msg.GetValue().Response.release().topic().path() == SHORT_TOPIC_NAME);
        UNIT_ASSERT(msg.GetValue().Response.release().cluster() == "dc1");
        UNIT_ASSERT(msg.GetValue().Response.release().forceful_release() == true);

        THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response;
        TActorId edge = server.CleverServer->GetRuntime()->AllocateEdgeActor();

        do {

            auto actorId = NKikimr::CreateClusterLabeledCountersAggregatorActor(edge, TTabletTypes::PersQueue, 3, "rt3.*--*,user*!!/!!*!!/rt3.*--*", 0); // remove !!
            server.CleverServer->GetRuntime()->Register(actorId);
            response = server.CleverServer->GetRuntime()->
                            GrabEdgeEvent<TEvTabletCounters::TEvTabletLabeledCountersResponse>();

           Cerr << "FINAL RESPONSE :\n" << response->Record.DebugString() << Endl;
        } while (response->Record.LabeledCountersByGroupSize() == 0);

        Cerr << "MULITREQUEST\n";

        auto actorId = NKikimr::CreateClusterLabeledCountersAggregatorActor(edge, TTabletTypes::PersQueue, 3, "rt3.*--*,user*!!/!!*!!/rt3.*--*", 3); // remove !!
        server.CleverServer->GetRuntime()->Register(actorId);
        response = server.CleverServer->GetRuntime()->
                        GrabEdgeEvent<TEvTabletCounters::TEvTabletLabeledCountersResponse>();

        Cerr << "FINAL RESPONSE2 :\n" << response->Record.DebugString() << Endl;
        UNIT_ASSERT(response->Record.LabeledCountersByGroupSize());

    }

    Y_UNIT_TEST(TestUncompressedSize) {
        TRateLimiterTestSetup setup(NKikimrPQ::TPQConfig::TQuotingConfig::USER_PAYLOAD_SIZE);

        setup.CreateTopic("account/topic");

        THolder<IProducer> producer = setup.StartProducer("account/topic", true);

        TString data = TString("12345") * 100;
        for (ui32 i = 0; i < 100; ++i) {
            producer->Write(data);
        }
        auto writeResult = producer->Write(data);
        const auto& writeResponse = writeResult.GetValueSync().Response;
        UNIT_ASSERT_EQUAL_C(Ydb::StatusIds::SUCCESS, writeResponse.status(), "Response: " << writeResponse);

        auto pqLib = TPQLib::WithCerrLogger();
        using namespace NPersQueue;
        auto [consumer, ccResult] = CreateConsumer(pqLib, setup.GetGrpcPort(), "shared/user", {"/account/topic" , {}}, 1000, false);
        Cerr << ccResult.Response << "\n";

        auto msg = consumer->GetNextMessage();
        msg.Wait();
        Cerr << "ASSIGN RESPONSE: " << msg.GetValue().Response << "\n";
        UNIT_ASSERT(msg.GetValue().Type == EMT_ASSIGNED);
        auto pp = msg.GetValue().StartRead;
        pp.SetValue(TAssignInfo());
        Cerr << "START READ\n";
        ui32 count = 0;
        do {
            msg = consumer->GetNextMessage();
            msg.Wait();
            Cerr << "GOT LAST MESSAGE: " << msg.GetValue().Response << "\n";
            for (auto& pd : msg.GetValue().Response.data_batch().partition_data()) {
                for (auto & b : pd.batches()) {
                    for (auto& md : b.message_data()) {
                        UNIT_ASSERT(md.uncompressed_size() == 500);
                        ++count;
                    }
                }
            }
            Cerr << count << "\n";
        } while (count < 100);
    }

    Y_UNIT_TEST(TestReadQuotasSimple) {
        TRateLimiterTestSetup setup(NKikimrPQ::TPQConfig::TQuotingConfig::USER_PAYLOAD_SIZE, 1000, 1000, true);

        const TString topicPath = "acc/topic1";
        const TString consumerPath = "acc2/reader1";
        setup.CreateTopic(topicPath);
        setup.CreateConsumer(consumerPath);

        THolder<IProducer> producer = setup.StartProducer(topicPath, true);

        auto pqLib = TPQLib::WithCerrLogger();

        auto [consumer, ccResult] = CreateConsumer(
                pqLib, setup.GetGrpcPort(), consumerPath, {topicPath , {}}, 1000, false
        );
        Cerr << ccResult.Response << "\n";

        {
            auto msg = consumer->GetNextMessage();
            msg.Wait();
            Cerr << "consumer assign response: " << msg.GetValue().Response << "\n";
            UNIT_ASSERT(msg.GetValue().Type == EMT_ASSIGNED);
            msg.GetValue().StartRead.SetValue(TAssignInfo());
        }

        TVector<NThreading::TFuture<Ydb::PersQueue::TProducerCommitResponse>> writeResults;
        TVector<NThreading::TFuture<Ydb::PersQueue::TConsumerMessage>> readResults;

        for (ui32 readBatches = 0; readBatches < 10; ++readBatches) {
            auto msg = consumer->GetNextMessage();
            while (!msg.HasValue()) {
                producer->Write(TString(std::string(10000, 'A')));
                Sleep(TDuration::MilliSeconds(10));
            }
            const auto& response = msg.GetValue().Response;
            Cerr << "next read response: " << response << "\n";

            for (auto& data : response.data_batch().partition_data()) {
                for (auto& batch : data.batches()) {
                    UNIT_ASSERT(batch.message_data_size() > 0);
                }
            }
        }
    }

    Y_UNIT_TEST(TestReadWithQuoterWithoutResources) {
        if (NSan::ASanIsOn()) {
            return;
        }
        TRateLimiterTestSetup setup(NKikimrPQ::TPQConfig::TQuotingConfig::USER_PAYLOAD_SIZE, 1000, 1000, true);

        const TString topicPath = "acc/topic1";
        const TString consumerPath = "acc2/reader1"; // don't create kesus resources
        setup.CreateTopic(topicPath);

        THolder<IProducer> producer = setup.StartProducer(topicPath, true);

        TPQLibSettings pqLibSettings({ .DefaultLogger = new TCerrLogger(DEBUG_LOG_LEVEL) });
        TPQLib PQLib(pqLibSettings);
        auto [consumer, ccResult] = CreateConsumer(PQLib, setup.GetGrpcPort(), consumerPath, {topicPath , {}}, 1000, false);
        Cerr << ccResult.Response << "\n";

        {
            auto msg = consumer->GetNextMessage();
            msg.Wait();
            Cerr << "consumer assign  response: " << msg.GetValue().Response << "\n";
            UNIT_ASSERT(msg.GetValue().Type == EMT_ASSIGNED);
            msg.GetValue().StartRead.SetValue(TAssignInfo());
        }

        TVector<NThreading::TFuture<Ydb::PersQueue::TProducerCommitResponse>> writeResults;
        TVector<NThreading::TFuture<Ydb::PersQueue::TConsumerMessage>> readResults;

        for (ui32 readBatches = 0; readBatches < 10; ++readBatches) {
            auto msg = consumer->GetNextMessage();
            while (!msg.HasValue()) {
                producer->Write(TString(std::string(10000, 'A')));
                Sleep(TDuration::MilliSeconds(10));
            }
            const auto& response = msg.GetValue().Response;
            Cerr << "next read response: " << response << "\n";

            for (auto& data : response.data_batch().partition_data()) {
                for (auto& batch : data.batches()) {
                    UNIT_ASSERT(batch.message_data_size() > 0);
                }
            }
        }
    }

    Y_UNIT_TEST(TestDeletionOfTopic) {
        if (NSan::ASanIsOn()) {
            return;
        }

        NPersQueue::TTestServer server;
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1);
        server.WaitInit(SHORT_TOPIC_NAME);
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY });

        server.AnnoyingClient->DeleteTopic2(DEFAULT_TOPIC_NAME, NPersQueue::NErrorCode::OK, false);
        auto pqLib = TPQLib::WithCerrLogger();

        ui32 maxCount = 1;
        bool unpack = false;
        auto [consumer, ccResult] = CreateConsumer(pqLib, server.GrpcPort, "user", {SHORT_TOPIC_NAME, {}}, maxCount, unpack);
        Cerr << "Consumer create response: " << ccResult.Response << "\n";

        auto isDead = consumer->IsDead();
        isDead.Wait();
        Cerr << "Is dead future value: " << isDead.GetValue() << "\n";
        UNIT_ASSERT_EQUAL(ccResult.Response.Getissues(0).issue_code(), Ydb::PersQueue::ErrorCode::UNKNOWN_TOPIC);
    }
*/
    Y_UNIT_TEST(Codecs_InitWriteSession_DefaultTopicSupportedCodecsInInitResponse) {
        APITestSetup setup{TEST_CASE_NAME};
        grpc::ClientContext context;
        auto session = setup.GetPersQueueService()->StreamingWrite(&context);


        auto serverMessage = setup.InitSession(session);


        auto defaultSupportedCodecs = TVector<Ydb::PersQueue::V1::Codec>{ Ydb::PersQueue::V1::CODEC_RAW, Ydb::PersQueue::V1::CODEC_GZIP, Ydb::PersQueue::V1::CODEC_LZOP };
        auto topicSupportedCodecs = serverMessage.init_response().supported_codecs();
        UNIT_ASSERT_VALUES_EQUAL_C(defaultSupportedCodecs.size(), topicSupportedCodecs.size(), serverMessage.init_response());
        UNIT_ASSERT_C(Equal(defaultSupportedCodecs.begin(), defaultSupportedCodecs.end(), topicSupportedCodecs.begin()), serverMessage.init_response());
    }

    Y_UNIT_TEST(Codecs_WriteMessageWithDefaultCodecs_MessagesAreAcknowledged) {
        APITestSetup setup{TEST_CASE_NAME};
        auto log = setup.GetLog();
        StreamingWriteClientMessage clientMessage;
        auto* writeRequest = clientMessage.mutable_write_request();
        auto sequenceNumber = 1;
        auto messageCount = 0;
        const auto message = NUnitTest::RandomString(250 * 1024, std::rand());
        auto compress = [](TString data, i32 codecID) {
            Y_UNUSED(codecID);
            return TString(data);
        };
        TVector<char> defaultCodecs{0, 1, 2};
        for (const auto& codecID : defaultCodecs) {
            auto compressedMessage = compress(message, codecID);

            writeRequest->add_sequence_numbers(sequenceNumber++);
            writeRequest->add_message_sizes(message.size());
            writeRequest->add_created_at_ms(TInstant::Now().MilliSeconds());
            writeRequest->add_sent_at_ms(TInstant::Now().MilliSeconds());
            writeRequest->add_blocks_offsets(0);
            writeRequest->add_blocks_part_numbers(0);
            writeRequest->add_blocks_message_counts(1);
            writeRequest->add_blocks_uncompressed_sizes(message.size());
            writeRequest->add_blocks_headers(TString(1, codecID));
            writeRequest->add_blocks_data(compressedMessage);
            ++messageCount;
        }
        auto session = setup.InitWriteSession();


        AssertSuccessfullStreamingOperation(session.first->Write(clientMessage), session.first, &clientMessage);


        StreamingWriteServerMessage serverMessage;
        log << TLOG_INFO << "Wait for write acknowledgement";
        AssertSuccessfullStreamingOperation(session.first->Read(&serverMessage), session.first);
        UNIT_ASSERT_C(serverMessage.server_message_case() == StreamingWriteServerMessage::kBatchWriteResponse, serverMessage);
        UNIT_ASSERT_VALUES_EQUAL_C(defaultCodecs.size(), serverMessage.batch_write_response().offsets_size(), serverMessage);
    }

    Y_UNIT_TEST(Codecs_WriteMessageWithNonDefaultCodecThatHasToBeConfiguredAdditionally_SessionClosedWithBadRequestError) {
        APITestSetup setup{TEST_CASE_NAME};
        auto log = setup.GetLog();
        StreamingWriteClientMessage clientMessage;
        auto* writeRequest = clientMessage.mutable_write_request();
        const auto message = NUnitTest::RandomString(250 * 1024, std::rand());
        const auto codecID = 3;
        writeRequest->add_sequence_numbers(1);
        writeRequest->add_message_sizes(message.size());
        writeRequest->add_created_at_ms(TInstant::Now().MilliSeconds());
        writeRequest->add_sent_at_ms(TInstant::Now().MilliSeconds());
        writeRequest->add_blocks_offsets(0);
        writeRequest->add_blocks_part_numbers(0);
        writeRequest->add_blocks_message_counts(1);
        writeRequest->add_blocks_uncompressed_sizes(message.size());
        writeRequest->add_blocks_headers(TString(1, codecID));
        writeRequest->add_blocks_data(message);
        auto session = setup.InitWriteSession();


        AssertSuccessfullStreamingOperation(session.first->Write(clientMessage), session.first, &clientMessage);

        log << TLOG_INFO << "Wait for session to die";
        AssertStreamingSessionDead(session.first, Ydb::StatusIds::BAD_REQUEST, Ydb::PersQueue::ErrorCode::BAD_REQUEST);
    }

    StreamingWriteClientMessage::InitRequest GenerateSessionSetupWithPreferredCluster(const TString preferredCluster) {
        StreamingWriteClientMessage::InitRequest sessionSetup;
        sessionSetup.set_preferred_cluster(preferredCluster);
        sessionSetup.set_message_group_id("test-message-group-id-" + preferredCluster);
        return sessionSetup;
    };

    Y_UNIT_TEST(PreferredCluster_TwoEnabledClustersAndWriteSessionsWithDifferentPreferredCluster_SessionWithMismatchedClusterDiesAndOthersAlive) {
        APITestSetup setup{TEST_CASE_NAME};
        auto log = setup.GetLog();

        setup.GetPQConfig().SetClustersUpdateTimeoutSec(0);
        setup.GetPQConfig().SetRemoteClusterEnabledDelaySec(0);
        setup.GetPQConfig().SetCloseClientSessionWithEnabledRemotePreferredClusterDelaySec(0);
        auto sessionWithNoPreferredCluster = setup.InitWriteSession(GenerateSessionSetupWithPreferredCluster(TString()));
        auto sessionWithLocalPreffedCluster = setup.InitWriteSession(GenerateSessionSetupWithPreferredCluster(setup.GetLocalCluster()));
        auto sessionWithRemotePrefferedCluster = setup.InitWriteSession(GenerateSessionSetupWithPreferredCluster(setup.GetRemoteCluster()));
        grpc::ClientContext context;
        auto sessionWithNoInitialization = setup.GetPersQueueService()->StreamingWrite(&context);

        log << TLOG_INFO << "Wait for session with remote preferred cluster to die";
        AssertStreamingSessionDead(sessionWithRemotePrefferedCluster.first, Ydb::StatusIds::ABORTED, Ydb::PersQueue::ErrorCode::PREFERRED_CLUSTER_MISMATCHED);
        AssertStreamingSessionAlive(sessionWithNoPreferredCluster.first);
        AssertStreamingSessionAlive(sessionWithLocalPreffedCluster.first);

        setup.InitSession(sessionWithNoInitialization);
        AssertStreamingSessionAlive(sessionWithNoInitialization);
    }

    Y_UNIT_TEST(PreferredCluster_DisabledRemoteClusterAndWriteSessionsWithDifferentPreferredClusterAndLaterRemoteClusterEnabled_SessionWithMismatchedClusterDiesAfterPreferredClusterEnabledAndOtherSessionsAlive) {
        APITestSetup setup{TEST_CASE_NAME};
        auto log = setup.GetLog();
        log << TLOG_INFO << "Disable remote cluster " << setup.GetRemoteCluster().Quote();
        setup.GetFlatMsgBusPQClient().UpdateDC(setup.GetRemoteCluster(), false, false);
        setup.GetPQConfig().SetClustersUpdateTimeoutSec(0);
        setup.GetPQConfig().SetRemoteClusterEnabledDelaySec(0);
        setup.GetPQConfig().SetCloseClientSessionWithEnabledRemotePreferredClusterDelaySec(0);
        auto sessionWithNoPreferredCluster = setup.InitWriteSession(GenerateSessionSetupWithPreferredCluster(TString()));
        auto sessionWithLocalPreffedCluster = setup.InitWriteSession(GenerateSessionSetupWithPreferredCluster(setup.GetLocalCluster()));
        auto sessionWithRemotePrefferedCluster = setup.InitWriteSession(GenerateSessionSetupWithPreferredCluster(setup.GetRemoteCluster()));
        AssertStreamingSessionAlive(sessionWithNoPreferredCluster.first);
        AssertStreamingSessionAlive(sessionWithLocalPreffedCluster.first);
        AssertStreamingSessionAlive(sessionWithRemotePrefferedCluster.first);

        log << TLOG_INFO << "Enable remote cluster " << setup.GetRemoteCluster().Quote();
        setup.GetFlatMsgBusPQClient().UpdateDC(setup.GetRemoteCluster(), false, true);

        log << TLOG_INFO << "Wait for session with remote preferred cluster to die";
        AssertStreamingSessionDead(sessionWithRemotePrefferedCluster.first, Ydb::StatusIds::ABORTED, Ydb::PersQueue::ErrorCode::PREFERRED_CLUSTER_MISMATCHED);
        AssertStreamingSessionAlive(sessionWithNoPreferredCluster.first);
        AssertStreamingSessionAlive(sessionWithLocalPreffedCluster.first);
    }

    Y_UNIT_TEST(PreferredCluster_EnabledRemotePreferredClusterAndCloseClientSessionWithEnabledRemotePreferredClusterDelaySec_SessionDiesOnlyAfterDelay) {
        APITestSetup setup{TEST_CASE_NAME};
        auto log = setup.GetLog();
        setup.GetPQConfig().SetClustersUpdateTimeoutSec(0);
        setup.GetPQConfig().SetRemoteClusterEnabledDelaySec(0);
        setup.GetPQConfig().SetCloseClientSessionWithEnabledRemotePreferredClusterDelaySec(0);

        const auto edgeActorID = setup.GetServer().GetRuntime()->AllocateEdgeActor();

        setup.GetServer().GetRuntime()->Send(new IEventHandle(NPQ::NClusterTracker::MakeClusterTrackerID(), edgeActorID, new NPQ::NClusterTracker::TEvClusterTracker::TEvSubscribe));
        log << TLOG_INFO << "Wait for cluster tracker event";
        auto clustersUpdate = setup.GetServer().GetRuntime()->GrabEdgeEvent<NPQ::NClusterTracker::TEvClusterTracker::TEvClustersUpdate>();


        auto session = setup.InitWriteSession(GenerateSessionSetupWithPreferredCluster(setup.GetRemoteCluster()));

        AssertStreamingSessionAlive(session.first);

        AssertStreamingSessionDead(session.first, Ydb::StatusIds::ABORTED, Ydb::PersQueue::ErrorCode::PREFERRED_CLUSTER_MISMATCHED);
    }

    Y_UNIT_TEST(PreferredCluster_NonExistentPreferredCluster_SessionDiesOnlyAfterDelay) {
        APITestSetup setup{TEST_CASE_NAME};
        auto log = setup.GetLog();
        setup.GetPQConfig().SetClustersUpdateTimeoutSec(0);
        setup.GetPQConfig().SetRemoteClusterEnabledDelaySec(0);
        setup.GetPQConfig().SetCloseClientSessionWithEnabledRemotePreferredClusterDelaySec(2);

        TInstant now(TInstant::Now());
        auto session = setup.InitWriteSession(GenerateSessionSetupWithPreferredCluster("non-existent-cluster"));
        AssertStreamingSessionAlive(session.first);

        AssertStreamingSessionDead(session.first, Ydb::StatusIds::ABORTED, Ydb::PersQueue::ErrorCode::PREFERRED_CLUSTER_MISMATCHED);
        UNIT_ASSERT(TInstant::Now() - now > TDuration::MilliSeconds(1999));
    }

    Y_UNIT_TEST(PreferredCluster_EnabledRemotePreferredClusterAndRemoteClusterEnabledDelaySec_SessionDiesOnlyAfterDelay) {
        APITestSetup setup{TEST_CASE_NAME};
        auto log = setup.GetLog();
        setup.GetPQConfig().SetClustersUpdateTimeoutSec(0);
        setup.GetPQConfig().SetCloseClientSessionWithEnabledRemotePreferredClusterDelaySec(2);
        const auto edgeActorID = setup.GetServer().GetRuntime()->AllocateEdgeActor();

        setup.GetPQConfig().SetRemoteClusterEnabledDelaySec(0);

        TInstant now(TInstant::Now());
        auto session = setup.InitWriteSession(GenerateSessionSetupWithPreferredCluster(setup.GetRemoteCluster()));

        setup.GetServer().GetRuntime()->Send(new IEventHandle(NPQ::NClusterTracker::MakeClusterTrackerID(), edgeActorID, new NPQ::NClusterTracker::TEvClusterTracker::TEvSubscribe));
        log << TLOG_INFO << "Wait for cluster tracker event";
        auto clustersUpdate = setup.GetServer().GetRuntime()->GrabEdgeEvent<NPQ::NClusterTracker::TEvClusterTracker::TEvClustersUpdate>();
        AssertStreamingSessionAlive(session.first);

        AssertStreamingSessionDead(session.first, Ydb::StatusIds::ABORTED, Ydb::PersQueue::ErrorCode::PREFERRED_CLUSTER_MISMATCHED);

        UNIT_ASSERT(TInstant::Now() - now > TDuration::MilliSeconds(1999));
    }

    Y_UNIT_TEST(PreferredCluster_RemotePreferredClusterEnabledWhileSessionInitializing_SessionDiesOnlyAfterInitializationAndDelay) {
        APITestSetup setup{TEST_CASE_NAME};
        auto log = setup.GetLog();
        setup.GetPQConfig().SetClustersUpdateTimeoutSec(0);
        setup.GetPQConfig().SetRemoteClusterEnabledDelaySec(0);
        setup.GetPQConfig().SetCloseClientSessionWithEnabledRemotePreferredClusterDelaySec(2);
        const auto edgeActorID = setup.GetServer().GetRuntime()->AllocateEdgeActor();
        setup.GetFlatMsgBusPQClient().UpdateDC(setup.GetRemoteCluster(), false, false);

        grpc::ClientContext context;
        auto session = setup.GetPersQueueService()->StreamingWrite(&context);

        setup.GetFlatMsgBusPQClient().UpdateDC(setup.GetRemoteCluster(), false, true);

        setup.GetServer().GetRuntime()->Send(new IEventHandle(NPQ::NClusterTracker::MakeClusterTrackerID(), edgeActorID, new NPQ::NClusterTracker::TEvClusterTracker::TEvSubscribe));
        log << TLOG_INFO << "Wait for cluster tracker event";
        auto clustersUpdate = setup.GetServer().GetRuntime()->GrabEdgeEvent<NPQ::NClusterTracker::TEvClusterTracker::TEvClustersUpdate>();
        TInstant now(TInstant::Now());
        setup.InitSession(session, GenerateSessionSetupWithPreferredCluster(setup.GetRemoteCluster()));

        AssertStreamingSessionAlive(session);

        log << TLOG_INFO << "Set small delay and wait for initialized session with remote preferred cluster to die";
        AssertStreamingSessionDead(session, Ydb::StatusIds::ABORTED, Ydb::PersQueue::ErrorCode::PREFERRED_CLUSTER_MISMATCHED);

        UNIT_ASSERT(TInstant::Now() - now > TDuration::MilliSeconds(1999));

    }

    Y_UNIT_TEST(SchemeOperationsTest) {
        NPersQueue::TTestServer server;
        server.EnableLogs({NKikimrServices::PQ_READ_PROXY, NKikimrServices::BLACKBOX_VALIDATOR });
        TString topic1 = "rt3.dc1--acc--topic1";
        TString topic3 = "rt3.dc1--acc--topic3";
        server.AnnoyingClient->CreateTopic(topic1, 1);
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1);
        server.AnnoyingClient->CreateConsumer("user");

        std::shared_ptr<grpc::Channel> Channel_;
        std::unique_ptr<Ydb::PersQueue::V1::PersQueueService::Stub> StubP_;

        {
            Channel_ = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
            StubP_ = Ydb::PersQueue::V1::PersQueueService::NewStub(Channel_);
        }

        do {
            CreateTopicRequest request;
            CreateTopicResponse response;
            request.set_path(TStringBuilder() << "/Root/PQ/" << topic3);
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            props->set_max_partition_storage_size(1000);
            props->set_max_partition_write_speed(1000);
            props->set_max_partition_write_burst(1000);
            grpc::ClientContext rcontext;
            rcontext.AddMetadata("x-ydb-auth-ticket", "user@" BUILTIN_ACL_DOMAIN);

            auto status = StubP_->CreateTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            if (response.operation().status() == Ydb::StatusIds::UNAVAILABLE) {
                Sleep(TDuration::Seconds(1));
                continue;
            }
            Cerr << response.operation() << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::UNAUTHORIZED);
            break;
        } while (true);

        {
            // local cluster
            CreateTopicRequest request;
            CreateTopicResponse response;
            request.set_path(TStringBuilder() << "/Root/PQ/" << topic3);
            auto props = request.mutable_settings();
            props->set_partitions_count(2);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            props->set_max_partition_storage_size(1000);
            props->set_max_partition_write_speed(1000);
            props->set_max_partition_write_burst(1000);

            grpc::ClientContext rcontext;

            auto status = StubP_->CreateTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
            server.AnnoyingClient->AddTopic(topic3);
        }

        auto alter =[&StubP_](const AlterTopicRequest& request, Ydb::StatusIds::StatusCode statusCode, bool auth)
        {
            AlterTopicResponse response;

            grpc::ClientContext rcontext;
            if (auth)
                rcontext.AddMetadata("x-ydb-auth-ticket", "user@" BUILTIN_ACL_DOMAIN);

            auto status = StubP_->AlterTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            AlterTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), statusCode);
        };

        AlterTopicRequest request;
        request.set_path(TStringBuilder() << "/Root/PQ/" << topic3);
        auto props = request.mutable_settings();
        props->set_partitions_count(1);
        props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
        props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
        props->set_max_partition_storage_size(1000);
        props->set_max_partition_write_speed(1000);
        props->set_max_partition_write_burst(1000);
        alter(request, Ydb::StatusIds::UNAUTHORIZED, true);
        alter(request, Ydb::StatusIds::GENERIC_ERROR, false);
        props->set_partitions_count(3);
        alter(request, Ydb::StatusIds::SUCCESS, false);
        props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(6));
        alter(request, Ydb::StatusIds::BAD_REQUEST, false);
        props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
        auto rr = props->add_read_rules();
        alter(request, Ydb::StatusIds::BAD_REQUEST, false);
        server.AnnoyingClient->AlterTopic();
        props->add_supported_codecs(Ydb::PersQueue::V1::CODEC_RAW);
        props->add_supported_codecs(Ydb::PersQueue::V1::CODEC_ZSTD);

        props->set_max_partition_write_speed(123);
        props->set_max_partition_write_burst(1234);
        props->set_max_partition_storage_size(234);
        props->set_partitions_count(3);

        rr->set_consumer_name("consumer");
        rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));

        rr->add_supported_codecs(Ydb::PersQueue::V1::CODEC_LZOP);
        rr->add_supported_codecs(Ydb::PersQueue::V1::CODEC_GZIP);

        rr->set_important(true);
        rr->set_starting_message_timestamp_ms(111);
        rr->set_version(567);

        (*props->mutable_attributes())["_allow_unauthenticated_read"] = "true";

        (*props->mutable_attributes())["_partitions_per_tablet"] = "5";
        alter(request, Ydb::StatusIds::SUCCESS, false);

        TString topic4 = "rt3.dc1--acc--topic4";
        server.AnnoyingClient->CreateTopic(topic4, 1); //ensure creation
        auto res = server.AnnoyingClient->DescribeTopic({topic3});
        Cerr << res.DebugString();
        TString resultDescribe = R"___(TopicInfo {
  Topic: "rt3.dc1--acc--topic3"
  NumPartitions: 3
  Config {
    PartitionConfig {
      MaxCountInPartition: 2147483647
      MaxSizeInPartition: 234
      LifetimeSeconds: 86400
      ImportantClientId: "consumer"
      SourceIdLifetimeSeconds: 1382400
      WriteSpeedInBytesPerSecond: 123
      BurstSize: 1234
      NumChannels: 10
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      ExplicitChannelProfiles {
        PoolKind: "test"
      }
      SourceIdMaxCounts: 6000000
    }
    Version: 3
    LocalDC: true
    RequireAuthWrite: true
    RequireAuthRead: false
    Producer: "acc"
    Ident: "acc"
    Topic: "topic3"
    DC: "dc1"
    ReadRules: "consumer"
    ReadFromTimestampsMs: 111
    ConsumerFormatVersions: 0
    ConsumerCodecs {
      Ids: 2
      Ids: 1
      Codecs: "lzop"
      Codecs: "gzip"
    }
    ReadRuleServiceTypes: "data-transfer"
    FormatVersion: 0
    Codecs {
      Ids: 0
      Ids: 3
      Codecs: "raw"
      Codecs: "zstd"
    }
    ReadRuleVersions: 567
  }
  ErrorCode: OK
}
)___";
        UNIT_ASSERT_VALUES_EQUAL(res.DebugString(), resultDescribe);

        Cerr << "DESCRIBES:\n";
        {
            DescribeTopicRequest request;
            DescribeTopicResponse response;
            request.set_path(TStringBuilder() << "/Root/PQ/" << topic3);
            grpc::ClientContext rcontext;
            rcontext.AddMetadata("x-ydb-auth-ticket", "user@" BUILTIN_ACL_DOMAIN);

            auto status = StubP_->DescribeTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            DescribeTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SCHEME_ERROR); // muts be Ydb::StatusIds::UNAUTHORIZED);
        }

        {
            DescribeTopicRequest request;
            DescribeTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--topic123");
            grpc::ClientContext rcontext;

            auto status = StubP_->DescribeTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            DescribeTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SCHEME_ERROR);
        }

        {
            DescribeTopicRequest request;
            DescribeTopicResponse response;
            request.set_path(TStringBuilder() << "/Root/PQ/" << topic3);
            grpc::ClientContext rcontext;

            auto status = StubP_->DescribeTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            DescribeTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            props->CopyFrom(res.settings());

            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        }


        alter(request, Ydb::StatusIds::SUCCESS, false);

        {
            DescribeTopicRequest request;
            DescribeTopicResponse response;
            request.set_path(TStringBuilder() << "/Root/PQ/" << topic3);
            grpc::ClientContext rcontext;

            auto status = StubP_->DescribeTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            DescribeTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
            UNIT_ASSERT_VALUES_EQUAL(props->DebugString(), res.settings().DebugString());
        }

        {
            DropTopicRequest request;
            DropTopicResponse response;
            request.set_path(TStringBuilder() << "/Root/PQ/" << topic3);
            grpc::ClientContext rcontext;
            auto status = StubP_->DropTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            DropTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
            server.AnnoyingClient->RemoveTopic(topic3);
        }


        {
            DropTopicRequest request;
            DropTopicResponse response;
            request.set_path(TStringBuilder() << "/Root/PQ/" << topic3);

            grpc::ClientContext rcontext;
            auto status = StubP_->DropTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            DropTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SCHEME_ERROR);
        }

        server.AnnoyingClient->CreateTopic("rt3.dc1--acc--topic5", 1); //ensure creation
        server.AnnoyingClient->DescribeTopic({topic3}, true);


        {
            NYdb::TDriverConfig driverCfg;
            driverCfg.SetEndpoint(TStringBuilder() << "localhost:" << server.GrpcPort);
            std::shared_ptr<NYdb::TDriver> ydbDriver(new NYdb::TDriver(driverCfg));
            auto pqClient = NYdb::NPersQueue::TPersQueueClient(*ydbDriver);

            auto res = pqClient.CreateTopic("/Root/PQ/rt3.dc1--acc2--topic2");
            res.Wait();
            Cerr << res.GetValue().IsSuccess() << " " << res.GetValue().GetIssues().ToString() << "\n";
        }

    }


    Y_UNIT_TEST(SchemeOperationsCheckPropValues) {
        NPersQueue::TTestServer server;
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY, NKikimrServices::BLACKBOX_VALIDATOR });

        server.AnnoyingClient->CreateTopic("rt3.dc1--acc--topic1", 1);
        server.AnnoyingClient->CreateTopic(DEFAULT_TOPIC_NAME, 1);
        server.AnnoyingClient->CreateConsumer("user");

        std::shared_ptr<grpc::Channel> Channel_;
        std::unique_ptr<Ydb::PersQueue::V1::PersQueueService::Stub> StubP_;

        {
            Channel_ = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
            StubP_ = Ydb::PersQueue::V1::PersQueueService::NewStub(Channel_);
        }

        {
            // zero value is forbidden for: partitions_count
            CreateTopicRequest request;
            CreateTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--topic1");
            auto props = request.mutable_settings();
            props->set_partitions_count(0);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());

            grpc::ClientContext rcontext;
            auto status = StubP_->CreateTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
        }
        {
            // zero value is forbidden for: retention_period_ms
            CreateTopicRequest request;
            CreateTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--topic1");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(0);

            grpc::ClientContext rcontext;
            auto status = StubP_->CreateTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
        }
        {
            // zero value is allowed for: partition_storage_size, max_partition_write_speed, max_partition_write_burst
            CreateTopicRequest request;
            CreateTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--topic1");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            props->set_max_partition_storage_size(0);
            props->set_max_partition_write_speed(0);
            props->set_max_partition_write_burst(0);

            grpc::ClientContext rcontext;
            auto status = StubP_->CreateTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        }
    }

    Y_UNIT_TEST(ReadRuleServiceType) {
        TServerSettings settings = PQSettings(0);
        {
            settings.PQConfig.AddClientServiceType()->SetName("MyGreatType");
            settings.PQConfig.AddClientServiceType()->SetName("AnotherType");
            settings.PQConfig.AddClientServiceType()->SetName("SecondType");
        }
        NPersQueue::TTestServer server(settings);
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY, NKikimrServices::BLACKBOX_VALIDATOR });

        std::unique_ptr<Ydb::PersQueue::V1::PersQueueService::Stub> pqStub;

        {
            std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
            pqStub = Ydb::PersQueue::V1::PersQueueService::NewStub(channel);
        }
        auto checkDescribe = [&](const TVector<std::pair<TString, TString>>& readRules) {
            DescribeTopicRequest request;
            DescribeTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            grpc::ClientContext rcontext;

            auto status = pqStub->DescribeTopic(&rcontext, request, &response);
            UNIT_ASSERT(status.ok());
            DescribeTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

            UNIT_ASSERT_VALUES_EQUAL(res.settings().read_rules().size(), readRules.size());
            for (ui64 i = 0; i < readRules.size(); ++i) {
                const auto& rr = res.settings().read_rules(i);
                UNIT_ASSERT_EQUAL(rr.consumer_name(), readRules[i].first);
                UNIT_ASSERT_EQUAL(rr.service_type(), readRules[i].second);
            }
        };
        {
            CreateTopicRequest request;
            CreateTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer1");
            }
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer2");
                rr->set_service_type("MyGreatType");
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->CreateTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        }
        checkDescribe({
            {"acc/consumer1", "data-transfer"},
            {"acc/consumer2", "MyGreatType"}
        });
        {
            AlterTopicRequest request;
            AlterTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer1");
            }
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer2");
                rr->set_service_type("AnotherType");
            }
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer3");
                rr->set_service_type("SecondType");
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->AlterTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        }
        checkDescribe({
            {"acc/consumer1", "data-transfer"},
            {"acc/consumer2", "AnotherType"},
            {"acc/consumer3", "SecondType"}
        });

        {
            AlterTopicRequest request;
            AlterTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer1");
                rr->set_service_type("BadServiceType");
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->AlterTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
        }
        checkDescribe({
            {"acc/consumer1", "data-transfer"},
            {"acc/consumer2", "AnotherType"},
            {"acc/consumer3", "SecondType"}
        });
    }


    Y_UNIT_TEST(ReadRuleServiceTypeLimit) {
        TServerSettings settings = PQSettings(0);
        {
            auto type = settings.PQConfig.AddClientServiceType();
            type->SetName("MyGreatType");
            type->SetMaxReadRulesCountPerTopic(3);
        }
        NPersQueue::TTestServer server(settings);
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY, NKikimrServices::BLACKBOX_VALIDATOR });

        std::unique_ptr<Ydb::PersQueue::V1::PersQueueService::Stub> pqStub;

        {
            std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
            pqStub = Ydb::PersQueue::V1::PersQueueService::NewStub(channel);
        }
        {
            CreateTopicRequest request;
            CreateTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());

            grpc::ClientContext rcontext;
            auto status = pqStub->CreateTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        }
        auto checkDescribe = [&](const TVector<std::pair<TString, TString>>& readRules) {
            DescribeTopicRequest request;
            DescribeTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            grpc::ClientContext rcontext;

            auto status = pqStub->DescribeTopic(&rcontext, request, &response);
            UNIT_ASSERT(status.ok());
            DescribeTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

            UNIT_ASSERT_VALUES_EQUAL(res.settings().read_rules().size(), readRules.size());
            for (ui64 i = 0; i < readRules.size(); ++i) {
                const auto& rr = res.settings().read_rules(i);
                UNIT_ASSERT_EQUAL(rr.consumer_name(), readRules[i].first);
                UNIT_ASSERT_EQUAL(rr.service_type(), readRules[i].second);
            }
        };

        TVector<std::pair<TString, TString>> readRules;
        for (ui32 i = 0; i < 4; ++i) {
            AddReadRuleRequest request;
            AddReadRuleResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto rr = request.mutable_read_rule();
            rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
            rr->set_consumer_name(TStringBuilder() << "acc/new_user" << i);
            rr->set_service_type("MyGreatType");
            readRules.push_back({TStringBuilder() << "acc/new_user" << i, "MyGreatType"});

            grpc::ClientContext rcontext;
            auto status = pqStub->AddReadRule(&rcontext, request, &response);
            Cerr << response << "\n";
            if (i < 3) {
                UNIT_ASSERT(status.ok());
                checkDescribe(readRules);
            }
            if (i == 3) {
                UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
            }
        }
        {
            AddReadRuleRequest request;
            AddReadRuleResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto rr = request.mutable_read_rule();
            rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
            rr->set_consumer_name(TStringBuilder() << "acc/new_user0");
            rr->set_service_type("MyGreatType");

            grpc::ClientContext rcontext;
            auto status = pqStub->AddReadRule(&rcontext, request, &response);
            Cerr << response << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::ALREADY_EXISTS);
        }
        {
            AlterTopicRequest request;
            AlterTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            for(ui32 i = 0; i < 4; ++i) {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name(TStringBuilder() << "acc/new_user" << i);
                rr->set_service_type("MyGreatType");
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->AlterTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
        }
    }


    Y_UNIT_TEST(ReadRuleDisallowDefaultServiceType) {
        TServerSettings settings = PQSettings(0);
        {
            settings.PQConfig.AddClientServiceType()->SetName("MyGreatType");
            settings.PQConfig.AddClientServiceType()->SetName("AnotherType");
            settings.PQConfig.AddClientServiceType()->SetName("SecondType");
            settings.PQConfig.SetDisallowDefaultClientServiceType(true);
        }
        NPersQueue::TTestServer server(settings);
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY, NKikimrServices::BLACKBOX_VALIDATOR });

        std::unique_ptr<Ydb::PersQueue::V1::PersQueueService::Stub> pqStub;

        {
            std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
            pqStub = Ydb::PersQueue::V1::PersQueueService::NewStub(channel);
        }
        auto checkDescribe = [&](const TVector<std::pair<TString, TString>>& readRules) {
            DescribeTopicRequest request;
            DescribeTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            grpc::ClientContext rcontext;

            auto status = pqStub->DescribeTopic(&rcontext, request, &response);
            UNIT_ASSERT(status.ok());
            DescribeTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

            UNIT_ASSERT_VALUES_EQUAL(res.settings().read_rules().size(), readRules.size());
            for (ui64 i = 0; i < readRules.size(); ++i) {
                const auto& rr = res.settings().read_rules(i);
                UNIT_ASSERT_EQUAL(rr.consumer_name(), readRules[i].first);
                UNIT_ASSERT_EQUAL(rr.service_type(), readRules[i].second);
            }
        };
        {
            CreateTopicRequest request;
            CreateTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer1");
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->CreateTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
        }
        {
            CreateTopicRequest request;
            CreateTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer1");
                rr->set_service_type("MyGreatType");
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->CreateTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        }
        checkDescribe({{"acc/consumer1", "MyGreatType"}});
        {
            AlterTopicRequest request;
            AlterTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer1");
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->AlterTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
        }
        checkDescribe({{"acc/consumer1", "MyGreatType"}});

        {
            AlterTopicRequest request;
            AlterTopicResponse response;
            request.set_path("/Root/PQ/rt3.dc1--acc--some-topic");
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer1");
                rr->set_service_type("AnotherType");
            }
            {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name("acc/consumer2");
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->AlterTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
        }
        checkDescribe({{"acc/consumer1", "MyGreatType"}});
    }

    Y_UNIT_TEST(ReadRuleServiceTypeMigration) {
        TServerSettings settings = PQSettings(0);
        {
            settings.PQConfig.MutableDefaultClientServiceType()->SetName("default_type");
            settings.PQConfig.AddClientServiceType()->SetName("MyGreatType");
            settings.PQConfig.AddClientServiceType()->SetName("AnotherType");
            settings.PQConfig.AddClientServiceType()->SetName("SecondType");
        }
        NPersQueue::TTestServer server(settings);

        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY, NKikimrServices::BLACKBOX_VALIDATOR });

        const ui32 topicsCount = 4;
        for (ui32 i = 1; i <= topicsCount; ++i) {
            TRequestCreatePQ createTopicRequest(TStringBuilder() << "rt3.dc1--topic_" << i, 1);
            createTopicRequest.ReadRules.push_back("acc@user1");
            createTopicRequest.ReadRules.push_back("acc@user2");
            createTopicRequest.ReadRules.push_back("acc@user3");
            server.AnnoyingClient->CreateTopic(createTopicRequest);
        }

        std::unique_ptr<Ydb::PersQueue::V1::PersQueueService::Stub> pqStub;
        {
            std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
            pqStub = Ydb::PersQueue::V1::PersQueueService::NewStub(channel);
        }
        auto doAlter = [&](const TString& topic, const TVector<std::pair<TString, TString>>& readRules) {
            AlterTopicRequest request;
            AlterTopicResponse response;
            request.set_path(topic);
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            for (auto rrInfo : readRules) {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name(rrInfo.first);
                rr->set_service_type(rrInfo.second);
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->AlterTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        };


        auto checkDescribe = [&](const TString& topic, const TVector<std::pair<TString, TString>>& readRules) {
            DescribeTopicRequest request;
            DescribeTopicResponse response;
            request.set_path(topic);
            grpc::ClientContext rcontext;

            auto status = pqStub->DescribeTopic(&rcontext, request, &response);
            UNIT_ASSERT(status.ok());
            DescribeTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

            UNIT_ASSERT_VALUES_EQUAL(res.settings().read_rules().size(), readRules.size());
            for (ui64 i = 0; i < readRules.size(); ++i) {
                const auto& rr = res.settings().read_rules(i);
                UNIT_ASSERT_EQUAL(rr.consumer_name(), readRules[i].first);
                UNIT_ASSERT_EQUAL(rr.service_type(), readRules[i].second);
            }
        };
        checkDescribe(
            "/Root/PQ/rt3.dc1--topic_1",
            {
                {"acc/user1", "default_type"},
                {"acc/user2", "default_type"},
                {"acc/user3", "default_type"}
            }
        );
        {
            doAlter(
                "/Root/PQ/rt3.dc1--topic_2",
                {
                    {"acc/user1", ""},
                    {"acc/new_user", "MyGreatType"},
                    {"acc/user2", "default_type"},
                    {"acc/user3", "default_type"},
                    {"acc/user4", "AnotherType"}
                }
            );
            checkDescribe(
                "/Root/PQ/rt3.dc1--topic_2",
                {
                    {"acc/user1", "default_type"},
                    {"acc/new_user", "MyGreatType"},
                    {"acc/user2", "default_type"},
                    {"acc/user3", "default_type"},
                    {"acc/user4", "AnotherType"}
                }
            );
        }
        {
            AddReadRuleRequest request;
            AddReadRuleResponse response;
            request.set_path("/Root/PQ/rt3.dc1--topic_3");
            auto rr = request.mutable_read_rule();
            rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
            rr->set_consumer_name("acc/new_user");
            rr->set_service_type("MyGreatType");

            grpc::ClientContext rcontext;
            auto status = pqStub->AddReadRule(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

            checkDescribe(
                "/Root/PQ/rt3.dc1--topic_3",
                {
                    {"acc/user1", "default_type"},
                    {"acc/user2", "default_type"},
                    {"acc/user3", "default_type"},
                    {"acc/new_user", "MyGreatType"}
                }
            );
        }

        {
            checkDescribe(
                "/Root/PQ/rt3.dc1--topic_4",
                {
                    {"acc/user1", "default_type"},
                    {"acc/user2", "default_type"},
                    {"acc/user3", "default_type"}
                }
            );

            RemoveReadRuleRequest request;
            RemoveReadRuleResponse response;
            request.set_path("/Root/PQ/rt3.dc1--topic_4");
            request.set_consumer_name("acc@user2");

            grpc::ClientContext rcontext;
            auto status = pqStub->RemoveReadRule(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

            checkDescribe(
                "/Root/PQ/rt3.dc1--topic_4",
                {
                    {"acc/user1", "default_type"},
                    {"acc/user3", "default_type"}
                }
            );
        }
    }

    Y_UNIT_TEST(ReadRuleServiceTypeMigrationWithDisallowDefault) {
        TServerSettings settings = PQSettings(0);
        {
            settings.PQConfig.MutableDefaultClientServiceType()->SetName("default_type");
            settings.PQConfig.AddClientServiceType()->SetName("MyGreatType");
            settings.PQConfig.AddClientServiceType()->SetName("AnotherType");
            settings.PQConfig.AddClientServiceType()->SetName("SecondType");
            settings.PQConfig.SetDisallowDefaultClientServiceType(true);
        }
        NPersQueue::TTestServer server(settings);

        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY, NKikimrServices::BLACKBOX_VALIDATOR });

        const ui32 topicsCount = 4;
        for (ui32 i = 1; i <= topicsCount; ++i) {
            TRequestCreatePQ createTopicRequest(TStringBuilder() << "rt3.dc1--topic_" << i, 1);
            createTopicRequest.ReadRules.push_back("acc@user1");
            createTopicRequest.ReadRules.push_back("acc@user2");
            createTopicRequest.ReadRules.push_back("acc@user3");
            server.AnnoyingClient->CreateTopic(createTopicRequest);
        }

        std::unique_ptr<Ydb::PersQueue::V1::PersQueueService::Stub> pqStub;
        {
            std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
            pqStub = Ydb::PersQueue::V1::PersQueueService::NewStub(channel);
        }

        auto doAlter = [&](
            const TString& topic,
            const TVector<std::pair<TString, TString>>& readRules,
            Ydb::StatusIds::StatusCode statusCode = Ydb::StatusIds::SUCCESS
        ) {
            AlterTopicRequest request;
            AlterTopicResponse response;
            request.set_path(topic);
            auto props = request.mutable_settings();
            props->set_partitions_count(1);
            props->set_supported_format(Ydb::PersQueue::V1::TopicSettings::FORMAT_BASE);
            props->set_retention_period_ms(TDuration::Days(1).MilliSeconds());
            for (auto rrInfo : readRules) {
                auto rr = props->add_read_rules();
                rr->set_supported_format(Ydb::PersQueue::V1::TopicSettings::Format(1));
                rr->set_consumer_name(rrInfo.first);
                rr->set_service_type(rrInfo.second);
            }

            grpc::ClientContext rcontext;
            auto status = pqStub->AlterTopic(&rcontext, request, &response);

            UNIT_ASSERT(status.ok());
            CreateTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), statusCode);
        };

        auto checkDescribe = [&](
            const TString& topic,
            const TVector<std::pair<TString, TString>>& readRules,
            Ydb::StatusIds::StatusCode statusCode = Ydb::StatusIds::SUCCESS
        ) {
            DescribeTopicRequest request;
            DescribeTopicResponse response;
            request.set_path(topic);
            grpc::ClientContext rcontext;

            auto status = pqStub->DescribeTopic(&rcontext, request, &response);
            UNIT_ASSERT(status.ok());
            DescribeTopicResult res;
            response.operation().result().UnpackTo(&res);
            Cerr << response << "\n" << res << "\n";
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), statusCode);
            if (statusCode == Ydb::StatusIds::SUCCESS) {
                UNIT_ASSERT_VALUES_EQUAL(res.settings().read_rules().size(), readRules.size());
                for (ui64 i = 0; i < readRules.size(); ++i) {
                    const auto& rr = res.settings().read_rules(i);
                    UNIT_ASSERT_EQUAL(rr.consumer_name(), readRules[i].first);
                    UNIT_ASSERT_EQUAL(rr.service_type(), readRules[i].second);
                }
            }
        };
        checkDescribe(
            "/Root/PQ/rt3.dc1--topic_1",
            {},
            Ydb::StatusIds::INTERNAL_ERROR
        );
        {
            doAlter(
                "/Root/PQ/rt3.dc1--topic_2",
                {
                    {"acc/new_user", "MyGreatType"},
                    {"acc/user2", "SecondType"},
                    {"acc/user3", "AnotherType"},
                    {"acc/user4", "AnotherType"}
                }
            );
            checkDescribe(
                "/Root/PQ/rt3.dc1--topic_2",
                {
                    {"acc/new_user", "MyGreatType"},
                    {"acc/user2", "SecondType"},
                    {"acc/user3", "AnotherType"},
                    {"acc/user4", "AnotherType"}
                }
            );
        }
    }

    Y_UNIT_TEST(TClusterTrackerTest) {
        APITestSetup setup{TEST_CASE_NAME};
        setup.GetPQConfig().SetClustersUpdateTimeoutSec(0);
        const auto edgeActorID = setup.GetServer().GetRuntime()->AllocateEdgeActor();
        THashMap<TString, TPQTestClusterInfo> clusters = DEFAULT_CLUSTERS_LIST;

        auto compareInfo = [](const TString& name, const TPQTestClusterInfo& info, const NPQ::NClusterTracker::TClustersList::TCluster& trackerInfo) {
            UNIT_ASSERT_EQUAL(name, trackerInfo.Name);
            UNIT_ASSERT_EQUAL(name, trackerInfo.Datacenter);
            UNIT_ASSERT_EQUAL(info.Balancer, trackerInfo.Balancer);
            UNIT_ASSERT_EQUAL(info.Enabled, trackerInfo.IsEnabled);
            UNIT_ASSERT_EQUAL(info.Weight, trackerInfo.Weight);
        };

        auto getClustersFromTracker = [&]() {
            setup.GetServer().GetRuntime()->Send(new IEventHandle(
                NPQ::NClusterTracker::MakeClusterTrackerID(),
                edgeActorID,
                new NPQ::NClusterTracker::TEvClusterTracker::TEvSubscribe
            ));
            return setup.GetServer().GetRuntime()->GrabEdgeEvent<NPQ::NClusterTracker::TEvClusterTracker::TEvClustersUpdate>();
        };


        {
            auto trackerResponce = getClustersFromTracker();
            for (auto& clusterInfo : trackerResponce->ClustersList->Clusters) {
                auto it = clusters.find(clusterInfo.Name);
                UNIT_ASSERT(it != clusters.end());
                compareInfo(it->first, it->second, clusterInfo);
            }
        }

        UNIT_ASSERT_EQUAL(clusters.count("dc1"), 1);
        UNIT_ASSERT_EQUAL(clusters.count("dc2"), 1);
        clusters["dc1"].Weight = 666;
        clusters["dc2"].Balancer = "newbalancer.net";
        setup.GetFlatMsgBusPQClient().InitDCs(clusters);
        TInstant updateTime = TInstant::Now();

        while (true) {
            auto trackerResponce = getClustersFromTracker();
            if (trackerResponce->ClustersListUpdateTimestamp) {
                if (trackerResponce->ClustersListUpdateTimestamp.GetRef() >= updateTime + TDuration::Seconds(5)) {
                    for (auto& clusterInfo : trackerResponce->ClustersList->Clusters) {
                        auto it = clusters.find(clusterInfo.Name);
                        UNIT_ASSERT(it != clusters.end());
                        compareInfo(it->first, it->second, clusterInfo);
                    }
                    break;
                }
            }
            Sleep(TDuration::MilliSeconds(100));
        }
    }

    Y_UNIT_TEST(TestReadPartitionByGroupId) {
        NPersQueue::TTestServer server;

        ui32 partitionsCount = 100;
        TString topic = "topic1";
        TString topicFullName = "rt3.dc1--" + topic;

        server.AnnoyingClient->CreateTopic(topicFullName, partitionsCount);
        server.EnableLogs({ NKikimrServices::PQ_READ_PROXY});

        auto driver = server.AnnoyingClient->GetDriver();

        for (ui32 partition = 30; partition < partitionsCount; ++partition) {
            auto reader = CreateReader(
                *driver,
                NYdb::NPersQueue::TReadSessionSettings()
                    .AppendTopics(
                        NYdb::NPersQueue::TTopicReadSettings(topic)
                            .AppendPartitionGroupIds(partition + 1)
                    )
                    .ConsumerName("shared/user")
                    .ReadOnlyOriginal(true)
            );

            TMaybe<NYdb::NPersQueue::TReadSessionEvent::TEvent> event = reader->GetEvent(true, 1);
            auto createStream = std::get_if<NYdb::NPersQueue::TReadSessionEvent::TCreatePartitionStreamEvent>(&*event);
            UNIT_ASSERT(createStream);
            TString stepDescription = TStringBuilder() << "create stream for partition=" << partition
                << " : " << createStream->DebugString();
            Cerr << stepDescription << Endl;
            UNIT_ASSERT_EQUAL_C(
                partition,
                createStream->GetPartitionStream()->GetPartitionId(),
                stepDescription
            );
        }
    }
}
}
