bin_SCRIPTS = mls-client.py drone_pb2.py

MOSTLYCLEANFILES = drone_pb2.py

drone_pb2.py: $(top_builddir)/proto/drone.proto Makefile
	$(PROTOC) --python_out=$(srcdir) --proto_path=$(top_builddir)/proto/ $(top_builddir)/proto/drone.proto

mls-client.py: drone_pb2.py Makefile
	cp drone_pb2.py mls-client.py $(top_builddir)/bin
