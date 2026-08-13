/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

/* ABOUTME: Native WinSock regression test for SocketEvents handle reconciliation. */
/* ABOUTME: Requires a recycled SOCKET value to re-arm FD_READ on its new socket. */

#include <cassert>
#include <clocale>
#include <cstdio>
#include <string>

#include "src/frontend/terminaloverlay.h"
#include "src/util/locale_utils.h"
#include "win32/mosh_core.h"
#include "win32/socket_events.h"
#include "win32/test_server.h"
#include "win32/wincompat.h"

static StartupOptions never_prediction()
{
  StartupOptions opts;
  opts.predict_display = Overlay::PredictionEngine::Never;
  return opts;
}

int main()
{
  set_native_locale();
#ifdef _WIN32
  assert( setlocale( LC_ALL, ".UTF-8" ) != nullptr );
  mosh_winsock_init();

  TestServer server( 80, 24 );
  MoshCore core( "127.0.0.1", server.port().c_str(), server.get_key().c_str(),
                 80, 24, never_prediction() );

  SOCKET old_fd = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
  assert( old_fd != INVALID_SOCKET );

  SocketEvents socket_events( core );
  socket_events.reconcile( { static_cast<intptr_t>( old_fd ) } );
  assert( closesocket( old_fd ) == 0 );

  SOCKET new_fd = INVALID_SOCKET;
  for ( int attempts = 0; attempts < 1024; ++attempts ) {
    SOCKET candidate = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    assert( candidate != INVALID_SOCKET );
    if ( candidate == old_fd ) {
      new_fd = candidate;
      break;
    }
    assert( closesocket( candidate ) == 0 );
  }
  assert( new_fd != INVALID_SOCKET );

  socket_events.reconcile( { static_cast<intptr_t>( new_fd ) } );
  const WSAEVENT event = socket_events.all().at( static_cast<intptr_t>( new_fd ) );

  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  assert( bind( new_fd, reinterpret_cast<sockaddr *>( &local ), sizeof( local ) ) == 0 );
  int local_len = sizeof( local );
  assert( getsockname( new_fd, reinterpret_cast<sockaddr *>( &local ), &local_len ) == 0 );
  const char byte = 'x';
  assert( sendto( new_fd, &byte, 1, 0, reinterpret_cast<const sockaddr *>( &local ), local_len ) == 1 );
  socket_events.reconcile( { static_cast<intptr_t>( new_fd ) } );

  WSANETWORKEVENTS network_events = {};
  assert( WSAEnumNetworkEvents( new_fd, event, &network_events ) == 0 );
  assert( ( network_events.lNetworkEvents & FD_READ ) != 0 );
  assert( closesocket( new_fd ) == 0 );
  socket_events.reconcile( {} );
#endif

  puts( "test_socket_events: passed" );
  return 0;
}
