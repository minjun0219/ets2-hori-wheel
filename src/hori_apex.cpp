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

inline unsigned u16(const uint8_t *r, size_t at) { return r[at] | (r[at + 1] << 8); }

void on_report(void *, IOReturn, void *, IOHIDReportType, uint32_t, uint8_t *report, CFIndex len)
{
	if (len < static_cast<CFIndex>(kReportLen)) return;
	std::lock_guard<std::mutex> g(g_lock);
	memcpy(g_report, report, kReportLen);
	g_have_report = true;
	++g_reports;
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
		plog(SCS_LOG_TYPE_message, "[hori-apex] status reports=%lu frames=%lu steer_raw=0x%04x", g_reports.load(), g_frames.load(), steer);
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
	case AX_STEER: {
		const float s = (static_cast<int>(u16(r, 50)) - 0x8000) / 32768.0f;
		return s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
	}
	case AX_THROTTLE: return u16(r, 52) / 65535.0f;
	case AX_BRAKE:    return u16(r, 54) / 65535.0f;
	case AX_L2:       return u16(r, 24) / 65535.0f;
	case AX_R2:       return u16(r, 26) / 65535.0f;
	}
	return 0.f;
}

bool button_value(const uint8_t *r, scs_u32_t b)
{
	if (b < 8) return (r[4] >> b) & 1;
	if (b < 16) return (r[5] >> (b - 8)) & 1;
	return (r[7] >> (b - 16)) & 1;
}

SCSAPI_RESULT input_event_callback(scs_input_event_t *const ev, const scs_u32_t flags, const scs_context_t)
{
	if (flags & SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) {
		std::lock_guard<std::mutex> g(g_lock);
		if (!g_have_report) { g_next = INPUT_COUNT; return SCS_RESULT_not_found; }
		memcpy(g_snap, g_report, kReportLen);
		g_next = 0;
		++g_frames;
	}
	if (g_next >= INPUT_COUNT) return SCS_RESULT_not_found;

	ev->input_index = g_next;
	if (g_next < AXIS_COUNT) ev->value_float.value = axis_value(g_snap, g_next);
	else ev->value_bool.value = button_value(g_snap, g_next - AXIS_COUNT) ? 1 : 0;
	++g_next;
	return SCS_RESULT_ok;
}

} // namespace

SCSAPI_RESULT scs_input_init(const scs_u32_t version, const scs_input_init_params_t *const params)
{
	if (version != SCS_INPUT_VERSION_1_00) return SCS_RESULT_unsupported;
	const auto *p = static_cast<const scs_input_init_params_v100_t *>(params);
	g_log = p->common.log;

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
