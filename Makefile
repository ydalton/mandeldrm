#CPPFLAGS=-I/usr/include/libdrm
CFLAGS=-Wall -Wextra -Wpedantic
LDFLAGS=-ldrm
OBJ=\
    main.o

mandeldrm: $(OBJ)
	gcc main.o -o mandeldrm $(LDFLAGS)

clean:
	rm -f $(OBJ) mandeldrm

.PHONY: clean mandeldrm
