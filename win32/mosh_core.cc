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

/* ABOUTME: OS-agnostic mosh client core implementation. */
/* ABOUTME: Ports stmclient's state machine, prediction, overlay, and framebuffer diffing. */

#include "win32/mosh_core.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/crypto/crypto.h"
#include "src/frontend/terminaloverlay.h"
#include "src/network/networktransport.h"
#include "src/network/networktransport-impl.h"
#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include "src/terminal/terminaldisplay.h"
#include "src/terminal/terminalframebuffer.h"
#include "src/util/timestamp.h"
#include "win32/wincompat.h"

using namespace Network;
using namespace Overlay;
using namespace Terminal;

namespace {
const unsigned int CONNECTION_TIMEOUT = 15000;
const unsigned int MAX_DATAGRAMS_PER_READABLE = 32;

bool no_packet_available( const NetworkException& error )
{
  return error.function == "No packet received";
}

void set_network_error( OverlayManager& overlays, const std::string& error )
{
  overlays.get_notification_engine().set_network_error( error );
}

void set_crypto_error( OverlayManager& overlays, const Crypto::CryptoException& error )
{
  wchar_t message[128];
  swprintf( message, sizeof message / sizeof *message, L"Crypto exception: %s", error.what() );
  overlays.get_notification_engine().set_notification_string( message );
}
}

class MoshCore::Impl {
public:
  using NetworkType = Transport<UserStream, Complete>;

  Complete local_terminal;
  OverlayManager overlays;
  std::unique_ptr<NetworkType> network;
  Display display;
  Framebuffer local_framebuffer;
  Framebuffer new_state;
  std::string open;
  std::string close;
  std::string frame;
  std::string status;
  std::wstring connecting_notification;
  bool repaint_requested;
  bool lf_entered;
  bool quit_sequence_started;
  bool finished;
  bool clean_shutdown;
  int escape_key;
  int escape_pass_key;
  int escape_pass_key2;
  bool escape_requires_lf;
  std::wstring escape_key_help;

  Impl( const char *ip, const char *port, const char *key, int cols, int rows, const char *predict )
    : local_terminal( cols, rows ), overlays(), network(), display( false ), local_framebuffer( cols, rows ),
      new_state( cols, rows ), open(), close(), frame(), status(), connecting_notification(), repaint_requested( true ),
      lf_entered( false ), quit_sequence_started( false ), finished( false ), clean_shutdown( false ), escape_key( 0x1e ),
      escape_pass_key( '^' ), escape_pass_key2( '^' ), escape_requires_lf( false ), escape_key_help()
  {
#ifdef _WIN32
    mosh_winsock_init();
#endif

    if ( predict ) {
      if ( !strcmp( predict, "always" ) ) {
        overlays.get_prediction_engine().set_display_preference( PredictionEngine::Always );
      } else if ( !strcmp( predict, "never" ) ) {
        overlays.get_prediction_engine().set_display_preference( PredictionEngine::Never );
      } else if ( !strcmp( predict, "adaptive" ) ) {
        overlays.get_prediction_engine().set_display_preference( PredictionEngine::Adaptive );
      } else if ( !strcmp( predict, "experimental" ) ) {
        overlays.get_prediction_engine().set_display_preference( PredictionEngine::Experimental );
      } else {
        throw std::runtime_error( std::string( "Unknown prediction mode " ) + predict + "." );
      }
    }

    char escape_pass_name_buf[16];
    char escape_key_name_buf[16];
    snprintf( escape_pass_name_buf, sizeof escape_pass_name_buf, "\"%c\"", escape_pass_key );
    snprintf( escape_key_name_buf, sizeof escape_key_name_buf, "Ctrl-%c", escape_pass_key );
    std::string escape_pass_name( escape_pass_name_buf );
    std::string escape_key_name( escape_key_name_buf );
    escape_key_help = L"Commands: \".\" quits, " + std::wstring( escape_pass_name.begin(), escape_pass_name.end() )
                      + L" gives literal " + std::wstring( escape_key_name.begin(), escape_key_name.end() );
    overlays.get_notification_engine().set_escape_key_string( escape_key_name );

    wchar_t connecting[128];
    swprintf( connecting, sizeof connecting / sizeof *connecting, L"Nothing received from server on UDP port %s.", port );
    connecting_notification = connecting;

    UserStream blank;
    network.reset( new NetworkType( blank, local_terminal, key, ip, port ) );
    network->set_send_delay( 1 );
    network->get_current_state().push_back( Parser::Resize( cols, rows ) );

    open = "\033[?1049h" + display.open();
    close = display.close() + "\033[?1049l";
  }

