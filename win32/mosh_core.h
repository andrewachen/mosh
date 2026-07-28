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

/* ABOUTME: OS-agnostic mosh client core: engine + state machine + prediction/overlay/diff, no Win32 calls. */
/* ABOUTME: Consumed by the console frontend; reusable by a future mintty embed. */
#ifndef MOSH_CORE_H
#define MOSH_CORE_H

#include <string>
#include <vector>
#include <cstdint>

class MoshCore {
private:
  /* PIMPL idiom - implementation details hidden in .cc */
  class Impl;
  Impl *impl;

public:
  /* predict: "adaptive" | "always" | "never" | "experimental" */
  MoshCore( const char *ip, const char *port, const char *key,
            int cols, int rows, const char *predict );
  ~MoshCore();
  MoshCore( const MoshCore& ) = delete;
  MoshCore& operator=( const MoshCore& ) = delete;

  /* Terminal lifecycle bytes: enter alternate screen / application-cursor
     mode, and restore on exit. The host MUST write open_sequence() once before
     the first frame and close_sequence() once at teardown — console mode alone
     cannot repair these VT modes, and mosh's input encoding assumes
     application-cursor mode is set. */
  /* The host must establish a UTF-8 locale before constructing MoshCore, so
     Terminal::Parser::UTF8Parser can decode remote terminal bytes correctly. */
  /* The host calls tick() at its event-loop boundary; tick refreshes mosh's
     process-wide cached timestamp before running transport timers. */
  const std::string& open_sequence() const;
  const std::string& close_sequence() const;

  /* Current UDP socket handles to wait on. May CHANGE across calls: mosh hops
     ports (Connection::hop_port) after ~10s without a round trip and prunes old
     sockets — the host MUST re-read this every loop iteration and reconcile its
     wait registrations, or roaming/recovery breaks. */
  std::vector<intptr_t> socket_fds() const;

  /* Feed user keystroke bytes toward the server. Includes the Ctrl-^ escape/quit
     parser (Ctrl-^ then '.' begins shutdown; Ctrl-^ then Ctrl-^ sends a literal);
     may transition the core toward shutdown. */
  void feed_input( const char *buf, size_t len );

  /* A socket signalled readable: pull datagrams (drain), update state. */
  void on_readable( intptr_t which_fd );

  /* Next output frame as ANSI bytes; reference valid until the next call. */
  const std::string& next_frame();

  void resize( int cols, int rows );

  /* Housekeeping: tick network, drain send errors, advance the state machine
     (incl. connection timeout). Returns ms until the next event is due, or a
     large sentinel if none — the host treats it as an effectively infinite wait. */
  int tick();

  /* Refreshes mosh's process-wide cached timestamp. The host calls this after its
     wait returns and before dispatching any ready source, so transport timing sees
     when the event arrived rather than when the wait began. tick() is the
     before-the-wait counterpart. */
  void refresh_clock();

  /* The cached timestamp itself, in milliseconds. Lets a host read the clock it
     is driving without reaching into the engine's timestamp header. Priming the
     cache is a side effect if nothing has frozen it yet, so this is not purely
     an observer; the constructor freezes once, which makes that moot in practice. */
  uint64_t cached_timestamp() const;

  /* Lifecycle / status. */
  void begin_shutdown();     /* user- or host-requested graceful shutdown */
  bool is_finished() const;  /* true on clean shutdown, remote exit, or timeout */
  bool exited_cleanly() const;               /* clean vs unclean (for exit code/message) */
  const std::string& status_message() const; /* human-readable reason, mirrors upstream */
};

#endif
