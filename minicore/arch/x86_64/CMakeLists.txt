# x86-64 부팅 stub/bootstrap (SP-8B6B8D25 §3.0 - "부팅 stub 및
# bootstrap, 초기 환경 구성 등 코드만 포함한다"). 재사용 가능한
# 아키텍처 라이브러리 코드(ISR 스텁, 포트 I/O 등)는 여기가 아니라
# minicore/libs/x86_64에 있다.
#
# OBJECT 라이브러리인 이유: pvh_start는 링커 스크립트 ENTRY/PVH ELF
# Note로만 참조되고 다른 오브젝트가 그 심볼을 직접 호출하지 않는다 -
# 일반 정적 아카이브(.a)면 "아무도 안 쓰는 심볼"로 보여 링크에서
# 통째로 빠질 수 있다.
add_library(minicore_arch_x86_64 OBJECT
    boot.S
    ap_trampoline.S
)
