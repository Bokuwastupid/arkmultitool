# Дев-цикл: пересобрать kopt_payload.dll и залить его в уже запущенный
# ShooterGame.exe под Proton, без перезапуска игры.
#
# Как это работает (см. обсуждение сессии): PID процесса, который видит
# Wine изнутри префикса, НЕ равен Linux PID -- поэтому инжектор ищется по
# имени процесса (`--process ShooterGame.exe`), а не по PID. Библиотеки
# pressure-vessel (`/usr/lib/pressure-vessel/overrides/...`) видны только
# внутри mount-namespace песочницы игры -- поэтому kopt_injector.exe
# запускается через `nsenter --target <linux-pid-игры> --user --mount
# --preserve-credentials`, а не напрямую с хоста.
#
# Переопределяй под свою систему: make APPID=... STEAM_ROOT=... <target>
# .env.mk -- локальный, не в git (.gitignore: .env.*); кладёт SHARE_TOKEN,
# чтобы не вставлять его руками в каждый make inject/watch.
-include .env.mk

STEAM_ROOT      ?= $(HOME)/.local/share/Steam
APPID           ?= 346110
PROTON_DIR      ?= $(STEAM_ROOT)/steamapps/common/Proton - Experimental
GAME_PROCESS    ?= ShooterGame.exe

BUILD_DIR       := build-mingw
DIST_DIR        := $(BUILD_DIR)/dist
WINEPREFIX_PATH := $(STEAM_ROOT)/steamapps/compatdata/$(APPID)/pfx
WINE64          := $(PROTON_DIR)/files/lib/wine/x86_64-unix/wine64

# Z:\... -- то же дерево, что и хостовый /..., see dosdevices/z: -> / в
# префиксе; кладём DLL/EXE рядом со сборкой, а не куда-то в WINEPREFIX, так
# что путь просто зеркалит абсолютный путь этого репозитория.
WIN_DLL := Z:$(subst /,\\,$(abspath $(DIST_DIR)/kopt_payload.dll))
WIN_EXE := Z:$(subst /,\\,$(abspath $(DIST_DIR)/kopt_injector.exe))

.PHONY: build inject unload reinject rebuild watch pid help

WATCH_INTERVAL ?= 2

help:
	@echo "make build     -- пересобрать kopt_payload.dll/kopt_injector.exe (cmake --build $(BUILD_DIR))"
	@echo "make inject    -- залить текущий $(DIST_DIR)/kopt_payload.dll в уже запущенный $(GAME_PROCESS)"
	@echo "make unload    -- выгрузить payload из запущенной игры (KoptRequestUnload)"
	@echo "make reinject  -- unload + inject (без пересборки)"
	@echo "make rebuild   -- build + unload (если загружен) + inject -- полный цикл правки-теста"
	@echo "make watch     -- ждать запуска $(GAME_PROCESS) (опрос раз в $(WATCH_INTERVAL)с) и залить payload сразу же"
	@echo "make pid       -- напечатать найденный Linux PID $(GAME_PROCESS) (для отладки, если nsenter промахнулся)"

# Реальный Linux pid игры -- нужен для nsenter --target, НЕ передаётся
# инжектору напрямую (см. шапку файла). Якорь ^\S* обязателен: без него
# pgrep -f матчит и обёртку `reaper SteamLaunch ... -- .../ShooterGame.exe`
# (её argv тоже упоминает ShooterGame.exe дальше по строке) -- нужен именно
# процесс, чья команда САМА начинается с пути до ShooterGame.exe.
PGREP_PATTERN := ^\S*$(GAME_PROCESS)

pid:
	@pid=$$(pgrep -f '$(PGREP_PATTERN)' | head -n1); \
	if [ -z "$$pid" ]; then echo "$(GAME_PROCESS) не запущен" >&2; exit 1; fi; \
	echo "$$pid"

build:
	cmake --build $(BUILD_DIR) -j$$(nproc)

# $(1) -- дополнительные аргументы kopt_injector.exe (--dll ... или --unload)
define run_injector
	pid=$$(pgrep -f '$(PGREP_PATTERN)' | head -n1); \
	if [ -z "$$pid" ]; then echo "[make] $(GAME_PROCESS) не найден (pgrep) -- игра запущена?" >&2; exit 1; fi; \
	nsenter --target "$$pid" --user --mount --preserve-credentials -- \
		env WINEPREFIX="$(WINEPREFIX_PATH)" "$(WINE64)" \
		"$(WIN_EXE)" --process $(GAME_PROCESS) $(1)
endef

# SHARE_TOKEN -- необязателен: make inject SHARE_TOKEN=eyJ... прокидывает
# --share-token инжектору (см. injector.cpp::publish_share_token) -- без
# аккаунта/логина, ровно "launch parameter", как договорились.
SHARE_TOKEN ?=

inject:
	$(call run_injector,--dll "$(WIN_DLL)"$(if $(SHARE_TOKEN), --share-token "$(SHARE_TOKEN)"))

unload:
	$(call run_injector,--unload)

reinject:
	-$(MAKE) --no-print-directory unload
	$(MAKE) --no-print-directory inject

rebuild: build reinject

# Ждёт появления $(GAME_PROCESS) (обычный опрос pgrep -- nsenter не может
# войти в неймспейсы процесса, которого ещё нет, поэтому ждать нужно ДО
# nsenter, не полагаясь на встроенный --wait самого kopt_injector.exe) и
# сразу инжектит. Полезно запускать ДО старта игры из Steam -- не нужно
# гадать с таймингом вручную каждый раз.
watch:
	@echo "[make] жду $(GAME_PROCESS)... (Ctrl+C, чтобы прервать)"
	@while true; do \
		pid=$$(pgrep -f '$(PGREP_PATTERN)' | head -n1); \
		if [ -n "$$pid" ]; then break; fi; \
		sleep $(WATCH_INTERVAL); \
	done; \
	echo "[make] найден pid $$pid, инжектирую"
	@$(MAKE) --no-print-directory inject
