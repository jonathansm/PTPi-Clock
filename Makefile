CXX := g++
AR := ar
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
INCLUDES := -Irpi-rgb-led-matrix/include
LIBDIRS := -Lrpi-rgb-led-matrix/lib
LIBS := -lrgbmatrix -lrt -lpthread

TARGET := ptpi-clock
PREFIX ?= /opt/ptpi-clock
FONTDIR := $(PREFIX)/fonts
INSTALL ?= install
SRC := ptpi-clock.cpp
INSTALL_FONTS := \
	rpi-rgb-led-matrix/fonts/7x14B.bdf \
	rpi-rgb-led-matrix/fonts/10x20.bdf \
	rpi-rgb-led-matrix/fonts/5x8.bdf

PTP_LIB := libptpi-ptp.a
PTP_OBJ := ptp_clock_ptp.o

.PHONY: all clean install

all: $(TARGET)

$(PTP_LIB): $(PTP_OBJ)
	$(AR) rcs $(PTP_LIB) $(PTP_OBJ)

$(TARGET): $(SRC) $(PTP_LIB)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(INCLUDES) $(PTP_LIB) $(LIBDIRS) $(LIBS)

install: $(TARGET)
	$(INSTALL) -d $(PREFIX) $(FONTDIR)
	$(INSTALL) -m 755 $(TARGET) $(PREFIX)/$(TARGET)
	$(INSTALL) -m 644 $(INSTALL_FONTS) $(FONTDIR)/

clean:
	rm -f $(TARGET) $(PTP_OBJ) $(PTP_LIB)
