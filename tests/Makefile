.PHONY: test libco

all: libco-test-64 libco-test-32

test: libco all
	@echo "==== TEST 64 bit ===="
	@LD_LIBRARY_PATH=.. ./libco-test-64
	# @echo "==== TEST 32 bit ===="
	# @LD_LIBRARY_PATH=.. ./libco-test-32

debug: libco all
	@LD_LIBRARY_PATH=.. gdb ./libco-test-64 -x gdb.init

libco:
	cd .. && make

libco-test-64: main.c
	gcc -I.. -L.. -m64 main.c -o $@ -g -lco-64

libco-test-32: main.c
	gcc -I.. -L.. -m32 main.c -o $@ -g -lco-32 

clean:
	rm -f libco-test-*