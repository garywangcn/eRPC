#ifndef ERPC_RPC_H
#define ERPC_RPC_H

#include <vector>
#include "common.h"
#include "nexus.h"
#include "session.h"
#include "transport.h"
#include "util/buffer.h"

namespace ERpc {

#define rpc_dprintf(fmt, ...)            \
  do {                                   \
    if (RPC_DPRINTF) {                   \
      fprintf(stderr, fmt, __VA_ARGS__); \
      fflush(stderr);                    \
    }                                    \
  } while (0)

// Per-thread RPC object
template <class Transport_>
class Rpc {
 public:
  Rpc(Nexus *nexus, void *context, session_mgmt_handler_t session_mgmt_handler,
      int app_tid, std::vector<int> fdev_port_vec);

  ~Rpc();

  void resolve_session(Session *session);

  void send_request(const Session *session, const Buffer *buffer);
  void send_response(const Session *session, const Buffer *buffer);

  void run_event_loop();

 private:
  Nexus *nexus;
  void *context; /* The application context */
  session_mgmt_handler_t session_mgmt_handler;
  int num_fdev_ports;
  int fdev_port_arr[kMaxFabDevPorts];
  int app_tid;
  Transport_ *transport; /* The unreliable transport */

  /* Shared with Nexus for session management */
  volatile SessionManagementHook sm_hook;
};

/* Instantiate required Rpc classes so they get compiled for the linker */
template class Rpc<InfiniBandTransport>;

}  // End ERpc

#endif  // ERPC_RPC_H
