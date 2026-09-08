# 완료 보고: 저장소 스캐폴딩

**대상 계획**: [scaffold-repo-skeleton.md](../plan/scaffold-repo-skeleton.md)
**대상 설계**: [repo-layout.md](../design/repo-layout.md)
**실행일**: 2026-09-08

## 수행한 작업

계획에 정의된 범위를 모두 실행했다.

1. 디렉토리 구조 생성 — `repo-layout.md`의 트리와 1:1로 대응하도록
   `kernel/{include,core/{ipc,sched,mm,object},arch/{x86_64,aarch64}}`,
   `libk/include/libk`, `init/initrun`,
   `servers/{procsrv,vfs,fs/memfs,netsrv,devmgr,drivers}`,
   `libc/sysdeps/minicore`, `userland`, `third_party/patches`, `tools`,
   `toolchain`을 생성. 아직 파일이 없는 리프 디렉토리에는 `.gitkeep` 배치.
2. `toolchain/common.cmake` — ADR-010 언어 정책(`-ffreestanding -fno-exceptions
   -fno-rtti -fno-stack-protector -fno-pic`, `-nostdlib -static` 링크 옵션) 반영.
3. `toolchain/{x86_64,aarch64}-{clang,gcc}.cmake` 4종 생성 — ADR-020에 따라
   Clang은 `--target=<triple>-unknown-none-elf`, GCC는 `<arch>-elf-gcc` 바이너리를
   사용하도록 분기.
4. 최상위 `CMakeLists.txt` — `MINICORE_ARCH` 캐시 변수(x86_64|aarch64)로 아키텍처를
   선택하고, x86_64에서만 `servers/`·`libc/`·`userland/`까지 빌드하도록 구성
   (ADR-009: aarch64는 현재 커널 컴파일 검증까지만).
5. `CMakePresets.json` — `x86_64-clang`/`x86_64-gcc`/`aarch64-clang`/`aarch64-gcc`
   4개 프리셋, Ninja 제너레이터로 구성.
6. 각 서브디렉토리에 최소 `CMakeLists.txt` 배치 — 현재는 소스가 없으므로 모두
   `INTERFACE` 라이브러리 타깃으로 골격만 유지(`kernel`, `libk`, `init/initrun`,
   `servers/*`, `libc`, `userland`). 각 파일에 어떤 ADR에 대응하는지, 소스가
   추가되면 어떻게 전환해야 하는지 TODO로 명시.
7. `.gitmodules` — 빈 상태(주석 예시만)로 생성.
8. `tools/apply-patches.sh`, `tools/mkinitrd.sh`, `tools/run-qemu.sh` — 사용법과
   TODO만 있는 스텁, 실행 권한(`chmod +x`) 부여.
9. (계획에는 없었으나 스캐폴딩에 필수적이라 판단하여 추가) `.gitignore` —
   `build/` 등 빌드 산출물이 커밋되지 않도록 설정.

## 검증 결과

- `CMakePresets.json`은 유효한 JSON임을 확인 (`python -m json` 파싱 통과).
- `cmake --preset x86_64-clang -S . -B build/x86_64-clang` 실행 결과:
  **구성 실패**. 단, 실패 원인은 이 개발 머신(Windows, MSYS2 gcc/g++만 설치됨)에
  **Clang/LLVM이 설치되어 있지 않기 때문**이며, 오류 메시지(`CMAKE_C_COMPILER: clang
  ... was not found in the PATH`)가 이를 명확히 가리킨다. `project()` 선언까지는
  정상적으로 도달했고 toolchain 파일이 올바르게 로드되어 `CMAKE_C_COMPILER`가
  의도대로 `clang`으로 설정된 것이 확인되어, **CMake 파일 자체의 구문·구조는
  정상**이라고 판단한다.
- 위 실패로 생성된 불완전한 `build/x86_64-clang` 캐시는 삭제했다(정상 빌드
  산출물이 아니므로).
- x86_64-gcc/aarch64-* 프리셋도 이 머신에는 `x86_64-elf-gcc`/`aarch64-elf-gcc`/
  Clang이 없어 동일한 이유로 실제 configure는 확인하지 못했다.

## 미해결/후속 필요 사항

- **크로스 툴체인 설치가 실제 검증의 전제 조건**이다. 이후 커널 코드 작성을
  시작하기 전에 다음 중 하나를 준비해야 한다:
  - Clang/LLVM 설치 (권장, ADR-020 1순위) — freestanding 타깃 컴파일 확인,
  - 또는 `x86_64-elf-gcc`/`aarch64-elf-gcc` 크로스 툴체인 설치.
  이 항목은 다음 계획(빌드 환경 준비)에서 다룰 것을 제안한다.
- `init/initrun`의 기동 매니페스트 형식은 여전히 OPEN-29로 남아 있다.
- 실제 커널 소스(부팅, IPC, 스케줄러 등) 작성은 이 스캐폴딩의 범위 밖이며
  별도 계획이 필요하다.

## 저장소 현재 상태

```
minicore/
├── .gitignore
├── .gitmodules
├── CMakeLists.txt
├── CMakePresets.json
├── docs/                (spec/plan/done/design/remind)
├── init/initrun/
├── kernel/{include,core/*,arch/*}
├── libc/sysdeps/minicore/
├── libk/include/libk/
├── servers/{procsrv,vfs,fs/memfs,netsrv,devmgr,drivers}/
├── third_party/patches/
├── toolchain/*.cmake
├── tools/*.sh
└── userland/
```

컴파일 가능한 실제 코드는 아직 없다 — 이 보고는 "빌드 골격이 설계대로
만들어졌고, 그 골격 자체에는 결함이 없다"는 것까지만 확인한다.
