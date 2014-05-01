// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A server side dispatcher which dispatches a given client's data to their
// stream.

#ifndef NET_TOOLS_QUIC_QUIC_DISPATCHER_H_
#define NET_TOOLS_QUIC_QUIC_DISPATCHER_H_

#include <list>

#include "base/basictypes.h"
#include "base/containers/hash_tables.h"
#include "base/memory/scoped_ptr.h"
#include "net/base/ip_endpoint.h"
#include "net/base/linked_hash_map.h"
#include "net/quic/quic_blocked_writer_interface.h"
#include "net/quic/quic_protocol.h"
#include "net/tools/epoll_server/epoll_server.h"
#include "net/tools/quic/quic_server_session.h"
#include "net/tools/quic/quic_time_wait_list_manager.h"

#if defined(COMPILER_GCC)
namespace BASE_HASH_NAMESPACE {
template<>
struct hash<net::QuicBlockedWriterInterface*> {
  std::size_t operator()(
      const net::QuicBlockedWriterInterface* ptr) const {
    return hash<size_t>()(reinterpret_cast<size_t>(ptr));
  }
};
}
#endif

namespace net {

class EpollServer;
class QuicConfig;
class QuicConnectionHelper;
class QuicCryptoServerConfig;
class QuicSession;

namespace tools {

class QuicPacketWriterWrapper;

namespace test {
class QuicDispatcherPeer;
}  // namespace test

class DeleteSessionsAlarm;
class QuicEpollConnectionHelper;

class QuicDispatcher : public QuicServerSessionVisitor {
 public:
  // Ideally we'd have a linked_hash_set: the  boolean is unused.
  typedef linked_hash_map<QuicBlockedWriterInterface*, bool> WriteBlockedList;

  // Due to the way delete_sessions_closure_ is registered, the Dispatcher
  // must live until epoll_server Shutdown. |supported_versions| specifies the
  // list of supported QUIC versions.
  QuicDispatcher(const QuicConfig& config,
                 const QuicCryptoServerConfig& crypto_config,
                 const QuicVersionVector& supported_versions,
                 EpollServer* epoll_server);

  virtual ~QuicDispatcher();

  void Initialize(int fd);

  // Process the incoming packet by creating a new session, passing it to
  // an existing session, or passing it to the TimeWaitListManager.
  virtual void ProcessPacket(const IPEndPoint& server_address,
                             const IPEndPoint& client_address,
                             const QuicEncryptedPacket& packet);

  // Called when the socket becomes writable to allow queued writes to happen.
  virtual void OnCanWrite();

  // Returns true if there's anything in the blocked writer list.
  virtual bool HasPendingWrites() const;

  // Sends ConnectionClose frames to all connected clients.
  void Shutdown();

  // QuicServerSessionVisitor interface implementation:
  // Ensure that the closed connection is cleaned up asynchronously.
  virtual void OnConnectionClosed(QuicGuid guid, QuicErrorCode error) OVERRIDE;

  // Queues the blocked writer for later resumption.
  virtual void OnWriteBlocked(QuicBlockedWriterInterface* writer) OVERRIDE;

  typedef base::hash_map<QuicGuid, QuicSession*> SessionMap;

  // Deletes all sessions on the closed session list and clears the list.
  void DeleteSessions();

  const SessionMap& session_map() const { return session_map_; }

  WriteBlockedList* write_blocked_list() { return &write_blocked_list_; }

 protected:
  // Instantiates a new low-level packet writer. Caller takes ownership of the
  // returned object.
  QuicPacketWriter* CreateWriter(int fd);

  // Instantiates a new top-level writer wrapper. Takes ownership of |writer|.
  // Caller takes ownership of the returned object.
  virtual QuicPacketWriterWrapper* CreateWriterWrapper(
      QuicPacketWriter* writer);

  virtual QuicSession* CreateQuicSession(QuicGuid guid,
                                         const IPEndPoint& server_address,
                                         const IPEndPoint& client_address);

  QuicConnection* CreateQuicConnection(QuicGuid guid,
                                       const IPEndPoint& server_address,
                                       const IPEndPoint& client_address);

  // Replaces the packet writer with |writer|. Takes ownership of |writer|.
  void set_writer(QuicPacketWriter* writer);

  QuicTimeWaitListManager* time_wait_list_manager() {
    return time_wait_list_manager_.get();
  }

  QuicEpollConnectionHelper* helper() { return helper_.get(); }
  EpollServer* epoll_server() { return epoll_server_; }

  const QuicVersionVector& supported_versions() const {
    return supported_versions_;
  }

  // Called by |framer_visitor_| when the public header has been parsed.
  virtual bool OnUnauthenticatedPublicHeader(
      const QuicPacketPublicHeader& header);

  // Information about the packet currently being dispatched.
  const IPEndPoint& current_client_address() {
    return current_client_address_;
  }
  const IPEndPoint& current_server_address() {
    return current_server_address_;
  }
  const QuicEncryptedPacket& current_packet() {
    return *current_packet_;
  }

  const QuicConfig& config() const { return config_; }

  const QuicCryptoServerConfig& crypto_config() const { return crypto_config_; }

 private:
  class QuicFramerVisitor;
  friend class net::tools::test::QuicDispatcherPeer;

  // Called by |framer_visitor_| when the private header has been parsed
  // of a data packet that is destined for the time wait manager.
  void OnUnauthenticatedHeader(const QuicPacketHeader& header);

  // Removes the session from the session map and write blocked list, and
  // adds the GUID to the time-wait list.
  void CleanUpSession(SessionMap::iterator it);

  bool HandlePacketForTimeWait(const QuicPacketPublicHeader& header);

  const QuicConfig& config_;

  const QuicCryptoServerConfig& crypto_config_;

  // The list of connections waiting to write.
  WriteBlockedList write_blocked_list_;

  SessionMap session_map_;

  // Entity that manages guids in time wait state.
  scoped_ptr<QuicTimeWaitListManager> time_wait_list_manager_;

  // An alarm which deletes closed sessions.
  scoped_ptr<DeleteSessionsAlarm> delete_sessions_alarm_;

  // The list of closed but not-yet-deleted sessions.
  std::list<QuicSession*> closed_session_list_;

  EpollServer* epoll_server_;  // Owned by the server.

  // The helper used for all connections.
  scoped_ptr<QuicEpollConnectionHelper> helper_;

  // The writer to write to the socket with. We require a writer wrapper to
  // allow replacing writer implementation without disturbing running
  // connections.
  scoped_ptr<QuicPacketWriterWrapper> writer_;

  // This vector contains QUIC versions which we currently support.
  // This should be ordered such that the highest supported version is the first
  // element, with subsequent elements in descending order (versions can be
  // skipped as necessary).
  const QuicVersionVector supported_versions_;

  // Information about the packet currently being handled.
  IPEndPoint current_client_address_;
  IPEndPoint current_server_address_;
  const QuicEncryptedPacket* current_packet_;

  QuicFramer framer_;
  scoped_ptr<QuicFramerVisitor> framer_visitor_;

  DISALLOW_COPY_AND_ASSIGN(QuicDispatcher);
};

}  // namespace tools
}  // namespace net

#endif  // NET_TOOLS_QUIC_QUIC_DISPATCHER_H_