/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

/* ABOUTME: Loopback test for WinSock port of mosh network transport. */
/* ABOUTME: Drives two Connection objects over localhost UDP with no terminal I/O. */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include "win32/wincompat.h"
#endif

#include "src/network/network.h"
#include "src/util/timestamp.h"

using namespace Network;

static void sleep_ms( int ms )
{
#ifdef _WIN32
  Sleep( ms );
#else
  struct timeval tv = { 0, ms * 1000 };
  select( 0, NULL, NULL, NULL, &tv );
#endif
}

/* Poll with bounded retries for recv() to succeed */
static std::string recv_with_retry( Connection& conn, int max_retries, int retry_sleep_ms )
{
  for ( int i = 0; i < max_retries; i++ ) {
    try {
      return conn.recv();
    } catch ( const NetworkException& e ) {
      /* No packet, WSAEWOULDBLOCK, or WSAEMSGSIZE: keep trying. */
#ifdef _WIN32
      if ( e.the_errno != 0 && e.the_errno != WSAEWOULDBLOCK && e.the_errno != WSAEMSGSIZE ) {
        throw;
      }
#else
      if ( e.the_errno != 0 && e.the_errno != EAGAIN && e.the_errno != EWOULDBLOCK ) {
        throw;
      }
#endif
    }
    sleep_ms( retry_sleep_ms );
  }
  fprintf( stderr, "recv_with_retry: timeout after %d retries\n", max_retries );
  return "";
}

/* Test that Socket handle is pointer-sized, not truncated to int */
static void test_socket_handle_width( Connection& conn, const char* name )
{
#ifdef _WIN32
  /* The whole point of mosh_socket_t: it must not narrow SOCKET (UINT_PTR). A
     32-bit regression is a compile error here, not a silently-passing test. */
  static_assert( sizeof( mosh_socket_t ) == sizeof( SOCKET ), "mosh_socket_t must not truncate SOCKET" );
  static_assert( sizeof( mosh_socket_t ) == sizeof( void* ), "mosh_socket_t must be pointer-sized on Win64/ARM64" );
#endif
  std::vector<mosh_socket_t> fds = conn.fds();
  assert( fds.size() > 0 );
  mosh_socket_t sock = fds.back();

  /* Verify handle is not truncated - on Win64/ARM64, SOCKET is UINT_PTR (64-bit) */
  if ( sizeof( mosh_socket_t ) == 8 ) {
    /* 64-bit handle - check that high 32 bits are zero for low-numbered sockets */
    /* This verifies we're not truncating to 32-bit int somewhere */
    unsigned long long val = (unsigned long long)sock;
    printf( "%s socket handle: 0x%llx (size: %zu bytes)\n", name, val, sizeof( mosh_socket_t ) );
  } else {
    printf( "%s socket handle: %d (size: %zu bytes)\n", name, (int)sock, sizeof( mosh_socket_t ) );
  }
}

/* A receive pass over client sockets that have not sent yet must find no
   packet rather than an error. Winsock returns WSAEINVAL from recvfrom on an
   unbound UDP socket, which the client surfaces as a transient "invalid
   argument" network error whenever a receive pass runs against such a socket
   (notably a fresh port-hop socket); POSIX returns EAGAIN instead. Client
   sockets are bound to the wildcard address at creation so the pass finds no
   data. Connection::recv() consumes per-socket would-block results, so the
   public no-packet outcome is the "No packet received" exception with errno
   0; anything else is the bug. */
static void expect_no_packet( Connection& client, const char* what )
{
  try {
    client.recv();
    fprintf( stderr, "%s: recv unexpectedly returned a packet\n", what );
    exit( 1 );
  } catch ( const NetworkException& e ) {
    if ( e.the_errno != 0 ) {
      fprintf( stderr, "%s: recv threw: %s\n", what, e.what() );
      exit( 1 );
    }
  }
  printf( "%s: no packet, no error\n", what );
}

/* An unconnected UDP socket must tolerate an ICMP port-unreachable response
   as a transient receive condition. Windows surfaces that ICMP as a
   WSAECONNRESET from recvfrom unless SIO_UDP_CONNRESET has been disabled; the
   port sets that ioctl in Connection::Socket's constructor so recvfrom behaves
   like POSIX, and any WSAECONNRESET escaping that setup must instead present
   through Connection's public recv() as a "No packet received" continue, not
   be rethrown here. */
