all:
	gcc -I../include/ -D__x86__ -o3 -Wall -Wextra mctest.c -o xcore -lpthread
