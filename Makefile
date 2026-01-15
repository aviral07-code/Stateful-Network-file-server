CFLAGS  = -Wall -g
LDFLAGS =

all: client server

ssnfs.h ssnfs_clnt.c ssnfs_svc.c ssnfs_xdr.c: ssnfs.x
	rpcgen ssnfs.x

client: client.o ssnfs_clnt.o ssnfs_xdr.o
	cc -o client client.o ssnfs_clnt.o ssnfs_xdr.o $(CFLAGS) $(LDFLAGS)

server: server.o ssnfs_svc.o ssnfs_xdr.o
	cc -o server server.o ssnfs_svc.o ssnfs_xdr.o $(CFLAGS) $(LDFLAGS)

client.o: client.c ssnfs.h
	cc -c client.c $(CFLAGS)

server.o: server.c ssnfs.h
	cc -c server.c $(CFLAGS)

ssnfs_clnt.o: ssnfs_clnt.c ssnfs.h
	cc -c ssnfs_clnt.c $(CFLAGS)

ssnfs_svc.o: ssnfs_svc.c ssnfs.h
	cc -c ssnfs_svc.c $(CFLAGS)

ssnfs_xdr.o: ssnfs_xdr.c ssnfs.h
	cc -c ssnfs_xdr.c $(CFLAGS)

clean:
	rm -f client server *.o ssnfs_clnt.c ssnfs_svc.c ssnfs_xdr.c ssnfs.h
