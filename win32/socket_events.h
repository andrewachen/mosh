/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

/* ABOUTME: Owns the WinSock FD_READ events associated with MoshCore sockets. */
/* ABOUTME: Reconciles changing UDP socket sets before each console-loop wait. */
#ifndef SOCKET_EVENTS_H
#define SOCKET_EVENTS_H

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

#include "win32/console_io.h"

inline std::string socket_event_error_message( const char *what, DWORD error )
{
  std::ostringstream message;
  message << what << " (error " << error << ")";
  return message.str();
}

class SocketEvents {
private:
  MoshCore &core;
  std::map<intptr_t, WSAEVENT> events;

public:
  explicit SocketEvents( MoshCore &session_core ) : core( session_core ) {}

  ~SocketEvents()
  {
    const std::vector<intptr_t> live_fds = core.socket_fds();
    for ( std::map<intptr_t, WSAEVENT>::iterator it = events.begin(); it != events.end(); ++it ) {
      if ( std::find( live_fds.begin(), live_fds.end(), it->first ) != live_fds.end() ) {
        WSAEventSelect( static_cast<SOCKET>( it->first ), NULL, 0 );
      }
      WSACloseEvent( it->second );
    }
  }

  void reconcile( const std::vector<intptr_t> &fds )
  {
    for ( std::map<intptr_t, WSAEVENT>::iterator it = events.begin(); it != events.end(); ) {
      if ( std::find( fds.begin(), fds.end(), it->first ) == fds.end() ) {
        WSACloseEvent( it->second );
        it = events.erase( it );
      } else {
        ++it;
      }
    }
    for ( std::vector<intptr_t>::const_iterator it = fds.begin(); it != fds.end(); ++it ) {
      std::map<intptr_t, WSAEVENT>::iterator registered = events.find( *it );
      bool created = false;
      if ( registered == events.end() ) {
        WSAEVENT event = WSACreateEvent();
        if ( event == WSA_INVALID_EVENT ) {
          const int error = WSAGetLastError();
          throw ConsoleError( static_cast<DWORD>( error ), socket_event_error_message( "WSACreateEvent", error ) );
        }
        registered = events.insert( std::make_pair( *it, event ) ).first;
        created = true;
      }
      /* WSAEventSelect re-records FD_READ when data is already queued. Re-arm
         every pass so a recycled SOCKET value is registered on its new socket. */
      if ( WSAEventSelect( static_cast<SOCKET>( *it ), registered->second, FD_READ ) == SOCKET_ERROR ) {
        const int error = WSAGetLastError();
        if ( created ) {
          WSACloseEvent( registered->second );
          events.erase( registered );
        }
        throw ConsoleError( static_cast<DWORD>( error ), socket_event_error_message( "WSAEventSelect", error ) );
      }
    }
  }

  const std::map<intptr_t, WSAEVENT> &all() const
  {
    return events;
  }
};

#endif
