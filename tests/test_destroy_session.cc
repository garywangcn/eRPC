#include <gtest/gtest.h>
#include <atomic>
#include <map>
#include <thread>
#include "rpc.h"

using namespace ERpc;

static constexpr uint16_t kAppNexusUdpPort = 31851;
static constexpr double kAppNexusPktDropProb = 0.0;
static constexpr size_t kAppEventLoopMs = 200;
static constexpr size_t kAppMaxEventLoopMs = 10000; /* 10 seconds */
static constexpr uint8_t kAppClientAppTid = 100;
static constexpr uint8_t kAppServerAppTid = 200;

/* Shared between client and server thread */
std::atomic<bool> server_ready; /* Client starts after server is ready */
std::atomic<bool> client_done;  /* Server ends after client is done */

const uint8_t phy_port = 0;
const size_t numa_node = 0;
char local_hostname[kMaxHostnameLen];

/// Per-thread application context
class AppContext {
 public:
  Rpc<IBTransport> *rpc;

  SessionMgmtEventType exp_event;
  SessionMgmtErrType exp_err;
  SessionState exp_state;
  int exp_session_num;

  size_t num_sm_events = 0;

  /// Fill in the values expected in the next session management callback
  void arm(SessionMgmtEventType exp_event, SessionMgmtErrType exp_err,
           SessionState exp_state, int exp_session_num) {
    num_sm_events = 0; /* Reset */
    this->exp_event = exp_event;
    this->exp_err = exp_err;
    this->exp_state = exp_state;
    this->exp_session_num = exp_session_num;
  }
};

/// The common session management handler for all subtests
void sm_handler(int session_num, SessionMgmtEventType sm_event_type,
                SessionMgmtErrType sm_err_type, void *_context) {
  _unused(session_num);

  AppContext *context = (AppContext *)_context;
  context->num_sm_events++;
  printf("sm_handler: num_sm_events = %zu\n", context->num_sm_events);

  /* Check that the event and error types matche their expected values */
  ASSERT_EQ(sm_event_type, context->exp_event);
  ASSERT_EQ(sm_err_type, context->exp_err);
  ASSERT_EQ(session_num, context->exp_session_num);
}

/// The server thread used for all subtests
void server_thread_func(Nexus *nexus, uint8_t app_tid) {
  Rpc<IBTransport> rpc(nexus, nullptr, app_tid, &sm_handler, phy_port,
                       numa_node);
  server_ready = true;

  while (!client_done) { /* Wait for the client */
    rpc.run_event_loop_timeout(kAppEventLoopMs);
  }

  /* The client is done after disconnecting */
  ASSERT_EQ(rpc.num_active_sessions(), 0);
}

/**
 * @brief Launch the server thread and the client thread
 * @param client_thread_func The function executed by the client threads
 */
void launch_server_client_threads(void (*client_thread_func)(Nexus *)) {
  Nexus nexus(kAppNexusUdpPort, 0, kAppNexusPktDropProb);

  server_ready = false;
  client_done = false;

  std::thread server_thread =
      std::thread(server_thread_func, &nexus, kAppServerAppTid);

  /* Wait for server before launching clients */
  while (!server_ready) {
    usleep(1);
  }

  std::thread client_thread(client_thread_func, &nexus);

  server_thread.join();
  client_thread.join();
}

/// Run the event loop until the context has \p num_new_sm_events events, or
/// until kAppMaxEventLoopMs are elapsed.
void client_wait_for_sm_resps_or_timeout(const Nexus *nexus,
                                         AppContext &context,
                                         size_t num_new_sm_events) {
  /* Run the event loop for up to kAppMaxEventLoopMs milliseconds */
  uint64_t cycles_start = rdtsc();
  while (context.num_sm_events != num_new_sm_events) {
    context.rpc->run_event_loop_timeout(kAppEventLoopMs);

    double ms_elapsed = to_msec(rdtsc() - cycles_start, nexus->freq_ghz);
    if (ms_elapsed > kAppMaxEventLoopMs) {
      break;
    }
  }
}

/// Simple successful disconnection of one session, and other simple tests
void simple_disconnect(Nexus *nexus) {
  assert(server_ready);
  AppContext context;
  Rpc<IBTransport> rpc(nexus, (void *)&context, kAppClientAppTid, &sm_handler,
                       phy_port, numa_node);
  context.rpc = &rpc;

  /* Create the session */
  int session_num =
      rpc.create_session(local_hostname, kAppServerAppTid, phy_port);
  ASSERT_GE(session_num, 0);
  ASSERT_NE(rpc.destroy_session(session_num), 0); /* Try early disconnect */

  /* Connect the session */
  context.arm(SessionMgmtEventType::kConnected, SessionMgmtErrType::kNoError,
              SessionState::kConnected, session_num);
  client_wait_for_sm_resps_or_timeout(nexus, context, 1);
  ASSERT_EQ(context.num_sm_events, 1); /* The connect event */

  /* Disconnect the session */
  context.arm(SessionMgmtEventType::kDisconnected, SessionMgmtErrType::kNoError,
              SessionState::kDisconnected, session_num);
  rpc.destroy_session(session_num);
  client_wait_for_sm_resps_or_timeout(nexus, context, 1);
  ASSERT_EQ(context.num_sm_events, 1); /* The disconnect event */
  ASSERT_EQ(rpc.num_active_sessions(), 0);

  // Other simple tests

  /* Try to disconnect the session again. This should fail. */
  ASSERT_NE(rpc.destroy_session(session_num), 0);

  /* Try to disconnect an invalid session number. This should fail. */
  ASSERT_NE(rpc.destroy_session(-1), 0);

  client_done = true;
}