  void update_lifecycle()
  {
    if ( finished ) {
      return;
    }

    if ( network->shutdown_in_progress() && network->shutdown_acknowledged() ) {
      clean_shutdown = true;
      finished = true;
      return;
    }
    if ( network->shutdown_in_progress() && network->shutdown_ack_timed_out() ) {
      if ( status.empty() ) {
        status = "Timed out waiting for shutdown acknowledgement.";
      }
      finished = true;
      return;
    }
    if ( network->counterparty_shutdown_ack_sent() ) {
      clean_shutdown = true;
      finished = true;
      return;
    }

    if ( network->get_remote_state_num() == 0 && !network->shutdown_in_progress() ) {
      uint64_t elapsed = timestamp() - network->get_latest_remote_state().timestamp;
      if ( elapsed > CONNECTION_TIMEOUT ) {
        status = "Timed out waiting for server...";
        overlays.get_notification_engine().set_notification_string( L"Timed out waiting for server...", true );
        finished = true;
      } else if ( elapsed > 250 ) {
        overlays.get_notification_engine().set_notification_string( connecting_notification );
      }
    } else if ( network->get_remote_state_num() != 0
                && overlays.get_notification_engine().get_notification_string() == connecting_notification ) {
      overlays.get_notification_engine().set_notification_string( L"" );
    }
  }

  void process_network_input()
  {
    unsigned int datagrams = 0;
    while ( datagrams < MAX_DATAGRAMS_PER_READABLE ) {
      datagrams++;
      try {
        network->recv();
      } catch ( const NetworkException& error ) {
        if ( no_packet_available( error ) ) {
          break;
        }
        if ( !network->shutdown_in_progress() ) {
          set_network_error( overlays, error.what() );
        }
        break;
      } catch ( const Crypto::CryptoException& error ) {
        if ( error.fatal ) {
          throw;
        }
        set_crypto_error( overlays, error );
        continue;
      }

      overlays.get_notification_engine().server_heard( network->get_latest_remote_state().timestamp );
      overlays.get_notification_engine().server_acked( network->get_sent_state_acked_timestamp() );
      overlays.get_prediction_engine().set_local_frame_acked( network->get_sent_state_acked() );
      overlays.get_prediction_engine().set_send_interval( network->send_interval() );
      overlays.get_prediction_engine().set_local_frame_late_acked( network->get_latest_remote_state().state.get_echo_ack() );
    }
  }

  void feed_input( const char *buf, size_t len )
  {
    if ( finished || network->shutdown_in_progress() ) {
      return;
    }

    NetworkType& net = *network;
    overlays.get_prediction_engine().set_local_frame_sent( net.get_sent_state_last() );
    const bool paste = len > 100;
    if ( paste ) {
      overlays.get_prediction_engine().reset();
    }

    for ( size_t i = 0; i < len; i++ ) {
      const char byte = buf[i];

      if ( !paste ) {
        overlays.get_prediction_engine().new_user_byte( byte, local_framebuffer );
      }

      if ( quit_sequence_started ) {
        if ( byte == '.' ) {
          begin_shutdown();
          return;
        } else if ( byte == 0x1a ) {
          /* Windows has no portable equivalent of STMClient's SIGSTOP path. */
        } else if ( byte == escape_pass_key || byte == escape_pass_key2 ) {
          net.get_current_state().push_back( Parser::UserByte( escape_key ) );
        } else {
          net.get_current_state().push_back( Parser::UserByte( escape_key ) );
          net.get_current_state().push_back( Parser::UserByte( byte ) );
        }

        quit_sequence_started = false;
        if ( overlays.get_notification_engine().get_notification_string() == escape_key_help ) {
          overlays.get_notification_engine().set_notification_string( L"" );
        }
        continue;
      }

      quit_sequence_started = escape_key > 0 && byte == escape_key && ( lf_entered || !escape_requires_lf );
      if ( quit_sequence_started ) {
        lf_entered = false;
        overlays.get_notification_engine().set_notification_string( escape_key_help, true, false );
        continue;
      }

      lf_entered = byte == 0x0a || byte == 0x0d;
      if ( byte == 0x0c ) {
        repaint_requested = true;
      }
      net.get_current_state().push_back( Parser::UserByte( byte ) );
    }
  }

  int lifecycle_wait_time() const
  {
    if ( finished || network->shutdown_in_progress() || network->get_remote_state_num() != 0 ) {
      return INT_MAX;
    }

    const uint64_t elapsed = timestamp() - network->get_latest_remote_state().timestamp;
    const uint64_t deadline = elapsed <= 250 ? 250 : CONNECTION_TIMEOUT;
    return elapsed < deadline ? static_cast<int>( deadline - elapsed ) : 0;
  }

