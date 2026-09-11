// servers/drivers/console/main.cpp — VGA 텍스트 콘솔 드라이버
// (docs/plan/system-servers-bringup.md §M17, boot-and-drivers.md
// ADR-097/111/164, M56 이후 ADR-230 — 실제 VGA 텍스트 모드 3 초기화).
//
// M1의 디버그 시리얼 콘솔(debug-console.md)과는 완전히 별개다 — 여기서
// 처음으로 진짜 "유저에게 보이는" 콘솔이 생긴다. ADR-164가 확정한
// M17 범위: TTY 1개만 하드코딩(cfgsrv가 아직 없다, M19 이후로 멀티
// TTY 미룸), 렌더링 백엔드는 VGA 텍스트 모드(물리주소 0xB8000,
// 80x25, 셀당 2바이트: 문자+속성) 하나뿐 — 그래픽 프레임버퍼 백엔드는
// 그래픽 드라이버가 실제로 생기는 시점까지 범위 밖. `sys_map_phys`
// (ADR-156)로 매핑한다 — PCI 장치가 아니라 QEMU q35의 레거시 VGA
// 호환 영역에 직접 접근하므로 devmgr 등록이 필요 없다(trusted=1만
// 필요).
//
// ADR-230(실행 중 발견) — 이 커널은 실제 BIOS(INT 10h)나 GRUB을
// 거치지 않고 qboot.rom(최소 PVH 스텁)으로 곧바로 부팅한다 — VGA
// 카드를 "80x25 텍스트 모드"로 세팅해 주는 존재가 원래부터 아무도
// 없었다. 지금까지는 이 드라이버가 0xB8000에 글자를 쓰기만 하면
// 화면에 보인다고 가정했지만, 실제로는 그 물리 메모리에 글자를
// 써도 VGA 카드 자신은(레지스터가 전부 리셋 상태 그대로라) 어떤
// 모드도 활성화된 적이 없다고 본다 — QEMU가 화면에
// "Guest has not initialized the display (yet)."만 띄우는 이유다.
// 그래서 이 드라이버가 직접, 실제 VGA BIOS의 INT 10h AH=00h AL=03h
// (80x25 16색 텍스트, "모드 3")가 하는 것과 같은 레지스터 프로그래밍
// 시퀀스를 포트 I/O로 재현한다(sys_io_activate, ps2 드라이버가 이미
// 쓰는 것과 같은 패턴) — Miscellaneous Output+Sequencer+CRTC+
// Graphics Controller+Attribute Controller 순서(FreeVGA 문서/여러
// 호비스트 OS의 "80x25 text" 레지스터 표와 동일한 값).
#include <mc/syscall.h>

