CC = gcc
CFLAGS = -Wall -g

OSS_SRC = oss.c
WORKER_SRC = worker.c

all: oss worker

oss: $(OSS_SRC)
	$(CC) $(CFLAGS) -o oss $(OSS_SRC)

worker: $(WORKER_SRC)
	$(CC) $(CFLAGS) -o worker $(WORKER_SRC)

clean:
	rm -f oss worker *.o oss_log.txt
