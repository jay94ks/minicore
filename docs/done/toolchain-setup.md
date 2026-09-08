# 완료 보고: 크로스 툴체인(Clang) 설치

**관련 전제 조건**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md)의 "전제 조건 (착수 전 반드시 해결)"
**관련 결정**: ADR-020, ADR-031
**실행일**: 2026-09-08

## 수행한 작업

ADR-020의 1순위 경로(Clang/LLVM)를 선택해 개발 머신에 설치했다.
ADR-031에 따라 **저장소 밖**에 설치했고, 저장소 안에는 툴체인 산출물을
전혀 커밋하지 않았다 — 이 문서가 유일한 기록이다.

1. `winget install --id LLVM.LLVM -e`로 LLVM 22.1.8을 설치
   (`C:\Program Files\LLVM\bin\clang.exe`).
2. 설치 관리자가 PATH를 갱신하지 않아, 사용자 PATH 환경 변수에
   `C:\Program Files\LLVM\bin`을 수동으로 추가.

## 검증 결과

- `clang --version` → Clang 22.1.8 확인.
- `clang --print-targets`에 `x86-64`, `aarch64` 백엔드가 모두 포함됨을 확인
  (LLVM 공식 릴리스는 기본적으로 전체 타깃을 빌드하므로 별도 조치 불필요).
- `--target=x86_64-unknown-none-elf -ffreestanding -fno-exceptions -fno-rtti
  -fno-stack-protector`로 최소 `_start` 함수 컴파일 성공.
- `cmake --preset x86_64-clang -S . -B build/x86_64-clang` — **구성 성공**
  (기존 [scaffold-repo-skeleton.md](scaffold-repo-skeleton.md)에서 컴파일러
  부재로 실패했던 것과 달리 이번에는 CXX/C 컴파일러 식별까지 정상 완료).
- `cmake --preset aarch64-clang -S . -B build/aarch64-clang` — 동일하게 구성 성공.
- 검증용으로 생성한 두 `build/*` 디렉토리는 산출물이 아니므로 삭제했다
  (`.gitignore`로 어차피 추적 대상 아님).

## 미해결/후속 필요 사항

- ADR-020의 폴백 경로(`x86_64-elf-gcc`/`aarch64-elf-gcc` 크로스 GCC)는
  이번에 준비하지 않았다 — 필요 시점(GCC 경로 검증 필요할 때)에 별도로 진행한다.
- `x86_64-gcc`/`aarch64-gcc` 프리셋은 여전히 컴파일러 부재로 미검증 상태다.
- 이것으로 [kernel-bootstrap.md](../plan/kernel-bootstrap.md)의 "전제 조건"이
  (Clang 경로 한정으로) 충족되어 M1 착수가 가능해졌다.
