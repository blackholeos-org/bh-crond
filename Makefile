CC = musl-gcc
CFLAGS = -Wall -Wextra -O3 -flto -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security -D_GNU_SOURCE
LDFLAGS = -static -flto

SRC = src/main.c
OBJ = $(SRC:.c=.o)
EXEC = bh-crond

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o src/*.d $(EXEC)