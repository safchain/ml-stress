BUILT_SOURCES = drone.pb-c.c

MOSTLYCLEANFILES = drone.pb-c.c drone.pb-c.h

drone.pb-c.c: $(top_builddir)/proto/drone.proto
	$(PROTOC_C) --c_out=$(srcdir) --proto_path=$(top_builddir)/proto $(top_builddir)/proto/drone.proto

bin_PROGRAMS = $(top_builddir)/bin/mls-server $(top_builddir)/bin/mls-drone

__top_builddir__bin_mls_server_SOURCES = server.c
__top_builddir__bin_mls_drone_SOURCES = drone.c
nodist___top_builddir__bin_mls_drone_SOURCES = drone.pb-c.c
