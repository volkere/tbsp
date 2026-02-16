CC     = clang
CFLAGS = -O2 -I include -Wall
HP_CFLAGS = -O3 -I include -Wall

.PHONY: all clean

all: sender receiver sender_hp receiver_hp file_receiver

sender: src/tbsp_sender.c
	$(CC) $(CFLAGS) -o sender src/tbsp_sender.c

receiver: src/tbsp_receiver.c
	$(CC) $(CFLAGS) -o receiver src/tbsp_receiver.c

sender_hp: src/tbsp_sender_hp.c
	$(CC) $(HP_CFLAGS) -o sender_hp src/tbsp_sender_hp.c

receiver_hp: src/tbsp_receiver_hp.c
	$(CC) $(HP_CFLAGS) -o receiver_hp src/tbsp_receiver_hp.c

file_receiver: src/file_receiver.c
	$(CC) $(CFLAGS) -o file_receiver src/file_receiver.c

clean:
	rm -f sender receiver sender_hp receiver_hp file_receiver
