# Сборка прошивки CardPuter Games (ESP-IDF 5.4), как в m5sPlaud.
# Окружение берётся из IDF_PATH (по умолчанию ~/Work/esp/esp-idf): . $(IDF_PATH)/export.sh
# Если export.sh жалуется «Python virtual environment not found» — окружение собрано под Python 3.14,
# а системный python3 — 3.9: PYTHON=/opt/homebrew/bin/python3.14 (Makefile подставит его в PATH).

IDF_PATH ?= $(HOME)/Work/esp/esp-idf
PYTHON   ?= /opt/homebrew/bin/python3.14
# PORT=/dev/cu.usbmodem1101
PORT     ?=
# SD=/Volumes/CARDPUTER
SD       ?=
NAME     := cardputer_games.bin
BIN      := build/cardputer_games.bin
PORTARG  := $(if $(PORT),-p $(PORT))

# python3 -> нужная версия через временный каталог-шим, затем export.sh и idf.py
SHIM     := build/.pyshim
IDF      := mkdir -p $(SHIM) && ln -sf $(PYTHON) $(SHIM)/python3 && \
            export PATH=$(CURDIR)/$(SHIM):$$PATH IDF_PATH=$(IDF_PATH) && . $(IDF_PATH)/export.sh >/dev/null 2>&1 && idf.py

.PHONY: all build flash monitor flash-monitor menuconfig clean fullclean size launcher-image sd

all: build

build:
	$(IDF) build

# Если порт занят — зажать G0 при подключении кабеля (режим загрузчика).
flash:
	$(IDF) $(PORTARG) flash

monitor:
	$(IDF) $(PORTARG) monitor

flash-monitor:
	$(IDF) $(PORTARG) flash monitor

menuconfig:
	$(IDF) menuconfig

size:
	$(IDF) size

clean:
	$(IDF) clean
	rm -f $(NAME)

fullclean:
	rm -rf build sdkconfig managed_components dependencies.lock $(NAME)

# Образ для запуска из M5Stack Launcher: ./$(NAME), либо на карту: make launcher-image SD=/Volumes/XXX
launcher-image: build
	cp $(BIN) $(NAME)
	@if [ -n "$(SD)" ]; then mkdir -p $(SD)/downloads && cp $(NAME) $(SD)/downloads/ && echo "-> $(SD)/downloads/$(NAME)"; \
	 else echo "-> $(NAME)"; fi

# Скопировать содержимое sdcard_example/ (папка l18games) на смонтированную SD: make sd SD=/Volumes/XXX
sd:
	@test -n "$(SD)" || { echo "укажите SD=/Volumes/<карта>"; exit 1; }
	cp -R sdcard_example/. $(SD)/
