CC = gcc
CFLAGS = -Wall -O3 -march=native
LIBS = -lX11 -lGL -lXext -lrt -lasound -lrt

TARGET = gl_overlay
SRC = main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

run: all
	./$(TARGET)