static void expect_no_connreset( Connection& conn, const char* what )
{
#ifdef _WIN32
  /* Send to a bound-but-unread loopback port, then close it before receiving.
     The resulting ICMP port-unreachable would make recvfrom report
     WSAECONNRESET if the constructor did not disable reset notifications. */
  SOCKET closed_port = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
  if ( closed_port == INVALID_SOCKET ) {
    fprintf( stderr, "%s: socket: %s\n", what, wsa_strerror( WSAGetLastError() ) );
    exit( 1 );
  }
  sockaddr_in target = {};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  if ( bind( closed_port, reinterpret_cast<sockaddr*>( &target ), sizeof( target ) ) == SOCKET_ERROR ) {
    fprintf( stderr, "%s: bind: %s\n", what, wsa_strerror( WSAGetLastError() ) );
    exit( 1 );
  }
  int target_len = sizeof( target );
  if ( getsockname( closed_port, reinterpret_cast<sockaddr*>( &target ), &target_len ) == SOCKET_ERROR
       || closesocket( closed_port ) == SOCKET_ERROR ) {
    fprintf( stderr, "%s: closed loopback port setup: %s\n", what, wsa_strerror( WSAGetLastError() ) );
    exit( 1 );
  }

  const SOCKET raw = static_cast<SOCKET>( conn.fds().back() );
  const char byte = 'x';
  if ( sendto( raw, &byte, 1, 0, reinterpret_cast<const sockaddr*>( &target ), target_len ) != 1 ) {
    fprintf( stderr, "%s: sendto: %s\n", what, wsa_strerror( WSAGetLastError() ) );
    exit( 1 );
  }

  for ( int retries = 0; retries < 100; ++retries ) {
    sleep_ms( 10 );
    try {
      conn.recv();
      fprintf( stderr, "%s: recv unexpectedly returned a packet\n", what );
      exit( 1 );
    } catch ( const NetworkException& e ) {
      if ( e.the_errno == 0 ) {
        continue;
      }
      fprintf( stderr, "%s: recv threw: %s\n", what, e.what() );
      exit( 1 );
    }
  }
#else
  expect_no_packet( conn, what );
#endif
  printf( "%s: no packet, no error\n", what );
}

int main( int argc, char* argv[] )
{
#ifdef _WIN32
  mosh_winsock_init();
#endif

  /* Create server connection first. */
  Connection server( NULL, NULL );
  std::string port = server.port();
  std::string key = server.get_key();
  printf( "Server listening on port: %s\n", port.c_str() );

  /* Create client connection with the server's actual key. */
  Connection client( key.c_str(), "127.0.0.1", port.c_str() );

  /* A receive requested for a stale, pruned socket must behave as no packet
     rather than dereference a socket no longer in Connection's live deque. */
  try {
    client.recv_from( static_cast<mosh_socket_t>( -1 ) );
    fprintf( stderr, "Stale targeted recv unexpectedly returned a packet\n" );
    return 1;
  } catch ( const NetworkException& error ) {
    if ( error.the_errno != 0 ) {
      fprintf( stderr, "Stale targeted recv threw: %s\n", error.what() );
      return 1;
    }
  }

  /* Test socket handle width */
  test_socket_handle_width( server, "Server" );
  test_socket_handle_width( client, "Client" );

  /* Fresh client socket must tolerate a receive pass before any send. */
  expect_no_packet( client, "Fresh client socket recv" );

  /* WSAECONNRESET tolerance: an unconnected recv after an ICMP
     port-unreachable must present as no packet, not as a fatal reset error.
     The ioctl is read back on the live socket to prove it is in force, then a
     datagram to a dead port would have escalated the ICMP reply to
     WSAECONNRESET before this fix; the receive pass must now find nothing. */
  expect_no_connreset( client, "Unconnected recv tolerates ICMP port-unreachable" );

  /* The hop check at the end of Connection::send() requires the last port
     choice and the last round trip to both be older than PORT_HOP_INTERVAL
     (10 s); the constructor's setup() restarts that clock, so wait it out.
     The triggering datagram goes out on the old socket, then the hop appends
     a fresh unsent socket. */
  printf( "Waiting out the port-hop interval...\n" );
  sleep_ms( 11000 );
  /* timestamp() is a cached value; the sleep is invisible until re-frozen. */
  freeze_timestamp();
  size_t sockets_before_hop = client.fds().size();
  client.send( "first send triggers the hop" );
  if ( client.fds().size() <= sockets_before_hop ) {
    fprintf( stderr, "Client send past the hop interval did not hop ports\n" );
    exit( 1 );
  }
  /* The next receive pass scans the old and new sockets together, with the
     new one still unsent. */
  expect_no_packet( client, "Post-hop recv (old and fresh sockets)" );

  /* Drain the hop-triggering datagram so later assertions see fresh data. */
  std::string hop_trigger = recv_with_retry( server, 200, 10 );
  assert( hop_trigger == "first send triggers the hop" );

  /* The fresh hop socket must complete a round trip once the server learns
     the new port from it. */
  const char* hop_msg = "hello from the hopped socket";
  client.send( hop_msg );
  std::string server_hop_recv = recv_with_retry( server, 200, 10 );
  assert( server_hop_recv == hop_msg );
  server.send( "reply to the hopped socket" );
  std::string client_hop_recv = recv_with_retry( client, 200, 10 );
  assert( client_hop_recv == "reply to the hopped socket" );
  printf( "Post-hop round trip passed\n" );

  /* First round-trip: client -> server */
  const char* client_msg = "hello from client";
  client.send( client_msg );
  std::string server_recv = recv_with_retry( server, 200, 10 );
  assert( server_recv == client_msg );
  printf( "Server received: \"%s\"\n", server_recv.c_str() );

  /* Second round-trip: server -> client */
  const char* server_msg = "hello from server";
  server.send( server_msg );
  std::string client_recv = recv_with_retry( client, 200, 10 );
  assert( client_recv == server_msg );
  printf( "Client received: \"%s\"\n", client_recv.c_str() );

  /* Third round-trip to verify move-only Socket survived push/pop without double-close */
  const char* client_msg2 = "second message from client";
  client.send( client_msg2 );
  std::string server_recv2 = recv_with_retry( server, 200, 10 );
  assert( server_recv2 == client_msg2 );
  printf( "Server received (2nd): \"%s\"\n", server_recv2.c_str() );

  printf( "All loopback tests passed!\n" );
  return 0;
}
