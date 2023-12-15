CPPFLAGS=$(shell pkg-config --cflags libdrm)
CFLAGS=-Wall -Wextra -Wpedantic
LDFLAGS=$(shell pkg-config --libs libdrm)
OBJ=\
    main.o

mandeldrm: $(OBJ)
	gcc main.o -o mandeldrm $(LDFLAGS)

clean:
	rm -f $(OBJ) mandeldrm

.PHONY: clean mandeldrm
