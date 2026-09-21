#ifndef MINICORE_LIBS_LIBKVDB_KVDB_H
#define MINICORE_LIBS_LIBKVDB_KVDB_H

// libkvdb: 고정 용량 Key-Value 저장소(SP-30FCC8AE §1-D, PN-24A2B6F5
// §1-C/§1-D 항목1 - 설계자 지시: "단순하게 UserRecord들을 바이너리로
// 직렬화하는 것이 아니라 Key-Value 데이터베이스를 구현해야 하는
// 과제로 해석해" / "커널 구성요소가 아닌데 여기(minicore/libs) 배치
// 되는 이유는 authmgr라는 커널 서비스의 의존성이기 때문"). authmgr은
// 유저랜드 프로세스(minicore/authmgr)라 kernel:: 네임스페이스나
// libkenv 전용 타입(동적 힙 할당 포함 - 이 유저랜드 툴체인엔 아직
// malloc/new 자체가 없다)에 의존하지 않는다 - libcpio/libjson과
// 동일한 관례(표준 C++ 타입만 사용, kernel/userland 양쪽에서 그대로
// 컴파일).
//
// v1 범위(PN-24A2B6F5 §1-C/D가 명시한 것): 메모리 전용(재부팅 시
// 소실 - fs 서비스가 생겨 영속화가 필요해지면 그때 온디스크 포맷/
// libkcrypto AES256 암호화를 얹는다, 지금은 그 대상 자체가 없어
// 설계하지 않는다, RM-23F4B687 §4). 저장소 용량은 컴파일 타임에
// 고정되고(동적 할당 전혀 없음 - 슬롯 배열을 그 자리에서 선형 탐색),
// 키/값은 호출부가 원하는 만큼의 임의 바이트 블롭이다(스키마를 이
// 라이브러리가 몰라도 됨 - UserRecord 필드 등은 상위 계층 몫).
//
// "로그인 명 별도 인덱스"(PN-24A2B6F5 §1-C/D 항목2)는 이 라이브러리
// 안에 네임스페이스 개념을 두지 않고, 완전히 분리된 두 번째
// `Store` 인스턴스(예: loginName 바이트 -> uid 바이트)로 표현한다 -
// 그 항목이 명시적으로 열어 둔 두 선택지 중 더 단순한 쪽(RM-23F4B687
// §4 취지, 착수 세션이 확정 가능한 구현 디테일).
namespace kvdb {

enum class ErrorCode {
    None,
    NotFound,
    KeyTooLong,
    ValueTooLong,
    StoreFull,
};

// MaxEntries개까지, 키는 MaxKeyBytes바이트까지, 값은 MaxValueBytes
// 바이트까지 담는 고정 용량 저장소. put()은 이미 있는 키면 값을
// 덮어쓰고, 없으면 빈 슬롯에 새로 채운다(선형 탐색 - 실측으로 규모가
// 커져 병목이 확인되면 그때 인덱싱 구조로 교체, 지금은 authmgr의
// UserRecord 캐시 규모(PN-B6DB692C 기준 최대 1024개) 정도를 전제로
// 한 v1 최소 구현).
template <unsigned int MaxEntries, unsigned int MaxKeyBytes, unsigned int MaxValueBytes>
class Store {
public:
    ErrorCode put(const unsigned char* key, unsigned int keyLen, const unsigned char* value,
                  unsigned int valueLen) {
        if (keyLen > MaxKeyBytes) {
            return ErrorCode::KeyTooLong;
        }
        if (valueLen > MaxValueBytes) {
            return ErrorCode::ValueTooLong;
        }
        Entry* slot = find(key, keyLen);
        if (slot == nullptr) {
            slot = findFreeSlot();
            if (slot == nullptr) {
                return ErrorCode::StoreFull;
            }
            slot->used = true;
            ++_count;
        }
        for (unsigned int i = 0; i < keyLen; ++i) {
            slot->key[i] = key[i];
        }
        slot->keyLen = keyLen;
        for (unsigned int i = 0; i < valueLen; ++i) {
            slot->value[i] = value[i];
        }
        slot->valueLen = valueLen;
        return ErrorCode::None;
    }

    ErrorCode get(const unsigned char* key, unsigned int keyLen, unsigned char* outValue,
                  unsigned int outCapacity, unsigned int* outValueLen) const {
        const Entry* slot = find(key, keyLen);
        if (slot == nullptr) {
            return ErrorCode::NotFound;
        }
        if (slot->valueLen > outCapacity) {
            return ErrorCode::ValueTooLong;
        }
        for (unsigned int i = 0; i < slot->valueLen; ++i) {
            outValue[i] = slot->value[i];
        }
        *outValueLen = slot->valueLen;
        return ErrorCode::None;
    }

    bool contains(const unsigned char* key, unsigned int keyLen) const { return find(key, keyLen) != nullptr; }

    ErrorCode erase(const unsigned char* key, unsigned int keyLen) {
        Entry* slot = find(key, keyLen);
        if (slot == nullptr) {
            return ErrorCode::NotFound;
        }
        slot->used = false;
        slot->keyLen = 0;
        slot->valueLen = 0;
        --_count;
        return ErrorCode::None;
    }

    // used인 항목을 전부 방문한다 - fn(const unsigned char* key, unsigned
    // int keyLen, const unsigned char* value, unsigned int valueLen).
    // 로그인 명 인덱스처럼 상위 계층이 전체를 스캔해야 하는 경우를 위함.
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (unsigned int i = 0; i < MaxEntries; ++i) {
            if (_entries[i].used) {
                fn(_entries[i].key, _entries[i].keyLen, _entries[i].value, _entries[i].valueLen);
            }
        }
    }

    unsigned int size() const { return _count; }
    static constexpr unsigned int capacity() { return MaxEntries; }

private:
    struct Entry {
        bool used = false;
        unsigned int keyLen = 0;
        unsigned int valueLen = 0;
        unsigned char key[MaxKeyBytes] = {};
        unsigned char value[MaxValueBytes] = {};
    };

    static bool keysEqual(const unsigned char* a, unsigned int aLen, const unsigned char* b, unsigned int bLen) {
        if (aLen != bLen) {
            return false;
        }
        for (unsigned int i = 0; i < aLen; ++i) {
            if (a[i] != b[i]) {
                return false;
            }
        }
        return true;
    }

    Entry* find(const unsigned char* key, unsigned int keyLen) {
        for (unsigned int i = 0; i < MaxEntries; ++i) {
            if (_entries[i].used && keysEqual(_entries[i].key, _entries[i].keyLen, key, keyLen)) {
                return &_entries[i];
            }
        }
        return nullptr;
    }

    const Entry* find(const unsigned char* key, unsigned int keyLen) const {
        for (unsigned int i = 0; i < MaxEntries; ++i) {
            if (_entries[i].used && keysEqual(_entries[i].key, _entries[i].keyLen, key, keyLen)) {
                return &_entries[i];
            }
        }
        return nullptr;
    }

    Entry* findFreeSlot() {
        for (unsigned int i = 0; i < MaxEntries; ++i) {
            if (!_entries[i].used) {
                return &_entries[i];
            }
        }
        return nullptr;
    }

    Entry _entries[MaxEntries] = {};
    unsigned int _count = 0;
};

}  // namespace kvdb

#endif  // MINICORE_LIBS_LIBKVDB_KVDB_H
