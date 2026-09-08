#!/usr/bin/env bash
# third_party/<project> submodule 체크아웃 위에 third_party/patches/<project>/*.patch를
# 순서대로 적용한다 (ADR-022).
#
# 사용법: tools/apply-patches.sh <project>
#
# TODO: 실제 포팅 대상이 확정되면 구현한다.
#   1. third_party/<project>가 clean한 submodule 체크아웃 상태인지 확인
#   2. third_party/patches/<project>/*.patch를 이름순으로 git apply
#   3. 실패 시 어떤 패치에서 충돌했는지 보고

set -euo pipefail
echo "TODO: apply-patches.sh는 아직 구현되지 않았다 (docs/design/repo-layout.md 참고)" >&2
exit 1
