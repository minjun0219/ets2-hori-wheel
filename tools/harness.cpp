// 게임 없이 플러그인을 시험하는 하네스: dlopen → scs_input_init(가짜 params) → 매 프레임 콜백 → 값 출력.
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include "scssdk_input.h"

static scs_input_event_callback_t cb;
static const scs_input_device_input_t *inputs;
static scs_u32_t count;

static SCSAPI_VOID log_fn(const scs_log_type_t, const scs_string_t m) { printf("LOG %s\n", m); }
static bool ok_display(const char *s) { for (; *s; ++s) if (!(isalnum((unsigned char)*s) || *s == '_' || *s == ' ' || *s == '.')) return false; return true; }
static bool ok_name(const char *s) { for (; *s; ++s) if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '_')) return false; return true; }
static SCSAPI_RESULT reg(const scs_input_device_t *d)
{
	// 게임과 같은 이름 규칙 검사(scssdk_input_device.h) — 실패하면 게임도 register_device 를 거부한다
	bool good = ok_name(d->name) && ok_display(d->display_name);
	for (scs_u32_t i = 0; i < d->input_count; ++i) good = good && ok_name(d->inputs[i].name) && ok_display(d->inputs[i].display_name);
	if (!good) { printf("REJECT invalid name/display_name\n"); return SCS_RESULT_invalid_parameter; }
	cb = d->input_event_callback; inputs = d->inputs; count = d->input_count;
	printf("REGISTER %s (%s) inputs=%u\n", d->name, d->display_name, count);
	return SCS_RESULT_ok;
}

int main(int argc, char **argv)
{
	void *h = dlopen(argv[1], RTLD_NOW);
	if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
	auto init = (decltype(&scs_input_init))dlsym(h, "scs_input_init");
	auto fini = (decltype(&scs_input_shutdown))dlsym(h, "scs_input_shutdown");
	scs_input_init_params_v100_t p; memset(&p, 0, sizeof p);
	p.common.game_name = "test"; p.common.game_id = "eut2"; p.common.log = log_fn;
	p.register_device = reg;
	printf("init=%d\n", init(SCS_INPUT_VERSION_1_00, &p));
	const int seconds = argc > 2 ? atoi(argv[2]) : 10;
	for (int f = 0; f < seconds * 10; ++f) {
		usleep(argc > 3 ? atoi(argv[3]) : 100000);
		scs_input_event_t ev; scs_u32_t flags = SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame;
		char line[512]; int n = 0, events = 0; char json[2048]; int jn = 0; json[0] = 0;
		while (cb && cb(&ev, flags, nullptr) == SCS_RESULT_ok) {
			flags = 0; ++events;
			const auto &in = inputs[ev.input_index];
			jn += snprintf(json + jn, sizeof json - jn, "%s\"%s\":%g", jn ? "," : "", in.name, in.value_type == SCS_VALUE_TYPE_float ? ev.value_float.value : (double)ev.value_bool.value);
			if (in.value_type == SCS_VALUE_TYPE_float) n += snprintf(line + n, sizeof line - n, "%s=%+.3f ", in.name, ev.value_float.value);
			else if (ev.value_bool.value) n += snprintf(line + n, sizeof line - n, "%s ", in.name);
		}
		if (f % 5 == 0) printf("frame %3d events=%d %s\n", f, events, events ? line : "(no report yet)");
		if (const char *out = getenv("STATE_JSON")) {
			// 시각화용: 마지막 프레임 값을 JSON 으로 (원자적 교체)
			char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", out);
			if (FILE *fp = fopen(tmp, "w")) { fprintf(fp, "{%s}", json); fclose(fp); rename(tmp, out); }
		}
	}
	fini();
	printf("shutdown ok\n");
}