  void begin_shutdown()
  {
    if ( finished || network->shutdown_in_progress() ) {
      return;
    }
    if ( !network->has_remote_addr() ) {
      status = "Exiting before connecting to server.";
      finished = true;
      return;
    }

    status = "Exiting...";
    overlays.get_notification_engine().set_notification_string( L"Exiting...", true );
    network->start_shutdown();
  }
};

MoshCore::MoshCore( const char *ip, const char *port, const char *key, int cols, int rows, const char *predict )
  : impl( nullptr )
{
  /* Functional locale probe: the framebuffer encodes wide characters with
     wcrtomb(), so require that e-acute encodes as its two UTF-8 bytes. Merely
     succeeding is not enough: a single-byte codepage such as Latin-1 encodes
     this character in one byte, so a check that only rejects (size_t)-1 would
     admit the very locales this rejects. */
  mbstate_t mbs = {};
  char mb[MB_LEN_MAX];
  if ( wcrtomb( mb, L'\u00E9', &mbs ) != 2 || mb[0] != '\xc3' || mb[1] != '\xa9' ) {
    throw std::runtime_error( "MoshCore requires a UTF-8 locale. The host must call setlocale( LC_ALL, \".UTF-8\" ) before constructing MoshCore." );
  }

  freeze_timestamp();
  impl = new Impl( ip, port, key, cols, rows, predict );
}

MoshCore::~MoshCore()
{
  delete impl;
}

const std::string& MoshCore::open_sequence() const
{
  return impl->open;
}

const std::string& MoshCore::close_sequence() const
{
  return impl->close;
}

std::vector<intptr_t> MoshCore::socket_fds() const
{
  std::vector<intptr_t> result;
  const auto fds = impl->network->fds();
  result.reserve( fds.size() );
  for ( const auto fd : fds ) {
    result.push_back( static_cast<intptr_t>( fd ) );
  }
  return result;
}

void MoshCore::feed_input( const char *buf, size_t len )
{
  impl->feed_input( buf, len );
}

void MoshCore::on_readable( intptr_t )
{
  impl->process_network_input();
  impl->update_lifecycle();
}

const std::string& MoshCore::next_frame()
{
  impl->new_state = impl->network->get_latest_remote_state().state.get_fb();
  impl->overlays.apply( impl->new_state );
  impl->frame = impl->display.new_frame( !impl->repaint_requested, impl->local_framebuffer, impl->new_state );
  impl->repaint_requested = false;
  impl->local_framebuffer = impl->new_state;
  return impl->frame;
}

void MoshCore::resize( int cols, int rows )
{
  if ( impl->finished || impl->network->shutdown_in_progress() ) {
    return;
  }
  impl->network->get_current_state().push_back( Parser::Resize( cols, rows ) );
  impl->local_framebuffer = Framebuffer( cols, rows );
  impl->new_state = Framebuffer( cols, rows );
  impl->overlays.get_prediction_engine().reset();
  impl->repaint_requested = true;
}

int MoshCore::tick()
{
  freeze_timestamp();
  bool retry_network_tick = false;

  if ( !impl->finished ) {
    try {
      impl->network->tick();
    } catch ( const NetworkException& error ) {
      if ( !impl->network->shutdown_in_progress() ) {
        set_network_error( impl->overlays, error.what() );
      }
      retry_network_tick = true;
    } catch ( const Crypto::CryptoException& error ) {
      if ( error.fatal ) {
        throw;
      }
      set_crypto_error( impl->overlays, error );
      retry_network_tick = true;
    }

    std::string& send_error = impl->network->get_send_error();
    if ( !send_error.empty() ) {
      impl->status = send_error;
      impl->overlays.get_notification_engine().set_network_error( send_error );
      send_error.clear();
    } else {
      impl->overlays.get_notification_engine().clear_network_error();
    }
    impl->update_lifecycle();
  }

  if ( impl->finished ) {
    return INT_MAX;
  }
  int wait_time = std::min( std::min( impl->network->wait_time(), impl->overlays.wait_time() ),
                            impl->lifecycle_wait_time() );
  if ( retry_network_tick ) {
    wait_time = std::max( wait_time, 200 );
  }
  return wait_time;
}

void MoshCore::refresh_clock()
{
  freeze_timestamp();
}

uint64_t MoshCore::cached_timestamp() const
{
  return frozen_timestamp();
}

void MoshCore::begin_shutdown()
{
  impl->begin_shutdown();
}

bool MoshCore::is_finished() const
{
  return impl->finished;
}

bool MoshCore::exited_cleanly() const
{
  return impl->clean_shutdown;
}

const std::string& MoshCore::status_message() const
{
  return impl->status;
}
