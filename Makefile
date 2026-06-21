CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pthread
TARGET    = server
SRC       = server.cpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
