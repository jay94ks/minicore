#ifndef MINICORE_LIBS_LIBKCRYPTO_SHA256_H
#define MINICORE_LIBS_LIBKCRYPTO_SHA256_H

// libkcrypto: 암호학적 해시/암호화(SP-CC1CF30E §8 - "이 프로젝트에
// 암호학적 해시/암호화 구현이 전혀 없다... minicore/libs/libkcrypto
// (커널/유저 공용)를 신설한다") - authmgr의 UserRecord::passwordHash
// (SHA256)/레코드 암호화(AES256)가 첫 소비자다. libutf8/libcpio와
// 동일한 관례(RM-23F4B687 §3) - 순수 계산이라 커널 전용 API를 전혀
// 안 쓰므로 매크로 게이팅 없이 표준 C++ 타입만 사용, 커널/유저
// 양쪽에서 그대로 컴파일된다. 동적 할당 없음(모든 상태는 스택/호출부
// 제공 버퍼).
//
// v1 범위: SHA256만(AES256은 §1-C 레코드 암호화가 실제로 착수될 때
// 후속 - RM-23F4B687 §4, 지금 쓰이지 않을 코드를 미리 만들지 않음).
namespace sha256 {

constexpr unsigned int kDigestBytes = 32;

struct Digest {
    unsigned char bytes[kDigestBytes];
};

// 입력 전체를 한 번에 해싱하는 one-shot API - authmgr의 패스워드
// 해싱(평문 최대 64바이트, SP-30FCC8AE §1-A.1)이 유일한 예상
// 소비자라 스트리밍/증분 API는 지금 필요 없다(RM-23F4B687 §4,
// 필요해지면 그때 추가).
Digest hash(const unsigned char* data, unsigned long length);

// Digest를 소문자 hex 64문자로 인코딩한다(UserRecord::passwordHash의
// "algorithm:value" 형식 중 <value> 부분, SP-CC1CF30E §8). out은
// 최소 64바이트, 널 종단은 out[64]에 별도로 쓰지 않는다(호출부가
// 필요하면 직접 붙임 - "algorithm:" 접두사도 이 함수 책임이 아님).
void toHex(const Digest& digest, char* out);

}  // namespace sha256

#endif  // MINICORE_LIBS_LIBKCRYPTO_SHA256_H
