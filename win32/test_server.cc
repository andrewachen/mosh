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

/* ABOUTME: Test-only reverse transport server for the MoshCore lifecycle test. */
/* ABOUTME: Uses the real Transport protocol to turn UserStream actions into Complete state. */

#include "win32/test_server.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "src/network/networktransport.h"
#include "src/network/networktransport-impl.h"
#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include "src/util/timestamp.h"
#include "win32/wincompat.h"

using namespace Network;
using namespace Terminal;

namespace {
bool no_packet_available( const NetworkException& error )
{
#ifdef _WIN32
  return error.the_errno == 0 || error.the_errno == WSAEWOULDBLOCK || error.the_errno == WSAEMSGSIZE;
#else
  return error.the_errno == 0 || error.the_errno == EAGAIN || error.the_errno == EWOULDBLOCK;
#endif
}
}

class TestServer::Impl {
public:
  using NetworkType = Transport<Complete, UserStream>;

  Complete terminal;
  UserStream empty_user_stream;
  NetworkType network;
  uint64_t last_remote_num;
  /* Raw user bytes the server has applied, in arrival order. Guarded by
     received_mutex: appended on the pump thread in process_remote_state, read
     on the test thread in received_byte. */
  std::mutex received_mutex;
  std::vector<char> received;

  Impl( int cols, int rows )
    : terminal( cols, rows ), empty_user_stream(), network( terminal, empty_user_stream, nullptr, nullptr ),
      last_remote_num( 0 )
  {}

  void process_remote_state()
  {
    if ( network.get_remote_state_num() == last_remote_num ) {
      return;
    }

    last_remote_num = network.get_remote_state_num();
    UserStream input;
    input.apply_string( network.get_remote_diff() );
    for ( size_t i = 0; i < input.size(); i++ ) {
      const Parser::Action &action = input.get_action( i );
      const Parser::UserByte *keystroke = dynamic_cast<const Parser::UserByte *>( &action );
      if ( keystroke != nullptr ) {
        std::lock_guard<std::mutex> lock( received_mutex );
        received.push_back( keystroke->c );
      }
      const std::string host_bytes = terminal.act( action );
      /* A real server writes these bytes to its pty. The surrogate loops that
         writeback into Complete so its published terminal state includes echo. */
      terminal.act( host_bytes );
    }

    if ( !input.empty() && !network.shutdown_in_progress() ) {
      terminal.register_input_frame( last_remote_num, timestamp() );
      network.set_current_state( terminal );
    }
  }

  void on_readable()
  {
    while ( true ) {
      try {
        network.recv();
      } catch ( const NetworkException& error ) {
        if ( no_packet_available( error ) ) {
          break;
        }
        throw;
      }
      process_remote_state();
    }
  }
};

TestServer::TestServer( int cols, int rows ) : impl()
{
  mosh_winsock_init();
  impl.reset( new Impl( cols, rows ) );
}

TestServer::~TestServer() = default;

std::string TestServer::port() const
{
  return impl->network.port();
}

std::string TestServer::get_key() const
{
  return impl->network.get_key();
}

void TestServer::set_title( const std::string& title )
{
  impl->terminal.act( std::string( "\033]0;" ) + title + "\007" );
  impl->network.set_current_state( impl->terminal );
}

std::vector<intptr_t> TestServer::socket_fds() const
{
  std::vector<intptr_t> result;
  const auto fds = impl->network.fds();
  result.reserve( fds.size() );
  for ( const auto fd : fds ) {
    result.push_back( static_cast<intptr_t>( fd ) );
  }
  return result;
}

void TestServer::on_readable( intptr_t )
{
  impl->on_readable();
}

void TestServer::tick()
{
  impl->network.tick();
}

void TestServer::start_shutdown()
{
  impl->network.start_shutdown();
}

bool TestServer::received_client_state() const
{
  return impl->last_remote_num != 0;
}

bool TestServer::received_byte( char byte ) const
{
  std::lock_guard<std::mutex> lock( impl->received_mutex );
  return std::find( impl->received.begin(), impl->received.end(), byte )
    != impl->received.end();
}
