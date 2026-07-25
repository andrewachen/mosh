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

/* ABOUTME: Automated lifecycle test for the OS-agnostic MoshCore. */
/* ABOUTME: Validates a real reverse Transport path, framebuffer output, and clean shutdown. */

#include <cassert>
#include <clocale>
#include <cstdio>
#include <cwchar>
#include <vector>

#include "src/util/locale_utils.h"
#include "src/util/timestamp.h"
#include "win32/mosh_core.h"
#include "win32/test_server.h"
#include "win32/wincompat.h"

static void pause_for_network()
{
#ifdef _WIN32
  Sleep( 10 );
#else
  struct timeval tv = { 0, 10000 };
  select( 0, nullptr, nullptr, nullptr, &tv );
#endif
}

static void service( MoshCore& core, TestServer& server )
{
  /* TestServer drives its Transport directly, so refresh the shared mosh clock
     once for this event-loop iteration before either endpoint runs timers. */
  freeze_timestamp();
  core.tick();
  server.tick();

  const std::vector<intptr_t> server_fds = server.socket_fds();
  for ( const intptr_t fd : server_fds ) {
    server.on_readable( fd );
  }

  const std::vector<intptr_t> core_fds = core.socket_fds();
  for ( const intptr_t fd : core_fds ) {
    core.on_readable( fd );
  }
}

int main()
{
  set_native_locale();
#ifdef _WIN32
  /* UCRT's explicit UTF-8 locale configures mbrtowc(), regardless of the
     Windows user ANSI code page reported by is_utf8_locale(). */
  assert( setlocale( LC_ALL, ".UTF-8" ) != nullptr );
  const char utf8_e_acute[] = "\xc3\xa9";
  wchar_t decoded = L'\0';
  assert( mbrtowc( &decoded, utf8_e_acute, sizeof utf8_e_acute, nullptr ) == 2 );
  assert( decoded == L'é' );
#else
  assert( is_utf8_locale() );
#endif
  mosh_winsock_init();

  TestServer server( 80, 24 );
  const std::string server_port = server.port();
  const std::string server_key = server.get_key();
  MoshCore core( "127.0.0.1", server_port.c_str(), server_key.c_str(), 80, 24, "never" );

  /* Display(false) deliberately supplies portable ANSI sequences; the native
     console frontend sets TERM before using environment-specific terminfo. */
  assert( !core.open_sequence().empty() );
  assert( !core.close_sequence().empty() );
  assert( !core.socket_fds().empty() );

  core.feed_input( "x", 1 );
  bool got_frame = false;
  for ( int i = 0; i < 200 && !got_frame; i++ ) {
    service( core, server );
    const std::string& frame = core.next_frame();
    if ( frame.find( 'x' ) != std::string::npos ) {
      got_frame = true;
    }
    pause_for_network();
  }
  assert( got_frame );

  /* A resize to the dimensions the display already has changes nothing on
     screen, so it must not dirty the display: once the diff has settled, the
     frame that follows such a resize carries no bytes. */
  bool settled = false;
  for ( int i = 0; i < 200 && !settled; i++ ) {
    service( core, server );
    settled = core.next_frame().empty();
    pause_for_network();
  }
  if ( !settled ) {
    fprintf( stderr, "FAIL: display never settled to an empty frame\n" );
    return 1;
  }

  core.resize( 80, 24 );
  const std::string resize_frame = core.next_frame();
  if ( !resize_frame.empty() ) {
    fprintf( stderr,
             "FAIL: resize to the current 80x24 emitted %u bytes of output\n",
             static_cast<unsigned>( resize_frame.size() ) );
    return 1;
  }

  /* A resize to new dimensions reaches the server, which echoes it back, and
     the echo is what changes the dimensions the display renders. That costs
     exactly one full repaint: a second one would mean the local state was
     disturbed ahead of the echo. */
  core.resize( 100, 30 );
  int repaints = 0;
  bool quiet = false;
  for ( int i = 0; i < 200; i++ ) {
    service( core, server );
    const std::string& frame = core.next_frame();
    if ( frame.find( "\033[2J" ) != std::string::npos ) {
      repaints++;
    }
    quiet = frame.empty();
    pause_for_network();
  }
  if ( repaints != 1 ) {
    fprintf( stderr, "FAIL: resize to 100x30 emitted %d full repaints, expected 1\n", repaints );
    return 1;
  }
  if ( !quiet ) {
    fprintf( stderr, "FAIL: display still emitting output after a resize to 100x30\n" );
    return 1;
  }

  core.begin_shutdown();
  for ( int i = 0; i < 400 && !core.is_finished(); i++ ) {
    service( core, server );
    pause_for_network();
  }
  assert( core.is_finished() );
  assert( core.exited_cleanly() );

  puts( "test_core: passed" );
  return 0;
}
