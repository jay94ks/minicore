#include "vfat_driver.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

// [실측으로 발견, minicore/libs/libext4/ext4_driver.cpp(PN-9AE5BFE4)와
// 동일한 제약] `Fat32Volume`의 옛 무상태 함수(resolvePath/readData/
// readdirAt)는 재사용하지 않는다 - 전부 `fs::BlockDevice::readBlocks()`
// (동기 래퍼, Task 레벨 블로킹)를 쓰는데, 이건 `onExec()`(코루틴) 안에서
// 못 쓴다(PN-6EDED542 문서 주석 참고). `kernel::AsyncTaskCoroAwaiter`로
// 바꿔도, 그 클래스가 `AsyncTask::current()`(항상 최상위 - 이 경우
// onExec 자신의 AsyncTask)를 기준으로 재개 대상을 고르기 때문에, 별도
// 코루틴 함수로 감싼 I/O 헬퍼를 onExec이 다시 co_await하는 합성은
// 안전하게 재개되지 않는다(SP-F682B889 §9.5 항목3, 2026-09-22 정정
// 문단). 그래서 이 파일도 ext4_driver.cpp와 동일하게, I/O 지점마다
// `co_await kernel::AsyncTaskCoroAwaiter(...)`를 onExec 자신의 몸체
// 안에 직접 박아 넣는 평탄화(flatten)된 버전으로 구현한다 - 순수 계산
// (버퍼 해석) 부분만 별도 함수로 뽑고, 실제 I/O 오케스트레이션은 전부
// onExec 한 함수 안에 있다. `Open`/`Stat` 둘 다 경로 탐색이 필요해 그
// 루프가 두 곳에 거의 그대로 중복되는데, 위 제약(합성 불가) 때문에
// 함수로 뽑아 공유할 수 없어 의도적으로 감수한 중복이다.
namespace vfat {

namespace {

// 경로 세그먼트(정규화 안 된 원문)를 8.3 형식(대문자, 11바이트)으로
// 정규화한다 - vfat.cpp에 있던 것과 동일(그쪽은 옛 resolvePath 전용
// anonymous namespace라 여기서 재사용할 수 없어 그대로 복제 - ext4의
// kIsDirMode 등과 동일한 관례).
void kNormalizeTo83(const char* seg, uint32_t segLen, char out11[11]) {
    for (uint32_t i = 0; i < 11; ++i) {
        out11[i] = ' ';
    }
    uint32_t dot = segLen;
    for (uint32_t i = 0; i < segLen; ++i) {
        if (seg[i] == '.') {
            dot = i;
        }
    }
    auto toUpper = [](char c) -> char { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; };
    uint32_t nameLen = (dot < segLen) ? dot : segLen;
    if (nameLen > 8) {
        nameLen = 8;
    }
    for (uint32_t i = 0; i < nameLen; ++i) {
        out11[i] = toUpper(seg[i]);
    }
    if (dot < segLen) {
        uint32_t extLen = segLen - dot - 1;
        if (extLen > 3) {
            extLen = 3;
        }
        for (uint32_t i = 0; i < extLen; ++i) {
            out11[8 + i] = toUpper(seg[dot + 1 + i]);
        }
    }
}

// [신규, 2026-09-23, PN-9D6FE4B6] relPath를 "부모 디렉터리 경로" +
// "마지막 세그먼트(leaf 이름)"로 나눈다(순수 계산, I/O 없음) -
// Mkdir/Unlink/Rmdir가 공통으로 필요(부모를 먼저 찾아 그 안에서
// leaf를 다루므로). 마지막 '/' 뒤(중간의 연속 '/'는 건너뜀)를 leaf로
// 본다. relPathLen==0이거나 leaf가 빈 문자열(경로가 "/"로만 끝남)이면
// false.
bool kSplitParentAndLeaf(const char* relPath, uint32_t relPathLen, uint32_t* outParentLen, uint32_t* outLeafStart,
                          uint32_t* outLeafLen) {
    if (relPathLen == 0) {
        return false;
    }
    uint32_t end = relPathLen;
    while (end > 0 && relPath[end - 1] == '/') {
        --end;
    }
    if (end == 0) {
        return false;  // 경로가 전부 '/'뿐
    }
    uint32_t leafStart = end;
    while (leafStart > 0 && relPath[leafStart - 1] != '/') {
        --leafStart;
    }
    if (leafStart == end) {
        return false;
    }
    uint32_t parentLen = leafStart;
    while (parentLen > 0 && relPath[parentLen - 1] == '/') {
        --parentLen;
    }
    *outParentLen = parentLen;
    *outLeafStart = leafStart;
    *outLeafLen = end - leafStart;
    return true;
}

// [신규, 2026-09-23, PN-3D39A53C] LFN 쓰기용 - v1이 8.3 짧은 이름에
// 그대로 허용하는 보수적 문자 집합(스펙은 더 넓은 집합을 허용하지만,
// 그 외 문자가 있으면 안전하게 LFN 경로로 보낸다).
bool kIsValid83Char(char c) {
    if (c >= '0' && c <= '9') {
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        return true;
    }
    if (c >= 'a' && c <= 'z') {
        return true;
    }
    return c == '_' || c == '-';
}

// [신규, 2026-09-23, PN-3D39A53C] 원본 이름이 손실 없이 8.3으로 정확히
// 표현 가능한지 판정한다(순수 계산) - 가능하면 정규화된 11바이트와
// NT 케이스 비트(kNtCaseLowerBase/Ext, vfat.h)까지 채워 돌려준다.
// 이게 true면 LFN 없이 짧은 엔트리만 쓰면 된다(Mkdir의 기존 동작).
// 점이 2개 이상, 이름부>8, 확장자>3, 대소문자가 이름부/확장자 각각
// 안에서 섞여 있음(NT 케이스 비트로는 표현 불가), 허용 문자 집합
// 밖의 문자 - 이 중 하나라도 해당하면 false(LFN 필요).
bool kTryExactShortName(const char* name, uint32_t nameLen, char out11[11], uint8_t* outNtReserved) {
    if (nameLen == 0 || nameLen > 12) {
        return false;
    }
    uint32_t dot = nameLen;
    uint32_t dotCount = 0;
    for (uint32_t i = 0; i < nameLen; ++i) {
        if (name[i] == '.') {
            dot = i;
            ++dotCount;
        }
    }
    if (dotCount > 1) {
        return false;
    }
    const uint32_t baseLen = (dot < nameLen) ? dot : nameLen;
    const uint32_t extLen = (dot < nameLen) ? (nameLen - dot - 1) : 0;
    if (baseLen == 0 || baseLen > 8 || extLen > 3) {
        return false;
    }
    if (dot < nameLen && extLen == 0) {
        return false;  // "NAME." 처럼 점만 있고 확장자가 없는 꼬리 - LFN으로
    }

    bool baseHasLower = false;
    bool baseHasUpper = false;
    for (uint32_t i = 0; i < baseLen; ++i) {
        const char c = name[i];
        if (!kIsValid83Char(c)) {
            return false;
        }
        if (c >= 'a' && c <= 'z') {
            baseHasLower = true;
        }
        if (c >= 'A' && c <= 'Z') {
            baseHasUpper = true;
        }
    }
    bool extHasLower = false;
    bool extHasUpper = false;
    for (uint32_t i = 0; i < extLen; ++i) {
        const char c = name[dot + 1 + i];
        if (!kIsValid83Char(c)) {
            return false;
        }
        if (c >= 'a' && c <= 'z') {
            extHasLower = true;
        }
        if (c >= 'A' && c <= 'Z') {
            extHasUpper = true;
        }
    }
    if ((baseHasLower && baseHasUpper) || (extHasLower && extHasUpper)) {
        return false;  // 이름부/확장자 안에서 대소문자가 섞임 - NT 비트로 표현 불가
    }

    auto toUpper = [](char c) -> char { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; };
    for (uint32_t i = 0; i < 11; ++i) {
        out11[i] = ' ';
    }
    for (uint32_t i = 0; i < baseLen; ++i) {
        out11[i] = toUpper(name[i]);
    }
    for (uint32_t i = 0; i < extLen; ++i) {
        out11[8 + i] = toUpper(name[dot + 1 + i]);
    }
    uint8_t nt = 0;
    if (baseHasLower) {
        nt |= kNtCaseLowerBase;
    }
    if (extHasLower) {
        nt |= kNtCaseLowerExt;
    }
    *outNtReserved = nt;
    return true;
}

// [신규, 2026-09-23, PN-3D39A53C] LFN이 필요할 때 스펙의 "~N" 관례
// (fatgen103/Linux fs/fat와 동일한 결)로 8.3 별칭을 생성한다(순수
// 계산) - collisionIndex(1부터)를 호출부가 반복 시도하며 실제 충돌
// 여부는 디렉터리를 스캔해 확인한다. 유효하지 않은 문자는 '_'로
// 대체(스펙의 "OS별 대체 문자" 관례를 단순화).
void kGenerateShortAlias(const char* name, uint32_t nameLen, uint32_t collisionIndex, char out11[11]) {
    for (uint32_t i = 0; i < 11; ++i) {
        out11[i] = ' ';
    }
    uint32_t dot = nameLen;
    for (uint32_t i = 0; i < nameLen; ++i) {
        if (name[i] == '.') {
            dot = i;  // 마지막 점 기준(스펙 관례)
        }
    }
    const uint32_t baseLen = (dot < nameLen) ? dot : nameLen;
    const uint32_t extLen = (dot < nameLen) ? (nameLen - dot - 1) : 0;

    char tail[8];
    uint32_t tailLen = 0;
    tail[tailLen++] = '~';
    char digits[7];
    uint32_t digitCount = 0;
    uint32_t v = collisionIndex;
    do {
        digits[digitCount++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v > 0 && digitCount < sizeof(digits));
    for (uint32_t i = 0; i < digitCount; ++i) {
        tail[tailLen++] = digits[digitCount - 1 - i];
    }

    auto toUpper = [](char c) -> char { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; };
    const uint32_t keepBase = (baseLen > 8 - tailLen) ? (8 - tailLen) : baseLen;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < keepBase; ++i) {
        const char c = name[i];
        out11[pos++] = kIsValid83Char(c) ? toUpper(c) : '_';
    }
    for (uint32_t i = 0; i < tailLen; ++i) {
        out11[pos++] = tail[i];
    }
    const uint32_t keepExt = (extLen > 3) ? 3 : extLen;
    for (uint32_t i = 0; i < keepExt; ++i) {
        const char c = name[dot + 1 + i];
        out11[8 + i] = kIsValid83Char(c) ? toUpper(c) : '_';
    }
}

// [신규, 2026-09-23, PN-3D39A53C] longName(nameLen바이트, ASCII 가정 -
// kLfnSlotChars의 읽기 쪽과 동일한 v1 스코프 컷)을 위한 LFN 슬롯
// totalSlots개를 outSlots[0..totalSlots-1]에 채운다(순수 계산, I/O
// 없음) - outSlots[0]이 디렉터리에 가장 먼저(짧은 엔트리에서 가장
// 먼 위치) 쓰여야 할, 가장 높은 시퀀스 번호의 슬롯이다(스펙의 역순
// 배치 관례 그대로 - kAccumulateLfn이 읽는 순서와 대칭).
void kBuildLfnSlots(const char* longName, uint32_t nameLen, uint8_t checksum, LfnSlot* outSlots,
                     uint32_t totalSlots) {
    for (uint32_t slotIdx = 0; slotIdx < totalSlots; ++slotIdx) {
        const uint32_t seq = totalSlots - slotIdx;  // outSlots[0] = 가장 높은 seq
        LfnSlot& slot = outSlots[slotIdx];
        slot.id = static_cast<uint8_t>(seq | ((seq == totalSlots) ? kLfnLastEntryFlag : 0));
        slot.attr = kAttrLongName;
        slot.slotType = 0;
        slot.checksum = checksum;
        slot.startCluster = 0;

        uint16_t chars[kLfnCharsPerSlot];
        const uint32_t charBase = (seq - 1) * kLfnCharsPerSlot;
        for (uint32_t i = 0; i < kLfnCharsPerSlot; ++i) {
            const uint32_t srcIdx = charBase + i;
            if (srcIdx < nameLen) {
                chars[i] = static_cast<uint16_t>(static_cast<uint8_t>(longName[srcIdx]));
            } else if (srcIdx == nameLen) {
                chars[i] = 0x0000;
            } else {
                chars[i] = 0xFFFF;
            }
        }
        memcpy(slot.name0_4, chars, sizeof(slot.name0_4));
        memcpy(slot.name5_10, chars + 5, sizeof(slot.name5_10));
        memcpy(slot.name11_12, chars + 11, sizeof(slot.name11_12));
    }
}

bool kNameMatches(const DirEntry& e, const char normalized11[11]) {
    char raw[11];
    memcpy(raw, e.name, 8);
    memcpy(raw + 8, e.ext, 3);
    if (static_cast<uint8_t>(raw[0]) == kNameEscapedE5) {
        raw[0] = static_cast<char>(kNameDeletedMarker);
    }
    for (uint32_t i = 0; i < 11; ++i) {
        if (raw[i] != normalized11[i]) {
            return false;
        }
    }
    return true;
}

// slab 버퍼 RAII - ext4_driver.cpp의 SlabBuf와 동일(코루틴 지역
// 변수는 co_return/조기 종료 어느 경로로도 프레임 소멸 시 소멸자가
// 정확히 불린다는 이 코드베이스의 기존 관례에 의존).
class SlabBuf {
public:
    explicit SlabBuf(uint32_t size)
        : _size(size), _ptr(static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(size))) {}
    ~SlabBuf() {
        if (_ptr) {
            kernel::GenericSlabAllocator::free(_ptr, _size);
        }
    }
    SlabBuf(const SlabBuf&) = delete;
    SlabBuf& operator=(const SlabBuf&) = delete;
    uint8_t* get() const { return _ptr; }
    explicit operator bool() const { return _ptr != nullptr; }

private:
    uint32_t _size;
    uint8_t* _ptr;
};

// 장치 블록 크기가 뭐든 섹터 단위(sectorSize바이트, sectorStart부터
// sectorCount개)로 co_await 가능한 읽기 AsyncTask를 제출한다 -
// ext4_driver.cpp의 kSubmitReadExtBlocks와 동일한 LBA 변환.
kernel::AsyncTask* kSubmitReadSectors(fs::BlockDevice* device, uint32_t sectorSize, uint64_t sectorStart,
                                       uint32_t sectorCount, void* buf, fs::BlockIoResult* outResult) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || sectorSize % devBlockSize != 0) {
        return nullptr;
    }
    const kernel::uint32_t devBlocksPerSector = sectorSize / devBlockSize;
    return device->submitReadBlocks(sectorStart * devBlocksPerSector, buf, sectorCount * devBlocksPerSector,
                                     outResult);
}

