// HORI Wireless Racing Wheel Apex → ETS2/ATS SCS Input API (generic device), macOS.
// 시험용: 휠의 vendor HID 리포트(64B)를 IOHIDManager 로 읽어 축 · 버튼으로 내보낸다.
// 바이트 배치는 2026-10-01 hidsniff 실측: steer u16le@50 (중앙 0x8000), throttle u16le@52, brake u16le@54,
// 아날로그 버튼 u16le@24 · @26, 버튼 비트 byte4 · byte5 · byte7(하위 3비트).

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <thread>

#include "scssdk_input.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_input_eut2.h"

namespace {

constexpr int kVendor = 0x0f0d;
constexpr int kProduct = 0x01bc;
constexpr size_t kReportLen = 64;

enum : scs_u32_t { AX_STEER, AX_THROTTLE, AX_BRAKE, AX_L2, AX_R2, AXIS_COUNT };
constexpr scs_u32_t BUTTON_COUNT = 8 + 8 + 3;
constexpr scs_u32_t INPUT_COUNT = AXIS_COUNT + BUTTON_COUNT;

std::mutex g_lock;
uint8_t g_report[kReportLen];
bool g_have_report = false;

std::thread g_thread;
std::atomic<CFRunLoopRef> g_loop{nullptr};
IOHIDManagerRef g_manager = nullptr;
uint8_t g_buf[kReportLen];
scs_log_t g_log = nullptr;

// 프레임마다 스냅샷을 떠서 입력을 하나씩 내보낸다.
uint8_t g_snap[kReportLen];
scs_u32_t g_next = INPUT_COUNT;

scs_input_device_input_t g_inputs[INPUT_COUNT];
char g_names[INPUT_COUNT][8];
char g_labels[INPUT_COUNT][24];

std::atomic<unsigned long> g_reports{0}, g_frames{0};
#ifdef HORI_APEX_DIAG
// 진단(`make DIAG=1`): 직전 5초 동안 페달 최대값 · 바이트별 최소 · 최대 — 움직인 바이트를 찾아 매핑을 실측한다.
std::atomic<unsigned> g_thr_max{0}, g_brk_max{0};
uint8_t g_bmin[kReportLen], g_bmax[kReportLen];
bool g_brange_init = false;
#endif
std::mutex g_log_lock;

// 로그 파일 경로: 환경 변수 HORI_APEX_LOG > 빌드 때 HORI_APEX_LOG_PATH > ~/Library/Logs/hori-apex.log
// 게임 로그(game.log.txt)와 별도로 남겨, 게임 밖에서도 상태(리포트 수 · 프레임 수)를 볼 수 있게 한다.
const char *log_path()
{
	static char path[1024];
	if (!path[0]) {
		if (const char *e = getenv("HORI_APEX_LOG")) snprintf(path, sizeof path, "%s", e);
#ifdef HORI_APEX_LOG_PATH
		else snprintf(path, sizeof path, "%s", HORI_APEX_LOG_PATH);
#else
		else snprintf(path, sizeof path, "%s/Library/Logs/hori-apex.log", getenv("HOME") ? getenv("HOME") : "/tmp");
#endif
	}
	return path;
}

void plog(scs_log_type_t type, const char *fmt, ...)
{
	char msg[512];
	va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
	if (g_log) g_log(type, msg);
	std::lock_guard<std::mutex> g(g_log_lock);
	if (FILE *f = fopen(log_path(), "a")) {
		char ts[32]; time_t t = time(nullptr); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
		fprintf(f, "%s uid=%d %s\n", ts, (int)getuid(), msg);
		fclose(f);
	}
}

// 조향 설정 — 게임 시작(또는 콘솔 `sdk reinit`) 때 설정 파일에서 읽는다.
// 파일: 환경 변수 HORI_APEX_CONF > 빌드 때 HORI_APEX_CONF_PATH > ~/Library/Application Support/hori-apex.conf
// 형식: `키 = 값` 한 줄씩, `#` 뒤는 주석. 없거나 못 읽으면 기본값(효과 없음).
struct Config {
	float steer_curve = 1.0f;     // 1 = 그대로, 2 = 가운데가 둔하고 끝에서 급해진다 (출력 = 부호 · |x|^curve)
	float steer_deadzone = 0.0f;  // 가운데에서 무시할 폭(0 … 0.3). 그 밖은 다시 0 … 1 로 펼친다
	float steer_scale = 1.0f;     // 마지막에 곱한다(1 미만이면 끝까지 돌려도 덜 꺾인다). ±1 로 자른다
} g_cfg;

const char *conf_path()
{
	static char path[1024];
	if (!path[0]) {
		if (const char *e = getenv("HORI_APEX_CONF")) snprintf(path, sizeof path, "%s", e);
#ifdef HORI_APEX_CONF_PATH
		else snprintf(path, sizeof path, "%s", HORI_APEX_CONF_PATH);
#else
		else snprintf(path, sizeof path, "%s/Library/Application Support/hori-apex.conf", getenv("HOME") ? getenv("HOME") : "/tmp");
#endif
	}
	return path;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void load_config()
{
	g_cfg = Config{};
	FILE *f = fopen(conf_path(), "r");
	if (!f) { plog(SCS_LOG_TYPE_message, "[hori-apex] no config at %s — defaults", conf_path()); return; }
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (char *h = strchr(line, '#')) *h = 0;
		char key[64]; float val;
		if (sscanf(line, " %63[a-z_] = %f", key, &val) != 2) continue;
		if (!strcmp(key, "steer_curve")) g_cfg.steer_curve = clampf(val, 0.2f, 5.0f);
		else if (!strcmp(key, "steer_deadzone")) g_cfg.steer_deadzone = clampf(val, 0.0f, 0.3f);
		else if (!strcmp(key, "steer_scale")) g_cfg.steer_scale = clampf(val, 0.1f, 2.0f);
		else plog(SCS_LOG_TYPE_warning, "[hori-apex] unknown config key '%s'", key);
	}
	fclose(f);
	plog(SCS_LOG_TYPE_message, "[hori-apex] config %s: steer_curve=%.2f steer_deadzone=%.3f steer_scale=%.2f",
		conf_path(), g_cfg.steer_curve, g_cfg.steer_deadzone, g_cfg.steer_scale);
}

// 원시 조향(-1 … +1)에 데드존 → 곡선 → 배율을 씌운다.
float shape_steer(float x)
{
	const float sign = x < 0 ? -1.f : 1.f;
	float a = fabsf(x);
	const float dz = g_cfg.steer_deadzone;
	a = a <= dz ? 0.f : (a - dz) / (1.f - dz);
	a = powf(a, g_cfg.steer_curve);
	return clampf(sign * a * g_cfg.steer_scale, -1.f, 1.f);
}

inline unsigned u16(const uint8_t *r, size_t at) { return r[at] | (r[at + 1] << 8); }

void on_report(void *, IOReturn, void *, IOHIDReportType, uint32_t, uint8_t *report, CFIndex len)
{
	if (len < static_cast<CFIndex>(kReportLen)) return;
	std::lock_guard<std::mutex> g(g_lock);
	memcpy(g_report, report, kReportLen);
	g_have_report = true;
	++g_reports;
#ifdef HORI_APEX_DIAG
	if (!g_brange_init) { memcpy(g_bmin, report, kReportLen); memcpy(g_bmax, report, kReportLen); g_brange_init = true; }
	for (size_t i = 0; i < kReportLen; ++i) { if (report[i] < g_bmin[i]) g_bmin[i] = report[i]; if (report[i] > g_bmax[i]) g_bmax[i] = report[i]; }
	// 상태 로그용: 직전 5초 동안 페달 최대값
	const unsigned t = u16(report, 52), b = u16(report, 54);
	for (unsigned cur = g_thr_max.load(); t > cur && !g_thr_max.compare_exchange_weak(cur, t);) {}
	for (unsigned cur = g_brk_max.load(); b > cur && !g_brk_max.compare_exchange_weak(cur, b);) {}
#endif
}

void on_match(void *, IOReturn, void *, IOHIDDeviceRef device)
{
	IOHIDDeviceRegisterInputReportCallback(device, g_buf, sizeof(g_buf), on_report, nullptr);
	plog(SCS_LOG_TYPE_message, "[hori-apex] wheel connected");
}

void hid_thread()
{
	g_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	const int vendor = kVendor, product = kProduct;
	CFNumberRef v = CFNumberCreate(nullptr, kCFNumberIntType, &vendor);
	CFNumberRef p = CFNumberCreate(nullptr, kCFNumberIntType, &product);
	const void *keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey)};
	const void *vals[] = {v, p};
	CFDictionaryRef match = CFDictionaryCreate(nullptr, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOHIDManagerSetDeviceMatching(g_manager, match);
	CFRelease(match); CFRelease(v); CFRelease(p);

	IOHIDManagerRegisterDeviceMatchingCallback(g_manager, on_match, nullptr);
	g_loop = CFRunLoopGetCurrent();
	IOHIDManagerScheduleWithRunLoop(g_manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	const IOReturn r = IOHIDManagerOpen(g_manager, kIOHIDOptionsTypeNone);
	if (r != kIOReturnSuccess) plog(SCS_LOG_TYPE_error, "[hori-apex] IOHIDManagerOpen failed 0x%08x", r);
	else plog(SCS_LOG_TYPE_message, "[hori-apex] HID manager open");
	// 5초마다 상태: 휠 리포트 수 · 게임이 값을 가져간 프레임 수 · 마지막 조향
	CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(nullptr, CFAbsoluteTimeGetCurrent() + 5, 5, 0, 0, ^(CFRunLoopTimerRef) {
		unsigned steer;
		{ std::lock_guard<std::mutex> g(g_lock); steer = g_have_report ? u16(g_report, 50) : 0; }
#ifdef HORI_APEX_DIAG
		const unsigned thr = g_thr_max.exchange(0), brk = g_brk_max.exchange(0);
		char moved[512]; int n = 0; moved[0] = 0;
		{
			std::lock_guard<std::mutex> g(g_lock);
			for (size_t i = 0; i < kReportLen; ++i)
				if (g_brange_init && g_bmin[i] != g_bmax[i]) n += snprintf(moved + n, sizeof moved - n, " %zu:%02x-%02x", i, g_bmin[i], g_bmax[i]);
			g_brange_init = false;
		}
		plog(SCS_LOG_TYPE_message, "[hori-apex] status reports=%lu frames=%lu steer_raw=0x%04x thr_max=0x%04x brk_max=0x%04x moved=[%s ]",
			g_reports.load(), g_frames.load(), steer, thr, brk, moved);
#else
		plog(SCS_LOG_TYPE_message, "[hori-apex] status reports=%lu frames=%lu steer_raw=0x%04x",
			g_reports.load(), g_frames.load(), steer);
#endif
	});
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
	CFRunLoopRun();
	CFRunLoopTimerInvalidate(timer); CFRelease(timer);

	IOHIDManagerUnscheduleFromRunLoop(g_manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	IOHIDManagerClose(g_manager, kIOHIDOptionsTypeNone);
	CFRelease(g_manager);
	g_manager = nullptr;
}

float axis_value(const uint8_t *r, scs_u32_t i)
{
	switch (i) {
	case AX_STEER:
		return shape_steer(clampf((static_cast<int>(u16(r, 50)) - 0x8000) / 32768.0f, -1.f, 1.f));
	// 페달 · 아날로그 버튼은 -1(뗌) … +1(끝까지). 게임은 축을 -1 … +1 로 읽어서 0 … 1 을 주면 뗀 상태(0)가
	// 가운데(50%)로 잡혔다 — 브레이크가 늘 반쯤 밟혀 키보드 가속까지 막혔다(2026-10-04 실측).
	case AX_THROTTLE: return u16(r, 52) / 32767.5f - 1.0f;
	case AX_BRAKE:    return u16(r, 54) / 32767.5f - 1.0f;
	case AX_L2:       return u16(r, 24) / 32767.5f - 1.0f;
	case AX_R2:       return u16(r, 26) / 32767.5f - 1.0f;
	}
	return 0.f;
}

bool button_value(const uint8_t *r, scs_u32_t b)
{
	if (b < 8) return (r[4] >> b) & 1;
	if (b < 16) return (r[5] >> (b - 8)) & 1;
	return (r[7] >> (b - 16)) & 1;
}

// 마지막으로 게임에 보낸 값. 바뀐 입력만 이벤트로 보낸다 — 매 프레임 전부 보내면 게임이 이 장치를
// 계속 "조작 중"으로 보고 같은 기능에 묶인 키보드 입력을 덮는다(2026-10-04 실측: 페달 0 이 키보드 가속 · 브레이크를 막았다).
float g_sent_axis[AXIS_COUNT];
bool g_sent_btn[BUTTON_COUNT];
bool g_force_all = true;

SCSAPI_RESULT input_event_callback(scs_input_event_t *const ev, const scs_u32_t flags, const scs_context_t)
{
	if (flags & SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) {
		std::lock_guard<std::mutex> g(g_lock);
		if (!g_have_report) { g_next = INPUT_COUNT; return SCS_RESULT_not_found; }
		memcpy(g_snap, g_report, kReportLen);
		g_next = 0;
		++g_frames;
		if (flags & SCS_INPUT_EVENT_CALLBACK_FLAG_first_after_activation) g_force_all = true;
	}
	for (; g_next < INPUT_COUNT; ++g_next) {
		const scs_u32_t i = g_next;
		if (i < AXIS_COUNT) {
			const float v = axis_value(g_snap, i);
			if (!g_force_all && (v - g_sent_axis[i]) < 0.002f && (g_sent_axis[i] - v) < 0.002f) continue;
			g_sent_axis[i] = v;
			ev->input_index = i;
			ev->value_float.value = v;
		} else {
			const bool v = button_value(g_snap, i - AXIS_COUNT);
			if (!g_force_all && v == g_sent_btn[i - AXIS_COUNT]) continue;
			g_sent_btn[i - AXIS_COUNT] = v;
			ev->input_index = i;
			ev->value_bool.value = v ? 1 : 0;
		}
		++g_next;
		return SCS_RESULT_ok;
	}
	g_force_all = false;
	return SCS_RESULT_not_found;
}

} // namespace

SCSAPI_RESULT scs_input_init(const scs_u32_t version, const scs_input_init_params_t *const params)
{
	if (version != SCS_INPUT_VERSION_1_00) return SCS_RESULT_unsupported;
	const auto *p = static_cast<const scs_input_init_params_v100_t *>(params);
	g_log = p->common.log;
	load_config();

	static const char *axis_names[AXIS_COUNT][2] = {
		{"steer", "Steering"}, {"thr", "Throttle"}, {"brk", "Brake"}, {"l2", "L2 analog"}, {"r2", "R2 analog"},
	};
	for (scs_u32_t i = 0; i < INPUT_COUNT; ++i) {
		if (i < AXIS_COUNT) {
			strcpy(g_names[i], axis_names[i][0]);
			strcpy(g_labels[i], axis_names[i][1]);
			g_inputs[i] = {g_names[i], g_labels[i], SCS_VALUE_TYPE_float};
		} else {
			snprintf(g_names[i], sizeof g_names[i], "b%u", i - AXIS_COUNT + 1);
			snprintf(g_labels[i], sizeof g_labels[i], "Button %u", i - AXIS_COUNT + 1);
			g_inputs[i] = {g_names[i], g_labels[i], SCS_VALUE_TYPE_bool};
		}
	}

	scs_input_device_t d;
	memset(&d, 0, sizeof d);
	d.name = "hori_apex";
	d.display_name = "HORI Racing Wheel Apex SDK";  // SDK: 영문 · 숫자 · _ · 공백 · . 만 (괄호 불가)
	d.type = SCS_INPUT_DEVICE_TYPE_generic;
	d.input_count = INPUT_COUNT;
	d.inputs = g_inputs;
	d.input_event_callback = input_event_callback;
	if (p->register_device(&d) != SCS_RESULT_ok) {
		plog(SCS_LOG_TYPE_error, "[hori-apex] register_device failed");
		return SCS_RESULT_generic_error;
	}

	g_thread = std::thread(hid_thread);
	plog(SCS_LOG_TYPE_message, "[hori-apex] input device registered (game %s)", p->common.game_id);
	return SCS_RESULT_ok;
}

SCSAPI_VOID scs_input_shutdown(void)
{
	for (int i = 0; i < 100 && !g_loop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	if (CFRunLoopRef loop = g_loop.exchange(nullptr)) CFRunLoopStop(loop);
	if (g_thread.joinable()) g_thread.join();
	plog(SCS_LOG_TYPE_message, "[hori-apex] shutdown");
	g_log = nullptr;
}
