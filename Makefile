# ETS2 / ATS SCS Input 플러그인 — HORI Racing Wheel Apex (macOS)
#
#   make            SDK 를 받아(처음 한 번) 플러그인과 도구를 빌드한다
#   make check      게임 없이 플러그인을 3초 돌려 장치 등록 · 이름 규칙 · 휠 연결을 본다
#   make install    현재 사용자의 Steam ETS2 plugins 폴더에 복사한다
#   make LOG=<경로> CONF=<경로>   로그 · 설정 파일 경로를 빌드에 박는다(여러 계정에서 같이 볼 때)

SDK_VERSION := 1_15
SDK_URL     := https://download.eurotrucksimulator2.com/scs_sdk_$(SDK_VERSION).zip
SDK_SHA256  := 77504f14d2ac1405ba70ee3a97351662adbb39c0cf9d3a085423f01d37bc28ec
SDK_DIR     := build/sdk

# 게임은 Intel 바이너리라 Apple Silicon 에서도 Rosetta 로 돈다. SDK 헤더도 x86 계열만 허용한다.
ARCH     := -arch x86_64
CXXFLAGS := -std=c++17 -O2 -Wall -fPIC $(ARCH)
SDK_INC  := -I$(SDK_DIR)/include -I$(SDK_DIR)/include/common -I$(SDK_DIR)/include/eurotrucks2 -I$(SDK_DIR)/include/amtrucks
ifdef LOG
CXXFLAGS += -DHORI_APEX_LOG_PATH='"$(LOG)"'
endif
ifdef CONF
CXXFLAGS += -DHORI_APEX_CONF_PATH='"$(CONF)"'
endif

PLUGINS_DIR := $(HOME)/Library/Application Support/Steam/steamapps/common/Euro Truck Simulator 2/Euro Truck Simulator 2.app/Contents/MacOS/plugins

.PHONY: all check install clean
all: build/hori_apex.so build/harness build/hidsniff

$(SDK_DIR)/include/scssdk_input.h:
	@mkdir -p build
	curl -fsSL -o build/sdk.zip $(SDK_URL)
	echo "$(SDK_SHA256)  build/sdk.zip" | shasum -a 256 -c -
	unzip -oq build/sdk.zip -d $(SDK_DIR)

build/hori_apex.so: src/hori_apex.cpp $(SDK_DIR)/include/scssdk_input.h
	clang++ $(CXXFLAGS) -shared $(SDK_INC) $< -framework IOKit -framework CoreFoundation -Wl,-install_name,hori_apex.so -o $@
	codesign -s - --force $@

build/harness: tools/harness.cpp $(SDK_DIR)/include/scssdk_input.h
	clang++ -std=c++17 $(ARCH) -I$(SDK_DIR)/include $< -o $@

build/hidsniff: tools/hidsniff.swift
	swiftc -O $< -o $@

check: build/hori_apex.so build/harness
	arch -x86_64 ./build/harness ./build/hori_apex.so 3

install: build/hori_apex.so
	@test -d "$(dir $(PLUGINS_DIR))" || { echo "ETS2 not found: $(dir $(PLUGINS_DIR))"; exit 1; }
	mkdir -p "$(PLUGINS_DIR)"
	cp build/hori_apex.so "$(PLUGINS_DIR)/"
	@echo "installed → $(PLUGINS_DIR)/hori_apex.so"

clean:
	rm -rf build
