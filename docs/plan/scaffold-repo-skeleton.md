# 실행 계획: 저장소 스캐폴딩

**대상 설계**: [repo-layout.md](../design/repo-layout.md)
**관련 결정**: ADR-019~022

## 목적

`docs/design/repo-layout.md`에 정의된 디렉토리 트리와 CMake 골격을 실제 파일로
만든다. 이 단계는 **컴파일 가능한 커널 코드를 작성하는 것이 아니라**, 이후
커널·서버·포팅 작업이 들어갈 자리와 빌드 진입점을 준비하는 것이다.

## 범위 (이번 실행에 포함)

1. 디렉토리 구조 생성 (`kernel/`, `libk/`, `init/initrun/`, `servers/*`,
   `libc/`, `userland/`, `third_party/`, `tools/`, `toolchain/`).
2. `toolchain/common.cmake` — ADR-010 언어 정책(`-fno-exceptions -fno-rtti`,
   freestanding 플래그) 반영.
3. `toolchain/{x86_64,aarch64}-{clang,gcc}.cmake` 4종 — ADR-020 툴체인 분기.
4. 최상위 `CMakeLists.txt` — `MINICORE_ARCH` 옵션, 서브디렉토리 오케스트레이션
   골격 (아직 실제 소스가 없으므로 각 서브프로젝트는 최소 placeholder).
5. `CMakePresets.json` — 4개 프리셋 (x86_64-clang / x86_64-gcc / aarch64-clang / aarch64-gcc).
6. 각 서브디렉토리에 최소 `CMakeLists.txt` placeholder (빈 인터페이스 타깃 또는 주석).
7. `.gitmodules` — 빈 상태로 생성(항목은 실제 포팅 착수 시 추가).
8. `tools/apply-patches.*`, `tools/run-qemu.*`, `tools/mkinitrd.*` — 스텁 스크립트
   (실제 로직은 이후 구현, 지금은 사용법 주석과 TODO만).

## 범위 밖 (하지 않음)

- 실제 부팅 코드, IPC 구현, 스케줄러 등 커널 로직 작성 — 별도 계획으로 분리.
- 특정 libc/셸/coreutils 프로젝트 선정 및 submodule 추가.
- CI 파이프라인 설정.

## 검증 방법

- `cmake --preset x86_64-clang -S . -B build/x86_64-clang` 구성이 오류 없이
  통과하는지 확인 (실제 컴파일 산출물은 없어도 됨 — 골격 유효성만 확인).
- 디렉토리 트리가 `repo-layout.md`와 1:1로 대응하는지 육안 대조.

## 완료 후

실행이 끝나면 결과를 `docs/done/scaffold-repo-skeleton.md`에 기록한다. 이 계획
문서 자체는 실행 후에도 수정하지 않고 "실행 전 계획" 그대로 보존한다.
