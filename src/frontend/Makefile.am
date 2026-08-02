AM_CPPFLAGS = -I$(top_srcdir)/ $(TINFO_CFLAGS) $(protobuf_CFLAGS) $(CRYPTO_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)
AM_LDFLAGS  = $(HARDEN_LDFLAGS) $(CODE_COVERAGE_LIBS)
LDADD = ../crypto/libmoshcrypto.a ../network/libmoshnetwork.a ../statesync/libmoshstatesync.a ../terminal/libmoshterminal.a ../util/libmoshutil.a ../protobufs/libmoshprotos.a -lm $(TINFO_LIBS) $(protobuf_LIBS) $(CRYPTO_LIBS)

mosh_server_LDADD = $(LDADD)

bin_PROGRAMS =

if BUILD_CLIENT
  bin_PROGRAMS += mosh-client
endif

if BUILD_SERVER
  bin_PROGRAMS += mosh-server
endif

mosh_client_SOURCES = mosh-client.cc stmclient.cc stmclient.h startup_config.cc startup_config.h terminaloverlay.cc terminaloverlay.h
mosh_server_SOURCES = mosh-server.cc

check_PROGRAMS = test_startup_config
test_startup_config_SOURCES = test_startup_config.cc startup_config.cc startup_config.h terminaloverlay.h
test_startup_config_LDADD =
TESTS = test_startup_config
