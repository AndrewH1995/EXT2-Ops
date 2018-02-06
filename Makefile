CFLAGS = -std=gnu99 -Wall -g
PROG = readimage ext2_mkdir ext2_cp ext2_ln ext2_rm ext2_restore ext2_checker
SRC = readimage.c ext2_mkdir.c ext2_cp.c ext2_ln.c ext2_rm.c ext2_restore.c ext2_checker.c

all: readimage ext2_mkdir ext2_cp ext2_ln ext2_rm ext2_restore ext2_checker

readimage: readimage.c ext2.h utils.o
	gcc ${CFLAGS} -o $@ $< utils.o

ext2_mkdir: ext2_mkdir.c ext2.h utils.o
	gcc ${CFLAGS} -o $@ $< utils.o

ext2_cp: ext2_cp.c ext2.h utils.o
	gcc ${CFLAGS} -o $@ $< utils.o

ext2_ln: ext2_ln.c ext2.h utils.o
	gcc ${CFLAGS} -o $@ $< utils.o

ext2_rm: ext2_rm.c ext2.h utils.o
	gcc ${CFLAGS} -o $@ $< utils.o

ext2_restore: ext2_restore.c ext2.h utils.o
	gcc ${CFLAGS} -o $@ $< utils.o

ext2_checker: ext2_checker.c ext2.h utils.o
	gcc ${CFLAGS} -o $@ $< utils.o

utils.o: utils.c utils.h ext2.h
	gcc ${CFLAGS} -c -o $@ $<

clean:
	rm -rf $(PROG) *.dSYM *.o
