TARGET = fightvm
#TARGET = fightvm.html
LIBS = -lm `pkg-config --cflags --libs sdl2 SDL2_mixer`
CFLAGS= -o fightvm -o 'fightvm.html' -g -Wall -std=c99 -pedantic -I ./include `pkg-config --cflags --libs sdl2 SDL2_mixer` -D DEBUG
#CFLAGS= -flto -O3 -o fightvm.html -sUSE_SDL=2 -I ./include
LDFLAGS =
CC= gcc

.PHONY: default all clean

default: clean $(TARGET)
all: default

#OBJECTS = $(patsubst %.c, %.o, $(wildcard *.c))
#HEADERS = $(wildcard *.h)

SRC = $(wildcard src/*.c) $(wildcard src/*/*.c)
OBJ = $(addprefix obj/,$(notdir $(SRC:.c=.o)))

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.o: src/%.c | obj
	$(CC) $< -c $(CFLAGS) -o $@

obj/%.o: src/*/%.c | obj
	$(CC) $< -c $(CFLAGS) -o $@

obj:
	mkdir obj

.PRECIOUS: $(TARGET) $(OBJ)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -Wall $(LIBS) -o $@

clean:
	-rm -f ./obj/*.o
	-rm -f $(TARGET)

