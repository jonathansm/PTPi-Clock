CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
INCLUDES := -Irpi-rgb-led-matrix/include
LIBDIRS := -Lrpi-rgb-led-matrix/lib
LIBS := -lrgbmatrix -lrt -lpthread

TARGET := ptpi-clock
SRC := ptpi-clock-7seg.cpp

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(INCLUDES) $(LIBDIRS) $(LIBS)

clean:
	rm -f $(TARGET)
