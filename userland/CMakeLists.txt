# 포팅된 셸·coreutils 등 (ADR-005). M20(ADR-170)이 한동안 이 자리를
# minicore 전용 최소 셸(userland/shell)로 채웠으나, M53(ADR-224)이
# 실제 포팅된 셸(userland/msh, 아래)로 완전히 대체하면서 제거했다 —
# userland/shell은 더 이상 이 저장소에 없다.

# real-libc-syscall-layer.md §M28 — musl 진짜 시작 경로 검증용 최초
# 테스트 프로그램(기존 셸/서버와 무관).
add_subdirectory(musl-hello)

# real-libc-syscall-layer.md §M32 — musl-hello가 fork()+execve()로
# 실행 이미지를 바꿔 넣을 "다른" 바이너리.
add_subdirectory(musl-exec-target)

# user-service-manager.md §M40 — svcmgr가 스폰할 하드코딩된 데모 유닛.
add_subdirectory(svcmgr-demo-unit)

# user-service-manager.md §M42 — svcmgr 컨트롤 프로토콜 검증용
# 최소 테스트 프로그램.
add_subdirectory(svcmgr-ctl-test)

# user-service-manager.md §M43 — 계정별 유저 서비스 위임(ADR-218)
# grant 검증용 최소 테스트 프로그램.
add_subdirectory(user-service-delegate-test)

# docs/plan/musl-userland-porting.md §M51 — 익명 파이프+dup2() 검증용
# 실제 musl 프로그램.
add_subdirectory(pipe-test)

# docs/plan/musl-userland-porting.md §M52(ADR-221) — BusyBox 도입을
# 철회하고 이 저장소 안에서 직접 작성한 최소 셸(msh)+coreutils
# (echo/ls/cat). msh가 이 셋을 별도 실행파일로 진짜 fork()+execve()
# 한다(빌트인 아님). M53(ADR-224)부터 msh는 더 이상 부팅 시점
# 서비스가 아니다 — servers/procsrv가 로그인 성공 시 자신의 컴파일
# 시점 데이터(bin2c로 심은 바이트)로 직접 스폰한다.
add_subdirectory(echo)
add_subdirectory(ls)
add_subdirectory(cat)
add_subdirectory(msh)

# docs/plan/musl-userland-porting.md §M55(ADR-227) — msh의 job
# control 자기테스트 대상(sched_yield() 루프, SIGINT로 종료돼야
# 정상). msh/echo/ls/cat과 같은 방식으로 procsrv가 자신의 컴파일
# 시점 데이터로 직접 스폰한다.
add_subdirectory(loop-test)