namespace kernsrv::drivers::console {

namespace {

constexpr uint32_t k_own_endpoint_handle = 1;

constexpr uint32_t k_op_print = 1;
constexpr uint64_t k_status_ok = 0;

constexpr uint64_t k_vga_phys = 0x00000000000B8000ull;
constexpr uint32_t k_cols = 80;
constexpr uint32_t k_rows = 25;
constexpr uint8_t k_default_attr = 0x07;  // 밝은 회색 글자, 검은 배경.
constexpr uint8_t k_ascii_backspace = 0x08;

// ADR-230 — VGA 레지스터 포트(레거시 컬러 어댑터 레인지 전부를
// 한 번에 활성화한다, 0x3B0~0x3DF — 모노크롬 레인지까지 포함해
// Input Status 1(0x3BA/0x3DA)을 어느 쪽으로 읽어도 안전하게 한다).
constexpr uint16_t k_io_activate_base = 0x3B0;
constexpr uint16_t k_io_activate_count = 0x30;  // 0x3B0..0x3DF.

constexpr uint16_t k_port_misc_output = 0x3C2;
constexpr uint16_t k_port_seq_index = 0x3C4;
constexpr uint16_t k_port_seq_data = 0x3C5;
constexpr uint16_t k_port_crtc_index = 0x3D4;
constexpr uint16_t k_port_crtc_data = 0x3D5;
constexpr uint16_t k_port_gc_index = 0x3CE;
constexpr uint16_t k_port_gc_data = 0x3CF;
constexpr uint16_t k_port_ac_index_data = 0x3C0;
constexpr uint16_t k_port_input_status1 = 0x3DA;
constexpr uint16_t k_port_dac_index = 0x3C8;
constexpr uint16_t k_port_dac_data = 0x3C9;

constexpr uint64_t k_font_phys = 0x00000000000A0000ull;
constexpr uint64_t k_font_window_size = 8192;  // 256글자 * 32바이트 슬롯.
constexpr uint32_t k_glyph_rows = 8;            // 8x8 폰트 — 아래 표 참고.

// ADR-230 실행 중 발견 — 글자+속성을 0xB8000에 아무리 정확히 써도,
// VGA 카드가 화면에 그릴 실제 글꼴 비트맵(플레인 2, 이 커널 안
// 어디에도 없었다 — 진짜 BIOS만 갖고 있던 것)이 없으면 각 셀이
// 배경색으로만 통째로 칠해진다(글자 모양이 아예 안 보임 — 텍스트
// 모드 자체는 제대로 켜졌다는 것을 720x400 해상도로 먼저 확인한
// 뒤에야, "글자가 안 보인다"는 이 문제가 별개의 원인이라는 걸
// 좁혀냈다). 이 화면에 실제로 나타나는 문자는 servers/login
// (콘솔에 직접 쓰는 유일한 소비자)이 쓰는 것뿐이라("minicore
// login: "/"Password: "/"Login successful"/"Login incorrect") 그
// 안에 실제로 쓰이는 20개 글자만 손으로 그린 5x7(8x8 셀 안에 여백
// 포함) 비트맵으로 담는다 — 전체 256자 폰트 ROM을 재현할 필요가
// 없다. 표에 없는 문자는 빈 칸(공백)으로 보인다.
struct glyph_entry {
    char ch;
    uint8_t rows[k_glyph_rows];
};

constexpr glyph_entry k_glyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {':', {0x00, 0x00, 0x60, 0x60, 0x00, 0x60, 0x60, 0x00}},
    {'P', {0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80, 0x00}},
    {'L', {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8, 0x00}},
    {'a', {0x00, 0x70, 0x88, 0x08, 0x78, 0x88, 0x78, 0x00}},
    {'c', {0x00, 0x70, 0x80, 0x80, 0x80, 0x80, 0x70, 0x00}},
    {'d', {0x08, 0x08, 0x70, 0x88, 0x88, 0x88, 0x78, 0x00}},
    {'e', {0x00, 0x70, 0x88, 0xF8, 0x80, 0x80, 0x78, 0x00}},
    {'f', {0x30, 0x40, 0x40, 0xF0, 0x40, 0x40, 0x40, 0x00}},
    {'g', {0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x70, 0x00}},
    {'i', {0x40, 0x00, 0xC0, 0x40, 0x40, 0x40, 0xE0, 0x00}},
    {'l', {0xC0, 0x40, 0x40, 0x40, 0x40, 0x40, 0xE0, 0x00}},
    {'m', {0x00, 0xD8, 0xA8, 0xA8, 0xA8, 0xA8, 0x88, 0x00}},
    {'n', {0x00, 0xD0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x00}},
    {'o', {0x00, 0x70, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00}},
    {'r', {0x00, 0xB0, 0xC0, 0x80, 0x80, 0x80, 0x80, 0x00}},
    {'s', {0x00, 0x78, 0x80, 0x70, 0x08, 0x08, 0xF0, 0x00}},
    {'t', {0x40, 0x40, 0xF0, 0x40, 0x40, 0x40, 0x30, 0x00}},
    {'u', {0x00, 0x88, 0x88, 0x88, 0x88, 0x88, 0x78, 0x00}},
    {'w', {0x00, 0x88, 0x88, 0xA8, 0xA8, 0xD8, 0x88, 0x00}},
};
constexpr uint32_t k_glyph_count = sizeof(k_glyphs) / sizeof(k_glyphs[0]);

uint16_t* g_vga = nullptr;
uint8_t* g_font_window = nullptr;
uint32_t g_cursor_row = 0;
uint32_t g_cursor_col = 0;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

uint8_t in8(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
void out8(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }

uint64_t cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}
void debug_log(const char* msg) {
    do_syscall(MC_SYSCALL_DEBUG_LOG, reinterpret_cast<uint64_t>(msg), cstr_len(msg), 0);
}

// 한 줄을 위로 밀어 올린다(마지막 줄이 25번째를 넘어갈 때) — 문자
// 셀을 통째로 옮기고 마지막 줄은 공백으로 지운다.
void scroll_up() {
    for (uint32_t row = 1; row < k_rows; ++row) {
        for (uint32_t col = 0; col < k_cols; ++col) {
            g_vga[(row - 1) * k_cols + col] = g_vga[row * k_cols + col];
        }
    }
    for (uint32_t col = 0; col < k_cols; ++col) {
        g_vga[(k_rows - 1) * k_cols + col] = static_cast<uint16_t>(' ') | (k_default_attr << 8);
    }
}

void newline() {
    g_cursor_col = 0;
    ++g_cursor_row;
    if (g_cursor_row >= k_rows) {
        scroll_up();
        g_cursor_row = k_rows - 1;
    }
}

// fs-protocol류 서버들과 같은 정신(regs[]만으로 충분히 짧은
// 프롬프트/메시지를 옮긴다, ADR-164 §결정3) — 한 바이트씩 VGA
// 버퍼에 쓴다. '\n'은 다음 줄, 0x08(backspace)은 커서를 한 칸
// 물리고 그 자리를 지운다(로그인 프롬프트가 사용자 입력을 지울 때
// 그대로 보낸다).
void print_char(char c) {
    if (c == '\n') {
        newline();
        return;
    }
    if (static_cast<uint8_t>(c) == k_ascii_backspace) {
        if (g_cursor_col > 0) {
            --g_cursor_col;
            g_vga[g_cursor_row * k_cols + g_cursor_col] =
                static_cast<uint16_t>(' ') | (k_default_attr << 8);
        }
        return;
    }
    g_vga[g_cursor_row * k_cols + g_cursor_col] =
        static_cast<uint16_t>(static_cast<uint8_t>(c)) | (k_default_attr << 8);
    ++g_cursor_col;
    if (g_cursor_col >= k_cols) {
        newline();
    }
}

// ADR-230 — 실제 VGA BIOS의 INT 10h AH=00h AL=03h("모드 3", 80x25
// 16색 텍스트)와 정확히 같은 레지스터 값(FreeVGA 문서/여러 호비스트
// OS 커널이 쓰는 것과 동일한 표)을 그대로 포트 I/O로 재현한다. 이
// 레지스터를 만지지 않으면 QEMU(그리고 실제 VGA 하드웨어도 마찬가지
// 원리)는 0xB8000에 쓰인 글자를 렌더링할 근거(어떤 모드/타이밍으로
// 화면을 그릴지)가 전혀 없다.
void set_text_mode_3() {
    // Miscellaneous Output Register — 컬러 에뮬레이션(bit0=1)+
    // 25MHz 클록(bit2..3=00)+수평/수직 동기 극성.
    out8(k_port_misc_output, 0x67);

    // Sequencer: [0]리셋 해제 [1]클럭 모드(문자당 9도트, screen off
    // 아님) [2]맵 마스크(평면 0/1 — 텍스트 모드 폰트/속성) [3]문자
    // 맵 선택 [4]메모리 모드(확장 메모리, odd/even 활성).
    static const uint8_t k_seq[5] = {0x03, 0x00, 0x03, 0x00, 0x02};
    for (uint8_t i = 0; i < 5; ++i) {
        out8(k_port_seq_index, i);
        out8(k_port_seq_data, k_seq[i]);
    }

    // CRTC(25개, 인덱스 0~24) — 80x25 텍스트의 표준 타이밍/커서/
    // 오프셋 값. 인덱스 17(Vertical Retrace End)의 bit7이 0~7번
    // 레지스터의 "쓰기 보호"라 마지막에 세팅해야 한다(그대로 순서
    // 0..24로 채워도 0~7은 이미 그 전에 다 써 뒤이므로 안전하다).
    static const uint8_t k_crtc[25] = {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
                                        0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
                                        0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
                                        0xFF};
    for (uint8_t i = 0; i < 25; ++i) {
        out8(k_port_crtc_index, i);
        out8(k_port_crtc_data, k_crtc[i]);
    }

    // Graphics Controller(9개) — 텍스트 모드(Miscellaneous=0x0E:
    // Odd/Even+텍스트), 그 외는 텍스트 모드에서 안 쓰이는 그래픽
    // 전용 필드라 리셋값 그대로.
    static const uint8_t k_gc[9] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF};
    for (uint8_t i = 0; i < 9; ++i) {
        out8(k_port_gc_index, i);
        out8(k_port_gc_data, k_gc[i]);
    }

