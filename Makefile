# Makefile for escreen

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -g -O2 -Iinclude -Ibuild $(shell pkg-config --cflags wayland-client wayland-cursor cairo libpng xkbcommon pixman-1)
CXXFLAGS = -Wall -Wextra -g -O2 -Iinclude -Ibuild -Iimgui $(shell pkg-config --cflags wayland-client wayland-cursor cairo libpng xkbcommon pixman-1)
LDFLAGS = $(shell pkg-config --libs wayland-client wayland-cursor cairo libpng xkbcommon pixman-1) -lm -lstdc++

PROTOCOLS_DIR = protocols
BUILD_DIR = build
SRC_DIR = src

PROTOCOLS = \
	$(PROTOCOLS_DIR)/wlr_layer_shell_unstable_v1.xml \
	$(PROTOCOLS_DIR)/wlr_screencopy_unstable_v1.xml \
	$(PROTOCOLS_DIR)/xdg_output_unstable_v1.xml \
	$(PROTOCOLS_DIR)/xdg_shell.xml \
	$(PROTOCOLS_DIR)/wlr_data_control_unstable_v1.xml

PROTOCOL_HEADERS = \
	$(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h \
	$(BUILD_DIR)/wlr-screencopy-unstable-v1-client-protocol.h \
	$(BUILD_DIR)/xdg-output-unstable-v1-client-protocol.h \
	$(BUILD_DIR)/xdg-shell-client-protocol.h \
	$(BUILD_DIR)/wlr-data-control-unstable-v1-client-protocol.h

PROTOCOL_SOURCES = \
	$(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.c \
	$(BUILD_DIR)/wlr-screencopy-unstable-v1-client-protocol.c \
	$(BUILD_DIR)/xdg-output-unstable-v1-client-protocol.c \
	$(BUILD_DIR)/xdg-shell-client-protocol.c \
	$(BUILD_DIR)/wlr-data-control-unstable-v1-client-protocol.c

SOURCES = $(SRC_DIR)/escreen.c $(SRC_DIR)/selection.c $(SRC_DIR)/freeze.c $(SRC_DIR)/image.c $(SRC_DIR)/clipboard.c \
          $(SRC_DIR)/tool_brush.cpp $(SRC_DIR)/tool_blur.cpp $(SRC_DIR)/tool_line.cpp $(SRC_DIR)/tool_rect.cpp $(SRC_DIR)/tool_arrow.cpp $(SRC_DIR)/tool_stamp.cpp $(SRC_DIR)/tool_text.cpp $(SRC_DIR)/tool_lasso.cpp \
          $(SRC_DIR)/tools.cpp $(SRC_DIR)/imgui_impl_cairo.cpp \
          imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_widgets.cpp imgui/imgui_tables.cpp
OBJECTS = $(BUILD_DIR)/escreen.o $(BUILD_DIR)/selection.o $(BUILD_DIR)/freeze.o $(BUILD_DIR)/image.o $(BUILD_DIR)/clipboard.o $(BUILD_DIR)/config.o \
          $(BUILD_DIR)/tool_brush.o $(BUILD_DIR)/tool_blur.o $(BUILD_DIR)/tool_line.o $(BUILD_DIR)/tool_rect.o $(BUILD_DIR)/tool_arrow.o $(BUILD_DIR)/tool_stamp.o $(BUILD_DIR)/tool_text.o $(BUILD_DIR)/tool_lasso.o \
          $(BUILD_DIR)/tools.o $(BUILD_DIR)/imgui_impl_cairo.o \
          $(BUILD_DIR)/imgui.o $(BUILD_DIR)/imgui_draw.o $(BUILD_DIR)/imgui_widgets.o $(BUILD_DIR)/imgui_tables.o \
          $(PROTOCOL_SOURCES:.c=.o)
HEADERS = include/escreen.h include/tools.h $(PROTOCOL_HEADERS)
IMGUI_HEADERS = imgui/imgui.h imgui/imconfig.h imgui/imgui_internal.h imgui/imstb_rectpack.h imgui/imstb_textedit.h imgui/imstb_truetype.h

SCANNER = wayland-scanner

all: $(BUILD_DIR) escreen

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

escreen: $(PROTOCOL_HEADERS) $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS)

$(BUILD_DIR)/%-client-protocol.h: $(PROTOCOLS_DIR)/%.xml
	$(SCANNER) client-header $< $@

$(BUILD_DIR)/%-client-protocol.c: $(PROTOCOLS_DIR)/%.xml
	$(SCANNER) private-code $< $@

# Specific rules for protocols to avoid confusion with building object files from build/
$(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.o: $(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.c $(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/wlr-screencopy-unstable-v1-client-protocol.o: $(BUILD_DIR)/wlr-screencopy-unstable-v1-client-protocol.c $(BUILD_DIR)/wlr-screencopy-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/xdg-output-unstable-v1-client-protocol.o: $(BUILD_DIR)/xdg-output-unstable-v1-client-protocol.c $(BUILD_DIR)/xdg-output-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/xdg-shell-client-protocol.o: $(BUILD_DIR)/xdg-shell-client-protocol.c $(BUILD_DIR)/xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/wlr-data-control-unstable-v1-client-protocol.o: $(BUILD_DIR)/wlr-data-control-unstable-v1-client-protocol.c $(BUILD_DIR)/wlr-data-control-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/config.o: $(SRC_DIR)/config.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) $(IMGUI_HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/imgui_impl_cairo.o: $(SRC_DIR)/imgui_impl_cairo.cpp $(IMGUI_HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: imgui/%.cpp $(IMGUI_HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf escreen $(BUILD_DIR)

.PHONY: all clean
