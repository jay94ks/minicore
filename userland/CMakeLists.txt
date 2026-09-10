# 포팅된 셸·coreutils 등 (ADR-005). 실제 서드파티 포팅 대상은 아직
# 확정되지 않았다 — M20(docs/design/foundations.md ADR-170)이 그 자리에
# minicore 전용 최소 셸(ADR-049 대체 구현)을 먼저 채운다.
# TODO: third_party/<도구>(ADR-022) 확정 시 add_subdirectory(<도구>) 추가.
add_subdirectory(shell)

# real-libc-syscall-layer.md §M28 — musl 진짜 시작 경로 검증용 최초
# 테스트 프로그램(기존 셸/서버와 무관).
add_subdirectory(musl-hello)