    // Attribute Controller(21개) — 팔레트 0~15는 그대로(인덱스=값,
    // EGA/VGA 표준 16색 팔레트 매핑), 모드 컨트롤(0x14 위치, 실제
    // 인덱스 0x10)은 텍스트 모드+블링크 활성, 그 외 오버스캔/색상
    // 플레인 마스크/픽셀 패닝은 표준값. Input Status 1을 먼저 읽어
    // index/data 플립플롭을 "index를 기다리는 상태"로 되돌린 뒤
    // 인덱스→값 순으로 쓴다(AC는 같은 포트로 인덱스/값이 번갈아
    // 오는 유일한 레지스터군).
    static const uint8_t k_ac[21] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
                                      0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
                                      0x0C, 0x00, 0x0F, 0x08, 0x00};
    for (uint8_t i = 0; i < 21; ++i) {
        (void)in8(k_port_input_status1);
        out8(k_port_ac_index_data, i);
        out8(k_port_ac_index_data, k_ac[i]);
    }
    // 마지막으로 index bit5(Palette Address Source)를 켜서 실제
    // 화면 출력을 다시 활성화한다 — 인덱스 레지스터에 쓸 때마다
    // 이 비트가 잠시 꺼지므로, 다 쓴 뒤 반드시 한 번 더 켜 줘야
    // 화면이 "blank" 상태로 멈추지 않는다.
    (void)in8(k_port_input_status1);
    out8(k_port_ac_index_data, 0x20);

    // ADR-230 실행 중 발견 — Attribute Controller는 속성 니블(0~15)을
    // "DAC 팔레트 인덱스"로 매핑만 할 뿐, 그 인덱스가 실제로 어떤
    // RGB 색인지는 DAC(0x3C8/0x3C9, 각 채널 6비트)가 정한다. 진짜
    // BIOS는 모드 세팅에 항상 표준 팔레트도 함께 로드하는데, 이
    // 커널은 그 BIOS가 없어 DAC가 리셋 상태(전부 검정)로 남아
    // 있었다 — 텍스트는 정확한 위치에 그려지는데도 글자/배경이
    // 전부 검정으로만 보이는 것으로 처음 드러났다(720x400 해상도
    // 자체는 정확히 잡혔으니 모드 세팅 자체는 성공했다는 뜻이었다).
    // EGA/VGA 표준 16색 팔레트(각 채널 0~63)를 그대로 싣는다.
    static const uint8_t k_palette16[16][3] = {
        {0, 0, 0},    {0, 0, 42},   {0, 42, 0},   {0, 42, 42},
        {42, 0, 0},   {42, 0, 42},  {42, 21, 0},  {42, 42, 42},
        {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
        {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63},
    };
    out8(k_port_dac_index, 0);
    for (uint8_t i = 0; i < 16; ++i) {
        out8(k_port_dac_data, k_palette16[i][0]);
        out8(k_port_dac_data, k_palette16[i][1]);
        out8(k_port_dac_data, k_palette16[i][2]);
    }
}

