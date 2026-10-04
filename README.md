# ets2-hori-wheel

macOS 에서 **HORI Racing Wheel Apex** 를 **Euro Truck Simulator 2 / American Truck Simulator** 의 입력 장치로 쓰게 하는
SCS SDK Input 플러그인.

> Status: experimental. 플러그인 로드 · 장치 등록, 조향 · 가속 · 브레이크가 게임에 들어가는 것까지 실측했다.

> HORI, Euro Truck Simulator 2, American Truck Simulator, SCS Software 와 관련 없는 비공식 프로젝트다.
> 각 상표는 해당 소유자의 것이며, 이 문서에서는 호환 대상을 밝히는 데만 쓴다.

## 왜 필요한가

이 휠은 macOS 에 **제조사 전용 HID 장치**(UsagePage `0xFF00`, 64바이트 불투명 리포트)로 잡힌다. 축 · 버튼 구조를 OS 에
알려 주지 않아서 macOS · 게임 · Steam 이 일반 휠 / 게임패드로 인식하지 못한다. 이 플러그인은 원시 리포트를 직접 해독해
SCS SDK Input API 로 게임에 **generic 입력 장치**를 등록한다 — 가상 HID 장치 · 커널 확장 · SIP 해제가 필요 없다.

## 입력 매핑 (`0f0d:01bc` 실측)

| 입력 | 리포트 위치 | 값 |
|---|---|---|
| `steer` | u16 LE @ 50 (중앙 `0x8000`) | -1 … +1 (Apex 270°) |
| `thr` (가속) | u16 LE @ 52 | -1(뗌) … +1(끝까지) |
| `brk` (브레이크) | u16 LE @ 54 | -1 … +1 |
| `l2` · `r2` | u16 LE @ 24 · 26 | -1 … +1 |
| `b1`–`b8` | byte 4 비트 0–7 | bool |
| `b9`–`b16` | byte 5 비트 0–7 | bool |
| `b17`–`b19` | byte 7 비트 0–2 (`b17` 왼 패들, `b18` 오른 패들) | bool |

## 빌드 · 설치

Xcode Command Line Tools 가 필요하다. SCS SDK 는 빌드 때 공식 주소에서 받아 해시를 확인한다(저장소에 넣지 않는다).

```sh
make            # build/hori_apex.so (x86_64 — 게임이 Rosetta 로 돈다)
make check      # 게임 없이 3초: 장치 등록 · 이름 규칙 · 휠 연결 확인
make install    # ~/Library/Application Support/Steam/.../Euro Truck Simulator 2.app/Contents/MacOS/plugins/
```

빌드 없이 넣을 때는 `hori_apex.so` 와 `tools/install.command` 를 같은 폴더에 두고 **`install.command` 를 더블클릭**한다 —
Steam 라이브러리에서 ETS2 를 찾아 plugins 폴더에 복사하고 원본과 같은지 확인한다.

게임을 켜면 "SDK 플러그인 사용" 경고가 뜬다. 옵션 → 조작에서 **"HORI Racing Wheel Apex SDK"** 를 고르고 `steer` · `thr` · `brk` 를 연결한다.

## 조향 설정 (270° 휠 → 트럭)

Apex 는 끝에서 끝까지 270° 라 트럭 핸들(900° 이상)보다 훨씬 적게 돈다 — 조금만 돌려도 크게 꺾인다. 설정 파일로
**가운데를 둔하게** 만든다. `hori-apex.conf.example` 을 `~/Library/Application Support/hori-apex.conf` 로 복사해 고친다
(경로는 환경 변수 `HORI_APEX_CONF` 또는 `make CONF=<경로>`). 게임 시작 때 읽는다 — 바꾼 뒤 게임을 다시 켜거나 콘솔에서 `sdk reinit`.

| 키 | 기본 | 뜻 |
|---|---|---|
| `steer_curve` | 1.0 | 출력 = 부호 · \|x\|^curve. 2.0 이면 절반 돌렸을 때 25% |
| `steer_deadzone` | 0.0 | 가운데 무시 폭(0 … 0.3), 그 밖은 다시 펼친다 |
| `steer_scale` | 1.0 | 마지막 배율(±1 로 자른다) |

게임 설정의 "조향 비선형성"과 겹치면 효과가 너무 세다 — 둘 중 하나만 쓴다. 읽은 값은 로그에 `config …` 줄로 남는다.

## 확인 도구

- `tools/hidsniff.swift` — 휠의 원시 리포트를 읽어 바뀐 바이트를 찍는다(매핑을 다시 잴 때).
- `tools/harness.cpp` — 게임 없이 플러그인을 `dlopen` 해 `scs_input_init` · 매 프레임 콜백을 흉내 낸다. 게임과 같은 이름 규칙을 검사한다.
  `STATE_JSON=<경로>` 를 주면 마지막 값을 JSON 으로 쓴다.
- `tools/viz.html` — 그 JSON 을 읽어 핸들 · 페달 · 버튼을 그린다(`python3 -m http.server` 로 같은 폴더를 띄워서).

## 로그

게임 로그(`~/Library/Application Support/Euro Truck Simulator 2/game.log.txt`)에 `[hori-apex]` 줄이 남고, 같은 내용을
`~/Library/Logs/hori-apex.log` 에도 쓴다(5초마다 휠 리포트 수 · 게임이 값을 가져간 프레임 수). 경로는 환경 변수 `HORI_APEX_LOG`
또는 빌드 옵션 `make LOG=<경로>` 로 바꾼다.

## 알려진 함정

- **페달은 -1 … +1 로 보낸다.** 게임은 축을 -1 … +1 로 읽는다. 0 … 1 로 보내면 뗀 상태가 가운데(50%)가 되어 브레이크가 늘 반쯤
  밟힌다(키보드 가속까지 막힌다).

- **Steam 이 휠을 먼저 잡으면 입력이 안 온다.** Steam(Steam Input)이 USB 장치를 직접 열면 플러그인 쪽 리포트가 0 이 된다.
  Steam 설정 → 컨트롤러 → *Generic Gamepad Configuration Support* 를 끈다.
- **장치 · 입력 표시 이름은 영문 · 숫자 · `_` · 공백 · `.` 만** 된다(SDK `scssdk_input_device.h`). 괄호가 들어가면 게임이
  `invalid device display_name` 으로 등록을 거부한다 — `make check` 가 같은 규칙을 검사한다.
- SDK 헤더는 x86 계열만 허용한다 — arm64 로는 빌드되지 않는다.

## License

MIT. SCS SDK 는 SCS Software 의 MIT 라이선스(빌드 때 받는 `build/sdk/sdk_license.txt`).
