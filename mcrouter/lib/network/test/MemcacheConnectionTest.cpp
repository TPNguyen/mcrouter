/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <vector>

#include <folly/Conv.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/fibers/FiberManager.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/synchronization/Baton.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <mcrouter/lib/network/AsyncMcClient.h>
#include <mcrouter/options.h>
#include "mcrouter/lib/network/gen/MemcacheConnection.h"
#include "mcrouter/lib/network/test/ListenSocket.h"
#include "mcrouter/lib/network/test/MockMcThriftServerHandler.h"
#include "mcrouter/lib/network/test/TestClientServerUtil.h"

using facebook::memcache::test::TestServer;

namespace {
uint16_t getRandomPort() {
  uint16_t port = 0;
  for (size_t retry = 0; port == 0 && retry < 10; ++retry) {
    port = folly::Random::rand32(10000, 3000);
    if (facebook::memcache::isPortOpen(port)) {
      LOG(INFO) << "port " << port << " is in use";
      port = 0;
    }
  }
  CHECK_GT(port, 0) << "Fail to find free port";
  return port;
}

std::pair<
    std::shared_ptr<apache::thrift::ThriftServer>,
    std::unique_ptr<std::thread>>
startMockMcThriftServer(uint16_t port) {
  auto server = std::make_shared<apache::thrift::ThriftServer>();
  server->setInterface(
      std::make_shared<facebook::memcache::test::MockMcThriftServerHandler>());
  server->setPort(port);
  server->setNumIOWorkerThreads(1);
  auto thread = std::make_unique<std::thread>([server]() {
    LOG(INFO) << "Starting thrift server.";
    server->serve();
    LOG(INFO) << "Shutting down thrift server.";
  });
  auto conn = std::make_unique<facebook::memcache::MemcacheExternalConnection>(
      facebook::memcache::ConnectionOptions(
          "localhost", port, mc_thrift_protocol));
  bool started = conn->healthCheck();
  for (int i = 0; !started && i < 5; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    LOG(INFO) << folly::sformat(
        "health check thrift server on port {}, retry={}", port, i);
    started = conn->healthCheck();
  }
  conn.reset();
  EXPECT_TRUE(started) << folly::sformat(
      "fail to start thrift server on port {} after max retries", port);
  return std::make_pair(server, std::move(thread));
}
} // namespace

TEST(MemcacheExternalConnectionTest, simpleExternalConnection) {
  TestServer::Config config;
  config.outOfOrder = false;
  config.useSsl = false;
  auto server = TestServer::create(std::move(config));
  auto conn = std::make_unique<facebook::memcache::MemcacheExternalConnection>(
      facebook::memcache::ConnectionOptions(
          "localhost", server->getListenPort(), mc_caret_protocol));
  facebook::memcache::McSetRequest request("hello");
  request.value_ref() = folly::IOBuf(folly::IOBuf::COPY_BUFFER, "world");
  folly::fibers::Baton baton;
  conn->sendRequestOne(
      request,
      [&baton](
          const facebook::memcache::McSetRequest& /* req */,
          facebook::memcache::McSetReply&& reply) {
        EXPECT_EQ(carbon::Result::STORED, *reply.result_ref());
        baton.post();
      });
  baton.wait();
  baton.reset();
  facebook::memcache::McGetRequest getReq("hello");
  conn->sendRequestOne(
      getReq,
      [&baton](
          const facebook::memcache::McGetRequest& /* req */,
          facebook::memcache::McGetReply&& reply) {
        EXPECT_EQ(carbon::Result::FOUND, *reply.result_ref());
        EXPECT_EQ("hello", folly::StringPiece(reply.value_ref()->coalesce()));
        baton.post();
      });
  baton.wait();
  conn.reset();
  server->shutdown();
  server->join();
}

TEST(MemcacheExternalConnectionTest, simpleExternalConnectionThrift) {
  auto port = getRandomPort();
  auto serverInfo = startMockMcThriftServer(port);
  auto conn = std::make_unique<facebook::memcache::MemcacheExternalConnection>(
      facebook::memcache::ConnectionOptions(
          "localhost", port, mc_thrift_protocol));
  facebook::memcache::McSetRequest request("hello");
  request.value_ref() = folly::IOBuf(folly::IOBuf::COPY_BUFFER, "world");
  folly::fibers::Baton baton;
  conn->sendRequestOne(
      request,
      [&baton](
          const facebook::memcache::McSetRequest& /* req */,
          facebook::memcache::McSetReply&& reply) {
        EXPECT_EQ(carbon::Result::STORED, *reply.result_ref());
        baton.post();
      });
  baton.wait();
  baton.reset();
  facebook::memcache::McGetRequest getReq("hello");
  conn->sendRequestOne(
      getReq,
      [&baton](
          const facebook::memcache::McGetRequest& /* req */,
          facebook::memcache::McGetReply&& reply) {
        EXPECT_EQ(carbon::Result::FOUND, *reply.result_ref());
        EXPECT_EQ("world", folly::StringPiece(reply.value_ref()->coalesce()));
        baton.post();
      });
  baton.wait();
  conn.reset();
  serverInfo.first->stop();
  serverInfo.second->join();
}