// ADR-230 실행 중 발견 — 텍스트 모드가 실제로 "80x25 모드"로 켜지고
// DAC 팔레트까지 정확해도(빨간 배경 테스트 바가 정확히 그려짐),
// 글자가 전혀 안 보이는(균일한 배경색 블록만 보임) 문제가 남았다.
// 원인: 각 문자 셀의 실제 모양(비트맵)은 VRAM "플레인 2"(문자
// 생성기 플레인)에서 읽히는데, 진짜 BIOS가 모드 세팅 때 항상 함께
// 로드해 주던 그 폰트 데이터가 이 커널에는 아무도 넣어 준 적이
// 없어 전부 0(빈 칸)이었다. VGA는 CPU가 플레인 2에 직접 쓸 수 있는
// 경로를 텍스트 모드 레지스터로는 안 주므로(Map Mask/Memory Mode/
// Graphics Mode가 텍스트 렌더링 경로에 최적화돼 있다), 표준 트릭대로
// 잠시 "그래픽 모드처럼" 레지스터를 바꿔 0xA0000을 플레인 2로 직접
// 선형 매핑한 뒤 폰트를 쓰고, 다시 텍스트 모드 값으로 되돌린다.
void load_font() {
    // ADR-230 실행 중 발견 — 처음엔 SEQ 리셋 없이 SEQ2/SEQ4/GR4/5/6만
    // 바꿔 봤는데, 그 상태에서 쓴 값을 "같은 상태에서" 즉시 읽어보면
    // 정확히 맞았는데도(자기 자신과의 자기 일치일 뿐이었다) 실제
    // 화면에는 전혀 그려지지 않았다 — Sequencer의 Memory Mode(SEQ4)
    // 변경은 실제 VGA 하드웨어(및 QEMU 에뮬레이션)에서 Synchronous
    // Reset(SEQ0=0x01)으로 시퀀서를 잠깐 멈춘 상태에서 적용해야
    // 안전하게 반영된다는 것을 뒤늦게 확인했다(FreeVGA류 폰트 로드
    // 표준 절차) — 그냥 값만 써도 포트 자체는 그 값을 저장하지만,
    // 문자 생성기가 실제로 그 설정으로 플레인 2를 읽으려면 이 리셋
    // 시퀀스가 필요하다. SEQ4 값도 0x06이 아니라 0x07(Chain-4는
    // 여전히 0, 대신 하위 비트까지 표준 절차와 동일하게)이어야 한다.
    out8(k_port_seq_index, 0);
    out8(k_port_seq_data, 0x01);  // synchronous reset — 시퀀서 정지.

    out8(k_port_seq_index, 2);
    uint8_t saved_seq2 = in8(k_port_seq_data);
    out8(k_port_seq_data, 0x04);  // Map Mask — 플레인 2만 쓰기.

    out8(k_port_seq_index, 4);
    uint8_t saved_seq4 = in8(k_port_seq_data);
    out8(k_port_seq_data, 0x07);  // Memory Mode — 선형 주소, 확장 메모리.

    out8(k_port_seq_index, 0);
    out8(k_port_seq_data, 0x03);  // 시퀀서 재시작.

    out8(k_port_gc_index, 4);
    uint8_t saved_gc4 = in8(k_port_gc_data);
    out8(k_port_gc_data, 0x02);  // Read Map Select — 플레인 2.

    out8(k_port_gc_index, 5);
    uint8_t saved_gc5 = in8(k_port_gc_data);
    out8(k_port_gc_data, 0x00);  // Graphics Mode — 쓰기 모드 0.

    out8(k_port_gc_index, 6);
    uint8_t saved_gc6 = in8(k_port_gc_data);
    out8(k_port_gc_data, 0x00);  // Miscellaneous — 0xA0000-0xBFFFF 128K 창.

    // 256글자 슬롯 전부를 먼저 0(빈 칸)으로 지운 뒤, 실제로 필요한
    // ~20글자만 손으로 그린 비트맵으로 덮어쓴다 — 표에 없는 문자는
    // 화면에 빈 칸으로만 보인다(login이 실제로 안 쓰는 문자라
    // 문제되지 않는다). CRTC Maximum Scan Line(=0x0F, 16줄/문자)에
    // 맞춰 8행 글꼴의 각 행을 스캔라인 두 줄에 그대로 복제한다.
    for (uint64_t i = 0; i < k_font_window_size; ++i) {
        g_font_window[i] = 0;
    }
    for (uint32_t g = 0; g < k_glyph_count; ++g) {
        uint32_t slot = static_cast<uint8_t>(k_glyphs[g].ch) * 32u;
        for (uint32_t row = 0; row < k_glyph_rows; ++row) {
            uint8_t bits = k_glyphs[g].rows[row];
            g_font_window[slot + row * 2] = bits;
            g_font_window[slot + row * 2 + 1] = bits;
        }
    }

    // 원래 값으로 복원 — 여기도 SEQ 리셋을 감싸야 한다(위와 같은 이유).
    out8(k_port_seq_index, 0);
    out8(k_port_seq_data, 0x01);

    out8(k_port_seq_index, 2);
    out8(k_port_seq_data, saved_seq2);
    out8(k_port_seq_index, 4);
    out8(k_port_seq_data, saved_seq4);

    out8(k_port_seq_index, 0);
    out8(k_port_seq_data, 0x03);

    out8(k_port_gc_index, 4);
    out8(k_port_gc_data, saved_gc4);
    out8(k_port_gc_index, 5);
    out8(k_port_gc_data, saved_gc5);
    out8(k_port_gc_index, 6);
    out8(k_port_gc_data, saved_gc6);
}

