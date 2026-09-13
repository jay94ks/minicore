#ifndef MINICORE_LIBS_LIBKENV_STRING_H
#define MINICORE_LIBS_LIBKENV_STRING_H

#include "spinlock.h"
#include "types.h"

// libkenv: freestanding 문자열 지원(QU-19B76E06, 설계자 지시,
// 2026-09-14 - "(b)를 채택하되, 1) 외부에서 레퍼런스 카운터를
// 주입하거나, 2) const 선언된 문자열이라 카운팅할 필요가 없도록,
// kernel::string을 구현하도록 해"). std::string_view처럼 소유권 없는
// 뷰(포인터+길이)만 들고, 참조 카운터 자체는 kernel::string이 만들지
// 않고 호출부가 주입한다 - 이 클래스는 카운터가 무엇을 세는지, 0이
// 됐을 때 무엇을 해제해야 하는지 전혀 모른다(동적 할당자가 아직 없는
// 프리스탠딩 단계라 해제 정책까지 넣을 수 없음 - 그건 나중에 힙
// 할당자가 생기고 나서 이 카운터를 재사용해 문자열 버퍼를 관리하는
// 상위 계층의 몫). 카운터 포인터가 nullptr이면 "카운팅이 필요 없는
// 문자열"(문자열 리터럴처럼 프로그램 내내 살아있는 const 문자열)로
// 취급해 증가/감소를 아예 건너뛴다.
namespace kernel {

class string {
public:
    // refCount가 nullptr이면 카운팅 없음(리터럴/정적 문자열용).
    constexpr string(const char* data, size_t length, AtomicU32* refCount = nullptr)
        : _data(data), _length(length), _refCount(refCount) {
        if (_refCount) {
            _refCount->fetchAdd(1);
        }
    }

    string(const string& other) : _data(other._data), _length(other._length), _refCount(other._refCount) {
        if (_refCount) {
            _refCount->fetchAdd(1);
        }
    }

    string& operator=(const string& other) {
        if (this == &other) {
            return *this;
        }
        if (_refCount) {
            _refCount->fetchSub(1);
        }
        _data = other._data;
        _length = other._length;
        _refCount = other._refCount;
        if (_refCount) {
            _refCount->fetchAdd(1);
        }
        return *this;
    }

    ~string() {
        if (_refCount) {
            _refCount->fetchSub(1);
        }
    }

    const char* data() const { return _data; }
    size_t length() const { return _length; }
    bool empty() const { return _length == 0; }

    // 카운팅 대상 문자열이고 이번 소멸/대입으로 참조가 0이 됐는지
    // 상위 계층(향후 할당자 연동 코드)이 판정할 때 쓴다 - kernel::
    // string 자신은 이 값으로 아무것도 해제하지 않는다.
    AtomicU32* refCount() const { return _refCount; }

private:
    const char* _data;
    size_t _length;
    AtomicU32* _refCount;
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_STRING_H
