cc=gcc

all: dchat

dchat:
	$(cc) src/dchat.c -lpthread -o dchat

clean:
	rm -rf dchat