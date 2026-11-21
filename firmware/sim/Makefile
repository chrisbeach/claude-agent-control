# Desktop renderer for the display's UI: the same ui.c and fleet.c as the firmware,
# drawn by LVGL into a frame buffer and saved as PNGs. `make png` writes out/*.png.
#   LVGL is cloned at the version the firmware pins (src/idf_component.yml).
#   cJSON comes from the ESP-IDF package PlatformIO installed; override CJSON_DIR if needed.
LVGL_TAG ?= v9.3.0
LVGL_DIR ?= lvgl
CJSON_DIR ?= $(HOME)/.platformio/packages/framework-espidf/components/json/cJSON
CC ?= gcc
CFLAGS = -O1 -g -std=gnu11 -Wall -Wno-unused-function -DLV_CONF_INCLUDE_SIMPLE \
         -I. -I$(LVGL_DIR) -I../shared -I$(CJSON_DIR) \
         -DSIM_TTF='"../shared/fonts/Antonio-Bold.ttf"'
LVGL_SRC = $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
SRC = sim_main.c ../shared/ui.c ../shared/fleet.c $(CJSON_DIR)/cJSON.c
OBJ = $(patsubst %.c,build/%.o,$(notdir $(SRC))) $(patsubst $(LVGL_DIR)/%.c,build/lvgl/%.o,$(LVGL_SRC))
HEADERS = ../shared/ui.h ../shared/model.h ../shared/fleet.h lv_conf.h

# Clone LVGL first, then make the binary in a second pass so LVGL's sources are found.
all: | $(LVGL_DIR)
	@$(MAKE) --no-print-directory build/sim

$(LVGL_DIR):
	git clone -q --depth 1 --branch $(LVGL_TAG) https://github.com/lvgl/lvgl.git $(LVGL_DIR)

build/sim: $(OBJ)
	$(CC) $(OBJ) -o $@ -lm

build/lvgl/%.o: $(LVGL_DIR)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/%.o: ../shared/%.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/cJSON.o: $(CJSON_DIR)/cJSON.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

png: all
	python3 render.py

clean:
	rm -rf build out

.PHONY: all png clean
