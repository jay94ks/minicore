# AsyncTask 컨텍스트 전환: 스택풀 vs C++20 코루틴 미정

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-23AEA8B4
  status: review
  updatedAt: 2026-09-14T04:01:45.707Z
  갱신: node scripts/export-cnw-docs.mjs
-->
# AsyncTask 컨텍스트 전환: 스택풀 vs C++20 코루틴 — 요구분석

## 배경

SP-F682B889 §7이 설계자 지시("Freestanding 환경에서 C++20의
coroutine을 활용할 방안을 찾아서 설계 제안에 반영하라")에 따라
C++20 코루틴을 실측 검증했다 - freestanding 환경(표준 라이브러리
없음, 예외/RTTI 없음, `-nostdlib`)에서 최소 커스텀 `<coroutine>`
대체 헤더(`libkenv/coroutine.h`) 하나로 실제 suspend/resume이
정상 동작함을 커널 부팅 중 실측으로 확인했다.

이미 확정된 `AsyncTask`의 컨텍스트 전환 방식(QU-F86426D5 -
"`kContextSwitch` 재사용하는 소프트웨어 스택 전환 + 자체 `yield()`")
을 코루틴 기반(스택 없음, `co_await`가 곧 yield)으로 바꿀 수 있다는
게 이번 실측의 결론이다 - 다만 이미 확정된 사항을 뒤집는 것이라
재검토를 요청한다.

## 결정이 필요한 항목

1. **스택풀(기존 확정) vs 코루틴(신규 제안) 중 어느 쪽으로
   `AsyncTask`를 구현할지.** SP-F682B889 §7.3에 양쪽의 장단점을
   정리해 뒀다 - 요약하면 코루틴 쪽은 전용 스택/트램폴린이 아예
   필요 없어지고 `AsyncTaskHandler::onExec`을 자연스러운 순차
   코드로 쓸 수 있지만, 코루틴 프레임 크기가 작업마다 달라 Slab
   버킷 경계를 어떻게 다룰지 실측이 더 필요하다.
2. (1에서 코루틴을 택한 경우) **코루틴 프레임 할당 실패 프로토콜**:
   `promise_type::get_return_object_on_allocation_failure()`를
   구현해 Slab 고갈 시 예외 없이 실패를 표현하는 방식으로 갈지,
   아니면 다른 방식을 원하는지.
