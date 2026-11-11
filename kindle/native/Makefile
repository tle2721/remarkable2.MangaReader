ZIG ?= /private/tmp/toolchain/zig-aarch64-macos-0.15.1/zig
CXX = $(ZIG) c++
CXXFLAGS = -std=c++17 -O2 -s -target arm-linux-musleabi -mcpu=cortex_a7 -static -Isrc -I../../backend/src
LDFLAGS = -static

TARGET = extension/bin/rm2manga
SRCS = src/main.cpp src/stb_impl.cpp src/stb_tt_impl.cpp
HDRS = $(wildcard src/*.hpp)

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	@mkdir -p extension/bin
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)
	@ls -lh $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
