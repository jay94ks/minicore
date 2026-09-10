#!/usr/bin/env python3
"""tools/export-sdk.py — 빌드된 minicore 트리에서 저장소 밖 프로그램이
minicore용 ELF를 만들 수 있는 최소 SDK를 뽑아낸다
(real-libc-syscall-layer.md §M38, docs/design/build-system.md ADR-190).
ADR-031("크로스 툴체인은 저장소 안에 vendoring하지 않는다")과 같은
정신 — SDK 산출물 자체도 저장소에 커밋하지 않는다, 이 스크립트만
커밋한다.

내보내는 것(<out>/x86_64-minicore/ 아래):
  include/musl/{arch/x86_64,arch/generic,generated,include}/  — musl의
    패치된 공개 헤더. libc/CMakeLists.txt가 minicore_libc에 PUBLIC으로
    거는 include 디렉터리 4개와 정확히 같은 목록·순서다(그 파일
    상단 주석 참고 — 순서가 실제로 중요하다, M36에서 실제로 겪음).
  include/mc/ — libs/mc/include/mc/ 그대로(libmc 공개 헤더).
  lib/libc.a, lib/libmc.a — 빌드된 정적 라이브러리(이름만 바꿔 복사).
    ADR-190 원안은 동적 libc.so/ld-musl-x86_64.so.1도 내보내려 했지만,
    M29가 이미 동적 링킹을 정적으로 되돌려 뒀다(ADR-203) — 그래서
    이번 라운드는 정적 libc.a만 있다(M38 실행 중 확정한 범위 축소).
  link.ld — 저장소 안 각 userland/*가 개별로 두던 링커 스크립트
    패턴과 달리, 저장소 밖 프로그램을 위한 범용 하나(ADR-160의 고정
    가상주소 배치를 따른다) — userland/musl-hello/link.ld를 그대로
    가져왔다(이미 그 자체가 musl-hello에 특화된 내용이 없었다).
  bin/x86_64-minicore-clang — 컴파일러 래퍼(bash). 이미 설치된
    크로스 clang을 이 SDK의 include/lib 경로로 감싼다.
  x86_64-minicore.cmake — 위 래퍼+정적 링크를 미리 박아 둔 최소
    CMake 툴체인 파일.

사용법: export-sdk.py <빌드 디렉터리, 예: build/x86_64-clang> <출력 디렉터리>
"""
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def copy_tree_merge(src: Path, dst: Path) -> None:
    """shutil.copytree(dirs_exist_ok=True)와 같지만 이 프로젝트가
    지원하는 파이썬 버전 전부에서 동작하도록 직접 구현한다."""
    dst.mkdir(parents=True, exist_ok=True)
    for entry in src.iterdir():
        target = dst / entry.name
        if entry.is_dir():
            copy_tree_merge(entry, target)
        else:
            shutil.copy2(entry, target)


def main(argv):
    if len(argv) != 3:
        print(f"사용법: {argv[0]} <빌드 디렉터리> <출력 디렉터리>", file=sys.stderr)
        return 1

    build_dir = Path(argv[1]).resolve()
    out_dir = Path(argv[2]).resolve()
    sdk_root = out_dir / "x86_64-minicore"

    musl_patched = build_dir / "libc" / "_patched" / "musl"
    musl_generated = build_dir / "libc" / "generated"
    libc_a = build_dir / "libc" / "libminicore_libc.a"
    libmc_a = build_dir / "libs" / "mc" / "libminicore_libmc.a"

    for required in (musl_patched, musl_generated, libc_a, libmc_a):
        if not required.exists():
            print(f"오류: {required}가 없다 — 먼저 minicore_libc/minicore_libmc를 빌드했는지 확인", file=sys.stderr)
            return 1

    if sdk_root.exists():
        shutil.rmtree(sdk_root)

    # libc/CMakeLists.txt::target_include_directories(minicore_libc PUBLIC...)
    # 3번 호출과 정확히 같은 목록 — 순서가 그 파일 상단 주석대로
    # 실제로 의미 있다(arch 그룹이 generated/include보다 먼저).
    include_dir = sdk_root / "include" / "musl"
    copy_tree_merge(musl_patched / "arch" / "x86_64", include_dir / "arch" / "x86_64")
    copy_tree_merge(musl_patched / "arch" / "generic", include_dir / "arch" / "generic")
    copy_tree_merge(musl_generated, include_dir / "generated")
    copy_tree_merge(musl_patched / "include", include_dir / "include")

    copy_tree_merge(REPO_ROOT / "libs" / "mc" / "include" / "mc", sdk_root / "include" / "mc")

    lib_dir = sdk_root / "lib"
    lib_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(libc_a, lib_dir / "libc.a")
    shutil.copy2(libmc_a, lib_dir / "libmc.a")

    shutil.copy2(REPO_ROOT / "tools" / "sdk-template" / "link.ld", sdk_root / "link.ld")

    bin_dir = sdk_root / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    wrapper_dst = bin_dir / "x86_64-minicore-clang"
    shutil.copy2(REPO_ROOT / "tools" / "sdk-template" / "x86_64-minicore-clang", wrapper_dst)
    wrapper_dst.chmod(0o755)

    shutil.copy2(REPO_ROOT / "tools" / "sdk-template" / "x86_64-minicore.cmake",
                 sdk_root / "x86_64-minicore.cmake")

    print(f"완료: {sdk_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
