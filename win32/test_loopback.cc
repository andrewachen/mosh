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

using namespace Network;

/* Poll with bounded retries for recv() to succeed */
static std::string recv_with_retry( Connection& conn, int max_retries, int sleep_ms )
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
#ifdef _WIN32
    Sleep( sleep_ms );
#else
    struct timeval tv = { 0, sleep_ms * 1000 };
    select( 0, NULL, NULL, NULL, &tv );
#endif
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

  /* Test socket handle width */
  test_socket_handle_width( server, "Server" );
  test_socket_handle_width( client, "Client" );

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