TEST(MemcachePooledConnectionTest, PooledExternalConnection) {
  TestServer::Config config;
  config.outOfOrder = false;
  config.useSsl = false;
  auto server = TestServer::create(std::move(config));
  std::vector<std::unique_ptr<facebook::memcache::MemcacheConnection>> conns;
  for (int i = 0; i < 4; i++) {
    conns.push_back(
        std::make_unique<facebook::memcache::MemcacheExternalConnection>(
            facebook::memcache::ConnectionOptions(
                "localhost", server->getListenPort(), mc_caret_protocol)));
  }
  auto pooledConn =
      std::make_unique<facebook::memcache::MemcachePooledConnection>(
          std::move(conns));
  facebook::memcache::McSetRequest request("pooled");
  request.value_ref() = folly::IOBuf(folly::IOBuf::COPY_BUFFER, "connection");
  folly::fibers::Baton baton;
  pooledConn->sendRequestOne(
      request,
      [&baton](
          const facebook::memcache::McSetRequest& /* req */,
          facebook::memcache::McSetReply&& reply) {
        EXPECT_EQ(carbon::Result::STORED, *reply.result_ref());
        baton.post();
      });
  baton.wait();
  baton.reset();
  facebook::memcache::McGetRequest getReq("pooled");
  pooledConn->sendRequestOne(
      getReq,
      [&baton](
          const facebook::memcache::McGetRequest& /* req */,
          facebook::memcache::McGetReply&& reply) {
        EXPECT_EQ(carbon::Result::FOUND, *reply.result_ref());
        EXPECT_EQ("pooled", folly::StringPiece(reply.value_ref()->coalesce()));
        baton.post();
      });
  baton.wait();
  pooledConn.reset();
  server->shutdown();
  server->join();
}

TEST(MemcacheInternalConnectionTest, simpleInternalConnection) {
  folly::SingletonVault::singleton()->destroyInstances();
  folly::SingletonVault::singleton()->reenableInstances();

  TestServer::Config config;
  config.outOfOrder = false;
  config.useSsl = false;
  auto server = TestServer::create(std::move(config));
  facebook::memcache::McrouterOptions mcrouterOptions;
  mcrouterOptions.num_proxies = 1;
  mcrouterOptions.default_route = "/oregon/*/";
  mcrouterOptions.config_str = folly::sformat(
      R"(
        {{
          "pools": {{
            "A": {{
              "servers": [ "{}:{}" ],
              "protocol": "caret"
            }}
          }},
          "route": "Pool|A"
        }}
      )",
      "localhost",
      server->getListenPort());
  auto conn = std::make_unique<facebook::memcache::MemcacheInternalConnection>(
      "simple-internal-test", mcrouterOptions);
  facebook::memcache::McSetRequest request("internal");
  request.value_ref() = folly::IOBuf(folly::IOBuf::COPY_BUFFER, "connection");
  folly::fibers::Baton baton;
  conn->sendRequestOne(
      request,
      [&baton](
          const facebook::memcache::McSetRequest& /* req */,
          facebook::memcache::McSetReply&& reply) {
        EXPECT_EQ(carbon::Result::STORED, *reply.result_ref());
        baton.post();
      });
  baton.wait();
  baton.reset();
  facebook::memcache::McGetRequest getReq("internal");
  conn->sendRequestOne(
      getReq,
      [&baton](
          const facebook::memcache::McGetRequest& /* req */,
          facebook::memcache::McGetReply&& reply) {
        EXPECT_EQ(carbon::Result::FOUND, *reply.result_ref());
        EXPECT_EQ(
            "internal", folly::StringPiece(reply.value_ref()->coalesce()));
        baton.post();
      });
  baton.wait();
  conn.reset();
  server->shutdown();
  server->join();
}

TEST(MemcachePooledConnectionTest, PooledInternalConnection) {
  folly::SingletonVault::singleton()->destroyInstances();
  folly::SingletonVault::singleton()->reenableInstances();

  TestServer::Config config;
  config.outOfOrder = false;
  config.useSsl = false;
  auto server = TestServer::create(std::move(config));
  facebook::memcache::McrouterOptions mcrouterOptions;
  mcrouterOptions.num_proxies = 1;
  mcrouterOptions.default_route = "/oregon/*/";
  mcrouterOptions.config_str = folly::sformat(
      R"(
        {{
          "pools": {{
            "A": {{
              "servers": [ "{}:{}" ],
              "protocol": "caret"
            }}
          }},
          "route": "Pool|A"
        }}
      )",
      "localhost",
      server->getListenPort());
  std::vector<std::unique_ptr<facebook::memcache::MemcacheConnection>> conns;
  for (int i = 0; i < 4; i++) {
    conns.push_back(
        std::make_unique<facebook::memcache::MemcacheInternalConnection>(
            "pooled-internal-test", mcrouterOptions));
  }
  auto pooledConn =
      std::make_unique<facebook::memcache::MemcachePooledConnection>(
          std::move(conns));
  facebook::memcache::McSetRequest request("pooled");
  request.value_ref() = folly::IOBuf(folly::IOBuf::COPY_BUFFER, "internal");
  folly::fibers::Baton baton;
  pooledConn->sendRequestOne(
      request,
      [&baton](
          const facebook::memcache::McSetRequest& /* req */,
          facebook::memcache::McSetReply&& reply) {
        EXPECT_EQ(carbon::Result::STORED, *reply.result_ref());
        baton.post();
      });
  baton.wait();
  baton.reset();
  facebook::memcache::McGetRequest getReq("pooled");
  pooledConn->sendRequestOne(
      getReq,
      [&baton](
          const facebook::memcache::McGetRequest& /* req */,
          facebook::memcache::McGetReply&& reply) {
        EXPECT_EQ(carbon::Result::FOUND, *reply.result_ref());
        EXPECT_EQ("pooled", folly::StringPiece(reply.value_ref()->coalesce()));
        baton.post();
      });
  baton.wait();
  pooledConn.reset();
  server->shutdown();
  server->join();
}
