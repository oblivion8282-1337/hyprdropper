CXX       ?= g++
CXXFLAGS  ?= -O2 -g
CXXFLAGS  += -std=c++2b -fPIC -fno-gnu-unique -Wall -Wextra -Wno-unused-parameter
PKGS       = hyprland pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon
PLUGIN     = build/hyprdropper.so

SRC        = src/main.cpp src/DropLogic.cpp

all: $(PLUGIN)

$(PLUGIN): $(SRC) src/DropLogic.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -shared $(SRC) -o $@ `pkg-config --cflags $(PKGS)` `pkg-config --libs pangocairo`

build/test_droplogic: tests/test_droplogic.cpp src/DropLogic.cpp src/DropLogic.hpp
	@mkdir -p build
	$(CXX) -std=c++2b -O0 -g -Wall -Wextra tests/test_droplogic.cpp src/DropLogic.cpp -o $@

test: build/test_droplogic
	./build/test_droplogic

clean:
	rm -rf build

.PHONY: all test clean