// [신규, 2026-09-23, PN-9D6FE4B6 준비 작업] kSubmitReadSectors의 쓰기
// 버전 - §3.4 FAT 엔트리 갱신/§4.2 데이터 클러스터 쓰기에 공용으로
// 쓴다.
kernel::AsyncTask* kSubmitWriteSectors(fs::BlockDevice* device, uint32_t sectorSize, uint64_t sectorStart,
                                        uint32_t sectorCount, const void* buf, fs::BlockIoResult* outResult) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || sectorSize % devBlockSize != 0) {
        return nullptr;
    }
    const kernel::uint32_t devBlocksPerSector = sectorSize / devBlockSize;
    return device->submitWriteBlocks(sectorStart * devBlocksPerSector, buf, sectorCount * devBlocksPerSector,
                                      outResult);
}

// 데이터 클러스터 번호를 장치 섹터 번호로 바꾼다(순수 계산) - vfat.cpp에
// 있던 옛 Fat32Volume::clusterToSector와 동일한 판별.
bool kClusterToSector(uint32_t dataStartSector, uint32_t sectorsPerCluster, uint32_t cluster, uint32_t* outSector) {
    if (cluster < kFirstDataCluster) {
        return false;
    }
    *outSector = dataStartSector + (cluster - kFirstDataCluster) * sectorsPerCluster;
    return true;
}

// cluster번째 FAT 엔트리(32비트)가 있는 섹터/그 섹터 안 바이트
// 오프셋을 계산한다(순수 계산) - 실제로 그 섹터를 읽는 건 호출부
// (onExec)의 몫(합성 불가 제약, 파일 상단 문서 주석 참고).
void kFatEntryLocation(uint32_t fatStartSector, uint32_t bytesPerSector, uint32_t cluster, uint32_t* outSector,
                        uint32_t* outByteOffset) {
    const uint64_t fatByteOffset = static_cast<uint64_t>(cluster) * 4;
    *outSector = fatStartSector + static_cast<uint32_t>(fatByteOffset / bytesPerSector);
    *outByteOffset = static_cast<uint32_t>(fatByteOffset % bytesPerSector);
}

enum class ChainStep { Next, End, Invalid };

// raw 32비트 FAT 엔트리를 해석한다(순수 계산, I/O 없음) - vfat.cpp에
// 있던 옛 Fat32Volume::nextCluster와 동일한 판별.
ChainStep kInterpretFatEntry(uint32_t raw, uint32_t* outNext) {
    const uint32_t entry = raw & kFatEntryMask;
    if (entry == 0 || entry == kFatBadCluster) {
        return ChainStep::Invalid;
    }
    if (entry >= kFatEocMin) {
        return ChainStep::End;
    }
    *outNext = entry;
    return ChainStep::Next;
}

// [신규, 2026-09-23, PN-9D6FE4B6 준비 작업 - §3.4] 새 FAT 엔트리 값을
// 기존 raw 32비트 값 위에 인코딩한다(순수 계산, I/O 없음) - kFatEntryMask
// (하위 28비트)만 바꾸고 상위 4예약비트는 원래 값 그대로 보존한다
// (스펙 관례 - 이 프로젝트가 그 예약 비트를 쓸 일이 없어도 다른
// 구현이 거기 뭔가 채워 뒀을 가능성을 존중, kInterpretFatEntry의
// 디코딩과 대칭).
uint32_t kEncodeFatEntry(uint32_t oldRaw, uint32_t newValue) {
    return (oldRaw & ~kFatEntryMask) | (newValue & kFatEntryMask);
}

bool kNamesEqualCi(const char* a, uint32_t aLen, const char* b, uint32_t bLen) {
    if (aLen != bLen) {
        return false;
    }
    auto toUpper = [](char c) -> char { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; };
    for (uint32_t i = 0; i < aLen; ++i) {
        if (toUpper(a[i]) != toUpper(b[i])) {
            return false;
        }
    }
    return true;
}

// [신규, 2026-09-23, PN-1A224EC2] LFN 슬롯 하나에서 UTF-16 코드유닛
// 13개를 뽑아 ASCII 범위(<0x80)만 char로 근사 변환한다 - v1 스코프
// 컷: BMP 밖 문자/서로게이트 쌍/비-ASCII 문자는 지원 안 함('?'로
// 대체). 이 커널의 경로 API 전체가 8비트 char* 하나뿐이라(다른 곳
// 어디에도 유니코드 처리가 없음) 이 범위가 자연스러운 첫 증분이다.
// 0x0000(이름 끝)/0xFFFF(패딩)를 만나면 그 자리에서 멈춘다.
uint32_t kLfnSlotChars(const LfnSlot& slot, char out[kLfnCharsPerSlot]) {
    uint16_t units[kLfnCharsPerSlot];
    memcpy(units, slot.name0_4, sizeof(slot.name0_4));
    memcpy(units + 5, slot.name5_10, sizeof(slot.name5_10));
    memcpy(units + 11, slot.name11_12, sizeof(slot.name11_12));
    uint32_t n = 0;
    for (uint32_t i = 0; i < kLfnCharsPerSlot; ++i) {
        if (units[i] == 0x0000 || units[i] == 0xFFFF) {
            break;
        }
        out[n++] = (units[i] < 0x80) ? static_cast<char>(units[i]) : '?';
    }
    return n;
}

// [신규, 2026-09-23, PN-1A224EC2] 디렉터리 클러스터 하나를 스캔하는
// 동안의 LFN 누적 상태 - 짧은 이름 엔트리 직전에 역순(높은 시퀀스
// 번호부터)으로 나열되는 조각들을 순서/체크섬이 맞는 동안만 계속
// 쌓고, 어긋나면(손상되었거나 잘려나간 체인) 무효화한다. 클러스터
// 경계를 넘는 LFN 체인은 v1 범위 밖(디렉터리 스캔 루프 자체가
// 클러스터 단위라 이 구조체도 클러스터마다 새로 초기화된다) -
// 실제로는 극히 드문 경우(체인 하나가 최대 20개 엔트리=640바이트,
// 일반적인 4KiB 클러스터의 극히 일부)라 흔한 케이스에 영향 없다.
struct LfnState {
    char fragments[kLfnMaxSlots][kLfnCharsPerSlot];
    uint32_t fragLen[kLfnMaxSlots];
    uint32_t highestSeq = 0;
    uint32_t nextExpectedSeq = 0;
    uint8_t checksum = 0;
    bool active = false;
};

void kAccumulateLfn(LfnState* s, const LfnSlot& slot) {
    const uint8_t seq = slot.id & kLfnSeqMask;
    if (seq == 0 || seq > kLfnMaxSlots) {
        s->active = false;
        return;
    }
    if (slot.id & kLfnLastEntryFlag) {
        s->active = true;
        s->highestSeq = seq;
        s->nextExpectedSeq = seq;
        s->checksum = slot.checksum;
    } else if (!s->active || seq != s->nextExpectedSeq - 1 || slot.checksum != s->checksum) {
        s->active = false;
        return;
    }
    s->fragLen[seq - 1] = kLfnSlotChars(slot, s->fragments[seq - 1]);
    s->nextExpectedSeq = seq;
}

// 짧은 이름 엔트리에 도달했을 때 누적된 LFN을 조립한다 - 체크섬이
// 그 짧은 이름과 일치하고 seq 1..highestSeq가 전부 채워졌을 때만
// 유효(체크섬 불일치는 고아가 된 LFN 조각 뒤에 무관한 짧은 엔트리가
// 온 경우 - 스펙이 명시한 무결성 검증).
bool kFinishLfn(const LfnState& s, const DirEntry& shortEntry, char* out, uint32_t outCap, uint32_t* outLen) {
    if (!s.active || s.highestSeq == 0 || s.nextExpectedSeq != 1) {
        return false;
    }
    char name11[11];
    memcpy(name11, shortEntry.name, 8);
    memcpy(name11 + 8, shortEntry.ext, 3);
    if (kLfnChecksum(name11) != s.checksum) {
        return false;
    }
    uint32_t pos = 0;
    for (uint32_t seq = 1; seq <= s.highestSeq; ++seq) {
        for (uint32_t i = 0; i < s.fragLen[seq - 1] && pos < outCap; ++i) {
            out[pos++] = s.fragments[seq - 1][i];
        }
    }
    *outLen = pos;
    return true;
}

enum class DirScanResult { Found, NotFoundContinue, EndOfDir };

// 디렉터리 데이터 클러스터 하나 안에서 8.3 정규화 이름 또는(있다면)
// LFN 긴 이름과 일치하는 엔트리를 찾는다(순수 계산, I/O 없음) -
// vfat.cpp에 있던 옛 Fat32Volume::findDirEntry의 블록-스캔 부분만
// 뽑은 버전. [갱신, 2026-09-23, PN-1A224EC2] origSeg/origSegLen(경로
// 세그먼트 원문, 정규화 전)이 짧은 엔트리 직전에 조립된 LFN과
// 대소문자 무시 비교로 일치하면 8.3 이름 여부와 무관하게 매치한다.
DirScanResult kScanDirClusterForName(const uint8_t* clusterBuf, uint32_t bytesPerCluster,
                                      const char normalized11[11], const char* origSeg, uint32_t origSegLen,
                                      ResolvedEntry* out) {
    const uint32_t entriesPerCluster = bytesPerCluster / sizeof(DirEntry);
    const auto* entries = reinterpret_cast<const DirEntry*>(clusterBuf);
    LfnState lfn{};
    for (uint32_t i = 0; i < entriesPerCluster; ++i) {
        const DirEntry& e = entries[i];
        const uint8_t firstByte = static_cast<uint8_t>(e.name[0]);
        if (firstByte == kNameFreeRestMarker) {
            return DirScanResult::EndOfDir;
        }
        if (firstByte == kNameDeletedMarker) {
            lfn.active = false;
            continue;
        }
        if (e.attr == kAttrLongName) {
            kAccumulateLfn(&lfn, *reinterpret_cast<const LfnSlot*>(&e));
            continue;
        }
        if ((e.attr & kAttrVolumeId) != 0) {
            lfn.active = false;
            continue;
        }
        char longName[kLfnMaxSlots * kLfnCharsPerSlot];
        uint32_t longNameLen = 0;
        const bool hasLongName = kFinishLfn(lfn, e, longName, sizeof(longName), &longNameLen);
        lfn.active = false;
        if ((hasLongName && kNamesEqualCi(longName, longNameLen, origSeg, origSegLen)) ||
            kNameMatches(e, normalized11)) {
            out->firstCluster = kFatFirstCluster(e);
            out->isDir = (e.attr & kAttrDirectory) != 0;
            out->fileSize = out->isDir ? 0 : e.fileSize;
            // [신규, 2026-09-23, PN-9D6FE4B6] entryCluster는 이 함수가
            // 스캔 중인 클러스터를 모르므로(호출부만 앎) 호출부가 직접
            // 채운다 - 이 함수는 클러스터 안에서의 바이트 오프셋만 안다.
            out->entryByteOffset = i * static_cast<uint32_t>(sizeof(DirEntry));
            return DirScanResult::Found;
        }
    }
    return DirScanResult::NotFoundContinue;
}

