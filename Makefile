# Else exist specifically for clang
ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -Wno-narrowing
SRC = $(shell find src -name '*.cpp' | sort)
FORMAT_FILES = $(shell find src \( -name '*.cpp' -o -name '*.hpp' \) -print | sort)

all:
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) $(SRC) -o hyprview.so `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon`

format-fix:
	clang-format -i $(FORMAT_FILES)

clean:
	rm ./hyprview.so
