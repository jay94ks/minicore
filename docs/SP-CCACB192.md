# libjson — 커널/유저 공용 JSON 파서/직렬화 라이브러리 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-CCACB192
  status: approved
  updatedAt: 2026-09-17T01:37:59.362Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# libjson — 커널/유저 공용 JSON 파서/직렬화 라이브러리 설계 제안

## 배경

SP-B071E628("프로세스간 \"공개 인터페이스\" Registry") §4가 `pubreg`
서비스와 tool 등록/조회에 참여하는 프로세스들을 위해
`minicore/libs/libjson`(신규, 커널/유저 공용) 신설을 요구했으나,
"파서 자체의 설계(스트리밍 vs 전체 파싱, 메모리 할당 전략)는 이
문서 범위 밖 - 착수 시 별도 SP"라고 명시적으로 미뤄 뒀다. 이 문서가
그 후속 SP다(PN-185406F6 착수를 위한 선행 설계).

## 1. 핵심 결정 - SAX 스타일(콜백 기반) 스트리밍 파서, 동적 할당 없음

`minicore/libs/libcpio`(CPIO 파서, 이미 구현·검증된 선례)가 이미
정확히 같은 제약(freestanding, 동적 할당 불가, 커널/유저 공용, 예외
없음) 아래에서 "원본 버퍼를 그 자리에서 훑으며 콜백을 부른다"는
방식을 채택해 성공적으로 동작 중이다 - libjson도 같은 관례를
따른다:

- 파서는 **DOM/트리를 만들지 않는다** - 별도 arena 할당자도, 동적
  할당도 전혀 하지 않는다. 호출부(예: `pubreg`)가 콜백 안에서 자신이
  원하는 필드만 자신의 자료구조로 옮겨 담는다(선택적 추출 - 문서
  전체를 다 이해할 필요 없음, `pubreg`가 `register` 메시지에서
  `processName`/`tools[].name`/`inputSchema` 등 필요한 것만 뽑아 쓰는
  용도에 정확히 맞는다).
- 문자열 값은 **이스케이프가 없으면 원본 버퍼 안 포인터+길이로
  그대로 넘긴다**(zero-copy) - 이스케이프가 있는 문자열만 호출부가
  제공한 스크래치 버퍼로 언이스케이프한다(아래 4번 참고).
- 콜백 이벤트: `onObjectStart`/`onObjectEnd`/`onArrayStart`/
  `onArrayEnd`/`onKey`/`onString`/`onNumber`/`onBool`/`onNull`/
  `onError` - `cpio::forEachEntry`와 동일한 관례로 단일 콜백 함수
  포인터 + `void* userData`.

## 2. 재귀 깊이 상한

재귀 하강 파서가 중첩된 object/array를 만날 때마다 C++ 콜스택을
쓰므로(커널 스택 크기가 유한 - `kAsyncTaskStackSize=4096` 같은 작은
전용 스택 위에서도 호출될 가능성이 있음), 무제한 중첩을 허용하면
스택 오버플로우 위험이 있다. **`kMaxJsonNestingDepth = 32`**(제안,
v1 실측 후 조정 가능한 구현 세부) 고정 상한 - 초과 시 `onError`로
`NestingTooDeep` 보고 후 파싱 중단.

## 3. 숫자 파싱 - [확정, 2026-09-17, QU-8E75915F 답변] 처음부터 정수+부동소수점 전부 지원

JSON 표준 number 문법(정수/소수/지수)을 어디까지 지원할지가
미확정이다:

- (a) **정수만 지원**(v1 축소) - `int64_t` 범위의 정수만 파싱,
  소수점/지수가 나오면 `onError`로 거부. freestanding 환경에
  `<stdlib.h>`의 `strtod`가 없어(다른 표준 헤더들도 이 타겟에
  없었던 전례, RM-23F4B687 §2 "PN-68871BC9/PN-B41D8C0E 실측" 참고)
  부동소수점 파싱을 직접 구현해야 하는 부담을 아예 피한다 - tool
  선언 JSON Schema가 실제로 소수를 담을 필요가 있는지도 불확실하다.