TEST(SimpleDisconnect, SimpleDisconnect) {
  launch_server_client_threads(simple_disconnect);
}

/// Repeat: Create a session to the server and disconnect it.
void disconnect_multi(Nexus *nexus) {
  assert(server_ready);

  AppContext context;
  Rpc<IBTransport> rpc(nexus, (void *)&context, kAppClientAppTid, &sm_handler,
                       phy_port, numa_node);
  context.rpc = &rpc;

  for (size_t i = 0; i < 3; i++) {
    int session_num =
        rpc.create_session(local_hostname, kAppServerAppTid, phy_port);
    ASSERT_GE(session_num, 0);

    /* Connect the session */
    context.arm(SessionMgmtEventType::kConnected, SessionMgmtErrType::kNoError,
                SessionState::kConnected, session_num);
    client_wait_for_sm_resps_or_timeout(nexus, context, 1);
    ASSERT_EQ(context.num_sm_events, 1); /* The connect event */

    /* Disconnect the session */
    context.arm(SessionMgmtEventType::kDisconnected,
                SessionMgmtErrType::kNoError, SessionState::kDisconnected,
                session_num);
    rpc.destroy_session(session_num);
    client_wait_for_sm_resps_or_timeout(nexus, context, 1);
    ASSERT_EQ(context.num_sm_events, 1); /* The disconnect event */

    ASSERT_EQ(rpc.num_active_sessions(), 0);
  }

  client_done = true;
}

TEST(DisconnectMulti, DisconnectMulti) {
  launch_server_client_threads(disconnect_multi);
}

/// Disconnect a session that encountered a remote error. This should succeed.
void disconnect_remote_error(Nexus *nexus) {
  assert(server_ready);

  AppContext context;
  Rpc<IBTransport> rpc(nexus, (void *)&context, kAppClientAppTid, &sm_handler,
                       phy_port, numa_node);
  context.rpc = &rpc;

  /* Create a session that uses an invalid remote port */
  int session_num =
      rpc.create_session(local_hostname, kAppServerAppTid, phy_port + 1);
  ASSERT_GE(session_num, 0);
  context.arm(SessionMgmtEventType::kConnectFailed,
              SessionMgmtErrType::kInvalidRemotePort,
              SessionState::kDisconnected, session_num);
  client_wait_for_sm_resps_or_timeout(nexus, context, 1);
  ASSERT_EQ(context.num_sm_events, 1); /* The connect failed event */

  /*
   * After invoking the kConnectFailed callback, the Rpc event loop immediately
   * buries the session since there are no server resources to free.
   */
  ASSERT_EQ(rpc.num_active_sessions(), 0);

  client_done = true;
}

TEST(DisconnectRemoteError, DisconnectRemoteError) {
  launch_server_client_threads(disconnect_remote_error);
}

/// Create a session for which the client fails to resolve the server's routing
/// info while processing the connect response.
void disconnect_local_error(Nexus *nexus) {
  assert(server_ready);

  AppContext context;
  Rpc<IBTransport> rpc(nexus, (void *)&context, kAppClientAppTid, &sm_handler,
                       phy_port, numa_node);
  context.rpc = &rpc;

  /* Force Rpc to fail remote routing info resolution at client */
  rpc.testing_fail_resolve_remote_rinfo_client = true;

  int session_num =
      rpc.create_session(local_hostname, kAppServerAppTid, phy_port);
  context.arm(SessionMgmtEventType::kConnectFailed,
              SessionMgmtErrType::kRoutingResolutionFailure,
              SessionState::kDisconnectInProgress, session_num);
  client_wait_for_sm_resps_or_timeout(nexus, context, 1);
  ASSERT_EQ(context.num_sm_events, 1); /* The connect failed event */

  /*
   * After invoking the kConnectFailed callback, the Rpc event loop tries to
   * free resources at the server. This won't invoke a callback, so just wait
   * for the callback-less freeing to complete.
   */
  rpc.run_event_loop_timeout(kAppEventLoopMs);
  ASSERT_EQ(rpc.num_active_sessions(), 0);

  client_done = true;
}

TEST(DisconnectLocalError, DisconnectLocalError) {
  launch_server_client_threads(disconnect_local_error);
}

int main(int argc, char **argv) {
  Nexus::get_hostname(local_hostname);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
