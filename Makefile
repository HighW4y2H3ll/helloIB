
LIBS=-libverbs

all : server client

% : %.o
	$(CC) $^ $(LIBS) -o $@

%.o : $(wildcard *.c)
	$(CC) $< -c -DBUILD_SERVER -o server.o
	$(CC) $< -c -DBUILD_CLIENT -o client.o