- (b) **정수+부동소수점 전부 지원** - `double`까지 지원(이 프로젝트는
  이미 FPU 지연 컨텍스트(`PN-F258698E`)가 구현돼 있어 커널 코드에서
  부동소수점 연산 자체는 가능하다) - 다만 `strtod`를 직접 구현해야
  한다(비표준 헤더 부재, 위와 동일한 이유).

**[확정, 2026-09-17, QU-8E75915F 답변]** 설계자가 두 권장안(정수만/
ASCII만) 대신 "처음부터 넓게(소수+유니코드) 지원"을 선택했다 - (b)
채택. `onNumber` 콜백이 정수/실수 겸용 값을 받도록 v1부터 구현하고,
`strtod` 없이 직접 부동소수점 파싱을 구현한다(freestanding 제약,
§3 원문 그대로).

## 4. 문자열 이스케이프 - [확정, 2026-09-17, QU-8E75915F 답변] 처음부터 \uXXXX 지원

표준 JSON 이스케이프(`\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t`)는
지원한다 - 흔하고 구현이 간단하다. **`\uXXXX`(유니코드 이스케이프,
서로게이트 페어 포함)는 확인 필요(질의 대상)**:

- (a) **v1은 미지원** - `\uXXXX`를 만나면 `onError`로 거부. tool
  이름/설명이 전부 ASCII로 충분하다면 당장 필요 없다.
- (b) **지원** - UTF-8로 디코딩해 콜백에 넘긴다. 구현 부담이 (a)보다
  꽤 큼(서로게이트 페어 조합, UTF-8 인코딩).

**[확정, 2026-09-17, QU-8E75915F 답변]** (b) 채택 - v1부터 UTF-8로
디코딩해 콜백에 넘긴다(서로게이트 페어 조합 포함).

## 5. 직렬화(쓰기) - `JsonWriter`

`pubreg`가 `query` 응답 등을 JSON으로 만들어 내야 하므로 최소한의
쓰기 기능도 필요하다 - 동적 할당 없이 **호출부가 제공한 고정
버퍼+용량에 append하는 방식**:

```cpp
class JsonWriter {
public:
    JsonWriter(char* buffer, size_t capacity);
    bool beginObject(); bool endObject();
    bool beginArray(); bool endArray();
    bool key(const char* name, size_t nameLen);
    bool value(const char* str, size_t len);  // 문자열 값(자동 이스케이프+따옴표)
    bool value(int64_t n);
    bool value(bool b);
    bool nullValue();
    size_t length() const;  // 지금까지 쓴 바이트 수
    bool overflowed() const;  // 버퍼 용량 초과 시도가 있었는지
};
```

버퍼가 부족하면 그 지점에서 쓰기를 조용히 중단하고
`overflowed()`가 `true`를 반환한다(부분 결과를 반환하지 않음 -
호출부가 반드시 확인 후 사용해야 함, "표준 커널 API 관례 - 실패를
숨기지 않는다"와 일치).

## 6. 파일 배치 및 빌드

`minicore/libs/libjson/json.h`/`json.cpp` + `CMakeLists.txt` -
`libelf`(`MINICORE_LIBELF_KERNEL` 매크로 게이팅, SP-68182FBD §2.4)와
동일한 패턴으로 커널/유저 양쪽 빌드를 분기한다(기본값은 유저 영역용
컴파일). 착수 시 `RM-7C249618`의 "예정" 표기를 "구현 완료"로 갱신
(CLAUDE.md 규칙 8).

## 7. 이 문서가 다루지 않는 것

- tool 선언 JSON Schema의 정확한 방언/확장 필드(`x-minicore-transport`
  류) - SP-B071E628 §6 항목2가 이미 별도로 "검토 필요"로 열어 둔
  영역, 이 문서(순수 파서/직렬화 엔진 설계)와 무관하다.
- `pubreg`의 `query` 매칭 문법(SP-B071E628 §6 항목3) - 마찬가지로
  파서 엔진과 무관, `pubreg` 자신의 로직 설계 영역.

## 참고

- SP-B071E628 §4 - 이 문서를 낳은 요구사항.
- `minicore/libs/libcpio` - 이 문서의 핵심 결정(콜백 기반, 동적
  할당 없음, zero-copy 문자열)의 직접 선례.
- RM-7C249618 - 라이브러리 목록(착수 시 갱신 대상).

