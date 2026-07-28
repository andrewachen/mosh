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

/* ABOUTME: Windows-console I/O layer for mosh.exe: raw-mode/UTF-8 setup, teardown, and event loop. */
/* ABOUTME: Keeps Win32 console calls out of the OS-agnostic MoshCore. */
#ifndef CONSOLE_IO_H
#define CONSOLE_IO_H

/* winsock2 must precede windows.h, which would otherwise include legacy winsock.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "mosh_core.h"
#include <windows.h>
#include <memory>
#include <stdexcept>
#include <string>

struct ConsoleError : public std::runtime_error {
  DWORD win32_code;
  ConsoleError( DWORD code, const std::string &msg )
    : std::runtime_error( msg ), win32_code( code ) {}
};

struct ConsoleState {
  HANDLE h_in;
  HANDLE h_out;
  DWORD in_mode;
  DWORD out_mode;
  UINT in_cp;
  UINT out_cp;
};

void console_raw_enter( ConsoleState *saved );
void console_raw_restore( const ConsoleState *saved );
void console_dims( int *cols, int *rows );

/* Write every byte or throw ConsoleError. */
void write_all( HANDLE handle, const std::string &bytes );

/* Owns the console event loop. Fairness-only scaffold: it does not implement
   graceful shutdown. */
class ConsoleSession {
public:
  ConsoleSession( MoshCore &core, const ConsoleState &state );
  ~ConsoleSession();
  ConsoleSession( const ConsoleSession & ) = delete;
  ConsoleSession &operator=( const ConsoleSession & ) = delete;

  /* Main event loop. Fairness-only: per wakeup it services termination, then
     polls for a resize, then at most one readable socket, then one bounded
     input chunk, then writes any pending frame. Does NOT call
     begin_shutdown().

     Returns when the termination re-test observes the termination event, or
     when core.is_finished() is true after core.tick() at the top of an
     iteration.

     Otherwise it throws rather than returning. ConsoleError comes from socket
     reconciliation, the handle-count guard, the wait itself, the reader's
     deferred ReadFile failure, the resize query, socket event enumeration, and
     frame output; core.tick() and core.on_readable() additionally rethrow a
     fatal Crypto::CryptoException. Both derive from std::exception, which is
     what callers should catch. */
  void run();

  /* Test-only accessors for the event-loop fairness harness. */
  size_t input_backlog_for_test() const;
  /* True if a drain ever left the input queue empty. Records the drain event
     itself rather than an end state, so it cannot be confused by whatever the
     queue happens to hold when run() returns. */
  bool input_ever_drained_for_test() const;
  static size_t input_budget_bytes();
  /* Holds the reader queue mutex across both the backlog check and the
     termination signal, so the backlog is read consistently against a queue the
     reader thread is still growing. Returns false if the backlog is short.
     Non-blocking — the caller establishes the backlog. */
  bool signal_termination_against_backlog_for_test( size_t minimum );
private:
  class Impl;
  std::unique_ptr<Impl> impl;
};
#endif