// 디렉터리 데이터 클러스터 하나 안에서 0-based index를 찾는다(순수
// 계산, I/O 없음) - vfat.cpp에 있던 옛 Fat32Volume::readdirAt의
// 블록-스캔 부분만 뽑은 버전. [갱신, 2026-09-23, PN-1A224EC2] 짧은
// 엔트리 직전에 LFN이 조립되면(실제 vfat 관례대로) 8.3 대신 그
// 긴 이름을 돌려준다.
DirScanResult kScanDirClusterForIndex(const uint8_t* clusterBuf, uint32_t bytesPerCluster, uint64_t target,
                                       uint64_t* seen, char* nameOut, uint32_t nameOutCap, uint32_t* outNameLen,
                                       bool* outIsDir) {
    const uint32_t entriesPerCluster = bytesPerCluster / sizeof(DirEntry);
    const auto* entries = reinterpret_cast<const DirEntry*>(clusterBuf);
    LfnState lfn{};
    for (uint32_t i = 0; i < entriesPerCluster; ++i) {
        const DirEntry& e = entries[i];
        const uint8_t firstByte = static_cast<uint8_t>(e.name[0]);
        if (firstByte == kNameFreeRestMarker) {
            return DirScanResult::EndOfDir;
        }
        if (firstByte == kNameDeletedMarker) {
            lfn.active = false;
            continue;
        }
        if (e.attr == kAttrLongName) {
            kAccumulateLfn(&lfn, *reinterpret_cast<const LfnSlot*>(&e));
            continue;
        }
        if ((e.attr & kAttrVolumeId) != 0) {
            lfn.active = false;
            continue;
        }
        char longName[kLfnMaxSlots * kLfnCharsPerSlot];
        uint32_t longNameLen = 0;
        const bool hasLongName = kFinishLfn(lfn, e, longName, sizeof(longName), &longNameLen);
        lfn.active = false;
        if (*seen == target) {
            uint32_t written = 0;
            if (hasLongName) {
                for (uint32_t k = 0; k < longNameLen && written < nameOutCap; ++k, ++written) {
                    nameOut[written] = longName[k];
                }
            } else {
                // [신규, 2026-09-23, PN-8CACD042] NT/VFAT 대소문자 확장 -
                // 온디스크는 대문자로 유지된 채 ntReserved 비트만으로
                // "이름"/"확장자" 부분을 각각 독립적으로 소문자 표시할지
                // 결정한다(vfat.h의 kNtCaseLowerBase/kNtCaseLowerExt
                // 문서 주석 참고). LFN이 있으면 이 변환은 아예 안 거친다
                // - 긴 이름은 이미 그 자체로 정확한 대소문자를 담고 있다.
                auto toLower = [](char c) -> char { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
                const bool lowerBase = (e.ntReserved & kNtCaseLowerBase) != 0;
                const bool lowerExt = (e.ntReserved & kNtCaseLowerExt) != 0;
                uint32_t nameLen = 8;
                while (nameLen > 0 && e.name[nameLen - 1] == ' ') {
                    --nameLen;
                }
                uint32_t extLen = 3;
                while (extLen > 0 && e.ext[extLen - 1] == ' ') {
                    --extLen;
                }
                for (uint32_t k = 0; k < nameLen && written < nameOutCap; ++k, ++written) {
                    const char c = (static_cast<uint8_t>(e.name[0]) == kNameEscapedE5 && k == 0)
                                       ? static_cast<char>(kNameDeletedMarker)
                                       : e.name[k];
                    nameOut[written] = lowerBase ? toLower(c) : c;
                }
                if (extLen > 0 && written < nameOutCap) {
                    nameOut[written++] = '.';
                    for (uint32_t k = 0; k < extLen && written < nameOutCap; ++k, ++written) {
                        nameOut[written] = lowerExt ? toLower(e.ext[k]) : e.ext[k];
                    }
                }
            }
            *outNameLen = written;
            *outIsDir = (e.attr & kAttrDirectory) != 0;
            return DirScanResult::Found;
        }
        ++(*seen);
    }
    return DirScanResult::NotFoundContinue;
}

// [신규, 2026-09-23, PN-547EF839] FAT[1] 예약 엔트리의 clean-shutdown
// 비트(vfat.h의 kFat32DirtyBitCleanShutdown 문서 주석 참고)를
// numFats개 FAT 사본 전부에 동일하게 반영한다 - §3.4 "numFats가
// 2 이상이면 모든 FAT 사본에 반영"과 같은 원칙. `Fat32Driver::mount()`/
// `remount()`는 `MountTable::mountKernel()` 등록 전/후 한 번만 동기
// 호출되는 준비 단계라 여기서도 `fs::BlockDevice`의 동기 read/writeBlocks
// 를 그대로 쓴다(onExec 코루틴 안이 아니므로 안전 - vfat.h의
// Fat32Volume 클래스 문서 주석과 동일한 근거).
bool kSetFat32CleanShutdownBit(fs::BlockDevice* device, uint32_t bytesPerSector, uint32_t fatStartSector,
                                uint32_t numFats, uint32_t fatSize32, bool clean) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || bytesPerSector % devBlockSize != 0) {
        return false;
    }
    const kernel::uint32_t blocksPerSector = bytesPerSector / devBlockSize;
    SlabBuf buf(bytesPerSector);
    if (!buf) {
        return false;
    }
    const uint32_t fatEntryByteOffset = kReservedFatEntryIndex * 4;
    for (uint32_t fatIndex = 0; fatIndex < numFats; ++fatIndex) {
        const uint32_t sector = fatStartSector + fatIndex * fatSize32;
        if (!device->readBlocks(sector * blocksPerSector, blocksPerSector, buf.get())) {
            return false;
        }
        uint32_t raw = 0;
        memcpy(&raw, buf.get() + fatEntryByteOffset, sizeof(raw));
        if (clean) {
            raw |= kFat32DirtyBitCleanShutdown;
        } else {
            raw &= ~kFat32DirtyBitCleanShutdown;
        }
        memcpy(buf.get() + fatEntryByteOffset, &raw, sizeof(raw));
        if (!device->writeBlocks(sector * blocksPerSector, blocksPerSector, buf.get())) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool Fat32Driver::mount(fs::BlockDevice* device, bool readOnly) {
    if (!volume_.mount(device)) {
        return false;
    }
    mounted_ = true;
    readOnly_ = readOnly;
    // [신규, 2026-09-23, PN-547EF839] 쓰기 가능하게 마운트되는 순간
    // dirty로 표시한다(clean-shutdown 비트 클리어) - 읽기 전용
    // 마운트는 볼륨을 변경할 수 없으므로 건드리지 않는다. 이 호출이
    // 실패해도(예: 매체가 실제로 read-only 하드웨어) mount() 자체를
    // 실패시키지 않는다 - dirty 비트 관리는 부가 기능이지 마운트
    // 성공의 전제조건이 아니다.
    if (!readOnly) {
        kSetFat32CleanShutdownBit(volume_.device(), volume_.bytesPerSectorValue(), volume_.fatStartSectorValue(),
                                   volume_.numFatsValue(), volume_.fatSize32Value(), /*clean=*/false);
    }
    return true;
}

bool Fat32Driver::remount(bool writable) {
    // [갱신, 2026-09-23, PN-547EF839] writable로 처음 전환되는 순간에도
    // mount()와 동일하게 dirty 표시 - SP-8B6B8D25 §5.1의 "부팅 초기
    // 임시 읽기전용 마운트 → init이 나중에 remount"라는 흐름 자체가
    // 바로 "이제부터 실제로 쓰기가 시작된다"는 신호다.
    const bool wasReadOnly = readOnly_;
    readOnly_ = !writable;
    if (writable && wasReadOnly) {
        kSetFat32CleanShutdownBit(volume_.device(), volume_.bytesPerSectorValue(), volume_.fatStartSectorValue(),
                                   volume_.numFatsValue(), volume_.fatSize32Value(), /*clean=*/false);
    }
    return true;
}

kernel::AsyncExecCoro Fat32Driver::onExec(kernel::AsyncTask*, void* argsRaw) {
    const auto op = *static_cast<const kernel::KernelFsOpCode*>(argsRaw);
    fs::BlockDevice* device = volume_.device();
    const uint32_t bytesPerSector = volume_.bytesPerSectorValue();
    const uint32_t sectorsPerCluster = volume_.sectorsPerClusterValue();
    const uint32_t bytesPerCluster = volume_.bytesPerClusterValue();
    const uint32_t fatStartSector = volume_.fatStartSectorValue();
    const uint32_t dataStartSector = volume_.dataStartSectorValue();

    switch (op) {
        case kernel::KernelFsOpCode::Open: {
            auto* args = static_cast<kernel::KernelFsOpenArgs*>(argsRaw);
            uint32_t currentCluster = volume_.rootFirstCluster();
            bool currentIsDir = true;
            uint64_t currentFileSize = 0;
            // [신규, 2026-09-23, PN-9D6FE4B6] 루트는 부모 디렉터리
            // 엔트리 자체가 없으므로 entryValid=false로 시작 - 경로가
            // 비어 있으면(루트 자신을 여는 경우) 이 상태 그대로 아래로
            // 내려간다.
            bool currentEntryValid = false;
            uint32_t currentEntryCluster = 0;
            uint32_t currentEntryByteOffset = 0;
            bool failed = false;

            uint32_t pos = 0;
            while (pos < args->relPathLen && !failed) {
                while (pos < args->relPathLen && args->relPath[pos] == '/') {
                    ++pos;
                }
                if (pos >= args->relPathLen) {
                    break;
                }
                const uint32_t segStart = pos;
                while (pos < args->relPathLen && args->relPath[pos] != '/') {
                    ++pos;
                }
                const uint32_t segLen = pos - segStart;
                if (!currentIsDir) {
                    failed = true;
                    break;
                }

                char normalized[11];
                kNormalizeTo83(args->relPath + segStart, segLen, normalized);

                ResolvedEntry matched;
                bool foundInThisDir = false;
                uint32_t scanCluster = currentCluster;
                while (true) {
                    uint32_t sector = 0;
                    if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                        break;
                    }
                    SlabBuf clusterBuf(bytesPerCluster);
                    if (!clusterBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                           &ioResult);
                    if (!ioTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        failed = true;
                        break;
                    }

                    const DirScanResult scanResult = kScanDirClusterForName(
                        clusterBuf.get(), bytesPerCluster, normalized, args->relPath + segStart, segLen, &matched);
                    if (scanResult == DirScanResult::Found) {
                        foundInThisDir = true;
                        break;
                    }
                    if (scanResult == DirScanResult::EndOfDir) {
                        break;
                    }

                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        failed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t nextCluster = 0;
                    if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                        break;  // 체인 끝(EOC) 또는 손상 - "이 이름 없음"으로 처리
                    }
                    scanCluster = nextCluster;
                }

                if (!foundInThisDir) {
                    failed = true;
                    break;
                }
                matched.entryCluster = scanCluster;
                currentCluster = matched.firstCluster;
                currentIsDir = matched.isDir;
                currentFileSize = matched.fileSize;
                currentEntryValid = true;
                currentEntryCluster = matched.entryCluster;
                currentEntryByteOffset = matched.entryByteOffset;
            }

            // [구현, 2026-09-23, PN-740005DF 항목1, SP-2AAD7C8D §9.3]
            // `OpenFlags::Truncate`(mount_table.h) - open()이 규정한
            // 대로 별도 syscall/op이 아니라 이 플래그 하나로 노출된다
            // (POSIX O_TRUNC와 동일한 결). 이 enum 자체가 §9.3에
            // 명시돼 있었는데 실제 코드 어디에도 정의/소비되지 않고
            // 있었다(RM-F2DAFF66 §1-V로 기록) - 대상이 일반 파일이고
            // (디렉터리는 무시), 쓰기 가능 마운트이고, 이미 클러스터가
            // 있으면(비어 있으면 할 일 없음) 체인을 전부 반납 + 크기를
            // 0으로 만든다. Open() 자신의 성공/실패에 영향을 주므로
            // 핸들 슬롯을 채우기 전, 위 경로 탐색 직후 처리한다.
            // 경로 탐색 실패는 항상 NotFound(기존 관례) - truncate
            // 자체가 실패하면 더 정확한 사유로 덮어쓴다.
            kernel::VfsError openFailReason = kernel::VfsError::NotFound;
            bool truncateFailed = false;
            if (!failed && !currentIsDir && currentEntryValid &&
                (args->flags & static_cast<uint32_t>(kernel::OpenFlags::Truncate)) != 0) {
                if (readOnly_) {
                    failed = true;
                    openFailReason = kernel::VfsError::PermissionDenied;
                } else if (currentCluster != 0) {
                    uint32_t c = currentCluster;
                    const uint32_t numFats = volume_.numFatsValue();
                    const uint32_t fatSize32 = volume_.fatSize32Value();
                    while (c != 0 && !truncateFailed) {
                        uint32_t nextC = 0;
                        ChainStep step = ChainStep::Invalid;
                        {
                            uint32_t fatSector = 0;
                            uint32_t fatByteOffset = 0;
                            kFatEntryLocation(fatStartSector, bytesPerSector, c, &fatSector, &fatByteOffset);
                            SlabBuf fatBuf(bytesPerSector);
                            if (!fatBuf) {
                                truncateFailed = true;
                            } else {
                                fs::BlockIoResult fatIoResult;
                                kernel::AsyncTask* fatIoTask = kSubmitReadSectors(
                                    device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                                if (!fatIoTask) {
                                    truncateFailed = true;
                                } else {
                                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                                    if (!fatIoResult.ok) {
                                        truncateFailed = true;
                                    } else {
                                        uint32_t raw = 0;
                                        memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                                        step = kInterpretFatEntry(raw, &nextC);
                                    }
                                }
                            }
                        }
                        for (uint32_t fatIndex = 0; fatIndex < numFats && !truncateFailed; ++fatIndex) {
                            uint32_t sector = 0;
                            uint32_t byteOffset = 0;
                            kFatEntryLocation(fatStartSector + fatIndex * fatSize32, bytesPerSector, c, &sector,
                                               &byteOffset);
                            SlabBuf buf(bytesPerSector);
                            if (!buf) {
                                truncateFailed = true;
                                break;
                            }
                            fs::BlockIoResult readResult;
                            kernel::AsyncTask* readTask =
                                kSubmitReadSectors(device, bytesPerSector, sector, 1, buf.get(), &readResult);
                            if (!readTask) {
                                truncateFailed = true;
                                break;
                            }
                            co_await kernel::AsyncTaskCoroAwaiter(readTask);
                            if (!readResult.ok) {
                                truncateFailed = true;
                                break;
                            }
                            uint32_t raw = 0;
                            memcpy(&raw, buf.get() + byteOffset, sizeof(raw));
                            const uint32_t newRaw = kEncodeFatEntry(raw, 0);
                            memcpy(buf.get() + byteOffset, &newRaw, sizeof(newRaw));
                            fs::BlockIoResult writeResult;
                            kernel::AsyncTask* writeTask =
                                kSubmitWriteSectors(device, bytesPerSector, sector, 1, buf.get(), &writeResult);
                            if (!writeTask) {
                                truncateFailed = true;
                                break;
                            }
                            co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                            if (!writeResult.ok) {
                                truncateFailed = true;
                                break;
                            }
                        }
                        if (truncateFailed || step != ChainStep::Next) {
                            break;
                        }
                        c = nextC;
                    }

                    if (!truncateFailed) {
                        uint32_t entrySector = 0;
                        if (!kClusterToSector(dataStartSector, sectorsPerCluster, currentEntryCluster,
                                               &entrySector)) {
                            truncateFailed = true;
                        } else {
                            entrySector += currentEntryByteOffset / bytesPerSector;
                            const uint32_t byteInSector = currentEntryByteOffset % bytesPerSector;
                            SlabBuf sectorBuf(bytesPerSector);
                            if (!sectorBuf) {
                                truncateFailed = true;
                            } else {
                                fs::BlockIoResult readResult;
                                kernel::AsyncTask* readTask = kSubmitReadSectors(
                                    device, bytesPerSector, entrySector, 1, sectorBuf.get(), &readResult);
                                if (!readTask) {
                                    truncateFailed = true;
                                } else {
                                    co_await kernel::AsyncTaskCoroAwaiter(readTask);
                                    if (!readResult.ok) {
                                        truncateFailed = true;
                                    } else {
                                        auto* entry = reinterpret_cast<DirEntry*>(sectorBuf.get() + byteInSector);
                                        entry->fileSize = 0;
                                        entry->fstClusHi = 0;
                                        entry->fstClusLo = 0;
                                        fs::BlockIoResult writeResult;
                                        kernel::AsyncTask* writeTask =
                                            kSubmitWriteSectors(device, bytesPerSector, entrySector, 1,
                                                                 sectorBuf.get(), &writeResult);
                                        if (!writeTask) {
                                            truncateFailed = true;
                                        } else {
                                            co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                                            if (!writeResult.ok) {
                                                truncateFailed = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (truncateFailed) {
                        failed = true;
                        openFailReason = kernel::VfsError::InvalidHandle;
                    } else {
                        currentCluster = 0;
                        currentFileSize = 0;
                    }
                }
            }

            if (failed) {
                args->result = kernel::OpenResult{kernel::FileHandle{}, false, openFailReason};
            } else {
                // [갱신, 2026-09-23, PN-9D6FE4B6, QU-E4E83A9A 답변] open-handle
                // 테이블에서 빈 슬롯을 찾아 채우고, 그 인덱스를 FileHandle로
                // 돌려준다(vfat_driver.h 문서 주석 - 이전의 "값 자체에
                // firstCluster+fileSize 직접 인코딩" 방식은 폐기).
                uint32_t slotIndex = kMaxOpenHandles;
                for (uint32_t i = 0; i < kMaxOpenHandles; ++i) {
                    if (!openHandles_[i].inUse) {
                        slotIndex = i;
                        break;
                    }
                }
                if (slotIndex == kMaxOpenHandles) {
                    args->result = kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NoSpace};
                } else {
                    OpenHandleEntry& slot = openHandles_[slotIndex];
                    slot.inUse = true;
                    slot.firstCluster = currentCluster;
                    slot.fileSize = currentFileSize;
                    slot.isDir = currentIsDir;
                    slot.entryValid = currentEntryValid;
                    slot.entryCluster = currentEntryCluster;
                    slot.entryByteOffset = currentEntryByteOffset;
                    args->result = kernel::OpenResult{kernel::FileHandle{slotIndex}, currentIsDir,
                                                       kernel::VfsError::None};
                }
            }
            break;
        }

        case kernel::KernelFsOpCode::Close: {
            // [갱신, 2026-09-23, PN-9D6FE4B6] open-handle 테이블 도입
            // 이후 - 이 핸들이 쓰던 슬롯을 반납한다. 범위 밖 인덱스나
            // 이미 닫힌 핸들은 조용히 무시(Ext4Driver의 Close도 실패
            // 반환 경로가 없다 - VFS 계층이 이미 유효한 핸들만 넘긴다는
            // 전제).
            auto* args = static_cast<kernel::KernelFsCloseArgs*>(argsRaw);
            const uint32_t index = static_cast<uint32_t>(args->handle.value);
            if (index < kMaxOpenHandles) {
                openHandles_[index].inUse = false;
            }
            break;
        }

        case kernel::KernelFsOpCode::Read: {
            auto* args = static_cast<kernel::KernelFsReadArgs*>(argsRaw);
            const uint32_t handleIndex = static_cast<uint32_t>(args->handle.value);
            if (handleIndex >= kMaxOpenHandles || !openHandles_[handleIndex].inUse) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            const uint32_t firstCluster = openHandles_[handleIndex].firstCluster;
            const uint64_t fileSize = openHandles_[handleIndex].fileSize;

            if (args->offset >= fileSize) {
                args->result = kernel::ReadResult{0, kernel::VfsError::None};  // EOF
                break;
            }
            uint64_t remaining = fileSize - args->offset;
            if (remaining > args->len) {
                remaining = args->len;
            }

            // 목표 오프셋이 속한 클러스터까지 체인을 선형으로 따라간다
            // (v1은 캐시 없음 - vfat.cpp에 있던 옛 readData와 동일한
            // 단순한 버전).
            uint32_t cluster = firstCluster;
            uint64_t clusterStartOffset = 0;
            bool chainBroken = false;
            while (clusterStartOffset + bytesPerCluster <= args->offset) {
                uint32_t fatSector = 0;
                uint32_t fatByteOffset = 0;
                kFatEntryLocation(fatStartSector, bytesPerSector, cluster, &fatSector, &fatByteOffset);
                SlabBuf fatBuf(bytesPerSector);
                if (!fatBuf) {
                    chainBroken = true;
                    break;
                }
                fs::BlockIoResult fatIoResult;
                kernel::AsyncTask* fatIoTask =
                    kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                if (!fatIoTask) {
                    chainBroken = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                if (!fatIoResult.ok) {
                    chainBroken = true;
                    break;
                }
                uint32_t raw = 0;
                memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                uint32_t nextCluster = 0;
                if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                    chainBroken = true;
                    break;
                }
                cluster = nextCluster;
                clusterStartOffset += bytesPerCluster;
            }

            if (chainBroken) {
                // 파일 크기보다 짧은 체인 - 손상되었거나 예상 밖이나,
                // 방어적으로 0 반환(vfat.cpp에 있던 옛 readData와 동일 관례).
                args->result = kernel::ReadResult{0, kernel::VfsError::None};
                break;
            }

            SlabBuf clusterBuf(bytesPerCluster);
            if (!clusterBuf) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            uint32_t totalCopied = 0;
            auto* out = static_cast<uint8_t*>(args->buf);
            bool ioFailed = false;
            while (remaining > 0 && !ioFailed) {
                uint32_t sector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, cluster, &sector)) {
                    ioFailed = true;
                    break;
                }
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                       &ioResult);
                if (!ioTask) {
                    ioFailed = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    ioFailed = true;
                    break;
                }

                const uint64_t curOffset = args->offset + totalCopied;
                const uint32_t offsetInCluster = static_cast<uint32_t>(curOffset - clusterStartOffset);
                const uint32_t chunk = static_cast<uint32_t>(
                    remaining < (bytesPerCluster - offsetInCluster) ? remaining : (bytesPerCluster - offsetInCluster));
                memcpy(out + totalCopied, clusterBuf.get() + offsetInCluster, chunk);
                totalCopied += chunk;
                remaining -= chunk;

                if (remaining > 0) {
                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, cluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t nextCluster = 0;
                    if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                        break;  // 체인 끝 - 지금까지 복사된 만큼만 반환
                    }
                    cluster = nextCluster;
                    clusterStartOffset += bytesPerCluster;
                }
            }
            args->result =
                kernel::ReadResult{totalCopied, ioFailed ? kernel::VfsError::InvalidHandle : kernel::VfsError::None};
            break;
        }

        case kernel::KernelFsOpCode::Write: {
            // [구현, 2026-09-23, PN-9D6FE4B6, QU-E4E83A9A 답변("(A) 별도
            // open-handle 테이블 도입")] libvfat 최초의 실제 쓰기 경로 -
            // 1) 기존 클러스터 체인 길이 확인, 2) writeEnd를 담기에
            // 모자라면 free 클러스터를 §3.4 힌트 기반 선형 스캔으로
            // 필요한 만큼 할당·링크(새 클러스터는 항상 0으로 초기화 -
            // 이전 파일 내용 유출 방지), 3) args->offset이 속한
            // 클러스터부터 청크 단위 read-modify-write, 4) fileSize/
            // firstCluster를 메모리(OpenHandleEntry)와 디스크 디렉터리
            // 엔트리 양쪽에 반영. Mkdir/Unlink/Rmdir(새 디렉터리 엔트리
            // 슬롯 할당/삭제)은 이 계획의 다음 증분으로 남겨 둔다(PN-9D6FE4B6
            // 참고) - Write는 이미 존재하는 디렉터리 엔트리의 위치만
            // 갱신하면 되므로 그 문제와 독립적으로 먼저 구현 가능했다.
            auto* args = static_cast<kernel::KernelFsWriteArgs*>(argsRaw);
            const uint32_t handleIndex = static_cast<uint32_t>(args->handle.value);
            if (handleIndex >= kMaxOpenHandles || !openHandles_[handleIndex].inUse) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            OpenHandleEntry& h = openHandles_[handleIndex];
            if (h.isDir) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            if (readOnly_) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            if (args->len == 0) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::None;
                break;
            }

            const uint64_t writeEnd = args->offset + args->len;
            const uint32_t numFats = volume_.numFatsValue();
            const uint32_t fatSize32 = volume_.fatSize32Value();
            const uint32_t clusterCount = volume_.clusterCountValue();

            bool failed = false;
            kernel::VfsError failReason = kernel::VfsError::InvalidHandle;

            // 1단계: 기존 체인 길이(클러스터 수)와 tail 클러스터를 구한다.
            uint32_t existingClusterCount = 0;
            uint32_t tailCluster = 0;
            uint32_t firstCluster = h.firstCluster;
            if (firstCluster != 0) {
                existingClusterCount = 1;
                uint32_t c = firstCluster;
                for (;;) {
                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, c, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        failed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t next = 0;
                    if (kInterpretFatEntry(raw, &next) == ChainStep::Next) {
                        c = next;
                        ++existingClusterCount;
                    } else {
                        tailCluster = c;
                        break;
                    }
                }
            }
            if (failed) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }

            // 2단계: writeEnd를 담기 모자라면 free 클러스터를 필요한
            // 만큼 할당·링크한다.
            uint64_t existingCapacity = static_cast<uint64_t>(existingClusterCount) * bytesPerCluster;
            SlabBuf zeroBuf(bytesPerCluster);
            if (!zeroBuf) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::NoSpace;
                break;
            }
            memset(zeroBuf.get(), 0, bytesPerCluster);

            while (!failed && existingCapacity < writeEnd) {
                uint32_t candidate = nextClusterScanHint_;
                if (candidate < kFirstDataCluster || candidate > clusterCount + 1) {
                    candidate = kFirstDataCluster;
                }
                bool foundFree = false;
                uint32_t newCluster = 0;
                uint32_t loadedFatSector = 0xFFFFFFFFu;
                SlabBuf scanBuf(bytesPerSector);
                if (!scanBuf) {
                    failed = true;
                    failReason = kernel::VfsError::NoSpace;
                    break;
                }
                for (uint32_t attempts = 0; attempts < clusterCount; ++attempts) {
                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, candidate, &fatSector, &fatByteOffset);
                    if (fatSector != loadedFatSector) {
                        fs::BlockIoResult scanIoResult;
                        kernel::AsyncTask* scanIoTask =
                            kSubmitReadSectors(device, bytesPerSector, fatSector, 1, scanBuf.get(), &scanIoResult);
                        if (!scanIoTask) {
                            failed = true;
                            failReason = kernel::VfsError::NoSpace;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(scanIoTask);
                        if (!scanIoResult.ok) {
                            failed = true;
                            failReason = kernel::VfsError::NoSpace;
                            break;
                        }
                        loadedFatSector = fatSector;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, scanBuf.get() + fatByteOffset, sizeof(raw));
                    if ((raw & kFatEntryMask) == 0) {
                        foundFree = true;
                        newCluster = candidate;
                        break;
                    }
                    candidate = (candidate >= clusterCount + 1) ? kFirstDataCluster : candidate + 1;
                }
                if (failed) {
                    break;
                }
                if (!foundFree) {
                    failed = true;
                    failReason = kernel::VfsError::NoSpace;
                    break;
                }
                nextClusterScanHint_ = (candidate >= clusterCount + 1) ? kFirstDataCluster : candidate + 1;

                // 새 클러스터를 EOC로 표시(모든 FAT 사본 먼저) - 그
                // 다음에야 이전 tail을 이 클러스터로 링크한다(순서
                // 방어적 - 링크가 먼저 걸리면 그 사이 다른 관측자가
                // 아직 EOC 아닌 새 클러스터를 가리키는 중간 상태를
                // 볼 여지가 있다).
                for (uint32_t fatIndex = 0; fatIndex < numFats && !failed; ++fatIndex) {
                    uint32_t sector = 0;
                    uint32_t byteOffset = 0;
                    kFatEntryLocation(fatStartSector + fatIndex * fatSize32, bytesPerSector, newCluster, &sector,
                                       &byteOffset);
                    SlabBuf buf(bytesPerSector);
                    if (!buf) {
                        failed = true;
                        failReason = kernel::VfsError::NoSpace;
                        break;
                    }
                    fs::BlockIoResult readResult;
                    kernel::AsyncTask* readTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, 1, buf.get(), &readResult);
                    if (!readTask) {
                        failed = true;
                        failReason = kernel::VfsError::NoSpace;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(readTask);
                    if (!readResult.ok) {
                        failed = true;
                        failReason = kernel::VfsError::NoSpace;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, buf.get() + byteOffset, sizeof(raw));
                    const uint32_t newRaw = kEncodeFatEntry(raw, kFatEocMin);
                    memcpy(buf.get() + byteOffset, &newRaw, sizeof(newRaw));
                    fs::BlockIoResult writeResult;
                    kernel::AsyncTask* writeTask =
                        kSubmitWriteSectors(device, bytesPerSector, sector, 1, buf.get(), &writeResult);
                    if (!writeTask) {
                        failed = true;
                        failReason = kernel::VfsError::NoSpace;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                    if (!writeResult.ok) {
                        failed = true;
                        failReason = kernel::VfsError::NoSpace;
                        break;
                    }
                }
                if (failed) {
                    break;
                }

                if (existingClusterCount > 0) {
                    for (uint32_t fatIndex = 0; fatIndex < numFats && !failed; ++fatIndex) {
                        uint32_t sector = 0;
                        uint32_t byteOffset = 0;
                        kFatEntryLocation(fatStartSector + fatIndex * fatSize32, bytesPerSector, tailCluster, &sector,
                                           &byteOffset);
                        SlabBuf buf(bytesPerSector);
                        if (!buf) {
                            failed = true;
                            failReason = kernel::VfsError::NoSpace;
                            break;
                        }
                        fs::BlockIoResult readResult;
                        kernel::AsyncTask* readTask =
                            kSubmitReadSectors(device, bytesPerSector, sector, 1, buf.get(), &readResult);
                        if (!readTask) {
                            failed = true;
                            failReason = kernel::VfsError::NoSpace;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(readTask);
                        if (!readResult.ok) {
                            failed = true;
                            failReason = kernel::VfsError::NoSpace;
                            break;
                        }
                        uint32_t raw = 0;
                        memcpy(&raw, buf.get() + byteOffset, sizeof(raw));
                        const uint32_t newRaw = kEncodeFatEntry(raw, newCluster);
                        memcpy(buf.get() + byteOffset, &newRaw, sizeof(newRaw));
                        fs::BlockIoResult writeResult;
                        kernel::AsyncTask* writeTask =
                            kSubmitWriteSectors(device, bytesPerSector, sector, 1, buf.get(), &writeResult);
                        if (!writeTask) {
                            failed = true;
                            failReason = kernel::VfsError::NoSpace;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                        if (!writeResult.ok) {
                            failed = true;
                            failReason = kernel::VfsError::NoSpace;
                            break;
                        }
                    }
                    if (failed) {
                        break;
                    }
                } else {
                    firstCluster = newCluster;  // 빈 파일이 처음으로 클러스터를 얻음
                }

                uint32_t dataSector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, newCluster, &dataSector)) {
                    failed = true;
                    failReason = kernel::VfsError::NoSpace;
                    break;
                }
                fs::BlockIoResult zeroResult;
                kernel::AsyncTask* zeroTask = kSubmitWriteSectors(device, bytesPerSector, dataSector,
                                                                   sectorsPerCluster, zeroBuf.get(), &zeroResult);
                if (!zeroTask) {
                    failed = true;
                    failReason = kernel::VfsError::NoSpace;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(zeroTask);
                if (!zeroResult.ok) {
                    failed = true;
                    failReason = kernel::VfsError::NoSpace;
                    break;
                }

                tailCluster = newCluster;
                ++existingClusterCount;
                existingCapacity += bytesPerCluster;
            }

            if (failed) {
                args->bytesWritten = 0;
                args->error = failReason;
                break;
            }

            // 3단계: args->offset이 속한 클러스터까지 체인을 따라간 뒤
            // 청크 단위로 read-modify-write.
            uint32_t cluster = firstCluster;
            uint64_t clusterStartOffset = 0;
            bool chainBroken = false;
            while (clusterStartOffset + bytesPerCluster <= args->offset) {
                uint32_t fatSector = 0;
                uint32_t fatByteOffset = 0;
                kFatEntryLocation(fatStartSector, bytesPerSector, cluster, &fatSector, &fatByteOffset);
                SlabBuf fatBuf(bytesPerSector);
                if (!fatBuf) {
                    chainBroken = true;
                    break;
                }
                fs::BlockIoResult fatIoResult;
                kernel::AsyncTask* fatIoTask =
                    kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                if (!fatIoTask) {
                    chainBroken = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                if (!fatIoResult.ok) {
                    chainBroken = true;
                    break;
                }
                uint32_t raw = 0;
                memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                uint32_t next = 0;
                if (kInterpretFatEntry(raw, &next) != ChainStep::Next) {
                    chainBroken = true;
                    break;
                }
                cluster = next;
                clusterStartOffset += bytesPerCluster;
            }
            if (chainBroken) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }

            SlabBuf clusterBuf(bytesPerCluster);
            if (!clusterBuf) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            uint32_t totalWritten = 0;
            const auto* src = static_cast<const uint8_t*>(args->buf);
            bool ioFailed = false;
            uint64_t remaining = args->len;
            while (remaining > 0 && !ioFailed) {
                uint32_t sector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, cluster, &sector)) {
                    ioFailed = true;
                    break;
                }

                const uint64_t curOffset = args->offset + totalWritten;
                const uint32_t offsetInCluster = static_cast<uint32_t>(curOffset - clusterStartOffset);
                const uint32_t chunk = static_cast<uint32_t>(
                    remaining < (bytesPerCluster - offsetInCluster) ? remaining : (bytesPerCluster - offsetInCluster));

                // 부분 쓰기 여부와 무관하게 항상 먼저 읽는다(v1 - 코드
                // 단순성 우선, RM-23F4B687 §4 취지 - 새로 할당된
                // 클러스터는 이미 0으로 초기화돼 있어 안전하게 정의된
                // 값을 읽는다).
                fs::BlockIoResult readResult;
                kernel::AsyncTask* readTask = kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster,
                                                                  clusterBuf.get(), &readResult);
                if (!readTask) {
                    ioFailed = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(readTask);
                if (!readResult.ok) {
                    ioFailed = true;
                    break;
                }

                memcpy(clusterBuf.get() + offsetInCluster, src + totalWritten, chunk);

                fs::BlockIoResult writeResult;
                kernel::AsyncTask* writeTask = kSubmitWriteSectors(device, bytesPerSector, sector, sectorsPerCluster,
                                                                    clusterBuf.get(), &writeResult);
                if (!writeTask) {
                    ioFailed = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                if (!writeResult.ok) {
                    ioFailed = true;
                    break;
                }

                totalWritten += chunk;
                remaining -= chunk;

                if (remaining > 0) {
                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, cluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t next = 0;
                    if (kInterpretFatEntry(raw, &next) != ChainStep::Next) {
                        // 2단계가 이미 충분히 확장했어야 하므로 여기
                        // 도달하면 안 됨 - 방어적으로 지금까지 쓴 만큼만
                        // 인정하고 멈춘다.
                        break;
                    }
                    cluster = next;
                    clusterStartOffset += bytesPerCluster;
                }
            }

            if (ioFailed) {
                args->bytesWritten = totalWritten;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }

            // 4단계: fileSize/firstCluster를 메모리 + 디스크 디렉터리
            // 엔트리 양쪽에 반영.
            const uint64_t newFileSize = (writeEnd > h.fileSize) ? writeEnd : h.fileSize;
            const bool clusterAssigned = (h.firstCluster == 0 && firstCluster != 0);
            if ((newFileSize != h.fileSize || clusterAssigned) && h.entryValid) {
                uint32_t entrySector = 0;
                if (kClusterToSector(dataStartSector, sectorsPerCluster, h.entryCluster, &entrySector)) {
                    entrySector += h.entryByteOffset / bytesPerSector;
                    const uint32_t byteInSector = h.entryByteOffset % bytesPerSector;
                    SlabBuf sectorBuf(bytesPerSector);
                    if (sectorBuf) {
                        fs::BlockIoResult readResult;
                        kernel::AsyncTask* readTask =
                            kSubmitReadSectors(device, bytesPerSector, entrySector, 1, sectorBuf.get(), &readResult);
                        if (readTask) {
                            co_await kernel::AsyncTaskCoroAwaiter(readTask);
                            if (readResult.ok) {
                                auto* entry = reinterpret_cast<DirEntry*>(sectorBuf.get() + byteInSector);
                                entry->fileSize = static_cast<uint32_t>(newFileSize);
                                entry->fstClusHi = static_cast<uint16_t>(firstCluster >> 16);
                                entry->fstClusLo = static_cast<uint16_t>(firstCluster & 0xFFFFu);
                                fs::BlockIoResult writeResult;
                                kernel::AsyncTask* writeTask = kSubmitWriteSectors(
                                    device, bytesPerSector, entrySector, 1, sectorBuf.get(), &writeResult);
                                if (writeTask) {
                                    co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                                }
                            }
                        }
                    }
                }
            }
            h.fileSize = newFileSize;
            h.firstCluster = firstCluster;

            args->bytesWritten = totalWritten;
            args->error = kernel::VfsError::None;
            break;
        }

        case kernel::KernelFsOpCode::Stat: {
            auto* args = static_cast<kernel::KernelFsStatArgs*>(argsRaw);
            uint32_t currentCluster = volume_.rootFirstCluster();
            bool currentIsDir = true;
            uint64_t currentFileSize = 0;
            bool failed = false;

            uint32_t pos = 0;
            while (pos < args->relPathLen && !failed) {
                while (pos < args->relPathLen && args->relPath[pos] == '/') {
                    ++pos;
                }
                if (pos >= args->relPathLen) {
                    break;
                }
                const uint32_t segStart = pos;
                while (pos < args->relPathLen && args->relPath[pos] != '/') {
                    ++pos;
                }
                const uint32_t segLen = pos - segStart;
                if (!currentIsDir) {
                    failed = true;
                    break;
                }

                char normalized[11];
                kNormalizeTo83(args->relPath + segStart, segLen, normalized);

                ResolvedEntry matched;
                bool foundInThisDir = false;
                uint32_t scanCluster = currentCluster;
                while (true) {
                    uint32_t sector = 0;
                    if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                        break;
                    }
                    SlabBuf clusterBuf(bytesPerCluster);
                    if (!clusterBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                           &ioResult);
                    if (!ioTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        failed = true;
                        break;
                    }

                    const DirScanResult scanResult = kScanDirClusterForName(
                        clusterBuf.get(), bytesPerCluster, normalized, args->relPath + segStart, segLen, &matched);
                    if (scanResult == DirScanResult::Found) {
                        foundInThisDir = true;
                        break;
                    }
                    if (scanResult == DirScanResult::EndOfDir) {
                        break;
                    }

                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        failed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t nextCluster = 0;
                    if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                        break;
                    }
                    scanCluster = nextCluster;
                }

                if (!foundInThisDir) {
                    failed = true;
                    break;
                }
                currentCluster = matched.firstCluster;
                currentIsDir = matched.isDir;
                currentFileSize = matched.fileSize;
            }

            if (failed) {
                args->error = kernel::VfsError::NotFound;
            } else {
                // ext4의 Stat과 달리 별도 "타깃 재조회"가 필요 없다 -
                // FAT은 크기/디렉터리 여부가 부모 디렉터리 엔트리
                // 자신에 이미 있어(vfat.h의 ResolvedEntry 문서 주석)
                // 위 탐색 루프의 마지막 매치 결과를 그대로 쓰면 된다.
                args->size = currentFileSize;
                args->isDirectory = currentIsDir;
                args->error = kernel::VfsError::None;
            }
            break;
        }

        case kernel::KernelFsOpCode::Mkdir: {
            // [구현, 2026-09-23, PN-9D6FE4B6] 부모 디렉터리 탐색은
            // Unlink/Rmdir와 동일(합성 불가 제약으로 중복) - 그 안에서
            // (1) 이름이 이미 있으면 AlreadyExists 거부 + 빈 슬롯(삭제
            // 마킹된 엔트리 또는 디렉터리 끝 마커) 탐색을 한 번에 처리,
            // (2) 새 디렉터리 자신의 클러스터 하나를 §3.4 방식으로
            // 할당해 "."/".." 초기화(부모가 루트면 ".."의 firstCluster는
            // 스펙 관례대로 0), (3) 부모의 빈 슬롯에 새 엔트리 기록.
            // v1은 부모 디렉터리 확장(새 클러스터 추가)은 지원하지
            // 않는다 - 기존 클러스터 체인에 빈 슬롯이 전혀 없으면
            // NoSpace(정직하게 남겨 둔 갭, PN-9D6FE4B6 참고).
            auto* args = static_cast<kernel::KernelFsMkdirArgs*>(argsRaw);
            if (readOnly_) {
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            uint32_t parentLen = 0;
            uint32_t leafStart = 0;
            uint32_t leafLen = 0;
            if (!kSplitParentAndLeaf(args->relPath, args->relPathLen, &parentLen, &leafStart, &leafLen)) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }

            uint32_t parentCluster = volume_.rootFirstCluster();
            bool parentFound = true;
            {
                uint32_t pos = 0;
                while (pos < parentLen) {
                    while (pos < parentLen && args->relPath[pos] == '/') {
                        ++pos;
                    }
                    if (pos >= parentLen) {
                        break;
                    }
                    const uint32_t segStart = pos;
                    while (pos < parentLen && args->relPath[pos] != '/') {
                        ++pos;
                    }
                    const uint32_t segLen = pos - segStart;

                    char normalized[11];
                    kNormalizeTo83(args->relPath + segStart, segLen, normalized);
                    ResolvedEntry matched;
                    bool foundInThisDir = false;
                    uint32_t scanCluster = parentCluster;
                    while (true) {
                        uint32_t sector = 0;
                        if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                            break;
                        }
                        SlabBuf clusterBuf(bytesPerCluster);
                        if (!clusterBuf) {
                            parentFound = false;
                            break;
                        }
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask = kSubmitReadSectors(device, bytesPerSector, sector,
                                                                        sectorsPerCluster, clusterBuf.get(), &ioResult);
                        if (!ioTask) {
                            parentFound = false;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            parentFound = false;
                            break;
                        }
                        const DirScanResult scanResult = kScanDirClusterForName(
                            clusterBuf.get(), bytesPerCluster, normalized, args->relPath + segStart, segLen, &matched);
                        if (scanResult == DirScanResult::Found) {
                            foundInThisDir = true;
                            break;
                        }
                        if (scanResult == DirScanResult::EndOfDir) {
                            break;
                        }
                        uint32_t fatSector = 0;
                        uint32_t fatByteOffset = 0;
                        kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                        SlabBuf fatBuf(bytesPerSector);
                        if (!fatBuf) {
                            parentFound = false;
                            break;
                        }
                        fs::BlockIoResult fatIoResult;
                        kernel::AsyncTask* fatIoTask =
                            kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                        if (!fatIoTask) {
                            parentFound = false;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                        if (!fatIoResult.ok) {
                            parentFound = false;
                            break;
                        }
                        uint32_t raw = 0;
                        memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                        uint32_t next = 0;
                        if (kInterpretFatEntry(raw, &next) != ChainStep::Next) {
                            break;
                        }
                        scanCluster = next;
                    }
                    if (!foundInThisDir || !matched.isDir) {
                        parentFound = false;
                        break;
                    }
                    parentCluster = matched.firstCluster;
                }
            }
            if (!parentFound) {
                args->error = kernel::VfsError::NotFound;
                break;
            }

            char leafNormalized[11];
            kNormalizeTo83(args->relPath + leafStart, leafLen, leafNormalized);

            // [신규, 2026-09-23, PN-3D39A53C] 이름이 8.3에 정확히 안
            // 들어가면(kTryExactShortName 실패) LFN이 필요 - 별칭
            // 후보 8개를 미리 계산해 두고, 부모 디렉터리 전체를 훑으며
            // (스캔 루프는 이미 전체 체인을 다 도니 그대로 재사용)
            // 이미 쓰인 8.3 이름과 충돌하는 후보를 걸러낸다.
            char exactShortName[11];
            uint8_t exactNtReserved = 0;
            const bool needsLfn = !kTryExactShortName(args->relPath + leafStart, leafLen, exactShortName, &exactNtReserved);
            uint32_t lfnSlotCount = 0;
            char aliasCandidates[8][11];
            bool aliasTaken[8] = {};
            if (needsLfn) {
                if (leafLen > kLfnMaxSlots * kLfnCharsPerSlot) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                lfnSlotCount = (leafLen + kLfnCharsPerSlot - 1) / kLfnCharsPerSlot;
                for (uint32_t a = 0; a < 8; ++a) {
                    kGenerateShortAlias(args->relPath + leafStart, leafLen, a + 1, aliasCandidates[a]);
                }
            }
            // LFN 슬롯(있다면) + 짧은 엔트리 1개가 반드시 "한 클러스터
            // 안에서" 연속된 빈 슬롯으로 확보돼야 한다(PN-1A224EC2가
            // 읽기 쪽에서 이미 확정한 "LFN 체인은 클러스터 경계를 넘지
            // 않는다" 제약을 쓰기 쪽에도 그대로 적용).
            const uint32_t neededSlots = needsLfn ? (lfnSlotCount + 1) : 1;
            if (neededSlots > bytesPerCluster / sizeof(DirEntry)) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }

            bool alreadyExists = false;
            bool freeSlotFound = false;
            uint32_t freeSlotCluster = 0;
            uint32_t freeSlotByteOffset = 0;
            bool ioFailed = false;
            // [신규, 2026-09-23, PN-740005DF 항목2] 빈 슬롯을 못 찾고
            // 체인이 끝났을 때, 그 체인의 tail 클러스터를 기억해 뒀다가
            // 새 클러스터를 이어붙이는 데 쓴다(부모 디렉터리 확장).
            uint32_t parentTailCluster = parentCluster;
            {
                uint32_t scanCluster = parentCluster;
                while (true) {
                    parentTailCluster = scanCluster;
                    uint32_t sector = 0;
                    if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                        break;
                    }
                    SlabBuf clusterBuf(bytesPerCluster);
                    if (!clusterBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                           &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                        break;
                    }

                    ResolvedEntry dummy;
                    const DirScanResult nameScan = kScanDirClusterForName(
                        clusterBuf.get(), bytesPerCluster, leafNormalized, args->relPath + leafStart, leafLen, &dummy);
                    if (nameScan == DirScanResult::Found) {
                        alreadyExists = true;
                        break;
                    }
                    {
                        // [갱신, 2026-09-23, PN-3D39A53C] 예전엔 빈 슬롯
                        // 1개만 찾으면 끝이었지만, LFN이 필요하면
                        // `neededSlots`개가 "이 클러스터 안에서" 연속으로
                        // 비어 있어야 한다 - 클러스터 경계를 넘는 런은
                        // 인정하지 않는다(runLength를 이 매 클러스터
                        // 반복마다 새로 0에서 시작). freeSlotFound가 이미
                        // true여도, needsLfn이면 이미 쓰인 8.3 이름과의
                        // 별칭 충돌을 걸러내기 위해 나머지 클러스터도
                        // 계속 훑어야 한다.
                        const uint32_t entriesPerCluster = bytesPerCluster / sizeof(DirEntry);
                        const auto* entries = reinterpret_cast<const DirEntry*>(clusterBuf.get());
                        uint32_t runLength = 0;
                        uint32_t runStartOffset = 0;
                        for (uint32_t i = 0; i < entriesPerCluster; ++i) {
                            const uint8_t firstByte = static_cast<uint8_t>(entries[i].name[0]);
                            const bool isFree = (firstByte == kNameDeletedMarker || firstByte == kNameFreeRestMarker);
                            if (isFree) {
                                if (runLength == 0) {
                                    runStartOffset = i * static_cast<uint32_t>(sizeof(DirEntry));
                                }
                                ++runLength;
                                if (!freeSlotFound && runLength >= neededSlots) {
                                    freeSlotFound = true;
                                    freeSlotCluster = scanCluster;
                                    freeSlotByteOffset = runStartOffset;
                                }
                            } else {
                                runLength = 0;
                                if (needsLfn && entries[i].attr != kAttrLongName) {
                                    for (uint32_t a = 0; a < 8; ++a) {
                                        if (!aliasTaken[a] && memcmp(entries[i].name, aliasCandidates[a], 8) == 0 &&
                                            memcmp(entries[i].ext, aliasCandidates[a] + 8, 3) == 0) {
                                            aliasTaken[a] = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (nameScan == DirScanResult::EndOfDir) {
                        break;
                    }

                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t next = 0;
                    if (kInterpretFatEntry(raw, &next) != ChainStep::Next) {
                        break;
                    }
                    scanCluster = next;
                }
            }
            if (ioFailed) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            if (alreadyExists) {
                args->error = kernel::VfsError::AlreadyExists;
                break;
            }

            // [신규, 2026-09-23, PN-3D39A53C] 실제로 쓸 8.3 이름 확정 -
            // needsLfn이면 8개 후보 중 부모 디렉터리 전체 스캔에서 안
            // 걸린 첫 번째를 쓴다(전부 걸리면 NoSpace) - 이후의 (비교적
            // 비싼) 디렉터리 확장/새 클러스터 할당보다 먼저 확인해
            // 헛수고를 줄인다. LFN이 있으면 정확한 대소문자는 LFN
            // 자신이 담으므로, 백업 8.3 엔트리엔 NT 케이스 비트를
            // 쓰지 않는 게 관례(exactShortName 경로만 그 비트를 씀).
            char finalShortName11[11];
            uint8_t finalNtReserved = 0;
            if (needsLfn) {
                int chosenAlias = -1;
                for (uint32_t a = 0; a < 8; ++a) {
                    if (!aliasTaken[a]) {
                        chosenAlias = static_cast<int>(a);
                        break;
                    }
                }
                if (chosenAlias < 0) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                memcpy(finalShortName11, aliasCandidates[chosenAlias], 11);
            } else {
                memcpy(finalShortName11, exactShortName, 11);
                finalNtReserved = exactNtReserved;
            }
            // [신규, 2026-09-23, PN-740005DF 항목2] 부모 디렉터리 확장 -
            // 기존 체인 안에 빈 슬롯이 전혀 없으면 새 클러스터 하나를
            // 할당해 tail에 이어붙인다. 새 클러스터를 전부 0으로
            // 초기화하면 그 첫 바이트(name[0])가 자동으로
            // `kNameFreeRestMarker`(0x00)가 돼 "여기부터 디렉터리 끝"
            // 이라는 기존 스펙 불변조건을 그대로 만족하므로, 그 클러스터
            // 오프셋 0을 곧바로 빈 슬롯으로 쓸 수 있다(새 설계 결정
            // 불필요 - Write()가 이미 구현한 "free 클러스터 할당+링크"
            // 패턴 재사용).
            if (!freeSlotFound) {
                uint32_t extCluster = 0;
                bool extAllocFailed = false;
                {
                    const uint32_t clusterCount = volume_.clusterCountValue();
                    uint32_t candidate = nextClusterScanHint_;
                    if (candidate < kFirstDataCluster || candidate > clusterCount + 1) {
                        candidate = kFirstDataCluster;
                    }
                    bool foundFree = false;
                    uint32_t loadedFatSector = 0xFFFFFFFFu;
                    SlabBuf scanBuf(bytesPerSector);
                    if (!scanBuf) {
                        extAllocFailed = true;
                    } else {
                        for (uint32_t attempts = 0; attempts < clusterCount && !extAllocFailed; ++attempts) {
                            uint32_t fatSector = 0;
                            uint32_t fatByteOffset = 0;
                            kFatEntryLocation(fatStartSector, bytesPerSector, candidate, &fatSector, &fatByteOffset);
                            if (fatSector != loadedFatSector) {
                                fs::BlockIoResult scanIoResult;
                                kernel::AsyncTask* scanIoTask = kSubmitReadSectors(
                                    device, bytesPerSector, fatSector, 1, scanBuf.get(), &scanIoResult);
                                if (!scanIoTask) {
                                    extAllocFailed = true;
                                    break;
                                }
                                co_await kernel::AsyncTaskCoroAwaiter(scanIoTask);
                                if (!scanIoResult.ok) {
                                    extAllocFailed = true;
                                    break;
                                }
                                loadedFatSector = fatSector;
                            }
                            uint32_t raw = 0;
                            memcpy(&raw, scanBuf.get() + fatByteOffset, sizeof(raw));
                            if ((raw & kFatEntryMask) == 0) {
                                foundFree = true;
                                extCluster = candidate;
                                break;
                            }
                            candidate = (candidate >= clusterCount + 1) ? kFirstDataCluster : candidate + 1;
                        }
                        if (!extAllocFailed && !foundFree) {
                            extAllocFailed = true;
                        }
                    }
                }
                if (extAllocFailed) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                nextClusterScanHint_ =
                    (extCluster >= volume_.clusterCountValue() + 1) ? kFirstDataCluster : extCluster + 1;

                // extCluster를 EOC로(모든 FAT 사본) + parentTailCluster를
                // extCluster로 링크(모든 FAT 사본) - 순서는 Write()와
                // 동일한 방어적 이유(EOC 먼저).
                bool extLinkFailed = false;
                const uint32_t numFats = volume_.numFatsValue();
                const uint32_t fatSize32 = volume_.fatSize32Value();
                for (uint32_t fatIndex = 0; fatIndex < numFats && !extLinkFailed; ++fatIndex) {
                    uint32_t sector = 0;
                    uint32_t byteOffset = 0;
                    kFatEntryLocation(fatStartSector + fatIndex * fatSize32, bytesPerSector, extCluster, &sector,
                                       &byteOffset);
                    SlabBuf buf(bytesPerSector);
                    if (!buf) {
                        extLinkFailed = true;
                        break;
                    }
                    fs::BlockIoResult readResult;
                    kernel::AsyncTask* readTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, 1, buf.get(), &readResult);
                    if (!readTask) {
                        extLinkFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(readTask);
                    if (!readResult.ok) {
                        extLinkFailed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, buf.get() + byteOffset, sizeof(raw));
                    const uint32_t newRaw = kEncodeFatEntry(raw, kFatEocMin);
                    memcpy(buf.get() + byteOffset, &newRaw, sizeof(newRaw));
                    fs::BlockIoResult writeResult;
                    kernel::AsyncTask* writeTask =
                        kSubmitWriteSectors(device, bytesPerSector, sector, 1, buf.get(), &writeResult);
                    if (!writeTask) {
                        extLinkFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                    if (!writeResult.ok) {
                        extLinkFailed = true;
                        break;
                    }
                }
                if (!extLinkFailed) {
                    for (uint32_t fatIndex = 0; fatIndex < numFats && !extLinkFailed; ++fatIndex) {
                        uint32_t sector = 0;
                        uint32_t byteOffset = 0;
                        kFatEntryLocation(fatStartSector + fatIndex * fatSize32, bytesPerSector, parentTailCluster,
                                           &sector, &byteOffset);
                        SlabBuf buf(bytesPerSector);
                        if (!buf) {
                            extLinkFailed = true;
                            break;
                        }
                        fs::BlockIoResult readResult;
                        kernel::AsyncTask* readTask =
                            kSubmitReadSectors(device, bytesPerSector, sector, 1, buf.get(), &readResult);
                        if (!readTask) {
                            extLinkFailed = true;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(readTask);
                        if (!readResult.ok) {
                            extLinkFailed = true;
                            break;
                        }
                        uint32_t raw = 0;
                        memcpy(&raw, buf.get() + byteOffset, sizeof(raw));
                        const uint32_t newRaw = kEncodeFatEntry(raw, extCluster);
                        memcpy(buf.get() + byteOffset, &newRaw, sizeof(newRaw));
                        fs::BlockIoResult writeResult;
                        kernel::AsyncTask* writeTask =
                            kSubmitWriteSectors(device, bytesPerSector, sector, 1, buf.get(), &writeResult);
                        if (!writeTask) {
                            extLinkFailed = true;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                        if (!writeResult.ok) {
                            extLinkFailed = true;
                            break;
                        }
                    }
                }
                if (extLinkFailed) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }

                SlabBuf zeroBuf(bytesPerCluster);
                if (!zeroBuf) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                memset(zeroBuf.get(), 0, bytesPerCluster);
                uint32_t extDataSector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, extCluster, &extDataSector)) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                fs::BlockIoResult zeroResult;
                kernel::AsyncTask* zeroTask = kSubmitWriteSectors(device, bytesPerSector, extDataSector,
                                                                   sectorsPerCluster, zeroBuf.get(), &zeroResult);
                if (!zeroTask) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(zeroTask);
                if (!zeroResult.ok) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }

                freeSlotCluster = extCluster;
                freeSlotByteOffset = 0;
                freeSlotFound = true;
            }

            uint32_t newCluster = 0;
            bool allocFailed = false;
            {
                const uint32_t clusterCount = volume_.clusterCountValue();
                uint32_t candidate = nextClusterScanHint_;
                if (candidate < kFirstDataCluster || candidate > clusterCount + 1) {
                    candidate = kFirstDataCluster;
                }
                bool foundFree = false;
                uint32_t loadedFatSector = 0xFFFFFFFFu;
                SlabBuf scanBuf(bytesPerSector);
                if (!scanBuf) {
                    allocFailed = true;
                } else {
                    for (uint32_t attempts = 0; attempts < clusterCount && !allocFailed; ++attempts) {
                        uint32_t fatSector = 0;
                        uint32_t fatByteOffset = 0;
                        kFatEntryLocation(fatStartSector, bytesPerSector, candidate, &fatSector, &fatByteOffset);
                        if (fatSector != loadedFatSector) {
                            fs::BlockIoResult scanIoResult;
                            kernel::AsyncTask* scanIoTask = kSubmitReadSectors(device, bytesPerSector, fatSector, 1,
                                                                                scanBuf.get(), &scanIoResult);
                            if (!scanIoTask) {
                                allocFailed = true;
                                break;
                            }
                            co_await kernel::AsyncTaskCoroAwaiter(scanIoTask);
                            if (!scanIoResult.ok) {
                                allocFailed = true;
                                break;
                            }
                            loadedFatSector = fatSector;
                        }
                        uint32_t raw = 0;
                        memcpy(&raw, scanBuf.get() + fatByteOffset, sizeof(raw));
                        if ((raw & kFatEntryMask) == 0) {
                            foundFree = true;
                            newCluster = candidate;
                            break;
                        }
                        candidate = (candidate >= clusterCount + 1) ? kFirstDataCluster : candidate + 1;
                    }
                    if (!allocFailed && !foundFree) {
                        allocFailed = true;
                    }
                }
            }
            if (allocFailed) {
                args->error = kernel::VfsError::NoSpace;
                break;
            }
            nextClusterScanHint_ =
                (newCluster >= volume_.clusterCountValue() + 1) ? kFirstDataCluster : newCluster + 1;

            {
                const uint32_t numFats = volume_.numFatsValue();
                const uint32_t fatSize32 = volume_.fatSize32Value();
                bool linkFailed = false;
                for (uint32_t fatIndex = 0; fatIndex < numFats && !linkFailed; ++fatIndex) {
                    uint32_t sector = 0;
                    uint32_t byteOffset = 0;
                    kFatEntryLocation(fatStartSector + fatIndex * fatSize32, bytesPerSector, newCluster, &sector,
                                       &byteOffset);
                    SlabBuf buf(bytesPerSector);
                    if (!buf) {
                        linkFailed = true;
                        break;
                    }
                    fs::BlockIoResult readResult;
                    kernel::AsyncTask* readTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, 1, buf.get(), &readResult);
                    if (!readTask) {
                        linkFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(readTask);
                    if (!readResult.ok) {
                        linkFailed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, buf.get() + byteOffset, sizeof(raw));
                    const uint32_t newRaw = kEncodeFatEntry(raw, kFatEocMin);
                    memcpy(buf.get() + byteOffset, &newRaw, sizeof(newRaw));
                    fs::BlockIoResult writeResult;
                    kernel::AsyncTask* writeTask =
                        kSubmitWriteSectors(device, bytesPerSector, sector, 1, buf.get(), &writeResult);
                    if (!writeTask) {
                        linkFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                    if (!writeResult.ok) {
                        linkFailed = true;
                        break;
                    }
                }
                if (linkFailed) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
            }

            {
                SlabBuf newClusterBuf(bytesPerCluster);
                if (!newClusterBuf) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                memset(newClusterBuf.get(), 0, bytesPerCluster);
                auto* dotEntries = reinterpret_cast<DirEntry*>(newClusterBuf.get());

                memset(dotEntries[0].name, ' ', 8);
                dotEntries[0].name[0] = '.';
                memset(dotEntries[0].ext, ' ', 3);
                dotEntries[0].attr = kAttrDirectory;
                dotEntries[0].fstClusHi = static_cast<uint16_t>(newCluster >> 16);
                dotEntries[0].fstClusLo = static_cast<uint16_t>(newCluster & 0xFFFFu);
                dotEntries[0].fileSize = 0;

                memset(dotEntries[1].name, ' ', 8);
                dotEntries[1].name[0] = '.';
                dotEntries[1].name[1] = '.';
                memset(dotEntries[1].ext, ' ', 3);
                dotEntries[1].attr = kAttrDirectory;
                // [FAT32 스펙 관례] 부모가 루트면 ".."의 firstCluster는
                // 루트의 실제 클러스터 번호가 아니라 0으로 쓴다(다른
                // FAT 드라이버와의 호환 관례 - 0을 "루트"로 해석).
                const uint32_t dotDotCluster = (parentCluster == volume_.rootFirstCluster()) ? 0 : parentCluster;
                dotEntries[1].fstClusHi = static_cast<uint16_t>(dotDotCluster >> 16);
                dotEntries[1].fstClusLo = static_cast<uint16_t>(dotDotCluster & 0xFFFFu);
                dotEntries[1].fileSize = 0;

                uint32_t newDataSector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, newCluster, &newDataSector)) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                fs::BlockIoResult writeResult;
                kernel::AsyncTask* writeTask = kSubmitWriteSectors(device, bytesPerSector, newDataSector,
                                                                    sectorsPerCluster, newClusterBuf.get(), &writeResult);
                if (!writeTask) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                if (!writeResult.ok) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
            }

            {
                // [갱신, 2026-09-23, PN-3D39A53C] 예전엔 엔트리 1개가
                // 들어가는 섹터 하나만 read-modify-write 했지만, LFN
                // 슬롯이 짧은 엔트리 앞에 여러 개 들어갈 수 있어(모두
                // 한 클러스터 안이라는 건 위에서 이미 보장) 클러스터
                // 전체를 읽고 고쳐서 한 번에 다시 쓴다.
                uint32_t entryClusterSector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, freeSlotCluster, &entryClusterSector)) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                SlabBuf entryClusterBuf(bytesPerCluster);
                if (!entryClusterBuf) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                fs::BlockIoResult readResult;
                kernel::AsyncTask* readTask = kSubmitReadSectors(
                    device, bytesPerSector, entryClusterSector, sectorsPerCluster, entryClusterBuf.get(), &readResult);
                if (!readTask) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(readTask);
                if (!readResult.ok) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }

                uint32_t writeOffset = freeSlotByteOffset;
                if (needsLfn) {
                    LfnSlot lfnSlots[kLfnMaxSlots];
                    kBuildLfnSlots(args->relPath + leafStart, leafLen, kLfnChecksum(finalShortName11), lfnSlots,
                                   lfnSlotCount);
                    memcpy(entryClusterBuf.get() + writeOffset, lfnSlots, lfnSlotCount * sizeof(LfnSlot));
                    writeOffset += lfnSlotCount * static_cast<uint32_t>(sizeof(LfnSlot));
                }

                auto* entry = reinterpret_cast<DirEntry*>(entryClusterBuf.get() + writeOffset);
                memset(entry, 0, sizeof(DirEntry));
                memcpy(entry->name, finalShortName11, 8);
                memcpy(entry->ext, finalShortName11 + 8, 3);
                entry->attr = kAttrDirectory;
                entry->ntReserved = finalNtReserved;
                entry->fstClusHi = static_cast<uint16_t>(newCluster >> 16);
                entry->fstClusLo = static_cast<uint16_t>(newCluster & 0xFFFFu);
                entry->fileSize = 0;

                fs::BlockIoResult writeResult;
                kernel::AsyncTask* writeTask = kSubmitWriteSectors(
                    device, bytesPerSector, entryClusterSector, sectorsPerCluster, entryClusterBuf.get(), &writeResult);
                if (!writeTask) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                if (!writeResult.ok) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
            }

            args->error = kernel::VfsError::None;
            break;
        }
        case kernel::KernelFsOpCode::Rmdir: {
            // [구현, 2026-09-23, PN-9D6FE4B6] Unlink와 거의 동일한 구조
            // (부모 탐색 → leaf 위치 확인 → 삭제 마킹 → 클러스터 반납) -
            // 다른 점 둘: (1) 대상이 디렉터리여야 함(파일이면 거부,
            // Unlink 써야 함), (2) 삭제 전 "."/".." 외의 유효 엔트리가
            // 없는지 확인(있으면 NotEmpty 거부) - Unlink와 합성 불가
            // 제약(파일 상단 문서 주석)으로 공유 못 해 그대로 중복.
            auto* args = static_cast<kernel::KernelFsRmdirArgs*>(argsRaw);
            if (readOnly_) {
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            uint32_t parentLen = 0;
            uint32_t leafStart = 0;
            uint32_t leafLen = 0;
            if (!kSplitParentAndLeaf(args->relPath, args->relPathLen, &parentLen, &leafStart, &leafLen)) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }

            uint32_t parentCluster = volume_.rootFirstCluster();
            bool parentFound = true;
            {
                uint32_t pos = 0;
                while (pos < parentLen) {
                    while (pos < parentLen && args->relPath[pos] == '/') {
                        ++pos;
                    }
                    if (pos >= parentLen) {
                        break;
                    }
                    const uint32_t segStart = pos;
                    while (pos < parentLen && args->relPath[pos] != '/') {
                        ++pos;
                    }
                    const uint32_t segLen = pos - segStart;

                    char normalized[11];
                    kNormalizeTo83(args->relPath + segStart, segLen, normalized);
                    ResolvedEntry matched;
                    bool foundInThisDir = false;
                    uint32_t scanCluster = parentCluster;
                    while (true) {
                        uint32_t sector = 0;
                        if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                            break;
                        }
                        SlabBuf clusterBuf(bytesPerCluster);
                        if (!clusterBuf) {
                            parentFound = false;
                            break;
                        }
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask = kSubmitReadSectors(device, bytesPerSector, sector,
                                                                        sectorsPerCluster, clusterBuf.get(), &ioResult);
                        if (!ioTask) {
                            parentFound = false;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            parentFound = false;
                            break;
                        }
                        const DirScanResult scanResult = kScanDirClusterForName(
                            clusterBuf.get(), bytesPerCluster, normalized, args->relPath + segStart, segLen, &matched);
                        if (scanResult == DirScanResult::Found) {
                            foundInThisDir = true;
                            break;
                        }
                        if (scanResult == DirScanResult::EndOfDir) {
                            break;
                        }
                        uint32_t fatSector = 0;
                        uint32_t fatByteOffset = 0;
                        kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                        SlabBuf fatBuf(bytesPerSector);
                        if (!fatBuf) {
                            parentFound = false;
                            break;
                        }
                        fs::BlockIoResult fatIoResult;
                        kernel::AsyncTask* fatIoTask =
                            kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                        if (!fatIoTask) {
                            parentFound = false;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                        if (!fatIoResult.ok) {
                            parentFound = false;
                            break;
                        }
                        uint32_t raw = 0;
                        memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                        uint32_t next = 0;
                        if (kInterpretFatEntry(raw, &next) != ChainStep::Next) {
                            break;
                        }
                        scanCluster = next;
                    }
                    if (!foundInThisDir || !matched.isDir) {
                        parentFound = false;
                        break;
                    }
                    parentCluster = matched.firstCluster;
                }
            }
            if (!parentFound) {
                args->error = kernel::VfsError::NotFound;
                break;
            }

            char leafNormalized[11];
            kNormalizeTo83(args->relPath + leafStart, leafLen, leafNormalized);
            ResolvedEntry leafMatched;
            bool leafFound = false;
            bool ioFailed = false;
            {
                uint32_t scanCluster = parentCluster;
                while (true) {
                    uint32_t sector = 0;
                    if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                        break;
                    }
                    SlabBuf clusterBuf(bytesPerCluster);
                    if (!clusterBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                           &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    const DirScanResult scanResult = kScanDirClusterForName(
                        clusterBuf.get(), bytesPerCluster, leafNormalized, args->relPath + leafStart, leafLen,
                        &leafMatched);
                    if (scanResult == DirScanResult::Found) {
                        leafMatched.entryCluster = scanCluster;
                        leafFound = true;
                        break;
                    }
                    if (scanResult == DirScanResult::EndOfDir) {
                        break;
                    }
                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t next = 0;
                    if (kInterpretFatEntry(raw, &next) != ChainStep::Next) {
                        break;
                    }
                    scanCluster = next;
                }
            }
            if (ioFailed) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            if (!leafFound) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            if (!leafMatched.isDir) {
                args->error = kernel::VfsError::InvalidArgument;  // Unlink를 써야 함
                break;
            }

            bool isEmpty = true;
            bool emptyCheckFailed = false;
            {
                uint32_t c = leafMatched.firstCluster;
                while (c != 0 && isEmpty && !emptyCheckFailed) {
                    uint32_t sector = 0;
                    if (!kClusterToSector(dataStartSector, sectorsPerCluster, c, &sector)) {
                        break;
                    }
                    SlabBuf clusterBuf(bytesPerCluster);
                    if (!clusterBuf) {
                        emptyCheckFailed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                           &ioResult);
                    if (!ioTask) {
                        emptyCheckFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        emptyCheckFailed = true;
                        break;
                    }

                    const uint32_t entriesPerCluster = bytesPerCluster / sizeof(DirEntry);
                    const auto* entries = reinterpret_cast<const DirEntry*>(clusterBuf.get());
                    bool hitEnd = false;
                    for (uint32_t i = 0; i < entriesPerCluster; ++i) {
                        const DirEntry& e = entries[i];
                        const uint8_t firstByte = static_cast<uint8_t>(e.name[0]);
                        if (firstByte == kNameFreeRestMarker) {
                            hitEnd = true;
                            break;
                        }
                        if (firstByte == kNameDeletedMarker) {
                            continue;
                        }
                        if (e.attr == kAttrLongName) {
                            continue;  // LFN 슬롯 - 짧은 엔트리 존재 여부만으로 판단
                        }
                        const bool isDot = memcmp(e.name, ".       ", 8) == 0 && memcmp(e.ext, "   ", 3) == 0;
                        const bool isDotDot = memcmp(e.name, "..      ", 8) == 0 && memcmp(e.ext, "   ", 3) == 0;
                        if (!isDot && !isDotDot) {
                            isEmpty = false;
                            break;
                        }
                    }
                    if (hitEnd || !isEmpty) {
                        break;
                    }

                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, c, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        emptyCheckFailed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        emptyCheckFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        emptyCheckFailed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t next = 0;
                    if (kInterpretFatEntry(raw, &next) != ChainStep::Next) {
                        break;
                    }
                    c = next;
                }
            }
            if (emptyCheckFailed) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            if (!isEmpty) {
                args->error = kernel::VfsError::NotEmpty;
                break;
            }

            {
                // [갱신, 2026-09-23, PN-740005DF 항목3] 예전엔 짧은
                // 엔트리가 든 섹터 하나만 지웠지만, 그 앞에 LFN 슬롯
                // 체인이 있으면 고아로 남는다(fsck.vfat이 "Orphaned
                // long file name part"로 검출 - 실측으로 이미 확인된
                // 무해하지만 정리 안 된 상태). LFN 슬롯은 PN-3D39A53C가
                // 쓰기 쪽에서도 확정한 "클러스터 경계를 넘지 않는다"
                // 제약 덕에 항상 짧은 엔트리와 같은 클러스터 안에 있다
                // - 클러스터 전체를 읽어 짧은 엔트리 바로 앞부터 역순
                // (LFN 슬롯은 항상 시퀀스 역순으로, 즉 엔트리에 가장
                // 가까운 것부터 저장돼 있음)으로 attr==kAttrLongName인
                // 슬롯을 전부(비-LFN 엔트리를 만나거나 클러스터 시작에
                // 닿을 때까지) 같이 0xE5로 마킹한 뒤 한 번에 다시 쓴다.
                uint32_t entryClusterSector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, leafMatched.entryCluster,
                                       &entryClusterSector)) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                SlabBuf entryClusterBuf(bytesPerCluster);
                if (!entryClusterBuf) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                fs::BlockIoResult readResult;
                kernel::AsyncTask* readTask = kSubmitReadSectors(
                    device, bytesPerSector, entryClusterSector, sectorsPerCluster, entryClusterBuf.get(), &readResult);
                if (!readTask) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(readTask);
                if (!readResult.ok) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }

                auto* entries = reinterpret_cast<DirEntry*>(entryClusterBuf.get());
                const uint32_t entryIndex = leafMatched.entryByteOffset / static_cast<uint32_t>(sizeof(DirEntry));
                entries[entryIndex].name[0] = static_cast<char>(kNameDeletedMarker);
                for (uint32_t i = entryIndex; i > 0 && entries[i - 1].attr == kAttrLongName; --i) {
                    entries[i - 1].name[0] = static_cast<char>(kNameDeletedMarker);
                }

                fs::BlockIoResult writeResult;
                kernel::AsyncTask* writeTask = kSubmitWriteSectors(
                    device, bytesPerSector, entryClusterSector, sectorsPerCluster, entryClusterBuf.get(), &writeResult);
                if (!writeTask) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                if (!writeResult.ok) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
            }

            {
                uint32_t c = leafMatched.firstCluster;
                const uint32_t numFats = volume_.numFatsValue();
                const uint32_t fatSize32 = volume_.fatSize32Value();
                while (c != 0) {
                    uint32_t nextC = 0;
                    ChainStep step = ChainStep::Invalid;
                    bool freeFailed = false;
                    {
                        uint32_t fatSector = 0;
                        uint32_t fatByteOffset = 0;
                        kFatEntryLocation(fatStartSector, bytesPerSector, c, &fatSector, &fatByteOffset);
                        SlabBuf fatBuf(bytesPerSector);
                        if (!fatBuf) {
                            freeFailed = true;
                        } else {
                            fs::BlockIoResult fatIoResult;
                            kernel::AsyncTask* fatIoTask =
                                kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                            if (!fatIoTask) {
                                freeFailed = true;
                            } else {
                                co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                                if (!fatIoResult.ok) {
                                    freeFailed = true;
                                } else {
                                    uint32_t raw = 0;
                                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                                    step = kInterpretFatEntry(raw, &nextC);
                                }
                            }
                        }
                    }
                    for (uint32_t fatIndex = 0; fatIndex < numFats && !freeFailed; ++fatIndex) {
                        uint32_t sector = 0;
                        uint32_t byteOffset = 0;
                        kFatEntryLocation(fatStartSector + fatIndex * fatSize32, bytesPerSector, c, &sector,
                                           &byteOffset);
                        SlabBuf buf(bytesPerSector);
                        if (!buf) {
                            freeFailed = true;
                            break;
                        }
                        fs::BlockIoResult readResult;
                        kernel::AsyncTask* readTask =
                            kSubmitReadSectors(device, bytesPerSector, sector, 1, buf.get(), &readResult);
                        if (!readTask) {
                            freeFailed = true;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(readTask);
                        if (!readResult.ok) {
                            freeFailed = true;
                            break;
                        }
                        uint32_t raw = 0;
                        memcpy(&raw, buf.get() + byteOffset, sizeof(raw));
                        const uint32_t newRaw = kEncodeFatEntry(raw, 0);
                        memcpy(buf.get() + byteOffset, &newRaw, sizeof(newRaw));
                        fs::BlockIoResult writeResult;
                        kernel::AsyncTask* writeTask =
                            kSubmitWriteSectors(device, bytesPerSector, sector, 1, buf.get(), &writeResult);
                        if (!writeTask) {
                            freeFailed = true;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                        if (!writeResult.ok) {
                            freeFailed = true;
                            break;
                        }
                    }
                    if (freeFailed || step != ChainStep::Next) {
                        break;
                    }
                    c = nextC;
                }
            }

            args->error = kernel::VfsError::None;
            break;
        }
        case kernel::KernelFsOpCode::Unlink: {
            // [구현, 2026-09-23, PN-9D6FE4B6] 대상의 부모 디렉터리를
            // 먼저 찾고(Open()/Stat()과 같은 세그먼트 순회 - 합성 불가
            // 제약으로 공유 못 함, 파일 상단 문서 주석 참고), 그 안에서
            // leaf 이름의 엔트리 위치(entryCluster/entryByteOffset)와
            // firstCluster를 찾아 (1) name[0]을 0xE5로 마킹, (2) 클러스터
            // 체인을 전부 반납(FAT 엔트리 0, 모든 사본)한다. 디렉터리는
            // 거부 - Rmdir 전용.
            auto* args = static_cast<kernel::KernelFsUnlinkArgs*>(argsRaw);
            if (readOnly_) {
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            uint32_t parentLen = 0;
            uint32_t leafStart = 0;
            uint32_t leafLen = 0;
            if (!kSplitParentAndLeaf(args->relPath, args->relPathLen, &parentLen, &leafStart, &leafLen)) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }

            uint32_t parentCluster = volume_.rootFirstCluster();
            bool parentFound = true;
            {
                uint32_t pos = 0;
                while (pos < parentLen) {
                    while (pos < parentLen && args->relPath[pos] == '/') {
                        ++pos;
                    }
                    if (pos >= parentLen) {
                        break;
                    }
                    const uint32_t segStart = pos;
                    while (pos < parentLen && args->relPath[pos] != '/') {
                        ++pos;
                    }
                    const uint32_t segLen = pos - segStart;

                    char normalized[11];
                    kNormalizeTo83(args->relPath + segStart, segLen, normalized);
                    ResolvedEntry matched;
                    bool foundInThisDir = false;
                    uint32_t scanCluster = parentCluster;
                    while (true) {
                        uint32_t sector = 0;
                        if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                            break;
                        }
                        SlabBuf clusterBuf(bytesPerCluster);
                        if (!clusterBuf) {
                            parentFound = false;
                            break;
                        }
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask = kSubmitReadSectors(device, bytesPerSector, sector,
                                                                        sectorsPerCluster, clusterBuf.get(), &ioResult);
                        if (!ioTask) {
                            parentFound = false;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            parentFound = false;
                            break;
                        }
                        const DirScanResult scanResult = kScanDirClusterForName(
                            clusterBuf.get(), bytesPerCluster, normalized, args->relPath + segStart, segLen, &matched);
                        if (scanResult == DirScanResult::Found) {
                            foundInThisDir = true;
                            break;
                        }
                        if (scanResult == DirScanResult::EndOfDir) {
                            break;
                        }
                        uint32_t fatSector = 0;
                        uint32_t fatByteOffset = 0;
                        kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                        SlabBuf fatBuf(bytesPerSector);
                        if (!fatBuf) {
                            parentFound = false;
                            break;
                        }
                        fs::BlockIoResult fatIoResult;
                        kernel::AsyncTask* fatIoTask =
                            kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                        if (!fatIoTask) {
                            parentFound = false;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                        if (!fatIoResult.ok) {
                            parentFound = false;
                            break;
                        }
                        uint32_t raw = 0;
                        memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                        uint32_t next = 0;
                        if (kInterpretFatEntry(raw, &next) != ChainStep::Next) {
                            break;
                        }
                        scanCluster = next;
                    }
                    if (!foundInThisDir || !matched.isDir) {
                        parentFound = false;
                        break;
                    }
                    parentCluster = matched.firstCluster;
                }
            }
            if (!parentFound) {
                args->error = kernel::VfsError::NotFound;
                break;
            }

            char leafNormalized[11];
            kNormalizeTo83(args->relPath + leafStart, leafLen, leafNormalized);
            ResolvedEntry leafMatched;
            bool leafFound = false;
            bool ioFailed = false;
            {
                uint32_t scanCluster = parentCluster;
                while (true) {
                    uint32_t sector = 0;
                    if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                        break;
                    }
                    SlabBuf clusterBuf(bytesPerCluster);
                    if (!clusterBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                           &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    const DirScanResult scanResult = kScanDirClusterForName(
                        clusterBuf.get(), bytesPerCluster, leafNormalized, args->relPath + leafStart, leafLen,
                        &leafMatched);
                    if (scanResult == DirScanResult::Found) {
                        leafMatched.entryCluster = scanCluster;
                        leafFound = true;
                        break;
                    }
                    if (scanResult == DirScanResult::EndOfDir) {
                        break;
                    }
                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t next = 0;
                    if (kInterpretFatEntry(raw, &next) != ChainStep::Next) {
                        break;
                    }
                    scanCluster = next;
                }
            }
            if (ioFailed) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            if (!leafFound) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            if (leafMatched.isDir) {
                args->error = kernel::VfsError::InvalidArgument;  // Rmdir을 써야 함
                break;
            }

            {
                // [갱신, 2026-09-23, PN-740005DF 항목3] 예전엔 짧은
                // 엔트리가 든 섹터 하나만 지웠지만, 그 앞에 LFN 슬롯
                // 체인이 있으면 고아로 남는다(fsck.vfat이 "Orphaned
                // long file name part"로 검출 - 실측으로 이미 확인된
                // 무해하지만 정리 안 된 상태). LFN 슬롯은 PN-3D39A53C가
                // 쓰기 쪽에서도 확정한 "클러스터 경계를 넘지 않는다"
                // 제약 덕에 항상 짧은 엔트리와 같은 클러스터 안에 있다
                // - 클러스터 전체를 읽어 짧은 엔트리 바로 앞부터 역순
                // (LFN 슬롯은 항상 시퀀스 역순으로, 즉 엔트리에 가장
                // 가까운 것부터 저장돼 있음)으로 attr==kAttrLongName인
                // 슬롯을 전부(비-LFN 엔트리를 만나거나 클러스터 시작에
                // 닿을 때까지) 같이 0xE5로 마킹한 뒤 한 번에 다시 쓴다.
                uint32_t entryClusterSector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, leafMatched.entryCluster,
                                       &entryClusterSector)) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                SlabBuf entryClusterBuf(bytesPerCluster);
                if (!entryClusterBuf) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                fs::BlockIoResult readResult;
                kernel::AsyncTask* readTask = kSubmitReadSectors(
                    device, bytesPerSector, entryClusterSector, sectorsPerCluster, entryClusterBuf.get(), &readResult);
                if (!readTask) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(readTask);
                if (!readResult.ok) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }

                auto* entries = reinterpret_cast<DirEntry*>(entryClusterBuf.get());
                const uint32_t entryIndex = leafMatched.entryByteOffset / static_cast<uint32_t>(sizeof(DirEntry));
                entries[entryIndex].name[0] = static_cast<char>(kNameDeletedMarker);
                for (uint32_t i = entryIndex; i > 0 && entries[i - 1].attr == kAttrLongName; --i) {
                    entries[i - 1].name[0] = static_cast<char>(kNameDeletedMarker);
                }

                fs::BlockIoResult writeResult;
                kernel::AsyncTask* writeTask = kSubmitWriteSectors(
                    device, bytesPerSector, entryClusterSector, sectorsPerCluster, entryClusterBuf.get(), &writeResult);
                if (!writeTask) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                if (!writeResult.ok) {
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
            }

            {
                uint32_t c = leafMatched.firstCluster;
                const uint32_t numFats = volume_.numFatsValue();
                const uint32_t fatSize32 = volume_.fatSize32Value();
                while (c != 0) {
                    uint32_t nextC = 0;
                    ChainStep step = ChainStep::Invalid;
                    bool freeFailed = false;
                    {
                        uint32_t fatSector = 0;
                        uint32_t fatByteOffset = 0;
                        kFatEntryLocation(fatStartSector, bytesPerSector, c, &fatSector, &fatByteOffset);
                        SlabBuf fatBuf(bytesPerSector);
                        if (!fatBuf) {
                            freeFailed = true;
                        } else {
                            fs::BlockIoResult fatIoResult;
                            kernel::AsyncTask* fatIoTask =
                                kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                            if (!fatIoTask) {
                                freeFailed = true;
                            } else {
                                co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                                if (!fatIoResult.ok) {
                                    freeFailed = true;
                                } else {
                                    uint32_t raw = 0;
                                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                                    step = kInterpretFatEntry(raw, &nextC);
                                }
                            }
                        }
                    }
                    for (uint32_t fatIndex = 0; fatIndex < numFats && !freeFailed; ++fatIndex) {
                        uint32_t sector = 0;
                        uint32_t byteOffset = 0;
                        kFatEntryLocation(fatStartSector + fatIndex * fatSize32, bytesPerSector, c, &sector,
                                           &byteOffset);
                        SlabBuf buf(bytesPerSector);
                        if (!buf) {
                            freeFailed = true;
                            break;
                        }
                        fs::BlockIoResult readResult;
                        kernel::AsyncTask* readTask =
                            kSubmitReadSectors(device, bytesPerSector, sector, 1, buf.get(), &readResult);
                        if (!readTask) {
                            freeFailed = true;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(readTask);
                        if (!readResult.ok) {
                            freeFailed = true;
                            break;
                        }
                        uint32_t raw = 0;
                        memcpy(&raw, buf.get() + byteOffset, sizeof(raw));
                        const uint32_t newRaw = kEncodeFatEntry(raw, 0);
                        memcpy(buf.get() + byteOffset, &newRaw, sizeof(newRaw));
                        fs::BlockIoResult writeResult;
                        kernel::AsyncTask* writeTask =
                            kSubmitWriteSectors(device, bytesPerSector, sector, 1, buf.get(), &writeResult);
                        if (!writeTask) {
                            freeFailed = true;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                        if (!writeResult.ok) {
                            freeFailed = true;
                            break;
                        }
                    }
                    if (freeFailed || step != ChainStep::Next) {
                        break;
                    }
                    c = nextC;
                }
            }

            args->error = kernel::VfsError::None;
            break;
        }

        case kernel::KernelFsOpCode::Readdir: {
            auto* args = static_cast<kernel::KernelFsReaddirArgs*>(argsRaw);
            const uint32_t dirHandleIndex = static_cast<uint32_t>(args->dirHandle.value);
            if (dirHandleIndex >= kMaxOpenHandles || !openHandles_[dirHandleIndex].inUse) {
                args->hasMore = false;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            const uint32_t dirFirstCluster = openHandles_[dirHandleIndex].firstCluster;

            uint64_t seen = 0;
            bool found = false;
            bool ioFailed = false;
            uint32_t cluster = dirFirstCluster;
            SlabBuf clusterBuf(bytesPerCluster);
            if (!clusterBuf) {
                args->hasMore = false;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            while (!found && !ioFailed) {
                uint32_t sector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, cluster, &sector)) {
                    break;
                }
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                       &ioResult);
                if (!ioTask) {
                    ioFailed = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    ioFailed = true;
                    break;
                }

                const DirScanResult scanResult =
                    kScanDirClusterForIndex(clusterBuf.get(), bytesPerCluster, args->index, &seen, args->entry.name,
                                             sizeof(args->entry.name), &args->entry.nameLength,
                                             &args->entry.isDirectory);
                if (scanResult == DirScanResult::Found) {
                    found = true;
                    break;
                }
                if (scanResult == DirScanResult::EndOfDir) {
                    break;
                }

                uint32_t fatSector = 0;
                uint32_t fatByteOffset = 0;
                kFatEntryLocation(fatStartSector, bytesPerSector, cluster, &fatSector, &fatByteOffset);
                SlabBuf fatBuf(bytesPerSector);
                if (!fatBuf) {
                    ioFailed = true;
                    break;
                }
                fs::BlockIoResult fatIoResult;
                kernel::AsyncTask* fatIoTask =
                    kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                if (!fatIoTask) {
                    ioFailed = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                if (!fatIoResult.ok) {
                    ioFailed = true;
                    break;
                }
                uint32_t raw = 0;
                memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                uint32_t nextCluster = 0;
                if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                    break;
                }
                cluster = nextCluster;
            }

            args->hasMore = found;
            args->error = ioFailed ? kernel::VfsError::InvalidHandle : kernel::VfsError::None;
            break;
        }
    }
    co_return;
}

}  // namespace vfat
