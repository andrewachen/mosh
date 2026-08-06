/* ABOUTME: Implements the shared startup-configuration parsers. */
/* ABOUTME: Pure string logic; refuses locally detectable errors via return value. */

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
    so, delete this exception statement from your version. If you delete this
    exception statement from all source files in the program, then
    also delete it here.
*/

#include <cstdio>
#include <cstring>
#include <string>

#include "src/frontend/startup_config.h"

EscapeConfig parse_escape_key( const char* env )
{
  EscapeConfig cfg;
  if ( env != NULL ) {
    if ( strlen( env ) == 1 ) {
      const int key = (int)env[0];
      if ( key > 0 && key < 128 ) {
        cfg.key = key;
        if ( key < 32 ) {
          cfg.pass_key = key + (int)'@';
        } else {
          cfg.pass_key = key;
        }
        if ( cfg.pass_key >= 'A' && cfg.pass_key <= 'Z' ) {
          cfg.pass_key2 = cfg.pass_key + (int)'a' - (int)'A';
        } else {
          cfg.pass_key2 = cfg.pass_key;
        }
      } else {
        cfg.key = 0x1E;
        cfg.pass_key = '^';
        cfg.pass_key2 = '^';
      }
    } else if ( strlen( env ) == 0 ) {
      cfg.key = -1;
    } else {
      cfg.key = 0x1E;
      cfg.pass_key = '^';
      cfg.pass_key2 = '^';
    }
  }

  if ( cfg.key == 0x03 || cfg.key == 0x04 || cfg.key == 0x0A || cfg.key == 0x0C || cfg.key == 0x0D ) {
    cfg.key = 0x1E;
    cfg.pass_key = '^';
    cfg.pass_key2 = '^';
  }

  if ( cfg.key > 0 ) {
    cfg.requires_lf = cfg.key >= 32;
  }

  return cfg;
}

void escape_key_names( const EscapeConfig& cfg, std::string* pass_name, std::string* key_name )
{
  char pass_buf[16];
  char key_buf[16];
  snprintf( pass_buf, sizeof pass_buf, "\"%c\"", cfg.pass_key );
  if ( cfg.key < 32 ) {
    snprintf( key_buf, sizeof key_buf, "Ctrl-%c", cfg.pass_key );
  } else {
    snprintf( key_buf, sizeof key_buf, "\"%c\"", cfg.key );
  }
  *pass_name = pass_buf;
  *key_name = key_buf;
}

bool parse_prediction_display( const char* env,
                               Overlay::PredictionEngine::DisplayPreference* out,
                               std::string* error )
{
  if ( env == NULL ) {
    *out = Overlay::PredictionEngine::Adaptive;
    return true;
  }
  const std::string mode( env );
  if ( mode == "always" ) {
    *out = Overlay::PredictionEngine::Always;
  } else if ( mode == "never" ) {
    *out = Overlay::PredictionEngine::Never;
  } else if ( mode == "adaptive" ) {
    *out = Overlay::PredictionEngine::Adaptive;
  } else if ( mode == "experimental" ) {
    *out = Overlay::PredictionEngine::Experimental;
  } else {
    *error = std::string( "Unknown prediction mode \"" ) + mode
             + "\" (expected adaptive, always, never, or experimental)";
    return false;
  }
  return true;
}

bool parse_prediction_overwrite( const char* env )
{
  return env != NULL && std::string( env ) == "yes";
}

bool wants_title_prefix( const char* env )
{
  return env == NULL;
}
