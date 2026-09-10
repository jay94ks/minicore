# 완료 보고: real-libc-syscall-layer M38 — minicore 타깃 SDK 내보내기

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M38
**관련 결정**: [build-system.md](../design/build-system.md) ADR-190, ADR-213
**실행일**: 2026-09-10

## 완료한 것

1. `tools/export-sdk.py` — 이미 빌드된 트리(`build/<arch>-clang`)
   에서 `<out>/x86_64-minicore/`로 musl의 패치된 공개 헤더(arch/
   x86_64, arch/generic, 생성된 bits/, 공개 include/)+`libmc` 헤더
   +정적 `libc.a`/`libmc.a`+공용 `link.ld`+컴파일러 래퍼+CMake
   툴체인 파일을 뽑는다.
2. `tools/sdk-template/link.ld` — `userland/musl-hello/link.ld`를
   그대로 재사용(그 자체가 musl-hello에 특화된 내용이 없었다).
3. `tools/sdk-template/x86_64-minicore-clang` — 셸 스크립트 참고용
   컴파일러 래퍼.
4. `tools/sdk-template/x86_64-minicore.cmake` — 저장소 밖 프로젝트가
   `-DCMAKE_TOOLCHAIN_FILE=`로 지정하면 되는 최소 CMake 툴체인
   파일. `toolchain/x86_64-clang.cmake`(ADR-020)와 같은 방식으로
   clang을 직접 부른다(아래 발견 참고).

## 계획 대비 범위 조정 (ADR-213)

1. **동적 `libc.so`는 내보내지 않는다.** ADR-190 원안은 M29의 동적
   링킹 결과물을 가정했지만, M29 자신이 이미 ADR-203으로 정적
   링킹으로 되돌아가 있었다 — 계획 문서를 다시 읽고서야 이 불일치를
   알아챘다. 존재하지 않는 산출물을 내보낼 수 없으므로 SDK는
   처음부터 정적 `libc.a`만 다룬다.
2. **타깃 트리플은 `x86_64-linux-musl`이 아니라
   `x86_64-unknown-none-elf`.** minicore 저장소 안의 `libc.a` 자신이
   실제로 이 트리플로 컴파일됐다 — 검증된 조합을 그대로 유지한다.

## 실행 중 발견한 것

컴파일러 래퍼(bash 스크립트)를 CMake의 `CMAKE_C_COMPILER`로 직접
지정했더니 ninja가 `CreateProcess failed... %1 is not a valid Win32
application`으로 실패했다 — cmake/ninja는 컴파일러를 셸을 거치지
않고 OS 프로세스 실행기로 직접 실행하는데, 이 세션의 Windows
호스트에서는 bash 스크립트를 그렇게 실행할 수 없다. `x86_64-minicore.cmake`
를 `toolchain/x86_64-clang.cmake`와 같은 방식(`CMAKE_C_COMPILER=clang`
+`CMAKE_C_FLAGS_INIT`으로 타깃 트리플/include 경로를 직접 주입)으로
바꿔 해결했다 — 셸 스크립트 래퍼는 CMake를 안 쓰는 사용자를 위한
참고 자료로만 남겼다.

## 검증

저장소 밖 스크래치 디렉터리에 `printf`+`malloc`+`strcpy`만 쓰는
순수 C "hello world"(minicore 소스 트리를 전혀 참조하지 않음)를
작성했다. `x86_64-minicore.cmake` 하나만 `-DCMAKE_TOOLCHAIN_FILE=`
로 지정해 CMake+ninja로 컴파일·링크에 성공했다(`sdk_hello.elf`,
ET_EXEC, 시작 주소 0x10007d70 — ADR-160 슬롯 배치와 일치).

결과 ELF를 `tools/mkbootdisk.py --service=sdk-hello=<그 ELF>
--linux-abi-stack=sdk-hello`(수동 호출 — 저장소의
`servers/CMakeLists.txt`는 건드리지 않았다, 이 검증은 일회성 증명이라
영구 기능으로 편입하지 않는다)로 기존 서비스들과 함께 임시
bootdisk.img에 넣고, `MINICORE_QEMU_BOOTDISK` 환경변수로 그 이미지를
지정해 `tools/run-qemu.sh`로 부팅했다. 확인: "hello from minicore SDK
(23 bytes)"가 정확히 출력됨. `tools/mkbootdisk.py`는 수정 없이 그대로
받아들였다(ADR-190이 미리 걸어 둔 확인 항목 — 형식이 minicore 자체
빌드 산출물과 같아 수정이 필요 없었다).

이 마일스톤은 새 커널/서버 코드를 건드리지 않아 QEMU 5개 회귀
스위트를 별도로 다시 돌리지 않았다(커널/유저랜드 소스 변경 없음 —
새 도구 파일만 추가) — M37 완료 시점의 회귀 확인이 여전히 유효하다.

## 남겨 둔 것

real-libc-syscall-layer.md M39(실제 서드파티 셸/coreutils 재포팅
시도, 스트레치)부터 계속 진행한다. SDK 버전 고정/배포 정책, 동적
링킹 지원, Linux/macOS 호스트에서의 실제 검증은 ADR-213이 이미
범위 밖으로 남겼다.
