CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
INCLUDES := -Irpi-rgb-led-matrix/include
LIBDIRS := -Lrpi-rgb-led-matrix/lib
LIBS := -lrgbmatrix -lrt -lpthread

TARGET := ptpi-clock
DISPLAY ?= 7seg
PREFIX ?= /opt/ptpi-clock
FONTDIR := $(PREFIX)/fonts
INSTALL ?= install

ifeq ($(DISPLAY),7seg)
SRC := ptpi-clock-7seg.cpp
INSTALL_FONTS := rpi-rgb-led-matrix/fonts/7x14B.bdf
else ifeq ($(DISPLAY),2line)
SRC := ptpi-clock-2line.cpp
INSTALL_FONTS := rpi-rgb-led-matrix/fonts/10x20.bdf rpi-rgb-led-matrix/fonts/5x8.bdf
else ifeq ($(DISPLAY),og)
SRC := ptpi-clock.cpp
INSTALL_FONTS := rpi-rgb-led-matrix/fonts/7x14B.bdf
else
$(error Unsupported DISPLAY '$(DISPLAY)'. Use DISPLAY=7seg, DISPLAY=2line, or DISPLAY=og)
endif

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(INCLUDES) $(LIBDIRS) $(LIBS)

install: $(TARGET)
	$(INSTALL) -d $(PREFIX) $(FONTDIR)
	$(INSTALL) -m 755 $(TARGET) $(PREFIX)/$(TARGET)
	$(INSTALL) -m 644 $(INSTALL_FONTS) $(FONTDIR)/

clean:
	rm -f $(TARGET)
