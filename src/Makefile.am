BUILT_SOURCES = drone.pb-c.c

MOSTLYCLEANFILES = drone.pb-c.c drone.pb-c.h $(top_builddir)/bin/drone_pb2.py

drone.pb-c.c: drone.proto
	$(PROTOC_C) --c_out=$(srcdir) drone.proto
	$(PROTOC) --python_out=$(top_builddir)/bin drone.proto

dist_noinst_DATA = drone.proto

bin_PROGRAMS = $(top_builddir)/bin/mls-server $(top_builddir)/bin/mls-drone

__top_builddir__bin_mls_server_SOURCES = server.c
__top_builddir__bin_mls_drone_SOURCES = drone.c
nodist___top_builddir__bin_mls_drone_SOURCES = drone.pb-c.c
