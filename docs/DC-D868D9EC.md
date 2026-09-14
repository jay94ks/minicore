# Syscall 서브시스템(SP-04EE2A18) 세부 결정 미정

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-D868D9EC
  status: approved
  updatedAt: 2026-09-14T06:45:11.465Z
  갱신: node scripts/export-cnw-docs.mjs
-->
## 배경

SP-04EE2A18(Syscall 디스패치 및 비동기 처리 서브시스템 — 설계 제안)
작성 중 임의로 정하지 않고 확인이 필요하다고 판단한 세부를 모았다.

## 결정 (QU-EF643652 답변, 2026-09-14 - 전부 해소됨)

1. **유저랜드 syscall 트랩 방식**: **syscall 명령과 레거시 `int
   0x80`류 게이트 둘 다 계획**하기로 확정("syscall과 legacy 호환으로
   int 0x80류 둘다 계획하라") - 두 경로 모두 같은 내부 디스패치로
   합류.
2. **`AsyncTaskHandler::onCancel` 추가**(SP-F682B889 개정) - **승인**
   ("onCancel 추가 승인한다"). SP-F682B889 §3.1에 반영 완료.
3. **재블로킹(rejoin) 진입점 설계**: 별도 endpoint를 만들지 않는
   방향으로 확정 - "syscall -> 추적코드 즉시 반환, waitForSyscall ->
   추적코드의 완료를 대기" 형태로 syscall 자체를 제출(submit)/대기
   (wait) 두 단계로 분리했다. 재블로킹은 그냥 같은 토큰으로
   `waitForSyscall`을 다시 호출하는 것과 동일해져 별도 endpoint가
   필요 없어졌다.

## 참고

- SP-04EE2A18 - 이 결정들이 반영된 최종 설계.
- SP-F682B889 - 2번 항목이 개정된 대상(반영 완료).