void handle_print(const mc_message& in, mc_message& out) {
    uint64_t length = in.regs[0];
    if (length > 3 * sizeof(uint64_t)) {
        length = 3 * sizeof(uint64_t);  // regs[1..3] = 24바이트 상한.
    }
    const auto* text_bytes = reinterpret_cast<const char*>(&in.regs[1]);
    for (uint64_t i = 0; i < length; ++i) {
        print_char(text_bytes[i]);
    }
    out.regs[1] = k_status_ok;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    // ADR-230 — 0xB8000에 글자를 쓰기 전에 VGA 카드 자신을 실제로
    // "80x25 텍스트 모드"로 세팅한다(이 커널엔 BIOS/GRUB이 없어
    // 아무도 대신 해 주지 않는다). 포트 I/O 권한 활성화가 실패해도
    // (트러스트 모델이 바뀌는 등) 화면 렌더링 없이 메모리 쓰기만
    // 계속하는 기존 동작으로 조용히 물러난다 — 디버그 시리얼 콘솔은
    // 이 드라이버와 무관하게 항상 동작하므로 부팅 자체를 막을 이유가
    // 없다.
    uint64_t io_err =
        do_syscall(MC_SYSCALL_IO_ACTIVATE, k_io_activate_base, k_io_activate_count, 0);

    if (io_err == 0) {
        set_text_mode_3();

        // ADR-230 실행 중 발견 — sys_map_phys(kernel/arch/x86_64/
        // process_ops.cpp::map_phys, ADR-007/038/039)는 프로세스당
        // 고정 가상주소 슬롯 하나(k_mmio_user_vaddr)만 재사용한다 —
        // 두 번째 map_phys 호출은 첫 번째 매핑을 조용히 다른 물리
        // 주소로 덮어써 버린다(같은 가상주소, 다른 물리 페이지).
        // 처음엔 0xB8000(VGA 텍스트 버퍼)을 먼저 매핑해 두고 나중에
        // 0xA0000(폰트 로드 창)을 매핑했다가, g_vga가 조용히
        // 0xA0000을 가리키게 바뀌어 이후 모든 화면 쓰기가 실제
        // VGA 텍스트 버퍼에는 전혀 도달하지 못하는 버그를 겪었다
        // (모드 세팅/DAC/글꼴 쓰기 자체는 전부 정상이었는데도 화면이
        // 완전히 검게 남아 원인 파악이 오래 걸렸다). 폰트 로드는
        // 부팅 시 한 번만 쓰고 버리는 임시 매핑이라, 반드시 먼저
        // 끝내고 **그 다음** 0xB8000을 매핑해야 마지막 매핑(=계속
        // 살아있는 g_vga)이 올바른 주소를 가리킨다.
        mc_map_phys_request font_req{};
        font_req.phys_addr = k_font_phys;
        font_req.size = k_font_window_size;
        uint64_t font_err =
            do_syscall(MC_SYSCALL_MAP_PHYS, reinterpret_cast<uint64_t>(&font_req), 0, 0);
        if (font_err == 0) {
            g_font_window = reinterpret_cast<uint8_t*>(font_req.out_virt_addr);
            load_font();
        } else {
            debug_log("[console] font map_phys failed — 글자 모양 없이 진행\n");
        }
    } else {
        debug_log("[console] io_activate failed — VGA 텍스트 모드 세팅 생략\n");
    }

    // 폰트 로드가 끝난 뒤(위 블록에서 이미 끝났다) 마지막으로 0xB8000
    // 을 매핑한다 — 이 매핑이 g_vga로 남아 드라이버 나머지 생애주기
    // 동안 유지된다(같은 고정 슬롯을 다시 덮어쓰는 마지막 호출이라
    // 항상 유효하다).
    mc_map_phys_request req{};
    req.phys_addr = k_vga_phys;
    req.size = k_cols * k_rows * 2;
    uint64_t err = do_syscall(MC_SYSCALL_MAP_PHYS, reinterpret_cast<uint64_t>(&req), 0, 0);
    if (err != 0) {
        debug_log("[console] map_phys failed\n");
        do_syscall(MC_SYSCALL_THREAD_EXIT, 0, 0, 0);
    }
    g_vga = reinterpret_cast<uint16_t*>(req.out_virt_addr);

    // 화면을 지우고 왼쪽 위로 커서를 둔다 — 부팅 로그(디버그 시리얼
    // 콘솔)와 뒤섞이지 않는 깨끗한 시작.
    for (uint32_t i = 0; i < k_cols * k_rows; ++i) {
        g_vga[i] = static_cast<uint16_t>(' ') | (k_default_attr << 8);
    }
    g_cursor_row = 0;
    g_cursor_col = 0;

    debug_log("[console] vga init ok=1\n");

    for (;;) {
        mc_message in{};
        uint64_t recv_err = do_syscall(MC_SYSCALL_IPC_RECV, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        mc_message out{};
        if (recv_err == 0) {
            out.label = in.label;
            if (in.label == k_op_print) {
                handle_print(in, out);
            }
        }
        do_syscall(MC_SYSCALL_IPC_REPLY, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}

}  // namespace kernsrv::drivers::console
