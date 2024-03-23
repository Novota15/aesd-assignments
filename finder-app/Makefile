CROSS_COMPILE = aarch64-none-linux-gnu-gcc
NATIVE = gcc
TARGET = writer
SRC = writer.c

all: 
	$(NATIVE) $(SRC) -o $(OBJECTS) $(TARGET)

arm:
	$(CROSS_COMPILE) $(SRC) -o $(OBJECTS) $(TARGET)

clean:
	rm -rf $(OBJECTS) $(TARGET)