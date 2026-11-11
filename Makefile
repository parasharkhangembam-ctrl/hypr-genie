# Genie: build with `make`, check the maths with `make test`
CXX      ?= g++
CXXFLAGS ?= -O2
PKGS      = hyprland pixman-1 libdrm pangocairo libinput wayland-server xkbcommon

# build to a temp file, then rename: never overwrite a .so Hyprland has loaded
genie.so: main.cpp genie_math.hpp
	$(CXX) -shared -fPIC --no-gnu-unique -std=c++26 $(CXXFLAGS) main.cpp -o genie.so.tmp $$(pkg-config --cflags $(PKGS))
	mv -f genie.so.tmp genie.so

test: tests/test_math.cpp genie_math.hpp
	$(CXX) -std=c++20 -O1 -Wall -Wextra -I. tests/test_math.cpp -o tests/test_math
	./tests/test_math

clean:
	rm -f genie.so genie.so.tmp tests/test_math

.PHONY: test clean
