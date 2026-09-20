import gdb

# PN-584DB994 갱신18 후속 - pn584_alloc_watch.py가 order=0 이중할당을
# 두 번 서로 다른 소비 지점(Lapic::init/execImage)에서 잡아냈고, 둘 다
# 부팅 극초반(첫 시도에서 곧바로, 확률적 재현이 아니라 결정적으로)
# 재현됐다 - 이는 런타임 레이스가 아니라 PageFrameAllocator::init()의
# 최초 free-list 구성 자체에 이미 중복 항목이 들어있을 가능성을
# 시사한다. 이 스크립트는 PageFrameAllocator::init()이 반환하는
# 순간(=아직 단 한 번의 allocOrder/freeOrder도 안 불린 시점) node 0의
# 모든 order별 free-list를 직접 순회해 "동일한 물리주소가 이미
# 두 번 이상 등장하는가"를 그 자리에서 검사한다 - 있으면 런타임 이전,
# 즉 초기화 자체의 버그로 확정.

kMaxOrder = 10  # page_frame_allocator.cpp의 kMaxOrder(익명 네임스페이스 상수)와 반드시 일치시킨다.
K_DIRECT_MAP_BASE = 0xFFFF800000000000  # paging.h의 kDirectMapBase(고정 constexpr)와 일치.


def log(msg):
    gdb.write(f"[pn584-freelist] {msg}\n")
    gdb.flush()


class InitFinish(gdb.FinishBreakpoint):
    def stop(self):
        log("PageFrameAllocator::init() returned - dumping node 0 free lists")
        seen = {}
        dup_found = False
        blocks = []  # (start, end, order) - 실제 겹침 검사용
        try:
            node0 = gdb.parse_and_eval("'page_frame_allocator.cpp'::gNodes[0]")
        except gdb.error:
            node0 = gdb.parse_and_eval("gNodes[0]")
        for order in range(0, kMaxOrder + 1):
            cur = int(node0["freeListHeads"][order])
            count_this_order = 0
            while cur != 0:
                if cur in seen:
                    dup_found = True
                    log(f"*** DUPLICATE IN INITIAL FREE LIST *** phys=0x{cur:x} "
                        f"appears at order={seen[cur]} AND order={order} - "
                        f"PageFrameAllocator::init() 자체가 이 물리주소를 두 번 등록함")
                else:
                    seen[cur] = order
                size = 4096 << order
                blocks.append((cur, cur + size, order))
                log(f"  order={order} block: phys=0x{cur:x}..0x{cur+size:x}")
                # kAsBlock(cur)->next - FreeBlock::next는 오프셋 0의 uint64_t.
                # kPhysToVirt()는 고정 오프셋(kDirectMapBase, paging.h)이라
                # 함수 호출 없이 파이썬에서 직접 더한다 - 인라인이라 gdb가
                # 실제 out-of-line 심볼을 못 찾을 위험을 피한다.
                virt = K_DIRECT_MAP_BASE + cur
                nxt = int(gdb.parse_and_eval(f"*(unsigned long*)0x{virt:x}"))
                cur = nxt
                count_this_order += 1
                if count_this_order > 2_000_000:
                    log(f"order={order}: 링크드리스트가 200만 항목을 넘음 - 순환 의심, 중단")
                    break
            log(f"order={order}: {count_this_order}개 블록")
        log(f"총 고유 시작주소 {len(seen)}개 관측, 시작주소 중복 {'있음' if dup_found else '없음'}")

        # [신규] 시작주소가 전부 달라도 서로 다른 order의 두 블록이 실제
        # 물리 범위로 겹칠 수 있다(예: order=10짜리 4MiB 블록 안에 다른
        # order=3짜리 블록이 우연히 포함됨) - 이게 진짜로 "물리 페이지
        # 이중소유"의 근본 원인일 가능성이 있어(초기 분할 시점부터 두
        # top-level 블록의 범위가 겹쳐 있었다면, 각자 따로 쪼개져도
        # 결국 같은 4KiB 주소가 양쪽에서 나올 수 있다) 전수 쌍대 비교.
        blocks.sort()
        overlap_found = False
        for i in range(len(blocks)):
            for j in range(i + 1, len(blocks)):
                s1, e1, o1 = blocks[i]
                s2, e2, o2 = blocks[j]
                if s1 < e2 and s2 < e1:
                    overlap_found = True
                    log(f"*** RANGE OVERLAP *** block[order={o1}] 0x{s1:x}..0x{e1:x} 와 "
                        f"block[order={o2}] 0x{s2:x}..0x{e2:x} 가 물리적으로 겹침 - "
                        f"초기 분할(kSubtractReservedFromList/kAssignRangeToNode) 자체의 버그")
        log(f"블록 간 물리 범위 겹침: {'있음' if overlap_found else '없음'} "
            f"(총 {len(blocks)}개 블록 전수 쌍대 비교)")
        return False  # 계속 진행


class InitEntry(gdb.Breakpoint):
    def stop(self):
        InitFinish(gdb.newest_frame(), internal=False)
        return False


InitEntry("kernel::PageFrameAllocator::init", internal=False)
log("armed - will dump free lists right after PageFrameAllocator::init() returns")
