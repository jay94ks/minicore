// userlandtest: 유저랜드 툴체인/링커 스크립트(0x400000 고정 베이스,
// PT_LOAD 세그먼트 분리)를 실제로 검증하기 위한 최소 실행 파일 -
// 아직 libmc(userland/libs/libmc, syscall 래퍼)도 ring3 진입
// (PN-124C105B)도 없어 syscall은 전혀 쓰지 않는다. 지금은 "이
// 툴체인으로 만든 ELF64가 minicore/libs/libelf가 요구하는 형태
// (ET_EXEC, EM_X86_64, PT_LOAD 세그먼트들)로 정확히 나오는지"만
// 확인하는 용도 - PN-16CA347D 다음 증분(libmc/minicore/init)이
// 이 자리를 이어받아 실제 syscall을 쓰는 프로그램으로 발전시킨다.
extern "C" void _start() {
    for (;;) {
    }
}
