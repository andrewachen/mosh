/* ABOUTME: Deterministic unit tests for the shared frontend startup-config parsers. */
/* ABOUTME: Pure — no terminal, network, or environment mutation. */

#include <cstdio>
#include <string>

#include "src/frontend/startup_config.h"

static int test_escape_key()
{
  struct EC
  {
    const char* env;
    int key;
    int pass;
    int pass2;
    bool lf;
  };
  const EC cases[] = {
    { nullptr, 0x1e, '^', '^', false }, /* absent → default Ctrl-^ */
    { "\x01", 0x01, 'A', 'a', false },  /* Ctrl-A: pass 'A' or lowercase 'a', no line start */
    { "A", 'A', 'A', 'a', true },       /* printable upper: alt lower, needs line start */
    { "", -1, '^', '^', false },        /* empty → parser disabled */
    { "abc", 0x1e, '^', '^', false },   /* multi-char → default */
    { "\x03", 0x1e, '^', '^', false },  /* forbidden Ctrl-C → default */
  };
  for ( const EC& c : cases ) {
    const EscapeConfig got = parse_escape_key( c.env );
    if ( got.key != c.key || got.pass_key != c.pass || got.pass_key2 != c.pass2 || got.requires_lf != c.lf ) {
      fprintf( stderr,
               "FAIL: parse_escape_key(\"%s\") = {%d,%d,%d,%d}, expected {%d,%d,%d,%d}\n",
               c.env ? c.env : "(null)",
               got.key,
               got.pass_key,
               got.pass_key2,
               (int)got.requires_lf,
               c.key,
               c.pass,
               c.pass2,
               (int)c.lf );
      return 1;
    }
  }
  /* Name spellings: control escape → Ctrl-<passkey>; printable → "<key>". */
  {
    std::string pass_name, key_name;
    escape_key_names( parse_escape_key( "\x01" ), &pass_name, &key_name );
    if ( pass_name != "\"A\"" || key_name != "Ctrl-A" ) {
      fprintf( stderr, "FAIL: escape_key_names(Ctrl-A) = %s / %s\n", pass_name.c_str(), key_name.c_str() );
      return 1;
    }
    escape_key_names( parse_escape_key( "A" ), &pass_name, &key_name );
    if ( pass_name != "\"A\"" || key_name != "\"A\"" ) {
      fprintf( stderr, "FAIL: escape_key_names(A) = %s / %s\n", pass_name.c_str(), key_name.c_str() );
      return 1;
    }
  }
  return 0;
}

static int test_prediction_display()
{
  struct PC
  {
    const char* env;
    Overlay::PredictionEngine::DisplayPreference want;
  };
  const PC cases[] = {
    { nullptr, Overlay::PredictionEngine::Adaptive },
    { "adaptive", Overlay::PredictionEngine::Adaptive },
    { "always", Overlay::PredictionEngine::Always },
    { "never", Overlay::PredictionEngine::Never },
    { "experimental", Overlay::PredictionEngine::Experimental },
  };
  for ( const PC& c : cases ) {
    Overlay::PredictionEngine::DisplayPreference out = Overlay::PredictionEngine::Adaptive;
    std::string err;
    if ( !parse_prediction_display( c.env, &out, &err ) ) {
      fprintf( stderr, "FAIL: prediction \"%s\" rejected: %s\n", c.env ? c.env : "(null)", err.c_str() );
      return 1;
    }
    if ( out != c.want ) {
      fprintf( stderr, "FAIL: prediction \"%s\" mapped wrong\n", c.env ? c.env : "(null)" );
      return 1;
    }
  }
  for ( const char* bad : { "bogus", "" } ) {
    Overlay::PredictionEngine::DisplayPreference out = Overlay::PredictionEngine::Adaptive;
    std::string err;
    if ( parse_prediction_display( bad, &out, &err ) || err.empty() ) {
      fprintf( stderr, "FAIL: prediction \"%s\" not refused with a message\n", bad );
      return 1;
    }
  }
  return 0;
}

static int test_overwrite_and_title()
{
  if ( parse_prediction_overwrite( nullptr ) || !parse_prediction_overwrite( "yes" )
       || parse_prediction_overwrite( "Yes" ) || parse_prediction_overwrite( "1" )
       || parse_prediction_overwrite( "" ) ) {
    fprintf( stderr, "FAIL: parse_prediction_overwrite\n" );
    return 1;
  }
  if ( !wants_title_prefix( nullptr ) || wants_title_prefix( "" ) || wants_title_prefix( "x" ) ) {
    fprintf( stderr, "FAIL: wants_title_prefix\n" );
    return 1;
  }
  return 0;
}

int main()
{
  if ( test_escape_key() || test_prediction_display() || test_overwrite_and_title() ) {
    return 1;
  }
  printf( "test_startup_config: all cases passed\n" );
  return 0;
}
